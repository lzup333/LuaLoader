# LuaLoader

LuaLoader is a TEFKernel ModLoader: the kernel handles injection and low-level hooks, LuaLoader runs
your Lua scripts and hands the kernel's patchlib powers to them through a `mod` table.

In one line: **write mods in Lua, no compiling, one script works on every platform.**

> Author: lzup333

## What it does

- Runs a mod's `main.lua`, manages its lifecycle and installs hooks;
- Gives scripts the `mod` table: logging, private-file IO, and `mod.patch.*` (types/fields/methods/hooks);
- Each mod gets its own Lua state, so mods don't interfere;
- Currently supports **Android arm64 / arm** and **Linux x64**.

## Layout

```
TEFKernel-LuaLoader/
├── CMakeLists.txt / CMakePresets.json   # build script and presets
├── Info.json / Manifest.json            # loader package info
├── LuaManaLock/                         # bundled demo mod (mana lock)
├── includes/                            # headers (core / logger / lua_engine / lua_api / tefkernel)
├── mod-api/                             # log level definitions
├── lib/                                 # bundled Lua 5.4 and spdlog
└── src/                                 # sources
```

## Build

Needs CMake ≥ 3.28 and a C++17 compiler. Lua and spdlog are bundled under `lib/`, nothing else to install.

```bash
cmake --preset linux-x86_64-release
cmake --build --preset linux-x86_64-release
```

The output is `libloader.<platform>.<arch>.so` (Linux x64 → `libloader.linux.x64.so`).

For Android you need the NDK:

```bash
export ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.0.13004108
cmake --preset android-arm64-release -DANDROID_NDK="$ANDROID_NDK_HOME"
cmake --build --preset android-arm64-release
# arm32: android-arm32-release
```

## Writing a mod

A mod is just a folder, laid out like a KernelLoader mod, except the entry is `main.lua` instead of a `.so`:

```
my_lua_mod/
├── Info.json          # mod metadata
├── Manifest.json      # manifest; parentLoader = lzup333.lualoader
├── luamod.json        # tells the loader which script to run
└── Resources/
    └── lib/
        └── main.lua   # entry script
```

`luamod.json`:

```json
{ "main": "main.lua" }
```

`main.lua` (this is the bundled ManaLock demo — keeps mana full every frame):

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

Only `main.lua` is run; other `.lua` files are not auto-run. Use `require("name")` to load them.

## The `mod` table

Context: `mod.id`, `mod.platform` / `mod.arch` (`android`/`linux`, `arm64`/`arm`/`x64`/`x86`),
`mod.private_dir`, `mod.mod_dir`, `mod.version`.

Logging: `mod.log(level, ...)` where level is `"trace"`/`"debug"`/`"info"`/`"warn"`/`"error"`/`"critical"`/`"fatal"`
(or a number 0–6), plus shortcuts `mod.trace`/`mod.debug`/`mod.info`/`mod.warn`/`mod.error`/`mod.fatal`.

Private files (only inside `mod.private_dir`, no `..` or absolute paths):
`mod.read_file(name)` → string or `nil, err`; `mod.write_file(name, data)` → `true`; `mod.file_exists(name)` → boolean.

Lifecycle: `mod.init()` runs on init, `mod.cleanup()` runs on unload.

## Platform differences

Prefer task lists: declare functions outside and list their names; the loader runs
`todo_list` → `todo_list_<platform>` → `todo_list_<platform>_<arch>`, only the ones matching the current platform.

```lua
function setup()         mod.info("common") end
function android_setup() mod.info("mobile") end

todo_list = { "setup" }
todo_list_android = { "android_setup" }        -- Android only
-- todo_list_android_arm64 = { "..." }         -- Android arm64 only
```

Entries may also be the functions themselves (`{ setup }`). For small differences you can just use
`if mod.platform == "android" then ... end`.

## `mod.patch`

Types/fields/methods:

- `mod.patch.get_type(ns, name)` — e.g. `get_type("Terraria", "Player")`
- `mod.patch.get_field(type, name)` / `mod.patch.get_property(type, name)`
- `mod.patch.get_method(type, name[, argc])`
- `mod.patch.new_instance(type)` / `mod.patch.get_parent(type)` / `mod.patch.type_name(type)`
- `mod.patch.get_basic_type(name)` — `"int32"`/`"float"`/`"bool"`/`"object"` ...
- `mod.patch.free(handle)`

Fields:

```lua
mod.patch.get_field_value(field, instance[, type])
mod.patch.set_field_value(field, instance, value[, type])
```

`type` may be omitted (looked up by the kernel), but it's better to pass it, e.g. `"int32"`.
**On Android you should always pass it**, because Android reads/writes the field's real pointer and
an unknown type can read the wrong thing.

Struct fields (e.g. `Vector2`) and arrays:

```lua
-- Vector2, or any 8-byte struct that starts with two floats
local x, y = mod.patch.get_field_vec2(field, instance)
mod.patch.set_field_vec2(field, instance, x, y)

-- Raw bytes, length = field size (pair with string.pack/unpack for arbitrary structs)
local raw = mod.patch.get_field_raw(field, instance)   -- nil on failure
mod.patch.set_field_raw(field, instance, raw)

-- Arrays (e.g. the object arrays Main.npc / Main.projectile)
local n = mod.patch.array_length(array)
local obj = mod.patch.array_at(array, i)               -- omitted type = object (pointer)
local num = mod.patch.array_at(array, i, "float")      -- value arrays need a type name
```

You can get an object array handle from a static field, e.g.
`mod.patch.get_field_value(mod.patch.get_field(main, "projectile"), nil, "object")`.

Methods: `mod.patch.invoke(method, [instance, ] ...)`. A reference return value comes back as
userdata; no result comes back as `nil`.

Hooks:

```lua
local hook = mod.patch.install_hook(method, {
    -- before the original: return true to skip it (and optionally return a replacement value)
    prefix = function(instance, args, result) return false end,
    -- after the original: result is its return value
    postfix = function(instance, args, result) end,
})
hook:remove()   -- remove manually
```

At least one of `prefix`/`postfix` is required. Up to 32 hooks per loader. A hook stays active until
`hook:remove()` or mod unload — dropping the returned value does **not** remove it.

## Demo

`LuaManaLock/` in this project is a complete demo (mana lock). More mods (e.g. `LuaHealthLock`, a
health lock) live in the separate `LuaMods` project.

## Packaging and deployment

Package the per-platform `libloader.<platform>.<arch>.so` into `lualoader.tefpkg` with TEFPkg-Tool,
then zip it together with `Info.json` and `Manifest.json` — that zip is the loader installer.

After install, the kernel extracts the mod's `Resources/` into its private dir, so the entry script
ends up at `<private_dir>/lib/main.lua`. The loader looks for it in this order:

1. `<private_dir>/lib/<main>`
2. `<private_dir>/<main>`
3. `<config dir>/<main>`
4. `<config dir>/Resources/lib/<main>`

## License

LuaLoader itself is **AGPL-3.0-or-later** (see `LICENSE`). `includes/tefkernel/` and `mod-api/` come
from TEFKernel (MIT); Lua, spdlog and json.hpp are MIT too.
