#include "stack_expand.h"

#include "frame.h"
#include "note.h"
#include "rcu.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define MINIMM_STACK_NODE_CAPACITY 3U
#define MINIMM_STACK_WALK_WAIT_NS 1000000000L

typedef struct minimm_stack_node {
	minimm_frame_t *frame;
	uint64_t generation;
	bool active;
} minimm_stack_node_t;

typedef struct minimm_stack_node_pool {
	minimm_stack_node_t nodes[MINIMM_STACK_NODE_CAPACITY];
	uint64_t next_generation;
} minimm_stack_node_pool_t;

typedef struct minimm_stack_expand_run {
	minimm_rcu_domain_t *rcu;
	_Atomic(minimm_stack_node_t *) root;
	minimm_stack_node_pool_t pool;
	pthread_rwlock_t mmap_lock;
	pthread_mutex_t gate_lock;
	pthread_cond_t gate_changed;
	const unsigned char *data;
	size_t within_page;
	size_t length;
	size_t completed;
	minimm_status_t walker_status;
	bool walker_loaded;
	bool update_started;
	bool walker_proceeding;
	bool walker_done;
	bool reuse_complete;
	bool abort;
	atomic_bool walker_exited;
} minimm_stack_expand_run_t;

static minimm_status_t minimm_stack_node_acquire(minimm_stack_node_pool_t *pool,
						 minimm_frame_t *frame,
						 minimm_stack_node_t **out_node)
{
	size_t index = 0U;

	if (pool == NULL || frame == NULL || out_node == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_node = NULL;
	if (pool->next_generation == UINT64_MAX) {
		return MINIMM_ERROR_NO_SPACE;
	}
	for (index = 0U; index < MINIMM_STACK_NODE_CAPACITY; ++index) {
		minimm_stack_node_t *node = &pool->nodes[index];

		if (node->active) {
			continue;
		}
		pool->next_generation += UINT64_C(1);
		node->frame = frame;
		node->generation = pool->next_generation;
		node->active = true;
		*out_node = node;
		return MINIMM_OK;
	}
	return MINIMM_ERROR_NO_SPACE;
}

/* Release node-owned state and return its slot to the bounded tree-node pool. */
static void minimm_stack_node_reclaim(void *object)
{
	minimm_stack_node_t *node = object;
	minimm_frame_t *frame = NULL;

	if (node == NULL || !node->active || node->frame == NULL) {
		return;
	}
	frame = node->frame;
	node->frame = NULL;
	node->active = false;
	minimm_frame_release(frame);
}

static void minimm_stack_node_pool_release(minimm_stack_node_pool_t *pool)
{
	size_t index = 0U;

	if (pool == NULL) {
		return;
	}
	for (index = 0U; index < MINIMM_STACK_NODE_CAPACITY; ++index) {
		if (pool->nodes[index].active) {
			minimm_stack_node_reclaim(&pool->nodes[index]);
		}
	}
}

static void minimm_stack_expand_signal(minimm_stack_expand_run_t *run, bool reuse_complete,
				       minimm_status_t status)
{
	(void)pthread_mutex_lock(&run->gate_lock);
	if (reuse_complete) {
		run->reuse_complete = true;
	}
	if (status != MINIMM_OK) {
		run->abort = true;
	}
	(void)pthread_cond_broadcast(&run->gate_changed);
	(void)pthread_mutex_unlock(&run->gate_lock);
}

/* Walk the current published stack-tree node and apply the transient marker. */
static void *minimm_stack_expand_walk_main(void *context)
{
	minimm_stack_expand_run_t *run = context;
	minimm_stack_node_t *node = NULL;
	struct timespec deadline = { 0 };
	minimm_status_t status = MINIMM_OK;
	int wait_status = 0;
	bool mmap_locked = false;
	bool timed_out = false;

	if (pthread_rwlock_rdlock(&run->mmap_lock) != 0) {
		status = MINIMM_ERROR_IO;
	} else {
		mmap_locked = true;
		node = atomic_load_explicit(&run->root, memory_order_seq_cst);
	}
	(void)pthread_mutex_lock(&run->gate_lock);
	if (status != MINIMM_OK || node == NULL || clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		run->abort = true;
		status = MINIMM_ERROR_IO;
	} else {
		deadline.tv_nsec += MINIMM_STACK_WALK_WAIT_NS;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec += 1;
			deadline.tv_nsec -= 1000000000L;
		}
		run->walker_loaded = true;
	}
	(void)pthread_cond_broadcast(&run->gate_changed);
	while (!run->reuse_complete && !run->abort) {
		wait_status = timed_out ? pthread_cond_wait(&run->gate_changed, &run->gate_lock) :
					  pthread_cond_timedwait(&run->gate_changed,
								 &run->gate_lock, &deadline);
		if (wait_status == ETIMEDOUT) {
			wait_status = 0;
			if (!run->update_started) {
				run->walker_proceeding = true;
				(void)pthread_cond_broadcast(&run->gate_changed);
				break;
			}
			timed_out = true;
		}
		if (wait_status != 0) {
			run->abort = true;
			status = MINIMM_ERROR_IO;
			(void)pthread_cond_broadcast(&run->gate_changed);
		}
	}
	if (run->abort && status == MINIMM_OK) {
		status = MINIMM_ERROR_BUSY;
	}
	(void)pthread_mutex_unlock(&run->gate_lock);

	if (status == MINIMM_OK) {
		status = minimm_frame_write(node->frame, run->within_page, run->data, run->length);
		if (status == MINIMM_OK) {
			run->completed = run->length;
		}
	}
	(void)pthread_mutex_lock(&run->gate_lock);
	run->walker_done = true;
	(void)pthread_cond_broadcast(&run->gate_changed);
	(void)pthread_mutex_unlock(&run->gate_lock);
	if (mmap_locked) {
		(void)pthread_rwlock_unlock(&run->mmap_lock);
	}
	run->walker_status = status;
	atomic_store_explicit(&run->walker_exited, true, memory_order_release);
	return NULL;
}

static minimm_status_t minimm_stack_expand_begin_update(minimm_stack_expand_run_t *run)
{
	minimm_status_t status = MINIMM_OK;
	int wait_status = 0;

	(void)pthread_mutex_lock(&run->gate_lock);
	if (!run->walker_proceeding) {
		run->update_started = true;
		(void)pthread_cond_broadcast(&run->gate_changed);
	} else {
		while (!run->walker_done && !run->abort) {
			wait_status = pthread_cond_wait(&run->gate_changed, &run->gate_lock);
			if (wait_status != 0) {
				run->abort = true;
				(void)pthread_cond_broadcast(&run->gate_changed);
				break;
			}
		}
		if (wait_status != 0) {
			status = MINIMM_ERROR_IO;
		} else if (!run->walker_done) {
			status = MINIMM_ERROR_BUSY;
		}
	}
	(void)pthread_mutex_unlock(&run->gate_lock);
	return status;
}

static minimm_status_t minimm_stack_expand_wait_for_walker(minimm_stack_expand_run_t *run)
{
	minimm_status_t status = MINIMM_OK;
	int wait_status = 0;

	(void)pthread_mutex_lock(&run->gate_lock);
	while (!run->walker_loaded && !run->abort) {
		wait_status = pthread_cond_wait(&run->gate_changed, &run->gate_lock);
		if (wait_status != 0) {
			run->abort = true;
			(void)pthread_cond_broadcast(&run->gate_changed);
			break;
		}
	}
	if (wait_status != 0) {
		status = MINIMM_ERROR_IO;
	} else if (!run->walker_loaded) {
		status = MINIMM_ERROR_BUSY;
	}
	(void)pthread_mutex_unlock(&run->gate_lock);
	return status;
}

static minimm_status_t minimm_stack_expand_replace_root(minimm_stack_expand_run_t *run,
							minimm_stack_node_t *replacement,
							minimm_frame_t *backing_frame)
{
	minimm_stack_node_t *old = NULL;
	minimm_stack_node_t *backing_node = NULL;
	minimm_status_t status = minimm_stack_expand_wait_for_walker(run);
	bool mmap_locked = false;

	if (status != MINIMM_OK) {
		return status;
	}
	if (pthread_rwlock_rdlock(&run->mmap_lock) != 0) {
		return MINIMM_ERROR_IO;
	}
	mmap_locked = true;
	status = minimm_stack_expand_begin_update(run);
	if (status != MINIMM_OK) {
		goto done;
	}
	old = atomic_exchange_explicit(&run->root, replacement, memory_order_seq_cst);
	if (old == NULL) {
		status = MINIMM_ERROR_IO;
		goto done;
	}
	status = minimm_rcu_retire(run->rcu, old, minimm_stack_node_reclaim);
	if (status != MINIMM_OK) {
		goto done;
	}

	/* Collect quiescent roots before allocating the backing node. */
	(void)minimm_rcu_poll(run->rcu);
	status = minimm_stack_node_acquire(&run->pool, backing_frame, &backing_node);
	if (status != MINIMM_OK) {
		goto done;
	}
	(void)backing_node;
	minimm_stack_expand_signal(run, true, MINIMM_OK);

done:
	if (mmap_locked) {
		(void)pthread_rwlock_unlock(&run->mmap_lock);
	}
	return status;
}

minimm_status_t minimm_stack_expand_apply(minimm_t *mm, minimm_note_t *note, uint64_t offset,
					  const void *data, size_t length, size_t *out_completed)
{
	const uint64_t page_mask = MINIMM_PAGE_SIZE - UINT64_C(1);
	const uint64_t note_size = note == NULL ? UINT64_C(0) : minimm_note_size(note);
	const uint64_t page_offset = offset & ~page_mask;
	const size_t within_page = (size_t)(offset & page_mask);
	unsigned char stable_data[MINIMM_PAGE_SIZE];
	minimm_stack_expand_run_t run = { 0 };
	minimm_frame_t *backing_frame = NULL;
	minimm_frame_t *first_private = NULL;
	minimm_frame_t *replacement_private = NULL;
	minimm_stack_node_t *first = NULL;
	minimm_stack_node_t *replacement = NULL;
	pthread_t walker;
	minimm_status_t status = MINIMM_OK;
	bool gate_lock_initialized = false;
	bool gate_changed_initialized = false;
	bool mmap_lock_initialized = false;
	bool walker_created = false;
	bool old_retired = false;
	int join_status = 0;

	if (out_completed != NULL) {
		*out_completed = 0U;
	}
	if (mm == NULL || note == NULL || data == NULL || length == 0U ||
	    length > (size_t)MINIMM_PAGE_SIZE || !minimm_note_belongs_to(note, mm) ||
	    (minimm_note_rights(note) & MINIMM_NOTE_RIGHT_READ) == 0U || offset > note_size ||
	    (uint64_t)length > note_size - offset ||
	    length > (size_t)MINIMM_PAGE_SIZE - within_page || page_offset > note_size ||
	    MINIMM_PAGE_SIZE > note_size - page_offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memcpy(stable_data, data, length);
	run.data = stable_data;
	run.within_page = within_page;
	run.length = length;
	run.walker_status = MINIMM_ERROR_BUSY;
	atomic_init(&run.root, NULL);
	atomic_init(&run.walker_exited, false);

	if (pthread_rwlock_init(&run.mmap_lock, NULL) != 0) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	mmap_lock_initialized = true;
	if (pthread_mutex_init(&run.gate_lock, NULL) != 0) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto cleanup;
	}
	gate_lock_initialized = true;
	if (pthread_cond_init(&run.gate_changed, NULL) != 0) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto cleanup;
	}
	gate_changed_initialized = true;
	status = minimm_rcu_domain_create(&run.rcu);
	if (status != MINIMM_OK) {
		goto cleanup;
	}
	status = minimm_note_get_frame(note, page_offset, &backing_frame);
	if (status == MINIMM_OK) {
		status = minimm_frame_copy(backing_frame, &first_private);
	}
	if (status == MINIMM_OK) {
		status = minimm_frame_copy(backing_frame, &replacement_private);
	}
	if (status == MINIMM_OK) {
		status = minimm_stack_node_acquire(&run.pool, first_private, &first);
		if (status == MINIMM_OK) {
			first_private = NULL;
		}
	}
	if (status == MINIMM_OK) {
		status = minimm_stack_node_acquire(&run.pool, replacement_private, &replacement);
		if (status == MINIMM_OK) {
			replacement_private = NULL;
		}
	}
	if (status != MINIMM_OK) {
		goto cleanup;
	}
	atomic_store_explicit(&run.root, first, memory_order_seq_cst);

	if (pthread_create(&walker, NULL, minimm_stack_expand_walk_main, &run) != 0) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto cleanup;
	}
	walker_created = true;
	status = minimm_stack_expand_replace_root(&run, replacement, backing_frame);
	if (status == MINIMM_OK) {
		backing_frame = NULL;
		old_retired = true;
	} else {
		minimm_stack_expand_signal(&run, false, status);
	}
	join_status = pthread_join(walker, NULL);
	if (join_status != 0) {
		minimm_stack_expand_signal(&run, false, MINIMM_ERROR_IO);
		while (!atomic_load_explicit(&run.walker_exited, memory_order_acquire)) {
			(void)sched_yield();
		}
		status = MINIMM_ERROR_IO;
	}
	walker_created = false;
	if (old_retired) {
		(void)minimm_rcu_synchronize(run.rcu);
	}
	if (status == MINIMM_OK) {
		status = run.walker_status;
	}
	if (status == MINIMM_OK && run.completed != length) {
		status = MINIMM_ERROR_IO;
	}
	if (status == MINIMM_OK && out_completed != NULL) {
		*out_completed = run.completed;
	}

cleanup:
	if (walker_created) {
		minimm_stack_expand_signal(&run, false, status);
		(void)pthread_join(walker, NULL);
	}
	if (run.rcu != NULL) {
		(void)minimm_rcu_synchronize(run.rcu);
	}
	minimm_frame_release(replacement_private);
	minimm_frame_release(first_private);
	minimm_frame_release(backing_frame);
	minimm_stack_node_pool_release(&run.pool);
	minimm_rcu_domain_destroy(run.rcu);
	if (gate_changed_initialized) {
		(void)pthread_cond_destroy(&run.gate_changed);
	}
	if (gate_lock_initialized) {
		(void)pthread_mutex_destroy(&run.gate_lock);
	}
	if (mmap_lock_initialized) {
		(void)pthread_rwlock_destroy(&run.mmap_lock);
	}
	return status;
}
