#ifndef MINIMM_FRAME_H
#define MINIMM_FRAME_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct minimm_frame_store minimm_frame_store_t;
typedef struct minimm_frame minimm_frame_t;
typedef struct minimm_file_backing minimm_file_backing_t;

typedef struct minimm_frame_store_stats {
	size_t frame_count;
	size_t resident_count;
	size_t resident_limit;
	uint64_t page_in_count;
	uint64_t page_out_count;
	uint64_t reclaim_scan_count;
	uint64_t reclaim_count;
	uint64_t refault_count;
	size_t swap_slot_count;
	size_t swap_slot_high_water;
	size_t transient_waiter_count;
} minimm_frame_store_stats_t;

typedef struct minimm_frame_state {
	bool resident;
	bool dirty;
	bool pinned;
	bool cold;
} minimm_frame_state_t;

typedef struct minimm_frame_observation {
	uint64_t frame_cookie;
	size_t mapping_count;
	bool resident;
	bool dirty;
	bool pinned;
	bool cold;
} minimm_frame_observation_t;

minimm_status_t minimm_frame_store_create(size_t resident_limit_pages,
					  minimm_frame_store_t **out_store);
/* No new store operation may race with destruction of the owner handle. */
void minimm_frame_store_destroy(minimm_frame_store_t *store);

minimm_status_t minimm_frame_create_zero(minimm_frame_store_t *store, minimm_frame_t **out_frame);

/*
 * A file backing owns one duplicated descriptor shared by every cached frame
 * for that logical file. This avoids consuming one host descriptor per page.
 */
minimm_status_t minimm_file_backing_create(int fd, minimm_file_backing_t **out_backing);
void minimm_file_backing_retain(minimm_file_backing_t *backing);
void minimm_file_backing_release(minimm_file_backing_t *backing);
minimm_status_t minimm_file_backing_resize(minimm_file_backing_t *backing, uint64_t size);
minimm_status_t minimm_file_backing_sync(minimm_file_backing_t *backing);

minimm_status_t minimm_frame_create_file_backing(minimm_frame_store_t *store,
						 minimm_file_backing_t *backing,
						 uint64_t file_offset, bool shared,
						 minimm_frame_t **out_frame);
minimm_status_t minimm_frame_create_file(minimm_frame_store_t *store, int fd, uint64_t file_offset,
					 bool shared, minimm_frame_t **out_frame);
minimm_status_t minimm_frame_copy(minimm_frame_t *source, minimm_frame_t **out_frame);
minimm_status_t minimm_frame_copy_file(minimm_frame_t *source, minimm_file_backing_t *backing,
				       uint64_t file_offset, minimm_frame_t **out_frame);

void minimm_frame_retain(minimm_frame_t *frame);
void minimm_frame_release(minimm_frame_t *frame);

/* PTE mapcount is deliberately separate from lifetime/cache references. */
void minimm_frame_map(minimm_frame_t *frame);
void minimm_frame_unmap(minimm_frame_t *frame);
size_t minimm_frame_mapping_count(const minimm_frame_t *frame);

minimm_status_t minimm_frame_read(minimm_frame_t *frame, size_t offset, void *buffer,
				  size_t length);
minimm_status_t minimm_frame_write(minimm_frame_t *frame, size_t offset, const void *buffer,
				   size_t length);
minimm_status_t minimm_frame_sync(minimm_frame_t *frame);
minimm_status_t minimm_frame_page_out(minimm_frame_t *frame);
minimm_status_t minimm_frame_ensure_resident(minimm_frame_t *frame, bool *out_paged_in);
void minimm_frame_mark_cold(minimm_frame_t *frame);

minimm_status_t minimm_frame_store_reclaim(minimm_frame_store_t *store, size_t target_pages,
					   minimm_reclaim_result_t *out_result);

void minimm_frame_pin(minimm_frame_t *frame);
void minimm_frame_unpin(minimm_frame_t *frame);

/*
 * Hold a short-lived eviction barrier around one access. Unlike mlock pins,
 * this pin succeeds only while the frame is already resident and is not
 * exposed through minimm_frame_state_t.
 */
bool minimm_frame_try_pin_resident(minimm_frame_t *frame);
void minimm_frame_unpin_resident(minimm_frame_t *frame);

uint64_t minimm_frame_id(const minimm_frame_t *frame);
size_t minimm_frame_reference_count(const minimm_frame_t *frame);
bool minimm_frame_is_resident(const minimm_frame_t *frame);
bool minimm_frame_is_dirty(const minimm_frame_t *frame);
void minimm_frame_get_state(const minimm_frame_t *frame, minimm_frame_state_t *out_state);

/* Observe frame state under one frame-store lock acquisition. */
minimm_status_t minimm_frame_observe_batch(minimm_frame_t *const *frames, size_t frame_count,
					   minimm_frame_observation_t *out_observations);

void minimm_frame_store_get_stats(minimm_frame_store_t *store,
				  minimm_frame_store_stats_t *out_stats);

#endif
