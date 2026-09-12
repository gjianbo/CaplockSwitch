// ---------------------------------------------------------------------------
// tray.cpp - 托盘图标与右键菜单
//
// 托盘是程序唯一的常驻入口，承担三件事：
//   * 用图标颜色 + tooltip 反映当前状态（启用 / 暂停）；
//   * 左键单击切换状态，右键弹出菜单；
//   * Explorer 重启后自动重新挂载图标（否则图标会凭空消失）。
// ---------------------------------------------------------------------------

#include "app.h"
#include "resource.h"
#include <shellapi.h>   // NOTIFYICONDATAW / Shell_NotifyIconW

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

/** 托盘图标数据，Shell_NotifyIcon 的 NIM_ADD / NIM_MODIFY 共用这份结构。 */
static NOTIFYICONDATAW g_nid = {};

/** 图标当前是否已挂载，用于避免重复 NIM_ADD。 */
static BOOL g_added = FALSE;

// ---------------------------------------------------------------------------
// tooltip
// ---------------------------------------------------------------------------

/**
 * 生成当前状态的 tooltip 文案。
 *
 * 远程桌面会话下追加一句提示：普通权限进程无法拦截提权窗口
 * （如 UAC 弹窗）的键盘输入，需要以管理员身份运行才能覆盖。
 *
 * @param out 输出缓冲区，长度至少为 128 个 wchar_t（NOTIFYICONDATAW.szTip 的容量）。
 * @param cch 缓冲区容量。
 */
static void BuildTooltip(wchar_t* out, int cch)
{
    if (out == NULL || cch <= 0) {
        return;
    }

    lstrcpynW(out, g_app.enabled
        ? L"CapsSwitch：已启用（Caps→输入法，Shift+Caps→大写）"
        : L"CapsSwitch：已暂停（原生 Caps Lock）", cch);

    if (g_app.remoteSession) {
        const int used = lstrlenW(out);
        if (used < cch - 1) {
            lstrcpynW(out + used, L" · 远程桌面环境建议以管理员运行", cch - used);
        }
    }
}

// ---------------------------------------------------------------------------
// 挂载 / 刷新 / 移除
// ---------------------------------------------------------------------------

BOOL Tray_Add(HWND owner)
{
    if (owner == NULL) {
        return FALSE;
    }

    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = owner;
    g_nid.uID              = CAPS_TRAY_UID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = CAPS_WM_TRAY;
    g_nid.hIcon            = g_app.enabled ? g_app.hIconOn : g_app.hIconOff;

    BuildTooltip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));

    g_added = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    return g_added;
}

void Tray_Refresh(void)
{
    if (!g_added) {
        return;
    }

    g_nid.uFlags = NIF_ICON | NIF_TIP;
    g_nid.hIcon  = g_app.enabled ? g_app.hIconOn : g_app.hIconOff;

    BuildTooltip(g_nid.szTip, ARRAYSIZE(g_nid.szTip));

    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void Tray_Remove(void)
{
    if (!g_added) {
        return;
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_added = FALSE;
}

// ---------------------------------------------------------------------------
// 右键菜单
// ---------------------------------------------------------------------------

void Tray_ShowMenu(HWND owner)
{
    if (owner == NULL) {
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (menu == NULL) {
        return;
    }

    // 右侧用 \t 标出键盘等价操作，让用户看得见 Ctrl+Caps 这个切换键。
    // 它不带托盘图标也能用，是程序唯一的键盘唤入口。
    AppendMenuW(menu, MF_STRING, IDM_STATE_ENABLE, L"启用（Caps→输入法）\tCtrl+Caps");
    AppendMenuW(menu, MF_STRING, IDM_STATE_PAUSE,  L"暂停（原生 Caps）\tCtrl+Caps");
    CheckMenuRadioItem(menu, IDM_STATE_ENABLE, IDM_STATE_PAUSE,
                       (UINT)(g_app.enabled ? IDM_STATE_ENABLE : IDM_STATE_PAUSE),
                       MF_BYCOMMAND);

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    AppendMenuW(menu, MF_STRING, IDM_AUTOSTART, L"开机启动");
    if (g_app.autoStart) {
        CheckMenuItem(menu, IDM_AUTOSTART, MF_BYCOMMAND | MF_CHECKED);
    }

    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置...");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    // 经典托盘菜单收尾三件套：先把宿主窗口抢到前台，让菜单失焦时能正常
    // 收起（否则点空白处菜单不会消失）；TPM_RETURNCMD 让 TrackPopupMenu
    // 直接返回命令 ID，省掉一轮 WM_COMMAND。
    POINT pt;
    GetCursorPos(&pt);

    SetForegroundWindow(owner);
    const int cmd = (int)TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        pt.x, pt.y, 0, owner, NULL);
    PostMessageW(owner, WM_NULL, 0, 0);

    DestroyMenu(menu);

    if (cmd != 0) {
        App_HandleCommand(cmd);
    }
}
