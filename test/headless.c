/*
 * Headless mock test for BGCE (same workflow as BGTK's test/headless.c).
 *
 * Build:  make headless
 * Run:    ./headless
 *
 * Writes headless_*.png for visual inspection.  No real display or input.
 */

#include "mock.h"

#include <stdio.h>
#include <stdint.h>

int main(void)
{
	struct Client *a, *b, *c;

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
	bgce_mock_zoom_at(0.5f, 320, 240);
	if (bgce_mock_screenshot("headless_06_zoomed_out.png") != 0)
		return 1;
	bgce_mock_pan_screen(40, 30);
	if (bgce_mock_screenshot("headless_07_panned.png") != 0)
		return 1;

	printf("headless complete. PNG frames written.\n");
	bgce_mock_fini();
	return 0;
}
