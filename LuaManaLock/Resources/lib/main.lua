-- LuaLoader 示范 Mod：魔力锁定（ManaLock 精简版）
-- 每帧在原版逻辑执行完后，把玩家当前魔力补满。

mod.meta = { pkg_id = "lzup.lua.manalock", version = "1.0.0" }

local stat_mana
local stat_mana_max
local fired = false

function lock_mana(instance)
    if not fired then
        fired = true
        mod.info("魔力锁定已生效")
    end
    -- 显式指定 int32，不依赖内核的字段类型查询（Android 上更稳）
    local max = mod.patch.get_field_value(stat_mana_max, instance, "int32")
    mod.patch.set_field_value(stat_mana, instance, max, "int32")
end

function setup()
    local player = mod.patch.get_type("Terraria", "Player")
    if not player then return end

    stat_mana = mod.patch.get_field(player, "statMana")
    stat_mana_max = mod.patch.get_field(player, "statManaMax")

    -- postfix：等 ResetEffects（每帧）执行完再锁满，否则会被原版覆盖
    mod.patch.install_hook(mod.patch.get_method(player, "ResetEffects", 0), { postfix = lock_mana })
end

todo_list = { "setup" }
