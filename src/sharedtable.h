#ifndef _SHAREDTABLE_H_
#define _SHAREDTABLE_H_

#include "luaT.h"

int sharedtable_create(lua_State *L);
int sharedtable_retain(lua_State *L);
int sharedtable_gc(lua_State *L);
int sharedtable_read(lua_State *L);
int sharedtable_write(lua_State *L);
int sharedtable_len(lua_State *L);
int sharedtable_pairs(lua_State *L);
int sharedtable_metatablename(lua_State *L);
int sharedtable_size(lua_State *L);

#endif
