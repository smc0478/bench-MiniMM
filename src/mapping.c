#include "space.h"

#include "fault.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MINIMM_MMAP_MIN_ADDRESS UINT64_C(0x10000)

static bool minimm_mapping_protection_is_valid(minimm_prot_t protection)
{
	const minimm_prot_t allowed = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT |
				      MINIMM_PROT_EXEC;

	return (protection & ~allowed) == UINT32_C(0) &&
	       ((protection & MINIMM_PROT_EDIT) == UINT32_C(0) ||
		(protection & MINIMM_PROT_WRITE) != UINT32_C(0));
}

static bool minimm_mapping_flags_are_valid(minimm_map_flags_t flags)
{
	const minimm_map_flags_t allowed = MINIMM_MAP_SHARED | MINIMM_MAP_PRIVATE |
					   MINIMM_MAP_FIXED | MINIMM_MAP_FIXED_NOREPLACE |
					   MINIMM_MAP_ANONYMOUS | MINIMM_MAP_POPULATE;
	const minimm_map_flags_t sharing = flags & (MINIMM_MAP_SHARED | MINIMM_MAP_PRIVATE);

	return (flags & ~allowed) == UINT32_C(0) &&
	       (sharing == MINIMM_MAP_SHARED || sharing == MINIMM_MAP_PRIVATE);
}

static minimm_status_t minimm_mapping_validate_backing(minimm_space_t *space,
						       const minimm_mmap_args_t *args,
						       uint64_t length,
						       minimm_prot_t maximum_protection)
{
	const bool anonymous = (args->flags & MINIMM_MAP_ANONYMOUS) != 0U;
	minimm_note_rights_t rights = MINIMM_NOTE_RIGHT_NONE;
	uint64_t note_size = UINT64_C(0);

	if (anonymous) {
		return args->note == NULL && args->note_offset == UINT64_C(0) ?
			       MINIMM_OK :
			       MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (args->note == NULL || !minimm_note_belongs_to(args->note, space->system) ||
	    (args->note_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	rights = minimm_note_rights(args->note);
	if ((rights & MINIMM_NOTE_RIGHT_READ) == 0U ||
	    ((args->flags & MINIMM_MAP_SHARED) != 0U && (rights & MINIMM_NOTE_RIGHT_SHARE) == 0U) ||
	    ((args->flags & MINIMM_MAP_SHARED) != 0U &&
	     (maximum_protection & MINIMM_PROT_WRITE) != 0U &&
	     (rights & MINIMM_NOTE_RIGHT_WRITE) == 0U) ||
	    ((maximum_protection & MINIMM_PROT_EDIT) != 0U &&
	     (rights & MINIMM_NOTE_RIGHT_EDIT) == 0U)) {
		return MINIMM_ERROR_PERMISSION;
	}

	note_size = minimm_note_size(args->note);
	if (args->note_offset > note_size || length > note_size - args->note_offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return MINIMM_OK;
}

static minimm_prot_t minimm_mapping_default_maximum_protection(const minimm_mmap_args_t *args)
{
	const bool anonymous = (args->flags & MINIMM_MAP_ANONYMOUS) != 0U;
	const bool private_mapping = (args->flags & MINIMM_MAP_PRIVATE) != 0U;
	const minimm_note_rights_t rights = minimm_note_rights(args->note);
	minimm_prot_t maximum = MINIMM_PROT_READ | MINIMM_PROT_EXEC;

	/*
	 * Model Linux VM_MAY* independently from the mapping's initial access.
	 * A private file mapping may become writable without a writable backing
	 * handle because writes are resolved into private COW pages.
	 */
	if (anonymous || private_mapping || (rights & MINIMM_NOTE_RIGHT_WRITE) != 0U) {
		maximum |= MINIMM_PROT_WRITE;
	}
	if (anonymous || (rights & MINIMM_NOTE_RIGHT_EDIT) != 0U) {
		maximum |= MINIMM_PROT_WRITE | MINIMM_PROT_EDIT;
	}
	return maximum | args->protection;
}

static minimm_status_t minimm_mapping_align_length(uint64_t length, uint64_t *out_length)
{
	const uint64_t mask = MINIMM_PAGE_SIZE - UINT64_C(1);

	if (out_length == NULL || length == UINT64_C(0) || length > UINT64_MAX - mask) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_length = (length + mask) & ~mask;
	if (*out_length == UINT64_C(0) || *out_length > MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return MINIMM_OK;
}

static bool minimm_mapping_range_is_valid(minimm_vaddr_t start, uint64_t length,
					  minimm_vaddr_t *out_end)
{
	if (start < MINIMM_MMAP_MIN_ADDRESS ||
	    (start & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    start >= MINIMM_USER_ADDRESS_LIMIT || length > MINIMM_USER_ADDRESS_LIMIT - start) {
		return false;
	}
	*out_end = start + length;
	return true;
}

static bool minimm_mapping_operation_range_is_valid(minimm_vaddr_t start, uint64_t length,
						    minimm_vaddr_t *out_end)
{
	if ((start & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    start >= MINIMM_USER_ADDRESS_LIMIT || length > MINIMM_USER_ADDRESS_LIMIT - start) {
		return false;
	}
	*out_end = start + length;
	return true;
}

static void minimm_mapping_reclaim_snapshot(void *object)
{
	minimm_vma_snapshot_destroy(object);
}

static void minimm_mapping_publish_locked(minimm_space_t *space, minimm_vma_snapshot_t *snapshot)
{
	minimm_vma_snapshot_t *old_snapshot =
		atomic_exchange_explicit(&space->vma_snapshot, snapshot, memory_order_seq_cst);

	if (old_snapshot == NULL) {
		return;
	}
	if (minimm_rcu_retire(space->vma_rcu, old_snapshot, minimm_mapping_reclaim_snapshot) !=
	    MINIMM_OK) {
		(void)minimm_rcu_synchronize(space->vma_rcu);
		minimm_vma_snapshot_destroy(old_snapshot);
	} else {
		(void)minimm_rcu_poll(space->vma_rcu);
	}
}

static minimm_vma_t minimm_mapping_make_vma(const minimm_mmap_args_t *args, minimm_vaddr_t start,
					    minimm_vaddr_t end, minimm_prot_t maximum_protection,
					    uint64_t cookie)
{
	minimm_vma_t mapping = { 0 };

	mapping.start = start;
	mapping.end = end;
	mapping.mapping_cookie = cookie;
	mapping.note_offset = args->note_offset;
	mapping.prot = args->protection;
	mapping.max_prot = maximum_protection;
	mapping.flags = (args->flags & MINIMM_MAP_SHARED) != 0U ? MINIMM_VMA_FLAG_SHARED :
								  MINIMM_VMA_FLAG_PRIVATE;
	return mapping;
}

static void minimm_mapping_unmap_pages_locked(minimm_space_t *space, minimm_vaddr_t start,
					      minimm_vaddr_t end)
{
	minimm_vaddr_t cursor = start;
	minimm_vaddr_t page = UINT64_C(0);

	minimm_tlb_invalidate_range(space->tlb, start, end);
	while (minimm_page_table_find_next(space->page_table, cursor, end, &page) == MINIMM_OK) {
		(void)minimm_page_table_unmap(space->page_table, page);
		cursor = page + MINIMM_PAGE_SIZE;
	}
}

typedef struct minimm_mapping_copy_page {
	minimm_vaddr_t source_address;
	minimm_frame_t *frame;
	minimm_prot_t protection;
	minimm_pte_flags_t flags;
	bool rollback_pin_held;
} minimm_mapping_copy_page_t;

typedef struct minimm_mapping_copy_collect {
	minimm_mapping_copy_page_t *pages;
	minimm_vaddr_t source_start;
	minimm_vaddr_t source_end;
	size_t capacity;
	size_t count;
} minimm_mapping_copy_collect_t;

static minimm_status_t minimm_mapping_collect_copy_page(minimm_vaddr_t page_address,
							const minimm_pte_t *pte, void *opaque)
{
	minimm_mapping_copy_collect_t *collect = opaque;

	if (page_address < collect->source_start || page_address >= collect->source_end) {
		return MINIMM_OK;
	}
	if (collect->count == SIZE_MAX ||
	    (collect->pages != NULL && collect->count >= collect->capacity)) {
		return MINIMM_ERROR_NO_SPACE;
	}
	if (collect->pages != NULL) {
		minimm_mapping_copy_page_t *page = &collect->pages[collect->count];

		page->source_address = page_address;
		page->frame = pte->frame;
		page->protection = pte->protection;
		page->flags = pte->flags;
	}
	collect->count += 1U;
	return MINIMM_OK;
}

static minimm_status_t
minimm_mapping_collect_copy_pages_locked(minimm_space_t *space, minimm_vaddr_t source_start,
					 minimm_vaddr_t source_end,
					 minimm_mapping_copy_page_t **out_pages, size_t *out_count)
{
	minimm_mapping_copy_collect_t collect = {
		.source_start = source_start,
		.source_end = source_end,
	};
	minimm_mapping_copy_page_t *pages = NULL;
	minimm_status_t status = MINIMM_OK;

	*out_pages = NULL;
	*out_count = 0U;
	status = minimm_page_table_for_each(space->page_table, minimm_mapping_collect_copy_page,
					    &collect);
	if (status != MINIMM_OK || collect.count == 0U) {
		return status;
	}
	if (collect.count > SIZE_MAX / sizeof(*pages)) {
		return MINIMM_ERROR_NO_SPACE;
	}
	pages = calloc(collect.count, sizeof(*pages));
	if (pages == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	collect.pages = pages;
	collect.capacity = collect.count;
	collect.count = 0U;
	status = minimm_page_table_for_each(space->page_table, minimm_mapping_collect_copy_page,
					    &collect);
	if (status != MINIMM_OK) {
		free(pages);
		return status;
	}
	*out_pages = pages;
	*out_count = collect.count;
	return MINIMM_OK;
}

static void minimm_mapping_unmap_copy_pages_locked(minimm_space_t *space,
						   const minimm_mapping_copy_page_t *pages,
						   size_t count, minimm_vaddr_t source_address,
						   minimm_vaddr_t destination_address)
{
	size_t index = 0U;

	for (index = 0U; index < count; ++index) {
		const minimm_vaddr_t destination_page =
			destination_address + (pages[index].source_address - source_address);

		if (minimm_page_table_lookup(space->page_table, destination_page) != NULL) {
			(void)minimm_page_table_unmap(space->page_table, destination_page);
		}
	}
}

static minimm_status_t minimm_mapping_copy_ptes_locked(minimm_space_t *space,
						       const minimm_mapping_copy_page_t *pages,
						       size_t count, minimm_vaddr_t source_address,
						       minimm_vaddr_t destination_address,
						       bool copy_on_write)
{
	size_t index = 0U;
	minimm_status_t status = MINIMM_OK;

	for (index = 0U; index < count; ++index) {
		const minimm_vaddr_t destination_page =
			destination_address + (pages[index].source_address - source_address);
		minimm_pte_flags_t flags =
			pages[index].flags &
			~(minimm_pte_flags_t)(MINIMM_PTE_SHARED | MINIMM_PTE_LOCKED |
					      MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY);

		if (copy_on_write) {
			flags |= MINIMM_PTE_COW;
		} else {
			flags &= ~(minimm_pte_flags_t)MINIMM_PTE_COW;
		}

		status = minimm_page_table_map(space->page_table, destination_page,
					       pages[index].frame, pages[index].protection, flags);
		if (status != MINIMM_OK) {
			minimm_mapping_unmap_copy_pages_locked(space, pages, index, source_address,
							       destination_address);
			break;
		}
	}
	return status;
}

static minimm_status_t
minimm_mapping_update_source_copy_state_locked(minimm_space_t *space,
					       const minimm_mapping_copy_page_t *pages,
					       size_t count, bool copy_on_write)
{
	const minimm_pte_flags_t copied_flags = MINIMM_PTE_COW | MINIMM_PTE_SHARED;
	const minimm_pte_flags_t source_set_flags = copy_on_write ? MINIMM_PTE_COW : 0U;
	const minimm_pte_flags_t source_clear_flags = copy_on_write ? MINIMM_PTE_SHARED :
								      copied_flags;
	size_t index = 0U;
	minimm_status_t status = MINIMM_OK;

	for (index = 0U; index < count; ++index) {
		status = minimm_page_table_update_attributes(space->page_table,
							     pages[index].source_address,
							     pages[index].protection,
							     source_set_flags, source_clear_flags);
		if (status != MINIMM_OK) {
			break;
		}
	}
	while (status != MINIMM_OK && index != 0U) {
		minimm_pte_flags_t set_flags = 0U;
		minimm_pte_flags_t clear_flags = 0U;

		index -= 1U;
		set_flags = pages[index].flags & copied_flags;
		clear_flags = copied_flags & ~pages[index].flags;
		(void)minimm_page_table_update_attributes(space->page_table,
							  pages[index].source_address,
							  pages[index].protection, set_flags,
							  clear_flags);
	}
	return status;
}

minimm_status_t minimm_mmap(minimm_space_t *space, const minimm_mmap_args_t *args,
			    minimm_vaddr_t *out_address)
{
	minimm_mmap_args_t args_snapshot = { 0 };
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *without_old = NULL;
	minimm_vma_snapshot_t *next = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t end = 0U;
	uint64_t length = 0U;
	minimm_prot_t maximum_protection = MINIMM_PROT_NONE;
	minimm_mapping_backing_t *backing = NULL;
	minimm_space_binding_t *binding = NULL;
	minimm_space_binding_t *removed_bindings = NULL;
	minimm_status_t status = MINIMM_OK;
	bool fixed = false;

	if (out_address == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (args != NULL) {
		args_snapshot = *args;
		args = &args_snapshot;
	}
	fixed = args != NULL &&
		(args->flags & (MINIMM_MAP_FIXED | MINIMM_MAP_FIXED_NOREPLACE)) != 0U;
	*out_address = MINIMM_ADDRESS_AUTO;
	if (space == NULL || args == NULL || !minimm_mapping_flags_are_valid(args->flags) ||
	    !minimm_mapping_protection_is_valid(args->protection)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	maximum_protection = args->maximum_protection == MINIMM_PROT_NONE ?
				     minimm_mapping_default_maximum_protection(args) :
				     args->maximum_protection;
	if (!minimm_mapping_protection_is_valid(maximum_protection) ||
	    (args->protection & ~maximum_protection) != UINT32_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = minimm_mapping_align_length(args->length, &length);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_mapping_backing_create(space->system, args->note, args->flags, &backing);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_mapping_validate_backing(space, args, length, maximum_protection);
	if (status == MINIMM_OK) {
		status = minimm_space_binding_create(backing, &binding);
	}
	if (status != MINIMM_OK) {
		minimm_mapping_backing_release(backing);
		return status;
	}
	backing = NULL;

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto done;
	}
	if (space->next_mapping_cookie == UINT64_MAX) {
		status = MINIMM_ERROR_NO_SPACE;
		goto done;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);

	if (fixed) {
		const minimm_vma_t mapping = minimm_mapping_make_vma(args, args->address_hint, 0U,
								     maximum_protection,
								     space->next_mapping_cookie);
		minimm_vma_t fixed_mapping = mapping;

		address = args->address_hint;
		if (address == MINIMM_ADDRESS_AUTO ||
		    !minimm_mapping_range_is_valid(address, length, &end)) {
			status = MINIMM_ERROR_INVALID_ARGUMENT;
			goto done;
		}
		fixed_mapping.end = end;

		if ((args->flags & MINIMM_MAP_FIXED_NOREPLACE) != 0U) {
			status = minimm_vma_snapshot_insert(current, &fixed_mapping, &next);
		} else {
			status = minimm_vma_snapshot_remove(current, address, end, &without_old);
			if (status == MINIMM_OK) {
				status = minimm_vma_snapshot_insert(without_old, &fixed_mapping,
								    &next);
			}
		}
	} else {
		bool tried_hint = false;

		if (args->address_hint != MINIMM_ADDRESS_AUTO &&
		    minimm_mapping_range_is_valid(args->address_hint, length, &end)) {
			const minimm_vma_t hinted = minimm_mapping_make_vma(
				args, args->address_hint, end, maximum_protection,
				space->next_mapping_cookie);

			tried_hint = true;
			status = minimm_vma_snapshot_insert(current, &hinted, &next);
			if (status == MINIMM_OK) {
				address = args->address_hint;
			} else if (status != MINIMM_ERROR_ADDRESS_IN_USE) {
				goto done;
			}
		}

		if (!tried_hint || status == MINIMM_ERROR_ADDRESS_IN_USE) {
			minimm_vaddr_t lower_bound = space->mmap_base;

			if (tried_hint && args->address_hint != MINIMM_ADDRESS_AUTO &&
			    args->address_hint > lower_bound) {
				lower_bound = args->address_hint;
			}
			status = minimm_vma_snapshot_find_gap(current, lower_bound,
							      MINIMM_USER_ADDRESS_LIMIT, length,
							      MINIMM_PAGE_SIZE, &address);
			if (status == MINIMM_ERROR_NO_SPACE && lower_bound != space->mmap_base) {
				status = minimm_vma_snapshot_find_gap(current, space->mmap_base,
								      MINIMM_USER_ADDRESS_LIMIT,
								      length, MINIMM_PAGE_SIZE,
								      &address);
			}
			if (status != MINIMM_OK) {
				goto done;
			}
			end = address + length;
			{
				const minimm_vma_t mapping = minimm_mapping_make_vma(
					args, address, end, maximum_protection,
					space->next_mapping_cookie);

				status = minimm_vma_snapshot_insert(current, &mapping, &next);
			}
		}
	}

	if (status != MINIMM_OK) {
		goto done;
	}
	if (fixed && (args->flags & MINIMM_MAP_FIXED_NOREPLACE) == 0U) {
		minimm_mapping_unmap_pages_locked(space, address, end);
	}
	binding->cookie = space->next_mapping_cookie;
	binding->next = space->bindings;
	space->bindings = binding;
	binding = NULL;
	minimm_mapping_publish_locked(space, next);
	next = NULL;
	removed_bindings = minimm_space_binding_prune(
		&space->bindings, atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst));

	if ((args->flags & MINIMM_MAP_POPULATE) != 0U && args->protection != MINIMM_PROT_NONE) {
		minimm_vaddr_t page = address;

		while (page < end) {
			status = minimm_populate_page_locked(space, page);
			if (status != MINIMM_OK) {
				break;
			}
			page += MINIMM_PAGE_SIZE;
		}
		/* MAP_POPULATE is a best-effort prefault hint, not mmap transactionality. */
		status = MINIMM_OK;
	}
	space->next_mapping_cookie += 1U;
	*out_address = address;

done:
	minimm_vma_snapshot_destroy(next);
	minimm_vma_snapshot_destroy(without_old);
	(void)pthread_mutex_unlock(&space->lock);
	minimm_space_binding_list_destroy(removed_bindings);
	minimm_space_binding_list_destroy(binding);
	minimm_mapping_backing_release(backing);
	return status;
}

minimm_status_t minimm_mapping_copy(minimm_space_t *space, minimm_vaddr_t source_address,
				    uint64_t length, minimm_vaddr_t destination_hint,
				    minimm_vaddr_t *out_address)
{
	minimm_mapping_copy_page_t *copy_pages = NULL;
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *next = NULL;
	const minimm_vma_t *source_mapping = NULL;
	minimm_space_binding_t *source_binding = NULL;
	minimm_space_binding_t *copy_binding = NULL;
	minimm_vma_t copy_mapping = { 0 };
	minimm_vaddr_t source_end = UINT64_C(0);
	minimm_vaddr_t destination = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t destination_end = UINT64_C(0);
	uint64_t aligned_length = UINT64_C(0);
	size_t copy_page_count = 0U;
	minimm_status_t status = MINIMM_OK;
	bool copy_on_write = false;

	if (out_address == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_address = MINIMM_ADDRESS_AUTO;
	status = minimm_mapping_align_length(length, &aligned_length);
	if (space == NULL || status != MINIMM_OK ||
	    !minimm_mapping_range_is_valid(source_address, aligned_length, &source_end) ||
	    (destination_hint != MINIMM_ADDRESS_AUTO &&
	     !minimm_mapping_range_is_valid(destination_hint, aligned_length, &destination_end))) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto done;
	}
	if (space->next_mapping_cookie == UINT64_MAX) {
		status = MINIMM_ERROR_NO_SPACE;
		goto done;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	source_mapping = minimm_vma_snapshot_lookup(current, source_address);
	if (source_mapping == NULL || source_mapping->start != source_address ||
	    source_mapping->end != source_end || source_mapping->flags != MINIMM_VMA_FLAG_PRIVATE) {
		status = MINIMM_ERROR_UNSUPPORTED;
		goto done;
	}
	copy_on_write = (source_mapping->max_prot & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) != 0U;
	source_binding = minimm_space_binding_find(space->bindings, source_mapping->mapping_cookie);
	if (source_binding == NULL) {
		status = MINIMM_ERROR_NOT_FOUND;
		goto done;
	}
	status = minimm_mapping_collect_copy_pages_locked(space, source_address, source_end,
							  &copy_pages, &copy_page_count);
	if (status != MINIMM_OK) {
		goto done;
	}

	copy_mapping = *source_mapping;
	copy_mapping.mapping_cookie = space->next_mapping_cookie;
	if (destination_hint != MINIMM_ADDRESS_AUTO) {
		copy_mapping.start = destination_hint;
		copy_mapping.end = destination_end;
		status = minimm_vma_snapshot_insert(current, &copy_mapping, &next);
		if (status == MINIMM_OK) {
			destination = destination_hint;
		} else if (status != MINIMM_ERROR_ADDRESS_IN_USE) {
			goto done;
		}
	}

	if (destination == MINIMM_ADDRESS_AUTO) {
		minimm_vaddr_t lower_bound = space->mmap_base;

		if (destination_hint != MINIMM_ADDRESS_AUTO && destination_hint > lower_bound) {
			lower_bound = destination_hint;
		}
		status = minimm_vma_snapshot_find_gap(current, lower_bound,
						      MINIMM_USER_ADDRESS_LIMIT, aligned_length,
						      MINIMM_PAGE_SIZE, &destination);
		if (status == MINIMM_ERROR_NO_SPACE && lower_bound != space->mmap_base) {
			status = minimm_vma_snapshot_find_gap(current, space->mmap_base,
							      MINIMM_USER_ADDRESS_LIMIT,
							      aligned_length, MINIMM_PAGE_SIZE,
							      &destination);
		}
		if (status != MINIMM_OK) {
			goto done;
		}
		copy_mapping.start = destination;
		copy_mapping.end = destination + aligned_length;
		status = minimm_vma_snapshot_insert(current, &copy_mapping, &next);
		if (status != MINIMM_OK) {
			goto done;
		}
	}

	status = minimm_space_binding_create(source_binding->backing, &copy_binding);
	if (status != MINIMM_OK) {
		goto done;
	}
	minimm_mapping_backing_retain(copy_binding->backing);
	status = minimm_mapping_copy_ptes_locked(space, copy_pages, copy_page_count, source_address,
						 destination, copy_on_write);
	if (status != MINIMM_OK) {
		goto done;
	}

	/* Every source PTE used above is still present while the space lock is held. */
	status = minimm_mapping_update_source_copy_state_locked(space, copy_pages, copy_page_count,
								copy_on_write);
	if (status != MINIMM_OK) {
		minimm_mapping_unmap_copy_pages_locked(space, copy_pages, copy_page_count,
						       source_address, destination);
		goto done;
	}
	minimm_tlb_invalidate_range(space->tlb, source_address, source_end);
	minimm_tlb_invalidate_range(space->tlb, destination, destination + aligned_length);

	copy_binding->cookie = space->next_mapping_cookie;
	copy_binding->next = space->bindings;
	space->bindings = copy_binding;
	copy_binding = NULL;
	minimm_mapping_publish_locked(space, next);
	next = NULL;
	space->next_mapping_cookie += 1U;
	*out_address = destination;

done:
	minimm_vma_snapshot_destroy(next);
	(void)pthread_mutex_unlock(&space->lock);
	free(copy_pages);
	minimm_space_binding_list_destroy(copy_binding);
	return status;
}

minimm_status_t minimm_munmap(minimm_space_t *space, minimm_vaddr_t address, uint64_t length)
{
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *next = NULL;
	minimm_vaddr_t end = 0U;
	uint64_t aligned_length = 0U;
	minimm_space_binding_t *removed_bindings = NULL;
	minimm_status_t status = minimm_mapping_align_length(length, &aligned_length);

	if (space == NULL || status != MINIMM_OK ||
	    !minimm_mapping_operation_range_is_valid(address, aligned_length, &end)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	status = minimm_vma_snapshot_remove(current, address, end, &next);
	if (status == MINIMM_OK) {
		minimm_mapping_unmap_pages_locked(space, address, end);
		minimm_mapping_publish_locked(space, next);
		next = NULL;
		removed_bindings = minimm_space_binding_prune(
			&space->bindings,
			atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst));
	}
	(void)pthread_mutex_unlock(&space->lock);
	minimm_vma_snapshot_destroy(next);
	minimm_space_binding_list_destroy(removed_bindings);
	return status;
}

minimm_status_t minimm_mprotect(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
				minimm_prot_t protection)
{
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *next = NULL;
	minimm_vaddr_t end = 0U;
	minimm_vaddr_t cursor = 0U;
	minimm_vaddr_t page = 0U;
	uint64_t aligned_length = 0U;
	minimm_status_t status = MINIMM_OK;

	if (space == NULL || !minimm_mapping_protection_is_valid(protection)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (length == UINT64_C(0)) {
		if (address >= MINIMM_USER_ADDRESS_LIMIT ||
		    (address & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
			return MINIMM_ERROR_INVALID_ARGUMENT;
		}
		return atomic_load_explicit(&space->closing, memory_order_acquire) ?
			       MINIMM_ERROR_BUSY :
			       MINIMM_OK;
	}
	status = minimm_mapping_align_length(length, &aligned_length);
	if (status != MINIMM_OK ||
	    !minimm_mapping_operation_range_is_valid(address, aligned_length, &end)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	status = minimm_vma_snapshot_protect(current, address, end, protection, &next);
	if (status == MINIMM_OK) {
		cursor = address;
		while (minimm_page_table_find_next(space->page_table, cursor, end, &page) ==
		       MINIMM_OK) {
			(void)minimm_page_table_protect(space->page_table, page, protection);
			cursor = page + MINIMM_PAGE_SIZE;
		}
		minimm_tlb_invalidate_range(space->tlb, address, end);
		minimm_mapping_publish_locked(space, next);
		next = NULL;
	}
	(void)pthread_mutex_unlock(&space->lock);
	minimm_vma_snapshot_destroy(next);
	return status;
}

minimm_status_t minimm_page_protect(minimm_space_t *space, minimm_vaddr_t page_address,
				    minimm_prot_t protection)
{
	return minimm_mprotect(space, page_address, MINIMM_PAGE_SIZE, protection);
}

minimm_status_t minimm_mapping_query(minimm_space_t *space, minimm_vaddr_t address,
				     minimm_mapping_info_t *out_mapping)
{
	minimm_vma_snapshot_t *snapshot = NULL;
	const minimm_vma_t *mapping = NULL;
	bool found = false;

	if (space == NULL || out_mapping == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		return MINIMM_ERROR_BUSY;
	}
	(void)memset(out_mapping, 0, sizeof(*out_mapping));

	minimm_rcu_read_lock(space->vma_rcu);
	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	mapping = minimm_vma_snapshot_lookup(snapshot, address);
	if (mapping != NULL) {
		out_mapping->start = mapping->start;
		out_mapping->end = mapping->end;
		out_mapping->mapping_cookie = mapping->mapping_cookie;
		out_mapping->note_offset = mapping->note_offset;
		out_mapping->protection = mapping->prot;
		out_mapping->maximum_protection = mapping->max_prot;
		out_mapping->flags = mapping->flags;
		found = true;
	}
	minimm_rcu_read_unlock(space->vma_rcu);

	return found ? MINIMM_OK : MINIMM_ERROR_NOT_FOUND;
}

minimm_status_t minimm_mapping_extend_heap(minimm_space_t *space, minimm_vaddr_t start,
					   minimm_vaddr_t end)
{
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *next = NULL;
	const minimm_vma_t *tail = NULL;
	minimm_space_binding_t *binding = NULL;
	minimm_vma_t extension = { 0 };
	minimm_vaddr_t cursor = UINT64_C(0);
	minimm_status_t status = MINIMM_OK;

	if (space == NULL || start < MINIMM_MMAP_MIN_ADDRESS || start >= end ||
	    end > MINIMM_USER_ADDRESS_LIMIT ||
	    ((start | end) & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto done;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	tail = minimm_vma_snapshot_lookup(current, start - UINT64_C(1));
	if (tail == NULL || tail->end != start || tail->flags != MINIMM_VMA_FLAG_PRIVATE) {
		status = MINIMM_ERROR_UNSUPPORTED;
		goto done;
	}
	/* Only extend one continuous heap identity; do not adopt a MAP_FIXED replacement. */
	cursor = space->brk_base;
	while (cursor < start) {
		const minimm_vma_t *fragment = minimm_vma_snapshot_find_next(current, cursor);

		if (fragment == NULL || fragment->start > cursor ||
		    fragment->mapping_cookie != tail->mapping_cookie) {
			status = MINIMM_ERROR_UNSUPPORTED;
			goto done;
		}
		cursor = fragment->end < start ? fragment->end : start;
	}
	binding = minimm_space_binding_find(space->bindings, tail->mapping_cookie);
	if (binding == NULL ||
	    minimm_mapping_backing_kind(binding->backing) != MINIMM_BACKING_ANON_PRIVATE) {
		status = MINIMM_ERROR_UNSUPPORTED;
		goto done;
	}

	extension.start = start;
	extension.end = end;
	extension.mapping_cookie = tail->mapping_cookie;
	extension.note_offset = tail->note_offset + (tail->end - tail->start);
	extension.prot = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE;
	extension.max_prot = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE | MINIMM_VMA_PROT_EDIT;
	extension.flags = MINIMM_VMA_FLAG_PRIVATE;
	status = minimm_vma_snapshot_insert(current, &extension, &next);
	if (status == MINIMM_OK) {
		minimm_mapping_publish_locked(space, next);
		next = NULL;
	}

done:
	minimm_vma_snapshot_destroy(next);
	(void)pthread_mutex_unlock(&space->lock);
	return status;
}

static minimm_status_t minimm_mapping_move_ptes_locked(minimm_space_t *space,
						       minimm_vaddr_t old_address,
						       minimm_vaddr_t new_address,
						       uint64_t move_length, uint64_t old_length)
{
	const minimm_vaddr_t move_end = old_address + move_length;
	minimm_vaddr_t cursor = old_address;
	minimm_vaddr_t source_page = UINT64_C(0);
	minimm_status_t status = MINIMM_OK;

	while (minimm_page_table_find_next(space->page_table, cursor, move_end, &source_page) ==
	       MINIMM_OK) {
		const minimm_pte_t *pte =
			minimm_page_table_lookup_const(space->page_table, source_page);

		status = minimm_page_table_map(space->page_table,
					       new_address + (source_page - old_address),
					       pte->frame, pte->protection, pte->flags);
		if (status != MINIMM_OK) {
			break;
		}
		cursor = source_page + MINIMM_PAGE_SIZE;
	}
	if (status != MINIMM_OK) {
		minimm_mapping_unmap_pages_locked(space, new_address, new_address + move_length);
		return status;
	}

	minimm_tlb_invalidate_range(space->tlb, new_address, new_address + move_length);
	minimm_mapping_unmap_pages_locked(space, old_address, old_address + old_length);
	return MINIMM_OK;
}

static const minimm_mapping_copy_page_t *
minimm_mapping_find_saved_page(const minimm_mapping_copy_page_t *pages, size_t count,
			       minimm_vaddr_t address)
{
	size_t low = 0U;
	size_t high = count;

	while (low < high) {
		const size_t middle = low + ((high - low) / 2U);

		if (pages[middle].source_address < address) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	return low < count && pages[low].source_address == address ? &pages[low] : NULL;
}

static minimm_status_t minimm_mapping_replace_pte_locked(minimm_space_t *space,
							 minimm_vaddr_t address,
							 const minimm_pte_t *source)
{
	const minimm_pte_flags_t mutable_flags = ~(minimm_pte_flags_t)MINIMM_PTE_PRESENT;
	const minimm_pte_flags_t set_flags = source->flags & mutable_flags;
	const minimm_pte_flags_t clear_flags = (~source->flags) & mutable_flags;

	if (minimm_page_table_lookup(space->page_table, address) == NULL) {
		return minimm_page_table_map(space->page_table, address, source->frame,
					     source->protection, source->flags);
	}
	return minimm_page_table_replace_frame(space->page_table, address, source->frame,
					       source->protection, set_flags, clear_flags);
}

static void minimm_mapping_restore_fixed_destination_locked(
	minimm_space_t *space, minimm_vaddr_t destination, uint64_t destination_length,
	const minimm_mapping_copy_page_t *saved_pages, size_t saved_count)
{
	minimm_vaddr_t cursor = destination;
	minimm_vaddr_t page = UINT64_C(0);
	size_t index = 0U;

	for (index = 0U; index < saved_count; ++index) {
		const minimm_pte_t original = {
			.frame = saved_pages[index].frame,
			.protection = saved_pages[index].protection,
			.flags = saved_pages[index].flags,
		};

		(void)minimm_mapping_replace_pte_locked(space, saved_pages[index].source_address,
							&original);
	}
	while (minimm_page_table_find_next(space->page_table, cursor,
					   destination + destination_length, &page) == MINIMM_OK) {
		if (minimm_mapping_find_saved_page(saved_pages, saved_count, page) == NULL) {
			(void)minimm_page_table_unmap(space->page_table, page);
		}
		cursor = page + MINIMM_PAGE_SIZE;
	}
	minimm_tlb_invalidate_range(space->tlb, destination, destination + destination_length);
}

static minimm_status_t minimm_mapping_move_ptes_fixed_locked(minimm_space_t *space,
							     minimm_vaddr_t old_address,
							     uint64_t old_length,
							     minimm_vaddr_t destination,
							     uint64_t destination_length)
{
	minimm_mapping_copy_page_t *saved_pages = NULL;
	const uint64_t move_length = old_length < destination_length ? old_length :
								       destination_length;
	const minimm_vaddr_t move_end = old_address + move_length;
	size_t saved_count = 0U;
	size_t retained_count = 0U;
	size_t index = 0U;
	minimm_vaddr_t cursor = old_address;
	minimm_vaddr_t source_page = UINT64_C(0);
	minimm_status_t status = minimm_mapping_collect_copy_pages_locked(
		space, destination, destination + destination_length, &saved_pages, &saved_count);

	if (status != MINIMM_OK) {
		return status;
	}
	for (index = 0U; index < saved_count; ++index) {
		minimm_frame_retain(saved_pages[index].frame);
		retained_count += 1U;
		if ((saved_pages[index].flags & MINIMM_PTE_LOCKED) != 0U) {
			if (!minimm_frame_try_pin_resident(saved_pages[index].frame)) {
				status = MINIMM_ERROR_BUSY;
				goto done;
			}
			saved_pages[index].rollback_pin_held = true;
		}
	}

	while (minimm_page_table_find_next(space->page_table, cursor, move_end, &source_page) ==
	       MINIMM_OK) {
		const minimm_pte_t *source =
			minimm_page_table_lookup_const(space->page_table, source_page);

		status = minimm_mapping_replace_pte_locked(
			space, destination + (source_page - old_address), source);
		if (status != MINIMM_OK) {
			break;
		}
		cursor = source_page + MINIMM_PAGE_SIZE;
	}
	if (status != MINIMM_OK) {
		minimm_mapping_restore_fixed_destination_locked(
			space, destination, destination_length, saved_pages, saved_count);
		goto done;
	}

	/* Source holes and a grown tail replace any old destination PTEs with holes. */
	cursor = destination;
	while (minimm_page_table_find_next(space->page_table, cursor,
					   destination + destination_length,
					   &source_page) == MINIMM_OK) {
		const uint64_t offset = source_page - destination;
		const minimm_pte_t *source =
			offset < move_length ?
				minimm_page_table_lookup_const(space->page_table,
							       old_address + offset) :
				NULL;

		if (source == NULL) {
			(void)minimm_page_table_unmap(space->page_table, source_page);
		}
		cursor = source_page + MINIMM_PAGE_SIZE;
	}
	minimm_tlb_invalidate_range(space->tlb, destination, destination + destination_length);
	minimm_mapping_unmap_pages_locked(space, old_address, old_address + old_length);

done:
	for (index = 0U; index < retained_count; ++index) {
		if (saved_pages[index].rollback_pin_held) {
			minimm_frame_unpin_resident(saved_pages[index].frame);
		}
		minimm_frame_release(saved_pages[index].frame);
	}
	free(saved_pages);
	return status;
}

minimm_status_t minimm_mremap(minimm_space_t *space, minimm_vaddr_t old_address,
			      uint64_t old_length, uint64_t new_length, uint32_t flags,
			      minimm_vaddr_t new_address_hint, minimm_vaddr_t *out_address)
{
	const uint32_t allowed_flags = MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_FIXED |
				       MINIMM_MREMAP_DONTUNMAP;
	const bool may_move = (flags & MINIMM_MREMAP_MAYMOVE) != 0U;
	const bool fixed = (flags & MINIMM_MREMAP_FIXED) != 0U;
	const bool dont_unmap = (flags & MINIMM_MREMAP_DONTUNMAP) != 0U;
	minimm_vma_snapshot_t *current = NULL;
	minimm_vma_snapshot_t *without_old = NULL;
	minimm_vma_snapshot_t *without_destination = NULL;
	minimm_vma_snapshot_t *next = NULL;
	const minimm_vma_t *mapping = NULL;
	minimm_space_binding_t *binding = NULL;
	minimm_space_binding_t *copy_binding = NULL;
	minimm_space_binding_t *removed_bindings = NULL;
	minimm_vma_t replacement = { 0 };
	uint64_t old_size = UINT64_C(0);
	uint64_t new_size = UINT64_C(0);
	minimm_vaddr_t old_end = 0U;
	minimm_vaddr_t new_end = 0U;
	minimm_vaddr_t destination = old_address;
	minimm_status_t status = MINIMM_OK;

	if (out_address == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_address = MINIMM_ADDRESS_AUTO;
	if (space == NULL || (flags & ~allowed_flags) != UINT32_C(0) || (fixed && !may_move) ||
	    (!fixed && new_address_hint != MINIMM_ADDRESS_AUTO) || (dont_unmap && !may_move)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (old_length == UINT64_C(0)) {
		/* Linux's shareable-mapping duplicate mode is outside MiniMM's VMA model. */
		return MINIMM_ERROR_UNSUPPORTED;
	}
	status = minimm_mapping_align_length(old_length, &old_size);
	if (status == MINIMM_OK) {
		status = minimm_mapping_align_length(new_length, &new_size);
	}
	if (status != MINIMM_OK ||
	    !minimm_mapping_range_is_valid(old_address, old_size, &old_end) ||
	    (fixed && (new_address_hint == MINIMM_ADDRESS_AUTO ||
		       !minimm_mapping_range_is_valid(new_address_hint, new_size, &new_end) ||
		       (new_address_hint < old_end && old_address < new_end)))) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto done;
	}
	current = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	mapping = minimm_vma_snapshot_lookup(current, old_address);
	if (mapping == NULL || mapping->start != old_address || mapping->end != old_end) {
		status = MINIMM_ERROR_UNSUPPORTED;
		goto done;
	}
	binding = minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
	if (binding == NULL) {
		status = MINIMM_ERROR_NOT_FOUND;
		goto done;
	}
	if (dont_unmap &&
	    (new_size != old_size || mapping->flags != MINIMM_VMA_FLAG_PRIVATE ||
	     minimm_mapping_backing_kind(binding->backing) != MINIMM_BACKING_ANON_PRIVATE)) {
		status = MINIMM_ERROR_UNSUPPORTED;
		goto done;
	}
	if (minimm_mapping_backing_note(binding->backing) != NULL &&
	    (mapping->note_offset >
		     minimm_note_size(minimm_mapping_backing_note(binding->backing)) ||
	     new_size > minimm_note_size(minimm_mapping_backing_note(binding->backing)) -
				mapping->note_offset)) {
		status = MINIMM_ERROR_INVALID_ARGUMENT;
		goto done;
	}

	if (!fixed && !dont_unmap && new_size == old_size) {
		*out_address = old_address;
		goto done;
	}
	if (!fixed && !dont_unmap && new_size < old_size) {
		new_end = old_address + new_size;
		status = minimm_vma_snapshot_remove(current, new_end, old_end, &next);
		if (status == MINIMM_OK) {
			minimm_mapping_unmap_pages_locked(space, new_end, old_end);
			minimm_mapping_publish_locked(space, next);
			next = NULL;
			*out_address = old_address;
		}
		goto done;
	}

	if (!fixed && !dont_unmap && new_size <= MINIMM_USER_ADDRESS_LIMIT - old_address) {
		new_end = old_address + new_size;
		replacement = *mapping;
		replacement.end = new_end;
		status = minimm_vma_snapshot_remove(current, old_address, old_end, &without_old);
		if (status == MINIMM_OK) {
			status = minimm_vma_snapshot_insert(without_old, &replacement, &next);
		}
		if (status == MINIMM_OK) {
			minimm_mapping_publish_locked(space, next);
			next = NULL;
			*out_address = old_address;
			goto done;
		}
		minimm_vma_snapshot_destroy(next);
		minimm_vma_snapshot_destroy(without_old);
		next = NULL;
		without_old = NULL;
		if (status != MINIMM_ERROR_ADDRESS_IN_USE) {
			goto done;
		}
	} else if (!fixed && !dont_unmap) {
		status = MINIMM_ERROR_NO_SPACE;
	}

	if (!may_move) {
		goto done;
	}
	if (fixed) {
		destination = new_address_hint;
	} else {
		status = minimm_vma_snapshot_find_gap(current, space->mmap_base,
						      MINIMM_USER_ADDRESS_LIMIT, new_size,
						      MINIMM_PAGE_SIZE, &destination);
		if (status != MINIMM_OK) {
			goto done;
		}
	}

	replacement = *mapping;
	replacement.start = destination;
	replacement.end = destination + new_size;
	if (dont_unmap) {
		if (space->next_mapping_cookie == UINT64_MAX) {
			status = MINIMM_ERROR_NO_SPACE;
			goto done;
		}
		replacement.mapping_cookie = space->next_mapping_cookie;
		if (fixed) {
			status = minimm_vma_snapshot_remove(
				current, destination, destination + new_size, &without_destination);
			if (status == MINIMM_OK) {
				status = minimm_vma_snapshot_insert(without_destination,
								    &replacement, &next);
			}
		} else {
			status = minimm_vma_snapshot_insert(current, &replacement, &next);
		}
		if (status == MINIMM_OK) {
			status = minimm_space_binding_create(binding->backing, &copy_binding);
		}
		if (status == MINIMM_OK) {
			minimm_mapping_backing_retain(copy_binding->backing);
		}
	} else {
		status = minimm_vma_snapshot_remove(current, old_address, old_end, &without_old);
		if (status == MINIMM_OK && fixed) {
			status = minimm_vma_snapshot_remove(without_old, destination,
							    destination + new_size,
							    &without_destination);
		}
		if (status == MINIMM_OK) {
			status = minimm_vma_snapshot_insert(
				fixed ? without_destination : without_old, &replacement, &next);
		}
	}
	if (status == MINIMM_OK) {
		status = fixed ? minimm_mapping_move_ptes_fixed_locked(space, old_address, old_size,
								       destination, new_size) :
				 minimm_mapping_move_ptes_locked(space, old_address, destination,
								 old_size, old_size);
	}
	if (status == MINIMM_OK) {
		if (dont_unmap) {
			copy_binding->cookie = space->next_mapping_cookie;
			copy_binding->next = space->bindings;
			space->bindings = copy_binding;
			copy_binding = NULL;
			space->next_mapping_cookie += 1U;
		}
		minimm_mapping_publish_locked(space, next);
		next = NULL;
		removed_bindings = minimm_space_binding_prune(
			&space->bindings,
			atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst));
		*out_address = destination;
	}

done:
	minimm_vma_snapshot_destroy(next);
	minimm_vma_snapshot_destroy(without_destination);
	minimm_vma_snapshot_destroy(without_old);
	(void)pthread_mutex_unlock(&space->lock);
	minimm_space_binding_list_destroy(removed_bindings);
	minimm_space_binding_list_destroy(copy_binding);
	return status;
}
