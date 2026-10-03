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

### 3.1 推荐：通过 `ll_api_t` 注入的 API 表（全平台一致）

Android 上 loader 由内核以 `RTLD_LOCAL` 从 memfd 加载，模块的 `lua_*` 未定义符号**无法**在
ELF 全局作用域解析。因此推荐统一走 loader 注入的 API 表（与 TEFKernel 给 C mod 传 TPF 符号表同理）：

```c
#include "lualoader_mod.h"          /* 随 LuaLoader 仓库提供 */

typedef struct lua_State lua_State; /* 只为拿类型，不需要完整 lua.h */

static const ll_api_t *LL = NULL;

/* loader 在 dlopen 后调用这个入口注入 API 表 */
LL_EXPORT void ll_set_api(const ll_api_t *api) { LL = api; }

static int l_greet(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_tolstring, lua_tolstring, const char *, (lua_State *, int, size_t *));
    LL_CACHE(p_pushfstring, lua_pushfstring, const char *, (lua_State *, const char *, ...));
    size_t n = 0;
    const char *name = p_tolstring(L, 1, &n);
    p_pushfstring(L, "hello, %s", name ? name : "world");
    return 1;
}

int luaopen_nativehello(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_createtable, lua_createtable, void, (lua_State *, int, int));
    LL_CACHE(p_pushcfunction, lua_pushcfunction, void, (lua_State *, int (*)(lua_State *)));
    LL_CACHE(p_setfield, lua_setfield, void, (lua_State *, int, const char *));
    p_createtable(L, 0, 1);
    p_pushcfunction(L, l_greet);
    p_setfield(L, -2, "greet");
    return 1;
}
```

要点：

- `ll_api_t { version, size, lookup(name) }`：`lookup` 可解析 **loader 自身导出的任意符号**
  （`lua_*`、`luaL_*`、`patchlib_*` …），无需链接任何库
- `LL_CACHE(变量, 符号名, 返回类型, (参数类型...))` 宏：首次调用时解析并缓存函数指针
- 拿 `patchlib_*` 同理：`LL_CACHE(p_type_get_type, patchlib_type_get_type, void *, (const char *, const char *));`

> 若 loader 未注入（旧版 loader），`LL` 为 NULL。模块里应 `if (!LL) return 0;` 优雅失败，不要崩溃。

### 3.2 直接 `extern`（仅 Linux/桌面可用）

在 Linux/桌面，loader 以 `RTLD_GLOBAL` 预加载原生库，模块可直接引用 loader 导出的符号
（`lua_*` 与 `patchlib_*` 函数指针变量，签名同 `includes/tefkernel/patchlib/*.h`）。
但 **Android 不可用**（符号作用域封闭），所以跨平台模块请用 3.1。

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
