CC = gcc
CFLAGS = -Wall -O2 -std=c99 -fPIC -g
LDFLAGS = -lrt

SERVER_OBJS = server.o shared.o server_handler.o
LIB_OBJS = libbgce.o shared.o

all: bgce libbgce.so test-client

bgce: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

test-client: test_client.c bgce.h
	$(CC) $(CFLAGS) -o $@ test_client.c -L. -lbgce $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce libbgce.so test-client

