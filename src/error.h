#ifndef _ERROR_H_
#define _ERROR_H_

#include "luaT.h"
#include <string.h>

static inline int _lua_error(lua_State *L, int ret, const char* file, int line) {
   int pos_ret = ret >= 0 ? ret : -ret;
   return luaL_error(L, "ERROR: (%s, %d): (%d, %s)\n", file, line, pos_ret, strerror(pos_ret));
}

static inline int _lua_error_str(lua_State *L, const char *str, const char* file, int line) {
   return luaL_error(L, "ERROR: (%s, %d): (%s)\n", file, line, str);
}

#define LUA_HANDLE_ERROR(L, ret) _lua_error(L, ret, __FILE__, __LINE__)
#define LUA_HANDLE_ERROR_STR(L, str) _lua_error_str(L, str, __FILE__, __LINE__)

#endif
