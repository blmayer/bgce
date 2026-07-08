/*
 * input_headless.c — stubs for headless/mock builds (no /dev/input).
 * Provides pick_client (geometry only) and no-op input loop.
 */

#include "server.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

extern struct ServerState server;

int mouse_x;
int mouse_y;
int ctrl_down;
int alt_down;
int shift_down;

int init_input(void)
{
	mouse_x = (int)server.display_w / 2;
	mouse_y = (int)server.display_h / 2;
	printf("[BGCE] headless input (no devices)\n");
	return 0;
}

void *input_loop(void *arg)
{
	(void)arg;
	return NULL;
}

void client_disconnected(struct Client *c)
{
	(void)c;
}

void deliver_interrupt_to_focus(void)
{
}

int resize_buffer(struct Client *c, int dx, int dy)
{
	(void)c;
	(void)dx;
	(void)dy;
	return 0;
}

struct Client *pick_client(int x, int y)
{
	float wx, wy;
	struct Client *c;
	struct Client *picked = NULL;

	screen_to_world(&server, (float)x, (float)y, &wx, &wy);
	c = server.clients;
	while (c) {
		uint32_t ww = c->world_w ? c->world_w : c->width;
		uint32_t wh = c->world_h ? c->world_h : c->height;
		if (wx >= (float)c->x && wx < (float)(c->x + ww) &&
		    wy >= (float)c->y && wy < (float)(c->y + wh)) {
			picked = c;
			break;
		}
		c = c->next;
	}
	return (picked && picked->z > 0) ? picked : NULL;
}
