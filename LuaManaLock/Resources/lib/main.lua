-- Copyright (C) 2026 lzup333
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU Affero General Public License as published
-- by the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU Affero General Public License for more details.
--
-- You should have received a copy of the GNU Affero General Public License
-- along with this program.  If not, see <https://www.gnu.org/licenses/>.
--
-- ManaLock —— LuaLoader 示范 Mod（魔力锁定）
-- 每帧在 Player.ResetEffects 原版逻辑执行完后，把当前魔力补满到上限。
--
-- 说明：
--   C 版在 Android 用 patchlib_field_get_pointer 直取字段真实指针、桌面用
--   get/set_value；Lua 版的 get_field_value/set_field_value 内部已按平台处理，
--   这里显式写 int32 即可，无功能差异。

mod.meta = { pkg_id = "lzup.lua.manalock", version = "1.0.0" }

local patch = mod.patch

-- ============ 句柄（setup 中解析后缓存，供每帧 Hook 使用）============
local F_stat_mana       -- Player.statMana    (int32)
local F_stat_mana_max   -- Player.statManaMax (int32)

-- ============ 状态 ============
local ready = false
local logged = false

-- ============ Hook: Player.ResetEffects (Postfix) ============
-- ResetEffects 每帧都会把当前魔力恢复为合法状态，因此必须在其执行完之后
-- 再把 statMana 置为 statManaMax。
local function lock_mana(instance)
    if not ready or not instance then return end
    if not F_stat_mana or not F_stat_mana_max then return end

    if not logged then
        logged = true
        mod.info("魔力锁定已生效")
    end

    local max_mana = patch.get_field_value(F_stat_mana_max, instance, "int32")
    if max_mana == nil then return end
    patch.set_field_value(F_stat_mana, instance, max_mana, "int32")
end

-- ============ 小工具 ============
local function safe_hook(method, spec, label)
    if not method then
        mod.error("安装钩子失败(" .. label .. ")：方法句柄为空")
        return false
    end
    local ok, err = pcall(patch.install_hook, method, spec)
    if not ok then
        mod.error("安装钩子失败(" .. label .. ")：" .. tostring(err))
        return false
    end
    return true
end

-- ============ 生命周期 ============
function setup()
    local player_type = patch.get_type("Terraria", "Player")
    if not player_type then
        mod.error("找不到类型 Terraria.Player，模组未启用")
        return
    end

    F_stat_mana = patch.get_field(player_type, "statMana")
    F_stat_mana_max = patch.get_field(player_type, "statManaMax")
    if not F_stat_mana or not F_stat_mana_max then
        mod.error("获取 statMana / statManaMax 字段失败，模组未启用")
        return
    end

    -- 先按参数个数(0)取，失败再按方法名取
    local reset = patch.get_method(player_type, "ResetEffects", 0)
            or patch.get_method(player_type, "ResetEffects")
    if not safe_hook(reset, { postfix = lock_mana }, "Player.ResetEffects") then
        mod.error("无法 Hook Player.ResetEffects，魔力锁定未启用")
        return
    end

    ready = true
    mod.info("魔力锁定已启用")
end

todo_list = { "setup" }
