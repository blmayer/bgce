// bgce-client.c
#include "libbgce.h"
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

int main() {
    if (bgce_connect(NULL) < 0) {
        fprintf(stderr, "Failed to connect to BGCE server\n");
        return 1;
    }
    struct bgce_info i = bgce_get_info();
    printf("Got server info: %ux%u depth=%u\n", i.width, i.height, i.depth);
    uint32_t w, h;
    uint32_t *buf = bgce_get_buffer(&w, &h);
    if (!buf) { fprintf(stderr,"no buffer\n"); return 1; }

    // draw a simple gradient or rectangle into client buffer
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t r = (x * 255) / (w-1);
            uint8_t g = (y * 255) / (h-1);
            uint8_t b = 0x40;
            uint32_t argb = (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            buf[y*w + x] = argb;
        }
    }
    if (bgce_draw(50, 50, 0) < 0) {
        fprintf(stderr,"draw failed\n");
    } else {
        printf("Requested draw. Check /tmp/bgce_frame.ppm\n");
    }

    printf("Client sleeping 20s to receive server events (if any)\n");
    sleep(20);

    bgce_disconnect();
    return 0;
}
