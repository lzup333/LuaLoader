/*
 * LuaLoader 原生模块示例（C）
 *
 * 编译：见 build.sh
 * 说明：
 *   - lua_* / luaL_* 由 LuaLoader 进程导出，链接时保持未定义即可
 *   - patchlib_* 同样是 LuaLoader 导出的函数指针变量，可通过内核头文件引用
 *     （本示例只依赖 Lua API，保证任何内核版本都能编译）
 */
#include <lua.h>
#include <lauxlib.h>

/* 示例：nativehello.greet(name) -> "hello, <name>" */
static int l_greet(lua_State *L) {
    const char *name = luaL_optstring(L, 1, "world");
    lua_pushfstring(L, "hello, %s", name);
    return 1;
}

static const luaL_Reg nativehello_funcs[] = {
    {"greet", l_greet},
    {NULL, NULL},
};

int luaopen_nativehello(lua_State *L) {
    luaL_newlib(L, nativehello_funcs);
    return 1;
}
