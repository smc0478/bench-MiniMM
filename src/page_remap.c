#include "page_remap.h"

#include "note.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct minimm_page_remap_context {
	bool read_implies_exec;
} minimm_page_remap_context_t;

static minimm_prot_t
minimm_page_remap_effective_protection(const minimm_page_remap_context_t *context,
				       minimm_prot_t protection)
{
	if (context->read_implies_exec && (protection & MINIMM_PROT_READ) != 0U) {
		protection |= MINIMM_PROT_EXEC;
	}
	return protection;
}

static minimm_status_t
minimm_page_remap_security_mmap_file(const minimm_page_remap_context_t *context,
				     const minimm_mmap_args_t *args)
{
	const minimm_prot_t effective =
		minimm_page_remap_effective_protection(context, args->protection);

	if ((args->flags & MINIMM_MAP_ANONYMOUS) == 0U &&
	    (effective & (MINIMM_PROT_WRITE | MINIMM_PROT_EXEC)) ==
		    (MINIMM_PROT_WRITE | MINIMM_PROT_EXEC)) {
		return MINIMM_ERROR_PERMISSION;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_page_remap_do_mmap(minimm_space_t *space,
						 const minimm_page_remap_context_t *context,
						 const minimm_mmap_args_t *args,
						 minimm_vaddr_t *out_address)
{
	minimm_mmap_args_t effective_args = *args;

	effective_args.protection =
		minimm_page_remap_effective_protection(context, effective_args.protection);
	return minimm_mmap(space, &effective_args, out_address);
}

static minimm_status_t minimm_page_remap_checked_mmap(minimm_space_t *space,
						      const minimm_page_remap_context_t *context,
						      const minimm_mmap_args_t *args,
						      minimm_vaddr_t *out_address)
{
	minimm_status_t status = minimm_page_remap_security_mmap_file(context, args);

	if (status != MINIMM_OK) {
		return status;
	}
	return minimm_page_remap_do_mmap(space, context, args, out_address);
}

static minimm_status_t minimm_page_remap_file_pages(minimm_space_t *space, minimm_note_t *note,
						    minimm_vaddr_t address, uint64_t note_offset)
{
	const minimm_page_remap_context_t context = {
		.read_implies_exec = true,
	};
	minimm_mapping_info_t mapping = { 0 };
	minimm_mmap_args_t args = { 0 };
	minimm_vaddr_t remapped = MINIMM_ADDRESS_AUTO;
	minimm_status_t status = minimm_mapping_query(space, address, &mapping);

	if (status != MINIMM_OK) {
		return status;
	}
	if (mapping.start != address || mapping.flags != MINIMM_MAP_SHARED ||
	    mapping.protection != (MINIMM_PROT_READ | MINIMM_PROT_WRITE) ||
	    (mapping.maximum_protection &
	     (MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EXEC)) !=
		    (MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EXEC)) {
		return MINIMM_ERROR_IO;
	}

	args.address_hint = address;
	args.length = MINIMM_PAGE_SIZE;
	args.note_offset = note_offset;
	args.protection = mapping.protection;
	args.maximum_protection = mapping.maximum_protection;
	args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_FIXED;
	args.note = note;
	status = minimm_page_remap_do_mmap(space, &context, &args, &remapped);
	if (status != MINIMM_OK) {
		return status;
	}
	return remapped == address ? MINIMM_OK : MINIMM_ERROR_IO;
}

minimm_status_t minimm_page_remap_apply(minimm_t *mm, minimm_note_t *note, uint64_t note_offset,
					minimm_prot_t *out_protection)
{
	const minimm_page_remap_context_t initial_context = {
		.read_implies_exec = false,
	};
	const minimm_note_rights_t required_rights =
		MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_SHARE;
	const uint64_t note_size = note == NULL ? UINT64_C(0) : minimm_note_size(note);
	minimm_space_t *space = NULL;
	minimm_mmap_args_t args = { 0 };
	minimm_mapping_info_t remapped = { 0 };
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_status_t status = MINIMM_OK;

	if (out_protection == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_protection = MINIMM_PROT_NONE;
	if (mm == NULL || note == NULL || !minimm_note_belongs_to(note, mm) ||
	    (note_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    note_offset > note_size || MINIMM_PAGE_SIZE > note_size - note_offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((minimm_note_rights(note) & required_rights) != required_rights) {
		return MINIMM_ERROR_PERMISSION;
	}

	status = minimm_space_create(mm, &space);
	if (status != MINIMM_OK) {
		return status;
	}
	args.address_hint = MINIMM_ADDRESS_AUTO;
	args.length = note_size;
	args.note_offset = UINT64_C(0);
	args.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE;
	args.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EXEC;
	args.flags = MINIMM_MAP_SHARED;
	args.note = note;
	status = minimm_page_remap_checked_mmap(space, &initial_context, &args, &address);
	if (status == MINIMM_OK) {
		status = minimm_page_remap_file_pages(space, note, address, note_offset);
	}
	if (status == MINIMM_OK) {
		status = minimm_mapping_query(space, address, &remapped);
	}
	if (status == MINIMM_OK) {
		*out_protection = remapped.protection;
	}
	minimm_space_destroy(space);
	return status;
}
