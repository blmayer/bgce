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

struct DisplayState {
	int drm_fd;            /* DRM device file descriptor */
	uint32_t crtc_id;      /* CRTC ID in use */
	uint32_t connector_id; /* Connector ID in use */
	uint32_t fb_id;        /* Framebuffer ID */
	uint32_t handle;       /* GEM buffer handle */
	uint32_t pitch;        /* Bytes per scanline */
	uint64_t size;         /* Total buffer size (bytes) */
	uint8_t* framebuffer;  /* Mapped framebuffer pointer */
	drmModeRes* resources;
	drmModeConnector* connector;
	drmModeEncoder* encoder;
	struct drm_mode_modeinfo mode; /* Active display mode */
};

struct ServerState {
	int server_fd;
	int color_depth;

	struct DisplayState display;
	struct InputState input;

	struct Client* clients;
	int client_count;

	struct Client* focused_client;
};

/* ----------------------------
 * Server Functions
 * ---------------------------- */

/**
 * Initialize the BGCE server socket.
 * Returns 0 on success, -1 on error.
 */
int bgce_server_init(struct ServerState* srv, const char* socket_path);

int init_display(struct ServerState* srv);

void draw(struct ServerState* srv, struct Client cli);

void bgce_display_shutdown(void);

/**
 * Run the main accept loop — listens for incoming clients and spawns threads.
 */
void bgce_server_run(struct ServerState* srv);

/**
 * Clean up sockets and client buffers.
 */
void bgce_server_shutdown(struct ServerState* srv);

/**
 * Handle a single connected client (thread function).
 */
void* bgce_handle_client(void* arg);

#endif /* BGCE_SERVER_H */
