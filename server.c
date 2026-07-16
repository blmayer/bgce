#include "server.h"
#include "bgce.h"
#include "compositor.h"
#include "location_cache.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct ServerState server = {}; /* Global server state */
struct config config = {};            /* Global config (background + shortcuts) */

static void ensure_dir(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		return;
	/* Try to create; ignore errors (parent may not exist, we'll fail open later) */
	mkdir(path, 0755);
}

static char listen_sock_path[BGCE_SOCKPATH_MAX];
static char log_file_path[512];
/* Original stderr before log redirect — so permission errors are not silent. */
static int console_fd = -1;

void bgce_announce(const char *fmt, ...)
{
	va_list ap;
	char buf[1024];
	int n;

	if (!fmt)
		return;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n >= (int)sizeof(buf))
		n = (int)sizeof(buf) - 1;

	/* Log file (stdout/stderr may already be redirected). */
	fwrite(buf, 1, (size_t)n, stderr);
	fflush(stderr);

	/* Original terminal, if we still have it. */
	if (console_fd >= 0)
		(void)write(console_fd, buf, (size_t)n);
}

/* Read lines from a pipe and append them to the log with timestamps. */
static void *log_timestamp_thread(void *arg)
{
	int rfd = (int)(intptr_t)arg;
	int out_fd;
	FILE *in;
	char line[8192];

	out_fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (out_fd < 0) {
		close(rfd);
		return NULL;
	}

	in = fdopen(rfd, "r");
	if (!in) {
		close(rfd);
		close(out_fd);
		return NULL;
	}

	/* Line-buffered so multi-thread printf interleaving is less chaotic */
	while (fgets(line, sizeof(line), in)) {
		struct timespec ts;
		struct tm tm;
		char stamp[40];

		if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
			ts.tv_sec = time(NULL), ts.tv_nsec = 0;
		if (!localtime_r(&ts.tv_sec, &tm)) {
			dprintf(out_fd, "%s", line);
			continue;
		}
		/* ISO-like local time with milliseconds */
		strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
		dprintf(out_fd, "%s.%03ld %s", stamp, ts.tv_nsec / 1000000L, line);
	}

	fclose(in); /* closes rfd */
	close(out_fd);
	return NULL;
}

static void cleanup_and_exit(int sig) {
	(void)sig;
	bgce_comp_shutdown();
	if (listen_sock_path[0])
		unlink(listen_sock_path);
	if (server.server_fd >= 0) {
		close(server.server_fd);
		server.server_fd = -1;
	}
	release_display();
	_exit(0);
}

void bgce_request_shutdown(void)
{
	/* Prefer the real console — avoid printf/stderr (log pipe can block). */
	if (console_fd >= 0)
		dprintf(console_fd, "[BGCE] Shortcut: exit — shutting down\n");
	cleanup_and_exit(SIGTERM);
}

static void setup_log_file(void) {
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	char dir[512];
	int pipefd[2];
	pthread_t tid;
	pthread_attr_t attr;

	if (xdg && xdg[0]) {
		snprintf(dir, sizeof(dir), "%s/bgce", xdg);
		ensure_dir(xdg);
		ensure_dir(dir);
	} else if (home && home[0]) {
		char dotcache[512];
		snprintf(dotcache, sizeof(dotcache), "%s/.cache", home);
		snprintf(dir, sizeof(dir), "%s/.cache/bgce", home);
		ensure_dir(dotcache);
		ensure_dir(dir);
	} else {
		return;
	}

	snprintf(log_file_path, sizeof(log_file_path), "%s/bgce.log", dir);

	/* Keep the real terminal for fatal messages after redirect. */
	if (console_fd < 0)
		console_fd = dup(STDERR_FILENO);

	/* Tell the user (on their original terminal) where logs are going */
	if (console_fd >= 0)
		dprintf(console_fd, "[BGCE] Logging to %s\n", log_file_path);
	else
		dprintf(STDERR_FILENO, "[BGCE] Logging to %s\n", log_file_path);

	if (pipe(pipefd) < 0) {
		/* Fall back to untimestamped direct log */
		int fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		return;
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &attr, log_timestamp_thread,
	                   (void *)(intptr_t)pipefd[0]) != 0) {
		pthread_attr_destroy(&attr);
		close(pipefd[0]);
		close(pipefd[1]);
		int fd = open(log_file_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		return;
	}
	pthread_attr_destroy(&attr);

	/* All server printf/fprintf go through the pipe → timestamped log */
	dup2(pipefd[1], STDOUT_FILENO);
	dup2(pipefd[1], STDERR_FILENO);
	close(pipefd[1]);
	/* pipefd[0] is owned by the log thread */
}

static void print_startup_banner(void)
{
	/* Matches the project site / README mark — first thing in the log. */
	static const char *const logo[] = {
		"┌────────────────────────────────────┐",
		"│ BGCE.                              │",
		"│     |\\                             │",
		"│      ^                             │",
		"│                   ┌───────┐        │",
		"│      ┌───────┐    │       │        │",
		"│      │       │    │       │        │",
		"│      │       │    │       │        │",
		"│      │       │    │       │        │",
		"│      └───────┘    └───────┘        │",
		"│                                    │",
		"└────────────────────────────────────┘",
		"Brian's Graphical Computer Environment",
		NULL,
	};
	int i;

	puts("");
	for (i = 0; logo[i]; i++)
		puts(logo[i]);
	puts("");
}

int main(void) {
	setup_log_file();

	/* Line-buffered so each log line is timestamped as a unit */
	setvbuf(stdout, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IOLBF, 0);

	print_startup_banner();

	/* Auto-reap child processes so command shortcuts don't leave zombies */
	signal(SIGCHLD, SIG_IGN);

	/* A crashed client must not take down the server on the next write(). */
	signal(SIGPIPE, SIG_IGN);

	/* Ctrl+C on the tty must not kill the compositor (trap & ignore). */
	/* Tty Ctrl+C must not kill the compositor; apps get Ctrl+C via /dev/input. */
	signal(SIGINT, SIG_IGN);
	/* Real shutdown: kill/SIGTERM or configured exit shortcut (e.g. ctrl+alt+q) */
	signal(SIGTERM, cleanup_and_exit);

	memset(&server, 0, sizeof(struct ServerState));
	server.display_fd = -1;
	server.server_fd = -1;
	server.framebuffer = NULL;
	server.display_pitch = 0;
	server.fb_size = 0;
	server.client_count = 0;
	listen_sock_path[0] = '\0';

	char* home = getenv("HOME");
	if (home) {
		if (load_config(&config) != 0)
			fprintf(stderr, "[BGCE] No config at ~/.config/bgce.conf; using defaults\n");
	} else {
		fprintf(stderr, "[BGCE] HOME unset; using default config\n");
	}
	print_config(&config);
	location_cache_load();

	if (!bgce_socket_path(listen_sock_path, sizeof(listen_sock_path))) {
		fprintf(stderr, "[BGCE] socket path too long\n");
		return 1;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("[BGCE] socket");
		return 1;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, listen_sock_path, strlen(listen_sock_path) + 1);

	if (unlink(listen_sock_path) < 0 && errno != ENOENT) {
		fprintf(stderr, "[BGCE] unlink %s: %s\n"
		        "  (stale socket from another user? remove it as that user/root)\n",
		        listen_sock_path, strerror(errno));
		close(fd);
		return 1;
	}

	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[BGCE] bind %s: %s\n", listen_sock_path, strerror(errno));
		if (errno == EADDRINUSE)
			fprintf(stderr, "  another bgce is already running for this user "
			        "(killall bgce && rm -f %s)\n", listen_sock_path);
		close(fd);
		return 1;
	}
	chmod(listen_sock_path, 0600);

	if (listen(fd, 8) < 0) {
		perror("[BGCE] listen");
		unlink(listen_sock_path);
		close(fd);
		return 1;
	}

	server.server_fd = fd;

	if (init_display() != 0) {
		bgce_announce("[BGCE] display init failed — cannot continue\n");
		if (log_file_path[0])
			bgce_announce("[BGCE] details are also in %s\n", log_file_path);
		release_display();
		return 1;
	}
	printf("[BGCE] Display initialised\n");

	/* Virtual desktop: WORLD_SCALE × physical display in each axis (4× area).
	 * Zoom 100% = 1:1; range [50%, 400%] so the full canvas fits at min. */
	server.virtual_w = server.display_w * BGCE_WORLD_SCALE;
	server.virtual_h = server.display_h * BGCE_WORLD_SCALE;
	server.zoom_pct = BGCE_ZOOM_PCT_1X;
	server.pan_x = 0;
	server.pan_y = 0;
	printf("[BGCE] Virtual desktop %ux%u (%.0fx physical area), zoom=%d%%\n",
	       server.virtual_w, server.virtual_h,
	       (double)(BGCE_WORLD_SCALE * BGCE_WORLD_SCALE), server.zoom_pct);

	/* Wallpaper is procedural (color or small source image) — no world-sized buffer. */
	server.clients = NULL;
	if (wallpaper_load(&config) != 0) {
		fprintf(stderr, "[BGCE] wallpaper_load failed\n");
		release_display();
		return 1;
	}

	puts("[BGCE] Drawing wallpaper");
	/* Async compositor owns FB writes after this. */
	if (bgce_comp_init(0) != 0)
		fprintf(stderr, "[BGCE] compositor init failed; using sync fallback\n");
	/* Full recomposite so the wallpaper fills the screen even if draw()
	 * bounds are edge-clamped oddly on the first paint. */
	bgce_comp_submit_full();
	bgce_comp_submit_cursor((int)server.display_w / 2,
	                        (int)server.display_h / 2);
	bgce_comp_flush();

	if (init_input() != 0) {
		perror("[BGCE] Failed to start input thread");
		return 4;
	}
	pthread_t input_thread;

	int rc = pthread_create(&input_thread, NULL, input_loop, NULL);
	if (rc != 0) {
		errno = rc;
		perror("[BGCE] Failed to start input thread");
		return 5;
	}
	pthread_detach(input_thread);

	printf("[BGCE] Server listening on %s\n", listen_sock_path);

	while (1) {
		int client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0) {
			perror("[BGCE] accept");
			continue;
		}

		printf("[BGCE] Client connected (fd=%d)\n", client_fd);

		pthread_t tid;
		int* arg = malloc(sizeof(int));
		if (!arg) {
			perror("[BGCE] malloc");
			close(client_fd);
			continue;
		}

		*arg = client_fd;
		if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
			perror("[BGCE] pthread_create client thread");
			free(arg);
			close(client_fd);
			continue;
		}

		pthread_detach(tid);
	}

	release_display();
	return 0;
}
