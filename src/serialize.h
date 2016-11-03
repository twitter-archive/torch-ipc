#ifndef _SERIALIZE_H_
#define _SERIALIZE_H_

#include "luaT.h"
#include "ringbuffer.h"

int rb_load(lua_State *L, struct ringbuffer_t *rb);
int rb_save(lua_State *L, int index, struct ringbuffer_t *rb, int oop, int upval);

#endif
