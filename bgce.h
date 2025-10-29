#ifndef BGCE_H
#define BGCE_H

#define _XOPEN_SOURCE 700
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* ---------------------------------------------------------------------
 * BGCE – Basic Graphics Compositing Engine
 * Public API (libbgce)
 *
 * This header defines the client-facing interface used to communicate
 * with a running BGCE server over a UNIX socket, request buffers,
 * and send draw commands.
 * ------------------------------------------------------------------ */

#define SOCKET_PATH "/tmp/bgce.sock"

/* ----------------------------
 * Protocol Message Types
 * ---------------------------- */
enum {
	MSG_GET_SERVER_INFO = 1,
	MSG_GET_BUFFER,
	MSG_DRAW,
	MSG_INPUT_EVENT,
	MSG_FOCUS_CHANGE
};

/* ----------------------------
 * Data Structures
 * ---------------------------- */

struct BGCEMessage {
	uint32_t type;
	char data[128];
};

struct ServerInfo {
	uint32_t width;
	uint32_t height;
	uint32_t color_depth;
};

struct ClientBufferRequest {
	uint32_t width;
	uint32_t height;
};

struct ClientBufferReply {
	char shm_name[64];
	uint32_t width;
	uint32_t height;
};

enum BGCEInputType {
	INPUT_KEYBOARD,
	INPUT_MOUSE_MOVE,
	INPUT_MOUSE_BUTTON,
};

struct BGCEInputEvent {
	enum BGCEInputType type;
	uint32_t code; /* key code or button code */
	int32_t value; /* press=1, release=0, or delta */
	int32_t x;     /* optional: for mouse move */
	int32_t y;     /* optional: for mouse move */
};

/* ----------------------------
 * API Functions
 * ---------------------------- */

ssize_t bgce_send_msg(int conn, struct BGCEMessage *msg);

ssize_t bgce_recv_msg(int conn, struct BGCEMessage *msg);

/**
 * Connect to a BGCE server socket.
 * Returns a file descriptor, or -1 on error.
 */
int bgce_connect(void);

/**
 * Request server info (width, height, color depth).
 * Returns 0 on success, -1 on failure.
 */
int bgce_get_server_info(int fd, struct ServerInfo* out_info);

/**
 * Request a shared memory buffer from the server.
 * Fills in the reply structure with shm name and dimensions.
 * Returns 0 on success, -1 on failure.
 */
void* bgce_get_buffer(int conn, const struct ClientBufferRequest req);

/**
 * Send a draw command to the server, telling it to blit the
 * shared memory contents to the framebuffer.
 * Returns 0 on success, -1 on failure.
 */
int bgce_draw(int fd);

/**
 * Gracefully close connection and unmap any buffer.
 */
void bgce_close(int fd);

#endif /* BGCE_H */
