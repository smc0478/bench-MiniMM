#ifndef MINIMM_PAGE_REMAP_H
#define MINIMM_PAGE_REMAP_H

#include "minimm/minimm.h"

#include <stdint.h>

/* The caller serializes access to the server-owned internal note. */
minimm_status_t minimm_page_remap_apply(minimm_t *mm, minimm_note_t *note, uint64_t note_offset,
					minimm_prot_t *out_protection);

#endif
