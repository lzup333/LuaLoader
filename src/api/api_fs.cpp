/**
 * Mod 私有目录文件读写：mod.read_file / mod.write_file / mod.file_exists。
 *
 * 所有路径都限制在 handle->private_dir 内：
 *   - 拒绝绝对路径（/、\、盘符）
 *   - 拒绝任何包含 ".." 的路径段
 * 这样 Mod 不能借文件 API 读到别的 Mod 或游戏目录。
 */

#include "api_internal.hpp"

namespace lualoader::lua_api {

// ========================================================================
// 文件 API（限制在 private_dir 内）
// ========================================================================

/// 判断 name 是否是安全的相对路径：
/// 非空、非绝对路径、且没有任何一段是 ".."。
static bool is_safe_relative(const char *name) {
    if (!name || name[0] == '\0')
        return false;
    if (name[0] == '/' || name[0] == '\\')
        return false;
    const std::filesystem::path p(name);
    if (p.is_absolute())
        return false;
    for (const auto &part : p) {
        if (part == "..")
            return false;
    }
    return true;
}

/// mod.read_file(name) -> string | nil, err
/// 以二进制方式读取私有目录下的文件。
int l_mod_read_file(lua_State *L) {
    auto *handle = get_handle(L);
    const char *name = luaL_checkstring(L, 1);
    if (!handle)
        return luaL_error(L, "no mod context");
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

/// mod.write_file(name, data) -> bool | nil, err
/// 覆盖写入；父目录不存在时自动创建。
int l_mod_write_file(lua_State *L) {
    auto *handle = get_handle(L);
    const char *name = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (!handle)
        return luaL_error(L, "no mod context");
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
    // 用 good() 反映写入/flush 是否成功（磁盘满、权限变化等）。
    lua_pushboolean(L, file.good());
    return 1;
}

/// mod.file_exists(name) -> bool
/// 路径不安全时直接返回 false，不抛错。
int l_mod_file_exists(lua_State *L) {
    auto *handle = get_handle(L);
    const char *name = luaL_checkstring(L, 1);
    if (!handle)
        return luaL_error(L, "no mod context");
    if (!is_safe_relative(name)) {
        lua_pushboolean(L, false);
        return 1;
    }
    const std::filesystem::path path = std::filesystem::path(handle->private_dir) / name;
    lua_pushboolean(L, std::filesystem::exists(path));
    return 1;
}

} // namespace lualoader::lua_api
