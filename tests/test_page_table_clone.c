#include "page_table.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

enum {
	TEST_PAGE_COUNT = 5
};

typedef struct visit_record {
	minimm_vaddr_t addresses[TEST_PAGE_COUNT];
	minimm_frame_t *frames[TEST_PAGE_COUNT];
	size_t count;
} visit_record_t;

typedef struct clone_context {
	minimm_page_table_t *source;
	minimm_page_table_t *destination;
	size_t count;
} clone_context_t;

typedef struct stop_context {
	size_t count;
	size_t stop_after;
} stop_context_t;

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static minimm_status_t record_mapping(minimm_vaddr_t page_address, const minimm_pte_t *pte,
				      void *opaque)
{
	visit_record_t *record = opaque;

	if (record->count >= TEST_PAGE_COUNT) {
		return MINIMM_ERROR_NO_SPACE;
	}
	record->addresses[record->count] = page_address;
	record->frames[record->count] = pte->frame;
	record->count += 1U;
	return MINIMM_OK;
}

static minimm_status_t clone_mapping_as_cow(minimm_vaddr_t page_address, const minimm_pte_t *pte,
					    void *opaque)
{
	clone_context_t *context = opaque;
	minimm_status_t status = MINIMM_OK;

	status = minimm_page_table_map(context->destination, page_address, pte->frame,
				       pte->protection, pte->flags | MINIMM_PTE_COW);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_page_table_update_attributes(context->source, page_address, pte->protection,
						     MINIMM_PTE_COW, MINIMM_PTE_DIRTY);
	if (status == MINIMM_OK) {
		context->count += 1U;
	}
	return status;
}

static minimm_status_t stop_mapping(minimm_vaddr_t page_address, const minimm_pte_t *pte,
				    void *opaque)
{
	stop_context_t *context = opaque;

	(void)page_address;
	(void)pte;
	context->count += 1U;
	return context->count == context->stop_after ? MINIMM_ERROR_BUSY : MINIMM_OK;
}

int main(void)
{
	const minimm_vaddr_t expected_addresses[TEST_PAGE_COUNT] = {
		UINT64_C(0x3000), (UINT64_C(1) << 21U) + UINT64_C(0x7000),
		(UINT64_C(1) << 30U) + UINT64_C(0x2000), (UINT64_C(1) << 39U) + UINT64_C(0x5000),
		MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE
	};
	const size_t insertion_order[TEST_PAGE_COUNT] = { 4U, 1U, 3U, 0U, 2U };
	minimm_frame_store_t *store = NULL;
	minimm_frame_t *frames[TEST_PAGE_COUNT] = { NULL };
	minimm_page_table_t *source = NULL;
	minimm_page_table_t *clone = NULL;
	visit_record_t record = { 0 };
	clone_context_t clone_context = { 0 };
	stop_context_t stop_context = { 0 };
	size_t index = 0U;
	bool success = true;

	if (!check(minimm_frame_store_create(TEST_PAGE_COUNT, &store) == MINIMM_OK,
		   "create frame store") ||
	    !check(minimm_page_table_create(&source) == MINIMM_OK, "create source page table") ||
	    !check(minimm_page_table_create(&clone) == MINIMM_OK,
		   "create destination page table")) {
		success = false;
		goto cleanup;
	}

	for (index = 0U; index < TEST_PAGE_COUNT; ++index) {
		const size_t mapping_index = insertion_order[index];

		if (!check(minimm_frame_create_zero(store, &frames[mapping_index]) == MINIMM_OK,
			   "create sparse mapping frame") ||
		    !check(minimm_page_table_map(source, expected_addresses[mapping_index],
						 frames[mapping_index],
						 MINIMM_PROT_READ | MINIMM_PROT_WRITE,
						 MINIMM_PTE_DIRTY) == MINIMM_OK,
			   "map sparse page in shuffled order")) {
			success = false;
			goto cleanup;
		}
	}

	if (!check(minimm_page_table_for_each(source, record_mapping, &record) == MINIMM_OK,
		   "walk all sparse mappings") ||
	    !check(record.count == TEST_PAGE_COUNT, "walk visits every mapping")) {
		success = false;
		goto cleanup;
	}
	for (index = 0U; index < TEST_PAGE_COUNT; ++index) {
		if (!check(record.addresses[index] == expected_addresses[index],
			   "walk reconstructs aligned addresses in ascending order") ||
		    !check(record.frames[index] == frames[index],
			   "walk reports the matching PTE")) {
			success = false;
			goto cleanup;
		}
	}

	stop_context.stop_after = 3U;
	if (!check(minimm_page_table_for_each(source, stop_mapping, &stop_context) ==
			   MINIMM_ERROR_BUSY,
		   "walk propagates callback failure") ||
	    !check(stop_context.count == 3U, "walk stops at callback failure") ||
	    !check(minimm_page_table_for_each(NULL, record_mapping, &record) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "walk rejects a null table") ||
	    !check(minimm_page_table_for_each(source, NULL, &record) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "walk rejects a null callback")) {
		success = false;
		goto cleanup;
	}

	clone_context.source = source;
	clone_context.destination = clone;
	if (!check(minimm_page_table_for_each(source, clone_mapping_as_cow, &clone_context) ==
			   MINIMM_OK,
		   "clone mappings through the iterator") ||
	    !check(clone_context.count == TEST_PAGE_COUNT, "clone callback sees every mapping") ||
	    !check(minimm_page_table_mapping_count(clone) == TEST_PAGE_COUNT,
		   "clone contains every sparse mapping")) {
		success = false;
		goto cleanup;
	}
	for (index = 0U; index < TEST_PAGE_COUNT; ++index) {
		const minimm_pte_t *source_pte =
			minimm_page_table_lookup_const(source, expected_addresses[index]);
		const minimm_pte_t *clone_pte =
			minimm_page_table_lookup_const(clone, expected_addresses[index]);

		if (!check(source_pte != NULL, "source PTE remains mapped") ||
		    !check(clone_pte != NULL, "cloned PTE is mapped") ||
		    !check(source_pte->frame == clone_pte->frame, "clone shares frame") ||
		    !check((source_pte->flags & MINIMM_PTE_COW) != 0U &&
				   (clone_pte->flags & MINIMM_PTE_COW) != 0U,
			   "both mappings are marked COW") ||
		    !check((source_pte->flags & MINIMM_PTE_DIRTY) == 0U,
			   "attribute update clears source dirty flag")) {
			success = false;
			goto cleanup;
		}
	}

	{
		const uint64_t generation = minimm_page_table_generation(source);

		if (!check(minimm_page_table_update_attributes(
				   source, expected_addresses[0], MINIMM_PROT_READ,
				   MINIMM_PTE_ACCESSED, MINIMM_PTE_COW) == MINIMM_OK,
			   "update protection and flags together") ||
		    !check(minimm_page_table_generation(source) == generation + 1U,
			   "combined attribute update advances one generation") ||
		    !check(minimm_page_table_update_attributes(
				   source, expected_addresses[0], MINIMM_PROT_READ,
				   MINIMM_PTE_ACCESSED, MINIMM_PTE_COW) == MINIMM_OK,
			   "repeating the same attribute update succeeds") ||
		    !check(minimm_page_table_generation(source) == generation + 1U,
			   "no-op attribute update preserves generation")) {
			success = false;
			goto cleanup;
		}
	}

	if (!check(minimm_page_table_update_attributes(source, expected_addresses[0],
						       MINIMM_PROT_READ, MINIMM_PTE_PRESENT,
						       0U) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "attribute update preserves the present invariant") ||
	    !check(minimm_page_table_update_attributes(source, expected_addresses[0] + 1U,
						       MINIMM_PROT_READ, 0U,
						       0U) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "attribute update requires a page-aligned address") ||
	    !check(minimm_page_table_update_attributes(source, UINT64_C(0x9000), MINIMM_PROT_READ,
						       0U, 0U) == MINIMM_ERROR_NOT_FOUND,
		   "attribute update rejects an unmapped page")) {
		success = false;
	}

cleanup:
	minimm_page_table_destroy(clone);
	minimm_page_table_destroy(source);
	for (index = 0U; index < TEST_PAGE_COUNT; ++index) {
		minimm_frame_release(frames[index]);
	}
	minimm_frame_store_destroy(store);
	return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
