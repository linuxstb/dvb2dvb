#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "dvb2dvb.h"
#include "ringbuffer.h"

/* A simple mutex-free ringbuffer.

   The read thread only modifies the head pointer.
   The write thread only modifies the tail pointer.

   One byte is always left empty to distinguish between an empty and
   full buffer.

   rb->head - the next byte to read
   rb->tail - the next free location to write

*/

int rb_init(struct ringbuffer_t *rb)
{
  // TODO: Init mutexes
  rb->head = rb->buf;
  rb->tail = rb->buf;
  return 0;
}

int rb_get_bytes_used(struct ringbuffer_t* rb)
{
  return((rb->tail - rb->head) % sizeof(rb->buf));
}

int rb_read(struct ringbuffer_t *rb, uint8_t* buf, int count)
{
  //  fprintf(stderr,"rb_read - entering loop\n");
  int bytes_used = rb_get_bytes_used(rb);

  //fprintf(stderr,"rb_read(): count=%d, bytes_used=%d\n",count,bytes_used);
  while (count >= bytes_used) {
    usleep(10);
    bytes_used = rb_get_bytes_used(rb);
    //    fprintf(stderr,"rb_read(): count=%d, bytes_used=%d\n",count,bytes_used);
  }
  //fprintf(stderr,"rb_read - leaving loop\n");

    //    fprintf(stderr,"rb->head=%p, count=%d, rb->buf=%p, sizeof(rb->buf)=%d\n",rb->head,count,rb->buf,(int)sizeof(rb->buf));
    if (rb->head + count >= rb->buf + sizeof(rb->buf)) {
      /* Two-part copy */
      int n1 = sizeof(rb->buf) - (rb->head - rb->buf);
      memcpy(buf,rb->head,n1);
      memcpy(buf,rb->buf,count-n1);
      rb->head = rb->buf + count - n1;
    } else {
      /* Single copy */
      memcpy(buf,rb->head,count);
      rb->head += count;
    }

  return count;
}

/* skip count bytes in the buffer (i.e. read and discard)
   can only be called by the read thread.
   caller is responsible for ensuring there are enough bytes in the buffer to skip.
*/
int rb_skip(struct ringbuffer_t *rb, int count)
{
  if (rb->head + count >= rb->buf + sizeof(rb->buf)) {
    /* Two-part copy */
    int n1 = sizeof(rb->buf) - (rb->head - rb->buf);
    rb->head = rb->buf + count - n1;
  } else {
    /* Single copy */
    rb->head += count;
  }

  return count;
}

int rb_write(struct ringbuffer_t *rb, uint8_t* buf, int count)
{
  int to_copy;

  int bytes_used = (rb->tail - rb->head) % sizeof(rb->buf);
  //fprintf(stderr,"bytes_used = %d\n",bytes_used);

  if (bytes_used + count >= (int)sizeof(rb->buf)) {
    to_copy = sizeof(rb->buf) - bytes_used - 1;
  } else {
    to_copy = count;
  }
  //fprintf(stderr,"to_copy=%d, requested=%d\n",to_copy,count);
  if (to_copy) {
    //fprintf(stderr,"rb->tail=%p, to_copy=%d, rb->buf=%p, sizeof(rb->buf)=%d\n",rb->tail,to_copy,rb->buf,(int)sizeof(rb->buf));
    if (rb->tail + to_copy >= rb->buf + sizeof(rb->buf)) {
      /* Two-part copy */
      int n1 = sizeof(rb->buf) - (rb->tail - rb->buf);
      memcpy(rb->tail,buf,n1);
      memcpy(rb->buf,buf+n1,to_copy-n1);
      rb->tail = rb->buf + to_copy - n1;
    } else {
      /* Single copy */
      memcpy(rb->tail,buf,to_copy);
      rb->tail += to_copy;
    }
  }

  return to_copy;
}
