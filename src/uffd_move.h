#ifndef MINIMM_UFFD_MOVE_H
#define MINIMM_UFFD_MOVE_H

#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_uffd_move_input {
	uint32_t swap_entry;
	uint32_t source_folio;
	uint32_t replacement_folio;
} minimm_uffd_move_input_t;

typedef struct minimm_uffd_move_result {
	uint32_t swap_entry;
	uint32_t expected_folio;
	uint32_t moved_folio;
	bool pte_entry_matches;
	bool folio_identity_valid;
	bool accounting_valid;
} minimm_uffd_move_result_t;

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_uffd_move_run(minimm_t *mm, minimm_note_t *note,
				     const minimm_uffd_move_input_t *input,
				     minimm_uffd_move_result_t *out_result);

#endif
