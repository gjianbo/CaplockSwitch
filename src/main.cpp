// ---------------------------------------------------------------------------
// main.cpp - 程序入口、消息循环、状态中枢
//
// 进程结构：
//   * 一个不可见的普通窗口作为「消息宿主」——它负责接收托盘回调
//     （CAPS_WM_TRAY）和全局热键（WM_HOTKEY），也是托盘菜单的宿主。
//     用普通窗口而不是 HWND_MESSAGE 消息窗口，是因为 SetForegroundWindow
//     对消息窗口无效，会导致托盘菜单失焦后收不起来。
//   * 全部状态变更走 App_Set* 这一组函数，它们统一负责：改内存状态 ->
//     同步托盘图标 -> 同步设置窗口控件 -> 按需落盘。避免出现「界面显示
//     已启用、实际钩子没装」这类不一致。
// ---------------------------------------------------------------------------

#include "app.h"
#include "resource.h"
#include <commctrl.h>   // InitCommonControlsEx

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------

AppState g_app = {};

/** 两个图标是否为系统共享句柄（LoadIcon 的返回值不可 DestroyIcon）。 */
static BOOL g_iconOnShared  = FALSE;
static BOOL g_iconOffShared = FALSE;

// ---------------------------------------------------------------------------
// 图标
// ---------------------------------------------------------------------------

/**
 * 按托盘所需尺寸（SM_CXSMICON）加载图标。
 *
 * 明确指定尺寸而不是用 LoadIcon，是为了让 16x16 那一档被直接选中，
 * 而不是把 32x32 缩下来 —— 后者在高 DPI 下会发糊。
 *
 * @param resId          资源 ID。
 * @param outShared      [out] 输出句柄是否为共享句柄。
 */
static HICON LoadAppIcon(int resId, BOOL* outShared)
{
    if (outShared != NULL) {
        *outShared = FALSE;
    }

    const int cx = GetSystemMetrics(SM_CXSMICON);
    const int cy = GetSystemMetrics(SM_CYSMICON);

    HICON icon = (HICON)LoadImageW(g_app.hInst, MAKEINTRESOURCEW(resId),
                                   IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (icon != NULL) {
        return icon;
    }

    // 兜底：LoadIcon 返回共享句柄，别去 DestroyIcon。
    icon = LoadIconW(g_app.hInst, MAKEINTRESOURCEW(resId));
    if (icon != NULL && outShared != NULL) {
        *outShared = TRUE;
    }
    return icon;
}

/** 释放 LoadImage 创建的图标；共享图标跳过。 */
static void DestroyOwnedIcon(HICON* icon, BOOL shared)
{
    if (*icon != NULL && !shared) {
        DestroyIcon(*icon);
    }
    *icon = NULL;
}

// ---------------------------------------------------------------------------
// 状态中枢
// ---------------------------------------------------------------------------

/** 把内存状态写回 config.ini。 */
static void SaveState(void)
{
    AppConfig cfg;
    cfg.enabled    = g_app.enabled;
    cfg.autoStart  = g_app.autoStart;
    cfg.hotkeyMode = g_app.hotkeyMode;
    Config_Save(&cfg);
}

void App_SetEnabled(BOOL enabled, BOOL persist)
{
    enabled = enabled ? TRUE : FALSE;

    if (enabled == g_app.enabled) {
        if (persist) {
            SaveState();
        }
        return;
    }

    if (enabled) {
        if (!Hook_Install()) {
            // 钩子装不上（极少见，通常是被安全软件拦截）——
            // 保持暂停状态，避免出现「显示已启用但实际不生效」的假象。
            MessageBoxW(NULL,
                        L"无法安装键盘钩子，CapsSwitch 仍处于暂停状态。\n"
                        L"请检查安全软件是否拦截了本程序，然后重试。",
                        CAPS_APP_NAME, MB_OK | MB_ICONWARNING);
            Settings_SyncFromState();
            return;
        }
    } else {
        Hook_Uninstall();
    }

    g_app.enabled = enabled;
    Tray_Refresh();
    Settings_SyncFromState();

    if (persist) {
        SaveState();
    }
}

void App_SetHotkeyMode(int mode, BOOL persist)
{
    if (mode < 0 || mode >= HOTKEY_MODE_COUNT) {
        mode = HOTKEY_CTRL_SPACE;
    }

    if (mode == g_app.hotkeyMode) {
        if (persist) {
            SaveState();
        }
        return;
    }

    // 无关钩子，无需重装：钩子回调每次都读 g_app.hotkeyMode。
    g_app.hotkeyMode = mode;
    Settings_SyncFromState();

    if (persist) {
        SaveState();
    }
}

void App_SetAutoStart(BOOL enable, BOOL persist)
{
    enable = enable ? TRUE : FALSE;

    if (enable == g_app.autoStart) {
        if (persist) {
            SaveState();
        }
        return;
    }

    // 注册表是开机启动的唯一事实来源；写失败就不改内存状态，
    // 保证菜单勾选 / 复选框与实际行为永远一致。
    if (!AutoStart_Apply(enable)) {
        MessageBoxW(NULL,
                    L"无法写入开机启动项，操作已取消。\n"
                    L"请确认当前账户对 HKEY_CURRENT_USER 有写权限。",
                    CAPS_APP_NAME, MB_OK | MB_ICONWARNING);
        Settings_SyncFromState();
        return;
    }

    g_app.autoStart = enable;
    Settings_SyncFromState();

    if (persist) {
        SaveState();
    }
}

void App_ToggleEnabled(void)
{
    App_SetEnabled(!g_app.enabled, TRUE);
}

void App_HandleCommand(int cmd)
{
    switch (cmd) {
    case IDM_STATE_ENABLE:
        App_SetEnabled(TRUE, TRUE);
        break;
    case IDM_STATE_PAUSE:
        App_SetEnabled(FALSE, TRUE);
        break;
    case IDM_AUTOSTART:
        App_SetAutoStart(!g_app.autoStart, TRUE);
        break;
    case IDM_SETTINGS:
        Settings_Open(g_app.hWnd);
        break;
    case IDM_EXIT:
        DestroyWindow(g_app.hWnd);   // 触发 WM_DESTROY -> PostQuitMessage
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// 消息宿主窗口
// ---------------------------------------------------------------------------

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // Explorer 崩溃重启后会广播 TaskbarCreated，此时必须重新挂载图标，
    // 否则托盘图标会永久消失（Windows 已把旧图标清掉了）。
    if (msg == g_app.wmTaskbarCreated && g_app.wmTaskbarCreated != 0) {
        Tray_Add(hwnd);
        return 0;
    }

    switch (msg) {
    case CAPS_WM_TRAY:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            App_ToggleEnabled();
            break;
        case WM_RBUTTONUP:
            Tray_ShowMenu(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_HOTKEY:
        if ((int)wParam == IDH_TOGGLE) {
            App_ToggleEnabled();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/** 注册消息宿主窗口类。 */
static BOOL RegisterMainClass(void)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = g_app.hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   // 窗口永不显示，不需要背景刷
    wc.lpszClassName = CAPS_WNDCLASS_MAIN;

    return (RegisterClassExW(&wc) != 0);
}

// ---------------------------------------------------------------------------
// 命令行与收尾
// ---------------------------------------------------------------------------

/**
 * 把 ASCII 小写字母转成大写，其余字符原样返回。
 *
 * @param c 待转换字符。
 * @return 转换结果。
 */
static wchar_t UpperAscii(wchar_t c)
{
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
}

/**
 * 判断命令行里是否出现了指定开关。
 *
 * 按空白切词后做「整词 + 不区分大小写」的比较：整词匹配可以避免
 * --selftest-verbose 这类前缀相同的参数被误判成 --selftest；
 * 带引号的参数（exe 自身路径通常带引号）会先剥掉引号再比较。
 *
 * 这里刻意不用 CRT 的 wcsstr / _wcsicmp —— 本工程有一条完全不链接
 * CRT 的构建路径（见 crt_free.cpp），选型上要整体避开 CRT 函数。
 *
 * @param cmdLine GetCommandLineW() 的返回值，含 exe 自身路径。
 * @param sw      要匹配的开关，例如 CAPS_SELFTEST_SWITCH。
 * @return 该开关作为独立的一个词出现时返回 TRUE。
 */
static BOOL HasSwitch(LPCWSTR cmdLine, LPCWSTR sw)
{
    if (cmdLine == NULL || sw == NULL) {
        return FALSE;
    }

    const int swLen = lstrlenW(sw);

    for (LPCWSTR p = cmdLine; *p != L'\0'; ) {
        while (*p == L' ' || *p == L'\t') {
            ++p;
        }
        if (*p == L'\0') {
            break;
        }

        LPCWSTR word = p;
        if (*word == L'"') {
            ++word;
            while (*p != L'\0' && *p != L'"') {
                ++p;
            }
            if (*p == L'"') {
                ++p;
            }
        } else {
            while (*p != L'\0' && *p != L' ' && *p != L'\t') {
                ++p;
            }
        }

        if ((int)(p - word) != swLen) {
            continue;
        }

        int i = 0;
        for (; i < swLen; ++i) {
            if (UpperAscii(word[i]) != UpperAscii(sw[i])) {
                break;
            }
        }
        if (i == swLen) {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * 进程退出前的统一收尾。
 *
 * 每一步都自带「尚未初始化」的判断，所以在任何阶段调用都是安全的 ——
 * 这正是自检模式需要的：还没进消息循环就得能干净退出。
 * 调用后 g_app.hWnd 会被置空，重复调用不会二次 DestroyWindow。
 */
static void Teardown(void)
{
    Hook_Uninstall();          // 未安装时空操作

    if (g_app.hWnd != NULL) {
        UnregisterHotKey(g_app.hWnd, IDH_TOGGLE);
    }

    Tray_Remove();             // 未挂载时空操作

    DestroyOwnedIcon(&g_app.hIconOn,  g_iconOnShared);
    DestroyOwnedIcon(&g_app.hIconOff, g_iconOffShared);

    Settings_Shutdown();       // 幂等：内部有 NULL 判断

    if (g_app.hWnd != NULL) {
        DestroyWindow(g_app.hWnd);
        g_app.hWnd = NULL;
    }
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 让托盘图标与窗口按物理像素渲染，避免高 DPI 下被系统拉伸。
    SetProcessDPIAware();

    // 自检模式：走完初始化后立即退出，用退出码报告结果，全程不弹对话框。
    // 详见 app.h 中 CAPS_SELFTEST_SWITCH 的说明。
    const BOOL selftest = HasSwitch(GetCommandLineW(), CAPS_SELFTEST_SWITCH);

    // ---- 单实例 ----
    HANDLE mutex = CreateMutexW(NULL, TRUE, CAPS_MUTEX_NAME);
    if (mutex == NULL) {
        return SELFTEST_ERR_MUTEX;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        if (selftest) {
            return SELFTEST_ERR_ALREADY_RUN;
        }
        MessageBoxW(NULL,
                    L"CapsSwitch 已经在运行了。\n请在任务栏通知区域查看它的托盘图标。",
                    CAPS_APP_NAME, MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // 声明使用 comctl32 v6（配合 manifest），让按钮 / 下拉框走系统主题。
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_app.hInst          = hInstance;
    g_app.wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.remoteSession  = (GetSystemMetrics(SM_REMOTESESSION) != 0);

    // ---- 配置 ----
    AppConfig cfg;
    Config_Load(&cfg);
    g_app.enabled    = cfg.enabled;
    g_app.hotkeyMode = cfg.hotkeyMode;

    // 开机启动以注册表为准（config.ini 里的 AutoStart 只是记录）。
    g_app.autoStart = AutoStart_IsEnabled();

    // exe 被移动过：开机启动项里还是旧路径，这里顺手修掉。
    if (g_app.autoStart && !AutoStart_IsPathCurrent()) {
        if (AutoStart_Apply(TRUE)) {
            SaveState();
        }
    }

    // ---- 图标 ----
    g_app.hIconOn  = LoadAppIcon(IDI_CAPSSWITCH_ON,  &g_iconOnShared);
    g_app.hIconOff = LoadAppIcon(IDI_CAPSSWITCH_OFF, &g_iconOffShared);

    // ---- 消息宿主窗口（创建后不 ShowWindow，保持隐藏）----
    if (!RegisterMainClass()) {
        CloseHandle(mutex);
        return SELFTEST_ERR_CLASS;
    }

    g_app.hWnd = CreateWindowExW(0, CAPS_WNDCLASS_MAIN, CAPS_APP_NAME, WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (g_app.hWnd == NULL) {
        CloseHandle(mutex);
        return SELFTEST_ERR_WINDOW;
    }

    // ---- 托盘 ----
    if (!Tray_Add(g_app.hWnd)) {
        if (!selftest) {
            MessageBoxW(NULL, L"无法创建托盘图标，程序即将退出。",
                        CAPS_APP_NAME, MB_OK | MB_ICONERROR);
        }
        Teardown();
        CloseHandle(mutex);
        return SELFTEST_ERR_TRAY;
    }

    // ---- 键盘钩子（仅在启用状态下安装）----
    if (g_app.enabled && !Hook_Install()) {
        g_app.enabled = FALSE;   // 保持内存状态与真实能力一致，但不落盘
        Tray_Refresh();

        if (selftest) {
            // 自检模式下这是硬失败：把 Caps 重映射出去是本程序存在的全部
            // 意义，钩子装不上就没有可交付的东西了。
            Teardown();
            CloseHandle(mutex);
            return SELFTEST_ERR_HOOK;
        }

        MessageBoxW(NULL,
                    L"键盘钩子安装失败，已临时切换到暂停模式。\n"
                    L"请检查安全软件是否拦截了本程序，再从托盘菜单重新启用。",
                    CAPS_APP_NAME, MB_OK | MB_ICONWARNING);
    }

    // ---- 全局热键 Ctrl+Alt+C ----
    // 失败不致命（可能是被别的软件占了），只是少一个快捷入口。
    RegisterHotKey(g_app.hWnd, IDH_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');

    // ---- 自检模式在此收工 ----
    // 能走到这里，说明消息宿主窗口、托盘图标、键盘钩子、全局热键四条链路
    // 都已就绪。不需要进消息循环，清理后返回 0 即可。
    if (selftest) {
        Teardown();
        CloseHandle(mutex);
        return SELFTEST_OK;
    }

    // ---- 消息循环 ----
    // 设置窗口不是真正的对话框，靠 IsDialogMessage 补上 Tab 遍历、
    // 回车触发默认按钮、Esc 关闭这些「对话框行为」。
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_app.hSettings == NULL || !IsDialogMessageW(g_app.hSettings, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ---- 收尾 ----
    Teardown();
    CloseHandle(mutex);

    return (int)msg.wParam;
}
