#include "minimm/minimm.h"

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
	static const char value[] = "heap bytes across pages";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_t *collision_space = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t heap_base = 0U;
	minimm_vaddr_t current = 0U;
	minimm_vaddr_t previous = 0U;
	minimm_vaddr_t collision_address = MINIMM_ADDRESS_AUTO;
	char buffer[sizeof(value)] = { 0 };
	size_t completed = 0U;
	minimm_mmap_args_t collision_args = {
		.address_hint = 0U,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
	};
	minimm_space_stats_t heap_stats = { 0 };

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create space") ||
	    !check(minimm_brk(space, 0U, &heap_base) == MINIMM_OK && heap_base != 0U,
		   "query initial break") ||
	    !check(minimm_brk(space, heap_base + MINIMM_PAGE_SIZE + UINT64_C(128), &current) ==
				   MINIMM_OK &&
			   current == heap_base + MINIMM_PAGE_SIZE + UINT64_C(128),
		   "grow break with exact byte granularity") ||
	    !check(minimm_write(space, heap_base + MINIMM_PAGE_SIZE - UINT64_C(8), value,
				sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "write across heap pages") ||
	    !check(minimm_read(space, heap_base + MINIMM_PAGE_SIZE - UINT64_C(8), buffer,
			       sizeof(buffer), &completed) == MINIMM_OK &&
			   memcmp(buffer, value, sizeof(value)) == 0,
		   "read heap bytes") ||
	    !check(minimm_sbrk(space, (intptr_t)MINIMM_PAGE_SIZE, &previous) == MINIMM_OK &&
			   previous == current,
		   "sbrk returns previous break") ||
	    !check(minimm_brk(space, 0U, &current) == MINIMM_OK &&
			   current == previous + MINIMM_PAGE_SIZE,
		   "sbrk advances current break") ||
	    !check(minimm_space_get_stats(space, &heap_stats) == MINIMM_OK &&
			   heap_stats.vma_count == 1U,
		   "heap growth keeps one VMA") ||
	    !check(minimm_mprotect(space, heap_base, MINIMM_PAGE_SIZE, MINIMM_PROT_READ) ==
				   MINIMM_OK &&
			   minimm_space_get_stats(space, &heap_stats) == MINIMM_OK &&
			   heap_stats.vma_count == 2U,
		   "mprotect may split the heap VMA") ||
	    !check(minimm_brk(space, current + MINIMM_PAGE_SIZE, &current) == MINIMM_OK &&
			   minimm_space_get_stats(space, &heap_stats) == MINIMM_OK &&
			   heap_stats.vma_count == 2U,
		   "grow a protection-split heap without changing its backing identity") ||
	    !check(minimm_mprotect(space, heap_base, current - heap_base,
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_OK &&
			   minimm_space_get_stats(space, &heap_stats) == MINIMM_OK &&
			   heap_stats.vma_count == 1U,
		   "restoring heap protection coalesces the original VMA") ||
	    !check(minimm_brk(space, MINIMM_USER_ADDRESS_LIMIT - UINT64_C(1), &current) ==
				   MINIMM_OK &&
			   current == MINIMM_USER_ADDRESS_LIMIT - UINT64_C(1),
		   "move break next to the user address boundary") ||
	    !check(minimm_sbrk(space, (intptr_t)1, &previous) == MINIMM_ERROR_NO_SPACE &&
			   previous == current,
		   "reject sbrk growth at the user address boundary without changing the break") ||
	    !check(minimm_space_fork(space, &child) == MINIMM_OK, "fork heap") ||
	    !check(minimm_brk(child, 0U, &previous) == MINIMM_OK && previous == current,
		   "fork inherits exact break") ||
	    !check(minimm_brk(space, heap_base, &current) == MINIMM_OK && current == heap_base,
		   "shrink break to heap base") ||
	    !check(minimm_read(space, heap_base, buffer, 1U, &completed) == MINIMM_ERROR_NOT_FOUND,
		   "shrunk heap pages are unmapped") ||
	    !check(minimm_sbrk(space, (intptr_t)-1, &previous) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   previous == heap_base,
		   "reject break underflow")) {
		goto failure;
	}

	if (!check(minimm_space_create(mm, &collision_space) == MINIMM_OK,
		   "create collision space") ||
	    !check(minimm_brk(collision_space, 0U, &heap_base) == MINIMM_OK,
		   "query collision heap base")) {
		goto failure;
	}
	collision_args.address_hint = heap_base;
	if (!check(minimm_mmap(collision_space, &collision_args, &collision_address) == MINIMM_OK &&
			   collision_address == heap_base,
		   "reserve first heap page") ||
	    !check(minimm_brk(collision_space, heap_base + UINT64_C(1), &current) ==
				   MINIMM_ERROR_ADDRESS_IN_USE &&
			   current == heap_base,
		   "heap collision leaves break unchanged")) {
		goto failure;
	}

	minimm_space_destroy(collision_space);
	minimm_space_destroy(child);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_SUCCESS;

failure:
	minimm_space_destroy(collision_space);
	minimm_space_destroy(child);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_FAILURE;
}
