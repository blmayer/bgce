#define _XOPEN_SOURCE 700

#include "bgce.h"
#include "shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

/* ============================================================
 *  LOW-LEVEL SOCKET UTILITIES
 * ============================================================ */

/* Write exactly 'size' bytes */
ssize_t bgce_send_data(int fd, const void *data, size_t size)
{
    const char *buf = data;
    size_t total = 0;
    while (total < size) {
        ssize_t n = write(fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("write");
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    return (ssize_t)total;
}

/* Read exactly 'size' bytes */
ssize_t bgce_recv_data(int fd, void *data, size_t size)
{
    char *buf = data;
    size_t total = 0;
    while (total < size) {
        ssize_t n = read(fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return -1;
        }
        if (n == 0)
            break;
        total += n;
    }
    return (ssize_t)total;
}

/* ============================================================
 *  MESSAGE HELPERS
 * ============================================================ */

ssize_t bgce_send_msg(int fd, const struct BGCEMessage *msg)
{
    size_t len = sizeof(msg->type) + sizeof(msg->length) + msg->length;
    return bgce_send_data(fd, msg, len);
}

ssize_t bgce_recv_msg(int fd, struct BGCEMessage *msg)
{
    ssize_t n = bgce_recv_data(fd, msg, sizeof(uint32_t) * 2);
    if (n <= 0)
        return n;

    if (msg->length > 0) {
        ssize_t d = bgce_recv_data(fd, msg->data, msg->length);
        if (d < 0)
            return -1;
        n += d;
    }
    return n;
}

