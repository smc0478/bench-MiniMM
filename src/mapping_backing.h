#ifndef MINIMM_MAPPING_BACKING_H
#define MINIMM_MAPPING_BACKING_H

#include "frame.h"
#include "note.h"
#include "vma_tree.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum minimm_backing_kind {
	MINIMM_BACKING_ANON_PRIVATE = 0,
	MINIMM_BACKING_ANON_SHARED,
	MINIMM_BACKING_NOTE_PRIVATE,
	MINIMM_BACKING_NOTE_SHARED
} minimm_backing_kind_t;

typedef struct minimm_mapping_backing minimm_mapping_backing_t;

typedef struct minimm_space_binding {
	struct minimm_space_binding *next;
	minimm_mapping_backing_t *backing;
	uint64_t cookie;
} minimm_space_binding_t;

minimm_status_t minimm_mapping_backing_create(minimm_t *system, minimm_note_t *note,
					      minimm_map_flags_t flags,
					      minimm_mapping_backing_t **out_backing);
void minimm_mapping_backing_retain(minimm_mapping_backing_t *backing);
void minimm_mapping_backing_release(minimm_mapping_backing_t *backing);
minimm_backing_kind_t minimm_mapping_backing_kind(const minimm_mapping_backing_t *backing);
minimm_note_t *minimm_mapping_backing_note(const minimm_mapping_backing_t *backing);
minimm_status_t minimm_mapping_backing_get_frame(minimm_mapping_backing_t *backing,
						 uint64_t page_offset, minimm_frame_t **out_frame,
						 bool *out_created);
minimm_status_t minimm_mapping_backing_peek_frame(minimm_mapping_backing_t *backing,
						  uint64_t page_offset, minimm_frame_t **out_frame);

minimm_status_t minimm_space_binding_create(minimm_mapping_backing_t *backing,
					    minimm_space_binding_t **out_binding);
void minimm_space_binding_list_destroy(minimm_space_binding_t *bindings);
minimm_space_binding_t *minimm_space_binding_find(minimm_space_binding_t *bindings,
						  uint64_t cookie);
minimm_space_binding_t *minimm_space_binding_prune(minimm_space_binding_t **bindings,
						   const minimm_vma_snapshot_t *snapshot);
minimm_status_t minimm_space_binding_list_clone(const minimm_space_binding_t *bindings,
						minimm_space_binding_t **out_bindings);

#endif
