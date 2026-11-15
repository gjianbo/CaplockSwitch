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

/**
 * 「发送输入法切换快捷键」请求（钩子 -> 消息宿主窗口）。
 *
 * 和 CAPS_WM_TOGGLE 同理：SendInput 不能放在低级键盘钩子回调里做。
 * 回调处在系统输入处理路径上，在那里注入按键会重入，且一旦输入法
 * 吃掉修饰键的 KEYUP，Ctrl 就会卡在按下状态，把后续的 Ctrl+V / Ctrl+C
 * 全部带坏。钩子只投递这条消息，注入由宿主窗口在正常消息循环里执行。
 */
#define CAPS_WM_SWITCH_HOTKEY  (WM_APP + 3)

/**
 * 忽略判定的低频刷新定时器 ID。
 *
 * 用 WM_TIMER 而不是「前台变化消息」驱动判定：WM_TIMER 是消息队列里
 * 优先级最低的消息，系统只在队列中没有输入消息时才生成它，因此不会
 * 挤占低级键盘钩子回调的派发。前台变化回调本身只记一个 PID。
 */
#define CAPS_IGNORE_TIMER_ID   1

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
// 忽略应用
// ---------------------------------------------------------------------------

/**
 * 忽略列表的最大条目数。
 *
 * 上限存在的意义是让 AppState 保持定长、不需要堆分配，同时也顺带
 * 挡住配置文件被写坏后撑爆内存。16 条对个人用户足够。
 */
#define CAPS_MAX_IGNORE        16

/** 单个进程名的最大长度（含结尾 '\0'），只存文件名，不存完整路径。 */
#define CAPS_IGNORE_NAME_LEN   64

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

    // [Ignore] App1..AppN：这些程序处于前台时完全不拦截按键。
    // 只保存进程文件名（如 uu.exe），比较时不区分大小写。
    int     ignoreCount;
    wchar_t ignoreApps[CAPS_MAX_IGNORE][CAPS_IGNORE_NAME_LEN];
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

/**
 * 发送一次「切换输入法」快捷键（按当前 HotkeyMode）。
 *
 * **只能在消息宿主窗口收到 CAPS_WM_SWITCH_HOTKEY 后调用**，
 * 绝不能在 LowLevelKeyboardProc 里直接调用 —— 见该消息常量的说明。
 * 注入部分成功时，内部会尝试补发缺失的 KEYUP。
 */
void Hook_SendSwitchHotkey(HWND foreground);

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
// 界面小工具（settings.cpp 实现，供各窗口共用）
// ---------------------------------------------------------------------------

/**
 * 取系统界面字体（跟随「显示设置」里的字号与缩放）。
 *
 * 进程内惰性创建并共享，调用方**不要** DeleteObject —— 生命周期由
 * Settings_Shutdown 统一管理。
 */
HFONT Ui_GetFont(void);

/**
 * 创建一个子控件并挂上界面字体。
 *
 * @param parent 父窗口。
 * @param cls    窗口类名，例如 L"BUTTON" / L"STATIC" / L"LISTBOX"。
 * @param text   初始文本，可为 NULL。
 * @param style  除 WS_CHILD | WS_VISIBLE 之外的样式。
 * @param id     控件 ID（作为 HMENU 传给 CreateWindowEx）。
 * @return 控件句柄；失败返回 NULL。
 */
HWND Ui_MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text,
                    DWORD style, int x, int y, int w, int h, int id);

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

    // ---- 忽略应用 ----
    int     ignoreCount;          // ignoreApps 的有效条目数
    wchar_t ignoreApps[CAPS_MAX_IGNORE][CAPS_IGNORE_NAME_LEN];
    BOOL    ignoreActive;         // 当前前台进程是否命中忽略列表（钩子只读这一位）
    HWINEVENTHOOK hWinEvent;      // 前台窗口变化监听；安装失败时为 NULL
    /**
     * 最近一个外部前台进程的 PID，供应用选择器预选。
     *
     * 只存 PID 而不是解析好的进程名：解析需要 OpenProcess，遇到提权进程
     * 还会退化成遍历全系统的 Toolhelp 快照。前台变化是高频事件，把这种
     * 开销放在输入路径上会拖住同一线程里的键盘钩子。名字改成在用户点
     * 「添加...」时按需解析一次，那里慢一点完全无感。
     */
    DWORD   lastForegroundPid;
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

/**
 * 忽略列表被改动后统一收尾：落盘 -> 同步设置窗口 -> 立刻重判当前前台应用。
 *
 * 设置窗口的「添加 / 移除」按钮调用它。之所以要立刻重判，是因为用户很可能
 * 就是站在那个被忽略的程序窗口前改的设置，等下一次切换前台才生效会显得没反应。
 */
void App_SaveIgnoreList(void);

// ---------------------------------------------------------------------------
// 忽略应用
// ---------------------------------------------------------------------------
//
// 语义：**列表中的进程处于前台时，钩子对该进程下的 Caps 按键完全不干预** ——
// 包括 Ctrl+Caps 在内，全部原样透传给系统，仿佛 CapsSwitch 没有运行。
//
// 典型场景：悠悠远程 / ToDesk 这类远程控制软件自己也要用 Caps Lock，
// 而全局低级键盘钩子挂在所有程序之前，不主动让路就会把按键抢走。

/**
 * 取指定进程的可执行文件名（不含路径）。
 *
 * @param pid 进程 ID。
 * @param out 输出缓冲区。
 * @param cch 缓冲区容量。
 * @return 取到非空文件名时返回 TRUE。
 *
 * 用 QueryFullProcessImageNameW 而不是 psapi 的 GetModuleFileNameEx，
 * 后者需要 PROCESS_VM_READ 权限、对 64 位进程还可能失败；
 * PROCESS_QUERY_LIMITED_INFORMATION 在 Win7 及以后都能拿到路径。
 */
BOOL Process_GetImageBaseName(DWORD pid, wchar_t* out, int cch);

/**
 * 把用户输入/配置文件里的名字规范化成可直接比较的进程名。
 *
 * 规则：去掉目录部分、去掉首尾空白、没有扩展名时补 .exe。
 * 不做大小写转换 —— 比较一律走 lstrcmpiW，保留原始大小写更利于显示。
 *
 * @param in  原始字符串，可以是完整路径。
 * @param out 输出缓冲区。
 * @param cch 缓冲区容量。
 * @return 规范化后非空时返回 TRUE。
 */
BOOL Ignore_Normalize(const wchar_t* in, wchar_t* out, int cch);

/** 安装前台窗口变化监听（异步回调，不在键盘输入路径上）。返回是否成功。 */
BOOL Ignore_InstallWatcher(void);

/** 卸载前台窗口变化监听；未安装时无副作用。 */
void Ignore_UninstallWatcher(void);

/**
 * 重扫一次系统进程，把所有「名字命中忽略列表」的进程 PID 收进内部集合。
 *
 * 这是**唯一**会调用 OpenProcess / Toolhelp 快照的地方，只在启动、
 * 用户修改忽略列表、以及低频定时器里执行 —— 都不在键盘输入路径上。
 * 运行时判定完全靠 PID 整数比较，不碰任何进程 API。
 *
 * 需要重扫的原因：进程可能是在程序启动之后才运行的，PID 那时还不存在。
 */
void Ignore_RebuildPidSet(void);

/**
 * 重算 ignoreActive，只做一次 PID 整数查找，**不碰进程 API、不碰界面**。
 *
 * 由 WM_TIMER 驱动，也会在启动、保存忽略列表等场合直接调用。
 * 绝对不能在这里调用 Tray_Refresh / Shell_NotifyIcon：托盘图标与 tooltip
 * 都不依赖 ignoreActive，那次刷新没有可见效果，却是同步跨进程调用，
 * 会拖住同一线程里的键盘钩子派发。
 */
void Ignore_Recompute(void);

/** 把配置里的忽略列表灌进 g_app，并按当前前台重算一次。 */
void Ignore_ApplyConfig(const AppConfig* cfg);

/** 把 g_app 里的忽略列表写进配置结构（供落盘 / 快照使用）。 */
void Ignore_WriteConfig(AppConfig* cfg);

/** 条目数。 */
int Ignore_Count(void);

/** 取第 index 条的名字；越界返回 NULL。 */
const wchar_t* Ignore_Get(int index);

/** 追加一条（自动规范化、去重、受上限约束）。返回是否真的加入了。 */
BOOL Ignore_Add(const wchar_t* name);

/** 删除第 index 条。返回是否删除成功。 */
BOOL Ignore_RemoveAt(int index);

/** 清空列表。 */
void Ignore_Clear(void);

// ---------------------------------------------------------------------------
// 应用选择器
// ---------------------------------------------------------------------------

/**
 * 弹出「选择运行中的程序」窗口，让用户挑一个进程加入忽略列表。
 *
 * 之所以不采用「点击按钮后抓当前前台窗口」的做法：用户只要一动鼠标去点
 * 设置窗口，前台就已经变成 CapsSwitch 自己了，抓到的永远是错误答案。
 * 列出现有窗口让他挑是唯一不会抓错的交互。
 *
 * 最近一个外部前台进程会被预选中 —— 用户通常是「先切到目标程序，
 * 再从托盘打开设置」，这样他只要连点两下确定。
 *
 * @param owner 父窗口，非模态期间会被禁用。
 * @param out   选中项的输出缓冲区（进程文件名）。
 * @param cch   缓冲区容量。
 * @return 用户点了「确定」并选中一项时返回 TRUE；取消返回 FALSE。
 */
BOOL AppPicker_Pick(HWND owner, wchar_t* out, int cch);
