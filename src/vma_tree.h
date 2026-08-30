#ifndef MINIMM_VMA_TREE_H
#define MINIMM_VMA_TREE_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINIMM_VMA_TREE_FANOUT 16U

typedef uint32_t minimm_vma_prot_t;
typedef uint32_t minimm_vma_flags_t;

#define MINIMM_VMA_PROT_NONE ((minimm_vma_prot_t)MINIMM_PROT_NONE)
#define MINIMM_VMA_PROT_READ ((minimm_vma_prot_t)MINIMM_PROT_READ)
#define MINIMM_VMA_PROT_WRITE ((minimm_vma_prot_t)MINIMM_PROT_WRITE)
#define MINIMM_VMA_PROT_EDIT ((minimm_vma_prot_t)MINIMM_PROT_EDIT)
#define MINIMM_VMA_PROT_EXEC ((minimm_vma_prot_t)MINIMM_PROT_EXEC)

#define MINIMM_VMA_FLAG_SHARED ((minimm_vma_flags_t)MINIMM_MAP_SHARED)
#define MINIMM_VMA_FLAG_PRIVATE ((minimm_vma_flags_t)MINIMM_MAP_PRIVATE)

typedef struct minimm_vma {
	minimm_vaddr_t start;
	minimm_vaddr_t end;
	uint64_t mapping_cookie;
	uint64_t note_offset;
	minimm_vma_prot_t prot;
	minimm_vma_prot_t max_prot;
	minimm_vma_flags_t flags;
} minimm_vma_t;

typedef struct minimm_vma_snapshot minimm_vma_snapshot_t;

/*
 * Snapshots never change after publication. Concurrent readers may use a
 * snapshot without locking while its owner guarantees the snapshot lifetime.
 */
minimm_status_t minimm_vma_snapshot_create(minimm_vma_snapshot_t **out_snapshot);
minimm_status_t minimm_vma_snapshot_clone(const minimm_vma_snapshot_t *snapshot,
					  minimm_vma_snapshot_t **out_snapshot);
void minimm_vma_snapshot_destroy(minimm_vma_snapshot_t *snapshot);

/* Returned storage is valid only while the snapshot itself remains protected. */
const minimm_vma_t *minimm_vma_snapshot_lookup(const minimm_vma_snapshot_t *snapshot,
					       minimm_vaddr_t address);
/* Return the mapping containing address, or the first mapping after it. */
const minimm_vma_t *minimm_vma_snapshot_find_next(const minimm_vma_snapshot_t *snapshot,
						  minimm_vaddr_t address);
size_t minimm_vma_snapshot_count(const minimm_vma_snapshot_t *snapshot);
uint64_t minimm_vma_snapshot_generation(const minimm_vma_snapshot_t *snapshot);
bool minimm_vma_snapshot_contains_cookie(const minimm_vma_snapshot_t *snapshot,
					 uint64_t mapping_cookie);

minimm_status_t minimm_vma_snapshot_insert(const minimm_vma_snapshot_t *snapshot,
					   const minimm_vma_t *mapping,
					   minimm_vma_snapshot_t **out_snapshot);
minimm_status_t minimm_vma_snapshot_remove(const minimm_vma_snapshot_t *snapshot,
					   minimm_vaddr_t start, minimm_vaddr_t end,
					   minimm_vma_snapshot_t **out_snapshot);
minimm_status_t minimm_vma_snapshot_protect(const minimm_vma_snapshot_t *snapshot,
					    minimm_vaddr_t start, minimm_vaddr_t end,
					    minimm_vma_prot_t protection,
					    minimm_vma_snapshot_t **out_snapshot);

minimm_status_t minimm_vma_snapshot_find_gap(const minimm_vma_snapshot_t *snapshot,
					     minimm_vaddr_t lower_bound, minimm_vaddr_t upper_bound,
					     uint64_t length, uint64_t alignment,
					     minimm_vaddr_t *out_address);

#endif
