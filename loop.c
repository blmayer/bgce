#define _XOPEN_SOURCE 700

#include "bgce.h"
#include "server.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* Externs from server.c */
extern struct ServerState server;

void* client_thread(void* arg) {
	int client_fd = *(int*)arg;
	free(arg);

	struct Client client = {0};
	client.fd = client_fd;
	server.focused_client = &client; /* last connected client gets focus */

	printf("[BGCE] Thread started for client fd=%d\n", client_fd);

	while (1) {
		struct BGCEMessage msg;
		ssize_t rc = bgce_recv_msg(client_fd, &msg);
		if (rc <= 0) {
			printf("[BGCE] Client disconnected (fd=%d)\n", client_fd);
			break;
		}

		switch (msg.type) {
		case MSG_GET_SERVER_INFO: {
			struct ServerInfo info = {
			        .width = server.display_w,
			        .height = server.display_h,
			        .color_depth = server.display_bpp,
			        .input_device_count = server.input.count,
			};
			for (int d = 0; d < server.input.count; d++) {
				info.devices[d] = server.input.devs[d];
			}

			memcpy(msg.data, &info, sizeof(info));
			bgce_send_msg(client_fd, &msg);
			break;
		}

		case MSG_GET_BUFFER: {
			struct BufferRequest req;
			memcpy(&req, msg.data, sizeof(req));
			printf(
			        "[BGCE] Client requested buffer of size %dx%d\n",
			        req.width,
			        req.height);

			snprintf(client.shm_name, sizeof(client.shm_name),
			         "/bgce_buf_%d_%ld", getpid(), time(NULL));

			int shm_fd = shm_open(client.shm_name, O_CREAT | O_RDWR, 0600);
			if (shm_fd < 0) {
				perror("shm_open");
				break;
			}

			size_t buf_size = req.width * req.height * 4;
			if (ftruncate(shm_fd, buf_size) < 0) {
				perror("ftruncate");
				close(shm_fd);
				break;
			}

			client.buffer = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
			client.width = req.width;
			client.height = req.height;
			close(shm_fd);
			printf("Server buffer: %p size=%zu (%dx%d) name=%s\n",
			       client.buffer,
			       client.width * client.height * 4UL,
			       client.width, client.height,
			       client.shm_name);

			struct BufferReply reply = {0};
			strncpy(reply.shm_name, client.shm_name, sizeof(reply.shm_name));
			reply.width = req.width;
			reply.height = req.height;
			memcpy(msg.data, &reply, sizeof(reply));
			bgce_send_msg(client_fd, &msg);
			break;
		}

		case MSG_DRAW: {
			printf("[BGCE] Drawing from client %d (%dx%d)\n",
			       client.fd, client.width, client.height);

			if (client_fd == server.focused_client->fd) {
				if (!client.buffer) {
					fprintf(stderr, "[BGCE] Client has no buffer!\n");
					break;
				}
				printf("[BGCE] Drawing from focused client %d\n", client_fd);
				printf("[BGCE] First pixel: %08x\n", ((uint32_t*)client.buffer)[0]);

				draw(&server, client);
			} else {
				printf("[BGCE] Ignoring draw from unfocused client %d\n", client_fd);
			}
			break;
		}

		default:
			fprintf(stderr, "[BGCE] Unknown message type %d\n", msg.type);
			break;
		}
	}

	if (client.buffer) {
		munmap(client.buffer, client.width * client.height * 3);
		shm_unlink(client.shm_name);
	}
	close(client_fd);

	printf("[BGCE] Thread exiting for client fd=%d\n", client_fd);
	return NULL;
}
