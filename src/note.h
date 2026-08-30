#ifndef MINIMM_NOTE_H
#define MINIMM_NOTE_H

#include "minimm/minimm.h"

#include "frame.h"

#include <stddef.h>
#include <stdint.h>

#ifndef MINIMM_NOTE_RIGHTS_API
#define MINIMM_NOTE_RIGHTS_API

typedef uint32_t minimm_note_rights_t;

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

#endif

/* Create an anonymous note backed by an unlinked temporary file. */
minimm_status_t minimm_note_create(minimm_t *mm, uint64_t size, minimm_note_rights_t rights,
				   minimm_note_t **out_note);
minimm_status_t minimm_note_copy(minimm_note_t *source, minimm_note_rights_t rights,
				 minimm_note_t **out_note);

/* Duplicate fd; ownership of the caller's descriptor is never transferred. */
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

/* Internal mapping/page-cache operations. */
minimm_status_t minimm_note_mapping_attach(minimm_note_t *note);
void minimm_note_mapping_detach(minimm_note_t *note);
bool minimm_note_belongs_to(const minimm_note_t *note, const minimm_t *mm);
minimm_status_t minimm_note_get_frame(minimm_note_t *note, uint64_t page_offset,
				      minimm_frame_t **out_frame);
/* Retain an already cached effective page without creating or paging it in. */
minimm_status_t minimm_note_peek_frame(minimm_note_t *note, uint64_t page_offset,
				       minimm_frame_t **out_frame);
minimm_status_t minimm_note_sync_range(minimm_note_t *note, uint64_t offset, uint64_t length);

#endif
