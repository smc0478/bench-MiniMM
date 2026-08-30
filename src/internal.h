#ifndef MINIMM_INTERNAL_H
#define MINIMM_INTERNAL_H

#include "frame.h"

#include <stddef.h>
#include <stdatomic.h>

struct minimm {
	size_t physical_memory_size;
	size_t page_size;
	size_t page_count;
	size_t tlb_entries;
	minimm_frame_store_t *frame_store;
	atomic_size_t references;
	atomic_bool closing;
};

bool minimm_system_try_retain(minimm_t *mm);
void minimm_system_release(minimm_t *mm);

#endif
