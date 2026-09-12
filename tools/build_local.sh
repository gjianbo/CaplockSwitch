#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build_local.sh - 在类 Unix shell（Git Bash / MSYS2）下直接调用 MSVC 构建
#
# Windows 用户请优先用 build.bat；本脚本存在的原因是它不依赖 vcvarsall.bat
# （后者要靠注册表定位 Windows SDK），改由脚本自己拼 INCLUDE / LIB，
# 构建环境是自证可复现的。
#
# 用法：
#   bash tools/build_local.sh            # 默认：静态 CRT (/MT)，x64 + x86
#   bash tools/build_local.sh nocrt      # 可选：无 CRT 精简版（目标 < 100KB）
# ---------------------------------------------------------------------------

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-std}"

MSVC_VER="${MSVC_VER:-14.51.36231}"
MSVC_ROOT="${MSVC_ROOT:-/f/visual_studio/18/Community/VC/Tools/MSVC}"
SDK_ROOT="${SDK_ROOT:-/f/Windows Kits/10}"
SDK_VER="${SDK_VER:-10.0.26100.0}"
MSVC="$MSVC_ROOT/$MSVC_VER"

cd "$ROOT" || exit 1
rm -rf obj bin/CapsSwitch-*.exe bin/CapsSwitch.res
mkdir -p bin obj

# ---------------------------------------------------------------------------
# 编译一个目标架构
#   $1 = x64 | x86
# ---------------------------------------------------------------------------
build_arch() {
  local ARCH="$1"
  local HOSTBIN ENTRY OUT OBJDIR

  case "$ARCH" in
    # x86 下 link.exe 会自行给 /ENTRY 的值补前导下划线，所以两边都写不带下划线的名字。
    x64) HOSTBIN="Hostx64/x64"; ENTRY="wWinMainCRTStartup"; OUT="bin/CapsSwitch-x64.exe" ;;
    x86) HOSTBIN="Hostx64/x86"; ENTRY="wWinMainCRTStartup"; OUT="bin/CapsSwitch-x86.exe" ;;
    *)   echo "未知架构: $ARCH"; return 1 ;;
  esac

  OBJDIR="obj/$ARCH"
  mkdir -p "$OBJDIR"

  export PATH="$MSVC/bin/$HOSTBIN:$SDK_ROOT/bin/$SDK_VER/x64:$PATH"
  export INCLUDE="$(cygpath -w "$MSVC/include");$(cygpath -w "$SDK_ROOT/Include/$SDK_VER/ucrt");$(cygpath -w "$SDK_ROOT/Include/$SDK_VER/shared");$(cygpath -w "$SDK_ROOT/Include/$SDK_VER/um")"
  export LIB="$(cygpath -w "$MSVC/lib/$ARCH");$(cygpath -w "$SDK_ROOT/Lib/$SDK_VER/ucrt/$ARCH");$(cygpath -w "$SDK_ROOT/Lib/$SDK_VER/um/$ARCH")"

  # /O1 体积优先，/GL 链接期优化，/Gy+/Gw+/GF 消除冗余代码与数据，
  # /GR- /EHs-c- 关 RTTI 与异常（本程序完全不使用），
  # /utf-8 保证源码里的中文按 UTF-8 解析而不是按系统 ANSI 代码页。
  local CFLAGS=(
    /nologo /c /O1 /Os /GL /Gy /Gw /GF
    /GR- /EHs-c- /D_HAS_EXCEPTIONS=0
    /DNDEBUG /DUNICODE /D_UNICODE
    /utf-8 /std:c++17 /W4 /WX-
    /I src "/Fo$(cygpath -w "$ROOT/$OBJDIR")\\"
  )
  local SOURCES=(src/main.cpp src/config.cpp src/hook.cpp src/tray.cpp src/settings.cpp)
  local LDFLAGS=(
    /nologo /SUBSYSTEM:WINDOWS "/ENTRY:$ENTRY"
    /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO /DYNAMICBASE /NXCOMPAT
  )
  local LIBS=(user32.lib kernel32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib)

  echo "== [$ARCH] 编译源码 =="
  if [ "$MODE" = "nocrt" ]; then
    # 无 CRT 构建：不链任何运行库，自己提供进程入口与内存函数。
    # /GS- 是必需的 —— 没有 CRT 就没有 __security_cookie。
    cl "${CFLAGS[@]}" /MT /GS- /DCAPS_NO_CRT "${SOURCES[@]}" || return 1

    # crt_free.cpp 自己实现了 memset / memcpy / memmove，而 LTCG 不允许把
    # 用户定义的内存函数当「库帮助器」参与链接期优化（C2268），
    # 所以这一个文件单独按无 /GL 编译。
    local NOGL=()
    local f
    for f in "${CFLAGS[@]}"; do
      [ "$f" = "/GL" ] || NOGL+=("$f")
    done
    cl "${NOGL[@]}" /MT /GS- /DCAPS_NO_CRT src/crt_free.cpp || return 1
    LDFLAGS+=(/NODEFAULTLIB)
  else
    # 标准构建：静态 CRT，栈保护保持打开；不定义 CAPS_NO_CRT，
    # 因此 crt_free.cpp 不会被编进产物。
    cl "${CFLAGS[@]}" /MT /GS "${SOURCES[@]}" || return 1
  fi

  echo "== [$ARCH] 链接 =="
  link "${LDFLAGS[@]}" "$OBJDIR"/*.obj bin/CapsSwitch.res "${LIBS[@]}" "/OUT:$OUT" || return 1

  local SIZE
  SIZE=$(stat -c %s "$OUT")
  echo "   -> $OUT  $SIZE 字节 ($((SIZE / 1024)) KB)"
  echo
}

# ---- 资源（与架构无关） ---------------------------------------------------
echo "== 编译资源 =="
export PATH="$MSVC/bin/Hostx64/x64:$SDK_ROOT/bin/$SDK_VER/x64:$PATH"
rc /nologo /c65001 /fo bin/CapsSwitch.res src/CapsSwitch.rc || exit 1
echo

build_arch x64 || exit 1
build_arch x86 || exit 1
