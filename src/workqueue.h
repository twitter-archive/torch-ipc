#ifndef _WORKQUEUE_H_
#define _WORKQUEUE_H_

#include "luaT.h"

int workqueue_open(lua_State *L);
int workqueue_close(lua_State *L);
int workqueue_read(lua_State *L);
int workqueue_write(lua_State *L);
int workqueue_writeup(lua_State *L);
int workqueue_drain(lua_State *L);
int workqueue_gc(lua_State *L);
int workqueue_retain(lua_State *L);
int workqueue_metatablename(lua_State *L);

#endif
