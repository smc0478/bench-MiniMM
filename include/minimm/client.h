#ifndef MINIMM_CLIENT_H
#define MINIMM_CLIENT_H

#include "minimm/minimm.h"
#include "minimm/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minimm_client minimm_client_t;

typedef struct minimm_remote_note {
	uint64_t handle;
	uint64_t size;
	minimm_remote_rights_t rights;
	minimm_capability_t capability;
} minimm_remote_note_t;

typedef struct minimm_remote_note_info {
	uint64_t size;
	minimm_remote_rights_t rights;
	uint32_t flags;
} minimm_remote_note_info_t;

typedef struct minimm_remote_mseal_merge_result {
	uint32_t total_pages;
	uint32_t sealed_pages;
	bool range_valid;
	uint64_t update_start;
	uint64_t current_start;
} minimm_remote_mseal_merge_result_t;

typedef struct minimm_remote_mglru_reparent_result {
	uint32_t total_pages;
	uint32_t parent_old_pages;
	uint32_t parent_new_pages;
	uint32_t child_old_debt_pages;
	uint32_t child_new_credit_pages;
	bool exit_clean;
	bool accounting_valid;
} minimm_remote_mglru_reparent_result_t;

typedef struct minimm_remote_rmap_unmap_result {
	uint32_t requested_pages;
	uint32_t scanned_pages;
	uint32_t safe_pages;
	uint32_t first_invalid_index;
	bool crossed_pte_boundary;
	bool bounds_valid;
} minimm_remote_rmap_unmap_result_t;

typedef struct minimm_remote_uffd_move_result {
	uint32_t swap_entry;
	uint32_t expected_folio;
	uint32_t moved_folio;
	bool pte_entry_matches;
	bool folio_identity_valid;
	bool accounting_valid;
} minimm_remote_uffd_move_result_t;

typedef struct minimm_remote_hugetlb_reserve_result {
	uint32_t requested_pages;
	uint32_t global_needed_pages;
	uint32_t allocated_pages;
	uint32_t used_before;
	uint32_t used_after;
	uint32_t rollback_pages;
	bool reservation_succeeded;
	bool accounting_valid;
} minimm_remote_hugetlb_reserve_result_t;

typedef struct minimm_remote_percpu_populate_result {
	uint32_t total_backing_pages;
	uint32_t bitmap_capacity;
	uint32_t mark_count;
	uint32_t first_invalid_index;
	uint32_t empty_pages_after;
	uint32_t expected_empty_pages;
	bool bounds_valid;
	bool accounting_valid;
} minimm_remote_percpu_populate_result_t;

/*
 * Connect and complete the mandatory protocol handshake. timeout_ms == 0 uses
 * the client default. The deadline covers address attempts and socket I/O but
 * not a blocking system DNS lookup. Calls on one client are serialized
 * internally; the caller must not race minimm_client_disconnect() with
 * another client call.
 */
minimm_status_t minimm_client_connect(const char *host, uint16_t port, uint32_t timeout_ms,
				      minimm_client_t **out_client);
void minimm_client_disconnect(minimm_client_t *client);

uint32_t minimm_client_max_payload(const minimm_client_t *client);
uint64_t minimm_client_max_note_size(const minimm_client_t *client);

minimm_status_t minimm_client_ping(minimm_client_t *client, uint64_t nonce, uint64_t *out_nonce);

minimm_status_t minimm_client_note_create(minimm_client_t *client, uint64_t size,
					  minimm_remote_rights_t rights,
					  minimm_remote_note_t *out_note);
/* Create an independent point-in-time copy. The source handle requires READ. */
minimm_status_t minimm_client_note_copy(minimm_client_t *client, uint64_t source_handle,
					minimm_remote_rights_t rights,
					minimm_remote_note_t *out_note);
minimm_status_t minimm_client_note_open(minimm_client_t *client,
					const minimm_capability_t *capability,
					minimm_remote_rights_t rights,
					minimm_remote_note_t *out_note);

/* Handles are opaque and valid only on the client connection that issued them. */
minimm_status_t minimm_client_note_close(minimm_client_t *client, minimm_remote_note_t *note);
minimm_status_t minimm_client_note_stat(minimm_client_t *client, uint64_t handle,
					minimm_remote_note_info_t *out_info);

/* The data APIs accept a null out_completed when the count is not needed. */
minimm_status_t minimm_client_note_read(minimm_client_t *client, uint64_t handle, uint64_t offset,
					void *destination, size_t length, size_t *out_completed);
minimm_status_t minimm_client_note_write(minimm_client_t *client, uint64_t handle, uint64_t offset,
					 const void *source, size_t length, size_t *out_completed);
minimm_status_t minimm_client_note_edit(minimm_client_t *client, uint64_t handle, uint64_t offset,
					const void *source, size_t length, size_t *out_completed);
/* Preview applies a nonempty, single-page range through a private view.
 * The handle requires READ and must not carry WRITE or EDIT. */
minimm_status_t minimm_client_note_preview(minimm_client_t *client, uint64_t handle,
					   uint64_t offset, const void *source, size_t length,
					   size_t *out_completed);
/* Stack expansion applies a nonempty, single-page marker to a transient
 * private stack view. The handle requires READ and must not carry WRITE or
 * EDIT. */
minimm_status_t minimm_client_note_stack_expand(minimm_client_t *client, uint64_t handle,
						uint64_t offset, const void *source, size_t length,
						size_t *out_completed);
/* Remap the first page of a transient shared view to note_offset and report
 * its effective protection. The handle requires READ, WRITE, and SHARE. */
minimm_status_t minimm_client_note_remap_page(minimm_client_t *client, uint64_t handle,
					      uint64_t note_offset, minimm_prot_t *out_protection);
/* Apply the bounded transient mseal merge model and report its final range
 * metadata. The handle requires READ, WRITE, and SHARE. */
minimm_status_t minimm_client_note_mseal_merge(minimm_client_t *client, uint64_t handle,
					       minimm_remote_mseal_merge_result_t *out_result);
/* Run the bounded transient MGLRU reparenting model and report generation
 * accounting metadata. The handle requires READ, WRITE, and SHARE. */
minimm_status_t
minimm_client_note_mglru_reparent(minimm_client_t *client, uint64_t handle,
				  minimm_remote_mglru_reparent_result_t *out_result);
/* Run the transient rmap unmap batch model and report traversal metadata. */
minimm_status_t minimm_client_note_rmap_unmap(minimm_client_t *client, uint64_t handle,
					      uint32_t pte_capacity, uint32_t pte_index,
					      uint32_t folio_pages, uint32_t vma_remaining,
					      minimm_remote_rmap_unmap_result_t *out_result);
/* Run the transient userfaultfd move model and report folio metadata. */
minimm_status_t minimm_client_note_uffd_move(minimm_client_t *client, uint64_t handle,
					     uint32_t swap_entry, uint32_t source_folio,
					     uint32_t replacement_folio,
					     minimm_remote_uffd_move_result_t *out_result);
/* Run the transient hugetlb reservation model and report accounting metadata. */
minimm_status_t
minimm_client_note_hugetlb_reserve(minimm_client_t *client, uint64_t handle, uint32_t maximum_pages,
				   uint32_t minimum_pages, uint32_t used_before,
				   uint32_t requested_pages, uint32_t global_free_pages,
				   minimm_remote_hugetlb_reserve_result_t *out_result);
/* Run the transient per-CPU population model from explicit unit geometry. */
minimm_status_t
minimm_client_note_percpu_populate(minimm_client_t *client, uint64_t handle, uint32_t unit_count,
				   uint32_t unit_pages,
				   minimm_remote_percpu_populate_result_t *out_result);
/* A failed server resize reports the unchanged size when its progress payload is present. */
minimm_status_t minimm_client_note_resize(minimm_client_t *client, uint64_t handle,
					  uint64_t new_size, uint64_t *out_actual_size);
minimm_status_t minimm_client_note_flush(minimm_client_t *client, uint64_t handle);

/*
 * Unlink removes the capability from the server registry. Existing handles
 * remain valid until they are closed, like an open file after unlink(2).
 */
minimm_status_t minimm_client_note_unlink(minimm_client_t *client,
					  const minimm_capability_t *capability);

#define MINIMM_CAPABILITY_HEX_LENGTH (MINIMM_PROTOCOL_CAPABILITY_SIZE * 2U)
#define MINIMM_CAPABILITY_HEX_BUFFER_SIZE (MINIMM_CAPABILITY_HEX_LENGTH + 1U)

minimm_status_t minimm_capability_format(const minimm_capability_t *capability,
					 char output[MINIMM_CAPABILITY_HEX_BUFFER_SIZE]);
minimm_status_t minimm_capability_parse(const char *text, minimm_capability_t *out_capability);

#ifdef __cplusplus
}
#endif

#endif
