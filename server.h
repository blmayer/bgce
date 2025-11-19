#ifndef BGCE_SERVER_H
#define BGCE_SERVER_H

#define _XOPEN_SOURCE 700
#include "bgce.h"

#include <drm/drm_mode.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <xf86drmMode.h>

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
	struct Client* next;
	int inputs[MAX_INPUT_DEVICES];
};

/* ----------------------------
 * Server State
 * ---------------------------- */

struct InputState {
	int fds[MAX_INPUT_DEVICES];
	struct BGCEInputDevice devs[MAX_INPUT_DEVICES];
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

/* ----------------------------
 * Cursor
 * ---------------------------- */

#define CURSOR_WIDTH 8
#define CURSOR_HEIGHT 8
#define CURSOR_HOTSPOT_X 0
#define CURSOR_HOTSPOT_Y 0

struct CursorState {
    int x;
    int y;
    uint32_t background_buffer[CURSOR_WIDTH * CURSOR_HEIGHT];
    int active; // 0 for hidden, 1 for visible
};


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
