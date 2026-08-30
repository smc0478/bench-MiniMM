#include "minimm/minimm.h"

#include "frame.h"
#include "page_table.h"
#include "space.h"
#include "tlb.h"
#include "vma_tree.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct minimm_space_snapshot {
	minimm_mapping_info_t *mappings;
	minimm_space_snapshot_page_t *pages;
	minimm_space_stats_t stats;
	size_t mapping_count;
	size_t page_count;
	uint64_t vma_generation;
	uint64_t page_table_generation;
};

typedef struct minimm_snapshot_page_walk {
	minimm_space_snapshot_t *snapshot;
	const minimm_vma_snapshot_t *vma_snapshot;
	minimm_frame_t **frames;
	size_t count;
} minimm_snapshot_page_walk_t;

static minimm_status_t minimm_snapshot_allocate(size_t count, size_t element_size,
						void **out_allocation)
{
	*out_allocation = NULL;
	if (count == 0U) {
		return MINIMM_OK;
	}
	if (element_size == 0U || count > SIZE_MAX / element_size) {
		return MINIMM_ERROR_NO_SPACE;
	}
	*out_allocation = calloc(count, element_size);
	return *out_allocation == NULL ? MINIMM_ERROR_OUT_OF_MEMORY : MINIMM_OK;
}

static minimm_status_t minimm_snapshot_copy_mappings(minimm_space_snapshot_t *snapshot,
						     const minimm_vma_snapshot_t *vma_snapshot)
{
	minimm_vaddr_t next_address = 0U;
	size_t index = 0U;

	for (index = 0U; index < snapshot->mapping_count; ++index) {
		const minimm_vma_t *mapping =
			minimm_vma_snapshot_find_next(vma_snapshot, next_address);
		minimm_mapping_info_t *destination = &snapshot->mappings[index];

		if (mapping == NULL || mapping->end <= next_address) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		destination->start = mapping->start;
		destination->end = mapping->end;
		destination->mapping_cookie = mapping->mapping_cookie;
		destination->note_offset = mapping->note_offset;
		destination->protection = mapping->prot;
		destination->maximum_protection = mapping->max_prot;
		destination->flags = mapping->flags;
		next_address = mapping->end;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_snapshot_copy_pte(minimm_vaddr_t page_address,
						const minimm_pte_t *pte, void *opaque)
{
	minimm_snapshot_page_walk_t *walk = opaque;
	minimm_space_snapshot_page_t *destination = NULL;
	const minimm_vma_t *mapping = NULL;

	if (walk->count >= walk->snapshot->page_count) {
		return MINIMM_ERROR_NO_SPACE;
	}
	mapping = minimm_vma_snapshot_lookup(walk->vma_snapshot, page_address);
	if (mapping == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	destination = &walk->snapshot->pages[walk->count];
	destination->page.page_address = page_address;
	destination->page.pfn = MINIMM_PFN_NONE;
	destination->page.protection = pte->protection;
	destination->page.present = (pte->flags & MINIMM_PTE_PRESENT) != 0U;
	destination->page.dirty = (pte->flags & MINIMM_PTE_DIRTY) != 0U;
	destination->page.accessed = (pte->flags & MINIMM_PTE_ACCESSED) != 0U;
	destination->page.cow = (pte->flags & MINIMM_PTE_COW) != 0U;
	destination->page.shared = (pte->flags & MINIMM_PTE_SHARED) != 0U;
	destination->page.locked = (pte->flags & MINIMM_PTE_LOCKED) != 0U;
	destination->mapping_cookie = mapping->mapping_cookie;
	walk->frames[walk->count] = pte->frame;
	walk->count += 1U;
	return MINIMM_OK;
}

static void minimm_snapshot_complete_pages(minimm_space_snapshot_t *snapshot,
					   const minimm_frame_observation_t *observations)
{
	size_t index = 0U;

	for (index = 0U; index < snapshot->page_count; ++index) {
		minimm_space_snapshot_page_t *entry = &snapshot->pages[index];
		const minimm_frame_observation_t *observation = &observations[index];

		entry->frame_cookie = observation->frame_cookie;
		entry->frame_mapping_count = observation->mapping_count;
		entry->page.resident = observation->resident;
		entry->page.present = entry->page.present && observation->resident;
		entry->page.dirty = entry->page.dirty || observation->dirty;
		entry->page.locked = entry->page.locked || observation->pinned;
		entry->page.cold = observation->cold;
		if (entry->page.present) {
			entry->page.pfn = observation->frame_cookie;
		}

		if (entry->page.present) {
			snapshot->stats.present_count += 1U;
		}
		if (entry->page.resident) {
			snapshot->stats.resident_count += 1U;
		}
		if (entry->page.dirty) {
			snapshot->stats.dirty_count += 1U;
		}
		if (entry->page.cow) {
			snapshot->stats.cow_count += 1U;
		}
		if (entry->page.shared) {
			snapshot->stats.shared_count += 1U;
		}
		if (entry->page.locked) {
			snapshot->stats.locked_count += 1U;
		}
	}
}

static void minimm_snapshot_capture_stats_locked(minimm_space_t *space,
						 minimm_space_snapshot_t *snapshot)
{
	minimm_tlb_stats_t tlb_stats = { 0 };

	snapshot->stats.vma_count = snapshot->mapping_count;
	snapshot->stats.pte_count = snapshot->page_count;
	snapshot->stats.fault_sequence = space->fault_sequence;
	minimm_tlb_get_stats(space->tlb, &tlb_stats);
	snapshot->stats.tlb_hits = tlb_stats.hits;
	snapshot->stats.tlb_misses = tlb_stats.misses;
	snapshot->stats.tlb_replacements = tlb_stats.replacements;
	snapshot->stats.tlb_invalidations = tlb_stats.invalidations;
}

minimm_status_t minimm_space_snapshot_capture(minimm_space_t *space,
					      minimm_space_snapshot_t **out_snapshot)
{
	minimm_space_snapshot_t *snapshot = NULL;
	minimm_vma_snapshot_t *vma_snapshot = NULL;
	minimm_snapshot_page_walk_t walk = { 0 };
	minimm_frame_t **frames = NULL;
	minimm_frame_observation_t *observations = NULL;
	void *allocation = NULL;
	minimm_status_t status = MINIMM_OK;

	if (out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_snapshot = NULL;
	if (space == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	snapshot = calloc(1U, sizeof(*snapshot));
	if (snapshot == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto unlock;
	}
	vma_snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	snapshot->mapping_count = minimm_vma_snapshot_count(vma_snapshot);
	snapshot->page_count = minimm_page_table_mapping_count(space->page_table);
	snapshot->vma_generation = minimm_vma_snapshot_generation(vma_snapshot);
	snapshot->page_table_generation = minimm_page_table_generation(space->page_table);

	status = minimm_snapshot_allocate(snapshot->mapping_count, sizeof(*snapshot->mappings),
					  &allocation);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	snapshot->mappings = allocation;
	status = minimm_snapshot_allocate(snapshot->page_count, sizeof(*snapshot->pages),
					  &allocation);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	snapshot->pages = allocation;
	status = minimm_snapshot_allocate(snapshot->page_count, sizeof(*frames), &allocation);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	frames = allocation;
	status = minimm_snapshot_allocate(snapshot->page_count, sizeof(*observations), &allocation);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	observations = allocation;

	status = minimm_snapshot_copy_mappings(snapshot, vma_snapshot);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	walk.snapshot = snapshot;
	walk.vma_snapshot = vma_snapshot;
	walk.frames = frames;
	status = minimm_page_table_for_each(space->page_table, minimm_snapshot_copy_pte, &walk);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	if (walk.count != snapshot->page_count) {
		status = MINIMM_ERROR_NOT_FOUND;
		goto unlock;
	}

	status = minimm_frame_observe_batch(frames, snapshot->page_count, observations);
	if (status != MINIMM_OK) {
		goto unlock;
	}
	minimm_snapshot_complete_pages(snapshot, observations);
	minimm_snapshot_capture_stats_locked(space, snapshot);

unlock:
	(void)pthread_mutex_unlock(&space->lock);
	free(observations);
	free(frames);
	if (status != MINIMM_OK) {
		minimm_space_snapshot_destroy(snapshot);
		return status;
	}
	*out_snapshot = snapshot;
	return MINIMM_OK;
}

void minimm_space_snapshot_destroy(minimm_space_snapshot_t *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	free(snapshot->pages);
	free(snapshot->mappings);
	free(snapshot);
}

size_t minimm_space_snapshot_mapping_count(const minimm_space_snapshot_t *snapshot)
{
	return snapshot == NULL ? 0U : snapshot->mapping_count;
}

size_t minimm_space_snapshot_page_count(const minimm_space_snapshot_t *snapshot)
{
	return snapshot == NULL ? 0U : snapshot->page_count;
}

uint64_t minimm_space_snapshot_vma_generation(const minimm_space_snapshot_t *snapshot)
{
	return snapshot == NULL ? UINT64_C(0) : snapshot->vma_generation;
}

uint64_t minimm_space_snapshot_page_table_generation(const minimm_space_snapshot_t *snapshot)
{
	return snapshot == NULL ? UINT64_C(0) : snapshot->page_table_generation;
}

minimm_status_t minimm_space_snapshot_get_stats(const minimm_space_snapshot_t *snapshot,
						minimm_space_stats_t *out_stats)
{
	if (out_stats == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_stats, 0, sizeof(*out_stats));
	if (snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_stats = snapshot->stats;
	return MINIMM_OK;
}

minimm_status_t minimm_space_snapshot_get_mapping(const minimm_space_snapshot_t *snapshot,
						  size_t index, minimm_mapping_info_t *out_mapping)
{
	if (out_mapping == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_mapping, 0, sizeof(*out_mapping));
	if (snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (index >= snapshot->mapping_count) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	*out_mapping = snapshot->mappings[index];
	return MINIMM_OK;
}

minimm_status_t minimm_space_snapshot_get_page(const minimm_space_snapshot_t *snapshot,
					       size_t index, minimm_space_snapshot_page_t *out_page)
{
	if (out_page == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_page, 0, sizeof(*out_page));
	out_page->page.pfn = MINIMM_PFN_NONE;
	if (snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (index >= snapshot->page_count) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	*out_page = snapshot->pages[index];
	return MINIMM_OK;
}
