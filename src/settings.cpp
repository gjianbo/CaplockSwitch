// ---------------------------------------------------------------------------
// settings.cpp - 设置窗口
//
// 用 CreateWindowEx 直接搭控件（不使用资源对话框模板），布局写死在
// OnInit 里，窗口不可调整大小。
//
// 关于「确定 / 取消」的语义：
//   规格书里同时出现了「复选框实时生效」「下拉框选择后即时保存」和
//   「取消放弃未保存修改」三条要求，字面上互相冲突。这里的取舍是：
//     * 任一控件变化立即生效并落盘（满足「实时生效 / 即时保存」）；
//     * 打开窗口时对配置拍一张快照，「取消」把快照写回去（满足
//       「取消放弃未保存修改」——放弃的是本次会话内的改动）；
//     * 「确定」只关闭窗口，不做额外回滚。
// ---------------------------------------------------------------------------

#include "app.h"
#include "resource.h"

// ---------------------------------------------------------------------------
// 布局常量（客户区坐标，单位像素）
// ---------------------------------------------------------------------------

static const int kClientW = 300;
static const int kClientH = 196;
static const int kMargin  = 16;
static const int kRowW    = kClientW - kMargin * 2;
static const int kBtnW    = 76;
static const int kBtnH    = 26;

/** 下拉框 4 个可选项的显示文本，顺序必须与 CapsHotkeyMode 一致。 */
static const wchar_t* const kHotkeyLabels[HOTKEY_MODE_COUNT] = {
    L"Ctrl + Space",
    L"Win + Space",
    L"Shift + Space",
    L"Ctrl + Shift + Space"
};

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

static BOOL       g_classRegistered = FALSE;
static HFONT      g_uiFont          = NULL;
static AppConfig  g_snapshot        = {};   // 打开窗口时的配置快照

/** 各控件句柄，随窗口生命周期存活。 */
static HWND g_hEnable = NULL;
static HWND g_hHotkey = NULL;
static HWND g_hAuto   = NULL;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

/** 取系统界面字体（跟随「显示设置」里的缩放与字号）。 */
static HFONT GetUiFont(void)
{
    if (g_uiFont != NULL) {
        return g_uiFont;
    }

    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);

    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (g_uiFont == NULL) {
        g_uiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    }
    return g_uiFont;
}

/** 把 mode 收敛到合法区间。 */
static int ClampMode(int mode)
{
    if (mode < 0 || mode >= HOTKEY_MODE_COUNT) {
        return HOTKEY_CTRL_SPACE;
    }
    return mode;
}

/** 创建子控件的公共部分，统一挂字体。 */
static HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                        DWORD style, int x, int y, int w, int h, int id)
{
    HWND ctl = CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, g_app.hInst, NULL);

    if (ctl != NULL) {
        SendMessageW(ctl, WM_SETFONT, (WPARAM)GetUiFont(), TRUE);
    }
    return ctl;
}

// ---------------------------------------------------------------------------
// 控件创建与回填
// ---------------------------------------------------------------------------

/** 创建全部子控件，并把当前状态回填到控件上。 */
static void CreateControls(HWND hwnd)
{
    const int yEnable = kMargin;

    g_hEnable = MakeControl(hwnd, L"BUTTON", L"启用 Caps Lock → 输入法切换",
                            BS_AUTOCHECKBOX | WS_TABSTOP,
                            kMargin, yEnable, kRowW, 22, IDC_ENABLE);

    MakeControl(hwnd, L"STATIC", L"切换输入法快捷键：",
                SS_LEFT,
                kMargin, yEnable + 34, kRowW, 18, IDC_LABEL_HOTKEY);

    // CBS_DROPDOWNLIST：只读下拉。创建时的 h 参数决定展开后的列表高度。
    g_hHotkey = MakeControl(hwnd, L"COMBOBOX", NULL,
                            CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                            kMargin, yEnable + 54, kRowW, 180, IDC_HOTKEY);

    g_hAuto = MakeControl(hwnd, L"BUTTON", L"开机自动启动",
                          BS_AUTOCHECKBOX | WS_TABSTOP,
                          kMargin, yEnable + 92, kRowW, 22, IDC_AUTOSTART);

    const int yBtn = kClientH - kMargin - kBtnH;
    MakeControl(hwnd, L"BUTTON", L"确定",
                BS_DEFPUSHBUTTON | WS_TABSTOP,
                kClientW - kMargin - kBtnW * 2 - 8, yBtn, kBtnW, kBtnH, IDC_OK);
    MakeControl(hwnd, L"BUTTON", L"取消",
                BS_PUSHBUTTON | WS_TABSTOP,
                kClientW - kMargin - kBtnW, yBtn, kBtnW, kBtnH, IDC_CANCEL);

    // ---- 填充可选项 ----
    if (g_hHotkey != NULL) {
        for (int i = 0; i < HOTKEY_MODE_COUNT; ++i) {
            SendMessageW(g_hHotkey, CB_ADDSTRING, 0, (LPARAM)kHotkeyLabels[i]);
        }
    }

    // ---- 回填当前状态 ----
    CheckDlgButton(hwnd, IDC_ENABLE,    g_app.enabled   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwnd, IDC_AUTOSTART, g_app.autoStart ? BST_CHECKED : BST_UNCHECKED);
    if (g_hHotkey != NULL) {
        SendMessageW(g_hHotkey, CB_SETCURSEL, (WPARAM)ClampMode(g_app.hotkeyMode), 0);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

/** 处理子控件通知：勾选、下拉选择、按钮。 */
static void OnCommand(HWND hwnd, int id, int code)
{
    switch (id) {
    case IDC_ENABLE:
        // 复选框状态已经由 BS_AUTOCHECKBOX 自己翻转，这里只读结果。
        App_SetEnabled(IsDlgButtonChecked(hwnd, IDC_ENABLE) == BST_CHECKED, TRUE);
        break;

    case IDC_AUTOSTART:
        App_SetAutoStart(IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED, TRUE);
        break;

    case IDC_HOTKEY:
        if (code == CBN_SELCHANGE && g_hHotkey != NULL) {
            const int sel = (int)SendMessageW(g_hHotkey, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                App_SetHotkeyMode(sel, TRUE);
            }
        }
        break;

    case IDC_OK:
        DestroyWindow(hwnd);
        break;

    case IDC_CANCEL:
        // 回滚到打开窗口那一刻的状态，并把回滚结果落盘。
        App_SetEnabled(g_snapshot.enabled, TRUE);
        App_SetHotkeyMode(g_snapshot.hotkeyMode, TRUE);
        App_SetAutoStart(g_snapshot.autoStart, TRUE);
        DestroyWindow(hwnd);
        break;

    default:
        break;
    }
}

static LRESULT CALLBACK SettingsProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;

    case WM_COMMAND:
        OnCommand(hwnd, LOWORD(wParam), HIWORD(wParam));
        return 0;

    // 关闭按钮等价的「取消」：走同一套回滚逻辑。
    case WM_CLOSE:
        OnCommand(hwnd, IDC_CANCEL, 0);
        return 0;

    // 静态文本与复选框需要父窗口的底色，否则会出现灰底方块。
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DESTROY:
        g_app.hSettings = NULL;
        g_hEnable = NULL;
        g_hHotkey = NULL;
        g_hAuto   = NULL;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/** 注册窗口类（进程内只需一次）。 */
static BOOL EnsureClassRegistered(void)
{
    if (g_classRegistered) {
        return TRUE;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = g_app.hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CapsSwitch.SettingsWindow";

    if (RegisterClassExW(&wc) == 0) {
        return FALSE;
    }
    g_classRegistered = TRUE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

void Settings_Open(HWND owner)
{
    // 已打开则只前置，避免出现多个设置窗口。
    if (g_app.hSettings != NULL) {
        SetForegroundWindow(g_app.hSettings);
        return;
    }

    if (!EnsureClassRegistered()) {
        return;
    }

    // 拍快照，供「取消」回滚。
    g_snapshot.enabled    = g_app.enabled;
    g_snapshot.autoStart  = g_app.autoStart;
    g_snapshot.hotkeyMode = g_app.hotkeyMode;

    const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_CONTROLPARENT;   // 让 IsDialogMessage 支持 Tab 遍历

    // 由客户区尺寸反推窗口尺寸，保证布局不随标题栏高度漂移。
    RECT rc = { 0, 0, kClientW, kClientH };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    const int x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(
        exStyle, L"CapsSwitch.SettingsWindow", L"CapsSwitch 设置",
        style, x, y, winW, winH,
        owner, NULL, g_app.hInst, NULL);

    if (hwnd == NULL) {
        return;
    }

    g_app.hSettings = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void Settings_SyncFromState(void)
{
    if (g_app.hSettings == NULL) {
        return;
    }

    CheckDlgButton(g_app.hSettings, IDC_ENABLE,    g_app.enabled   ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hSettings, IDC_AUTOSTART, g_app.autoStart ? BST_CHECKED : BST_UNCHECKED);

    if (g_hHotkey != NULL) {
        SendMessageW(g_hHotkey, CB_SETCURSEL, (WPARAM)ClampMode(g_app.hotkeyMode), 0);
    }
}

void Settings_Shutdown(void)
{
    if (g_uiFont != NULL) {
        DeleteObject(g_uiFont);
        g_uiFont = NULL;
    }
}
