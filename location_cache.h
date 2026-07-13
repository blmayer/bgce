#ifndef BGCE_LOCATION_CACHE_H
#define BGCE_LOCATION_CACHE_H

#include <stdint.h>

struct Client;
struct ServerState;

/** Load cache from disk into memory (call once at server start). */
void location_cache_load(void);

/** Write memory cache to disk. */
void location_cache_save(void);

/**
 * Look up last world position for app_id.
 * Returns 1 if found (writes *x, *y), 0 if unknown.
 */
int location_cache_lookup(const char *app_id, uint32_t *x, uint32_t *y);

/**
 * Look up last viewport (zoom + pan) for app_id.
 * Returns 1 if a viewport was stored, 0 if unknown / legacy entry.
 */
int location_cache_lookup_viewport(const char *app_id, int *zoom_pct,
                                   int *pan_x, int *pan_y);

/**
 * Remember world position and viewport for app_id (memory only until save).
 * zoom_pct <= 0 means "no viewport" (legacy / position-only entry).
 */
void location_cache_store(const char *app_id, uint32_t x, uint32_t y,
                          int zoom_pct, int pan_x, int pan_y);

/**
 * Fill client->app_id from the peer process (SO_PEERCRED + /proc/pid/comm
 * on Linux).  Falls back to "client" if unavailable.
 */
void location_cache_identify_client(struct Client *client, int sock_fd);

/**
 * Store this client's x,y and the current server viewport under its app_id
 * and flush to disk.  Pure pan of the desktop does not call this.
 */
void location_cache_remember_client(const struct Client *client);

/**
 * Alt+Tab / Alt+Shift+Tab: remember the current focus viewport, cycle to the
 * next (or previous) client, raise it, and restore its cached zoom/pan so the
 * window appears on screen in the same place it was left.
 * reverse=0 → Alt+Tab, reverse=1 → Alt+Shift+Tab.
 */
void bgce_cycle_focus(struct ServerState *srv, int reverse);

#endif
