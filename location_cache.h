#ifndef BGCE_LOCATION_CACHE_H
#define BGCE_LOCATION_CACHE_H

#include <stdint.h>

struct Client;

/** Load cache from disk into memory (call once at server start). */
void location_cache_load(void);

/** Write memory cache to disk. */
void location_cache_save(void);

/**
 * Look up last world position for app_id.
 * Returns 1 if found (writes *x, *y), 0 if unknown.
 */
int location_cache_lookup(const char *app_id, uint32_t *x, uint32_t *y);

/** Remember world position for app_id (memory only until save). */
void location_cache_store(const char *app_id, uint32_t x, uint32_t y);

/**
 * Fill client->app_id from the peer process (SO_PEERCRED + /proc/pid/comm
 * on Linux).  Falls back to "client" if unavailable.
 */
void location_cache_identify_client(struct Client *client, int sock_fd);

/** Store this client's current x,y under its app_id and flush to disk. */
void location_cache_remember_client(const struct Client *client);

#endif
