#include "fault.h"

#include "internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool minimm_fault_access_is_valid(minimm_access_t access)
{
	const minimm_access_t allowed = MINIMM_ACCESS_READ | MINIMM_ACCESS_WRITE |
					MINIMM_ACCESS_EDIT | MINIMM_ACCESS_EXECUTE;

	return access != 0U && (access & ~allowed) == 0U && (access & (access - 1U)) == 0U;
}

bool minimm_fault_access_is_allowed(minimm_prot_t protection, minimm_access_t access)
{
	if (access == MINIMM_ACCESS_READ) {
		return (protection & MINIMM_PROT_READ) != 0U;
	}
	if (access == MINIMM_ACCESS_WRITE) {
		return (protection & MINIMM_PROT_WRITE) != 0U;
	}
	if (access == MINIMM_ACCESS_EDIT) {
		return (protection & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) ==
		       (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT);
	}
	return (protection & MINIMM_PROT_EXEC) != 0U;
}

static void minimm_fault_begin(minimm_space_t *space, minimm_vaddr_t address,
			       minimm_access_t access, minimm_fault_origin_t origin,
			       minimm_fault_info_t *fault)
{
	if (space->fault_sequence == UINT64_MAX) {
		space->fault_sequence = 1U;
	} else {
		space->fault_sequence += 1U;
	}

	(void)memset(fault, 0, sizeof(*fault));
	fault->sequence = space->fault_sequence;
	fault->address = address;
	fault->page_address = address & ~(MINIMM_PAGE_SIZE - UINT64_C(1));
	fault->access = access;
	fault->origin = origin;
	fault->status = MINIMM_OK;
}

static minimm_status_t minimm_fault_fail(minimm_fault_info_t *fault, minimm_fault_reason_t reason,
					 minimm_status_t status)
{
	fault->reason = reason;
	fault->resolution = reason == MINIMM_FAULT_PERMISSION || reason == MINIMM_FAULT_UNMAPPED ?
				    MINIMM_FAULT_DENIED :
				    MINIMM_FAULT_UNRESOLVED;
	fault->status = status;
	return status;
}

static void minimm_fault_trace_append_locked(minimm_space_t *space,
					     const minimm_fault_info_t *fault)
{
	size_t index = 0U;

	if (space->fault_trace_count < MINIMM_FAULT_TRACE_CAPACITY) {
		index = (space->fault_trace_start + space->fault_trace_count) %
			MINIMM_FAULT_TRACE_CAPACITY;
		space->fault_trace_count += 1U;
	} else {
		index = space->fault_trace_start;
		space->fault_trace_start =
			(space->fault_trace_start + 1U) % MINIMM_FAULT_TRACE_CAPACITY;
		if (space->fault_trace_overwritten_count != UINT64_MAX) {
			space->fault_trace_overwritten_count += 1U;
		}
	}
	space->fault_trace[index] = *fault;
}

minimm_status_t minimm_populate_page_locked(minimm_space_t *space, minimm_vaddr_t address)
{
	minimm_vma_snapshot_t *snapshot = NULL;
	const minimm_vma_t *mapping = NULL;
	minimm_space_binding_t *binding = NULL;
	minimm_backing_kind_t backing_kind = MINIMM_BACKING_ANON_PRIVATE;
	minimm_pte_t *pte = NULL;
	minimm_frame_t *frame = NULL;
	minimm_pte_flags_t mapping_flags = 0U;
	minimm_status_t status = MINIMM_OK;
	bool paged_in = false;

	if (space == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	mapping = minimm_vma_snapshot_lookup(snapshot, address);
	if (mapping == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	binding = minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
	if (binding == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	pte = minimm_page_table_lookup(space->page_table, address);
	if (pte != NULL) {
		return minimm_frame_ensure_resident(pte->frame, &paged_in);
	}

	backing_kind = minimm_mapping_backing_kind(binding->backing);
	status = minimm_mapping_backing_get_frame(
		binding->backing,
		mapping->note_offset +
			((address & ~(MINIMM_PAGE_SIZE - UINT64_C(1))) - mapping->start),
		&frame, NULL);
	if (status == MINIMM_OK) {
		status = minimm_frame_ensure_resident(frame, &paged_in);
	}
	if (status != MINIMM_OK) {
		minimm_frame_release(frame);
		return status;
	}

	if (backing_kind == MINIMM_BACKING_ANON_SHARED ||
	    backing_kind == MINIMM_BACKING_NOTE_SHARED) {
		mapping_flags |= MINIMM_PTE_SHARED;
	} else if (backing_kind == MINIMM_BACKING_NOTE_PRIVATE &&
		   (mapping->max_prot & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) != 0U) {
		mapping_flags |= MINIMM_PTE_COW;
	}

	status = minimm_page_table_map(space->page_table,
				       address & ~(MINIMM_PAGE_SIZE - UINT64_C(1)), frame,
				       mapping->prot, mapping_flags);
	minimm_frame_release(frame);
	return status;
}

minimm_status_t minimm_handle_page_fault_locked(minimm_space_t *space, minimm_vaddr_t address,
						minimm_access_t access,
						minimm_fault_origin_t origin,
						minimm_fault_info_t *out_fault)
{
	minimm_fault_info_t fault = { 0 };
	minimm_vma_snapshot_t *snapshot = NULL;
	const minimm_vma_t *mapping = NULL;
	minimm_space_binding_t *binding = NULL;
	minimm_backing_kind_t backing_kind = MINIMM_BACKING_ANON_PRIVATE;
	minimm_pte_t *pte = NULL;
	minimm_frame_t *frame = NULL;
	minimm_status_t status = MINIMM_OK;
	minimm_pte_flags_t access_flags = MINIMM_PTE_ACCESSED;
	bool paged_in = false;
	bool backing_frame_created = false;
	bool copied_on_first_write = false;

	if (space == NULL || address >= MINIMM_USER_ADDRESS_LIMIT ||
	    !minimm_fault_access_is_valid(access)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_fault_begin(space, address, access, origin, &fault);
	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	mapping = minimm_vma_snapshot_lookup(snapshot, address);
	if (mapping == NULL) {
		status = minimm_fault_fail(&fault, MINIMM_FAULT_UNMAPPED, MINIMM_ERROR_NOT_FOUND);
		goto done;
	}
	if (!minimm_fault_access_is_allowed(mapping->prot, access)) {
		status =
			minimm_fault_fail(&fault, MINIMM_FAULT_PERMISSION, MINIMM_ERROR_PERMISSION);
		goto done;
	}
	binding = minimm_space_binding_find(space->bindings, mapping->mapping_cookie);
	if (binding == NULL) {
		status = minimm_fault_fail(&fault, MINIMM_FAULT_NO_FRAME, MINIMM_ERROR_NOT_FOUND);
		goto done;
	}
	backing_kind = minimm_mapping_backing_kind(binding->backing);

	if (access == MINIMM_ACCESS_WRITE || access == MINIMM_ACCESS_EDIT) {
		access_flags |= MINIMM_PTE_DIRTY;
	}
	pte = minimm_page_table_lookup(space->page_table, fault.page_address);
	if (pte == NULL) {
		const uint64_t backing_offset =
			mapping->note_offset + (fault.page_address - mapping->start);
		minimm_pte_flags_t mapping_flags = access_flags;

		status = minimm_mapping_backing_get_frame(binding->backing, backing_offset, &frame,
							  &backing_frame_created);
		if (status != MINIMM_OK) {
			(void)minimm_fault_fail(&fault,
						status == MINIMM_ERROR_IO ?
							MINIMM_FAULT_BACKING_IO :
							MINIMM_FAULT_NO_FRAME,
						status);
			goto done;
		}
		if (backing_kind == MINIMM_BACKING_NOTE_PRIVATE &&
		    (access == MINIMM_ACCESS_WRITE || access == MINIMM_ACCESS_EDIT)) {
			minimm_frame_t *private_frame = NULL;

			status = minimm_frame_copy(frame, &private_frame);
			minimm_frame_release(frame);
			frame = private_frame;
			copied_on_first_write = status == MINIMM_OK;
		} else {
			status = minimm_frame_ensure_resident(frame, &paged_in);
		}
		if (status != MINIMM_OK) {
			minimm_frame_release(frame);
			frame = NULL;
			(void)minimm_fault_fail(&fault,
						status == MINIMM_ERROR_IO ?
							MINIMM_FAULT_BACKING_IO :
							MINIMM_FAULT_NO_FRAME,
						status);
			goto done;
		}

		if (backing_kind == MINIMM_BACKING_ANON_SHARED ||
		    backing_kind == MINIMM_BACKING_NOTE_SHARED) {
			mapping_flags |= MINIMM_PTE_SHARED;
		} else if (backing_kind == MINIMM_BACKING_NOTE_PRIVATE &&
			   (mapping->max_prot & (MINIMM_PROT_WRITE | MINIMM_PROT_EDIT)) != 0U &&
			   !copied_on_first_write) {
			mapping_flags |= MINIMM_PTE_COW;
		}

		status = minimm_page_table_map(space->page_table, fault.page_address, frame,
					       mapping->prot, mapping_flags);
		minimm_frame_release(frame);
		frame = NULL;
		if (status != MINIMM_OK) {
			(void)minimm_fault_fail(&fault, MINIMM_FAULT_NO_FRAME, status);
			goto done;
		}
		if (copied_on_first_write) {
			fault.reason = MINIMM_FAULT_COW;
			fault.resolution = MINIMM_FAULT_COW_COPIED;
		} else {
			fault.reason = MINIMM_FAULT_NOT_PRESENT;
			fault.resolution =
				backing_kind == MINIMM_BACKING_ANON_PRIVATE ||
						(backing_kind == MINIMM_BACKING_ANON_SHARED &&
						 backing_frame_created) ?
					MINIMM_FAULT_ZERO_FILLED :
					MINIMM_FAULT_PAGE_IN;
		}
		pte = minimm_page_table_lookup(space->page_table, fault.page_address);
	} else if ((pte->flags & MINIMM_PTE_COW) != 0U &&
		   (access == MINIMM_ACCESS_WRITE || access == MINIMM_ACCESS_EDIT)) {
		status = minimm_frame_copy(pte->frame, &frame);
		if (status == MINIMM_OK) {
			status = minimm_page_table_replace_frame(space->page_table,
								 fault.page_address, frame,
								 mapping->prot, access_flags,
								 MINIMM_PTE_COW);
		}
		minimm_frame_release(frame);
		frame = NULL;
		if (status != MINIMM_OK) {
			(void)minimm_fault_fail(&fault,
						status == MINIMM_ERROR_IO ?
							MINIMM_FAULT_BACKING_IO :
							MINIMM_FAULT_NO_FRAME,
						status);
			goto done;
		}
		fault.reason = MINIMM_FAULT_COW;
		fault.resolution = MINIMM_FAULT_COW_COPIED;
		pte = minimm_page_table_lookup(space->page_table, fault.page_address);
	} else {
		if (!minimm_fault_access_is_allowed(pte->protection, access)) {
			status = minimm_fault_fail(&fault, MINIMM_FAULT_PERMISSION,
						   MINIMM_ERROR_PERMISSION);
			goto done;
		}
		status = minimm_frame_ensure_resident(pte->frame, &paged_in);
		if (status != MINIMM_OK) {
			(void)minimm_fault_fail(&fault,
						status == MINIMM_ERROR_IO ?
							MINIMM_FAULT_BACKING_IO :
							MINIMM_FAULT_NO_FRAME,
						status);
			goto done;
		}
		status = minimm_page_table_update_flags(space->page_table, fault.page_address,
							access_flags, 0U);
		if (status != MINIMM_OK) {
			(void)minimm_fault_fail(&fault, MINIMM_FAULT_NO_FRAME, status);
			goto done;
		}
		if (paged_in) {
			fault.reason = MINIMM_FAULT_NOT_PRESENT;
			fault.resolution = MINIMM_FAULT_PAGE_IN;
		} else {
			fault.resolution = MINIMM_FAULT_NO_ACTION;
		}
		pte = minimm_page_table_lookup(space->page_table, fault.page_address);
	}

	minimm_tlb_invalidate_page(space->tlb, fault.page_address);
	minimm_tlb_insert(space->tlb, fault.page_address, pte);

done:
	fault.status = status;
	minimm_fault_trace_append_locked(space, &fault);
	if (out_fault != NULL) {
		*out_fault = fault;
	}
	return status;
}

minimm_status_t minimm_handle_page_fault(minimm_space_t *space, minimm_vaddr_t address,
					 minimm_access_t access, minimm_fault_info_t *out_fault)
{
	minimm_status_t status = MINIMM_OK;

	if (space == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	status = minimm_handle_page_fault_locked(space, address, access,
						 MINIMM_FAULT_ORIGIN_EXPLICIT, out_fault);
	(void)pthread_mutex_unlock(&space->lock);
	return status;
}

minimm_status_t minimm_space_get_fault_trace(minimm_space_t *space, minimm_fault_trace_t *out_trace)
{
	size_t index = 0U;

	if (space == NULL || out_trace == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_trace, 0, sizeof(*out_trace));

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	out_trace->count = space->fault_trace_count;
	out_trace->overwritten_count = space->fault_trace_overwritten_count;
	for (index = 0U; index < space->fault_trace_count; ++index) {
		const size_t source =
			(space->fault_trace_start + index) % MINIMM_FAULT_TRACE_CAPACITY;

		out_trace->events[index] = space->fault_trace[source];
	}
	(void)pthread_mutex_unlock(&space->lock);
	return MINIMM_OK;
}

minimm_status_t minimm_space_clear_fault_trace(minimm_space_t *space)
{
	if (space == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_BUSY;
	}
	(void)memset(space->fault_trace, 0, sizeof(space->fault_trace));
	space->fault_trace_start = 0U;
	space->fault_trace_count = 0U;
	space->fault_trace_overwritten_count = 0U;
	(void)pthread_mutex_unlock(&space->lock);
	return MINIMM_OK;
}

minimm_status_t minimm_query_page(minimm_space_t *space, minimm_vaddr_t address,
				  minimm_page_info_t *out_page)
{
	minimm_vma_snapshot_t *snapshot = NULL;
	const minimm_vma_t *mapping = NULL;
	const minimm_pte_t *pte = NULL;
	minimm_frame_state_t frame_state = { 0 };

	if (space == NULL || out_page == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_page, 0, sizeof(*out_page));
	out_page->pfn = MINIMM_PFN_NONE;
	out_page->page_address = address & ~(MINIMM_PAGE_SIZE - UINT64_C(1));

	(void)pthread_mutex_lock(&space->lock);
	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	mapping = minimm_vma_snapshot_lookup(snapshot, address);
	if (mapping == NULL) {
		(void)pthread_mutex_unlock(&space->lock);
		return MINIMM_ERROR_NOT_FOUND;
	}
	out_page->protection = mapping->prot;
	out_page->shared = mapping->flags == MINIMM_VMA_FLAG_SHARED;

	pte = minimm_page_table_lookup_const(space->page_table, address);
	if (pte != NULL) {
		minimm_frame_get_state(pte->frame, &frame_state);
		out_page->protection = pte->protection;
		out_page->resident = frame_state.resident;
		out_page->present = frame_state.resident && (pte->flags & MINIMM_PTE_PRESENT) != 0U;
		if (out_page->present) {
			out_page->pfn = minimm_frame_id(pte->frame);
		}
		out_page->dirty = frame_state.dirty || (pte->flags & MINIMM_PTE_DIRTY) != 0U;
		out_page->accessed = (pte->flags & MINIMM_PTE_ACCESSED) != 0U;
		out_page->cow = (pte->flags & MINIMM_PTE_COW) != 0U;
		out_page->shared = (pte->flags & MINIMM_PTE_SHARED) != 0U;
		out_page->locked = frame_state.pinned || (pte->flags & MINIMM_PTE_LOCKED) != 0U;
		out_page->cold = frame_state.cold;
	}
	(void)pthread_mutex_unlock(&space->lock);
	return MINIMM_OK;
}

minimm_status_t minimm_translate(minimm_space_t *space, minimm_vaddr_t address,
				 minimm_pfn_t *out_pfn, uint16_t *out_page_offset)
{
	const minimm_pte_t *pte = NULL;
	bool found = false;

	if (space == NULL || out_pfn == NULL || out_page_offset == NULL ||
	    address >= MINIMM_USER_ADDRESS_LIMIT) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_pfn = MINIMM_PFN_NONE;
	*out_page_offset = 0U;

	(void)pthread_mutex_lock(&space->lock);
	pte = minimm_page_table_lookup_const(space->page_table, address);
	if (pte != NULL && (pte->flags & MINIMM_PTE_PRESENT) != 0U &&
	    minimm_frame_is_resident(pte->frame)) {
		*out_pfn = minimm_frame_id(pte->frame);
		*out_page_offset = (uint16_t)(address & (MINIMM_PAGE_SIZE - UINT64_C(1)));
		found = true;
	}
	(void)pthread_mutex_unlock(&space->lock);
	return found ? MINIMM_OK : MINIMM_ERROR_NOT_FOUND;
}
