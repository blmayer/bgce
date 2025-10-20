// libbgce.c
#define _GNU_SOURCE
#include "libbgce.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>

#define SOCK_PATH "/tmp/bgce.sock"
#define SHM_NAME_MAX 256

enum {
    MSG_INFO = 1,
    MSG_DRAW = 2,
    MSG_EVENT = 3,
    MSG_RESIZE = 4,
    MSG_EXIT = 5
};

static int sockfd = -1;
static struct bgce_info ginfo;
static char g_shm_name[SHM_NAME_MAX];
static uint32_t *g_buf = NULL;
static size_t g_buf_size = 0;

static int send_message(int fd, uint32_t type, const void *payload, uint32_t len) {
    uint32_t hdr[2] = {type, len};
    if (write(fd, hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (len > 0 && write(fd, payload, len) != (ssize_t)len) return -1;
    return 0;
}

static int recv_message_once(int fd, uint32_t *type, void **payload, uint32_t *len) {
    uint32_t hdr[2];
    ssize_t r = read(fd, hdr, sizeof(hdr));
    if (r <= 0) return (int)r;
    if (r != sizeof(hdr)) return -1;
    *type = hdr[0];
    *len = hdr[1];
    if (*len > 0) {
        void *buf = malloc(*len);
        if (!buf) return -1;
        ssize_t got = 0;
        while (got < *len) {
            ssize_t s = read(fd, (char*)buf + got, *len - got);
            if (s <= 0) { free(buf); return -1; }
            got += s;
        }
        *payload = buf;
    } else {
        *payload = NULL;
    }
    return 1;
}


int bgce_connect(const char *sockpath) {
    (void)sockpath;
    if (sockfd != -1) return 0;
    sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr;
    memset(&addr,0,sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("connect"); close(sockfd); sockfd = -1; return -1; }

    // wait for MSG_INFO
    uint32_t type; void *payload; uint32_t len;
    if (recv_message_once(sockfd, &type, &payload, &len) <= 0) { perror("recv info"); close(sockfd); sockfd=-1; return -1; }
    if (type != MSG_INFO) { fprintf(stderr,"expected MSG_INFO\n"); free(payload); return -1; }
    if (len != sizeof(struct { uint32_t width,height,depth; char shm_name[SHM_NAME_MAX]; })) {
        // we'll parse assuming server_info size is known
    }
    // parse
    uint32_t *p = (uint32_t*)payload;
    ginfo.width = p[0];
    ginfo.height = p[1];
    ginfo.depth = p[2];
    // shm name starts at offset 12
    char *shm = (char*)payload + 12;
    strncpy(g_shm_name, shm, SHM_NAME_MAX-1);
    free(payload);

    // open shared memory and mmap
    int shmfd = shm_open(g_shm_name, O_RDWR, 0);
    if (shmfd < 0) { perror("shm_open client"); return -1; }
    g_buf_size = ginfo.width * ginfo.height * sizeof(uint32_t);
    g_buf = mmap(NULL, g_buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    if (g_buf == MAP_FAILED) { perror("mmap client"); close(shmfd); g_buf = NULL; return -1; }
    close(shmfd);
    return 0;
}

void bgce_disconnect(void) {
    if (g_buf) {
        munmap(g_buf, g_buf_size);
        g_buf = NULL;
    }
    if (sockfd != -1) {
        // send exit
        send_message(sockfd, MSG_EXIT, NULL, 0);
        close(sockfd); sockfd = -1;
    }
}

struct bgce_info bgce_get_info(void) {
    return ginfo;
}

uint32_t * bgce_get_buffer(uint32_t *out_width, uint32_t *out_height) {
    if (out_width) *out_width = ginfo.width;
    if (out_height) *out_height = ginfo.height;
    return g_buf;
}

int bgce_draw(int32_t x, int32_t y, int32_t z) {
    if (sockfd < 0) return -1;
    uint32_t payload[5];
    payload[0] = (uint32_t)x;
    payload[1] = (uint32_t)y;
    payload[2] = (uint32_t)z;
    payload[3] = ginfo.width;
    payload[4] = ginfo.height;
    return send_message(sockfd, MSG_DRAW, payload, sizeof(payload));
}
