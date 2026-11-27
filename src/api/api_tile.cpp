#include "api_internal.hpp"

#include <algorithm>

namespace lualoader::lua_api {

// ========================================================================
// Shared Localization & PNG Texture Helpers (used by NPC / Tile APIs)
// ========================================================================

int culture_from_lang_key(const char *key) {
    if (!key)
        return 0;
    std::string s(key);
    for (char &ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s == "en" || s == "english")
        return LANGUAGE_ENGLISH;
    if (s == "de" || s == "german")
        return LANGUAGE_GERMAN;
    if (s == "it" || s == "italian")
        return LANGUAGE_ITALIAN;
    if (s == "fr" || s == "french")
        return LANGUAGE_FRENCH;
    if (s == "es" || s == "spanish")
        return LANGUAGE_SPANISH;
    if (s == "ru" || s == "russian")
        return LANGUAGE_RUSSIAN;
    if (s == "zh" || s == "zh-cn" || s == "zh-hans" || s == "chinese" || s == "chinesesimplified")
        return LANGUAGE_CHINESE_SIMPLIFIED;
    if (s == "pt" || s == "portuguese")
        return LANGUAGE_PORTUGUESE;
    if (s == "pl" || s == "polish")
        return LANGUAGE_POLISH;
    if (s == "ja" || s == "japanese")
        return LANGUAGE_JAPANESE;
    if (s == "ko" || s == "korean")
        return LANGUAGE_KOREAN;
    if (s == "zh-tw" || s == "zh-hant" || s == "chinesetraditional")
        return LANGUAGE_CHINESE_TRADITIONAL;
    return 0;
}

std::string json_escape_str(const std::string &in) {
    std::string o;
    o.reserve(in.size() + 8);
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += static_cast<char>(c);
            }
        }
    }
    return o;
}

patch_handle_t create_texture_asset_from_png(const std::string &path, std::vector<unsigned char> &pixels,
                                             int &w, int &h, bool &tried, const char *log_prefix) {
    if (path.empty())
        return PATCH_NULL;
    if (!tried) {
        tried = true;
        if (!image::decode_png_file(path, pixels, w, h))
            LOG_WARN("{} 贴图解码失败: {}", log_prefix ? log_prefix : "[lualoader]", path);
    }
    if (pixels.empty() || w <= 0 || h <= 0)
        return PATCH_NULL;

    const size_t uw = static_cast<size_t>(w);
    const size_t uh = static_cast<size_t>(h);
    const size_t sz = uw * uh * 4;
    auto *buf = static_cast<unsigned char *>(malloc(sz));
    if (!buf)
        return PATCH_NULL;
#if defined(__ANDROID__)
    for (size_t y = 0; y < uh; ++y)
        std::memcpy(buf + y * uw * 4, pixels.data() + (uh - 1 - y) * uw * 4, uw * 4);
#else
    std::memcpy(buf, pixels.data(), sz);
#endif
    premultiply_alpha(buf, sz);
    const patch_handle_t tex = terraria_texture2d_create(w, h, TEXTURE_FORMAT_RGBA32, buf, sz);
    free(buf);
    if (!patchlib_is_valid(tex))
        return PATCH_NULL;

    const patch_handle_t tex_type = terraria_texture2d_get_class();
    const patch_handle_t asset = terraria_asset_create(tex_type, tex);
    if (patchlib_is_valid(tex_type))
        patchlib_free(tex_type);
    if (patchlib_is_valid(tex))
        patchlib_free(tex);
    return asset;
}

// ========================================================================
// mod.tile：Lua 侧注册自定义图格
// ========================================================================

struct LuaTileEntry {
    terraria_tile_handle_t tile; // 必须是首成员
    lua_mod_handle_t *owner = nullptr;
    lua_State *L = nullptr;
    std::string name;
    std::string texture_path;
    std::vector<unsigned char> tex_pixels;
    int tex_w = 0;
    int tex_h = 0;
    bool tex_tried = false;
    std::string drop_item_name;
    std::vector<int> merge_ids;
    std::vector<std::string> merge_names;
    std::vector<const char *> merge_name_ptrs;
    std::vector<int> adj_ids;
    std::vector<std::string> adj_names;
    int on_drop_ref = LUA_NOREF;
    std::map<int, std::string> lang; // culture -> map name
    bool registered = false;
    int runtime_id = -1;
};

static std::vector<LuaTileEntry *> g_lua_tiles;
static std::map<int, std::string> g_lua_tile_lang;
static bool g_tile_lang_hooked = false;

static LuaTileEntry *tile_from_handle(terraria_tile_handle_t *h) {
    return reinterpret_cast<LuaTileEntry *>(h);
}

static terraria_tile_object_style_t parse_object_style(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const int v = static_cast<int>(lua_tointeger(L, idx));
        if (v >= TERRARIA_TILE_OBJECT_NONE && v <= TERRARIA_TILE_OBJECT_STYLE_3X3_WALL)
            return static_cast<terraria_tile_object_style_t>(v);
        return TERRARIA_TILE_OBJECT_NONE;
    }
    if (lua_type(L, idx) != LUA_TSTRING)
        return TERRARIA_TILE_OBJECT_NONE;

    std::string s(lua_tostring(L, idx));
    for (char &ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (s == "torch")
        return TERRARIA_TILE_OBJECT_TORCH;
    if (s == "platform")
        return TERRARIA_TILE_OBJECT_PLATFORM;
    if (s == "chair")
        return TERRARIA_TILE_OBJECT_CHAIR;
    if (s == "workbench" || s == "work_bench")
        return TERRARIA_TILE_OBJECT_WORKBENCH;
    if (s == "table")
        return TERRARIA_TILE_OBJECT_TABLE;
    if (s == "sapling")
        return TERRARIA_TILE_OBJECT_SAPLING;
    if (s == "banner")
        return TERRARIA_TILE_OBJECT_BANNER;
    if (s == "closed_door" || s == "door")
        return TERRARIA_TILE_OBJECT_CLOSED_DOOR;
    if (s == "1x1" || s == "style_1x1")
        return TERRARIA_TILE_OBJECT_STYLE_1X1;
    if (s == "1x2" || s == "style_1x2")
        return TERRARIA_TILE_OBJECT_STYLE_1X2;
    if (s == "2x1" || s == "style_2x1")
        return TERRARIA_TILE_OBJECT_STYLE_2X1;
    if (s == "2x2" || s == "style_2x2")
        return TERRARIA_TILE_OBJECT_STYLE_2X2;
    if (s == "3x2" || s == "style_3x2")
        return TERRARIA_TILE_OBJECT_STYLE_3X2;
    if (s == "3x3" || s == "style_3x3")
        return TERRARIA_TILE_OBJECT_STYLE_3X3;
    if (s == "3x3_wall" || s == "painting_3x3" || s == "style_3x3_wall")
        return TERRARIA_TILE_OBJECT_STYLE_3X3_WALL;
    return TERRARIA_TILE_OBJECT_NONE;
}

static bool read_table_bool_opt(lua_State *L, int tbl_idx, const char *key, bool fallback) {
    lua_getfield(L, tbl_idx, key);
    bool out = fallback;
    if (lua_isboolean(L, -1))
        out = (lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    return out;
}

static int read_table_int_opt(lua_State *L, int tbl_idx, const char *key, int fallback) {
    lua_getfield(L, tbl_idx, key);
    int out = fallback;
    if (lua_isnumber(L, -1))
        out = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    return out;
}

static double read_table_num_opt(lua_State *L, int tbl_idx, const char *key, double fallback) {
    lua_getfield(L, tbl_idx, key);
    double out = fallback;
    if (lua_isnumber(L, -1))
        out = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return out;
}

static void read_rgb_triple(lua_State *L, int tbl_idx, const char *field_name, bool *out_has,
                            double *out_r, double *out_g, double *out_b) {
    lua_getfield(L, tbl_idx, field_name);
    if (lua_istable(L, -1)) {
        *out_has = true;
        lua_getfield(L, -1, "r");
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 1);
        }
        *out_r = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "g");
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
        }
        *out_g = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "b");
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 3);
        }
        *out_b = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int find_tile_runtime_id_for_owner(const lua_mod_handle_t *owner, const std::string &name) {
    if (!owner || name.empty())
        return -1;
    for (LuaTileEntry *e : g_lua_tiles) {
        if (e && e->owner == owner && e->name == name)
            return e->tile.runtime_id;
    }
    return -1;
}

static void lua_tile_init_static(terraria_tile_handle_t *current) {
    LuaTileEntry *e = tile_from_handle(current);
    if (!e)
        return;
    e->runtime_id = e->tile.runtime_id;
    if (!e->adj_names.empty()) {
        for (const auto &tname : e->adj_names) {
            const int tid = find_tile_runtime_id_for_owner(e->owner, tname);
            if (tid >= 0)
                e->adj_ids.push_back(tid);
        }
        e->adj_names.clear();
        e->tile.adj_tile_ids = e->adj_ids.empty() ? nullptr : e->adj_ids.data();
        e->tile.adj_tile_count = static_cast<int>(e->adj_ids.size());
    }
}

static patch_handle_t lua_tile_get_texture(terraria_tile_handle_t *current) {
    LuaTileEntry *e = tile_from_handle(current);
    if (!e)
        return PATCH_NULL;
    return create_texture_asset_from_png(e->texture_path, e->tex_pixels, e->tex_w, e->tex_h,
                                         e->tex_tried, "[mod.tile]");
}

static bool lua_tile_get_drop(terraria_tile_handle_t *current, int x, int y,
                              int *out_item_id, int *out_stack) {
    LuaTileEntry *e = tile_from_handle(current);
    if (!e || e->on_drop_ref == LUA_NOREF || !call_ctx_alive(e->L, e->owner))
        return false;

    lua_State *L = e->L;
    const int base_top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->on_drop_ref);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    if (lua_pcall(L, 2, 2, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        LOG_ERROR("[mod.tile.on_drop] {}", err ? err : "<non-string error>");
        lua_settop(L, base_top);
        return false;
    }

    bool handled = false;
    const int r1_idx = base_top + 1;
    const int r2_idx = base_top + 2;
    if (lua_isboolean(L, r1_idx) && !lua_toboolean(L, r1_idx)) {
        if (out_item_id) *out_item_id = -1;
        if (out_stack) *out_stack = 0;
        handled = true;
    } else if (lua_type(L, r1_idx) == LUA_TNUMBER) {
        const int id = static_cast<int>(lua_tointeger(L, r1_idx));
        const int stack = lua_isnumber(L, r2_idx) ? static_cast<int>(lua_tointeger(L, r2_idx)) : 1;
        if (out_item_id) *out_item_id = (id > 0) ? id : -1;
        if (out_stack) *out_stack = (stack > 0) ? stack : 1;
        handled = true;
    } else if (lua_type(L, r1_idx) == LUA_TSTRING) {
        const char *iname = lua_tostring(L, r1_idx);
        const int id = iname ? find_item_runtime_id(e->owner, iname) : -1;
        if (id > 0) {
            const int stack = lua_isnumber(L, r2_idx) ? static_cast<int>(lua_tointeger(L, r2_idx)) : 1;
            if (out_item_id) *out_item_id = id;
            if (out_stack) *out_stack = (stack > 0) ? stack : 1;
            handled = true;
        }
    }
    lua_settop(L, base_top);
    return handled;
}

static void rebuild_tile_language_cache() {
    g_lua_tile_lang.clear();
    using EntryList = std::vector<std::pair<std::string, std::string>>;
    for (int c = 1; c <= 12; ++c) {
        std::map<std::string, EntryList> names;
        for (LuaTileEntry *e : g_lua_tiles) {
            const auto it = e->lang.find(c);
            if (it == e->lang.end() || it->second.empty())
                continue;
            const std::string mod = e->owner ? e->owner->mod_id : std::string("?");
            names[mod].emplace_back(e->name, it->second);
        }
        if (names.empty())
            continue;
        std::string json = "{";
        bool first = true;
        for (const auto &kv : names) {
            if (!first)
                json += ",";
            first = false;
            json += "\"tile_name_";
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
        json += "}";
        g_lua_tile_lang[c] = std::move(json);
    }
}

static const char *lua_tile_language(const language_culture_t culture) {
    const auto it = g_lua_tile_lang.find(static_cast<int>(culture));
    return it == g_lua_tile_lang.end() ? "{}" : it->second.c_str();
}

static void ensure_tile_language_hook() {
    if (g_tile_lang_hooked)
        return;
    g_tile_lang_hooked = true;
    terraria_language_manager_register_callback(lua_tile_language);
}

void shutdown_tile_hooks() {
    if (!g_tile_lang_hooked)
        return;
    if (terraria_language_manager_unregister_callback)
        terraria_language_manager_unregister_callback(lua_tile_language);
    g_tile_lang_hooked = false;
}

static int l_tile_register(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_mod_handle_t *handle = get_handle(L);
    if (!handle)
        return luaL_error(L, "mod handle missing");

    lua_getfield(L, 1, "name");
    const char *name = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
    if (!name || !*name) {
        lua_pop(L, 1);
        return luaL_error(L, "mod.tile.register: 缺少 name");
    }
    const std::string tile_name = name;
    lua_pop(L, 1);

    auto *e = new LuaTileEntry();
    std::memset(&e->tile, 0, sizeof(e->tile));
    e->owner = handle;
    e->L = L;
    e->name = tile_name;

    lua_getfield(L, 1, "object_style");
    const terraria_tile_object_style_t style = parse_object_style(L, -1);
    lua_pop(L, 1);
    e->tile.object_style = style;

    const bool is_none = (style == TERRARIA_TILE_OBJECT_NONE);
    const bool def_solid =
        is_none || style == TERRARIA_TILE_OBJECT_PLATFORM || style == TERRARIA_TILE_OBJECT_CLOSED_DOOR;
    const bool def_solid_top = (style == TERRARIA_TILE_OBJECT_PLATFORM);

    e->tile.solid = read_table_bool_opt(L, 1, "solid", def_solid);
    e->tile.solid_top = read_table_bool_opt(L, 1, "solid_top", def_solid_top);
    e->tile.merge_dirt = read_table_bool_opt(L, 1, "merge_dirt", false);
    const bool def_block_light =
        is_none ? e->tile.solid : (style == TERRARIA_TILE_OBJECT_CLOSED_DOOR);
    e->tile.block_light = read_table_bool_opt(L, 1, "block_light", def_block_light);
    e->tile.frame_important = read_table_bool_opt(L, 1, "frame_important", !is_none);

    e->tile.min_pick = read_table_int_opt(L, 1, "min_pick", 0);
    e->tile.mine_resist = static_cast<float>(read_table_num_opt(L, 1, "mine_resist", 1.0));
    e->tile.cut = read_table_bool_opt(L, 1, "cut", false);
    e->tile.no_fail = read_table_bool_opt(L, 1, "no_fail", false);
    e->tile.no_attach = read_table_bool_opt(L, 1, "no_attach", false);
    e->tile.table = read_table_bool_opt(L, 1, "table", false);
    e->tile.lava_death = read_table_bool_opt(L, 1, "lava_death", false);
    e->tile.water_death = read_table_bool_opt(L, 1, "water_death", false);
    e->tile.shine = read_table_int_opt(L, 1, "shine", 0);
    e->tile.spelunker = read_table_bool_opt(L, 1, "spelunker", false);
    e->tile.bouncy = read_table_bool_opt(L, 1, "bouncy", false);
    e->tile.blend_all = read_table_bool_opt(L, 1, "blend_all", false);
    e->tile.lava_immune_object = read_table_bool_opt(L, 1, "lava_immune_object", false);

    lua_getfield(L, 1, "dust_type");
    if (lua_isnumber(L, -1)) {
        e->tile.has_dust_type = true;
        e->tile.dust_type = static_cast<int>(lua_tointeger(L, -1));
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "hit_sound");
    if (lua_isnumber(L, -1)) {
        e->tile.has_hit_sound = true;
        e->tile.hit_sound_type = static_cast<int>(lua_tointeger(L, -1));
        e->tile.hit_sound_style = 1;
    } else if (lua_istable(L, -1)) {
        e->tile.has_hit_sound = true;
        e->tile.hit_sound_type = read_table_int_opt(L, -1, "type", 0);
        e->tile.hit_sound_style = read_table_int_opt(L, -1, "style", 1);
    }
    lua_pop(L, 1);

    e->tile.drop_stack = 1;
    lua_getfield(L, 1, "drop");
    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
        e->tile.drop_item_id = -1;
        e->tile.drop_stack = 0;
    } else if (lua_type(L, -1) == LUA_TNUMBER) {
        const int id = static_cast<int>(lua_tointeger(L, -1));
        e->tile.drop_item_id = (id > 0) ? id : -1;
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        const char *s = lua_tostring(L, -1);
        if (s && *s) {
            e->drop_item_name = s;
            e->tile.drop_item_name = e->drop_item_name.c_str();
        }
    } else if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "id");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, -1, "item");
        }
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, -1, "name");
        }
        if (lua_type(L, -1) == LUA_TNUMBER) {
            const int id = static_cast<int>(lua_tointeger(L, -1));
            e->tile.drop_item_id = (id > 0) ? id : -1;
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            const char *s = lua_tostring(L, -1);
            if (s && *s) {
                e->drop_item_name = s;
                e->tile.drop_item_name = e->drop_item_name.c_str();
            }
        }
        lua_pop(L, 1);

        int stack = read_table_int_opt(L, -1, "count", -1);
        if (stack <= 0)
            stack = read_table_int_opt(L, -1, "stack", 1);
        if (stack > 0)
            e->tile.drop_stack = stack;
    }
    lua_pop(L, 1);

    const int top_stack = read_table_int_opt(L, 1, "drop_stack", 0);
    if (top_stack > 0)
        e->tile.drop_stack = top_stack;

    lua_getfield(L, 1, "on_drop");
    if (lua_isfunction(L, -1))
        e->on_drop_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    lua_getfield(L, 1, "merge_with");
    if (lua_istable(L, -1)) {
        const int n = static_cast<int>(lua_rawlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TNUMBER) {
                const int id = static_cast<int>(lua_tointeger(L, -1));
                if (id >= 0)
                    e->merge_ids.push_back(id);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                const char *s = lua_tostring(L, -1);
                if (s && *s)
                    e->merge_names.emplace_back(s);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    for (const auto &mn : e->merge_names)
        e->merge_name_ptrs.push_back(mn.c_str());
    e->tile.merge_with_ids = e->merge_ids.empty() ? nullptr : e->merge_ids.data();
    e->tile.merge_with_count = static_cast<int>(e->merge_ids.size());
    e->tile.merge_with_names = e->merge_name_ptrs.empty() ? nullptr : e->merge_name_ptrs.data();
    e->tile.merge_with_names_count = static_cast<int>(e->merge_name_ptrs.size());

    lua_getfield(L, 1, "adj_tiles");
    if (lua_istable(L, -1)) {
        const int n = static_cast<int>(lua_rawlen(L, -1));
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TNUMBER) {
                const int id = static_cast<int>(lua_tointeger(L, -1));
                if (id >= 0)
                    e->adj_ids.push_back(id);
            } else if (lua_type(L, -1) == LUA_TSTRING) {
                const char *s = lua_tostring(L, -1);
                if (s && *s)
                    e->adj_names.emplace_back(s);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    e->tile.adj_tile_ids = e->adj_ids.empty() ? nullptr : e->adj_ids.data();
    e->tile.adj_tile_count = static_cast<int>(e->adj_ids.size());

    bool has_light = false;
    double lr = 0.0, lg = 0.0, lb = 0.0;
    read_rgb_triple(L, 1, "light", &has_light, &lr, &lg, &lb);
    if (has_light) {
        e->tile.lighted = true;
        e->tile.light_r = static_cast<float>(lr);
        e->tile.light_g = static_cast<float>(lg);
        e->tile.light_b = static_cast<float>(lb);
    }

    bool has_map = false;
    double mr = 0.0, mg = 0.0, mb = 0.0;
    read_rgb_triple(L, 1, "map_color", &has_map, &mr, &mg, &mb);
    if (has_map) {
        auto clamp_u8 = [](double v) -> unsigned char {
            const int iv = static_cast<int>(v);
            return static_cast<unsigned char>(std::max(0, std::min(255, iv)));
        };
        e->tile.has_map_color = true;
        e->tile.map_r = clamp_u8(mr);
        e->tile.map_g = clamp_u8(mg);
        e->tile.map_b = clamp_u8(mb);
    }

    lua_getfield(L, 1, "language");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const int culture = culture_from_lang_key(lua_tostring(L, -2));
                if (culture > 0) {
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        e->lang[culture] = lua_tostring(L, -1);
                    } else if (lua_istable(L, -1)) {
                        lua_getfield(L, -1, "name");
                        if (lua_type(L, -1) == LUA_TSTRING)
                            e->lang[culture] = lua_tostring(L, -1);
                        lua_pop(L, 1);
                    }
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    e->tile.has_map_name = !e->lang.empty();

    lua_getfield(L, 1, "texture");
    if (lua_type(L, -1) == LUA_TSTRING) {
        const char *rel = lua_tostring(L, -1);
        e->texture_path = resolve_item_image(handle->private_dir, handle->mod_dir, rel);
        if (e->texture_path.empty())
            LOG_WARN("[mod.tile] {}: 未找到贴图 {}", tile_name, rel);
    }
    lua_pop(L, 1);

    e->tile.parent_modloader_id = LUALOADER_PKG_ID;
    e->tile.parent_id = handle->mod_id.c_str();
    e->tile.internal_name = e->name.c_str();
    e->tile.runtime_id = -1;
    e->tile.tile_ops.init_static = lua_tile_init_static;
    e->tile.tile_ops.get_texture = lua_tile_get_texture;
    e->tile.tile_ops.get_drop = (e->on_drop_ref != LUA_NOREF) ? lua_tile_get_drop : nullptr;
    e->tile.tile_ops.get_light = nullptr;

    if (!terraria_tile_manager_register_tile_ext(&e->tile, sizeof(e->tile))) {
        LOG_ERROR("[mod.tile] {}: 注册失败", tile_name);
        if (e->on_drop_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, e->on_drop_ref);
        delete e;
        lua_pushnil(L);
        return 1;
    }

    e->registered = true;
    e->runtime_id = e->tile.runtime_id;
    g_lua_tiles.push_back(e);
    if (!e->lang.empty()) {
        ensure_tile_language_hook();
        rebuild_tile_language_cache();
    }
    LOG_INFO("[mod.tile] 已注册 {} (runtime_id={})", tile_name, e->runtime_id);
    lua_pushinteger(L, e->runtime_id);
    return 1;
}

static int l_tile_runtime_id(lua_State *L) {
    const char *arg1 = luaL_checkstring(L, 1);
    const char *arg2 = luaL_optstring(L, 2, nullptr);
    if (arg2 && *arg2) {
        terraria_tile_handle_t *t = terraria_tile_manager_find_tile(LUALOADER_PKG_ID, arg1, arg2);
        if (t && t->runtime_id >= 0) {
            lua_pushinteger(L, t->runtime_id);
            return 1;
        }
        lua_pushnil(L);
        return 1;
    }

    lua_mod_handle_t *handle = get_handle(L);
    const int rid = find_tile_runtime_id_for_owner(handle, arg1);
    if (rid >= 0)
        lua_pushinteger(L, rid);
    else
        lua_pushnil(L);
    return 1;
}

static int l_tile_set_merge(lua_State *L) {
    lua_mod_handle_t *handle = get_handle(L);
    auto resolve_arg = [&](int idx) -> int {
        if (lua_type(L, idx) == LUA_TNUMBER)
            return static_cast<int>(lua_tointeger(L, idx));
        if (lua_type(L, idx) == LUA_TSTRING)
            return find_tile_runtime_id_for_owner(handle, lua_tostring(L, idx));
        return -1;
    };
    const int id_a = resolve_arg(1);
    const int id_b = resolve_arg(2);
    if (id_a < 0 || id_b < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool merge = lua_isnoneornil(L, 3) ? true : (lua_toboolean(L, 3) != 0);
    terraria_tile_manager_set_merge(id_a, id_b, merge);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg mod_tile_functions[] = {
    {"register", l_tile_register},
    {"runtime_id", l_tile_runtime_id},
    {"set_merge", l_tile_set_merge},
    {nullptr, nullptr},
};

void unregister_tiles(lua_mod_handle_t *handle) {
    if (!handle)
        return;
    bool lang_changed = false;
    for (size_t i = g_lua_tiles.size(); i-- > 0;) {
        LuaTileEntry *e = g_lua_tiles[i];
        if (e->owner != handle)
            continue;
        if (!e->lang.empty())
            lang_changed = true;
        if (e->registered)
            terraria_tile_manager_unregister_tile(&e->tile);
        if (handle->L && e->on_drop_ref != LUA_NOREF)
            luaL_unref(handle->L, LUA_REGISTRYINDEX, e->on_drop_ref);
        delete e;
        g_lua_tiles.erase(g_lua_tiles.begin() + static_cast<long>(i));
    }
    if (lang_changed)
        rebuild_tile_language_cache();
}

void add_tile(lua_State *L) {
    luaL_setfuncs(L, mod_tile_functions, 0);
}

} // namespace lualoader::lua_api
