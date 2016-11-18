#include "TH.h"
#include "luaT.h"
#include "ringbuffer.h"
#include "serialize.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <lualib.h>
#include "error.h"
#ifdef _OPENMP
#include <omp.h>
#endif
#include <unistd.h>
#include <errno.h>

#define BUFFER_SIZE (16*1024)

typedef struct sharedtable_t {
   struct ringbuffer_t* rb;
   pthread_mutex_t mutex;
   lua_State* L; // the shared table is stored in its own lua_State
   size_t size_increment;
   int ref_count;
} sharedtable_t;

static int rb_save_with_growth(lua_State *L, int index, struct ringbuffer_t *rb, size_t size) {
   while (1) {
      ringbuffer_push_write_pos(rb);
      int ret = rb_save(L, index, rb, 0, 0);
      if (ret == -ENOMEM) {
         ringbuffer_pop_write_pos(rb);
         ringbuffer_grow_by(rb, size);
      } else {
         return ret;
      }
   }
}

static int copy_entry_to_table(lua_State *L, sharedtable_t *table, int move) {
   int top = lua_gettop(L);
   int key_pos = top-1;
   int val_pos = top;
   int ret;

   ret = rb_save_with_growth(L, key_pos, table->rb, table->size_increment);
   if (ret) {
      lua_pop(L, 2);
      return ret;
   }
   ret = rb_load(table->L, table->rb);

   ret = rb_save_with_growth(L, val_pos, table->rb, table->size_increment);
   if (ret) {
      lua_pop(L, 2);
      return ret;
   }
   ret = rb_load(table->L, table->rb);

   lua_settable(table->L, 1);
   if (move) {
      lua_pushvalue(L, key_pos);
      lua_pushnil(L);
      lua_settable(L, 1);
   }

   return 0;
}

static int init_table(lua_State *L, sharedtable_t *table, int move) {
   lua_pushnil(L);
   while (lua_next(L, 1) != 0) {
      int ret = copy_entry_to_table(L, table, move);
      if (ret) return ret;
      lua_pop(L, 1);
   }
   return 0;
}

typedef int (*ThreadInitFunc) (lua_State *L);
extern ThreadInitFunc _ipc_static_init_thread;

int sharedtable_create(lua_State *L) {
   if (lua_gettop(L) > 0
         && lua_type(L, 1) != LUA_TTABLE
         && lua_type(L, 1) != LUA_TNIL)
      return LUA_HANDLE_ERROR_STR(L, "sharedtable arg #1 expected to be a table or nil");

   int move = lua_toboolean(L, 2);
   const char *requires = luaL_optlstring(L, 3, "", NULL);
   size_t size = luaL_optnumber(L, 4, BUFFER_SIZE);
   size_t size_increment = luaL_optnumber(L, 5, size);
   sharedtable_t *table = (sharedtable_t *)calloc(1, sizeof(sharedtable_t));
   table->L = luaL_newstate();
   if (_ipc_static_init_thread) {
      _ipc_static_init_thread(table->L);
   } else {
      luaL_openlibs(table->L);
   }

   if (luaL_loadstring(table->L, "require 'torch'; require 'libipc';")) {
      lua_close(table->L);
      free(table);
      return 0;
   }
   if (lua_pcall(table->L, 0, 0, 0)) {
      fprintf(stderr, "WARN: ipc.sharedtable initialization failed: %s\n", lua_tostring(table->L, -1));
      lua_close(table->L);
      free(table);
      return 0;
   }

   if (requires) {
      if (luaL_loadstring(table->L, requires)) {
         lua_close(table->L);
         free(table);
         return 0;
      }
      if (lua_pcall(table->L, 0, 0, 0)) {
         fprintf(stderr, "WARN: ipc.sharedtable initialization failed: %s\n", lua_tostring(table->L, -1));
         lua_close(table->L);
         free(table);
         return 0;
      }
   }

   table->rb = ringbuffer_create(size);
   lua_newtable(table->L);
   table->size_increment = size_increment;

   int ret = pthread_mutex_init(&table->mutex, NULL);
   if (ret) {
      ringbuffer_destroy(table->rb);
      lua_close(table->L);
      free(table);
      return LUA_HANDLE_ERROR(L, errno);
   }

   if (lua_gettop(L) > 0 && !lua_isnil(L, 1)) {
      ret = init_table(L, table, move);
      if (ret) {
         pthread_mutex_destroy(&table->mutex);
         ringbuffer_destroy(table->rb);
         lua_close(table->L);
         free(table);
         return LUA_HANDLE_ERROR(L, ret);
      }
   }

   sharedtable_t **ptable = lua_newuserdata(L, sizeof(sharedtable_t *));
   *ptable = table;
   table->ref_count = 1;
   luaL_getmetatable(L, "ipc.sharedtable");
   lua_setmetatable(L, -2);
   return 1;
}

int sharedtable_retain(lua_State *L) {
   lua_touserdata(L, 1);
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;
   THAtomicIncrementRef(&table->ref_count);
   return 0;
}

int sharedtable_gc(lua_State *L) {
   lua_touserdata(L, 1);
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;
   if (THAtomicDecrementRef(&table->ref_count)) {
      pthread_mutex_destroy(&table->mutex);
      ringbuffer_destroy(table->rb);
      lua_close(table->L);
      free(table);
   }
   return 0;
}

int sharedtable_read(lua_State *L) {
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;

   int ret = pthread_mutex_lock(&table->mutex);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }

   ret = rb_save_with_growth(L, 2, table->rb, table->size_increment);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(table->L, table->rb);
   lua_gettable(table->L, 1);

   ret = rb_save_with_growth(table->L, 2, table->rb, table->size_increment);
   if (ret) {
      lua_pop(table->L, 1);
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(L, table->rb);

   lua_pop(table->L, 1);
   ret = pthread_mutex_unlock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   return 1;
}

int sharedtable_write(lua_State *L) {
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;

   int ret = pthread_mutex_lock(&table->mutex);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }

   ret = rb_save_with_growth(L, 2, table->rb, table->size_increment);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(table->L, table->rb);

   ret = rb_save_with_growth(L, 3, table->rb, table->size_increment);
   if (ret) {
      lua_pop(table->L, 1);
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(table->L, table->rb);

   lua_settable(table->L, 1);

   ret = pthread_mutex_unlock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   return 0;
}

int sharedtable_len(lua_State *L) {
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;

   int ret = pthread_mutex_lock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   size_t counter = 0;
   lua_pushnil(table->L);
   while (lua_next(table->L, 1) != 0) {
      lua_pop(table->L, 1);
      counter++;
   }

   ret = pthread_mutex_unlock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   lua_pushinteger(L, counter);

   return 1;
}

static int sharedtable_next(lua_State *L) {
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;

   int ret = pthread_mutex_lock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   ret = rb_save_with_growth(L, 2, table->rb, table->size_increment);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(table->L, table->rb);
   ret = lua_next(table->L, 1);
   if (ret == 0) {
      lua_pushnil(L);
      ret = pthread_mutex_unlock(&table->mutex);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
      return 1;
   }

   ret = rb_save_with_growth(table->L, 2, table->rb, table->size_increment);
   if (ret) {
      lua_pop(table->L, 2);
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(L, table->rb);

   ret = rb_save_with_growth(table->L, 3, table->rb, table->size_increment);
   if (ret) {
      lua_pop(table->L, 2);
      lua_pop(L, 1);
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }
   ret = rb_load(L, table->rb);

   lua_pop(table->L, 2);
   ret = pthread_mutex_unlock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   return 2;
}

int sharedtable_pairs(lua_State *L) {
   lua_pushcfunction(L, sharedtable_next);
   lua_pushvalue(L, 1);
   lua_pushnil(L);
   return 3;
}

int sharedtable_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.sharedtable");
   return 1;
}

int sharedtable_size(lua_State *L) {
   sharedtable_t **ptable = (sharedtable_t **)lua_touserdata(L, 1);
   sharedtable_t *table = *ptable;

   int ret = pthread_mutex_lock(&table->mutex);
   if (ret) {
      pthread_mutex_unlock(&table->mutex);
      return LUA_HANDLE_ERROR(L, ret);
   }

   double count1 = lua_gc(table->L, LUA_GCCOUNT, 0);
   double count2 = lua_gc(table->L, LUA_GCCOUNTB, 0);
   double count = count1 + count2/1024;
   lua_pushnumber(L, count);

   ret = pthread_mutex_unlock(&table->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);

   return 1;
}
