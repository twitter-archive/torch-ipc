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

#define MAX_ARG_SIZE (16*1024)

typedef struct map_thread_t {
   pthread_t thread;
   ringbuffer_t *rb;
   int ret;
} map_thread_t;

typedef struct map_t {
   map_thread_t *threads;
   uint32_t num_threads;
} map_t;

typedef int (*ThreadInitFunc) (lua_State *L);
ThreadInitFunc _ipc_static_init_thread = NULL;

static int rb_save_with_growth(lua_State *L, int index, struct ringbuffer_t *rb) {
   while (1) {
      ringbuffer_push_write_pos(rb);
      int startsize = lua_gettop(L);
      int ret = rb_save(L, index, rb, 0, 0); // map doesn't support upvalues
      if (ret == -ENOMEM) {
         int top = lua_gettop(L);
         if (top > startsize) {
            lua_pop(L, top-startsize);
         } else if (top < startsize) {
            LUA_HANDLE_ERROR_STR(L, "too many items popped during serialization");
         }
         ringbuffer_pop_write_pos(rb);
         ringbuffer_grow_by(rb, MAX_ARG_SIZE);
      } else {
         return ret;
      }
   }
}

static int start_thread(void* arg, map_thread_t **map_thread, lua_State **L, int *top, const char *name) {
#ifdef _OPENMP
   // prevent MKL/BLAS from crashing on the reader threads
   // its use of open-mp eats up way too many threads
   omp_set_num_threads(1);
#endif

   *map_thread = (map_thread_t *)arg;
   *L = luaL_newstate();
   if (_ipc_static_init_thread) {
      _ipc_static_init_thread(*L);
   } else {
      luaL_openlibs(*L);
   }
   // in order to deserialize arguments we need torch and libipc
   // TODO: detect these on the main thread when serializing arguments
   *top = lua_gettop(*L);
   if (luaL_loadstring(*L, "require 'torch'; require 'libipc'; pcall(function() require 'twutil' end)")) {
      lua_close(*L);
      return 0;
   }
   (*map_thread)->ret = lua_pcall(*L, 0, 0, 0);
   if ((*map_thread)->ret) {
      fprintf(stderr, "WARN1: ipc.%s thread pcall failed: %s\n", name, lua_tostring(*L, -1));
      return 0;
   } else {
      return 1;
   }
}

static void end_thread(map_thread_t *map_thread, lua_State *L, int top, const char *name) {
   int k = lua_gettop(L) - top;
   for (int i = 1; i <= k; i++) {
      int ret = rb_save_with_growth(L, top + i, map_thread->rb);
      if (ret) {
         fprintf(stderr, "WARN: ipc.%s thread failed to write results: %s\n", name, strerror(-ret));
         map_thread->ret = ret;
         break;
      }
   }
   lua_close(L);
}

static void core_thread(map_thread_t *map_thread, lua_State *L, const char *name) {
   int i = 0;
   while (ringbuffer_peek(map_thread->rb)) {
      int ret = rb_load(L, map_thread->rb);
      if (ret < 0) {
         LUA_HANDLE_ERROR_STR(L, "thread Lua data wasn't loaded correctly");
      };
      i++;
   }
   map_thread->ret = lua_pcall(L, i - 1, LUA_MULTRET, 0);
   if (map_thread->ret) {
      fprintf(stderr, "WARN2: ipc.%s thread pcall failed: %s\n", name, lua_tostring(L, -1));
   }
}

static void* thread_func(void *arg) {
   map_thread_t *map_thread;
   lua_State *L;
   int top;
   if (start_thread(arg, &map_thread, &L, &top, "map")) {
      top = lua_gettop(L);
      core_thread(map_thread, L, "map");
   }
   end_thread(map_thread, L, top, "map");
   return 0;
}

static int core_map(lua_State *L, void* (*func)(void*)) {
   uint32_t num_threads = lua_tonumber(L, 1);
   map_thread_t *threads = (map_thread_t *)calloc(num_threads, sizeof(map_thread_t));
   int k = lua_gettop(L);
   for (uint32_t i = 0; i < num_threads; i++) {
      threads[i].rb = ringbuffer_create(MAX_ARG_SIZE);
      for (int j = 2; j <= k; j++) { // save function and arguments
         int ret = rb_save_with_growth(L, j, threads[i].rb);
         if (ret) return LUA_HANDLE_ERROR(L, ret);
      }
      lua_pushinteger(L, i + 1); // mapid is the last argument (id of the thread)
      int ret = rb_save_with_growth(L, k + 1, threads[i].rb);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
      lua_pop(L, 1);
      ret = pthread_create(&threads[i].thread, NULL, func, &threads[i]);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
   }
   map_t *map = (map_t *)lua_newuserdata(L, sizeof(map_t));
   map->num_threads = num_threads;
   map->threads = threads;
   luaL_getmetatable(L, "ipc.map");
   lua_setmetatable(L, -2);
   return 1;
}

int map_open(lua_State *L) {
   if (lua_type(L, 2) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "map arg #2 expected a function");
   return core_map(L, thread_func);
}

static void* thread_extended_func(void *arg) {
   map_thread_t *map_thread;
   lua_State *L;
   int top;
   if (start_thread(arg, &map_thread, &L, &top, "map_extended")) {
      top = lua_gettop(L);
      rb_load(L, map_thread->rb);
      if (lua_type(L, top+1) == LUA_TSTRING) {
         size_t str_len;
         const char *str = lua_tolstring(L, top+1, &str_len);
         char *other_str = malloc(str_len+1);
         memcpy((void*)other_str, (void*)str, str_len);
         other_str[str_len] = 0;
         lua_pop(L, 1);
         luaL_loadstring(L, other_str);
      }
      if (lua_isnil(L, top+1)) {
         map_thread->ret = 0;
         lua_pop(L, 1);
      } else {
         map_thread->ret = lua_pcall(L, 0, 0, 0);
      }
      if (map_thread->ret) {
         fprintf(stderr, "WARN: ipc.map_extended thread pcall failed: %s\n", lua_tostring(L, -1));
      } else {
         core_thread(map_thread, L, "map_extended");
      }
   }
   end_thread(map_thread, L, top, "map_extended");
   return 0;
}

int map_extended_open(lua_State *L) {
   if (lua_type(L, 2) != LUA_TFUNCTION
         && lua_type(L, 2) != LUA_TSTRING
         && lua_type(L, 2) != LUA_TNIL)
      return LUA_HANDLE_ERROR_STR(L, "map_extended arg #2 expected a function, string or nil");
   if (lua_type(L, 3) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "map_extended arg #3 expected a function");
   return core_map(L, thread_extended_func);
}

int map_join(lua_State *L) {
   int rc = 0;
   int err_rc = -1;
   map_t *map = (map_t *)lua_touserdata(L, 1);
   for (uint32_t i = 0; i < map->num_threads; i++) {
      if (map->threads[i].rb) {
         int ret = pthread_join(map->threads[i].thread, NULL);
         if (ret) return LUA_HANDLE_ERROR(L, ret);
         if (map->threads[i].ret) {
            err_rc = rc;
         }
         while (ringbuffer_peek(map->threads[i].rb)) {
            rb_load(L, map->threads[i].rb);
            rc++;
         }
         ringbuffer_destroy(map->threads[i].rb);
      }
   }
   free(map->threads);
   map->threads = NULL;
   map->num_threads = 0;
   if (err_rc >= 0) {
      return LUA_HANDLE_ERROR_STR(L, lua_tostring(L, err_rc - rc));
   }
   return rc;
}

int map_check_errors(lua_State *L) {
   map_t *map = (map_t *)lua_touserdata(L, 1);
   for (uint32_t i = 0; i < map->num_threads; i++) {
      if (map->threads[i].ret) {
         pthread_join(map->threads[i].thread, NULL);
         while (ringbuffer_peek(map->threads[i].rb)) {
            rb_load(L, map->threads[i].rb);
         }
         ringbuffer_destroy(map->threads[i].rb);
         map->threads[i].rb = NULL;
         return LUA_HANDLE_ERROR_STR(L, lua_tostring(L, -1));
      }
   }
   return 0;
}
