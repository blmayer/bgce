CC ?= cc
BACKEND ?= fbdev
UNAME_S := $(shell uname -s)

# No default -g: some small linkers reject STABS (.rela.stab) and fail with
# "Invalid relocation entry ... .rela.stab". For local debugging:
#   make CFLAGS='-Wall -O1 -std=c99 -fPIC -gdwarf-4 -I.'
CFLAGS ?= -Wall -O1 -std=c99 -fPIC -I.
LDFLAGS ?= -lm -lpthread

# Non-Linux: use in-tree linux/*.h stubs for headless builds
ifneq ($(UNAME_S),Linux)
  CFLAGS += -Icompat
endif

ifeq ($(BACKEND),drm)
CFLAGS += -DBGCE_USE_DRM -I/include/libdrm
LDFLAGS += -ldrm
DISPLAY_OBJ = display_drm.o
else ifeq ($(BACKEND),mock)
DISPLAY_OBJ = display_mock.o
else
DISPLAY_OBJ = display_fbdev.o
endif

SERVER_OBJS = server.o loop.o libbgce.so input.o display.o $(DISPLAY_OBJ) config.o location_cache.o
LIB_OBJS = libbgce.o

# Headless mock compositor (no fbdev/input) — works on macOS + Linux
HEADLESS_OBJS = display.o display_mock.o config.o mock.o input_headless.o location_cache.o

all: bgce libbgce.so

bgce: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ server.o loop.o input.o display.o $(DISPLAY_OBJ) config.o -L. -lbgce $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS)

client: client.c bgce.h
	$(CC) $(CFLAGS) -o $@ client.c -L. -lbgce

# BGTK-style mock tests: in-memory FB + PNG screenshots
headless: test/headless.c $(HEADLESS_OBJS)
	$(CC) $(CFLAGS) -o $@ test/headless.c $(HEADLESS_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce libbgce.so client app headless headless_*.png

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
