/*
 * Persist last window positions and viewports per app id under the user
 * cache dir:
 *   $XDG_CACHE_HOME/bgce/windows.cache
 *   or ~/.cache/bgce/windows.cache
 *
 * File format (one entry per line):
 *   app_id x y [zoom_pct pan_x pan_y]
 *
 * x,y are world pixels.  Optional zoom/pan are the viewport that was active
 * when the app was last left (Alt+Tab target restore, move end, disconnect).
 * Pure desktop pan does not write this file.
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
	int zoom_pct; /* 0 = no viewport stored (legacy line) */
	int pan_x;
	int pan_y;
	int used;
};

static struct loc_entry entries[LOC_CACHE_MAX];
static int entries_loaded;

extern struct ServerState server;

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
		int zoom, pan_x, pan_y;
		int nfields;

		if (line[0] == '#' || line[0] == '\n')
			continue;
		nfields = sscanf(line, "%63s %u %u %d %d %d", id, &x, &y, &zoom,
		                 &pan_x, &pan_y);
		if (nfields != 3 && nfields != 6)
			continue;
		strncpy(entries[i].id, id, sizeof(entries[i].id) - 1);
		entries[i].id[sizeof(entries[i].id) - 1] = '\0';
		entries[i].x = x;
		entries[i].y = y;
		if (nfields == 6 && zoom > 0) {
			entries[i].zoom_pct = zoom;
			entries[i].pan_x = pan_x;
			entries[i].pan_y = pan_y;
		} else {
			entries[i].zoom_pct = 0;
			entries[i].pan_x = 0;
			entries[i].pan_y = 0;
		}
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
	fprintf(f, "# BGCE window locations: app_id x y [zoom pan_x pan_y]\n");
	for (i = 0; i < LOC_CACHE_MAX; i++) {
		if (!entries[i].used)
			continue;
		if (entries[i].zoom_pct > 0)
			fprintf(f, "%s %u %u %d %d %d\n", entries[i].id,
			        entries[i].x, entries[i].y, entries[i].zoom_pct,
			        entries[i].pan_x, entries[i].pan_y);
		else
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

int location_cache_lookup_viewport(const char *app_id, int *zoom_pct,
                                   int *pan_x, int *pan_y)
{
	int i;

	if (!app_id || !app_id[0] || !entries_loaded)
		return 0;
	for (i = 0; i < LOC_CACHE_MAX; i++) {
		if (entries[i].used && strcmp(entries[i].id, app_id) == 0) {
			if (entries[i].zoom_pct <= 0)
				return 0;
			if (zoom_pct)
				*zoom_pct = entries[i].zoom_pct;
			if (pan_x)
				*pan_x = entries[i].pan_x;
			if (pan_y)
				*pan_y = entries[i].pan_y;
			return 1;
		}
	}
	return 0;
}

void location_cache_store(const char *app_id, uint32_t x, uint32_t y,
                          int zoom_pct, int pan_x, int pan_y)
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
			if (zoom_pct > 0) {
				entries[i].zoom_pct = zoom_pct;
				entries[i].pan_x = pan_x;
				entries[i].pan_y = pan_y;
			}
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
	if (zoom_pct > 0) {
		entries[free_slot].zoom_pct = zoom_pct;
		entries[free_slot].pan_x = pan_x;
		entries[free_slot].pan_y = pan_y;
	} else {
		entries[free_slot].zoom_pct = 0;
		entries[free_slot].pan_x = 0;
		entries[free_slot].pan_y = 0;
	}
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
	int z;

	if (!client || !client->app_id[0])
		return;
	/* Skip placeholder background client */
	if (client->z == 0 && client->fd < 0)
		return;
	z = server.zoom_pct > 0 ? server.zoom_pct : BGCE_ZOOM_PCT_1X;
	location_cache_store(client->app_id, client->x, client->y, z,
	                     server.pan_x, server.pan_y);
	location_cache_save();
}

/*
 * Raise client to the head of the stacking list and set focus.
 * Does not send MSG_FOCUS_CHANGE (caller / input path may do that).
 */
static void raise_and_focus(struct ServerState *srv, struct Client *c)
{
	struct Client *prev;
	struct Client *curr;

	if (!srv || !c)
		return;

	if (c != srv->clients) {
		prev = NULL;
		curr = srv->clients;
		while (curr && curr != c) {
			prev = curr;
			curr = curr->next;
		}
		if (curr && prev) {
			prev->next = curr->next;
			curr->next = srv->clients;
			srv->clients = curr;
		}
	}
	if (srv->focused_client && srv->focused_client != c)
		c->z = srv->focused_client->z + 1;
	srv->focused_client = c;
}

void bgce_cycle_focus(struct ServerState *srv, int reverse)
{
	struct Client *list[LOC_CACHE_MAX];
	struct Client *c;
	struct Client *target;
	struct Client *old_focus;
	int n = 0;
	int idx = 0;
	int next;
	int zoom, pan_x, pan_y;
	int i;

	if (!srv)
		return;

	for (c = srv->clients; c && n < LOC_CACHE_MAX; c = c->next) {
		if (c->z > 0)
			list[n++] = c;
	}
	if (n == 0)
		return;

	old_focus = srv->focused_client;
	if (old_focus) {
		for (i = 0; i < n; i++) {
			if (list[i] == old_focus) {
				idx = i;
				break;
			}
		}
		/* Snapshot position + viewport of the window we leave. */
		location_cache_remember_client(old_focus);
	}

	if (n == 1)
		next = 0;
	else if (reverse)
		next = (idx - 1 + n) % n;
	else
		next = (idx + 1) % n;

	target = list[next];
	raise_and_focus(srv, target);

	if (location_cache_lookup_viewport(target->app_id, &zoom, &pan_x,
	                                   &pan_y)) {
		if (!bgce_zoom_set(srv, zoom))
			srv->zoom_pct = zoom; /* already at that zoom */
		srv->pan_x = pan_x;
		srv->pan_y = pan_y;
		clamp_viewport(srv);
		redraw_all(srv);
	} else {
		draw(srv, target);
	}

	printf("[BGCE] Alt+%sTab → '%s' zoom=%d%% pan=(%d,%d)\n",
	       reverse ? "Shift+" : "", target->app_id,
	       srv->zoom_pct, srv->pan_x, srv->pan_y);
}
