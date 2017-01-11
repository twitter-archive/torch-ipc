#include "TH.h"
#include "luaT.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include "ringbuffer.h"
#include "serialize.h"
#include "error.h"

#define DEFAULT_CHANNEL_SIZE (16*1024)

#define CHANNEL_VERBOSE (0)

#define STATUS_OPEN ":open"
#define STATUS_CLOSED ":closed"
#define STATUS_DRAINED ":drained"

typedef struct channel_t {
   struct ringbuffer_t* rb;
   pthread_mutex_t mutex;
   pthread_cond_t read_avail_cond;
   int closed;
   int drained;
   uint32_t num_items;
   int refcount;
   size_t size_increment;
} channel_t;

static void channel_init_queue(channel_t *channel, size_t size) {
   // init queue mutex
   pthread_mutexattr_t mutex_attr;
   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&channel->mutex, &mutex_attr);

   // init condition variables
   pthread_cond_init(&channel->read_avail_cond, NULL);

   // init ring buffer
   channel->rb = ringbuffer_create(size);
}

int channel_create(lua_State *L) {
   channel_t *channel = calloc(1, sizeof(channel_t));
   channel->refcount = 1;
   channel->closed = 0;
   channel->drained = 0;
   channel_t **ud = (channel_t **)lua_newuserdata(L, sizeof(channel_t*));
   channel_init_queue(channel, DEFAULT_CHANNEL_SIZE);
   channel->size_increment = DEFAULT_CHANNEL_SIZE;
   *ud = channel;
   luaL_getmetatable(L, "ipc.channel");
   lua_setmetatable(L, -2);
   return 1;
}

int channel_close(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   if (!channel->closed) {
      channel->closed = 1;
      if (channel->num_items == 0) {
         channel->drained = 1;
      }
      pthread_cond_broadcast(&channel->read_avail_cond);
   }
   pthread_mutex_unlock(&channel->mutex);
   return 0;
}

int channel_closed(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   lua_pushboolean(L, channel->closed);
   pthread_mutex_unlock(&channel->mutex);
   return 1;
}

int channel_drained(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   lua_pushboolean(L, channel->drained);
   pthread_mutex_unlock(&channel->mutex);
   return 1;
}

int channel_read(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   int doNotBlock = luaT_optboolean(L, 2, 0);
   pthread_mutex_lock(&channel->mutex);
   while (1) {
      if (channel->num_items) {
         if (channel->closed && channel->num_items == 1) {
            channel->drained = 1;
            pthread_cond_broadcast(&channel->read_avail_cond);
         }
         if (channel->closed) {
            lua_pushstring(L, STATUS_CLOSED);
         } else {
            lua_pushstring(L, STATUS_OPEN);
         }
         int ret = rb_load(L, channel->rb);
         channel->num_items--;
         pthread_mutex_unlock(&channel->mutex);
         if (ret < 0) return LUA_HANDLE_ERROR(L, ret);
         return ret + 1;
      } else if (channel->drained) {
         pthread_mutex_unlock(&channel->mutex);
         lua_pushstring(L, STATUS_DRAINED);
         return 1;
      } else if (doNotBlock) {
         break;
      } else {
         pthread_cond_wait(&channel->read_avail_cond, &channel->mutex);
      }
   }
   if (channel->drained) {
      lua_pushstring(L, STATUS_DRAINED);
   } else if (channel->closed) {
      lua_pushstring(L, STATUS_CLOSED);
   } else {
      lua_pushstring(L, STATUS_OPEN);
   }
   pthread_mutex_unlock(&channel->mutex);
   return 1;
}

// TODO: Blocking writes should also be supported. This allows for
// backpressure to work. The current implementation grows the
// underlying ringbuffer if it is full.
int channel_write(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   if (channel->drained) {
      lua_pushstring(L, STATUS_DRAINED);
      pthread_mutex_unlock(&channel->mutex);
      return 1;
   } else if (channel->closed) {
      lua_pushstring(L, STATUS_CLOSED);
      pthread_mutex_unlock(&channel->mutex);
      return 1;
   }
   int upval = 0;
   int index = 2;
   int top = lua_gettop(L);
   while (index <= top) {
      ringbuffer_push_write_pos(channel->rb);
      int ret = rb_save(L, index, channel->rb, 0, upval);
      if (ret == -ENOMEM) {
         ringbuffer_pop_write_pos(channel->rb);
         ringbuffer_grow_by(channel->rb, channel->size_increment);
#if CHANNEL_VERBOSE
         fprintf(stderr, "INFO: ipc.channel grew to %zu bytes\n", channel->rb->cb);
#endif
      } else if (ret) {
         ringbuffer_pop_write_pos(channel->rb);
         pthread_mutex_unlock(&channel->mutex);
         return LUA_HANDLE_ERROR(L, -ret);
      } else {
         index++;
         channel->num_items++;
      }
   }
   pthread_cond_signal(&channel->read_avail_cond);
   lua_pushstring(L, STATUS_OPEN);
   pthread_mutex_unlock(&channel->mutex);
   return 1;
}

int channel_num_items(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   lua_pushinteger(L, channel->num_items);
   pthread_mutex_unlock(&channel->mutex);
   return 1;
}

int channel_gc(lua_State *L) {
   channel_t **ud = (channel_t **)lua_touserdata(L, 1);
   channel_t *channel = *ud;
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   if (THAtomicDecrementRef(&channel->refcount)) {
      pthread_cond_destroy(&channel->read_avail_cond);
      ringbuffer_destroy(channel->rb);
      pthread_mutex_unlock(&channel->mutex);
      pthread_mutex_destroy(&channel->mutex);
      free(channel);
      *ud = NULL;
   } else {
      pthread_mutex_unlock(&channel->mutex);
   }
   return 0;
}

int channel_retain(lua_State *L) {
   channel_t *channel = *(channel_t **)lua_touserdata(L, 1);
   if (!channel) return LUA_HANDLE_ERROR_STR(L, "invalid channel");
   pthread_mutex_lock(&channel->mutex);
   THAtomicIncrementRef(&channel->refcount);
   pthread_mutex_unlock(&channel->mutex);
   return 0;
}

int channel_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.channel");
   return 1;
}
