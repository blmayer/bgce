#include "server.h"
#include "bgce.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct ServerState server = {}; /* Global server state */

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stderr

	memset(&server, 0, sizeof(struct ServerState));
	server.drm_fd = -1;
	server.framebuffer = NULL;
	server.crtc_id = 0;
	server.client_count = 0;

	struct config config;
	char* home = getenv("HOME");
	if (home) {
		char user_config[512];
		snprintf(user_config, sizeof(user_config), "%s/.config/bgce.conf", home);
		parse_config(&config);
	}
	printf("[BGCE] Loaded config type=%u, path=%s, mode=%u\n", config.type, config.path, config.mode);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
	unlink(SOCKET_PATH);

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return 1;
	}

	if (listen(fd, 8) < 0) {
		perror("listen");
		close(fd);
		return 1;
	}

	server.server_fd = fd;

	if (init_display() != 0) {
		fprintf(stderr, "display init failed\n");
		release_display();
		return 1;
	}
	printf("[BGCE] Display initialised\n");

	/* Add a background client */
	struct Client background_client;
	background_client.x = 0;
	background_client.y = 0;
	background_client.z = 0; // Special case
	background_client.width = server.display_w;
	background_client.height = server.display_h;
	background_client.buffer = malloc(server.display_w * server.display_h * 4);
	background_client.next = NULL;
	server.clients = &background_client;

	// Apply background based on config
	apply_background(&config, background_client.buffer, server.display_w, server.display_h);

	puts("[BGCE] Drawing background");
	draw(&server, background_client);

	if (init_input() != 0) {
		perror("[BGCE] Failed to start input thread");
		return 4;
	}
	pthread_t input_thread;

	int rc = pthread_create(&input_thread, NULL, input_loop, NULL);
	if (rc != 0) {
		errno = rc;
		perror("[BGCE] Failed to start input thread");
		return 5;
	}
	pthread_detach(input_thread);

	printf("[BGCE] Server listening on %s\n", SOCKET_PATH);

	while (1) {
		int client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0) {
			perror("accept");
			continue;
		}

		printf("[BGCE] Client connected (fd=%d)\n", client_fd);

		pthread_t tid;
		int* arg = malloc(sizeof(int));
		if (!arg) {
			perror("malloc");
			close(client_fd);
			continue;
		}

		*arg = client_fd;
		if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
			perror("pthread_create input thread");
			free(arg);
			close(client_fd);
			continue;
		}

		pthread_detach(tid);
	}

	release_display();
	free(server.framebuffer);

	return 0;
}
