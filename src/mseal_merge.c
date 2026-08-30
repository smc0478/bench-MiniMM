#include "mseal_merge.h"

#include "note.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINIMM_MSEAL_MERGE_VMA_CAPACITY 3U
#define MINIMM_MSEAL_MERGE_TOTAL_PAGES 3U

typedef struct minimm_mseal_merge_vma {
	uint64_t start;
	uint64_t end;
	minimm_prot_t protection;
	bool sealed;
} minimm_mseal_merge_vma_t;

typedef struct minimm_mseal_merge_layout {
	minimm_mseal_merge_vma_t vmas[MINIMM_MSEAL_MERGE_VMA_CAPACITY];
	size_t count;
} minimm_mseal_merge_layout_t;

static bool minimm_mseal_merge_vmas_match(const minimm_mseal_merge_vma_t *left,
					  const minimm_mseal_merge_vma_t *right)
{
	return left->end == right->start && left->protection == right->protection &&
	       left->sealed == right->sealed;
}

static bool minimm_mseal_merge_modify_flags(minimm_mseal_merge_layout_t *layout, size_t index,
					    uint64_t update_start, uint64_t update_end)
{
	minimm_mseal_merge_vma_t *vma = NULL;
	size_t move = 0U;

	if (layout == NULL || index >= layout->count) {
		return false;
	}
	vma = &layout->vmas[index];
	if (update_start < vma->start || update_start >= update_end || update_end > vma->end) {
		return false;
	}

	vma->sealed = true;
	if (index + 1U >= layout->count ||
	    !minimm_mseal_merge_vmas_match(vma, &layout->vmas[index + 1U])) {
		return true;
	}

	vma->end = layout->vmas[index + 1U].end;
	for (move = index + 1U; move + 1U < layout->count; ++move) {
		layout->vmas[move] = layout->vmas[move + 1U];
	}
	layout->count -= 1U;
	return true;
}

static uint32_t minimm_mseal_merge_count_sealed_pages(const minimm_mseal_merge_layout_t *layout)
{
	uint32_t pages = 0U;
	size_t index = 0U;

	for (index = 0U; index < layout->count; ++index) {
		const minimm_mseal_merge_vma_t *vma = &layout->vmas[index];

		if (vma->sealed) {
			pages += (uint32_t)((vma->end - vma->start) / MINIMM_PAGE_SIZE);
		}
	}
	return pages;
}

static minimm_status_t minimm_mseal_merge_apply_range(minimm_mseal_merge_layout_t *layout,
						      uint64_t start, uint64_t end,
						      minimm_mseal_merge_result_t *out_result)
{
	uint64_t curr_start = start;
	size_t index = 0U;

	out_result->total_pages = MINIMM_MSEAL_MERGE_TOTAL_PAGES;
	out_result->range_valid = true;
	for (index = 0U; index < layout->count && layout->vmas[index].start < end; ++index) {
		minimm_mseal_merge_vma_t *vma = &layout->vmas[index];
		const uint64_t curr_end = vma->end < end ? vma->end : end;

		out_result->update_start = curr_start;
		out_result->current_start = vma->start;
		if (!vma->sealed &&
		    !minimm_mseal_merge_modify_flags(layout, index, curr_start, curr_end)) {
			out_result->range_valid = false;
			out_result->sealed_pages = minimm_mseal_merge_count_sealed_pages(layout);
			return MINIMM_OK;
		}

		/* Advance to the boundary observed for this iteration. */
		curr_start = curr_end;
	}

	out_result->sealed_pages = minimm_mseal_merge_count_sealed_pages(layout);
	return MINIMM_OK;
}

minimm_status_t minimm_mseal_merge_apply(minimm_t *mm, minimm_note_t *note,
					 minimm_mseal_merge_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;
	const minimm_prot_t mergeable_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE;
	const uint64_t expected_size = MINIMM_PAGE_SIZE * MINIMM_MSEAL_MERGE_TOTAL_PAGES;
	minimm_mseal_merge_layout_t layout = {
		.vmas = {
			{
				.start = UINT64_C(0),
				.end = MINIMM_PAGE_SIZE,
				.protection = mergeable_protection,
				.sealed = false,
			},
			{
				.start = MINIMM_PAGE_SIZE,
				.end = MINIMM_PAGE_SIZE * UINT64_C(2),
				.protection = mergeable_protection,
				.sealed = true,
			},
			{
				.start = MINIMM_PAGE_SIZE * UINT64_C(2),
				.end = MINIMM_PAGE_SIZE * UINT64_C(3),
				.protection = MINIMM_PROT_READ,
				.sealed = false,
			},
		},
		.count = MINIMM_MSEAL_MERGE_VMA_CAPACITY,
	};

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_mseal_merge_result_t){ 0 };
	if (mm == NULL || note == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != expected_size) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	return minimm_mseal_merge_apply_range(&layout, UINT64_C(0), expected_size, out_result);
}
