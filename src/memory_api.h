#ifndef MINIMM_MEMORY_API_H
#define MINIMM_MEMORY_API_H

#include "minimm/minimm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MINIMM_MEMORY_API
typedef enum minimm_advice {
	MINIMM_MADV_NORMAL = 0,
	MINIMM_MADV_RANDOM,
	MINIMM_MADV_SEQUENTIAL,
	MINIMM_MADV_WILLNEED,
	MINIMM_MADV_DONTNEED,
	MINIMM_MADV_PAGEOUT,
	MINIMM_MADV_COLD
} minimm_advice_t;

enum {
	/* Linux-compatible residency ABI: callers may inspect only bit zero. */
	MINIMM_MINCORE_RESIDENT = UINT8_C(1) << 0,
	/* Higher bits are MiniMM diagnostics; minimm_query_page is the richer API. */
	MINIMM_MINCORE_PRESENT = UINT8_C(1) << 1,
	MINIMM_MINCORE_DIRTY = UINT8_C(1) << 2,
	MINIMM_MINCORE_LOCKED = UINT8_C(1) << 3,
	MINIMM_MINCORE_SHARED = UINT8_C(1) << 4,
	MINIMM_MINCORE_COW = UINT8_C(1) << 5,
	MINIMM_MINCORE_ACCESSED = UINT8_C(1) << 6
};

minimm_status_t minimm_msync(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);

/* page_flags_count must cover the page-aligned form of length. */
minimm_status_t minimm_mincore(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       uint8_t *page_flags, size_t page_flags_count);

minimm_status_t minimm_mlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);

minimm_status_t minimm_munlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);

minimm_status_t minimm_madvise(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       minimm_advice_t advice);
#endif

#ifdef __cplusplus
}
#endif

#endif
