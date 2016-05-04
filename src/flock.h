#ifndef _FLOCK_H_
#define _FLOCK_H_

#include "luaT.h"

int flock_open(lua_State *L);
int flock_close(lua_State *L);
int flock_read(lua_State *L);
int flock_write(lua_State *L);
int flock_truncate(lua_State *L);

#endif
