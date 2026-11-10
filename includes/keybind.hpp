#pragma once

#include "lua_engine.hpp"

namespace lualoader::keybind {

/// 安装 PC 原生键位注入（内核 keybind_manager）。
void install();

/// 卸载：移除所有 Mod 绑定并关闭内核键位管理器。
void shutdown();

/// 向 Lua 注入 mod.controls.*（PC 版本：键盘按键，注册进游戏原生按键菜单）。
void register_api(lua_State *L, lua_mod_handle_t *owner);

/// Mod 卸载时注销其全部按键绑定与 Lua 回调引用。
void unregister_owner(lua_mod_handle_t *owner);

} // namespace lualoader::keybind
