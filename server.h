#ifndef BGCE_SERVER_H
#define BGCE_SERVER_H

#define _XOPEN_SOURCE 700
#include "bgce.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#define MAX_PATH_LEN 512

/* Virtual desktop is WORLD_SCALE × display in each axis (4× area).
 * Zoom is an integer percent: 100 = 1 world pixel per screen pixel.
 * Range [50, 400] so the full canvas fits when zoomed out (2× world). */
#define BGCE_WORLD_SCALE 2
#define BGCE_ZOOM_PCT_MIN 50
#define BGCE_ZOOM_PCT_MAX 400
#define BGCE_ZOOM_PCT_1X 100
#define BGCE_ZOOM_PCT_STEP 10 /* wheel click */

/* ----------------------------
 * Client Representation
 * ---------------------------- */

struct Client {
	int fd;
	pid_t pid;
	/* Stable app key for location cache (e.g. /proc/pid/comm). */
	char app_id[64];
	char shm_name[64];
	void* buffer;
	/* Buffer pixel size (what the client draws into). */
	uint32_t width;
	uint32_t height;
	/*
	 * On-screen geometry in virtual-desktop (world) pixels.
	 * New windows set world ≈ buffer / current_zoom so a client that
	 * asks for e.g. 800×600 looks ~800×600 screen pixels at any zoom.
	 * Background uses world_* == width/height (1:1).
	 */
	uint32_t world_w;
	uint32_t world_h;
	/* Position in virtual-desktop (world) pixels */
	uint32_t x;
	uint32_t y;
	uint32_t z;
	struct Client* next;
	int inputs[MAX_INPUT_DEVICES];
};

/* ----------------------------
 * Server State
 * ---------------------------- */

struct InputState {
	int fds[MAX_INPUT_DEVICES];
	struct InputDevice devs[MAX_INPUT_DEVICES];
	size_t count;
};

struct ServerState {
	int server_fd;
	int display_fd;          /* /dev/fb* or /dev/dri/card* */
	uint32_t display_w;
	uint32_t display_h;
	uint32_t display_bpp;
	uint32_t display_pitch;  /* bytes per row */
	size_t fb_size;
	void* framebuffer;
#ifdef BGCE_USE_DRM
	uint32_t crtc_id;
#endif

	/* Virtual desktop / viewport (see BGCE_WORLD_SCALE) — all integers */
	uint32_t virtual_w;
	uint32_t virtual_h;
	/* Zoom percent: world→screen scale is zoom_pct/100.  100 = identity. */
	int zoom_pct;
	/*
	 * Screen-pixel pan (not world).  Mapping:
	 *   sx = (wx * zoom_pct) / 100 - pan_x
	 *   wx = (sx + pan_x) * 100 / zoom_pct
	 * Pan by sdx screen pixels is exactly pan_x -= sdx (and fb_scroll).
	 */
	int pan_x;
	int pan_y;

	struct InputState input;

	struct Client* clients;
	int client_count;

	struct Client* focused_client;
};

// Background types
typedef enum {
	BG_COLOR,
	BG_IMAGE
} BackgroundType;

// Image display modes
typedef enum {
	IMAGE_TILED,
	IMAGE_SCALED
} ImageMode;

/* ----------------------------
 * Keyboard shortcuts
 * ---------------------------- */
#define MAX_SHORTCUTS 16

struct key_combo {
	int ctrl;
	int alt;
	int shift;
	uint16_t key; // Linux key code (KEY_* from linux/input.h)
};

enum shortcut_type {
	SHORTCUT_NONE = 0,
	SHORTCUT_BUILTIN,
	SHORTCUT_COMMAND,
};

enum shortcut_parse_result {
	SHORTCUT_PARSE_OK = 0,
	SHORTCUT_PARSE_BAD_FORMAT,
	SHORTCUT_PARSE_UNSUPPORTED_BUILTIN,
	SHORTCUT_PARSE_EMPTY_COMMAND,
};

struct shortcut {
	struct key_combo combo;
	enum shortcut_type type;
	/* builtin: "exit"/"screenshot"; command: program + args (no shell) */
	char value[256];
};

// Cursor theme: per-type image data (ARGB, pre-scaled to CURSOR_WIDTH x CURSOR_HEIGHT)
struct cursor_theme {
	uint32_t* images[BGCE_CURSOR_COUNT]; // NULL = use built-in fallback
	int hotspot_x[BGCE_CURSOR_COUNT];
	int hotspot_y[BGCE_CURSOR_COUNT];
};

// Background configuration for now
struct config {
	BackgroundType type;
	uint32_t color; // RGBA format
	ImageMode mode;
	char path[MAX_PATH_LEN];

	struct shortcut shortcuts[MAX_SHORTCUTS];
	int shortcut_count;

	struct cursor_theme cursors;

	/*
	 * Pointer drag feel: screen-pixel mouse delta is scaled by these, then
	 * by 1/zoom so on-screen pan/move speed stays consistent at any zoom.
	 * 1.0 = one screen pixel of motion per mouse pixel at zoom 1.
	 */
	float move_speed;
	float pan_speed;
	/*
	 * Natural scrolling (macOS-style): reverse wheel direction for zoom
	 * and for wheel events forwarded to clients.  0 = traditional, 1 = on.
	 */
	int natural_scrolling;
};

// Load config file (~/.config/bgce.conf) and apply defaults
int load_config(struct config* config);
/** Log the full effective configuration (background, shortcuts, cursors). */
void print_config(const struct config* config);
/**
 * Paint background into a width×height buffer (usually the full virtual desktop).
 * IMAGE_SCALED stretches the image over the whole buffer; IMAGE_TILED repeats
 * source texels. tile_w/tile_h are reserved (pass display size; unused for scaled).
 */
int apply_background(struct config* config, uint32_t* buffer,
                     uint32_t width, uint32_t height,
                     uint32_t tile_w, uint32_t tile_h);

/* ----------------------------
 * Cursor
 * ---------------------------- */

#define CURSOR_WIDTH 32
#define CURSOR_HEIGHT 32
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

/* ----------------------------
 * Server Functions
 * ---------------------------- */

/**
 * After setup_log_file(), stdout/stderr go to the log file only.
 * Use this for fatal/startup errors that must also appear on the
 * original terminal (e.g. cannot open /dev/fb0).
 */
void bgce_announce(const char *fmt, ...);

/** Set by SIGINT handler; input thread clears it (server stays up). */
extern volatile sig_atomic_t bgce_sigint_pending;

/** Historical hook; SIGINT is swallowed — clients use keyboard input only. */
void deliver_interrupt_to_focus(void);

/** Clean shutdown (restore VT, unlink socket). Safe from the input thread. */
void bgce_request_shutdown(void);

/**
 * Display
 */
int init_display(void);

void release_display(void);

void set_cursor_pos(struct ServerState* srv, int x, int y);

int setup_vt_handling(void);
void release_vt(void);
int display_cursor_init(void);
void display_cursor_fini(void);
void display_cursor_refresh(void);
/** 1 if the input thread should re-paint the software cursor. */
int display_cursor_pending(void);
/** Paint cursor if dirty (input thread only). */
void display_cursor_present(void);

void set_cursor_type(enum BGCECursorType type);

/**
 * Client content changed (MSG_DRAW): blit only this client, clipped by
 * windows stacked above it.  Does not repaint wallpaper or clients below,
 * and does not raise the client.  See draw() in display.c for the model.
 */
void draw(struct ServerState *srv, struct Client *cli);

/**
 * Move client by world delta (dx, dy).  `c` is still at the old position.
 * Design: L-shaped expose with underlay stack only; full blit of the mover
 * at the new place (no FB scroll, no underlay under the window body).
 * See redraw_region() in display.c.
 */
void redraw_region(struct ServerState *srv, struct Client *c, int dx, int dy);

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy);

/** Full-scene recomposite (used after zoom and other full invalidations). */
void redraw_all(struct ServerState* srv);

/**
 * After a client is removed from the list: repaint only its former screen
 * footprint from the remaining stack (background + windows that were under
 * or overlapped that rect).  Does not touch the rest of the desktop.
 */
void erase_client(struct ServerState* srv, const struct Client* gone);

/**
 * Pan by integer screen pixels (sdx, sdy): content moves by that delta.
 * memmove the framebuffer, then composite only newly exposed edge strip(s).
 * Never a full-scene redraw for ordinary pan.
 */
void redraw_pan(struct ServerState *srv, int sdx, int sdy);

/** Clamp pan so the viewport stays over the virtual desktop. */
void clamp_viewport(struct ServerState* srv);

/**
 * One discrete zoom click (dir > 0 = in, dir < 0 = out).
 * Adjusts zoom_pct by ±BGCE_ZOOM_PCT_STEP, clamped to [MIN, MAX].
 * 100 is always reachable.  Returns 1 if zoom changed.
 */
int bgce_zoom_step(struct ServerState *srv, int dir);

/** Set zoom to an absolute percent (clamped). Returns 1 if changed. */
int bgce_zoom_set(struct ServerState *srv, int zoom_pct);

/**
 * Alt+Tab (reverse=0) / Alt+Shift+Tab (reverse=1): cycle focus among clients,
 * raise the target, and restore its cached zoom/pan.  Declared here for the
 * input path; implementation lives with the location cache.
 */
void bgce_cycle_focus(struct ServerState *srv, int reverse);

/** Map a screen pixel to virtual-desktop (world) coordinates. */
void screen_to_world(const struct ServerState* srv, int sx, int sy,
                     int* wx, int* wy);

/** Map a world point to screen coordinates. */
void world_to_screen(const struct ServerState* srv, int wx, int wy,
                     int* sx, int* sy);

/**
 * Capture the current framebuffer and save it as a screenshot.
 * Returns 0 on success, -1 on failure.
 */
int take_screenshot(const char* filename);

/**
 * Input device related functions
 * from input.c
 */
int init_input(void);

void* input_loop(void* arg);

/** Drop any drag targeting this client (call before freeing it). */
void client_disconnected(struct Client* c);

/*
 * Client related stuff
 * from loop.c mainly
 */
void* client_thread(void* arg);

int setup_vt_handling(void);

#endif /* BGCE_SERVER_H */
