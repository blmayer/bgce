/*
 * Headless mock test for BGCE (same workflow as BGTK's test/headless.c).
 *
 * Build:  make headless
 * Run:    ./headless
 *
 * Writes headless_*.png for visual inspection.  No real display or input.
 */

#include "location_cache.h"
#include "mock.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Extern server state (owned by mock.c) for assertions. */
extern struct ServerState server;

int main(void)
{
	struct Client *a, *b, *c;
	char cache_home[] = "/tmp/bgce_headless_cache_XXXXXX";

	/* Isolate window cache so tests never touch ~/.cache. */
	if (!mkdtemp(cache_home)) {
		perror("headless: mkdtemp");
		return 1;
	}
	setenv("XDG_CACHE_HOME", cache_home, 1);

	if (bgce_mock_init(640, 480) != 0) {
		fprintf(stderr, "headless: mock init failed\n");
		return 1;
	}

	/* Red desktop only */
	bgce_mock_redraw_all();
	if (bgce_mock_screenshot("headless_00_bg.png") != 0)
		return 1;

	/* Two overlapping solid windows */
	a = bgce_mock_add_client(40, 40, 200, 150, 0xFFE74C3C); /* red */
	b = bgce_mock_add_client(120, 100, 220, 160, 0xFF3498DB); /* blue on top */
	if (!a || !b) {
		fprintf(stderr, "headless: add_client failed\n");
		bgce_mock_fini();
		return 1;
	}
	bgce_mock_draw(a);
	bgce_mock_draw(b);
	if (bgce_mock_screenshot("headless_01_two_windows.png") != 0)
		return 1;

	/* Redraw of the bottom window must NOT raise it over blue */
	bgce_mock_fill_client(a, 0xFFF39C12); /* orange */
	if (bgce_mock_screenshot("headless_02_bottom_redraw.png") != 0)
		return 1;

	/* Click focuses red (under blue? pick top — blue still top at 120,100) */
	c = bgce_mock_click(50, 50); /* should hit orange/red a */
	if (c != a)
		fprintf(stderr, "headless: expected click on A (got %p)\n", (void *)c);
	bgce_mock_draw(a);
	if (bgce_mock_screenshot("headless_03_focus_a.png") != 0)
		return 1;

	/* Move A; blue stays put */
	bgce_mock_move(a, 80, 40);
	if (bgce_mock_screenshot("headless_04_moved_a.png") != 0)
		return 1;

	/* Disconnect B — only its footprint erased */
	bgce_mock_remove_client(b);
	b = NULL;
	if (bgce_mock_screenshot("headless_05_removed_b.png") != 0)
		return 1;

	/* Zoom out and pan */
	bgce_mock_zoom_to(50, 320, 240);
	if (bgce_mock_screenshot("headless_06_zoomed_out.png") != 0)
		return 1;
	bgce_mock_pan_screen(40, 30);
	if (bgce_mock_screenshot("headless_07_panned.png") != 0)
		return 1;

	/* Location cache: remember A, reopen with same app_id */
	{
		uint32_t saved_x, saved_y, cx = 0, cy = 0;

		strncpy(a->app_id, "test_app", sizeof(a->app_id) - 1);
		a->app_id[sizeof(a->app_id) - 1] = '\0';
		bgce_mock_move(a, 50, 30); /* stores cache on move */
		saved_x = a->x;
		saved_y = a->y;
		bgce_mock_remove_client(a);
		a = NULL;

		a = bgce_mock_add_client(0, 0, 200, 150, 0xFF2ECC71);
		if (!a) {
			fprintf(stderr, "headless: re-add client failed\n");
			bgce_mock_fini();
			return 1;
		}
		strncpy(a->app_id, "test_app", sizeof(a->app_id) - 1);
		a->app_id[sizeof(a->app_id) - 1] = '\0';
		/* Same path as server first-buffer placement */
		if (location_cache_lookup("test_app", &cx, &cy)) {
			a->x = cx;
			a->y = cy;
		}
		bgce_mock_draw(a);
		if (a->x != saved_x || a->y != saved_y) {
			fprintf(stderr,
			        "headless: location cache failed (got %u,%u want %u,%u)\n",
			        a->x, a->y, saved_x, saved_y);
			bgce_mock_fini();
			return 1;
		}
		if (bgce_mock_screenshot("headless_08_location_restored.png") != 0)
			return 1;
	}

	/*
	 * Alt+Tab: each app remembers its own viewport.  Switching focus pans
	 * and zooms back to how that app was left — it does not move windows.
	 */
	{
		int zoom_a, pan_x_a, pan_y_a;
		int zoom_b, pan_x_b, pan_y_b;
		int zoom_chk, pan_x_chk, pan_y_chk;
		uint32_t ax, ay, bx, by;

		bgce_mock_fini();
		if (bgce_mock_init(640, 480) != 0) {
			fprintf(stderr, "headless: re-init failed\n");
			return 1;
		}

		/* A near world origin; B deeper on the virtual desktop. */
		a = bgce_mock_add_client(40, 40, 180, 120, 0xFFE74C3C);
		b = bgce_mock_add_client(500, 350, 180, 120, 0xFF3498DB);
		if (!a || !b) {
			fprintf(stderr, "headless: alt-tab clients failed\n");
			bgce_mock_fini();
			return 1;
		}
		strncpy(a->app_id, "alt_a", sizeof(a->app_id) - 1);
		a->app_id[sizeof(a->app_id) - 1] = '\0';
		strncpy(b->app_id, "alt_b", sizeof(b->app_id) - 1);
		b->app_id[sizeof(b->app_id) - 1] = '\0';
		bgce_mock_draw(a);
		bgce_mock_draw(b);

		/*
		 * Viewport for A: zoomed in near the origin so A is large and
		 * sits toward the top-left of the screen (pan 0,0).
		 */
		bgce_mock_focus(a);
		bgce_mock_set_viewport(200, 0, 0);
		zoom_a = server.zoom_pct;
		pan_x_a = server.pan_x;
		pan_y_a = server.pan_y;
		ax = a->x;
		ay = a->y;
		bgce_mock_remember_focus();
		if (bgce_mock_screenshot("headless_09_alt_a_viewport.png") != 0)
			return 1;

		/*
		 * Viewport for B: 1:1 zoom, panned so B (world 500,350) lands
		 * near the middle of the 640×480 screen.
		 *   sx = 500 - pan_x ≈ 220 → pan_x = 280
		 *   sy = 350 - pan_y ≈ 180 → pan_y = 170
		 */
		bgce_mock_focus(b);
		bgce_mock_set_viewport(100, 280, 170);
		zoom_b = server.zoom_pct;
		pan_x_b = server.pan_x;
		pan_y_b = server.pan_y;
		bx = b->x;
		by = b->y;
		bgce_mock_remember_focus();
		if (bgce_mock_screenshot("headless_10_alt_b_viewport.png") != 0)
			return 1;

		/*
		 * Pure pan must not rewrite the cache.  After panning away from B's
		 * remembered viewport, the stored entry is still the old one.
		 */
		bgce_mock_pan_screen(100, 50);
		if (!location_cache_lookup_viewport("alt_b", &zoom_chk, &pan_x_chk,
		                                    &pan_y_chk) ||
		    zoom_chk != zoom_b || pan_x_chk != pan_x_b ||
		    pan_y_chk != pan_y_b) {
			fprintf(stderr,
			        "headless: pan updated cache (got zoom=%d pan=%d,%d "
			        "want %d %d,%d)\n",
			        zoom_chk, pan_x_chk, pan_y_chk, zoom_b, pan_x_b,
			        pan_y_b);
			bgce_mock_fini();
			return 1;
		}
		if (bgce_mock_screenshot("headless_11_panned_no_cache.png") != 0)
			return 1;

		/*
		 * Put the live camera back on B's remembered viewport (without
		 * re-writing the cache) so Alt+Tab leave still records the same
		 * values we assert later.
		 */
		bgce_mock_set_viewport(zoom_b, pan_x_b, pan_y_b);

		/* Stacking: B is focused on top.  Alt+Tab → A, restore A's view. */
		if (server.focused_client != b) {
			fprintf(stderr, "headless: expected focus on B before alt-tab\n");
			bgce_mock_fini();
			return 1;
		}
		bgce_mock_alt_tab(0);
		if (server.focused_client != a) {
			fprintf(stderr, "headless: Alt+Tab did not focus A\n");
			bgce_mock_fini();
			return 1;
		}
		if (server.clients != a) {
			fprintf(stderr, "headless: Alt+Tab did not raise A\n");
			bgce_mock_fini();
			return 1;
		}
		if (server.zoom_pct != zoom_a || server.pan_x != pan_x_a ||
		    server.pan_y != pan_y_a) {
			fprintf(stderr,
			        "headless: Alt+Tab viewport for A wrong "
			        "(zoom=%d pan=%d,%d want %d %d,%d)\n",
			        server.zoom_pct, server.pan_x, server.pan_y, zoom_a,
			        pan_x_a, pan_y_a);
			bgce_mock_fini();
			return 1;
		}
		/* Window positions must not change — only the camera moves. */
		if (a->x != ax || a->y != ay || b->x != bx || b->y != by) {
			fprintf(stderr, "headless: Alt+Tab moved a window\n");
			bgce_mock_fini();
			return 1;
		}
		if (bgce_mock_screenshot("headless_12_alt_tab_to_a.png") != 0)
			return 1;

		/* Alt+Tab again → B with B's viewport. */
		bgce_mock_alt_tab(0);
		if (server.focused_client != b) {
			fprintf(stderr, "headless: Alt+Tab did not return to B\n");
			bgce_mock_fini();
			return 1;
		}
		if (server.zoom_pct != zoom_b || server.pan_x != pan_x_b ||
		    server.pan_y != pan_y_b) {
			fprintf(stderr,
			        "headless: Alt+Tab viewport for B wrong "
			        "(zoom=%d pan=%d,%d want %d %d,%d)\n",
			        server.zoom_pct, server.pan_x, server.pan_y, zoom_b,
			        pan_x_b, pan_y_b);
			bgce_mock_fini();
			return 1;
		}
		if (bgce_mock_screenshot("headless_13_alt_tab_to_b.png") != 0)
			return 1;

		/* Alt+Shift+Tab cycles backwards → A again. */
		bgce_mock_alt_tab(1);
		if (server.focused_client != a) {
			fprintf(stderr, "headless: Alt+Shift+Tab did not focus A\n");
			bgce_mock_fini();
			return 1;
		}
		if (server.zoom_pct != zoom_a || server.pan_x != pan_x_a ||
		    server.pan_y != pan_y_a) {
			fprintf(stderr,
			        "headless: Alt+Shift+Tab viewport for A wrong\n");
			bgce_mock_fini();
			return 1;
		}
		if (bgce_mock_screenshot("headless_14_alt_shift_tab.png") != 0)
			return 1;
	}

	/*
	 * Slow multi-step moves at several zooms must match a full recomposite.
	 * Catches edge rounding / skipped redraw when the screen rect is stable
	 * under a 1-world-pixel step (common when zoomed out).
	 */
	{
		int zooms[] = { 50, 100, 150, 200 };
		int zi, step;
		int fails = 0;

		bgce_mock_fini();
		if (bgce_mock_init(640, 480) != 0) {
			fprintf(stderr, "headless: move-edge re-init failed\n");
			return 1;
		}

		a = bgce_mock_add_client(80, 60, 160, 100, 0xFFE67E22);
		b = bgce_mock_add_client(200, 140, 140, 90, 0xFF8E44AD);
		if (!a || !b) {
			fprintf(stderr, "headless: move-edge clients failed\n");
			bgce_mock_fini();
			return 1;
		}
		/* Skip location-cache I/O during pixel-step stress. */
		a->app_id[0] = '\0';
		b->app_id[0] = '\0';
		bgce_mock_draw(a);
		bgce_mock_draw(b);

		for (zi = 0; zi < (int)(sizeof(zooms) / sizeof(zooms[0])); zi++) {
			bgce_mock_set_viewport(zooms[zi], 0, 0);
			bgce_mock_focus(a);
			/* Walk right/down one world pixel at a time. */
			for (step = 0; step < 40; step++) {
				bgce_mock_move(a, 1, (step % 3 == 0) ? 1 : 0);
				if (bgce_mock_fb_matches_full_redraw() != 0) {
					fprintf(stderr,
					        "headless: move damage mismatch "
					        "zoom=%d%% step=%d pos=%u,%u\n",
					        zooms[zi], step, a->x, a->y);
					fails++;
					break;
				}
			}
			/* Diagonal-ish reverse */
			for (step = 0; step < 25; step++) {
				bgce_mock_move(a, -1, -1);
				if (bgce_mock_fb_matches_full_redraw() != 0) {
					fprintf(stderr,
					        "headless: reverse move mismatch "
					        "zoom=%d%% step=%d pos=%u,%u\n",
					        zooms[zi], step, a->x, a->y);
					fails++;
					break;
				}
			}
		}

		if (bgce_mock_screenshot("headless_15_slow_move.png") != 0)
			return 1;
		if (fails) {
			bgce_mock_fini();
			return 1;
		}
	}

	/*
	 * 1px left at 100% zoom (user example shape: (x,y) 200×200 → left 1):
	 *   old [100,300), new [99,299)
	 *   underlay column 299 only (1×200); mover exactly 200×200
	 *   not 201-wide (that is only old∪new for windows above)
	 */
	{
		const uint32_t win = 0xFF112233u;
		const uint32_t bg = 0xFF336699u; /* mock.c config.color */
		uint32_t *fb;
		uint32_t stride;
		const int x0 = 100, y0 = 80, w = 200;
		int fails = 0;

		bgce_mock_fini();
		if (bgce_mock_init(640, 480) != 0) {
			fprintf(stderr, "headless: 1px-left re-init failed\n");
			return 1;
		}
		a = bgce_mock_add_client((uint32_t)x0, (uint32_t)y0,
		                         (uint32_t)w, 200, win);
		if (!a) {
			fprintf(stderr, "headless: 1px-left client failed\n");
			bgce_mock_fini();
			return 1;
		}
		/* No cache restore / remember — pin world pos. */
		a->app_id[0] = '\0';
		a->x = (uint32_t)x0;
		a->y = (uint32_t)y0;
		bgce_mock_set_viewport(100, 0, 0);
		bgce_mock_draw(a);
		bgce_mock_move(a, -1, 0);

		fb = (uint32_t *)server.framebuffer;
		stride = server.display_w;
		if (a->x != (uint32_t)(x0 - 1) || a->y != (uint32_t)y0) {
			fprintf(stderr, "headless: 1px-left pos got %u,%u\n",
			        a->x, a->y);
			fails++;
		}
		/* trail: last column of old window → background */
		if (fb[y0 * (int)stride + (x0 + w - 1)] != bg) {
			fprintf(stderr,
			        "headless: trail col %d not bg (0x%08x)\n",
			        x0 + w - 1,
			        fb[y0 * (int)stride + (x0 + w - 1)]);
			fails++;
		}
		if (fb[y0 * (int)stride + (x0 + w)] != bg) {
			fprintf(stderr, "headless: col %d past old end not bg\n",
			        x0 + w);
			fails++;
		}
		if (fb[y0 * (int)stride + (x0 - 1)] != win) {
			fprintf(stderr, "headless: new left col %d not win\n",
			        x0 - 1);
			fails++;
		}
		if (fb[y0 * (int)stride + (x0 + w - 2)] != win) {
			fprintf(stderr, "headless: new right col %d not win\n",
			        x0 + w - 2);
			fails++;
		}
		if (bgce_mock_fb_matches_full_redraw() != 0) {
			fprintf(stderr, "headless: 1px-left full-redraw mismatch\n");
			fails++;
		}
		if (bgce_mock_screenshot("headless_16_1px_left.png") != 0)
			return 1;
		if (fails) {
			bgce_mock_fini();
			return 1;
		}
	}

	printf("headless complete. PNG frames written.\n");
	bgce_mock_fini();
	return 0;
}
