#ifndef MINIMM_TLB_H
#define MINIMM_TLB_H

#include "page_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct minimm_tlb minimm_tlb_t;

typedef struct minimm_tlb_translation {
	minimm_frame_t *frame;
	uint32_t protection;
	minimm_pte_flags_t flags;
} minimm_tlb_translation_t;

typedef struct minimm_tlb_stats {
	uint64_t hits;
	uint64_t misses;
	uint64_t replacements;
	uint64_t invalidations;
} minimm_tlb_stats_t;

/*
 * A TLB belongs to one page table and requires external serialization.
 * Page-table mutations must explicitly invalidate the affected page or range;
 * unrelated PTE changes do not age otherwise valid entries.
 */
minimm_status_t minimm_tlb_create(size_t capacity, minimm_tlb_t **out_tlb);
void minimm_tlb_destroy(minimm_tlb_t *tlb);

/* Every miss, including invalid input with a non-null output, clears the translation. */
bool minimm_tlb_lookup(minimm_tlb_t *tlb, minimm_vaddr_t address,
		       minimm_tlb_translation_t *out_translation);
void minimm_tlb_translation_release(minimm_tlb_translation_t *translation);
void minimm_tlb_insert(minimm_tlb_t *tlb, minimm_vaddr_t address, const minimm_pte_t *pte);
void minimm_tlb_invalidate_page(minimm_tlb_t *tlb, minimm_vaddr_t address);
void minimm_tlb_invalidate_range(minimm_tlb_t *tlb, minimm_vaddr_t start, minimm_vaddr_t end);
void minimm_tlb_flush(minimm_tlb_t *tlb);
void minimm_tlb_get_stats(const minimm_tlb_t *tlb, minimm_tlb_stats_t *out_stats);

#endif
