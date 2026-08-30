#ifndef MINIMM_PERCPU_POPULATE_H
#define MINIMM_PERCPU_POPULATE_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_percpu_populate_input {
	uint32_t unit_count;
	uint32_t unit_pages;
} minimm_percpu_populate_input_t;

typedef struct minimm_percpu_populate_result {
	uint32_t total_backing_pages;
	uint32_t bitmap_capacity;
	uint32_t mark_count;
	uint32_t first_invalid_index;
	uint32_t empty_pages_after;
	uint32_t expected_empty_pages;
	bool bounds_valid;
	bool accounting_valid;
} minimm_percpu_populate_result_t;

/* The caller serializes access to note. This model never performs an OOB write. */
minimm_status_t minimm_percpu_populate_run(minimm_t *mm, minimm_note_t *note,
					   const minimm_percpu_populate_input_t *input,
					   minimm_percpu_populate_result_t *out_result);

#endif
