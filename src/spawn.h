#ifndef _SPAWN_H_
#define _SPAWN_H_

#include "luaT.h"

int spawn_open(lua_State *L);
int spawn_wait(lua_State *L);
int spawn_close(lua_State *L);
int spawn_gc(lua_State *L);
int spawn_stdin(lua_State *L);
int spawn_stdout(lua_State *L);
int spawn_stderr(lua_State *L);

#endif
