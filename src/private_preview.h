#ifndef MINIMM_PRIVATE_PREVIEW_H
#define MINIMM_PRIVATE_PREVIEW_H

#include "minimm/minimm.h"

#include <stddef.h>
#include <stdint.h>

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_private_preview_apply(minimm_t *mm, minimm_note_t *note, uint64_t offset,
					     const void *data, size_t length,
					     size_t *out_completed);

#endif
