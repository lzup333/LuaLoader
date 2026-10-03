/*******************************************************************************
 * LuaLoader - mod_api
 * Copyright (C) 2026 lzup333
 *******************************************************************************/

#pragma once

#include <cstddef>

struct ll_api_t;

namespace lualoader::mod_api {

    /// 提供给原生模块的 API 表（进程内单例）
    const ll_api_t *get_api();

    /// 已解析的自身导出符号数量（诊断用）
    size_t symbol_count();

} // namespace lualoader::mod_api
