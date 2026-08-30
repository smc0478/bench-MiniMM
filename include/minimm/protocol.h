#ifndef MINIMM_PROTOCOL_H
#define MINIMM_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All multibyte wire integers use network (big-endian) byte order. */
#define MINIMM_PROTOCOL_MAGIC UINT32_C(0x4d494d4d) /* ASCII "MIMM" */
#define MINIMM_PROTOCOL_VERSION_MAJOR UINT8_C(1)
#define MINIMM_PROTOCOL_VERSION_MINOR UINT8_C(0)
#define MINIMM_PROTOCOL_HEADER_SIZE UINT16_C(32)
#define MINIMM_PROTOCOL_FLAG_RESPONSE UINT16_C(1)
#define MINIMM_PROTOCOL_MAX_DATA_SIZE UINT32_C(1048576)
#define MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE (MINIMM_PROTOCOL_MAX_DATA_SIZE + UINT32_C(24))

#define MINIMM_PROTOCOL_CAPABILITY_SIZE 16U

typedef struct minimm_protocol_capability {
	uint8_t bytes[MINIMM_PROTOCOL_CAPABILITY_SIZE];
} minimm_protocol_capability_t;

typedef minimm_protocol_capability_t minimm_capability_t;

typedef enum minimm_protocol_opcode {
	MINIMM_PROTOCOL_OP_HELLO = 0x0001,
	MINIMM_PROTOCOL_OP_PING = 0x0002,
	MINIMM_PROTOCOL_OP_CREATE = 0x0100,
	MINIMM_PROTOCOL_OP_OPEN = 0x0101,
	MINIMM_PROTOCOL_OP_CLOSE = 0x0102,
	MINIMM_PROTOCOL_OP_STAT = 0x0103,
	MINIMM_PROTOCOL_OP_READ = 0x0104,
	MINIMM_PROTOCOL_OP_WRITE = 0x0105,
	MINIMM_PROTOCOL_OP_EDIT = 0x0106,
	MINIMM_PROTOCOL_OP_RESIZE = 0x0107,
	MINIMM_PROTOCOL_OP_FLUSH = 0x0108,
	MINIMM_PROTOCOL_OP_UNLINK = 0x0109,
	MINIMM_PROTOCOL_OP_COPY = 0x010a,
	MINIMM_PROTOCOL_OP_PREVIEW = 0x010b,
	MINIMM_PROTOCOL_OP_STACK_EXPAND = 0x010c,
	MINIMM_PROTOCOL_OP_REMAP_PAGE = 0x010d,
	MINIMM_PROTOCOL_OP_MSEAL_MERGE = 0x010e,
	MINIMM_PROTOCOL_OP_MGLRU_REPARENT = 0x010f,
	MINIMM_PROTOCOL_OP_RMAP_UNMAP = 0x0110,
	MINIMM_PROTOCOL_OP_UFFD_MOVE = 0x0111,
	MINIMM_PROTOCOL_OP_HUGETLB_RESERVE = 0x0112,
	MINIMM_PROTOCOL_OP_PERCPU_POPULATE = 0x0113
} minimm_protocol_opcode_t;

/* These values are stable wire values, not minimm_status_t values. */
typedef enum minimm_protocol_wire_status {
	MINIMM_PROTOCOL_STATUS_OK = 0,
	MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE = 1,
	MINIMM_PROTOCOL_STATUS_UNSUPPORTED_VERSION = 2,
	MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE = 3,
	MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT = 4,
	MINIMM_PROTOCOL_STATUS_NOT_FOUND = 5,
	MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED = 6,
	MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY = 7,
	MINIMM_PROTOCOL_STATUS_NO_SPACE = 8,
	MINIMM_PROTOCOL_STATUS_IO_ERROR = 9,
	MINIMM_PROTOCOL_STATUS_BUSY = 10,
	MINIMM_PROTOCOL_STATUS_ADDRESS_IN_USE = 11,
	MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED = 12,
	MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR = 13
} minimm_protocol_wire_status_t;

/* Remote note rights match the core note rights; DELETE is protocol-only. */
typedef uint32_t minimm_protocol_rights_t;

typedef minimm_protocol_rights_t minimm_remote_rights_t;

enum {
	MINIMM_PROTOCOL_RIGHT_READ = UINT32_C(1) << 0,
	MINIMM_PROTOCOL_RIGHT_WRITE = UINT32_C(1) << 1,
	MINIMM_PROTOCOL_RIGHT_EDIT = UINT32_C(1) << 2,
	MINIMM_PROTOCOL_RIGHT_SHARE = UINT32_C(1) << 3,
	MINIMM_PROTOCOL_RIGHT_RESIZE = UINT32_C(1) << 4,
	MINIMM_PROTOCOL_RIGHT_DELETE = UINT32_C(1) << 5,
	MINIMM_PROTOCOL_RIGHT_ALL = MINIMM_PROTOCOL_RIGHT_READ | MINIMM_PROTOCOL_RIGHT_WRITE |
				    MINIMM_PROTOCOL_RIGHT_EDIT | MINIMM_PROTOCOL_RIGHT_SHARE |
				    MINIMM_PROTOCOL_RIGHT_RESIZE | MINIMM_PROTOCOL_RIGHT_DELETE
};

#define MINIMM_REMOTE_RIGHT_READ MINIMM_PROTOCOL_RIGHT_READ
#define MINIMM_REMOTE_RIGHT_WRITE MINIMM_PROTOCOL_RIGHT_WRITE
#define MINIMM_REMOTE_RIGHT_EDIT MINIMM_PROTOCOL_RIGHT_EDIT
#define MINIMM_REMOTE_RIGHT_SHARE MINIMM_PROTOCOL_RIGHT_SHARE
#define MINIMM_REMOTE_RIGHT_RESIZE MINIMM_PROTOCOL_RIGHT_RESIZE
#define MINIMM_REMOTE_RIGHT_DELETE MINIMM_PROTOCOL_RIGHT_DELETE
#define MINIMM_REMOTE_RIGHT_ALL MINIMM_PROTOCOL_RIGHT_ALL

/*
 * Exact v1 payload layouts, with offsets measured from the payload start:
 *
 * HELLO request (16):
 *   0:u8 min_major, 1:u8 min_minor, 2:u8 max_major, 3:u8 max_minor,
 *   4:u32 feature_bits, 8:u32 receive_max_payload, 12:u32 reserved.
 * HELLO response (32):
 *   0:u8 selected_major, 1:u8 selected_minor, 2:u16 reserved,
 *   4:u32 feature_bits, 8:u32 negotiated_max_payload,
 *   12:u32 max_handles, 16:u32 page_size, 20:u32 max_inflight,
 *   24:u64 max_note_size.
 * PING request/response (8): 0:u64 nonce.
 * CREATE request (16): 0:u64 size, 8:u32 rights, 12:u32 flags.
 * CREATE response (40): 0:u64 handle, 8:u8 capability[16], 24:u64 size,
 *   32:u32 rights, 36:u32 reserved.
 * COPY request (16): 0:u64 source_handle, 8:u32 destination_rights,
 *   12:u32 flags.
 * COPY response (40): same layout as CREATE response.
 * OPEN request (24): 0:u8 capability[16], 16:u32 requested_rights,
 *   20:u32 flags.
 * OPEN response (24): 0:u64 handle, 8:u64 size, 16:u32 rights,
 *   20:u32 reserved.
 * CLOSE request (8): 0:u64 handle. Response has no payload.
 * STAT request (8): 0:u64 handle.
 * STAT response (16): 0:u64 size, 8:u32 rights, 12:u32 flags.
 * READ request (24): 0:u64 handle, 8:u64 offset, 16:u32 length,
 *   20:u32 flags.
 * READ response (8+N): 0:u32 completed, 4:u32 reserved, 8:u8 data[N].
 * WRITE/EDIT/PREVIEW/STACK_EXPAND request (24+N): 0:u64 handle, 8:u64 offset,
 *   16:u32 data_length, 20:u32 flags, 24:u8 data[N].
 * WRITE/EDIT/PREVIEW/STACK_EXPAND response (8):
 *   0:u32 completed, 4:u32 reserved.
 * PREVIEW and STACK_EXPAND require 1 <= N <= page_size and may not cross a
 * page boundary.
 * REMAP_PAGE request (16): 0:u64 handle, 8:u64 note_offset.
 * REMAP_PAGE response (8): 0:u32 protection, 4:u32 reserved.
 * MSEAL_MERGE request (8): 0:u64 handle.
 * MSEAL_MERGE response (32): 0:u32 total_pages, 4:u32 sealed_pages,
 *   8:u32 range_valid, 12:u32 reserved, 16:u64 update_start,
 *   24:u64 current_start.
 * MGLRU_REPARENT request (8): 0:u64 handle.
 * MGLRU_REPARENT response (32): 0:u32 total_pages, 4:u32 parent_old_pages,
 *   8:u32 parent_new_pages, 12:u32 child_old_debt_pages,
 *   16:u32 child_new_credit_pages, 20:u32 exit_clean,
 *   24:u32 accounting_valid, 28:u32 reserved.
 * RMAP_UNMAP request (24): 0:u64 handle, 8:u32 pte_capacity,
 *   12:u32 pte_index, 16:u32 folio_pages, 20:u32 vma_remaining.
 * RMAP_UNMAP response (24): 0:u32 requested_pages, 4:u32 scanned_pages,
 *   8:u32 safe_pages, 12:u32 first_invalid_index,
 *   16:u32 crossed_pte_boundary, 20:u32 bounds_valid.
 * UFFD_MOVE request (24): 0:u64 handle, 8:u32 swap_entry,
 *   12:u32 source_folio, 16:u32 replacement_folio, 20:u32 reserved.
 * UFFD_MOVE response (24): 0:u32 swap_entry, 4:u32 expected_folio,
 *   8:u32 moved_folio, 12:u32 pte_entry_matches,
 *   16:u32 folio_identity_valid, 20:u32 accounting_valid.
 * HUGETLB_RESERVE request (32): 0:u64 handle, 8:u32 maximum_pages,
 *   12:u32 minimum_pages, 16:u32 used_before, 20:u32 requested_pages,
 *   24:u32 global_free_pages, 28:u32 reserved.
 * HUGETLB_RESERVE response (32): 0:u32 requested_pages,
 *   4:u32 global_needed_pages, 8:u32 allocated_pages, 12:u32 used_before,
 *   16:u32 used_after, 20:u32 rollback_pages,
 *   24:u32 reservation_succeeded, 28:u32 accounting_valid.
 * PERCPU_POPULATE request (16): 0:u64 handle, 8:u32 unit_count,
 *   12:u32 unit_pages.
 * PERCPU_POPULATE response (32): 0:u32 total_backing_pages,
 *   4:u32 bitmap_capacity, 8:u32 mark_count, 12:u32 first_invalid_index,
 *   16:u32 empty_pages_after, 20:u32 expected_empty_pages,
 *   24:u32 bounds_valid, 28:u32 accounting_valid.
 * RESIZE request (16): 0:u64 handle, 8:u64 new_size.
 * RESIZE response (8): 0:u64 actual_size.
 * FLUSH request (8): 0:u64 handle. Response has no payload.
 * UNLINK request (16): 0:u8 capability[16]. Response has no payload.
 *
 * All v1 flags and reserved payload fields are zero. Other data requests allow
 * N up to 1 MiB.
 */
#define MINIMM_PROTOCOL_HELLO_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_PING_PAYLOAD_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_CREATE_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE UINT32_C(40)
#define MINIMM_PROTOCOL_COPY_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_COPY_RESPONSE_SIZE MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE
#define MINIMM_PROTOCOL_OPEN_REQUEST_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_OPEN_RESPONSE_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_CLOSE_REQUEST_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_STAT_REQUEST_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_STAT_RESPONSE_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_READ_REQUEST_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_EDIT_REQUEST_FIXED_SIZE MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE
#define MINIMM_PROTOCOL_EDIT_RESPONSE_SIZE MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE
#define MINIMM_PROTOCOL_PREVIEW_REQUEST_FIXED_SIZE MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE
#define MINIMM_PROTOCOL_PREVIEW_RESPONSE_SIZE MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE
#define MINIMM_PROTOCOL_STACK_EXPAND_REQUEST_FIXED_SIZE MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE
#define MINIMM_PROTOCOL_STACK_EXPAND_RESPONSE_SIZE MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE
#define MINIMM_PROTOCOL_REMAP_PAGE_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_REMAP_PAGE_RESPONSE_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_MSEAL_MERGE_REQUEST_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_MGLRU_REPARENT_REQUEST_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_MGLRU_REPARENT_RESPONSE_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_RMAP_UNMAP_REQUEST_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_RMAP_UNMAP_RESPONSE_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_UFFD_MOVE_REQUEST_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_UFFD_MOVE_RESPONSE_SIZE UINT32_C(24)
#define MINIMM_PROTOCOL_HUGETLB_RESERVE_REQUEST_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_HUGETLB_RESERVE_RESPONSE_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_PERCPU_POPULATE_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_PERCPU_POPULATE_RESPONSE_SIZE UINT32_C(32)
#define MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE UINT32_C(16)
#define MINIMM_PROTOCOL_RESIZE_RESPONSE_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_FLUSH_REQUEST_SIZE UINT32_C(8)
#define MINIMM_PROTOCOL_UNLINK_REQUEST_SIZE UINT32_C(16)

#ifdef __cplusplus
}
#endif

#endif
