#ifndef BGCE_MOCK_H
#define BGCE_MOCK_H

/*
 * Headless / mock compositor API (same idea as BGTK's bgtk_init_mock).
 *
 * Build:  make headless
 * Run:    ./headless
 *
 * No /dev/fb0, DRM, VT, or real input.  In-memory framebuffer + PNG dumps
 * for visual regression tests.
 */

#include "server.h"

#include <stdint.h>

/** Start mock server: malloc FB, virtual desktop, solid background. */
int bgce_mock_init(uint32_t width, uint32_t height);

/** Tear down mock (frees clients and FB). */
void bgce_mock_fini(void);

/**
 * Add a client window with its own buffer (ARGB), filled with solid color.
 * x,y,w,h are in world pixels.  Returns the client, or NULL.
 * Sets app_id to mock_N; override c->app_id before draw to test location cache.
 */
struct Client *bgce_mock_add_client(uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h, uint32_t argb);

/** Fill client buffer with solid color and redraw (respects stacking). */
void bgce_mock_fill_client(struct Client *c, uint32_t argb);

/** MSG_DRAW equivalent: recompose this client's screen rect. */
void bgce_mock_draw(struct Client *c);

/** Remove client and erase its footprint. */
void bgce_mock_remove_client(struct Client *c);

/** Raise + focus like a click (list reorder + draw). */
void bgce_mock_focus(struct Client *c);

/** Click at screen coords (pick top client, focus if any). */
struct Client *bgce_mock_click(int screen_x, int screen_y);

/** Move client by world delta (L-shaped damage). */
void bgce_mock_move(struct Client *c, int wdx, int wdy);

/** Pan viewport by screen-pixel mouse delta. */
void bgce_mock_pan_screen(int sdx, int sdy);

/** Set absolute zoom percent (50–400, 100 = 1×) toward screen point. */
void bgce_mock_zoom_to(int zoom_pct, int sx, int sy);

/** Set absolute zoom percent and screen-pixel pan (clamped), then redraw. */
void bgce_mock_set_viewport(int zoom_pct, int pan_x, int pan_y);

/**
 * Alt+Tab (reverse=0) / Alt+Shift+Tab (reverse=1): cycle focus, raise the
 * target, and restore its cached viewport (zoom + pan).
 */
void bgce_mock_alt_tab(int reverse);

/**
 * Snapshot focused client's position + current viewport into the location
 * cache (same as move-end / disconnect).  Desktop pan alone never does this.
 */
void bgce_mock_remember_focus(void);

/** Dump framebuffer to PNG (cursor omitted). path NULL → mock_shot.png */
int bgce_mock_screenshot(const char *path);

/** Full redraw (background + all clients). */
void bgce_mock_redraw_all(void);

/**
 * Snapshot the FB, run a full redraw, compare.  Returns 0 if identical,
 * -1 if any pixel differs (FB left as after redraw_all).  Used to catch
 * move damage holes / edge rounding bugs.
 */
int bgce_mock_fb_matches_full_redraw(void);

#endif /* BGCE_MOCK_H */
