CFLAGS =  -g -Wall -W -O2 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
LIBS = -lpthread -lcurl
OBJS = dvb2dvb.o psi_read.o psi_create.o crc32.o ringbuffer.o

all: dvb2dvb

dvb2dvb: $(OBJS)
	$(CC) $(CFLAGS) $(LIBS) -o dvb2dvb $(OBJS)

dvb2dvb.o: dvb2dvb.c dvb2dvb.h psi_read.h psi_create.h crc32.h ringbuffer.h
	$(CC) $(CFLAGS) -c -o dvb2dvb.o dvb2dvb.c

psi_create.o: psi_create.c dvb2dvb.h psi_create.h crc32.h
	$(CC) $(CFLAGS) -c -o psi_create.o psi_create.c

psi_read.o: psi_read.c dvb2dvb.h psi_read.h crc32.h
	$(CC) $(CFLAGS) -c -o psi_read.o psi_read.c

crc32.o: crc32.c crc32.h
	$(CC) $(CFLAGS) -c -o crc32.o crc32.c

ringbuffer.o: ringbuffer.c ringbuffer.h
	$(CC) $(CFLAGS) -c -o ringbuffer.o ringbuffer.c


clean:
	rm -f dvb2dvb $(OBJS) *~
