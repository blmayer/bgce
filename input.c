#include "bgce.h"   /* for access to global server state */
#include "server.h" /* for access to global server state */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_INPUT_DEVICES 8
#define test_bit(bit, array) ((array)[(bit) / 8] & (1 << ((bit) % 8)))
#define INPUT_DIR "/dev/input"

static struct {
	size_t count;
	struct pollfd fds[MAX_INPUT_DEVICES];
} input_state;

extern struct ServerState server;

int bgce_input_init(void) {
	input_state.count = 0;
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

	return 0;
}

void* bgce_input_thread(void* arg) {
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
		printf("[BGCE] poll() returned %d\n", ret);

		if (!server.focused_client) {
			printf("[BGCE] No focused client for input event!\n");
			continue;
		}

		for (size_t i = 0; i < input_state.count; i++) {
			if (input_state.fds[i].revents) {
				printf("[BGCE] fd[%zu]=%d revents=0x%0x\n", i, input_state.fds[i].fd, input_state.fds[i].revents);
				continue;
			}

			struct input_event ev;
			ssize_t n;

			while ((n = read(input_state.fds[i].fd, &ev, sizeof(ev))) == sizeof(ev)) {
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

				/* Send to focused client */
				struct BGCEMessage msg;
				msg.type = MSG_INPUT_EVENT;
				memcpy(msg.data, &ev, sizeof(ev));

				bgce_send_msg(server.focused_client->fd, &msg);
			}

			if (n < 0 && errno != EAGAIN)
				perror("[BGCE] read");
		}
	}
	return NULL;
}
