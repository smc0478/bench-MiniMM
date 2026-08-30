#include "space.h"

#include <stdatomic.h>
#include <stdint.h>

typedef struct minimm_fork_context {
	minimm_space_t *parent;
	minimm_space_t *child;
	const minimm_vma_snapshot_t *snapshot;
} minimm_fork_context_t;

static minimm_status_t minimm_fork_copy_pte(minimm_vaddr_t page_address, const minimm_pte_t *pte,
					    void *opaque)
{
	minimm_fork_context_t *context = opaque;
	const minimm_vma_t *mapping = minimm_vma_snapshot_lookup(context->snapshot, page_address);
	const minimm_vma_prot_t writable = MINIMM_VMA_PROT_WRITE | MINIMM_VMA_PROT_EDIT;
	minimm_pte_flags_t flags = pte->flags &
				   ~(minimm_pte_flags_t)(MINIMM_PTE_LOCKED | MINIMM_PTE_ACCESSED);

	if (mapping == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	if (mapping->flags == MINIMM_VMA_FLAG_SHARED) {
		flags |= MINIMM_PTE_SHARED;
		flags &= ~(minimm_pte_flags_t)(MINIMM_PTE_COW | MINIMM_PTE_DIRTY);
	} else if ((mapping->max_prot & writable) != 0U) {
		flags |= MINIMM_PTE_COW;
		flags &= ~(minimm_pte_flags_t)MINIMM_PTE_SHARED;
	} else {
		flags &= ~(minimm_pte_flags_t)(MINIMM_PTE_COW | MINIMM_PTE_SHARED);
	}
	return minimm_page_table_map(context->child->page_table, page_address, pte->frame,
				     mapping->prot, flags);
}

static minimm_status_t minimm_fork_commit_parent_cow(minimm_vaddr_t page_address,
						     const minimm_pte_t *pte, void *opaque)
{
	minimm_fork_context_t *context = opaque;
	const minimm_vma_t *mapping = minimm_vma_snapshot_lookup(context->snapshot, page_address);
	const minimm_vma_prot_t writable = MINIMM_VMA_PROT_WRITE | MINIMM_VMA_PROT_EDIT;

	(void)pte;
	if (mapping == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	if (mapping->flags == MINIMM_VMA_FLAG_SHARED) {
		return minimm_page_table_update_attributes(context->parent->page_table,
							   page_address, mapping->prot,
							   MINIMM_PTE_SHARED, MINIMM_PTE_COW);
	}
	if ((mapping->max_prot & writable) == 0U) {
		return minimm_page_table_update_attributes(context->parent->page_table,
							   page_address, mapping->prot, 0U,
							   MINIMM_PTE_COW | MINIMM_PTE_SHARED);
	}
	return minimm_page_table_update_attributes(context->parent->page_table, page_address,
						   mapping->prot, MINIMM_PTE_COW,
						   MINIMM_PTE_SHARED);
}

minimm_status_t minimm_space_fork(minimm_space_t *parent, minimm_space_t **out_child)
{
	minimm_space_t *child = NULL;
	minimm_vma_snapshot_t *parent_snapshot = NULL;
	minimm_vma_snapshot_t *child_snapshot = NULL;
	minimm_vma_snapshot_t *empty_snapshot = NULL;
	minimm_space_binding_t *child_bindings = NULL;
	minimm_fork_context_t context = { 0 };
	minimm_status_t status = MINIMM_OK;

	if (out_child == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_child = NULL;
	if (parent == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	status = minimm_space_create(parent->system, &child);
	if (status != MINIMM_OK) {
		return status;
	}

	(void)pthread_mutex_lock(&parent->brk_lock);
	(void)pthread_mutex_lock(&parent->lock);
	if (atomic_load_explicit(&parent->closing, memory_order_acquire)) {
		status = MINIMM_ERROR_BUSY;
		goto done;
	}
	parent_snapshot = atomic_load_explicit(&parent->vma_snapshot, memory_order_seq_cst);
	status = minimm_vma_snapshot_clone(parent_snapshot, &child_snapshot);
	if (status == MINIMM_OK) {
		status = minimm_space_binding_list_clone(parent->bindings, &child_bindings);
	}
	if (status != MINIMM_OK) {
		goto done;
	}

	context.parent = parent;
	context.child = child;
	context.snapshot = parent_snapshot;
	status = minimm_page_table_for_each(parent->page_table, minimm_fork_copy_pte, &context);
	if (status != MINIMM_OK) {
		goto done;
	}

	/* This pass cannot allocate: all addresses came from the same table. */
	status = minimm_page_table_for_each(parent->page_table, minimm_fork_commit_parent_cow,
					    &context);
	if (status != MINIMM_OK) {
		goto done;
	}
	minimm_tlb_flush(parent->tlb);

	empty_snapshot = atomic_exchange_explicit(&child->vma_snapshot, child_snapshot,
						  memory_order_seq_cst);
	child_snapshot = NULL;
	minimm_vma_snapshot_destroy(empty_snapshot);
	child->bindings = child_bindings;
	child_bindings = NULL;
	child->mmap_base = parent->mmap_base;
	child->brk_base = parent->brk_base;
	child->brk_end = parent->brk_end;
	child->next_mapping_cookie = parent->next_mapping_cookie;
	*out_child = child;
	child = NULL;

done:
	(void)pthread_mutex_unlock(&parent->lock);
	(void)pthread_mutex_unlock(&parent->brk_lock);
	minimm_vma_snapshot_destroy(child_snapshot);
	minimm_space_binding_list_destroy(child_bindings);
	minimm_space_destroy(child);
	return status;
}
