#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

int main(void)
{
	static const char initial[] = "shared note bytes";
	static const char shared_update[] = "shared update";
	static const char private_update[] = "private copy";
	static const char direct_private_update[] = "direct COW";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	minimm_space_t *first_space = NULL;
	minimm_space_t *second_space = NULL;
	minimm_vaddr_t first_shared = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t second_shared = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t private_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t direct_private = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t first_page = { 0 };
	minimm_page_info_t second_page = { 0 };
	minimm_page_info_t private_page = { 0 };
	minimm_fault_info_t fault = { 0 };
	char buffer[64] = { 0 };
	size_t completed = 0U;
	const minimm_prot_t rights = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT;
	minimm_mmap_args_t shared_args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.note_offset = 0U,
		.protection = rights,
		.maximum_protection = rights,
		.flags = MINIMM_MAP_SHARED,
		.note = NULL,
	};
	minimm_mmap_args_t private_args = shared_args;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE * UINT64_C(2), MINIMM_NOTE_RIGHT_ALL,
				      &note) == MINIMM_OK,
		   "create mapped note") ||
	    !check(minimm_note_pwrite(note, 0U, initial, sizeof(initial), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(initial),
		   "seed note page cache") ||
	    !check(minimm_space_create(mm, &first_space) == MINIMM_OK &&
			   minimm_space_create(mm, &second_space) == MINIMM_OK,
		   "create two address spaces")) {
		goto failure;
	}

	shared_args.note = note;
	private_args.note = note;
	private_args.length = MINIMM_PAGE_SIZE;
	private_args.flags = MINIMM_MAP_PRIVATE;
	if (!check(minimm_mmap(first_space, &shared_args, &first_shared) == MINIMM_OK &&
			   minimm_mmap(second_space, &shared_args, &second_shared) == MINIMM_OK,
		   "map one note shared in two spaces") ||
	    !check(minimm_read(first_space, first_shared, buffer, sizeof(initial), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, initial, sizeof(initial)) == 0,
		   "first shared mapping reads seeded bytes") ||
	    !check(minimm_read(second_space, second_shared, buffer, sizeof(initial), &completed) ==
				   MINIMM_OK &&
			   minimm_query_page(first_space, first_shared, &first_page) == MINIMM_OK &&
			   minimm_query_page(second_space, second_shared, &second_page) ==
				   MINIMM_OK &&
			   first_page.pfn == second_page.pfn && first_page.shared &&
			   second_page.shared,
		   "shared aliases use one synthetic PFN") ||
	    !check(minimm_edit(first_space, first_shared, shared_update, sizeof(shared_update),
			       &completed) == MINIMM_OK,
		   "edit shared mapping") ||
	    !check(minimm_read(second_space, second_shared, buffer, sizeof(shared_update),
			       &completed) == MINIMM_OK &&
			   memcmp(buffer, shared_update, sizeof(shared_update)) == 0,
		   "shared write is immediately visible in another space") ||
	    !check(minimm_note_pread(note, 0U, buffer, sizeof(shared_update), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, shared_update, sizeof(shared_update)) == 0,
		   "note API observes mapped shared write")) {
		goto failure;
	}

	if (!check(minimm_mmap(second_space, &private_args, &private_address) == MINIMM_OK,
		   "map private note view") ||
	    !check(minimm_read(second_space, private_address, buffer, sizeof(shared_update),
			       &completed) == MINIMM_OK &&
			   minimm_query_page(second_space, private_address, &private_page) ==
				   MINIMM_OK &&
			   private_page.pfn == first_page.pfn && private_page.cow,
		   "private view initially shares the note frame as COW") ||
	    !check(minimm_write(second_space, private_address, private_update,
				sizeof(private_update), &completed) == MINIMM_OK &&
			   minimm_query_page(second_space, private_address, &private_page) ==
				   MINIMM_OK &&
			   private_page.pfn != first_page.pfn && !private_page.cow &&
			   !private_page.shared,
		   "private write copies the frame") ||
	    !check(minimm_read(first_space, first_shared, buffer, sizeof(shared_update),
			       &completed) == MINIMM_OK &&
			   memcmp(buffer, shared_update, sizeof(shared_update)) == 0,
		   "private write leaves shared mapping unchanged")) {
		goto failure;
	}

	private_args.note_offset = MINIMM_PAGE_SIZE;
	if (!check(minimm_mmap(first_space, &private_args, &direct_private) == MINIMM_OK,
		   "map a second private note page") ||
	    !check(minimm_handle_page_fault(first_space, direct_private, MINIMM_ACCESS_WRITE,
					    &fault) == MINIMM_OK &&
			   fault.reason == MINIMM_FAULT_COW &&
			   fault.resolution == MINIMM_FAULT_COW_COPIED,
		   "first write fault copies a note frame before exposure") ||
	    !check(minimm_write(first_space, direct_private, direct_private_update,
				sizeof(direct_private_update), &completed) == MINIMM_OK,
		   "write direct private copy") ||
	    !check(minimm_note_pread(note, MINIMM_PAGE_SIZE, buffer, sizeof(direct_private_update),
				     &completed) == MINIMM_OK,
		   "read private source page through note") ||
	    !check(memcmp(buffer, direct_private_update, sizeof(direct_private_update)) != 0,
		   "direct private write does not modify note backing") ||
	    !check(minimm_note_resize(note, MINIMM_PAGE_SIZE * UINT64_C(3)) == MINIMM_OK,
		   "mapped note growth is safe") ||
	    !check(minimm_note_resize(note, MINIMM_PAGE_SIZE) == MINIMM_ERROR_BUSY,
		   "mapped note shrink requires SIGBUS invalidation support")) {
		goto failure;
	}

	minimm_space_destroy(second_space);
	minimm_space_destroy(first_space);
	if (!check(minimm_note_resize(note, MINIMM_PAGE_SIZE * UINT64_C(3)) == MINIMM_OK,
		   "note becomes resizable after the final mapping disappears") ||
	    !check(minimm_note_flush(note) == MINIMM_OK, "flush mapped note")) {
		minimm_note_release(note);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}
	minimm_note_release(note);
	minimm_destroy(mm);
	return EXIT_SUCCESS;

failure:
	minimm_space_destroy(second_space);
	minimm_space_destroy(first_space);
	minimm_note_release(note);
	minimm_destroy(mm);
	return EXIT_FAILURE;
}
