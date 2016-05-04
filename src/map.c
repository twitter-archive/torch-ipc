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
      int ret = rb_save(L, index, rb, 0);
      if (ret == -ENOMEM) {
         ringbuffer_pop_write_pos(rb);
         ringbuffer_grow_by(rb, MAX_ARG_SIZE);
      } else {
         return ret;
      }
   }
}

static void* thread_func(void *arg) {
#ifdef _OPENMP
   // prevent MKL/BLAS from crashing on the reader threads
   // its use of open-mp eats up way too many threads
   omp_set_num_threads(1);
#endif
   map_thread_t *map_thread = (map_thread_t *)arg;
   lua_State *L = luaL_newstate();
   if (_ipc_static_init_thread) {
      _ipc_static_init_thread(L);
   } else {
      luaL_openlibs(L);
   }
   // in order to deserialize arguments we need torch and libipc
   // TODO: detect these on the main thread when serializing arguments
   int top = lua_gettop(L);
   if (luaL_loadstring(L, "require 'torch'; require 'libipc'; pcall(require, 'twutil')")) {
      lua_close(L);
      return NULL;
   }
   map_thread->ret = lua_pcall(L, 0, 0, 0);
   if (map_thread->ret) {
      fprintf(stderr, "WARN: ipc.map thread pcall failed: %s\n", lua_tostring(L, -1));
   } else {
      top = lua_gettop(L);
      int i = 0;
      while (ringbuffer_peek(map_thread->rb)) {
         rb_load(L, map_thread->rb);
         i++;
      }
      map_thread->ret = lua_pcall(L, i - 1, LUA_MULTRET, 0);
      if (map_thread->ret) {
         fprintf(stderr, "WARN: ipc.map thread pcall failed: %s\n", lua_tostring(L, -1));
      }
   }
   int k = lua_gettop(L) - top;
   for (int i = 1; i <= k; i++) {
      int ret = rb_save_with_growth(L, top + i, map_thread->rb);
      if (ret) {
         fprintf(stderr, "WARN: ipc.map thread failed to write results: %s\n", strerror(-ret));
         map_thread->ret = ret;
         break;
      }
   }
   lua_close(L);
   return 0;
}

int map_open(lua_State *L) {
   uint32_t num_threads = lua_tonumber(L, 1);
   if (lua_type(L, 2) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "map arg #2 expected a function");
   map_thread_t *threads = (map_thread_t *)calloc(num_threads, sizeof(map_thread_t));
   int k = lua_gettop(L);
   for (uint32_t i = 0; i < num_threads; i++) {
      threads[i].rb = ringbuffer_create(MAX_ARG_SIZE);
      for (int j = 2; j <= k; j++) {
         int ret = rb_save_with_growth(L, j, threads[i].rb);
         if (ret) return LUA_HANDLE_ERROR(L, ret);
      }
      lua_pushinteger(L, i + 1);
      int ret = rb_save_with_growth(L, k + 1, threads[i].rb);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
      lua_pop(L, 1);
      ret = pthread_create(&threads[i].thread, NULL, thread_func, &threads[i]);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
   }
   map_t *map = (map_t *)lua_newuserdata(L, sizeof(map_t));
   map->num_threads = num_threads;
   map->threads = threads;
   luaL_getmetatable(L, "ipc.map");
   lua_setmetatable(L, -2);
   return 1;
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
