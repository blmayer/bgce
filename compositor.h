#ifndef BGCE_COMPOSITOR_H
#define BGCE_COMPOSITOR_H

#include <stdint.h>

struct Client;

/**
 * Compositor: owns framebuffer writes via a job queue.
 *
 * - Async mode: orchestrator pops high-level jobs (DRAW/MOVE/PAN/…).
 *   A pool of 3 blit workers runs fine-grained tasks from a free FCFS
 *   task queue (any worker can take any task — underlay or mover, etc.).
 *   No coalescing — every submitted event is painted.
 * - Sync mode (headless/mock): submit runs paint on the caller; parallel
 *   tasks run serially.
 *
 * Debug rect logs: set env BGCE_DEBUG_DAMAGE=1.
 */

/** Start compositor. sync_mode != 0 → no background threads (inline paint). */
int bgce_comp_init(int sync_mode);

/** Stop workers, drain queue, join threads. Safe if never inited. */
void bgce_comp_shutdown(void);

/** Block until the queue is empty and no job is in flight. */
void bgce_comp_flush(void);

/** MSG_DRAW: blit one client (clipped by windows above). */
void bgce_comp_submit_draw(uint32_t client_id);

/**
 * Window move by world delta.  Caller has already applied new_x/new_y to the
 * client; the job paints from (old_x,old_y) → (new_x,new_y).
 */
void bgce_comp_submit_move(uint32_t client_id, int old_x, int old_y,
                           int new_x, int new_y);

/** Desktop pan by screen-pixel delta (caller already updated pan, or not —
 *  pan job applies redraw_pan which updates pan inside display). */
void bgce_comp_submit_pan(int sdx, int sdy);

/**
 * Client gone: repaint its former footprint.  Geometry is snapshotted in the
 * job (safe after the Client is unlinked / freed).
 */
void bgce_comp_submit_erase(uint32_t x, uint32_t y, uint32_t world_w,
                            uint32_t world_h);

/** Full-scene recomposite (zoom, Alt+Tab viewport, hard invalidate). */
void bgce_comp_submit_full(void);

/**
 * Enqueue up to three independent paint tasks and wait until all complete.
 * Workers grab tasks first-come-first-serve (not pinned to L0/L1/mover).
 * NULL function slots are skipped.  In sync mode, runs serially.
 */
void bgce_comp_parallel3(void (*f0)(void *), void *a0, void (*f1)(void *),
                         void *a1, void (*f2)(void *), void *a2);

/**
 * Id of the blit worker running this thread (0..2), or -1 if not a worker
 * (serial/sync path).  Useful for damage debug logs.
 */
int bgce_comp_worker_id(void);

/** Debug: log a damage rect if BGCE_DEBUG_DAMAGE is set. worker=-1 → auto/serial. */
void bgce_comp_damage_log(const char *tag, int x0, int y0, int x1, int y1,
                          int worker);

/** Look up a live client by id (for paint paths). */
struct Client *bgce_comp_find_client(uint32_t client_id);

#endif /* BGCE_COMPOSITOR_H */
