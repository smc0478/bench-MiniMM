#include "minimm/minimm.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool test_cow_under_pin_pressure(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t child_page = { 0 };
	minimm_system_stats_t system_stats = { 0 };
	const unsigned char original = UINT8_C(0x27);
	const unsigned char replacement = UINT8_C(0x91);
	unsigned char observed = UINT8_C(0);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create COW pressure system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create COW pressure parent") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map COW pressure page") ||
	    !check(minimm_write(parent, address, &original, sizeof(original), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(original),
		   "initialize COW pressure page") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork a COW alias") ||
	    !check(minimm_mlock(parent, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "pin the only resident COW frame") ||
	    !check(minimm_write(child, address, &replacement, sizeof(replacement), &completed) ==
				   MINIMM_ERROR_BUSY &&
			   completed == 0U,
		   "COW allocation fails cleanly while every resident frame is pinned") ||
	    !check(minimm_query_page(child, address, &child_page) == MINIMM_OK && child_page.cow,
		   "failed COW keeps the child PTE shared and write-protected") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   system_stats.frame_count == 1U && system_stats.resident_count == 1U,
		   "failed COW releases its transient frame") ||
	    !check(minimm_read(child, address, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == original,
		   "failed COW preserves the original bytes") ||
	    !check(minimm_munlock(parent, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "release COW pressure pin") ||
	    !check(minimm_write(child, address, &replacement, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement),
		   "COW succeeds after pin pressure is removed") ||
	    !check(minimm_read(parent, address, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == original,
		   "successful child COW preserves parent bytes")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

static bool test_write_completed_output_alias(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	const size_t expected = SIZE_MAX / 3U + 1U;
	const size_t edit_expected = SIZE_MAX / 5U + 3U;
	size_t source_and_completed = expected;
	size_t observed = 0U;
	size_t completed = 0U;
	bool passed = false;

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create access-alias system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create access-alias space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map access-alias page") ||
	    !check(minimm_write(space, address, &source_and_completed, sizeof(source_and_completed),
				&source_and_completed) == MINIMM_OK &&
			   source_and_completed == sizeof(source_and_completed),
		   "write snapshots input before updating an aliased completion output") ||
	    !check(minimm_read(space, address, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == expected,
		   "aliased write preserves the original input bytes") ||
	    !check((source_and_completed = edit_expected,
		    minimm_edit(space, address + UINT64_C(64), &source_and_completed,
				sizeof(source_and_completed), &source_and_completed)) ==
				   MINIMM_OK &&
			   source_and_completed == sizeof(source_and_completed),
		   "edit snapshots input before updating an aliased completion output") ||
	    !check(minimm_read(space, address + UINT64_C(64), &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == edit_expected,
		   "aliased edit preserves the original input bytes")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

typedef struct eviction_race_context {
	minimm_space_t *space;
	minimm_vaddr_t address;
	atomic_bool *start;
	atomic_uint *failures;
	unsigned char expected;
} eviction_race_context_t;

static void *run_eviction_race(void *opaque)
{
	eviction_race_context_t *context = opaque;
	size_t iteration = 0U;

	while (!atomic_load_explicit(context->start, memory_order_acquire)) {
	}
	for (iteration = 0U; iteration < 500000U; ++iteration) {
		unsigned char observed = UINT8_C(0);
		size_t completed = 0U;

		if (minimm_read(context->space, context->address, &observed, sizeof(observed),
				&completed) != MINIMM_OK ||
		    completed != sizeof(observed) || observed != context->expected) {
			(void)atomic_fetch_add_explicit(context->failures, 1U,
							memory_order_relaxed);
		}
	}
	return NULL;
}

static bool test_cross_space_eviction_fault_accounting(void)
{
	const unsigned char first_value = UINT8_C(0x35);
	const unsigned char second_value = UINT8_C(0xa7);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *first = NULL;
	minimm_space_t *second = NULL;
	minimm_vaddr_t first_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t second_address = MINIMM_ADDRESS_AUTO;
	minimm_system_stats_t system_stats = { 0 };
	minimm_space_stats_t first_stats = { 0 };
	minimm_space_stats_t second_stats = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	atomic_bool start = false;
	atomic_uint failures = 0U;
	eviction_race_context_t first_context = { 0 };
	eviction_race_context_t second_context = { 0 };
	pthread_t first_thread;
	pthread_t second_thread;
	size_t completed = 0U;
	bool first_started = false;
	bool second_started = false;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create eviction-race system") ||
	    !check(minimm_space_create(mm, &first) == MINIMM_OK &&
			   minimm_space_create(mm, &second) == MINIMM_OK,
		   "create independent eviction-race spaces") ||
	    !check(minimm_mmap(first, &args, &first_address) == MINIMM_OK &&
			   minimm_mmap(second, &args, &second_address) == MINIMM_OK,
		   "map competing eviction-race pages") ||
	    !check(minimm_write(first, first_address, &first_value, sizeof(first_value),
				&completed) == MINIMM_OK &&
			   minimm_write(second, second_address, &second_value, sizeof(second_value),
					&completed) == MINIMM_OK,
		   "initialize competing eviction-race pages")) {
		goto done;
	}

	first_context.space = first;
	first_context.address = first_address;
	first_context.start = &start;
	first_context.failures = &failures;
	first_context.expected = first_value;
	second_context.space = second;
	second_context.address = second_address;
	second_context.start = &start;
	second_context.failures = &failures;
	second_context.expected = second_value;
	if (!check(pthread_create(&first_thread, NULL, run_eviction_race, &first_context) == 0,
		   "start first eviction-race reader")) {
		goto done;
	}
	first_started = true;
	if (!check(pthread_create(&second_thread, NULL, run_eviction_race, &second_context) == 0,
		   "start second eviction-race reader")) {
		goto done;
	}
	second_started = true;
	atomic_store_explicit(&start, true, memory_order_release);
	(void)pthread_join(first_thread, NULL);
	first_started = false;
	(void)pthread_join(second_thread, NULL);
	second_started = false;

	if (!check(atomic_load_explicit(&failures, memory_order_relaxed) == 0U,
		   "cross-space eviction never exposes failed or corrupt reads") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   minimm_space_get_stats(first, &first_stats) == MINIMM_OK &&
			   minimm_space_get_stats(second, &second_stats) == MINIMM_OK,
		   "query eviction-race accounting") ||
	    !check(system_stats.page_in_count ==
			   first_stats.fault_sequence + second_stats.fault_sequence,
		   "every cross-space page-in passes through the fault path")) {
		goto done;
	}
	passed = true;

done:
	atomic_store_explicit(&start, true, memory_order_release);
	if (first_started) {
		(void)pthread_join(first_thread, NULL);
	}
	if (second_started) {
		(void)pthread_join(second_thread, NULL);
	}
	minimm_space_destroy(second);
	minimm_space_destroy(first);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	enum {
		DATA_SIZE = 5000
	};
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	unsigned char source[DATA_SIZE] = { 0 };
	unsigned char destination[DATA_SIZE] = { 0 };
	const unsigned char partial_source[4] = { UINT8_C(0x41), UINT8_C(0x42), UINT8_C(0x43),
						  UINT8_C(0x44) };
	unsigned char partial_destination[4] = { 0 };
	size_t completed = 0U;
	size_t index = 0U;
	minimm_page_info_t first_page = { 0 };
	minimm_page_info_t second_page = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	for (index = 0U; index < sizeof(source); ++index) {
		source[index] = (unsigned char)((index * 31U) & 0xffU);
	}

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create address space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map pages") ||
	    !check(minimm_write(space, address + UINT64_C(3000), source, sizeof(source),
				&completed) == MINIMM_OK,
		   "write across a page boundary") ||
	    !check(completed == sizeof(source), "cross-page write completes") ||
	    !check(minimm_read(space, address + UINT64_C(3000), destination, sizeof(destination),
			       &completed) == MINIMM_OK,
		   "read paged-out data across a boundary") ||
	    !check(completed == sizeof(destination), "cross-page read completes") ||
	    !check(memcmp(source, destination, sizeof(source)) == 0,
		   "temporary-file paging preserves note bytes") ||
	    !check(minimm_query_page(space, address, &first_page) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &second_page) ==
				   MINIMM_OK,
		   "query both accessed pages") ||
	    !check(first_page.pfn != second_page.pfn,
		   "different virtual pages map through distinct PFNs")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	if (!check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE * UINT64_C(2),
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_OK,
		   "remove edit permission") ||
	    !check(minimm_write(space, address, source, 1U, &completed) == MINIMM_OK &&
			   completed == 1U,
		   "ordinary write only requires write permission") ||
	    !check(minimm_edit(space, address, source, 1U, &completed) == MINIMM_ERROR_PERMISSION,
		   "edit operation requires the additional edit right") ||
	    !check(completed == 0U, "denied edit writes no bytes") ||
	    !check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE * UINT64_C(2),
				   MINIMM_PROT_READ) == MINIMM_OK,
		   "make mapping read-only") ||
	    !check(minimm_write(space, address, source, 1U, &completed) == MINIMM_ERROR_PERMISSION,
		   "read-only mapping rejects writes") ||
	    !check(completed == 0U, "denied write reports no progress")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	if (!check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE,
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_OK,
		   "restore writes on only the first page") ||
	    !check(minimm_write(space, address + MINIMM_PAGE_SIZE - UINT64_C(2), partial_source,
				sizeof(partial_source), &completed) == MINIMM_ERROR_PERMISSION &&
			   completed == 2U,
		   "cross-page write reports progress before a permission failure") ||
	    !check(minimm_read(space, address + MINIMM_PAGE_SIZE - UINT64_C(2), partial_destination,
			       2U, &completed) == MINIMM_OK &&
			   completed == 2U && memcmp(partial_destination, partial_source, 2U) == 0,
		   "partial write commits only the completed prefix") ||
	    !check(minimm_munmap(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "remove the second page before a partial read") ||
	    !check(minimm_read(space, address + MINIMM_PAGE_SIZE - UINT64_C(2), partial_destination,
			       sizeof(partial_destination), &completed) == MINIMM_ERROR_NOT_FOUND &&
			   completed == 2U && memcmp(partial_destination, partial_source, 2U) == 0,
		   "cross-page read reports progress before an unmapped page")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	args.address_hint = MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE;
	args.length = MINIMM_PAGE_SIZE;
	args.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK &&
			   address == MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE,
		   "map the highest canonical user page") ||
	    !check(minimm_write(space, MINIMM_USER_ADDRESS_LIMIT - UINT64_C(2), partial_source, 2U,
				&completed) == MINIMM_OK &&
			   completed == 2U,
		   "an access may end exactly at the user limit") ||
	    !check(minimm_write(space, MINIMM_USER_ADDRESS_LIMIT - UINT64_C(2), partial_source, 3U,
				&completed) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   completed == 0U,
		   "an access crossing the user limit is rejected before writing") ||
	    !check(minimm_read(space, MINIMM_USER_ADDRESS_LIMIT, NULL, 0U, &completed) ==
				   MINIMM_OK &&
			   completed == 0U,
		   "a zero-byte access at the exclusive limit is a no-op")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(space);
	minimm_destroy(mm);
	return test_cow_under_pin_pressure() && test_write_completed_output_alias() &&
			       test_cross_space_eviction_fault_accounting() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
