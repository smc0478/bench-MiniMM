#include "rmap_unmap.h"

#include "note.h"

#include <stdbool.h>
#include <stdint.h>

#define MINIMM_RMAP_UNMAP_MAX_PAGES UINT32_C(4096)

static minimm_status_t minimm_rmap_unmap_batch(const minimm_rmap_unmap_input_t *input,
					       minimm_rmap_unmap_result_t *out_result)
{
	const uint32_t requested_pages = input->folio_pages;
	const uint32_t scanned_pages =
		requested_pages < input->vma_remaining ? requested_pages : input->vma_remaining;
	const uint32_t available_pages = input->pte_capacity - input->pte_index;
	const uint32_t safe_pages = scanned_pages < available_pages ? scanned_pages :
								      available_pages;
	const bool crossed_pte_boundary = scanned_pages > safe_pages;

	if (input->pte_index > UINT32_MAX - scanned_pages ||
	    input->pte_index > UINT32_MAX - safe_pages) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	out_result->requested_pages = requested_pages;
	out_result->scanned_pages = scanned_pages;
	out_result->safe_pages = safe_pages;
	out_result->first_invalid_index = crossed_pte_boundary ? input->pte_index + safe_pages :
								 UINT32_MAX;
	out_result->crossed_pte_boundary = crossed_pte_boundary;
	out_result->bounds_valid = !crossed_pte_boundary;
	return MINIMM_OK;
}

minimm_status_t minimm_rmap_unmap_run(minimm_t *mm, minimm_note_t *note,
				      const minimm_rmap_unmap_input_t *input,
				      minimm_rmap_unmap_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_rmap_unmap_result_t){ 0 };
	if (mm == NULL || note == NULL || input == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != MINIMM_PAGE_SIZE || input->pte_capacity == UINT32_C(0) ||
	    input->pte_capacity > MINIMM_RMAP_UNMAP_MAX_PAGES ||
	    input->pte_index >= input->pte_capacity || input->folio_pages == UINT32_C(0) ||
	    input->folio_pages > MINIMM_RMAP_UNMAP_MAX_PAGES ||
	    input->vma_remaining == UINT32_C(0) ||
	    input->vma_remaining > MINIMM_RMAP_UNMAP_MAX_PAGES) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	return minimm_rmap_unmap_batch(input, out_result);
}
