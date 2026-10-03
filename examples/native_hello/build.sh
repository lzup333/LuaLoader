#!/usr/bin/env bash
# 构建原生模块示例到 Mod 的对应平台目录
# 用法:
#   ./build.sh linux_x64
#   ./build.sh android_arm64
#   ./build.sh windows_x64
set -e
cd "$(dirname "$0")"

TARGET="${1:-linux_x64}"
OUT_DIR="Mod/Resources/native/${TARGET}"
mkdir -p "$OUT_DIR"

# LuaLoader 的 Lua 头文件（仓库内自带）
LUA_INC="$(cd ../.. && pwd)/lib/lua-5.4.8/src"
MOD_INC="$(cd ../.. && pwd)/includes"

case "$TARGET" in
  linux_x64)
    clang -O2 -fPIC -shared -o "$OUT_DIR/nativehello.so" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC"
    ;;
  linux_x86)
    clang -m32 -O2 -fPIC -shared -o "$OUT_DIR/nativehello.so" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC"
    ;;
  windows_x64)
    # Windows 需要在链接时解析 lua_* 符号：使用从 loader DLL 生成的 import lib
    IMPLIB="${LUALOADER_WIN_IMPLIB:-$(cd ../.. && pwd)/sdk/windows_x64/libloader.windows.x64.dll.a}"
    [ -f "$IMPLIB" ] || { echo "缺少 import lib: $IMPLIB（用 tools/make_win_implib.sh 生成）" >&2; exit 1; }
    x86_64-w64-mingw32-gcc -O2 -shared -o "$OUT_DIR/nativehello.dll" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC" "$IMPLIB"
    ;;
  windows_x86)
    IMPLIB="${LUALOADER_WIN_IMPLIB:-$(cd ../.. && pwd)/sdk/windows_x86/libloader.windows.x86.dll.a}"
    [ -f "$IMPLIB" ] || { echo "缺少 import lib: $IMPLIB（用 tools/make_win_implib.sh 生成）" >&2; exit 1; }
    i686-w64-mingw32-gcc -O2 -shared -o "$OUT_DIR/nativehello.dll" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC" "$IMPLIB"
    ;;
  android_arm64)
    : "${ANDROID_NDK_HOME:?请先 export ANDROID_NDK_HOME}"
    CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang"
    "$CC" -O2 -fPIC -shared -o "$OUT_DIR/nativehello.so" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC"
    ;;
  android_arm)
    : "${ANDROID_NDK_HOME:?请先 export ANDROID_NDK_HOME}"
    CC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi24-clang"
    "$CC" -O2 -fPIC -shared -o "$OUT_DIR/nativehello.so" src/nativehello.c -I"$LUA_INC" -I"$MOD_INC"
    ;;
  *)
    echo "未知目标: $TARGET" >&2
    exit 1
    ;;
esac

echo "built -> $OUT_DIR"
