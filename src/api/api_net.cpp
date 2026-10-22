#include "api_internal.hpp"

namespace lualoader::lua_api {

    // ---------------- mod.net：联机同步 ----------------

    /// mod.net.mode() -> 0=单机 1=客户端 2=服务端
    static int l_net_mode(lua_State *L) {
        static patch_handle_t f_net_mode = PATCH_NULL;
        static bool tried = false;
        if (!tried) {
            tried = true;
            const patch_handle_t cls = patchlib_type_get_type("Terraria", "Main");
            if (patchlib_is_valid(cls)) {
                f_net_mode = patchlib_type_get_field(cls, "netMode");
                patchlib_free(cls);
            }
        }
        int mode = 0;
        if (patchlib_is_valid(f_net_mode))
            patchlib_field_get_value(f_net_mode, PATCH_NULL, &mode);
        lua_pushinteger(L, mode);
        return 1;
    }

    /// mod.net.tile_kill(x, y[, no_item])
    /// 客户端请求服务端挖掉一个图格（服务端权威执行并广播）。
    static int l_net_tile_kill(lua_State *L) {
        const int x = static_cast<int>(luaL_checkinteger(L, 1));
        const int y = static_cast<int>(luaL_checkinteger(L, 2));
        const bool no_item = lua_toboolean(L, 3) != 0;
        terraria_net_send_tile_action(x, y, no_item ? 4 : 0);
        return 0;
    }

    /// mod.net.tile_square(x, y, width, height)
    static int l_net_tile_square(lua_State *L) {
        const int x = static_cast<int>(luaL_checkinteger(L, 1));
        const int y = static_cast<int>(luaL_checkinteger(L, 2));
        const int w = static_cast<int>(luaL_checkinteger(L, 3));
        const int h = static_cast<int>(luaL_checkinteger(L, 4));
        terraria_net_send_tile_square(x, y, w, h);
        return 0;
    }

    /// mod.net.send_to_server(channel, payload)
    /// 客户端 -> 服务端；服务端收到后调用该 Mod 的 on_receive 回调。
    static int l_net_send_to_server(lua_State *L) {
        lua_mod_handle_t *h = get_handle(L);
        if (!h) return luaL_error(L, "mod handle missing");
        const char *channel = luaL_checkstring(L, 1);
        size_t len = 0;
        const char *payload = luaL_optlstring(L, 2, "", &len);
        terraria_net_send_mod_message(h->mod_id.c_str(), channel, payload,
                                      static_cast<int>(len));
        return 0;
    }

    /// mod.net.on_receive(function(channel, payload, player) end)
    /// 只在服务端（房主/专用服）触发。
    static int l_net_on_receive(lua_State *L) {
        lua_mod_handle_t *h = get_handle(L);
        if (!h) return luaL_error(L, "mod handle missing");
        if (!lua_isfunction(L, 1))
            return luaL_error(L, "on_receive 需要一个函数");
        if (h->net_recv_ref != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, h->net_recv_ref);
        h->net_recv_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;
    }

    static const luaL_Reg mod_net_functions[] = {
            {"mode",           l_net_mode},
            {"send_to_server", l_net_send_to_server},
            {"on_receive",     l_net_on_receive},
            {"tile_kill",   l_net_tile_kill},
            {"tile_square", l_net_tile_square},
            {nullptr,       nullptr},
    };


void add_net(lua_State *L) {
    luaL_setfuncs(L, mod_net_functions, 0);
}

} // namespace lualoader::lua_api
