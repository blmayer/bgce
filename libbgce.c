#include "bgce.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Write exactly 'size' bytes */
ssize_t bgce_send_msg(int conn, struct BGCEMessage* msg) {
	size_t size = sizeof(struct BGCEMessage);

	ssize_t n = write(conn, msg, size);
	if (n < 0) {
		if (errno == EINTR)
			perror("write");
		return -1;
	}
	return n;
}

ssize_t bgce_recv_msg(int conn, struct BGCEMessage* msg) {
	ssize_t n = read(conn, msg, sizeof(struct BGCEMessage));
	if (n < 0) {
		if (errno == EINTR)
			perror("read");
		return -1;
	}
	return n;
}

/* Connect to the BGCE server */
int bgce_connect(void) {
	int bgce_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bgce_fd < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (connect(bgce_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(bgce_fd);
		bgce_fd = -1;
		return -2;
	}
	return bgce_fd;
}

/* Public API: Get server info */
int bgce_get_server_info(int conn, struct ServerInfo* info) {
	if (conn < 0)
		return -1;

	struct BGCEMessage msg = {0};
	msg.type = MSG_GET_SERVER_INFO;

	if (bgce_send_msg(conn, &msg) <= 0)
		return -2;

	if (bgce_recv_msg(conn, &msg) <= 0)
		return -3;

	memcpy(info, msg.data, sizeof(struct ServerInfo));

	return 0;
}

/* Public API: Get shared buffer */
void* bgce_get_buffer(int conn, struct ClientBufferRequest req) {
	if (conn < 0)
		return NULL;

	struct BGCEMessage msg;
	msg.type = MSG_GET_BUFFER;
	memcpy(msg.data, &req, sizeof(req));

	int code = bgce_send_msg(conn, &msg);
	if (code <= 0)
		return NULL;

	if (bgce_recv_msg(conn, &msg) <= 0)
		return NULL;

	struct ClientBufferReply reply;
	memcpy(&reply, msg.data, sizeof(reply));

	size_t size = reply.width * reply.height * 3;
	int shm_fd = shm_open(reply.shm_name, O_RDWR, 0600);
	if (shm_fd < 0) {
		perror("shm_open (client)");
		return NULL;
	}

	void* buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (buf == MAP_FAILED) {
		perror("mmap (client)");
		close(shm_fd);
		return NULL;
	}

	return buf;
}

/* Public API: Draw current buffer */
int bgce_draw(int conn) {
	if (conn < 0)
		return -1;

	struct BGCEMessage msg = {0};
	msg.type = MSG_DRAW;

	if (bgce_send_msg(conn, &msg) <= 0)
		return -1;

	return 0;
}

/* Public API: Disconnect */
void bgce_close(int conn) {
	if (conn >= 0) {
		close(conn);
	}
}
