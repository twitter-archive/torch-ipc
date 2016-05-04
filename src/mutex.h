#ifndef _MUTEX_H_
#define _MUTEX_H_

#include "luaT.h"

int mutex_create(lua_State *L);
int mutex_lock(lua_State *L);
int mutex_unlock(lua_State *L);
int mutex_barrier(lua_State *L);
int mutex_retain(lua_State *L);
int mutex_metatablename(lua_State *L);
int mutex_gc(lua_State *L);

#endif
