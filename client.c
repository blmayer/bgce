#include "bgce.h"

#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int w = 800;
int h = 600;
uint8_t* buf = NULL;

void draw_gradient() {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t* p = (uint32_t*)buf + (y * w + x);

			uint8_t r = (x * 255) / w;
			uint8_t g = (y * 255) / h;
			uint8_t b = 128;

			*p = 0xFF000000 | (r << 16) | (g << 8) | b; // ARGB
		}
	}
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // Disable buffering for stdout
	setvbuf(stderr, NULL, _IONBF, 0); // Disable buffering for stderr

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

	struct BufferRequest req = {.width = w, .height = h};
	buf = bgce_get_buffer(conn, req);
	if (!buf) {
		fprintf(stderr, "[BGCE] Failed to get buffer\n");
		return 3;
	}
	printf("Client got buffer at: %p\n", buf);

	printf("[BGCE] Drawing gradient...\n");
	draw_gradient();

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
			struct InputEvent ev = msg.data.input_event;

			/* Example: Print keyboard/mouse input */
			printf("[BGCE Client] Input event: device=%s code=%hu value=%d\n",
			       ev.device.name, ev.code, ev.value);
			break;
		}
		case MSG_BUFFER_CHANGE: {
			struct BufferReply b = msg.data.buffer_reply;
			printf("[BGCE] Buffer change event: w=%u h=%u shm_name=%s\n", b.width, b.height, b.shm_name);

			w = b.width;
			h = b.height;

			printf("[BGCE] Drawing gradient...\n");
			draw_gradient();

			if (bgce_draw(conn) < 0) {
				fprintf(stderr, "[BGCE] Draw failed\n");
				return 4;
			}
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
