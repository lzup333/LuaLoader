# LuaLoader

**用 Lua 写 Terraria Mod，不编译，一份脚本全平台通用。**

LuaLoader 是 TEFKernel 的一个 ModLoader：内核负责注入和底层 hook，LuaLoader 把内核的
patchlib 能力封装成一张 `mod` 表交给脚本。

- 📖 完整 API 参考：[`doc/api.md`](doc/api.md)
- 🎨 图形界面：[`doc/gui.md`](doc/gui.md)
- 🧩 原生扩展：[`doc/native.md`](doc/native.md)

## 快速开始

一个 Mod 就是一个目录：

```
my_mod/
├── Info.json            # Mod 元信息
├── Manifest.json        # parentLoader 写 "lzup333.lualoader"
├── luamod.json          # { "main": "main.lua" }
└── Resources/
    └── lib/
        └── main.lua
```

`main.lua`：

```lua
mod.meta = { pkg_id = "lzup.lua.hello", version = "1.0.0" }

function setup()
    mod.info("hello from " .. mod.id)
end

todo_list = { "setup" }   -- 启动时按顺序执行这些函数
```

加载器只跑入口脚本；其它 `.lua` 用 `require("文件名")` 按需加载（`package.path` 已含 Mod 目录）。

## 能力一览

| 能力 | 入口 | 说明 |
|---|---|---|
| 日志 | `mod.info/warn/error/...` | 输出到内核日志 |
| 文件 | `mod.read_file/write_file/file_exists` | 限 `mod.private_dir` 内 |
| 图形界面 | `mod.on_gui` + `mod.gui.*` | 定义 `on_gui` 即得一个可拖动窗口 |
| 反射 | `mod.patch.*` | 类型/字段/方法/数组/钩子，全部内核能力 |
| 自定义物品 | `mod.item.register{...}` | 贴图 + 本地化 + 合成表 |
| 自定义弹幕 | `mod.projectile.register{...}` | 贴图 + AI + 帧图 |
| 联机同步 | `mod.net.*` | 服务端权威 RPC |

用法都在 [`doc/api.md`](doc/api.md)，一个完整示例（每帧补满魔力）：

```lua
local stat_mana, stat_mana_max

function lock_mana(p)
    mod.patch.set_field_value(stat_mana, p,
        mod.patch.get_field_value(stat_mana_max, p, "int32"), "int32")
end

function setup()
    local player = mod.patch.get_type("Terraria", "Player")
    stat_mana     = mod.patch.get_field(player, "statMana")
    stat_mana_max = mod.patch.get_field(player, "statManaMax")
    mod.patch.install_hook(mod.patch.get_method(player, "ResetEffects", 0), { postfix = lock_mana })
end

todo_list = { "setup" }
```

## 平台与构建

支持 **Android arm64 / arm、Linux x64 / x86、Windows x64 / x86**。Lua 与 spdlog 已内置。

```bash
# 本地
cmake --preset linux-x86_64-release
cmake --build --preset linux-x86_64-release

# 一次性构建 + 打安装包（需 NDK 与内核库）
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108
export TEFKERNEL_LIB=./libtefkernel.linux.x86_64.so
./build.sh -r --package
```

## 原生扩展

性能敏感或 Lua 不好实现的部分，可以自带 C/C++ 原生模块（`Resources/native/<平台>_<架构>/*.so`），
`require` 即用。详见 [`doc/native.md`](doc/native.md)。

## 安全提示

LuaLoader 开放了完整 Lua 标准库和原生模块加载——Mod 脚本可以读写文件、执行命令、跑原生代码。
**只安装信任来源的 Mod**；装前建议打开压缩包看一眼 `.lua` 和 `native/` 目录。

## 许可证

LuaLoader 本体 **AGPL-3.0-or-later**（[`LICENSE`](LICENSE)）。第三方组件见 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。
