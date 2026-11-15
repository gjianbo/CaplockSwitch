// ---------------------------------------------------------------------------
// ignore.cpp - 忽略应用：列表管理 + 前台进程判定 + 前台变化监听
//
// 需求来源：悠悠远程这类远程控制软件自己也要接管 Caps Lock，而低级键盘
// 钩子挂在所有程序之前，不主动让路就会把按键抢走。于是增加一条「白名单」
// 语义：**列表中的程序处于前台时，本程序对 Caps 完全不干预**，包括
// Ctrl+Caps —— 全部原样透传，仿佛 CapsSwitch 没有运行。
//
// 性能约束（最重要的一条）：
// **运行时绝不做任何进程名解析**。判定结果缓存成 g_app.ignoreActive 一个
// BOOL，键盘钩子回调只读这一位；前台变化回调只记一个 PID（O(1)）。
//
// 会碰 OpenProcess / Toolhelp 快照的地方只有三处，全都不在输入路径上：
//   1. Ignore_RebuildPidSet()：启动、用户改忽略列表、低频 WM_TIMER 时，
//      一次快照把所有命中规则的进程 PID 收进整数集合；
//   2. 用户点「添加...」时，应用选择器枚举窗口现查一次名字；
//   3. 选择器预选时按 lastForegroundPid 现查一次名字。
//
// 为什么这么改：旧实现每次前台变化都要解析前台进程名（OpenProcess，
// 失败则全量 Toolhelp 快照），而解析失败的 PID 不入缓存，会反复重来。
// 截图时前台在「被忽略程序 <-> 截图工具 <-> 目标程序」之间高频切换，
// 这些开销全压在承载低级键盘钩子的消息循环上，把 Ctrl+V 事件拖掉。
//
// 本文件同样遵守「不依赖 CRT」的约定：只用 lstr* 系列，不用 wcscmp/swprintf。
// ---------------------------------------------------------------------------

#include "app.h"
#include <tlhelp32.h>   // CreateToolhelp32Snapshot：取进程名的兜底通道

// ---------------------------------------------------------------------------
// 忽略进程的 PID 集合（放在文件前部，Ignore_Clear 也要用）
// ---------------------------------------------------------------------------

/**
 * 运行时判定只靠这张整数表，绝不解析进程名。
 * 一个程序可能有多个进程，容量按规则数放大几倍；满了就停止收录，
 * 属极端情况，宁可少忽略几个也不能让表越界。
 */
#define CAPS_MAX_IGNORED_PIDS  (CAPS_MAX_IGNORE * 4)

static DWORD g_ignoredPids[CAPS_MAX_IGNORED_PIDS];
static int   g_ignoredPidCount = 0;

// ---------------------------------------------------------------------------
// 进程名查询
// ---------------------------------------------------------------------------

/**
 * 用 Toolhelp 快照按 PID 查进程文件名。
 *
 * 这是 QueryFullProcessImageNameW 失败时的兜底：当目标进程以更高权限运行
 * （例如以管理员身份启动的远程控制软件），非提权进程调用 OpenProcess
 * 会直接被拒。而 TH32CS_SNAPPROCESS 快照不需要打开目标进程，照样能读到
 * 文件名 —— 对「按名字匹配」这个用途来说完全够用。
 *
 * @param pid 进程 ID。
 * @param out 输出缓冲区。
 * @param cch 缓冲区容量。
 * @return 找到时返回 TRUE。
 */
static BOOL LookupNameBySnapshot(DWORD pid, wchar_t* out, int cch)
{
    if (pid == 0 || out == NULL || cch <= 0) {
        return FALSE;
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    BOOL found = FALSE;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) {
                lstrcpynW(out, pe.szExeFile, cch);   // szExeFile 本身就是文件名
                found = (out[0] != L'\0');
                break;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return found;
}

BOOL Process_GetImageBaseName(DWORD pid, wchar_t* out, int cch)
{
    if (pid == 0 || out == NULL || cch <= 0) {
        return FALSE;
    }
    out[0] = L'\0';

    BOOL got = FALSE;
    wchar_t full[MAX_PATH];

    // 首选通道：一次 OpenProcess 就能拿到完整路径，开销最小。
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc != NULL) {
        DWORD len = ARRAYSIZE(full);
        if (QueryFullProcessImageNameW(proc, 0, full, &len)) {
            const wchar_t* base = full;
            for (const wchar_t* p = full; *p != L'\0'; ++p) {
                if (*p == L'\\' || *p == L'/') {
                    base = p + 1;
                }
            }
            if (*base != L'\0') {
                lstrcpynW(out, base, cch);
                got = (out[0] != L'\0');
            }
        }
        CloseHandle(proc);
    }

    // 兜底通道：提权进程打不开时改用快照查名字。
    if (!got) {
        got = LookupNameBySnapshot(pid, out, cch);
    }

    return got;
}

// ---------------------------------------------------------------------------
// 名字规范化
// ---------------------------------------------------------------------------

BOOL Ignore_Normalize(const wchar_t* in, wchar_t* out, int cch)
{
    if (in == NULL || out == NULL || cch <= 0) {
        return FALSE;
    }
    out[0] = L'\0';

    // ---- 1) 丢掉目录部分，只保留文件名 ----
    // 存完整路径会很脆：远程软件一升级、换个安装目录，规则就失效了。
    const wchar_t* base = in;
    for (const wchar_t* p = in; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }

    // ---- 2) 去掉首尾空白（手工改 ini 时很容易带进来）----
    const wchar_t* s = base;
    while (*s == L' ' || *s == L'\t') {
        ++s;
    }

    const wchar_t* e = s;
    while (*e != L'\0') {
        ++e;
    }
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) {
        --e;
    }

    const int len = (int)(e - s);
    if (len <= 0) {
        return FALSE;
    }

    // ---- 3) 没有扩展名时补 .exe ----
    // 用户手输「uu」和「uu.exe」应当被当成同一条规则，不能因为少打后缀就漏配。
    BOOL hasDot = FALSE;
    for (int i = 0; i < len; ++i) {
        if (s[i] == L'.') {
            hasDot = TRUE;
            break;
        }
    }

    const int suffixLen = hasDot ? 0 : 4;   // ".exe"
    if (len + suffixLen >= cch) {
        return FALSE;   // 放不下就整条拒绝，绝不静默截断成另一个名字
    }

    int n = 0;
    for (int i = 0; i < len; ++i) {
        out[n++] = s[i];
    }
    if (!hasDot) {
        out[n++] = L'.';
        out[n++] = L'e';
        out[n++] = L'x';
        out[n++] = L'e';
    }
    out[n] = L'\0';

    return TRUE;
}

// ---------------------------------------------------------------------------
// 列表管理
// ---------------------------------------------------------------------------

int Ignore_Count(void)
{
    return g_app.ignoreCount;
}

const wchar_t* Ignore_Get(int index)
{
    if (index < 0 || index >= g_app.ignoreCount) {
        return NULL;
    }
    return g_app.ignoreApps[index];
}

BOOL Ignore_Add(const wchar_t* name)
{
    wchar_t norm[CAPS_IGNORE_NAME_LEN];
    if (!Ignore_Normalize(name, norm, ARRAYSIZE(norm))) {
        return FALSE;
    }
    if (g_app.ignoreCount >= CAPS_MAX_IGNORE) {
        return FALSE;
    }

    // 重复添加视作成功，且不产生第二条 —— 界面上的「添加」按钮
    // 因此不需要额外提示「已经加过了」。
    for (int i = 0; i < g_app.ignoreCount; ++i) {
        if (lstrcmpiW(g_app.ignoreApps[i], norm) == 0) {
            return TRUE;
        }
    }

    lstrcpynW(g_app.ignoreApps[g_app.ignoreCount], norm, CAPS_IGNORE_NAME_LEN);
    ++g_app.ignoreCount;
    return TRUE;
}

BOOL Ignore_RemoveAt(int index)
{
    if (index < 0 || index >= g_app.ignoreCount) {
        return FALSE;
    }

    // 后面的条目整体前移，保持顺序稳定（列表顺序就是用户看到的顺序）。
    for (int i = index; i < g_app.ignoreCount - 1; ++i) {
        lstrcpynW(g_app.ignoreApps[i], g_app.ignoreApps[i + 1], CAPS_IGNORE_NAME_LEN);
    }
    --g_app.ignoreCount;
    g_app.ignoreApps[g_app.ignoreCount][0] = L'\0';
    return TRUE;
}

void Ignore_Clear(void)
{
    for (int i = 0; i < CAPS_MAX_IGNORE; ++i) {
        g_app.ignoreApps[i][0] = L'\0';
    }
    g_app.ignoreCount  = 0;
    g_app.ignoreActive = FALSE;
    g_ignoredPidCount  = 0;
}

// ---------------------------------------------------------------------------
// 判定
// ---------------------------------------------------------------------------

void Ignore_RebuildPidSet(void)
{
    g_ignoredPidCount = 0;

    if (g_app.ignoreCount <= 0) {
        g_app.ignoreActive = FALSE;
        return;
    }

    // TH32CS_SNAPPROCESS 不需要 OpenProcess，提权/受保护进程照样能列出。
    // szExeFile 本来就是文件名，与配置里的 uu.exe 格式一致，直接比较。
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            BOOL matched = FALSE;
            for (int i = 0; i < g_app.ignoreCount; ++i) {
                if (lstrcmpiW(pe.szExeFile, g_app.ignoreApps[i]) == 0) {
                    matched = TRUE;
                    break;
                }
            }
            if (matched && g_ignoredPidCount < CAPS_MAX_IGNORED_PIDS) {
                g_ignoredPids[g_ignoredPidCount++] = pe.th32ProcessID;
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
}

void Ignore_Recompute(void)
{
    BOOL active = FALSE;

    // 纯内存：一次 GetForegroundWindow + 一次 PID 线性查找。
    // 没有 OpenProcess、没有 Toolhelp、没有字符串比较、没有托盘刷新。
    if (g_ignoredPidCount > 0) {
        HWND fg = GetForegroundWindow();
        if (fg != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            if (pid != 0) {
                for (int i = 0; i < g_ignoredPidCount; ++i) {
                    if (g_ignoredPids[i] == pid) {
                        active = TRUE;
                        break;
                    }
                }
            }
        }
    }

    g_app.ignoreActive = active;
}

// 注意：**这里刻意不刷新托盘**。
// 曾经有个 Ignore_Refresh() 在 ignoreActive 翻转时调 Tray_Refresh() ->
// Shell_NotifyIconW(NIM_MODIFY)。但托盘图标和 tooltip 都只取决于
// enabled / remoteSession，压根不看 ignoreActive，那次刷新没有任何可见效果；
// 而 Shell_NotifyIconW 是同步跨进程调用，又发生在承载键盘钩子的消息循环
// 线程上。前台在「忽略应用 <-> 截图工具 <-> 目标程序」之间来回切换时，
// 状态反复翻转就会反复同步等 Explorer，把全局按键派发一起拖住 ——
// 表现为从被忽略进程截图后 Ctrl+V 粘不出图片。纯属有害无益，已移除。

void Ignore_ApplyConfig(const AppConfig* cfg)
{
    Ignore_Clear();

    if (cfg != NULL) {
        for (int i = 0; i < cfg->ignoreCount && i < CAPS_MAX_IGNORE; ++i) {
            Ignore_Add(cfg->ignoreApps[i]);
        }
    }

    // 记录启动时的前台进程 PID（只取 PID，不解析名字，开销可忽略）。
    // 这样即使启动后一次窗口都没切过就打开「添加...」，选择器也有预选项。
    HWND fg = GetForegroundWindow();
    if (fg != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != 0 && pid != GetCurrentProcessId()) {
            g_app.lastForegroundPid = pid;
        }
    }

    // 启动时扫一次进程表，之后运行时只做 PID 整数比较。
    Ignore_RebuildPidSet();
    Ignore_Recompute();
}

void Ignore_WriteConfig(AppConfig* cfg)
{
    if (cfg == NULL) {
        return;
    }

    cfg->ignoreCount = g_app.ignoreCount;
    for (int i = 0; i < CAPS_MAX_IGNORE; ++i) {
        if (i < g_app.ignoreCount) {
            lstrcpynW(cfg->ignoreApps[i], g_app.ignoreApps[i], CAPS_IGNORE_NAME_LEN);
        } else {
            cfg->ignoreApps[i][0] = L'\0';
        }
    }
}

// ---------------------------------------------------------------------------
// 前台变化监听
// ---------------------------------------------------------------------------

/**
 * EVENT_SYSTEM_FOREGROUND 回调。
 *
 * 使用 WINEVENT_OUTOFCONTEXT：系统把事件投递到安装钩子的线程的消息队列，
 * 由 GetMessage/DispatchMessage 派发。**这个线程同时也是低级键盘钩子的
 * 宿主线程** —— 在这里做任何耗时操作（OpenProcess、Toolhelp 快照、
 * Shell_NotifyIcon）都会把钩子回调的派发一起拖慢，表现为全局按键偶发
 * 延迟；剪贴板图片的延迟渲染对这种时序尤其敏感。
 *
 * 因此本回调**只做一件事**：记下前台进程 PID。
 * 不投递消息、不查进程名、不刷托盘 —— 忽略判定由低频 WM_TIMER 驱动，
 * 前台变化再频繁也不会往消息队列里堆积工作。
 *
 * @param idObject / idChild 只处理「窗口对象本身」的事件，忽略子元素。
 */
static void CALLBACK OnForegroundEvent(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                       LONG idObject, LONG idChild,
                                       DWORD threadId, DWORD time)
{
    UNREFERENCED_PARAMETER(hook);
    UNREFERENCED_PARAMETER(event);
    UNREFERENCED_PARAMETER(threadId);
    UNREFERENCED_PARAMETER(time);

    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == NULL) {
        return;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return;
    }

    // 自己的窗口成为前台：不算「被忽略」。
    // 同时刻意**不覆盖** lastForegroundPid —— 用户通常是「先切到目标程序，
    // 再从托盘打开设置」，保留上一个外部 PID 才能在选择器里预选中它。
    if (pid != GetCurrentProcessId()) {
        g_app.lastForegroundPid = pid;
    }
    // 刻意不 PostMessage：ignoreActive 由 WM_TIMER 低频刷新，
    // 前台变化回调保持 O(1)，不往键盘钩子所在的消息队列里塞任何东西。
}

BOOL Ignore_InstallWatcher(void)
{
    if (g_app.hWinEvent != NULL) {
        return TRUE;   // 已安装，幂等
    }

    g_app.hWinEvent = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL,                  // 不需要 DLL，回调在本进程内
        OnForegroundEvent,
        0, 0,                  // 所有进程、所有线程
        WINEVENT_OUTOFCONTEXT);

    return (g_app.hWinEvent != NULL);
}

void Ignore_UninstallWatcher(void)
{
    if (g_app.hWinEvent != NULL) {
        UnhookWinEvent(g_app.hWinEvent);
        g_app.hWinEvent = NULL;
    }
    g_app.ignoreActive = FALSE;
}
