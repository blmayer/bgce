/*
 * display.c — compositing, software cursor, VT handling.
 * Scanout setup lives in display_fbdev.c (default) or display_drm.c (BGCE_USE_DRM).
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "server.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stb_image_write.h>

extern struct ServerState server;
extern struct config config;

static int vt_fd = -1;

#if CURSOR_WIDTH != 32 || CURSOR_HEIGHT != 32
#error "software cursor blit assumes CURSOR_WIDTH/HEIGHT == 32"
#endif

/* Software cursor: fixed 32×32 glyph + underlay, row memcpy onto fbdev. */
static uint32_t cur_img[32 * 32];
static uint32_t cur_underlay[32 * 32];
static int cursor_x;
static int cursor_y;
static int cursor_visible;

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

static void cursor_restore(void);
static void cursor_paint(void);

static uint32_t display_stride_px(const struct ServerState *srv)
{
	if (srv->display_pitch >= srv->display_w * 4)
		return srv->display_pitch / 4;
	return srv->display_w;
}

/* Branchless src-over; a==0 keeps dst, a==255 keeps src. */
static inline uint32_t blend32(uint32_t s, uint32_t d)
{
	uint32_t a = s >> 24;
	uint32_t na = 255u - a;
	uint32_t sr = (s >> 16) & 255u, sg = (s >> 8) & 255u, sb = s & 255u;
	uint32_t dr = (d >> 16) & 255u, dg = (d >> 8) & 255u, db = d & 255u;
	return (255u << 24)
		| (((sr * a + dr * na) / 255u) << 16)
		| (((sg * a + dg * na) / 255u) << 8)
		| ((sb * a + db * na) / 255u);
}

/*
 * Blend a contiguous 32×32 block in place (1024 px). Unrolled — no row loop.
 * sq holds underlay on entry, composited result on exit; src is the glyph.
 */
static void blend_sq32(uint32_t sq[32 * 32], const uint32_t src[32 * 32])
{
#define B(i) sq[i] = blend32(src[i], sq[i])
#define BROW(o) \
	B((o)+0);  B((o)+1);  B((o)+2);  B((o)+3);  \
	B((o)+4);  B((o)+5);  B((o)+6);  B((o)+7);  \
	B((o)+8);  B((o)+9);  B((o)+10); B((o)+11); \
	B((o)+12); B((o)+13); B((o)+14); B((o)+15); \
	B((o)+16); B((o)+17); B((o)+18); B((o)+19); \
	B((o)+20); B((o)+21); B((o)+22); B((o)+23); \
	B((o)+24); B((o)+25); B((o)+26); B((o)+27); \
	B((o)+28); B((o)+29); B((o)+30); B((o)+31)
	BROW(0);    BROW(32);   BROW(64);   BROW(96);
	BROW(128);  BROW(160);  BROW(192);  BROW(224);
	BROW(256);  BROW(288);  BROW(320);  BROW(352);
	BROW(384);  BROW(416);  BROW(448);  BROW(480);
	BROW(512);  BROW(544);  BROW(576);  BROW(608);
	BROW(640);  BROW(672);  BROW(704);  BROW(736);
	BROW(768);  BROW(800);  BROW(832);  BROW(864);
	BROW(896);  BROW(928);  BROW(960);  BROW(992);
#undef BROW
#undef B
}

/* fb rows are pitch-separated; gather/scatter one 32×32 tile (unrolled memcpy). */
static void fb_gather32(uint32_t sq[32 * 32], uint32_t *fb, uint32_t sp, int x, int y)
{
	size_t n = 32 * sizeof(uint32_t);
#define G(r) memcpy(sq + (r) * 32, fb + (y + (r)) * (int)sp + x, n)
	G(0);  G(1);  G(2);  G(3);  G(4);  G(5);  G(6);  G(7);
	G(8);  G(9);  G(10); G(11); G(12); G(13); G(14); G(15);
	G(16); G(17); G(18); G(19); G(20); G(21); G(22); G(23);
	G(24); G(25); G(26); G(27); G(28); G(29); G(30); G(31);
#undef G
}

static void fb_scatter32(uint32_t *fb, uint32_t sp, int x, int y,
                         const uint32_t sq[32 * 32])
{
	size_t n = 32 * sizeof(uint32_t);
#define S(r) memcpy(fb + (y + (r)) * (int)sp + x, sq + (r) * 32, n)
	S(0);  S(1);  S(2);  S(3);  S(4);  S(5);  S(6);  S(7);
	S(8);  S(9);  S(10); S(11); S(12); S(13); S(14); S(15);
	S(16); S(17); S(18); S(19); S(20); S(21); S(22); S(23);
	S(24); S(25); S(26); S(27); S(28); S(29); S(30); S(31);
#undef S
}

static void clamp_cursor_pos(int *x, int *y)
{
	int max_x = (int)server.display_w - 32;
	int max_y = (int)server.display_h - 32;
	if (max_x < 0) max_x = 0;
	if (max_y < 0) max_y = 0;
	if (*x < 0) *x = 0;
	if (*y < 0) *y = 0;
	if (*x > max_x) *x = max_x;
	if (*y > max_y) *y = max_y;
}

int display_cursor_init(void)
{
	render_cursor(cur_img, 32, 32, 32);
	cursor_x = (int)server.display_w / 2;
	cursor_y = (int)server.display_h / 2;
	clamp_cursor_pos(&cursor_x, &cursor_y);
	cursor_visible = 0;
	cursor_paint();
	return 0;
}

void display_cursor_fini(void)
{
	cursor_restore();
	cursor_visible = 0;
}

static void cursor_restore(void)
{
	uint32_t sp;

	if (!cursor_visible || !server.framebuffer)
		return;
	sp = display_stride_px(&server);
	fb_scatter32(server.framebuffer, sp, cursor_x, cursor_y, cur_underlay);
	cursor_visible = 0;
}

static void cursor_paint(void)
{
	uint32_t sp;
	uint32_t block[32 * 32];

	if (!server.framebuffer)
		return;
	sp = display_stride_px(&server);
	fb_gather32(block, server.framebuffer, sp, cursor_x, cursor_y);
	memcpy(cur_underlay, block, sizeof(block));
	blend_sq32(block, cur_img);
	fb_scatter32(server.framebuffer, sp, cursor_x, cursor_y, block);
	cursor_visible = 1;
}

void display_cursor_refresh(void)
{
	cursor_restore();
	cursor_paint();
}

void set_cursor_pos(struct ServerState *srv, int x, int y)
{
	(void)srv;
	clamp_cursor_pos(&x, &y);
	if (x == cursor_x && y == cursor_y && cursor_visible)
		return;
	cursor_restore();
	cursor_x = x;
	cursor_y = y;
	cursor_paint();
}

void set_cursor_type(enum BGCECursorType type)
{
	if (type < 0 || type >= BGCE_CURSOR_COUNT)
		type = BGCE_CURSOR_DEFAULT;
	if (type == current_cursor)
		return;
	current_cursor = type;
	cursor_restore();
	render_cursor(cur_img, 32, 32, 32);
	cursor_paint();
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

void release_vt(void) {
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

/* ------------------------------------------------------------------
 * Viewport / coordinate transforms
 * ------------------------------------------------------------------ */

void screen_to_world(const struct ServerState* srv, float sx, float sy,
                     float* wx, float* wy) {
	float z = srv->zoom > 0.0f ? srv->zoom : 1.0f;
	if (wx)
		*wx = sx / z + srv->pan_x;
	if (wy)
		*wy = sy / z + srv->pan_y;
}

void world_to_screen(const struct ServerState* srv, float wx, float wy,
                     float* sx, float* sy) {
	if (sx)
		*sx = (wx - srv->pan_x) * srv->zoom;
	if (sy)
		*sy = (wy - srv->pan_y) * srv->zoom;
}

void clamp_viewport(struct ServerState* srv) {
	if (!srv)
		return;
	if (srv->zoom < BGCE_ZOOM_MIN)
		srv->zoom = BGCE_ZOOM_MIN;
	if (srv->zoom > BGCE_ZOOM_MAX)
		srv->zoom = BGCE_ZOOM_MAX;

	float vis_w = (float)srv->display_w / srv->zoom;
	float vis_h = (float)srv->display_h / srv->zoom;
	float max_x = (float)srv->virtual_w - vis_w;
	float max_y = (float)srv->virtual_h - vis_h;

	if (max_x < 0.0f) {
		/* View larger than world — center the desktop */
		srv->pan_x = max_x * 0.5f;
	} else {
		if (srv->pan_x < 0.0f)
			srv->pan_x = 0.0f;
		if (srv->pan_x > max_x)
			srv->pan_x = max_x;
	}

	if (max_y < 0.0f) {
		srv->pan_y = max_y * 0.5f;
	} else {
		if (srv->pan_y < 0.0f)
			srv->pan_y = 0.0f;
		if (srv->pan_y > max_y)
			srv->pan_y = max_y;
	}
}

/* Screen-space axis-aligned bounds of a client (exclusive end). */
static void client_screen_bounds(const struct ServerState* srv, const struct Client* cli,
                                 int* sx0, int* sy0, int* sx1, int* sy1) {
	float fsx0, fsy0, fsx1, fsy1;
	world_to_screen(srv, (float)cli->x, (float)cli->y, &fsx0, &fsy0);
	world_to_screen(srv, (float)(cli->x + cli->width), (float)(cli->y + cli->height),
	                &fsx1, &fsy1);
	*sx0 = (int)floorf(fsx0);
	*sy0 = (int)floorf(fsy0);
	*sx1 = (int)ceilf(fsx1);
	*sy1 = (int)ceilf(fsy1);
}

/*
 * Nearest-neighbour blit of a client into a screen-space damage rect.
 * Client geometry is in world coordinates; the viewport (pan/zoom) maps
 * world → screen.
 */
static void blit_client_overlap(struct ServerState* srv, const struct Client* cli,
                                int rx0, int ry0, int rx1, int ry1) {
	if (!srv || !srv->framebuffer || !cli || !cli->buffer)
		return;
	if (cli->width == 0 || cli->height == 0)
		return;

	int csx0, csy0, csx1, csy1;
	client_screen_bounds(srv, cli, &csx0, &csy0, &csx1, &csy1);

	int ox0 = rx0 > csx0 ? rx0 : csx0;
	int oy0 = ry0 > csy0 ? ry0 : csy0;
	int ox1 = rx1 < csx1 ? rx1 : csx1;
	int oy1 = ry1 < csy1 ? ry1 : csy1;

	if (ox0 < 0)
		ox0 = 0;
	if (oy0 < 0)
		oy0 = 0;
	if (ox1 > (int)srv->display_w)
		ox1 = (int)srv->display_w;
	if (oy1 > (int)srv->display_h)
		oy1 = (int)srv->display_h;

	if (ox0 >= ox1 || oy0 >= oy1)
		return;

	uint32_t screen_w = display_stride_px(srv);
	uint32_t* dst = (uint32_t*)srv->framebuffer;
	uint32_t* src = (uint32_t*)cli->buffer;
	const int cw = (int)cli->width;
	const int ch = (int)cli->height;
	const float inv_z = 1.0f / srv->zoom;
	const float pan_x = srv->pan_x;
	const float pan_y = srv->pan_y;
	const float cli_x = (float)cli->x;
	const float cli_y = (float)cli->y;

	/* Fast path: identity transform (zoom≈1, pan integer, aligned). */
	if (fabsf(srv->zoom - 1.0f) < 1e-6f &&
	    fabsf(pan_x - roundf(pan_x)) < 1e-6f &&
	    fabsf(pan_y - roundf(pan_y)) < 1e-6f) {
		int ipx = (int)roundf(pan_x);
		int ipy = (int)roundf(pan_y);
		int wox0 = ox0 + ipx;
		int woy0 = oy0 + ipy;
		int wox1 = ox1 + ipx;
		int woy1 = oy1 + ipy;

		int cx0 = (int)cli->x;
		int cy0 = (int)cli->y;
		int cx1 = cx0 + cw;
		int cy1 = cy0 + ch;

		int ix0 = wox0 > cx0 ? wox0 : cx0;
		int iy0 = woy0 > cy0 ? woy0 : cy0;
		int ix1 = wox1 < cx1 ? wox1 : cx1;
		int iy1 = woy1 < cy1 ? woy1 : cy1;
		if (ix0 >= ix1 || iy0 >= iy1)
			return;

		for (int wy = iy0; wy < iy1; wy++) {
			int sy = wy - ipy;
			uint32_t* drow = dst + sy * (int)screen_w + (ix0 - ipx);
			uint32_t* srow = src + (wy - cy0) * cw + (ix0 - cx0);
			memcpy(drow, srow, (size_t)(ix1 - ix0) * 4);
		}
		return;
	}

	for (int sy = oy0; sy < oy1; sy++) {
		float wy = (float)sy * inv_z + pan_y;
		int icy = (int)floorf(wy - cli_y);
		if (icy < 0 || icy >= ch)
			continue;
		uint32_t* drow = dst + sy * (int)screen_w;
		uint32_t* srow = src + icy * cw;
		for (int sx = ox0; sx < ox1; sx++) {
			float wx = (float)sx * inv_z + pan_x;
			int icx = (int)floorf(wx - cli_x);
			if (icx >= 0 && icx < cw)
				drow[sx] = srow[icx];
		}
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

void redraw_all(struct ServerState* srv) {
	if (!srv || !srv->framebuffer)
		return;
	composite_chain_to_rect(srv, srv->clients, 0, 0,
	                        (int)srv->display_w, (int)srv->display_h);
	display_cursor_refresh();
}

void draw(struct ServerState* srv, struct Client cli) {
	if (!srv || !srv->framebuffer || !cli.buffer) {
		fprintf(stderr, "[BGCE] Draw: Invalid server, framebuffer, or client buffer\n");
		return;
	}

	int sx0, sy0, sx1, sy1;
	client_screen_bounds(srv, &cli, &sx0, &sy0, &sx1, &sy1);

	if (sx0 < 0)
		sx0 = 0;
	if (sy0 < 0)
		sy0 = 0;
	if (sx1 > (int)srv->display_w)
		sx1 = (int)srv->display_w;
	if (sy1 > (int)srv->display_h)
		sy1 = (int)srv->display_h;

	if (sx0 >= sx1 || sy0 >= sy1)
		return;

	blit_client_overlap(srv, &cli, sx0, sy0, sx1, sy1);
	display_cursor_refresh();
}

/*
 * Redraw screen regions exposed when a client moves by (wdx, wdy) in
 * world space.  `c` is still at the old position when this is called.
 * dx/dy here are world-space deltas (not screen pixels).
 */
void redraw_region(struct ServerState* srv, struct Client c, int wdx, int wdy) {
	if (!srv || !srv->framebuffer) {
		fprintf(stderr, "[BGCE] Redraw: Invalid server, framebuffer, or client\n");
		return;
	}

	/* Screen bounds of the old position */
	int ox0, oy0, ox1, oy1;
	client_screen_bounds(srv, &c, &ox0, &oy0, &ox1, &oy1);

	/* Screen bounds of the new position */
	struct Client moved = c;
	moved.x = (uint32_t)((int)c.x + wdx);
	moved.y = (uint32_t)((int)c.y + wdy);
	int nx0, ny0, nx1, ny1;
	client_screen_bounds(srv, &moved, &nx0, &ny0, &nx1, &ny1);

	/* Union of old and new (the moved window will be redrawn by draw()) */
	int ux0 = ox0 < nx0 ? ox0 : nx0;
	int uy0 = oy0 < ny0 ? oy0 : ny0;
	int ux1 = ox1 > nx1 ? ox1 : nx1;
	int uy1 = oy1 > ny1 ? oy1 : ny1;

	if (ux0 < 0)
		ux0 = 0;
	if (uy0 < 0)
		uy0 = 0;
	if (ux1 > (int)srv->display_w)
		ux1 = (int)srv->display_w;
	if (uy1 > (int)srv->display_h)
		uy1 = (int)srv->display_h;

	/* Fill the union with everything behind the moving client; caller
	 * then draw()s the client on top at its new position. */
	if (ux0 < ux1 && uy0 < uy1)
		composite_chain_to_rect(srv, c.next, ux0, uy0, ux1, uy1);
	display_cursor_refresh();
}

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy) {
	if (!srv || !srv->framebuffer) {
		fprintf(stderr, "[BGCE] Redraw from resize: Invalid server or framebuffer\n");
		return;
	}

	/*
	 * With an arbitrary zoom/pan transform it is simplest and correct to
	 * recompose the screen bounds of the old (larger) and new sizes.
	 * `c` already has the new size; old size = new - (dx,dy).
	 */
	struct Client old = c;
	old.width = (uint32_t)((int)c.width - dx);
	old.height = (uint32_t)((int)c.height - dy);

	int ox0, oy0, ox1, oy1;
	int nx0, ny0, nx1, ny1;
	client_screen_bounds(srv, &old, &ox0, &oy0, &ox1, &oy1);
	client_screen_bounds(srv, &c, &nx0, &ny0, &nx1, &ny1);

	int ux0 = ox0 < nx0 ? ox0 : nx0;
	int uy0 = oy0 < ny0 ? oy0 : ny0;
	int ux1 = ox1 > nx1 ? ox1 : nx1;
	int uy1 = oy1 > ny1 ? oy1 : ny1;

	if (ux0 < 0)
		ux0 = 0;
	if (uy0 < 0)
		uy0 = 0;
	if (ux1 > (int)srv->display_w)
		ux1 = (int)srv->display_w;
	if (uy1 > (int)srv->display_h)
		uy1 = (int)srv->display_h;

	/* Everything behind the resized client into the union; draw() later. */
	if (ux0 < ux1 && uy0 < uy1)
		composite_chain_to_rect(srv, c.next, ux0, uy0, ux1, uy1);
	display_cursor_refresh();
}

int take_screenshot(const char* filename) {
	if (!server.framebuffer) {
		fprintf(stderr, "[BGCE] No framebuffer available for screenshot.\n");
		return -1;
	}

	uint32_t width = server.display_w;
	uint32_t height = server.display_h;
	uint32_t stride = server.display_pitch ? server.display_pitch
	                                       : width * BGCE_BYTES_PER_PIXEL;

	int result = stbi_write_png(
		filename,
		width,
		height,
		BGCE_BYTES_PER_PIXEL,
		server.framebuffer,
		(int)stride
	);

	if (!result) {
		fprintf(stderr, "[BGCE] Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("[BGCE] Screenshot saved to %s.\n", filename);
	return 0;
}
