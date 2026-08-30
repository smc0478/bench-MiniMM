#include "stats.h"

#include "frame.h"
#include "internal.h"
#include "page_table.h"
#include "space.h"
#include "tlb.h"
#include "vma_tree.h"

#include <stdatomic.h>
#include <string.h>

typedef struct minimm_stats_walk {
	minimm_space_stats_t *stats;
} minimm_stats_walk_t;

static minimm_status_t minimm_stats_count_pte(minimm_vaddr_t page_address, const minimm_pte_t *pte,
					      void *opaque)
{
	minimm_stats_walk_t *walk = opaque;
	minimm_space_stats_t *stats = walk->stats;
	minimm_frame_state_t frame_state = { 0 };

	(void)page_address;
	minimm_frame_get_state(pte->frame, &frame_state);

	stats->pte_count += 1U;
	if ((pte->flags & MINIMM_PTE_PRESENT) != 0U && frame_state.resident) {
		stats->present_count += 1U;
	}
	if (frame_state.resident) {
		stats->resident_count += 1U;
	}
	if (frame_state.dirty || (pte->flags & MINIMM_PTE_DIRTY) != 0U) {
		stats->dirty_count += 1U;
	}
	if ((pte->flags & MINIMM_PTE_COW) != 0U) {
		stats->cow_count += 1U;
	}
	if ((pte->flags & MINIMM_PTE_SHARED) != 0U) {
		stats->shared_count += 1U;
	}
	if (frame_state.pinned || (pte->flags & MINIMM_PTE_LOCKED) != 0U) {
		stats->locked_count += 1U;
	}
	return MINIMM_OK;
}

minimm_status_t minimm_system_get_stats(const minimm_t *mm, minimm_system_stats_t *out_stats)
{
	minimm_frame_store_stats_t frame_stats = { 0 };

	if (mm == NULL || out_stats == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_frame_store_get_stats(mm->frame_store, &frame_stats);
	out_stats->frame_count = frame_stats.frame_count;
	out_stats->resident_count = frame_stats.resident_count;
	out_stats->resident_limit = frame_stats.resident_limit;
	out_stats->page_in_count = frame_stats.page_in_count;
	out_stats->page_out_count = frame_stats.page_out_count;
	out_stats->reclaim_scan_count = frame_stats.reclaim_scan_count;
	out_stats->reclaim_count = frame_stats.reclaim_count;
	out_stats->refault_count = frame_stats.refault_count;
	return MINIMM_OK;
}

minimm_status_t minimm_space_get_stats(minimm_space_t *space, minimm_space_stats_t *out_stats)
{
	minimm_vma_snapshot_t *snapshot = NULL;
	minimm_tlb_stats_t tlb_stats = { 0 };
	minimm_stats_walk_t walk = { .stats = out_stats };
	minimm_status_t status = MINIMM_OK;

	if (space == NULL || out_stats == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_stats, 0, sizeof(*out_stats));

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}

	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	out_stats->vma_count = minimm_vma_snapshot_count(snapshot);
	out_stats->fault_sequence = space->fault_sequence;
	status = minimm_page_table_for_each(space->page_table, minimm_stats_count_pte, &walk);
	minimm_tlb_get_stats(space->tlb, &tlb_stats);
	out_stats->tlb_hits = tlb_stats.hits;
	out_stats->tlb_misses = tlb_stats.misses;
	out_stats->tlb_replacements = tlb_stats.replacements;
	out_stats->tlb_invalidations = tlb_stats.invalidations;
	(void)pthread_mutex_unlock(&space->lock);
	return status;
}

minimm_status_t minimm_space_flush_tlb(minimm_space_t *space)
{
	if (space == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	minimm_tlb_flush(space->tlb);
	(void)pthread_mutex_unlock(&space->lock);
	return MINIMM_OK;
}
