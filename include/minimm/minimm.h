#ifndef MINIMM_MINIMM_H
#define MINIMM_MINIMM_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minimm minimm_t;
typedef struct minimm_space minimm_space_t;
typedef struct minimm_space_snapshot minimm_space_snapshot_t;
typedef struct minimm_note minimm_note_t;

#define MINIMM_PAGE_SHIFT 12U
#define MINIMM_PAGE_SIZE (UINT64_C(1) << MINIMM_PAGE_SHIFT)
#define MINIMM_PAGE_TABLE_LEVELS 4U
#define MINIMM_PAGE_TABLE_ENTRIES 512U
#define MINIMM_VIRTUAL_ADDRESS_BITS 48U
#define MINIMM_USER_ADDRESS_LIMIT (UINT64_C(1) << 47U)
#define MINIMM_ADDRESS_AUTO UINT64_MAX
#define MINIMM_PFN_NONE UINT64_MAX

typedef uint64_t minimm_vaddr_t;
typedef uint64_t minimm_pfn_t;
typedef uint32_t minimm_prot_t;
typedef uint32_t minimm_map_flags_t;
typedef uint32_t minimm_access_t;
typedef uint32_t minimm_note_rights_t;

enum {
	MINIMM_PROT_NONE = 0U,
	MINIMM_PROT_READ = UINT32_C(1) << 0,
	MINIMM_PROT_WRITE = UINT32_C(1) << 1,
	MINIMM_PROT_EDIT = UINT32_C(1) << 2,
	MINIMM_PROT_EXEC = UINT32_C(1) << 3
};

enum {
	MINIMM_ACCESS_READ = UINT32_C(1) << 0,
	MINIMM_ACCESS_WRITE = UINT32_C(1) << 1,
	MINIMM_ACCESS_EDIT = UINT32_C(1) << 2,
	MINIMM_ACCESS_EXECUTE = UINT32_C(1) << 3
};

#define MINIMM_NOTE_RIGHTS_API
enum {
	MINIMM_NOTE_RIGHT_NONE = 0U,
	MINIMM_NOTE_RIGHT_READ = UINT32_C(1) << 0,
	MINIMM_NOTE_RIGHT_WRITE = UINT32_C(1) << 1,
	MINIMM_NOTE_RIGHT_EDIT = UINT32_C(1) << 2,
	MINIMM_NOTE_RIGHT_SHARE = UINT32_C(1) << 3,
	MINIMM_NOTE_RIGHT_RESIZE = UINT32_C(1) << 4,
	MINIMM_NOTE_RIGHT_ALL = MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE |
				MINIMM_NOTE_RIGHT_EDIT | MINIMM_NOTE_RIGHT_SHARE |
				MINIMM_NOTE_RIGHT_RESIZE
};

enum {
	MINIMM_MAP_SHARED = UINT32_C(1) << 0,
	MINIMM_MAP_PRIVATE = UINT32_C(1) << 1,
	MINIMM_MAP_FIXED = UINT32_C(1) << 2,
	MINIMM_MAP_FIXED_NOREPLACE = UINT32_C(1) << 3,
	MINIMM_MAP_ANONYMOUS = UINT32_C(1) << 4,
	MINIMM_MAP_POPULATE = UINT32_C(1) << 5
};

enum {
	MINIMM_MREMAP_MAYMOVE = UINT32_C(1) << 0,
	MINIMM_MREMAP_FIXED = UINT32_C(1) << 1,
	MINIMM_MREMAP_DONTUNMAP = UINT32_C(1) << 2
};

#define MINIMM_MEMORY_API
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

typedef enum minimm_status {
	MINIMM_OK = 0,
	MINIMM_ERROR_INVALID_ARGUMENT,
	MINIMM_ERROR_OUT_OF_MEMORY,
	MINIMM_ERROR_NO_SPACE,
	MINIMM_ERROR_IO,
	MINIMM_ERROR_BUSY,
	MINIMM_ERROR_NOT_FOUND,
	MINIMM_ERROR_PERMISSION,
	MINIMM_ERROR_ADDRESS_IN_USE,
	MINIMM_ERROR_UNSUPPORTED
} minimm_status_t;

typedef struct minimm_config {
	size_t physical_memory_size;
	size_t page_size;
	size_t tlb_entries;
} minimm_config_t;

typedef struct minimm_mmap_args {
	minimm_vaddr_t address_hint;
	uint64_t length;
	uint64_t note_offset;
	minimm_prot_t protection;
	minimm_prot_t maximum_protection;
	minimm_map_flags_t flags;
	minimm_note_t *note;
} minimm_mmap_args_t;

typedef struct minimm_mapping_info {
	minimm_vaddr_t start;
	minimm_vaddr_t end;
	uint64_t mapping_cookie;
	uint64_t note_offset;
	minimm_prot_t protection;
	minimm_prot_t maximum_protection;
	minimm_map_flags_t flags;
} minimm_mapping_info_t;

typedef enum minimm_fault_reason {
	MINIMM_FAULT_NONE = 0,
	MINIMM_FAULT_UNMAPPED,
	MINIMM_FAULT_NOT_PRESENT,
	MINIMM_FAULT_PERMISSION,
	MINIMM_FAULT_COW,
	MINIMM_FAULT_BACKING_IO,
	MINIMM_FAULT_NO_FRAME
} minimm_fault_reason_t;

typedef enum minimm_fault_resolution {
	MINIMM_FAULT_UNRESOLVED = 0,
	MINIMM_FAULT_NO_ACTION,
	MINIMM_FAULT_ZERO_FILLED,
	MINIMM_FAULT_PAGE_IN,
	MINIMM_FAULT_COW_COPIED,
	MINIMM_FAULT_DENIED
} minimm_fault_resolution_t;

typedef enum minimm_fault_origin {
	MINIMM_FAULT_ORIGIN_NONE = 0,
	MINIMM_FAULT_ORIGIN_EXPLICIT,
	MINIMM_FAULT_ORIGIN_ACCESS
} minimm_fault_origin_t;

typedef struct minimm_fault_info {
	uint64_t sequence;
	minimm_vaddr_t address;
	minimm_vaddr_t page_address;
	minimm_access_t access;
	minimm_fault_origin_t origin;
	minimm_fault_reason_t reason;
	minimm_fault_resolution_t resolution;
	minimm_status_t status;
} minimm_fault_info_t;

#define MINIMM_FAULT_TRACE_CAPACITY 64U
typedef struct minimm_fault_trace {
	size_t count;
	uint64_t overwritten_count;
	minimm_fault_info_t events[MINIMM_FAULT_TRACE_CAPACITY];
} minimm_fault_trace_t;

typedef struct minimm_page_info {
	minimm_vaddr_t page_address;
	minimm_pfn_t pfn;
	minimm_prot_t protection;
	bool present;
	bool resident;
	bool dirty;
	bool accessed;
	bool cow;
	bool shared;
	bool locked;
	bool cold;
} minimm_page_info_t;

/* One installed sparse PTE copied into an immutable inspection snapshot. */
typedef struct minimm_space_snapshot_page {
	minimm_page_info_t page;
	uint64_t mapping_cookie;
	/* Model frame identity remains available while the frame is nonresident. */
	uint64_t frame_cookie;
	/* Number of installed PTEs referencing this frame across all spaces. */
	size_t frame_mapping_count;
} minimm_space_snapshot_page_t;

#define MINIMM_STATS_API
typedef struct minimm_reclaim_result {
	size_t scanned_count;
	size_t reclaimed_count;
} minimm_reclaim_result_t;

typedef struct minimm_system_stats {
	size_t frame_count;
	size_t resident_count;
	size_t resident_limit;
	uint64_t page_in_count;
	uint64_t page_out_count;
	uint64_t reclaim_scan_count;
	uint64_t reclaim_count;
	uint64_t refault_count;
} minimm_system_stats_t;

typedef struct minimm_space_stats {
	size_t vma_count;
	size_t pte_count;
	size_t present_count;
	size_t resident_count;
	size_t dirty_count;
	size_t cow_count;
	size_t shared_count;
	size_t locked_count;
	uint64_t fault_sequence;
	uint64_t tlb_hits;
	uint64_t tlb_misses;
	uint64_t tlb_replacements;
	uint64_t tlb_invalidations;
} minimm_space_stats_t;

/* Return a small, deterministic configuration suitable for examples and tests. */
minimm_config_t minimm_config_default(void);

/* Create a memory-model context and its file-backed physical-frame store. */
minimm_status_t minimm_create(const minimm_config_t *config, minimm_t **out_mm);

void minimm_destroy(minimm_t *mm);

minimm_status_t minimm_note_create(minimm_t *mm, uint64_t size, minimm_note_rights_t rights,
				   minimm_note_t **out_note);
/* Create an independent snapshot; internal notes split lazily by page on write. */
minimm_status_t minimm_note_copy(minimm_note_t *source, minimm_note_rights_t rights,
				 minimm_note_t **out_note);
minimm_status_t minimm_note_open_fd(minimm_t *mm, int fd, minimm_note_rights_t rights,
				    minimm_note_t **out_note);
void minimm_note_retain(minimm_note_t *note);
void minimm_note_release(minimm_note_t *note);
uint64_t minimm_note_id(const minimm_note_t *note);
uint64_t minimm_note_size(const minimm_note_t *note);
minimm_note_rights_t minimm_note_rights(const minimm_note_t *note);
minimm_status_t minimm_note_resize(minimm_note_t *note, uint64_t size);
minimm_status_t minimm_note_pread(minimm_note_t *note, uint64_t offset, void *destination,
				  size_t length, size_t *out_completed);
minimm_status_t minimm_note_pwrite(minimm_note_t *note, uint64_t offset, const void *source,
				   size_t length, size_t *out_completed);
minimm_status_t minimm_note_pedit(minimm_note_t *note, uint64_t offset, const void *source,
				  size_t length, size_t *out_completed);
minimm_status_t minimm_note_flush(minimm_note_t *note);

/* A space models one Linux-like mm_struct/address space. */
minimm_status_t minimm_space_create(minimm_t *mm, minimm_space_t **out_space);
minimm_status_t minimm_space_fork(minimm_space_t *parent, minimm_space_t **out_child);
/* The caller must ensure no API call races with destruction of the handle. */
void minimm_space_destroy(minimm_space_t *space);

minimm_status_t minimm_brk(minimm_space_t *space, minimm_vaddr_t requested_end,
			   minimm_vaddr_t *out_current_end);
minimm_status_t minimm_sbrk(minimm_space_t *space, intptr_t increment,
			    minimm_vaddr_t *out_previous_end);

minimm_status_t minimm_mmap(minimm_space_t *space, const minimm_mmap_args_t *args,
			    minimm_vaddr_t *out_address);
/* Clone one private mapping; future-writable present frames become copy-on-write. */
minimm_status_t minimm_mapping_copy(minimm_space_t *space, minimm_vaddr_t source_address,
				    uint64_t length, minimm_vaddr_t destination_hint,
				    minimm_vaddr_t *out_address);
minimm_status_t minimm_munmap(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);
minimm_status_t minimm_mremap(minimm_space_t *space, minimm_vaddr_t old_address,
			      uint64_t old_length, uint64_t new_length, uint32_t flags,
			      minimm_vaddr_t new_address_hint, minimm_vaddr_t *out_address);
minimm_status_t minimm_mprotect(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
				minimm_prot_t protection);
minimm_status_t minimm_page_protect(minimm_space_t *space, minimm_vaddr_t page_address,
				    minimm_prot_t protection);
minimm_status_t minimm_mapping_query(minimm_space_t *space, minimm_vaddr_t address,
				     minimm_mapping_info_t *out_mapping);
minimm_status_t minimm_msync(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);
minimm_status_t minimm_mincore(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       uint8_t *page_flags, size_t page_flags_count);
minimm_status_t minimm_mlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);
minimm_status_t minimm_munlock(minimm_space_t *space, minimm_vaddr_t address, uint64_t length);
minimm_status_t minimm_madvise(minimm_space_t *space, minimm_vaddr_t address, uint64_t length,
			       minimm_advice_t advice);

minimm_status_t minimm_handle_page_fault(minimm_space_t *space, minimm_vaddr_t address,
					 minimm_access_t access, minimm_fault_info_t *out_fault);
/* Snapshot recent handler results in chronological order, including automatic faults. */
minimm_status_t minimm_space_get_fault_trace(minimm_space_t *space,
					     minimm_fault_trace_t *out_trace);
/* Clear only the bounded trace; the lifetime fault sequence is not reset. */
minimm_status_t minimm_space_clear_fault_trace(minimm_space_t *space);
minimm_status_t minimm_query_page(minimm_space_t *space, minimm_vaddr_t address,
				  minimm_page_info_t *out_page);
minimm_status_t minimm_translate(minimm_space_t *space, minimm_vaddr_t address,
				 minimm_pfn_t *out_pfn, uint16_t *out_page_offset);
minimm_status_t minimm_read(minimm_space_t *space, minimm_vaddr_t source, void *destination,
			    size_t length, size_t *out_completed);
minimm_status_t minimm_write(minimm_space_t *space, minimm_vaddr_t destination, const void *source,
			     size_t length, size_t *out_completed);
minimm_status_t minimm_edit(minimm_space_t *space, minimm_vaddr_t destination, const void *source,
			    size_t length, size_t *out_completed);

minimm_status_t minimm_system_get_stats(const minimm_t *mm, minimm_system_stats_t *out_stats);
minimm_status_t minimm_system_reclaim(minimm_t *mm, size_t target_pages,
				      minimm_reclaim_result_t *out_result);
minimm_status_t minimm_space_get_stats(minimm_space_t *space, minimm_space_stats_t *out_stats);
minimm_status_t minimm_space_flush_tlb(minimm_space_t *space);

/*
 * Capture an immutable value copy of one space's VMA and installed-PTE state.
 * The result does not retain the source space or its frames and remains valid
 * after either is changed or destroyed.
 */
minimm_status_t minimm_space_snapshot_capture(minimm_space_t *space,
					      minimm_space_snapshot_t **out_snapshot);
void minimm_space_snapshot_destroy(minimm_space_snapshot_t *snapshot);

/* Scalar snapshot accessors return zero for a null snapshot. */
size_t minimm_space_snapshot_mapping_count(const minimm_space_snapshot_t *snapshot);
size_t minimm_space_snapshot_page_count(const minimm_space_snapshot_t *snapshot);
uint64_t minimm_space_snapshot_vma_generation(const minimm_space_snapshot_t *snapshot);
uint64_t minimm_space_snapshot_page_table_generation(const minimm_space_snapshot_t *snapshot);

/*
 * Value-copy accessors return INVALID_ARGUMENT for a null handle/output and
 * NOT_FOUND for an index outside the snapshot.
 */
minimm_status_t minimm_space_snapshot_get_stats(const minimm_space_snapshot_t *snapshot,
						minimm_space_stats_t *out_stats);
minimm_status_t minimm_space_snapshot_get_mapping(const minimm_space_snapshot_t *snapshot,
						  size_t index, minimm_mapping_info_t *out_mapping);
minimm_status_t minimm_space_snapshot_get_page(const minimm_space_snapshot_t *snapshot,
					       size_t index,
					       minimm_space_snapshot_page_t *out_page);

size_t minimm_physical_memory_size(const minimm_t *mm);
size_t minimm_page_size(const minimm_t *mm);
size_t minimm_page_count(const minimm_t *mm);

const char *minimm_version(void);
const char *minimm_status_string(minimm_status_t status);

#ifdef __cplusplus
}
#endif

#endif
