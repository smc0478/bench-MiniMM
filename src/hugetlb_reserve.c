#include "hugetlb_reserve.h"

#include "note.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINIMM_HUGETLB_RESERVE_PAGE_LIMIT UINT32_C(1000000)

typedef struct minimm_hugetlb_subpool {
	uint32_t maximum_pages;
	uint32_t minimum_pages;
	uint32_t used_pages;
} minimm_hugetlb_subpool_t;

static bool minimm_hugetlb_reserve_input_valid(const minimm_hugetlb_reserve_input_t *input)
{
	if (input->maximum_pages == 0U ||
	    input->maximum_pages > MINIMM_HUGETLB_RESERVE_PAGE_LIMIT ||
	    input->minimum_pages > MINIMM_HUGETLB_RESERVE_PAGE_LIMIT ||
	    input->used_before > MINIMM_HUGETLB_RESERVE_PAGE_LIMIT ||
	    input->requested_pages > MINIMM_HUGETLB_RESERVE_PAGE_LIMIT ||
	    input->global_free_pages > MINIMM_HUGETLB_RESERVE_PAGE_LIMIT) {
		return false;
	}
	if (input->minimum_pages > input->maximum_pages ||
	    input->used_before > input->maximum_pages || input->requested_pages == 0U) {
		return false;
	}
	return input->requested_pages <= input->maximum_pages - input->used_before;
}

static uint32_t minimm_hugetlb_pages_above_minimum(const minimm_hugetlb_subpool_t *subpool)
{
	if (subpool->used_pages <= subpool->minimum_pages) {
		return 0U;
	}
	return subpool->used_pages - subpool->minimum_pages;
}

static uint32_t minimm_hugetlb_subpool_get_pages(minimm_hugetlb_subpool_t *subpool, uint32_t pages)
{
	const uint32_t global_before = minimm_hugetlb_pages_above_minimum(subpool);
	uint32_t global_after = 0U;

	subpool->used_pages += pages;
	global_after = minimm_hugetlb_pages_above_minimum(subpool);
	return global_after - global_before;
}

static void minimm_hugetlb_subpool_put_pages(minimm_hugetlb_subpool_t *subpool, uint32_t pages)
{
	subpool->used_pages -= pages;
}

minimm_status_t minimm_hugetlb_reserve_run(minimm_t *mm, minimm_note_t *note,
					   const minimm_hugetlb_reserve_input_t *input,
					   minimm_hugetlb_reserve_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;
	minimm_hugetlb_subpool_t subpool;
	uint32_t global_needed_pages = 0U;
	uint32_t subpool_pages = 0U;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_hugetlb_reserve_result_t){ 0 };
	if (mm == NULL || note == NULL || input == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != MINIMM_PAGE_SIZE) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}
	if (!minimm_hugetlb_reserve_input_valid(input)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	subpool = (minimm_hugetlb_subpool_t){
		.maximum_pages = input->maximum_pages,
		.minimum_pages = input->minimum_pages,
		.used_pages = input->used_before,
	};
	global_needed_pages = minimm_hugetlb_subpool_get_pages(&subpool, input->requested_pages);
	subpool_pages = input->requested_pages - global_needed_pages;

	out_result->requested_pages = input->requested_pages;
	out_result->global_needed_pages = global_needed_pages;
	out_result->used_before = input->used_before;
	if (input->global_free_pages < global_needed_pages) {
		minimm_hugetlb_subpool_put_pages(&subpool, subpool_pages);
		out_result->allocated_pages = 0U;
		out_result->used_after = subpool.used_pages;
		out_result->rollback_pages = subpool_pages;
		out_result->reservation_succeeded = false;
		out_result->accounting_valid = subpool.used_pages == input->used_before;
		return MINIMM_OK;
	}

	out_result->allocated_pages = input->requested_pages;
	out_result->used_after = subpool.used_pages;
	out_result->rollback_pages = 0U;
	out_result->reservation_succeeded = true;
	out_result->accounting_valid = true;
	return MINIMM_OK;
}
