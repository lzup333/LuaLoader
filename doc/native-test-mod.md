# 测试 Mod：用 C 模块把日志写入私有目录

这是一个**验证用 Mod**，用来确认 LuaLoader 的原生模块链路：

1. `require("testlog")` 能从 `Resources/native/<平台>_<架构>/` 加载；
2. 模块里的 `lua_*` / `luaL_*` 由 loader 导出并解析（`luaL_checkversion_` 通过，版本一致）；
3. C 代码能把日志追加到 `mod.private_dir`；
4. 计算型接口 `sum(n)` 正常返回。

> 仓库不提交 `examples/` / `tests/` 目录（CI 会拦截）。请按本文在本地自建一个 Mod 目录，
> 例如 `native_logger_test/`，再把下面的文件放进去。

## 目录结构

```
native_logger_test/
├── Info.json
├── Manifest.json
├── luamod.json
├── native/
│   └── testlog.c
└── Resources/
    ├── lib/
    │   └── main.lua
    └── native/
        └── <平台>_<架构>/
            └── testlog.so|dll      # 由 testlog.c 编译生成
```

平台/架构目录名：`linux_x64`、`linux_x86`、`windows_x64`、`windows_x86`、
`android_arm64`、`android_arm`。

## 1. 清单文件

### `Info.json`

```json
{
  "pkgId": "lzup333.test.native_logger",
  "name": "Native Logger Test",
  "author": "lzup333",
  "version": "1.0.0",
  "versionCode": 1,
  "brieflyDescribe": "用标准 Lua C 模块把日志写进 Mod 私有目录",
  "description": "测试原生模块加载、Lua 5.5 符号解析与私有目录写入。",
  "features": [],
  "sizeCategory": "TINY",
  "targetGameVersion": "1.4.5.8",
  "minGameVersion": "1.4.5.8",
  "maxGameVersion": "1.4.5.8",
  "support": {
    "android": { "arm64": true,  "arm": true,  "x64": false, "x86": false },
    "windows": { "arm64": false, "arm": false, "x64": true,  "x86": true  },
    "linux":   { "arm64": false, "arm": false, "x64": true,  "x86": true  },
    "mac":     { "arm64": false, "arm": false, "x64": false, "x86": false },
    "ios":     { "arm64": false, "arm": false, "x64": false, "x86": false }
  },
  "dependence": [],
  "conflicts": [],
  "stableVerified": false,
  "experimental": false,
  "deprecated": false,
  "hasExtendedContent": false
}
```

### `luamod.json`

```json
{
  "main": "main.lua",
  "pkg_id": "lzup333.test.native_logger",
  "version": "1.0.0",
  "version_code": 1,
  "api_version": 1,
  "multiplayer_safe": true
}
```

### `Manifest.json`

```json
{
  "type": "Mod",
  "file": "luamod.json",
  "parentLoader": "lzup333.lualoader",
  "resources": "Resources",
  "modloader": { "type": "null" },
  "plugins": { "type": "null" },
  "tefkernel": { "minVersion": "1.0.0" }
}
```

## 2. Lua 入口 `Resources/lib/main.lua`

```lua
mod.meta = { pkg_id = "lzup333.test.native_logger", version = "1.0.0" }

local LOG = mod.private_dir .. "/native_test.log"

local ok, logmod = pcall(require, "testlog")
if not ok then
    mod.error("require(testlog) failed: " .. tostring(logmod))
    return
end

local function write(level, msg)
    local wok, err = logmod.log(LOG, level, msg)
    if not wok then
        mod.warn("native log write failed: " .. tostring(err))
    end
end

function setup()
    write("INFO", "native module loaded, compiled for " .. logmod.version())
    write("INFO", "platform=" .. mod.platform .. "/" .. mod.arch)
    write("INFO", "private_dir=" .. mod.private_dir)

    local n = 1000000
    local s = logmod.sum(n)
    write("INFO", string.format("native sum(%d) = %d", n, s))

    local wok, werr = mod.write_file("lua_side_check.txt", "written by Lua\n")
    write("INFO", "lua write_file ok=" .. tostring(wok) .. " err=" .. tostring(werr))

    mod.info("native_logger_test: log file = " .. LOG)
end

function mod.cleanup()
    write("INFO", "cleanup called")
end

todo_list = { "setup" }
```

说明：

- `require("testlog")` 对应模块入口 `luaopen_testlog`；
- `LOG` 在 `mod.private_dir` 下，C 模块只负责追加写；
- 用 `pcall` 包住 `require`，加载失败时把原因写进 loader 日志，而不是直接中断。

## 3. C 模块 `native/testlog.c`

```c
#include <lua.h>
#include <lauxlib.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define TESTLOG_EXPORT __declspec(dllexport)
#else
#define TESTLOG_EXPORT __attribute__((visibility("default")))
#endif

/* 追加一行日志到指定文件（路径由 Lua 侧传入 mod.private_dir）。 */
static int l_log(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *level = luaL_optstring(L, 2, "INFO");
    const char *msg = luaL_checkstring(L, 3);

    FILE *f = fopen(path, "ab");
    if (!f) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", path);
        return 2;
    }

    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    char ts[32];
    if (strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv) == 0)
        strcpy(ts, "0000-00-00 00:00:00");

    fprintf(f, "[%s] [%s] %s\n", ts, level, msg);
    const int ok = ferror(f) == 0;
    fclose(f);

    if (!ok) {
        lua_pushnil(L);
        lua_pushliteral(L, "write failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* 返回编译该模块时使用的 Lua 版本，用来核对与 loader 是否一致。 */
static int l_version(lua_State *L) {
    lua_pushstring(L, LUA_RELEASE);
    return 1;
}

/* 一个最简“计算型”接口：1..n 求和，验证原生代码确实在跑。 */
static int l_sum(lua_State *L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    if (n < 0) n = 0;
    lua_Integer s = 0;
    for (lua_Integer i = 1; i <= n; ++i) s += i;
    lua_pushinteger(L, s);
    return 1;
}

static const luaL_Reg testlog_funcs[] = {
        {"log", l_log},
        {"version", l_version},
        {"sum", l_sum},
        {NULL, NULL},
};

TESTLOG_EXPORT int luaopen_testlog(lua_State *L) {
    luaL_newlib(L, testlog_funcs);
    return 1;
}
```

要点：

- 这是标准 Lua C 模块，不需要 `lualoader_mod.h` 或 `ll_set_api`；
- 模块只引用 `lua_*` / `luaL_*`，不访问 TEFKernel / patchlib；
- `luaL_newlib` 会展开为 `luaL_checkversion_` + `luaL_setfuncs`，loader 已导出；
- Windows 需要导出 `luaopen_testlog`（上面的 `TESTLOG_EXPORT`）。

## 4. 编译

头文件目录：`<LuaLoader>/lib/lua-5.5.1/src`。
模块必须链接 loader（Linux/Android 链接 `.so` 写入 `DT_NEEDED`，Windows 链接导入库），
否则 Android 上会报 `cannot locate symbol "lua_*"`。

```bash
INC=-I<LuaLoader>/lib/lua-5.5.1/src

# Linux：链接 loader .so；不要链接 liblua
clang -O2 -fPIC -shared -o Resources/native/linux_x64/testlog.so native/testlog.c $INC \
  -L<LuaLoader>/sdk/loader/linux_x64 -Wl,-l:libloader.linux.x64.so

# Android：链接对应架构的 loader .so
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108
$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  -O2 -fPIC -shared -o Resources/native/android_arm64/testlog.so native/testlog.c $INC \
  -L<LuaLoader>/sdk/loader/android_arm64 -Wl,-l:libloader.android.arm64.so

# Windows：链接 loader 的导入库
# 先在 LuaLoader 仓库根目录生成：./build.sh win-x64 --implib
x86_64-w64-mingw32-gcc -O2 -shared -o Resources/native/windows_x64/testlog.dll native/testlog.c $INC \
  <LuaLoader>/sdk/windows_x64/libloader.windows.x64.dll.a
```

可用 `readelf -d Resources/native/linux_x64/testlog.so | grep NEEDED` 确认
`libloader.linux.x64.so` 已写入。

> 不要把 Lua 静态库/动态库链接进模块，否则会出现第二份 Lua 运行时，与 loader 的
> `lua_State` 不兼容。链接 loader 只是为了拿到 `lua_*` 符号。

## 5. 安装与查看结果

把整个 `native_logger_test/` 目录按普通 Mod 安装（管理器安装，或手动放进 Mod 目录）。

启动游戏后：

- 加载器日志里会出现：

  ```
  native_logger_test: log file = <private_dir>/native_test.log
  ```

- `<private_dir>/native_test.log` 内容应类似：

  ```
  [2026-11-06 12:00:00] [INFO] native module loaded, compiled for Lua 5.5.1
  [2026-11-06 12:00:00] [INFO] platform=linux/x64
  [2026-11-06 12:00:00] [INFO] private_dir=...
  [2026-11-06 12:00:00] [INFO] native sum(1000000) = 500000500000
  [2026-11-06 12:00:00] [INFO] lua write_file ok=true err=nil
  [2026-11-06 12:00:00] [INFO] cleanup called
  ```

- `<private_dir>/lua_side_check.txt` 内容为 `written by Lua`。

`<private_dir>` 就是 `mod.private_dir`（内核为每个 Mod 分配的私有目录）。

## 6. 常见失败

| 现象 | 原因 |
|---|---|
| `require(testlog) failed: ... cannot locate symbol "lua_*"` | 模块没有链接 loader（缺 `DT_NEEDED`）；Windows 没链接导入库；或架构不匹配 |
| `version mismatch`（来自 `luaL_checkversion_`） | 模块不是用 `lib/lua-5.5.1/src` 头文件编译的 |
| `module 'testlog' not found` | `.so/.dll` 不在 `Resources/native/<平台>_<架构>/`，或目录名写错 |
| 日志文件打不开 | `mod.private_dir` 不存在/不可写（正常部署下由内核/管理器创建） |

## 7. 这个测试覆盖了什么

- Lua 5.5.1 下标准 Lua C 模块的加载与符号解析；
- `mod.private_dir` 的 C 侧写入；
- 纯计算接口在原生代码中的执行；
- `mod.cleanup()` 生命周期回调。
