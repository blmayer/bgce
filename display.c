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
#include <linux/kd.h>
#include <linux/vt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__has_include)
#  if __has_include(<libdrm/drm.h>)
#    include <libdrm/drm.h>
#  elif __has_include(<drm/drm.h>)
#    include <drm/drm.h>
#  else
#    include <drm/drm.h>
#  endif
#else
#  include <drm/drm.h>
#endif
#if defined(__has_include)
#  if __has_include(<libdrm/drm_mode.h>)
#    include <libdrm/drm_mode.h>
#  elif __has_include(<drm/drm_mode.h>)
#    include <drm/drm_mode.h>
#  else
#    include <drm/drm_mode.h>
#  endif
#else
#  include <drm/drm_mode.h>
#endif
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stb_image_write.h>

extern struct ServerState server;
extern struct config config;

static int vt_fd = -1;

int drm_fd = -1;
uint32_t conn_id = 0;
uint32_t cur_fb = 0;
uint32_t cur_handle;
uint64_t cur_size;
uint32_t cur_w = CURSOR_WIDTH;
uint32_t cur_h = CURSOR_HEIGHT;
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
		perror("[BGCE] DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}
	return 0;
}

static int drm_map_dumb(int fd, uint32_t handle, uint64_t* offset) {
	struct drm_mode_map_dumb map = {0};
	map.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("[BGCE] DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}
	*offset = map.offset;
	return 0;
}

static int drm_destroy_dumb(int fd, uint32_t handle) {
	struct drm_mode_destroy_dumb dest = {0};
	dest.handle = handle;
	if (ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dest) < 0) {
		perror("[BGCE] DRM_IOCTL_MODE_DESTROY_DUMB");
		return -1;
	}
	return 0;
}

static enum BGCECursorType current_cursor = BGCE_CURSOR_DEFAULT;

/* Helper: set a single pixel (ARGB8888) with bounds check */
static inline void cur_px(uint32_t* buf, uint32_t stride_px, uint32_t w, uint32_t h,
                          int x, int y, uint32_t argb) {
	if (x >= 0 && x < (int)w && y >= 0 && y < (int)h)
		buf[y * stride_px + x] = argb;
}

/* Draw a filled line (horizontal) */
static void cur_hline(uint32_t* buf, uint32_t stride_px, uint32_t w, uint32_t h,
                      int x0, int x1, int y, uint32_t argb) {
	for (int x = x0; x <= x1; x++)
		cur_px(buf, stride_px, w, h, x, y, argb);
}

/* Draw a filled line (vertical) */
static void cur_vline(uint32_t* buf, uint32_t stride_px, uint32_t w, uint32_t h,
                      int x, int y0, int y1, uint32_t argb) {
	for (int y = y0; y <= y1; y++)
		cur_px(buf, stride_px, w, h, x, y, argb);
}

/* Outline color and fill color for all cursors */
#define CUR_BLACK 0xFF000000u
#define CUR_WHITE 0xFFFFFFFFu

/* ---------- Individual cursor renderers ---------- */

static void draw_cursor_default(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Classic arrow pointer, ~20px tall */
	/* Outline */
	static const int arrow[][2] = {
		{0,0},{0,1},{0,2},{0,3},{0,4},{0,5},{0,6},{0,7},{0,8},{0,9},{0,10},{0,11},{0,12},{0,13},{0,14},{0,15},{0,16},
		{1,1},{1,2},{1,3},{1,4},{1,5},{1,6},{1,7},{1,8},{1,9},{1,10},{1,11},{1,12},{1,13},{1,14},{1,15},
		{2,2},{2,3},{2,4},{2,5},{2,6},{2,7},{2,8},{2,9},{2,10},{2,11},{2,12},{2,13},{2,14},
		{3,3},{3,4},{3,5},{3,6},{3,7},{3,8},{3,9},{3,10},{3,11},{3,12},{3,13},
		{4,4},{4,5},{4,6},{4,7},{4,8},{4,9},{4,10},{4,11},{4,12},
		{5,5},{5,6},{5,7},{5,8},{5,9},{5,10},{5,11},
		{6,6},{6,7},{6,8},{6,9},{6,10},
		{7,7},{7,8},{7,9},
		{8,8},
	};
	/* White fill (interior) */
	for (size_t i = 0; i < sizeof(arrow)/sizeof(arrow[0]); i++)
		cur_px(buf, sp, w, h, arrow[i][0], arrow[i][1], CUR_WHITE);

	/* Black outline around the arrow */
	/* Left edge */
	for (int y = 0; y <= 16; y++)
		cur_px(buf, sp, w, h, 0, y, CUR_BLACK);
	/* Diagonal right edge */
	for (int i = 0; i <= 16; i++)
		cur_px(buf, sp, w, h, (i+1)/2, i, CUR_BLACK);
	/* Bottom horizontal at y=11 */
	for (int x = 0; x <= 6; x++)
		cur_px(buf, sp, w, h, x, 11, CUR_BLACK);
	/* Bottom tip */
	cur_px(buf, sp, w, h, 0, 16, CUR_BLACK);
	cur_px(buf, sp, w, h, 0, 17, CUR_BLACK);
	/* Diagonal from (6,11) -> (3,17) for the notch */
	cur_px(buf, sp, w, h, 6, 11, CUR_BLACK);
	cur_px(buf, sp, w, h, 5, 12, CUR_BLACK);
	cur_px(buf, sp, w, h, 5, 13, CUR_BLACK);
	cur_px(buf, sp, w, h, 4, 14, CUR_BLACK);
	cur_px(buf, sp, w, h, 4, 15, CUR_BLACK);
	cur_px(buf, sp, w, h, 3, 16, CUR_BLACK);
	cur_px(buf, sp, w, h, 3, 17, CUR_BLACK);
	/* Tail shaft from notch */
	cur_px(buf, sp, w, h, 3, 12, CUR_BLACK);
	cur_px(buf, sp, w, h, 3, 13, CUR_BLACK);
	cur_px(buf, sp, w, h, 2, 14, CUR_BLACK);
	cur_px(buf, sp, w, h, 2, 15, CUR_BLACK);
	cur_px(buf, sp, w, h, 1, 16, CUR_BLACK);
	cur_px(buf, sp, w, h, 1, 17, CUR_BLACK);
	/* White fill for shaft */
	cur_px(buf, sp, w, h, 4, 12, CUR_WHITE);
	cur_px(buf, sp, w, h, 4, 13, CUR_WHITE);
	cur_px(buf, sp, w, h, 3, 14, CUR_WHITE);
	cur_px(buf, sp, w, h, 3, 15, CUR_WHITE);
	cur_px(buf, sp, w, h, 2, 16, CUR_WHITE);
	cur_px(buf, sp, w, h, 2, 17, CUR_WHITE);
}

static void draw_cursor_text(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* I-beam cursor, centered at x=7 */
	int cx = 7;
	/* Top serif */
	cur_hline(buf, sp, w, h, cx - 4, cx + 4, 0, CUR_BLACK);
	cur_hline(buf, sp, w, h, cx - 3, cx + 3, 1, CUR_BLACK);
	/* Vertical bar */
	cur_vline(buf, sp, w, h, cx, 2, 17, CUR_BLACK);
	cur_vline(buf, sp, w, h, cx - 1, 2, 17, CUR_WHITE);
	cur_vline(buf, sp, w, h, cx + 1, 2, 17, CUR_WHITE);
	/* Bottom serif */
	cur_hline(buf, sp, w, h, cx - 3, cx + 3, 18, CUR_BLACK);
	cur_hline(buf, sp, w, h, cx - 4, cx + 4, 19, CUR_BLACK);
}

static void draw_cursor_hand(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Pointing hand — finger up, simplified */
	/* Index finger */
	cur_vline(buf, sp, w, h, 6, 0, 7, CUR_BLACK);
	cur_vline(buf, sp, w, h, 7, 0, 7, CUR_WHITE);
	cur_vline(buf, sp, w, h, 8, 0, 7, CUR_WHITE);
	cur_vline(buf, sp, w, h, 9, 0, 7, CUR_BLACK);
	/* Other fingers opening at y=8 */
	for (int f = 0; f < 4; f++) {
		int bx = 3 + f * 3;
		cur_vline(buf, sp, w, h, bx, 8, 12, CUR_BLACK);
		cur_vline(buf, sp, w, h, bx + 1, 8, 12, CUR_WHITE);
		cur_vline(buf, sp, w, h, bx + 2, 8, 12, CUR_BLACK);
	}
	/* Palm */
	for (int y = 13; y <= 18; y++) {
		cur_px(buf, sp, w, h, 3, y, CUR_BLACK);
		cur_hline(buf, sp, w, h, 4, 13, y, CUR_WHITE);
		cur_px(buf, sp, w, h, 14, y, CUR_BLACK);
	}
	/* Bottom */
	cur_hline(buf, sp, w, h, 3, 14, 19, CUR_BLACK);
}

static void draw_cursor_resize_ns(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Vertical double-headed arrow, centered at (7, 10) */
	int cx = 7;
	/* Up arrow head */
	for (int i = 0; i < 5; i++) {
		cur_hline(buf, sp, w, h, cx - i, cx + i, 4 + i, CUR_BLACK);
		if (i > 0)
			cur_hline(buf, sp, w, h, cx - i + 1, cx + i - 1, 4 + i, CUR_WHITE);
	}
	cur_hline(buf, sp, w, h, cx - 4, cx + 4, 8, CUR_BLACK);
	/* Shaft */
	cur_vline(buf, sp, w, h, cx - 1, 9, 12, CUR_BLACK);
	cur_vline(buf, sp, w, h, cx, 9, 12, CUR_WHITE);
	cur_vline(buf, sp, w, h, cx + 1, 9, 12, CUR_BLACK);
	/* Down arrow head */
	cur_hline(buf, sp, w, h, cx - 4, cx + 4, 13, CUR_BLACK);
	for (int i = 0; i < 5; i++) {
		cur_hline(buf, sp, w, h, cx - 4 + i, cx + 4 - i, 13 + i, CUR_BLACK);
		if (i > 0 && 4 - i > 0)
			cur_hline(buf, sp, w, h, cx - 4 + i + 1, cx + 4 - i - 1, 13 + i, CUR_WHITE);
	}
}

static void draw_cursor_resize_ew(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Horizontal double-headed arrow, centered at (9, 7) */
	int cy = 7;
	/* Left arrow head */
	for (int i = 0; i < 5; i++) {
		cur_vline(buf, sp, w, h, 2 + i, cy - i, cy + i, CUR_BLACK);
		if (i > 0)
			cur_vline(buf, sp, w, h, 2 + i, cy - i + 1, cy + i - 1, CUR_WHITE);
	}
	cur_vline(buf, sp, w, h, 6, cy - 4, cy + 4, CUR_BLACK);
	/* Shaft */
	cur_hline(buf, sp, w, h, 7, 11, cy - 1, CUR_BLACK);
	cur_hline(buf, sp, w, h, 7, 11, cy, CUR_WHITE);
	cur_hline(buf, sp, w, h, 7, 11, cy + 1, CUR_BLACK);
	/* Right arrow head */
	cur_vline(buf, sp, w, h, 12, cy - 4, cy + 4, CUR_BLACK);
	for (int i = 0; i < 5; i++) {
		cur_vline(buf, sp, w, h, 12 + i, cy - 4 + i, cy + 4 - i, CUR_BLACK);
		if (i > 0 && 4 - i > 0)
			cur_vline(buf, sp, w, h, 12 + i, cy - 4 + i + 1, cy + 4 - i - 1, CUR_WHITE);
	}
}

static void draw_cursor_resize_nwse(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Diagonal (NW-SE) double arrow */
	/* NW arrow head */
	for (int i = 0; i < 6; i++) {
		cur_px(buf, sp, w, h, i, 0, CUR_BLACK);
		cur_px(buf, sp, w, h, 0, i, CUR_BLACK);
	}
	for (int i = 1; i < 5; i++) {
		cur_px(buf, sp, w, h, i, 1, CUR_WHITE);
		cur_px(buf, sp, w, h, 1, i, CUR_WHITE);
	}
	/* Diagonal shaft */
	for (int i = 3; i <= 14; i++) {
		cur_px(buf, sp, w, h, i - 1, i, CUR_BLACK);
		cur_px(buf, sp, w, h, i, i, CUR_WHITE);
		cur_px(buf, sp, w, h, i + 1, i, CUR_BLACK);
	}
	/* SE arrow head */
	for (int i = 0; i < 6; i++) {
		cur_px(buf, sp, w, h, 17 - i, 17, CUR_BLACK);
		cur_px(buf, sp, w, h, 17, 17 - i, CUR_BLACK);
	}
	for (int i = 1; i < 5; i++) {
		cur_px(buf, sp, w, h, 17 - i, 16, CUR_WHITE);
		cur_px(buf, sp, w, h, 16, 17 - i, CUR_WHITE);
	}
}

static void draw_cursor_move(uint32_t* buf, uint32_t sp, uint32_t w, uint32_t h) {
	/* Four-way arrow, centered at (9, 9) */
	int cx = 9, cy = 9;
	/* Horizontal bar */
	cur_hline(buf, sp, w, h, 2, 16, cy - 1, CUR_BLACK);
	cur_hline(buf, sp, w, h, 2, 16, cy, CUR_WHITE);
	cur_hline(buf, sp, w, h, 2, 16, cy + 1, CUR_BLACK);
	/* Vertical bar */
	cur_vline(buf, sp, w, h, cx - 1, 2, 16, CUR_BLACK);
	cur_vline(buf, sp, w, h, cx, 2, 16, CUR_WHITE);
	cur_vline(buf, sp, w, h, cx + 1, 2, 16, CUR_BLACK);
	/* Up arrow */
	for (int i = 0; i < 4; i++) {
		cur_px(buf, sp, w, h, cx - i, 2 + i, CUR_BLACK);
		cur_px(buf, sp, w, h, cx + i, 2 + i, CUR_BLACK);
	}
	/* Down arrow */
	for (int i = 0; i < 4; i++) {
		cur_px(buf, sp, w, h, cx - i, 16 - i, CUR_BLACK);
		cur_px(buf, sp, w, h, cx + i, 16 - i, CUR_BLACK);
	}
	/* Left arrow */
	for (int i = 0; i < 4; i++) {
		cur_px(buf, sp, w, h, 2 + i, cy - i, CUR_BLACK);
		cur_px(buf, sp, w, h, 2 + i, cy + i, CUR_BLACK);
	}
	/* Right arrow */
	for (int i = 0; i < 4; i++) {
		cur_px(buf, sp, w, h, 16 - i, cy - i, CUR_BLACK);
		cur_px(buf, sp, w, h, 16 - i, cy + i, CUR_BLACK);
	}
}

/* Render the currently selected cursor into the cursor buffer.
 * If a cursor image was loaded from the config, blit it directly;
 * otherwise fall back to the built-in procedural shape. */
static void render_cursor(uint32_t* buf, uint32_t stride_px, uint32_t w, uint32_t h) {
	memset(buf, 0, h * stride_px * 4); /* clear to fully transparent */

	/* Try loaded image first */
	if (current_cursor >= 0 && current_cursor < BGCE_CURSOR_COUNT &&
	    config.cursors.images[current_cursor]) {
		uint32_t* src = config.cursors.images[current_cursor];
		for (uint32_t y = 0; y < h; y++)
			memcpy(buf + y * stride_px, src + y * w, w * sizeof(uint32_t));
		return;
	}

	/* Built-in fallback */
	switch (current_cursor) {
	case BGCE_CURSOR_TEXT:        draw_cursor_text(buf, stride_px, w, h); break;
	case BGCE_CURSOR_HAND:        draw_cursor_hand(buf, stride_px, w, h); break;
	case BGCE_CURSOR_RESIZE_NS:   draw_cursor_resize_ns(buf, stride_px, w, h); break;
	case BGCE_CURSOR_RESIZE_EW:   draw_cursor_resize_ew(buf, stride_px, w, h); break;
	case BGCE_CURSOR_RESIZE_NWSE: draw_cursor_resize_nwse(buf, stride_px, w, h); break;
	case BGCE_CURSOR_MOVE:        draw_cursor_move(buf, stride_px, w, h); break;
	default:                      draw_cursor_default(buf, stride_px, w, h); break;
	}
}

void set_cursor_type(enum BGCECursorType type) {
	if (type < 0 || type >= BGCE_CURSOR_COUNT)
		type = BGCE_CURSOR_DEFAULT;
	if (type == current_cursor)
		return;

	current_cursor = type;

	/* cur_map is the mmap'd DRM dumb buffer for the cursor */
	if (!cur_map || cur_map == MAP_FAILED)
		return;

	uint32_t stride_px = cur_w; /* pitch in pixels (32bpp, tightly packed) */
	render_cursor((uint32_t*)cur_map, stride_px, cur_w, cur_h);

	/* Re-upload cursor to hardware */
	drmModeSetCursor(server.drm_fd, server.crtc_id, cur_handle, cur_w, cur_h);
}

int setup_vt_handling(void) {
	/* Try the current tty first, then fall back to /dev/tty0 */
	vt_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (vt_fd < 0)
		vt_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (vt_fd < 0) {
		fprintf(stderr, "[BGCE] VT: cannot open tty: %s "
		        "(keystrokes will echo on the console)\n",
		        strerror(errno));
		return -1;
	}

	if (ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		fprintf(stderr, "[BGCE] VT: KDSETMODE KD_GRAPHICS failed: %s "
		        "(keystrokes will echo on the console)\n",
		        strerror(errno));
		close(vt_fd);
		vt_fd = -1;
		return -1;
	}

	printf("[BGCE] VT: switched to KD_GRAPHICS mode\n");
	return 0;
}

static void release_vt(void) {
	if (vt_fd < 0)
		return;
	if (ioctl(vt_fd, KDSETMODE, KD_TEXT) < 0)
		fprintf(stderr, "[BGCE] VT: KDSETMODE KD_TEXT failed: %s\n",
		        strerror(errno));
	else
		printf("[BGCE] VT: restored KD_TEXT mode\n");
	close(vt_fd);
	vt_fd = -1;
}

int init_display() {
	uint32_t crtc_id = 0;
	drmModeModeInfo chosen_mode;
	bool found = false;

	drm_fd = -1;
	for (int card = 0; card < 10; card++) {
		char path[64];
		snprintf(path, sizeof(path), "/dev/dri/card%d", card);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd >= 0) {
			break;
		}
	}
	if (drm_fd < 0) {
		perror("[BGCE] open drm device (tried /dev/dri/card0..9)");
		return 1;
	}
	server.drm_fd = drm_fd;

	setup_vt_handling();

	resources = drmModeGetResources(drm_fd);
	if (!resources) {
		fprintf(stderr, "[BGCE] drmModeGetResources failed\n");
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
		fprintf(stderr, "[BGCE] No connected connector with modes found\n");
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
		fprintf(stderr, "[BGCE] Failed to find a suitable CRTC\n");
		drmModeFreeConnector(connector);
		drmModeFreeResources(resources);
		close(drm_fd);
		return 1;
	}

	/* Save current CRTC to restore later */
	saved_crtc = drmModeGetCrtc(drm_fd, crtc_id);
	if (!saved_crtc) {
		fprintf(stderr, "[BGCE] drmModeGetCrtc failed\n");
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
		fprintf(stderr, "[BGCE] Failed to create dumb buffer for scanout\n");
		return -1;
	}
	scanout_handle = create.handle;
	scanout_size = create.size;
	uint32_t scanout_pitch = create.pitch;

	/* allocate map */
	uint64_t scanout_offset;
	if (drm_map_dumb(drm_fd, scanout_handle, &scanout_offset) < 0) {
		fprintf(stderr, "[BGCE] Failed to map dumb buffer for scanout\n");
		return -1;
	}

	server.framebuffer = mmap(NULL, scanout_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, scanout_offset);
	if (server.framebuffer == MAP_FAILED) {
		perror("[BGCE] mmap scanout");
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
			fprintf(stderr, "[BGCE] drmModeAddFB failed\n");
			return -1;
		}
	}

	/* ---------- Create dumb cursor buffer (small ARGB) ---------- */

	struct drm_mode_create_dumb cur_create = {0};
	if (drm_create_dumb(drm_fd, CURSOR_WIDTH, CURSOR_HEIGHT, 32, &cur_create) < 0) {
		fprintf(stderr, "[BGCE] Failed to create dumb buffer for cursor\n");
		return -1;
	}
	cur_handle = cur_create.handle;
	cur_size = cur_create.size;

	uint64_t cur_offset;
	uint32_t cur_pitch = cur_create.pitch;
	if (drm_map_dumb(drm_fd, cur_handle, &cur_offset) < 0) {
		fprintf(stderr, "[BGCE] Failed to map dumb cursor\n");
		return -1;
	}
	cur_map = mmap(NULL, cur_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, cur_offset);
	if (cur_map == MAP_FAILED) {
		perror("[BGCE] mmap cursor");
		return -1;
	}
	memset(cur_map, 0, cur_size);

	render_cursor((uint32_t*)cur_map, cur_w, CURSOR_WIDTH, CURSOR_HEIGHT);

#ifdef DRM_FORMAT_ARGB8888
	{
		uint32_t handles[4] = {cur_handle, 0, 0, 0};
		uint32_t pitches[4] = {cur_pitch, 0, 0, 0};
		uint32_t offsets[4] = {0, 0, 0, 0};
		uint32_t format = DRM_FORMAT_ARGB8888;
		if (drmModeAddFB2(drm_fd, cur_w, cur_h, format, handles, pitches, offsets, &cur_fb, 0) != 0) {
			fprintf(stderr, "[BGCE] drmModeAddFB2 for cursor failed, trying legacy\n");
			cur_fb = 0;
		}
	}
#endif
	if (!cur_fb) {
		/* Legacy fb creation for cursor might not support alpha; still try */
		uint32_t depth = 24;
		if (drmModeAddFB(drm_fd, CURSOR_WIDTH, CURSOR_HEIGHT, depth, 32, cur_pitch, cur_handle, &cur_fb) != 0) {
			fprintf(stderr, "[BGCE] drmModeAddFB for cursor failed\n");
			return -1;
		}
	}

	/* ---------- Set CRTC (scanout) ---------- */
	if (drmModeSetCrtc(drm_fd, crtc_id, fb_id, 0, 0, &conn_id, 1, &chosen_mode) != 0) {
		fprintf(stderr, "[BGCE] drmModeSetCrtc failed: %s\n", strerror(errno));
		return -1;
	}

	/* ---------- Set cursor ---------- */
	if (drmModeSetCursor(drm_fd, crtc_id, cur_handle, CURSOR_WIDTH, CURSOR_HEIGHT) != 0) {
		fprintf(stderr, "[BGCE] drmModeSetCursor failed: %s\n", strerror(errno));
		/* keep going — maybe hardware doesn't support cursor */
	} else {
		/* move cursor to near center */
		if (drmModeMoveCursor(drm_fd, crtc_id, width / 2, height / 2) != 0) {
			fprintf(stderr, "[BGCE] drmModeMoveCursor failed\n");
		}
	}

	return 0;
}

void draw(struct ServerState* srv, struct Client cli) {
	if (!srv || !srv->framebuffer || !cli.buffer) {
		fprintf(stderr, "[BGCE] Draw: Invalid server, framebuffer, or client buffer\n");
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
		fprintf(stderr, "[BGCE] Draw: Client entirely outside screen\n");
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

static void blit_client_overlap(struct ServerState* srv, const struct Client* cli,
                                int rx0, int ry0, int rx1, int ry1) {
	if (!srv || !srv->framebuffer || !cli || !cli->buffer)
		return;

	int cx0 = (int)cli->x;
	int cy0 = (int)cli->y;
	int cx1 = (int)cli->x + (int)cli->width;
	int cy1 = (int)cli->y + (int)cli->height;

	int ox0 = rx0 > cx0 ? rx0 : cx0;
	int oy0 = ry0 > cy0 ? ry0 : cy0;
	int ox1 = rx1 < cx1 ? rx1 : cx1;
	int oy1 = ry1 < cy1 ? ry1 : cy1;

	if (ox0 >= ox1 || oy0 >= oy1)
		return;

	uint32_t screen_w = srv->display_w;
	uint32_t* dst = (uint32_t*)srv->framebuffer;
	uint32_t* src = (uint32_t*)cli->buffer;

	for (int y = oy0; y < oy1; y++) {
		uint32_t* drow = dst + y * (int)screen_w + ox0;
		uint32_t* srow = src + (y - cy0) * (int)cli->width + (ox0 - cx0);
		memcpy(drow, srow, (size_t)(ox1 - ox0) * 4);
	}
}

static void composite_chain_to_rect(struct ServerState* srv, struct Client* first,
                                   int rx0, int ry0, int rx1, int ry1) {
	if (!srv || !srv->framebuffer)
		return;
	if (rx0 >= rx1 || ry0 >= ry1)
		return;

	int n = 0;
	for (struct Client* c = first; c; c = c->next)
		n++;
	if (n <= 0)
		return;

	struct Client** stack = malloc((size_t)n * sizeof(*stack));
	if (!stack)
		return;

	int i = 0;
	for (struct Client* c = first; c; c = c->next)
		stack[i++] = c;

	/* The linked-list is ordered top->bottom.
	 * For correct composition we must draw bottom->top so that higher windows
	 * overwrite lower ones (background is typically last in the chain).
	 */
	for (i = n - 1; i >= 0; i--) {
		blit_client_overlap(srv, stack[i], rx0, ry0, rx1, ry1);
	}

	free(stack);
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
		fprintf(stderr, "[BGCE] Redraw: Invalid server, framebuffer, or client\n");
		return;
	}

	uint32_t width = c.width;
	uint32_t height = c.height;
	uint32_t screen_w = srv->display_w;
	uint32_t screen_h = srv->display_h;

	/* Rectangle A: exposed area in y-direction (top or bottom strip of the old location) */
	int rect_a_start_x = (int)c.x;
	int rect_a_end_x = (int)c.x + (int)width;
	int rect_a_start_y;
	int rect_a_end_y;
	if (dy > 0) {
		rect_a_start_y = (int)c.y;
		rect_a_end_y = (int)c.y + dy;
	} else {
		rect_a_start_y = (int)c.y + (int)height + dy;
		rect_a_end_y = (int)c.y + (int)height;
	}

	/* Rectangle B: exposed area in x-direction (left or right strip of the old location) */
	int rect_b_start_x;
	int rect_b_end_x;
	if (dx > 0) {
		rect_b_start_x = (int)c.x;
		rect_b_end_x = (int)c.x + dx;
	} else {
		rect_b_start_x = (int)c.x + (int)width + dx;
		rect_b_end_x = (int)c.x + (int)width;
	}

	int rect_b_start_y = (int)c.y;
	int rect_b_end_y = (int)c.y + (int)height;
	/* Avoid overdrawing the corner already covered by rect A (optional) */
	if (dy > 0)
		rect_b_start_y += dy;
	if (dy < 0)
		rect_b_end_y += dy;

	/* Clip to screen boundaries */
	if (rect_a_start_x < 0)
		rect_a_start_x = 0;
	if (rect_a_start_y < 0)
		rect_a_start_y = 0;
	if (rect_a_end_x > (int)screen_w)
		rect_a_end_x = (int)screen_w;
	if (rect_a_end_y > (int)screen_h)
		rect_a_end_y = (int)screen_h;

	if (rect_b_start_x < 0)
		rect_b_start_x = 0;
	if (rect_b_start_y < 0)
		rect_b_start_y = 0;
	if (rect_b_end_x > (int)screen_w)
		rect_b_end_x = (int)screen_w;
	if (rect_b_end_y > (int)screen_h)
		rect_b_end_y = (int)screen_h;

	/* Composite underlying clients (background -> ... -> topmost behind 'c') */
	if (dy && rect_a_start_x < rect_a_end_x && rect_a_start_y < rect_a_end_y) {
		composite_chain_to_rect(srv, c.next, rect_a_start_x, rect_a_start_y, rect_a_end_x, rect_a_end_y);
	}
	if (dx && rect_b_start_x < rect_b_end_x && rect_b_start_y < rect_b_end_y) {
		composite_chain_to_rect(srv, c.next, rect_b_start_x, rect_b_start_y, rect_b_end_x, rect_b_end_y);
	}
}
static void redraw_exposed_rect(struct ServerState* srv, const struct Client* resized_client,
                                 int exposed_x, int exposed_y, int exposed_width, int exposed_height) {
	if (exposed_width <= 0 || exposed_height <= 0) {
		return; // Nothing to draw
	}

	/* Composite everything behind the resized client into the exposed rectangle.
	 * Must be composed bottom->top to preserve correct stacking.
	 */
	composite_chain_to_rect(srv,
	                        resized_client ? resized_client->next : NULL,
	                        exposed_x,
	                        exposed_y,
	                        exposed_x + exposed_width,
	                        exposed_y + exposed_height);
}

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy) {
	if (!srv || !srv->framebuffer) {
		fprintf(stderr, "[BGCE] Redraw from resize: Invalid server or framebuffer\n");
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
	/* Restore text mode first so the terminal is usable even if
	 * the DRM teardown below hangs or crashes. */
	release_vt();

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
		fprintf(stderr, "[BGCE] No framebuffer available for screenshot.\n");
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
		fprintf(stderr, "[BGCE] Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("[BGCE] Screenshot saved to %s.\n", filename);
	return 0;
}
