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
// 入口
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    // 让托盘图标与窗口按物理像素渲染，避免高 DPI 下被系统拉伸。
    SetProcessDPIAware();

    // ---- 单实例 ----
    HANDLE mutex = CreateMutexW(NULL, TRUE, CAPS_MUTEX_NAME);
    if (mutex == NULL) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL,
                    L"CapsSwitch 已经在运行了。\n请在任务栏通知区域查看它的托盘图标。",
                    CAPS_APP_NAME, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
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
        return 1;
    }

    g_app.hWnd = CreateWindowExW(0, CAPS_WNDCLASS_MAIN, CAPS_APP_NAME, WS_OVERLAPPED,
                                 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (g_app.hWnd == NULL) {
        CloseHandle(mutex);
        return 1;
    }

    // ---- 托盘 ----
    if (!Tray_Add(g_app.hWnd)) {
        MessageBoxW(NULL, L"无法创建托盘图标，程序即将退出。",
                    CAPS_APP_NAME, MB_OK | MB_ICONERROR);
        DestroyWindow(g_app.hWnd);
        CloseHandle(mutex);
        return 1;
    }

    // ---- 键盘钩子（仅在启用状态下安装）----
    if (g_app.enabled && !Hook_Install()) {
        g_app.enabled = FALSE;   // 保持内存状态与真实能力一致，但不落盘
        Tray_Refresh();
        MessageBoxW(NULL,
                    L"键盘钩子安装失败，已临时切换到暂停模式。\n"
                    L"请检查安全软件是否拦截了本程序，再从托盘菜单重新启用。",
                    CAPS_APP_NAME, MB_OK | MB_ICONWARNING);
    }

    // ---- 全局热键 Ctrl+Alt+C ----
    // 失败不致命（可能是被别的软件占了），只是少一个快捷入口。
    RegisterHotKey(g_app.hWnd, IDH_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'C');

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
    Hook_Uninstall();
    UnregisterHotKey(g_app.hWnd, IDH_TOGGLE);
    Tray_Remove();
    DestroyOwnedIcon(&g_app.hIconOn,  g_iconOnShared);
    DestroyOwnedIcon(&g_app.hIconOff, g_iconOffShared);
    Settings_Shutdown();
    CloseHandle(mutex);

    return (int)msg.wParam;
}
