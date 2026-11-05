# 原生模块（C/C++ 扩展）

Mod 可自带原生库（`.so` / `.dll`），用于性能敏感或 Lua 难实现的场景（例如批量数值计算）。
纯 Lua Mod 不受影响。

原生模块就是**标准 Lua C 模块**：不再需要专用头文件、API 表或 `ll_set_api` 注入。

## 目录约定

```
Resources/native/<平台>_<架构>/<模块名>.so      # 首选：linux_x64 / android_arm64 / windows_x64 ...
Resources/native/<平台>/<模块名>.so             # 次选（不区分架构）
```

部署后加载器把命中的目录加入 `package.cpath`；`require` 时由 Lua 的 `loadlib` 负责加载，
State 关闭时由 Lua 自动卸载，不残留句柄。

## Lua 侧

普通 C 模块，`require` 即用：

```lua
local hello = require("hello")   -- 找 hello.so，调 luaopen_hello
print(hello.greet("world"))
```

## C 侧

直接包含与加载器同版本的 Lua 头文件即可：

```c
#include <lua.h>
#include <lauxlib.h>

static int greet(lua_State *L) {
    lua_pushstring(L, "hello");
    return 1;
}

static const luaL_Reg hello_funcs[] = {
    {"greet", greet},
    {NULL, NULL},
};

int luaopen_hello(lua_State *L) {
    luaL_newlib(L, hello_funcs);
    return 1;
}
```

- 只能用 Lua C API 做计算/数据处理；加载器不再向模块暴露 TEFKernel / patchlib API。
- 必须使用**与 LuaLoader 相同的 Lua 版本（当前 5.5.1）**的头文件编译；Lua 不同版本之间没有 ABI 兼容。

## 编译

头文件目录：`<LuaLoader>/lib/lua-5.5.1/src`。

```bash
INC=-I<LuaLoader>/lib/lua-5.5.1/src

# Linux：不要链接 liblua；lua_* 在运行时由 loader 解析
clang -O2 -fPIC -shared -o hello.so hello.c $INC

# Android：同样不要链接 liblua
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  -O2 -fPIC -shared -o hello.so hello.c $INC

# Windows：链接 loader 的导入库（否则 lua_* 无法解析）
x86_64-w64-mingw32-gcc -O2 -shared -o hello.dll hello.c $INC \
  <SDK>/windows_x64/libloader.windows.x64.dll.a
```

Windows 导入库不随源码仓库分发，请从 Releases 下载 `LuaLoader-sdk-v<版本>.zip`，
解压后把其中的 `sdk/` 目录当作上面的 `<SDK>`。需要自行生成时：
`./build.sh win-x64 win-x86 --implib --sdk-zip`。

> 不要把 Lua 静态库/动态库链接进模块——那会导致模块里出现第二份 Lua 运行时，
> 与加载器的 `lua_State` 不兼容。Linux/Android 保持 `lua_*` 未定义即可。

打包即普通 Mod 目录：`Resources/lib/main.lua` + `Resources/native/<平台>_<架构>/hello.so`。

## 排查

- `require` 失败会返回错误字符串（找不到模块、符号缺失、架构不匹配等）；
- Windows 提示缺少 `lua_*` 符号：确认链接了对应架构的 `libloader.*.dll.a`；
- Linux/Android 提示 `undefined symbol: lua_*`：确认没有把模块用 `-Wl,-z,defs` 之类强制解析，
  且 loader 版本与模块编译时使用的 Lua 头文件一致；
- 报 `dynamic libraries not enabled` 表示内置 Lua 没开 `LUA_USE_DLOPEN`（本项目已开）。

## 安全

原生模块等于在游戏进程内执行任意原生代码，风险高于纯 Lua。请只安装信任来源的 Mod。
