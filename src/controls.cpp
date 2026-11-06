#if defined(__ANDROID__)

/**
 * PE（手机版）原生触摸按键桥接。
 *
 * 把 Lua 侧的 mod.controls.* 调用转成内核 button_manager 的注册/查询接口。
 * 图标 PNG 由加载器自己解码成 RGBA，再交给内核绘制；内核只负责布局与命中测试。
 */

#include "controls.hpp"
#include "logger.hpp"
#include "tefkernel/terraria/content/button_manager.h"

#include <cstring>
#include <filesystem>

#include "image_decode.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace lualoader::controls {
namespace {

/// 按钮作用域的加载器标识；内核用 (loader_id, mod_id) 区分不同 Mod。
constexpr const char *loader_id = "lzup333.lualoader";

/// 当前 Mod 的按钮作用域。
terraria_button_scope_t scope_of(lua_mod_handle_t *h) {
    return {loader_id, h->mod_id.c_str()};
}

/// 从闭包 upvalue 取回 Mod 句柄（register_api 时绑定）。
lua_mod_handle_t *owner(lua_State *L) {
    return static_cast<lua_mod_handle_t *>(lua_touserdata(L, lua_upvalueindex(1)));
}

/// 内核是否导出了完整的 button_manager API。
bool linked() {
    return terraria_button_manager_available && terraria_button_manager_error &&
           terraria_button_manager_configure_storage && terraria_button_manager_install &&
           terraria_button_manager_shutdown && terraria_button_manager_register && terraria_button_manager_unregister &&
           terraria_button_manager_unregister_owner && terraria_button_manager_down &&
           terraria_button_manager_pressed && terraria_button_manager_released && terraria_button_manager_set_enabled &&
           terraria_button_manager_set_visible;
}

/// 内核返回的错误描述；未链接时给固定文案。
const char *error() {
    return linked() ? terraria_button_manager_error() : "kernel button_manager unavailable";
}

/// mod.controls.available() -> bool, err
int available(lua_State *L) {
    lua_pushboolean(L, linked() && terraria_button_manager_available());
    lua_pushstring(L, error());
    return 2;
}

/// mod.controls.register{ id=..., name=..., icon=..., show_label=... } -> id
///
/// icon 是相对 mod.private_dir 的 PNG 路径；会做 canonical + 越界检查，
/// 防止通过绝对路径或 .. 读到 Mod 私有目录之外的文件。
int add(lua_State *L) {
    if (!linked() || !terraria_button_manager_available())
        return luaL_error(L, "native touch bindings unavailable: %s", error());

    luaL_checktype(L, 1, LUA_TTABLE);

    // id：1-64 个 ASCII 字母/数字/_/-/.，作为内核侧的唯一键。
    lua_getfield(L, 1, "id");
    std::string id = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    if (id.empty() || id.size() > 64 ||
        id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") != std::string::npos)
        return luaL_error(L, "control id must be 1-64 ASCII letters, digits, _, - or .");

    // name：1-128 字节（UTF-8 直接按字节限制）。
    lua_getfield(L, 1, "name");
    std::string name = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    if (name.empty() || name.size() > 128)
        return luaL_error(L, "control name must be 1-128 UTF-8 bytes");

    // icon：可选，相对路径，长度 <= 512，不允许内嵌 NUL。
    lua_getfield(L, 1, "icon");
    std::string icon_path;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TSTRING);
        size_t length = 0;
        const char *path = lua_tolstring(L, -1, &length);
        if (length > 512 || std::memchr(path, 0, length))
            return luaL_error(L, "control icon path must be at most 512 bytes without NUL");
        icon_path.assign(path, length);
    }
    lua_pop(L, 1);

    // show_label：可选布尔，默认 true。
    lua_getfield(L, 1, "show_label");
    bool show_label = true;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        show_label = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    auto *h = owner(L);

    // 解码图标：确保路径始终落在 mod.private_dir 内（含符号链接解析）。
    std::vector<unsigned char> pixels;
    terraria_button_image_t image{};
    if (!icon_path.empty()) {
        std::error_code ec;
        auto root = std::filesystem::canonical(h->private_dir, ec);
        if (ec)
            return luaL_error(L, "control icon directory unavailable");

        auto relative = std::filesystem::path(icon_path);
        if (relative.is_absolute())
            return luaL_error(L, "control icon must be relative to the mod directory");
        for (const auto &part : relative)
            if (part == "..")
                return luaL_error(L, "control icon path escapes mod directory");

        auto path = std::filesystem::canonical(root / relative, ec);
        if (ec)
            return luaL_error(L, "control icon file unavailable");
        // 符号链接可能指向 root 之外，再检查一次相对位置。
        auto child = path.lexically_relative(root);
        if (child.empty() || *child.begin() == "..")
            return luaL_error(L, "control icon path escapes mod directory");

        if (!image::decode_png_file(path.string(), pixels, image.width, image.height))
            return luaL_error(L, "control icon PNG decode failed");
        if (image.width < 1 || image.height < 1 || image.width > 2048 || image.height > 2048)
            return luaL_error(L, "control icon dimensions must be 1-2048");
        image.rgba = pixels.data();
        image.size = pixels.size();
    }

    // 注册到内核；pixels 在调用期间保持存活。
    terraria_button_desc_t desc{scope_of(h), id.c_str(), name.c_str(), icon_path.empty() ? nullptr : &image,
                                show_label};
    if (!terraria_button_manager_register(&desc))
        return luaL_error(L, "control registration failed: %s", error());

    lua_pushlstring(L, id.data(), id.size());
    return 1;
}

/// mod.controls.unregister(id) -> bool
int remove(lua_State *L) {
    auto *h = owner(L);
    auto scope = scope_of(h);
    const char *id = luaL_checkstring(L, 1);
    lua_pushboolean(L, linked() && terraria_button_manager_unregister(&scope, id));
    return 1;
}

/// mod.controls.down(id) -> bool（当前是否按住）
int down(lua_State *L) {
    auto *h = owner(L);
    auto scope = scope_of(h);
    const char *id = luaL_checkstring(L, 1);
    lua_pushboolean(L, linked() && terraria_button_manager_down(&scope, id));
    return 1;
}

/// mod.controls.pressed(id) -> bool（本帧刚按下）
int pressed(lua_State *L) {
    auto *h = owner(L);
    auto scope = scope_of(h);
    const char *id = luaL_checkstring(L, 1);
    lua_pushboolean(L, linked() && terraria_button_manager_pressed(&scope, id));
    return 1;
}

/// mod.controls.released(id) -> bool（本帧刚松开）
int released(lua_State *L) {
    auto *h = owner(L);
    auto scope = scope_of(h);
    const char *id = luaL_checkstring(L, 1);
    lua_pushboolean(L, linked() && terraria_button_manager_released(&scope, id));
    return 1;
}

/// set_enabled / set_visible 的公共实现。
int flag(lua_State *L, bool visible) {
    auto *h = owner(L);
    auto scope = scope_of(h);
    const char *id = luaL_checkstring(L, 1);
    const bool value = lua_toboolean(L, 2);
    // 先确认内核 API 已链接，再做实际调用；否则函数指针可能为空。
    const bool ok = linked() && (visible ? terraria_button_manager_set_visible(&scope, id, value)
                                         : terraria_button_manager_set_enabled(&scope, id, value));
    if (!ok)
        return luaL_error(L, "control not registered");
    return 0;
}

/// mod.controls.set_enabled(id, bool)
int enable(lua_State *L) {
    return flag(L, false);
}

/// mod.controls.set_visible(id, bool)
int show(lua_State *L) {
    return flag(L, true);
}

} // namespace

void configure_storage(const std::string &directory) {
    if (linked())
        terraria_button_manager_configure_storage(directory.c_str());
}

void install() {
    if (!linked()) {
        LOG_ERROR("[button-interface] kernel button_manager exports missing; install the modified PE kernel");
        return;
    }
    terraria_button_manager_install();
    if (!terraria_button_manager_available())
        LOG_ERROR("[button-interface] kernel initialization failed: {}", error());
    else
        LOG_INFO("[button-interface] ready: native PE buttons and caller-decoded RGBA icons");
}

void shutdown() {
    // 内核自己管理 hook；Lua 侧只需要注销 Mod 的按钮作用域。
}

void append_image_owners(std::vector<std::string> &) {
    // 图标已在注册时解码并复制给内核，这里没有额外需要跟踪的 owner。
}

void unregister_owner(lua_mod_handle_t *h) {
    if (h && linked()) {
        auto scope = scope_of(h);
        terraria_button_manager_unregister_owner(&scope);
    }
}

void register_api(lua_State *L, lua_mod_handle_t *h) {
    // 每个函数都用同一个 lightuserdata 作为 upvalue，回调里即可取回 Mod 句柄。
    lua_newtable(L);
    const luaL_Reg funcs[] = {
        {"available", available}, {"register", add},     {"unregister", remove},
        {"down", down},           {"pressed", pressed},  {"released", released},
        {"set_enabled", enable},  {"set_visible", show}, {nullptr, nullptr},
    };
    for (auto *f = funcs; f->name; ++f) {
        lua_pushlightuserdata(L, h);
        lua_pushcclosure(L, f->func, 1);
        lua_setfield(L, -2, f->name);
    }
    lua_setfield(L, -2, "controls");
}

} // namespace lualoader::controls

#endif
