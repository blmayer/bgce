#define _XOPEN_SOURCE 700

#include "bgce.h"
#include "server.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Externs from server.c */
extern struct ServerState server;

void* client_thread(void* arg) {
	int client_fd = *(int*)arg;
	free(arg);

	// Allocate memory for the client
	struct Client* client = calloc(1, sizeof(struct Client));
	if (!client) {
		perror("[BGCE] Failed to allocate memory for client");
		close(client_fd);
		return NULL;
	}

	client->fd = client_fd;

	// Add client to the linked list
	client->next = server.clients;
	client->z = server.clients->z + 1;
	server.clients = client;

	/* last connected client gets focus (notify old focus only; don't notify the new client yet) */
	struct Client* old_focus = server.focused_client;
	if (old_focus && old_focus != client) {
		struct BGCEMessage lost = {0};
		lost.type = MSG_FOCUS_CHANGE;
		lost.data.focus_event.state = 0;
		bgce_send_msg(old_focus->fd, &lost);
	}
	server.focused_client = client;

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
			struct BufferReply reply = {0};
			size_t buf_size;
			int shm_fd;
			void *map;
			char old_name[64];
			int had_buffer;

			printf(
			        "[BGCE] Client requested buffer of size %dx%d\n",
			        req.width,
			        req.height);

			reply.status = -1;
			msg.type = MSG_GET_BUFFER;

			if (req.width == 0 || req.height == 0) {
				fprintf(stderr, "[BGCE] invalid buffer size %ux%u\n",
				        req.width, req.height);
				msg.data.buffer_reply = reply;
				bgce_send_msg(client_fd, &msg);
				break;
			}

			/* Unmap and remember old token before creating a new one. */
			had_buffer = client->buffer != NULL;
			old_name[0] = '\0';
			if (had_buffer) {
				printf("[BGCE] Client already has a buffer, unmapping.\n");
				munmap(client->buffer, client->width * client->height * 4);
				client->buffer = NULL;
				strncpy(old_name, client->shm_name, sizeof(old_name) - 1);
				old_name[sizeof(old_name) - 1] = '\0';
			}

			buf_size = (size_t)req.width * req.height * 4;
			shm_fd = bgce_buf_create(client->shm_name,
			                         sizeof(client->shm_name), buf_size);
			if (shm_fd < 0) {
				perror("[BGCE] create buffer");
				if (old_name[0])
					bgce_buf_unlink(old_name);
				msg.data.buffer_reply = reply;
				bgce_send_msg(client_fd, &msg);
				break;
			}

			map = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED,
			           shm_fd, 0);
			close(shm_fd);
			if (map == MAP_FAILED) {
				perror("[BGCE] mmap buffer");
				bgce_buf_unlink(client->shm_name);
				client->shm_name[0] = '\0';
				if (old_name[0])
					bgce_buf_unlink(old_name);
				msg.data.buffer_reply = reply;
				bgce_send_msg(client_fd, &msg);
				break;
			}

			if (old_name[0])
				bgce_buf_unlink(old_name);

			client->buffer = map;
			client->width = req.width;
			client->height = req.height;
			/* First buffer only: place at the current viewport so the window
			 * appears on-screen even when the user has panned/zoomed. */
			if (!had_buffer) {
				float wx, wy;
				screen_to_world(&server, 0.0f, 0.0f, &wx, &wy);
				if (wx < 0.0f)
					wx = 0.0f;
				if (wy < 0.0f)
					wy = 0.0f;
				if (wx > (float)server.virtual_w)
					wx = (float)server.virtual_w;
				if (wy > (float)server.virtual_h)
					wy = (float)server.virtual_h;
				client->x = (uint32_t)wx;
				client->y = (uint32_t)wy;
			}
			printf("[BGCE] Client buffer: %p size=%zu (%dx%d) name=%s\n",
			       client->buffer,
			       client->width * client->height * 4UL,
			       client->width, client->height,
			       client->shm_name);

			reply.status = 0;
			strncpy(reply.shm_name, client->shm_name, sizeof(reply.shm_name) - 1);
			reply.shm_name[sizeof(reply.shm_name) - 1] = '\0';
			reply.width = req.width;
			reply.height = req.height;
			msg.data.buffer_reply = reply;
			bgce_send_msg(client_fd, &msg);
			break;
		}

		case MSG_DRAW: {
			printf("[BGCE] Received draw event from client %s\n", client->shm_name);
			/*
			 * Drawing must be allowed even when the client is not focused.
			 * Focus only affects input routing.
			 */
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
		case MSG_SET_CURSOR: {
			int ctype = msg.data.cursor_request.cursor_type;
			printf("[BGCE] Client requested cursor type %d\n", ctype);
			/* Only the focused client may change the cursor */
			if (server.focused_client == client)
				set_cursor_type((enum BGCECursorType)ctype);
			break;
		}
		default:
			fprintf(stderr, "[BGCE] Unknown message type %d\n", msg.type);
		}
	}

	if (client->buffer) {
		munmap(client->buffer, client->width * client->height * 4);
		client->buffer = NULL;
		bgce_buf_unlink(client->shm_name);
		client->shm_name[0] = '\0';
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
		/* notify focused client it lost focus before we clear */
		struct BGCEMessage lost = {0};
		lost.type = MSG_FOCUS_CHANGE;
		lost.data.focus_event.state = 0;
		bgce_send_msg(client->fd, &lost);
		server.focused_client = NULL;
	}
	close(client->fd);

	printf("[BGCE] Thread exiting for client fd=%d\n", client->fd);
	free(client);
	return NULL;
}
