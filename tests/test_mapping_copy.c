#include "minimm/minimm.h"
#include "../src/space.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_TEST_HINT UINT64_C(0x400000)

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool copy_metadata_matches(const minimm_mapping_info_t *source,
				  const minimm_mapping_info_t *copy, minimm_vaddr_t copy_address)
{
	return copy->start == copy_address &&
	       copy->end - copy->start == source->end - source->start &&
	       copy->mapping_cookie != UINT64_C(0) &&
	       copy->mapping_cookie != source->mapping_cookie &&
	       copy->note_offset == source->note_offset && copy->protection == source->protection &&
	       copy->maximum_protection == source->maximum_protection &&
	       copy->flags == source->flags;
}

static bool failed_copy_clears_output(minimm_space_t *space, minimm_vaddr_t source, uint64_t length,
				      minimm_vaddr_t hint, minimm_status_t expected,
				      const char *message)
{
	minimm_vaddr_t output = UINT64_C(0);
	const minimm_status_t status = minimm_mapping_copy(space, source, length, hint, &output);

	return check(status == expected && output == MINIMM_ADDRESS_AUTO, message);
}

static bool copied_ptes_have_linux_cow_state(minimm_space_t *space, minimm_vaddr_t source,
					     minimm_vaddr_t copy)
{
	const minimm_pte_t *source_pte = NULL;
	const minimm_pte_t *copy_pte = NULL;
	bool matches = false;

	(void)pthread_mutex_lock(&space->lock);
	source_pte = minimm_page_table_lookup_const(space->page_table, source);
	copy_pte = minimm_page_table_lookup_const(space->page_table, copy);
	if (source_pte != NULL && copy_pte != NULL) {
		const minimm_prot_t writable = MINIMM_PROT_WRITE | MINIMM_PROT_EDIT;

		matches = (source_pte->flags & (MINIMM_PTE_COW | MINIMM_PTE_ACCESSED |
						MINIMM_PTE_DIRTY | MINIMM_PTE_LOCKED)) ==
				  (MINIMM_PTE_COW | MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY |
				   MINIMM_PTE_LOCKED) &&
			  (source_pte->flags & MINIMM_PTE_SHARED) == 0U &&
			  (copy_pte->flags & MINIMM_PTE_COW) != 0U &&
			  (copy_pte->flags & (MINIMM_PTE_SHARED | MINIMM_PTE_ACCESSED |
					      MINIMM_PTE_DIRTY | MINIMM_PTE_LOCKED)) == 0U &&
			  (source_pte->protection & writable) == 0U &&
			  (copy_pte->protection & writable) == 0U;
	}
	(void)pthread_mutex_unlock(&space->lock);
	return matches;
}

static bool test_mapping_copy(void)
{
	static const uint8_t original[] = { UINT8_C(0x10), UINT8_C(0x21), UINT8_C(0x32),
					    UINT8_C(0x43), UINT8_C(0x54), UINT8_C(0x65),
					    UINT8_C(0x76), UINT8_C(0x87) };
	static const uint8_t destination_value[] = { UINT8_C(0x98), UINT8_C(0xa9), UINT8_C(0xba),
						     UINT8_C(0xcb), UINT8_C(0xdc), UINT8_C(0xed),
						     UINT8_C(0xfe), UINT8_C(0x0f) };
	static const uint8_t source_value[] = { UINT8_C(0xf0), UINT8_C(0xe1), UINT8_C(0xd2),
						UINT8_C(0xc3), UINT8_C(0xb4), UINT8_C(0xa5),
						UINT8_C(0x96), UINT8_C(0x87) };
	const uint64_t mapping_length = MINIMM_PAGE_SIZE * UINT64_C(2);
	const uint64_t value_offset = UINT64_C(37);
	const uint64_t second_page_offset = MINIMM_PAGE_SIZE + UINT64_C(19);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t source = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t blocker = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t shared = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t copy = MINIMM_ADDRESS_AUTO;
	minimm_mapping_info_t source_mapping = { 0 };
	minimm_mapping_info_t copy_mapping = { 0 };
	minimm_mapping_info_t blocker_before = { 0 };
	minimm_mapping_info_t blocker_after = { 0 };
	minimm_page_info_t source_page = { 0 };
	minimm_page_info_t copy_page = { 0 };
	minimm_page_info_t source_unfaulted = { 0 };
	minimm_page_info_t copy_unfaulted = { 0 };
	minimm_system_stats_t system_before = { 0 };
	minimm_system_stats_t system_after = { 0 };
	minimm_space_stats_t space_before = { 0 };
	minimm_space_stats_t space_after = { 0 };
	uint8_t source_core[2] = { UINT8_C(0), UINT8_C(0) };
	uint8_t copy_core[2] = { UINT8_C(0), UINT8_C(0) };
	uint8_t buffer[sizeof(original)] = { 0 };
	uint8_t byte = UINT8_C(0xff);
	const uint8_t destination_second = UINT8_C(0xd4);
	const uint8_t source_second = UINT8_C(0x5a);
	size_t completed = 0U;
	minimm_mmap_args_t source_args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = mapping_length,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	minimm_mmap_args_t blocker_args = {
		.address_hint = COPY_TEST_HINT,
		.length = mapping_length,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_READ,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
		.note = NULL,
	};
	minimm_mmap_args_t shared_args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 8U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create copy test system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create copy test space") ||
	    !check(minimm_mmap(space, &source_args, &source) == MINIMM_OK,
		   "map private anonymous source") ||
	    !check(minimm_mmap(space, &blocker_args, &blocker) == MINIMM_OK &&
			   blocker == COPY_TEST_HINT,
		   "map the occupied destination hint") ||
	    !check(minimm_mmap(space, &shared_args, &shared) == MINIMM_OK,
		   "map unsupported shared source") ||
	    !check(minimm_write(space, source + value_offset, original, sizeof(original),
				&completed) == MINIMM_OK &&
			   completed == sizeof(original),
		   "fault and initialize the source page") ||
	    !check(minimm_mlock(space, source, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock the source page") ||
	    !check(minimm_read(space, source + value_offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, original, sizeof(original)) == 0,
		   "warm the source TLB before copying") ||
	    !check(minimm_mapping_query(space, source, &source_mapping) == MINIMM_OK &&
			   source_mapping.flags == MINIMM_MAP_PRIVATE,
		   "query source metadata") ||
	    !check(minimm_mapping_query(space, blocker, &blocker_before) == MINIMM_OK,
		   "query occupied hint metadata") ||
	    !check(minimm_query_page(space, source + MINIMM_PAGE_SIZE, &source_unfaulted) ==
				   MINIMM_OK &&
			   !source_unfaulted.present && source_unfaulted.pfn == MINIMM_PFN_NONE,
		   "leave the second source page unfaulted") ||
	    !check(minimm_system_get_stats(mm, &system_before) == MINIMM_OK &&
			   minimm_space_get_stats(space, &space_before) == MINIMM_OK,
		   "capture pre-copy allocation statistics")) {
		goto done;
	}

	if (!failed_copy_clears_output(NULL, source, mapping_length, MINIMM_ADDRESS_AUTO,
				       MINIMM_ERROR_INVALID_ARGUMENT,
				       "reject a null address space") ||
	    !check(minimm_mapping_copy(space, source, mapping_length, MINIMM_ADDRESS_AUTO, NULL) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null output") ||
	    !failed_copy_clears_output(space, source, UINT64_C(0), MINIMM_ADDRESS_AUTO,
				       MINIMM_ERROR_INVALID_ARGUMENT,
				       "reject a zero copy length") ||
	    !failed_copy_clears_output(space, source, UINT64_MAX, MINIMM_ADDRESS_AUTO,
				       MINIMM_ERROR_INVALID_ARGUMENT,
				       "reject an overflowing copy length") ||
	    !failed_copy_clears_output(space, source + UINT64_C(1), mapping_length,
				       MINIMM_ADDRESS_AUTO, MINIMM_ERROR_INVALID_ARGUMENT,
				       "reject an unaligned source") ||
	    !failed_copy_clears_output(space, source, mapping_length, COPY_TEST_HINT + UINT64_C(1),
				       MINIMM_ERROR_INVALID_ARGUMENT,
				       "reject an unaligned destination hint") ||
	    !failed_copy_clears_output(
		    space, source, mapping_length, MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE,
		    MINIMM_ERROR_INVALID_ARGUMENT, "reject a destination hint that cannot fit") ||
	    !failed_copy_clears_output(space, source, MINIMM_PAGE_SIZE, MINIMM_ADDRESS_AUTO,
				       MINIMM_ERROR_UNSUPPORTED,
				       "reject a partial source mapping") ||
	    !failed_copy_clears_output(space, source + MINIMM_PAGE_SIZE, mapping_length,
				       MINIMM_ADDRESS_AUTO, MINIMM_ERROR_UNSUPPORTED,
				       "reject a copy starting inside a mapping") ||
	    !failed_copy_clears_output(space, UINT64_C(0x800000), MINIMM_PAGE_SIZE,
				       MINIMM_ADDRESS_AUTO, MINIMM_ERROR_UNSUPPORTED,
				       "reject an unmapped source") ||
	    !failed_copy_clears_output(space, shared, MINIMM_PAGE_SIZE, MINIMM_ADDRESS_AUTO,
				       MINIMM_ERROR_UNSUPPORTED, "reject a shared mapping") ||
	    !check(minimm_system_get_stats(mm, &system_after) == MINIMM_OK &&
			   minimm_space_get_stats(space, &space_after) == MINIMM_OK &&
			   system_after.frame_count == system_before.frame_count &&
			   system_after.resident_count == system_before.resident_count &&
			   space_after.vma_count == space_before.vma_count &&
			   space_after.pte_count == space_before.pte_count,
		   "failed copies do not change mappings or frames")) {
		goto done;
	}

	if (!check(minimm_mapping_copy(space, source, MINIMM_PAGE_SIZE + UINT64_C(1),
				       COPY_TEST_HINT, &copy) == MINIMM_OK,
		   "copy the complete rounded private mapping") ||
	    !check(copy == COPY_TEST_HINT + mapping_length,
		   "occupied hint falls forward to the next gap") ||
	    !check(minimm_mapping_query(space, source, &source_mapping) == MINIMM_OK &&
			   minimm_mapping_query(space, copy, &copy_mapping) == MINIMM_OK &&
			   copy_metadata_matches(&source_mapping, &copy_mapping, copy),
		   "copy preserves metadata and receives a new cookie") ||
	    !check(minimm_mapping_query(space, blocker, &blocker_after) == MINIMM_OK &&
			   blocker_after.start == blocker_before.start &&
			   blocker_after.end == blocker_before.end &&
			   blocker_after.mapping_cookie == blocker_before.mapping_cookie,
		   "hint fallback preserves the occupied mapping") ||
	    !check(minimm_system_get_stats(mm, &system_after) == MINIMM_OK &&
			   minimm_space_get_stats(space, &space_after) == MINIMM_OK &&
			   system_after.frame_count == system_before.frame_count &&
			   system_after.resident_count == system_before.resident_count &&
			   space_after.vma_count == space_before.vma_count + 1U &&
			   space_after.pte_count == space_before.pte_count + 1U,
		   "copy shares present PTEs without eager frame allocation") ||
	    !check(minimm_query_page(space, source, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy, &copy_page) == MINIMM_OK &&
			   source_page.present && copy_page.present &&
			   source_page.pfn == copy_page.pfn && source_page.cow && copy_page.cow &&
			   !source_page.shared && !copy_page.shared && source_page.accessed &&
			   !copy_page.accessed,
		   "present private page is shared by PFN under COW") ||
	    !check(copied_ptes_have_linux_cow_state(space, source, copy),
		   "copy starts old, clean, unlocked, and effectively write-protected") ||
	    !check(minimm_query_page(space, source + MINIMM_PAGE_SIZE, &source_unfaulted) ==
				   MINIMM_OK &&
			   minimm_query_page(space, copy + MINIMM_PAGE_SIZE, &copy_unfaulted) ==
				   MINIMM_OK &&
			   !source_unfaulted.present && !copy_unfaulted.present &&
			   source_unfaulted.pfn == MINIMM_PFN_NONE &&
			   copy_unfaulted.pfn == MINIMM_PFN_NONE,
		   "copy leaves unfaulted pages absent") ||
	    !check(minimm_mincore(space, source, mapping_length, source_core, 2U) == MINIMM_OK &&
			   minimm_mincore(space, copy, mapping_length, copy_core, 2U) ==
				   MINIMM_OK &&
			   (source_core[0] & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_COW |
					      MINIMM_MINCORE_LOCKED)) ==
				   (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_COW |
				    MINIMM_MINCORE_LOCKED) &&
			   (copy_core[0] & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_COW)) ==
				   (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_COW) &&
			   (copy_core[0] & MINIMM_MINCORE_LOCKED) == UINT8_C(0) &&
			   (source_core[1] & MINIMM_MINCORE_PRESENT) == UINT8_C(0) &&
			   (copy_core[1] & MINIMM_MINCORE_PRESENT) == UINT8_C(0),
		   "copy retains the source lock without inheriting it")) {
		goto done;
	}

	if (!check(minimm_write(space, copy + value_offset, destination_value,
				sizeof(destination_value), &completed) == MINIMM_OK &&
			   completed == sizeof(destination_value),
		   "write the destination through COW") ||
	    !check(minimm_query_page(space, source, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy, &copy_page) == MINIMM_OK &&
			   source_page.pfn != copy_page.pfn && source_page.cow && !copy_page.cow,
		   "destination write installs a distinct PFN") ||
	    !check(minimm_read(space, source + value_offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, original, sizeof(original)) == 0,
		   "destination write preserves source bytes") ||
	    !check(minimm_read(space, copy + value_offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, destination_value, sizeof(destination_value)) == 0,
		   "destination exposes its copied bytes") ||
	    !check(minimm_write(space, source + value_offset, source_value, sizeof(source_value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(source_value),
		   "write the TLB-warm source through COW") ||
	    !check(minimm_query_page(space, source, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy, &copy_page) == MINIMM_OK &&
			   source_page.pfn != copy_page.pfn && !source_page.cow && !copy_page.cow,
		   "source write cannot bypass COW through a stale TLB entry") ||
	    !check(minimm_read(space, source + value_offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, source_value, sizeof(source_value)) == 0,
		   "source exposes its private update") ||
	    !check(minimm_read(space, copy + value_offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, destination_value, sizeof(destination_value)) == 0,
		   "source update preserves destination bytes") ||
	    !check(minimm_mincore(space, source, MINIMM_PAGE_SIZE, source_core, 1U) == MINIMM_OK &&
			   minimm_mincore(space, copy, MINIMM_PAGE_SIZE, copy_core, 1U) ==
				   MINIMM_OK &&
			   (source_core[0] & MINIMM_MINCORE_LOCKED) != UINT8_C(0) &&
			   (copy_core[0] & MINIMM_MINCORE_LOCKED) == UINT8_C(0),
		   "source COW retains only the source memory lock")) {
		goto done;
	}

	if (!check(minimm_write(space, copy + second_page_offset, &destination_second,
				sizeof(destination_second), &completed) == MINIMM_OK &&
			   completed == sizeof(destination_second),
		   "fault the copied second page") ||
	    !check((byte = UINT8_C(0xff), minimm_read(space, source + second_page_offset, &byte,
						      sizeof(byte), &completed)) == MINIMM_OK &&
			   completed == sizeof(byte) && byte == UINT8_C(0),
		   "unfaulted source page remains zero after destination write") ||
	    !check(minimm_write(space, source + second_page_offset, &source_second,
				sizeof(source_second), &completed) == MINIMM_OK &&
			   completed == sizeof(source_second),
		   "write the independently faulted source page") ||
	    !check((byte = UINT8_C(0), minimm_read(space, copy + second_page_offset, &byte,
						   sizeof(byte), &completed)) == MINIMM_OK &&
			   completed == sizeof(byte) && byte == destination_second,
		   "source second-page write preserves destination bytes") ||
	    !check(minimm_query_page(space, source + MINIMM_PAGE_SIZE, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy + MINIMM_PAGE_SIZE, &copy_page) ==
				   MINIMM_OK &&
			   source_page.present && copy_page.present &&
			   source_page.pfn != copy_page.pfn && !source_page.cow && !copy_page.cow,
		   "previously unfaulted pages allocate independent PFNs") ||
	    !check(minimm_munlock(space, source, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "unlock the source page")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_readonly_mapping_copy(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t source = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t copy = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t source_page = { 0 };
	minimm_page_info_t copy_page = { 0 };
	uint8_t source_core = UINT8_C(0);
	uint8_t copy_core = UINT8_C(0);
	uint8_t value = UINT8_C(0xff);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_READ,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create readonly copy system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create readonly copy space") ||
	    !check(minimm_mmap(space, &args, &source) == MINIMM_OK,
		   "map permanently readonly private source") ||
	    !check(minimm_read(space, source, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value) && value == UINT8_C(0),
		   "fault the readonly source") ||
	    !check(minimm_mapping_copy(space, source, MINIMM_PAGE_SIZE, MINIMM_ADDRESS_AUTO,
				       &copy) == MINIMM_OK,
		   "copy the readonly private mapping") ||
	    !check(minimm_query_page(space, source, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy, &copy_page) == MINIMM_OK &&
			   source_page.present && copy_page.present &&
			   source_page.pfn == copy_page.pfn && !source_page.cow && !copy_page.cow &&
			   source_page.protection == MINIMM_PROT_READ &&
			   copy_page.protection == MINIMM_PROT_READ,
		   "readonly copy shares its frame without spurious COW state") ||
	    !check(minimm_mincore(space, source, MINIMM_PAGE_SIZE, &source_core, 1U) == MINIMM_OK &&
			   minimm_mincore(space, copy, MINIMM_PAGE_SIZE, &copy_core, 1U) ==
				   MINIMM_OK &&
			   (source_core & MINIMM_MINCORE_COW) == UINT8_C(0) &&
			   (copy_core & MINIMM_MINCORE_COW) == UINT8_C(0),
		   "mincore does not diagnose unreachable COW faults") ||
	    !check(minimm_mprotect(space, copy, MINIMM_PAGE_SIZE,
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_ERROR_PERMISSION,
		   "readonly copy cannot acquire write permission")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_sparse_mapping_copy(void)
{
	const uint64_t sparse_mapping_length = UINT64_C(1) << 40U;
	const uint64_t sparse_offset = sparse_mapping_length - MINIMM_PAGE_SIZE;
	const uint8_t source_value = UINT8_C(0x39);
	const uint8_t copy_value = UINT8_C(0xc7);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t source = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t copy = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t source_page = { 0 };
	minimm_page_info_t copy_page = { 0 };
	uint8_t observed = UINT8_C(0);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = sparse_mapping_length,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create sparse copy system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create sparse copy space") ||
	    !check(minimm_mmap(space, &args, &source) == MINIMM_OK,
		   "map a one-terabyte sparse source") ||
	    !check(minimm_write(space, source + sparse_offset, &source_value, sizeof(source_value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(source_value),
		   "fault one page near the end of the sparse source") ||
	    !check(minimm_mapping_copy(space, source, sparse_mapping_length, MINIMM_ADDRESS_AUTO,
				       &copy) == MINIMM_OK,
		   "copy a large sparse mapping without scanning every virtual page") ||
	    !check(minimm_query_page(space, source + sparse_offset, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy + sparse_offset, &copy_page) ==
				   MINIMM_OK &&
			   source_page.present && copy_page.present && source_page.cow &&
			   copy_page.cow && source_page.pfn == copy_page.pfn,
		   "sparse copy shares only the present far page under COW") ||
	    !check(minimm_write(space, copy + sparse_offset, &copy_value, sizeof(copy_value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(copy_value),
		   "split the sparse destination page on write") ||
	    !check(minimm_read(space, source + sparse_offset, &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == source_value,
		   "sparse destination write preserves the source")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_note_backed_mapping_copy(void)
{
	const minimm_vaddr_t destination_hint = UINT64_C(0x800000);
	const uint64_t value_offset = UINT64_C(113);
	const uint8_t original = UINT8_C(0x2d);
	const uint8_t changed = UINT8_C(0xe4);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t source = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t copy = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t source_page = { 0 };
	minimm_page_info_t copy_page = { 0 };
	uint8_t observed = UINT8_C(0);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create note copy system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_ALL, &note) ==
			   MINIMM_OK,
		   "create note-backed copy source") ||
	    !check(minimm_note_pwrite(note, value_offset, &original, sizeof(original),
				      &completed) == MINIMM_OK &&
			   completed == sizeof(original),
		   "initialize note-backed source bytes")) {
		goto done;
	}
	args.note = note;
	if (!check(minimm_space_create(mm, &space) == MINIMM_OK, "create note-backed copy space") ||
	    !check(minimm_mmap(space, &args, &source) == MINIMM_OK,
		   "map the private note-backed source")) {
		goto done;
	}
	minimm_note_release(note);
	note = NULL;

	if (!check(minimm_read(space, source + value_offset, &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == original,
		   "fault the private note-backed source") ||
	    !check(minimm_mapping_copy(space, source, MINIMM_PAGE_SIZE, destination_hint, &copy) ==
				   MINIMM_OK &&
			   copy == destination_hint,
		   "honor an empty destination hint for a note-backed copy") ||
	    !check(minimm_query_page(space, source, &source_page) == MINIMM_OK &&
			   minimm_query_page(space, copy, &copy_page) == MINIMM_OK &&
			   source_page.present && copy_page.present && source_page.cow &&
			   copy_page.cow && source_page.pfn == copy_page.pfn,
		   "share a present note-backed frame under COW") ||
	    !check(minimm_write(space, copy + value_offset, &changed, sizeof(changed),
				&completed) == MINIMM_OK &&
			   completed == sizeof(changed),
		   "split the note-backed destination on write") ||
	    !check(minimm_read(space, source + value_offset, &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == original,
		   "note-backed COW preserves the source bytes")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_note_release(note);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	return test_mapping_copy() && test_readonly_mapping_copy() && test_sparse_mapping_copy() &&
			       test_note_backed_mapping_copy() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
