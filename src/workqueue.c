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

#define DEFAULT_WORKQUEUE_SIZE (16*1024)

#define TOO_TRICKY (0)
#define WORKQUEUE_VERBOSE (0)

typedef struct queue_t {
   struct ringbuffer_t* rb;
   pthread_mutex_t mutex;
   pthread_cond_t read_avail_cond;
#if TOO_TRICKY
   pthread_cond_t write_avail_cond;
#endif
   uint32_t num_items;
} queue_t;

typedef struct workqueue_t {
   struct workqueue_t *next;
   struct workqueue_t *prev;
   int refcount;
   size_t size_increment;
   const char *name;
   queue_t questions;
   queue_t answers;
   pthread_t owner_thread;
   pthread_mutex_t mutex;
} workqueue_t;

static pthread_once_t workqueue_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t workqueue_mutex;
static workqueue_t *workqueue_head;

static void workqueue_one_time_init_inner() {
   pthread_mutexattr_t mutex_attr;
   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&workqueue_mutex, &mutex_attr);
   workqueue_head = NULL;
}

static void workqueue_one_time_init() {
   pthread_once(&workqueue_once, workqueue_one_time_init_inner);
}

static void workqueue_insert(workqueue_t *workqueue) {
   if (workqueue->name == NULL)
      return;
   if (workqueue_head) {
      workqueue_head->prev = workqueue;
      workqueue->next = workqueue_head;
   }
   workqueue_head = workqueue;
}

static void workqueue_remove(workqueue_t *workqueue) {
   if (workqueue->name == NULL)
      return;
   if (workqueue_head == workqueue) {
      workqueue_head = workqueue->next;
   }
   if (workqueue->next) {
      workqueue->next->prev = workqueue->prev;
   }
   if (workqueue->prev) {
      workqueue->prev->next = workqueue->next;
   }
}

static workqueue_t *workqueue_find(const char *name) {
   if (name == NULL)
      return NULL;
   workqueue_t *workqueue = workqueue_head;
   while (workqueue && strcmp(workqueue->name, name) != 0) {
      workqueue = workqueue->next;
   }
   if (workqueue) {
      THAtomicIncrementRef(&workqueue->refcount);
   }
   return workqueue;
}

static void workqueue_init_queue(queue_t *queue, size_t size) {
   pthread_mutexattr_t mutex_attr;
   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&queue->mutex, &mutex_attr);
   pthread_cond_init(&queue->read_avail_cond, NULL);
#if TOO_TRICKY
   pthread_cond_init(&queue->write_avail_cond, NULL);
#endif
   queue->rb = ringbuffer_create(size);
}

static void workqueue_destroy_queue(queue_t *queue) {
   pthread_mutex_destroy(&queue->mutex);
   pthread_cond_destroy(&queue->read_avail_cond);
#if TOO_TRICKY
   pthread_cond_destroy(&queue->write_avail_cond);
#endif
   ringbuffer_destroy(queue->rb);
}

int workqueue_open(lua_State *L) {
   workqueue_one_time_init();
   const char *name = luaL_optlstring(L, 1, NULL, NULL);
   size_t size = luaL_optnumber(L, 2, DEFAULT_WORKQUEUE_SIZE);
   size_t size_increment = luaL_optnumber(L, 3, size);
   pthread_mutex_lock(&workqueue_mutex);
   workqueue_t *workqueue = workqueue_find(name);
   int creator = 0;
   if (!workqueue) {
      creator = 1;
      workqueue = (workqueue_t *)calloc(1, sizeof(workqueue_t));
      workqueue->refcount = 1;
      workqueue->size_increment = size_increment;
      if (name == NULL)
         workqueue->name = NULL;
      else
         workqueue->name = strdup(name);
      workqueue_init_queue(&workqueue->questions, size);
      workqueue_init_queue(&workqueue->answers, size);
      workqueue->owner_thread = pthread_self();
      pthread_mutexattr_t mutex_attr;
      pthread_mutexattr_init(&mutex_attr);
      pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
      pthread_mutex_init(&workqueue->mutex, &mutex_attr);
      workqueue_insert(workqueue);
   }
   pthread_mutex_unlock(&workqueue_mutex);
   workqueue_t** ud = (workqueue_t **)lua_newuserdata(L, sizeof(workqueue_t*));
   *ud = workqueue;
   luaL_getmetatable(L, "ipc.workqueue");
   lua_setmetatable(L, -2);
   lua_pushinteger(L, creator);
   return 2;
}

int workqueue_queue_read(lua_State *L, queue_t *queue, int doNotBlock) {
   pthread_mutex_lock(&queue->mutex);
   while (1) {
      if (queue->num_items) {
         int ret = rb_load(L, queue->rb);
         queue->num_items--;
#if TOO_TRICKY
         pthread_cond_signal(&queue->write_avail_cond);
#endif
         pthread_mutex_unlock(&queue->mutex);
         if (ret < 0) return LUA_HANDLE_ERROR(L, ret);
         return ret;
      } else if (doNotBlock) {
         break;
      } else {
         pthread_cond_wait(&queue->read_avail_cond, &queue->mutex);
      }
   }
   pthread_mutex_unlock(&queue->mutex);
   return 0;
}

int workqueue_read(lua_State *L) {
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   int doNotBlock = luaT_optboolean(L, 2, 0);
   if (workqueue->owner_thread == pthread_self()) {
      return workqueue_queue_read(L, &workqueue->answers, doNotBlock);
   } else {
      return workqueue_queue_read(L, &workqueue->questions, doNotBlock);
   }
}

static int workqueue_queue_write(lua_State *L, int index, queue_t *queue, size_t size_increment, int upval) {
   pthread_mutex_lock(&queue->mutex);
   int top = lua_gettop(L);
   while (index <= top) {
      ringbuffer_push_write_pos(queue->rb);
      int ret = rb_save(L, index, queue->rb, 0, upval);
      if (ret == -ENOMEM) {
         ringbuffer_pop_write_pos(queue->rb);
#if TOO_TRICKY
         if (ringbuffer_peek(queue->rb)) {
            pthread_cond_wait(&queue->write_avail_cond, &queue->mutex);
         } else
#endif
         {
            ringbuffer_grow_by(queue->rb, size_increment);
#if WORKQUEUE_VERBOSE
            fprintf(stderr, "INFO: ipc.workqueue grew to %zu bytes\n", queue->rb->cb);
#endif
         }
      } else if (ret) {
         ringbuffer_pop_write_pos(queue->rb);
         pthread_mutex_unlock(&queue->mutex);
         return LUA_HANDLE_ERROR(L, -ret);
      } else {
         index++;
         queue->num_items++;
      }
   }
   pthread_cond_signal(&queue->read_avail_cond);
   pthread_mutex_unlock(&queue->mutex);
   return 0;
}

int workqueue_write(lua_State *L) {
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   if (workqueue->owner_thread == pthread_self()) {
      return workqueue_queue_write(L, 2, &workqueue->questions, workqueue->size_increment, 0);
   } else {
      return workqueue_queue_write(L, 2, &workqueue->answers, workqueue->size_increment, 0);
   }
}

int workqueue_writeup(lua_State *L) {
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   if (workqueue->owner_thread == pthread_self()) {
      return workqueue_queue_write(L, 2, &workqueue->questions, workqueue->size_increment, 1);
   } else {
      return workqueue_queue_write(L, 2, &workqueue->answers, workqueue->size_increment, 1);
   }
}

int workqueue_drain(lua_State *L) {
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   if (workqueue->owner_thread != pthread_self()) return LUA_HANDLE_ERROR_STR(L, "workqueue drain is only available on the owner thread");
   pthread_mutex_lock(&workqueue->questions.mutex);
   pthread_mutex_lock(&workqueue->answers.mutex);
   uint32_t mark = workqueue->answers.num_items + workqueue->questions.num_items;
   pthread_mutex_unlock(&workqueue->questions.mutex);
   while (workqueue->answers.num_items < mark) {
      pthread_cond_wait(&workqueue->answers.read_avail_cond, &workqueue->answers.mutex);
   }
   pthread_mutex_unlock(&workqueue->answers.mutex);
   return 0;
}

int workqueue_close(lua_State *L) {
   workqueue_t **ud = (workqueue_t **)lua_touserdata(L, 1);
   workqueue_t *workqueue = *ud;
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is already closed");
   pthread_mutex_lock(&workqueue_mutex);
   if (THAtomicDecrementRef(&workqueue->refcount)) {
      workqueue_remove(workqueue);
      workqueue_destroy_queue(&workqueue->questions);
      workqueue_destroy_queue(&workqueue->answers);
      pthread_mutex_destroy(&workqueue->mutex);
      free((void *)workqueue->name);
      workqueue->name = NULL;
      free(workqueue);
   }
   *ud = NULL;
   pthread_mutex_unlock(&workqueue_mutex);
   return 0;
}

int workqueue_gc(lua_State *L) {
   pthread_mutex_lock(&workqueue_mutex);
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (workqueue) {
      workqueue_close(L);
   }
   pthread_mutex_unlock(&workqueue_mutex);
   return 0;
}

int workqueue_retain(lua_State *L) {
   workqueue_t *workqueue = *(workqueue_t **)lua_touserdata(L, 1);
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   if (workqueue->name == NULL) {
      pthread_mutex_lock(&workqueue_mutex);
      THAtomicIncrementRef(&workqueue->refcount);
      pthread_mutex_unlock(&workqueue_mutex);
   }
   return 0;
}

int workqueue_metatablename(lua_State *L) {
   lua_pushstring(L, "ipc.workqueue");
   return 1;
}
