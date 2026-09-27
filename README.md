# LuaLoader

LuaLoader 是 TEFKernel 的一个 ModLoader：内核负责注入和底层 hook，LuaLoader 负责把 Lua 脚本跑起来，
并把内核的 patchlib 能力通过一张 `mod` 表交给脚本使用。

一句话：**Mod 用 Lua 写，不用编译，一份脚本全平台通用。**

> 作者：lzup333

## 它做什么

- 加载 Mod 目录里的 `main.lua`，跑脚本、管生命周期、装钩子；
- 给脚本提供 `mod` 表：日志、私有目录读写、`mod.patch.*`（类型/字段/方法/钩子）；
- 每个 Mod 用独立的 Lua 状态机，互不干扰；
- 目前支持 **Android arm64 / arm** 和 **Linux x64**。

## 目录结构

```
TEFKernel-LuaLoader/
├── CMakeLists.txt / CMakePresets.json   # 构建脚本和预设
├── Info.json / Manifest.json            # 加载器包信息
├── LuaManaLock/                         # 自带示范 Mod（魔力锁定）
├── includes/                            # 头文件（core / logger / lua_engine / lua_api / tefkernel）
├── mod-api/                             # 日志级别定义
├── lib/                                 # 内置 Lua 5.4 和 spdlog
└── src/                                 # 源码
```

## 编译

需要 CMake ≥ 3.28 和支持 C++17 的编译器。Lua 和 spdlog 已经放在 `lib/` 里，不用另外装。

```bash
cmake --preset linux-x86_64-release
cmake --build --preset linux-x86_64-release
```

产物是 `libloader.<平台>.<架构>.so`（Linux x64 就是 `libloader.linux.x64.so`）。

Android 需要 NDK：

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108
cmake --preset android-arm64-release -DANDROID_NDK="$ANDROID_NDK_HOME"
cmake --build --preset android-arm64-release
# arm32 用 android-arm32-release
```

## 写一个 Mod

一个 Mod 就是一个目录，结构和 KernelLoader 的 Mod 一样，只是入口从 `.so` 换成了 `main.lua`：

```
my_lua_mod/
├── Info.json          # Mod 元信息
├── Manifest.json      # 清单，parentLoader 写 lzup333.lualoader
├── luamod.json        # 告诉加载器入口脚本是谁
└── Resources/
    └── lib/
        └── main.lua   # 入口脚本
```

`luamod.json`：

```json
{ "main": "main.lua" }
```

`main.lua` 长这样（这是自带的 ManaLock 示范，每帧把魔力补满）：

```lua
mod.meta = { pkg_id = "lzup.lua.manalock", version = "1.0.0" }

local stat_mana
local stat_mana_max

function lock_mana(instance)
    local max = mod.patch.get_field_value(stat_mana_max, instance, "int32")
    mod.patch.set_field_value(stat_mana, instance, max, "int32")
end

function setup()
    local player = mod.patch.get_type("Terraria", "Player")
    if not player then return end

    stat_mana = mod.patch.get_field(player, "statMana")
    stat_mana_max = mod.patch.get_field(player, "statManaMax")
    mod.patch.install_hook(mod.patch.get_method(player, "ResetEffects", 0), { postfix = lock_mana })
end

todo_list = { "setup" }
```

加载器只跑 `main.lua`，其它 `.lua` 文件不会自动执行，需要的话用 `require("文件名")` 按需加载。

## `mod` 表

环境信息：

- `mod.id`：Mod 的 id
- `mod.platform` / `mod.arch`：当前平台和架构（`android`/`linux`…、`arm64`/`arm`/`x64`/`x86`）
- `mod.private_dir` / `mod.mod_dir` / `mod.version`

日志：

- `mod.log(level, ...)`，level 是 `"trace"`/`"debug"`/`"info"`/`"warn"`/`"error"`/`"critical"`/`"fatal"`（或数字 0~6）
- 快捷方法：`mod.trace` / `mod.debug` / `mod.info` / `mod.warn` / `mod.error` / `mod.fatal`

私有目录读写（只能在 `mod.private_dir` 里，禁止 `..` 和绝对路径）：

- `mod.read_file(name)` → 内容字符串，失败返回 `nil, err`
- `mod.write_file(name, data)` → 成功返回 `true`
- `mod.file_exists(name)` → `true`/`false`

生命周期：

- `mod.init()`：Mod 初始化时调用
- `mod.cleanup()`：Mod 卸载时调用

## 平台差异

推荐用“任务清单”：把函数定义写在外面，清单里只写函数名，加载器按 `todo_list` →
`todo_list_<平台>` → `todo_list_<平台>_<架构>` 的顺序执行与当前平台匹配的清单。

```lua
function setup()         mod.info("通用逻辑") end
function android_setup() mod.info("移动端逻辑") end

todo_list = { "setup" }
todo_list_android = { "android_setup" }        -- 只在 Android 执行
-- todo_list_android_arm64 = { "..." }         -- 只在 Android arm64 执行
```

清单里的元素也可以直接写函数（`{ setup }`）。零星差异也可以用 `if mod.platform == "android" then ... end`。

## `mod.patch`（调用内核能力）

取类型/字段/方法：

- `mod.patch.get_type(ns, name)`：按命名空间+类名取类型，如 `get_type("Terraria", "Player")`
- `mod.patch.get_field(type, name)` / `mod.patch.get_property(type, name)`
- `mod.patch.get_method(type, name[, argc])`
- `mod.patch.new_instance(type)` / `mod.patch.get_parent(type)` / `mod.patch.type_name(type)`
- `mod.patch.get_basic_type(name)`：`"int32"`/`"float"`/`"bool"`/`"object"` 等
- `mod.patch.free(handle)`

读写字段：

```lua
mod.patch.get_field_value(field, instance[, type])
mod.patch.set_field_value(field, instance, value[, type])
```

`type` 可以省略（由内核查），但建议显式写，例如 `"int32"`。**Android 上请一定写**，因为
Android 走的是字段真实指针，类型不明确容易读错。

调用方法：

```lua
mod.patch.invoke(method, [instance, ] ...)
```

装钩子：

```lua
local hook = mod.patch.install_hook(method, {
    -- 原方法执行前：返回 true 表示跳过原方法（可再返回一个值作为返回值）
    prefix = function(instance, args, result) return false end,
    -- 原方法执行后：result 是原方法的返回值
    postfix = function(instance, args, result) end,
})
hook:remove()   -- 手动卸载
```

- `prefix` / `postfix` 至少写一个；
- 最多 32 个钩子，钩子装上后一直有效，直到 `hook:remove()` 或 Mod 卸载（丢弃返回值不会导致失效）。

## 示范 Mod

项目里的 `LuaManaLock/` 是一个完整示范（魔力锁定）。更多 Mod（例如锁血的 `LuaHealthLock`）
放在单独的 `LuaMods` 项目里。

## 打包和部署

打包需要 TEFPkg-Tool：把各平台的 `libloader.<平台>.<架构>.so` 打进 `lualoader.tefpkg`，
再和 `Info.json`、`Manifest.json` 一起压缩成一个 zip 就是加载器安装包。

部署后内核会把 Mod 的 `Resources/` 释放到 Mod 私有目录，所以入口脚本最终在
`<private_dir>/lib/main.lua`；加载器会按下面顺序找入口脚本：

1. `<private_dir>/lib/<main>`
2. `<private_dir>/<main>`
3. `<配置文件目录>/<main>`
4. `<配置文件目录>/Resources/lib/<main>`

## 许可证

LuaLoader 本体是 **AGPL-3.0-or-later**（见 `LICENSE`）。
`includes/tefkernel/` 和 `mod-api/` 来自 TEFKernel，是 MIT；Lua、spdlog、json.hpp 也都是 MIT。
