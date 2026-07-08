/*
 * Persist last window positions per app id under the user cache dir:
 *   $XDG_CACHE_HOME/bgce/windows.cache
 *   or ~/.cache/bgce/windows.cache
 *
 * File format (one entry per line):
 *   app_id x y
 */

#define _GNU_SOURCE
#include "location_cache.h"
#include "server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SO_PEERCRED
/* Non-Linux: identification falls back to "client". */
#endif

#define LOC_CACHE_MAX 64
#define LOC_ID_MAX 64

struct loc_entry {
	char id[LOC_ID_MAX];
	uint32_t x;
	uint32_t y;
	int used;
};

static struct loc_entry entries[LOC_CACHE_MAX];
static int entries_loaded;

static int cache_dir(char *buf, size_t buflen)
{
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	int n;

	if (xdg && xdg[0] == '/')
		n = snprintf(buf, buflen, "%s/bgce", xdg);
	else if (home && home[0])
		n = snprintf(buf, buflen, "%s/.cache/bgce", home);
	else
		return -1;
	if (n < 0 || (size_t)n >= buflen)
		return -1;
	return 0;
}

static int cache_path(char *buf, size_t buflen)
{
	char dir[512];

	if (cache_dir(dir, sizeof(dir)) < 0)
		return -1;
	if (snprintf(buf, buflen, "%s/windows.cache", dir) >= (int)buflen)
		return -1;
	return 0;
}

static void ensure_cache_dir(void)
{
	char dir[512];
	char parent[512];
	const char *home;
	const char *xdg;

	if (cache_dir(dir, sizeof(dir)) < 0)
		return;

	xdg = getenv("XDG_CACHE_HOME");
	home = getenv("HOME");
	if (xdg && xdg[0] == '/') {
		mkdir(xdg, 0700);
	} else if (home && home[0]) {
		snprintf(parent, sizeof(parent), "%s/.cache", home);
		mkdir(parent, 0700);
	}
	mkdir(dir, 0700);
}

void location_cache_load(void)
{
	char path[512];
	FILE *f;
	char line[256];
	int i;

	memset(entries, 0, sizeof(entries));
	entries_loaded = 1;

	if (cache_path(path, sizeof(path)) < 0)
		return;
	f = fopen(path, "r");
	if (!f)
		return;

	i = 0;
	while (fgets(line, sizeof(line), f) && i < LOC_CACHE_MAX) {
		char id[LOC_ID_MAX];
		unsigned int x, y;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		if (sscanf(line, "%63s %u %u", id, &x, &y) != 3)
			continue;
		strncpy(entries[i].id, id, sizeof(entries[i].id) - 1);
		entries[i].x = x;
		entries[i].y = y;
		entries[i].used = 1;
		i++;
	}
	fclose(f);
	printf("[BGCE] location cache: loaded %d entr%s from %s\n",
	       i, i == 1 ? "y" : "ies", path);
}

void location_cache_save(void)
{
	char path[512];
	FILE *f;
	int i, n = 0;

	if (!entries_loaded)
		return;
	ensure_cache_dir();
	if (cache_path(path, sizeof(path)) < 0)
		return;

	f = fopen(path, "w");
	if (!f) {
		perror("[BGCE] location cache write");
		return;
	}
	fprintf(f, "# BGCE window locations: app_id x y (world pixels)\n");
	for (i = 0; i < LOC_CACHE_MAX; i++) {
		if (!entries[i].used)
			continue;
		fprintf(f, "%s %u %u\n", entries[i].id, entries[i].x,
		        entries[i].y);
		n++;
	}
	fclose(f);
	printf("[BGCE] location cache: saved %d entr%s to %s\n",
	       n, n == 1 ? "y" : "ies", path);
}

int location_cache_lookup(const char *app_id, uint32_t *x, uint32_t *y)
{
	int i;

	if (!app_id || !app_id[0] || !entries_loaded)
		return 0;
	for (i = 0; i < LOC_CACHE_MAX; i++) {
		if (entries[i].used && strcmp(entries[i].id, app_id) == 0) {
			if (x)
				*x = entries[i].x;
			if (y)
				*y = entries[i].y;
			return 1;
		}
	}
	return 0;
}

void location_cache_store(const char *app_id, uint32_t x, uint32_t y)
{
	int i;
	int free_slot = -1;

	if (!app_id || !app_id[0])
		return;
	if (!entries_loaded)
		location_cache_load();

	for (i = 0; i < LOC_CACHE_MAX; i++) {
		if (entries[i].used && strcmp(entries[i].id, app_id) == 0) {
			entries[i].x = x;
			entries[i].y = y;
			return;
		}
		if (!entries[i].used && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		free_slot = 0; /* overwrite first if full */
	strncpy(entries[free_slot].id, app_id, sizeof(entries[free_slot].id) - 1);
	entries[free_slot].id[sizeof(entries[free_slot].id) - 1] = '\0';
	entries[free_slot].x = x;
	entries[free_slot].y = y;
	entries[free_slot].used = 1;
}

void location_cache_identify_client(struct Client *client, int sock_fd)
{
	if (!client)
		return;
	strncpy(client->app_id, "client", sizeof(client->app_id) - 1);
	client->app_id[sizeof(client->app_id) - 1] = '\0';

#ifdef SO_PEERCRED
	{
		struct ucred cred;
		socklen_t len = sizeof(cred);
		char path[64];
		char comm[LOC_ID_MAX];
		FILE *f;
		size_t n;

		memset(&cred, 0, sizeof(cred));
		if (getsockopt(sock_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
		    cred.pid > 0) {
			client->pid = cred.pid;
			snprintf(path, sizeof(path), "/proc/%d/comm", (int)cred.pid);
			f = fopen(path, "r");
			if (f) {
				if (fgets(comm, sizeof(comm), f)) {
					n = strlen(comm);
					while (n > 0 &&
					       (comm[n - 1] == '\n' ||
					        comm[n - 1] == '\r'))
						comm[--n] = '\0';
					if (comm[0]) {
						strncpy(client->app_id, comm,
						        sizeof(client->app_id) - 1);
						client->app_id[sizeof(client->app_id) - 1] =
						        '\0';
					}
				}
				fclose(f);
			}
		}
	}
#else
	(void)sock_fd;
#endif
	printf("[BGCE] client id='%s' pid=%d\n", client->app_id,
	       (int)client->pid);
}

void location_cache_remember_client(const struct Client *client)
{
	if (!client || !client->app_id[0])
		return;
	/* Skip placeholder background client */
	if (client->z == 0 && client->fd < 0)
		return;
	location_cache_store(client->app_id, client->x, client->y);
	location_cache_save();
}
