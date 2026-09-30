/*******************************************************************************
 * LuaLoader - lua_recipe（基于游戏原生 Recipe 流程的配方注册）
 * Copyright (C) 2026 lzup333
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 不使用内核的 terraria_recipe_* API，而是直接驱动游戏自身的合成系统：
 *   Recipe.currentRecipe -> 填 createItem / requiredItem[i] / requiredTile
 *   -> Recipe.AddRecipe() -> CreateRequiredItemQuickLookups()
 *   -> UpdateWhichItemsAreMaterials() -> UpdateRecipeList()
 *
 * Lua API（挂在每个 Mod 的 mod 表上）：
 *   mod.recipe.add{ result = <物品ID>, stack = 1,
 *                   ingredients = { { item = <ID>, stack = 1 }, ... },
 *                   tile = <工作台tileID, 可选> }
 *   mod.recipe.refresh()  -- 手动刷新合成列表（一般不需要，注册后自动刷新）
 *
 * 物品 ID 可以用 mod.item.runtime_id(name) 取自定义物品的运行时 ID。
 * 时机：Mod 加载期调用 add() 会先入队；游戏 Recipe.SetupRecipes() 完成后自动
 * 统一注册（若调用 add() 时游戏已构建过配方表，则立即注册）。
 *******************************************************************************/

#pragma once

#include <utility>
#include <vector>

struct lua_State;

namespace lualoader {
    struct lua_mod_handle_t;
}

namespace lualoader::lua_recipe {

    // 在栈顶留下一个包含 add/refresh 的 table（由 lua_api.cpp 挂到 mod.recipe）
    void register_api(lua_State *L, lua_mod_handle_t *handle);

    // C++ 侧入队一条配方（供 JSON 物品加载器使用）；游戏已就绪时会立即注册
    bool queue_recipe(int result_id, int result_stack,
                      const std::vector<std::pair<int, int>> &ingredients, int tile);

    // 注册“补队回调”：flush 前调用（物品 runtime_id 分配后补交配方）
    void set_queue_supplier(void (*fn)());

} // namespace lualoader::lua_recipe
