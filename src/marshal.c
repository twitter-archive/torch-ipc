#include "TH.h"
#include "luaT.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "ringbuffer.h"
#include "serialize.h"
#include "error.h"

#define DEFAULT_MARSHAL_SIZE (1024*16)

typedef struct marshal_t {
   struct ringbuffer_t* rb;
   int refcount;
   size_t size_increment;
   int empty;
} marshal_t;

static int marshal_write(lua_State *L, int index, marshal_t *marshal, int upval) {
   while (1) {
      ringbuffer_push_write_pos(marshal->rb);
      int ret = rb_save(L, index, marshal->rb, 0, upval);
      if (ret == -ENOMEM) {
         ringbuffer_pop_write_pos(marshal->rb);
         ringbuffer_grow_by(marshal->rb, marshal->size_increment);
      } else if (ret) {
         ringbuffer_pop_write_pos(marshal->rb);
         return LUA_HANDLE_ERROR(L, -ret);
      } else {
         break;
      }
   }
   return 0;
}

int marshal_open(lua_State *L) {
   int upval = lua_toboolean(L, 2);
   size_t size = luaL_optnumber(L, 3, DEFAULT_MARSHAL_SIZE);
   size_t size_increment = luaL_optnumber(L, 4, size);
   marshal_t *marshal = (marshal_t *)calloc(1, sizeof(marshal_t));
   marshal->refcount = 1;
   marshal->size_increment = size_increment;
   marshal->rb = ringbuffer_create(size);
   marshal_t** ud = (marshal_t **)lua_newuserdata(L, sizeof(marshal_t*));
   *ud = marshal;
   luaL_getmetatable(L, "ipc.marshal");
   lua_setmetatable(L, -2);

   if (lua_isnil(L, 1) == 1) {
      LUA_HANDLE_ERROR_STR(L, "must provide object to serialize at arg 1");
   } else {
      marshal_write(L, 1, marshal, upval);
   }
   return 1;
}

int marshal_read(lua_State *L) {
   marshal_t *marshal = *(marshal_t **)lua_touserdata(L, 1);
   if (!marshal) return LUA_HANDLE_ERROR_STR(L, "marshal is not open");
   ringbuffer_t* rb = ringbuffer_clone(marshal->rb);
   int ret = rb_load(L, rb);
   free(rb);
   if (ret < 0) return LUA_HANDLE_ERROR(L, ret);
   return 1;
}

int marshal_close(lua_State *L) {
   marshal_t **ud = (marshal_t **)lua_touserdata(L, 1);
   marshal_t *marshal = *ud;
   if (!marshal) return LUA_HANDLE_ERROR_STR(L, "marshal is already closed");
   if (THAtomicDecrementRef(&marshal->refcount)) {
      ringbuffer_destroy(marshal->rb);
      free(marshal);
   }
   *ud = NULL;
   return 0;
}

int marshal_gc(lua_State *L) {
   marshal_t *marshal = *(marshal_t **)lua_touserdata(L, 1);
   if (marshal) {
      marshal_close(L);
   }
   return 0;
}

int marshal_retain(lua_State *L) {
   marshal_t *marshal = *(marshal_t **)lua_touserdata(L, 1);
   if (!marshal) return LUA_HANDLE_ERROR_STR(L, "marshal is not open");
   THAtomicIncrementRef(&marshal->refcount);
   return 0;
}

int marshal_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.marshal");
   return 1;
}
