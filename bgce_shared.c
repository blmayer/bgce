#include "bgce_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write exactly len bytes */
ssize_t bgce_send_data(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, p + total, len - total);
		if (n <= 0) return n;
		total += n;
	}
	return total;
}

/* Read exactly len bytes */
ssize_t bgce_recv_data(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;
	size_t total = 0;
	while (total < len) {
		ssize_t n = read(fd, p + total, len - total);
		if (n <= 0) return n;
		total += n;
	}
	return total;
}

/* Send a structured message */
ssize_t bgce_send_msg(int fd, const BGCEMessage *msg)
{
	return bgce_send_data(fd, msg, sizeof(uint32_t)*2 + msg->length);
}

/* Receive a structured message */
ssize_t bgce_recv_msg(int fd, BGCEMessage *msg)
{
	ssize_t n = bgce_recv_data(fd, msg, sizeof(uint32_t)*2);
	if (n <= 0) return n;
	if (msg->length > sizeof(msg->data)) {
		fprintf(stderr, "[BGCE] Invalid message length: %u\n", msg->length);
		return -1;
	}
	if (msg->length > 0)
		n = bgce_recv_data(fd, msg->data, msg->length);
	return n;
}

/* Copy one client's buffer to the global framebuffer */
void bgce_blit_to_framebuffer(ServerState *server, Client *client)
{
	int w = client->width;
	int h = client->height;
	if (!server->framebuffer || !client->buffer)
		return;

	for (int y = 0; y < h && y < server->height; y++) {
		uint8_t *src = client->buffer + y * w * 3;
		uint8_t *dst = server->framebuffer + y * server->width * 3;
		memcpy(dst, src, w * 3);
	}

	/* Save global frame to file for debugging */
	FILE *f = fopen("/tmp/bgce_frame.ppm", "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", server->width, server->height);
		fwrite(server->framebuffer, 1, server->width * server->height * 3, f);
		fclose(f);
	}
}

