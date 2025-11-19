// libbgce.h
#ifndef LIBBGCE_H
#define LIBBGCE_H

#include <stdint.h>

struct bgce_info {
    uint32_t width, height, depth;
};

int bgce_connect();
void bgce_disconnect(int conn);
struct bgce_info bgce_get_info(void);
uint32_t * bgce_get_buffer(uint32_t *out_width, uint32_t *out_height);
int bgce_draw(int32_t x, int32_t y, int32_t z);

#endif
