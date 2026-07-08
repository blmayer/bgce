/*
 * display_mock.c — in-memory framebuffer for headless/mock tests.
 * Selected with BACKEND=mock (or linked instead of display_fbdev.o).
 * No /dev/fb0, DRM, or VT.
 */

#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct ServerState server;

int init_display(void)
{
	uint32_t w = 800;
	uint32_t h = 600;
	const char *ew = getenv("BGCE_MOCK_WIDTH");
	const char *eh = getenv("BGCE_MOCK_HEIGHT");

	if (ew && ew[0])
		w = (uint32_t)atoi(ew);
	if (eh && eh[0])
		h = (uint32_t)atoi(eh);
	if (w < 64)
		w = 64;
	if (h < 64)
		h = 64;

	server.display_fd = -1;
	server.display_w = w;
	server.display_h = h;
	server.display_bpp = 32;
	server.display_pitch = w * 4;
	server.fb_size = (size_t)w * h * 4;
	server.framebuffer = calloc(1, server.fb_size);
	if (!server.framebuffer) {
		perror("[BGCE] mock framebuffer");
		return 1;
	}

	printf("[BGCE] mock display %ux%u (in-memory)\n", w, h);

	if (display_cursor_init() != 0) {
		fprintf(stderr, "[BGCE] mock cursor init failed\n");
		free(server.framebuffer);
		server.framebuffer = NULL;
		return 1;
	}
	return 0;
}

void release_display(void)
{
	display_cursor_fini();
	free(server.framebuffer);
	server.framebuffer = NULL;
	server.fb_size = 0;
	server.display_fd = -1;
	printf("[BGCE] mock display released\n");
}
