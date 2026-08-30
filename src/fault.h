#ifndef MINIMM_FAULT_H
#define MINIMM_FAULT_H

#include "space.h"

#include <stdbool.h>

bool minimm_fault_access_is_allowed(minimm_prot_t protection, minimm_access_t access);

minimm_status_t minimm_handle_page_fault_locked(minimm_space_t *space, minimm_vaddr_t address,
						minimm_access_t access,
						minimm_fault_origin_t origin,
						minimm_fault_info_t *out_fault);

/*
 * Materialize one page without simulating a memory access. This path is used
 * by MAP_POPULATE while the caller holds space->lock: it does not require a
 * current VMA access permission, add ACCESSED/DIRTY flags, resolve COW, update
 * fault counters, or fill the TLB. Existing PTE attributes remain unchanged.
 */
minimm_status_t minimm_populate_page_locked(minimm_space_t *space, minimm_vaddr_t address);

#endif
