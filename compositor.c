/*
 * compositor.c — high-level job queue + orchestrator + free FCFS blit workers.
 *
 * display.c owns paint algorithms; this file owns when they run and threading.
 *
 * Two levels (strict roles):
 *   1) Job queue  — DRAW/MOVE/PAN/… consumed only by the orchestrator.
 *   2) Task queue — fine-grained paint fns; only the 3 workers grab FCFS.
 *      The orchestrator enqueues a batch and waits — it never runs tasks.
 */

#include "compositor.h"
#include "server.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct ServerState server;

#define COMP_QUEUE_CAP 256
#define COMP_NWORKERS 3

enum comp_op {
	COMP_DRAW = 1,
	COMP_MOVE,
	COMP_PAN,
	COMP_ERASE,
	COMP_FULL,
	COMP_CURSOR,
	COMP_QUIT
};

struct comp_job {
	enum comp_op op;
	uint64_t job_id; /* monotonic; ties enqueue/run/worker logs together */
	uint32_t client_id;
	int old_x, old_y, new_x, new_y;
	int sdx, sdy;
	int cursor_x, cursor_y;
	uint32_t erase_x, erase_y, erase_ww, erase_wh;
};

struct paint_task {
	void (*fn)(void *);
	void *arg;
	const char *tag; /* for debug: "underlay0", "mover", … */
	uint64_t job_id;
};

static struct comp_job queue[COMP_QUEUE_CAP];
static int q_head, q_tail, q_count;

static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_not_full = PTHREAD_COND_INITIALIZER;
static pthread_cond_t q_idle = PTHREAD_COND_INITIALIZER;

static pthread_t orch_thread;
static pthread_t workers[COMP_NWORKERS];

static int comp_inited;
static int comp_sync;
static int orch_running;
static int job_in_flight;
static int bgce_debug;
static uint64_t next_job_id = 1; /* 0 = none */

/* ---- Parallel batch hand-out for blit workers ---- */
static pthread_mutex_t work_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t work_done_cv = PTHREAD_COND_INITIALIZER;
static int tasks_left; /* incomplete tasks in the current batch */
static int task_next;  /* claim index (protected by work_mu) */
static int task_batch_n;
static struct paint_task task_batch[COMP_NWORKERS];
static int workers_running;
static int work_shutdown;

static __thread int tls_worker_id = -1;
/* Job currently running on this thread (orchestrator or worker via task). */
static __thread uint64_t tls_job_id;

static const char *op_name(enum comp_op op)
{
	switch (op) {
	case COMP_DRAW:   return "DRAW";
	case COMP_MOVE:   return "MOVE";
	case COMP_PAN:    return "PAN";
	case COMP_ERASE:  return "ERASE";
	case COMP_FULL:   return "FULL";
	case COMP_CURSOR: return "CURSOR";
	case COMP_QUIT:   return "QUIT";
	default:          return "?";
	}
}

static void debug_init(void)
{
	const char *e = getenv("BGCE_DEBUG");

	bgce_debug = (e && e[0] && e[0] != '0');
	if (bgce_debug)
		printf("[BGCE] debug: BGCE_DEBUG on\n");
}

int bgce_comp_debug(void)
{
	return bgce_debug;
}

int bgce_comp_worker_id(void)
{
	return tls_worker_id;
}

static void debug_fflush(void)
{
	fflush(stdout);
	fflush(stderr);
}

void bgce_comp_damage_log(const char *tag, int x0, int y0, int x1, int y1,
                          int worker)
{
	if (!bgce_debug)
		return;
	if (worker < 0)
		worker = tls_worker_id;
	if (worker >= 0)
		printf("[BGCE] damage job=%llu %s rect=(%d,%d)-(%d,%d) worker=%d\n",
		       (unsigned long long)tls_job_id, tag ? tag : "?",
		       x0, y0, x1, y1, worker);
	else
		printf("[BGCE] damage job=%llu %s rect=(%d,%d)-(%d,%d) worker=orch\n",
		       (unsigned long long)tls_job_id, tag ? tag : "?",
		       x0, y0, x1, y1);
	debug_fflush();
}

struct Client *bgce_comp_find_client(uint32_t client_id)
{
	struct Client *c;

	if (client_id == 0)
		return NULL;
	for (c = server.clients; c; c = c->next) {
		if (c->id == client_id)
			return c;
	}
	return NULL;
}

static void run_job(const struct comp_job *job)
{
	struct Client *c;
	struct Client gone;
	int wdx, wdy;
	uint32_t save_x, save_y;
	const char *app = "";

	tls_job_id = job->job_id;

	c = (job->client_id != 0) ? bgce_comp_find_client(job->client_id) : NULL;
	if (c && c->app_id[0])
		app = c->app_id;

	if (bgce_debug) {
		uint32_t ww = 0, wh = 0;
		int sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;

		if (c) {
			ww = c->world_w ? c->world_w : c->width;
			wh = c->world_h ? c->world_h : c->height;
			/* Approximate screen footprint at current zoom/pan. */
			{
				int z = server.zoom_pct > 0 ? server.zoom_pct
				                            : BGCE_ZOOM_PCT_1X;
				sx0 = ((int)c->x * z + 99) / 100 - server.pan_x;
				sy0 = ((int)c->y * z + 99) / 100 - server.pan_y;
				sx1 = ((int)(c->x + ww) * z + 99) / 100 -
				      server.pan_x;
				sy1 = ((int)(c->y + wh) * z + 99) / 100 -
				      server.pan_y;
			}
		}

		switch (job->op) {
		case COMP_DRAW:
			printf("[BGCE] comp: run job=%llu DRAW client=%u app='%s' "
			       "world=(%u,%u) %ux%u screen=(%d,%d)-(%d,%d) "
			       "zoom=%d%% pan=(%d,%d)\n",
			       (unsigned long long)job->job_id,
			       (unsigned)job->client_id, app,
			       c ? c->x : 0, c ? c->y : 0, ww, wh,
			       sx0, sy0, sx1, sy1,
			       server.zoom_pct, server.pan_x, server.pan_y);
			break;
		case COMP_MOVE:
			printf("[BGCE] comp: run job=%llu MOVE client=%u app='%s' "
			       "world (%d,%d)->(%d,%d) size=%ux%u "
			       "zoom=%d%% pan=(%d,%d)\n",
			       (unsigned long long)job->job_id,
			       (unsigned)job->client_id, app,
			       job->old_x, job->old_y, job->new_x, job->new_y,
			       ww, wh, server.zoom_pct, server.pan_x,
			       server.pan_y);
			break;
		case COMP_PAN:
			printf("[BGCE] comp: run job=%llu PAN d=(%d,%d) "
			       "(pan was %d,%d zoom=%d%%)\n",
			       (unsigned long long)job->job_id,
			       job->sdx, job->sdy,
			       server.pan_x, server.pan_y, server.zoom_pct);
			break;
		case COMP_ERASE:
			printf("[BGCE] comp: run job=%llu ERASE world=(%u,%u) %ux%u "
			       "zoom=%d%% pan=(%d,%d)\n",
			       (unsigned long long)job->job_id,
			       job->erase_x, job->erase_y,
			       job->erase_ww, job->erase_wh,
			       server.zoom_pct, server.pan_x, server.pan_y);
			break;
		case COMP_FULL:
			printf("[BGCE] comp: run job=%llu FULL zoom=%d%% pan=(%d,%d) "
			       "display=%ux%u\n",
			       (unsigned long long)job->job_id,
			       server.zoom_pct, server.pan_x, server.pan_y,
			       server.display_w, server.display_h);
			break;
		case COMP_CURSOR: {
			/* Mouse moves constantly — log 1 in 32 so DRAW stays visible. */
			static int cursor_run_n;
			if ((cursor_run_n++ % 32) == 0) {
				printf("[BGCE] comp: run job=%llu CURSOR screen=(%d,%d) "
				       "(1/32) zoom=%d%% pan=(%d,%d)\n",
				       (unsigned long long)job->job_id,
				       job->cursor_x, job->cursor_y,
				       server.zoom_pct, server.pan_x,
				       server.pan_y);
				debug_fflush();
			}
			break;
		}
		default:
			printf("[BGCE] comp: run job=%llu %s\n",
			       (unsigned long long)job->job_id, op_name(job->op));
			break;
		}
		if (job->op != COMP_CURSOR)
			debug_fflush();
	}

	switch (job->op) {
	case COMP_DRAW:
		if (c)
			draw(&server, c);
		else if (bgce_debug) {
			printf("[BGCE] comp: job=%llu DRAW client=%u — gone\n",
			       (unsigned long long)job->job_id,
			       (unsigned)job->client_id);
			debug_fflush();
		}
		break;

	case COMP_MOVE:
		if (!c)
			break;
		wdx = job->new_x - job->old_x;
		wdy = job->new_y - job->old_y;
		if (wdx == 0 && wdy == 0)
			break;
		/*
		 * Logical position may already be at new (or further ahead).
		 * Paint from old → new, then restore whatever input set.
		 */
		save_x = c->x;
		save_y = c->y;
		c->x = (uint32_t)job->old_x;
		c->y = (uint32_t)job->old_y;
		redraw_region(&server, c, wdx, wdy);
		c->x = save_x;
		c->y = save_y;
		break;

	case COMP_PAN:
		if (job->sdx || job->sdy)
			redraw_pan(&server, job->sdx, job->sdy);
		break;

	case COMP_ERASE:
		memset(&gone, 0, sizeof(gone));
		gone.x = job->erase_x;
		gone.y = job->erase_y;
		gone.world_w = job->erase_ww;
		gone.world_h = job->erase_wh;
		gone.width = job->erase_ww;
		gone.height = job->erase_wh;
		erase_client(&server, &gone);
		break;

	case COMP_FULL:
		redraw_all(&server);
		break;

	case COMP_CURSOR:
		set_cursor_pos(&server, job->cursor_x, job->cursor_y);
		break;

	case COMP_QUIT:
	default:
		break;
	}

	if (bgce_debug && job->op != COMP_CURSOR) {
		printf("[BGCE] comp: done job=%llu %s\n",
		       (unsigned long long)job->job_id, op_name(job->op));
		debug_fflush();
	}
	tls_job_id = 0;
}

static void *worker_main(void *arg)
{
	int id = (int)(intptr_t)arg;

	tls_worker_id = id;

	for (;;) {
		struct paint_task task;
		int my;

		memset(&task, 0, sizeof(task));

		pthread_mutex_lock(&work_mu);
		/*
		 * Wait for a new batch (task_batch_n > 0 and claims remain),
		 * or shutdown.  Claim index is shared so each task is taken
		 * by exactly one worker — true parallel hand-out, not
		 * "fastest worker drains the whole queue alone".
		 */
		while (!work_shutdown &&
		       (task_batch_n == 0 || task_next >= task_batch_n))
			pthread_cond_wait(&work_cv, &work_mu);
		if (work_shutdown &&
		    (task_batch_n == 0 || task_next >= task_batch_n)) {
			pthread_mutex_unlock(&work_mu);
			break;
		}
		my = task_next++;
		if (my >= task_batch_n) {
			pthread_mutex_unlock(&work_mu);
			continue;
		}
		task = task_batch[my];
		pthread_mutex_unlock(&work_mu);

		tls_job_id = task.job_id;

		if (bgce_debug) {
			printf("[BGCE] worker %d: job=%llu got task '%s' "
			       "(claim %d/%d)\n",
			       id, (unsigned long long)task.job_id,
			       task.tag ? task.tag : "?", my + 1, task_batch_n);
			debug_fflush();
		}

		if (task.fn)
			task.fn(task.arg);

		if (bgce_debug) {
			printf("[BGCE] worker %d: job=%llu done task '%s'\n",
			       id, (unsigned long long)task.job_id,
			       task.tag ? task.tag : "?");
			debug_fflush();
		}
		tls_job_id = 0;

		pthread_mutex_lock(&work_mu);
		if (tasks_left > 0)
			tasks_left--;
		if (tasks_left == 0) {
			task_batch_n = 0;
			task_next = 0;
			pthread_cond_signal(&work_done_cv);
		}
		pthread_mutex_unlock(&work_mu);
	}

	tls_worker_id = -1;
	return NULL;
}

void bgce_comp_parallel3(void (*f0)(void *), void *a0, void (*f1)(void *),
                         void *a1, void (*f2)(void *), void *a2)
{
	int n = 0;
	int i;
	void (*fns[3])(void *) = { f0, f1, f2 };
	void *args[3] = { a0, a1, a2 };
	static const char *const tags[3] = { "underlay0", "underlay1", "mover" };

	for (i = 0; i < 3; i++) {
		if (fns[i])
			n++;
	}
	if (n == 0)
		return;

	/* Sync mode or workers not up: serial on caller. */
	if (comp_sync || !workers_running) {
		if (bgce_debug) {
			printf("[BGCE] comp: job=%llu parallel3 serial n=%d\n",
			       (unsigned long long)tls_job_id, n);
			debug_fflush();
		}
		for (i = 0; i < 3; i++) {
			if (!fns[i])
				continue;
			if (bgce_debug) {
				printf("[BGCE] worker orch: job=%llu got task '%s'\n",
				       (unsigned long long)tls_job_id, tags[i]);
				debug_fflush();
			}
			fns[i](args[i]);
		}
		return;
	}

	/*
	 * Build a fixed batch; workers claim indices 0..n-1 under the mutex
	 * so three free workers each take one task and run in parallel.
	 * (A plain FIFO queue let one worker finish and re-grab the rest
	 * before siblings ran — looked like "only worker 2 works".)
	 */
	pthread_mutex_lock(&work_mu);
	while (tasks_left > 0)
		pthread_cond_wait(&work_done_cv, &work_mu);

	task_batch_n = 0;
	task_next = 0;
	tasks_left = n;
	for (i = 0; i < 3; i++) {
		if (!fns[i])
			continue;
		task_batch[task_batch_n].fn = fns[i];
		task_batch[task_batch_n].arg = args[i];
		task_batch[task_batch_n].tag = tags[i];
		task_batch[task_batch_n].job_id = tls_job_id;
		task_batch_n++;
	}
	if (bgce_debug) {
		printf("[BGCE] comp: job=%llu parallel3 batch n=%d → claim 0..%d\n",
		       (unsigned long long)tls_job_id, n, n - 1);
		debug_fflush();
	}
	pthread_cond_broadcast(&work_cv);
	while (tasks_left > 0)
		pthread_cond_wait(&work_done_cv, &work_mu);
	pthread_mutex_unlock(&work_mu);
}

/* Assign monotonic job id before enqueue / sync run. */
static void assign_job_id(struct comp_job *job)
{
	job->job_id = next_job_id++;
	if (job->job_id == 0)
		job->job_id = next_job_id++;
}

/*
 * Never block the producer (input) on a full queue — that freezes the mouse.
 * Drop the new job if the queue is saturated (paint may lag; input stays live).
 */
static void enqueue(struct comp_job *job)
{
	static int drop_log;

	assign_job_id(job);

	if (comp_sync) {
		run_job(job);
		return;
	}

	pthread_mutex_lock(&q_mu);
	if (q_count >= COMP_QUEUE_CAP) {
		pthread_mutex_unlock(&q_mu);
		if ((drop_log++ % 64) == 0) {
			fprintf(stderr,
			        "[BGCE] compositor: queue full, dropping job=%llu %s\n",
			        (unsigned long long)job->job_id, op_name(job->op));
			debug_fflush();
		}
		return;
	}
	queue[q_tail] = *job;
	q_tail = (q_tail + 1) % COMP_QUEUE_CAP;
	q_count++;
	if (bgce_debug && job->op != COMP_CURSOR) {
		/* CURSOR is rate-limited in submit_cursor (see comment there). */
		struct Client *c = NULL;
		const char *app = "";
		uint32_t cx = 0, cy = 0, ww = 0, wh = 0;

		if (job->client_id)
			c = bgce_comp_find_client(job->client_id);
		if (c) {
			if (c->app_id[0])
				app = c->app_id;
			cx = c->x;
			cy = c->y;
			ww = c->world_w ? c->world_w : c->width;
			wh = c->world_h ? c->world_h : c->height;
		}
		if (job->op == COMP_DRAW)
			printf("[BGCE] comp: enqueue job=%llu DRAW client=%u "
			       "app='%s' world=(%u,%u) %ux%u q=%d\n",
			       (unsigned long long)job->job_id,
			       (unsigned)job->client_id, app, cx, cy, ww, wh,
			       q_count);
		else if (job->op == COMP_MOVE)
			printf("[BGCE] comp: enqueue job=%llu MOVE client=%u "
			       "app='%s' (%d,%d)->(%d,%d) q=%d\n",
			       (unsigned long long)job->job_id,
			       (unsigned)job->client_id, app,
			       job->old_x, job->old_y, job->new_x, job->new_y,
			       q_count);
		else if (job->op == COMP_PAN)
			printf("[BGCE] comp: enqueue job=%llu PAN d=(%d,%d) q=%d\n",
			       (unsigned long long)job->job_id,
			       job->sdx, job->sdy, q_count);
		else
			printf("[BGCE] comp: enqueue job=%llu %s client=%u "
			       "app='%s' q=%d\n",
			       (unsigned long long)job->job_id, op_name(job->op),
			       (unsigned)job->client_id, app, q_count);
		debug_fflush();
	}
	pthread_cond_signal(&q_not_empty);
	pthread_mutex_unlock(&q_mu);
}

static int dequeue(struct comp_job *out)
{
	pthread_mutex_lock(&q_mu);
	while (q_count == 0 && orch_running)
		pthread_cond_wait(&q_not_empty, &q_mu);
	if (q_count == 0) {
		pthread_mutex_unlock(&q_mu);
		return 0;
	}
	*out = queue[q_head];
	q_head = (q_head + 1) % COMP_QUEUE_CAP;
	q_count--;
	job_in_flight = 1;
	pthread_cond_signal(&q_not_full);
	pthread_mutex_unlock(&q_mu);
	return 1;
}

static void job_done(void)
{
	pthread_mutex_lock(&q_mu);
	job_in_flight = 0;
	if (q_count == 0)
		pthread_cond_broadcast(&q_idle);
	pthread_mutex_unlock(&q_mu);
}

static void *orchestrator_main(void *arg)
{
	struct comp_job job;

	(void)arg;
	for (;;) {
		if (!dequeue(&job))
			break;
		if (job.op == COMP_QUIT) {
			job_done();
			break;
		}
		run_job(&job);
		job_done();
	}
	return NULL;
}

int bgce_comp_init(int sync_mode)
{
	int i;

	if (comp_inited)
		bgce_comp_shutdown();

	debug_init();
	comp_sync = sync_mode ? 1 : 0;
	q_head = q_tail = q_count = 0;
	job_in_flight = 0;
	work_shutdown = 0;
	tasks_left = 0;
	task_next = 0;
	task_batch_n = 0;
	workers_running = 0;
	orch_running = 0;

	if (comp_sync) {
		comp_inited = 1;
		printf("[BGCE] compositor: sync mode (inline paint)\n");
		return 0;
	}

	orch_running = 1;
	if (pthread_create(&orch_thread, NULL, orchestrator_main, NULL) != 0) {
		perror("[BGCE] compositor orchestrator");
		orch_running = 0;
		comp_sync = 1;
		comp_inited = 1;
		return -1;
	}

	/* Default: 3 FCFS blit workers for MOVE underlay/mover tasks. */
	for (i = 0; i < COMP_NWORKERS; i++) {
		if (pthread_create(&workers[i], NULL, worker_main,
		                   (void *)(intptr_t)i) != 0) {
			perror("[BGCE] compositor worker");
			break;
		}
		workers_running++;
	}

	comp_inited = 1;
	printf("[BGCE] compositor: async queue, %d FCFS blit worker(s)\n",
	       workers_running);
	return 0;
}

void bgce_comp_shutdown(void)
{
	struct comp_job quit;
	int i;

	if (!comp_inited)
		return;

	if (!comp_sync && orch_running) {
		memset(&quit, 0, sizeof(quit));
		quit.op = COMP_QUIT;
		enqueue(&quit);
		pthread_join(orch_thread, NULL);
		orch_running = 0;
	}

	if (workers_running > 0) {
		pthread_mutex_lock(&work_mu);
		work_shutdown = 1;
		pthread_cond_broadcast(&work_cv);
		pthread_mutex_unlock(&work_mu);
		for (i = 0; i < workers_running; i++)
			pthread_join(workers[i], NULL);
		workers_running = 0;
	}

	comp_inited = 0;
	comp_sync = 0;
	q_head = q_tail = q_count = 0;
	printf("[BGCE] compositor: shutdown\n");
}

void bgce_comp_flush(void)
{
	if (!comp_inited || comp_sync)
		return;

	pthread_mutex_lock(&q_mu);
	while (q_count > 0 || job_in_flight)
		pthread_cond_wait(&q_idle, &q_mu);
	pthread_mutex_unlock(&q_mu);
}

void bgce_comp_submit_draw(uint32_t client_id)
{
	struct comp_job j;

	if (!comp_inited) {
		struct Client *c = bgce_comp_find_client(client_id);
		if (c)
			draw(&server, c);
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_DRAW;
	j.client_id = client_id;
	enqueue(&j);
}

void bgce_comp_submit_move(uint32_t client_id, int old_x, int old_y, int new_x,
                           int new_y)
{
	struct comp_job j;

	if (!comp_inited) {
		struct Client *c = bgce_comp_find_client(client_id);
		int wdx, wdy;
		uint32_t sx, sy;

		if (!c)
			return;
		wdx = new_x - old_x;
		wdy = new_y - old_y;
		sx = c->x;
		sy = c->y;
		c->x = (uint32_t)old_x;
		c->y = (uint32_t)old_y;
		redraw_region(&server, c, wdx, wdy);
		c->x = sx;
		c->y = sy;
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_MOVE;
	j.client_id = client_id;
	j.old_x = old_x;
	j.old_y = old_y;
	j.new_x = new_x;
	j.new_y = new_y;
	enqueue(&j);
}

void bgce_comp_submit_pan(int sdx, int sdy)
{
	struct comp_job j;

	if (!comp_inited) {
		if (sdx || sdy)
			redraw_pan(&server, sdx, sdy);
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_PAN;
	j.sdx = sdx;
	j.sdy = sdy;
	enqueue(&j);
}

void bgce_comp_submit_erase(uint32_t x, uint32_t y, uint32_t world_w,
                            uint32_t world_h)
{
	struct comp_job j;

	if (!comp_inited) {
		struct Client gone;

		memset(&gone, 0, sizeof(gone));
		gone.x = x;
		gone.y = y;
		gone.world_w = world_w;
		gone.world_h = world_h;
		gone.width = world_w;
		gone.height = world_h;
		erase_client(&server, &gone);
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_ERASE;
	j.erase_x = x;
	j.erase_y = y;
	j.erase_ww = world_w;
	j.erase_wh = world_h;
	enqueue(&j);
}

void bgce_comp_submit_full(void)
{
	struct comp_job j;

	if (!comp_inited) {
		redraw_all(&server);
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_FULL;
	enqueue(&j);
}

void bgce_comp_submit_cursor(int x, int y)
{
	struct comp_job j;

	if (!comp_inited) {
		set_cursor_pos(&server, x, y);
		return;
	}
	memset(&j, 0, sizeof(j));
	j.op = COMP_CURSOR;
	j.cursor_x = x;
	j.cursor_y = y;
	/* job_id assigned in enqueue; rate-limit log after assign via flag */
	{
		static int n;
		int log_this = bgce_debug && ((n++ % 32) == 0);

		enqueue(&j);
		if (log_this) {
			printf("[BGCE] comp: enqueue job=%llu CURSOR screen=(%d,%d) "
			       "(1/32 samples)\n",
			       (unsigned long long)j.job_id, x, y);
			debug_fflush();
		}
	}
}
