CC = gcc
CFLAGS = -Wall -O2 -std=c99 -fPIC -g
LDFLAGS = -lrt

SERVER_OBJS = bgce_server.o bgce_shared.o
LIB_OBJS = libbgce.o bgce_shared.o

all: bgce-server libbgce.so bgce-test-client

bgce-server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

bgce-test-client: bgce_test_client.c bgce.h
	$(CC) $(CFLAGS) -o $@ bgce_test_client.c -L. -lbgce $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce-server libbgce.so bgce-test-client

