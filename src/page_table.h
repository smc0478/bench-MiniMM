#ifndef MINIMM_PAGE_TABLE_H
#define MINIMM_PAGE_TABLE_H

#include "frame.h"

#include <stddef.h>
#include <stdint.h>

typedef uint32_t minimm_pte_flags_t;

enum {
	MINIMM_PTE_PRESENT = UINT32_C(1) << 0,
	MINIMM_PTE_COW = UINT32_C(1) << 1,
	MINIMM_PTE_SHARED = UINT32_C(1) << 2,
	MINIMM_PTE_ACCESSED = UINT32_C(1) << 3,
	MINIMM_PTE_DIRTY = UINT32_C(1) << 4,
	MINIMM_PTE_LOCKED = UINT32_C(1) << 5
};

typedef struct minimm_pte {
	minimm_frame_t *frame;
	uint32_t protection;
	minimm_pte_flags_t flags;
} minimm_pte_t;

typedef struct minimm_page_table minimm_page_table_t;

typedef minimm_status_t (*minimm_page_table_visitor_t)(minimm_vaddr_t page_address,
						       const minimm_pte_t *pte, void *context);

minimm_status_t minimm_page_table_create(minimm_page_table_t **out_table);
void minimm_page_table_destroy(minimm_page_table_t *table);

minimm_status_t minimm_page_table_map(minimm_page_table_t *table, minimm_vaddr_t page_address,
				      minimm_frame_t *frame, uint32_t protection,
				      minimm_pte_flags_t flags);
minimm_status_t minimm_page_table_unmap(minimm_page_table_t *table, minimm_vaddr_t page_address);
minimm_pte_t *minimm_page_table_lookup(minimm_page_table_t *table, minimm_vaddr_t address);
const minimm_pte_t *minimm_page_table_lookup_const(const minimm_page_table_t *table,
						   minimm_vaddr_t address);
minimm_status_t minimm_page_table_protect(minimm_page_table_t *table, minimm_vaddr_t page_address,
					  uint32_t protection);
/*
 * Update protection and software flags as one operation. Callers provide any
 * synchronization needed around the page table (normally the space lock).
 */
minimm_status_t minimm_page_table_update_attributes(minimm_page_table_t *table,
						    minimm_vaddr_t page_address,
						    uint32_t protection,
						    minimm_pte_flags_t set_flags,
						    minimm_pte_flags_t clear_flags);
minimm_status_t minimm_page_table_replace_frame(minimm_page_table_t *table,
						minimm_vaddr_t page_address, minimm_frame_t *frame,
						uint32_t protection, minimm_pte_flags_t set_flags,
						minimm_pte_flags_t clear_flags);
minimm_status_t minimm_page_table_update_flags(minimm_page_table_t *table,
					       minimm_vaddr_t page_address,
					       minimm_pte_flags_t set_flags,
					       minimm_pte_flags_t clear_flags);

/*
 * Visit every mapped page in ascending virtual-address order. The PTE is only
 * valid for the duration of the callback. The callback may update PTE
 * attributes, but it must not map, unmap, or destroy the table being walked.
 * A non-OK callback result stops the walk and is returned unchanged.
 */
minimm_status_t minimm_page_table_for_each(const minimm_page_table_t *table,
					   minimm_page_table_visitor_t visitor, void *context);

/*
 * Find the lowest mapped page in [start, end). This walks only allocated page-
 * table nodes, so callers can advance start to the returned address plus one
 * page and mutate (including unmap) between calls without scanning holes.
 */
minimm_status_t minimm_page_table_find_next(const minimm_page_table_t *table, minimm_vaddr_t start,
					    minimm_vaddr_t end, minimm_vaddr_t *out_page_address);

size_t minimm_page_table_mapping_count(const minimm_page_table_t *table);
uint64_t minimm_page_table_generation(const minimm_page_table_t *table);

#endif
