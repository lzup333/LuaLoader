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
- 必须使用 **与 LuaLoader 相同的 Lua 版本（当前 5.5.1)** 的头文件编译；Lua 不同版本之间没有 ABI 兼容。

## 编译

头文件目录：`<LuaLoader>/lib/lua-5.5.1/src`。

**模块必须记录对 loader 的依赖**：

- Linux / Android：链接 loader 的 `.so`，让模块产生 `DT_NEEDED libloader.<平台>.<架构>.so`；
- Windows：链接 loader 的导入库。

```bash
INC=-I<LuaLoader>/lib/lua-5.5.1/src

# Linux：链接 loader .so（写入 DT_NEEDED）；不要链接 liblua
clang -O2 -fPIC -shared -o hello.so hello.c $INC \
  -L<SDK>/loader/linux_x64 -Wl,-l:libloader.linux.x64.so

# Android：链接对应架构的 loader .so
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
  -O2 -fPIC -shared -o hello.so hello.c $INC \
  -L<SDK>/loader/android_arm64 -Wl,-l:libloader.android.arm64.so

# Windows：链接 loader 的导入库
x86_64-w64-mingw32-gcc -O2 -shared -o hello.dll hello.c $INC \
  <SDK>/windows_x64/libloader.windows.x64.dll.a
```

> **为什么必须链接 loader？**
> Android 上内核以 `RTLD_LOCAL` 从 memfd 加载 loader，`lua_*` 符号不在全局作用域。
> 模块如果只是留下未定义的 `lua_*`，`dlopen` 会报 `cannot locate symbol "lua_..."`。
> 链接 loader 后模块带着 `DT_NEEDED`，bionic 会把**已加载的 loader**当作模块的直接依赖，
> 在模块自己的 local group 里解析符号——这比 `RTLD_GLOBAL` 提升可靠（对已加载库提升无效）。

SDK 压缩包 `LuaLoader-sdk-v<版本>.zip` 提供链接所需文件：

```
sdk/windows_x64/libloader.windows.x64.dll.a
sdk/windows_x86/libloader.windows.x86.dll.a
sdk/loader/linux_x64/libloader.linux.x64.so
sdk/loader/linux_x86/libloader.linux.x86.so
sdk/loader/android_arm64/libloader.android.arm64.so
sdk/loader/android_arm/libloader.android.arm.so
```

需要自行生成时：`./build.sh all --sdk-zip`。

> 不要把 Lua 静态库/动态库链接进模块——那会导致模块里出现第二份 Lua 运行时，
> 与加载器的 `lua_State` 不兼容。链接 loader 只是为了拿到 `lua_*` 符号。

打包即普通 Mod 目录：`Resources/lib/main.lua` + `Resources/native/<平台>_<架构>/hello.so`。

## 排查

- `require` 失败会返回错误字符串（找不到模块、符号缺失、架构不匹配等）；
- `cannot locate symbol "lua_*"`：模块没有链接 loader。用
  `readelf -d hello.so | grep NEEDED` 确认有 `libloader.<平台>.<架构>.so`；
- `version mismatch`：模块与 loader 的 Lua 版本不一致，用 `lib/lua-5.5.1/src` 重编；
- `module 'hello' not found`：检查 `Resources/native/<平台>_<架构>/` 目录名与设备架构；
- 报 `dynamic libraries not enabled` 表示内置 Lua 没开 `LUA_USE_DLOPEN`（本项目已开）。

## 安全

原生模块等于在游戏进程内执行任意原生代码，风险高于纯 Lua。请只安装信任来源的 Mod。

## 完整测试 Mod

完整可复制的测试 Mod（清单、Lua、C 模块、三平台编译命令、预期日志）见
[`native-test-mod.md`](native-test-mod.md)：用标准 C 模块把日志追加到
`mod.private_dir/native_test.log`。
