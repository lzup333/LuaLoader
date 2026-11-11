#if defined(__ANDROID__)

/**
 * PE（手机版）原生触摸按键桥接。
 *
 * 把 Lua 侧的 mod.controls.* 调用转成内核 button_manager 的注册/查询接口。
 * 图标 PNG 由加载器自己解码成 RGBA，再交给内核绘制；内核只负责布局与命中测试。
 */

#include "controls.hpp"
#include "api/api_internal.hpp"
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

/// 解析按键图标路径。
///
/// 新约定：图标统一放 `<mod>/Image/` 下（与物品/弹幕贴图同一套查找顺序）；
/// 老 Mod 原来放的位置仍然兼容（Mod 根目录、`Resources/Image`、`lib/Image` 等）。
/// 无论命中哪一个，都会 canonical 化并确认最终文件仍在 Mod 目录内，防止
/// 绝对路径或符号链接读到 Mod 目录之外。失败时通过 error 返回可直接展示的原因。
static bool resolve_icon_path(const lua_mod_handle_t *h, const std::string &icon,
                              std::filesystem::path &path, const char **error) {
    const std::filesystem::path relative(icon);
    if (relative.is_absolute()) {
        *error = "control icon must be relative to the mod directory";
        return false;
    }
    for (const auto &part : relative) {
        if (part == "..") {
            *error = "control icon path escapes mod directory";
            return false;
        }
    }

    // 统一走贴图解析：<Image>/ 优先，其次旧位置。
    const std::string resolved =
        lua_api::resolve_item_image(h->private_dir, h->mod_dir, icon);
    std::error_code ec;
    if (resolved.empty() || !std::filesystem::is_regular_file(resolved, ec)) {
        *error = "control icon not found (put it under Image/)";
        return false;
    }
    path = std::filesystem::canonical(resolved, ec);
    if (ec) {
        *error = "control icon file unavailable";
        return false;
    }

    const auto inside = [](const std::filesystem::path &root,
                           const std::filesystem::path &candidate) {
        if (root.empty()) return false;
        const auto child = candidate.lexically_relative(root);
        return !child.empty() && *child.begin() != "..";
    };
    std::error_code root_ec;
    if (!h->private_dir.empty()) {
        const auto root = std::filesystem::canonical(h->private_dir, root_ec);
        if (!root_ec && inside(root, path)) return true;
    }
    if (!h->mod_dir.empty()) {
        const auto root = std::filesystem::canonical(h->mod_dir, root_ec);
        if (!root_ec && inside(root, path)) return true;
    }
    *error = "control icon path escapes mod directory";
    return false;
}

/// mod.controls.register{ id=..., name=..., icon=..., show_label=... } -> id
///
/// icon 是 PNG 相对路径：优先在 `<mod>/Image/` 下查找（新约定），
/// 找不到时自动回退到旧位置（Mod 根目录 / Resources/Image 等），
/// 并且始终做 canonical + 越界检查。
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

    // 解码图标：`<mod>/Image/` 优先，旧位置自动回退，最终仍在 Mod 目录内。
    std::vector<unsigned char> pixels;
    terraria_button_image_t image{};
    if (!icon_path.empty()) {
        std::filesystem::path path;
        const char *icon_error = "control icon file unavailable";
        if (!resolve_icon_path(h, icon_path, path, &icon_error))
            return luaL_error(L, "%s", icon_error);

        if (!image::decode_png_file(path.string(), pixels, image.width, image.height))
            return luaL_error(L, "control icon PNG decode failed");
        if (image.width < 1 || image.height < 1 || image.width > 2048 || image.height > 2048)
            return luaL_error(L, "control icon dimensions must be 1-2048");
        LOG_INFO("[controls] {} icon -> {}", id, path.string());
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
