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
    uint32_t length;
    char data[0]; /* Flexible array member */
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

/* ----------------------------
 * API Functions
 * ---------------------------- */

/**
 * Connect to a BGCE server socket.
 * Returns a file descriptor, or -1 on error.
 */
int bgce_connect(const char *socket_path);

/**
 * Request server info (width, height, color depth).
 * Returns 0 on success, -1 on failure.
 */
int bgce_get_server_info(int fd, struct ServerInfo *out_info);

/**
 * Request a shared memory buffer from the server.
 * Fills in the reply structure with shm name and dimensions.
 * Returns 0 on success, -1 on failure.
 */
void *bgce_get_buffer(const struct ClientBufferRequest *req,
                        struct ClientBufferReply *reply);

/**
 * Send a draw command to the server, telling it to blit the
 * shared memory contents to the framebuffer.
 * Returns 0 on success, -1 on failure.
 */
int bgce_draw(int fd);

/**
 * Gracefully close connection and unmap any buffer.
 */
void bgce_close(int fd, void *mapped_buffer, size_t size);

#endif /* BGCE_H */

