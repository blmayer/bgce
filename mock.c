/*
 * mock.c — headless compositor harness (BGTK-style mock + screenshots).
 */

#include "mock.h"
#include "compositor.h"
#include "location_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Headless binary does not link server.o — own the globals here. */
struct ServerState server;
struct config config;

/* From input_headless / input */
struct Client *pick_client(int x, int y);

static struct Client *bg_client;
static int mock_active;
static uint32_t mock_next_id = 1;

static void fill_u32(uint32_t *buf, size_t n, uint32_t v)
{
	size_t i;
	for (i = 0; i < n; i++)
		buf[i] = v;
}

int bgce_mock_init(uint32_t width, uint32_t height)
{
	char wbuf[16], hbuf[16];

	if (mock_active)
		bgce_mock_fini();

	snprintf(wbuf, sizeof(wbuf), "%u", width ? width : 800);
	snprintf(hbuf, sizeof(hbuf), "%u", height ? height : 600);
	setenv("BGCE_MOCK_WIDTH", wbuf, 1);
	setenv("BGCE_MOCK_HEIGHT", hbuf, 1);

	memset(&server, 0, sizeof(server));
	server.display_fd = -1;
	server.server_fd = -1;

	memset(&config, 0, sizeof(config));
	config.type = BG_COLOR;
	config.color = 0xFF336699;
	config.mode = IMAGE_SCALED;
	config.move_speed = 1.0f;
	config.pan_speed = 1.0f;
	config.natural_scrolling = 0;

	if (init_display() != 0)
		return -1;

	server.virtual_w = server.display_w * BGCE_WORLD_SCALE;
	server.virtual_h = server.display_h * BGCE_WORLD_SCALE;
	server.zoom_pct = BGCE_ZOOM_PCT_1X;
	server.pan_x = 0;
	server.pan_y = 0;
	server.client_count = 0;
	server.focused_client = NULL;

	bg_client = calloc(1, sizeof(*bg_client));
	if (!bg_client) {
		release_display();
		return -1;
	}
	bg_client->z = 0;
	bg_client->fd = -1;
	bg_client->width = server.virtual_w;
	bg_client->height = server.virtual_h;
	bg_client->world_w = server.virtual_w;
	bg_client->world_h = server.virtual_h;
	bg_client->buffer = malloc((size_t)server.virtual_w * server.virtual_h * 4);
	if (!bg_client->buffer) {
		free(bg_client);
		bg_client = NULL;
		release_display();
		return -1;
	}
	apply_background(&config, bg_client->buffer,
	                 server.virtual_w, server.virtual_h,
	                 server.virtual_w, server.virtual_h);
	server.clients = bg_client;

	init_input();
	location_cache_load();
	/* Sync compositor: paint inline so headless tests need no flush. */
	if (bgce_comp_init(1) != 0) {
		release_display();
		return -1;
	}
	bgce_comp_submit_full();
	display_cursor_present();
	mock_active = 1;
	printf("[BGCE] mock init %ux%u virtual %ux%u\n",
	       server.display_w, server.display_h,
	       server.virtual_w, server.virtual_h);
	return 0;
}

void bgce_mock_fini(void)
{
	struct Client *c;

	if (!mock_active)
		return;

	bgce_comp_shutdown();

	c = server.clients;
	while (c) {
		struct Client *n = c->next;
		if (c->buffer)
			free(c->buffer);
		if (c != bg_client)
			free(c);
		c = n;
	}
	if (bg_client) {
		/* buffer already freed in loop if bg was in list */
		free(bg_client);
		bg_client = NULL;
	}
	server.clients = NULL;
	server.focused_client = NULL;
	release_display();
	mock_active = 0;
}

struct Client *bgce_mock_add_client(uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, uint32_t argb)
{
	struct Client *c;
	size_t n;

	if (!mock_active || w == 0 || h == 0)
		return NULL;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	n = (size_t)w * h;
	c->buffer = malloc(n * 4);
	if (!c->buffer) {
		free(c);
		return NULL;
	}
	fill_u32((uint32_t *)c->buffer, n, argb);
	c->width = w;
	c->height = h;
	c->world_w = w;
	c->world_h = h;
	c->x = x;
	c->y = y;
	c->fd = -1;
	c->id = mock_next_id++;
	if (c->id == 0)
		c->id = mock_next_id++;
	snprintf(c->app_id, sizeof(c->app_id), "mock_%u",
	         (unsigned)server.client_count + 1);
	/* Restore last place if this app_id was cached (tests can set app_id). */
	{
		uint32_t cx, cy;
		if (location_cache_lookup(c->app_id, &cx, &cy)) {
			c->x = cx;
			c->y = cy;
		}
	}
	c->next = server.clients;
	c->z = server.clients ? server.clients->z + 1 : 1;
	server.clients = c;
	server.client_count++;
	server.focused_client = c;
	return c;
}

void bgce_mock_fill_client(struct Client *c, uint32_t argb)
{
	if (!c || !c->buffer)
		return;
	fill_u32((uint32_t *)c->buffer, (size_t)c->width * c->height, argb);
	bgce_mock_draw(c);
}

void bgce_mock_draw(struct Client *c)
{
	if (!c)
		return;
	bgce_comp_submit_draw(c->id);
}

void bgce_mock_remove_client(struct Client *c)
{
	struct Client *prev = NULL;
	struct Client *curr;

	if (!c || c == bg_client)
		return;

	curr = server.clients;
	while (curr) {
		if (curr == c) {
			if (prev)
				prev->next = curr->next;
			else
				server.clients = curr->next;
			break;
		}
		prev = curr;
		curr = curr->next;
	}
	if (server.focused_client == c)
		server.focused_client = NULL;

	location_cache_remember_client(c);
	{
		uint32_t ww = c->world_w ? c->world_w : c->width;
		uint32_t wh = c->world_h ? c->world_h : c->height;

		bgce_comp_submit_erase(c->x, c->y, ww, wh);
	}
	if (c->buffer)
		free(c->buffer);
	free(c);
	if (server.client_count > 0)
		server.client_count--;
}

void bgce_mock_focus(struct Client *c)
{
	struct Client *prev;
	struct Client *curr;

	if (!c || c == bg_client)
		return;

	/* Raise to head of list (top). */
	if (c != server.clients) {
		prev = NULL;
		curr = server.clients;
		while (curr && curr != c) {
			prev = curr;
			curr = curr->next;
		}
		if (curr && prev) {
			prev->next = curr->next;
			curr->next = server.clients;
			server.clients = curr;
		}
	}
	if (server.focused_client)
		c->z = server.focused_client->z + 1;
	server.focused_client = c;
	bgce_comp_submit_draw(c->id);
}

struct Client *bgce_mock_click(int screen_x, int screen_y)
{
	struct Client *c = pick_client(screen_x, screen_y);
	if (c)
		bgce_mock_focus(c);
	else
		server.focused_client = NULL;
	set_cursor_pos(&server, screen_x, screen_y);
	return c;
}

void bgce_mock_move(struct Client *c, int wdx, int wdy)
{
	int old_x, old_y;

	if (!c || (wdx == 0 && wdy == 0))
		return;
	old_x = (int)c->x;
	old_y = (int)c->y;
	c->x = (uint32_t)(old_x + wdx);
	c->y = (uint32_t)(old_y + wdy);
	bgce_comp_submit_move(c->id, old_x, old_y, (int)c->x, (int)c->y);
	location_cache_remember_client(c);
}

void bgce_mock_pan_screen(int sdx, int sdy)
{
	int sp = (int)(config.pan_speed * 256.0f + 0.5f);
	int scx, scy;

	if (sp < 1)
		sp = 256;
	scx = sdx * sp / 256;
	scy = sdy * sp / 256;
	if (scx || scy)
		bgce_comp_submit_pan(scx, scy);
}

void bgce_mock_zoom_to(int zoom_pct, int sx, int sy)
{
	int wx, wy;
	int z;

	screen_to_world(&server, sx, sy, &wx, &wy);
	if (!bgce_zoom_set(&server, zoom_pct)) {
		/* Still re-anchor pan even when zoom is unchanged. */
		z = server.zoom_pct > 0 ? server.zoom_pct : BGCE_ZOOM_PCT_1X;
		server.pan_x = wx * z / 100 - sx;
		server.pan_y = wy * z / 100 - sy;
		clamp_viewport(&server);
		bgce_comp_submit_full();
		display_cursor_present();
		return;
	}
	z = server.zoom_pct > 0 ? server.zoom_pct : BGCE_ZOOM_PCT_1X;
	/* pan is screen-pixel: pan = wx*z/100 - sx */
	server.pan_x = wx * z / 100 - sx;
	server.pan_y = wy * z / 100 - sy;
	clamp_viewport(&server);
	bgce_comp_submit_full();
	display_cursor_present();
}

void bgce_mock_set_viewport(int zoom_pct, int pan_x, int pan_y)
{
	if (!mock_active)
		return;
	if (zoom_pct < BGCE_ZOOM_PCT_MIN)
		zoom_pct = BGCE_ZOOM_PCT_MIN;
	if (zoom_pct > BGCE_ZOOM_PCT_MAX)
		zoom_pct = BGCE_ZOOM_PCT_MAX;
	server.zoom_pct = zoom_pct;
	server.pan_x = pan_x;
	server.pan_y = pan_y;
	clamp_viewport(&server);
	bgce_comp_submit_full();
	display_cursor_present();
}

void bgce_mock_alt_tab(int reverse)
{
	if (!mock_active)
		return;
	bgce_cycle_focus(&server, reverse ? 1 : 0);
	display_cursor_present();
}

void bgce_mock_remember_focus(void)
{
	if (!mock_active || !server.focused_client)
		return;
	location_cache_remember_client(server.focused_client);
}

int bgce_mock_screenshot(const char *path)
{
	return take_screenshot(path ? path : "mock_shot.png");
}

void bgce_mock_redraw_all(void)
{
	bgce_comp_submit_full();
}

int bgce_mock_fb_matches_full_redraw(void)
{
	size_t nbytes;
	void *snap;
	int match;

	if (!mock_active || !server.framebuffer || server.fb_size == 0)
		return -1;

	nbytes = server.fb_size;
	snap = malloc(nbytes);
	if (!snap)
		return -1;
	memcpy(snap, server.framebuffer, nbytes);
	bgce_comp_submit_full();
	match = (memcmp(snap, server.framebuffer, nbytes) == 0);
	free(snap);
	return match ? 0 : -1;
}

/* Stubs so we don't need server.o (which provides main). */
void bgce_announce(const char *fmt, ...)
{
	(void)fmt;
}

volatile sig_atomic_t bgce_sigint_pending;

void bgce_request_shutdown(void)
{
}
