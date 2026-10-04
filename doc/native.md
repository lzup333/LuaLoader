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
local hello = require("hello")   -- 加载 hello.so / hello.dll
print(hello.greet("world"))
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

int luaopen_hello(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_createtable, lua_createtable, void, (lua_State *, int, int));
    LL_CACHE(p_pushcclosure, lua_pushcclosure, void, (lua_State *, int (*)(lua_State *), int));
    LL_CACHE(p_setfield, lua_setfield, void, (lua_State *, int, const char *));
    p_createtable(L, 0, 1);
    p_pushcclosure(L, l_greet, 0);
    p_setfield(L, -2, "greet");
    return 1;
}
```

要点：

- `ll_api_t { version, size, lookup(name) }`：`lookup` 可解析 **loader 自身导出的符号**
  （`lua_*`、`luaL_*` 函数，以及 `patchlib_*` 函数指针变量），无需链接任何库
- `LL_CACHE(变量, 符号名, 返回类型, (参数类型...))` 宏：首次调用时解析并缓存函数指针
- 拿 `patchlib_*` 同理：`LL_CACHE(p_type_get_type, patchlib_type_get_type, void *, (const char *, const char *));`
  loader 里的 `patchlib_*` 是函数指针变量，`lookup` 会先解引用，所以拿到即可直接调用

> 若 loader 未注入（旧版 loader），`LL` 为 NULL。模块里应 `if (!LL) return 0;` 优雅失败，不要崩溃。

### 3.2 直接 `extern`（仅 Linux/桌面可用）

在 Linux/桌面，loader 以 `RTLD_GLOBAL` 预加载原生库，模块可直接引用 loader 导出的符号
（`lua_*` 与 `patchlib_*` 函数指针变量，签名同 `includes/tefkernel/patchlib/*.h`）。
但 **Android 不可用**（符号作用域封闭），所以跨平台模块请用 3.1。

### 3.3 注意：有些名字是“宏”，不是符号

`lookup` 只能拿到**真实导出符号**。Lua 头文件里不少常用名是宏，直接 lookup 会得到 NULL，
请改用其底层真实函数：

| 宏（无符号） | 真实符号（可 lookup） |
|---|---|
| `lua_pushcfunction` | `lua_pushcclosure`（补 `0` 参数） |
| `lua_tostring` | `lua_tolstring`（补 `NULL`） |
| `lua_tonumber` | `lua_tonumberx`（补 `NULL`） |
| `lua_tointeger` | `lua_tointegerx`（补 `NULL`） |
| `lua_newtable` | `lua_createtable(L,0,0)` |
| `lua_pop` | `lua_settop(L,-(n)-1)` |
| `luaL_checkstring` | `luaL_checklstring`（补 `NULL`） |
| `luaL_optstring` | `luaL_optlstring`（补 `NULL`） |
| `luaL_newlib` | `luaL_checkversion` + `luaL_newlibtable` + `luaL_setfuncs` |

不确定某个名字是函数还是宏时，查 `lib/lua-5.4.8/src/lua.h` 里的 `LUA_API` / `#define`。

## 4. 完整示例

```c
/* mymod.c —— 编译见下方命令；不需要 <lua.h> */
#include "lualoader_mod.h"
#include <stddef.h>

typedef struct lua_State lua_State;

static const ll_api_t *LL = NULL;
LL_EXPORT void ll_set_api(const ll_api_t *api) { LL = api; }   /* loader 注入 */

/* add(a, b) -> number */
static int l_add(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_tointeger, lua_tointegerx, long long, (lua_State *, int, int *));
    LL_CACHE(p_pushinteger, lua_pushinteger, void, (lua_State *, long long));
    if (!p_tointeger || !p_pushinteger) return 0;
    int isnum = 0;
    long long a = p_tointeger(L, 1, &isnum);
    long long b = p_tointeger(L, 2, &isnum);
    p_pushinteger(L, a + b);
    return 1;
}

/* my_player() -> number（用内核 API 读 Main.myPlayer） */
static int l_my_player(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_type_get_type, patchlib_type_get_type, void *, (const char *, const char *));
    LL_CACHE(p_type_get_field, patchlib_type_get_field, void *, (void *, const char *));
    LL_CACHE(p_field_get_value, patchlib_field_get_value, void, (void *, void *, void *));
    LL_CACHE(p_pushinteger, lua_pushinteger, void, (lua_State *, long long));
    if (!p_type_get_type || !p_type_get_field || !p_field_get_value || !p_pushinteger)
        return 0;

    void *main_cls = p_type_get_type("Terraria", "Main");
    void *f = main_cls ? p_type_get_field(main_cls, "myPlayer") : NULL;
    if (!f) return 0;
    int idx = -1;
    p_field_get_value(f, NULL, &idx);          /* 静态字段，实例传 NULL */
    p_pushinteger(L, idx);
    return 1;
}

int luaopen_mymod(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_createtable, lua_createtable, void, (lua_State *, int, int));
    LL_CACHE(p_pushcclosure, lua_pushcclosure, void, (lua_State *, int (*)(lua_State *), int));
    LL_CACHE(p_setfield, lua_setfield, void, (lua_State *, int, const char *));
    if (!p_createtable || !p_pushcclosure || !p_setfield) return 0;

    p_createtable(L, 0, 2);
    p_pushcclosure(L, l_add, 0);
    p_setfield(L, -2, "add");
    p_pushcclosure(L, l_my_player, 0);
    p_setfield(L, -2, "my_player");
    return 1;
}
```

编译（Linux / Android / Windows）：

```bash
INC=-I<LuaLoader>/includes

# Linux x64
clang -O2 -fPIC -shared -o mymod.so mymod.c $INC

# Android arm64
$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  -O2 -fPIC -shared -o mymod.so mymod.c $INC

# Windows x64（PE 需 import lib，仓库 sdk/ 下提供）
x86_64-w64-mingw32-gcc -O2 -shared -o mymod.dll mymod.c $INC \
  <LuaLoader>/sdk/windows_x64/libloader.windows.x64.dll.a
```

放置与打包：

```
MyMod/
├── Info.json / luamod.json / Manifest.json
└── Resources/
    ├── lib/main.lua                  -- local m = require("mymod")
    └── native/<平台>_<架构>/mymod.so  -- 按平台放置
cd MyMod && zip -qrX ../MyMod.zip .
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
