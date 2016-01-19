#include "TH.h"
#include "luaT.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "ringbuffer.h"
#include "serialize.h"
#include "error.h"

#define DEFAULT_WORKQUEUE_SIZE (256*1024)

static pthread_once_t workqueue_once = PTHREAD_ONCE_INIT;

typedef struct queue_t {
   struct ringbuffer_t* rb;
   pthread_mutex_t mutex;
   pthread_cond_t read_avail_cond;
   pthread_cond_t write_avail_cond;
   uint32_t num_items;
} queue_t;

typedef struct workqueue_t {
   struct workqueue_t *next;
   struct workqueue_t *prev;
   uint32_t refcount;
   const char *name;
   queue_t questions;
   queue_t answers;
   pthread_t owner_thread;
   pthread_mutex_t mutex;
} workqueue_t;

typedef struct context_t {
   workqueue_t *workqueue;
} context_t;

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
   if (workqueue_head) {
      workqueue_head->prev = workqueue;
      workqueue->next = workqueue_head;
   }
   workqueue_head = workqueue;
}

static void workqueue_remove(workqueue_t *workqueue) {
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
   workqueue_t *workqueue = workqueue_head;
   while (workqueue && strcmp(workqueue->name, name) != 0) {
      workqueue = workqueue->next;
   }
   if (workqueue) {
      workqueue->refcount++;
   }
   return workqueue;
}

static void workqueue_init_queue(queue_t *queue, size_t size) {
   pthread_mutexattr_t mutex_attr;
   pthread_mutexattr_init(&mutex_attr);
   pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
   pthread_mutex_init(&queue->mutex, &mutex_attr);
   pthread_cond_init(&queue->read_avail_cond, NULL);
   pthread_cond_init(&queue->write_avail_cond, NULL);
   queue->rb = ringbuffer_create(size);
}

static void workqueue_destroy_queue(queue_t *queue) {
   pthread_mutex_destroy(&queue->mutex);
   pthread_cond_destroy(&queue->read_avail_cond);
   pthread_cond_destroy(&queue->write_avail_cond);
   ringbuffer_destroy(queue->rb);
}

int workqueue_open(lua_State *L) {
   workqueue_one_time_init();
   const char *name = luaL_checkstring(L, 1);
   size_t size = luaL_optnumber(L, 2, DEFAULT_WORKQUEUE_SIZE);
   pthread_mutex_lock(&workqueue_mutex);
   workqueue_t *workqueue = workqueue_find(name);
   if (!workqueue) {
      workqueue = (workqueue_t *)calloc(1, sizeof(workqueue_t));
      workqueue->refcount = 1;
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
   context_t *context = (context_t *)lua_newuserdata(L, sizeof(context_t));
   context->workqueue = workqueue;
   luaL_getmetatable(L, "parallel.workqueue");
   lua_setmetatable(L, -2);
   return 1;
}

int workqueue_close(lua_State *L) {
   context_t *context = (context_t *)lua_touserdata(L, 1);
   workqueue_t *workqueue = context->workqueue;
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue has already been closed");
   pthread_mutex_lock(&workqueue_mutex);
   workqueue->refcount--;
   if (workqueue->refcount == 0) {
      workqueue_remove(workqueue);
      workqueue_destroy_queue(&workqueue->questions);
      workqueue_destroy_queue(&workqueue->answers);
      pthread_mutex_destroy(&workqueue->mutex);
      free((void *)workqueue->name);
   }
   pthread_mutex_unlock(&workqueue_mutex);
   context->workqueue = NULL;
   return 0;
}

static int workqueue_queue_read(lua_State *L, queue_t *queue, int doNotBlock) {
   pthread_mutex_lock(&queue->mutex);
   while (1) {
      if (queue->num_items) {
         int ret = rb_load(L, queue->rb);
         queue->num_items--;
         pthread_cond_signal(&queue->write_avail_cond);
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
   context_t *context = (context_t *)lua_touserdata(L, 1);
   workqueue_t *workqueue = context->workqueue;
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   int doNotBlock = luaT_optboolean(L, 2, 0);
   if (workqueue->owner_thread == pthread_self()) {
      return workqueue_queue_read(L, &workqueue->answers, doNotBlock);
   } else {
      return workqueue_queue_read(L, &workqueue->questions, doNotBlock);
   }
}

static int workqueue_queue_write(lua_State *L, int index, queue_t *queue) {
   pthread_mutex_lock(&queue->mutex);
   int ret = 0;
   while (1) {
      ringbuffer_push_write_pos(queue->rb);
      ret = rb_save(L, index, queue->rb, 0);
      if (ret) {
         ringbuffer_pop_write_pos(queue->rb);
         if (ringbuffer_peek(queue->rb)) {
            pthread_cond_wait(&queue->write_avail_cond, &queue->mutex);
         } else {
            return LUA_HANDLE_ERROR_STR(L, "workqueue.write message is too big for the ring buffer.");
         }
      } else {
         break;
      }
   }
   queue->num_items++;
   pthread_cond_signal(&queue->read_avail_cond);
   pthread_mutex_unlock(&queue->mutex);
   return ret;
}

int workqueue_write(lua_State *L) {
   context_t *context = (context_t *)lua_touserdata(L, 1);
   workqueue_t *workqueue = context->workqueue;
   if (!workqueue) return LUA_HANDLE_ERROR_STR(L, "workqueue is not open");
   if (workqueue->owner_thread == pthread_self()) {
      return workqueue_queue_write(L, 2, &workqueue->questions);
   } else {
      return workqueue_queue_write(L, 2, &workqueue->answers);
   }
}

int workqueue_drain(lua_State *L) {
   context_t *context = (context_t *)lua_touserdata(L, 1);
   workqueue_t *workqueue = context->workqueue;
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
