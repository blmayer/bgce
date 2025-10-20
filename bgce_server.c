#define _XOPEN_SOURCE 700

#include "bgce.h"
#include "bgce_shared.h"
#include "bgce_client_handler.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

int main(void)
{
	ServerState server;
	server.width = 640;
	server.height = 480;
	server.color_depth = 24;
	server.framebuffer = calloc(server.width * server.height * 3, 1);

	int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv_fd < 0) {
		perror("socket");
		exit(1);
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, BGCE_SOCKET_PATH, sizeof(addr.sun_path)-1);
	unlink(BGCE_SOCKET_PATH);

	if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}

	if (listen(srv_fd, 4) < 0) {
		perror("listen");
		exit(1);
	}

	printf("[BGCE] Server listening on %s\n", BGCE_SOCKET_PATH);

	while (1) {
		int client_fd = accept(srv_fd, NULL, NULL);
		if (client_fd < 0) {
			perror("accept");
			continue;
		}

		pid_t pid = fork();
		if (pid == 0) {
			close(srv_fd);
			handle_client(client_fd, &server);
			exit(0);
		}
		close(client_fd);
	}

	free(server.framebuffer);
	close(srv_fd);
	unlink(BGCE_SOCKET_PATH);
	return 0;
}

