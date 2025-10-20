#ifndef BGCE_H
#define BGCE_H

#include <stdint.h>

#define BGCE_SOCKET_PATH "/tmp/bgce_socket"

/* Message types for the binary protocol */
typedef enum {
	MSG_GET_SERVER_INFO = 1,
	MSG_GET_BUFFER      = 2,
	MSG_DRAW            = 3
} MessageType;

/* Fixed-size message header */
typedef struct {
	uint32_t type;
	uint32_t length;
	uint8_t  data[256];   /* Inline payload (small messages only) */
} BGCEMessage;

/* Server global state */
typedef struct {
	int width;
	int height;
	int color_depth;
	uint8_t *framebuffer;
} ServerState;

/* Client state (used per connection) */
typedef struct {
	int fd;
	char shm_name[64];
	uint8_t *buffer;
	int width;
	int height;
} Client;

/* Structures for specific message payloads */
typedef struct {
	int width;
	int height;
} ClientBufferRequest;

typedef struct {
	char shm_name[64];
	int width;
	int height;
} ClientBufferReply;

typedef struct {
	int width;
	int height;
	int color_depth;
} ServerInfo;

#endif /* BGCE_H */

