#include "luaT.h"
#include "ringbuffer.h"
#include "serialize.h"
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <lualib.h>
#include "error.h"

#define MAX_ARG_SIZE (16*1024)

typedef struct map_thread_t {
   pthread_t thread;
   struct ringbuffer_t *rb;
   int ret;
} map_thread_t;

typedef struct map_t {
   struct map_thread_t *threads;
   uint32_t num_threads;
} map_t;

#ifdef __APPLE__
/*
  Super lame, but with a low ulimit on files spawning a 100s
  of threads will crash in the require system with too many
  open files. So we only allow a single thread to be in the
  require system at any given time.
*/
static pthread_once_t _safe_require_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t _safe_require_mutex;

static void _safe_require_one_time_init_inner() {
   pthread_mutexattr_t mutex_attr;

   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&_safe_require_mutex, &mutex_attr);
}

static void _safe_require_one_time_init() {
   pthread_once(&_safe_require_once, _safe_require_one_time_init_inner);
}

static int _safe_require(lua_State *L) {
   int n, i;

   pthread_mutex_lock(&_safe_require_mutex);
   n = lua_gettop(L);
   lua_getglobal(L, "_old_require");
   for (i = 1; i <= n; i++) {
      lua_pushvalue(L, i);
   }
   i = lua_pcall(L, n, LUA_MULTRET, 0);
   pthread_mutex_unlock(&_safe_require_mutex);
   if (i) {
      return luaL_error(L, lua_tostring(L, 1));
   } else {
      return lua_gettop(L) - n;
   }
}

static void _replace_require(lua_State *L) {
   lua_getglobal(L, "require");
   lua_setglobal(L, "_old_require");
   lua_register(L, "require", _safe_require);
}
#endif

typedef int (*ThreadInitFunc) (lua_State *L);
ThreadInitFunc _parallel_static_init_thread = NULL;

static void* thread_func(void *arg) {
   struct map_thread_t *map_thread;
   lua_State *L;
   int i, k, top;

   map_thread = (struct map_thread_t *)arg;
   L = luaL_newstate();
   luaL_openlibs(L);
#ifdef STATIC_TH
   if (_parallel_static_init_thread) {
      _parallel_static_init_thread(L);
   }
#else
#ifdef __APPLE__
   _replace_require(L);
#endif
   // in order to deserialize arguments we need torch and libparallel
   if (luaL_loadstring(L, "require 'torch'; require 'libparallel'")) {
      lua_close(L);
      return NULL;
   }
   if (lua_pcall(L, 0, 0, 0)) {
      lua_close(L);
      return NULL;
   }
#endif
   top = lua_gettop(L);
   i = 0;
   while (ringbuffer_peek(map_thread->rb)) {
      rb_load(L, map_thread->rb);
      i++;
   }
   map_thread->ret = lua_pcall(L, i - 1, LUA_MULTRET, 0);
   k = lua_gettop(L) - top;
   for (i = 1; i <= k; i++) {
      rb_save(L, top + i, map_thread->rb, 0);
   }
   lua_close(L);
   return 0;
}

int map_open(lua_State *L) {
   uint32_t i, num_threads;
   struct map_thread_t *threads;
   struct map_t *map;
   int j, k;

#ifdef __APPLE__
   _safe_require_one_time_init();
#endif

   num_threads = lua_tonumber(L, 1);
   if (lua_type(L, 2) != LUA_TFUNCTION) return LUA_HANDLE_ERROR_STR(L, "map arg #2 expected a function");
   threads = (struct map_thread_t *)calloc(num_threads, sizeof(struct map_thread_t));
   k = lua_gettop(L);
   for (i = 0; i < num_threads; i++) {
      threads[i].rb = ringbuffer_create(MAX_ARG_SIZE);
      for (j = 2; j <= k; j++) {
         rb_save(L, j, threads[i].rb, 0);
      }
      lua_pushinteger(L, i + 1);
      rb_save(L, k + 1, threads[i].rb, 0);
      lua_pop(L, 1);
      pthread_create(&threads[i].thread, NULL, thread_func, threads + i);
   }
   map = (struct map_t *)lua_newuserdata(L, sizeof(map_t));
   map->num_threads = num_threads;
   map->threads = threads;
   luaL_getmetatable(L, "parallel.map");
   lua_setmetatable(L, -2);
   return 1;
}

int map_join(lua_State *L) {
   struct map_t *map;
   uint32_t i;
   int rc;
   int err_rc;

   rc = 0;
   err_rc = -1;
   map = (struct map_t *)lua_touserdata(L, 1);
   for (i = 0; i < map->num_threads; i++) {
      if (map->threads[i].rb) {
         pthread_join(map->threads[i].thread, NULL);
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
   struct map_t *map;
   uint32_t i;

   map = (struct map_t *)lua_touserdata(L, 1);
   for (i = 0; i < map->num_threads; i++) {
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
