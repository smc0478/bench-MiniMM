#include "percpu_populate.h"

#include "note.h"

#include <stdint.h>

#define MINIMM_PERCPU_POPULATE_MAX_UNITS UINT32_C(4096)
#define MINIMM_PERCPU_POPULATE_MAX_UNIT_PAGES UINT32_C(4096)
#define MINIMM_PERCPU_POPULATE_MAX_TOTAL_PAGES UINT32_C(1048576)

static minimm_status_t minimm_percpu_mark_populated(const minimm_percpu_populate_input_t *input,
						    minimm_percpu_populate_result_t *out_result)
{
	const uint64_t total_pages = (uint64_t)input->unit_count * input->unit_pages;

	if (total_pages > MINIMM_PERCPU_POPULATE_MAX_TOTAL_PAGES) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	out_result->total_backing_pages = (uint32_t)total_pages;
	out_result->bitmap_capacity = input->unit_pages;
	/* Record the transient population count without allocating a real bitmap. */
	out_result->mark_count = (uint32_t)total_pages;
	out_result->bounds_valid = out_result->mark_count <= out_result->bitmap_capacity;
	out_result->first_invalid_index = out_result->bounds_valid ? UINT32_MAX :
								     out_result->bitmap_capacity;
	out_result->empty_pages_after = out_result->mark_count;
	out_result->expected_empty_pages = out_result->bitmap_capacity;
	out_result->accounting_valid = out_result->empty_pages_after ==
				       out_result->expected_empty_pages;
	return MINIMM_OK;
}

minimm_status_t minimm_percpu_populate_run(minimm_t *mm, minimm_note_t *note,
					   const minimm_percpu_populate_input_t *input,
					   minimm_percpu_populate_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_percpu_populate_result_t){ 0 };
	if (mm == NULL || note == NULL || input == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != MINIMM_PAGE_SIZE || input->unit_count == UINT32_C(0) ||
	    input->unit_count > MINIMM_PERCPU_POPULATE_MAX_UNITS ||
	    input->unit_pages == UINT32_C(0) ||
	    input->unit_pages > MINIMM_PERCPU_POPULATE_MAX_UNIT_PAGES) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	return minimm_percpu_mark_populated(input, out_result);
}
