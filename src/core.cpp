/*******************************************************************************
 * LuaLoader - core
 * Copyright (C) 2026 lzup333
 * Author: lzup333
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *******************************************************************************/

#include "core.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "json.hpp"
#include "logger.hpp"
#include "lua_engine.hpp"
#include "platform.hpp"

API_EXPORT const ml_ops_t *API_CALL ml_create() {
    static ml_ops_t ml_ops{};

    ml_ops.cleanup_ml = lualoader::core::cleanup_ml;
    ml_ops.get_info = lualoader::core::get_info;
    ml_ops.get_multiplayer_info = lualoader::core::get_multiplayer_info;
    ml_ops.init_ml = lualoader::core::init_ml;
    ml_ops.init_mod = lualoader::core::init_mod;
    ml_ops.load_mod = lualoader::core::load_mod;
    ml_ops.reload_mod = lualoader::core::reload_mod;
    ml_ops.unload_mod = lualoader::core::unload_mod;

    return &ml_ops;
}

ml_result_t lualoader::core::load_mod(mod_manifest_t *mod_manifest) {
    LOG_INFO("Starting to load Lua mod: mod_id={}, path={}, private_dir={}",
             mod_manifest->mod_id, mod_manifest->path, mod_manifest->private_dir);

    std::filesystem::path config_path(mod_manifest->path);
    if (!std::filesystem::exists(config_path)) {
        LOG_ERROR("Mod config not found: {}", config_path.string());
        return ML_ERROR_NOT_FOUND;
    }

    auto *handle = new lua_mod_handle_t;
    auto *multiplayer_info = new multiplayer_mod_info_t;

    try {
        std::ifstream mod_file(config_path);
        auto json_data = nlohmann::json::parse(mod_file);

        handle->mod_id = mod_manifest->mod_id ? mod_manifest->mod_id : "";
        handle->entry = json_data.value("main", std::string("main.lua"));
        handle->info.pkg_id = json_data.value("pkg_id", json_data.value("name", handle->mod_id));
        handle->info.version = json_data.value("version", std::string("1.0.0"));
        handle->info.version_code = json_data.value("version_code", 1);
        handle->info.api_version = json_data.value("api_version", 1);
        const bool multiplayer_safe = json_data.value("multiplayer_safe", true);

        std::string err;
        if (!lua_engine::load(handle, config_path.string(), mod_manifest->private_dir, err)) {
            LOG_ERROR("Failed to load Lua mod {}: {}", handle->mod_id, err);
            delete handle;
            delete multiplayer_info;
            return ML_ERROR;
        }

        multiplayer_info->mod_id = strdup(handle->info.pkg_id.c_str());
        multiplayer_info->is_multiplayer_safe = multiplayer_safe ? 1 : 0;
        multiplayer_info->version = strdup(handle->info.version.c_str());
        multiplayer_info->version_code = handle->info.version_code;

        lua_mod_handles.emplace(handle->mod_id, handle);
        lua_mod_multiplayer_infos.emplace(handle->mod_id, multiplayer_info);

        LOG_INFO("Lua mod loaded: {} v{} ({}), entry={}",
                 handle->info.pkg_id, handle->info.version, handle->mod_id, handle->entry);
        return ML_SUCCESS;
    } catch (const nlohmann::json::parse_error &e) {
        LOG_ERROR("JSON parse error while loading mod {}: {}", mod_manifest->mod_id, e.what());
    } catch (const std::filesystem::filesystem_error &e) {
        LOG_ERROR("Filesystem error while loading mod {}: {}", mod_manifest->mod_id, e.what());
    } catch (const std::exception &e) {
        LOG_ERROR("Exception while loading mod {}: {}", mod_manifest->mod_id, e.what());
    }

    lua_engine::close(handle);
    delete handle;
    delete multiplayer_info;
    return ML_ERROR;
}

ml_result_t lualoader::core::unload_mod(mod_manifest_t *mod_manifest) {
    LOG_INFO("Unloading Lua mod: {}", mod_manifest->mod_id);

    const auto it = lua_mod_handles.find(mod_manifest->mod_id);
    if (it == lua_mod_handles.end()) {
        LOG_WARN("Mod not found: {}", mod_manifest->mod_id);
        return ML_ERROR_NOT_FOUND;
    }

    auto *handle = it->second;

    LOG_DEBUG("Calling mod.cleanup ...");
    lua_engine::call_cleanup(handle);
    lua_engine::close(handle);
    delete handle;

    const auto info_it = lua_mod_multiplayer_infos.find(mod_manifest->mod_id);
    if (info_it != lua_mod_multiplayer_infos.end()) {
        free(const_cast<char *>(info_it->second->mod_id));
        free(const_cast<char *>(info_it->second->version));
        delete info_it->second;
        lua_mod_multiplayer_infos.erase(info_it);
    }

    lua_mod_handles.erase(it);
    LOG_INFO("Lua mod unloaded: {}", mod_manifest->mod_id);
    return ML_SUCCESS;
}

ml_result_t lualoader::core::reload_mod(mod_manifest_t *mod_manifest) {
    LOG_INFO("Reloading Lua mod: {}", mod_manifest->mod_id);
    if (lua_mod_handles.find(mod_manifest->mod_id) != lua_mod_handles.end()) {
        unload_mod(mod_manifest);
    }
    return load_mod(mod_manifest);
}

ml_result_t lualoader::core::init_mod(mod_manifest_t *mod_manifest) {
    LOG_INFO("Initializing Lua mod: {}", mod_manifest->mod_id);

    const auto it = lua_mod_handles.find(mod_manifest->mod_id);
    if (it == lua_mod_handles.end()) {
        LOG_ERROR("Cannot initialize mod - not found: {}", mod_manifest->mod_id);
        return ML_ERROR_NOT_FOUND;
    }

    lua_engine::call_init(it->second);
    LOG_INFO("Lua mod initialized: {}", mod_manifest->mod_id);
    return ML_SUCCESS;
}

const multiplayer_mod_info_t *lualoader::core::get_multiplayer_info(mod_manifest_t *mod_manifest) {
    const auto it = lua_mod_multiplayer_infos.find(mod_manifest->mod_id);
    if (it == lua_mod_multiplayer_infos.end()) {
        LOG_WARN("Multiplayer info not found for mod: {}", mod_manifest->mod_id);
        return nullptr;
    }
    return it->second;
}

ml_result_t lualoader::core::init_ml(ml_entry_t *ml_entry) {
    LOG_INFO("Initializing LuaLoader...");
    LOG_DEBUG("Platform: {}, Architecture: {}", LUALOADER_PLATFORM_NAME, LUALOADER_ARCH_NAME);

    logger::init();
    logger::init_mod();

    LOG_INFO("LuaLoader initialized successfully");
    return ML_SUCCESS;
}

ml_result_t lualoader::core::cleanup_ml(ml_entry_t *ml_entry) {
    LOG_INFO("Cleaning up LuaLoader...");

    for (auto &entry: lua_mod_handles) {
        lua_engine::call_cleanup(entry.second);
        lua_engine::close(entry.second);
        delete entry.second;
    }
    lua_mod_handles.clear();

    for (auto &entry: lua_mod_multiplayer_infos) {
        free(const_cast<char *>(entry.second->mod_id));
        free(const_cast<char *>(entry.second->version));
        delete entry.second;
    }
    lua_mod_multiplayer_infos.clear();

    LOG_INFO("LuaLoader cleaned up successfully");

    logger::shutdown_mod();
    logger::shutdown();

    return ML_SUCCESS;
}

const ml_info_t *lualoader::core::get_info() {
    static ml_info_t info = {
            "lzup333.lualoader",
            1,
            "1.0.0",
            1,
            0,
            nullptr,
    };
    return &info;
}
