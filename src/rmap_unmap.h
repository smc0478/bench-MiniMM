#ifndef MINIMM_RMAP_UNMAP_H
#define MINIMM_RMAP_UNMAP_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_rmap_unmap_input {
	uint32_t pte_capacity;
	uint32_t pte_index;
	uint32_t folio_pages;
	uint32_t vma_remaining;
} minimm_rmap_unmap_input_t;

typedef struct minimm_rmap_unmap_result {
	uint32_t requested_pages;
	uint32_t scanned_pages;
	uint32_t safe_pages;
	uint32_t first_invalid_index;
	bool crossed_pte_boundary;
	bool bounds_valid;
} minimm_rmap_unmap_result_t;

/* The caller serializes access to note. The model only computes metadata. */
minimm_status_t minimm_rmap_unmap_run(minimm_t *mm, minimm_note_t *note,
				      const minimm_rmap_unmap_input_t *input,
				      minimm_rmap_unmap_result_t *out_result);

#endif
