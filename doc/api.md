# LuaLoader API 参考

用 Lua 写 Mod；每个 Mod 跑在独立的 Lua 状态机里，通过全局表 `mod` 访问一切能力。

## 最小 Mod

```
my_mod/
├── Info.json            # Mod 元信息
├── Manifest.json        # parentLoader = "lzup333.lualoader"
├── luamod.json          # { "main": "main.lua" }
└── Resources/lib/main.lua
```

```lua
mod.meta = { pkg_id = "lzup.lua.hello", version = "1.0.0" }  -- Mod 唯一 id + 版本

function setup()
    mod.info("hello")     -- 打日志：信息会进内核日志
end

todo_list = { "setup" }   -- 启动时按顺序执行清单里的函数
```

## 生命周期

- `todo_list`（通用）→ `todo_list_<平台>`（如 `todo_list_android`）→ `todo_list_<平台>_<架构>`。
  清单元素是函数名字符串，也可直接写函数。
- 可选 `mod.init()`（清单之后调用）/ `mod.cleanup()`（卸载时调用）。
- 定义 `mod.on_gui()` 后加载器为该 Mod 开一个窗口并每帧回调，见 [`gui.md`](gui.md)。

## `mod` 表

| 字段 | 说明 |
|---|---|
| `mod.id` | Mod 唯一 id |
| `mod.version` | 版本字符串 |
| `mod.platform` / `mod.arch` | `android/linux/windows/...`、`arm64/arm/x64/x86` |
| `mod.mod_dir` | 入口脚本所在目录（`require` 搜索根） |
| `mod.private_dir` | 私有数据目录，文件 API 的根 |

`mod.meta` 可覆盖包内元信息：`{ pkg_id, version, version_code, api_version }`。

### 日志

```lua
mod.log("info", "x =", 42)   -- level: trace/debug/info/warn/warning/error/critical/fatal 或 0~6
mod.info("已锁定")            -- 快捷方法：trace/debug/info/warn/error/fatal
```

多参数空格拼接，非字符串自动转字符串。

### 文件

限 `mod.private_dir` 内，禁绝对路径和 `..`：

```lua
local data, err = mod.read_file("cfg.txt")   -- 失败 nil, err
mod.write_file("cfg.txt", "x")               -- 成功 true
mod.file_exists("cfg.txt")                   -- true/false
```

## `mod.patch`：调用内核

所有句柄在 Lua 里是 lightuserdata / GC userdata，可直接互相传参。

### 取类型与成员

```lua
local player = mod.patch.get_type("Terraria", "Player")      -- 命名空间 + 类名 -> 类型句柄
local field  = mod.patch.get_field(player, "statMana")       -- 字段句柄（含父类）
local prop   = mod.patch.get_property(player, "SomeProp")    -- 属性句柄
local method = mod.patch.get_method(player, "ResetEffects", 0)  -- 0 参重载；唯一重载可省略个数
```

改一个字段的最小流程：

```lua
local player = mod.patch.get_type("Terraria", "Player")   -- 1. 取类型
local mana   = mod.patch.get_field(player, "statMana")    -- 2. 取字段
local p      = ...                                         -- 3. 玩家实例（来自 hook 参数等）
mod.patch.set_field_value(mana, p, 999, "int32")          -- 4. 写：字段, 实例, 值, 类型
```

| 函数 | 说明 |
|---|---|
| `get_basic_type(name)` | `"int32"`/`"float"`/`"bool"`/`"object"`… |
| `get_method_by_names(type, name, {参数名...})` | 按参数名精确选重载 |
| `property_get_method/set_method(prop)` | 属性访问器 |
| `type_name / get_parent / get_full_name / get_namespace` | 类型信息 |
| `get_inner_type(parent, name)` | 嵌套类型（如 `ItemID.Sets`，见下） |
| `get_fields/methods/properties/inner_types(type[, 含父类])` | 枚举成员，返回句柄表 |
| `get_method_by_param_types(type, name, {类型句柄...})` | 按参数类型选重载 |
| `get_method_by_signature(type, name, {类型句柄...}, {参数名...})` | 同上 + 参数名 |
| `make_generic_type(def, {type...})` / `make_generic_instance(m, {type...})` | 泛型实例化 |
| `method_name/param_count/token/is_instance/is_static` | 方法信息 |
| `field_name/is_const/is_instance/is_static` / `property_name` | 成员信息 |
| `is_valid(handle)` / `free(handle)` | 校验 / 手动释放（一般不用） |

> **嵌套类型**：`ItemID.Sets`、`PrefixLegacy.ItemSets` 在 Android 上不能直接 `get_type("Terraria.ID","ItemID.Sets")`，
> 要先取外层再 `get_inner_type(ItemID, "Sets")`。

### 字段读写

```lua
mod.patch.get_field_value(field, instance[, type])   -- instance=nil 即静态字段
mod.patch.set_field_value(field, instance, value[, type])
```

`type` 可省略（内核查），但 **Android 必须写**。类型名：`int8/16/32/64`、`uint8/16/32/64`、
`bool`、`float`、`double`、`pointer`、`object`、`char`，伪类型 `string`（`System.String`）。

结构体与原始字节：

```lua
local x, y = mod.patch.get_field_vec2(field, instance)   -- Vector2 等两个 float 开头
mod.patch.set_field_vec2(field, instance, x, y)
mod.patch.get_field_raw(field, instance[, size])         -- 原始字节，省略 size 按字段推断
mod.patch.set_field_raw(field, instance, raw)            -- 1~16 字节
```

### 数组

```lua
local n   = mod.patch.array_length(array)
local obj = mod.patch.array_at(array, i)             -- 省略类型按对象
local num = mod.patch.array_at(array, i, "float")    -- 值类型数组给类型名
mod.patch.array_set(array, i, value[, type])
mod.patch.array_fill(array, value[, type])
mod.patch.array_create(size, elem_type)              -- elem_type 为类型句柄或名
mod.patch.array_at_raw(array, i, size)               -- 原始字节读
mod.patch.array_set_raw(array, i, raw)
```

对象数组（如 `Main.npc`）用 `get_field_value(field, nil, "object")` 取：

```lua
local main = mod.patch.get_type("Terraria", "Main")
local npcs = mod.patch.get_field_value(mod.patch.get_field(main, "npc"), nil, "object")  -- 静态字段，instance=nil
for i = 0, mod.patch.array_length(npcs) - 1 do
    local npc = mod.patch.array_at(npcs, i)   -- 对象数组：省略类型，返回句柄
    if mod.patch.is_valid(npc) then
        -- 拿到的是“借用”句柄，只在本次循环里有效；要缓存用 retain
    end
end
```

### 托管字符串 / 创建实例 / 调用方法

```lua
mod.patch.string_create("hello")  -- 句柄由 Lua GC 管理
mod.patch.string_value(h)         -- "hello"
mod.patch.string_empty(h) / mod.patch.string_length(h)

mod.patch.new_instance(type)                 -- 无参
mod.patch.new_instance(type, 1, "x")         -- 按 .ctor 参数个数自动匹配
mod.patch.construct(ctor, 1, "x")            -- 显式构造函数

mod.patch.invoke(method, [instance,] ...)    -- 实例方法传 instance，静态不传

-- 实例方法 Player.Heal(int)：第二个参数是接收者
mod.patch.invoke(heal, player, 20)
-- 静态方法：不传 instance
mod.patch.invoke(some_static_method, arg1, arg2)
```

`invoke` 引用类型返回**借用句柄**（跨帧要 `retain`），`void` 返回 `nil`，失败返回 `false`。

### 容器（Dictionary / List）

```lua
local int_t = mod.patch.get_basic_type("int32")            -- 类型句柄
local dict  = mod.patch.dictionary_create(int_t, int_t)    -- create 传句柄
mod.patch.dictionary_add(dict, 1, "int32", 100, "int32")   -- 增删改查传类型名
mod.patch.dictionary_get_value(dict, 1, "int32", "int32")  -- 100
```

`dictionary_add/set_value/get_value/remove`、`list_add/remove` 的类型参数是**类型名字符串**；
`dictionary_create/list_create` 用**类型句柄**。另有 `dictionary_length/clear`、
`list_remove_at/clear/copy_from/get_array`、`array_empty`。

### C 快通道（内存 / 指针，仅 Android）

```lua
local ptr = mod.patch.field_pointer(field, instance)    -- 真实指针；桌面端 nil
mod.patch.mem_read(ptr, offset, size) / mod.patch.mem_write(ptr, offset, raw)
mod.patch.ptr_add(ptr, off) / mod.patch.ptr_deref(ptr, off)
mod.patch.mem_read_values(ptr, off, count, type)        -- 批量读 -> 表
mod.patch.mem_write_values(ptr, off, table, type)
mod.patch.field_size(field)
```

把“逐元素跨边界”压成“一次批量读”，适用于任何平坦内存布局（Tile 快通道、内联结构体数组、
对象指针数组等）。桌面端 `field_pointer` 返回 `nil`，退回托管路径即可保持一份脚本全平台。

### 按值结构体参数（仅 Android）

```lua
mod.patch.invoke_value_args(method, { mod.patch.struct_arg({"float","float"}, {x,y}), 其它参数 })
```

### Hook

```lua
local hook = mod.patch.install_hook(method, {
    prefix  = function(instance, args, result) return false end,  -- 返回 true 跳过原方法
    postfix = function(instance, args, result) end,               -- result 是原返回值
    -- copy = true,            -- instance/对象参数/返回值包成 GC 托管副本，可跨帧缓存
    -- override_result = true, -- 允许 postfix 返回非 nil 覆盖返回值
})
hook:remove()
```

- `args` 是 1 起始的参数数组；静态方法 `instance` 为 `nil`；`prefix`/`postfix` 至少一个。
- 最多 1024 个钩子；装上后到 `remove()` 或 Mod 卸载前一直有效。

一个真实例子：每次玩家 `ResetEffects` 后把魔力补满（postfix，不被原版覆盖）：

```lua
local stat_mana, stat_mana_max

function lock_mana(instance)                            -- instance = 这个 Player
    local max = mod.patch.get_field_value(stat_mana_max, instance, "int32")
    mod.patch.set_field_value(stat_mana, instance, max, "int32")
end

function setup()
    local player = mod.patch.get_type("Terraria", "Player")
    stat_mana     = mod.patch.get_field(player, "statMana")
    stat_mana_max = mod.patch.get_field(player, "statManaMax")
    mod.patch.install_hook(
        mod.patch.get_method(player, "ResetEffects", 0),
        { postfix = lock_mana })
end

todo_list = { "setup" }
```

### 句柄生命周期

普通 API 返回的句柄是**借用**的，只在当前调用内有效——**不要**把 hook 里的 `instance`/参数
存起来跨帧用。需要缓存用 `retain`（GC 托管副本，不必 `free`）：

```lua
local npc = mod.patch.retain(instance)
```

## `mod.item`：自定义物品

在 `mod.init` 阶段注册：

```lua
mod.item.register{
    name    = "test_sword",        -- 必填，同 Mod 内唯一
    texture = "sword.png",         -- PNG；不放用内核占位贴图
    tooltip = true,                -- 显示简介
    sacrifice = 1,                 -- 研究所需数量；0 不可研究
    defaults = { damage = 25, useTime = 20, useStyle = 1, autoReuse = true,
                 width = 40, height = 40, melee = true, rare = 2, maxStack = 1 },
    can_use = function(player, item, ignore_cursed) return true end,
    language = { en = { name = "Test Sword", tooltip = "..." },
                 zh = { name = "测试之剑", tooltip = "..." } },
    recipe  = { result = 1, materials = { { id = 9, count = 10 } }, station = 18 },
    animation = { ticks = 8, frames = 9 },   -- 竖向帧图；texture 高度须被 frames 整除
}
```

- `defaults` 字段名与 `Terraria.Item` 一致；值类型不符会打日志忽略。函数形式 `function(item) end`（非托管句柄）。
- 语言键：`en/de/it/fr/es/ru/zh/pt/pl/ja/ko/zh-tw`。
- PNG 放 `Resources/Image/` 或 `Resources/lib/Image/`；解码为 RGBA32（Android 自动 Y 翻转,因为不翻转贴图就是反的！翻转是修正处理）。
- `mod.item.runtime_id(name)` 取内核分配的 ID（注册阶段通常为 -1）。

## `mod.projectile`：自定义弹幕

```lua
mod.projectile.register{
    name    = "lua_orb",          -- 必填，同 Mod 内唯一
    texture = "orb.png",
    frames  = 0,                  -- 竖向帧图帧数；0 = 单帧整图
    no_ai     = false,            -- true：不接管 AI，沿用 aiStyle
    vanilla_ai= false,            -- true：原版 AI 先跑，再调 ai
    defaults = { width = 16, height = 16, damage = 18, timeLeft = 300,
                 friendly = true, aiStyle = 0 },
    ai = function(p) end,         -- 写了 ai 默认跳过原版 AI
}
```

AI 模式：

| 模式 | 执行顺序 |
|---|---|
| 默认（`ai`） | 只调 `ai`，跳过原版 `Projectile.AI()` |
| `vanilla_ai = true` | 原版 AI 先跑，再调 `ai` |
| `no_ai = true` | 只跑原版 AI，不调 `ai` |

> ⚠️ **写了 `ai` 就要自己算位移**：原版 `Projectile.Update()` 只在 `AI()` 里做 `position += velocity`。
> 只改 `velocity` 不写 `position`，弹幕会停原地。`timeLeft` / 命中判定仍由原版负责。

`mod.projectile.runtime_id(name)` 取实时 ID。卸载 Mod 自动注销。

## `mod.recipe`：添加任意配方

不限于自定义物品，**原版物品**也可以加配方：

```lua
mod.recipe.add{
    result    = 9,                              -- 产物 id，或 { id = 9, count = 10 }
    materials = { { id = 20, count = 5 },       -- 材料：{ id, count }
                  { id = 22, count = 2 } },
    station   = 18,                             -- 合成站方块 id；-1 = 无
}

```

- 可以在 `setup()` 里调用；配方会在**下一次游戏重建配方表**（进世界/加入服务器/换语言）时生效。
- 卸载 Mod 时自动清理它注册的配方，不需要手动删除。
- 一条配方只写一个产物；要多个产物就调多次 `add`。
- `materials` 至少一项；材料数量上限与游戏一致（通常 15 种）。

## `mod.net`：联机同步（服务端权威）

```lua
mod.net.mode()                          -- 0 单机 / 1 客户端 / 2 专用服 / 3 开房
mod.net.send(channel, payload)          -- 客户端 -> 服务端发消息
mod.net.on(channel, function(payload, player) end)  -- 服务端按频道路由到回调
mod.net.tile_kill(x, y[, no_item])      -- 请求服务端挖图格
mod.net.tile_square(x, y, w, h)         -- 请求服务端同步区域
```

> `Main.netMode`：**2 = 专用服务端，3 = 开房**。判断“我是服务端”写 `mode == 2 or mode == 3`。

要点：

- 客户端只发包，世界修改在服务端做；
- 服务端直接 `WorldGen.KillTile` **不会自动广播**（原版靠 `Player.PickTile`），批量改图后要显式 `mod.net.tile_square(...)`；
- 消息走原版已知消息号 + magic 校验，未知消息号在 PC↔Android 传输层会被丢弃。

完整骨架（客户端请求 → 服务端执行并广播）：

```lua
-- 客户端：某处点击后，请求服务端挖掉 (x, y)
function request_mine(x, y)
    if mod.net.mode() == 1 then            -- 只有客户端才发包
        mod.net.send("mine", x .. "," .. y)
    end
end

-- 服务端：给 "mine" 频道注册回调（这段只在服务端运行）
mod.net.on("mine", function(payload, player)
    local sx, sy = payload:match("^(%d+),(%d+)$")
    mod.net.tile_kill(tonumber(sx), tonumber(sy))   -- 服务端权威执行，并广播给所有客户端
end)
```

## 常见陷阱

- **Android 字段读写必须显式写类型**。
- **不要跨帧缓存 hook 里的 `instance`**——用 `retain` 或 `copy = true`。
- `copy`/`retain` 是同对象的另一个句柄，读写仍作用于原对象。
- 想改动**不被原版每帧逻辑覆盖**用 `postfix`；想拦截用 `prefix` 返回 `true`。
- 字段/方法可能因版本差异缺失，取不到记得判空。
