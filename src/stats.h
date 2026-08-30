#ifndef MINIMM_STATS_H
#define MINIMM_STATS_H

#include "minimm/minimm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MINIMM_STATS_API
/*
 * Proposed public statistics API. These declarations live here until they are
 * promoted to include/minimm/minimm.h with the rest of the user API.
 */
typedef struct minimm_system_stats {
	size_t frame_count;
	size_t resident_count;
	size_t resident_limit;
	uint64_t page_in_count;
	uint64_t page_out_count;
	uint64_t reclaim_scan_count;
	uint64_t reclaim_count;
	uint64_t refault_count;
} minimm_system_stats_t;

typedef struct minimm_reclaim_result {
	size_t scanned_count;
	size_t reclaimed_count;
} minimm_reclaim_result_t;

/*
 * Page counters describe PTEs in this address space, so a frame shared by two
 * spaces is counted once in each space. Dirty and locked include both the PTE
 * bit and the current state of the underlying frame. TLB counters are
 * cumulative for the lifetime of the space.
 */
typedef struct minimm_space_stats {
	size_t vma_count;
	size_t pte_count;
	size_t present_count;
	size_t resident_count;
	size_t dirty_count;
	size_t cow_count;
	size_t shared_count;
	size_t locked_count;
	uint64_t fault_sequence;
	uint64_t tlb_hits;
	uint64_t tlb_misses;
	uint64_t tlb_replacements;
	uint64_t tlb_invalidations;
} minimm_space_stats_t;

minimm_status_t minimm_system_get_stats(const minimm_t *mm, minimm_system_stats_t *out_stats);
minimm_status_t minimm_system_reclaim(minimm_t *mm, size_t target_pages,
				      minimm_reclaim_result_t *out_result);
minimm_status_t minimm_space_get_stats(minimm_space_t *space, minimm_space_stats_t *out_stats);
minimm_status_t minimm_space_flush_tlb(minimm_space_t *space);
#endif

#ifdef __cplusplus
}
#endif

#endif
