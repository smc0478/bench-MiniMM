#ifndef MINIMM_HUGETLB_RESERVE_H
#define MINIMM_HUGETLB_RESERVE_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_hugetlb_reserve_input {
	uint32_t maximum_pages;
	uint32_t minimum_pages;
	uint32_t used_before;
	uint32_t requested_pages;
	uint32_t global_free_pages;
} minimm_hugetlb_reserve_input_t;

typedef struct minimm_hugetlb_reserve_result {
	uint32_t requested_pages;
	uint32_t global_needed_pages;
	uint32_t allocated_pages;
	uint32_t used_before;
	uint32_t used_after;
	uint32_t rollback_pages;
	bool reservation_succeeded;
	bool accounting_valid;
} minimm_hugetlb_reserve_result_t;

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_hugetlb_reserve_run(minimm_t *mm, minimm_note_t *note,
					   const minimm_hugetlb_reserve_input_t *input,
					   minimm_hugetlb_reserve_result_t *out_result);

#endif
