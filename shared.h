#ifndef BGCE_SHARED_H
#define BGCE_SHARED_H

#define _XOPEN_SOURCE 700

#include "bgce.h"

#include <sys/types.h>
#include <stdint.h>
#include <unistd.h>
#include <stddef.h>

#define SOCKET_PATH "/tmp/bgce.sock"

/* ----------------------------
 * Shared Utilities
 * ---------------------------- */

/**
 * Send raw data over a socket (blocking until all bytes sent).
 * Returns number of bytes written, or -1 on error.
 */
ssize_t bgce_send_data(int fd, const void *data, size_t size);

/**
 * Receive raw data over a socket (blocking until full message read).
 * Returns number of bytes read, or -1 on error.
 */
ssize_t bgce_recv_data(int fd, void *data, size_t size);

/**
 * Send a structured BGCE message.
 * Returns total bytes sent, or -1 on error.
 */
ssize_t bgce_send_msg(int fd, const struct BGCEMessage *msg);

/**
 * Receive a structured BGCE message.
 * Returns total bytes read, or -1 on error.
 */
ssize_t bgce_recv_msg(int fd, struct BGCEMessage *msg);

#endif /* BGCE_SHARED_H */

