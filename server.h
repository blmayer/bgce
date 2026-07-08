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
 * Zoom 1.0 = 100% (1 world pixel = 1 screen pixel).
 * Zoom range [ZOOM_MIN, ZOOM_MAX]: fully zoomed-out fits the whole
 * virtual desktop; fully zoomed-in is 4× magnification. */
#define BGCE_WORLD_SCALE 2
#define BGCE_ZOOM_MIN 0.5f
#define BGCE_ZOOM_MAX 4.0f
#define BGCE_ZOOM_STEP 1.15f

/* ----------------------------
 * Client Representation
 * ---------------------------- */

struct Client {
	int fd;
	pid_t pid;
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

	/* Virtual desktop / viewport (see BGCE_WORLD_SCALE) */
	uint32_t virtual_w;
	uint32_t virtual_h;
	float zoom;   /* screen_pixels = world_pixels * zoom */
	float pan_x;  /* world coords of top-left of the screen */
	float pan_y;

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
	char value[256]; // for builtin: "exit" or "screenshot"; for command: the command line to run
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

/** Set by SIGINT handler; input thread delivers as Ctrl+C to focus. */
extern volatile sig_atomic_t bgce_sigint_pending;

/** Forward a synthetic Ctrl+C (KEY_LEFTCTRL + KEY_C) to the focused client. */
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

void set_cursor_type(enum BGCECursorType type);

void draw(struct ServerState* srv, struct Client cli);

void redraw_region(struct ServerState* srv, struct Client c, int dx, int dy);

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy);

/** Full-scene recomposite (used after zoom and other full invalidations). */
void redraw_all(struct ServerState* srv);

/**
 * Pan: shift pixels already on the framebuffer and only composite the
 * newly exposed edge(s).  old_pan_* is the viewport origin before the pan
 * update.  No rescaling of existing pixels — scale is only applied when
 * filling exposed edges (and when zoom changes via redraw_all).
 */
void redraw_pan(struct ServerState* srv, float old_pan_x, float old_pan_y);

/** Clamp pan so the viewport stays over the virtual desktop. */
void clamp_viewport(struct ServerState* srv);

/** Map a screen pixel to virtual-desktop (world) coordinates. */
void screen_to_world(const struct ServerState* srv, float sx, float sy,
                     float* wx, float* wy);

/** Map a world point to screen coordinates. */
void world_to_screen(const struct ServerState* srv, float wx, float wy,
                     float* sx, float* sy);

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
