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

#define BGCE_BYTES_PER_PIXEL 4
#define MAX_INPUT_DEVICES 4
/* sun_path capacity used by bgce_socket_path() */
#define BGCE_SOCKPATH_MAX 108

/**
 * Fill buf with this user's server socket path:
 *   $XDG_RUNTIME_DIR/bgce.sock  or  /tmp/bgce-<uid>.sock
 * Returns buf, or NULL if buf is too small.
 */
char *bgce_socket_path(char *buf, size_t buflen);

/* ----------------------------
 * Protocol Message Types
 * ---------------------------- */
enum {
	MSG_GET_SERVER_INFO = 1,
	MSG_GET_BUFFER,
	MSG_DRAW,
	MSG_INPUT_EVENT,
	MSG_BUFFER_CHANGE,
	MSG_FOCUS_CHANGE,
	MSG_SUBSCRIBE_INPUT,
	MSG_MOVE,
	MSG_SET_CURSOR
};

/* ----------------------------
 * Cursor Types
 * ---------------------------- */
enum BGCECursorType {
	BGCE_CURSOR_DEFAULT = 0,
	BGCE_CURSOR_TEXT,
	BGCE_CURSOR_HAND,
	BGCE_CURSOR_RESIZE_NS,
	BGCE_CURSOR_RESIZE_EW,
	BGCE_CURSOR_RESIZE_NWSE,
	BGCE_CURSOR_MOVE,
	BGCE_CURSOR_COUNT /* sentinel – must be last */
};

/* ----------------------------
 * Data Structures
 * ---------------------------- */

struct InputDevice {
	uint16_t id;        // internal index of server
	uint16_t type_mask; // bitmask: KEY, REL, ABS, etc
	char name[256];
};

struct ServerInfo {
	uint32_t width;
	uint32_t height;
	uint32_t color_depth;
	uint16_t input_device_count;
	struct InputDevice devices[MAX_INPUT_DEVICES];
};

struct BufferRequest {
	uint32_t width;
	uint32_t height;
};

struct MoveRequest {
	int32_t x;
	int32_t y;
};

struct ResizeRequest {
	uint32_t width;
	uint32_t height;
};

struct CursorRequest {
	int32_t cursor_type; /* enum BGCECursorType */
};

struct BufferReply {
	int status; // 0 for success, -1 for failure
	char shm_name[64];
	uint32_t width;
	uint32_t height;
};

struct InputEvent {
       int32_t type; /* press=1, release=0, or delta */
       int32_t value; /* press=1, release=0, or delta */
       uint32_t code; /* key code or button code */
       int32_t x;     /* optional: for mouse move */
       int32_t y;     /* optional: for mouse move */
       struct InputDevice device;
};

/* Focus event: state=0 (lost), state=1 (gained) */
struct FocusEvent {
	int32_t state;
};

struct BGCEMessage {
	uint32_t type;
	union {
		struct ServerInfo server_info;
		struct BufferRequest buffer_request;
		struct BufferReply buffer_reply;
		struct MoveRequest move_buffer_request;
		struct InputEvent input_event;
		struct FocusEvent focus_event;
		struct MoveRequest move_request;
		struct CursorRequest cursor_request;
	} data;
};

/* ----------------------------
 * API Functions
 * ---------------------------- */

ssize_t bgce_send_msg(int conn, struct BGCEMessage* msg);

ssize_t bgce_recv_msg(int conn, struct BGCEMessage* msg);

/**
 * Connect to a BGCE server socket (see bgce_socket_path).
 * Returns a file descriptor, or negative on error.
 */
int bgce_connect(void);

/**
 * Request server info (width, height, color depth).
 * Returns 0 on success, -1 on failure.
 */
int bgce_get_server_info(int fd, struct ServerInfo* out_info);

/**
 * Request a shared memory buffer from the server.
 * width/height are the client's drawing size (buffer pixels). The server
 * places the window so it appears roughly that large on screen at the
 * current zoom (world size = buffer / zoom). Returns a mapped buffer, or NULL.
 */
void* bgce_get_buffer(int conn, struct BufferRequest req);

/**
 * Shared pixel buffers (server creates, client maps by token in BufferReply.shm_name).
 * File-backed under the user cache directory (mmap MAP_SHARED), never /dev/shm:
 *   $XDG_CACHE_HOME/bgce/buf/<token>  or  $HOME/.cache/bgce/buf/<token>
 */
int bgce_buf_create(char *name, size_t namelen, size_t size);
int bgce_buf_open(const char *name);
void bgce_buf_unlink(const char *name);

int bgce_move(int fd, int x, int y);

/**
 * Set the cursor type displayed by the server.
 * Returns 0 on success, -1 on failure.
 */
int bgce_set_cursor(int fd, enum BGCECursorType type);

/**
 * Send a draw command to the server, telling it to blit the
 * shared memory contents to the framebuffer.
 * Returns 0 on success, -1 on failure.
 */
int bgce_draw(int fd);

/**
 * Gracefully close connection and unmap any buffer.
 */
void bgce_disconnect(int fd);

#endif /* BGCE_H */
