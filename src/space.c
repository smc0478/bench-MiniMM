#include "space.h"

#include "internal.h"

#include <stdlib.h>

#define MINIMM_BRK_BASE UINT64_C(0x100000000)

static void minimm_space_cleanup_partial(minimm_space_t *space)
{
	minimm_vma_snapshot_t *snapshot = NULL;

	if (space == NULL) {
		return;
	}

	snapshot = atomic_load_explicit(&space->vma_snapshot, memory_order_seq_cst);
	minimm_vma_snapshot_destroy(snapshot);
	minimm_rcu_domain_destroy(space->vma_rcu);
	minimm_tlb_destroy(space->tlb);
	minimm_page_table_destroy(space->page_table);
	minimm_space_binding_list_destroy(space->bindings);
	(void)pthread_mutex_destroy(&space->brk_lock);
	(void)pthread_mutex_destroy(&space->lock);
	minimm_system_release(space->system);
	free(space);
}

minimm_status_t minimm_space_create(minimm_t *mm, minimm_space_t **out_space)
{
	minimm_space_t *space = NULL;
	minimm_vma_snapshot_t *snapshot = NULL;
	minimm_status_t status = MINIMM_OK;

	if (out_space == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_space = NULL;
	if (mm == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	if (!minimm_system_try_retain(mm)) {
		return MINIMM_ERROR_BUSY;
	}

	space = calloc(1U, sizeof(*space));
	if (space == NULL) {
		minimm_system_release(mm);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	space->system = mm;
	atomic_init(&space->vma_snapshot, NULL);
	atomic_init(&space->closing, false);

	if (pthread_mutex_init(&space->lock, NULL) != 0) {
		minimm_system_release(mm);
		free(space);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&space->brk_lock, NULL) != 0) {
		(void)pthread_mutex_destroy(&space->lock);
		minimm_system_release(mm);
		free(space);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	status = minimm_page_table_create(&space->page_table);
	if (status != MINIMM_OK) {
		minimm_space_cleanup_partial(space);
		return status;
	}
	status = minimm_tlb_create(mm->tlb_entries, &space->tlb);
	if (status != MINIMM_OK) {
		minimm_space_cleanup_partial(space);
		return status;
	}
	status = minimm_rcu_domain_create(&space->vma_rcu);
	if (status != MINIMM_OK) {
		minimm_space_cleanup_partial(space);
		return status;
	}
	status = minimm_vma_snapshot_create(&snapshot);
	if (status != MINIMM_OK) {
		minimm_space_cleanup_partial(space);
		return status;
	}

	atomic_store_explicit(&space->vma_snapshot, snapshot, memory_order_seq_cst);
	space->mmap_base = UINT64_C(0x100000);
	space->brk_base = MINIMM_BRK_BASE;
	space->brk_end = MINIMM_BRK_BASE;
	space->next_mapping_cookie = 1U;

	*out_space = space;
	return MINIMM_OK;
}

void minimm_space_destroy(minimm_space_t *space)
{
	minimm_vma_snapshot_t *snapshot = NULL;

	if (space == NULL) {
		return;
	}

	(void)pthread_mutex_lock(&space->lock);
	atomic_store_explicit(&space->closing, true, memory_order_release);
	snapshot = atomic_exchange_explicit(&space->vma_snapshot, NULL, memory_order_seq_cst);
	(void)pthread_mutex_unlock(&space->lock);

	(void)minimm_rcu_synchronize(space->vma_rcu);
	minimm_vma_snapshot_destroy(snapshot);

	minimm_tlb_destroy(space->tlb);
	minimm_page_table_destroy(space->page_table);
	minimm_space_binding_list_destroy(space->bindings);
	minimm_rcu_domain_destroy(space->vma_rcu);
	(void)pthread_mutex_destroy(&space->brk_lock);
	(void)pthread_mutex_destroy(&space->lock);
	minimm_system_release(space->system);
	free(space);
}
