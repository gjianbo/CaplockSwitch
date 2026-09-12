// ---------------------------------------------------------------------------
// app.h - CapsSwitch 全局声明：常量、共享状态、各模块对外接口
//
// CapsSwitch 是一个纯 Win32（不使用 MFC / Qt / .NET）的托盘常驻小程序：
//   * 用 WH_KEYBOARD_LL 低级键盘钩子，把「单独按 Caps Lock」重映射为
//     「切换输入法」快捷键；
//   * Shift + Caps Lock 原样放行给系统，保留原生大写锁定；
//   * Ctrl + Caps Lock 切换「启用 / 暂停」，是程序唯一的键盘唤入口；
//   * 无主窗口，全部交互收敛到托盘图标 + 一个设置窗口。
//
// 本文件只放声明与常量，不产生任何代码（唯一的例外是 g_app 的定义，
// 它放在 main.cpp 中）。
// ---------------------------------------------------------------------------

#pragma once

// 目标系统最低为 Windows 7 / Server 2008 R2。
// 注意：它会影响部分 API 的可用性判断，但不会自动降低工具集对
// 运行时的要求（/MT 静态 CRT 在 Win7 上需要 KB2999226 或更高版本，
// 详见 README.txt 的「兼容性」一节）。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef WINVER
#define WINVER 0x0601
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/** 应用名，用于窗口标题、菜单与提示文案。 */
#define CAPS_APP_NAME          L"CapsSwitch"

/** 单实例互斥体名（带 GUID 前缀，避免与其他软件撞名）。 */
#define CAPS_MUTEX_NAME        L"CapsSwitch_Mutex_{8A1F3C64-2B7E-4D51-9E0A-5C3D7F21B4A9}"

/** 隐藏消息窗口的窗口类名。 */
#define CAPS_WNDCLASS_MAIN     L"CapsSwitch.MainWindow"

/** 托盘图标回调消息（发给隐藏消息窗口）。 */
#define CAPS_WM_TRAY           (WM_APP + 1)

/**
 * 「切换启用 / 暂停」请求（钩子 -> 消息宿主窗口）。
 *
 * Ctrl+Caps 是在钩子回调里识别的，而回调跑在系统输入处理路径上，必须尽快
 * 返回：里面不能做托盘刷新、写配置文件这类事。所以那里只 Post 这条消息，
 * 真正的状态切换交给宿主窗口在正常消息循环里做。
 */
#define CAPS_WM_TOGGLE         (WM_APP + 2)

/** 托盘图标在本进程内的唯一 ID。 */
#define CAPS_TRAY_UID          1

/**
 * 注入按键的签名，写入 INPUT.ki.dwExtraInfo。
 *
 * 钩子回调用它识别「这是自己注入的按键」并直接放行，是防止
 * SendInput -> 钩子 -> 再 SendInput 死循环的第一道防线。
 * 取值本身无意义，只要不与常见软件冲突即可。
 */
#define CAPS_SIGNATURE         ((ULONG_PTR)0x43'53'57'31)   /* 'CSW1' */

// ---------------------------------------------------------------------------
// 命令行自检（--selftest）
// ---------------------------------------------------------------------------

/**
 * 自检开关。
 *
 * 带上它启动时，程序照常走完整个初始化流程（消息宿主窗口 -> 托盘图标 ->
 * 键盘钩子 -> 全局热键），全部成功就立即退出并返回 SELFTEST_OK；任何一步
 * 失败则跳过消息循环，直接用退出码说明是卡在哪一环。**全程不弹任何对话框**。
 *
 * 存在的理由：正常模式下，托盘挂载失败或钩子安装失败都会弹 MessageBox，
 * 而 MessageBox 是阻塞的 —— 无人值守的脚本只能看到「进程还活着」，会把
 * 失败误判成成功。自检模式把「能不能跑起来」这件事收敛成一个退出码。
 *
 * 注意钩子安装不受配置里的启用状态影响、必定被执行到：钩子自启动起常驻
 * （见 Hook_Install）。否则配置写成暂停的机器上，自检会整片漏掉
 * 「钩子被安全软件拦掉」这一类故障。
 *
 * 也可以手工用它排查「为什么这台机器上没生效」：
 *     CapsSwitch-x64.exe --selftest & echo %ERRORLEVEL%
 */
#define CAPS_SELFTEST_SWITCH   L"--selftest"

/**
 * --selftest 的退出码。
 *
 * 0 是唯一表示「初始化链路全通」的值，其余都对应具体的失败环节。
 * 注意这些值与正常启动（不带 --selftest）时的返回码各自独立，不要混用。
 */
enum CapsSelfTestExit {
    SELFTEST_OK              = 0,   /**< 初始化全部成功。 */
    SELFTEST_ERR_MUTEX       = 1,   /**< 单实例互斥体创建失败。 */
    SELFTEST_ERR_ALREADY_RUN = 2,   /**< 已有实例在运行，未能取得互斥体。 */
    SELFTEST_ERR_CLASS       = 3,   /**< 消息宿主窗口类注册失败。 */
    SELFTEST_ERR_WINDOW      = 4,   /**< 消息宿主窗口创建失败。 */
    SELFTEST_ERR_TRAY        = 5,   /**< 托盘图标挂载失败（Shell_NotifyIcon）。 */
    SELFTEST_ERR_HOOK        = 6    /**< 键盘钩子安装失败（SetWindowsHookEx）。 */
};

// ---------------------------------------------------------------------------
// 输入法切换快捷键模式
// ---------------------------------------------------------------------------

/**
 * 可选的「切换输入法」快捷键模式。
 *
 * 取值同时用作 config.ini 中 HotkeyMode 的取值、设置窗口下拉框的
 * 索引，以及 SendKeyboardCombo 的查表下标，三者顺序必须保持一致。
 */
enum CapsHotkeyMode {
    HOTKEY_CTRL_SPACE       = 0,   // Ctrl + Space（Windows 默认输入法切换）
    HOTKEY_WIN_SPACE        = 1,   // Win + Space（Win10/11 输入法循环）
    HOTKEY_SHIFT_SPACE      = 2,   // Shift + Space（部分第三方输入法）
    HOTKEY_CTRL_SHIFT_SPACE = 3,   // Ctrl + Shift + Space（传统中文输入法）
    HOTKEY_MODE_COUNT       = 4    // 哨兵：模式总数
};

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

/** 落盘到 %APPDATA%\CapsSwitch\config.ini 的配置项。 */
struct AppConfig {
    BOOL enabled;      // [General] Enabled    : 是否启用 Caps 替换
    BOOL autoStart;    // [General] AutoStart  : 是否开机自动启动
    int  hotkeyMode;   // [General] HotkeyMode : CapsHotkeyMode 取值 (0-3)
};

/** 把配置恢复为出厂默认值（启用 / 不开机启动 / Ctrl+Space）。 */
void Config_Default(AppConfig* cfg);

/** 取配置文件完整路径；必要时创建 %APPDATA%\CapsSwitch 目录。 */
BOOL Config_GetPath(wchar_t* out, int cch);

/** 读取配置；文件不存在或缺失项一律回落到默认值，不报错。 */
void Config_Load(AppConfig* cfg);

/** 写入配置；失败时静默忽略（配置文件只影响下次启动的初始状态）。 */
void Config_Save(const AppConfig* cfg);

// ---- 开机启动（HKCU\...\Run） ---------------------------------------------

/** 注册表 Run 项中当前是否存在 CapsSwitch 值。 */
BOOL AutoStart_IsEnabled(void);

/** 注册表 Run 项里记录的路径是否就是当前 exe 路径（用于移动后自愈）。 */
BOOL AutoStart_IsPathCurrent(void);

/** 写入 / 删除 Run 项。返回是否操作成功。 */
BOOL AutoStart_Apply(BOOL enable);

// ---------------------------------------------------------------------------
// 键盘钩子
// ---------------------------------------------------------------------------

/**
 * 安装 WH_KEYBOARD_LL 低级键盘钩子。
 *
 * **钩子自启动起全程常驻，暂停时也不卸载。** 原因：Ctrl+Caps 这个切换键
 * 本身也要靠钩子来识别，而它的职责之一正是把程序从暂停状态重新打开 ——
 * 暂停时若把钩子摘掉，就再没有任何键盘通路能唤醒程序了。
 * 于是「启用 / 暂停」只是 g_app.enabled 一个标志位，回调每次读它决定
 * 该重映射还是原样透传。
 *
 * 钩子挂在本线程（即安装它的线程）上，因此调用方必须保证该线程
 * 之后会进入 GetMessage 消息循环，否则钩子不会被派发。
 * 重复调用是安全的（已安装时直接返回 TRUE）。
 */
BOOL Hook_Install(void);

/** 卸载键盘钩子；未安装时无副作用。只在进程退出时调用。 */
void Hook_Uninstall(void);

// ---------------------------------------------------------------------------
// 托盘图标
// ---------------------------------------------------------------------------

/** 挂载托盘图标。返回是否成功。 */
BOOL Tray_Add(HWND owner);

/** 按当前状态刷新图标与 tooltip（状态变化后调用）。 */
void Tray_Refresh(void);

/** 移除托盘图标（退出前调用）。 */
void Tray_Remove(void);

/** 弹出右键菜单，并把用户选择交给 App_HandleCommand 处理。 */
void Tray_ShowMenu(HWND owner);

// ---------------------------------------------------------------------------
// 设置窗口
// ---------------------------------------------------------------------------

/** 打开设置窗口；已打开时只把它前置。 */
void Settings_Open(HWND owner);

/**
 * 把当前 AppState 回填到设置窗口的控件上。
 *
 * 状态可能从托盘菜单、全局热键等窗口之外的路径被改动，
 * 每次 App_Set* 之后都要调一次，避免界面与实际状态不一致。
 * 窗口未打开时是空操作。
 */
void Settings_SyncFromState(void);

/** 释放设置窗口占用的资源（进程退出前调用）。 */
void Settings_Shutdown(void);

// ---------------------------------------------------------------------------
// 应用状态
// ---------------------------------------------------------------------------

/** 进程内共享状态。定义在 main.cpp。 */
struct AppState {
    HINSTANCE hInst;              // 模块实例句柄
    HWND      hWnd;               // 隐藏的消息窗口（托盘回调 / 热键的宿主）
    HWND      hSettings;          // 设置窗口，未打开时为 NULL
    HHOOK     hHook;              // 键盘钩子，未安装时为 NULL
    HICON     hIconOn;            // 启用态图标
    HICON     hIconOff;           // 暂停态图标
    UINT      wmTaskbarCreated;   // Explorer 重启广播，用于重新挂载托盘图标

    BOOL enabled;                 // 当前是否启用 Caps 替换
    BOOL autoStart;               // 当前是否开机启动
    int  hotkeyMode;              // 当前快捷键模式（CapsHotkeyMode）
    BOOL remoteSession;           // 是否运行在远程桌面会话中
};

/** 全局唯一状态实例。 */
extern AppState g_app;

/**
 * 处理托盘菜单 / 热键产生的命令。
 *
 * @param cmd resource.h 中的 IDM_* 常量。
 */
void App_HandleCommand(int cmd);

/**
 * 切换启用 / 暂停状态，与托盘图标左键等效。
 */
void App_ToggleEnabled(void);

/**
 * 设置启用状态。
 *
 * @param enabled 目标状态。
 * @param persist TRUE 时同时写入 config.ini。
 *
 * 只翻标志位，不碰钩子 —— 钩子自启动起常驻，见 Hook_Install。
 * 若钩子在启动时就没能装上（被安全软件拦截等），则拒绝启用：
 * 这种情况下「已启用」只是个假象，不如保持暂停。
 */
void App_SetEnabled(BOOL enabled, BOOL persist);

/**
 * 设置输入法切换快捷键模式。
 *
 * @param mode    CapsHotkeyMode 取值，越界时回落到 Ctrl+Space。
 * @param persist TRUE 时同时写入 config.ini。
 */
void App_SetHotkeyMode(int mode, BOOL persist);

/**
 * 设置开机启动。
 *
 * @param enable  TRUE 写入 Run 项，FALSE 删除。
 * @param persist TRUE 时同时写入 config.ini。
 *
 * 注册表写入失败时不改变内存状态，避免界面与实际不一致。
 */
void App_SetAutoStart(BOOL enable, BOOL persist);
