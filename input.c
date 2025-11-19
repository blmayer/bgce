#include "bgce.h"   /* for access to global server state */
#include "server.h" /* for access to global server state */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define test_bit(bit, array) ((array)[(bit) / 8] & (1 << ((bit) % 8)))
#define INPUT_DIR "/dev/input"

static struct {
	size_t count;
	struct pollfd fds[MAX_INPUT_DEVICES];
	int ctrl_down;
	int alt_down;
	int mouse_x;
	int mouse_y;
	struct {
		int active;
		struct Client* target;
		int start_mouse_x;
		int start_mouse_y;
		uint32_t start_win_x;
		uint32_t start_win_y;
		uint32_t start_win_width;
		uint32_t start_win_height;
		enum {
			DRAG_NONE,
			DRAG_MOVE,
			DRAG_RESIZE
		} type;
	} drag;
} input_state;

extern struct ServerState server;

struct Client* pick_client(int x, int y) {
	// Iterate through clients to find the topmost client under the cursor
	struct Client* c = server.clients;
	struct Client* picked = NULL;
	while (c) {
		if (x >= c->x && x <= (c->x + c->width) &&
		    y >= c->y && y <= (c->y + c->height)) {
			picked = c;
		}
		c = c->next;
	}
	return picked;
}

void move_client(struct Client* c, uint32_t x, uint32_t y,
                 uint32_t width, uint32_t height) {
	c->x = x;
	c->y = y;
	c->width = width;
	c->height = height;

	struct BGCEMessage msg;
	msg.type = MSG_BUFFER_CHANGE;
	struct MoveBufferRequest req = {
	        .x = x,
	        .y = y,
	        .width = width,
	        .height = height};
	memcpy(msg.data, &req, sizeof(req));
	bgce_send_msg(c->fd, &msg);
}

int init_input(void) {
	input_state.count = 0;
	input_state.ctrl_down = 0;
	input_state.alt_down = 0;
	input_state.mouse_x = server.display_w / 2;
	input_state.mouse_y = server.display_h / 2;
	input_state.drag.active = 0;
	input_state.drag.target = NULL;
	input_state.drag.type = DRAG_NONE;

	DIR* dir = opendir(INPUT_DIR);
	if (!dir) {
		perror("[BGCE] Failed to open /dev/input");
		return -1;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL && input_state.count < 8) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		char path[256 + 12];
		snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, ent->d_name);

		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue; // skip inaccessible devices

		unsigned long ev_bits[(EV_MAX + 7) / 8] = {0};
		if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
			close(fd);
			continue;
		}

		// Filter for useful input types
		int has_key = test_bit(EV_KEY, ev_bits);
		int has_rel = test_bit(EV_REL, ev_bits);
		if (!has_key && !has_rel) {
			close(fd);
			continue;
		}

		// Try to get device name
		char name[256] = "Unknown";
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			strcpy(name, "Unknown");

		input_state.fds[input_state.count].fd = fd;
		input_state.fds[input_state.count].events = POLLIN;

		server.input.devs[input_state.count].id = input_state.count;
		strcpy(server.input.devs[input_state.count].name, name);

		input_state.count++;

		printf("[BGCE] Input device accepted: %s (%s)%s%s\n",
		       path, name,
		       has_key ? " [KEY]" : "",
		       has_rel ? " [REL]" : "");
	}

	closedir(dir);

	if (input_state.count == 0) {
		fprintf(stderr, "[BGCE] No suitable input devices found\n");
		return -1;
	}
	server.input.count = input_state.count;

	return 0;
}

/*
 * This is the key mappings handling part, for now this is hardcoded
 * but in the future will be read from config.
 * Shortcuts available:
 *  ESC: exit
 *  CTRL + ALT + q: exit
 *  ALT + CLICK + DRAG: move
 *  ALT + RIGHT_CLICK + DRAG: resize
 *
 *  Returns if shortcut was handled
 */
static int handle_input_event(struct input_event ev) {
	if (ev.type == EV_KEY && ev.value == 1) { // Key press
		if (ev.code == KEY_ESC) {
			printf("[BGCE] ESC pressed, exiting.\n");
			return 1;
		}

		// Ctrl+Alt+Q combo
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
			input_state.ctrl_down = 1;
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
			input_state.alt_down = 1;

		if (input_state.ctrl_down && input_state.alt_down && ev.code == KEY_Q) {
			printf("[BGCE] Ctrl+Alt+Q pressed, exiting.\n");
			return 1;
		}
	}

	if (ev.type == EV_KEY && ev.value == 0) { // Key release
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
			input_state.ctrl_down = 0;
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
			input_state.alt_down = 0;

		// Stop drag on button release
		if (((ev.code == BTN_LEFT && input_state.drag.type == DRAG_MOVE) ||
		     (ev.code == BTN_RIGHT && input_state.drag.type == DRAG_RESIZE)) &&
		    input_state.drag.active) { // Only stop if it was an active drag of that type
			// Send final buffer change message to client
			struct Client* c = input_state.drag.target;
			if (c) {
				move_client(c, c->x, c->y, c->width, c->height);
			}
			input_state.drag.active = 0;
			input_state.drag.target = NULL;
			input_state.drag.type = DRAG_NONE;
			printf("[BGCE] drag event.\n");
			return 1;
		}
	}

	if (ev.type == EV_KEY && ev.code == BTN_LEFT &&
	    ev.value == 1 && input_state.alt_down) { // Alt + Left click down
		int mx = input_state.mouse_x;
		int my = input_state.mouse_y;

		struct Client* c = pick_client(mx, my);
		if (c) {
			server.focused_client = c;
			input_state.drag.active = 1;
			input_state.drag.target = c;
			input_state.drag.start_mouse_x = mx;
			input_state.drag.start_mouse_y = my;
			input_state.drag.start_win_x = c->x;
			input_state.drag.start_win_y = c->y;
			input_state.drag.start_win_width = c->width;
			input_state.drag.start_win_height = c->height;
			input_state.drag.type = DRAG_MOVE;
			printf("[BGCE] move event.\n");
			return 1;
		}
	}

	if (ev.type == EV_KEY && ev.code == BTN_RIGHT &&
	    ev.value == 1 && input_state.alt_down) { // Alt + Right click down
		int mx = input_state.mouse_x;
		int my = input_state.mouse_y;

		struct Client* c = pick_client(mx, my);
		if (c) {
			server.focused_client = c;
			input_state.drag.active = 1;
			input_state.drag.target = c;
			input_state.drag.start_mouse_x = mx;
			input_state.drag.start_mouse_y = my;
			input_state.drag.start_win_x = c->x;
			input_state.drag.start_win_y = c->y;
			input_state.drag.start_win_width = c->width;
			input_state.drag.start_win_height = c->height;
			input_state.drag.type = DRAG_RESIZE;
			printf("[BGCE] resize event.\n");
			return 1;
		}
	}

	if (ev.type == EV_REL) {
		if (ev.code == REL_X)
			input_state.mouse_x += ev.value;
		else if (ev.code == REL_Y)
			input_state.mouse_y += ev.value;
		printf("[BGCE] mouse position: (%u,%u).\n", input_state.mouse_x, input_state.mouse_y);

		// Clamp mouse coordinates to screen boundaries
		if (input_state.mouse_x < 0)
			input_state.mouse_x = 0;
		if (input_state.mouse_y < 0)
			input_state.mouse_y = 0;
		if (input_state.mouse_x > server.display_w)
			input_state.mouse_x = server.display_w;
		if (input_state.mouse_y > server.display_h)
			input_state.mouse_y = server.display_h;

		// Set the cursor position
		printf("[BGCE] new mouse position: (%u,%u).\n", input_state.mouse_x, input_state.mouse_y);
		printf("[BGCE] server: %u %u.\n", server.drm_fd, server.crtc_id);
		drmModeMoveCursor(
		        server.drm_fd,
		        server.crtc_id,
		        input_state.mouse_x,
		        input_state.mouse_y);

		if (input_state.drag.active) {
			int dx = input_state.mouse_x - input_state.drag.start_mouse_x;
			int dy = input_state.mouse_y - input_state.drag.start_mouse_y;

			struct Client* c = input_state.drag.target;
			if (!c)
				return 1; // Should not happen

			uint32_t new_x = c->x;
			uint32_t new_y = c->y;
			uint32_t new_width = c->width;
			uint32_t new_height = c->height;

			if (input_state.drag.type == DRAG_MOVE) {
				new_x = input_state.drag.start_win_x + dx;
				new_y = input_state.drag.start_win_y + dy;
			} else if (input_state.drag.type == DRAG_RESIZE) {
				new_width = input_state.drag.start_win_width + dx;
				new_height = input_state.drag.start_win_height + dy;

				if (new_width < 10)
					new_width = 10; // Minimum width
				if (new_height < 10)
					new_height = 10; // Minimum height
			}
			// Update client's position/size directly for visual feedback
			c->x = new_x;
			c->y = new_y;
			c->width = new_width;
			c->height = new_height;
			return 1;
		}
	}

	// This means nothing was handled
	return 0;
}

void* input_loop(void* arg) {
	(void)arg;

	printf("[BGCE] Input thread started\n");

	while (1) {
		int ret = poll(input_state.fds, input_state.count, -1);
		if (ret < 0) {
			if (errno == EINTR) {
				printf("EINTR\n");
				continue;
			}
			perror("[BGCE] poll");
			break;
		}

		for (size_t i = 0; i < input_state.count; i++) {
			struct input_event ev;

			if (input_state.fds[i].revents & POLLIN) {
				while (1) {
					ssize_t n = read(input_state.fds[i].fd, &ev, sizeof(ev));
					if (n == -1) {
						if (errno == EAGAIN || errno == EWOULDBLOCK)
							break;
						perror("read input");
						break;
					}
					if (n != sizeof(ev))
						break;

					printf("input: type=%d code=%d value=%d\n",
					       ev.type, ev.code, ev.value);

					if (handle_input_event(ev)) {
						printf("[BGCE] Event handled\n");
						continue;
					}

					if (!server.focused_client) {
						continue;
					}

					struct BGCEInputEvent e = {0};

					if (ev.type == EV_KEY) {
						e.type = INPUT_KEYBOARD;
						e.code = ev.code;
						e.value = ev.value;
					} else if (ev.type == EV_REL) {
						e.type = INPUT_MOUSE_MOVE;
						if (ev.code == REL_X)
							e.x = ev.value;
						else if (ev.code == REL_Y)
							e.y = ev.value;
					} else if (ev.type == EV_SYN) {
						continue;
					} else {
						continue;
					}

					for (int sub = 0; sub < MAX_INPUT_DEVICES; sub++) {
						if (server.focused_client->inputs[sub] == i) {
							/* Send to focused client */
							struct BGCEMessage msg;
							msg.type = MSG_INPUT_EVENT;
							memcpy(msg.data, &ev, sizeof(ev));

							bgce_send_msg(server.focused_client->fd, &msg);
						}
					}
				}
			}
		}
	}
	return NULL;
}
