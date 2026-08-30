#ifndef MINIMM_SPACE_H
#define MINIMM_SPACE_H

#include "minimm/minimm.h"

#include "mapping_backing.h"
#include "page_table.h"
#include "rcu.h"
#include "tlb.h"
#include "vma_tree.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

struct minimm_space {
	minimm_t *system;
	pthread_mutex_t lock;
	pthread_mutex_t brk_lock;
	minimm_page_table_t *page_table;
	minimm_tlb_t *tlb;
	minimm_rcu_domain_t *vma_rcu;
	_Atomic(minimm_vma_snapshot_t *) vma_snapshot;
	minimm_vaddr_t mmap_base;
	minimm_vaddr_t brk_base;
	minimm_vaddr_t brk_end;
	uint64_t next_mapping_cookie;
	uint64_t fault_sequence;
	minimm_fault_info_t fault_trace[MINIMM_FAULT_TRACE_CAPACITY];
	size_t fault_trace_start;
	size_t fault_trace_count;
	uint64_t fault_trace_overwritten_count;
	minimm_space_binding_t *bindings;
	atomic_bool closing;
};

/* Internal in-place heap growth that preserves a compatible tail binding. */
minimm_status_t minimm_mapping_extend_heap(minimm_space_t *space, minimm_vaddr_t start,
					   minimm_vaddr_t end);

#endif
