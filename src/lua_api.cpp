/*******************************************************************************
 * LuaLoader - lua mod api
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

#include "lua_api.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include "logger.hpp"

#include "tefkernel/patchlib/field.h"
#include "tefkernel/patchlib/method.h"
#include "tefkernel/patchlib/property.h"
#include "tefkernel/patchlib/struct/array.h"
#include "tefkernel/patchlib/struct/string.h"
#include "tefkernel/patchlib/type.h"
#include "tefkernel/tefstd/vector.h"

namespace lualoader::lua_api {

    static constexpr const char *HANDLE_KEY = "lualoader.mod.handle";
    static constexpr const char *HOOK_METATABLE = "LuaLoader.Hook";
    static constexpr const char *OWNED_HANDLE_MT = "LuaLoader.OwnedHandle";
    static constexpr int MAX_HOOK_SLOTS = 32;
    static constexpr int MAX_CALL_ARGS = 16;

    // ========================================================================
    // 基础工具
    // ========================================================================

    static lua_mod_handle_t *get_handle(lua_State *L) {
        lua_getfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);
        auto *h = static_cast<lua_mod_handle_t *>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return h;
    }

    /// 由 Lua GC 托管的内核对象句柄：__gc 时自动释放（Android 上释放为空操作）
    struct OwnedHandle {
        patch_handle_t handle;
    };

    /// 复制内核对象句柄，使其生命周期独立于 hook 调用。
    /// 桌面端为新 GCHandle（指向同一对象，读写字段仍作用于原对象）；Android 直接返回原句柄。
    static patch_handle_t copy_handle(const patch_handle_t h) {
#if defined(__ANDROID__)
        return h;
#else
        return patchlib_handle_copy(h);
#endif
    }

    /// 压入一个由 Lua GC 托管的句柄（nil 安全）
    static void push_owned_handle(lua_State *L, const patch_handle_t h) {
        if (!h) {
            lua_pushnil(L);
            return;
        }
        auto *ud = static_cast<OwnedHandle *>(lua_newuserdatauv(L, sizeof(OwnedHandle), 0));
        ud->handle = h;
        luaL_setmetatable(L, OWNED_HANDLE_MT);
    }

    /// 托管句柄的 __gc：Lua GC 回收时释放内核句柄，避免泄漏
    static int l_owned_handle_gc(lua_State *L) {
        auto *ud = static_cast<OwnedHandle *>(luaL_checkudata(L, 1, OWNED_HANDLE_MT));
        if (ud && ud->handle) {
            patchlib_free(ud->handle);
            ud->handle = nullptr;
        }
        return 0;
    }

    static void push_handle(lua_State *L, const patch_handle_t h) {
        if (h) {
            lua_pushlightuserdata(L, h);
        } else {
            lua_pushnil(L);
        }
    }

    /// 从 Lua 栈取句柄：兼容 lightuserdata（借用）与 GC 托管 userdata（自有）
    static patch_handle_t to_handle(lua_State *L, const int idx) {
        if (lua_isnoneornil(L, idx)) return nullptr;
        if (lua_type(L, idx) == LUA_TLIGHTUSERDATA) {
            return static_cast<patch_handle_t>(lua_touserdata(L, idx));
        }
        if (auto *ud = static_cast<OwnedHandle *>(luaL_testudata(L, idx, OWNED_HANDLE_MT))) {
            return ud->handle;
        }
        return nullptr;
    }

    /// 将 patchlib 的值缓冲区压入 Lua 栈
    static void push_patch_value(lua_State *L, const patch_type_t type, const void *value) {
        if (!value) {
            lua_pushnil(L);
            return;
        }
        switch (type) {
            case PATCH_BOOL:     lua_pushboolean(L, *static_cast<const bool *>(value)); break;
            case PATCH_INT8:     lua_pushinteger(L, *static_cast<const int8_t *>(value)); break;
            case PATCH_INT16:    lua_pushinteger(L, *static_cast<const int16_t *>(value)); break;
            case PATCH_INT32:    lua_pushinteger(L, *static_cast<const int32_t *>(value)); break;
            case PATCH_INT64:    lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<const int64_t *>(value))); break;
            case PATCH_UINT8:    lua_pushinteger(L, *static_cast<const uint8_t *>(value)); break;
            case PATCH_UINT16:   lua_pushinteger(L, *static_cast<const uint16_t *>(value)); break;
            case PATCH_UINT32:   lua_pushinteger(L, *static_cast<const uint32_t *>(value)); break;
            case PATCH_UINT64:   lua_pushinteger(L, static_cast<lua_Integer>(*static_cast<const uint64_t *>(value))); break;
            case PATCH_FLOAT:    lua_pushnumber(L, *static_cast<const float *>(value)); break;
            case PATCH_DOUBLE:   lua_pushnumber(L, *static_cast<const double *>(value)); break;
            case PATCH_CHAR:     lua_pushinteger(L, *static_cast<const char *>(value)); break;
            case PATCH_POINTER:
            case PATCH_OBJECT: {
                void *ptr = *static_cast<void *const *>(value);
                if (ptr) lua_pushlightuserdata(L, ptr);
                else lua_pushnil(L);
                break;
            }
            case PATCH_VOID:
            default:
                lua_pushnil(L);
                break;
        }
    }

    /// 从 Lua 栈读取值写入 patchlib 值缓冲区，缓冲区至少 8 字节
    static bool read_patch_value(lua_State *L, const int idx, const patch_type_t type, void *out) {
        switch (type) {
            case PATCH_BOOL:
                *static_cast<bool *>(out) = lua_toboolean(L, idx) != 0;
                break;
            case PATCH_INT8:
                *static_cast<int8_t *>(out) = static_cast<int8_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT16:
                *static_cast<int16_t *>(out) = static_cast<int16_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT32:
                *static_cast<int32_t *>(out) = static_cast<int32_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_INT64:
                *static_cast<int64_t *>(out) = static_cast<int64_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT8:
                *static_cast<uint8_t *>(out) = static_cast<uint8_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT16:
                *static_cast<uint16_t *>(out) = static_cast<uint16_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT32:
                *static_cast<uint32_t *>(out) = static_cast<uint32_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_UINT64:
                *static_cast<uint64_t *>(out) = static_cast<uint64_t>(luaL_checkinteger(L, idx));
                break;
            case PATCH_FLOAT:
                *static_cast<float *>(out) = static_cast<float>(luaL_checknumber(L, idx));
                break;
            case PATCH_DOUBLE:
                *static_cast<double *>(out) = static_cast<double>(luaL_checknumber(L, idx));
                break;
            case PATCH_CHAR:
                *static_cast<char *>(out) = static_cast<char>(luaL_checkinteger(L, idx));
                break;
            case PATCH_POINTER:
            case PATCH_OBJECT:
                *static_cast<void **>(out) = to_handle(L, idx);
                break;
            case PATCH_VOID:
            default:
                break;
        }
        return true;
    }

    static int lua_traceback(lua_State *L) {
        const char *msg = lua_tostring(L, 1);
        if (msg) {
            luaL_traceback(L, L, msg, 1);
        } else {
            lua_pushliteral(L, "<error object>");
        }
        return 1;
    }

    /// 调用栈顶的函数（函数与参数已按顺序入栈），带 traceback，出错时记录日志
    static int pcall_log(lua_State *L, const int nargs, const int nresults, const char *what) {
        const int funcidx = lua_gettop(L) - nargs;
        lua_pushcfunction(L, lua_traceback);
        lua_insert(L, funcidx);
        const int status = lua_pcall(L, nargs, nresults, funcidx);
        lua_remove(L, funcidx);
        if (status != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_ERROR("[lua] {} failed: {}", what, err ? err : "unknown error");
            lua_pop(L, 1);
        }
        return status;
    }

    // ========================================================================
    // 日志 API
    // ========================================================================

    static mod_log_level_t parse_log_level(lua_State *L, const int idx) {
        if (lua_type(L, idx) == LUA_TNUMBER) {
            const int lv = static_cast<int>(lua_tointeger(L, idx));
            if (lv >= MOD_LOG_LEVEL_TRACE && lv <= MOD_LOG_LEVEL_FATAL) {
                return static_cast<mod_log_level_t>(lv);
            }
            return MOD_LOG_LEVEL_INFO;
        }
        const char *name = lua_tostring(L, idx);
        if (!name) return MOD_LOG_LEVEL_INFO;
        if (std::strcmp(name, "trace") == 0) return MOD_LOG_LEVEL_TRACE;
        if (std::strcmp(name, "debug") == 0) return MOD_LOG_LEVEL_DEBUG;
        if (std::strcmp(name, "info") == 0) return MOD_LOG_LEVEL_INFO;
        if (std::strcmp(name, "warn") == 0 || std::strcmp(name, "warning") == 0) return MOD_LOG_LEVEL_WARNING;
        if (std::strcmp(name, "error") == 0) return MOD_LOG_LEVEL_ERROR;
        if (std::strcmp(name, "critical") == 0) return MOD_LOG_LEVEL_CRITICAL;
        if (std::strcmp(name, "fatal") == 0) return MOD_LOG_LEVEL_FATAL;
        return MOD_LOG_LEVEL_INFO;
    }

    static std::string join_lua_args(lua_State *L, const int first) {
        const int top = lua_gettop(L);
        std::string out;
        for (int i = first; i <= top; ++i) {
            size_t len = 0;
            const char *s = luaL_tolstring(L, i, &len);
            if (i > first) out += ' ';
            if (s) out.append(s, len);
            lua_pop(L, 1);
        }
        return out;
    }

    static int log_impl(lua_State *L, const mod_log_level_t level, const int first_arg) {
        auto *handle = get_handle(L);
        const std::string message = join_lua_args(L, first_arg);
        const char *tag = handle ? handle->mod_id.c_str() : "lua";
        logger::write(level, tag, message.c_str());
        return 0;
    }

    static int l_mod_log(lua_State *L) {
        const mod_log_level_t level = parse_log_level(L, 1);
        return log_impl(L, level, 2);
    }

#define LUALOADER_LOG_WRAPPER(fn_name, level_const)          \
    static int fn_name(lua_State *L) {                       \
        return log_impl(L, level_const, 1);                  \
    }

    LUALOADER_LOG_WRAPPER(l_mod_trace, MOD_LOG_LEVEL_TRACE)
    LUALOADER_LOG_WRAPPER(l_mod_debug, MOD_LOG_LEVEL_DEBUG)
    LUALOADER_LOG_WRAPPER(l_mod_info, MOD_LOG_LEVEL_INFO)
    LUALOADER_LOG_WRAPPER(l_mod_warn, MOD_LOG_LEVEL_WARNING)
    LUALOADER_LOG_WRAPPER(l_mod_error, MOD_LOG_LEVEL_ERROR)
    LUALOADER_LOG_WRAPPER(l_mod_fatal, MOD_LOG_LEVEL_FATAL)

#undef LUALOADER_LOG_WRAPPER

    // ========================================================================
    // 文件 API（限制在 private_dir 内）
    // ========================================================================

    static bool is_safe_relative(const char *name) {
        if (!name || name[0] == '\0') return false;
        if (name[0] == '/' || name[0] == '\\') return false;
        const std::filesystem::path p(name);
        if (p.is_absolute()) return false;
        for (const auto &part: p) {
            if (part == "..") return false;
        }
        return true;
    }

    static int l_mod_read_file(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushnil(L);
            lua_pushfstring(L, "unsafe path: %s", name);
            return 2;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot open %s", name);
            return 2;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string data = ss.str();
        lua_pushlstring(L, data.data(), data.size());
        return 1;
    }

    static int l_mod_write_file(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        size_t len = 0;
        const char *data = luaL_checklstring(L, 2, &len);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushnil(L);
            lua_pushfstring(L, "unsafe path: %s", name);
            return 2;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            lua_pushnil(L);
            lua_pushfstring(L, "cannot write %s", name);
            return 2;
        }
        file.write(data, static_cast<std::streamsize>(len));
        lua_pushboolean(L, file.good());
        return 1;
    }

    static int l_mod_file_exists(lua_State *L) {
        auto *handle = get_handle(L);
        const char *name = luaL_checkstring(L, 1);
        if (!handle) return luaL_error(L, "no mod context");
        if (!is_safe_relative(name)) {
            lua_pushboolean(L, false);
            return 1;
        }
        const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
        lua_pushboolean(L, std::filesystem::exists(path));
        return 1;
    }

    // ========================================================================
    // Patchlib: 类型 / 字段 / 方法
    // ========================================================================

    static int l_patch_get_type(lua_State *L) {
        const char *ns = luaL_checkstring(L, 1);
        const char *name = luaL_checkstring(L, 2);
        push_handle(L, patchlib_type_get_type(ns, name));
        return 1;
    }

    static bool type_from_name(const char *name, patch_type_t *out) {
        static const struct {
            const char *name;
            patch_type_t type;
        } table[] = {
                {"void", PATCH_VOID}, {"int8", PATCH_INT8}, {"int16", PATCH_INT16}, {"int32", PATCH_INT32},
                {"int64", PATCH_INT64}, {"uint8", PATCH_UINT8}, {"uint16", PATCH_UINT16}, {"uint32", PATCH_UINT32},
                {"uint64", PATCH_UINT64}, {"bool", PATCH_BOOL}, {"float", PATCH_FLOAT}, {"double", PATCH_DOUBLE},
                {"pointer", PATCH_POINTER}, {"object", PATCH_OBJECT}, {"char", PATCH_CHAR},
        };
        for (const auto &entry: table) {
            if (std::strcmp(entry.name, name) == 0) {
                *out = entry.type;
                return true;
            }
        }
        return false;
    }

    /// 取字段类型：优先使用 Lua 显式传入的类型名，否则向内核查询
    static patch_type_t resolve_field_type(lua_State *L, const int type_idx, const patch_handle_t field) {
        if (!lua_isnoneornil(L, type_idx)) {
            if (lua_type(L, type_idx) == LUA_TSTRING) {
                patch_type_t type;
                if (type_from_name(lua_tostring(L, type_idx), &type)) return type;
                luaL_error(L, "unknown type name: %s", lua_tostring(L, type_idx));
            } else if (lua_isnumber(L, type_idx)) {
                return static_cast<patch_type_t>(lua_tointeger(L, type_idx));
            }
        }
        return patchlib_field_get_type(field);
    }

    static int l_patch_get_basic_type(lua_State *L) {
        const char *name = luaL_checkstring(L, 1);
        patch_type_t type;
        if (type_from_name(name, &type)) {
            push_handle(L, patchlib_get_basic_type(type));
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    static int l_patch_get_parent(lua_State *L) {
        push_handle(L, patchlib_type_get_parent(to_handle(L, 1)));
        return 1;
    }

    static int l_patch_type_name(lua_State *L) {
        const char *name = patchlib_type_get_name(to_handle(L, 1));
        if (name) lua_pushstring(L, name); else lua_pushnil(L);
        return 1;
    }

    static int l_patch_is_valid(lua_State *L) {
        lua_pushboolean(L, patchlib_is_valid(to_handle(L, 1)));
        return 1;
    }

    static int l_patch_get_field(lua_State *L) {
        push_handle(L, patchlib_type_get_field(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_property(lua_State *L) {
        push_handle(L, patchlib_type_get_property(to_handle(L, 1), luaL_checkstring(L, 2)));
        return 1;
    }

    static int l_patch_get_method(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const char *name = luaL_checkstring(L, 2);
        if (lua_isnoneornil(L, 3)) {
            push_handle(L, patchlib_type_get_method(type, name));
        } else {
            const int argc = static_cast<int>(luaL_checkinteger(L, 3));
            push_handle(L, patchlib_type_get_method_by_param_count(type, name, argc));
        }
        return 1;
    }

    // ========================================================================
    // Patchlib: 托管字符串辅助
    // ========================================================================

    /// 是否为字符串伪类型名 "string"
    static bool is_string_type_arg(lua_State *L, const int idx) {
        return lua_type(L, idx) == LUA_TSTRING && std::strcmp(lua_tostring(L, idx), "string") == 0;
    }

    /// 读取引用类型字段的对象句柄（Android 走真实指针，桌面走 get_value）
    static patch_handle_t read_field_object(const patch_handle_t field, const patch_handle_t instance) {
        patch_handle_t obj = nullptr;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (ptr) obj = *static_cast<patch_handle_t *>(ptr);
#else
        patchlib_field_get_value(field, instance, &obj);
#endif
        return obj;
    }

    /// 写入引用类型字段的对象句柄
    static void write_field_object(const patch_handle_t field, const patch_handle_t instance, const patch_handle_t obj) {
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (ptr) *static_cast<patch_handle_t *>(ptr) = obj;
#else
        patch_handle_t value = obj;
        patchlib_field_set_value(field, instance, &value);
#endif
    }

    /// 把托管 System.String 对象句柄压入 Lua（失败压 nil）
    static void push_managed_string(lua_State *L, const patch_handle_t str) {
        if (!str) {
            lua_pushnil(L);
            return;
        }
        char *c = patchlib_string_cstr(str);
        if (!c) {
            lua_pushnil(L);
            return;
        }
        lua_pushstring(L, c);
        free(c);
    }

    static int l_patch_field_get_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (is_string_type_arg(L, 3)) {
            push_managed_string(L, read_field_object(field, instance));
            return 1;
        }
        const patch_type_t type = resolve_field_type(L, 3, field);
#if defined(__ANDROID__)
        // Android/IL2CPP 下 get_value 不可靠，直接用字段真实指针
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) {
            lua_pushnil(L);
            return 1;
        }
        push_patch_value(L, type, ptr);
#else
        uint64_t buffer = 0;
        patchlib_field_get_value(field, instance, &buffer);
        push_patch_value(L, type, &buffer);
#endif
        return 1;
    }

    static int l_patch_field_set_value(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        if (is_string_type_arg(L, 4)) {
            const char *s = luaL_checkstring(L, 3);
            const patch_handle_t str = patchlib_string_create(s);
            if (!str) return luaL_error(L, "cannot create managed string");
            write_field_object(field, instance, str);
            patchlib_free(str);
            return 0;
        }
        const patch_type_t type = resolve_field_type(L, 4, field);
#if defined(__ANDROID__)
        // Android/IL2CPP 下 set_value 不可靠，直接写入字段真实指针
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return luaL_error(L, "cannot get field pointer");
        read_patch_value(L, 3, type, ptr);
#else
        uint64_t buffer = 0;
        read_patch_value(L, 3, type, &buffer);
        patchlib_field_set_value(field, instance, &buffer);
#endif
        return 0;
    }

    /// 依据签名把 Lua 参数编组为 patchlib 的 void** 参数数组
    static int build_arg_array(lua_State *L, const int first, const patch_method_signature_t *sig,
                               uint64_t storage[][1], void *argv[], const int capacity) {
        const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
        if (argc > capacity) {
            return luaL_error(L, "too many method arguments (max %d)", capacity);
        }
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
            read_patch_value(L, first + i, type, &storage[i]);
            argv[i] = &storage[i];
        }
        return argc;
    }

    static int l_patch_invoke(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(method, &sig)) {
            return luaL_error(L, "cannot get method signature");
        }

        patch_handle_t instance = nullptr;
        int arg_start = 2;
        if (sig.is_instance) {
            instance = to_handle(L, 2);
            arg_start = 3;
        }

        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        build_arg_array(L, arg_start, &sig, storage, argv, MAX_CALL_ARGS);

        uint64_t result = 0;
        const bool ok = patchlib_method_invoke_args(method, instance, &result, argv);
        const patch_type_t return_type = sig.return_type;
        patchlib_method_signature_free(&sig);

        if (!ok) {
            lua_pushboolean(L, false);
            return 1;
        }
        push_patch_value(L, return_type, &result);
        return 1;
    }

    static int l_patch_free(lua_State *L) {
        if (auto *ud = static_cast<OwnedHandle *>(luaL_testudata(L, 1, OWNED_HANDLE_MT))) {
            if (ud->handle) {
                patchlib_free(ud->handle);
                ud->handle = nullptr;
            }
            return 0;
        }
        patchlib_free(to_handle(L, 1));
        return 0;
    }

    // ========================================================================
    // Patchlib: 带参构造
    // ========================================================================

    /// 解析构造函数并创建实例（参数从 first_arg 开始按签名编组）
    static int construct_instance(lua_State *L, const patch_handle_t ctor, const int first_arg) {
        if (!ctor) return luaL_error(L, "constructor not found");

        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(ctor, &sig)) {
            return luaL_error(L, "cannot get constructor signature");
        }
        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        build_arg_array(L, first_arg, &sig, storage, argv, MAX_CALL_ARGS);
        patchlib_method_signature_free(&sig);

        patch_handle_t instance = nullptr;
        if (!patchlib_constructor_invoke(ctor, &instance, argv)) {
            lua_pushnil(L);
            return 1;
        }
        // 新实例句柄由 Lua GC 管理，避免 GCHandle 泄漏
        push_owned_handle(L, instance);
        return 1;
    }

    /// mod.patch.new_instance(type[, ...])：
    /// 无额外参数时走无参实例化（与旧版一致）；有参数时按 ".ctor" 带参构造
    static int l_patch_new_instance(lua_State *L) {
        const patch_handle_t type = to_handle(L, 1);
        const int argc = lua_gettop(L) - 1;
        if (argc <= 0) {
            push_handle(L, patchlib_type_new_instance(type));
            return 1;
        }
        if (argc > MAX_CALL_ARGS) {
            return luaL_error(L, "too many constructor arguments (max %d)", MAX_CALL_ARGS);
        }
        const patch_handle_t ctor = patchlib_type_get_method_by_param_count(type, ".ctor", argc);
        return construct_instance(L, ctor, 2);
    }

    /// mod.patch.construct(ctor, ...)：使用显式构造函数句柄创建实例
    static int l_patch_construct(lua_State *L) {
        const patch_handle_t ctor = to_handle(L, 1);
        if (!ctor) return luaL_error(L, "invalid constructor handle");
        return construct_instance(L, ctor, 2);
    }

    // ========================================================================
    // Patchlib: 托管字符串
    // ========================================================================

    /// mod.patch.string_create(s) -> 托管字符串句柄（由 Lua GC 管理）
    static int l_patch_string_create(lua_State *L) {
        const char *s = luaL_checkstring(L, 1);
        push_owned_handle(L, patchlib_string_create(s));
        return 1;
    }

    /// mod.patch.string_value(handle|string) -> Lua 字符串
    static int l_patch_string_value(lua_State *L) {
        const int t = lua_type(L, 1);
        if (t == LUA_TSTRING || t == LUA_TNUMBER) {
            lua_pushvalue(L, 1);
            return 1;
        }
        push_managed_string(L, to_handle(L, 1));
        return 1;
    }

    /// mod.patch.string_empty(handle) -> bool
    static int l_patch_string_empty(lua_State *L) {
        lua_pushboolean(L, patchlib_string_empty(to_handle(L, 1)));
        return 1;
    }

    /// mod.patch.string_length(handle) -> int
    static int l_patch_string_length(lua_State *L) {
        lua_pushinteger(L, static_cast<lua_Integer>(patchlib_string_length(to_handle(L, 1))));
        return 1;
    }

    // ========================================================================
    // Patchlib: 句柄生命周期
    // ========================================================================

    /// mod.patch.retain(handle) -> 由 Lua GC 管理的句柄副本
    /// 用于把 hook 临时对象跨帧缓存，避免内核释放原句柄后失效（Android 上为同一句柄）
    static int l_patch_retain(lua_State *L) {
        const patch_handle_t h = to_handle(L, 1);
        if (!h) {
            lua_pushnil(L);
            return 1;
        }
        push_owned_handle(L, copy_handle(h));
        return 1;
    }

    // ========================================================================
    // Patchlib: 结构体字段(Vector2 等) / 数组
    // ========================================================================

    // 注意: 不要调用 patchlib_field_get_size —— 内核的符号注入表里没有它，
    // 在模块中会是 NULL 函数指针, 调用即崩溃。尺寸用 patchlib_field_get_type +
    // get_size_from_patch_type 推断, 或由调用方显式给出。

    /// 读取字段的原始字节。Android/IL2CPP 走字段真实指针，桌面端走 get_value。
    static bool read_field_bytes(const patch_handle_t field, const patch_handle_t instance, void *out,
                                 const size_t size) {
        if (!field || !out || size == 0 || size > 16) return false;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return false;
        std::memcpy(out, ptr, size);
        return true;
#else
        std::memset(out, 0, size);
        patchlib_field_get_value(field, instance, out);
        return true;
#endif
    }

    /// 写入字段的原始字节。Android/IL2CPP 走字段真实指针，桌面端走 set_value。
    static bool write_field_bytes(const patch_handle_t field, const patch_handle_t instance, const void *in,
                                  const size_t size) {
        if (!field || !in || size == 0 || size > 16) return false;
#if defined(__ANDROID__)
        void *ptr = patchlib_field_get_pointer(field, instance);
        if (!ptr) return false;
        std::memcpy(ptr, in, size);
        return true;
#else
        patchlib_field_set_value(field, instance, const_cast<void *>(in));
        return true;
#endif
    }

    /// 推断字段大小(1~16)，失败返回 0。不使用未注入的 patchlib_field_get_size。
    static size_t guess_field_size(const patch_handle_t field) {
        if (!field) return 0;
        const size_t size = get_size_from_patch_type(patchlib_field_get_type(field));
        return size;
    }

    /// 读取 Vector2(或任意以两个 float 开头的 8 字节结构体)字段 → x, y
    static int l_patch_field_get_vec2(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        float v[2] = {0.0f, 0.0f};
        if (!read_field_bytes(field, instance, v, sizeof(v))) {
            lua_pushnil(L);
            lua_pushnil(L);
            return 2;
        }
        lua_pushnumber(L, v[0]);
        lua_pushnumber(L, v[1]);
        return 2;
    }

    /// 写入 Vector2(或任意以两个 float 开头的 8 字节结构体)字段 ← x, y
    static int l_patch_field_set_vec2(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        const float v[2] = {
                static_cast<float>(luaL_checknumber(L, 3)),
                static_cast<float>(luaL_checknumber(L, 4)),
        };
        if (!write_field_bytes(field, instance, v, sizeof(v))) {
            return luaL_error(L, "cannot write vector field");
        }
        return 0;
    }

    /// 读取字段原始字节，返回 Lua 字符串。
    /// 第三个参数为可选字节数；省略时按字段类型推断(失败则取 8)。
    static int l_patch_field_get_raw(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");

        size_t size;
        if (lua_isnoneornil(L, 3)) {
            size = guess_field_size(field);
            if (size == 0) size = 8; // 结构体等未知类型默认 8 字节
        } else {
            const lua_Integer requested = luaL_checkinteger(L, 3);
            size = requested > 0 ? static_cast<size_t>(requested) : 0;
        }
        if (size == 0 || size > 16) {
            lua_pushnil(L);
            return 1;
        }
        unsigned char buffer[16] = {};
        if (!read_field_bytes(field, instance, buffer, size)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushlstring(L, reinterpret_cast<const char *>(buffer), size);
        return 1;
    }

    /// 以 Lua 字符串写入字段原始字节（长度 1~16）
    static int l_patch_field_set_raw(lua_State *L) {
        const patch_handle_t field = to_handle(L, 1);
        const patch_handle_t instance = to_handle(L, 2);
        if (!field) return luaL_error(L, "invalid field handle");
        size_t len = 0;
        const char *data = luaL_checklstring(L, 3, &len);
        if (len == 0 || len > 16) {
            return luaL_error(L, "raw data length out of range (1..16)");
        }
        if (!write_field_bytes(field, instance, data, len)) {
            return luaL_error(L, "cannot write raw field");
        }
        return 0;
    }

    /// 数组长度
    static int l_patch_array_length(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        lua_pushinteger(L, static_cast<lua_Integer>(patchlib_array_length(array)));
        return 1;
    }

    /// 数组取元素。第三个参数为元素类型名，省略时按对象(指针)处理。
    static int l_patch_array_at(lua_State *L) {
        const patch_handle_t array = to_handle(L, 1);
        if (!array) return luaL_error(L, "invalid array handle");
        const size_t index = static_cast<size_t>(luaL_checkinteger(L, 2));
        uint64_t buffer = 0;
        if (!patchlib_array_at(array, index, &buffer)) {
            lua_pushnil(L);
            return 1;
        }
        if (lua_isnoneornil(L, 3)) {
            void *ptr = *reinterpret_cast<void **>(&buffer);
            if (ptr) lua_pushlightuserdata(L, ptr);
            else lua_pushnil(L);
            return 1;
        }
        const char *type_name = luaL_checkstring(L, 3);
        if (std::strcmp(type_name, "string") == 0) {
            push_managed_string(L, *reinterpret_cast<void **>(&buffer));
            return 1;
        }
        patch_type_t type;
        if (!type_from_name(type_name, &type)) {
            return luaL_error(L, "unknown type name: %s", type_name);
        }
        push_patch_value(L, type, &buffer);
        return 1;
    }

    // ========================================================================
    // Patchlib: 钩子（Prefix / Postfix）
    // ========================================================================

    struct HookSlot {
        bool used{false};
        lua_State *L{nullptr};
        lua_mod_handle_t *handle{nullptr};
        int prefix_ref{LUA_NOREF};
        int postfix_ref{LUA_NOREF};
        bool copy_handles{false}; ///< true 时把 instance/对象参数/返回值复制为 GC 托管句柄
        patch_hook_id_t hook_id{PATCH_HOOK_INVALID_ID};
    };

    static HookSlot g_hook_slots[MAX_HOOK_SLOTS];

    struct HookUserdata {
        int slot;
    };

    /// 钩子回调：先声明分发函数，再由宏生成固定签名的 C 入口
    static bool hook_prefix_dispatch(int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result);

    static void hook_postfix_dispatch(int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig);

#define LUALOADER_FOR_EACH_TRAMPOLINE(M)                 \
    M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)             \
    M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)       \
    M(16) M(17) M(18) M(19) M(20) M(21) M(22) M(23)     \
    M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)

#define LUALOADER_DEFINE_TRAMPOLINE(N)                                                                          \
    static bool prefix_trampoline_##N(patch_handle_t instance, void **args,                                     \
                                      const patch_method_signature_t *sig, void *result) {                      \
        return hook_prefix_dispatch(N, instance, args, sig, result);                                            \
    }                                                                                                           \
    static void postfix_trampoline_##N(patch_handle_t instance, void **args, void *result,                      \
                                       const patch_method_signature_t *sig) {                                   \
        hook_postfix_dispatch(N, instance, args, result, sig);                                                  \
    }

    LUALOADER_FOR_EACH_TRAMPOLINE(LUALOADER_DEFINE_TRAMPOLINE)

#define LUALOADER_PREFIX_PTR(N) prefix_trampoline_##N,
#define LUALOADER_POSTFIX_PTR(N) postfix_trampoline_##N,

    static prefix_callback_t g_prefix_table[] = {LUALOADER_FOR_EACH_TRAMPOLINE(LUALOADER_PREFIX_PTR)};
    static postfix_callback_t g_postfix_table[] = {LUALOADER_FOR_EACH_TRAMPOLINE(LUALOADER_POSTFIX_PTR)};

#undef LUALOADER_PREFIX_PTR
#undef LUALOADER_POSTFIX_PTR
#undef LUALOADER_DEFINE_TRAMPOLINE
#undef LUALOADER_FOR_EACH_TRAMPOLINE

    /// 压入一个参数值；copy_handles 时把对象/指针句柄复制为 GC 托管句柄
    static void push_hook_arg_value(lua_State *L, const patch_type_t type, const void *value, const bool copy_handles) {
        if (copy_handles && (type == PATCH_OBJECT || type == PATCH_POINTER) && value) {
            auto *ptr = *static_cast<void *const *>(value);
            if (ptr) {
                push_owned_handle(L, copy_handle(ptr));
                return;
            }
        }
        push_patch_value(L, type, value);
    }

    /// 钩子回调参数：instance、参数表 args（数组）、返回值 result
    static void push_hook_args(lua_State *L, patch_handle_t instance, void **args,
                               const patch_method_signature_t *sig, const bool copy_handles) {
        if (!instance) {
            lua_pushnil(L);
        } else if (copy_handles) {
            push_owned_handle(L, copy_handle(instance));
        } else {
            lua_pushlightuserdata(L, instance);
        }
        const int argc = sig ? static_cast<int>(tefstd_vector_size(&sig->arg_types)) : 0;
        lua_createtable(L, argc, 0);
        for (int i = 0; i < argc; ++i) {
            const auto type = *static_cast<patch_type_t *>(tefstd_vector_at(&sig->arg_types, i));
            push_hook_arg_value(L, type, args[i], copy_handles);
            lua_rawseti(L, -2, i + 1);
        }
    }

    static bool hook_prefix_dispatch(const int slot, patch_handle_t instance, void **args,
                                     const patch_method_signature_t *sig, void *result) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return false;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.prefix_ref == LUA_NOREF) return false;

        lua_State *L = s.L;
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.prefix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        // 参数：instance, args, result -> 3 个
        if (pcall_log(L, 3, 2, "prefix hook") != LUA_OK) {
            lua_settop(L, base);
            return false;
        }

        const bool skip = lua_toboolean(L, -2) != 0;
        if (skip && result && sig && sig->return_type != PATCH_VOID && !lua_isnil(L, -1)) {
            read_patch_value(L, -1, sig->return_type, result);
        }
        lua_settop(L, base);
        return skip;
    }

    static void hook_postfix_dispatch(const int slot, patch_handle_t instance, void **args, void *result,
                                      const patch_method_signature_t *sig) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used || !s.L || s.postfix_ref == LUA_NOREF) return;

        lua_State *L = s.L;
        const int base = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, s.postfix_ref);
        push_hook_args(L, instance, args, sig, s.copy_handles);
        if (result && sig && sig->return_type != PATCH_VOID) {
            push_hook_arg_value(L, sig->return_type, result, s.copy_handles);
        } else {
            lua_pushnil(L);
        }

        pcall_log(L, 3, 0, "postfix hook");
        lua_settop(L, base);
    }

    static void release_slot(const int slot) {
        if (slot < 0 || slot >= MAX_HOOK_SLOTS) return;
        HookSlot &s = g_hook_slots[slot];
        if (!s.used) return;

        if (s.hook_id != PATCH_HOOK_INVALID_ID) {
            patchlib_uninstall_hook(s.hook_id);
        }
        if (s.L) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(s.L, LUA_REGISTRYINDEX, s.postfix_ref);
        }
        if (s.handle) {
            auto &slots = s.handle->hook_slots;
            for (auto it = slots.begin(); it != slots.end(); ++it) {
                if (*it == slot) {
                    slots.erase(it);
                    break;
                }
            }
        }
        s = HookSlot{};
    }

    static int alloc_slot() {
        for (int i = 0; i < MAX_HOOK_SLOTS; ++i) {
            if (!g_hook_slots[i].used) {
                g_hook_slots[i].used = true;
                return i;
            }
        }
        return -1;
    }

    static int ref_function_field(lua_State *L, int table_idx, const char *key) {
        if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
        lua_getfield(L, table_idx, key);
        int ref = LUA_NOREF;
        if (lua_isfunction(L, -1)) {
            ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        return ref;
    }

    static int l_patch_install_hook(lua_State *L) {
        const patch_handle_t method = to_handle(L, 1);
        if (!method) return luaL_error(L, "invalid method handle");
        luaL_checktype(L, 2, LUA_TTABLE);

        const int slot = alloc_slot();
        if (slot < 0) return luaL_error(L, "no free hook slots (max %d)", MAX_HOOK_SLOTS);

        HookSlot &s = g_hook_slots[slot];
        s.L = L;
        s.handle = get_handle(L);
        s.prefix_ref = ref_function_field(L, 2, "prefix");
        s.postfix_ref = ref_function_field(L, 2, "postfix");
        lua_getfield(L, 2, "copy");
        s.copy_handles = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        if (s.prefix_ref == LUA_NOREF && s.postfix_ref == LUA_NOREF) {
            s = HookSlot{};
            return luaL_error(L, "hook must define a 'prefix' and/or 'postfix' function");
        }

        const prefix_callback_t prefix = s.prefix_ref != LUA_NOREF ? g_prefix_table[slot] : nullptr;
        const postfix_callback_t postfix = s.postfix_ref != LUA_NOREF ? g_postfix_table[slot] : nullptr;
        s.hook_id = patchlib_install_prepost_hook(method, prefix, postfix);

        if (s.hook_id == PATCH_HOOK_INVALID_ID) {
            if (s.prefix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.prefix_ref);
            if (s.postfix_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s.postfix_ref);
            s = HookSlot{};
            return luaL_error(L, "failed to install hook");
        }

        if (s.handle) s.handle->hook_slots.push_back(slot);

        auto *ud = static_cast<HookUserdata *>(lua_newuserdatauv(L, sizeof(HookUserdata), 0));
        ud->slot = slot;
        luaL_getmetatable(L, HOOK_METATABLE);
        lua_setmetatable(L, -2);
        return 1;
    }

    static int l_hook_remove(lua_State *L) {
        auto *ud = static_cast<HookUserdata *>(luaL_checkudata(L, 1, HOOK_METATABLE));
        if (ud->slot >= 0) {
            release_slot(ud->slot);
            ud->slot = -1;
        }
        return 0;
    }

    // ========================================================================
    // GUI: 即时模式 mod.gui（基于 UnityEngine.GUILayout）
    // 只能在 mod.on_gui 回调里调用（此时处于 Unity 的 OnGUI 阶段）
    // ========================================================================

    static patch_handle_t g_gui_type = nullptr;
    static patch_handle_t g_gui_label = nullptr;
    static patch_handle_t g_gui_button = nullptr;
    static patch_handle_t g_gui_toggle = nullptr;
    static patch_handle_t g_gui_slider = nullptr;
    static patch_handle_t g_gui_space = nullptr;
    static patch_handle_t g_gui_box = nullptr;
    static patch_handle_t g_gui_begin_h = nullptr;
    static patch_handle_t g_gui_end_h = nullptr;
    static patch_handle_t g_gui_begin_v = nullptr;
    static patch_handle_t g_gui_end_v = nullptr;
    static bool g_gui_inited = false;

    /// 先按参数名选重载，失败再退回按参数个数
    static patch_handle_t gui_find_method(const patch_handle_t type, const char *name, const int argc,
                                          const char **names) {
        patch_handle_t m = patchlib_type_get_method_by_param_names(type, name, argc, names);
        if (!m) m = patchlib_type_get_method_by_param_count(type, name, argc);
        return m;
    }

    static void ensure_gui() {
        if (g_gui_inited) return;
        g_gui_inited = true;
        g_gui_type = patchlib_type_get_type("UnityEngine", "GUILayout");
        if (!g_gui_type) {
            LOG_WARN("[gui] UnityEngine.GUILayout not found");
            return;
        }
        static const char *n_text_opts[] = {"text", "options"};
        static const char *n_val_text_opts[] = {"value", "text", "options"};
        static const char *n_slider[] = {"value", "leftValue", "rightValue", "options"};
        static const char *n_pixels[] = {"pixels"};
        static const char *n_options[] = {"options"};
        g_gui_label = gui_find_method(g_gui_type, "Label", 2, n_text_opts);
        g_gui_button = gui_find_method(g_gui_type, "Button", 2, n_text_opts);
        g_gui_box = gui_find_method(g_gui_type, "Box", 2, n_text_opts);
        g_gui_toggle = gui_find_method(g_gui_type, "Toggle", 3, n_val_text_opts);
        g_gui_slider = gui_find_method(g_gui_type, "HorizontalSlider", 4, n_slider);
        g_gui_space = gui_find_method(g_gui_type, "Space", 1, n_pixels);
        g_gui_begin_h = gui_find_method(g_gui_type, "BeginHorizontal", 1, n_options);
        g_gui_begin_v = gui_find_method(g_gui_type, "BeginVertical", 1, n_options);
        g_gui_end_h = patchlib_type_get_method_by_param_count(g_gui_type, "EndHorizontal", 0);
        g_gui_end_v = patchlib_type_get_method_by_param_count(g_gui_type, "EndVertical", 0);
        LOG_INFO("[gui] GUILayout resolved: label={} button={} toggle={} slider={} space={}",
                 g_gui_label != nullptr, g_gui_button != nullptr, g_gui_toggle != nullptr,
                 g_gui_slider != nullptr, g_gui_space != nullptr);
    }

    /// 调用静态方法，参数已在 Lua 栈 1..N
    static int gui_call(lua_State *L, const patch_handle_t method) {
        if (!method) {
            lua_pushnil(L);
            return 1;
        }
        patch_method_signature_t sig;
        if (!patchlib_method_get_signature(method, &sig)) {
            lua_pushnil(L);
            return 1;
        }
        uint64_t storage[MAX_CALL_ARGS][1] = {};
        void *argv[MAX_CALL_ARGS] = {};
        build_arg_array(L, 1, &sig, storage, argv, MAX_CALL_ARGS);
        const patch_type_t rt = sig.return_type;
        patchlib_method_signature_free(&sig);

        uint64_t result = 0;
        if (!patchlib_method_invoke_args(method, nullptr, &result, argv)) {
            lua_pushnil(L);
            return 1;
        }
        push_patch_value(L, rt, &result);
        return 1;
    }

    /// 把 Lua 字符串转成托管字符串句柄，并由调用方在 invoke 后 free
    static patch_handle_t make_gui_text(lua_State *L, const int idx) {
        return patchlib_string_create(luaL_checkstring(L, idx));
    }

    static int l_gui_label(lua_State *L) {
        ensure_gui();
        const patch_handle_t str = make_gui_text(L, 1);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, str);
        lua_pushnil(L);
        const int rc = gui_call(L, g_gui_label);
        patchlib_free(str);
        return rc;
    }

    static int l_gui_box(lua_State *L) {
        ensure_gui();
        const patch_handle_t str = make_gui_text(L, 1);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, str);
        lua_pushnil(L);
        const int rc = gui_call(L, g_gui_box);
        patchlib_free(str);
        return rc;
    }

    static int l_gui_button(lua_State *L) {
        ensure_gui();
        const patch_handle_t str = make_gui_text(L, 1);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, str);
        lua_pushnil(L);
        const int rc = gui_call(L, g_gui_button);
        patchlib_free(str);
        return rc;
    }

    static int l_gui_toggle(lua_State *L) {
        ensure_gui();
        const int value = lua_toboolean(L, 1);
        const patch_handle_t str = make_gui_text(L, 2);
        lua_settop(L, 0);
        lua_pushboolean(L, value);
        lua_pushlightuserdata(L, str);
        lua_pushnil(L);
        const int rc = gui_call(L, g_gui_toggle);
        patchlib_free(str);
        return rc;
    }

    static int l_gui_slider(lua_State *L) {
        ensure_gui();
        lua_settop(L, 3);
        lua_pushnil(L);
        return gui_call(L, g_gui_slider);
    }

    static int l_gui_space(lua_State *L) {
        ensure_gui();
        lua_settop(L, 1);
        return gui_call(L, g_gui_space);
    }

    static int l_gui_begin_h(lua_State *L) {
        ensure_gui();
        lua_settop(L, 0);
        lua_pushnil(L);
        return gui_call(L, g_gui_begin_h);
    }

    static int l_gui_end_h(lua_State *L) {
        ensure_gui();
        lua_settop(L, 0);
        return gui_call(L, g_gui_end_h);
    }

    static int l_gui_begin_v(lua_State *L) {
        ensure_gui();
        lua_settop(L, 0);
        lua_pushnil(L);
        return gui_call(L, g_gui_begin_v);
    }

    static int l_gui_end_v(lua_State *L) {
        ensure_gui();
        lua_settop(L, 0);
        return gui_call(L, g_gui_end_v);
    }

    static const luaL_Reg mod_gui_functions[] = {
            {"label", l_gui_label},
            {"box", l_gui_box},
            {"button", l_gui_button},
            {"toggle", l_gui_toggle},
            {"slider", l_gui_slider},
            {"space", l_gui_space},
            {"begin_horizontal", l_gui_begin_h},
            {"end_horizontal", l_gui_end_h},
            {"begin_vertical", l_gui_begin_v},
            {"end_vertical", l_gui_end_v},
            {nullptr, nullptr},
    };

    static const luaL_Reg mod_patch_functions[] = {
            {"get_type", l_patch_get_type},
            {"get_basic_type", l_patch_get_basic_type},
            {"new_instance", l_patch_new_instance},
            {"construct", l_patch_construct},
            {"get_parent", l_patch_get_parent},
            {"type_name", l_patch_type_name},
            {"is_valid", l_patch_is_valid},
            {"get_field", l_patch_get_field},
            {"get_property", l_patch_get_property},
            {"get_method", l_patch_get_method},
            {"get_field_value", l_patch_field_get_value},
            {"set_field_value", l_patch_field_set_value},
            {"get_field_vec2", l_patch_field_get_vec2},
            {"set_field_vec2", l_patch_field_set_vec2},
            {"get_field_raw", l_patch_field_get_raw},
            {"set_field_raw", l_patch_field_set_raw},
            {"array_length", l_patch_array_length},
            {"array_at", l_patch_array_at},
            {"string_create", l_patch_string_create},
            {"string_value", l_patch_string_value},
            {"string_empty", l_patch_string_empty},
            {"string_length", l_patch_string_length},
            {"retain", l_patch_retain},
            {"copy", l_patch_retain},
            {"invoke", l_patch_invoke},
            {"install_hook", l_patch_install_hook},
            {"free", l_patch_free},
            {nullptr, nullptr},
    };

    void register_api(lua_State *L, lua_mod_handle_t *handle) {
        lua_pushlightuserdata(L, handle);
        lua_setfield(L, LUA_REGISTRYINDEX, HANDLE_KEY);

        // 托管句柄元表：__gc 由 Lua GC 自动释放内核句柄（Android 上释放为空操作）
        luaL_newmetatable(L, OWNED_HANDLE_MT);
        lua_pushcfunction(L, l_owned_handle_gc);
        lua_setfield(L, -2, "__gc");
        lua_pop(L, 1);

        // 钩子句柄元表：不再注册 __gc 自动卸载，钩子只在显式 :remove()
        // 或 Mod 卸载时移除（否则丢弃返回值会被 Lua GC 误卸载）
        luaL_newmetatable(L, HOOK_METATABLE);
        lua_pushcfunction(L, l_hook_remove);
        lua_setfield(L, -2, "remove");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pop(L, 1);

        // 全局 mod 表
        lua_newtable(L);

        lua_pushstring(L, handle->mod_id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, handle->mod_dir.c_str());
        lua_setfield(L, -2, "mod_dir");
        lua_pushstring(L, handle->private_dir.c_str());
        lua_setfield(L, -2, "private_dir");
        lua_pushstring(L, handle->platform.c_str());
        lua_setfield(L, -2, "platform");
        lua_pushstring(L, handle->arch.c_str());
        lua_setfield(L, -2, "arch");
        lua_pushstring(L, handle->info.version.c_str());
        lua_setfield(L, -2, "version");

        lua_pushcfunction(L, l_mod_log);
        lua_setfield(L, -2, "log");
        lua_pushcfunction(L, l_mod_trace);
        lua_setfield(L, -2, "trace");
        lua_pushcfunction(L, l_mod_debug);
        lua_setfield(L, -2, "debug");
        lua_pushcfunction(L, l_mod_info);
        lua_setfield(L, -2, "info");
        lua_pushcfunction(L, l_mod_warn);
        lua_setfield(L, -2, "warn");
        lua_pushcfunction(L, l_mod_error);
        lua_setfield(L, -2, "error");
        lua_pushcfunction(L, l_mod_fatal);
        lua_setfield(L, -2, "fatal");

        lua_pushcfunction(L, l_mod_read_file);
        lua_setfield(L, -2, "read_file");
        lua_pushcfunction(L, l_mod_write_file);
        lua_setfield(L, -2, "write_file");
        lua_pushcfunction(L, l_mod_file_exists);
        lua_setfield(L, -2, "file_exists");

        lua_newtable(L);
        luaL_setfuncs(L, mod_patch_functions, 0);
        lua_setfield(L, -2, "patch");

        // mod.gui：即时模式 GUI（需在 mod.on_gui 回调内使用）
        lua_newtable(L);
        luaL_setfuncs(L, mod_gui_functions, 0);
        lua_setfield(L, -2, "gui");

        lua_pushvalue(L, -1);
        lua_setglobal(L, "mod");
        lua_pop(L, 1);
    }

    void install_gui_hook(lua_mod_handle_t *handle) {
        if (!handle || !handle->L) return;
        lua_State *L = handle->L;

        lua_getglobal(L, "mod");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        lua_getfield(L, -1, "on_gui");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            return; // 脚本没定义 on_gui，不挂
        }
        const int ref = luaL_ref(L, LUA_REGISTRYINDEX); // 弹出 on_gui
        lua_pop(L, 1); // 弹出 mod

        // XNAUnityRunner 在全局命名空间，OnGUI 是 Unity 每帧调用的空方法
        const patch_handle_t type = patchlib_type_get_type("", "XNAUnityRunner");
        const patch_handle_t method = type ? patchlib_type_get_method_by_param_count(type, "OnGUI", 0) : nullptr;
        if (!method) {
            LOG_WARN("[gui] XNAUnityRunner.OnGUI not found, on_gui disabled");
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return;
        }

        const int slot = alloc_slot();
        if (slot < 0) {
            LOG_WARN("[gui] no free hook slot for on_gui");
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            return;
        }
        HookSlot &s = g_hook_slots[slot];
        s.L = L;
        s.handle = handle;
        s.postfix_ref = ref;
        s.hook_id = patchlib_install_prepost_hook(method, nullptr, g_postfix_table[slot]);
        if (s.hook_id == PATCH_HOOK_INVALID_ID) {
            LOG_WARN("[gui] failed to install on_gui hook");
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            s = HookSlot{};
            return;
        }
        handle->hook_slots.push_back(slot);
        LOG_INFO("[gui] on_gui hook installed (id={})", static_cast<int>(s.hook_id));
    }

    void uninstall_all_hooks(lua_mod_handle_t *handle) {
        if (!handle) return;
        const std::vector<int> slots = handle->hook_slots;
        for (const int slot: slots) {
            release_slot(slot);
        }
    }

} // namespace lualoader::lua_api
