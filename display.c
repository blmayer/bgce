/*
 * display.c — compositing, software cursor, VT handling.
 * Scanout setup lives in display_fbdev.c (default) or display_drm.c (BGCE_USE_DRM).
 */

#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "compositor.h"
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
/* Previous KDSKBMODE so we can restore on exit (KDGKBMODE uses long). */
static long vt_saved_kbmode = -1;
/* 1 if we successfully set K_OFF and must undo it even when KDGKBMODE failed. */
static int vt_kb_off_active;
#ifndef K_OFF
/* Disabled keyboard → console (Linux ≥ 2.6.39). */
#define K_OFF 0x04
#endif
#ifndef K_XLATE
#define K_XLATE 0x00
#endif

#if CURSOR_WIDTH != 32 || CURSOR_HEIGHT != 32
#error "software cursor blit assumes CURSOR_WIDTH/HEIGHT == 32"
#endif

/* Software cursor: fixed 32×32 glyph + underlay, row memcpy onto fbdev. */
static uint32_t cur_img[32 * 32];
static uint32_t cur_underlay[32 * 32];
/* Logical hotspot (mouse) position — top-left of the 32×32 tile. */
static int cursor_x;
static int cursor_y;
/*
 * Where cur_underlay was captured / the glyph was last drawn.  Restoring
 * always uses these coords, not cursor_x/y, so a move never leaves a trail
 * if paint/restore get out of sync.
 */
static int cur_saved_x;
static int cur_saved_y;
static int cur_underlay_valid; /* 1 = FB has glyph; underlay is live */
/*
 * Software cursor is painted only on the compositor (orchestrator) thread.
 * Input enqueues COMP_CURSOR jobs; it never blits the glyph itself.
 */
static int cursor_dirty;

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
	/* Not visible yet: first scene draw restores (no-op) then paints.
	 * Painting here would capture a black/empty underlay and leave a
	 * permanent black square on the first mouse move. */
	cur_underlay_valid = 0;
	cur_saved_x = cursor_x;
	cur_saved_y = cursor_y;
	return 0;
}

void display_cursor_fini(void)
{
	cursor_restore();
}

/*
 * Put back the pixels under the last painted glyph.  Uses cur_saved_* so a
 * pending move of cursor_x/y cannot restore the wrong square.
 */
static void cursor_restore(void)
{
	uint32_t sp;

	if (!cur_underlay_valid || !server.framebuffer)
		return;
	sp = display_stride_px(&server);
	fb_scatter32(server.framebuffer, sp, cur_saved_x, cur_saved_y,
	             cur_underlay);
	cur_underlay_valid = 0;
}

/*
 * Draw the glyph at cursor_x/y.  Always lifts any previous glyph first so
 * calling paint twice never bakes a pointer into the underlay.
 * Compositor thread only.
 */
static void cursor_paint(void)
{
	uint32_t sp;
	uint32_t block[32 * 32];

	if (!server.framebuffer)
		return;

	if (cur_underlay_valid)
		cursor_restore();

	sp = display_stride_px(&server);
	fb_gather32(block, server.framebuffer, sp, cursor_x, cursor_y);
	memcpy(cur_underlay, block, sizeof(block));
	blend_sq32(block, cur_img);
	fb_scatter32(server.framebuffer, sp, cursor_x, cursor_y, block);
	cur_saved_x = cursor_x;
	cur_saved_y = cursor_y;
	cur_underlay_valid = 1;
	cursor_dirty = 0;
}

/* True if [x0,x1)×[y0,y1) intersects the cursor's 32×32 tile. */
static int cursor_tile_overlaps(int x0, int y0, int x1, int y1)
{
	int cx = cur_underlay_valid ? cur_saved_x : cursor_x;
	int cy = cur_underlay_valid ? cur_saved_y : cursor_y;
	int cx1 = cx + 32;
	int cy1 = cy + 32;

	return x0 < cx1 && x1 > cx && y0 < cy1 && y1 > cy;
}

static int cursor_lift_for_rect_ret(int x0, int y0, int x1, int y1)
{
	if (!cur_underlay_valid)
		return 0;
	if (!cursor_tile_overlaps(x0, y0, x1, y1))
		return 0;
	cursor_restore();
	return 1;
}

static int cursor_lift_all_ret(void)
{
	if (!cur_underlay_valid)
		return 0;
	cursor_restore();
	return 1;
}

/*
 * Begin/end scene paint on the compositor thread.  Glyph is lifted for the
 * duration; restored at end if need_present (or left dirty for a later
 * COMP_CURSOR).
 */
static void cursor_fb_begin(void)
{
	(void)cursor_lift_all_ret();
}

static void cursor_fb_end(int need_present)
{
	if (need_present)
		cursor_dirty = 1;
	if (server.framebuffer && (cursor_dirty || !cur_underlay_valid))
		cursor_paint();
}

int display_cursor_pending(void)
{
	return cursor_dirty || !cur_underlay_valid;
}

void display_cursor_present(void)
{
	if (!server.framebuffer)
		return;
	if (cursor_dirty || !cur_underlay_valid)
		cursor_paint();
}

void display_cursor_refresh(void)
{
	cursor_restore();
	cursor_paint();
}

/*
 * Move the software cursor and paint it.  Must run on the compositor
 * thread (via COMP_CURSOR or after a scene job).  Input only enqueues.
 */
void set_cursor_pos(struct ServerState *srv, int x, int y)
{
	(void)srv;
	clamp_cursor_pos(&x, &y);

	if (x == cursor_x && y == cursor_y) {
		if (cursor_dirty || !cur_underlay_valid)
			cursor_paint();
		return;
	}
	cursor_restore();
	cursor_x = x;
	cursor_y = y;
	cursor_paint();
}

void set_cursor_type(enum BGCECursorType type)
{
	if (type < 0 || type >= BGCE_CURSOR_COUNT)
		type = BGCE_CURSOR_DEFAULT;
	if (type == current_cursor && cur_underlay_valid && !cursor_dirty)
		return;
	current_cursor = type;
	/*
	 * Rebuild glyph only.  Caller should enqueue COMP_CURSOR so the
	 * compositor paints (safe for client threads).
	 */
	render_cursor(cur_img, 32, 32, 32);
	cursor_dirty = 1;
}


int setup_vt_handling(void) {
	/*
	 * KD_GRAPHICS only stops the *console driver drawing* text.
	 * Keys still go to the VT line discipline unless we also disable
	 * keyboard mode (K_OFF).  Without that, getty/shell/new apps on the
	 * same tty still see keystrokes while bgce reads /dev/input.
	 */
	vt_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (vt_fd < 0)
		vt_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (vt_fd < 0) {
		fprintf(stderr, "[BGCE] VT: cannot open tty: %s "
		        "(keystrokes may still go to the console)\n",
		        strerror(errno));
		return -1;
	}

	vt_kb_off_active = 0;
	vt_saved_kbmode = -1;
	if (ioctl(vt_fd, KDGKBMODE, &vt_saved_kbmode) < 0) {
		fprintf(stderr, "[BGCE] VT: KDGKBMODE failed: %s "
		        "(will restore K_XLATE on exit)\n",
		        strerror(errno));
		vt_saved_kbmode = -1;
	}

	if (ioctl(vt_fd, KDSETMODE, KD_GRAPHICS) < 0) {
		fprintf(stderr, "[BGCE] VT: KDSETMODE KD_GRAPHICS failed: %s "
		        "(keystrokes may still go to the console)\n",
		        strerror(errno));
		close(vt_fd);
		vt_fd = -1;
		vt_saved_kbmode = -1;
		return -1;
	}

	/*
	 * Silence the VT keyboard: no more cooked input / Ctrl+C / echo for
	 * processes on this console.  bgce keeps using /dev/input/event*.
	 */
	if (ioctl(vt_fd, KDSKBMODE, (long)K_OFF) < 0) {
		fprintf(stderr, "[BGCE] VT: KDSKBMODE K_OFF failed: %s "
		        "(keys may still reach the tty)\n",
		        strerror(errno));
	} else {
		vt_kb_off_active = 1;
		printf("[BGCE] VT: KD_GRAPHICS + keyboard off "
		       "(input via /dev/input only)\n");
	}

	return 0;
}

/*
 * Always put the console back to a usable state.  Leaving K_OFF active
 * freezes the tty (no keyboard) after quit — that was the exit bug.
 */
void release_vt(void) {
	int fd = vt_fd;
	long mode;

	/* Re-open if needed so we can still recover after a partial teardown. */
	if (fd < 0) {
		fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
		if (fd < 0)
			fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
		if (fd < 0)
			return;
	}

	/*
	 * Prefer the saved mode; never restore K_OFF.  If we never saw a
	 * mode (KDGKBMODE failed), fall back to K_XLATE so the shell works.
	 * K_XLATE is 0 — do not use ">= 0" alone as a "valid" flag.
	 */
	if (vt_saved_kbmode >= 0 && vt_saved_kbmode != (long)K_OFF)
		mode = vt_saved_kbmode;
	else
		mode = (long)K_XLATE;

	/* Keyboard first so the console can accept input as soon as text is on. */
	if (vt_kb_off_active || vt_saved_kbmode >= 0) {
		if (ioctl(fd, KDSKBMODE, mode) < 0) {
			fprintf(stderr, "[BGCE] VT: restore KDSKBMODE %ld failed: %s; "
			        "trying K_XLATE\n",
			        mode, strerror(errno));
			if (ioctl(fd, KDSKBMODE, (long)K_XLATE) < 0)
				fprintf(stderr, "[BGCE] VT: K_XLATE restore failed: %s\n",
				        strerror(errno));
		}
	}

	if (ioctl(fd, KDSETMODE, KD_TEXT) < 0)
		fprintf(stderr, "[BGCE] VT: KDSETMODE KD_TEXT failed: %s\n",
		        strerror(errno));
	else
		printf("[BGCE] VT: restored KD_TEXT + keyboard mode\n");

	if (fd == vt_fd)
		vt_fd = -1;
	close(fd);
	vt_saved_kbmode = -1;
	vt_kb_off_active = 0;
}

/* ------------------------------------------------------------------
 * Viewport / coordinate transforms
 * ------------------------------------------------------------------ */

/* Zoom percent helpers (all integer). screen = world * pct / 100 */
static int zoom_pct_of(const struct ServerState *srv)
{
	int z = srv && srv->zoom_pct > 0 ? srv->zoom_pct : BGCE_ZOOM_PCT_1X;
	if (z < BGCE_ZOOM_PCT_MIN)
		z = BGCE_ZOOM_PCT_MIN;
	if (z > BGCE_ZOOM_PCT_MAX)
		z = BGCE_ZOOM_PCT_MAX;
	return z;
}

/*
 * pan_x/y are screen-pixel offsets (see server.h):
 *   sx = (wx * z) / 100 - pan_x
 *   wx = (sx + pan_x) * 100 / z
 */
void screen_to_world(const struct ServerState *srv, int sx, int sy,
                     int *wx, int *wy)
{
	int z = zoom_pct_of(srv);
	if (wx)
		*wx = (sx + srv->pan_x) * 100 / z;
	if (wy)
		*wy = (sy + srv->pan_y) * 100 / z;
}

void world_to_screen(const struct ServerState *srv, int wx, int wy,
                     int *sx, int *sy)
{
	int z = zoom_pct_of(srv);
	if (sx)
		*sx = (wx * z) / 100 - srv->pan_x;
	if (sy)
		*sy = (wy * z) / 100 - srv->pan_y;
}

int bgce_zoom_set(struct ServerState *srv, int zoom_pct)
{
	int old;

	if (!srv)
		return 0;
	if (zoom_pct < BGCE_ZOOM_PCT_MIN)
		zoom_pct = BGCE_ZOOM_PCT_MIN;
	if (zoom_pct > BGCE_ZOOM_PCT_MAX)
		zoom_pct = BGCE_ZOOM_PCT_MAX;
	old = srv->zoom_pct > 0 ? srv->zoom_pct : BGCE_ZOOM_PCT_1X;
	if (zoom_pct == old)
		return 0;
	srv->zoom_pct = zoom_pct;
	return 1;
}

int bgce_zoom_step(struct ServerState *srv, int dir)
{
	int cur, next;

	if (!srv || dir == 0)
		return 0;
	cur = srv->zoom_pct > 0 ? srv->zoom_pct : BGCE_ZOOM_PCT_1X;
	next = cur + (dir > 0 ? BGCE_ZOOM_PCT_STEP : -BGCE_ZOOM_PCT_STEP);
	return bgce_zoom_set(srv, next);
}

void clamp_viewport(struct ServerState *srv)
{
	int z, max_x, max_y;

	if (!srv)
		return;
	z = zoom_pct_of(srv);
	srv->zoom_pct = z;

	/* Max pan in screen pixels: world [0, virtual) must cover the view. */
	max_x = (int)srv->virtual_w * z / 100 - (int)srv->display_w;
	max_y = (int)srv->virtual_h * z / 100 - (int)srv->display_h;

	if (max_x < 0)
		srv->pan_x = max_x / 2;
	else {
		if (srv->pan_x < 0)
			srv->pan_x = 0;
		if (srv->pan_x > max_x)
			srv->pan_x = max_x;
	}

	if (max_y < 0)
		srv->pan_y = max_y / 2;
	else {
		if (srv->pan_y < 0)
			srv->pan_y = 0;
		if (srv->pan_y > max_y)
			srv->pan_y = max_y;
	}
}

/*
 * Screen-space axis-aligned bounds of a client (exclusive end).
 *
 * Screen pixel (sx,sy) samples world
 *   wx = floor((sx + pan_x) * 100 / z)
 * (see blit_client_scaled / screen_to_world).  The bounds must cover every
 * pixel whose sample lands in [x, x+ww) × [y, y+wh):
 *
 *   sx0 = ceil(x * z / 100) - pan_x
 *   sx1 = ceil((x + ww) * z / 100) - pan_x   (exclusive)
 *
 * Using floor(world_to_screen) for both corners under-covers the trailing
 * edge and can leave 1px hairs / soft edges when the window moves slowly
 * (especially at zoom ≠ 100%).
 */
static void client_screen_bounds(const struct ServerState *srv,
                                 const struct Client *cli,
                                 int *sx0, int *sy0, int *sx1, int *sy1)
{
	uint32_t ww = cli->world_w ? cli->world_w : cli->width;
	uint32_t wh = cli->world_h ? cli->world_h : cli->height;
	int z = zoom_pct_of(srv);
	int x = (int)cli->x;
	int y = (int)cli->y;
	int w = (int)ww;
	int h = (int)wh;

	/* ceil(n/100) for n >= 0 is (n + 99) / 100 */
	*sx0 = (x * z + 99) / 100 - srv->pan_x;
	*sy0 = (y * z + 99) / 100 - srv->pan_y;
	*sx1 = ((x + w) * z + 99) / 100 - srv->pan_x;
	*sy1 = ((y + h) * z + 99) / 100 - srv->pan_y;
	if (*sx1 <= *sx0)
		*sx1 = *sx0 + 1;
	if (*sy1 <= *sy0)
		*sy1 = *sy0 + 1;
}

/*
 * Fill a horizontal run of identical pixels (unrolled like blend_sq32).
 */
static inline void fill_u32(uint32_t *p, int n, uint32_t v)
{
	int i = 0;
	for (; i + 8 <= n; i += 8) {
		p[i + 0] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
		p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
	}
	for (; i < n; i++)
		p[i] = v;
}

/*
 * 1:1 blit when zoom_pct == 100 and buffer size == world size.
 *   pan is screen-pixel pan: world = screen + pan
 */
static void blit_client_1to1(uint32_t *dst, uint32_t screen_w,
                             const uint32_t *src, int cw, int ch,
                             int cli_x, int cli_y,
                             int ox0, int oy0, int ox1, int oy1,
                             int pan_x, int pan_y)
{
	int ipx = pan_x;
	int ipy = pan_y;
	int cx0 = cli_x, cy0 = cli_y;
	int cx1 = cx0 + cw, cy1 = cy0 + ch;

	int ix0 = (ox0 + ipx) > cx0 ? (ox0 + ipx) : cx0;
	int iy0 = (oy0 + ipy) > cy0 ? (oy0 + ipy) : cy0;
	int ix1 = (ox1 + ipx) < cx1 ? (ox1 + ipx) : cx1;
	int iy1 = (oy1 + ipy) < cy1 ? (oy1 + ipy) : cy1;
	if (ix0 >= ix1 || iy0 >= iy1)
		return;

	size_t row_bytes = (size_t)(ix1 - ix0) * sizeof(uint32_t);
	for (int wy = iy0; wy < iy1; wy++) {
		int sy = wy - ipy;
		uint32_t *drow = dst + sy * (int)screen_w + (ix0 - ipx);
		const uint32_t *srow = src + (wy - cy0) * cw + (ix0 - cx0);
		memcpy(drow, srow, row_bytes);
	}
}

/*
 * Fixed-point nearest-neighbour blit (16.16), integer zoom percent.
 *
 *   wx = (sx + pan) * 100 / zoom_pct   (pan in screen pixels)
 *   icx = (wx - cli_x) * bw / ww
 */
static void blit_client_scaled(uint32_t *dst, uint32_t screen_w,
                               const uint32_t *src, int bw, int bh,
                               int ww, int wh,
                               int cli_x, int cli_y,
                               int ox0, int oy0, int ox1, int oy1,
                               int pan_x, int pan_y, int zoom_pct)
{
	int64_t step_x_fp, step_y_fp, x0_fp, y0_fp;
	int width = ox1 - ox0;
	int magnify;

	if (ww < 1)
		ww = 1;
	if (wh < 1)
		wh = 1;
	if (zoom_pct < 1)
		zoom_pct = BGCE_ZOOM_PCT_1X;

	step_x_fp = ((int64_t)100 * bw << 16) / ((int64_t)zoom_pct * ww);
	step_y_fp = ((int64_t)100 * bh << 16) / ((int64_t)zoom_pct * wh);

	{
		int64_t wx0 =
		        ((int64_t)ox0 + pan_x) * 100 / zoom_pct - cli_x;
		int64_t wy0 =
		        ((int64_t)oy0 + pan_y) * 100 / zoom_pct - cli_y;
		x0_fp = (wx0 * bw << 16) / ww;
		y0_fp = (wy0 * bh << 16) / wh;
	}

	magnify = (step_x_fp < 65536);

	if (magnify) {
		int32_t y_fp = (int32_t)y0_fp;
		int32_t step_y = (int32_t)step_y_fp;
		int32_t step_x = (int32_t)step_x_fp;
		int32_t x0 = (int32_t)x0_fp;
		for (int sy = oy0; sy < oy1; sy++) {
			int icy = y_fp >> 16;
			y_fp += step_y;
			if ((unsigned)icy >= (unsigned)bh)
				continue;

			uint32_t *drow = dst + sy * (int)screen_w + ox0;
			const uint32_t *srow = src + icy * bw;
			int32_t x_fp = x0;
			int sx = 0;
			while (sx < width) {
				int icx = x_fp >> 16;
				int run = 1;
				int32_t probe = x_fp + step_x;
				while (sx + run < width && (probe >> 16) == icx) {
					run++;
					probe += step_x;
				}
				if ((unsigned)icx < (unsigned)bw)
					fill_u32(drow + sx, run, srow[icx]);
				sx += run;
				x_fp = x0 + (int32_t)sx * step_x;
			}
		}
		return;
	}

	{
		int stack_map[2048];
		int *xmap = stack_map;
		int heap = 0;
		int32_t step_x = (int32_t)step_x_fp;
		int32_t step_y = (int32_t)step_y_fp;
		int32_t x0 = (int32_t)x0_fp;

		if (width > (int)(sizeof(stack_map) / sizeof(stack_map[0]))) {
			xmap = malloc((size_t)width * sizeof(int));
			if (!xmap)
				return;
			heap = 1;
		}
		{
			int32_t x_fp = x0;
			for (int i = 0; i < width; i++) {
				xmap[i] = x_fp >> 16;
				x_fp += step_x;
			}
		}

		{
			int32_t y_fp = (int32_t)y0_fp;
			for (int sy = oy0; sy < oy1; sy++) {
				int icy = y_fp >> 16;
				y_fp += step_y;
				if ((unsigned)icy >= (unsigned)bh)
					continue;

				uint32_t *drow = dst + sy * (int)screen_w + ox0;
				const uint32_t *srow = src + icy * bw;
				for (int i = 0; i < width; i++) {
					int icx = xmap[i];
					if ((unsigned)icx < (unsigned)bw)
						drow[i] = srow[icx];
				}
			}
		}
		if (heap)
			free(xmap);
	}
}

/*
 * Nearest-neighbour blit of a client into a screen-space damage rect.
 * Paths: 1) zoom 100% + world==buffer → memcpy  2) else 16.16 NN
 */
static void blit_client_overlap(struct ServerState *srv, const struct Client *cli,
                                int rx0, int ry0, int rx1, int ry1)
{
	int csx0, csy0, csx1, csy1;
	int ox0, oy0, ox1, oy1;
	uint32_t screen_w;
	uint32_t *dst;
	const uint32_t *src;
	int bw, bh, ww, wh, z;

	if (!srv || !srv->framebuffer || !cli || !cli->buffer)
		return;
	if (cli->width == 0 || cli->height == 0)
		return;

	client_screen_bounds(srv, cli, &csx0, &csy0, &csx1, &csy1);

	ox0 = rx0 > csx0 ? rx0 : csx0;
	oy0 = ry0 > csy0 ? ry0 : csy0;
	ox1 = rx1 < csx1 ? rx1 : csx1;
	oy1 = ry1 < csy1 ? ry1 : csy1;

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

	screen_w = display_stride_px(srv);
	dst = (uint32_t *)srv->framebuffer;
	src = (const uint32_t *)cli->buffer;
	bw = (int)cli->width;
	bh = (int)cli->height;
	ww = (int)(cli->world_w ? cli->world_w : cli->width);
	wh = (int)(cli->world_h ? cli->world_h : cli->height);
	z = zoom_pct_of(srv);

	/*
	 * Shared by MSG_DRAW, MOVE underlay/mover/above, pan edges, etc.
	 * Logs every actual FB blit so MOVE is visible even though it never
	 * calls draw().
	 */
	if (bgce_comp_debug()) {
		const char *app = cli->app_id[0] ? cli->app_id : "?";
		const char *path = (z == BGCE_ZOOM_PCT_1X && bw == ww && bh == wh)
		                           ? "1to1"
		                           : "scaled";

		printf("[BGCE] blit: id=%u app='%s' z=%d world=(%u,%u) %dx%d "
		       "clip=(%d,%d)-(%d,%d) %dx%d path=%s\n",
		       (unsigned)cli->id, app, (int)cli->z,
		       cli->x, cli->y, ww, wh,
		       ox0, oy0,
		       ox1 > ox0 ? ox1 - 1 : ox0,
		       oy1 > oy0 ? oy1 - 1 : oy0,
		       ox1 - ox0, oy1 - oy0, path);
		fflush(stdout);
	}

	if (z == BGCE_ZOOM_PCT_1X && bw == ww && bh == wh) {
		blit_client_1to1(dst, screen_w, src, bw, bh,
		                 (int)cli->x, (int)cli->y,
		                 ox0, oy0, ox1, oy1, srv->pan_x, srv->pan_y);
		return;
	}

	blit_client_scaled(dst, screen_w, src, bw, bh, ww, wh,
	                   (int)cli->x, (int)cli->y,
	                   ox0, oy0, ox1, oy1, srv->pan_x, srv->pan_y, z);
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

	/* Prefer stack for the common few-window case; avoid heap on every move. */
	struct Client *stack_local[32];
	struct Client **stack = stack_local;
	int heap = 0;
	int i = 0;

	if (n > (int)(sizeof(stack_local) / sizeof(stack_local[0]))) {
		stack = malloc((size_t)n * sizeof(*stack));
		if (!stack)
			return;
		heap = 1;
	}

	for (struct Client* c = first; c; c = c->next)
		stack[i++] = c;

	/* The linked-list is ordered top->bottom.
	 * For correct composition we must draw bottom->top so that higher windows
	 * overwrite lower ones (background is typically last in the chain).
	 */
	for (i = n - 1; i >= 0; i--) {
		blit_client_overlap(srv, stack[i], rx0, ry0, rx1, ry1);
	}

	if (heap)
		free(stack);
}

/* Clip [x0,x1)×[y0,y1) to the display; returns 0 if empty. */
static int clip_to_display(const struct ServerState *srv,
                           int *x0, int *y0, int *x1, int *y1)
{
	if (*x0 < 0)
		*x0 = 0;
	if (*y0 < 0)
		*y0 = 0;
	if (*x1 > (int)srv->display_w)
		*x1 = (int)srv->display_w;
	if (*y1 > (int)srv->display_h)
		*y1 = (int)srv->display_h;
	return *x0 < *x1 && *y0 < *y1;
}

/* ------------------------------------------------------------------
 * Visible-region helpers for MSG_DRAW (opaque windows).
 *
 * Subtract occluder [bx0,by0)×[bx1,by1) from subject [ax0,ay0)×[ax1,ay1).
 * Writes 0–4 non-empty remnants into out[]; returns count.
 * ------------------------------------------------------------------ */
static int rect_subtract(int ax0, int ay0, int ax1, int ay1,
                         int bx0, int by0, int bx1, int by1,
                         int out[][4], int out_max)
{
	int n = 0;
	int ix0, iy0, ix1, iy1;

	if (out_max < 1)
		return 0;
	/* No intersection → subject unchanged. */
	if (bx1 <= ax0 || bx0 >= ax1 || by1 <= ay0 || by0 >= ay1) {
		out[0][0] = ax0;
		out[0][1] = ay0;
		out[0][2] = ax1;
		out[0][3] = ay1;
		return 1;
	}
	/* Clip occluder to subject. */
	ix0 = bx0 > ax0 ? bx0 : ax0;
	iy0 = by0 > ay0 ? by0 : ay0;
	ix1 = bx1 < ax1 ? bx1 : ax1;
	iy1 = by1 < ay1 ? by1 : ay1;
	if (ix0 >= ix1 || iy0 >= iy1) {
		out[0][0] = ax0;
		out[0][1] = ay0;
		out[0][2] = ax1;
		out[0][3] = ay1;
		return 1;
	}
	/* Top strip */
	if (ay0 < iy0 && n < out_max) {
		out[n][0] = ax0;
		out[n][1] = ay0;
		out[n][2] = ax1;
		out[n][3] = iy0;
		n++;
	}
	/* Bottom strip */
	if (iy1 < ay1 && n < out_max) {
		out[n][0] = ax0;
		out[n][1] = iy1;
		out[n][2] = ax1;
		out[n][3] = ay1;
		n++;
	}
	/* Left strip (middle band only) */
	if (ax0 < ix0 && n < out_max) {
		out[n][0] = ax0;
		out[n][1] = iy0;
		out[n][2] = ix0;
		out[n][3] = iy1;
		n++;
	}
	/* Right strip (middle band only) */
	if (ix1 < ax1 && n < out_max) {
		out[n][0] = ix1;
		out[n][1] = iy0;
		out[n][2] = ax1;
		out[n][3] = iy1;
		n++;
	}
	return n;
}

#define DRAW_VIS_MAX 32

/*
 * Subtract one opaque occluder screen rect from a list of visible rects.
 * in_n rects in vis[][4]; result written back into vis, returns new count.
 */
static int vis_subtract_occluder(int vis[][4], int in_n,
                                 int ox0, int oy0, int ox1, int oy1)
{
	int tmp[DRAW_VIS_MAX][4];
	int out_n = 0;
	int i, j, k;
	int pieces[4][4];
	int np;

	for (i = 0; i < in_n; i++) {
		np = rect_subtract(vis[i][0], vis[i][1], vis[i][2], vis[i][3],
		                   ox0, oy0, ox1, oy1, pieces, 4);
		for (j = 0; j < np && out_n < DRAW_VIS_MAX; j++) {
			tmp[out_n][0] = pieces[j][0];
			tmp[out_n][1] = pieces[j][1];
			tmp[out_n][2] = pieces[j][2];
			tmp[out_n][3] = pieces[j][3];
			out_n++;
		}
	}
	for (k = 0; k < out_n; k++) {
		vis[k][0] = tmp[k][0];
		vis[k][1] = tmp[k][1];
		vis[k][2] = tmp[k][2];
		vis[k][3] = tmp[k][3];
	}
	return out_n;
}

void redraw_all(struct ServerState* srv) {
	if (!srv || !srv->framebuffer)
		return;
	cursor_fb_begin();
	(void)cursor_lift_all_ret();
	composite_chain_to_rect(srv, srv->clients, 0, 0,
	                        (int)srv->display_w, (int)srv->display_h);
	cursor_fb_end(1);
}

void erase_client(struct ServerState* srv, const struct Client* gone) {
	int sx0, sy0, sx1, sy1;
	int lifted;

	if (!srv || !srv->framebuffer || !gone)
		return;

	client_screen_bounds(srv, gone, &sx0, &sy0, &sx1, &sy1);
	if (!clip_to_display(srv, &sx0, &sy0, &sx1, &sy1))
		return;

	cursor_fb_begin();
	lifted = cursor_lift_for_rect_ret(sx0, sy0, sx1, sy1);
	/* Remaining clients only (caller already unlinked `gone`). */
	composite_chain_to_rect(srv, srv->clients, sx0, sy0, sx1, sy1);
	cursor_fb_end(lifted);
}

/*
 * Shift framebuffer pixels by (sdx, sdy) screen pixels.
 * Positive sdx/sdy move content right/down.
 */
static void fb_scroll(struct ServerState *srv, int sdx, int sdy)
{
	uint32_t *fb;
	uint32_t sp;
	int w, h, y;
	size_t row_bytes;

	if (!srv || !srv->framebuffer || (sdx == 0 && sdy == 0))
		return;

	fb = (uint32_t *)srv->framebuffer;
	sp = display_stride_px(srv);
	w = (int)srv->display_w;
	h = (int)srv->display_h;
	row_bytes = (size_t)w * sizeof(uint32_t);

	/* Vertical first (full-width rows). */
	if (sdy > 0 && sdy < h) {
		for (y = h - 1; y >= sdy; y--)
			memcpy(fb + y * (int)sp, fb + (y - sdy) * (int)sp, row_bytes);
	} else if (sdy < 0 && -sdy < h) {
		int ady = -sdy;
		for (y = 0; y < h - ady; y++)
			memcpy(fb + y * (int)sp, fb + (y + ady) * (int)sp, row_bytes);
	}

	/* Horizontal per row. */
	if (sdx > 0 && sdx < w) {
		size_t keep = (size_t)(w - sdx) * sizeof(uint32_t);
		for (y = 0; y < h; y++) {
			uint32_t *row = fb + y * (int)sp;
			memmove(row + sdx, row, keep);
		}
	} else if (sdx < 0 && -sdx < w) {
		int adx = -sdx;
		size_t keep = (size_t)(w - adx) * sizeof(uint32_t);
		for (y = 0; y < h; y++) {
			uint32_t *row = fb + y * (int)sp;
			memmove(row, row + adx, keep);
		}
	}
}

/*
 * Pan — pure integer screen scroll (pan is screen-pixel pan):
 *
 *   content moves by (sdx, sdy) on the FB  →  fb_scroll(sdx, sdy)
 *   pan_x -= sdx;  pan_y -= sdy            →  exact, no zoom division
 *   fill only the newly exposed strip(s)
 *
 * That identity is why pan must be screen pixels: world pan + zoom made
 * Δpan→sdx non-invertible and produced 1px staircase edges + trails when
 * zoomed out.  Clamp reduces sdx/sdy; never full-redraw as a “fallback”.
 */
void redraw_pan(struct ServerState *srv, int sdx, int sdy)
{
	int z, w, h;
	int max_x, max_y;

	if (!srv || !srv->framebuffer)
		return;
	if (sdx == 0 && sdy == 0)
		return;

	z = zoom_pct_of(srv);
	w = (int)srv->display_w;
	h = (int)srv->display_h;
	max_x = (int)srv->virtual_w * z / 100 - w;
	max_y = (int)srv->virtual_h * z / 100 - h;

	/* View larger than world: pan locked (centered). */
	if (max_x < 0)
		sdx = 0;
	if (max_y < 0)
		sdy = 0;

	/* sdx > 0: content right, pan decreases → stop at 0 */
	if (sdx > 0) {
		if (sdx > srv->pan_x)
			sdx = srv->pan_x;
	} else if (sdx < 0) {
		/* pan increases → stop at max_x */
		if (max_x >= 0 && srv->pan_x - sdx > max_x)
			sdx = srv->pan_x - max_x;
	}
	if (sdy > 0) {
		if (sdy > srv->pan_y)
			sdy = srv->pan_y;
	} else if (sdy < 0) {
		if (max_y >= 0 && srv->pan_y - sdy > max_y)
			sdy = srv->pan_y - max_y;
	}

	if (sdx == 0 && sdy == 0)
		return;

	srv->pan_x -= sdx;
	srv->pan_y -= sdy;

	cursor_fb_begin();
	(void)cursor_lift_all_ret();

	/* Whole view replaced — nothing left to copy. */
	if (sdx <= -w || sdx >= w || sdy <= -h || sdy >= h) {
		composite_chain_to_rect(srv, srv->clients, 0, 0, w, h);
		cursor_fb_end(1);
		return;
	}

	fb_scroll(srv, sdx, sdy);

	if (sdx > 0)
		composite_chain_to_rect(srv, srv->clients, 0, 0, sdx, h);
	else if (sdx < 0)
		composite_chain_to_rect(srv, srv->clients, w + sdx, 0, w, h);

	if (sdy > 0)
		composite_chain_to_rect(srv, srv->clients, 0, 0, w, sdy);
	else if (sdy < 0)
		composite_chain_to_rect(srv, srv->clients, 0, h + sdy, w, h);

	cursor_fb_end(1);
}

/*
 * MSG_DRAW — design (do not “fix” by full-stack recomposite):
 *
 * Original BGCE model: the client that asked to draw only updates *its own*
 * visible pixels.  We do NOT repaint wallpaper or windows below, and we do
 * NOT walk the whole stack bottom→top into the client’s rect (that caused
 * visible refill/blink and was a regression from this design).
 *
 * Algorithm (opaque windows; list is top → bottom):
 *   1. Start with the drawer’s screen bounds as the visible region.
 *   2. Walk clients from the head (topmost) until we reach `cli`.
 *      Each window above subtracts its screen rect from the visible region.
 *   3. When we arrive at `cli`, blit only the remaining rects from its buffer.
 *   4. Windows above are never overwritten; windows below are never touched.
 *
 * Use composite_chain_to_rect / erase_client when underlay must change
 * (unmap, move expose, pan edges, zoom) — not for ordinary MSG_DRAW.
 *
 * Cursor: lift only if damage hits the glyph; input thread re-presents.
 */
void draw(struct ServerState *srv, struct Client *cli)
{
	int sx0, sy0, sx1, sy1;
	int vis[DRAW_VIS_MAX][4];
	int nvis;
	int lifted = 0;
	int i;
	struct Client *c;
	uint32_t ww, wh;
	const char *app;

	if (!srv || !srv->framebuffer || !cli || !cli->buffer) {
		fprintf(stderr,
		        "[BGCE] Draw: Invalid server, framebuffer, or client buffer\n");
		return;
	}

	client_screen_bounds(srv, cli, &sx0, &sy0, &sx1, &sy1);
	if (!clip_to_display(srv, &sx0, &sy0, &sx1, &sy1)) {
		if (bgce_comp_debug()) {
			printf("[BGCE] draw: id=%u app='%s' off-screen, skip "
			       "world=(%u,%u)\n",
			       (unsigned)cli->id,
			       cli->app_id[0] ? cli->app_id : "?",
			       cli->x, cli->y);
			fflush(stdout);
		}
		return;
	}

	/* Visible pieces of this client after subtracting occluders above. */
	vis[0][0] = sx0;
	vis[0][1] = sy0;
	vis[0][2] = sx1;
	vis[0][3] = sy1;
	nvis = 1;

	for (c = srv->clients; c; c = c->next) {
		int cx0, cy0, cx1, cy1;

		if (c == cli)
			break;
		/* Skip non-drawable entries (e.g. empty); still occlude if they have size. */
		client_screen_bounds(srv, c, &cx0, &cy0, &cx1, &cy1);
		if (!clip_to_display(srv, &cx0, &cy0, &cx1, &cy1))
			continue;
		if (bgce_comp_debug()) {
			printf("[BGCE] draw: occlude by id=%u app='%s' "
			       "screen=(%d,%d)-(%d,%d) %dx%d\n",
			       (unsigned)c->id,
			       c->app_id[0] ? c->app_id : "?",
			       cx0, cy0,
			       cx1 > cx0 ? cx1 - 1 : cx0,
			       cy1 > cy0 ? cy1 - 1 : cy0,
			       cx1 - cx0, cy1 - cy0);
			fflush(stdout);
		}
		nvis = vis_subtract_occluder(vis, nvis, cx0, cy0, cx1, cy1);
		if (nvis <= 0) {
			if (bgce_comp_debug()) {
				printf("[BGCE] draw: id=%u app='%s' fully covered, "
				       "skip blit\n",
				       (unsigned)cli->id,
				       cli->app_id[0] ? cli->app_id : "?");
				fflush(stdout);
			}
			return; /* fully covered by windows above */
		}
	}

	if (!c) {
		/* Drawer not in the list — should not happen; refuse full-stack fallback. */
		fprintf(stderr, "[BGCE] Draw: client not in stack, skip\n");
		return;
	}

	ww = cli->world_w ? cli->world_w : cli->width;
	wh = cli->world_h ? cli->world_h : cli->height;
	app = cli->app_id[0] ? cli->app_id : "?";

	if (bgce_comp_debug()) {
		printf("[BGCE] draw: blit id=%u app='%s' world=(%u,%u) %ux%u "
		       "bounds=(%d,%d)-(%d,%d) %dx%d nvis=%d z=%d\n",
		       (unsigned)cli->id, app, cli->x, cli->y, ww, wh,
		       sx0, sy0,
		       sx1 > sx0 ? sx1 - 1 : sx0,
		       sy1 > sy0 ? sy1 - 1 : sy0,
		       sx1 - sx0, sy1 - sy0,
		       nvis, (int)cli->z);
		for (i = 0; i < nvis; i++) {
			int px0 = vis[i][0], py0 = vis[i][1];
			int px1 = vis[i][2], py1 = vis[i][3];

			printf("[BGCE] draw:   piece[%d] "
			       "screen=(%d,%d)-(%d,%d) %dx%d\n",
			       i, px0, py0,
			       px1 > px0 ? px1 - 1 : px0,
			       py1 > py0 ? py1 - 1 : py0,
			       px1 - px0, py1 - py0);
		}
		fflush(stdout);
	}

	cursor_fb_begin();
	for (i = 0; i < nvis; i++) {
		if (cursor_lift_for_rect_ret(vis[i][0], vis[i][1],
		                             vis[i][2], vis[i][3]))
			lifted = 1;
	}
	for (i = 0; i < nvis; i++) {
		if (bgce_comp_debug()) {
			bgce_comp_damage_log("draw-blit", vis[i][0], vis[i][1],
			                     vis[i][2], vis[i][3], -1);
		}
		blit_client_overlap(srv, cli, vis[i][0], vis[i][1],
		                    vis[i][2], vis[i][3]);
	}
	cursor_fb_end(lifted);

	if (bgce_comp_debug()) {
		printf("[BGCE] draw: done id=%u app='%s' pieces=%d lifted_cursor=%d\n",
		       (unsigned)cli->id, app, nvis, lifted);
		fflush(stdout);
	}
}

/*
 * Paint every client except `skip` into a screen rect, bottom → top.
 * Used for move expose so wallpaper (and true underlay) always runs even
 * if list topology is odd — never leave the mover’s old pixels in the trail.
 */
static void composite_all_except(struct ServerState *srv,
                                 const struct Client *skip,
                                 int x0, int y0, int x1, int y1)
{
	struct Client *stack_local[64];
	struct Client **stack = stack_local;
	struct Client *c;
	int n = 0, i, heap = 0;

	if (!clip_to_display(srv, &x0, &y0, &x1, &y1))
		return;

	for (c = srv->clients; c; c = c->next) {
		if (c == skip || !c->buffer)
			continue;
		n++;
	}
	if (n <= 0)
		return;
	if (n > (int)(sizeof(stack_local) / sizeof(stack_local[0]))) {
		stack = malloc((size_t)n * sizeof(*stack));
		if (!stack)
			return;
		heap = 1;
	}
	i = 0;
	for (c = srv->clients; c; c = c->next) {
		if (c == skip || !c->buffer)
			continue;
		stack[i++] = c;
	}
	if (bgce_comp_debug()) {
		printf("[BGCE] underlay-stack: %d layer(s) into "
		       "screen=(%d,%d)-(%d,%d) %dx%d skip_id=%u\n",
		       n, x0, y0,
		       x1 > x0 ? x1 - 1 : x0,
		       y1 > y0 ? y1 - 1 : y0,
		       x1 - x0, y1 - y0,
		       skip ? (unsigned)skip->id : 0);
		for (i = n - 1; i >= 0; i--) {
			printf("[BGCE] underlay-stack:   [%d] id=%u app='%s' "
			       "z=%d world=(%u,%u)\n",
			       n - 1 - i, (unsigned)stack[i]->id,
			       stack[i]->app_id[0] ? stack[i]->app_id : "?",
			       (int)stack[i]->z, stack[i]->x, stack[i]->y);
		}
		fflush(stdout);
	}
	/* stack[0] is topmost among remaining; paint bottom → top. */
	for (i = n - 1; i >= 0; i--)
		blit_client_overlap(srv, stack[i], x0, y0, x1, y1);
	if (heap)
		free(stack);
}

/*
 * Half-open rectangle: pixels x in [x0, x1), y in [y0, y1).
 * Used for move damage (world and screen space).
 */
struct rect {
	int x0, y0, x1, y1;
};

#define MOVE_TRAIL_MAX 4

static struct rect rect_make(int x0, int y0, int x1, int y1)
{
	struct rect r = { x0, y0, x1, y1 };
	return r;
}

static int rect_empty(struct rect r)
{
	return r.x0 >= r.x1 || r.y0 >= r.y1;
}

static struct rect rect_union(struct rect a, struct rect b)
{
	return rect_make(a.x0 < b.x0 ? a.x0 : b.x0,
	                 a.y0 < b.y0 ? a.y0 : b.y0,
	                 a.x1 > b.x1 ? a.x1 : b.x1,
	                 a.y1 > b.y1 ? a.y1 : b.y1);
}

static struct rect rect_grow1(struct rect r, int max_x, int max_y)
{
	if (r.x0 > 0)
		r.x0--;
	if (r.y0 > 0)
		r.y0--;
	if (r.x1 < max_x)
		r.x1++;
	if (r.y1 < max_y)
		r.y1++;
	return r;
}

/* Clip to display; returns empty rect if nothing remains. */
static struct rect rect_clip(const struct ServerState *srv, struct rect r)
{
	if (r.x0 < 0)
		r.x0 = 0;
	if (r.y0 < 0)
		r.y0 = 0;
	if (r.x1 > (int)srv->display_w)
		r.x1 = (int)srv->display_w;
	if (r.y1 > (int)srv->display_h)
		r.y1 = (int)srv->display_h;
	if (rect_empty(r))
		return rect_make(0, 0, 0, 0);
	return r;
}

/* Same ceil mapping as client_screen_bounds. */
static struct rect rect_world_to_screen(const struct ServerState *srv,
                                       struct rect world)
{
	int z = zoom_pct_of(srv);
	struct rect s;

	s.x0 = (world.x0 * z + 99) / 100 - srv->pan_x;
	s.y0 = (world.y0 * z + 99) / 100 - srv->pan_y;
	s.x1 = (world.x1 * z + 99) / 100 - srv->pan_x;
	s.y1 = (world.y1 * z + 99) / 100 - srv->pan_y;
	if (s.x1 <= s.x0)
		s.x1 = s.x0 + 1;
	if (s.y1 <= s.y0)
		s.y1 = s.y0 + 1;
	return s;
}

/* a \ b → up to 4 pieces in out[]; returns count. */
static int rect_minus(struct rect a, struct rect b,
                      struct rect *out, int out_max)
{
	int raw[4][4];
	int n, i;

	n = rect_subtract(a.x0, a.y0, a.x1, a.y1,
	                  b.x0, b.y0, b.x1, b.y1, raw, out_max < 4 ? out_max : 4);
	for (i = 0; i < n; i++)
		out[i] = rect_make(raw[i][0], raw[i][1], raw[i][2], raw[i][3]);
	return n;
}

static void rect_log(const char *tag, struct rect r)
{
	if (!bgce_comp_debug() || rect_empty(r))
		return;
	printf("[BGCE] move: %s [%d,%d)×[%d,%d) %dx%d\n",
	       tag, r.x0, r.x1, r.y0, r.y1, r.x1 - r.x0, r.y1 - r.y0);
	fflush(stdout);
}

/*
 * Worker args: paint one screen rect (underlay stack or mover blit).
 * Lifetime: stack of redraw_region until parallel join returns.
 */
struct move_paint_job {
	struct ServerState *srv;
	const struct Client *client; /* mover, or skip-id for underlay */
	struct rect area;
	int is_underlay; /* 1 = composite everyone except client */
};

static void move_paint_worker(void *arg)
{
	struct move_paint_job *job = arg;

	bgce_comp_damage_log(job->is_underlay ? "underlay" : "mover",
	                     job->area.x0, job->area.y0,
	                     job->area.x1, job->area.y1, -1);
	if (job->is_underlay)
		composite_all_except(job->srv, job->client,
		                     job->area.x0, job->area.y0,
		                     job->area.x1, job->area.y1);
	else
		blit_client_overlap(job->srv, job->client,
		                    job->area.x0, job->area.y0,
		                    job->area.x1, job->area.y1);
}

/*
 * Window move paint.  Does not read or write c->x/y — only buffer/stack id.
 *
 *   trail_screen = old_screen \ new_screen   (what the FB must lose)
 *   underlay each trail strip (wallpaper + every client except the mover)
 *   blit mover on new_screen only
 *   re-blit windows above over old_screen ∪ new_screen
 *
 * Example: (1000,500) 200×200, left 1 → (999,500) at 100% zoom:
 *   old_screen [1000,1200)×[500,700)
 *   new_screen [999,1199)×[500,700)
 *   trail      [1199,1200)×[500,700)  underlay 1×200
 *   mover      [999,1199)×[500,700)   200×200
 */
void redraw_region(struct ServerState *srv, struct Client *c,
                   int old_x, int old_y, int new_x, int new_y)
{
	struct Client at_new;
	struct Client *above;
	struct rect old_world, new_world;
	struct rect old_screen, new_screen;
	struct rect trail[MOVE_TRAIL_MAX];
	struct move_paint_job underlay_job[2];
	struct move_paint_job mover_job;
	int width, height;
	int n_trail, i;
	int z1x;

	if (!srv || !srv->framebuffer || !c) {
		fprintf(stderr, "[BGCE] Redraw: Invalid server, framebuffer, or client\n");
		return;
	}
	if (old_x == new_x && old_y == new_y)
		return;

	width = (int)(c->world_w ? c->world_w : c->width);
	height = (int)(c->world_h ? c->world_h : c->height);
	if (width < 1 || height < 1)
		return;

	old_world = rect_make(old_x, old_y, old_x + width, old_y + height);
	new_world = rect_make(new_x, new_y, new_x + width, new_y + height);
	old_screen = rect_world_to_screen(srv, old_world);
	new_screen = rect_world_to_screen(srv, new_world);

	/* Snapshot for blit only — live c->x may already be further ahead. */
	at_new = *c;
	at_new.x = (uint32_t)new_x;
	at_new.y = (uint32_t)new_y;

	cursor_fb_begin();
	(void)cursor_lift_all_ret();

	/* Zoomed out: integer screen rect may not move; still re-sample. */
	if (old_screen.x0 == new_screen.x0 && old_screen.y0 == new_screen.y0 &&
	    old_screen.x1 == new_screen.x1 && old_screen.y1 == new_screen.y1) {
		struct rect area = rect_clip(srv, new_screen);

		if (!rect_empty(area)) {
			bgce_comp_damage_log("mover-same", area.x0, area.y0,
			                     area.x1, area.y1, -1);
			blit_client_overlap(srv, &at_new,
			                    area.x0, area.y0, area.x1, area.y1);
			for (above = srv->clients; above && above != c;
			     above = above->next)
				blit_client_overlap(srv, above,
				                    area.x0, area.y0,
				                    area.x1, area.y1);
		}
		cursor_fb_end(1);
		return;
	}

	/*
	 * Trail in screen space: pixels that were in the old footprint but
	 * are not in the new one.  Same rule the previous frame used to paint.
	 */
	n_trail = rect_minus(old_screen, new_screen, trail, MOVE_TRAIL_MAX);
	z1x = (zoom_pct_of(srv) == BGCE_ZOOM_PCT_1X);

	/* Non-1×: pad then cut new out to cover ceil/sample gaps. */
	if (!z1x && n_trail > 0) {
		struct rect padded_bits[MOVE_TRAIL_MAX];
		int n_pad = 0;

		for (i = 0; i < n_trail && n_pad < MOVE_TRAIL_MAX; i++) {
			struct rect pieces[MOVE_TRAIL_MAX];
			struct rect grown = rect_grow1(trail[i],
			                               (int)srv->display_w,
			                               (int)srv->display_h);
			int np = rect_minus(grown, new_screen, pieces,
			                    MOVE_TRAIL_MAX);
			int j;

			for (j = 0; j < np && n_pad < MOVE_TRAIL_MAX; j++) {
				struct rect cl = rect_clip(srv, pieces[j]);

				if (!rect_empty(cl))
					padded_bits[n_pad++] = cl;
			}
		}
		for (i = 0; i < n_pad; i++)
			trail[i] = padded_bits[i];
		n_trail = n_pad;
	}

	for (i = 0; i < n_trail; i++)
		trail[i] = rect_clip(srv, trail[i]);

	if (bgce_comp_debug()) {
		printf("[BGCE] move: (%d,%d)->(%d,%d) size=%dx%d\n",
		       old_x, old_y, new_x, new_y, width, height);
		rect_log("old_screen", old_screen);
		rect_log("new_screen", new_screen);
		for (i = 0; i < n_trail; i++)
			rect_log("trail", trail[i]);
	}

	/* Underlay trail (parallel strips), then mover — no shared column. */
	{
		void (*fn0)(void *) = NULL, (*fn1)(void *) = NULL;
		void *arg0 = NULL, *arg1 = NULL;

		if (n_trail > 0 && !rect_empty(trail[0])) {
			underlay_job[0].srv = srv;
			underlay_job[0].client = c; /* skip real list node */
			underlay_job[0].area = trail[0];
			underlay_job[0].is_underlay = 1;
			fn0 = move_paint_worker;
			arg0 = &underlay_job[0];
		}
		if (n_trail > 1 && !rect_empty(trail[1])) {
			underlay_job[1].srv = srv;
			underlay_job[1].client = c;
			underlay_job[1].area = trail[1];
			underlay_job[1].is_underlay = 1;
			fn1 = move_paint_worker;
			arg1 = &underlay_job[1];
		}
		bgce_comp_parallel3(fn0, arg0, fn1, arg1, NULL, NULL);

		for (i = 2; i < n_trail; i++) {
			if (rect_empty(trail[i]))
				continue;
			underlay_job[0].area = trail[i];
			move_paint_worker(&underlay_job[0]);
		}
	}

	{
		struct rect mover_area = rect_clip(srv, new_screen);

		if (!rect_empty(mover_area)) {
			mover_job.srv = srv;
			mover_job.client = &at_new;
			mover_job.area = mover_area;
			mover_job.is_underlay = 0;
			move_paint_worker(&mover_job);
		}
	}

	/* Windows above the mover (list is top → bottom). */
	for (above = srv->clients; above && above != c; above = above->next) {
		struct rect cover;

		if (!above->buffer)
			continue;
		cover = rect_clip(srv, rect_union(old_screen, new_screen));
		if (!z1x)
			cover = rect_clip(srv,
			                  rect_grow1(cover, (int)srv->display_w,
			                             (int)srv->display_h));
		if (rect_empty(cover))
			break;
		bgce_comp_damage_log("above", cover.x0, cover.y0,
		                     cover.x1, cover.y1, -1);
		for (; above && above != c; above = above->next)
			blit_client_overlap(srv, above,
			                    cover.x0, cover.y0,
			                    cover.x1, cover.y1);
		break;
	}

	cursor_fb_end(1);
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
	/* dx/dy are world-size deltas; bounds use world_* only. */
	{
		uint32_t ww = c.world_w ? c.world_w : c.width;
		uint32_t wh = c.world_h ? c.world_h : c.height;
		old.world_w = (uint32_t)((int)ww - dx);
		old.world_h = (uint32_t)((int)wh - dy);
	}

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
	{
		int lifted;
		cursor_fb_begin();
		lifted = cursor_lift_for_rect_ret(ux0, uy0, ux1, uy1);
		if (ux0 < ux1 && uy0 < uy1)
			composite_chain_to_rect(srv, c.next, ux0, uy0, ux1, uy1);
		cursor_fb_end(lifted);
	}
}

int take_screenshot(const char* filename) {
	uint32_t width, height, stride;
	int result;

	if (!server.framebuffer) {
		fprintf(stderr, "[BGCE] No framebuffer available for screenshot.\n");
		return -1;
	}

	/* Don't bake the software cursor into the PNG. */
	cursor_fb_begin();
	(void)cursor_lift_all_ret();

	width = server.display_w;
	height = server.display_h;
	stride = server.display_pitch ? server.display_pitch
	                              : width * BGCE_BYTES_PER_PIXEL;

	result = stbi_write_png(
		filename,
		(int)width,
		(int)height,
		BGCE_BYTES_PER_PIXEL,
		server.framebuffer,
		(int)stride
	);

	cursor_fb_end(1);
	/* Re-present immediately; screenshot runs on input/server path. */
	display_cursor_present();

	if (!result) {
		fprintf(stderr, "[BGCE] Failed to save screenshot to %s.\n", filename);
		return -1;
	}

	printf("[BGCE] Screenshot saved to %s.\n", filename);
	return 0;
}
