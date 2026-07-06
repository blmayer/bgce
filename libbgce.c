#include "bgce.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

char *bgce_socket_path(char *buf, size_t buflen)
{
	const char *runtime;
	int n;

	if (!buf || buflen < 16)
		return NULL;

	runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime && runtime[0] == '/')
		n = snprintf(buf, buflen, "%s/bgce.sock", runtime);
	else
		n = snprintf(buf, buflen, "/tmp/bgce-%ld.sock", (long)getuid());

	if (n < 0 || (size_t)n >= buflen || (size_t)n >= BGCE_SOCKPATH_MAX)
		return NULL;
	return buf;
}

/* Shared buffers live under the user cache dir (not /dev/shm):
 *   $XDG_CACHE_HOME/bgce/buf/   or   $HOME/.cache/bgce/buf/
 */
static int bgce_mkdir_p(const char *path)
{
	struct stat st;

	if (!path || !path[0])
		return -1;
	if (stat(path, &st) == 0)
		return S_ISDIR(st.st_mode) ? 0 : -1;
	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int bgce_buf_dir(char *buf, size_t buflen)
{
	const char *xdg;
	const char *home;
	int n;

	if (!buf || buflen < 32)
		return -1;

	xdg = getenv("XDG_CACHE_HOME");
	if (xdg && xdg[0] == '/') {
		n = snprintf(buf, buflen, "%s/bgce/buf", xdg);
	} else {
		home = getenv("HOME");
		if (!home || !home[0])
			return -1;
		n = snprintf(buf, buflen, "%s/.cache/bgce/buf", home);
	}

	if (n < 0 || (size_t)n >= buflen)
		return -1;
	return 0;
}

static int bgce_buf_file_path(const char *token, char *path, size_t pathlen)
{
	char dir[512];

	if (!token || !token[0] || token[0] == '/' || strchr(token, '/'))
		return -1;
	if (bgce_buf_dir(dir, sizeof(dir)) < 0)
		return -1;
	if (snprintf(path, pathlen, "%s/%s", dir, token) < 0 ||
	    strlen(path) >= pathlen)
		return -1;
	return 0;
}

static int bgce_ensure_buf_dir(void)
{
	const char *xdg;
	const char *home;
	char path[512];

	xdg = getenv("XDG_CACHE_HOME");
	if (xdg && xdg[0] == '/') {
		if (snprintf(path, sizeof(path), "%s", xdg) >= (int)sizeof(path))
			return -1;
		if (bgce_mkdir_p(path) < 0)
			return -1;
		if (snprintf(path, sizeof(path), "%s/bgce", xdg) >= (int)sizeof(path))
			return -1;
		if (bgce_mkdir_p(path) < 0)
			return -1;
		if (snprintf(path, sizeof(path), "%s/bgce/buf", xdg) >= (int)sizeof(path))
			return -1;
		return bgce_mkdir_p(path);
	}

	home = getenv("HOME");
	if (!home || !home[0])
		return -1;
	if (snprintf(path, sizeof(path), "%s/.cache", home) >= (int)sizeof(path))
		return -1;
	if (bgce_mkdir_p(path) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "%s/.cache/bgce", home) >= (int)sizeof(path))
		return -1;
	if (bgce_mkdir_p(path) < 0)
		return -1;
	if (snprintf(path, sizeof(path), "%s/.cache/bgce/buf", home) >= (int)sizeof(path))
		return -1;
	return bgce_mkdir_p(path);
}

int bgce_buf_create(char *name, size_t namelen, size_t size)
{
	unsigned char rnd[4];
	int fd;
	int rfd;
	char path[512];

	if (!name || namelen < 16 || size == 0)
		return -1;

	if (bgce_ensure_buf_dir() < 0)
		return -1;

	rfd = open("/dev/urandom", O_RDONLY);
	if (rfd < 0)
		return -1;
	if (read(rfd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
		close(rfd);
		return -1;
	}
	close(rfd);

	if (snprintf(name, namelen, "bgce_%02x%02x%02x%02x",
	             rnd[0], rnd[1], rnd[2], rnd[3]) < 0 ||
	    (size_t)strlen(name) >= namelen)
		return -1;
	if (bgce_buf_file_path(name, path, sizeof(path)) < 0)
		return -1;

	fd = open(path, O_CREAT | O_RDWR | O_EXCL, 0600);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		int e = errno;
		close(fd);
		unlink(path);
		errno = e;
		return -1;
	}
	return fd;
}

int bgce_buf_open(const char *name)
{
	char path[512];

	if (!name || !name[0]) {
		errno = EINVAL;
		return -1;
	}
	if (bgce_buf_file_path(name, path, sizeof(path)) < 0) {
		errno = EINVAL;
		return -1;
	}
	return open(path, O_RDWR);
}

void bgce_buf_unlink(const char *name)
{
	char path[512];

	if (!name || !name[0])
		return;
	if (bgce_buf_file_path(name, path, sizeof(path)) == 0)
		unlink(path);
}

/* Write exactly 'size' bytes */
ssize_t bgce_send_msg(int conn, struct BGCEMessage* msg) {
	size_t size = sizeof(struct BGCEMessage);

	ssize_t n = write(conn, msg, size);
	if (n < 0) {
		/* EPIPE/ECONNRESET: peer gone — caller should drop the client.
		 * With SIGPIPE ignored on the server, this is a normal path. */
		if (errno != EPIPE && errno != ECONNRESET && errno != EINTR)
			perror("[BGCE] write");
		return -1;
	}
	return n;
}

ssize_t bgce_recv_msg(int conn, struct BGCEMessage* msg) {
	ssize_t n = read(conn, msg, sizeof(struct BGCEMessage));
	if (n < 0) {
		if (errno == EINTR)
			perror("[BGCE] read");
		return -1;
	}
	return n;
}

/* Connect to the BGCE server */
int bgce_connect(void) {
	char path[BGCE_SOCKPATH_MAX];
	int bgce_fd;
	struct sockaddr_un addr;

	if (!bgce_socket_path(path, sizeof(path))) {
		fprintf(stderr, "[BGCE] socket path too long\n");
		return -1;
	}

	bgce_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bgce_fd < 0) {
		perror("[BGCE] socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path, strlen(path) + 1);

	if (connect(bgce_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[BGCE] connect %s: %s\n", path, strerror(errno));
		close(bgce_fd);
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

	*info = msg.data.server_info;

	return 0;
}

/* Public API: Get shared buffer */
void* bgce_get_buffer(int conn, struct BufferRequest req) {
	if (conn < 0)
		return NULL;

	struct BGCEMessage msg = {0};
	msg.type = MSG_GET_BUFFER;
	msg.data.buffer_request = req;

	int code = bgce_send_msg(conn, &msg);
	if (code <= 0)
		return NULL;

	if (bgce_recv_msg(conn, &msg) <= 0)
		return NULL;

	struct BufferReply reply = msg.data.buffer_reply;
	if (reply.status != 0 || reply.width == 0 || reply.height == 0 ||
	    !reply.shm_name[0]) {
		fprintf(stderr, "[BGCE] buffer request failed (status=%d name='%s')\n",
		        reply.status, reply.shm_name);
		return NULL;
	}

	size_t size = (size_t)reply.width * reply.height * 4;
	int shm_fd = bgce_buf_open(reply.shm_name);
	if (shm_fd < 0) {
		perror("[BGCE] open buffer (client)");
		return NULL;
	}

	void* buf = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
	close(shm_fd);
	if (buf == MAP_FAILED) {
		perror("[BGCE] mmap (client)");
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

int bgce_move(int conn, int x, int y) {
	if (conn < 0)
		return -1;

	struct BGCEMessage msg = {0};
	msg.type = MSG_MOVE;
	msg.data.move_request.x = x;
	msg.data.move_request.y = y;

	if (bgce_send_msg(conn, &msg) <= 0)
		return -1;

	return 0;
}

/* Public API: Set cursor type */
int bgce_set_cursor(int conn, enum BGCECursorType type) {
	if (conn < 0)
		return -1;

	struct BGCEMessage msg = {0};
	msg.type = MSG_SET_CURSOR;
	msg.data.cursor_request.cursor_type = (int32_t)type;

	if (bgce_send_msg(conn, &msg) <= 0)
		return -1;

	return 0;
}

/* Public API: Disconnect */
void bgce_disconnect(int conn) {
	if (conn >= 0) {
		close(conn);
	}
}
