#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MINIMM_FRAME_NO_RESIDENT_SLOT SIZE_MAX

typedef struct minimm_swap_slot {
	struct minimm_swap_slot *next;
	uint64_t offset;
} minimm_swap_slot_t;

typedef enum minimm_frame_source {
	MINIMM_FRAME_SOURCE_ZERO = 0,
	MINIMM_FRAME_SOURCE_FILE
} minimm_frame_source_t;

struct minimm_frame {
	minimm_frame_store_t *store;
	minimm_frame_t *next;
	atomic_size_t references;
	atomic_size_t mappings;
	uint64_t id;
	uint64_t last_access;
	uint64_t file_offset;
	unsigned char *data;
	minimm_file_backing_t *file_backing;
	minimm_swap_slot_t *swap_slot;
	size_t pin_count;
	size_t transient_pin_count;
	size_t resident_slot;
	minimm_frame_source_t source;
	bool file_shared;
	bool dirty;
	bool cold;
	bool reclaim_pending;
};

struct minimm_file_backing {
	atomic_size_t references;
	atomic_int writeback_error;
	int fd;
};

struct minimm_frame_store {
	pthread_mutex_t lock;
	pthread_cond_t transient_unpinned;
	minimm_frame_t *frames;
	size_t frame_count;
	size_t resident_count;
	size_t resident_limit;
	size_t transient_waiter_count;
	uint64_t next_frame_id;
	uint64_t next_swap_offset;
	uint64_t clock;
	uint64_t page_in_count;
	uint64_t page_out_count;
	uint64_t reclaim_scan_count;
	uint64_t reclaim_count;
	uint64_t refault_count;
	size_t swap_slot_count;
	size_t swap_slot_high_water;
	minimm_swap_slot_t *free_swap_slots;
	/*
	 * This host mapping is only a resident-byte arena. MiniMM virtual
	 * addresses, permissions, faults, and COW never map to its protection.
	 */
	unsigned char *resident_arena;
	size_t resident_arena_size;
	size_t *free_resident_slots;
	size_t free_resident_slot_count;
	int swap_fd;
	bool destroy_requested;
};

static void minimm_frame_counter_increment_saturating(uint64_t *counter)
{
	if (*counter != UINT64_MAX) {
		*counter += 1U;
	}
}

static void minimm_frame_size_add_saturating(size_t *value, size_t increment)
{
	if (increment > SIZE_MAX - *value) {
		*value = SIZE_MAX;
	} else {
		*value += increment;
	}
}

static void minimm_frame_store_free(minimm_frame_store_t *store)
{
	minimm_swap_slot_t *slot = store->free_swap_slots;

	while (slot != NULL) {
		minimm_swap_slot_t *next = slot->next;

		free(slot);
		slot = next;
	}
	if (store->resident_arena != NULL) {
		(void)munmap(store->resident_arena, store->resident_arena_size);
	}
	free(store->free_resident_slots);
	(void)close(store->swap_fd);
	(void)pthread_cond_destroy(&store->transient_unpinned);
	(void)pthread_mutex_destroy(&store->lock);
	free(store);
}

static minimm_status_t minimm_frame_status_from_errno(int error_number)
{
	switch (error_number) {
	case ENOMEM:
		return MINIMM_ERROR_OUT_OF_MEMORY;
	case EDQUOT:
	case EFBIG:
	case EMFILE:
	case ENFILE:
	case ENOSPC:
	case EOVERFLOW:
		return MINIMM_ERROR_NO_SPACE;
	case EBADF:
	case EINVAL:
		return MINIMM_ERROR_INVALID_ARGUMENT;
	default:
		return MINIMM_ERROR_IO;
	}
}

#if !defined(F_DUPFD_CLOEXEC) || !defined(O_CLOEXEC)
static int minimm_set_cloexec(int fd)
{
	int result = 0;

	do {
		result = fcntl(fd, F_SETFD, FD_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	return result;
}
#endif

static int minimm_unlink_retry(const char *path)
{
	int result = 0;

	do {
		result = unlink(path);
	} while (result != 0 && errno == EINTR);
	return result;
}

static minimm_status_t minimm_read_page(int fd, uint64_t offset, unsigned char *buffer)
{
	size_t completed = 0U;

	if (offset > (uint64_t)INT64_MAX) {
		return MINIMM_ERROR_IO;
	}

	while (completed < MINIMM_PAGE_SIZE) {
		ssize_t result = 0;
		const uint64_t current_offset = offset + completed;

		if (current_offset > (uint64_t)INT64_MAX) {
			return MINIMM_ERROR_IO;
		}

		result = pread(fd, buffer + completed, MINIMM_PAGE_SIZE - completed,
			       (off_t)current_offset);
		if (result > 0) {
			completed += (size_t)result;
			continue;
		}
		if (result == 0) {
			(void)memset(buffer + completed, 0, MINIMM_PAGE_SIZE - completed);
			return MINIMM_OK;
		}
		if (errno != EINTR) {
			return MINIMM_ERROR_IO;
		}
	}

	return MINIMM_OK;
}

static minimm_status_t minimm_write_page(int fd, uint64_t offset, const unsigned char *buffer)
{
	size_t completed = 0U;

	if (offset > (uint64_t)INT64_MAX) {
		return MINIMM_ERROR_IO;
	}

	while (completed < MINIMM_PAGE_SIZE) {
		ssize_t result = 0;
		const uint64_t current_offset = offset + completed;

		if (current_offset > (uint64_t)INT64_MAX) {
			return MINIMM_ERROR_IO;
		}

		result = pwrite(fd, buffer + completed, MINIMM_PAGE_SIZE - completed,
				(off_t)current_offset);
		if (result > 0) {
			completed += (size_t)result;
			continue;
		}
		if (result < 0 && errno == EINTR) {
			continue;
		}
		return MINIMM_ERROR_IO;
	}

	return MINIMM_OK;
}

static int minimm_duplicate_fd(int fd)
{
#ifdef F_DUPFD_CLOEXEC
	int duplicate = -1;

	do {
		duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	} while (duplicate < 0 && errno == EINTR);
	return duplicate;
#else
	int duplicate = -1;

	do {
		duplicate = dup(fd);
	} while (duplicate < 0 && errno == EINTR);

	if (duplicate >= 0 && minimm_set_cloexec(duplicate) < 0) {
		const int error_number = errno;

		(void)close(duplicate);
		errno = error_number;
		return -1;
	}
	return duplicate;
#endif
}

static void minimm_file_backing_latch_error(minimm_file_backing_t *backing, minimm_status_t status)
{
	int expected = (int)MINIMM_OK;

	if (backing == NULL || status == MINIMM_OK) {
		return;
	}
	(void)atomic_compare_exchange_strong_explicit(&backing->writeback_error, &expected,
						      (int)status, memory_order_release,
						      memory_order_relaxed);
}

minimm_status_t minimm_file_backing_create(int fd, minimm_file_backing_t **out_backing)
{
	minimm_file_backing_t *backing = NULL;
	int duplicate = -1;

	if (fd < 0 || out_backing == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_backing = NULL;
	duplicate = minimm_duplicate_fd(fd);
	if (duplicate < 0) {
		return minimm_frame_status_from_errno(errno);
	}
	backing = calloc(1U, sizeof(*backing));
	if (backing == NULL) {
		(void)close(duplicate);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	atomic_init(&backing->references, 1U);
	atomic_init(&backing->writeback_error, (int)MINIMM_OK);
	backing->fd = duplicate;
	*out_backing = backing;
	return MINIMM_OK;
}

void minimm_file_backing_retain(minimm_file_backing_t *backing)
{
	size_t references = 0U;

	if (backing == NULL) {
		return;
	}
	references = atomic_load_explicit(&backing->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&backing->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

void minimm_file_backing_release(minimm_file_backing_t *backing)
{
	if (backing == NULL ||
	    atomic_fetch_sub_explicit(&backing->references, 1U, memory_order_acq_rel) != 1U) {
		return;
	}
	(void)close(backing->fd);
	free(backing);
}

minimm_status_t minimm_file_backing_resize(minimm_file_backing_t *backing, uint64_t size)
{
	minimm_status_t status = MINIMM_OK;

	if (backing == NULL || size > (uint64_t)INT64_MAX) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = (minimm_status_t)atomic_load_explicit(&backing->writeback_error,
						       memory_order_acquire);
	if (status != MINIMM_OK) {
		return status;
	}
	while (ftruncate(backing->fd, (off_t)size) != 0) {
		if (errno != EINTR) {
			status = minimm_frame_status_from_errno(errno);
			minimm_file_backing_latch_error(backing, status);
			return status;
		}
	}
	return MINIMM_OK;
}

minimm_status_t minimm_file_backing_sync(minimm_file_backing_t *backing)
{
	minimm_status_t status = MINIMM_OK;

	if (backing == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = (minimm_status_t)atomic_load_explicit(&backing->writeback_error,
						       memory_order_acquire);
	if (status != MINIMM_OK) {
		return status;
	}
	while (fsync(backing->fd) != 0) {
		if (errno != EINTR) {
			status = minimm_frame_status_from_errno(errno);
			minimm_file_backing_latch_error(backing, status);
			return status;
		}
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_frame_acquire_swap_slot_locked(minimm_frame_t *frame)
{
	minimm_frame_store_t *store = frame->store;
	minimm_swap_slot_t *slot = NULL;

	if (frame->swap_slot != NULL) {
		return MINIMM_OK;
	}
	if (store->free_swap_slots != NULL) {
		slot = store->free_swap_slots;
		store->free_swap_slots = slot->next;
	} else {
		if (store->next_swap_offset > (uint64_t)INT64_MAX - (uint64_t)MINIMM_PAGE_SIZE ||
		    store->swap_slot_high_water == SIZE_MAX) {
			return MINIMM_ERROR_NO_SPACE;
		}
		slot = malloc(sizeof(*slot));
		if (slot == NULL) {
			return MINIMM_ERROR_OUT_OF_MEMORY;
		}
		slot->offset = store->next_swap_offset;
		store->next_swap_offset += MINIMM_PAGE_SIZE;
		store->swap_slot_high_water += 1U;
	}
	slot->next = NULL;
	frame->swap_slot = slot;
	store->swap_slot_count += 1U;
	return MINIMM_OK;
}

static void minimm_frame_release_swap_slot_locked(minimm_frame_t *frame)
{
	minimm_frame_store_t *store = frame->store;

	if (frame->swap_slot == NULL) {
		return;
	}
	if (store->swap_slot_count == 0U) {
		abort();
	}
	frame->swap_slot->next = store->free_swap_slots;
	store->free_swap_slots = frame->swap_slot;
	frame->swap_slot = NULL;
	store->swap_slot_count -= 1U;
}

static minimm_status_t minimm_frame_write_back_locked(minimm_frame_t *frame)
{
	minimm_frame_store_t *store = frame->store;
	minimm_status_t status = MINIMM_OK;

	if (!frame->dirty || frame->data == NULL) {
		return MINIMM_OK;
	}

	if (frame->source == MINIMM_FRAME_SOURCE_FILE && frame->file_shared) {
		status = (minimm_status_t)atomic_load_explicit(
			&frame->file_backing->writeback_error, memory_order_acquire);
		if (status == MINIMM_OK) {
			status = minimm_write_page(frame->file_backing->fd, frame->file_offset,
						   frame->data);
			minimm_file_backing_latch_error(frame->file_backing, status);
		}
	} else {
		status = minimm_frame_acquire_swap_slot_locked(frame);
		if (status != MINIMM_OK) {
			return status;
		}
		status = minimm_write_page(store->swap_fd, frame->swap_slot->offset, frame->data);
	}

	if (status == MINIMM_OK) {
		frame->dirty = false;
	}
	return status;
}

static unsigned char *minimm_frame_acquire_resident_slot_locked(minimm_frame_store_t *store,
								size_t *out_slot)
{
	size_t slot = 0U;

	if (store->free_resident_slot_count == 0U) {
		return NULL;
	}
	store->free_resident_slot_count -= 1U;
	slot = store->free_resident_slots[store->free_resident_slot_count];
	*out_slot = slot;
	return store->resident_arena + (slot * (size_t)MINIMM_PAGE_SIZE);
}

static void minimm_frame_release_resident_slot_locked(minimm_frame_t *frame)
{
	minimm_frame_store_t *store = frame->store;

	if (frame->data == NULL || frame->resident_slot >= store->resident_limit ||
	    store->free_resident_slot_count >= store->resident_limit ||
	    store->resident_count == 0U) {
		abort();
	}
	store->free_resident_slots[store->free_resident_slot_count] = frame->resident_slot;
	store->free_resident_slot_count += 1U;
	store->resident_count -= 1U;
	frame->resident_slot = MINIMM_FRAME_NO_RESIDENT_SLOT;
	frame->data = NULL;
}

static minimm_status_t minimm_frame_page_out_locked(minimm_frame_t *frame)
{
	minimm_status_t status = MINIMM_OK;

	if (frame->data == NULL) {
		frame->cold = false;
		return MINIMM_OK;
	}
	if (frame->pin_count != 0U || frame->transient_pin_count != 0U) {
		return MINIMM_ERROR_BUSY;
	}

	status = minimm_frame_write_back_locked(frame);
	if (status != MINIMM_OK) {
		return status;
	}

	minimm_frame_release_resident_slot_locked(frame);
	frame->cold = false;
	minimm_frame_counter_increment_saturating(&frame->store->page_out_count);
	return MINIMM_OK;
}

static bool minimm_frame_precedes_candidate(const minimm_frame_t *frame,
					    const minimm_frame_t *candidate)
{
	if (candidate == NULL) {
		return true;
	}
	if (frame->cold != candidate->cold) {
		return frame->cold;
	}
	if (frame->last_access != candidate->last_access) {
		return frame->last_access < candidate->last_access;
	}
	return frame->id < candidate->id;
}

static minimm_frame_t *minimm_frame_select_victim_locked(minimm_frame_store_t *store,
							 const minimm_frame_t *excluded,
							 size_t *out_scanned)
{
	minimm_frame_t *candidate = NULL;
	minimm_frame_t *current = store->frames;

	if (out_scanned != NULL) {
		*out_scanned = 0U;
	}
	while (current != NULL) {
		if (current != excluded && current->data != NULL) {
			minimm_frame_counter_increment_saturating(&store->reclaim_scan_count);
			if (out_scanned != NULL && *out_scanned != SIZE_MAX) {
				*out_scanned += 1U;
			}
			if (current->pin_count == 0U && current->transient_pin_count == 0U &&
			    minimm_frame_precedes_candidate(current, candidate)) {
				candidate = current;
			}
		}
		current = current->next;
	}

	return candidate;
}

static minimm_status_t minimm_frame_reclaim_page_locked(minimm_frame_t *frame)
{
	const bool was_resident = frame->data != NULL;
	const minimm_status_t status = minimm_frame_page_out_locked(frame);

	if (status == MINIMM_OK && was_resident) {
		frame->cold = false;
		frame->reclaim_pending = true;
		minimm_frame_counter_increment_saturating(&frame->store->reclaim_count);
	}
	return status;
}

static bool minimm_frame_has_transient_victim_locked(const minimm_frame_store_t *store,
						     const minimm_frame_t *excluded)
{
	const minimm_frame_t *current = store->frames;

	while (current != NULL) {
		if (current != excluded && current->data != NULL && current->pin_count == 0U &&
		    current->transient_pin_count != 0U) {
			return true;
		}
		current = current->next;
	}
	return false;
}

static minimm_status_t minimm_frame_make_resident_locked(minimm_frame_t *frame, bool *out_paged_in)
{
	minimm_frame_store_t *store = frame->store;
	unsigned char data[MINIMM_PAGE_SIZE];

	if (out_paged_in != NULL) {
		*out_paged_in = false;
	}
	for (;;) {
		unsigned char *resident_data = NULL;
		size_t resident_slot = MINIMM_FRAME_NO_RESIDENT_SLOT;
		minimm_status_t status = MINIMM_OK;

		if (frame->data != NULL) {
			frame->cold = false;
			store->clock += 1U;
			frame->last_access = store->clock;
			return MINIMM_OK;
		}

		/* A condition wait can make an earlier backing read stale. */
		if (frame->swap_slot != NULL) {
			status = minimm_read_page(store->swap_fd, frame->swap_slot->offset, data);
		} else if (frame->source == MINIMM_FRAME_SOURCE_FILE) {
			status =
				minimm_read_page(frame->file_backing->fd, frame->file_offset, data);
		} else {
			(void)memset(data, 0, MINIMM_PAGE_SIZE);
		}
		if (status != MINIMM_OK) {
			return status;
		}

		if (store->resident_count >= store->resident_limit) {
			minimm_frame_t *victim =
				minimm_frame_select_victim_locked(store, frame, NULL);

			if (victim == NULL) {
				int wait_status = 0;

				if (!minimm_frame_has_transient_victim_locked(store, frame)) {
					return MINIMM_ERROR_BUSY;
				}
				if (store->transient_waiter_count == SIZE_MAX) {
					abort();
				}
				store->transient_waiter_count += 1U;
				wait_status =
					pthread_cond_wait(&store->transient_unpinned, &store->lock);
				store->transient_waiter_count -= 1U;
				if (wait_status != 0) {
					return MINIMM_ERROR_BUSY;
				}
				continue;
			}
			status = minimm_frame_reclaim_page_locked(victim);
			if (status != MINIMM_OK) {
				return status;
			}
		}

		resident_data = minimm_frame_acquire_resident_slot_locked(store, &resident_slot);
		if (resident_data == NULL) {
			return MINIMM_ERROR_BUSY;
		}
		(void)memcpy(resident_data, data, MINIMM_PAGE_SIZE);

		store->clock += 1U;
		frame->last_access = store->clock;
		frame->data = resident_data;
		frame->resident_slot = resident_slot;
		frame->cold = false;
		store->resident_count += 1U;
		minimm_frame_counter_increment_saturating(&store->page_in_count);
		if (frame->reclaim_pending) {
			frame->reclaim_pending = false;
			minimm_frame_counter_increment_saturating(&store->refault_count);
		}
		if (out_paged_in != NULL) {
			*out_paged_in = true;
		}
		return MINIMM_OK;
	}
}

static minimm_status_t minimm_frame_create(minimm_frame_store_t *store,
					   minimm_frame_source_t source,
					   minimm_file_backing_t *file_backing,
					   uint64_t file_offset, bool shared,
					   minimm_frame_t **out_frame)
{
	minimm_frame_t *frame = NULL;

	if (store == NULL || out_frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;

	frame = calloc(1U, sizeof(*frame));
	if (frame == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	frame->store = store;
	frame->source = source;
	frame->file_offset = file_offset;
	frame->file_shared = shared;
	frame->resident_slot = MINIMM_FRAME_NO_RESIDENT_SLOT;
	atomic_init(&frame->references, 1U);
	atomic_init(&frame->mappings, 0U);

	if (source == MINIMM_FRAME_SOURCE_FILE) {
		if (file_backing == NULL) {
			free(frame);
			return MINIMM_ERROR_INVALID_ARGUMENT;
		}
		minimm_file_backing_retain(file_backing);
		frame->file_backing = file_backing;
	}

	(void)pthread_mutex_lock(&store->lock);
	if (store->destroy_requested) {
		(void)pthread_mutex_unlock(&store->lock);
		minimm_file_backing_release(frame->file_backing);
		free(frame);
		return MINIMM_ERROR_BUSY;
	}
	if (store->next_frame_id == UINT64_MAX) {
		(void)pthread_mutex_unlock(&store->lock);
		minimm_file_backing_release(frame->file_backing);
		free(frame);
		return MINIMM_ERROR_NO_SPACE;
	}
	frame->id = store->next_frame_id;
	store->next_frame_id += 1U;
	frame->next = store->frames;
	store->frames = frame;
	store->frame_count += 1U;
	(void)pthread_mutex_unlock(&store->lock);

	*out_frame = frame;
	return MINIMM_OK;
}

minimm_status_t minimm_frame_store_create(size_t resident_limit_pages,
					  minimm_frame_store_t **out_store)
{
	minimm_frame_store_t *store = NULL;
	char path[] = "/tmp/minimm-pages-XXXXXX";
	size_t arena_size = 0U;
	size_t index = 0U;
	minimm_status_t status = MINIMM_OK;

	if (resident_limit_pages == 0U || out_store == NULL ||
	    resident_limit_pages > SIZE_MAX / (size_t)MINIMM_PAGE_SIZE) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_store = NULL;
	arena_size = resident_limit_pages * (size_t)MINIMM_PAGE_SIZE;
	if (arena_size > (size_t)INT64_MAX) {
		return MINIMM_ERROR_NO_SPACE;
	}

	store = calloc(1U, sizeof(*store));
	if (store == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	store->swap_fd = -1;
	store->resident_limit = resident_limit_pages;
	store->next_frame_id = 1U;
	store->resident_arena_size = arena_size;
	if (pthread_mutex_init(&store->lock, NULL) != 0) {
		free(store);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_cond_init(&store->transient_unpinned, NULL) != 0) {
		(void)pthread_mutex_destroy(&store->lock);
		free(store);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	store->free_resident_slots =
		malloc(resident_limit_pages * sizeof(*store->free_resident_slots));
	if (store->free_resident_slots == NULL) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto fail_lock;
	}

#ifdef O_CLOEXEC
	store->swap_fd = mkostemp(path, O_CLOEXEC);
#else
	store->swap_fd = mkstemp(path);
#endif
	if (store->swap_fd < 0) {
		status = minimm_frame_status_from_errno(errno);
		goto fail_slots;
	}
	if (minimm_unlink_retry(path) != 0) {
		status = minimm_frame_status_from_errno(errno);
		goto fail_fd;
	}
#ifndef O_CLOEXEC
	if (minimm_set_cloexec(store->swap_fd) < 0) {
		status = minimm_frame_status_from_errno(errno);
		goto fail_fd;
	}
#endif
	store->resident_arena = mmap(NULL, arena_size, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, (off_t)0);
	if (store->resident_arena == MAP_FAILED) {
		store->resident_arena = NULL;
		status = minimm_frame_status_from_errno(errno);
		goto fail_fd;
	}
	for (index = 0U; index < resident_limit_pages; ++index) {
		store->free_resident_slots[index] = resident_limit_pages - index - 1U;
	}
	store->free_resident_slot_count = resident_limit_pages;

	*out_store = store;
	return MINIMM_OK;

fail_fd:
	(void)close(store->swap_fd);
fail_slots:
	free(store->free_resident_slots);
fail_lock:
	(void)pthread_cond_destroy(&store->transient_unpinned);
	(void)pthread_mutex_destroy(&store->lock);
	free(store);
	return status;
}

void minimm_frame_store_destroy(minimm_frame_store_t *store)
{
	bool can_free = false;

	if (store == NULL) {
		return;
	}

	(void)pthread_mutex_lock(&store->lock);
	if (!store->destroy_requested) {
		store->destroy_requested = true;
		can_free = store->frame_count == 0U;
	}
	(void)pthread_mutex_unlock(&store->lock);

	if (can_free) {
		minimm_frame_store_free(store);
	}
}

minimm_status_t minimm_frame_create_zero(minimm_frame_store_t *store, minimm_frame_t **out_frame)
{
	return minimm_frame_create(store, MINIMM_FRAME_SOURCE_ZERO, NULL, 0U, false, out_frame);
}

minimm_status_t minimm_frame_create_file_backing(minimm_frame_store_t *store,
						 minimm_file_backing_t *backing,
						 uint64_t file_offset, bool shared,
						 minimm_frame_t **out_frame)
{
	if (backing == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_frame_create(store, MINIMM_FRAME_SOURCE_FILE, backing, file_offset, shared,
				   out_frame);
}

minimm_status_t minimm_frame_create_file(minimm_frame_store_t *store, int fd, uint64_t file_offset,
					 bool shared, minimm_frame_t **out_frame)
{
	minimm_file_backing_t *backing = NULL;
	minimm_status_t status = MINIMM_OK;

	if (fd < 0) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = minimm_file_backing_create(fd, &backing);
	if (status == MINIMM_OK) {
		status = minimm_frame_create_file_backing(store, backing, file_offset, shared,
							  out_frame);
	}
	minimm_file_backing_release(backing);
	return status;
}

minimm_status_t minimm_frame_copy(minimm_frame_t *source, minimm_frame_t **out_frame)
{
	unsigned char data[MINIMM_PAGE_SIZE];
	minimm_frame_t *copy = NULL;
	minimm_status_t status = MINIMM_OK;

	if (source == NULL || out_frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;

	status = minimm_frame_read(source, 0U, data, sizeof(data));
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_frame_create_zero(source->store, &copy);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_frame_write(copy, 0U, data, sizeof(data));
	if (status != MINIMM_OK) {
		minimm_frame_release(copy);
		return status;
	}

	*out_frame = copy;
	return MINIMM_OK;
}

minimm_status_t minimm_frame_copy_file(minimm_frame_t *source, minimm_file_backing_t *backing,
				       uint64_t file_offset, minimm_frame_t **out_frame)
{
	unsigned char data[MINIMM_PAGE_SIZE];
	minimm_frame_t *copy = NULL;
	minimm_status_t status = MINIMM_OK;

	if (source == NULL || backing == NULL || out_frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;
	status = minimm_frame_read(source, 0U, data, sizeof(data));
	if (status == MINIMM_OK) {
		status = minimm_frame_create_file_backing(source->store, backing, file_offset, true,
							  out_frame);
	}
	copy = *out_frame;
	if (status == MINIMM_OK) {
		status = minimm_frame_write(copy, 0U, data, sizeof(data));
	}
	if (status != MINIMM_OK) {
		minimm_frame_release(copy);
		*out_frame = NULL;
	}
	return status;
}

void minimm_frame_retain(minimm_frame_t *frame)
{
	size_t references = 0U;

	if (frame == NULL) {
		return;
	}
	references = atomic_load_explicit(&frame->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&frame->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

void minimm_frame_release(minimm_frame_t *frame)
{
	minimm_frame_store_t *store = NULL;
	minimm_frame_t **link = NULL;
	bool free_store = false;

	if (frame == NULL ||
	    atomic_fetch_sub_explicit(&frame->references, 1U, memory_order_acq_rel) != 1U) {
		return;
	}

	store = frame->store;
	(void)pthread_mutex_lock(&store->lock);
	if (frame->source == MINIMM_FRAME_SOURCE_FILE && frame->file_shared && frame->dirty) {
		(void)minimm_frame_write_back_locked(frame);
	}
	link = &store->frames;
	while (*link != NULL && *link != frame) {
		link = &(*link)->next;
	}
	if (*link == frame) {
		*link = frame->next;
		store->frame_count -= 1U;
		if (frame->data != NULL) {
			minimm_frame_release_resident_slot_locked(frame);
		}
		minimm_frame_release_swap_slot_locked(frame);
		free_store = store->destroy_requested && store->frame_count == 0U;
	}
	(void)pthread_mutex_unlock(&store->lock);

	minimm_file_backing_release(frame->file_backing);
	free(frame);
	if (free_store) {
		minimm_frame_store_free(store);
	}
}

void minimm_frame_map(minimm_frame_t *frame)
{
	size_t mappings = 0U;

	if (frame == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	mappings = atomic_load_explicit(&frame->mappings, memory_order_relaxed);
	if (mappings == SIZE_MAX) {
		(void)pthread_mutex_unlock(&frame->store->lock);
		abort();
	}
	atomic_store_explicit(&frame->mappings, mappings + 1U, memory_order_relaxed);
	(void)pthread_mutex_unlock(&frame->store->lock);
}

void minimm_frame_unmap(minimm_frame_t *frame)
{
	size_t previous = 0U;

	if (frame == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	previous = atomic_load_explicit(&frame->mappings, memory_order_relaxed);
	if (previous == 0U) {
		(void)pthread_mutex_unlock(&frame->store->lock);
		abort();
	}
	atomic_store_explicit(&frame->mappings, previous - 1U, memory_order_relaxed);
	(void)pthread_mutex_unlock(&frame->store->lock);
}

size_t minimm_frame_mapping_count(const minimm_frame_t *frame)
{
	return frame == NULL ? 0U : atomic_load_explicit(&frame->mappings, memory_order_relaxed);
}

minimm_status_t minimm_frame_read(minimm_frame_t *frame, size_t offset, void *buffer, size_t length)
{
	minimm_status_t status = MINIMM_OK;

	if (frame == NULL || (buffer == NULL && length != 0U) || offset > MINIMM_PAGE_SIZE ||
	    length > MINIMM_PAGE_SIZE - offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (length == 0U) {
		return MINIMM_OK;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	status = minimm_frame_make_resident_locked(frame, NULL);
	if (status == MINIMM_OK && length != 0U) {
		(void)memcpy(buffer, frame->data + offset, length);
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
	return status;
}

minimm_status_t minimm_frame_write(minimm_frame_t *frame, size_t offset, const void *buffer,
				   size_t length)
{
	minimm_status_t status = MINIMM_OK;

	if (frame == NULL || (buffer == NULL && length != 0U) || offset > MINIMM_PAGE_SIZE ||
	    length > MINIMM_PAGE_SIZE - offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (length == 0U) {
		return MINIMM_OK;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	status = minimm_frame_make_resident_locked(frame, NULL);
	if (status == MINIMM_OK && length != 0U) {
		(void)memcpy(frame->data + offset, buffer, length);
		frame->dirty = true;
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
	return status;
}

minimm_status_t minimm_frame_sync(minimm_frame_t *frame)
{
	minimm_status_t status = MINIMM_OK;

	if (frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->source == MINIMM_FRAME_SOURCE_FILE && frame->file_shared) {
		status = minimm_frame_write_back_locked(frame);
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
	return status;
}

minimm_status_t minimm_frame_page_out(minimm_frame_t *frame)
{
	minimm_status_t status = MINIMM_OK;

	if (frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	status = minimm_frame_page_out_locked(frame);
	(void)pthread_mutex_unlock(&frame->store->lock);
	return status;
}

minimm_status_t minimm_frame_ensure_resident(minimm_frame_t *frame, bool *out_paged_in)
{
	minimm_status_t status = MINIMM_OK;

	if (frame == NULL || out_paged_in == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_paged_in = false;

	(void)pthread_mutex_lock(&frame->store->lock);
	status = minimm_frame_make_resident_locked(frame, out_paged_in);
	(void)pthread_mutex_unlock(&frame->store->lock);
	return status;
}

void minimm_frame_mark_cold(minimm_frame_t *frame)
{
	if (frame == NULL) {
		return;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->data != NULL) {
		frame->cold = true;
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
}

minimm_status_t minimm_frame_store_reclaim(minimm_frame_store_t *store, size_t target_pages,
					   minimm_reclaim_result_t *out_result)
{
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	out_result->scanned_count = 0U;
	out_result->reclaimed_count = 0U;
	if (store == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&store->lock);
	if (store->destroy_requested) {
		status = MINIMM_ERROR_BUSY;
		goto unlock;
	}
	while (out_result->reclaimed_count < target_pages) {
		minimm_frame_t *victim = NULL;
		size_t scanned = 0U;

		victim = minimm_frame_select_victim_locked(store, NULL, &scanned);
		minimm_frame_size_add_saturating(&out_result->scanned_count, scanned);
		if (victim == NULL) {
			break;
		}
		status = minimm_frame_reclaim_page_locked(victim);
		if (status != MINIMM_OK) {
			break;
		}
		if (out_result->reclaimed_count != SIZE_MAX) {
			out_result->reclaimed_count += 1U;
		}
	}

unlock:
	(void)pthread_mutex_unlock(&store->lock);
	return status;
}

void minimm_frame_pin(minimm_frame_t *frame)
{
	if (frame == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->pin_count == SIZE_MAX) {
		(void)pthread_mutex_unlock(&frame->store->lock);
		abort();
	}
	frame->pin_count += 1U;
	(void)pthread_mutex_unlock(&frame->store->lock);
}

void minimm_frame_unpin(minimm_frame_t *frame)
{
	if (frame == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->pin_count != 0U) {
		frame->pin_count -= 1U;
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
}

bool minimm_frame_try_pin_resident(minimm_frame_t *frame)
{
	bool pinned = false;

	if (frame == NULL) {
		return false;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->data != NULL) {
		if (frame->transient_pin_count == SIZE_MAX) {
			(void)pthread_mutex_unlock(&frame->store->lock);
			abort();
		}
		frame->transient_pin_count += 1U;
		pinned = true;
	}
	(void)pthread_mutex_unlock(&frame->store->lock);
	return pinned;
}

void minimm_frame_unpin_resident(minimm_frame_t *frame)
{
	if (frame == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	if (frame->transient_pin_count == 0U) {
		(void)pthread_mutex_unlock(&frame->store->lock);
		abort();
	}
	frame->transient_pin_count -= 1U;
	(void)pthread_cond_broadcast(&frame->store->transient_unpinned);
	(void)pthread_mutex_unlock(&frame->store->lock);
}

uint64_t minimm_frame_id(const minimm_frame_t *frame)
{
	return frame == NULL ? 0U : frame->id;
}

size_t minimm_frame_reference_count(const minimm_frame_t *frame)
{
	if (frame == NULL) {
		return 0U;
	}
	return atomic_load_explicit(&frame->references, memory_order_relaxed);
}

bool minimm_frame_is_resident(const minimm_frame_t *frame)
{
	bool resident = false;

	if (frame == NULL) {
		return false;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	resident = frame->data != NULL;
	(void)pthread_mutex_unlock(&frame->store->lock);
	return resident;
}

bool minimm_frame_is_dirty(const minimm_frame_t *frame)
{
	bool dirty = false;

	if (frame == NULL) {
		return false;
	}
	(void)pthread_mutex_lock(&frame->store->lock);
	dirty = frame->dirty;
	(void)pthread_mutex_unlock(&frame->store->lock);
	return dirty;
}

void minimm_frame_get_state(const minimm_frame_t *frame, minimm_frame_state_t *out_state)
{
	if (frame == NULL || out_state == NULL) {
		return;
	}

	(void)pthread_mutex_lock(&frame->store->lock);
	out_state->resident = frame->data != NULL;
	out_state->dirty = frame->dirty;
	out_state->pinned = frame->pin_count != 0U;
	out_state->cold = frame->cold;
	(void)pthread_mutex_unlock(&frame->store->lock);
}

minimm_status_t minimm_frame_observe_batch(minimm_frame_t *const *frames, size_t frame_count,
					   minimm_frame_observation_t *out_observations)
{
	minimm_frame_store_t *store = NULL;
	size_t index = 0U;

	if (frame_count == 0U) {
		return MINIMM_OK;
	}
	if (frames == NULL || out_observations == NULL || frames[0] == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	store = frames[0]->store;
	for (index = 1U; index < frame_count; ++index) {
		if (frames[index] == NULL || frames[index]->store != store) {
			return MINIMM_ERROR_INVALID_ARGUMENT;
		}
	}

	(void)pthread_mutex_lock(&store->lock);
	for (index = 0U; index < frame_count; ++index) {
		out_observations[index].frame_cookie = frames[index]->id;
		out_observations[index].mapping_count =
			atomic_load_explicit(&frames[index]->mappings, memory_order_relaxed);
		out_observations[index].resident = frames[index]->data != NULL;
		out_observations[index].dirty = frames[index]->dirty;
		out_observations[index].pinned = frames[index]->pin_count != 0U;
		out_observations[index].cold = frames[index]->cold;
	}
	(void)pthread_mutex_unlock(&store->lock);
	return MINIMM_OK;
}

void minimm_frame_store_get_stats(minimm_frame_store_t *store,
				  minimm_frame_store_stats_t *out_stats)
{
	if (store == NULL || out_stats == NULL) {
		return;
	}

	(void)pthread_mutex_lock(&store->lock);
	out_stats->frame_count = store->frame_count;
	out_stats->resident_count = store->resident_count;
	out_stats->resident_limit = store->resident_limit;
	out_stats->page_in_count = store->page_in_count;
	out_stats->page_out_count = store->page_out_count;
	out_stats->reclaim_scan_count = store->reclaim_scan_count;
	out_stats->reclaim_count = store->reclaim_count;
	out_stats->refault_count = store->refault_count;
	out_stats->swap_slot_count = store->swap_slot_count;
	out_stats->swap_slot_high_water = store->swap_slot_high_water;
	out_stats->transient_waiter_count = store->transient_waiter_count;
	(void)pthread_mutex_unlock(&store->lock);
}
