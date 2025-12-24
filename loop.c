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

	// Allocate memory for the client
	struct Client* client = calloc(1, sizeof(struct Client));
	if (!client) {
		perror("Failed to allocate memory for client");
		close(client_fd);
		return NULL;
	}

	client->fd = client_fd;

	// Add client to the linked list
	client->next = server.clients;
	client->z = server.clients->z + 1;
	server.clients = client;
	server.focused_client = client; /* last connected client gets focus */

	if (!client) {
		fprintf(stderr, "[BGCE] No available client slots\n");
		close(client_fd);
		return NULL;
	}

	printf("[BGCE] Thread started for client fd=%d z=%d\n", client_fd, client->z);

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

			msg.data.server_info = info;
			bgce_send_msg(client_fd, &msg);
			break;
		}

		case MSG_GET_BUFFER: {
			struct BufferRequest req = msg.data.buffer_request;
			printf(
			        "[BGCE] Client requested buffer of size %dx%d\n",
			        req.width,
			        req.height);

			snprintf(client->shm_name, sizeof(client->shm_name),
			         "bgce_buf_%d_%ld", getpid(), time(NULL));

			// Unmap and unlink the existing buffer: for resize
			if (client->buffer) {
				printf("[BGCE] Client already has a buffer, unmapping.\n",
				munmap(client->buffer, client->width * client->height * 4);
				shm_unlink(client->shm_name);
			}

			int shm_fd = shm_open(client->shm_name, O_CREAT | O_RDWR, 0600);
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

			client->buffer = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
			client->width = req.width;
			client->height = req.height;
			client->x = 0;
			client->y = 0;
			close(shm_fd);
			printf("[BGCE] Client buffer: %p size=%zu (%dx%d) name=%s\n",
			       client->buffer,
			       client->width * client->height * 4UL,
			       client->width, client->height,
			       client->shm_name);

			struct BufferReply reply = {0};
			strncpy(reply.shm_name, client->shm_name, sizeof(reply.shm_name));
			reply.width = req.width;
			reply.height = req.height;
			msg.data.buffer_reply = reply;
			bgce_send_msg(client_fd, &msg);
			break;
		}

		case MSG_DRAW: {
			printf("[BGCE] Received draw event from client %s\n", client->shm_name);
			if (client_fd != server.focused_client->fd) {
				printf("[BGCE] Client is not focused!\n");
				break;
			}

			draw(&server, *client);
			break;
		}
		case MSG_MOVE: {
			struct MoveRequest move_req = msg.data.move_request;
			printf(
				"[BGCE] Client requested move to position (%d, %d)\n",
				move_req.x, move_req.y);

			// Update client position
			client->x = move_req.x;
			client->y = move_req.y;

			break;
		}
		default:
			fprintf(stderr, "[BGCE] Unknown message type %d\n", msg.type);
		}
	}

	if (client->buffer) {
		munmap(client->buffer, client->width * client->height * 4);
		shm_unlink(client->shm_name);
	}

	// Remove client from the linked list
	struct Client* prev = NULL;
	struct Client* curr = server.clients;
	while (curr) {
		if (curr == client) {
			if (prev) {
				prev->next = curr->next;
			} else {
				server.clients = curr->next;
			}
			break;
		}
		prev = curr;
		curr = curr->next;
	}

	if (server.focused_client == client) {
		server.focused_client = NULL;
	}

	close(client->fd);

	printf("[BGCE] Thread exiting for client fd=%d\n", client->fd);
	free(client);
	return NULL;
}
