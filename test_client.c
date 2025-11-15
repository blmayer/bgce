#include "bgce.h"

#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

	int w = info.width;
	int h = info.height;

	struct ClientBufferRequest req = {.width = w, .height = h};
	uint8_t* buf = bgce_get_buffer(conn, req);
	if (!buf) {
		fprintf(stderr, "[BGCE] Failed to get buffer\n");
		return 3;
	}

	printf("[BGCE] Drawing gradient...\n");
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint8_t* p = buf + (y * w + x) * 3;
			p[0] = (x * 255) / w;  // R
			p[1] = (y * 255) / h; // G
			p[2] = 128;                       // B
		}
	}

	if (bgce_draw(conn) < 0) {
		fprintf(stderr, "[BGCE] Draw failed\n");
		return 4;
	}

	printf("[BGCE] Frame drawn. Check /tmp/bgce_frame.ppm\n");

	//while (1) {
	//	struct BGCEMessage msg;
	//	ssize_t rc = bgce_recv_msg(conn, &msg);
	//	if (rc <= 0) {
	//		printf("[BGCE Client] Disconnected from server\n");
	//		break;
	//	}

	//	switch (msg.type) {
	//	case MSG_INPUT_EVENT: {
	//		struct input_event ev;
	//		memcpy(&ev, msg.data, sizeof(ev));

	//		/* Example: Print keyboard/mouse input */
	//		printf("[BGCE Client] Input event: type=%hu code=%hu value=%d\n",
	//		       ev.type, ev.code, ev.value);
	//		break;
	//	}

	//	default:
	//		printf("[BGCE Client] Unknown message type %d\n", msg.type);
	//		break;
	//	}
	//}

	bgce_close(conn);

	return 0;
}
