#ifndef MINIMM_MGLRU_REPARENT_H
#define MINIMM_MGLRU_REPARENT_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_mglru_reparent_result {
	uint32_t total_pages;
	uint32_t parent_old_pages;
	uint32_t parent_new_pages;
	uint32_t child_old_debt_pages;
	uint32_t child_new_credit_pages;
	bool exit_clean;
	bool accounting_valid;
} minimm_mglru_reparent_result_t;

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_mglru_reparent_run(minimm_t *mm, minimm_note_t *note,
					  minimm_mglru_reparent_result_t *out_result);

#endif
