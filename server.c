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

/* Cleanup on Ctrl+C */
static void handle_sigint(int sig) {
	(void)sig;
	unlink(SOCKET_PATH);
	printf("\n[BGCE] Server terminated.\n");
	exit(0);
}

int main(void) {
	signal(SIGINT, handle_sigint);
	memset(&server, 0, sizeof(struct ServerState));
	server.drm_fd = -1;
	server.framebuffer = NULL;
	server.crtc_id = 0;

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
