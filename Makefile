CC = gcc
CFLAGS = -Wall -O0 -std=c99 -fPIC -g -I/usr/include/libdrm -I/usr/include/freetype2 -I/usr/include/harfbuzz
LDFLAGS = -lrt -ldrm -lfreetype

SERVER_OBJS = server.o loop.o libbgce.so input.o display.o
LIB_OBJS = libbgce.o

all: bgce libbgce.so client app

bgce: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) -L. -lbgce $(LDFLAGS)

libbgce.so: $(LIB_OBJS)
	$(CC) -shared -o $@ $(LIB_OBJS) $(LDFLAGS)

client: client.c bgce.h
	$(CC) $(CFLAGS) -o $@ client.c -L. -lbgce $(LDFLAGS)

app: app.c bgtk.h bgtk.o
	$(CC) $(CFLAGS) -o $@ app.c bgtk.o -L. -lbgce $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o bgce libbgce.so client app

.PHONY: test
test-server: bgce
	@echo "[MAKE] Starting BGCE server..."
	@LD_LIBRARY_PATH=. ./bgce > $(SERVER_LOG) 2>&1 & echo $$! > $(SERVER_PID)
	chvt 1
	@sleep 3
	@echo "[MAKE] Server running with PID `cat $(SERVER_PID)`"

	@sleep 3
	@echo "[MAKE] Stopping server..."
	@kill `cat $(SERVER_PID)` 2>/dev/null || true
	@rm -f $(SERVER_PID)

	@echo ""
	@echo "=== SERVER LOG ==="
	@tail -n 50 $(SERVER_LOG)

test-client: client
	@echo "[MAKE] Running test client..."
	@LD_LIBRARY_PATH=. ./client > $(CLIENT_LOG) 2>&1 || true

	@echo ""
	@echo "=== CLIENT LOG ==="
	@tail -n 50 $(CLIENT_LOG)
	@echo ""
	@echo "[MAKE] Test finished."
