#include "bgce.h"   /* for access to global server state */
#include "server.h" /* for access to global server state */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
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
int shift_down = 0;
int mouse_x;
int mouse_y;

/* Accumulate REL/ABS until EV_SYN so the software cursor is painted once
 * per hardware report, not once per axis. */
static int pointer_dirty;
static int skip_cursor_paint; /* set when a redraw will repaint the cursor */

size_t count;
struct pollfd fds[MAX_INPUT_DEVICES];

struct {
	int active;
	struct Client* target;
	int dx;
	int dy;
	/* Sub-pixel accumulators so fine zoom still moves windows smoothly */
	float acc_x;
	float acc_y;
	enum {
		DRAG_MOVE,
		DRAG_RESIZE,
		DRAG_PAN
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
	char old_name[64];
	size_t buf_size;
	int shm_fd;
	void *map;
	uint32_t new_world_w, new_world_h, new_w, new_h;
	uint32_t old_ww, old_wh;

	if (!c)
		return 0;

	old_ww = c->world_w ? c->world_w : c->width;
	old_wh = c->world_h ? c->world_h : c->height;
	new_world_w = (uint32_t)((int)old_ww + dx);
	new_world_h = (uint32_t)((int)old_wh + dy);
	if (new_world_w == 0 || new_world_h == 0)
		return 0;

	/*
	 * Keep buffer:world ratio so content scale matches creation zoom.
	 * Client is told the new buffer size via MSG_BUFFER_CHANGE.
	 */
	new_w = (uint32_t)((float)new_world_w * (float)c->width / (float)old_ww + 0.5f);
	new_h = (uint32_t)((float)new_world_h * (float)c->height / (float)old_wh + 0.5f);
	if (new_w < 1)
		new_w = 1;
	if (new_h < 1)
		new_h = 1;

	old_name[0] = '\0';
	if (c->buffer) {
		munmap(c->buffer, c->width * c->height * BGCE_BYTES_PER_PIXEL);
		c->buffer = NULL;
		strncpy(old_name, c->shm_name, sizeof(old_name) - 1);
		old_name[sizeof(old_name) - 1] = '\0';
	}

	buf_size = (size_t)new_w * new_h * BGCE_BYTES_PER_PIXEL;
	shm_fd = bgce_buf_create(c->shm_name, sizeof(c->shm_name), buf_size);
	if (shm_fd < 0) {
		perror("[BGCE] create buffer for resize");
		if (old_name[0])
			bgce_buf_unlink(old_name);
		return 0;
	}

	map = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (map == MAP_FAILED) {
		perror("[BGCE] mmap for resize");
		bgce_buf_unlink(c->shm_name);
		c->shm_name[0] = '\0';
		if (old_name[0])
			bgce_buf_unlink(old_name);
		return 0;
	}

	if (old_name[0])
		bgce_buf_unlink(old_name);

	c->buffer = map;
	c->width = new_w;
	c->height = new_h;
	c->world_w = new_world_w;
	c->world_h = new_world_h;
	printf("[BGCE] Client resized: %p size=%zu buf=%ux%u world=%ux%u name=%s\n",
	       c->buffer,
	       c->width * c->height * 4UL,
	       c->width, c->height,
	       c->world_w, c->world_h,
	       c->shm_name);
	return 1;
}

/* Pick topmost non-background client under screen-space (x, y). */
struct Client* pick_client(int x, int y) {
	float wx, wy;
	screen_to_world(&server, (float)x, (float)y, &wx, &wy);

	struct Client* c = server.clients;
	struct Client* picked = NULL;
	while (c) {
		uint32_t ww = c->world_w ? c->world_w : c->width;
		uint32_t wh = c->world_h ? c->world_h : c->height;
		if (wx >= (float)c->x && wx < (float)(c->x + ww) &&
		    wy >= (float)c->y && wy < (float)(c->y + wh)) {
			picked = c;
			break;
		}
		c = c->next;
	}
	return (picked && picked->z > 0) ? picked : NULL; /* skip background */
}

/* Convert a screen-pixel delta to an integer world-pixel delta, with
 * fractional accumulation so slow drags still move at high zoom. */
static void screen_delta_to_world(float sdx, float sdy, float* acc_x, float* acc_y,
                                  int* wdx, int* wdy) {
	float z = server.zoom > 0.0f ? server.zoom : 1.0f;
	*acc_x += sdx / z;
	*acc_y += sdy / z;
	*wdx = (int)truncf(*acc_x);
	*wdy = (int)truncf(*acc_y);
	*acc_x -= (float)*wdx;
	*acc_y -= (float)*wdy;
}

static void apply_zoom_at_cursor(float factor) {
	float old_zoom = server.zoom;
	float new_zoom = old_zoom * factor;
	if (new_zoom < BGCE_ZOOM_MIN)
		new_zoom = BGCE_ZOOM_MIN;
	if (new_zoom > BGCE_ZOOM_MAX)
		new_zoom = BGCE_ZOOM_MAX;
	if (fabsf(new_zoom - old_zoom) < 1e-6f)
		return;

	/* Keep the world point under the cursor fixed on screen. */
	float wx, wy;
	screen_to_world(&server, (float)mouse_x, (float)mouse_y, &wx, &wy);
	server.zoom = new_zoom;
	server.pan_x = wx - (float)mouse_x / new_zoom;
	server.pan_y = wy - (float)mouse_y / new_zoom;
	clamp_viewport(&server);
	redraw_all(&server);
	printf("[BGCE] Zoom: %.2f  pan=(%.1f, %.1f)\n",
	       server.zoom, server.pan_x, server.pan_y);
}

void client_disconnected(struct Client* c)
{
	if (!c)
		return;
	if (drag.active && drag.target == c) {
		drag.active = 0;
		drag.target = NULL;
		drag.dx = 0;
		drag.dy = 0;
		drag.acc_x = 0;
		drag.acc_y = 0;
		set_cursor_type(BGCE_CURSOR_DEFAULT);
		printf("[BGCE] Drag cancelled (client disconnected).\n");
	}
}

int init_input(void) {
	count = 0;
	drag.active = 0;
	mouse_x = (int)server.display_w / 2;
	mouse_y = (int)server.display_h / 2;
	if (mouse_x > (int)server.display_w - CURSOR_WIDTH)
		mouse_x = (int)server.display_w - CURSOR_WIDTH;
	if (mouse_y > (int)server.display_h - CURSOR_HEIGHT)
		mouse_y = (int)server.display_h - CURSOR_HEIGHT;
	if (mouse_x < 0) mouse_x = 0;
	if (mouse_y < 0) mouse_y = 0;
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

		int fd = open(path, O_RDONLY | O_CLOEXEC);
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
 * Required modifiers on the binding must be held; bare keys (no mods in the
 * binding) only match when no mods are held.
 */
static struct shortcut *match_shortcut(int ctrl, int alt, int shift, uint16_t key) {
	for (int i = 0; i < config.shortcut_count; i++) {
		struct shortcut* sc = &config.shortcuts[i];
		int bare;

		if (sc->combo.key != key)
			continue;
		if (sc->combo.ctrl && !ctrl)
			continue;
		if (sc->combo.alt && !alt)
			continue;
		if (sc->combo.shift && !shift)
			continue;
		bare = !sc->combo.ctrl && !sc->combo.alt && !sc->combo.shift;
		if (bare && (ctrl || alt || shift))
			continue;
		return sc;
	}
	return NULL;
}

/*
 * Keyboard shortcuts are configured via the config file ([shortcuts] section).
 * Built-in behavior for mouse modifiers:
 *  ALT + LEFT_CLICK + DRAG on a client: move window
 *  ALT + RIGHT_CLICK + DRAG on a client: resize window
 *  ALT + LEFT_CLICK + DRAG on empty space: pan the desktop
 *  ALT + SCROLL: zoom in/out (centered on cursor)
 *
 *  Returns if shortcut was handled
 */
static int handle_input_event(struct input_event ev, size_t dev_idx) {
	(void)dev_idx;

	if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 2)) {
		/* value 1 = press, 2 = autorepeat — update mod state on both */
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
			ctrl_down = 1;
		else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
			alt_down = 1;
		else if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
			shift_down = 1;

		/* Only fire shortcuts on the non-modifier key of the chord. */
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL ||
		    ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT ||
		    ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
			return 0;

		/* Press only (not autorepeat) for actions */
		if (ev.value != 1)
			return 0;

		struct shortcut *sc =
		        match_shortcut(ctrl_down, alt_down, shift_down, ev.code);
		if (sc) {
			if (sc->type == SHORTCUT_BUILTIN) {
				if (strcmp(sc->value, "exit") == 0) {
					/* Do not printf here: a full log pipe can block
					 * forever so exit() never runs. */
					bgce_request_shutdown();
					return 1; /* not reached */
				} else if (strcmp(sc->value, "screenshot") == 0) {
					if (server.focused_client)
						return 0;
					take_screenshot("screenshot.png");
					return 1;
				}
			} else if (sc->type == SHORTCUT_COMMAND) {
				pid_t pid = fork();
				if (pid == 0) {
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
		if (ev.code == KEY_LEFTCTRL || ev.code == KEY_RIGHTCTRL)
			ctrl_down = 0;
		else if (ev.code == KEY_LEFTALT || ev.code == KEY_RIGHTALT)
			alt_down = 0;
		else if (ev.code == KEY_LEFTSHIFT || ev.code == KEY_RIGHTSHIFT)
			shift_down = 0;

		// Stop drag/move/pan on button release
		if ((ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && drag.active) {
			printf("[BGCE] End of drag event.\n");
			set_cursor_type(BGCE_CURSOR_DEFAULT);

			if (drag.type == DRAG_MOVE || drag.type == DRAG_PAN) {
				drag.active = 0;
				drag.target = NULL;
				drag.acc_x = 0;
				drag.acc_y = 0;
				return 1;
			}

			/* DRAG_RESIZE */
			struct Client* c = drag.target;
			if (c && resize_buffer(c, drag.dx, drag.dy)) {
				printf("[BGCE] Redrawing dx=%d dy=%d.\n", drag.dx, drag.dy);
				if (drag.dx < 0 || drag.dy < 0) {
					redraw_from_resize(&server, *c, drag.dx, drag.dy);
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
			drag.acc_x = 0;
			drag.acc_y = 0;
			return 1;
		}
	}

	if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && ev.value == 1) {
		printf("[BGCE] Click detected at (%d, %d).\n", mouse_x, mouse_y);

		struct Client* c = pick_client(mouse_x, mouse_y);
		struct Client* old_focus = server.focused_client;

		/* Alt + left click on empty space → pan the desktop */
		if (!c && alt_down && ev.code == BTN_LEFT) {
			if (old_focus) {
				struct BGCEMessage lost = {0};
				lost.type = MSG_FOCUS_CHANGE;
				lost.data.focus_event.state = 0;
				bgce_send_msg(old_focus->fd, &lost);
			}
			server.focused_client = NULL;
			drag.active = 1;
			drag.target = NULL;
			drag.type = DRAG_PAN;
			drag.dx = 0;
			drag.dy = 0;
			drag.acc_x = 0;
			drag.acc_y = 0;
			set_cursor_type(BGCE_CURSOR_MOVE);
			printf("[BGCE] Pan desktop started.\n");
			return 1;
		}

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
		drag.acc_x = 0;
		drag.acc_y = 0;

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

	/* Alt + scroll wheel → zoom toward cursor */
	if (ev.type == EV_REL && ev.code == REL_WHEEL && alt_down) {
		/* Positive value = scroll up = zoom in */
		float factor = (ev.value > 0) ? BGCE_ZOOM_STEP : (1.0f / BGCE_ZOOM_STEP);
		int steps = ev.value >= 0 ? ev.value : -ev.value;
		if (steps < 1)
			steps = 1;
		for (int i = 0; i < steps; i++)
			apply_zoom_at_cursor(factor);
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
			break;
		default:
			/* Wheel without alt, etc. — not a pointer move */
			return 0;
		}
		mouse_x += dx;
		mouse_y += dy;

		// Clamp mouse coordinates to screen boundaries
		{
			int max_x = (int)server.display_w - CURSOR_WIDTH;
			int max_y = (int)server.display_h - CURSOR_HEIGHT;
			if (max_x < 0) max_x = 0;
			if (max_y < 0) max_y = 0;
			if (mouse_x < 0) mouse_x = 0;
			if (mouse_y < 0) mouse_y = 0;
			if (mouse_x > max_x) mouse_x = max_x;
			if (mouse_y > max_y) mouse_y = max_y;
		}

		pointer_dirty = 1;

		if (drag.active) {
			switch (drag.type) {
			case DRAG_PAN: {
				float z = server.zoom > 0.0f ? server.zoom : 1.0f;
				float old_px = server.pan_x;
				float old_py = server.pan_y;
				server.pan_x -= (float)dx / z;
				server.pan_y -= (float)dy / z;
				clamp_viewport(&server);
				skip_cursor_paint = 1;
				/* Shift existing FB pixels; only paint exposed edges. */
				redraw_pan(&server, old_px, old_py);
				break;
			}
			case DRAG_MOVE: {
				struct Client* c = drag.target;
				if (!c)
					return 1;
				int wdx, wdy;
				screen_delta_to_world((float)dx, (float)dy,
				                     &drag.acc_x, &drag.acc_y, &wdx, &wdy);
				if (wdx || wdy) {
					skip_cursor_paint = 1;
					redraw_region(&server, *c, wdx, wdy);
					c->x = (uint32_t)((int)c->x + wdx);
					c->y = (uint32_t)((int)c->y + wdy);
					draw(&server, *c);
				}
				break;
			}
			case DRAG_RESIZE: {
				int wdx, wdy;
				screen_delta_to_world((float)dx, (float)dy,
				                     &drag.acc_x, &drag.acc_y, &wdx, &wdy);
				drag.dx += wdx;
				drag.dy += wdy;
				break;
			}
			}
			return 1;
		}
		return 0;
	}

	if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
		if (pointer_dirty) {
			if (!skip_cursor_paint)
				set_cursor_pos(&server, mouse_x, mouse_y);
			pointer_dirty = 0;
			skip_cursor_paint = 0;
		}
		return 0;
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
		{
			int max_x = (int)server.display_w - CURSOR_WIDTH;
			int max_y = (int)server.display_h - CURSOR_HEIGHT;
			if (max_x < 0) max_x = 0;
			if (max_y < 0) max_y = 0;
			if (mouse_x < 0) mouse_x = 0;
			if (mouse_y < 0) mouse_y = 0;
			if (mouse_x > max_x) mouse_x = max_x;
			if (mouse_y > max_y) mouse_y = max_y;
		}

		pointer_dirty = 1;

		if (drag.active) {
			int dx = mouse_x - old_x;
			int dy = mouse_y - old_y;

			switch (drag.type) {
			case DRAG_PAN: {
				float z = server.zoom > 0.0f ? server.zoom : 1.0f;
				float old_px = server.pan_x;
				float old_py = server.pan_y;
				server.pan_x -= (float)dx / z;
				server.pan_y -= (float)dy / z;
				clamp_viewport(&server);
				skip_cursor_paint = 1;
				redraw_pan(&server, old_px, old_py);
				break;
			}
			case DRAG_MOVE: {
				struct Client* c = drag.target;
				if (!c)
					return 1;
				int wdx, wdy;
				screen_delta_to_world((float)dx, (float)dy,
				                     &drag.acc_x, &drag.acc_y, &wdx, &wdy);
				if (wdx || wdy) {
					skip_cursor_paint = 1;
					redraw_region(&server, *c, wdx, wdy);
					c->x = (uint32_t)((int)c->x + wdx);
					c->y = (uint32_t)((int)c->y + wdy);
					draw(&server, *c);
				}
				break;
			}
			case DRAG_RESIZE: {
				int wdx, wdy;
				screen_delta_to_world((float)dx, (float)dy,
				                     &drag.acc_x, &drag.acc_y, &wdx, &wdy);
				drag.dx += wdx;
				drag.dy += wdy;
				break;
			}
			}
			return 1;
		}
	}

	// This means nothing was handled
	return 0;
}

/* Send one EV_KEY to the focused client (no-op if nothing focused). */
static void send_key_to_focused(uint16_t code, int32_t value)
{
	struct Client *fc = server.focused_client;
	struct BGCEMessage msg;
	struct InputEvent *e;

	if (!fc || fc->fd < 0)
		return;

	memset(&msg, 0, sizeof(msg));
	msg.type = MSG_INPUT_EVENT;
	e = &msg.data.input_event;
	e->type = EV_KEY;
	e->code = code;
	e->value = value;
	if (server.input.count > 0)
		e->device = server.input.devs[0];
	bgce_send_msg(fc->fd, &msg);
}

void deliver_interrupt_to_focus(void)
{
	struct Client *fc = server.focused_client;
	int inject_ctrl;

	if (!fc || fc->fd < 0) {
		printf("[BGCE] SIGINT/Ctrl+C: no focused client — ignored "
		       "(use exit shortcut to quit the server)\n");
		return;
	}

	printf("[BGCE] SIGINT/Ctrl+C: forwarding to focused client fd=%d\n", fc->fd);

	/*
	 * Clients see normal EV_KEY traffic (not Unix signals). Synthesize the
	 * usual Ctrl+C chord. If Ctrl is already held from the keyboard, only
	 * send C press/release.
	 */
	inject_ctrl = !ctrl_down;
	if (inject_ctrl)
		send_key_to_focused(KEY_LEFTCTRL, 1);
	send_key_to_focused(KEY_C, 1);
	send_key_to_focused(KEY_C, 0);
	if (inject_ctrl)
		send_key_to_focused(KEY_LEFTCTRL, 0);
}

static void poll_sigint(void)
{
	if (!bgce_sigint_pending)
		return;
	bgce_sigint_pending = 0;
	deliver_interrupt_to_focus();
}

void* input_loop(void* arg) {
	(void)arg;

	if (count == 0) {
		printf("[BGCE] Input: no devices, input thread parked\n");
		while (1) {
			poll_sigint();
			sleep(1);
		}
		return NULL;
	}

	printf("[BGCE] Input: polling %zu device(s)\n", count);

	while (1) {
		/* Finite timeout so SIGINT (set from another thread/handler) is noticed. */
		int ret = poll(fds, count, 200);
		poll_sigint();
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[BGCE] Input: poll error: %s\n", strerror(errno));
			break;
		}
		if (ret == 0)
			continue;

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

			if (handle_input_event(ev, (int)i))
				continue;

			if (!server.focused_client)
				continue;
			if (ev.type == EV_SYN)
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
				float wx, wy;
				uint32_t ww = c.world_w ? c.world_w : c.width;
				uint32_t wh = c.world_h ? c.world_h : c.height;
				screen_to_world(&server, (float)mouse_x, (float)mouse_y,
				                &wx, &wy);
				int in = wx >= (float)c.x &&
				         wx < (float)(c.x + ww) &&
				         wy >= (float)c.y &&
				         wy < (float)(c.y + wh);
				if (!in)
					continue;
				/* Report buffer-local coords so clients match their pixels. */
				if (ww > 0 && wh > 0) {
					e.x = (int32_t)((wx - (float)c.x) *
					                (float)c.width / (float)ww);
					e.y = (int32_t)((wy - (float)c.y) *
					                (float)c.height / (float)wh);
				} else {
					e.x = (int32_t)(wx - (float)c.x);
					e.y = (int32_t)(wy - (float)c.y);
				}
				break;
			}
			default:
				continue;
			}

			struct BGCEMessage msg;
			msg.type = MSG_INPUT_EVENT;
			msg.data.input_event = e;
			bgce_send_msg(c.fd, &msg);
		}
	}
	return NULL;
}
