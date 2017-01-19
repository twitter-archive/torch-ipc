#ifndef _CHANNEL_H_
#define _CHANNEL_H_

#include "luaT.h"

#define STATUS_OPEN 0
#define STATUS_CLOSED 1
#define STATUS_DRAINED 2

int channel_create(lua_State *L);
int channel_close(lua_State *L);
int channel_closed(lua_State *L);
int channel_drained(lua_State *L);
int channel_read(lua_State *L);
int channel_write(lua_State *L);
int channel_num_items(lua_State *L);
int channel_gc(lua_State *L);
int channel_retain(lua_State *L);
int channel_metatablename(lua_State *L);

#endif
