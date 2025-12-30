/*
 * drm_dumb_cursor.c
 *
 * Minimal example showing how to:
 *  - initialize DRM
 *  - create dumb buffers (scanout + cursor)
 *  - use drmModeSetCrtc and drmModeSetCursor
 *
 * Build:
 *   gcc drm_dumb_cursor.c -o drm_dumb_cursor -ldrm
 *
 * Run (needs permissions to /dev/dri/cardX):
 *   sudo ./drm_dumb_cursor
 *
 * NOTE: This is example/demo code. Error handling tries to be good, but
 * real production code should be more thorough and handle more corner cases.
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "server.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stb_image_write.h>

extern struct ServerState server;

int drm_fd = -1;
uint32_t conn_id = 0;
uint32_t cur_fb = 0;
uint32_t cur_handle;
uint64_t cur_size;
void* cur_map;
uint32_t scanout_handle;
uint64_t scanout_size;
drmModeConnector* connector = NULL;
uint32_t fb_id = 0;
drmModeRes* resources = NULL;
drmModeEncoder* encoder = NULL;
drmModeCrtc* saved_crtc = NULL;

/* wrappers for ioctl structures (from drm_mode.h) */
static int drm_create_dumb(int fd, uint32_t width, uint32_t height, uint32_t bpp,
                           struct drm_mode_create_dumb* create) {
	memset(create, 0, sizeof(*create));
	create->width = width;
	create->height = height;
	create->bpp = bpp;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}
	return 0;
}

static int drm_map_dumb(int fd, uint32_t handle, uint64_t* offset) {
	struct drm_mode_map_dumb map = {0};
	map.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}
	*offset = map.offset;
	return 0;
}

static int drm_destroy_dumb(int fd, uint32_t handle) {
	struct drm_mode_destroy_dumb dest = {0};
	dest.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dest) < 0) {
		perror("DRM_IOCTL_MODE_DESTROY_DUMB");
		return -1;
	}
	return 0;
}

static void draw_cursor(uint8_t* buf, uint32_t stride, uint32_t w, uint32_t h) {
	/* Cursor is ARGB8888 */
	for (uint32_t y = 0; y < h; y++) {
		uint32_t* line = (uint32_t*)(buf + y * stride);
		for (uint32_t x = 0; x < w; x++) {
			/* simple triangle with alpha */
			if (x < y && x > y / 5 && x + y < 64) {
				uint8_t alpha = 200;
				uint8_t red = 255;
				uint8_t green = (x * 255) / (w - 1);
				uint8_t blue = (y * 255) / (h - 1);
				line[x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
			} else {
				line[x] = 0; /* transparent */
			}
		}
	}
}

int init_display() {
	uint32_t crtc_id = 0;
	drmModeModeInfo chosen_mode;
	bool found = false;

	const char* dri_card = "/dev/dri/card1";
	drm_fd = open(dri_card, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open drm device");
		return 1;
	}
	server.drm_fd = drm_fd;

	// if (setup_vt_handling() < 0) {
	//	close(server->drm_fd);
	//	return -1;
	// }

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		fprintf(stderr, "drmModeGetResources failed\n");
		close(drm_fd);
		return 1;
	}

	/* Find first connected connector with at least one mode */
	for (int i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm_fd, resources->connectors[i]);
		if (!connector)
			continue;
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
			/* choose first mode */
			chosen_mode = connector->modes[0];
			conn_id = connector->connector_id;
			found = true;
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!found) {
		fprintf(stderr, "No connected connector with modes found\n");
		drmModeFreeResources(resources);
		close(drm_fd);
		return 1;
	}

	/* Try to find an encoder and CRTC */
	if (connector->encoder_id)
		encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);

	if (encoder && encoder->crtc_id) {
		crtc_id = encoder->crtc_id;
	} else {
		/* fallback: choose any possible CRTC from resources */
		for (int i = 0; i < resources->count_encoders; i++) {
			drmModeEncoder* enc = drmModeGetEncoder(drm_fd, resources->encoders[i]);
			if (!enc)
				continue;
			/* pick first crtc that exists */
			for (int c = 0; c < resources->count_crtcs; c++) {
				uint32_t possible = enc->possible_crtcs;
				if (possible & (1 << c)) {
					crtc_id = resources->crtcs[c];
					break;
				}
			}
			drmModeFreeEncoder(enc);
			if (crtc_id)
				break;
		}
	}

	if (!crtc_id) {
		fprintf(stderr, "Failed to find a suitable CRTC\n");
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		close(drm_fd);
		return 1;
	}

	/* Save current CRTC to restore later */
	saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);
	if (!saved_crtc) {
		fprintf(stderr, "drmModeGetCrtc failed\n");
		/* continue anyway, but we'll try to restore nothing */
	}

	uint32_t width = chosen_mode.hdisplay;
	uint32_t height = chosen_mode.vdisplay;
	uint32_t bpp = 32; /* use 32bpp for scanout */
	server.crtc_id = crtc_id;
	server.display_w = chosen_mode.hdisplay;
	server.display_h = chosen_mode.vdisplay;
	server.display_bpp = bpp;

	printf("[BGCE] Setting up connector %u, CRTC %u, mode %ux%u@%u\n",
	       conn_id, crtc_id, width, height, chosen_mode.vrefresh);

	/* ---------- Create dumb scanout buffer ---------- */
	struct drm_mode_create_dumb create = {0};
	if (drm_create_dumb(drm_fd, width, height, bpp, &create) < 0) {
		fprintf(stderr, "Failed to create dumb buffer for scanout\n");
		return -1;
	}
	scanout_handle = create.handle;
	scanout_size = create.size;
	uint32_t scanout_pitch = create.pitch;

	/* allocate map */
	uint64_t scanout_offset;
	if (drm_map_dumb(drm_fd, scanout_handle, &scanout_offset) < 0) {
		fprintf(stderr, "Failed to map dumb buffer for scanout\n");
		return -1;
	}

	server.framebuffer = mmap(NULL, scanout_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, scanout_offset);
	if (server.framebuffer == MAP_FAILED) {
		perror("mmap scanout");
		return -1;
	}

	/* Clear/draw content */
	memset(server.framebuffer, 0x00, scanout_size);

	/* Create framebuffer object for scanout.
	 * Prefer drmModeAddFB2 (for specifying pixel-format), fallback to drmModeAddFB.
	 */
	bool fb2_ok = false;
#ifdef DRM_FORMAT_ARGB8888
	/* try adb2 path */
	{
		uint32_t handles[4] = {scanout_handle, 0, 0, 0};
		uint32_t pitches[4] = {scanout_pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		uint32_t format = DRM_FORMAT_XRGB8888; /* scanout content uses XRGB (no alpha) */
		if (drmModeAddFB2(drm_fd, width, height, format, handles, pitches, offsets, &fb_id, 0) == 0) {
			fb2_ok = true;
		} else {
			/* drmModeAddFB2 may fail on older drivers; we'll fallback */
			fb2_ok = false;
		}
	}
#endif

	if (!fb2_ok) {
		/* compute depth and bpp for legacy call */
		uint32_t depth = 24;
		if (drmModeAddFB(drm_fd, width, height, depth, bpp, scanout_pitch, scanout_handle, &fb_id) != 0) {
			fprintf(stderr, "drmModeAddFB failed\n");
			return -1;
		}
	}

	/* ---------- Create dumb cursor buffer (small ARGB) ---------- */

	struct drm_mode_create_dumb cur_create = {0};
	if (drm_create_dumb(drm_fd, CURSOR_WIDTH, CURSOR_HEIGHT, 32, &cur_create) < 0) {
		fprintf(stderr, "Failed to create dumb buffer for cursor\n");
		return -1;
	}
	cur_handle = cur_create.handle;
	cur_size = cur_create.size;

	uint64_t cur_offset;
	uint32_t cur_pitch = cur_create.pitch;
	if (drm_map_dumb(drm_fd, cur_handle, &cur_offset) < 0) {
		fprintf(stderr, "Failed to map dumb cursor\n");
		return -1;
	}
	cur_map = mmap(NULL, cur_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, cur_offset);
	if (cur_map == MAP_FAILED) {
		perror("mmap cursor");
		return -1;
	}
	memset(cur_map, 0, cur_size);

	draw_cursor((uint8_t*)cur_map, cur_pitch, CURSOR_WIDTH, CURSOR_HEIGHT);

#ifdef DRM_FORMAT_ARGB8888
	{
		uint32_t handles[4] = {cur_handle, 0, 0, 0};
		uint32_t pitches[4] = {cur_pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		uint32_t format = DRM_FORMAT_ARGB8888;
		if (drmModeAddFB2(drm_fd, cur_w, cur_h, format, handles, pitches, offsets, &cur_fb, 0) != 0) {
			fprintf(stderr, "drmModeAddFB2 for cursor failed, trying legacy\n");
			cur_fb = 0;
		}
	}
#endif
	if (!cur_fb) {
		/* Legacy fb creation for cursor might not support alpha; still try */
		uint32_t depth = 24;
		if (drmModeAddFB(drm_fd, CURSOR_WIDTH, CURSOR_HEIGHT, depth, 32, cur_pitch, cur_handle, &cur_fb) != 0) {
			fprintf(stderr, "drmModeAddFB for cursor failed\n");
			return -1;
		}
	}

	/* ---------- Set CRTC (scanout) ---------- */
	if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn_id, 1, &chosen_mode) != 0) {
		fprintf(stderr, "drmModeSetCrtc failed: %s\n", strerror(errno));
		return -1;
	}

	/* ---------- Set cursor ---------- */
	if (drmModeSetCursor(drm_fd, crtc_id, cur_handle, CURSOR_WIDTH, CURSOR_HEIGHT) != 0) {
		fprintf(stderr, "drmModeSetCursor failed: %s\n", strerror(errno));
		/* keep going — maybe hardware doesn't support cursor */
	} else {
		/* move cursor to near center */
		if (drmModeMoveCursor(drm_fd, crtc_id, width / 2, height / 2) != 0) {
			fprintf(stderr, "drmModeMoveCursor failed\n");
		}
	}

	return 0;
}

void draw(struct ServerState* srv, struct Client cli) {
	if (!srv || !srv->framebuffer || !cli.buffer) {
		fprintf(stderr, "Draw: Invalid server, framebuffer, or client buffer\n");
		return;
	}

	uint32_t screen_w = srv->display_w;
	uint32_t screen_h = srv->display_h;

	uint32_t client_w = cli.width;
	uint32_t client_h = cli.height;

	int cx = cli.x;
	int cy = cli.y;

	uint32_t* dst = (uint32_t*)srv->framebuffer;
	uint32_t* src = (uint32_t*)cli.buffer;

	/* ---------------- Clip Region ---------------- */

	int start_x = cx < 0 ? 0 : cx;
	int start_y = cy < 0 ? 0 : cy;

	int end_x = cx + client_w;
	int end_y = cy + client_h;

	if (end_x > (int)screen_w)
		end_x = screen_w;
	if (end_y > (int)screen_h)
		end_y = screen_h;

	/* Entire window is outside screen */
	if (start_x >= end_x || start_y >= end_y) {
		fprintf(stderr, "Draw: Client entirely outside screen\n");
		return;
	}

	int src_start_x = start_x - cx;
	int src_start_y = start_y - cy;

	int copy_w = end_x - start_x;
	int copy_h = end_y - start_y;

	uint32_t screen_stride_pixels = screen_w; /* No stride stored → compute */

	/* ------------- Copy to DRM FB --------------- */

	for (int y = 0; y < copy_h; y++) {
		uint32_t* drow = dst + (start_y + y) * screen_stride_pixels + start_x;
		uint32_t* srow = src + (src_start_y + y) * client_w + src_start_x;
		memcpy(drow, srow, copy_w * 4);
	}
}

/*
 * The situation:
 *
 * dx<0:                             dx>0:  . (x,y)
 * dy<0:  +----------------+         dy>0:  +----------------+
 *        |     (x,y)      | dx             |     rect A     | dy
 *        |    .           +----+           +----+-----------+----+
 *        |                |    |           |    |                |
 *        |                |    |           |    |                |
 *        |                |    |           |    |                |
 *        |                | B  |           |  B |                |
 *        +----+-----------+----+           +----+                |
 *          dy |     rect A     |             dx |                |
 *             +----------------+                +----------------+
 *
 * So we redraw the rectangles:
 * A: (x, y) (x+width, y+dy) and
 * B: (x, y+dy) (x+dx, y+height)
 */
void redraw_region(struct ServerState* srv, struct Client c, int dx, int dy) {
	if (!srv || !srv->framebuffer) {
		fprintf(stderr, "Redraw: Invalid server, framebuffer, or client\n");
		return;
	}

	uint32_t width = c.width;
	uint32_t height = c.height;
	uint32_t screen_w = srv->display_w;
	uint32_t screen_h = srv->display_h;

	/* Rectangle A: Exposed area in y-direction */
	int rect_a_start_x = c.x;
	int rect_a_start_y = dy > 0 ? c.y : c.y + height + dy;
	int rect_a_end_x = c.x + width;
	int rect_a_end_y = dy > 0 ? c.y + dy : c.y + height;

	/* Rectangle B: Exposed area in x-direction */
	int rect_b_start_x = dx > 0 ? c.x : c.x + width + dx;
	int rect_b_start_y = c.y + (dy > 0 ? dy : 0);
	int rect_b_end_x = dx > 0 ? c.x + dx : c.x + width;
	int rect_b_end_y = c.y + height + (dy > 0 ? dy : 0);

	/* Clip rectangles to screen boundaries */
	rect_a_start_x = rect_a_start_x < 0 ? 0 : rect_a_start_x;
	rect_a_start_y = rect_a_start_y < 0 ? 0 : rect_a_start_y;
	rect_a_end_x = rect_a_end_x > (int)screen_w ? screen_w : rect_a_end_x;
	rect_a_end_y = rect_a_end_y > (int)screen_h ? screen_h : rect_a_end_y;

	rect_b_start_x = rect_b_start_x < 0 ? 0 : rect_b_start_x;
	rect_b_start_y = rect_b_start_y < 0 ? 0 : rect_b_start_y;
	rect_b_end_x = rect_b_end_x > (int)screen_w ? screen_w : rect_b_end_x;
	rect_b_end_y = rect_b_end_y > (int)screen_h ? screen_h : rect_b_end_y;

	struct Client* cli = c.next;
	while (cli) {
		/* Redraw Rectangle A */
		if (dy) {
			int cli_end_x = cli->x + cli->width;
			int cli_end_y = cli->y + cli->height;

			int overlap_start_x = rect_a_start_x > cli->x ? rect_a_start_x : cli->x;
			int overlap_start_y = rect_a_start_y > cli->y ? rect_a_start_y : cli->y;

			int overlap_end_x = rect_a_end_x < cli_end_x ? rect_a_end_x : cli_end_x;
			int overlap_end_y = rect_a_end_y < cli_end_y ? rect_a_end_y : cli_end_y;

			if (overlap_start_x < overlap_end_x && overlap_start_y < overlap_end_y) {
				for (int y = overlap_start_y; y < overlap_end_y; y++) {
					uint32_t* drow = (uint32_t*)srv->framebuffer + y * screen_w + overlap_start_x;
					uint32_t* srow = (uint32_t*)cli->buffer + (y - cli->y) * cli->width + (overlap_start_x - cli->x);
					memcpy(drow, srow, (overlap_end_x - overlap_start_x) * 4);
				}
			}
		}

		/* Redraw Rectangle B */
		if (dx) {
			int cli_end_x = cli->x + cli->width;
			int cli_end_y = cli->y + cli->height;

			int overlap_start_x = rect_b_start_x > cli->x ? rect_b_start_x : cli->x;
			int overlap_start_y = rect_b_start_y > cli->y ? rect_b_start_y : cli->y;

			int overlap_end_x = rect_b_end_x < cli_end_x ? rect_b_end_x : cli_end_x;
			int overlap_end_y = rect_b_end_y < cli_end_y ? rect_b_end_y : cli_end_y;

			if (overlap_start_x < overlap_end_x && overlap_start_y < overlap_end_y) {
				for (int y = overlap_start_y; y < overlap_end_y; y++) {
					uint32_t* drow = (uint32_t*)srv->framebuffer + y * screen_w + overlap_start_x;
					uint32_t* srow = (uint32_t*)cli->buffer + (y - cli->y) * cli->width + (overlap_start_x - cli->x);
					memcpy(drow, srow, (overlap_end_x - overlap_start_x) * 4);
				}
			}
		}
		cli = cli->next;
	}
}

static void redraw_exposed_rect(struct ServerState* srv, const struct Client* resized_client,
                                int exposed_x, int exposed_y, int exposed_width, int exposed_height) {
	if (exposed_width <= 0 || exposed_height <= 0) {
		return; // Nothing to draw
	}

	uint32_t screen_w = srv->display_w;

	// Now, iterate through clients behind the resized_client and draw them if they overlap
	struct Client* cli = resized_client->next;
	while (cli) {
		// Calculate overlap between the exposed rectangle and the current client 'cli'
		int cli_end_x = cli->x + cli->width;
		int cli_end_y = cli->y + cli->height;

		int overlap_start_x = exposed_x > cli->x ? exposed_x : cli->x;
		int overlap_start_y = exposed_y > cli->y ? exposed_y : cli->y;

		int overlap_end_x = (exposed_x + exposed_width) < cli_end_x ? (exposed_x + exposed_width) : cli_end_x;
		int overlap_end_y = (exposed_y + exposed_height) < cli_end_y ? (exposed_y + exposed_height) : cli_end_y;

		if (overlap_start_x < overlap_end_x && overlap_start_y < overlap_end_y) {
			// There's an overlap, copy from client's buffer to framebuffer
			for (int y = overlap_start_y; y < overlap_end_y; y++) {
				uint32_t* drow = (uint32_t*)srv->framebuffer + y * screen_w + overlap_start_x;
				uint32_t* srow = (uint32_t*)cli->buffer + (y - cli->y) * cli->width + (overlap_start_x - cli->x);
				memcpy(drow, srow, (overlap_end_x - overlap_start_x) * 4);
			}
		}
		cli = cli->next;
	}
}

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy) {
	if (!srv || !srv->framebuffer) {
		fprintf(stderr, "Redraw from resize: Invalid server or framebuffer\n");
		return;
	}

	// Calculate the client's old dimensions
	int old_width = c.width - dx;
	int old_height = c.height - dy;

	// Handle horizontal shrinkage (area on the right)
	if (dx < 0) {
		int exposed_x = c.x + c.width; // Start of the exposed area
		int exposed_y = c.y;
		int exposed_width = -dx;       // The amount it shrunk
		int exposed_height = old_height; // This should be the old height

		redraw_exposed_rect(srv, &c, exposed_x, exposed_y, exposed_width, exposed_height);
	}

	// Handle vertical shrinkage (area at the bottom)
	if (dy < 0) {
		int exposed_x = c.x;
		int exposed_y = c.y + c.height; // Start of the exposed area
		int exposed_width = old_width;   // This should be the old width
		int exposed_height = -dy;       // The amount it shrunk

		redraw_exposed_rect(srv, &c, exposed_x, exposed_y, exposed_width, exposed_height);
	}
}

void release_display(void) {
	if (cur_fb)
		drmModeRmFB(drm_fd, cur_fb);

	if (cur_map && cur_map != MAP_FAILED)
		munmap(cur_map, cur_size);

	if (cur_handle)
		drm_destroy_dumb(drm_fd, cur_handle);

	if (fb_id)
		drmModeRmFB(drm_fd, fb_id);

	if (server.framebuffer && server.framebuffer != MAP_FAILED)
		munmap(server.framebuffer, scanout_size);

	if (scanout_handle)
		drm_destroy_dumb(drm_fd, scanout_handle);

	/* restore saved CRTC if we have it */
	if (saved_crtc) {
		drmModeSetCrtc(drm_fd, saved_crtc->crtc_id,
		               saved_crtc->buffer_id,
		               saved_crtc->x, saved_crtc->y,
		               &conn_id, 1,
		               &saved_crtc->mode);
		drmModeFreeCrtc(saved_crtc);
	}

	if (connector)
		drmModeFreeConnector(connector);
	if (resources)
		drmModeFreeResources(resources);
	if (encoder)
		drmModeFreeEncoder(encoder);

	close(drm_fd);
	printf("[BGCE] Display released.\n");
}

int take_screenshot(const char* filename) {
	if (!server.framebuffer) {
		fprintf(stderr, "No framebuffer available for screenshot.\n");
		return -1;
	}

	uint32_t width = server.display_w;
	uint32_t height = server.display_h;
	uint32_t stride = width * BGCE_BYTES_PER_PIXEL;

	// Write the framebuffer to a PNG file
	int result = stbi_write_png(
		filename,
		width,
		height,
		BGCE_BYTES_PER_PIXEL,
		server.framebuffer,
		stride
	);

	if (!result) {
		fprintf(stderr, "Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("Screenshot saved to %s.\n", filename);
	return 0;
}
