#include "server.h"
#include "bgce.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct ServerState server = {}; /* Global server state */
struct config config = {};            /* Global config (background + shortcuts) */

static void ensure_dir(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	/* Try to create; ignore errors (parent may not exist, we'll fail open later) */
	mkdir(path, 0755);
}

static void cleanup_and_exit(int sig) {
	(void)sig;
	release_display();
	_exit(0);
}

static void setup_log_file(void) {
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char dir[512];

	if (xdg && xdg[0]) {
		snprintf(dir, sizeof(dir), "%s/bgce", xdg);
		ensure_dir(xdg);
		ensure_dir(dir);
	} else if (home && home[0]) {
		char dotcache[512];
		snprintf(dotcache, sizeof(dotcache), "%s/.cache", home);
		snprintf(dir, sizeof(dir), "%s/.cache/bgce", home);
		ensure_dir(dotcache);
		ensure_dir(dir);
	} else {
		return;
	}

	char log_path[512];
	snprintf(log_path, sizeof(log_path), "%s/bgce.log", dir);

	int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return;

	/* Tell the user (on their original terminal) where logs are going */
	dprintf(STDERR_FILENO, "[BGCE] Logging to %s\n", log_path);

	/* Redirect all future stdout/stderr output into the log file */
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);
}

int main(void) {
	setup_log_file();

	setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stderr

	/* Auto-reap child processes so command shortcuts don't leave zombies */
	signal(SIGCHLD, SIG_IGN);

	/* Ensure VT is restored on termination signals */
	signal(SIGINT, cleanup_and_exit);
	signal(SIGTERM, cleanup_and_exit);

	memset(&server, 0, sizeof(struct ServerState));
	server.drm_fd = -1;
	server.framebuffer = NULL;
	server.crtc_id = 0;
	server.client_count = 0;

	char* home = getenv("HOME");
	if (home) {
		char user_config[512];
		snprintf(user_config, sizeof(user_config), "%s/.config/bgce.conf", home);
		parse_config(&config);
	}
	printf("[BGCE] Loaded config type=%u, path=%s, mode=%u, shortcuts=%d\n",
	       config.type, config.path, config.mode, config.shortcut_count);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("[BGCE] socket");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
	unlink(SOCKET_PATH);

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("[BGCE] bind");
		close(fd);
		return 1;
	}

	if (listen(fd, 8) < 0) {
		perror("[BGCE] listen");
		close(fd);
		return 1;
	}

	server.server_fd = fd;

	if (init_display() != 0) {
		fprintf(stderr, "[BGCE] display init failed\n");
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
			perror("[BGCE] accept");
			continue;
		}

		printf("[BGCE] Client connected (fd=%d)\n", client_fd);

		pthread_t tid;
		int* arg = malloc(sizeof(int));
		if (!arg) {
			perror("[BGCE] malloc");
			close(client_fd);
			continue;
		}

		*arg = client_fd;
		if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
			perror("[BGCE] pthread_create client thread");
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
