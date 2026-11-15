// ---------------------------------------------------------------------------
// apppicker.cpp - 「选择运行中的程序」窗口
//
// 用途：给忽略列表挑一个进程。
//
// 为什么不做「点按钮就抓当前前台窗口」：
// 用户只要一动鼠标去点设置窗口，前台就已经变成 CapsSwitch 自己了，
// 抓到的永远是错误答案。列出现有窗口让他挑，是唯一不会抓错的交互。
//
// 实现上是一个自己管消息循环的模态窗口（不用资源对话框模板，与本工程
// 其余窗口保持一致）。嵌套消息循环期间，托盘、键盘钩子等消息照常派发，
// 因为它们都挂在主线程的消息队列上。
// ---------------------------------------------------------------------------

#include "app.h"

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------

static const wchar_t* const kPickerClass = L"CapsSwitch.AppPicker";

static HWND  g_hPicker = NULL;
static HWND  g_hList   = NULL;
static BOOL  g_accepted = FALSE;
static wchar_t g_result[CAPS_IGNORE_NAME_LEN];

/** 一条候选项：进程文件名 + 一个用于辨认的窗口标题。 */
struct PickerItem {
    wchar_t exe[CAPS_IGNORE_NAME_LEN];
    wchar_t title[96];
};

/** 候选上限。正常桌面同时有窗口的程序远少于这个数。 */
#define PICKER_MAX_ITEMS 64

static PickerItem g_items[PICKER_MAX_ITEMS];
static int        g_itemCount = 0;

// ---------------------------------------------------------------------------
// 候选收集
// ---------------------------------------------------------------------------

/**
 * 判断某个窗口是不是桌面外壳的「假窗口」（Program Manager / 任务栏等）。
 *
 * 它们确实会出现在 EnumWindows 里，但显然不该出现在「要忽略的程序」列表中。
 */
static BOOL IsShellWindow(HWND hwnd, const wchar_t* exe)
{
    wchar_t cls[64] = {0};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0) {
        if (lstrcmpiW(cls, L"Progman") == 0 ||
            lstrcmpiW(cls, L"WorkerW") == 0 ||
            lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
            lstrcmpiW(cls, L"Shell_SecondaryTrayWnd") == 0) {
            return TRUE;
        }
    }
    return (lstrcmpiW(exe, L"explorer.exe") == 0 && cls[0] == L'\0');
}

/** EnumWindows 回调：收集有可见窗口的进程，按 exe 名去重。 */
static BOOL CALLBACK EnumAppsProc(HWND hwnd, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    if (GetWindowTextLengthW(hwnd) == 0) {
        return TRUE;
    }
    // 跳过 owned 窗口（工具窗、弹出层），只要真正的顶层主窗口。
    if (GetWindow(hwnd, GW_OWNER) != NULL) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return TRUE;
    }

    wchar_t exe[CAPS_IGNORE_NAME_LEN];
    if (!Process_GetImageBaseName(pid, exe, ARRAYSIZE(exe))) {
        return TRUE;
    }

    // 去重：同一个程序开了多个窗口只保留一条。
    for (int i = 0; i < g_itemCount; ++i) {
        if (lstrcmpiW(g_items[i].exe, exe) == 0) {
            return TRUE;
        }
    }

    if (g_itemCount >= PICKER_MAX_ITEMS) {
        return FALSE;   // 收集满了，停止枚举
    }

    if (IsShellWindow(hwnd, exe)) {
        return TRUE;
    }

    PickerItem* it = &g_items[g_itemCount];
    lstrcpynW(it->exe, exe, ARRAYSIZE(it->exe));
    GetWindowTextW(hwnd, it->title, ARRAYSIZE(it->title));
    ++g_itemCount;

    return TRUE;
}

/** 按 exe 名插入排序（数量很小，插入排序最省事且稳定）。 */
static void SortItems(void)
{
    for (int i = 1; i < g_itemCount; ++i) {
        PickerItem key = g_items[i];
        int j = i - 1;
        while (j >= 0 && lstrcmpiW(g_items[j].exe, key.exe) > 0) {
            g_items[j + 1] = g_items[j];
            --j;
        }
        g_items[j + 1] = key;
    }
}

/** 填充列表并把最近一个外部前台程序预选中。 */
static void FillList(void)
{
    g_itemCount = 0;
    EnumWindows(EnumAppsProc, 0);
    SortItems();

    if (g_hList == NULL) {
        return;
    }

    SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);

    // 到这里才解析「最近一个外部前台程序」的名字。
    // 前台变化回调只记录 PID，因为解析进程名要 OpenProcess、遇到提权
    // 进程还要遍历 Toolhelp 快照 —— 那种开销不能放在输入路径上。
    // 用户点开「添加...」时查一次，慢几十毫秒也完全无感。
    wchar_t recent[CAPS_IGNORE_NAME_LEN];
    recent[0] = L'\0';
    if (g_app.lastForegroundPid != 0) {
        Process_GetImageBaseName(g_app.lastForegroundPid, recent, ARRAYSIZE(recent));
    }

    int preselect = 0;
    for (int i = 0; i < g_itemCount; ++i) {
        wchar_t line[CAPS_IGNORE_NAME_LEN + 100];
        // 形如 "uu.exe — 悠悠远程控制"
        lstrcpynW(line, g_items[i].exe, ARRAYSIZE(line));
        const int used = lstrlenW(line);
        if (used < (int)ARRAYSIZE(line) - 4 && g_items[i].title[0] != L'\0') {
            lstrcpynW(line + used, L"  —  ", (int)ARRAYSIZE(line) - used);
            const int used2 = lstrlenW(line);
            lstrcpynW(line + used2, g_items[i].title, (int)ARRAYSIZE(line) - used2);
        }

        SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)line);

        if (recent[0] != L'\0' &&
            lstrcmpiW(g_items[i].exe, recent) == 0) {
            preselect = i;
        }
    }

    if (g_itemCount > 0) {
        SendMessageW(g_hList, LB_SETCURSEL, (WPARAM)preselect, 0);
    }
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

/** 取当前选中项写入 g_result。返回是否取到。 */
static BOOL TakeSelection(void)
{
    if (g_hList == NULL) {
        return FALSE;
    }

    const int sel = (int)SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || sel >= g_itemCount) {
        return FALSE;
    }

    lstrcpynW(g_result, g_items[sel].exe, ARRAYSIZE(g_result));
    return (g_result[0] != L'\0');
}

static void CreatePickerControls(HWND hwnd)
{
    const int margin = 16;
    const int clientW = 380;
    const int listW = clientW - margin * 2;

    Ui_MakeControl(hwnd, L"STATIC",
                   L"选择要忽略的程序。该程序位于前台时，CapsSwitch 不拦截任何按键"
                   L"（包括 Ctrl+Caps），全部交还给它。",
                   SS_LEFT,
                   margin, margin, listW, 34, 0);

    g_hList = Ui_MakeControl(hwnd, L"LISTBOX", NULL,
                             LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL |
                             WS_TABSTOP | WS_BORDER,
                             margin, 54, listW, 170, 1001);

    Ui_MakeControl(hwnd, L"STATIC",
                   L"没有找到目标程序？让它的窗口先显示出来再打开本对话框。",
                   SS_LEFT | SS_ENDELLIPSIS,
                   margin, 232, listW, 16, 0);

    const int btnW = 76;
    const int btnH = 26;
    const int yBtn = 258;

    Ui_MakeControl(hwnd, L"BUTTON", L"确定",
                   BS_DEFPUSHBUTTON | WS_TABSTOP,
                   clientW - margin - btnW * 2 - 8, yBtn, btnW, btnH, IDOK);
    Ui_MakeControl(hwnd, L"BUTTON", L"取消",
                   BS_PUSHBUTTON | WS_TABSTOP,
                   clientW - margin - btnW, yBtn, btnW, btnH, IDCANCEL);
}

static LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreatePickerControls(hwnd);
        FillList();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (TakeSelection()) {
                g_accepted = TRUE;
                DestroyWindow(hwnd);
            }
            // 没选中就不关窗，让用户继续选（避免点确定后什么都没发生）。
            return 0;

        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;

        case 1001:   // 列表框
            if (HIWORD(wParam) == LBN_DBLCLK) {
                if (TakeSelection()) {
                    g_accepted = TRUE;
                    DestroyWindow(hwnd);
                }
            }
            return 0;

        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }

    case WM_DESTROY:
        g_hList   = NULL;
        g_hPicker = NULL;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL EnsureClassRegistered(void)
{
    static BOOL registered = FALSE;
    if (registered) {
        return TRUE;
    }

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PickerProc;
    wc.hInstance     = g_app.hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = kPickerClass;

    if (RegisterClassExW(&wc) == 0) {
        return FALSE;
    }
    registered = TRUE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// 对外接口
// ---------------------------------------------------------------------------

BOOL AppPicker_Pick(HWND owner, wchar_t* out, int cch)
{
    if (out == NULL || cch <= 0) {
        return FALSE;
    }
    out[0] = L'\0';

    // 同一时刻只允许一个选择器。
    if (g_hPicker != NULL) {
        SetForegroundWindow(g_hPicker);
        return FALSE;
    }

    if (!EnsureClassRegistered()) {
        return FALSE;
    }

    const int clientW = 380;
    const int clientH = 300;
    const DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_CONTROLPARENT;   // 让 IsDialogMessage 支持 Tab 遍历

    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;

    int x = 0;
    int y = 0;
    if (owner != NULL && IsWindow(owner)) {
        RECT orc;
        GetWindowRect(owner, &orc);
        x = orc.left + ((orc.right - orc.left) - winW) / 2;
        y = orc.top  + ((orc.bottom - orc.top) - winH) / 2;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    }

    g_accepted = FALSE;
    g_result[0] = L'\0';

    HWND hwnd = CreateWindowExW(
        exStyle, kPickerClass, L"选择要忽略的程序",
        style, x, y, winW, winH,
        owner, NULL, g_app.hInst, NULL);

    if (hwnd == NULL) {
        return FALSE;
    }

    g_hPicker = hwnd;

    if (owner != NULL && IsWindow(owner)) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);

    // ---- 自己的模态消息循环 ----
    // IsDialogMessage 负责 Tab 遍历、回车触发「确定」、Esc 关闭。
    MSG msg;
    while (g_hPicker != NULL) {
        const BOOL got = GetMessageW(&msg, NULL, 0, 0);
        if (got <= 0) {
            // 主循环收到 WM_QUIT：转发出去，别把它吞掉。
            if (got == 0) {
                PostQuitMessage((int)msg.wParam);
            }
            break;
        }
        if (g_hPicker == NULL) {
            break;
        }
        if (!IsDialogMessageW(g_hPicker, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner != NULL && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    if (g_accepted) {
        lstrcpynW(out, g_result, cch);
    }
    return (g_accepted && out[0] != L'\0');
}
