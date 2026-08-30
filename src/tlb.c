#include "tlb.h"

#include <stdlib.h>

typedef struct minimm_tlb_entry {
	uint64_t virtual_page;
	minimm_tlb_translation_t translation;
	bool valid;
} minimm_tlb_entry_t;

struct minimm_tlb {
	minimm_tlb_entry_t *entries;
	size_t capacity;
	minimm_tlb_stats_t stats;
};

static size_t minimm_tlb_index(const minimm_tlb_t *tlb, uint64_t virtual_page)
{
	return (size_t)(virtual_page % tlb->capacity);
}

static void minimm_tlb_entry_clear(minimm_tlb_entry_t *entry)
{
	if (entry->translation.frame != NULL) {
		minimm_frame_release(entry->translation.frame);
		entry->translation.frame = NULL;
	}
	entry->valid = false;
}

minimm_status_t minimm_tlb_create(size_t capacity, minimm_tlb_t **out_tlb)
{
	minimm_tlb_t *tlb = NULL;

	if (out_tlb == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_tlb = NULL;
	if (capacity == 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	tlb = calloc(1U, sizeof(*tlb));
	if (tlb == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	tlb->entries = calloc(capacity, sizeof(*tlb->entries));
	if (tlb->entries == NULL) {
		free(tlb);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	tlb->capacity = capacity;

	*out_tlb = tlb;
	return MINIMM_OK;
}

void minimm_tlb_destroy(minimm_tlb_t *tlb)
{
	size_t index = 0U;

	if (tlb == NULL) {
		return;
	}
	for (index = 0U; index < tlb->capacity; ++index) {
		minimm_tlb_entry_clear(&tlb->entries[index]);
	}
	free(tlb->entries);
	free(tlb);
}

bool minimm_tlb_lookup(minimm_tlb_t *tlb, minimm_vaddr_t address,
		       minimm_tlb_translation_t *out_translation)
{
	const uint64_t virtual_page = address >> MINIMM_PAGE_SHIFT;
	minimm_tlb_entry_t *entry = NULL;

	if (out_translation == NULL) {
		return false;
	}
	out_translation->frame = NULL;
	out_translation->protection = 0U;
	out_translation->flags = 0U;
	if (tlb == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return false;
	}

	entry = &tlb->entries[minimm_tlb_index(tlb, virtual_page)];
	if (!entry->valid || entry->virtual_page != virtual_page) {
		tlb->stats.misses += 1U;
		return false;
	}
	if (!minimm_frame_is_resident(entry->translation.frame)) {
		minimm_tlb_entry_clear(entry);
		tlb->stats.misses += 1U;
		tlb->stats.invalidations += 1U;
		return false;
	}
	tlb->stats.hits += 1U;
	minimm_frame_retain(entry->translation.frame);
	*out_translation = entry->translation;
	return true;
}

void minimm_tlb_translation_release(minimm_tlb_translation_t *translation)
{
	if (translation == NULL) {
		return;
	}
	minimm_frame_release(translation->frame);
	translation->frame = NULL;
	translation->protection = 0U;
	translation->flags = 0U;
}

void minimm_tlb_insert(minimm_tlb_t *tlb, minimm_vaddr_t address, const minimm_pte_t *pte)
{
	const uint64_t virtual_page = address >> MINIMM_PAGE_SHIFT;
	minimm_tlb_entry_t *entry = NULL;

	if (tlb == NULL || pte == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return;
	}

	entry = &tlb->entries[minimm_tlb_index(tlb, virtual_page)];
	if (entry->valid && entry->virtual_page != virtual_page) {
		tlb->stats.replacements += 1U;
	}
	minimm_frame_retain(pte->frame);
	minimm_tlb_entry_clear(entry);
	entry->virtual_page = virtual_page;
	entry->translation.frame = pte->frame;
	entry->translation.protection = pte->protection;
	entry->translation.flags = pte->flags;
	entry->valid = true;
}

void minimm_tlb_invalidate_page(minimm_tlb_t *tlb, minimm_vaddr_t address)
{
	const uint64_t virtual_page = address >> MINIMM_PAGE_SHIFT;
	minimm_tlb_entry_t *entry = NULL;

	if (tlb == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return;
	}
	entry = &tlb->entries[minimm_tlb_index(tlb, virtual_page)];
	if (entry->valid && entry->virtual_page == virtual_page) {
		minimm_tlb_entry_clear(entry);
		tlb->stats.invalidations += 1U;
	}
}

void minimm_tlb_invalidate_range(minimm_tlb_t *tlb, minimm_vaddr_t start, minimm_vaddr_t end)
{
	size_t index = 0U;
	const uint64_t first_page = start >> MINIMM_PAGE_SHIFT;
	const uint64_t last_page = (end + MINIMM_PAGE_SIZE - 1U) >> MINIMM_PAGE_SHIFT;

	if (tlb == NULL || start >= end || end > MINIMM_USER_ADDRESS_LIMIT) {
		return;
	}

	for (index = 0U; index < tlb->capacity; ++index) {
		minimm_tlb_entry_t *entry = &tlb->entries[index];

		if (entry->valid && entry->virtual_page >= first_page &&
		    entry->virtual_page < last_page) {
			minimm_tlb_entry_clear(entry);
			tlb->stats.invalidations += 1U;
		}
	}
}

void minimm_tlb_flush(minimm_tlb_t *tlb)
{
	size_t index = 0U;

	if (tlb == NULL) {
		return;
	}
	for (index = 0U; index < tlb->capacity; ++index) {
		if (tlb->entries[index].valid) {
			minimm_tlb_entry_clear(&tlb->entries[index]);
			tlb->stats.invalidations += 1U;
		}
	}
}

void minimm_tlb_get_stats(const minimm_tlb_t *tlb, minimm_tlb_stats_t *out_stats)
{
	if (tlb != NULL && out_stats != NULL) {
		*out_stats = tlb->stats;
	}
}
