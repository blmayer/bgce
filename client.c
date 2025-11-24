#include "bgce.h"

#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(void) {
	int conn = bgce_connect();
	if (conn < 0) {
		fprintf(stderr, "[BGCE] Failed to connect\n");
		return 1;
	}

	struct ServerInfo info;
	if (bgce_get_server_info(conn, &info) < 0) {
		fprintf(stderr, "[BGCE] Failed to get server info\n");
		return 2;
	}

	printf("[BGCE] Server info: %dx%d, %d-bit color\n",
	       info.width, info.height, info.color_depth);

	printf("Available devices:\n");
	for (int i = 0; i < info.input_device_count; i++) {
		printf("  [%d] %s (mask=%x)\n",
		       info.devices[i].id,
		       info.devices[i].name,
		       info.devices[i].type_mask);
	}
	int w = 800;
	int h = 600;

	struct BufferRequest req = {.width = w, .height = h};
	uint8_t* buf = bgce_get_buffer(conn, req);
	if (!buf) {
		fprintf(stderr, "[BGCE] Failed to get buffer\n");
		return 3;
	}
	printf("Client got buffer at: %p\n", buf);

	printf("[BGCE] Drawing gradient...\n");
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t* p = (uint32_t*)buf + (y * w + x);

			uint8_t r = (x * 255) / w;
			uint8_t g = (y * 255) / h;
			uint8_t b = 128;

			*p = 0xFF000000 | (r << 16) | (g << 8) | b; // ARGB
		}
	}

	if (bgce_draw(conn) < 0) {
		fprintf(stderr, "[BGCE] Draw failed\n");
		return 4;
	}

	printf("[BGCE] Frame drawn. Check /tmp/bgce_frame.ppm\n");

	time_t start_time = time(NULL);
	while (1) {
		struct BGCEMessage msg;
		ssize_t rc = bgce_recv_msg(conn, &msg);
		if (rc <= 0) {
			printf("[BGCE Client] Disconnected from server\n");
			break;
		}

		switch (msg.type) {
		case MSG_INPUT_EVENT: {
			struct input_event ev;
			memcpy(&ev, msg.data, sizeof(ev));

			/* Example: Print keyboard/mouse input */
			printf("[BGCE Client] Input event: type=%hu code=%hu value=%d\n",
			       ev.type, ev.code, ev.value);
			break;
		}

		default:
			printf("[BGCE Client] Unknown message type %d\n", msg.type);
			break;
		}

		// Check if 10 seconds have passed
		if (time(NULL) - start_time >= 10) {
			printf("[BGCE Client] Timeout reached, exiting...\n");
			break;
		}
	}

	bgce_disconnect(conn);

	return 0;
}
