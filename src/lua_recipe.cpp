/*******************************************************************************
 * LuaLoader - lua_recipe
 * Copyright (C) 2026 lzup333
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *******************************************************************************/

#include "lua_recipe.hpp"
#include "logger.hpp"

#include <utility>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/type.h"

namespace lualoader::lua_recipe {

    namespace {

        // ---- 游戏符号（惰性解析） ----
        patch_handle_t g_item_class = nullptr;
        patch_handle_t g_recipe_class = nullptr;

        patch_handle_t f_item_stack = nullptr;        // Item.stack (int)
        patch_handle_t f_current_recipe = nullptr;    // Recipe.currentRecipe (static Recipe)
        patch_handle_t f_create_item = nullptr;       // Recipe.createItem (Item)
        patch_handle_t f_required_item = nullptr;     // Recipe.requiredItem (Item[])
        patch_handle_t f_required_tile = nullptr;     // Recipe.requiredTile (int)
        patch_handle_t f_num_recipes = nullptr;       // Recipe.numRecipes (static int)

        patch_handle_t m_item_set_defaults = nullptr; // Item.SetDefaults(int, ItemVariant)
        patch_handle_t m_item_net_defaults = nullptr; // Item.netDefaults(int)
        patch_handle_t m_add_recipe = nullptr;        // Recipe.AddRecipe() (private static)
        patch_handle_t m_create_quick = nullptr;      // Recipe.CreateRequiredItemQuickLookups()
        patch_handle_t m_update_materials = nullptr;  // Recipe.UpdateWhichItemsAreMaterials()
        patch_handle_t m_update_list = nullptr;       // Recipe.UpdateRecipeList()
        patch_handle_t m_main_update = nullptr;       // Main.Update(float)（只用于触发首次 flush）

        patch_hook_id_t g_update_hook = PATCH_HOOK_INVALID_ID;
        bool g_handles_ready = false;
        void (*g_queue_supplier)() = nullptr; // flush 前补队（JSON 物品等）

        struct QueuedRecipe {
            int result_id = 0;
            int result_stack = 1;
            std::vector<std::pair<int, int>> ingredients; // {item_id, stack}
            int tile = -1;
        };
        std::vector<QueuedRecipe> g_queue;

        // ---- 工具 ----
        patch_handle_t get_static_object(patch_handle_t field) {
            patch_handle_t v = nullptr;
            patchlib_field_get_value(field, nullptr, &v);
            return v;
        }

        int get_static_int(patch_handle_t field) {
            int v = 0;
            patchlib_field_get_value(field, nullptr, &v);
            return v;
        }

        // Item.SetDefaults(id[, variant])：改用 netDefaults(int)——
        // 桌面端 invoke 无法编组 SetDefaults 的 ItemVariant 结构体参数
        bool item_set_defaults(patch_handle_t item, int id) {
            if (!item) return false;
            if (patchlib_is_valid(m_item_net_defaults)) {
                int iid = id;
                void *args[1] = {&iid};
                return patchlib_method_invoke_args(m_item_net_defaults, item, nullptr, args);
            }
            if (patchlib_is_valid(m_item_set_defaults)) {
                void *variant = nullptr; // ItemVariant 可选参数传 null（注意传指针的指针）
                int iid = id;
                void *args[2] = {&iid, &variant};
                return patchlib_method_invoke_args(m_item_set_defaults, item, nullptr, args);
            }
            LOG_WARN("[recipe] Item.SetDefaults/netDefaults 不可用");
            return false;
        }

        // 把一个配方写入 currentRecipe 并 AddRecipe()
        bool add_one(const QueuedRecipe &r) {
            patch_handle_t cur = get_static_object(f_current_recipe);
            if (!cur) {
                LOG_WARN("[recipe] Recipe.currentRecipe 不可用");
                return false;
            }

            // 产物
            patch_handle_t create_item = nullptr;
            patchlib_field_get_value(f_create_item, cur, &create_item);
            if (!create_item) {
                LOG_WARN("[recipe] {} createItem 为空", r.result_id);
                return false;
            }
            if (!item_set_defaults(create_item, r.result_id)) {
                LOG_WARN("[recipe] {} 产物 SetDefaults/netDefaults 失败", r.result_id);
                return false;
            }
            int stack = r.result_stack > 0 ? r.result_stack : 1;
            patchlib_field_set_value(f_item_stack, create_item, &stack);

            // 材料
            patch_handle_t req_arr = nullptr;
            patchlib_field_get_value(f_required_item, cur, &req_arr);
            if (!req_arr) {
                LOG_WARN("[recipe] {} requiredItem 数组不可用", r.result_id);
                return false;
            }
            const int n = static_cast<int>(r.ingredients.size());
            for (int i = 0; i < n; i++) {
                patch_handle_t ing = nullptr;
                if (!patchlib_array_at(req_arr, i, &ing) || !ing) {
                    LOG_WARN("[recipe] requiredItem[%d] 为空（材料过多? 上限 Recipe.maxRequirements）", i);
                    return false;
                }
                if (!item_set_defaults(ing, r.ingredients[i].first)) return false;
                int istack = r.ingredients[i].second > 0 ? r.ingredients[i].second : 1;
                patchlib_field_set_value(f_item_stack, ing, &istack);
            }

            // 工作站（可选）
            if (r.tile >= 0) {
                int tile = r.tile;
                patchlib_field_set_value(f_required_tile, cur, &tile);
            }

            if (!patchlib_method_invoke_args(m_add_recipe, nullptr, nullptr, nullptr)) {
                LOG_WARN("[recipe] {} Recipe.AddRecipe 调用失败", r.result_id);
                return false;
            }
            return true;
        }

        // 刷新合成界面 / 材料标记
        void refresh_game_lists() {
            if (patchlib_is_valid(m_create_quick))
                patchlib_method_invoke_args(m_create_quick, nullptr, nullptr, nullptr);
            if (patchlib_is_valid(m_update_materials))
                patchlib_method_invoke_args(m_update_materials, nullptr, nullptr, nullptr);
            if (patchlib_is_valid(m_update_list))
                patchlib_method_invoke_args(m_update_list, nullptr, nullptr, nullptr);
        }

        bool ensure_handles() {
            if (g_handles_ready) return true;

            g_item_class = patchlib_type_get_type("Terraria", "Item");
            g_recipe_class = patchlib_type_get_type("Terraria", "Recipe");
            if (!g_item_class || !g_recipe_class) {
                LOG_WARN("[recipe] 找不到 Terraria.Item / Terraria.Recipe");
                return false;
            }

            f_item_stack = patchlib_type_get_field(g_item_class, "stack");
            m_item_net_defaults =
                patchlib_type_get_method_by_param_count(g_item_class, "netDefaults", 1);
            m_item_set_defaults =
                patchlib_type_get_method_by_param_count(g_item_class, "SetDefaults", 2);
            if (!patchlib_is_valid(m_item_set_defaults))
                m_item_set_defaults =
                    patchlib_type_get_method_by_param_count(g_item_class, "SetDefaults", 1);

            f_current_recipe = patchlib_type_get_field(g_recipe_class, "currentRecipe");
            f_create_item = patchlib_type_get_field(g_recipe_class, "createItem");
            f_required_item = patchlib_type_get_field(g_recipe_class, "requiredItem");
            f_required_tile = patchlib_type_get_field(g_recipe_class, "requiredTile");
            f_num_recipes = patchlib_type_get_field(g_recipe_class, "numRecipes");
            m_add_recipe = patchlib_type_get_method_by_param_count(g_recipe_class, "AddRecipe", 0);
            // 私有/内部方法：手机端若没有则跳过对应刷新步骤
            m_create_quick = patchlib_type_get_method_by_param_count(
                g_recipe_class, "CreateRequiredItemQuickLookups", 0);
            m_update_materials = patchlib_type_get_method_by_param_count(
                g_recipe_class, "UpdateWhichItemsAreMaterials", 0);
            m_update_list =
                patchlib_type_get_method_by_param_count(g_recipe_class, "UpdateRecipeList", 0);

            const bool ok = f_item_stack && f_current_recipe && f_create_item &&
                            f_required_item && f_required_tile && f_num_recipes &&
                            (patchlib_is_valid(m_item_net_defaults) ||
                             patchlib_is_valid(m_item_set_defaults)) &&
                            patchlib_is_valid(m_add_recipe);
            if (!ok) {
                LOG_WARN("[recipe] Recipe 符号解析不完整，配方注册不可用");
                return false;
            }
            g_handles_ready = true;
            return true;
        }

        // 游戏是否已经跑过 SetupRecipes（配方表已建立）
        bool recipes_ready() {
            if (!g_handles_ready && !ensure_handles()) return false;
            return get_static_int(f_num_recipes) > 0;
        }

        void flush_queue();
        void ensure_update_hook();

        void flush_queue() {
            if (!ensure_handles()) return;
            ensure_update_hook(); // 兜底：若安装时未就绪，此处重试
            // 防重入：queue_recipe -> flush_queue -> supplier -> queue_recipe 会无限递归
            static bool g_flushing = false;
            if (g_flushing) return;
            g_flushing = true;
            // runtime_id 可能在 Mod 加载后才分配，flush 前让各模块补交配方
            if (g_queue_supplier) g_queue_supplier();
            if (g_queue.empty()) {
                g_flushing = false;
                return;
            }

            const size_t total = g_queue.size();
            size_t ok_count = 0;
            for (const auto &r : g_queue) {
                if (add_one(r))
                    ++ok_count;
                else
                    LOG_WARN("[recipe] 注册配方失败 (result={})", r.result_id);
            }
            g_queue.clear();

            refresh_game_lists();
            g_flushing = false;
            LOG_INFO("[recipe] 已注册 {}/{} 条配方并刷新合成列表", ok_count, total);
        }

        // SetupRecipes 的 postfix 时机太早（内核数组扩容/ItemsByType 注册之前），
        // 因此改在第一帧 Main.Update 前统一 flush：此时游戏与内核初始化都已完成。
        bool g_update_flushed = false;
        bool main_update_prefix(patch_handle_t instance, void **args,
                                const patch_method_signature_t *sig_info, void *result) {
            (void)instance;
            (void)args;
            (void)sig_info;
            (void)result;
            if (!g_update_flushed) {
                g_update_flushed = true;
                flush_queue();
            }
            return false; // 必须返回 false：继续调用原函数（true = 跳过原函数！）
        }

        // 桌面端 HookManager 对“只有 prefix 没有 postfix”的钩子不会回调 prefix
        // （实测 Main.Update 纯 prefix 时回调从不触发），因此补一个空 postfix。
        void main_update_postfix(patch_handle_t instance, void **args, void *result,
                                 const patch_method_signature_t *sig_info) {
            (void)instance;
            (void)args;
            (void)result;
            (void)sig_info;
        }

        // 惰性安装 Main.Update 前缀钩子（Mod 加载极早时 il2cpp 类可能还没就绪，需要重试）
        void ensure_update_hook() {
            if (g_update_hook != PATCH_HOOK_INVALID_ID)
                return;
            if (!ensure_handles())
                return;
            patch_handle_t main_cls = patchlib_type_get_type("Terraria", "Main");
            if (patchlib_is_valid(main_cls)) {
                m_main_update =
                    patchlib_type_get_method_by_param_count(main_cls, "Update", 1);
                patchlib_free(main_cls);
            }
            if (!patchlib_is_valid(m_main_update)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    LOG_WARN("[recipe] 找不到 Terraria.Main:Update，配方将在 mod.recipe.add 时直接注册");
                }
                return;
            }
            g_update_hook =
                patchlib_install_prepost_hook(m_main_update, main_update_prefix,
                                              main_update_postfix);
            if (g_update_hook == PATCH_HOOK_INVALID_ID) {
                static bool warned2 = false;
                if (!warned2) {
                    warned2 = true;
                    LOG_WARN("[recipe] Main.Update 钩子安装失败，配方将在 mod.recipe.add 时直接注册");
                }
            } else {
                LOG_INFO("[recipe] Main.Update 钩子已安装（首帧统一注册配方）");
            }
        }

        // ---- Lua 绑定 ----
        lua_Integer opt_int(lua_State *L, int idx, const char *key, lua_Integer def) {
            lua_getfield(L, idx, key);
            const lua_Integer v = lua_isnil(L, -1) ? def : luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            return v;
        }

        // mod.recipe.add{ result=, stack=, ingredients={ {item=,stack=}, ... }, tile= }
        int l_recipe_add(lua_State *L) {
            luaL_checktype(L, 1, LUA_TTABLE);

            QueuedRecipe r;
            r.result_id = static_cast<int>(opt_int(L, 1, "result", 0));
            r.result_stack = static_cast<int>(opt_int(L, 1, "stack", 1));
            r.tile = static_cast<int>(opt_int(L, 1, "tile", -1));
            if (r.result_id <= 0)
                return luaL_error(L, "mod.recipe.add: result 必须是有效的物品ID");

            lua_getfield(L, 1, "ingredients");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                const int n = static_cast<int>(lua_rawlen(L, -1));
                for (int i = 1; i <= n; i++) {
                    lua_rawgeti(L, -1, i);
                    luaL_checktype(L, -1, LUA_TTABLE);
                    lua_getfield(L, -1, "item");
                    lua_getfield(L, -2, "stack");
                    const int item = static_cast<int>(luaL_checkinteger(L, -2));
                    const int istack = static_cast<int>(
                        lua_isnil(L, -1) ? 1 : luaL_checkinteger(L, -1));
                    if (item > 0)
                        r.ingredients.emplace_back(item, istack);
                    lua_pop(L, 3); // stack, item, ing-table
                }
            }
            lua_pop(L, 1);

            if (r.ingredients.empty())
                return luaL_error(L, "mod.recipe.add: 至少需要一种材料");

            g_queue.push_back(std::move(r));

            // 游戏已建好配方表（热重载/晚加载）则立即注册；否则等 SetupRecipes postfix
            if (recipes_ready())
                flush_queue();

            lua_pushboolean(L, 1);
            return 1;
        }

        // mod.recipe.refresh()
        int l_recipe_refresh(lua_State *L) {
            if (ensure_handles())
                refresh_game_lists();
            return 0;
        }

        // mod.recipe.pending() -> 待注册数量
        int l_recipe_pending(lua_State *L) {
            lua_pushinteger(L, static_cast<lua_Integer>(g_queue.size()));
            return 1;
        }

    } // namespace

    void register_api(lua_State *L, lua_mod_handle_t *handle) {
        (void)handle;

        // 全局只挂一次 Main.Update 前缀（第一帧统一 flush 配方队列）
        if (g_update_hook == PATCH_HOOK_INVALID_ID)
            ensure_update_hook();

        lua_createtable(L, 0, 3);
        lua_pushcfunction(L, l_recipe_add);
        lua_setfield(L, -2, "add");
        lua_pushcfunction(L, l_recipe_refresh);
        lua_setfield(L, -2, "refresh");
        lua_pushcfunction(L, l_recipe_pending);
        lua_setfield(L, -2, "pending");
    }

    bool queue_recipe(int result_id, int result_stack,
                      const std::vector<std::pair<int, int>> &ingredients, int tile) {
        if (result_id <= 0 || ingredients.empty()) return false;
        ensure_update_hook();
        QueuedRecipe r;
        r.result_id = result_id;
        r.result_stack = result_stack > 0 ? result_stack : 1;
        r.tile = tile;
        for (const auto &ing : ingredients)
            if (ing.first > 0) r.ingredients.push_back(ing);
        if (r.ingredients.empty()) return false;
        g_queue.push_back(std::move(r));
        if (recipes_ready())
            flush_queue();
        return true;
    }

    void set_queue_supplier(void (*fn)()) { g_queue_supplier = fn; }

} // namespace lualoader::lua_recipe
