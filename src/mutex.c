#include "mutex.h"
#include "error.h"
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include "TH.h"
#include "luaT.h"

typedef struct mutex_t {
   int ref_count;
   pthread_mutex_t mutex;
   pthread_cond_t cond;
   int64_t barrier;
} mutex_t;

int mutex_create(lua_State *L) {
   mutex_t *mutex = calloc(1, sizeof(mutex_t));
   pthread_mutexattr_t mutex_attr;
   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   int ret = pthread_mutex_init(&mutex->mutex, &mutex_attr);
   if (ret) {
      free(mutex);
      return LUA_HANDLE_ERROR(L, errno);
   }
   ret = pthread_cond_init(&mutex->cond, NULL);
   if (ret) {
      pthread_mutex_destroy(&mutex->mutex);
      free(mutex);
      return LUA_HANDLE_ERROR(L, errno);
   }
   mutex_t **umutex = lua_newuserdata(L, sizeof(mutex_t *));
   *umutex = mutex;
   mutex->ref_count = 1;
   luaL_getmetatable(L, "ipc.mutex");
   lua_setmetatable(L, -2);
   return 1;
}

int mutex_lock(lua_State *L) {
   mutex_t *mutex = *(mutex_t **)lua_touserdata(L, 1);
   int ret = pthread_mutex_lock(&mutex->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   return 0;
}

int mutex_unlock(lua_State *L) {
   mutex_t *mutex = *(mutex_t **)lua_touserdata(L, 1);
   int ret = pthread_mutex_unlock(&mutex->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   return 0;
}

int mutex_barrier(lua_State *L) {
   mutex_t *mutex = *(mutex_t **)lua_touserdata(L, 1);
   int64_t count = lua_tointeger(L, 2);
   int ret = pthread_mutex_lock(&mutex->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   mutex->barrier++;
   if (mutex->barrier == count) {
      ret = pthread_cond_broadcast(&mutex->cond);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
      mutex->barrier = 0;
   } else {
      ret = pthread_cond_wait(&mutex->cond, &mutex->mutex);
      if (ret) return LUA_HANDLE_ERROR(L, ret);
   }
   ret = pthread_mutex_unlock(&mutex->mutex);
   if (ret) return LUA_HANDLE_ERROR(L, ret);
   return 0;
}

int mutex_retain(lua_State *L) {
   mutex_t *mutex = *(mutex_t **)lua_touserdata(L, 1);
   THAtomicIncrementRef(&mutex->ref_count);
   return 0;
}

int mutex_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.mutex");
   return 1;
}

int mutex_gc(lua_State *L) {
   mutex_t *mutex = *(mutex_t **)lua_touserdata(L, 1);
   if (THAtomicDecrementRef(&mutex->ref_count)) {
      pthread_mutex_destroy(&mutex->mutex);
      pthread_cond_destroy(&mutex->cond);
      free(mutex);
   }
   return 0;
}
