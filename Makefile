CC ?= cc
BACKEND ?= fbdev

CFLAGS ?= -Wall -O1 -std=c99 -fPIC -g -I.
LDFLAGS ?= -lrt -lm -lpthread

ifeq ($(BACKEND),drm)
CFLAGS += -DBGCE_USE_DRM -I/include/libdrm
LDFLAGS += -ldrm
DISPLAY_OBJ = display_drm.o
else
DISPLAY_OBJ = display_fbdev.o
endif

SERVER_OBJS = server.o loop.o libbgce.so input.o display.o $(DISPLAY_OBJ) config.o
LIB_OBJS = libbgce.o

all: bgce libbgce.so

bgce: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ server.o loop.o input.o display.o $(DISPLAY_OBJ) config.o -L. -lbgce $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) -lrt

client: client.c bgce.h
	$(CC) $(CFLAGS) -o $@ client.c -L. -lbgce -lrt

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce libbgce.so client app

INSTALL_BIN = /bin
INSTALL_LIB = /lib
INSTALL_INCLUDE = /include

.PHONY: install
install: bgce libbgce.so bgce.h
	install -d $(INSTALL_BIN)
	install -m 755 bgce $(INSTALL_BIN)
	install -d $(INSTALL_LIB)
	install -m 755 libbgce.so $(INSTALL_LIB)
	install -d $(INSTALL_INCLUDE)
	install -m 644 bgce.h $(INSTALL_INCLUDE)
	-ldconfig 2>/dev/null || true
