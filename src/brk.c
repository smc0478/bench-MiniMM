#include "space.h"

#include <stdint.h>

static minimm_vaddr_t minimm_brk_page_end(minimm_vaddr_t address)
{
	return (address + MINIMM_PAGE_SIZE - UINT64_C(1)) & ~(MINIMM_PAGE_SIZE - UINT64_C(1));
}

static minimm_status_t minimm_brk_map_range(minimm_space_t *space, minimm_vaddr_t start,
					    minimm_vaddr_t end)
{
	const minimm_mmap_args_t args = {
		.address_hint = start,
		.length = end - start,
		.note_offset = 0U,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
		.note = NULL,
	};
	minimm_vaddr_t mapped = MINIMM_ADDRESS_AUTO;
	minimm_status_t status = minimm_mmap(space, &args, &mapped);

	if (status == MINIMM_OK && mapped != start) {
		(void)minimm_munmap(space, mapped, end - start);
		status = MINIMM_ERROR_ADDRESS_IN_USE;
	}
	return status;
}

static minimm_status_t minimm_brk_set_locked(minimm_space_t *space, minimm_vaddr_t requested_end)
{
	const minimm_vaddr_t current_end = space->brk_end;
	const minimm_vaddr_t old_page_end = minimm_brk_page_end(current_end);
	minimm_vaddr_t new_page_end = 0U;
	minimm_status_t status = MINIMM_OK;

	if (requested_end < space->brk_base || requested_end >= MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	new_page_end = minimm_brk_page_end(requested_end);

	if (new_page_end == old_page_end) {
		/* The byte-granular break changed within the existing final page. */
	} else if (old_page_end == space->brk_base) {
		status = minimm_brk_map_range(space, space->brk_base, new_page_end);
	} else if (new_page_end == space->brk_base) {
		status = minimm_munmap(space, space->brk_base, old_page_end - space->brk_base);
	} else if (new_page_end < old_page_end) {
		/* Shrinking may cross heap VMAs split by mprotect. */
		status = minimm_munmap(space, new_page_end, old_page_end - new_page_end);
	} else {
		status = minimm_mapping_extend_heap(space, old_page_end, new_page_end);
		if (status == MINIMM_ERROR_UNSUPPORTED) {
			/* A missing/replaced tail starts a new private heap fragment. */
			status = minimm_brk_map_range(space, old_page_end, new_page_end);
		}
	}
	if (status == MINIMM_OK) {
		space->brk_end = requested_end;
	}
	return status;
}

minimm_status_t minimm_brk(minimm_space_t *space, minimm_vaddr_t requested_end,
			   minimm_vaddr_t *out_current_end)
{
	minimm_status_t status = MINIMM_OK;

	if (space == NULL || out_current_end == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->brk_lock);
	if (requested_end != UINT64_C(0)) {
		status = minimm_brk_set_locked(space, requested_end);
	}
	*out_current_end = space->brk_end;
	(void)pthread_mutex_unlock(&space->brk_lock);
	return status;
}

minimm_status_t minimm_sbrk(minimm_space_t *space, intptr_t increment,
			    minimm_vaddr_t *out_previous_end)
{
	minimm_vaddr_t requested_end = 0U;
	minimm_status_t status = MINIMM_OK;

	if (space == NULL || out_previous_end == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->brk_lock);
	*out_previous_end = space->brk_end;
	if (increment >= 0) {
		const uint64_t amount = (uint64_t)increment;

		if (amount >= MINIMM_USER_ADDRESS_LIMIT - space->brk_end && amount != UINT64_C(0)) {
			status = MINIMM_ERROR_NO_SPACE;
		} else {
			requested_end = space->brk_end + amount;
		}
	} else {
		const uint64_t amount = (uint64_t)(-(increment + 1)) + UINT64_C(1);

		if (amount > space->brk_end - space->brk_base) {
			status = MINIMM_ERROR_INVALID_ARGUMENT;
		} else {
			requested_end = space->brk_end - amount;
		}
	}
	if (status == MINIMM_OK && increment != 0) {
		status = minimm_brk_set_locked(space, requested_end);
	}
	(void)pthread_mutex_unlock(&space->brk_lock);
	return status;
}
