CC = gcc
CFLAGS = -Wall -O2 -std=c99 -fPIC -g
LDFLAGS = -lrt

SERVER_OBJS = server.o server_handler.o libbgce.so input.o
LIB_OBJS = libbgce.o

all: bgce libbgce.so test-client

bgce: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) -L. -lbgce $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

test-client: test_client.c bgce.h
	$(CC) $(CFLAGS) -o $@ test_client.c -L. -lbgce $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce libbgce.so test-client

