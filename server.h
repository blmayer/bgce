#ifndef BGCE_SERVER_H
#define BGCE_SERVER_H

#define _XOPEN_SOURCE 700
#include "bgce.h"

#include <drm/drm_mode.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <xf86drmMode.h>

#define MAX_PATH_LEN 512

/* ----------------------------
 * Client Representation
 * ---------------------------- */

struct Client {
	int fd;
	pid_t pid;
	char shm_name[64];
	void* buffer;
	uint32_t width;
	uint32_t height;
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
	int drm_fd;
	uint32_t crtc_id;
	uint32_t display_w;
	uint32_t display_h;
	uint32_t display_bpp;
	void* framebuffer;

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

// Background configuration for now
struct config {
	BackgroundType type;
	uint32_t color; // RGBA format
	ImageMode mode;
	char path[MAX_PATH_LEN];
};

// Parse config file
int parse_config(struct config* config);
int apply_background(struct config* config, uint32_t* buffer, uint32_t width, uint32_t height);

/* ----------------------------
 * Cursor
 * ---------------------------- */

#define CURSOR_WIDTH 64
#define CURSOR_HEIGHT 64
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

/* ----------------------------
 * Server Functions
 * ---------------------------- */

/**
 * Display
 */
int init_display();

void release_display(void);

void set_drm_cursor(struct ServerState* srv, int x, int y);

void draw(struct ServerState* srv, struct Client cli);

void redraw_region(struct ServerState* srv, struct Client c, int dx, int dy);

void redraw_from_resize(struct ServerState* srv, struct Client c, int dx, int dy);

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

/*
 * Client related stuff
 * from loop.c mainly
 */
void* client_thread(void* arg);

int setup_vt_handling(void);

#endif /* BGCE_SERVER_H */
