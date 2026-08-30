#include "fault.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static minimm_status_t minimm_access_validate(minimm_space_t *space, minimm_vaddr_t address,
					      const void *buffer, size_t length)
{
	if (space == NULL || (buffer == NULL && length != 0U) ||
	    address > MINIMM_USER_ADDRESS_LIMIT ||
	    (length != 0U && address >= MINIMM_USER_ADDRESS_LIMIT) ||
	    (uint64_t)length > MINIMM_USER_ADDRESS_LIMIT - address) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return MINIMM_OK;
}

static bool minimm_access_pte_requires_fault(const minimm_pte_t *pte, minimm_access_t access,
					     bool write)
{
	return pte == NULL || !minimm_frame_is_resident(pte->frame) ||
	       (write && (pte->flags & MINIMM_PTE_COW) != 0U) ||
	       !minimm_fault_access_is_allowed(pte->protection, access);
}

static minimm_status_t minimm_access_refill_locked(minimm_space_t *space, minimm_vaddr_t address,
						   minimm_access_t access, bool write,
						   minimm_tlb_translation_t *out_translation)
{
	const minimm_vaddr_t page_address = address & ~(MINIMM_PAGE_SIZE - UINT64_C(1));
	minimm_pte_t *pte = minimm_page_table_lookup(space->page_table, page_address);
	minimm_pte_flags_t access_flags = MINIMM_PTE_ACCESSED;
	minimm_status_t status = MINIMM_OK;

	for (;;) {
		if (minimm_access_pte_requires_fault(pte, access, write)) {
			status = minimm_handle_page_fault_locked(space, address, access,
								 MINIMM_FAULT_ORIGIN_ACCESS, NULL);
			if (status != MINIMM_OK) {
				return status;
			}
			pte = minimm_page_table_lookup(space->page_table, page_address);
		} else {
			if (!minimm_frame_try_pin_resident(pte->frame)) {
				/* Eviction won the race after the residency check. */
				continue;
			}
			if (write) {
				access_flags |= MINIMM_PTE_DIRTY;
			}
			if ((pte->flags & access_flags) != access_flags) {
				status = minimm_page_table_update_flags(
					space->page_table, page_address, access_flags, 0U);
				if (status != MINIMM_OK) {
					minimm_frame_unpin_resident(pte->frame);
					return status;
				}
				pte = minimm_page_table_lookup(space->page_table, page_address);
			}
			break;
		}

		if (pte == NULL) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		if (minimm_frame_try_pin_resident(pte->frame)) {
			break;
		}
	}

	minimm_tlb_insert(space->tlb, page_address, pte);
	minimm_frame_retain(pte->frame);
	out_translation->frame = pte->frame;
	out_translation->protection = pte->protection;
	out_translation->flags = pte->flags;
	return MINIMM_OK;
}

static minimm_status_t minimm_access_memory(minimm_space_t *space, minimm_vaddr_t address,
					    void *buffer, size_t length, size_t *out_completed,
					    minimm_access_t access, bool write)
{
	unsigned char *bytes = buffer;
	size_t completed = 0U;
	minimm_status_t status = minimm_access_validate(space, address, buffer, length);

	if (status != MINIMM_OK || length == 0U) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return status;
	}

	(void)pthread_mutex_lock(&space->lock);
	if (atomic_load_explicit(&space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&space->lock);
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_BUSY;
	}

	while (completed < length) {
		const minimm_vaddr_t current_address = address + completed;
		const size_t page_offset =
			(size_t)(current_address & (MINIMM_PAGE_SIZE - UINT64_C(1)));
		const size_t remaining = length - completed;
		const size_t page_remaining = (size_t)MINIMM_PAGE_SIZE - page_offset;
		const size_t chunk = remaining < page_remaining ? remaining : page_remaining;
		minimm_vma_snapshot_t *snapshot =
			atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
		const minimm_vma_t *mapping = minimm_vma_snapshot_lookup(snapshot, current_address);
		minimm_tlb_translation_t translation = { 0 };
		bool translated = false;
		bool translation_pinned = false;

		if (mapping == NULL || !minimm_fault_access_is_allowed(mapping->prot, access)) {
			/* Model the CPU fault and its denied handler result as an observable event. */
			status = minimm_handle_page_fault_locked(space, current_address, access,
								 MINIMM_FAULT_ORIGIN_ACCESS, NULL);
			break;
		}

		translated = minimm_tlb_lookup(space->tlb, current_address, &translation);
		if (translated &&
		    ((write && (translation.flags & MINIMM_PTE_COW) != 0U) ||
		     !minimm_fault_access_is_allowed(translation.protection, access) ||
		     (translation.flags & MINIMM_PTE_ACCESSED) == 0U ||
		     (write && (translation.flags & MINIMM_PTE_DIRTY) == 0U))) {
			minimm_tlb_translation_release(&translation);
			translated = false;
		}
		if (translated) {
			translation_pinned = minimm_frame_try_pin_resident(translation.frame);
			if (!translation_pinned) {
				minimm_tlb_translation_release(&translation);
				translated = false;
			}
		}

		if (!translated) {
			status = minimm_access_refill_locked(space, current_address, access, write,
							     &translation);
			if (status != MINIMM_OK) {
				break;
			}
			translation_pinned = true;
		}

		if (!minimm_fault_access_is_allowed(translation.protection, access)) {
			minimm_frame_unpin_resident(translation.frame);
			minimm_tlb_translation_release(&translation);
			status = MINIMM_ERROR_PERMISSION;
			break;
		}

		if (write) {
			status = minimm_frame_write(translation.frame, page_offset,
						    bytes + completed, chunk);
		} else {
			status = minimm_frame_read(translation.frame, page_offset,
						   bytes + completed, chunk);
		}
		if (translation_pinned) {
			minimm_frame_unpin_resident(translation.frame);
		}
		minimm_tlb_translation_release(&translation);
		if (status != MINIMM_OK) {
			break;
		}
		completed += chunk;
	}

	(void)pthread_mutex_unlock(&space->lock);
	if (out_completed != NULL) {
		*out_completed = completed;
	}
	return status;
}

minimm_status_t minimm_read(minimm_space_t *space, minimm_vaddr_t source, void *destination,
			    size_t length, size_t *out_completed)
{
	return minimm_access_memory(space, source, destination, length, out_completed,
				    MINIMM_ACCESS_READ, false);
}

minimm_status_t minimm_write(minimm_space_t *space, minimm_vaddr_t destination, const void *source,
			     size_t length, size_t *out_completed)
{
	return minimm_access_memory(space, destination, (void *)source, length, out_completed,
				    MINIMM_ACCESS_WRITE, true);
}

minimm_status_t minimm_edit(minimm_space_t *space, minimm_vaddr_t destination, const void *source,
			    size_t length, size_t *out_completed)
{
	return minimm_access_memory(space, destination, (void *)source, length, out_completed,
				    MINIMM_ACCESS_EDIT, true);
}
