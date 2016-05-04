#ifndef _SPAWN_H_
#define _SPAWN_H_

#include "luaT.h"

int spawn_open(lua_State *L);
int spawn_wait(lua_State *L);
int spawn_stdin(lua_State *L);
int spawn_stdout(lua_State *L);
int spawn_stdout_file_id(lua_State *L);
int spawn_pid(lua_State *L);
int spawn_running(lua_State *L);
int spawn_gc(lua_State *L);

#endif
