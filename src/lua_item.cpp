/*******************************************************************************
 * LuaLoader - lua_item (数据驱动自定义物品：lib/item/<name>.json + <name>.png)
 * Copyright (C) 2026 lzup333
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * 目录约定（随 Mod 一起分发，加载时自动扫描）：
 *   Resources/lib/item/<name>.json   —— 物品定义
 *   Resources/lib/item/<name>.png    —— 物品贴图（可再分发；可用 "texture" 指定其它文件名）
 *
 * JSON 示例：
 *   {
 *     "id": "mysword",            // 可选，默认取文件名
 *     "name": "我的剑",            // 可选，显示名
 *     "clone_from": 24,            // 可选，先套用原版物品默认值
 *     "texture": "mysword.png",    // 可选，默认同名 .json.png
 *     "properties": { "damage": 10, "useTime": 15, "useStyle": 1, "melee": true }
 *   }
 *******************************************************************************/

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "lua_item.hpp"
#include "json.hpp"
#include "lua_recipe.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "logger.hpp"

extern "C" {
#include "lua.h"
}

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/terraria/asset.h"
#include "tefkernel/terraria/item_manager.h"
#include "tefkernel/terraria/recipe_manager.h"
#include "tefkernel/terraria/texture2d.h"

namespace lualoader::lua_item {

    namespace {

        struct JsonItem {
            terraria_item_handle_t base; // 必须第一个成员（回调用它反推本结构）
            std::string loader_id;
            std::string mod_id;
            std::string internal_name;
            std::string display_name;
            nlohmann::json props;
            int clone_from = -1;

            std::vector<unsigned char> rgba;
            int tex_w = 0;
            int tex_h = 0;
            patch_handle_t texture_asset = nullptr;

            // 合成配方（可选）
            bool has_recipe = false;
            int recipe_count = 1;
            int recipe_station = -1;
            std::vector<int> recipe_mats; // 扁平 [id, count, id, count, ...]
            bool recipe_queued = false;
        };

        std::vector<JsonItem *> g_items; // 保活（内核只存指针）

        // 把带配方的 JSON 物品排队到 mod.recipe（游戏原生 Recipe 流程，SetupRecipes 后统一注册）
        void queue_json_recipes() {
            for (auto *it: g_items) {
                if (!it->has_recipe || it->recipe_queued || it->base.runtime_id < 0) continue;
                std::vector<std::pair<int, int>> mats;
                for (size_t k = 0; k + 1 < it->recipe_mats.size(); k += 2)
                    mats.emplace_back(it->recipe_mats[k], it->recipe_mats[k + 1]);
                if (mats.empty()) continue;
                if (lualoader::lua_recipe::queue_recipe(it->base.runtime_id, it->recipe_count,
                                                        mats, it->recipe_station)) {
                    it->recipe_queued = true;
                    LOG_INFO("[item] 已排队配方: {} <- {} 种材料", it->internal_name, mats.size());
                }
            }
        }

        patch_handle_t g_item_type = nullptr;
        patch_handle_t g_set_defaults = nullptr; // Item.SetDefaults(int, ItemVariant)
        patch_handle_t g_tex_class = nullptr;

        void ensure_handles() {
            if (!patchlib_is_valid(g_item_type)) {
                g_item_type = patchlib_type_get_type("Terraria", "Item");
                if (patchlib_is_valid(g_item_type)) {
                    g_set_defaults =
                            patchlib_type_get_method_by_param_count(g_item_type, "SetDefaults", 2);
                }
            }
        }

        void set_string_field(patch_handle_t inst, const char *field_name, const std::string &value) {
            patch_handle_t f = patchlib_type_get_field(g_item_type, field_name);
            if (!patchlib_is_valid(f)) {
                patchlib_free(f);
                return;
            }
            patch_handle_t s = patchlib_string_create(value.c_str());
            if (patchlib_is_valid(s)) {
                patchlib_field_set_value(f, inst, &s);
                patchlib_free(s);
            }
            patchlib_free(f);
        }

        void apply_field(patch_handle_t inst, const std::string &name, const nlohmann::json &v) {
            patch_handle_t f = patchlib_type_get_field(g_item_type, name.c_str());
            if (!patchlib_is_valid(f)) {
                LOG_WARN("[item] 未知字段: {}", name);
                patchlib_free(f);
                return;
            }
            const patch_type_t t = patchlib_field_get_type(f);

            if (v.is_boolean()) {
                bool b = v.get<bool>();
                patchlib_field_set_value(f, inst, &b);
            } else if (v.is_string()) {
                set_string_field(inst, name.c_str(), v.get<std::string>());
            } else if (v.is_number_float()) {
                double d = v.get<double>();
                if (t == PATCH_FLOAT) {
                    float x = static_cast<float>(d);
                    patchlib_field_set_value(f, inst, &x);
                } else if (t == PATCH_DOUBLE) {
                    patchlib_field_set_value(f, inst, &d);
                } else {
                    int x = static_cast<int>(d);
                    patchlib_field_set_value(f, inst, &x);
                }
            } else if (v.is_number_integer()) {
                int64_t i = v.get<int64_t>();
                switch (t) {
                    case PATCH_BOOL: { bool x = i != 0; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_INT8: { int8_t x = (int8_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_UINT8: { uint8_t x = (uint8_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_INT16: { int16_t x = (int16_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_UINT16: { uint16_t x = (uint16_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_UINT32: { uint32_t x = (uint32_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_UINT64: { uint64_t x = (uint64_t) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_INT64: { patchlib_field_set_value(f, inst, &i); break; }
                    case PATCH_FLOAT: { float x = (float) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_DOUBLE: { double x = (double) i; patchlib_field_set_value(f, inst, &x); break; }
                    case PATCH_INT32:
                    default: { int32_t x = (int32_t) i; patchlib_field_set_value(f, inst, &x); break; }
                }
            }
            patchlib_free(f);
        }

        void json_set_defaults(terraria_item_handle_t *current, patch_handle_t instance) {
            auto *it = reinterpret_cast<JsonItem *>(current);
            ensure_handles();
            if (!patchlib_is_valid(g_item_type)) return;
            LOG_INFO("[item] {}: set_defaults 开始(clone_from={})", it->internal_name, it->clone_from);

            // 内核刚把 type 写成自定义 id，先记下来
            int my_id = 0;
            patch_handle_t ftype = patchlib_type_get_field(g_item_type, "type");
            if (patchlib_is_valid(ftype)) {
                patchlib_field_get_value(ftype, instance, &my_id);
            }

            // 先套用原版物品模板
            if (it->clone_from >= 0 && patchlib_is_valid(g_set_defaults)) {
                int src = it->clone_from;
#if defined(__ANDROID__)
                // Android：SetDefaults 已被内核 hook，不能再用 ffi（签名为 Dobby 闭包而
                // 非原方法），必须用函数指针直接调用，签名 SetDefaults(int, ItemVariant, MethodInfo*)。
                using set_defaults_fn = void (*)(void *, int, void *);
                auto fn = reinterpret_cast<set_defaults_fn>(patchlib_method_get_pointer(g_set_defaults));
                if (fn) fn(instance, src, nullptr);
#else
                void *variant_null = nullptr;          // ItemVariant 参数值 = null
                void *args[2] = {&src, &variant_null}; // args[i] 必须是“指向参数值”的指针
                patchlib_method_invoke_args(g_set_defaults, instance, nullptr, args);
#endif
            }
            LOG_INFO("[item] {}: clone_from 完成", it->internal_name);

            // 再覆盖自定义属性
            if (it->props.is_object()) {
                for (auto &kv: it->props.items()) {
                    apply_field(instance, kv.key(), kv.value());
                }
            }

            // 恢复自定义 id（clone_from 可能把它改回原版 id）
            if (patchlib_is_valid(ftype)) {
                patchlib_field_set_value(ftype, instance, &my_id);
                patchlib_free(ftype);
            }

            LOG_INFO("[item] {}: set_defaults 属性完成", it->internal_name);

            // 显示名
            if (!it->display_name.empty()) {
                set_string_field(instance, "_nameOverride", it->display_name);
            }
            LOG_INFO("[item] {}: set_defaults 结束", it->internal_name);
        }

        patch_handle_t json_get_texture(terraria_item_handle_t *current) {
            auto *it = reinterpret_cast<JsonItem *>(current);
            if (patchlib_is_valid(it->texture_asset)) return it->texture_asset;
            if (it->rgba.empty() || it->tex_w <= 0 || it->tex_h <= 0) return nullptr;
            if (!terraria_texture2d_create || !terraria_asset_create) return nullptr;

            // Texture2D 类型句柄延迟到此时（图形设备已就绪）再取
            if (!patchlib_is_valid(g_tex_class)) {
                g_tex_class = terraria_texture2d_get_class();
            }
            if (!patchlib_is_valid(g_tex_class)) {
                LOG_WARN("[item] {}: 拿不到 Texture2D 类型", it->internal_name);
                return nullptr;
            }
            LOG_INFO("[item] {}: 创建贴图 {}x{} ...", it->internal_name, it->tex_w, it->tex_h);
            patch_handle_t tex = terraria_texture2d_create(
                    it->tex_w, it->tex_h, TEXTURE_FORMAT_RGBA32,
                    it->rgba.data(), it->rgba.size());
            if (!patchlib_is_valid(tex)) {
                LOG_WARN("[item] {}: 创建贴图失败", it->internal_name);
                return nullptr;
            }
            LOG_INFO("[item] {}: 贴图已创建，转为 Asset ...", it->internal_name);
            patch_handle_t asset = terraria_asset_create(g_tex_class, tex);
            patchlib_free(tex);
            it->texture_asset = asset;
            LOG_INFO("[item] {}: 贴图资源完成", it->internal_name);
            return asset;
        }

        bool load_texture(JsonItem *it, const std::filesystem::path &png_path) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(png_path, ec)) return false;

#if defined(__ANDROID__)
            stbi_set_flip_vertically_on_load(1); // Android 贴图 Y 轴与内核默认一致
#else
            stbi_set_flip_vertically_on_load(0);
#endif
            int w = 0, h = 0, comp = 0;
            unsigned char *data = stbi_load(png_path.string().c_str(), &w, &h, &comp, 4);
            if (!data) {
                LOG_WARN("[item] {}: 无法解码贴图 {}", it->internal_name, png_path.string());
                return false;
            }
            it->rgba.assign(data, data + static_cast<size_t>(w) * h * 4);
            it->tex_w = w;
            it->tex_h = h;
            stbi_image_free(data);
            return true;
        }

        bool register_one(lua_mod_handle_t *handle, const std::filesystem::path &json_path) {
            std::ifstream ifs(json_path);
            if (!ifs.is_open()) return false;
            nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
            if (j.is_discarded() || !j.is_object()) {
                LOG_WARN("[item] {}: JSON 解析失败", json_path.string());
                return false;
            }

            auto *it = new JsonItem();
            it->loader_id = "lzup333.lualoader";
            it->mod_id = handle->mod_id;
            it->internal_name = j.value("id", json_path.stem().string());
            it->display_name = j.value("name", std::string{});
            it->clone_from = j.value("clone_from", -1);
            it->props = j.value("properties", nlohmann::json::object());

            // 合成配方（可选）
            if (j.contains("recipe") && j["recipe"].is_object()) {
                const auto &r = j["recipe"];
                it->has_recipe = true;
                it->recipe_count = r.value("result_count", 1);
                it->recipe_station = r.value("station", -1);
                if (r.contains("materials") && r["materials"].is_array()) {
                    for (const auto &m: r["materials"]) {
                        int mid = 0, cnt = 1;
                        if (m.is_array() && m.size() >= 2) {
                            mid = m[0].get<int>();
                            cnt = m[1].get<int>();
                        } else if (m.is_object()) {
                            mid = m.value("item", 0);
                            cnt = m.value("count", 1);
                        }
                        if (mid > 0 && cnt > 0) {
                            it->recipe_mats.push_back(mid);
                            it->recipe_mats.push_back(cnt);
                        }
                    }
                }
            }

            it->base.parent_modloader_id = it->loader_id.c_str();
            it->base.parent_id = it->mod_id.c_str();
            it->base.internal_name = it->internal_name.c_str();
            it->base.has_tooltip = false;
            it->base.runtime_id = -1;
            it->base.item_ops.early_init = nullptr;
            it->base.item_ops.init_static = nullptr;
            it->base.item_ops.set_defaults = json_set_defaults;
            it->base.item_ops.can_use = nullptr;
            it->base.item_ops.get_texture = json_get_texture;

            const std::string tex_name = j.value("texture", json_path.stem().string() + ".png");
            load_texture(it, json_path.parent_path() / tex_name);

            if (!terraria_item_manager_register_item ||
                !terraria_item_manager_register_item(&it->base)) {
                LOG_WARN("[item] {}: 注册物品失败（{}）", it->internal_name, json_path.string());
                delete it;
                return false;
            }

            // 保活；热重载时替换旧条目，避免重复
            bool replaced = false;
            for (auto *&old: g_items) {
                if (old->mod_id == it->mod_id && old->internal_name == it->internal_name) {
                    delete old;
                    old = it;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) g_items.push_back(it);

            LOG_INFO("[item] 已从 JSON 注册物品 {}（mod {}，贴图 {}x{}{}{}）",
                     it->internal_name, it->mod_id, it->tex_w, it->tex_h,
                     it->rgba.empty() ? "，无贴图" : "",
                     it->has_recipe ? "，含配方" : "");
            return true;
        }
    } // namespace

    int load_items(lua_mod_handle_t *handle) {
        if (!handle || !terraria_item_manager_register_item) return 0;
        ensure_handles();
        if (!patchlib_is_valid(g_item_type)) {
            LOG_WARN("[item] 找不到 Terraria.Item，跳过 JSON 物品");
            return 0;
        }

        // 注意：private_dir/lib/item 与 mod_dir/item 可能是同一处，只扫前者；
        // 再用文件名去重，避免同一物品被注册两次。
        std::vector<std::filesystem::path> dirs;
        if (!handle->private_dir.empty())
            dirs.emplace_back(std::filesystem::path(handle->private_dir) / "lib" / "item");
        if (!handle->config_path.empty())
            dirs.emplace_back(std::filesystem::path(handle->config_path).parent_path() /
                              "Resources" / "lib" / "item");

        std::error_code ec;
        int count = 0;
        std::vector<std::string> seen;
        for (const auto &dir: dirs) {
            if (!std::filesystem::is_directory(dir, ec)) continue;
            for (const auto &entry: std::filesystem::directory_iterator(dir, ec)) {
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() != ".json") continue;
                const std::string stem = entry.path().stem().string();
                bool dup = false;
                for (const auto &s: seen) {
                    if (s == stem) { dup = true; break; }
                }
                if (dup) continue;
                seen.push_back(stem);
                if (register_one(handle, entry.path())) count++;
            }
        }
        // JSON 配方：走 LuaLoader 自带的 mod.recipe（游戏原生 Recipe 流程）
        // runtime_id 在内核 Main.Initialize 时才分配，因此注册“补队回调”，
        // 由 lua_recipe 在 flush 前调用，把此刻已就绪的配方补进队列。
        lualoader::lua_recipe::set_queue_supplier(&queue_json_recipes);
        queue_json_recipes();

        if (count > 0) {
            LOG_INFO("[item] Mod {} 共注册 {} 个 JSON 物品", handle->mod_id, count);
        }
        return count;
    }
} // namespace lualoader::lua_item
