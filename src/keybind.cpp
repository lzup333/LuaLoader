#if !defined(__ANDROID__)

/**
 * PC 原生键位桥接。
 *
 * 把 Lua 侧 mod.controls.* 映射到内核 keybind_manager：
 *   - 注册项会进入 Terraria 原生「设置 -> 控制」菜单，玩家可直接改键；
 *   - on_press / on_release 由内核在 Main.Update 后置回调；
 *   - down / pressed / released 每帧可由 Lua 轮询。
 */

#include "keybind.hpp"
#include "logger.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "tefkernel/terraria/content/keybind_manager.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace lualoader::keybind {
namespace {

constexpr const char *loader_id = "lzup333.lualoader";

struct Binding {
    lua_State *L{nullptr};
    lua_mod_handle_t *owner{nullptr};
    std::string mod_id;
    std::string id;
    int on_press_ref{LUA_NOREF};
    int on_release_ref{LUA_NOREF};
};

std::vector<std::unique_ptr<Binding>> g_bindings;

terraria_keybind_scope_t scope_of(lua_mod_handle_t *h) {
    return {loader_id, h ? h->mod_id.c_str() : ""};
}

lua_mod_handle_t *owner(lua_State *L) {
    return static_cast<lua_mod_handle_t *>(lua_touserdata(L, lua_upvalueindex(1)));
}

Binding *find_binding(lua_mod_handle_t *h, const char *id) {
    if (!h || !id)
        return nullptr;
    for (auto &binding : g_bindings) {
        if (binding->owner == h && binding->id == id)
            return binding.get();
    }
    return nullptr;
}

/// 调用注册表里的 Lua 回调；参数为 keybind id。
void invoke_callback(Binding *binding, int ref) {
    if (!binding || !binding->owner || binding->owner->L != binding->L || ref == LUA_NOREF)
        return;

    lua_State *L = binding->L;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushstring(L, binding->id.c_str());
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        LOG_ERROR("[keybind] {} callback failed: {}", binding->id, err ? err : "?");
        lua_pop(L, 1);
    }
}

void on_press_trampoline(const terraria_keybind_scope_t *scope, const char *id, void *user) {
    (void) scope;
    (void) id;
    auto *binding = static_cast<Binding *>(user);
    invoke_callback(binding, binding->on_press_ref);
}

void on_release_trampoline(const terraria_keybind_scope_t *scope, const char *id, void *user) {
    (void) scope;
    (void) id;
    auto *binding = static_cast<Binding *>(user);
    invoke_callback(binding, binding->on_release_ref);
}

void release_binding(Binding *binding) {
    if (!binding)
        return;
    if (binding->L) {
        if (binding->on_press_ref != LUA_NOREF)
            luaL_unref(binding->L, LUA_REGISTRYINDEX, binding->on_press_ref);
        if (binding->on_release_ref != LUA_NOREF)
            luaL_unref(binding->L, LUA_REGISTRYINDEX, binding->on_release_ref);
    }
    binding->on_press_ref = LUA_NOREF;
    binding->on_release_ref = LUA_NOREF;
}

bool linked() {
    return terraria_keybind_manager_available && terraria_keybind_manager_error &&
           terraria_keybind_manager_install && terraria_keybind_manager_shutdown &&
           terraria_keybind_manager_register && terraria_keybind_manager_unregister &&
           terraria_keybind_manager_unregister_owner && terraria_keybind_manager_down &&
           terraria_keybind_manager_pressed && terraria_keybind_manager_released &&
           terraria_keybind_manager_set_enabled;
}

const char *error_text() {
    return linked() ? terraria_keybind_manager_error() : "kernel keybind_manager unavailable";
}


/// mod.controls.available() -> bool, err
int available(lua_State *L) {
    lua_pushboolean(L, linked() && terraria_keybind_manager_available());
    lua_pushstring(L, error_text());
    return 2;
}

/// mod.controls.register{ id=, name=, key=, on_press=, on_release=, enabled= } -> id
int add(lua_State *L) {
    if (!linked() || !terraria_keybind_manager_available())
        return luaL_error(L, "PC keybinds unavailable: %s", error_text());

    luaL_checktype(L, 1, LUA_TTABLE);
    auto *h = owner(L);

    lua_getfield(L, 1, "id");
    std::string id = luaL_checkstring(L, -1);
    lua_pop(L, 1);
    if (id.empty() || id.size() > 64 ||
        id.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-") !=
            std::string::npos)
        return luaL_error(L, "control id must be 1-64 ASCII letters, digits, _, - or .");

    lua_getfield(L, 1, "name");
    std::string name;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TSTRING);
        name = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    if (name.empty())
        name = id;
    if (name.size() > 128)
        return luaL_error(L, "control name must be at most 128 UTF-8 bytes");

    lua_getfield(L, 1, "key");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, 1, "default_key");
    }
    std::string key;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TSTRING);
        key = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "enabled");
    const bool enabled = lua_isnil(L, -1) ? true : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    int on_press_ref = LUA_NOREF;
    int on_release_ref = LUA_NOREF;
    lua_getfield(L, 1, "on_press");
    if (lua_isfunction(L, -1))
        on_press_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);
    lua_getfield(L, 1, "on_release");
    if (lua_isfunction(L, -1))
        on_release_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    else
        lua_pop(L, 1);

    std::unique_ptr<Binding> storage;
    Binding *binding = find_binding(h, id.c_str());
    if (binding) {
        /* 同一 Mod 重复注册同一 id：复用绑定对象，更新回调引用。 */
        release_binding(binding);
    } else {
        storage = std::make_unique<Binding>();
        storage->L = L;
        storage->owner = h;
        storage->mod_id = h ? h->mod_id : "";
        storage->id = id;
        binding = storage.get();
    }
    binding->L = L;
    binding->owner = h;
    binding->mod_id = h ? h->mod_id : "";
    binding->id = id;
    binding->on_press_ref = on_press_ref;
    binding->on_release_ref = on_release_ref;

    terraria_keybind_desc_t desc{};
    desc.scope = scope_of(h);
    desc.id = id.c_str();
    desc.name = name.c_str();
    desc.default_key = key.empty() ? nullptr : key.c_str();
    desc.on_press = on_press_ref != LUA_NOREF ? on_press_trampoline : nullptr;
    desc.on_release = on_release_ref != LUA_NOREF ? on_release_trampoline : nullptr;
    desc.user = binding;
    desc.enabled = enabled;

    if (!terraria_keybind_manager_register(&desc)) {
        release_binding(binding);
        return luaL_error(L, "failed to register keybind: %s", error_text());
    }

    if (storage)
        g_bindings.push_back(std::move(storage));
    lua_pushstring(L, id.c_str());
    return 1;
}

/// mod.controls.unregister(id) -> bool
int remove(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    for (auto it = g_bindings.begin(); it != g_bindings.end(); ++it) {
        Binding *binding = it->get();
        if (binding->owner != h || binding->id != id)
            continue;
        auto scope = scope_of(h);
        const bool ok = linked() && terraria_keybind_manager_unregister(&scope, id);
        release_binding(binding);
        g_bindings.erase(it);
        lua_pushboolean(L, ok || !linked());
        return 1;
    }
    lua_pushboolean(L, false);
    return 1;
}

/// mod.controls.down(id) -> bool
int down(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    auto scope = scope_of(h);
    lua_pushboolean(L, linked() && terraria_keybind_manager_down(&scope, id));
    return 1;
}

/// mod.controls.pressed(id) -> bool
int pressed(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    auto scope = scope_of(h);
    lua_pushboolean(L, linked() && terraria_keybind_manager_pressed(&scope, id));
    return 1;
}

/// mod.controls.released(id) -> bool
int released(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    auto scope = scope_of(h);
    lua_pushboolean(L, linked() && terraria_keybind_manager_released(&scope, id));
    return 1;
}

/// mod.controls.set_enabled(id, bool)
int enable(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    const bool value = lua_toboolean(L, 2) != 0;
    auto scope = scope_of(h);
    if (!linked() || !terraria_keybind_manager_set_enabled(&scope, id, value))
        return luaL_error(L, "control not registered");
    return 0;
}

/// mod.controls.set_visible(id, bool)：PC 没有可视化控件，保留接口供跨平台脚本使用。
int show(lua_State *L) {
    auto *h = owner(L);
    const char *id = luaL_checkstring(L, 1);
    (void) lua_toboolean(L, 2);
    if (!find_binding(h, id))
        return luaL_error(L, "control not registered");
    return 0;
}

} // namespace

void install() {
    if (!linked()) {
        LOG_ERROR("[keybind] kernel keybind_manager exports missing; update the desktop kernel");
        return;
    }
    terraria_keybind_manager_install();
    if (!terraria_keybind_manager_available())
        LOG_ERROR("[keybind] kernel initialization failed: {}", error_text());
    else
        LOG_INFO("[keybind] PC native keybinds ready");
}

void shutdown() {
    /* 先让内核把当前绑定写盘（此时注册项还完整），再释放 Lua 引用。
     * 不要先 unregister：那会把条目清空，导致保存出空文件。 */
    if (linked())
        terraria_keybind_manager_shutdown();

    for (auto &binding : g_bindings)
        release_binding(binding.get());
    g_bindings.clear();
}

void unregister_owner(lua_mod_handle_t *h) {
    if (!h)
        return;
    auto scope = scope_of(h);
    if (linked())
        terraria_keybind_manager_unregister_owner(&scope);

    for (auto it = g_bindings.begin(); it != g_bindings.end();) {
        if ((*it)->owner != h) {
            ++it;
            continue;
        }
        release_binding(it->get());
        it = g_bindings.erase(it);
    }
}

void register_api(lua_State *L, lua_mod_handle_t *h) {
    lua_newtable(L);
    const luaL_Reg funcs[] = {
        {"available", available}, {"register", add},    {"unregister", remove},
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

} // namespace lualoader::keybind

#endif /* !__ANDROID__ */

