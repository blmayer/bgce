#ifndef BGCE_COMPOSITOR_H
#define BGCE_COMPOSITOR_H

#include <stdint.h>

struct Client;

/**
 * Compositor: owns all framebuffer writes via a job queue.
 *
 * - Async mode: orchestrator pops jobs (DRAW/MOVE/PAN/CURSOR/…).
 *   A pool of 3 blit workers runs fine-grained MOVE tasks FCFS.
 *   Input never paints — it only enqueues (including mouse motion).
 * - Sync mode (headless): submit runs on the caller; parallel tasks serial.
 *
 * Debug: BGCE_DEBUG=1 — op names, enqueue/run/done, which worker took each
 *   MOVE task, client DRAW, damage rects (one switch; fflush'd for log pipe).
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

/** Desktop pan by screen-pixel delta. */
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
 * Software cursor move — input enqueues this; compositor paints the glyph.
 * Screen pixel coordinates (top-left of 32×32 tile).
 */
void bgce_comp_submit_cursor(int x, int y);

/**
 * Enqueue up to three independent paint tasks and wait until all complete.
 * Workers grab FCFS.  NULL slots skipped.  Sync mode runs serially.
 */
void bgce_comp_parallel3(void (*f0)(void *), void *a0, void (*f1)(void *),
                         void *a1, void (*f2)(void *), void *a2);

/** Blit worker id 0..2, or -1 if not a worker. */
int bgce_comp_worker_id(void);

/** Log a damage rect when BGCE_DEBUG is set. worker=-1 → auto from TLS. */
void bgce_comp_damage_log(const char *tag, int x0, int y0, int x1, int y1,
                          int worker);

/** Look up a live client by id (for paint paths). */
struct Client *bgce_comp_find_client(uint32_t client_id);

/** Non-zero when BGCE_DEBUG is enabled. */
int bgce_comp_debug(void);

#endif /* BGCE_COMPOSITOR_H */
