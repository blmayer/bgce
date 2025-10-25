#ifndef BGCE_SERVER_H
#define BGCE_SERVER_H

#define _XOPEN_SOURCE 700
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>

/* ----------------------------
 * Client Representation
 * ---------------------------- */

struct Client {
    int fd;
    pid_t pid;
    char shm_name[64];
    void *buffer;
    uint32_t width;
    uint32_t height;
    struct Client *next;
};


/* ----------------------------
 * Server State
 * ---------------------------- */

struct ServerState {
    int server_fd;
    int width;
    int height;
    int color_depth;

    struct Client *clients;
    int client_count;

    struct Client *focused_client;
};

/* ----------------------------
 * Server Functions
 * ---------------------------- */

/**
 * Initialize the BGCE server socket.
 * Returns 0 on success, -1 on error.
 */
int bgce_server_init(struct ServerState *srv, const char *socket_path);

/**
 * Run the main accept loop — listens for incoming clients and spawns threads.
 */
void bgce_server_run(struct ServerState *srv);

/**
 * Clean up sockets and client buffers.
 */
void bgce_server_shutdown(struct ServerState *srv);

/**
 * Handle a single connected client (thread function).
 */
void *bgce_handle_client(void *arg);

#endif /* BGCE_SERVER_H */

