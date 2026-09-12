@echo off
rem ===========================================================================
rem  CapsSwitch 构建脚本（Windows / MSVC）
rem
rem  用法：
rem      build.bat            构建 x64 + x86（默认，静态 CRT）
rem      build.bat nocrt      无 CRT 精简版，体积最小（不需要可忽略）
rem
rem  依赖：Visual Studio 2019/2022/2026 且勾选「使用 C++ 的桌面开发」，
rem        或独立的 Build Tools。
rem
rem  产物：bin\CapsSwitch-x64.exe、bin\CapsSwitch-x86.exe
rem
rem  说明：本脚本通过 vcvarsall.bat 初始化编译环境（微软官方推荐做法）。
rem        若要完全绕开注册表定位 SDK，可改用 tools\build_local.sh
rem        （Git Bash / MSYS2 下运行），那条路径是实测验证过的。
rem ===========================================================================

setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || exit /b 1

set "MODE=%~1"
if "%MODE%"=="" set "MODE=std"

rem ---- 定位 Visual Studio ---------------------------------------------------
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"

if not defined VSROOT if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not defined VSROOT if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VSROOT=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSROOT goto :novs
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" goto :novs

echo 使用 Visual Studio: %VSROOT%
echo.

if not exist bin mkdir bin
if not exist obj mkdir obj

rem vcvarsall 会改写 PATH / INCLUDE / LIB / LIBPATH。这里在第一次调用前把原始
rem 环境整体快照下来，之后每次切架构都先还原再 call，杜绝上一轮的 x64 路径残留
rem 到 x86 编译里（典型症状：链接期报 LNK1112「模块计算机类型冲突」）。
rem
rem 依据：vcvarsall 默认并不重置这些变量 —— 它转调的 vsdevcmd\ext\vcvars.bat 里
rem 写的是 set "LIB=%__VCVARS_ADD_TO_LIB%;%LIB%"（前置追加），vsdevcmd.bat 里
rem 写的是 set "INCLUDE=%__VSCMD_INCLUDE_ORDER%%INCLUDE%"（同样是前置追加）。
rem 只有显式传 /clean_env 才会清空，而那条路径会跳过架构初始化。所以同一进程内
rem 连续切两次架构，第二次一定拿到被污染的 INCLUDE/LIB。
set "PATHBAK=%PATH%"
set "INCLUDEBAK=%INCLUDE%"
set "LIBBAK=%LIB%"
set "LIBPATHBAK=%LIBPATH%"

call :vcenv x64
if errorlevel 1 goto :novs

echo == 编译资源 ==
rc /nologo /c65001 /fo bin\CapsSwitch.res src\CapsSwitch.rc || exit /b 1

call :arch x64 || exit /b 1
call :arch x86 || exit /b 1

echo.
echo 构建完成。
exit /b 0

rem ===========================================================================
rem  子过程
rem ===========================================================================

:arch
set "ARCH=%~1"
set "OBJ=obj\%ARCH%"
set "OUT=bin\CapsSwitch-%ARCH%.exe"
if not exist "%OBJ%" mkdir "%OBJ%"

call :vcenv %ARCH%
if errorlevel 1 exit /b 1

rem /O1 体积优先，/GL 链接期优化，/Gy+/Gw+/GF 消除冗余代码与数据，
rem /GR- /EHs-c- 关 RTTI 与异常，/utf-8 让源码里的中文按 UTF-8 解析。
set CFLAGS=/nologo /c /O1 /Os /GL /Gy /Gw /GF /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /DNDEBUG /DUNICODE /D_UNICODE /utf-8 /std:c++17 /W4 /WX- /I src /Fo%OBJ%\
set CFLAGS_NOGL=/nologo /c /O1 /Os /Gy /Gw /GF /GR- /EHs-c- /D_HAS_EXCEPTIONS=0 /DNDEBUG /DUNICODE /D_UNICODE /utf-8 /std:c++17 /W4 /WX- /I src /Fo%OBJ%\
set SOURCES=src\main.cpp src\config.cpp src\hook.cpp src\tray.cpp src\settings.cpp
set LDFLAGS=/nologo /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /DYNAMICBASE /NXCOMPAT
set LIBS=user32.lib kernel32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib

echo == [%ARCH%] 编译源码 ==
if /i "%MODE%"=="nocrt" goto :arch_nocrt

cl %CFLAGS% /MT /GS %SOURCES% || exit /b 1
goto :arch_link

:arch_nocrt
rem 无 CRT：自己提供进程入口与内存函数，/GS- 是必需的（没有 __security_cookie）。
cl %CFLAGS% /MT /GS- /DCAPS_NO_CRT %SOURCES% || exit /b 1
rem crt_free.cpp 自带 memset/memcpy/memmove，LTCG 不允许这类「库帮助器」（C2268），单独无 /GL 编译。
cl %CFLAGS_NOGL% /MT /GS- /DCAPS_NO_CRT src\crt_free.cpp || exit /b 1
set LDFLAGS=%LDFLAGS% /NODEFAULTLIB

:arch_link
echo == [%ARCH%] 链接 ==
link %LDFLAGS% %OBJ%\*.obj bin\CapsSwitch.res %LIBS% /OUT:%OUT% || exit /b 1

for %%F in ("%OUT%") do echo     %%~nF%%~xF  %%~zF 字节
echo.
exit /b 0

:vcenv
rem 参数：x64 / x86。先把环境还原到脚本启动时的原始状态，再交给 vcvarsall。
set "PATH=%PATHBAK%"
set "INCLUDE=%INCLUDEBAK%"
set "LIB=%LIBBAK%"
set "LIBPATH=%LIBPATHBAK%"
call "%VSROOT%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul
if errorlevel 1 exit /b 1
exit /b 0

:novs
echo.
echo [错误] 未找到可用的 Visual Studio 编译环境。
echo        请安装 Visual Studio 并在安装器中勾选
echo        「使用 C++ 的桌面开发」（含 MSVC v143+ 与 Windows SDK）。
exit /b 1
