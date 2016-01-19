#ifndef _ERROR_H_
#define _ERROR_H_

#include "luaT.h"

int _parallel_log_error(int ret, const char* file, int line);
int _parallel_lua_error(lua_State *L, int ret, const char* file, int line);
int _parallel_lua_error_str(lua_State *L, const char *str, const char* file, int line);

#define HANDLE_ERROR(ret) _parallel_log_error(ret, __FILE__, __LINE__)
#define LUA_HANDLE_ERROR(L, ret) _parallel_lua_error(L, ret, __FILE__, __LINE__)
#define LUA_HANDLE_ERROR_STR(L, str) _parallel_lua_error_str(L, str, __FILE__, __LINE__)

double _parallel_seconds();

#endif
