#include "api_internal.hpp"
#include <cmath>

namespace lualoader::lua_api {

// ========================================================================
// mod.npc：Lua 侧注册自定义 NPC
//
//   mod.npc.register{
//     name     = "my_slime",
//     texture  = "slime.png",
//     frames   = 1,
//     defaults = { width=32, height=32, lifeMax=100, damage=10, ... },
//     language = { en = { name = "My Slime", bestiary = "..." }, zh = { ... } },
//     bestiary = { kind = "enemy", stars = 2, tags = {"Forest", "DayTime"}, ... },
//     ai       = function(npc) end,
//   } -> runtime_id | nil
//
//   mod.npc.spawn("my_slime", x, y)
// ========================================================================

struct LuaNpcLoot {
    int item_id = 0;
    std::string item_name;
    int min_stack = 1;
    int max_stack = 1;
    int chance_num = 1;
    int chance_den = 1;
};

struct LuaNpcBestiary {
    bool configured = false;
    bool exclude = false;
    terraria_npc_bestiary_kind_t kind = TERRARIA_NPC_BESTIARY_ENEMY;
    int stars = 1;
    bool quick_unlock = false;
    std::vector<std::string> tags;
    int banner_item_id = 0;
    std::string banner_item_name;
    int kills_to_banner = 50;
    bool include_loot = true;
};

struct LuaNpcEntry {
    terraria_npc_handle_t npc; // 必须是首成员
    lua_mod_handle_t *owner;
    lua_State *L;
    std::string name;
    std::vector<LuaItemFieldValue> defaults;
    int defaults_ref;
    int ai_ref;
    int on_death_ref;
    std::vector<LuaNpcLoot> loot;
    LuaNpcBestiary bestiary;
    std::map<int, std::pair<std::string, std::string>> lang; // culture -> {name, bestiary}
    patch_handle_t f_pos;
    patch_handle_t f_width;
    patch_handle_t f_height;
    std::string texture_path;
    std::vector<unsigned char> tex_pixels;
    int tex_w, tex_h;
    bool tex_tried;
    std::string boss_head_path;
    std::vector<unsigned char> boss_head_pixels;
    int boss_head_w, boss_head_h;
    bool boss_head_tried;
    patch_handle_t hit_sound;
    patch_handle_t death_sound;
    bool registered;
    int runtime_id;
};

static std::vector<LuaNpcEntry *> g_lua_npcs;
static std::map<int, std::string> g_lua_npc_lang;
static bool g_npc_lang_hooked = false;

/// 内核回调里的 handle 实际就是 LuaNpcEntry*。
static LuaNpcEntry *npc_from_handle(terraria_npc_handle_t *h) {
    return reinterpret_cast<LuaNpcEntry *>(h);
}

static bool has_npc_flavor_text(const LuaNpcEntry *e) {
    if (!e)
        return false;
    for (const auto &kv : e->lang) {
        if (!kv.second.second.empty())
            return true;
    }
    return false;
}

static void lua_npc_init_static(terraria_npc_handle_t *current) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e || e->npc.runtime_id < 0)
        return;

    std::vector<const char *> tag_ptrs;
    tag_ptrs.reserve(e->bestiary.tags.size());
    for (const auto &t : e->bestiary.tags)
        tag_ptrs.push_back(t.c_str());

    std::vector<terraria_npc_loot_entry_t> drop_entries;
    if (e->bestiary.include_loot && !e->loot.empty()) {
        drop_entries.reserve(e->loot.size());
        for (const auto &l : e->loot) {
            terraria_npc_loot_entry_t d{};
            d.item_id = l.item_id;
            d.item_name = l.item_name.empty() ? nullptr : l.item_name.c_str();
            d.chance_numerator = l.chance_num;
            d.chance_denominator = l.chance_den;
            d.min_stack = l.min_stack;
            d.max_stack = l.max_stack;
            drop_entries.push_back(d);
        }
    }

    terraria_npc_bestiary_t cfg{};
    cfg.exclude = e->bestiary.exclude;
    cfg.kind = e->bestiary.kind;
    cfg.stars = e->bestiary.stars;
    cfg.quick_unlock = e->bestiary.quick_unlock;
    cfg.has_flavor_text = has_npc_flavor_text(e);
    cfg.tags = tag_ptrs.empty() ? nullptr : tag_ptrs.data();
    cfg.tag_count = static_cast<int>(tag_ptrs.size());
    cfg.drops = drop_entries.empty() ? nullptr : drop_entries.data();
    cfg.drop_count = static_cast<int>(drop_entries.size());
    cfg.banner_item_id = e->bestiary.banner_item_id;
    cfg.banner_item_name =
        e->bestiary.banner_item_name.empty() ? nullptr : e->bestiary.banner_item_name.c_str();
    cfg.kills_to_banner = e->bestiary.kills_to_banner;

    terraria_npc_manager_set_bestiary(e->npc.runtime_id, &cfg);
}

/// NPC.SetDefaults 回调：写入默认字段，并调用可选的 Lua defaults(npc)。
static void lua_npc_set_defaults(terraria_npc_handle_t *current, patch_handle_t inst) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e || !inst)
        return;
    for (const auto &d : e->defaults)
        if (patchlib_is_valid(d.field))
            item_field_write(d.field, inst, d.type, &d.value);
    if (patchlib_is_valid(e->hit_sound) || patchlib_is_valid(e->death_sound)) {
        static patch_handle_t f_hit = PATCH_NULL;
        static patch_handle_t f_death = PATCH_NULL;
        static bool resolved = false;
        if (!resolved) {
            patch_handle_t cls = patchlib_type_get_type("Terraria", "NPC");
            if (patchlib_is_valid(cls)) {
                f_hit = patchlib_type_get_field(cls, "HitSound");
                f_death = patchlib_type_get_field(cls, "DeathSound");
                patchlib_free(cls);
            }
            resolved = true;
        }
        if (patchlib_is_valid(e->hit_sound) && patchlib_is_valid(f_hit))
            patchlib_field_set_value(f_hit, inst, &e->hit_sound);
        if (patchlib_is_valid(e->death_sound) && patchlib_is_valid(f_death))
            patchlib_field_set_value(f_death, inst, &e->death_sound);
    }
    if (e->defaults_ref != LUA_NOREF && call_ctx_alive(e->L, e->owner)) {
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->defaults_ref);
        push_handle(L, inst);
        pcall_log(L, 1, 0, "mod.npc.defaults");
    }
}

/// NPC.AI 回调：每帧调用 Lua 的 mod.npc.ai(npc)。
static void lua_npc_ai(terraria_npc_handle_t *current, patch_handle_t inst) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e || !inst || e->ai_ref == LUA_NOREF || !call_ctx_alive(e->L, e->owner))
        return;
    lua_State *L = e->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->ai_ref);
    push_handle(L, inst);
    pcall_log(L, 1, 0, "mod.npc.ai");
}

/// NPC 死亡回调：先调用 Lua 的 on_death，再按注册的 loot 在尸体中心掉落物品。
static void lua_npc_on_death(terraria_npc_handle_t *current, patch_handle_t inst) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e || !inst)
        return;

    if (e->on_death_ref != LUA_NOREF && call_ctx_alive(e->L, e->owner)) {
        lua_State *L = e->L;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->on_death_ref);
        push_handle(L, inst);
        pcall_log(L, 1, 0, "mod.npc.on_death");
    }

    if (e->loot.empty())
        return;
    float pos[2] = {0.0f, 0.0f};
    int w = 0, h = 0;
    patchlib_field_get_value(e->f_pos, inst, pos);
    patchlib_field_get_value(e->f_width, inst, &w);
    patchlib_field_get_value(e->f_height, inst, &h);
    const float cx = pos[0] + (float)w / 2.0f;
    const float cy = pos[1] + (float)h / 2.0f;
    LOG_DEBUG("[mod.npc] on_death: dropping {} loot entries at ({:.0f},{:.0f})", e->loot.size(), cx, cy);
    for (const auto &l : e->loot) {
        if (l.chance_num <= 0)
            continue;
        if (l.chance_num < l.chance_den && (std::rand() % l.chance_den) >= l.chance_num)
            continue;
        const int item_id = (l.item_id > 0) ? l.item_id : find_item_runtime_id(e->owner, l.item_name);
        if (item_id <= 0)
            continue;
        int stack = l.min_stack;
        if (l.max_stack > l.min_stack)
            stack = l.min_stack + (std::rand() % (l.max_stack - l.min_stack + 1));
        if (stack > 0)
            terraria_npc_manager_drop_item(item_id, cx, cy, stack);
    }
}

static patch_handle_t lua_npc_get_texture(terraria_npc_handle_t *current) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e)
        return PATCH_NULL;
    return create_texture_asset_from_png(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h,
                                         e->tex_tried, "[mod.npc]");
}

static patch_handle_t lua_npc_get_boss_head(terraria_npc_handle_t *current) {
    LuaNpcEntry *e = npc_from_handle(current);
    if (!e || e->boss_head_path.empty())
        return PATCH_NULL;
    return create_texture_asset_from_png(e->boss_head_path, e->boss_head_pixels,
                                         e->boss_head_w, e->boss_head_h,
                                         e->boss_head_tried, "[mod.npc] Boss 头像");
}

static void rebuild_npc_language_cache() {
    g_lua_npc_lang.clear();
    using EntryList = std::vector<std::pair<std::string, std::string>>;
    for (int c = 1; c <= 12; ++c) {
        std::map<std::string, EntryList> names, bestiaries;
        for (LuaNpcEntry *e : g_lua_npcs) {
            const auto it = e->lang.find(c);
            if (it == e->lang.end())
                continue;
            const std::string mod = e->owner ? e->owner->mod_id : std::string("?");
            if (!it->second.first.empty())
                names[mod].emplace_back(e->name, it->second.first);
            if (!it->second.second.empty())
                bestiaries[mod].emplace_back(e->name, it->second.second);
        }
        if (names.empty() && bestiaries.empty())
            continue;
        std::string json = "{";
        bool first = true;
        const auto emit = [&](const char *prefix, const std::map<std::string, EntryList> &m) {
            for (const auto &kv : m) {
                if (!first)
                    json += ",";
                first = false;
                json += "\"";
                json += prefix;
                json += "_";
                json += LUALOADER_PKG_ID;
                json += "_";
                json += json_escape_str(kv.first);
                json += "\":{";
                bool f2 = true;
                for (const auto &p : kv.second) {
                    if (!f2)
                        json += ",";
                    f2 = false;
                    json += "\"";
                    json += json_escape_str(p.first);
                    json += "\":\"";
                    json += json_escape_str(p.second);
                    json += "\"";
                }
                json += "}";
            }
        };
        emit("npc_name", names);
        emit("npc_bestiary", bestiaries);
        json += "}";
        g_lua_npc_lang[c] = std::move(json);
    }
}

static const char *lua_npc_language(const language_culture_t culture) {
    const auto it = g_lua_npc_lang.find(static_cast<int>(culture));
    return it == g_lua_npc_lang.end() ? "{}" : it->second.c_str();
}

static void ensure_npc_language_hook() {
    if (g_npc_lang_hooked)
        return;
    g_npc_lang_hooked = true;
    terraria_language_manager_register_callback(lua_npc_language);
}

void shutdown_npc_hooks() {
    if (!g_npc_lang_hooked)
        return;
    if (terraria_language_manager_unregister_callback)
        terraria_language_manager_unregister_callback(lua_npc_language);
    g_npc_lang_hooked = false;
}

static terraria_npc_bestiary_kind_t parse_bestiary_kind(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const int v = static_cast<int>(lua_tointeger(L, idx));
        if (v == TERRARIA_NPC_BESTIARY_CRITTER)
            return TERRARIA_NPC_BESTIARY_CRITTER;
        if (v == TERRARIA_NPC_BESTIARY_TOWN_NPC)
            return TERRARIA_NPC_BESTIARY_TOWN_NPC;
        return TERRARIA_NPC_BESTIARY_ENEMY;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        std::string s(lua_tostring(L, idx));
        for (char &ch : s)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (s == "critter" || s == "passive")
            return TERRARIA_NPC_BESTIARY_CRITTER;
        if (s == "town" || s == "town_npc" || s == "townnpc")
            return TERRARIA_NPC_BESTIARY_TOWN_NPC;
    }
    return TERRARIA_NPC_BESTIARY_ENEMY;
}

static void read_npc_item_ref(lua_State *L, int idx, int *out_id, std::string *out_name) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const int v = static_cast<int>(lua_tointeger(L, idx));
        if (v > 0 && out_id)
            *out_id = v;
    } else if (lua_type(L, idx) == LUA_TSTRING) {
        const char *s = lua_tostring(L, idx);
        if (s && *s && out_name)
            *out_name = s;
    }
}

/// mod.npc.register{ ... } -> runtime_id
/// 保存默认字段/回调/贴图/战利品，交给内核分配 NPC 运行时 ID。
static int l_npc_register(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_mod_handle_t *handle = get_handle(L);
    if (!handle)
        return luaL_error(L, "mod handle missing");

    lua_getfield(L, 1, "name");
    const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
    if (!name || !*name) {
        lua_pop(L, 1);
        return luaL_error(L, "mod.npc.register: 缺少 name");
    }
    const std::string npc_name = name;
    lua_pop(L, 1);

    auto *e = new LuaNpcEntry();
    std::memset(&e->npc, 0, sizeof(e->npc));
    e->owner = handle;
    e->L = L;
    e->name = npc_name;
    e->defaults_ref = LUA_NOREF;
    e->ai_ref = LUA_NOREF;
    e->on_death_ref = LUA_NOREF;
    e->f_pos = PATCH_NULL;
    e->f_width = PATCH_NULL;
    e->f_height = PATCH_NULL;
    e->tex_w = e->tex_h = 0;
    e->tex_tried = false;
    e->hit_sound = PATCH_NULL;
    e->death_sound = PATCH_NULL;
    e->boss_head_w = e->boss_head_h = 0;
    e->boss_head_tried = false;
    e->registered = false;
    e->runtime_id = -1;

    // vanilla_ai：true = 原版 AI 先执行，再调用 ai
    lua_getfield(L, 1, "vanilla_ai");
    const bool vanilla_ai = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_getfield(L, 1, "frames");
    e->npc.frames = static_cast<int>(luaL_optinteger(L, -1, 0));
    lua_pop(L, 1);
    if (e->npc.frames < 0)
        e->npc.frames = 0;

    // defaults：表格或函数
    lua_getfield(L, 1, "defaults");
    if (lua_isfunction(L, -1)) {
        e->defaults_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
        if (lua_istable(L, -1)) {
            const patch_handle_t npc_class = patchlib_type_get_type("Terraria", "NPC");
            if (patchlib_is_valid(npc_class)) {
                lua_pushnil(L);
                while (lua_next(L, -2)) {
                    if (lua_type(L, -2) == LUA_TSTRING) {
                        const char *fname = lua_tostring(L, -2);
                        const patch_handle_t f = patchlib_type_get_field(npc_class, fname);
                        if (patchlib_is_valid(f)) {
                            const patch_type_t t = patchlib_field_get_type(f);
                            uint64_t v = 0;
                            if (try_read_patch_value(L, -1, t, &v))
                                e->defaults.push_back({f, t, v, ref_default_value(L, -1, t)});
                            else
                                LOG_WARN("[mod.npc] {}: 字段 {} 值类型不匹配，已忽略", npc_name, fname);
                        } else {
                            patchlib_free(f);
                            LOG_WARN("[mod.npc] {}: 找不到 NPC 字段 {}", npc_name, fname);
                        }
                    }
                    lua_pop(L, 1);
                }
            }
            /* field 句柄已存进 defaults；桌面端不能释放其 class */
        }
        lua_pop(L, 1);
    }

    lua_getfield(L, 1, "ai");
    if (lua_isfunction(L, -1))
        e->ai_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 1, "on_death");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "on_kill");
    }
    if (lua_isfunction(L, -1))
        e->on_death_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 1, "loot");
    if (lua_istable(L, -1)) {
        const int n = (int)lua_rawlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_istable(L, -1)) {
                LuaNpcLoot l;
                lua_getfield(L, -1, "id");
                read_npc_item_ref(L, -1, &l.item_id, &l.item_name);
                lua_pop(L, 1);
                if (l.item_id <= 0 && l.item_name.empty()) {
                    lua_getfield(L, -1, "item");
                    read_npc_item_ref(L, -1, &l.item_id, &l.item_name);
                    lua_pop(L, 1);
                }
                if (l.item_id <= 0 && l.item_name.empty()) {
                    lua_getfield(L, -1, "name");
                    read_npc_item_ref(L, -1, &l.item_id, &l.item_name);
                    lua_pop(L, 1);
                }

                lua_getfield(L, -1, "count");
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "stack");
                }
                if (lua_isnumber(L, -1)) {
                    const int c = static_cast<int>(lua_tointeger(L, -1));
                    if (c > 0)
                        l.min_stack = l.max_stack = c;
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "min_stack");
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "min");
                }
                if (lua_isnumber(L, -1)) {
                    const int v = static_cast<int>(lua_tointeger(L, -1));
                    if (v > 0)
                        l.min_stack = v;
                }
                lua_pop(L, 1);

                lua_getfield(L, -1, "max_stack");
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "max");
                }
                if (lua_isnumber(L, -1)) {
                    const int v = static_cast<int>(lua_tointeger(L, -1));
                    if (v > 0)
                        l.max_stack = v;
                }
                lua_pop(L, 1);
                if (l.max_stack < l.min_stack)
                    l.max_stack = l.min_stack;

                lua_getfield(L, -1, "chance_numerator");
                lua_getfield(L, -2, "chance_denominator");
                if (lua_isnumber(L, -2) || lua_isnumber(L, -1)) {
                    l.chance_num = static_cast<int>(luaL_optinteger(L, -2, 1));
                    l.chance_den = static_cast<int>(luaL_optinteger(L, -1, 1));
                    if (l.chance_den <= 0)
                        l.chance_den = 1;
                    lua_pop(L, 2);
                } else {
                    lua_pop(L, 2);
                    lua_getfield(L, -1, "chance");
                    if (lua_isnumber(L, -1)) {
                        double p = lua_tonumber(L, -1);
                        if (p < 0.0) p = 0.0;
                        if (p > 1.0) p = 1.0;
                        l.chance_den = 10000;
                        l.chance_num = static_cast<int>(std::lround(p * 10000.0));
                    }
                    lua_pop(L, 1);
                }

                if (l.item_id > 0 || !l.item_name.empty())
                    e->loot.push_back(l);
                else
                    LOG_WARN("[mod.npc] {}: loot 需要物品 id 或物品名", npc_name);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "language");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const int culture = culture_from_lang_key(lua_tostring(L, -2));
                if (culture > 0) {
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        e->lang[culture].first = lua_tostring(L, -1);
                    } else if (lua_istable(L, -1)) {
                        lua_getfield(L, -1, "name");
                        if (lua_type(L, -1) == LUA_TSTRING)
                            e->lang[culture].first = lua_tostring(L, -1);
                        lua_pop(L, 1);

                        lua_getfield(L, -1, "bestiary");
                        if (lua_type(L, -1) != LUA_TSTRING) {
                            lua_pop(L, 1);
                            lua_getfield(L, -1, "flavor");
                        }
                        if (lua_type(L, -1) != LUA_TSTRING) {
                            lua_pop(L, 1);
                            lua_getfield(L, -1, "description");
                        }
                        if (lua_type(L, -1) == LUA_TSTRING)
                            e->lang[culture].second = lua_tostring(L, -1);
                        lua_pop(L, 1);
                    }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "bestiary");
    if (lua_isboolean(L, -1)) {
        e->bestiary.configured = true;
        e->bestiary.exclude = (lua_toboolean(L, -1) == 0);
    } else if (lua_istable(L, -1)) {
        e->bestiary.configured = true;
        lua_getfield(L, -1, "exclude");
        if (lua_isboolean(L, -1))
            e->bestiary.exclude = (lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "kind");
        e->bestiary.kind = parse_bestiary_kind(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "stars");
        if (lua_isnumber(L, -1))
            e->bestiary.stars = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "quick_unlock");
        if (lua_isboolean(L, -1))
            e->bestiary.quick_unlock = (lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "include_loot");
        if (lua_isboolean(L, -1))
            e->bestiary.include_loot = (lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        lua_getfield(L, -1, "kills_to_banner");
        if (lua_isnumber(L, -1))
            e->bestiary.kills_to_banner = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "banner_item");
        read_npc_item_ref(L, -1, &e->bestiary.banner_item_id, &e->bestiary.banner_item_name);
        lua_pop(L, 1);

        lua_getfield(L, -1, "tags");
        if (lua_istable(L, -1)) {
            const int tn = static_cast<int>(lua_rawlen(L, -1));
            for (int i = 1; i <= tn; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    const char *ts = lua_tostring(L, -1);
                    if (ts && *ts)
                        e->bestiary.tags.emplace_back(ts);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "texture");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *rel = lua_tostring(L, -1);
        e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
        if (e->texture_path.empty())
            LOG_WARN("[mod.npc] {}: 未找到贴图 {}", npc_name, rel);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "boss_head");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *rel = lua_tostring(L, -1);
        e->boss_head_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
        if (e->boss_head_path.empty())
            LOG_WARN("[mod.npc] {}: 未找到 Boss 头像贴图 {}", npc_name, rel);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "hit_sound");
    if (lua_type(L, -1) == LUA_TSTRING)
        e->hit_sound = resolve_vanilla_sound(lua_tostring(L, -1));
    lua_pop(L, 1);

    lua_getfield(L, 1, "death_sound");
    if (lua_type(L, -1) == LUA_TSTRING)
        e->death_sound = resolve_vanilla_sound(lua_tostring(L, -1));
    lua_pop(L, 1);

    e->npc.parent_modloader_id = LUALOADER_PKG_ID;
    e->npc.parent_id = handle->mod_id.c_str();
    e->npc.internal_name = e->name.c_str();
    e->npc.runtime_id = -1;
    e->npc.ai_after_vanilla = vanilla_ai;
    e->npc.npc_ops.early_init = nullptr;
    e->npc.npc_ops.init_static = lua_npc_init_static;
    e->npc.npc_ops.set_defaults = lua_npc_set_defaults;
    e->npc.npc_ops.ai = (e->ai_ref != LUA_NOREF) ? lua_npc_ai : nullptr;
    e->npc.npc_ops.get_texture = lua_npc_get_texture;
    e->npc.npc_ops.get_boss_head = lua_npc_get_boss_head;
    e->npc.npc_ops.on_death = (e->on_death_ref != LUA_NOREF) ? lua_npc_on_death : nullptr;
    if (!e->loot.empty())
        e->npc.npc_ops.on_death = lua_npc_on_death;

    static patch_handle_t npc_cls = PATCH_NULL;
    if (!patchlib_is_valid(npc_cls))
        npc_cls = patchlib_type_get_type("Terraria", "NPC");
    e->f_pos = patchlib_type_get_field(npc_cls, "position");
    e->f_width = patchlib_type_get_field(npc_cls, "width");
    e->f_height = patchlib_type_get_field(npc_cls, "height");
    /* class 常驻：桌面端 field 句柄依附它 */

    if (!terraria_npc_manager_register_npc(&e->npc)) {
        LOG_ERROR("[mod.npc] {}: 注册失败", npc_name);
        if (e->defaults_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, e->defaults_ref);
        if (e->ai_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, e->ai_ref);
        if (e->on_death_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, e->on_death_ref);
        unref_default_values(L, e->defaults);
        patchlib_free(e->f_pos);
        patchlib_free(e->f_width);
        patchlib_free(e->f_height);
        delete e;
        lua_pushnil(L);
        return 1;
    }

    e->registered = true;
    e->runtime_id = e->npc.runtime_id;
    g_lua_npcs.push_back(e);
    if (!e->lang.empty()) {
        ensure_npc_language_hook();
        rebuild_npc_language_cache();
    }
    LOG_INFO("[mod.npc] 已注册 {} (runtime_id={})", npc_name, e->runtime_id);
    lua_pushinteger(L, e->runtime_id);
    return 1;
}

/// mod.npc.runtime_id(name) -> int | nil
static int l_npc_runtime_id(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    lua_mod_handle_t *handle = get_handle(L);
    for (LuaNpcEntry *e : g_lua_npcs) {
        if (e->owner == handle && e->name == name) {
            if (e->npc.runtime_id >= 0)
                lua_pushinteger(L, e->npc.runtime_id);
            else
                lua_pushnil(L);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/// mod.npc.spawn(name, x, y) -> index | nil
/// 在指定世界坐标生成该 Mod 注册的 NPC。
static int l_npc_spawn(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    const float x = static_cast<float>(luaL_checknumber(L, 2));
    const float y = static_cast<float>(luaL_checknumber(L, 3));
    lua_mod_handle_t *handle = get_handle(L);
    for (LuaNpcEntry *e : g_lua_npcs) {
        if (e->owner == handle && e->name == name) {
            if (e->npc.runtime_id < 0)
                break;
            const int index = terraria_npc_manager_spawn(e->npc.runtime_id, x, y);
            if (index >= 0)
                lua_pushinteger(L, index);
            else
                lua_pushnil(L);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/// mod.npc.drop_item(id, x, y[, stack]) -> 在指定位置生成掉落物
static int l_npc_drop_item(lua_State *L) {
    const int id = (int)luaL_checkinteger(L, 1);
    const float x = (float)luaL_checknumber(L, 2);
    const float y = (float)luaL_checknumber(L, 3);
    const int count = (int)luaL_optinteger(L, 4, 1);
    const int index = terraria_npc_manager_drop_item(id, x, y, count);
    if (index >= 0)
        lua_pushinteger(L, index);
    else
        lua_pushnil(L);
    return 1;
}

static const luaL_Reg mod_npc_functions[] = {
    {"register", l_npc_register}, {"runtime_id", l_npc_runtime_id},
    {"spawn", l_npc_spawn},       {"drop_item", l_npc_drop_item},
    {nullptr, nullptr},
};

/// 注销某个 Mod 的所有 NPC：内核注销 → 释放 Lua 引用/托管句柄 → 删条目。
void unregister_npcs(lua_mod_handle_t *handle) {
    if (!handle)
        return;
    bool lang_changed = false;
    for (size_t i = g_lua_npcs.size(); i-- > 0;) {
        LuaNpcEntry *e = g_lua_npcs[i];
        if (e->owner != handle)
            continue;
        if (!e->lang.empty())
            lang_changed = true;
        if (e->registered)
            terraria_npc_manager_unregister_npc(&e->npc);
        if (patchlib_is_valid(e->hit_sound)) patchlib_free(e->hit_sound);
        if (patchlib_is_valid(e->death_sound)) patchlib_free(e->death_sound);
        if (handle->L) {
            if (e->defaults_ref != LUA_NOREF)
                luaL_unref(handle->L, LUA_REGISTRYINDEX, e->defaults_ref);
            if (e->ai_ref != LUA_NOREF)
                luaL_unref(handle->L, LUA_REGISTRYINDEX, e->ai_ref);
            if (e->on_death_ref != LUA_NOREF)
                luaL_unref(handle->L, LUA_REGISTRYINDEX, e->on_death_ref);
            unref_default_values(handle->L, e->defaults);
        }
        patchlib_free(e->f_pos);
        patchlib_free(e->f_width);
        patchlib_free(e->f_height);
        e->f_pos = e->f_width = e->f_height = PATCH_NULL;
        delete e;
        g_lua_npcs.erase(g_lua_npcs.begin() + static_cast<long>(i));
    }
    if (lang_changed)
        rebuild_npc_language_cache();
}

/// 把 mod.npc 相关函数注册到当前 table。
void add_npc(lua_State *L) {
    luaL_setfuncs(L, mod_npc_functions, 0);
}

} // namespace lualoader::lua_api
