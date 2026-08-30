#include "minimm/minimm.h"

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

static bool test_write_only_population_is_clean(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t page = { 0 };
	minimm_fault_info_t fault = { 0 };
	minimm_space_stats_t stats = { 0 };
	size_t completed = 0U;
	const unsigned char value = UINT8_C(0xa5);
	bool success = true;
	const minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_POPULATE,
	};

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "populate a write-only anonymous page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && !page.dirty && !page.accessed,
		   "write-only population is clean and is not an access") ||
	    !check(minimm_space_get_stats(space, &stats) == MINIMM_OK &&
			   stats.fault_sequence == UINT64_C(0),
		   "population does not increment the access-fault sequence") ||
	    !check(minimm_handle_page_fault(space, address, MINIMM_ACCESS_READ, &fault) ==
				   MINIMM_ERROR_PERMISSION &&
			   fault.reason == MINIMM_FAULT_PERMISSION,
		   "ordinary read faults still enforce write-only protection") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && !page.dirty &&
			   !page.accessed,
		   "denied access does not modify populated PTE flags") ||
	    !check(minimm_write(space, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "ordinary write access succeeds") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.dirty &&
			   page.accessed,
		   "ordinary write access marks the page dirty and accessed")) {
		success = false;
	}

	minimm_space_destroy(space);
	minimm_destroy(mm);
	return success;
}

static bool test_private_note_population_preserves_cow(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_note_t *note = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t page = { 0 };
	minimm_pfn_t shared_pfn = MINIMM_PFN_NONE;
	size_t completed = 0U;
	unsigned char observed = 0U;
	const unsigned char original = UINT8_C(0x31);
	const unsigned char replacement = UINT8_C(0x72);
	bool success = true;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_POPULATE,
	};

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create space") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE,
				      MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE,
				      &note) == MINIMM_OK,
		   "create note backing") ||
	    !check(minimm_note_pwrite(note, 0U, &original, sizeof(original), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(original) && minimm_note_flush(note) == MINIMM_OK,
		   "seed and flush the note page")) {
		success = false;
		goto cleanup;
	}

	args.note = note;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "populate a write-only private-note page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && page.cow && !page.shared && !page.dirty &&
			   !page.accessed,
		   "private-note population keeps a clean COW PTE")) {
		success = false;
		goto cleanup;
	}
	shared_pfn = page.pfn;

	if (!check(minimm_write(space, address, &replacement, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement),
		   "first ordinary write resolves private-note COW") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK &&
			   page.pfn != shared_pfn && !page.cow && page.dirty && page.accessed,
		   "ordinary write creates and dirties a private frame") ||
	    !check(minimm_note_pread(note, 0U, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == original,
		   "private COW write leaves the note page unchanged")) {
		success = false;
	}

cleanup:
	minimm_note_release(note);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return success;
}

int main(void)
{
	const minimm_vaddr_t populate_hint = UINT64_C(0x30000000);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t locked_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t populated = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t first = { 0 };
	minimm_page_info_t second = { 0 };
	minimm_mapping_info_t mapping = { 0 };
	minimm_space_stats_t space_stats = { 0 };
	minimm_system_stats_t system_stats = { 0 };
	minimm_mmap_args_t locked_args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	minimm_mmap_args_t populate_args = {
		.address_hint = populate_hint,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_POPULATE,
	};

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create space") ||
	    !check(minimm_mmap(space, &locked_args, &locked_address) == MINIMM_OK &&
			   minimm_mlock(space, locked_address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock the only resident frame") ||
	    !check(minimm_mmap(space, &populate_args, &populated) == MINIMM_OK &&
			   populated == populate_hint,
		   "populate remains a successful best-effort mmap under pressure") ||
	    !check(minimm_mapping_query(space, populate_hint, &mapping) == MINIMM_OK,
		   "best-effort populate preserves mapping metadata") ||
	    !check(minimm_query_page(space, populate_hint, &first) == MINIMM_OK && !first.present,
		   "failed prefault leaves the page demand-faultable") ||
	    !check(minimm_munlock(space, locked_address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "unlock resident frame") ||
	    !check(minimm_munmap(space, populate_hint, populate_args.length) == MINIMM_OK,
		   "remove the best-effort mapping before retry") ||
	    !check(minimm_mmap(space, &populate_args, &populated) == MINIMM_OK &&
			   populated == populate_hint,
		   "populate succeeds after memory pressure is released") ||
	    !check(minimm_query_page(space, populated, &first) == MINIMM_OK &&
			   minimm_query_page(space, populated + MINIMM_PAGE_SIZE, &second) ==
				   MINIMM_OK &&
			   minimm_space_get_stats(space, &space_stats) == MINIMM_OK &&
			   minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   first.resident != second.resident && space_stats.pte_count == 3U &&
			   system_stats.frame_count == 3U && system_stats.resident_count == 1U,
		   "populate creates every PTE while residency remains capacity bounded")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	populate_args.flags |= MINIMM_MAP_FIXED_NOREPLACE;
	populate_args.address_hint = UINT64_C(0x31000000);
	if (!check(minimm_mmap(space, &populate_args, &populated) == MINIMM_OK &&
			   populated == populate_args.address_hint &&
			   minimm_mapping_query(space, populated, &mapping) == MINIMM_OK,
		   "fixed-noreplace and populate may be combined")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(space);
	minimm_destroy(mm);
	if (!test_write_only_population_is_clean() ||
	    !test_private_note_population_preserves_cow()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
