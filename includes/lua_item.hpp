/*******************************************************************************
 * LuaLoader - lua_item (数据驱动自定义物品：lib/item/<name>.json + <name>.png)
 * Copyright (C) 2026 lzup333
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *******************************************************************************/

#pragma once

#include "lua_engine.hpp"

namespace lualoader::lua_item {
    /**
     * @brief 扫描 Mod 的 `lib/item/<name>.json`，为每个 JSON 注册一个自定义物品。
     *        贴图取同目录下 `<name>.png`（或 json 里的 "texture"）。
     * @param handle 当前 Mod 句柄（已填充 private_dir / mod_dir / mod_id）
     * @return 成功注册的物品数量
     */
    int load_items(lua_mod_handle_t *handle);
}
