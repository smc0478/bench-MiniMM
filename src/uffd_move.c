#include "uffd_move.h"

#include "note.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum minimm_uffd_move_pte_kind {
	MINIMM_UFFD_MOVE_PTE_RESIDENT = 0,
	MINIMM_UFFD_MOVE_PTE_SWAP,
} minimm_uffd_move_pte_kind_t;

typedef struct minimm_uffd_move_pte {
	minimm_uffd_move_pte_kind_t kind;
	uint32_t value;
} minimm_uffd_move_pte_t;

typedef struct minimm_uffd_move_swap_cache {
	uint32_t entry;
	uint32_t folio;
	bool present;
} minimm_uffd_move_swap_cache_t;

typedef struct minimm_uffd_move_candidate {
	uint32_t entry;
	uint32_t folio;
	bool found;
} minimm_uffd_move_candidate_t;

static void minimm_uffd_move_install_resident(minimm_uffd_move_pte_t *pte, uint32_t folio)
{
	pte->kind = MINIMM_UFFD_MOVE_PTE_RESIDENT;
	pte->value = folio;
}

static void minimm_uffd_move_install_swap(minimm_uffd_move_pte_t *pte,
					  minimm_uffd_move_swap_cache_t *cache, uint32_t entry,
					  uint32_t folio)
{
	pte->kind = MINIMM_UFFD_MOVE_PTE_SWAP;
	pte->value = entry;
	cache->entry = entry;
	cache->folio = folio;
	cache->present = true;
}

static minimm_uffd_move_candidate_t
minimm_uffd_move_lookup_swap_cache(const minimm_uffd_move_swap_cache_t *cache, uint32_t entry)
{
	minimm_uffd_move_candidate_t candidate = { 0 };

	if (cache->present && cache->entry == entry) {
		candidate.entry = entry;
		candidate.folio = cache->folio;
		candidate.found = true;
	}
	return candidate;
}

static uint32_t minimm_uffd_move_move_swap_pte(const minimm_uffd_move_pte_t *observed,
					       const minimm_uffd_move_pte_t *current,
					       const minimm_uffd_move_candidate_t *candidate,
					       bool *out_entry_matches)
{
	*out_entry_matches = observed->kind == MINIMM_UFFD_MOVE_PTE_SWAP &&
			     current->kind == MINIMM_UFFD_MOVE_PTE_SWAP &&
			     observed->value == current->value;
	if (!*out_entry_matches || !candidate->found || candidate->entry != current->value) {
		return 0U;
	}
	return candidate->folio;
}

static minimm_status_t minimm_uffd_move_apply_model(const minimm_uffd_move_input_t *input,
						    minimm_uffd_move_result_t *out_result)
{
	minimm_uffd_move_pte_t source_pte = {
		.kind = MINIMM_UFFD_MOVE_PTE_SWAP,
		.value = input->swap_entry,
	};
	const minimm_uffd_move_pte_t observed_pte = source_pte;
	minimm_uffd_move_pte_t replacement_pte = {
		.kind = MINIMM_UFFD_MOVE_PTE_RESIDENT,
		.value = input->replacement_folio,
	};
	minimm_uffd_move_swap_cache_t swap_cache = { 0 };
	minimm_uffd_move_candidate_t candidate = { 0 };

	minimm_uffd_move_install_resident(&source_pte, input->source_folio);
	minimm_uffd_move_install_swap(&replacement_pte, &swap_cache, input->swap_entry,
				      input->replacement_folio);
	candidate = minimm_uffd_move_lookup_swap_cache(&swap_cache, input->swap_entry);
	if (!candidate.found) {
		return MINIMM_ERROR_IO;
	}

	swap_cache = (minimm_uffd_move_swap_cache_t){ 0 };
	minimm_uffd_move_install_resident(&replacement_pte, input->replacement_folio);
	minimm_uffd_move_install_swap(&source_pte, &swap_cache, input->swap_entry,
				      input->source_folio);

	out_result->swap_entry = source_pte.value;
	out_result->expected_folio = swap_cache.folio;
	out_result->moved_folio = minimm_uffd_move_move_swap_pte(
		&observed_pte, &source_pte, &candidate, &out_result->pte_entry_matches);
	if (out_result->moved_folio == 0U) {
		return MINIMM_ERROR_IO;
	}
	out_result->folio_identity_valid = swap_cache.present &&
					   swap_cache.entry == out_result->swap_entry &&
					   swap_cache.folio == out_result->moved_folio;
	out_result->accounting_valid = out_result->folio_identity_valid &&
				       out_result->moved_folio == out_result->expected_folio;
	return MINIMM_OK;
}

minimm_status_t minimm_uffd_move_run(minimm_t *mm, minimm_note_t *note,
				     const minimm_uffd_move_input_t *input,
				     minimm_uffd_move_result_t *out_result)
{
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_result = (minimm_uffd_move_result_t){ 0 };
	if (mm == NULL || note == NULL || input == NULL || !minimm_note_belongs_to(note, mm) ||
	    minimm_note_size(note) != MINIMM_PAGE_SIZE || input->swap_entry == 0U ||
	    input->source_folio == 0U || input->replacement_folio == 0U ||
	    input->source_folio == input->replacement_folio) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	return minimm_uffd_move_apply_model(input, out_result);
}
