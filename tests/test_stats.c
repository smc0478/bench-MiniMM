#include "minimm/minimm.h"

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
	static const unsigned char value = 0x5aU;
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	minimm_system_stats_t system_stats = { 0 };
	minimm_space_stats_t initial = { 0 };
	minimm_space_stats_t after_write = { 0 };
	minimm_space_stats_t after_hit = { 0 };
	minimm_space_stats_t parent_cow = { 0 };
	minimm_space_stats_t child_cow = { 0 };
	minimm_space_stats_t before_flush = { 0 };
	minimm_space_stats_t after_flush = { 0 };
	minimm_space_stats_t after_miss = { 0 };
	minimm_space_stats_t child_private = { 0 };
	unsigned char byte = 0U;
	size_t completed = 0U;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	config.tlb_entries = 2U;
	if (!check(minimm_system_get_stats(NULL, &system_stats) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject null system") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   system_stats.frame_count == 0U && system_stats.resident_count == 0U &&
			   system_stats.resident_limit == 2U,
		   "initial system statistics") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create space") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map VMA") ||
	    !check(minimm_space_get_stats(parent, &initial) == MINIMM_OK &&
			   initial.vma_count == 1U && initial.pte_count == 0U &&
			   initial.fault_sequence == 0U,
		   "mapping is visible before demand fault") ||
	    !check(minimm_write(parent, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "fault and dirty first page") ||
	    !check(minimm_space_get_stats(parent, &after_write) == MINIMM_OK &&
			   after_write.vma_count == 1U && after_write.pte_count == 1U &&
			   after_write.present_count == 1U && after_write.resident_count == 1U &&
			   after_write.dirty_count == 1U && after_write.cow_count == 0U &&
			   after_write.fault_sequence == 1U && after_write.tlb_misses == 1U &&
			   after_write.tlb_hits == 0U,
		   "first access records one TLB miss without a synthetic refill hit") ||
	    !check(minimm_read(parent, address, &byte, sizeof(byte), &completed) == MINIMM_OK &&
			   byte == value,
		   "read through warm TLB") ||
	    !check(minimm_space_get_stats(parent, &after_hit) == MINIMM_OK &&
			   after_hit.tlb_hits > after_write.tlb_hits &&
			   after_hit.fault_sequence == after_write.fault_sequence,
		   "TLB hit does not create another fault") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork space") ||
	    !check(minimm_space_get_stats(parent, &parent_cow) == MINIMM_OK &&
			   minimm_space_get_stats(child, &child_cow) == MINIMM_OK &&
			   parent_cow.cow_count == 1U && child_cow.cow_count == 1U &&
			   parent_cow.pte_count == 1U && child_cow.pte_count == 1U,
		   "fork exposes COW mappings in both spaces") ||
	    !check(minimm_read(parent, address, &byte, sizeof(byte), &completed) == MINIMM_OK,
		   "rewarm parent TLB after fork flush") ||
	    !check(minimm_space_get_stats(parent, &before_flush) == MINIMM_OK &&
			   minimm_space_flush_tlb(parent) == MINIMM_OK &&
			   minimm_space_get_stats(parent, &after_flush) == MINIMM_OK &&
			   after_flush.tlb_invalidations == before_flush.tlb_invalidations + 1U,
		   "explicit TLB flush records invalidation") ||
	    !check(minimm_read(parent, address, &byte, sizeof(byte), &completed) == MINIMM_OK &&
			   minimm_space_get_stats(parent, &after_miss) == MINIMM_OK &&
			   after_miss.tlb_misses > after_flush.tlb_misses &&
			   after_miss.tlb_hits == after_flush.tlb_hits &&
			   after_miss.fault_sequence == after_flush.fault_sequence,
		   "resident access after flush is a TLB miss, not a page fault") ||
	    !check(minimm_write(child, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   minimm_space_get_stats(child, &child_private) == MINIMM_OK &&
			   child_private.cow_count == 0U && child_private.dirty_count == 1U,
		   "child COW write becomes private and stays dirty") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   system_stats.frame_count == 2U && system_stats.resident_count == 2U,
		   "system counters include the COW frame")) {
		minimm_space_destroy(child);
		minimm_space_destroy(parent);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return EXIT_SUCCESS;
}
