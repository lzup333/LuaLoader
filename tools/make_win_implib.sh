#!/usr/bin/env bash
# 从 LuaLoader 的 Windows DLL 生成原生模块所需的 import lib（.dll.a）
# 依赖 mingw-w64-tools 的 gendef + dlltool（Debian: apt install mingw-w64-tools）
# 用法: ./make_win_implib.sh <libloader.windows.x64.dll> <输出目录>
set -e
DLL="$1"; OUT="${2:-.}"
command -v gendef >/dev/null || { echo "缺少 gendef（apt install mingw-w64-tools）" >&2; exit 1; }
BASE="$(basename "$DLL" .dll)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
gendef "$DLL" -o "$TMP/$BASE.def" >/dev/null 2>&1 || gendef "$DLL" > "$TMP/$BASE.def"
case "$DLL" in
  *x86_64*) DT=x86_64-w64-mingw32-dlltool ;;
  *)        DT=i686-w64-mingw32-dlltool ;;
esac
"$DT" -d "$TMP/$BASE.def" -l "$OUT/$BASE.dll.a" -D "$(basename "$DLL")"
echo "生成: $OUT/$BASE.dll.a"
