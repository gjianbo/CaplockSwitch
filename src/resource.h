// ---------------------------------------------------------------------------
// resource.h - CapsSwitch 资源标识符
//
// 本文件只放 #define，由 C++ 源码与 CapsSwitch.rc 共用，
// 因此不要在这里包含 windows.h 之类的重型头文件。
// ---------------------------------------------------------------------------
#pragma once

// ---- 图标资源（编译进 exe，源文件见 icons/ 目录） -------------------------
#define IDI_CAPSSWITCH_ON        101    // 启用态图标
#define IDI_CAPSSWITCH_OFF       102    // 暂停态图标

// ---- 托盘右键菜单命令 ID --------------------------------------------------
#define IDM_STATE_ENABLE         201    // 单选：启用（Caps -> 输入法）
#define IDM_STATE_PAUSE          202    // 单选：暂停（原生 Caps Lock）
#define IDM_AUTOSTART            203    // 勾选：开机启动
#define IDM_SETTINGS             204    // 设置...
#define IDM_EXIT                 205    // 退出

// ---- 全局热键 ID ----------------------------------------------------------
#define IDH_TOGGLE               301    // Ctrl + Alt + C（与 Ctrl+Caps 等效）

// ---- 设置窗口子控件 ID ----------------------------------------------------
#define IDC_ENABLE               401
#define IDC_HOTKEY               402
#define IDC_AUTOSTART            403
#define IDC_OK                   404
#define IDC_CANCEL               405
#define IDC_LABEL_HOTKEY         406
#define IDC_LABEL_TOGGLEHINT     407    // 纯说明文字，不接收通知
