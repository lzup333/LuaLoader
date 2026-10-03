/*******************************************************************************
 * LuaLoader - 原生模块 C API（lualoader_mod.h）
 * Copyright (C) 2026 lzup333
 *
 * 用法（推荐）：
 *   1) 模块导出一个可选入口：
 *        #include "lualoader_mod.h"
 *        static const ll_api_t *LL = NULL;
 *        LL_EXPORT void ll_set_api(const ll_api_t *api) { LL = api; }
 *   2) 用 LL->lookup("符号名") 取 Lua/内核 API（本头文件提供常用便捷宏）
 *
 * 说明：Android 上 loader 由内核以 RTLD_LOCAL 从 memfd 加载，模块无法通过 ELF 全局作用域
 * 解析 lua_* / patchlib_*，因此统一走本表注入（与 TEFKernel 给 C mod 传 TPF 符号表同理）。
 *******************************************************************************/

#ifndef LUALOADER_MOD_H
#define LUALOADER_MOD_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define LL_EXPORT __declspec(dllexport)
#else
#define LL_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ll_api_t {
    uint32_t version; /* 当前为 1 */
    uint32_t size;    /* sizeof(ll_api_t)，用于向后兼容追加字段 */
    /* 按名字解析符号（lua_* / luaL_* / patchlib_* 等，loader 自身导出的都能拿到）；
     * 返回 NULL 表示不存在。 */
    void *(*lookup)(const char *name);
} ll_api_t;

/* 便捷宏：把名字解析成指定函数指针类型
 *   static lua_State *(*p_lua_pushstring)(lua_State*, const char*);
 *   p_lua_pushstring = LL_FN(LL, lua_pushstring, lua_State* (*)(lua_State*, const char*));
 */
#define LL_FN(api, name, type) ((type) ((api)->lookup(#name)))

/* 便捷宏：声明并缓存一个函数指针（按需解析一次）
 *   LL_CACHE(变量, 符号名, 返回类型, (参数类型...));
 * 例：LL_CACHE(p_tolstring, lua_tolstring, const char*, (lua_State*, int, size_t*));
 */
#define LL_CACHE(var, sym, ret, args)                  \
    static ret (*var) args = NULL;                     \
    if (!(var)) (var) = (ret (*) args) LL->lookup(#sym)

#ifdef __cplusplus
}
#endif

#endif /* LUALOADER_MOD_H */
