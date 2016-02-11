#ifndef _RINGBUFFER_H_
#define _RINGBUFFER_H_

#include <stddef.h>
#include <stdint.h>

typedef struct ringbuffer_t {
  uint8_t* buf;
  size_t cb;
  size_t rp;
  size_t wp;
  size_t rcb;
  size_t saved_wp;
  size_t saved_rcb;
} ringbuffer_t;

ringbuffer_t* ringbuffer_create(size_t cb);
void ringbuffer_destroy(ringbuffer_t* rb);
void ringbuffer_grow_by(ringbuffer_t *rb, size_t cb);
size_t ringbuffer_write(ringbuffer_t* rb, const void* in, size_t cb);
size_t ringbuffer_read(ringbuffer_t* rb, void* out, size_t cb);
size_t ringbuffer_peek(ringbuffer_t* rb);
void ringbuffer_push_write_pos(ringbuffer_t* rb);
void ringbuffer_pop_write_pos(ringbuffer_t* rb);
void ringbuffer_reset_read_pos(ringbuffer_t* rb);
void* ringbuffer_buf_ptr(ringbuffer_t* rb);

#endif
