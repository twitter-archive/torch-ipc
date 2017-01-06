#ifndef _MARSHAL_H_
#define _MARSHAL_H_

#include "luaT.h"
#include "ringbuffer.h"

int marshal_open(lua_State *L);
int marshal_close(lua_State *L);
int marshal_read(lua_State *L);
int marshal_gc(lua_State *L);
int marshal_retain(lua_State *L);
int marshal_metatablename(lua_State *L);

#endif
