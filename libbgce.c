#include "bgce.h"
#include "bgce_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

static int bgce_fd = -1;
static int shm_fd = -1;
static void *shared_buf = NULL;

/* Connect to the BGCE server */
static int bgce_connect(void)
{
	if (bgce_fd >= 0) return bgce_fd;

	bgce_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bgce_fd < 0) {
		perror("socket");
		return -1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, BGCE_SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (connect(bgce_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(bgce_fd);
		bgce_fd = -1;
		return -1;
	}
	return bgce_fd;
}

/* Public API: Get server info */
int getServerInfo(ServerInfo *info)
{
	if (bgce_connect() < 0) return -1;

	BGCEMessage msg = {0};
	msg.type = MSG_GET_SERVER_INFO;
	msg.length = 0;

	if (bgce_send_msg(bgce_fd, &msg) <= 0)
		return -1;

	if (bgce_recv_data(bgce_fd, info, sizeof(ServerInfo)) <= 0)
		return -1;

	return 0;
}

/* Public API: Get shared buffer */
void *getBuffer(int width, int height)
{
	if (bgce_connect() < 0) return NULL;

	ClientBufferRequest req = { .width = width, .height = height };

	BGCEMessage msg = {0};
	msg.type = MSG_GET_BUFFER;
	msg.length = sizeof(req);
	memcpy(msg.data, &req, sizeof(req));

	if (bgce_send_msg(bgce_fd, &msg) <= 0)
		return NULL;

	ClientBufferReply reply;
	if (bgce_recv_data(bgce_fd, &reply, sizeof(reply)) <= 0)
		return NULL;

	size_t size = reply.width * reply.height * 3;
	shm_fd = shm_open(reply.shm_name, O_RDWR, 0600);
	if (shm_fd < 0) {
		perror("shm_open (client)");
		return NULL;
	}

	shared_buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	if (shared_buf == MAP_FAILED) {
		perror("mmap (client)");
		close(shm_fd);
		return NULL;
	}

	return shared_buf;
}

/* Public API: Draw current buffer */
int draw(void)
{
	if (bgce_connect() < 0) return -1;

	BGCEMessage msg = {0};
	msg.type = MSG_DRAW;
	msg.length = 0;

	if (bgce_send_msg(bgce_fd, &msg) <= 0)
		return -1;

	return 0;
}

/* Public API: Disconnect */
void bgce_close(void)
{
	if (shared_buf) {
		munmap(shared_buf, 0);
		shared_buf = NULL;
	}
	if (shm_fd >= 0) {
		close(shm_fd);
		shm_fd = -1;
	}
	if (bgce_fd >= 0) {
		close(bgce_fd);
		bgce_fd = -1;
	}
}

