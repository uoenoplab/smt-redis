.PHONY: all clean

CC = gcc

OBJS = homa_api.o homa_hl.o
CFLAGS = -g -O3 -Wall

all: libhoma.a

%.o: %.c
	$(CC) -fPIC -c -o $@ $< $(CFLAGS)

libhoma.a: $(OBJS)
	ar rcs $@ homa_api.o homa_hl.o

clean:
	-rm -f $(OBJS) libhoma.a
