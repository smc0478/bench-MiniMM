#include "minimm/minimm.h"

#include "page_table.h"
#include "space.h"
#include "stats.h"

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
	const minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_t *shared_child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t shared_address = MINIMM_ADDRESS_AUTO;
	minimm_pfn_t pfn = MINIMM_PFN_NONE;
	minimm_pfn_t old_pfn = MINIMM_PFN_NONE;
	uint16_t page_offset = 0U;
	minimm_page_info_t page = { 0 };
	minimm_fault_info_t fault = { 0 };
	uint64_t refill_fault_sequence = 0U;
	unsigned char written = UINT8_C(0x7b);
	unsigned char shared_value = UINT8_C(0x5a);
	unsigned char shared_observed = UINT8_C(0);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create address space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK,
		   "query not-present page") ||
	    !check(!page.present && !page.resident, "mapping starts demand-paged") ||
	    !check(minimm_handle_page_fault(space, address + 123U, MINIMM_ACCESS_READ, &fault) ==
			   MINIMM_OK,
		   "resolve demand-zero page fault") ||
	    !check(fault.reason == MINIMM_FAULT_NOT_PRESENT &&
			   fault.resolution == MINIMM_FAULT_ZERO_FILLED,
		   "fault reports zero-fill resolution") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && page.accessed && page.pfn != MINIMM_PFN_NONE,
		   "fault installs a resident PTE") ||
	    !check(minimm_translate(space, address + 123U, &pfn, &page_offset) == MINIMM_OK,
		   "translate virtual address") ||
	    !check(pfn == page.pfn && page_offset == 123U,
		   "translation exposes synthetic PFN and page offset") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "page out the resident translation") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && !page.present &&
			   !page.resident && page.pfn == MINIMM_PFN_NONE,
		   "paged-out PTE is not exposed as a present physical translation") ||
	    !check(minimm_translate(space, address, &pfn, &page_offset) == MINIMM_ERROR_NOT_FOUND &&
			   pfn == MINIMM_PFN_NONE,
		   "paged-out PTE has no translatable PFN") ||
	    !check(minimm_handle_page_fault(space, address, MINIMM_ACCESS_READ, &fault) ==
				   MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_NOT_PRESENT &&
			   fault.resolution == MINIMM_FAULT_PAGE_IN,
		   "fault pages a nonresident PTE back in") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && page.pfn != MINIMM_PFN_NONE,
		   "page-in restores a present physical translation") ||
	    !check(minimm_handle_page_fault(space, address, MINIMM_ACCESS_READ, &fault) ==
				   MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_NONE &&
			   fault.resolution == MINIMM_FAULT_NO_ACTION,
		   "resident page fault reports a no-op resolution") ||
	    !check((refill_fault_sequence = space->fault_sequence,
		    minimm_page_table_update_flags(space->page_table, address, 0U,
						   MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY)) ==
				   MINIMM_OK &&
			   minimm_space_flush_tlb(space) == MINIMM_OK &&
			   minimm_write(space, address, &written, sizeof(written), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(written) &&
			   space->fault_sequence == refill_fault_sequence &&
			   minimm_query_page(space, address, &page) == MINIMM_OK && page.accessed &&
			   page.dirty,
		   "resident TLB miss walks the PTE and records access without a fault") ||
	    !check(minimm_page_table_update_attributes(space->page_table, address,
						       MINIMM_PROT_READ | MINIMM_PROT_WRITE,
						       MINIMM_PTE_COW, 0U) == MINIMM_OK &&
			   minimm_query_page(space, address, &page) == MINIMM_OK && page.cow &&
			   (page.protection & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) == 0U,
		   "COW keeps VMA intent but write-protects the effective PTE") ||
	    !check((old_pfn = page.pfn,
		    minimm_handle_page_fault(space, address, MINIMM_ACCESS_WRITE, &fault)) ==
				   MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_COW &&
			   fault.resolution == MINIMM_FAULT_COW_COPIED,
		   "resolve a COW write fault") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.pfn != old_pfn &&
			   old_pfn != MINIMM_PFN_NONE && !page.cow &&
			   page.protection == (MINIMM_PROT_READ | MINIMM_PROT_WRITE),
		   "COW copies the frame and restores writable access")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	if (!check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE, MINIMM_PROT_READ) == MINIMM_OK,
		   "make page read-only") ||
	    !check(minimm_handle_page_fault(space, address, MINIMM_ACCESS_WRITE, &fault) ==
			   MINIMM_ERROR_PERMISSION,
		   "write fault is denied") ||
	    !check(fault.reason == MINIMM_FAULT_PERMISSION &&
			   fault.resolution == MINIMM_FAULT_DENIED,
		   "permission fault is classified") ||
	    !check(minimm_handle_page_fault(space, address + (MINIMM_PAGE_SIZE * UINT64_C(4)),
					    MINIMM_ACCESS_READ, &fault) == MINIMM_ERROR_NOT_FOUND,
		   "unmapped access is denied") ||
	    !check(fault.reason == MINIMM_FAULT_UNMAPPED, "unmapped fault is classified")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS;
	if (!check(minimm_mmap(space, &args, &shared_address) == MINIMM_OK,
		   "map a shared anonymous page") ||
	    !check(minimm_space_fork(space, &shared_child) == MINIMM_OK,
		   "fork before materializing the shared page") ||
	    !check(minimm_handle_page_fault(space, shared_address, MINIMM_ACCESS_WRITE, &fault) ==
				   MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_NOT_PRESENT &&
			   fault.resolution == MINIMM_FAULT_ZERO_FILLED,
		   "the first shared-anonymous fault allocates a zero page") ||
	    !check(minimm_write(space, shared_address, &shared_value, sizeof(shared_value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(shared_value),
		   "write the shared backing page") ||
	    !check(minimm_handle_page_fault(shared_child, shared_address, MINIMM_ACCESS_READ,
					    &fault) == MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_NOT_PRESENT &&
			   fault.resolution == MINIMM_FAULT_PAGE_IN,
		   "a resident shared backing alias is not reported as zero-filled") ||
	    !check(minimm_read(shared_child, shared_address, &shared_observed,
			       sizeof(shared_observed), &completed) == MINIMM_OK &&
			   completed == sizeof(shared_observed) && shared_observed == shared_value,
		   "the resident shared alias preserves backing bytes") ||
	    !check(minimm_madvise(shared_child, shared_address, MINIMM_PAGE_SIZE,
				  MINIMM_MADV_DONTNEED) == MINIMM_OK,
		   "discard the child PTE before testing a nonresident refault") ||
	    !check(minimm_madvise(space, shared_address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "page out the shared backing before the child faults") ||
	    !check(minimm_handle_page_fault(shared_child, shared_address, MINIMM_ACCESS_READ,
					    &fault) == MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_NOT_PRESENT &&
			   fault.resolution == MINIMM_FAULT_PAGE_IN,
		   "an existing shared backing page is reported as a page-in") ||
	    !check(minimm_read(shared_child, shared_address, &shared_observed,
			       sizeof(shared_observed), &completed) == MINIMM_OK &&
			   completed == sizeof(shared_observed) && shared_observed == shared_value,
		   "the shared page-in preserves backing bytes")) {
		minimm_space_destroy(shared_child);
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(shared_child);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_SUCCESS;
}
