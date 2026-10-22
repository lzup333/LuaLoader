#include "api_internal.hpp"

namespace lualoader::lua_api {

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


    int l_mod_read_file(lua_State *L) {
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

    int l_mod_write_file(lua_State *L) {
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

    int l_mod_file_exists(lua_State *L) {
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


} // namespace lualoader::lua_api
