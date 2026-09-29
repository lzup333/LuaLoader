/*******************************************************************************
 * LuaLoader - lua engine
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

#include "lua_engine.hpp"
#include "lua_api.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "logger.hpp"
#include "platform.hpp"

namespace lualoader::lua_engine {

    static int traceback(lua_State *L) {
        const char *msg = lua_tostring(L, 1);
        if (msg) {
            luaL_traceback(L, L, msg, 1);
        } else {
            lua_pushliteral(L, "<error object>");
        }
        return 1;
    }

    /// 打开全部 Lua 标准库（base/table/string/math/utf8/coroutine/io/os/debug/package）。
    /// 说明：LuaLoader 不再限制 Lua 能力；Mod 是否可信由玩家自行审查 Mod 包决定。
    static void open_all_libs(lua_State *L) {
        luaL_openlibs(L);
    }

    /// 配置 package：把 Mod 目录加入 package.path（保留系统默认路径），
    /// 同时禁用 C 原生模块加载（清空 cpath 并移除 loadlib），避免混入 .so 原生代码。
    static void configure_package(lua_State *L, const std::string &mod_dir) {
        lua_getglobal(L, "package");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }

        // 在默认搜索路径前追加 Mod 目录，保证 require 能加载 Mod 自带的 Lua 模块
        lua_getfield(L, -1, "path");
        const char *default_path = lua_tostring(L, -1);
        const std::string mod_pattern = mod_dir + "/?.lua;" + mod_dir + "/?/init.lua;";
        const std::string pattern = mod_pattern + (default_path ? default_path : "");
        lua_pop(L, 1);
        lua_pushstring(L, pattern.c_str());
        lua_setfield(L, -2, "path");

        // 禁用原生 C 模块加载
        lua_pushliteral(L, "");
        lua_setfield(L, -2, "cpath");
        lua_pushnil(L);
        lua_setfield(L, -2, "loadlib");
        lua_pop(L, 1);
    }

    static bool run_file(lua_State *L, const std::string &path, std::string &err) {
        if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            err = msg ? msg : "failed to load lua file";
            lua_pop(L, 1);
            return false;
        }

        lua_pushcfunction(L, traceback);
        lua_insert(L, 1);
        if (lua_pcall(L, 0, 0, 1) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            err = msg ? msg : "failed to run lua file";
            lua_pop(L, 2); // error + traceback function
            return false;
        }
        lua_remove(L, 1); // traceback function
        return true;
    }

    static void protected_call(lua_State *L, const int ref, const char *what) {
        if (ref == LUA_NOREF) return;
        const int base = lua_gettop(L);
        lua_pushcfunction(L, traceback);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_pcall(L, 0, 0, base + 1) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
        }
        lua_settop(L, base);
    }

    /// 调用栈顶的函数（函数与参数已入栈），带 traceback。调用后函数/参数出栈。
    static void pcall_top(lua_State *L, const int nargs, const char *what) {
        const int funcidx = lua_gettop(L) - nargs;
        lua_pushcfunction(L, traceback);
        lua_insert(L, funcidx);
        const int status = lua_pcall(L, nargs, 0, funcidx);
        lua_remove(L, funcidx); // 移除 traceback
        if (status != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, msg ? msg : "unknown error");
            lua_pop(L, 1);
        }
    }

    /// 执行全局表 name（若存在）中的每个条目：
    ///   - 函数值：直接调用
    ///   - 字符串：作为全局函数名查找后调用
    /// 其余类型与未知的函数名会被跳过（并给出警告）
    static void run_todo_list(lua_State *L, const char *name) {
        lua_getglobal(L, name);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        const int len = static_cast<int>(luaL_len(L, -1));
        for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isfunction(L, -1)) {
                pcall_top(L, 0, name);
                continue;
            }
            if (lua_type(L, -1) == LUA_TSTRING) {
                const std::string func_name = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_getglobal(L, func_name.c_str());
                if (lua_isfunction(L, -1)) {
                    const std::string label = std::string(name) + " -> " + func_name;
                    pcall_top(L, 0, label.c_str());
                } else {
                    LOG_WARN("[lua] {} refers to unknown function '{}'", name, func_name);
                    lua_pop(L, 1);
                }
                continue;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    static int ref_global_field(lua_State *L, const char *key) {
        lua_getglobal(L, "mod");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return LUA_NOREF;
        }
        lua_getfield(L, -1, key);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // mod
        return ref;
    }

    static bool read_string_field(lua_State *L, int table_idx, const char *key, std::string &out) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        bool ok = false;
        if (lua_isstring(L, -1)) {
            out = lua_tostring(L, -1);
            ok = true;
        }
        lua_pop(L, 1);
        return ok;
    }

    static bool read_int_field(lua_State *L, int table_idx, const char *key, int &out) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        bool ok = false;
        if (lua_isnumber(L, -1)) {
            out = static_cast<int>(lua_tointeger(L, -1));
            ok = true;
        }
        lua_pop(L, 1);
        return ok;
    }

    /// 读取 Lua 脚本中的 mod.meta 覆盖 mod.json 中的信息
    static void read_info(lua_State *L, lua_mod_handle_t *handle) {
        lua_getglobal(L, "mod");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        lua_getfield(L, -1, "meta");
        if (lua_istable(L, -1)) {
            std::string text;
            if (read_string_field(L, -1, "pkg_id", text)) {
                handle->info.pkg_id = text;
            } else if (read_string_field(L, -1, "name", text)) {
                handle->info.pkg_id = text;
            }
            if (read_string_field(L, -1, "version", text)) {
                handle->info.version = text;
            }
            read_int_field(L, -1, "version_code", handle->info.version_code);
            read_int_field(L, -1, "api_version", handle->info.api_version);
        }
        lua_pop(L, 1); // meta
        lua_pop(L, 1); // mod
    }

    /// 解析入口脚本路径。优先使用内核部署 Mod 资源的位置 <private_dir>/lib/，
    /// 与 KernelLoader 查找动态库的约定保持一致，并提供若干回退路径便于本地开发。
    static std::vector<std::filesystem::path> entry_candidates(const lua_mod_handle_t *handle,
                                                               const std::string &config_dir) {
        const std::vector<std::filesystem::path> dirs = {
                std::filesystem::path(handle->private_dir) / "lib",
                std::filesystem::path(handle->private_dir),
                std::filesystem::path(config_dir),
                std::filesystem::path(config_dir) / "Resources" / "lib",
        };
        std::vector<std::filesystem::path> candidates;
        for (const auto &dir: dirs) {
            candidates.push_back(dir / handle->entry);
        }
        return candidates;
    }

    bool load(lua_mod_handle_t *handle,
              const std::string &config_path,
              const std::string &private_dir,
              std::string &err) {
        handle->private_dir = private_dir;
        handle->platform = LUALOADER_PLATFORM_NAME;
        handle->arch = LUALOADER_ARCH_NAME;
        if (handle->entry.empty()) handle->entry = "main.lua";

        const std::string config_dir = std::filesystem::path(config_path).parent_path().string();
        std::string entry_path;
        for (const auto &candidate: entry_candidates(handle, config_dir)) {
            if (std::filesystem::exists(candidate)) {
                entry_path = candidate.string();
                break;
            }
        }
        if (entry_path.empty()) {
            err = "entry script not found: " + (std::filesystem::path(private_dir) / "lib" / handle->entry).string();
            return false;
        }
        handle->mod_dir = std::filesystem::path(entry_path).parent_path().string();

        lua_State *L = luaL_newstate();
        if (!L) {
            err = "failed to create Lua state";
            return false;
        }
        handle->L = L;

        open_all_libs(L);
        configure_package(L, handle->mod_dir);
        lua_api::register_api(L, handle);

        LOG_INFO("Running Lua entry: {}", entry_path);
        if (!run_file(L, entry_path, err)) {
            LOG_ERROR("Failed to run Lua mod {}: {}", handle->mod_id, err);
            lua_close(L);
            handle->L = nullptr;
            return false;
        }

        read_info(L, handle);
        handle->init_ref = ref_global_field(L, "init");
        handle->cleanup_ref = ref_global_field(L, "cleanup");
        return true;
    }

    void call_init(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        lua_State *L = handle->L;

        // 平台化任务清单，按“通用 -> 平台 -> 平台+架构”的顺序执行
        run_todo_list(L, "todo_list");
        const std::string platform_list = "todo_list_" + handle->platform;
        run_todo_list(L, platform_list.c_str());
        const std::string platform_arch_list = "todo_list_" + handle->platform + "_" + handle->arch;
        run_todo_list(L, platform_arch_list.c_str());

        protected_call(L, handle->init_ref, "mod.init");

        // 若脚本定义了 mod.on_gui，挂到 XNAUnityRunner.OnGUI 上（每帧绘制）
        lua_api::install_gui_hook(handle);
    }

    void call_cleanup(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        protected_call(handle->L, handle->cleanup_ref, "mod.cleanup");
    }

    void uninstall_hooks(lua_mod_handle_t *handle) {
        lua_api::uninstall_all_hooks(handle);
    }

    void close(lua_mod_handle_t *handle) {
        if (!handle) return;
        lua_api::uninstall_all_hooks(handle);
        if (handle->L) {
            lua_close(handle->L);
            handle->L = nullptr;
        }
        handle->init_ref = LUA_NOREF;
        handle->cleanup_ref = LUA_NOREF;
    }

} // namespace lualoader::lua_engine
