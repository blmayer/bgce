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

int ctrl_down;
int alt_down;
int mouse_x;
int mouse_y;

size_t count;
struct pollfd fds[MAX_INPUT_DEVICES];

struct {
	int active;
	struct Client* target;
	int start_mouse_x;
	int start_mouse_y;
	enum {
		DRAG_MOVE,
		DRAG_RESIZE
	} type;
} drag;

extern struct ServerState server;

void resize_buffer(struct Client* c) {
	// for resize we must reallocate the buffer
	// Unmap and unlink old buffer
	if (c->buffer) {
		munmap(c->buffer, c->width * c->height * BGCE_BYTES_PER_PIXEL);
		shm_unlink(c->shm_name);
	}

	// Create new shared memory name and object
	snprintf(c->shm_name, sizeof(c->shm_name),
	         "/bgce_buf_%d_%ld", getpid(), time(NULL));
	int shm_fd = shm_open(c->shm_name, O_CREAT | O_RDWR, 0600);
	if (shm_fd < 0) {
		perror("shm_open for resize");
		bgce_send_msg(client_fd, &msg);
		break;
	}

	size_t buf_size = req.width * req.height * BGCE_BYTES_PER_PIXEL;
	if (ftruncate(shm_fd, buf_size) < 0) {
		perror("ftruncate for resize");
		close(shm_fd);
		bgce_send_msg(client_fd, &msg);
		break;
	}

	c->buffer = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (c->buffer == MAP_FAILED) {
		perror("mmap for resize");
		close(shm_fd);
		bgce_send_msg(client_fd, &msg);
		break;
	}
	c->width = req.width;
	c->height = req.height;
	close(shm_fd);
}

struct Client* pick_client(int x, int y) {
	// Iterate through clients to find the topmost client under the cursor
	struct Client* c = server.clients;
	struct Client* picked = NULL;
	while (c) {
		if (x >= c->x && x <= (c->x + c->width) &&
		    y >= c->y && y <= (c->y + c->height)) {
			picked = c;
			break;
		}
		c = c->next;
	}
	return picked->z > 0 ? picked : NULL; // avoid getting the background
}

int init_input(void) {
	count = 0;
	drag.active = 0;
	mouse_x = server.display_w / 2;
	mouse_y = server.display_h / 2;

	DIR* dir = opendir(INPUT_DIR);
	if (!dir) {
		perror("[BGCE] Failed to open /dev/input");
		return -1;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL && count < MAX_INPUT_DEVICES) {
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

		fds[count].fd = fd;
		fds[count].events = POLLIN;

		server.input.devs[count].id = count;
		strcpy(server.input.devs[count].name, name);

		count++;

		printf("[BGCE] Input device accepted: %s (%s)%s%s\n",
		       path, name,
		       has_key ? " [KEY]" : "",
		       has_rel ? " [REL]" : "");
	}

	closedir(dir);

	if (count == 0) {
		fprintf(stderr, "[BGCE] No suitable input devices found\n");
		return -1;
	}
	server.input.count = count;

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
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
			printf("[BGCE] Ctrl pressed.\n");
			ctrl_down = 1;
		}
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
			printf("[BGCE] Alt pressed.\n");
			alt_down = 1;
		}
		if (ctrl_down == 1 && alt_down == 1 && ev.code == KEY_Q) {
			printf("[BGCE] Ctrl+Alt+Q pressed, exiting.\n");
			exit(1);
		}
	}

	if (ev.type == EV_KEY && ev.value == 0) { // Key release
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
			printf("[BGCE] Ctrl released.\n");
			ctrl_down = 0;
		}
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
			printf("[BGCE] Alt released.\n");
			alt_down = 0;
		}

		// Stop drag/move on button release
		if ((ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && drag.active) { // Only stop if it was an active drag of that type
			printf("[BGCE] End of drag event.\n");
			struct Client* c = drag.target;
			drag.active = 0;
			drag.target = NULL;

			if (drag.type == DRAG_RESIZE) {
				int resize_client(c);
			}
			return 1;
		}
	}

	if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && ev.value == 1) {
		int mx = mouse_x;
		int my = mouse_y;
		printf("[BGCE] Click detected at (%d, %d).\n", mx, my);

		// switch focuse
		struct Client* c = pick_client(mx, my);
		if (!c) {
			return 0;
		}

		if (c != server.focused_client) {
			printf("[BGCE] Click detected at client %s.\n", c->shm_name);

			// If the clicked client is not already the first, move it
			if (c != server.clients) {
				struct Client* prev = server.clients;
				while (prev && prev->next != c) {
					prev = prev->next;
				}
				if (prev) {
					prev->next = c->next;
					c->next = server.clients;
					server.clients = c;
				}
			}
			c->z = server.focused_client->z + 1;
			server.focused_client = c;
			draw(&server, *c);
			printf("[BGCE] Client focused.\n");
		}

		if (!alt_down) {
			return 0; // Alt + Left click down
		}

		printf("[BGCE] Alt is pressed, starting move/resize.\n");
		drag.start_mouse_x = mx;
		drag.start_mouse_y = my;
		drag.active = 1;
		drag.target = c;

		if (ev.code == BTN_RIGHT) {
			drag.type = DRAG_RESIZE;
			printf("[BGCE] Resize event.\n");
		} else {
			drag.type = DRAG_MOVE;
			printf("[BGCE] Move event.\n");
		}

		return 1;
	}

	if (ev.type == EV_REL) {
		if (ev.code == REL_X)
			mouse_x += ev.value;
		else if (ev.code == REL_Y)
			mouse_y += ev.value;

		// Clamp mouse coordinates to screen boundaries
		if (mouse_x < 0)
			mouse_x = 0;
		if (mouse_y < 0)
			mouse_y = 0;
		if (mouse_x > server.display_w)
			mouse_x = server.display_w;
		if (mouse_y > server.display_h)
			mouse_y = server.display_h;

		drmModeMoveCursor(
		        server.drm_fd,
		        server.crtc_id,
		        mouse_x,
		        mouse_y);

		if (drag.active) {
			int dx = mouse_x - drag.start_mouse_x;
			int dy = mouse_y - drag.start_mouse_y;
			drag.start_mouse_x = mouse_x;
			drag.start_mouse_y = mouse_y;
			struct Client* c = drag.target;
			if (!c) {
				printf("[BGCE] No client to drag\n");
				return 1; // Should not happen
			}

			switch (drag.type) {
			case DRAG_MOVE:
				// if moving, redraw old region.
				if (drag.type == DRAG_MOVE) {
					redraw_region(&server, *c, dx, dy);
				}

				// Update client's position
				c->x = c->x + dx;
				c->y = c->y + dy;
				draw(&server, *c);
				break;

			case DRAG_RESIZE:
				// Calculate new width and height
				c->width += dx;
				c->height += dy;

				// Enforce minimum size
				if (c->width < 10)
					new_width = 10;
				if (c->height < 10)
					new_height = 10;

				break;
			}
		}
		return 1;
	}

	// This means nothing was handled
	return 0;
}

void* input_loop(void* arg) {
	(void)arg;

	while (1) {
		int ret = poll(fds, count, -1);
		if (ret < 0) {
			if (errno == EINTR) {
				printf("EINTR\n");
				continue;
			}
			perror("[BGCE] poll");
			break;
		}

		for (size_t i = 0; i < count; i++) {
			struct input_event ev;

			if (fds[i].revents & POLLIN) {
				ssize_t n = read(fds[i].fd, &ev, sizeof(ev));
				if (n == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK) {
						perror("[BGCE] read input");
						break;
					}
					perror("read input");
					break;
				}
				if (n != sizeof(ev))
					break;

				if (handle_input_event(ev)) {
					continue;
				}

				if (!server.focused_client) {
					continue;
				}

				struct InputEvent e = {0};
				e.device = server.input.devs[i];
				e.code = ev.code;

				if (ev.type == EV_KEY) {
					e.value = ev.value;
				} else if (ev.type == EV_REL) {
					if (ev.code == REL_X)
						e.value = ev.value;
					else if (ev.code == REL_Y)
						e.value = ev.value;
				} else if (ev.type == EV_SYN) {
					continue;
				} else {
					continue;
				}

				/* Send to focused client */
				struct BGCEMessage msg;
				msg.type = MSG_INPUT_EVENT;
				msg.data.input_event = e;
				bgce_send_msg(server.focused_client->fd, &msg);
			}
		}
	}
	return NULL;
}
