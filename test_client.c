#include "bgce.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

	struct ClientBufferRequest req = { .width = w, .height = h };
	uint8_t *buf = bgce_get_buffer(conn, req);
	if (!buf) {
		fprintf(stderr, "[BGCE] Failed to get buffer\n");
		return 3;
	}

	printf("[BGCE] Drawing gradient...\n");
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int idx = (y * w + x) * 3;
			buf[idx + 0] = (x * 255) / w;   /* Red */
			buf[idx + 1] = (y * 255) / h;   /* Green */
			buf[idx + 2] = 64;              /* Blue constant */
		}
	}

	if (bgce_draw(conn) < 0) {
		fprintf(stderr, "[BGCE] Draw failed\n");
		return 4;
	}

	printf("[BGCE] Frame drawn. Check /tmp/bgce_frame.ppm\n");
	bgce_close(conn);

	return 0;
}

