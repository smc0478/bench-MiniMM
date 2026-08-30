#ifndef MINIMM_MSEAL_MERGE_H
#define MINIMM_MSEAL_MERGE_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_mseal_merge_result {
	uint32_t total_pages;
	uint32_t sealed_pages;
	bool range_valid;
	uint64_t update_start;
	uint64_t current_start;
} minimm_mseal_merge_result_t;

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_mseal_merge_apply(minimm_t *mm, minimm_note_t *note,
					 minimm_mseal_merge_result_t *out_result);

#endif
