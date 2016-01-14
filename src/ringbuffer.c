#include "ringbuffer.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct ringbuffer_t
{
  uint8_t* buf;
  size_t cb;
  size_t rp;
  size_t wp;
  size_t rcb;
  size_t saved_wp;
  size_t saved_rcb;
} ringbuffer_t;

ringbuffer_t* ringbuffer_create(size_t cb)
{
  ringbuffer_t* rb = malloc(sizeof(ringbuffer_t));
  rb->buf = malloc(cb);
  rb->cb = cb;
  rb->rp = 0;
  rb->wp = 0;
  rb->rcb = 0;
  rb->saved_wp = 0;
  rb->saved_rcb = 0;
  return rb;
}

void ringbuffer_destroy(ringbuffer_t* rb)
{
  free(rb->buf);
  free(rb);
}

static size_t min(size_t a, size_t b)
{
  if (a < b) {
    return a;
  }
  return b;
}

size_t ringbuffer_write(ringbuffer_t* rb, const void* in, size_t cb)
{
  size_t i = min(cb, rb->cb - rb->rcb);
  size_t j = 0;
  while (j < i) {
    if (in) {
      rb->buf[rb->wp] = ((uint8_t *)in)[j];
    }
    rb->wp = (rb->wp + 1) % rb->cb;
    j++;
  }
  rb->rcb += i;
  return i;
}

size_t ringbuffer_read(ringbuffer_t* rb, void* out, size_t cb)
{
  size_t i = min(cb, rb->rcb);
  size_t j = 0;
  while (j < i) {
    ((uint8_t *)out)[j] = rb->buf[rb->rp];
    rb->rp = (rb->rp + 1) % rb->cb;
    j++;
  }
  rb->rcb -= i;
  return i;
}

size_t ringbuffer_peek(struct ringbuffer_t* rb)
{
  return rb->rcb;
}

void ringbuffer_push_write_pos(struct ringbuffer_t* rb)
{
  rb->saved_wp = rb->wp;
  rb->saved_rcb = rb->rcb;
}

void ringbuffer_pop_write_pos(struct ringbuffer_t* rb)
{
  rb->wp = rb->saved_wp;
  rb->rcb = rb->saved_rcb;
}

void ringbuffer_reset_read_pos(struct ringbuffer_t* rb)
{
  rb->rp = 0;
}

void* ringbuffer_buf_ptr(struct ringbuffer_t* rb)
{
  return rb->buf;
}
