#define _XOPEN_SOURCE 700

#include "bgce_shared.h"
#include "bgce_client_handler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <time.h>

/* Externs from bgce_server.c */
extern struct ServerState server;
extern int focused_client_fd;

void *client_thread_main(void *arg) {
    int client_fd = *(int *)arg;
    free(arg);

    Client client = {0};
    client.fd = client_fd;

    printf("[BGCE] Thread started for client fd=%d\n", client_fd);

    while (1) {
        BGCEMessage msg;
        ssize_t rc = bgce_recv_msg(client_fd, &msg);
        if (rc <= 0) {
            printf("[BGCE] Client disconnected (fd=%d)\n", client_fd);
            break;
        }

        switch (msg.type) {
        case MSG_GET_SERVER_INFO: {
            ServerInfo info = {
                .width = server.width,
                .height = server.height,
                .color_depth = server.color_depth
            };
            bgce_send_data(client_fd, &info, sizeof(info));
            break;
        }

        case MSG_GET_BUFFER: {
            if (msg.length < sizeof(ClientBufferRequest)) {
                fprintf(stderr, "[BGCE] Invalid buffer request size\n");
                break;
            }

            ClientBufferRequest req;
            memcpy(&req, msg.data, sizeof(req));

            snprintf(client.shm_name, sizeof(client.shm_name),
                     "/bgce_buf_%d_%ld", getpid(), time(NULL));

            int shm_fd = shm_open(client.shm_name, O_CREAT | O_RDWR, 0600);
            if (shm_fd < 0) {
                perror("shm_open");
                break;
            }

            size_t buf_size = req.width * req.height * 3;
            if (ftruncate(shm_fd, buf_size) < 0) {
                perror("ftruncate");
                close(shm_fd);
                break;
            }

            client.buffer = mmap(NULL, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            client.width = req.width;
            client.height = req.height;
            close(shm_fd);

            ClientBufferReply reply = {0};
            strncpy(reply.shm_name, client.shm_name, sizeof(reply.shm_name));
            reply.width = req.width;
            reply.height = req.height;
            bgce_send_data(client_fd, &reply, sizeof(reply));
            break;
        }

        case MSG_DRAW: {
            if (client_fd == focused_client_fd) {
                if (!client.buffer) {
                    fprintf(stderr, "[BGCE] Client has no buffer!\n");
                    break;
                }
                printf("[BGCE] Drawing from focused client %d\n", client_fd);
                bgce_blit_to_framebuffer(&server, &client);
            } else {
                printf("[BGCE] Ignoring draw from unfocused client %d\n", client_fd);
            }
            break;
        }

        case MSG_SET_FOCUS: {
            /* Any client can request focus, in this simple model */
            focused_client_fd = client_fd;
            printf("[BGCE] Focus set to client %d\n", client_fd);
            break;
        }

        default:
            fprintf(stderr, "[BGCE] Unknown message type %d\n", msg.type);
            break;
        }
    }

    if (client.buffer) {
        munmap(client.buffer, client.width * client.height * 3);
        shm_unlink(client.shm_name);
    }
    close(client_fd);

    printf("[BGCE] Thread exiting for client fd=%d\n", client_fd);
    return NULL;
}

