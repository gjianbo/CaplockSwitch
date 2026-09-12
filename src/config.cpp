// ---------------------------------------------------------------------------
// config.cpp - 配置持久化
//
// 包含两块彼此独立、但都归「记住用户设置」这件事管辖的逻辑：
//   1. %APPDATA%\CapsSwitch\config.ini 的读写（Enabled / AutoStart / HotkeyMode）；
//   2. HKCU\Software\Microsoft\Windows\CurrentVersion\Run 的开关机启动项。
//
// 设计原则：这一层永不弹窗、永不失败退出。配置读不到就用默认值，
// 写不进就静默忽略——配置文件只决定「下次启动时的初始状态」，
// 不该因为磁盘或权限问题把主功能拖下水。
// ---------------------------------------------------------------------------

#include "app.h"

// ---------------------------------------------------------------------------
// 小工具（刻意不使用 CRT 的 printf / itoa，避免把格式化引擎链进 exe）
// ---------------------------------------------------------------------------

/**
 * 把非负/负数按十进制写入缓冲区。调用方需保证 out 至少 12 个 wchar_t。
 */
static void IntToStrW(int value, wchar_t* out)
{
    if (value == 0) {
        out[0] = L'0';
        out[1] = L'\0';
        return;
    }

    wchar_t tmp[12];
    int n = 0;
    const BOOL negative = (value < 0);
    unsigned int u = negative ? (unsigned int)(-value) : (unsigned int)value;

    while (u != 0 && n < 11) {
        tmp[n++] = (wchar_t)(L'0' + (u % 10u));
        u /= 10u;
    }

    int i = 0;
    if (negative) {
        out[i++] = L'-';
    }
    while (n > 0) {
        out[i++] = tmp[--n];
    }
    out[i] = L'\0';
}

/** 拼接 a + '\\' + b，始终以 '\0' 结尾。 */
static void JoinPathW(wchar_t* dst, int cch, const wchar_t* a, const wchar_t* b)
{
    if (cch <= 0) {
        return;
    }
    lstrcpynW(dst, a, cch);

    int len = lstrlenW(dst);
    if (len < cch - 1) {
        dst[len++] = L'\\';
        dst[len]   = L'\0';
    }

    const int remain = cch - len;
    if (remain > 0) {
        lstrcpynW(dst + len, b, remain);
    }
}

// ---------------------------------------------------------------------------
// config.ini
// ---------------------------------------------------------------------------

/** config.ini 所在的子目录名（位于 %APPDATA% 之下）。 */
static const wchar_t* const kAppDataSubDir = L"CapsSwitch";
static const wchar_t* const kConfigFile   = L"config.ini";
static const wchar_t* const kIniSection   = L"General";

void Config_Default(AppConfig* cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->enabled    = TRUE;
    cfg->autoStart  = FALSE;
    cfg->hotkeyMode = HOTKEY_CTRL_SPACE;
}

BOOL Config_GetPath(wchar_t* out, int cch)
{
    if (out == NULL || cch < MAX_PATH) {
        return FALSE;
    }

    wchar_t appData[MAX_PATH];
    if (GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) {
        return FALSE;   // 环境变量缺失（极端情况），放弃持久化
    }

    wchar_t dir[MAX_PATH];
    JoinPathW(dir, MAX_PATH, appData, kAppDataSubDir);

    // 目录已存在时 CreateDirectory 返回 ERROR_ALREADY_EXISTS，属正常。
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return FALSE;
    }

    JoinPathW(out, cch, dir, kConfigFile);
    return TRUE;
}

void Config_Load(AppConfig* cfg)
{
    if (cfg == NULL) {
        return;
    }

    Config_Default(cfg);

    wchar_t path[MAX_PATH];
    if (!Config_GetPath(path, MAX_PATH)) {
        return;
    }

    cfg->enabled   = GetPrivateProfileIntW(kIniSection, L"Enabled",   cfg->enabled,   path) ? TRUE : FALSE;
    cfg->autoStart = GetPrivateProfileIntW(kIniSection, L"AutoStart", cfg->autoStart, path) ? TRUE : FALSE;
    cfg->hotkeyMode = (int)GetPrivateProfileIntW(kIniSection, L"HotkeyMode", (UINT)cfg->hotkeyMode, path);

    // 防止配置文件被手工改坏后越界。
    if (cfg->hotkeyMode < 0 || cfg->hotkeyMode >= HOTKEY_MODE_COUNT) {
        cfg->hotkeyMode = HOTKEY_CTRL_SPACE;
    }
}

void Config_Save(const AppConfig* cfg)
{
    if (cfg == NULL) {
        return;
    }

    wchar_t path[MAX_PATH];
    if (!Config_GetPath(path, MAX_PATH)) {
        return;
    }

    wchar_t mode[12];
    IntToStrW(cfg->hotkeyMode, mode);

    WritePrivateProfileStringW(kIniSection, L"Enabled",   cfg->enabled   ? L"1" : L"0", path);
    WritePrivateProfileStringW(kIniSection, L"AutoStart", cfg->autoStart ? L"1" : L"0", path);
    WritePrivateProfileStringW(kIniSection, L"HotkeyMode", mode, path);
}

// ---------------------------------------------------------------------------
// 开机启动（HKCU Run 项）
// ---------------------------------------------------------------------------

static const wchar_t* const kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* const kRunValue   = L"CapsSwitch";

BOOL AutoStart_IsEnabled(void)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return FALSE;
    }

    const LONG rc = RegQueryValueExW(key, kRunValue, NULL, NULL, NULL, NULL);
    RegCloseKey(key);
    return (rc == ERROR_SUCCESS);
}

BOOL AutoStart_IsPathCurrent(void)
{
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return FALSE;
    }

    wchar_t stored[MAX_PATH * 2] = {0};
    DWORD cb = sizeof(stored);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, kRunValue, NULL, &type,
                                     (LPBYTE)stored, &cb);
    RegCloseKey(key);

    if (rc != ERROR_SUCCESS || type != REG_SZ) {
        return FALSE;
    }

    // 去掉可能的引号，再与当前 exe 路径做不区分大小写的比较。
    wchar_t* p = stored;
    if (*p == L'"') {
        ++p;
        wchar_t* end = p + lstrlenW(p);
        if (end > p && *(end - 1) == L'"') {
            *(end - 1) = L'\0';
        }
    }

    wchar_t current[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, current, MAX_PATH) == 0) {
        return FALSE;
    }

    return (lstrcmpiW(p, current) == 0);
}

BOOL AutoStart_Apply(BOOL enable)
{
    HKEY key = NULL;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, NULL, 0,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) {
        return FALSE;
    }

    LONG rc;
    if (enable) {
        wchar_t exe[MAX_PATH] = {0};
        if (GetModuleFileNameW(NULL, exe, MAX_PATH) == 0) {
            RegCloseKey(key);
            return FALSE;
        }

        // 路径可能含空格，必须加引号，否则系统会按空格拆成「程序 + 参数」。
        wchar_t quoted[MAX_PATH + 4];
        quoted[0] = L'"';
        lstrcpynW(quoted + 1, exe, MAX_PATH + 2);
        const int len = lstrlenW(quoted);
        quoted[len]     = L'"';
        quoted[len + 1] = L'\0';

        rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)quoted,
                            (DWORD)((len + 2) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(key, kRunValue);
        if (rc == ERROR_FILE_NOT_FOUND) {
            rc = ERROR_SUCCESS;   // 本来就不存在，视作成功
        }
    }

    RegCloseKey(key);
    return (rc == ERROR_SUCCESS);
}
