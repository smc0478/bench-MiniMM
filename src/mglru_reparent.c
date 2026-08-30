#include "mglru_reparent.h"

#include "note.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINIMM_MGLRU_REPARENT_GENERATIONS 2U
#define MINIMM_MGLRU_REPARENT_OLD_GENERATION 0U
#define MINIMM_MGLRU_REPARENT_NEW_GENERATION 1U
#define MINIMM_MGLRU_REPARENT_TOTAL_PAGES 1U

typedef struct minimm_mglru_lruvec {
	int64_t pages[MINIMM_MGLRU_REPARENT_GENERATIONS];
	bool dying;
	struct minimm_mglru_lruvec *parent;
} minimm_mglru_lruvec_t;

typedef struct minimm_mglru_walk {
	minimm_mglru_lruvec_t *lruvec;
	int64_t nr_pages[MINIMM_MGLRU_REPARENT_GENERATIONS];
} minimm_mglru_walk_t;

static void minimm_mglru_record_promotion(minimm_mglru_walk_t *walk, uint32_t pages)
{
	walk->nr_pages[MINIMM_MGLRU_REPARENT_OLD_GENERATION] -= (int64_t)pages;
	walk->nr_pages[MINIMM_MGLRU_REPARENT_NEW_GENERATION] += (int64_t)pages;
}

static void minimm_mglru_reparent_lruvec(minimm_mglru_lruvec_t *child,
					 minimm_mglru_lruvec_t *parent)
{
	size_t generation = 0U;

	child->dying = true;
	child->parent = parent;
	for (generation = 0U; generation < MINIMM_MGLRU_REPARENT_GENERATIONS; ++generation) {
		parent->pages[generation] += child->pages[generation];
		child->pages[generation] = INT64_C(0);
	}
}

static void minimm_mglru_reset_batch_size(minimm_mglru_walk_t *walk)
{
	minimm_mglru_lruvec_t *lruvec = walk->lruvec;
	size_t generation = 0U;

	for (generation = 0U; generation < MINIMM_MGLRU_REPARENT_GENERATIONS; ++generation) {
		lruvec->pages[generation] += walk->nr_pages[generation];
		walk->nr_pages[generation] = INT64_C(0);
	}
}

static minimm_status_t minimm_mglru_collect_result(const minimm_mglru_lruvec_t *child,
						   const minimm_mglru_lruvec_t *parent,
						   minimm_mglru_reparent_result_t *out_result)
{
	const int64_t child_old = child->pages[MINIMM_MGLRU_REPARENT_OLD_GENERATION];
	const int64_t child_new = child->pages[MINIMM_MGLRU_REPARENT_NEW_GENERATION];
	const int64_t parent_old = parent->pages[MINIMM_MGLRU_REPARENT_OLD_GENERATION];
	const int64_t parent_new = parent->pages[MINIMM_MGLRU_REPARENT_NEW_GENERATION];

	if (child_old > INT64_C(0) || child_old < -((int64_t)UINT32_MAX) ||
	    child_new < INT64_C(0) || child_new > (int64_t)UINT32_MAX || parent_old < INT64_C(0) ||
	    parent_old > (int64_t)UINT32_MAX || parent_new < INT64_C(0) ||
	    parent_new > (int64_t)UINT32_MAX) {
		return MINIMM_ERROR_IO;
	}

	out_result->total_pages = MINIMM_MGLRU_REPARENT_TOTAL_PAGES;
	out_result->parent_old_pages = (uint32_t)parent_old;
	out_result->parent_new_pages = (uint32_t)parent_new;
	out_result->child_old_debt_pages = (uint32_t)(-child_old);
	out_result->child_new_credit_pages = (uint32_t)child_new;
	out_result->exit_clean = child_old == INT64_C(0) && child_new == INT64_C(0);
	out_result->accounting_valid = out_result->exit_clean && parent_old == INT64_C(0) &&
				       parent_new == (int64_t)MINIMM_MGLRU_REPARENT_TOTAL_PAGES;
	return MINIMM_OK;
}

minimm_status_t minimm_mglru_reparent_run(minimm_t *mm, minimm_note_t *note,
					  minimm_mglru_reparent_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;
	minimm_mglru_lruvec_t parent = {
		.pages = { INT64_C(0), INT64_C(0) },
		.dying = false,
		.parent = NULL,
	};
	minimm_mglru_lruvec_t child = {
		.pages = { INT64_C(1), INT64_C(0) },
		.dying = false,
		.parent = NULL,
	};
	minimm_mglru_walk_t walk = {
		.lruvec = &child,
		.nr_pages = { INT64_C(0), INT64_C(0) },
	};

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_mglru_reparent_result_t){ 0 };
	if (mm == NULL || note == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != MINIMM_PAGE_SIZE) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	minimm_mglru_record_promotion(&walk, MINIMM_MGLRU_REPARENT_TOTAL_PAGES);
	minimm_mglru_reparent_lruvec(&child, &parent);
	minimm_mglru_reset_batch_size(&walk);
	return minimm_mglru_collect_result(&child, &parent, out_result);
}
