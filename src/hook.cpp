// ---------------------------------------------------------------------------
// hook.cpp - 键盘重映射核心
//
// 这是整个程序唯一「有魔法」的地方，逻辑集中在这里，方便单独审查。
//
// 决策表（启用状态下，且按键来自真实硬件）：
//
//   按住 Shift            -> 放行，系统执行原生 Caps Lock 大写切换
//   按住 Ctrl / Alt / Win -> 吞掉 Caps 本身，但不发送输入法切换快捷键
//   单独按 Caps           -> 吞掉，并发出一条配置好的输入法切换快捷键
//
// 两条防回环措施缺一不可：
//   1. 注入按键带 CAPS_SIGNATURE 签名，见到即放行（识别「自己」）；
//   2. 任何带 LLKHF_INJECTED 标志的事件一律放行（识别「非硬件」），
//      保证程序只对物理键盘动作做决策。
// ---------------------------------------------------------------------------

#include "app.h"

// ---------------------------------------------------------------------------
// 内部状态
// ---------------------------------------------------------------------------

/**
 * 物理 Caps Lock 是否处于「已按下、尚未抬起」状态。
 *
 * 用来过滤按住不放时系统产生的自动重复（auto-repeat）：只有第一次
 * WM_KEYDOWN 才发快捷键，后续重复按下被静默吞掉，避免连发。
 */
static BOOL g_capsHeld = FALSE;

// ---------------------------------------------------------------------------
// 按键序列
// ---------------------------------------------------------------------------

/** 组合键按键表：元素顺序 = 按下顺序，抬起时逆序执行。 */
static const WORD kComboCtrlSpace[]      = { VK_CONTROL, VK_SPACE };
static const WORD kComboWinSpace[]       = { VK_LWIN,    VK_SPACE };
static const WORD kComboShiftSpace[]     = { VK_SHIFT,   VK_SPACE };
static const WORD kComboCtrlShiftSpace[] = { VK_CONTROL, VK_SHIFT, VK_SPACE };

/**
 * 有任一 Shift 按下吗？
 *
 * 用左右键码分别查询，比 GetAsyncKeyState(VK_SHIFT) 更明确，
 * 也顺带覆盖了「左 Shift + 右 Caps」这类组合。
 */
static BOOL IsShiftDown(void)
{
    return ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0);
}

/**
 * 有任一「非 Shift 的修饰键」按下吗（Ctrl / Alt / Win）？
 *
 * 这些修饰键与 Caps 同按时，按规格只吞掉 Caps，不触发输入法切换。
 */
static BOOL IsOtherModifierDown(void)
{
    return ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_LMENU)    & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_RMENU)    & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_LWIN)     & 0x8000) != 0)
        || ((GetAsyncKeyState(VK_RWIN)     & 0x8000) != 0);
}

/**
 * 注入一组「修饰键 + 主键」组合。
 *
 * 一次性把按下与抬起全部投递（先顺序按下，再逆序抬起），
 * 避免中间被其他输入打断。每个事件都打上 CAPS_SIGNATURE 签名。
 *
 * @param keys  按键序列，顺序即按下顺序。
 * @param count 序列长度，1..4。
 */
static void SendKeyboardCombo(const WORD* keys, int count)
{
    if (keys == NULL || count <= 0 || count > 4) {
        return;
    }

    INPUT input[8];
    ZeroMemory(input, sizeof(input));

    int n = 0;
    for (int i = 0; i < count; ++i) {
        input[n].type           = INPUT_KEYBOARD;
        input[n].ki.wVk         = keys[i];
        input[n].ki.dwExtraInfo = CAPS_SIGNATURE;
        ++n;
    }
    for (int i = count - 1; i >= 0; --i) {
        input[n].type           = INPUT_KEYBOARD;
        input[n].ki.wVk         = keys[i];
        input[n].ki.dwFlags     = KEYEVENTF_KEYUP;
        input[n].ki.dwExtraInfo = CAPS_SIGNATURE;
        ++n;
    }

    SendInput((UINT)n, input, sizeof(INPUT));
}

/**
 * 按当前配置发送一次「切换输入法」快捷键。
 */
static void SendSwitchHotkey(void)
{
    switch (g_app.hotkeyMode) {
    case HOTKEY_WIN_SPACE:
        SendKeyboardCombo(kComboWinSpace, 2);
        break;
    case HOTKEY_SHIFT_SPACE:
        SendKeyboardCombo(kComboShiftSpace, 2);
        break;
    case HOTKEY_CTRL_SHIFT_SPACE:
        SendKeyboardCombo(kComboCtrlShiftSpace, 3);
        break;
    case HOTKEY_CTRL_SPACE:
    default:
        SendKeyboardCombo(kComboCtrlSpace, 2);
        break;
    }
}

// ---------------------------------------------------------------------------
// 钩子回调
// ---------------------------------------------------------------------------

/**
 * WH_KEYBOARD_LL 低级键盘钩子回调。
 *
 * 返回值语义：返回 1（非 0）表示吞掉该事件、不再向下传递；
 * 返回 CallNextHookEx(...) 表示放行。
 */
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    // 规范要求：nCode < 0 时必须原样转发，不能做任何处理。
    if (nCode != HC_ACTION) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
    if (kb == NULL) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- 防回环第 1 道：自己注入的按键 ------------------------------------
    if (kb->dwExtraInfo == CAPS_SIGNATURE) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- 防回环第 2 道：任何来源的注入按键都不是物理输入 --------------------
    if ((kb->flags & LLKHF_INJECTED) != 0) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- 只关心 Caps Lock --------------------------------------------------
    if (kb->vkCode != VK_CAPITAL) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- 暂停模式：完全透传，系统原生处理 ----------------------------------
    if (!g_app.enabled) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    const BOOL isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const BOOL isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp) {
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- Shift + Caps：放行，交给系统做原生大写锁定 ------------------------
    if (IsShiftDown()) {
        g_capsHeld = FALSE;
        return CallNextHookEx(g_app.hHook, nCode, wParam, lParam);
    }

    // ---- Ctrl / Alt / Win + Caps：吞掉 Caps，但不切换输入法 ----------------
    if (IsOtherModifierDown()) {
        g_capsHeld = FALSE;
        return 1;
    }

    // ---- 单独按 Caps -------------------------------------------------------
    if (isDown) {
        // 只在「首次按下」发键；按住不放产生的自动重复被静默吞掉。
        if (!g_capsHeld) {
            g_capsHeld = TRUE;
            SendSwitchHotkey();
        }
        return 1;   // 吞掉 KEYDOWN，阻止系统把大写状态切成 ON
    }

    // isUp：吞掉 KEYUP，避免系统补发一次大写状态切换。
    g_capsHeld = FALSE;
    return 1;
}

// ---------------------------------------------------------------------------
// 安装 / 卸载
// ---------------------------------------------------------------------------

BOOL Hook_Install(void)
{
    if (g_app.hHook != NULL) {
        return TRUE;   // 已安装，幂等
    }

    g_capsHeld = FALSE;

    // WH_KEYBOARD_LL 是全局钩子，但不需要注入 DLL：系统把事件派发到
    // 安装它的线程，因此安装后该线程必须继续跑消息循环。
    g_app.hHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_app.hInst, 0);
    return (g_app.hHook != NULL);
}

void Hook_Uninstall(void)
{
    if (g_app.hHook != NULL) {
        UnhookWindowsHookEx(g_app.hHook);
        g_app.hHook = NULL;
    }
    g_capsHeld = FALSE;
}
