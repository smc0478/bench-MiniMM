#include "minimm/minimm.h"

#include "../src/space.h"

#include <stdbool.h>
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

int main(void)
{
	static const char original[] = "parent private bytes";
	static const char child_value[] = "child COW bytes";
	static const char shared_value[] = "post-fork shared bytes";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t private_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t shared_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t locked_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t readonly_address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t parent_page = { 0 };
	minimm_page_info_t child_page = { 0 };
	const minimm_pte_t *parent_shared_pte = NULL;
	const minimm_pte_t *child_shared_pte = NULL;
	uint8_t child_core = UINT8_C(0);
	char buffer[64] = { 0 };
	unsigned char locked_value = UINT8_C(0x5a);
	size_t completed = 0U;
	minimm_mmap_args_t private_args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	minimm_mmap_args_t shared_args = private_args;
	minimm_mmap_args_t readonly_args = private_args;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	shared_args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS;
	readonly_args.protection = MINIMM_PROT_READ;
	readonly_args.maximum_protection = MINIMM_PROT_READ;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create parent") ||
	    !check(minimm_mmap(parent, &private_args, &private_address) == MINIMM_OK,
		   "map parent private page") ||
	    !check(minimm_mmap(parent, &shared_args, &shared_address) == MINIMM_OK,
		   "map unfaulted shared anonymous page") ||
	    !check(minimm_mmap(parent, &readonly_args, &readonly_address) == MINIMM_OK &&
			   minimm_read(parent, readonly_address, buffer, UINT64_C(1), &completed) ==
				   MINIMM_OK,
		   "fault a permanently read-only private page") ||
	    !check(minimm_mmap(parent, &private_args, &locked_address) == MINIMM_OK &&
			   minimm_write(parent, locked_address, &locked_value, sizeof(locked_value),
					&completed) == MINIMM_OK &&
			   minimm_mlock(parent, locked_address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "fault and lock a parent-only page") ||
	    !check(minimm_write(parent, private_address, original, sizeof(original), &completed) ==
			   MINIMM_OK,
		   "write private parent page") ||
	    !check(minimm_write(parent, shared_address, shared_value, sizeof(shared_value),
				&completed) == MINIMM_OK,
		   "dirty the shared parent page before fork") ||
	    !check(minimm_read(parent, private_address, buffer, sizeof(original), &completed) ==
			   MINIMM_OK,
		   "warm parent TLB before fork") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork space") ||
	    !check(minimm_query_page(parent, private_address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, private_address, &child_page) == MINIMM_OK &&
			   parent_page.pfn == child_page.pfn && parent_page.cow && child_page.cow &&
			   !child_page.accessed &&
			   (parent_page.protection & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) ==
				   0U &&
			   (child_page.protection & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) == 0U,
		   "fork shares an old, write-protected private frame as COW") ||
	    !check((parent_shared_pte = minimm_page_table_lookup_const(parent->page_table,
								       shared_address)) != NULL &&
			   (child_shared_pte = minimm_page_table_lookup_const(
				    child->page_table, shared_address)) != NULL &&
			   (parent_shared_pte->flags & (MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY)) ==
				   (MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY) &&
			   (child_shared_pte->flags & (MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY)) ==
				   0U,
		   "fork keeps the parent state but installs a clean, old shared child PTE") ||
	    !check(minimm_query_page(parent, readonly_address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, readonly_address, &child_page) == MINIMM_OK &&
			   parent_page.pfn == child_page.pfn && !parent_page.cow &&
			   !child_page.cow && parent_page.protection == MINIMM_PROT_READ &&
			   child_page.protection == MINIMM_PROT_READ,
		   "fork does not create needless COW state for a never-writable private VMA") ||
	    !check(minimm_query_page(parent, locked_address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, locked_address, &child_page) == MINIMM_OK &&
			   minimm_mincore(child, locked_address, MINIMM_PAGE_SIZE, &child_core,
					  1U) == MINIMM_OK &&
			   parent_page.pfn == child_page.pfn && parent_page.locked &&
			   (child_core & MINIMM_MINCORE_LOCKED) == UINT8_C(0),
		   "fork does not inherit the parent's memory lock") ||
	    !check(minimm_munlock(parent, locked_address, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_query_page(child, locked_address, &child_page) == MINIMM_OK &&
			   !child_page.locked &&
			   minimm_madvise(parent, locked_address, MINIMM_PAGE_SIZE,
					  MINIMM_MADV_PAGEOUT) == MINIMM_OK &&
			   minimm_query_page(child, locked_address, &child_page) == MINIMM_OK &&
			   !child_page.resident,
		   "parent unlock drops the final pin and permits reclamation")) {
		goto failure;
	}

	if (!check(minimm_write(child, private_address, child_value, sizeof(child_value),
				&completed) == MINIMM_OK,
		   "child writes through COW") ||
	    !check(minimm_query_page(child, private_address, &child_page) == MINIMM_OK &&
			   minimm_query_page(parent, private_address, &parent_page) == MINIMM_OK &&
			   child_page.pfn != parent_page.pfn && !child_page.cow && parent_page.cow,
		   "child receives a distinct frame") ||
	    !check(minimm_read(parent, private_address, buffer, sizeof(original), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, original, sizeof(original)) == 0,
		   "warm parent TLB cannot bypass fork COW") ||
	    !check(minimm_read(child, private_address, buffer, sizeof(child_value), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, child_value, sizeof(child_value)) == 0,
		   "child keeps its copied bytes")) {
		goto failure;
	}

	if (!check(minimm_write(child, shared_address, shared_value, sizeof(shared_value),
				&completed) == MINIMM_OK,
		   "write the inherited shared anonymous page in child") ||
	    !check(minimm_read(parent, shared_address, buffer, sizeof(shared_value), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, shared_value, sizeof(shared_value)) == 0,
		   "shared backing remains coherent across fork") ||
	    !check(minimm_query_page(parent, shared_address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, shared_address, &child_page) == MINIMM_OK &&
			   parent_page.pfn == child_page.pfn && parent_page.shared &&
			   child_page.shared && !parent_page.cow && !child_page.cow,
		   "forked shared aliases use one PFN without COW")) {
		goto failure;
	}

	minimm_space_destroy(parent);
	parent = NULL;
	if (!check(minimm_read(child, private_address, buffer, sizeof(child_value), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, child_value, sizeof(child_value)) == 0,
		   "child survives parent destruction")) {
		goto failure;
	}

	minimm_space_destroy(child);
	minimm_destroy(mm);
	return EXIT_SUCCESS;

failure:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return EXIT_FAILURE;
}
