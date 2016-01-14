#include <stddef.h>

struct ringbuffer_t;

struct ringbuffer_t* ringbuffer_create(size_t cb);
void ringbuffer_destroy(struct ringbuffer_t* rb);
size_t ringbuffer_write(struct ringbuffer_t* rb, const void* in, size_t cb);
size_t ringbuffer_read(struct ringbuffer_t* rb, void* out, size_t cb);
size_t ringbuffer_peek(struct ringbuffer_t* rb);
void ringbuffer_push_write_pos(struct ringbuffer_t* rb);
void ringbuffer_pop_write_pos(struct ringbuffer_t* rb);
void ringbuffer_reset_read_pos(struct ringbuffer_t* rb);
void* ringbuffer_buf_ptr(struct ringbuffer_t* rb);
