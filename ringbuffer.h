#ifndef _RINGBUFFER_H
#define _RINGBUFFER_H

struct ringbuffer_t {
  uint8_t*  head;
  uint8_t*  tail;
  uint8_t   buf[15*1024*1024];
};

int rb_init(struct ringbuffer_t *rb);
int rb_write(struct ringbuffer_t *rb, uint8_t* buf, int count);
int rb_read(struct ringbuffer_t *rb, uint8_t* buf, int count);
int rb_skip(struct ringbuffer_t *rb, int count);
int rb_get_bytes_used(struct ringbuffer_t* rb);

#endif
