/*******************************************************************************
 * LuaLoader - gui (Dear ImGui 内置 GUI)
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

#pragma once

#include "lua_engine.hpp"

namespace lualoader::gui {

    /// 是否已成功安装（成功 hook 了渲染后端）
    bool installed();

    /// 安装 GUI：hook 游戏渲染后端的出帧函数，并在每帧渲染 ImGui 界面。
    /// 平台/后端不可用时静默失败（只记日志）。
    void install();

    /// 反安装（还原 hook、销毁 ImGui 上下文）
    void shutdown();

    /// 向 Lua 注入 mod.gui（ImGui 即时模式 API）
    void register_api(lua_State *L, lua_mod_handle_t *handle);

} // namespace lualoader::gui
