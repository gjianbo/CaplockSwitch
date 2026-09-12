// ---------------------------------------------------------------------------
// crt_free.cpp - 无 C 运行库（CRT）的进程入口与最小运行时支撑
//
// 为什么要这么做：
//   规格要求「单 exe、< 100KB、零运行时依赖」。
//   实测 MSVC 14.51 + 静态 UCRT（/MT）下，哪怕只有一个 MessageBox 的
//   最小 Win32 程序也有 97KB —— 用 /MT 永远压不进 100KB。而改用 /MD
//   又会引入 vcruntime140.dll / ucrtbase.dll 依赖，同样不满足要求。
//
//   本程序只调用 Win32 API，完全没有用到 CRT（没有 printf / malloc /
//   new / std::，字符串处理一律走 kernel32 的 lstr* 系列）。所以可以
//   直接 /NODEFAULTLIB 掉整个 CRT，自己提供进程入口和编译器可能内联
//   出来的极少数内存函数。
//
// 代价与约束：
//   * 不能用任何 C++ 动态初始化（带构造函数的全局对象）—— 我们没有；
//   * 不能用异常、RTTI、浮点（/EHs-c- /GR- 已关，且代码里无浮点）；
//   * 关闭了 /GS 栈保护 —— 本程序不处理任何外部输入，缓冲区长度全部
//     由 MAX_PATH 等常量界定，风险可接受；
//   * 入口点符号名与位数有关：x64 下没有名字修饰，x86 下是
//     _wWinMainCRTStartup。因此下面用 __cdecl 声明，构建脚本按位数
//     传对应的 /ENTRY 值（见 build.bat / build_local.sh）。
//
// 编译开关 CAPS_NO_CRT 未定义时本文件不产生任何代码，方便切回标准 /MT
// 构建做对照（见 build.bat / build_local.sh 的 crt 参数）。
// ---------------------------------------------------------------------------

#ifdef CAPS_NO_CRT

#include "app.h"

/** 进程入口（实现见 main.cpp）。这里只做前置声明，不重复导出。 */
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPWSTR lpCmdLine, int nCmdShow);

/**
 * 让编译器知道本模块不会产生浮点运算，从而不去引用 CRT 的浮点支持。
 */
extern "C" int _fltused = 0;

// ---------------------------------------------------------------------------
// 编译器可能内联出来的内存函数
//
// 这几个符号平时由 CRT 提供。/NODEFAULTLIB 之后如果编译器决定不内联，
// 就会引用它们，所以必须自己实现。
//
// #pragma function(...) 的作用是禁止编译器在本翻译单元内把调用替换成
// 内联固有形式 —— 否则实现体里那个循环可能被识别成 memset 并递归调用自己。
// ---------------------------------------------------------------------------

#pragma function(memset)
extern "C" void* __cdecl memset(void* dst, int value, size_t count)
{
    unsigned char* p = (unsigned char*)dst;
    while (count-- > 0) {
        *p++ = (unsigned char)value;
    }
    return dst;
}

#pragma function(memcpy)
extern "C" void* __cdecl memcpy(void* dst, const void* src, size_t count)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (count-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

#pragma function(memmove)
extern "C" void* __cdecl memmove(void* dst, const void* src, size_t count)
{
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;

    if (d == s || count == 0) {
        return dst;
    }
    if (d < s) {
        while (count-- > 0) {
            *d++ = *s++;
        }
    } else {
        d += count;
        s += count;
        while (count-- > 0) {
            *--d = *--s;
        }
    }
    return dst;
}

// ---------------------------------------------------------------------------
// 进程入口
// ---------------------------------------------------------------------------

/**
 * Windows 加载器在 /SUBSYSTEM:WINDOWS 下实际调用的入口。
 *
 * 替代 CRT 的 wWinMainCRTStartup：不做堆初始化、不跑静态构造、不装
 * SEH —— 对纯 Win32 程序而言这些本来也用不上。
 */
extern "C" void __cdecl wWinMainCRTStartup(void)
{
    const HINSTANCE hInstance = GetModuleHandleW(NULL);
    const int rc = wWinMain(hInstance, NULL, GetCommandLineW(), SW_SHOWDEFAULT);
    ExitProcess((UINT)rc);
}

#endif // CAPS_NO_CRT
