# BGCE Makefile
# Build: make [all|server|client|clean]

CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra
LDFLAGS := -lrt

SERVER  := bgce-server
CLIENT  := bgce-client
LIB     := libbgce.a

SRC_SERVER := bgce-server.c
SRC_CLIENT := bgce-client.c
SRC_LIB    := libbgce.c

OBJ_SERVER := $(SRC_SERVER:.c=.o)
OBJ_CLIENT := $(SRC_CLIENT:.c=.o)
OBJ_LIB    := $(SRC_LIB:.c=.o)

.PHONY: all clean server client lib

all: $(SERVER) $(CLIENT)

server: $(SERVER)
client: $(CLIENT)
lib: $(LIB)

$(SERVER): $(OBJ_SERVER)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT): $(OBJ_CLIENT) $(LIB)
	$(CC) $(CFLAGS) -o $@ $< -L. -lbgce $(LDFLAGS)

$(LIB): $(OBJ_LIB)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(SERVER) $(CLIENT) $(LIB) *.o

