#ifndef _MAP_H_
#define _MAP_H_

#include "luaT.h"

int map_open(lua_State *L);
int map_extended_open(lua_State *L);
int map_join(lua_State *L);
int map_check_errors(lua_State *L);

#endif
