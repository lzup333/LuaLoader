/*
 * LuaLoader 原生模块示例（C）—— 通过 ll_set_api 注入的 API 表工作
 *
 * 为什么不用 <lua.h> 直接调 lua_*？
 *   Android 上 loader 由内核以 RTLD_LOCAL 从 memfd 加载，模块的 lua_* 未定义符号
 *   无法在 ELF 全局作用域解析。统一改为从 ll_api_t.lookup() 取函数指针
 *   （与 TEFKernel 给 C mod 传 TPF 符号表同理）。
 *
 * 编译：见 build.sh
 */
#include "lualoader_mod.h"

#include <stddef.h>

/* 仅用于拿 lua_State*；不需要完整的 lua.h */
typedef struct lua_State lua_State;

static const ll_api_t *LL = NULL;

/* loader 在 dlopen 后调用，注入 API 表 */
LL_EXPORT void ll_set_api(const ll_api_t *api) {
    LL = api;
}

static int l_greet(lua_State *L) {
    if (!LL) return 0;
    LL_CACHE(p_tolstring, lua_tolstring, const char *, (lua_State *, int, size_t *));
    LL_CACHE(p_pushfstring, lua_pushfstring, const char *, (lua_State *, const char *, ...));
    if (!p_tolstring || !p_pushfstring) return 0;

    size_t len = 0;
    const char *name = p_tolstring(L, 1, &len);
    if (!name) name = "world";
    p_pushfstring(L, "hello, %s", name);
    return 1;
}

int luaopen_nativehello(lua_State *L) {
    if (!LL) return 0; /* loader 未注入 API 表（旧版 loader）*/
    LL_CACHE(p_createtable, lua_createtable, void, (lua_State *, int, int));
    LL_CACHE(p_pushcfunction, lua_pushcfunction, void, (lua_State *, int (*)(lua_State *)));
    LL_CACHE(p_setfield, lua_setfield, void, (lua_State *, int, const char *));
    if (!p_createtable || !p_pushcfunction || !p_setfield) return 0;

    p_createtable(L, 0, 1);
    p_pushcfunction(L, l_greet);
    p_setfield(L, -2, "greet");
    return 1;
}
