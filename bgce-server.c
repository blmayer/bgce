// bgce-server.c
// Minimal single-client BGCE prototype.
// Builds: gcc -std=c11 -O2 bgce-server.c -o bgce-server -lrt
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <time.h>

#define SOCK_PATH "/tmp/bgce.sock"
#define SHM_NAME_FMT "/bgce_client_%d"
#define SHM_NAME_MAX 256

enum {
    MSG_INFO = 1,
    MSG_DRAW = 2,
    MSG_EVENT = 3,
    MSG_RESIZE = 4,
    MSG_EXIT = 5
};

struct __attribute__((packed)) server_info {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    char shm_name[SHM_NAME_MAX];
};

static int send_message(int fd, uint32_t type, const void *payload, uint32_t len) {
    uint32_t hdr[2] = {type, len};
    if (write(fd, hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (len > 0 && write(fd, payload, len) != (ssize_t)len) return -1;
    return 0;
}

static int recv_message(int fd, uint32_t *type, void **payload, uint32_t *len) {
    uint32_t hdr[2];
    ssize_t r = read(fd, hdr, sizeof(hdr));
    if (r == 0) return 0; // peer closed
    if (r < 0) return -1;
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

static void write_ppm(const char *path, uint32_t *pixels, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror("fopen ppm"); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    // pixels are ARGB; output RGB
    for (uint32_t i = 0; i < w*h; ++i) {
        uint32_t p = pixels[i];
        uint8_t a = (p >> 24) & 0xFF;
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = p & 0xFF;
        (void)a; // alpha ignored in this simple prototype
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    uint32_t screen_w = 800, screen_h = 600;
    if (argc >= 3) {
        screen_w = (uint32_t)atoi(argv[1]);
        screen_h = (uint32_t)atoi(argv[2]);
    }
    printf("BGCE server: screen %ux%u\n", screen_w, screen_h);

    // Create listening socket
    unlink(SOCK_PATH);
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 1) < 0) { perror("listen"); return 1; }

    // Server-side "real" screen buffer
    uint32_t *screen = calloc(screen_w * screen_h, sizeof(uint32_t));
    if (!screen) { perror("calloc"); return 1; }
    // Fill with dark gray
    for (uint32_t i = 0; i < screen_w * screen_h; ++i) screen[i] = 0xFF202020;

    printf("Waiting for client to connect on %s ...\n", SOCK_PATH);
    int client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0) { perror("accept"); return 1; }
    printf("Client connected\n");

    // Create per-client shared memory
    char shm_name[SHM_NAME_MAX];
    snprintf(shm_name, sizeof(shm_name), SHM_NAME_FMT, getpid());
    int pixels_count = screen_w * screen_h; // give client same dims for now
    size_t shm_size = pixels_count * sizeof(uint32_t);
    int shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(shm_fd, shm_size) < 0) { perror("ftruncate"); return 1; }
    uint32_t *client_buf = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (client_buf == MAP_FAILED) { perror("mmap"); return 1; }
    // initialize client buffer transparent (0)
    memset(client_buf, 0, shm_size);

    // Send server info to client
    struct server_info sinf;
    sinf.width = screen_w;
    sinf.height = screen_h;
    sinf.depth = 32;
    strncpy(sinf.shm_name, shm_name, sizeof(sinf.shm_name)-1);
    if (send_message(client_fd, MSG_INFO, &sinf, sizeof(sinf)) < 0) { perror("send info"); return 1; }
    printf("Sent server info and shm name %s\n", shm_name);

    // Main loop: wait for client messages and also read stdin for simple events.
    fd_set rfds;
    int maxfd = client_fd > STDIN_FILENO ? client_fd : STDIN_FILENO;
    char linebuf[256];

    printf("Server ready. Type lines into server stdin to send simple input events to client.\n");
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(client_fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int rc = select(maxfd+1, &rfds, NULL, NULL, NULL);
        if (rc < 0) { if (errno==EINTR) continue; perror("select"); break; }

        if (FD_ISSET(client_fd, &rfds)) {
            uint32_t type; void *payload; uint32_t len;
            int rr = recv_message(client_fd, &type, &payload, &len);
            if (rr <= 0) { printf("Client disconnected\n"); break; }
            if (type == MSG_DRAW) {
                if (len >= sizeof(int32_t)*3 + sizeof(uint32_t)*2) {
                    int32_t x = ((int32_t*)payload)[0];
                    int32_t y = ((int32_t*)payload)[1];
                    int32_t z = ((int32_t*)payload)[2];
                    uint32_t cw = ((uint32_t*)payload)[3];
                    uint32_t ch = ((uint32_t*)payload)[4];
                    printf("Received DRAW from client: x=%d y=%d z=%d w=%u h=%u\n", x,y,z,cw,ch);
                    // For simplicity copy entire client buffer into screen at x,y (no alpha)
                    for (uint32_t cy = 0; cy < ch; ++cy) {
                        int32_t sy = y + (int32_t)cy;
                        if (sy < 0 || sy >= (int32_t)screen_h) continue;
                        for (uint32_t cx = 0; cx < cw; ++cx) {
                            int32_t sx = x + (int32_t)cx;
                            if (sx < 0 || sx >= (int32_t)screen_w) continue;
                            uint32_t pix = client_buf[cy * cw + cx];
                            screen[sy * screen_w + sx] = pix;
                        }
                    }
                    // write out to ppm
                    write_ppm("/tmp/bgce_frame.ppm", screen, screen_w, screen_h);
                    printf("Wrote /tmp/bgce_frame.ppm\n");
                } else {
                    printf("Malformed DRAW payload\n");
                }
            } else if (type == MSG_RESIZE) {
                printf("Resize not implemented in prototype\n");
            } else if (type == MSG_EXIT) {
                printf("Client requested exit\n");
                free(payload);
                break;
            }
            if (payload) free(payload);
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (!fgets(linebuf, sizeof(linebuf), stdin)) {
                // EOF
                printf("stdin EOF\n");
                break;
            }
            // send event to client (text)
            size_t l = strlen(linebuf);
            if (l && linebuf[l-1] == '\n') linebuf[l-1] = 0, --l;
            if (send_message(client_fd, MSG_EVENT, linebuf, (uint32_t)l) < 0) {
                perror("send event");
                break;
            }
            printf("Sent event to client: '%s'\n", linebuf);
        }
    }

    // cleanup
    close(client_fd);
    close(listen_fd);
    munmap(client_buf, shm_size);
    shm_unlink(shm_name);
    free(screen);
    unlink(SOCK_PATH);
    return 0;
}
