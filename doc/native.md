# 原生模块（C/C++ 扩展）

LuaLoader 支持 Mod 携带 **C/C++ 原生模块**（`.so` / `.dll`），用于性能敏感或 Lua 不易实现的场景。
纯 Lua Mod 完全不受影响。

---

## 1. 目录约定

原生库按“平台_架构”放置，随 Mod 包一起分发：

```
Resources/native/<平台>_<架构>/<模块名>.so      # 首选：linux_x64 / linux_x86 / windows_x64 / windows_x86 / android_arm64 / android_arm
Resources/native/<平台>/<模块名>.so             # 次选：不区分架构
Resources/lib/native/<平台>_<架构>/<模块名>.so  # 同上，兼容入口脚本在 lib/ 的布局
```

部署后加载器会在以下位置查找（`<私有目录>` = Mod 运行时私有目录）：

```
<私有目录>/native/<平台>_<架构>/
<私有目录>/native/<平台>/
<私有目录>/lib/native/<平台>_<架构>/
<私有目录>/lib/native/<平台>/
```

命中后该目录会被加入 `package.cpath`，并且所有库会以 `RTLD_GLOBAL` **预加载**（让模块能解析到
loader 进程导出的符号）。

## 2. 在 Lua 里使用

就是标准 Lua C 模块，`require` 即可：

```lua
local nativehello = require("nativehello")   -- 加载 nativehello.so / nativehello.dll
print(nativehello.greet("world"))
```

命名规则同 Lua 官方：`require("foo")` → 查找 `foo.so` → 调用 `luaopen_foo`。

## 3. 在 C 侧能拿到什么

### 3.1 Lua C API

LuaLoader 导出了 `lua_*` / `luaL_*` 符号，模块按标准写法即可：

```c
#include <lua.h>
#include <lauxlib.h>

static int l_greet(lua_State *L) {
    const char *name = luaL_optstring(L, 1, "world");
    lua_pushfstring(L, "hello, %s", name);
    return 1;
}

static const luaL_Reg funcs[] = { {"greet", l_greet}, {NULL, NULL} };

int luaopen_nativehello(lua_State *L) {
    luaL_newlib(L, funcs);
    return 1;
}
```

> 编译时**不要链接** Lua 库；`lua_*` 保持未定义，运行时由 loader 解析。
> Windows 例外：PE 不允许 DLL 带未解析符号，需要链接 loader 的 import lib（见 3.3）。

### 3.2 内核 API（`patchlib_*`）

LuaLoader 同时导出了内核的 `patchlib_*` **函数指针变量**，签名与
`includes/tefkernel/patchlib/*.h` 完全一致。把该头文件目录加入 include 路径即可直接调用：

```c
#include "patchlib/type.h"
#include "patchlib/field.h"

int luaopen_mymod(lua_State *L) {
    patch_handle_t item = patchlib_type_get_type("Terraria", "Item");
    patch_handle_t f    = patchlib_type_get_field(item, "useTime");
    // ... 直接使用内核能力
    lua_pushboolean(L, f != 0);
    return 1;
}
```

注意：
- 这些是**指针变量**，由内核启动时填充；内核没有的能力其值为 `NULL`，**调用前判空**
- 正式版内核与开发版内核导出的能力集合不同（开发版更多）

### 3.3 平台差异

| 平台 | 编译 | 链接 |
|---|---|---|
| Linux / Android | `-fPIC -shared`，`lua_*` 允许未定义 | 不需要额外库 |
| Windows (MinGW) | `-shared` | 必须链接 loader 的 import lib（PE 要求符号全部解析）：`sdk/windows_<arch>/libloader.windows.<arch>.dll.a` |

Windows import lib 由 loader DLL 生成：

```bash
# 依赖 mingw-w64-tools（apt install mingw-w64-tools）
./tools/make_win_implib.sh <libloader.windows.x64.dll> sdk/windows_x64
```

## 4. 完整示例

见 [`examples/native_hello`](../examples/native_hello)：

- `src/nativehello.c`：C 模块源码
- `build.sh <目标>`：一键编译到 `Mod/Resources/native/<目标>/`
  - 支持 `linux_x64`、`linux_x86`、`windows_x64`、`windows_x86`、`android_arm64`、`android_arm`
  - Android 需先 `export ANDROID_NDK_HOME=...`
  - Windows 需 import lib（`LUALOADER_WIN_IMPLIB` 可指定）
- `Mod/`：可直接打成 zip 的 Mod 包骨架

打包 Mod：

```bash
cd examples/native_hello/Mod && zip -qrX ../NativeHello.zip .
```

## 5. 加载与失败排查

- 预加载成功：日志 `[mod <id>] preloaded native module: <文件名>`
- 预加载失败：日志 `[mod <id>] dlopen failed: <路径> (<原因>)`
- `require` 失败：Lua 侧会返回错误字符串（如符号缺失、架构不匹配、`dynamic libraries not enabled`）
- 架构不匹配：不要给 x86 平台放 arm64 库；加载器只扫描当前 `平台_架构` 目录

> 若报 `dynamic libraries not enabled`，说明运行的内置 Lua 没开 `LUA_USE_DLOPEN`；
> 本项目自 1.6.0 起已开启。

## 6. 安全提示

原生模块等同于在游戏进程内执行任意原生代码，风险高于纯 Lua。请只安装信任来源的 Mod；
安装前建议检查包内 `native/` 目录与 `.lua` 脚本。
