#ifndef BGCE_SHARED_H
#define BGCE_SHARED_H

#define _XOPEN_SOURCE 700
#include "bgce.h"
#include <unistd.h>
#include <sys/types.h>

ssize_t bgce_send_data(int fd, const void *buf, size_t len);
ssize_t bgce_recv_data(int fd, void *buf, size_t len);
ssize_t bgce_send_msg(int fd, const BGCEMessage *msg);
ssize_t bgce_recv_msg(int fd, BGCEMessage *msg);

void bgce_blit_to_framebuffer(ServerState *server, Client *client);

#endif /* BGCE_SHARED_H */

