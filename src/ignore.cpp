// ---------------------------------------------------------------------------
// ignore.cpp - 忽略应用：列表管理 + 前台进程判定 + 前台变化监听
//
// 需求来源：悠悠远程这类远程控制软件自己也要接管 Caps Lock，而低级键盘
// 钩子挂在所有程序之前，不主动让路就会把按键抢走。于是增加一条「白名单」
// 语义：**列表中的程序处于前台时，本程序对 Caps 完全不干预**，包括
// Ctrl+Caps —— 全部原样透传，仿佛 CapsSwitch 没有运行。
//
// 性能约束（重要）：
// 判定结果缓存成 g_app.ignoreActive 一个 BOOL，键盘钩子回调里**只读这一位**。
// 真正的进程查询只发生在两个地方，都不在输入路径上：
//   1. SetWinEventHook 的前台变化回调只记一个 PID，然后投递
//      CAPS_WM_FOREGROUND；窗口过程收到后才做 Ignore_Refresh()
//      （进程名解析 + 忽略判定 + 托盘刷新）。
//   2. 钩子里的兜底分支 —— 仅当第 1 条安装失败时才启用，限频 300ms，
//      且同样只投递消息，连进程名解析都不做。
//   3. 用户点「添加...」时，应用选择器按 PID 现查一次名字用于预选。
//
// 之所以这么绕：前台变化回调与低级键盘钩子共用安装线程的消息循环，
// 在那里 OpenProcess / 遍历 Toolhelp 快照会连键盘事件派发一起拖慢。
//
// 本文件同样遵守「不依赖 CRT」的约定：只用 lstr* 系列，不用 wcscmp/swprintf。
// ---------------------------------------------------------------------------

#include "app.h"
#include <tlhelp32.h>   // CreateToolhelp32Snapshot：取进程名的兜底通道

// ---------------------------------------------------------------------------
// 前台进程名缓存（放在文件前部，Ignore_Clear 也要用）
// ---------------------------------------------------------------------------

/**
 * 前台变化是高频事件（截图工具弹工具条、切换窗口都会触发），但同一个
 * 进程的名字不会变。按 PID 缓存后，OpenProcess / Toolhelp 快照每个进程
 * 最多走一次；同进程内反复切换前台只剩一次字符串比较。
 */
static DWORD   g_cachedPid  = 0;
static wchar_t g_cachedName[CAPS_IGNORE_NAME_LEN] = L"";

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
    g_cachedPid        = 0;
    g_cachedName[0]    = L'\0';
}

// ---------------------------------------------------------------------------
// 判定
// ---------------------------------------------------------------------------

void Ignore_Recompute(void)
{
    BOOL active = FALSE;

    if (g_app.ignoreCount > 0) {
        HWND fg = GetForegroundWindow();
        if (fg != NULL) {
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);

            if (pid != 0) {
                if (pid != g_cachedPid) {
                    wchar_t name[CAPS_IGNORE_NAME_LEN];
                    name[0] = L'\0';
                    if (Process_GetImageBaseName(pid, name, ARRAYSIZE(name))) {
                        lstrcpynW(g_cachedName, name, ARRAYSIZE(g_cachedName));
                        g_cachedPid = pid;
                    } else {
                        // 查不到就不缓存 PID，下次还有机会重试。
                        g_cachedPid = 0;
                        g_cachedName[0] = L'\0';
                    }
                }

                if (g_cachedName[0] != L'\0') {
                    for (int i = 0; i < g_app.ignoreCount; ++i) {
                        // 大小写不敏感：Windows 文件名本来就不区分大小写，
                        // 用户在 ini 里写成 UU.EXE 也该生效。
                        if (lstrcmpiW(g_cachedName, g_app.ignoreApps[i]) == 0) {
                            active = TRUE;
                            break;
                        }
                    }
                }
            }
        }
    }

    g_app.ignoreActive = active;
}

void Ignore_Refresh(void)
{
    const BOOL before = g_app.ignoreActive;
    Ignore_Recompute();

    // 只在状态翻转时刷新托盘，避免每次前台变化都做一次 Shell_NotifyIcon。
    if (before != g_app.ignoreActive) {
        Tray_Refresh();
    }
}

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
 * 因此本回调只做两件 O(1) 的事：记下前台进程 PID、投递一条消息。
 * 进程名解析留给应用选择器按需执行，忽略判定与托盘刷新留给窗口过程。
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

    if (g_app.hWnd != NULL) {
        PostMessageW(g_app.hWnd, CAPS_WM_FOREGROUND, 0, 0);
    }
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
