#include "page_table.h"
#include "tlb.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

int main(void)
{
	const minimm_vaddr_t first_address = UINT64_C(0x1000);
	const minimm_vaddr_t distant_address = (UINT64_C(1) << 39U) + UINT64_C(0x2000);
	minimm_frame_store_t *store = NULL;
	minimm_frame_t *first_frame = NULL;
	minimm_frame_t *second_frame = NULL;
	minimm_page_table_t *table = NULL;
	minimm_tlb_t *tlb = NULL;
	minimm_tlb_translation_t translation = { 0 };
	minimm_tlb_stats_t stats = { 0 };
	minimm_pte_t *pte = NULL;
	minimm_vaddr_t found_address = 0U;
	bool paged_in = false;
	minimm_tlb_t *invalid_tlb = (minimm_tlb_t *)(uintptr_t)1U;

	if (!check(minimm_tlb_create(0U, &invalid_tlb) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject an empty TLB") ||
	    !check(invalid_tlb == NULL, "failed TLB creation clears output")) {
		return EXIT_FAILURE;
	}

	if (!check(minimm_frame_store_create(2U, &store) == MINIMM_OK, "create frame store") ||
	    !check(minimm_frame_create_zero(store, &first_frame) == MINIMM_OK,
		   "create first frame") ||
	    !check(minimm_frame_create_zero(store, &second_frame) == MINIMM_OK,
		   "create second frame") ||
	    !check(minimm_page_table_create(&table) == MINIMM_OK, "create page table") ||
	    !check(minimm_tlb_create(2U, &tlb) == MINIMM_OK, "create TLB")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	if (!check(minimm_page_table_map(table, first_address, first_frame, UINT32_C(3), 0U) ==
			   MINIMM_OK,
		   "map first page") ||
	    !check(minimm_page_table_map(table, distant_address, second_frame,
					 MINIMM_PROT_READ | MINIMM_PROT_WRITE,
					 MINIMM_PTE_COW) == MINIMM_OK,
		   "map page through a different top-level slot") ||
	    !check(minimm_page_table_mapping_count(table) == 2U, "page table counts mappings") ||
	    !check(minimm_frame_ensure_resident(first_frame, &paged_in) == MINIMM_OK && paged_in,
		   "make the cached translation resident") ||
	    !check(minimm_frame_mapping_count(first_frame) == 1U &&
			   minimm_frame_mapping_count(second_frame) == 1U,
		   "frame mapcount tracks installed PTEs") ||
	    !check(minimm_page_table_map(table, MINIMM_USER_ADDRESS_LIMIT, second_frame, 0U, 0U) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "addresses outside the low canonical half are rejected") ||
	    !check(minimm_page_table_map(table, first_address, second_frame, UINT32_C(3), 0U) ==
			   MINIMM_ERROR_ADDRESS_IN_USE,
		   "duplicate mapping is rejected")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	translation.frame = first_frame;
	translation.protection = UINT32_MAX;
	translation.flags = UINT32_MAX;
	if (!check(!minimm_tlb_lookup(NULL, first_address, &translation),
		   "reject a lookup without a TLB") ||
	    !check(translation.frame == NULL && translation.protection == 0U &&
			   translation.flags == 0U,
		   "failed TLB lookup clears output")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	pte = minimm_page_table_lookup(table, first_address + 31U);
	if (!check(pte != NULL, "lookup accepts a page offset") ||
	    !check(pte->frame == first_frame, "lookup returns the mapped frame") ||
	    !check(!minimm_tlb_lookup(tlb, first_address, &translation),
		   "first TLB lookup misses")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	pte = minimm_page_table_lookup(table, distant_address);
	if (!check(pte != NULL && pte->protection == MINIMM_PROT_READ,
		   "COW mapping has write-protected effective permissions") ||
	    !check(minimm_page_table_find_next(table, 0U, distant_address, &found_address) ==
				   MINIMM_OK &&
			   found_address == first_address,
		   "sparse cursor finds the first allocated PTE") ||
	    !check(minimm_page_table_find_next(table, first_address + MINIMM_PAGE_SIZE,
					       distant_address,
					       &found_address) == MINIMM_ERROR_NOT_FOUND,
		   "sparse cursor skips a large empty range") ||
	    !check(minimm_page_table_find_next(table, distant_address, MINIMM_USER_ADDRESS_LIMIT,
					       &found_address) == MINIMM_OK &&
			   found_address == distant_address,
		   "sparse cursor finds a distant allocated PTE") ||
	    !check(minimm_page_table_find_next(table, MINIMM_USER_ADDRESS_LIMIT,
					       MINIMM_USER_ADDRESS_LIMIT,
					       &found_address) == MINIMM_ERROR_NOT_FOUND,
		   "sparse cursor treats an empty boundary range as exhausted")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	pte = minimm_page_table_lookup(table, first_address);

	if (!check(minimm_page_table_replace_frame(table, first_address, second_frame, UINT32_C(3),
						   0U, MINIMM_PTE_PRESENT) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "PTE replacement cannot clear the present invariant")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	if (!check(minimm_page_table_replace_frame(table, first_address, second_frame, UINT32_C(3),
						   0U, 0U) == MINIMM_OK &&
			   minimm_frame_mapping_count(first_frame) == 0U &&
			   minimm_frame_mapping_count(second_frame) == 2U,
		   "frame replacement transfers the PTE mapcount") ||
	    !check(minimm_page_table_replace_frame(table, first_address, first_frame, UINT32_C(3),
						   0U, 0U) == MINIMM_OK &&
			   minimm_frame_mapping_count(first_frame) == 1U &&
			   minimm_frame_mapping_count(second_frame) == 1U,
		   "replacing the original frame restores both mapcounts")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	pte = minimm_page_table_lookup(table, first_address);

	minimm_tlb_insert(tlb, first_address, pte);
	if (!check(minimm_tlb_lookup(tlb, first_address + 31U, &translation),
		   "cached translation hits") ||
	    !check(translation.frame == first_frame, "TLB retains the translated frame identity")) {
		minimm_tlb_translation_release(&translation);
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_tlb_translation_release(&translation);

	if (!check(minimm_page_table_update_flags(table, distant_address, MINIMM_PTE_ACCESSED,
						  0U) == MINIMM_OK,
		   "mutate an unrelated PTE") ||
	    !check(minimm_tlb_lookup(tlb, first_address, &translation),
		   "unrelated PTE mutation preserves a valid TLB entry")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_tlb_translation_release(&translation);

	if (!check(minimm_page_table_protect(table, first_address, UINT32_C(1)) == MINIMM_OK,
		   "change PTE protection")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_tlb_invalidate_page(tlb, first_address);
	if (!check(!minimm_tlb_lookup(tlb, first_address, &translation),
		   "explicit page invalidation rejects stale TLB permissions")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	pte = minimm_page_table_lookup(table, first_address);
	minimm_tlb_insert(tlb, first_address, pte);
	minimm_tlb_invalidate_page(tlb, first_address);
	if (!check(!minimm_tlb_lookup(tlb, first_address, &translation),
		   "invalidated translation misses") ||
	    !check(minimm_page_table_unmap(table, first_address) == MINIMM_OK,
		   "unmap first page") ||
	    !check(minimm_page_table_lookup(table, first_address) == NULL,
		   "unmapped page is absent") ||
	    !check(minimm_page_table_mapping_count(table) == 1U, "unmap updates mapping count") ||
	    !check(minimm_frame_mapping_count(first_frame) == 0U,
		   "unmap drops the frame mapcount")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	minimm_tlb_get_stats(tlb, &stats);
	if (!check(stats.hits == 2U, "TLB hits are counted") ||
	    !check(stats.misses == 3U, "TLB misses are counted") ||
	    !check(stats.invalidations == 2U, "TLB invalidations are counted")) {
		minimm_tlb_destroy(tlb);
		minimm_page_table_destroy(table);
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	minimm_tlb_destroy(tlb);
	minimm_page_table_destroy(table);
	if (!check(minimm_frame_mapping_count(second_frame) == 0U,
		   "page-table destruction drops remaining frame mapcounts")) {
		minimm_frame_release(second_frame);
		minimm_frame_release(first_frame);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_frame_release(second_frame);
	minimm_frame_release(first_frame);
	minimm_frame_store_destroy(store);
	return EXIT_SUCCESS;
}
