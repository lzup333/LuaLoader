#include "api_internal.hpp"

namespace lualoader::lua_api {

// ---------------- mod.recipe：任意配方（含原版物品） ----------------
//
// 内核在 Recipe.SetupRecipes 时回调所有注册者；这里把 Lua 侧登记的
// 配方统一写入。也就是说：mod.recipe.add 可以在任意时刻调用，但真正
// 生效是在下一次游戏重建配方表（进世界/换语言）时。

/// 一条 Lua 侧登记的配方。真正写入内核是在游戏重建配方表时。
struct LuaRecipeEntry {
    lua_mod_handle_t *owner;                    // 所属 Mod，卸载时据此清理
    int result_id;                              // 产物物品 ID
    int result_stack;                           // 产物数量
    int station;                                // 合成站物品 ID；-1 = 无
    std::vector<std::pair<int, int>> materials; // (材料 ID, 数量)
};

static std::vector<LuaRecipeEntry> g_lua_recipes;
static bool g_recipe_hooked = false;

/// 内核回调：把当前所有 Lua 配方写入内核。
/// 空结果/无材料的条目直接跳过（add 时已校验，这里再防御一次）。
static void lua_recipe_apply() {
    for (const auto &r : g_lua_recipes) {
        if (r.result_id <= 0 || r.materials.empty())
            continue;

        // 内核接口要求扁平的 [id, count, id, count, ...] 数组。
        std::vector<int> mats;
        mats.reserve(r.materials.size() * 2);
        for (const auto &m : r.materials) {
            mats.push_back(m.first);
            mats.push_back(m.second);
        }

        terraria_recipe_set_result(r.result_id, r.result_stack);
        terraria_recipe_set_material(mats.data(), static_cast<int>(mats.size() / 2));
        terraria_recipe_set_station(r.station);
        terraria_recipe_add();
    }
}

/// 懒注册内核回调；全局只注册一次。
static void ensure_recipe_hooks() {
    if (g_recipe_hooked)
        return;
    g_recipe_hooked = true;
    terraria_recipe_manager_register_callback(lua_recipe_apply);
}

/// 注销内核回调（加载器关闭时调用）。
void shutdown_recipe_hooks() {
    if (!g_recipe_hooked)
        return;
    if (terraria_recipe_manager_unregister_callback)
        terraria_recipe_manager_unregister_callback(lua_recipe_apply);
    g_recipe_hooked = false;
}

/// mod.recipe.add{ result = id|{id,count}, materials = {{id,count},...}, station = id }
static int l_recipe_add(lua_State *L) {
    lua_mod_handle_t *h = get_handle(L);
    if (!h)
        return luaL_error(L, "mod handle missing");
    luaL_checktype(L, 1, LUA_TTABLE);

    LuaRecipeEntry e{};
    e.owner = h;
    e.result_id = 0;
    e.result_stack = 1;
    e.station = -1;

    lua_getfield(L, 1, "result");
    if (lua_isnumber(L, -1)) {
        e.result_id = static_cast<int>(lua_tointeger(L, -1));
    } else if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "id");
        e.result_id = static_cast<int>(luaL_optinteger(L, -1, 0));
        lua_pop(L, 1);
        lua_getfield(L, -1, "count");
        e.result_stack = static_cast<int>(luaL_optinteger(L, -1, 1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (e.result_id <= 0)
        return luaL_error(L, "mod.recipe.add: result 无效");
    if (e.result_stack <= 0)
        e.result_stack = 1;

    lua_getfield(L, 1, "station");
    e.station = static_cast<int>(luaL_optinteger(L, -1, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "materials");
    if (lua_istable(L, -1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, static_cast<int>(i));
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "id");
                const int id = static_cast<int>(luaL_optinteger(L, -1, 0));
                lua_pop(L, 1);

                lua_getfield(L, -1, "count");
                int count = static_cast<int>(luaL_optinteger(L, -1, 1));
                lua_pop(L, 1);

                if (id > 0) {
                    if (count <= 0)
                        count = 1;
                    e.materials.emplace_back(id, count);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    if (e.materials.empty())
        return luaL_error(L, "mod.recipe.add: materials 不能为空");

    ensure_recipe_hooks();
    g_lua_recipes.push_back(std::move(e));
    return 0;
}

static const luaL_Reg mod_recipe_functions[] = {
    {"add", l_recipe_add},
    {nullptr, nullptr},
};

void add_recipe(lua_State *L) {
    luaL_setfuncs(L, mod_recipe_functions, 0);
}

/// 移除某个 Mod 登记的所有配方（倒序遍历，erase 不会打乱未处理的下标）。
void unregister_recipes(lua_mod_handle_t *handle) {
    if (!handle)
        return;
    for (size_t i = g_lua_recipes.size(); i-- > 0;) {
        if (g_lua_recipes[i].owner == handle)
            g_lua_recipes.erase(g_lua_recipes.begin() + static_cast<long>(i));
    }
}

} // namespace lualoader::lua_api
