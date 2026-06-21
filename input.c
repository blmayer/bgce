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
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#define test_bit(bit, array) ((array)[(bit) / 8] & (1 << ((bit) % 8)))
#define INPUT_DIR "/dev/input"

int ctrl_down = 0;
int alt_down = 0;
int mouse_x;
int mouse_y;

size_t count;
struct pollfd fds[MAX_INPUT_DEVICES];

struct {
	int active;
	struct Client* target;
	int dx;
	int dy;
	enum {
		DRAG_MOVE,
		DRAG_RESIZE
	} type;
} drag;

/* Per-device metadata for absolute pointer scaling */
static struct {
	int is_abs_pointer;
	int abs_x_min, abs_x_max;
	int abs_y_min, abs_y_max;
} dev_info[MAX_INPUT_DEVICES];

extern struct ServerState server;
extern struct config config;

int resize_buffer(struct Client* c, int dx, int dy) {
	// for resize we must reallocate the buffer
	// Unmap and unlink old buffer
	if (c->buffer) {
		munmap(c->buffer, c->width * c->height * BGCE_BYTES_PER_PIXEL);
		shm_unlink(c->shm_name);
	}

	// Create new shared memory name and object
	snprintf(c->shm_name, sizeof(c->shm_name),
	         "bgce_buf_%d_%ld", getpid(), time(NULL));
	int shm_fd = shm_open(c->shm_name, O_CREAT | O_RDWR, 0600);
	if (shm_fd < 0) {
		perror("[BGCE] shm_open for resize");
		return 0;
	}

	size_t buf_size = (c->width + dx) * (c->height + dy) * BGCE_BYTES_PER_PIXEL;
	if (ftruncate(shm_fd, buf_size) < 0) {
		perror("[BGCE] ftruncate for resize");
		close(shm_fd);
		return 0;
	}

	c->buffer = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (c->buffer == MAP_FAILED) {
		perror("[BGCE] mmap for resize");
		close(shm_fd);
		return 0;
	}

	c->width += dx;
	c->height += dy;
	close(shm_fd);
	printf("[BGCE] Client resized: %p size=%zu (%dx%d) name=%s\n",
	       c->buffer,
	       c->width * c->height * 4UL,
	       c->width, c->height,
	       c->shm_name);
	return 1;
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
	return (picked && picked->z > 0) ? picked : NULL; // avoid getting the background
}

int init_input(void) {
	count = 0;
	drag.active = 0;
	mouse_x = server.display_w / 2;
	mouse_y = server.display_h / 2;
	memset(dev_info, 0, sizeof(dev_info));

	printf("[BGCE] Input: scanning %s for devices (max %d slots)\n",
	       INPUT_DIR, MAX_INPUT_DEVICES);

	DIR* dir = opendir(INPUT_DIR);
	if (!dir) {
		fprintf(stderr, "[BGCE] Input: failed to open %s: %s\n",
		        INPUT_DIR, strerror(errno));
		return -1;
	}

	struct dirent* ent;
	while ((ent = readdir(dir)) != NULL && count < MAX_INPUT_DEVICES) {
		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		char path[256 + 12];
		snprintf(path, sizeof(path), "%s/%s", INPUT_DIR, ent->d_name);

		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			fprintf(stderr, "[BGCE] Input: cannot open %s: %s%s\n",
			        path, strerror(errno),
			        (errno == EACCES || errno == EPERM)
			            ? " (try adding your user to the 'input' group)"
			            : "");
			continue;
		}

		/* Device name */
		char name[256] = "Unknown";
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
			strcpy(name, "Unknown");

		/* Physical topology */
		char phys[256] = "";
		ioctl(fd, EVIOCGPHYS(sizeof(phys)), phys);

		/* Bus / vendor / product / version */
		struct input_id id = {0};
		ioctl(fd, EVIOCGID, &id);

		printf("[BGCE] Input: probing %s \"%s\"\n", path, name);
		printf("[BGCE]   phys: %s\n", phys[0] ? phys : "(none)");
		printf("[BGCE]   bus=0x%04x vendor=0x%04x product=0x%04x version=0x%04x\n",
		       id.bustype, id.vendor, id.product, id.version);

		/* Top-level event types */
		unsigned long ev_bits[(EV_MAX + 7) / 8] = {0};
		if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
			fprintf(stderr, "[BGCE]   EVIOCGBIT failed: %s — skipping\n",
			        strerror(errno));
			close(fd);
			continue;
		}

		int has_key = test_bit(EV_KEY, ev_bits);
		int has_rel = test_bit(EV_REL, ev_bits);
		int has_abs = test_bit(EV_ABS, ev_bits);

		printf("[BGCE]   event types:%s%s%s%s%s\n",
		       has_key                      ? " KEY" : "",
		       has_rel                      ? " REL" : "",
		       has_abs                      ? " ABS" : "",
		       test_bit(EV_MSC, ev_bits)    ? " MSC" : "",
		       test_bit(EV_SW,  ev_bits)    ? " SW"  : "");

		/* Detailed key/button bits */
		unsigned long key_bits[(KEY_MAX + 7) / 8] = {0};
		if (has_key)
			ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

		int has_mouse_btn = test_bit(BTN_LEFT,   key_bits) ||
		                    test_bit(BTN_RIGHT,  key_bits) ||
		                    test_bit(BTN_MIDDLE, key_bits) ||
		                    test_bit(BTN_MOUSE,  key_bits);
		int has_touch_btn = test_bit(BTN_TOUCH,  key_bits);
		int has_tool_finger = test_bit(BTN_TOOL_FINGER, key_bits);
		int has_kbd_key   = test_bit(KEY_A,     key_bits) ||
		                    test_bit(KEY_B,     key_bits) ||
		                    test_bit(KEY_SPACE, key_bits) ||
		                    test_bit(KEY_ENTER, key_bits) ||
		                    test_bit(KEY_ESC,   key_bits);

		if (has_key)
			printf("[BGCE]   buttons:%s%s%s%s%s%s%s\n",
			       test_bit(BTN_LEFT,   key_bits) ? " BTN_LEFT"   : "",
			       test_bit(BTN_RIGHT,  key_bits) ? " BTN_RIGHT"  : "",
			       test_bit(BTN_MIDDLE, key_bits) ? " BTN_MIDDLE" : "",
			       test_bit(BTN_TOUCH,  key_bits) ? " BTN_TOUCH"  : "",
			       test_bit(BTN_TOOL_FINGER, key_bits) ? " BTN_TOOL_FINGER" : "",
			       has_kbd_key                    ? " [has keyboard keys]" : "",
			       (!has_mouse_btn && !has_touch_btn && !has_kbd_key)
			                                      ? " (none relevant)" : "");

		/* REL sub-capabilities */
		if (has_rel) {
			unsigned long rel_bits[(REL_MAX + 7) / 8] = {0};
			ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits);
			printf("[BGCE]   rel axes:%s%s%s%s\n",
			       test_bit(REL_X,     rel_bits) ? " REL_X"     : "",
			       test_bit(REL_Y,     rel_bits) ? " REL_Y"     : "",
			       test_bit(REL_WHEEL, rel_bits) ? " REL_WHEEL" : "",
			       test_bit(REL_HWHEEL,rel_bits) ? " REL_HWHEEL": "");
		}

		/* ABS sub-capabilities and ranges */
		int has_abs_x = 0, has_abs_y = 0;
		struct input_absinfo abs_x_info = {0};
		struct input_absinfo abs_y_info = {0};
		if (has_abs) {
			unsigned long abs_bits[(ABS_MAX + 7) / 8] = {0};
			ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
			has_abs_x = test_bit(ABS_X, abs_bits);
			has_abs_y = test_bit(ABS_Y, abs_bits);
			printf("[BGCE]   abs axes:%s%s%s%s%s\n",
			       has_abs_x                           ? " ABS_X"        : "",
			       has_abs_y                           ? " ABS_Y"        : "",
			       test_bit(ABS_MT_POSITION_X, abs_bits) ? " ABS_MT_X"  : "",
			       test_bit(ABS_MT_POSITION_Y, abs_bits) ? " ABS_MT_Y"  : "",
			       test_bit(ABS_PRESSURE, abs_bits)      ? " PRESSURE"  : "");

			if (has_abs_x && ioctl(fd, EVIOCGABS(ABS_X), &abs_x_info) == 0)
				printf("[BGCE]   ABS_X range: %d..%d (res %d)\n",
				       abs_x_info.minimum, abs_x_info.maximum, abs_x_info.resolution);
			if (has_abs_y && ioctl(fd, EVIOCGABS(ABS_Y), &abs_y_info) == 0)
				printf("[BGCE]   ABS_Y range: %d..%d (res %d)\n",
				       abs_y_info.minimum, abs_y_info.maximum, abs_y_info.resolution);
		}

		if (!has_key && !has_rel && !has_abs) {
			printf("[BGCE]   SKIP: no KEY/REL/ABS capabilities\n");
			close(fd);
			continue;
		}

		/* Accept mice, touchpads, abs pointers, keyboards */
		int accept = 0;
		const char* accept_reason = NULL;

		if (has_rel) {
			accept = 1;
			accept_reason = "relative pointer (mouse/trackball)";
		}
		if (has_abs && has_mouse_btn) {
			accept = 1;
			accept_reason = "absolute pointer with mouse buttons (touchpad/tablet)";
		}
		if (has_abs && has_touch_btn && has_abs_x && has_abs_y) {
			accept = 1;
			accept_reason = "touchscreen (BTN_TOUCH + ABS_X/Y)";
		}
		if (has_abs && has_tool_finger && has_abs_x && has_abs_y) {
			accept = 1;
			accept_reason = "touchpad (BTN_TOOL_FINGER + ABS_X/Y)";
		}
		if (has_key && (has_kbd_key || has_mouse_btn)) {
			accept = 1;
			accept_reason = has_kbd_key ? "keyboard" : "button device";
		}

		if (!accept) {
			printf("[BGCE]   SKIP: does not match any known device pattern\n");
			close(fd);
			continue;
		}

		printf("[BGCE]   ACCEPT: %s\n", accept_reason);

		/* Build type_mask */
		uint16_t type_mask = 0;
		if (has_key) type_mask |= (1 << EV_KEY);
		if (has_rel) type_mask |= (1 << EV_REL);
		if (has_abs) type_mask |= (1 << EV_ABS);

		fds[count].fd = fd;
		fds[count].events = POLLIN;

		server.input.devs[count].id = count;
		server.input.devs[count].type_mask = type_mask;
		strncpy(server.input.devs[count].name, name,
		        sizeof(server.input.devs[count].name) - 1);

		/* Store ABS range for absolute pointer scaling */
		if (has_abs && has_abs_x && has_abs_y &&
		    (has_mouse_btn || has_touch_btn || has_tool_finger)) {
			dev_info[count].is_abs_pointer = 1;
			dev_info[count].abs_x_min = abs_x_info.minimum;
			dev_info[count].abs_x_max = abs_x_info.maximum;
			dev_info[count].abs_y_min = abs_y_info.minimum;
			dev_info[count].abs_y_max = abs_y_info.maximum;
			printf("[BGCE]   -> registered as absolute pointer (slot %zu)\n", count);
		}

		count++;
	}

	closedir(dir);

	printf("[BGCE] Input: %zu device(s) accepted\n", count);
	if (count == 0) {
		fprintf(stderr, "[BGCE] Input: WARNING — no suitable input devices found\n");
		fprintf(stderr, "[BGCE] Input:   check permissions (user in 'input' group?)\n");
		fprintf(stderr, "[BGCE] Input:   check that mouse/keyboard is plugged in\n");
		server.input.count = 0;
		return 0;
	}
	server.input.count = count;

	return 0;
}

/*
 * Check if current modifier+key state matches a configured shortcut.
 * Returns pointer to the matching shortcut, or NULL.
 */
static struct shortcut *match_shortcut(int ctrl, int alt, int shift, uint16_t key) {
	for (int i = 0; i < config.shortcut_count; i++) {
		struct shortcut* sc = &config.shortcuts[i];
		if (sc->combo.ctrl == ctrl &&
		    sc->combo.alt == alt &&
		    sc->combo.shift == shift &&
		    sc->combo.key == key) {
			return sc;
		}
	}
	return NULL;
}

/*
 * Keyboard shortcuts are configured via the config file ([shortcuts] section).
 * Built-in behavior for mouse modifiers:
 *  ALT + LEFT_CLICK + DRAG: move
 *  ALT + RIGHT_CLICK + DRAG: resize
 *
 *  Returns if shortcut was handled
 */
static int handle_input_event(struct input_event ev, size_t dev_idx) {
	if (ev.type == EV_KEY && ev.value == 1) { // Key press
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL) {
			printf("[BGCE] Ctrl pressed.\n");
			ctrl_down = 1;
		}
		if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT) {
			printf("[BGCE] Alt pressed.\n");
			alt_down = 1;
		}

		// Check configured keyboard shortcuts (key press)
		struct shortcut *sc = match_shortcut(ctrl_down, alt_down, 0, ev.code);
		if (sc) {
			if (sc->type == SHORTCUT_BUILTIN) {
				if (strcmp(sc->value, "exit") == 0) {
					printf("[BGCE] Exit shortcut triggered, exiting.\n");
					exit(1);
				} else if (strcmp(sc->value, "screenshot") == 0) {
					printf("[BGCE] Screenshot shortcut triggered.\n");
					if (server.focused_client) {
						return 0;
					}
					take_screenshot("screenshot.png");
					return 1;
				}
			} else if (sc->type == SHORTCUT_COMMAND) {
				printf("[BGCE] Command shortcut triggered: %s\n", sc->value);
				pid_t pid = fork();
				if (pid == 0) {
					/* child: run command via shell, do not block server */
					execl("/bin/sh", "sh", "-c", sc->value, (char *)NULL);
					_exit(127);
				} else if (pid < 0) {
					perror("[BGCE] fork for shortcut command");
				}
				return 1;
			}
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
			set_cursor_type(BGCE_CURSOR_DEFAULT);
			if (drag.type == DRAG_MOVE) {
				drag.active = 0;
				drag.target = NULL;
				return 1;
			}

			struct Client* c = drag.target;
			if (resize_buffer(c, drag.dx, drag.dy)) {
				printf("[BGCE] Redrawing dx=%d dy=%d.\n", drag.dx, drag.dy);
				if (drag.dx < 0 || drag.dy < 0) {
					redraw_from_resize(
					        &server,
					        *c,
					        drag.dx,
					        drag.dy);
				}
				draw(&server, *c);

				struct BGCEMessage msg;
				msg.type = MSG_BUFFER_CHANGE;
				struct BufferReply reply = {0};
				strncpy(reply.shm_name, c->shm_name, sizeof(reply.shm_name));
				reply.width = c->width;
				reply.height = c->height;
				msg.data.buffer_reply = reply;
				bgce_send_msg(c->fd, &msg);
			}
			drag.active = 0;
			drag.target = NULL;

			return 1;
		}
	}

	if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && ev.value == 1) {
		printf("[BGCE] Click detected at (%d, %d).\n", mouse_x, mouse_y);

		/* switch focus */
		struct Client* c = pick_client(mouse_x, mouse_y);
		struct Client* old_focus = server.focused_client;
		if (!c) {
			if (old_focus) {
				struct BGCEMessage lost = {0};
				lost.type = MSG_FOCUS_CHANGE;
				lost.data.focus_event.state = 0;
				bgce_send_msg(old_focus->fd, &lost);
			}
			server.focused_client = NULL;
			return 0;
		}
		printf("[BGCE] Click detected at client %s z=%d.\n", c->shm_name, c->z);

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

		if (c != old_focus) {
			if (old_focus) {
				struct BGCEMessage lost = {0};
				lost.type = MSG_FOCUS_CHANGE;
				lost.data.focus_event.state = 0;
				bgce_send_msg(old_focus->fd, &lost);
			}

			/* keep z monotonic (if no previous focus, keep current z) */
			if (old_focus) {
				c->z = old_focus->z + 1;
			}
			server.focused_client = c;

			struct BGCEMessage got = {0};
			got.type = MSG_FOCUS_CHANGE;
			got.data.focus_event.state = 1;
			bgce_send_msg(c->fd, &got);

			draw(&server, *c);
			printf("[BGCE] Client focused.\n");
		}

		if (!alt_down) {
			return 0; // Left click
		}

		printf("[BGCE] Alt is pressed, starting move/resize.\n");
		drag.active = 1;
		drag.target = c;
		drag.dx = 0;
		drag.dy = 0;

		if (ev.code == BTN_RIGHT) {
			drag.type = DRAG_RESIZE;
			set_cursor_type(BGCE_CURSOR_RESIZE_NWSE);
			printf("[BGCE] Resize event.\n");
		} else {
			drag.type = DRAG_MOVE;
			set_cursor_type(BGCE_CURSOR_MOVE);
			printf("[BGCE] Move event.\n");
		}

		return 1;
	}

	if (ev.type == EV_REL) {
		int dx = 0;
		int dy = 0;
		switch (ev.code) {
		case REL_X:
			dx += ev.value;
			break;
		case REL_Y:
			dy += ev.value;
		}
		mouse_x += dx;
		mouse_y += dy;

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
				// Accumulate new width and height
				drag.dx += dx;
				drag.dy += dy;
			}
			return 1;
		}
	}

	/* Absolute pointer events (touchpads, touchscreens, tablets) */
	if (ev.type == EV_ABS && dev_idx < MAX_INPUT_DEVICES &&
	    dev_info[dev_idx].is_abs_pointer) {
		int old_x = mouse_x;
		int old_y = mouse_y;

		if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
			int range = dev_info[dev_idx].abs_x_max - dev_info[dev_idx].abs_x_min;
			if (range > 0) {
				mouse_x = (ev.value - dev_info[dev_idx].abs_x_min)
				          * (int)server.display_w / range;
			} else {
				mouse_x = ev.value;
			}
		}
		if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
			int range = dev_info[dev_idx].abs_y_max - dev_info[dev_idx].abs_y_min;
			if (range > 0) {
				mouse_y = (ev.value - dev_info[dev_idx].abs_y_min)
				          * (int)server.display_h / range;
			} else {
				mouse_y = ev.value;
			}
		}

		/* Clamp */
		if (mouse_x < 0) mouse_x = 0;
		if (mouse_y < 0) mouse_y = 0;
		if (mouse_x > (int)server.display_w) mouse_x = server.display_w;
		if (mouse_y > (int)server.display_h) mouse_y = server.display_h;

		drmModeMoveCursor(
		        server.drm_fd,
		        server.crtc_id,
		        mouse_x,
		        mouse_y);

		if (drag.active) {
			struct Client* c = drag.target;
			if (!c) return 1;

			int dx = mouse_x - old_x;
			int dy = mouse_y - old_y;

			switch (drag.type) {
			case DRAG_MOVE:
				redraw_region(&server, *c, dx, dy);
				c->x = c->x + dx;
				c->y = c->y + dy;
				draw(&server, *c);
				break;
			case DRAG_RESIZE:
				drag.dx += dx;
				drag.dy += dy;
			}
			return 1;
		}
	}

	// This means nothing was handled
	return 0;
}

void* input_loop(void* arg) {
	(void)arg;

	if (count == 0) {
		printf("[BGCE] Input: no devices, input thread parked\n");
		while (1) pause();
		return NULL;
	}

	printf("[BGCE] Input: polling %zu device(s)\n", count);

	while (1) {
		int ret = poll(fds, count, -1);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[BGCE] Input: poll error: %s\n", strerror(errno));
			break;
		}

		for (size_t i = 0; i < count; i++) {
			if (fds[i].revents & (POLLHUP | POLLERR)) {
				fprintf(stderr, "[BGCE] Input: device slot %zu (\"%s\") disconnected or error\n",
				        i, server.input.devs[i].name);
				close(fds[i].fd);
				fds[i].fd = -1;
				fds[i].events = 0;
				continue;
			}

			if (!(fds[i].revents & POLLIN))
				continue;

			struct input_event ev;
			ssize_t n = read(fds[i].fd, &ev, sizeof(ev));
			if (n == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				fprintf(stderr, "[BGCE] Input: read error on slot %zu: %s\n",
				        i, strerror(errno));
				continue;
			}
			if (n != sizeof(ev))
				continue;

			if (handle_input_event(ev, i))
				continue;

			if (!server.focused_client)
				continue;

			struct Client c = *server.focused_client;

			struct InputEvent e = {0};
			e.type = ev.type;
			e.device = server.input.devs[i];
			e.code = ev.code;
			e.value = ev.value;

			switch (ev.type) {
			case EV_KEY:
				if (ev.code != BTN_LEFT && ev.code != BTN_RIGHT)
					break;
				/* fall through — mouse buttons carry position */
			case EV_REL:
			case EV_ABS: {
				int in = mouse_x >= (int)c.x &&
				         mouse_x <= (int)(c.x + c.width) &&
				         mouse_y >= (int)c.y &&
				         mouse_y <= (int)(c.y + c.height);
				if (!in)
					continue;

				e.x = mouse_x - c.x;
				e.y = mouse_y - c.y;
				break;
			}
			default:
				continue;
			}

			/* Send to focused client */
			struct BGCEMessage msg;
			msg.type = MSG_INPUT_EVENT;
			msg.data.input_event = e;
			bgce_send_msg(c.fd, &msg);
		}
	}
	return NULL;
}
