#ifndef LUA_HOOK_H
#define LUA_HOOK_H

#include <cstdint>

void hook_lua_functions(void* tolua_handle);

// 全局 Lua State（供 bridge 调用）
extern void* g_lua_state;
extern bool g_lua_ready;

#endif