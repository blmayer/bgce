#include "bgce.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>

/* Functions provided by libbgce.so */
int getServerInfo(ServerInfo *info);
void *getBuffer(int width, int height);
int draw(void);
void bgce_close(void);

int main(void)
{
	ServerInfo info;
	if (getServerInfo(&info) < 0) {
		fprintf(stderr, "[BGCE] Failed to get server info\n");
		return 1;
	}

	printf("[BGCE] Server info: %dx%d, %d-bit color\n",
		info.width, info.height, info.color_depth);

	int w = info.width;
	int h = info.height;

	uint8_t *buf = getBuffer(w, h);
	if (!buf) {
		fprintf(stderr, "[BGCE] Failed to get buffer\n");
		return 1;
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

	if (draw() < 0) {
		fprintf(stderr, "[BGCE] Draw failed\n");
		return 1;
	}

	printf("[BGCE] Frame drawn. Check /tmp/bgce_frame.ppm\n");
	bgce_close();
	return 0;
}

