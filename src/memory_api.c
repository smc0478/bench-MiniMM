#include "memory_api.h"

#include "fault.h"
#include "mapping_backing.h"
#include "note.h"
#include "page_table.h"
#include "space.h"
#include "tlb.h"
#include "vma_tree.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct minimm_memory_range {
	minimm_vaddr_t start;
	minimm_vaddr_t end;
	uint64_t page_count;
} minimm_memory_range_t;

static minimm_status_t minimm_memory_prepare_range(minimm_space_t *space, minimm_vaddr_t address,
						   uint64_t length,
						   minimm_memory_range_t *out_range)
{
	uint64_t aligned_length = UINT64_C(0);

	if (space == NULL || out_range == NULL || length == UINT64_C(0) ||
	    (address & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    length > UINT64_MAX - (MINIMM_PAGE_SIZE - UINT64_C(1))) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	aligned_length = (length + MINIMM_PAGE_SIZE - UINT64_C(1)) &
			 ~(MINIMM_PAGE_SIZE - UINT64_C(1));
	if (address >= MINIMM_USER_ADDRESS_LIMIT ||
	    aligned_length > MINIMM_USER_ADDRESS_LIMIT - address) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	out_range->start = address;
	out_range->end = address + aligned_length;
	out_range->page_count = aligned_length / MINIMM_PAGE_SIZE;
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_prepare_lock_range(minimm_space_t *space,
							minimm_vaddr_t address, uint64_t length,
							minimm_memory_range_t *out_range)
{
	const uint64_t mask = MINIMM_PAGE_SIZE - UINT64_C(1);
	const minimm_vaddr_t start = address & ~mask;
	const uint64_t offset = address - start;
	uint64_t span = UINT64_C(0);
	uint64_t aligned_length = UINT64_C(0);

	if (space == NULL || out_range == NULL || length == UINT64_C(0) ||
	    address >= MINIMM_USER_ADDRESS_LIMIT || length > UINT64_MAX - offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	span = length + offset;
	if (span > UINT64_MAX - mask) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	aligned_length = (span + mask) & ~mask;
	if (aligned_length == UINT64_C(0) || aligned_length > MINIMM_USER_ADDRESS_LIMIT - start) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	out_range->start = start;
	out_range->end = start + aligned_length;
	out_range->page_count = aligned_length / MINIMM_PAGE_SIZE;
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_validate_empty_range(minimm_space_t *space,
							  minimm_vaddr_t address,
							  bool require_alignment)
{
	if (space == NULL || address >= MINIMM_USER_ADDRESS_LIMIT ||
	    (require_alignment && (address & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0))) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return atomic_load_explicit(&space->closing, memory_order_acquire) ? MINIMM_ERROR_BUSY :
									     MINIMM_OK;
}

static minimm_status_t minimm_memory_validate_mapped_locked(minimm_space_t *space,
							    const minimm_memory_range_t *range)
{
	const minimm_vma_snapshot_t *snapshot =
		atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	minimm_vaddr_t cursor = range->start;

	while (cursor < range->end) {
		const minimm_vma_t *mapping = minimm_vma_snapshot_find_next(snapshot, cursor);

		if (mapping == NULL || mapping->start > cursor) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		cursor = mapping->end < range->end ? mapping->end : range->end;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_lock_space(minimm_space_t *space,
						const minimm_memory_range_t *range)
{
	minimm_status_t status = MINIMM_OK;

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
	} else {
		status = minimm_memory_validate_mapped_locked(space, range);
	}
	if (status != MINIMM_OK) {
		(void)pthread_mutex_unlock(&space->lock);
	}
	return status;
}

static minimm_status_t minimm_memory_lock_space_unchecked(minimm_space_t *space)
{
	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	return MINIMM_OK;
}

static void minimm_memory_clear_dirty_locked(minimm_space_t *space, minimm_vaddr_t start,
					     minimm_vaddr_t end)
{
	minimm_vaddr_t cursor = start;
	minimm_vaddr_t page = UINT64_C(0);

	while (minimm_page_table_find_next(space->page_table, cursor, end, &page) == MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL && (pte->flags & MINIMM_PTE_DIRTY) != 0U) {
			(void)minimm_page_table_update_attributes(
				space->page_table, page, pte->protection, 0U, MINIMM_PTE_DIRTY);
			minimm_tlb_invalidate_page(space->tlb, page);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
}

minimm_status_t minimm_msync(minimm_space_t *space, minimm_vaddr_t address, uint64_t length)
{
	minimm_memory_range_t range = { 0 };
	const minimm_vma_snapshot_t *snapshot = NULL;
	minimm_vaddr_t page = address;
	minimm_status_t unmapped_status = MINIMM_OK;
	minimm_status_t status = MINIMM_OK;

	if (length == UINT64_C(0)) {
		return minimm_memory_validate_empty_range(space, address, true);
	}
	status = minimm_memory_prepare_range(space, address, length, &range);

	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_memory_lock_space_unchecked(space);
	if (status != MINIMM_OK) {
		return status;
	}
	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);

	while (page < range.end) {
		const minimm_vma_t *mapping = minimm_vma_snapshot_find_next(snapshot, page);
		minimm_space_binding_t *binding = NULL;
		minimm_vaddr_t chunk_end = range.end;

		if (mapping == NULL) {
			unmapped_status = MINIMM_ERROR_NOT_FOUND;
			break;
		}
		if (mapping->start > page) {
			unmapped_status = MINIMM_ERROR_NOT_FOUND;
			page = mapping->start < range.end ? mapping->start : range.end;
			continue;
		}
		binding = minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
		chunk_end = mapping->end < range.end ? mapping->end : range.end;

		if (binding == NULL) {
			status = MINIMM_ERROR_NOT_FOUND;
			break;
		}
		if (minimm_mapping_backing_kind(binding->backing) == MINIMM_BACKING_NOTE_SHARED) {
			minimm_note_t *note = minimm_mapping_backing_note(binding->backing);
			const uint64_t note_offset = mapping->note_offset + (page - mapping->start);

			status = minimm_note_sync_range(note, note_offset, chunk_end - page);
			if (status != MINIMM_OK) {
				break;
			}
			minimm_memory_clear_dirty_locked(space, page, chunk_end);
		}
		page = chunk_end;
	}

	(void)pthread_mutex_unlock(&space->lock);
	return status == MINIMM_OK ? unmapped_status : status;
}

minimm_status_t minimm_mincore(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       uint8_t *page_flags, size_t page_flags_count)
{
	minimm_memory_range_t range = { 0 };
	const minimm_vma_snapshot_t *snapshot = NULL;
	minimm_vaddr_t page = address;
	size_t index = 0U;
	minimm_status_t status = MINIMM_OK;

	if (length == UINT64_C(0)) {
		return minimm_memory_validate_empty_range(space, address, true);
	}
	status = minimm_memory_prepare_range(space, address, length, &range);

	if (status != MINIMM_OK || page_flags == NULL || range.page_count > (uint64_t)SIZE_MAX ||
	    page_flags_count < (size_t)range.page_count) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = minimm_memory_lock_space(space, &range);
	if (status != MINIMM_OK) {
		return status;
	}
	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);

	while (page < range.end) {
		const minimm_vma_t *mapping = minimm_vma_snapshot_lookup(snapshot, page);
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);
		uint8_t flags = mapping->flags == MINIMM_VMA_FLAG_SHARED ? MINIMM_MINCORE_SHARED :
									   UINT8_C(0);

		if (pte != NULL) {
			minimm_frame_state_t frame_state = { 0 };

			minimm_frame_get_state(pte->frame, &frame_state);
			if (frame_state.resident) {
				flags |= MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_PRESENT;
			}
			if (frame_state.dirty || (pte->flags & MINIMM_PTE_DIRTY) != 0U) {
				flags |= MINIMM_MINCORE_DIRTY;
			}
			if ((pte->flags & MINIMM_PTE_LOCKED) != 0U) {
				flags |= MINIMM_MINCORE_LOCKED;
			}
			if ((pte->flags & MINIMM_PTE_SHARED) != 0U) {
				flags |= MINIMM_MINCORE_SHARED;
			}
			if ((pte->flags & MINIMM_PTE_COW) != 0U) {
				flags |= MINIMM_MINCORE_COW;
			}
			if ((pte->flags & MINIMM_PTE_ACCESSED) != 0U) {
				flags |= MINIMM_MINCORE_ACCESSED;
			}
		} else {
			minimm_space_binding_t *binding =
				minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
			minimm_frame_t *cached_frame = NULL;

			if (binding == NULL) {
				status = MINIMM_ERROR_NOT_FOUND;
				break;
			}
			status = minimm_mapping_backing_peek_frame(
				binding->backing, mapping->note_offset + (page - mapping->start),
				&cached_frame);
			if (status == MINIMM_OK) {
				minimm_frame_state_t frame_state = { 0 };

				minimm_frame_get_state(cached_frame, &frame_state);
				if (frame_state.resident) {
					flags |= MINIMM_MINCORE_RESIDENT;
				}
				if (frame_state.dirty) {
					flags |= MINIMM_MINCORE_DIRTY;
				}
				minimm_frame_release(cached_frame);
			} else if (status == MINIMM_ERROR_NOT_FOUND) {
				status = MINIMM_OK;
			} else {
				break;
			}
		}
		page_flags[index] = flags;
		index += 1U;
		page += MINIMM_PAGE_SIZE;
	}

	(void)pthread_mutex_unlock(&space->lock);
	return status;
}

typedef struct minimm_mlock_page_snapshot {
	minimm_frame_t *frame;
	minimm_prot_t protection;
	minimm_pte_flags_t flags;
	bool present;
} minimm_mlock_page_snapshot_t;

static void minimm_memory_snapshot_mlock_pages_locked(minimm_space_t *space, minimm_vaddr_t start,
						      minimm_mlock_page_snapshot_t *pages,
						      size_t page_count)
{
	size_t index = 0U;

	for (index = 0U; index < page_count; ++index) {
		const minimm_vaddr_t page = start + ((minimm_vaddr_t)index * MINIMM_PAGE_SIZE);
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte == NULL) {
			continue;
		}
		minimm_frame_retain(pte->frame);
		pages[index].frame = pte->frame;
		pages[index].protection = pte->protection;
		pages[index].flags = pte->flags;
		pages[index].present = true;
	}
}

static void minimm_memory_release_mlock_snapshots(minimm_mlock_page_snapshot_t *pages,
						  size_t page_count)
{
	size_t index = 0U;

	for (index = 0U; index < page_count; ++index) {
		minimm_frame_release(pages[index].frame);
		pages[index].frame = NULL;
	}
}

static minimm_status_t
minimm_memory_restore_mlock_pages_locked(minimm_space_t *space, minimm_vaddr_t start,
					 const minimm_mlock_page_snapshot_t *pages,
					 size_t attempted_pages)
{
	const minimm_pte_flags_t restorable_flags = ~(minimm_pte_flags_t)MINIMM_PTE_PRESENT;
	minimm_status_t rollback_status = MINIMM_OK;
	size_t index = 0U;

	for (index = 0U; index < attempted_pages; ++index) {
		const minimm_vaddr_t page = start + ((minimm_vaddr_t)index * MINIMM_PAGE_SIZE);
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);
		minimm_status_t status = MINIMM_OK;

		if (pages[index].present && pte != NULL && pte->frame == pages[index].frame &&
		    pte->protection == pages[index].protection &&
		    pte->flags == pages[index].flags) {
			continue;
		}
		minimm_tlb_invalidate_page(space->tlb, page);
		if (!pages[index].present) {
			if (pte != NULL) {
				status = minimm_page_table_unmap(space->page_table, page);
			}
		} else if (pte == NULL) {
			status = minimm_page_table_map(space->page_table, page, pages[index].frame,
						       pages[index].protection, pages[index].flags);
		} else {
			const minimm_pte_flags_t set_flags = pages[index].flags & restorable_flags;
			const minimm_pte_flags_t clear_flags =
				((minimm_pte_flags_t)~pages[index].flags) & restorable_flags;

			status = minimm_page_table_replace_frame(space->page_table, page,
								 pages[index].frame,
								 pages[index].protection, set_flags,
								 clear_flags);
		}
		if (rollback_status == MINIMM_OK && status != MINIMM_OK) {
			rollback_status = status;
		}
	}
	return rollback_status;
}

minimm_status_t minimm_mlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length)
{
	minimm_memory_range_t range = { 0 };
	minimm_mlock_page_snapshot_t *pages = NULL;
	minimm_vaddr_t page = UINT64_C(0);
	size_t index = 0U;
	size_t attempted_pages = 0U;
	minimm_status_t status = MINIMM_OK;

	if (length == UINT64_C(0)) {
		return minimm_memory_validate_empty_range(space, address, false);
	}
	status = minimm_memory_prepare_lock_range(space, address, length, &range);

	if (status != MINIMM_OK || range.page_count > (uint64_t)SIZE_MAX) {
		return status == MINIMM_OK ? MINIMM_ERROR_OUT_OF_MEMORY : status;
	}
	page = range.start;
	pages = calloc((size_t)range.page_count, sizeof(*pages));
	if (pages == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	status = minimm_memory_lock_space(space, &range);
	if (status != MINIMM_OK) {
		free(pages);
		return status;
	}
	minimm_memory_snapshot_mlock_pages_locked(space, range.start, pages,
						  (size_t)range.page_count);

	while (page < range.end) {
		minimm_pte_t *pte = minimm_page_table_lookup(space->page_table, page);

		attempted_pages = index + 1U;
		if (pte == NULL) {
			status = minimm_populate_page_locked(space, page);
			if (status != MINIMM_OK) {
				break;
			}
			pte = minimm_page_table_lookup(space->page_table, page);
		}
		if (pte == NULL) {
			status = MINIMM_ERROR_NOT_FOUND;
			break;
		}
		if ((pte->flags & MINIMM_PTE_LOCKED) == 0U) {
			status = minimm_page_table_update_attributes(
				space->page_table, page, pte->protection, MINIMM_PTE_LOCKED, 0U);
			if (status != MINIMM_OK) {
				break;
			}
			minimm_tlb_invalidate_page(space->tlb, page);
		}
		/*
		 * Install the pin before the final residency check.  A shared alias in
		 * another space can otherwise page the frame out between
		 * ensure-resident and setting MINIMM_PTE_LOCKED, allowing mlock to
		 * return success for a nonresident frame.
		 */
		status = minimm_populate_page_locked(space, page);
		if (status != MINIMM_OK) {
			break;
		}
		index += 1U;
		page += MINIMM_PAGE_SIZE;
	}

	if (status != MINIMM_OK) {
		const minimm_status_t rollback_status = minimm_memory_restore_mlock_pages_locked(
			space, range.start, pages, attempted_pages);

		if (rollback_status != MINIMM_OK) {
			status = rollback_status;
		}
	}
	minimm_memory_release_mlock_snapshots(pages, (size_t)range.page_count);
	(void)pthread_mutex_unlock(&space->lock);
	free(pages);
	return status;
}

minimm_status_t minimm_munlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length)
{
	minimm_memory_range_t range = { 0 };
	minimm_vaddr_t cursor = UINT64_C(0);
	minimm_vaddr_t page = UINT64_C(0);
	minimm_status_t status = MINIMM_OK;

	if (length == UINT64_C(0)) {
		return minimm_memory_validate_empty_range(space, address, false);
	}
	status = minimm_memory_prepare_lock_range(space, address, length, &range);

	if (status != MINIMM_OK) {
		return status;
	}
	cursor = range.start;
	status = minimm_memory_lock_space(space, &range);
	if (status != MINIMM_OK) {
		return status;
	}

	while (minimm_page_table_find_next(space->page_table, cursor, range.end, &page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL && (pte->flags & MINIMM_PTE_LOCKED) != 0U) {
			status = minimm_page_table_update_attributes(
				space->page_table, page, pte->protection, 0U, MINIMM_PTE_LOCKED);
			if (status != MINIMM_OK) {
				break;
			}
			minimm_tlb_invalidate_page(space->tlb, page);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}

	(void)pthread_mutex_unlock(&space->lock);
	return status;
}

static minimm_status_t minimm_memory_willneed_locked(minimm_space_t *space,
						     const minimm_memory_range_t *range)
{
	const minimm_vma_snapshot_t *snapshot =
		atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	minimm_vaddr_t cursor = range->start;

	while (cursor < range->end) {
		const minimm_vma_t *mapping = minimm_vma_snapshot_find_next(snapshot, cursor);
		minimm_space_binding_t *binding = NULL;
		minimm_backing_kind_t kind = MINIMM_BACKING_ANON_PRIVATE;
		minimm_vaddr_t chunk_end = range->end;

		if (mapping == NULL || mapping->start > cursor) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		binding = minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
		if (binding == NULL) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		chunk_end = mapping->end < range->end ? mapping->end : range->end;
		kind = minimm_mapping_backing_kind(binding->backing);
		if (kind != MINIMM_BACKING_ANON_PRIVATE) {
			minimm_vaddr_t page = cursor;

			while (page < chunk_end) {
				const minimm_pte_t *pte =
					minimm_page_table_lookup_const(space->page_table, page);
				minimm_frame_t *frame = NULL;
				minimm_status_t status = MINIMM_OK;
				bool paged_in = false;

				if (pte != NULL) {
					status =
						minimm_frame_ensure_resident(pte->frame, &paged_in);
				} else {
					const uint64_t backing_offset =
						mapping->note_offset + (page - mapping->start);

					status = minimm_mapping_backing_get_frame(
						binding->backing, backing_offset, &frame, NULL);
					if (status == MINIMM_OK) {
						status = minimm_frame_ensure_resident(frame,
										      &paged_in);
					}
					minimm_frame_release(frame);
				}
				if (status != MINIMM_OK) {
					return status;
				}
				page += MINIMM_PAGE_SIZE;
			}
		} else {
			minimm_vaddr_t pte_cursor = cursor;
			minimm_vaddr_t page = UINT64_C(0);

			while (minimm_page_table_find_next(space->page_table, pte_cursor, chunk_end,
							   &page) == MINIMM_OK) {
				const minimm_pte_t *pte =
					minimm_page_table_lookup_const(space->page_table, page);
				bool paged_in = false;
				const minimm_status_t status =
					minimm_frame_ensure_resident(pte->frame, &paged_in);

				if (status != MINIMM_OK) {
					return status;
				}
				pte_cursor = page + MINIMM_PAGE_SIZE;
			}
		}
		cursor = chunk_end;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_dontneed_locked(minimm_space_t *space,
						     const minimm_memory_range_t *range)
{
	minimm_vaddr_t cursor = range->start;
	minimm_vaddr_t page = UINT64_C(0);

	while (minimm_page_table_find_next(space->page_table, cursor, range->end, &page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL && (pte->flags & MINIMM_PTE_LOCKED) == 0U) {
			minimm_tlb_invalidate_page(space->tlb, page);
			(void)minimm_page_table_unmap(space->page_table, page);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_cold_locked(minimm_space_t *space,
						 const minimm_memory_range_t *range)
{
	minimm_vaddr_t cursor = range->start;
	minimm_vaddr_t page = UINT64_C(0);

	while (minimm_page_table_find_next(space->page_table, cursor, range->end, &page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL) {
			const minimm_status_t status = minimm_page_table_update_attributes(
				space->page_table, page, pte->protection, 0U, MINIMM_PTE_ACCESSED);

			if (status != MINIMM_OK) {
				return status;
			}
			minimm_tlb_invalidate_page(space->tlb, page);
			minimm_frame_mark_cold(pte->frame);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_validate_reclaimable_locked(minimm_space_t *space,
								 const minimm_memory_range_t *range,
								 bool include_frame_pins)
{
	minimm_vaddr_t cursor = range->start;
	minimm_vaddr_t page = UINT64_C(0);

	while (minimm_page_table_find_next(space->page_table, cursor, range->end, &page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL) {
			minimm_frame_state_t state = { 0 };

			minimm_frame_get_state(pte->frame, &state);
			if ((pte->flags & MINIMM_PTE_LOCKED) != 0U ||
			    (include_frame_pins && state.pinned)) {
				return MINIMM_ERROR_BUSY;
			}
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_memory_pageout_locked(minimm_space_t *space,
						    const minimm_memory_range_t *range)
{
	minimm_vaddr_t cursor = range->start;
	minimm_vaddr_t page = UINT64_C(0);

	while (minimm_page_table_find_next(space->page_table, cursor, range->end, &page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte = minimm_page_table_lookup_const(space->page_table, page);

		if (pte != NULL) {
			const minimm_status_t status = minimm_frame_page_out(pte->frame);

			if (status != MINIMM_OK) {
				return status;
			}
			(void)minimm_page_table_update_attributes(
				space->page_table, page, pte->protection, 0U, MINIMM_PTE_DIRTY);
			minimm_tlb_invalidate_page(space->tlb, page);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
	return MINIMM_OK;
}

minimm_status_t minimm_madvise(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       minimm_advice_t advice)
{
	minimm_memory_range_t range = { 0 };
	minimm_status_t status = MINIMM_OK;

	if (advice < MINIMM_MADV_NORMAL || advice > MINIMM_MADV_COLD) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (length == UINT64_C(0)) {
		return minimm_memory_validate_empty_range(space, address, true);
	}
	status = minimm_memory_prepare_range(space, address, length, &range);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_memory_lock_space(space, &range);
	if (status != MINIMM_OK) {
		return status;
	}

	if (advice == MINIMM_MADV_WILLNEED) {
		status = minimm_memory_willneed_locked(space, &range);
	} else if (advice == MINIMM_MADV_COLD) {
		status = minimm_memory_cold_locked(space, &range);
	} else if (advice == MINIMM_MADV_DONTNEED || advice == MINIMM_MADV_PAGEOUT) {
		status = minimm_memory_validate_reclaimable_locked(space, &range,
								   advice == MINIMM_MADV_PAGEOUT);
		if (status == MINIMM_OK && advice == MINIMM_MADV_DONTNEED) {
			status = minimm_memory_dontneed_locked(space, &range);
		} else if (status == MINIMM_OK) {
			status = minimm_memory_pageout_locked(space, &range);
		}
	}

	(void)pthread_mutex_unlock(&space->lock);
	return status;
}
