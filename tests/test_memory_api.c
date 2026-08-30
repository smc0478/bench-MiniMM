#define _POSIX_C_SOURCE 200809L

#include "memory_api.h"
#include "space.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool test_lock_and_advice(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_system_stats_t system_stats = { 0 };
	uint8_t core[2] = { UINT8_C(0), UINT8_C(0) };
	unsigned char value = UINT8_C(0x5a);
	unsigned char read_back = UINT8_C(0xff);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.note_offset = 0U,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create address space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map pages")) {
		goto done;
	}

	if (!check(MINIMM_MINCORE_RESIDENT == UINT8_C(1),
		   "mincore bit zero is Linux-compatible residency") ||
	    !check(minimm_mlock(space, address, MINIMM_PAGE_SIZE * UINT64_C(2)) ==
			   MINIMM_ERROR_BUSY,
		   "multi-page mlock reports pinned-memory pressure") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE, core, 1U) == MINIMM_OK &&
			   (core[0] & MINIMM_MINCORE_PRESENT) == UINT8_C(0),
		   "failed mlock removes its newly faulted PTE") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   system_stats.frame_count == 0U && system_stats.resident_count == 0U,
		   "failed mlock releases newly faulted frames") ||
	    !check(minimm_mlock(space, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock one resident page") ||
	    !check(minimm_mlock(space, address, MINIMM_PAGE_SIZE * UINT64_C(2)) ==
			   MINIMM_ERROR_BUSY,
		   "failed mlock preserves a pre-existing lock") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE, core, 1U) == MINIMM_OK &&
			   (core[0] & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_RESIDENT |
				       MINIMM_MINCORE_LOCKED)) ==
				   (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_RESIDENT |
				    MINIMM_MINCORE_LOCKED),
		   "pre-existing PTE and pin survive rollback") ||
	    !check(minimm_madvise(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				  MINIMM_MADV_WILLNEED) == MINIMM_OK &&
			   minimm_mincore(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
					  &core[1], 1U) == MINIMM_OK &&
			   (core[1] & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_RESIDENT)) ==
				   UINT8_C(0),
		   "anonymous WILLNEED stays advisory without installing a PTE") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_ERROR_BUSY,
		   "locked page rejects explicit pageout") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_DONTNEED) ==
			   MINIMM_ERROR_BUSY,
		   "locked page rejects discard") ||
	    !check(minimm_munlock(space, address, MINIMM_PAGE_SIZE) == MINIMM_OK, "unlock page") ||
	    !check(minimm_mlock(space, address + UINT64_C(1), MINIMM_PAGE_SIZE - UINT64_C(1)) ==
				   MINIMM_OK &&
			   minimm_mincore(space, address, MINIMM_PAGE_SIZE, core, 1U) ==
				   MINIMM_OK &&
			   (core[0] & MINIMM_MINCORE_LOCKED) != UINT8_C(0),
		   "mlock rounds an unaligned range down") ||
	    !check(minimm_munlock(space, address + UINT64_C(1), MINIMM_PAGE_SIZE - UINT64_C(1)) ==
			   MINIMM_OK,
		   "munlock rounds an unaligned range down") ||
	    !check(minimm_write(space, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value) &&
			   minimm_mincore(space, address, MINIMM_PAGE_SIZE, core, 1U) ==
				   MINIMM_OK &&
			   (core[0] & MINIMM_MINCORE_DIRTY) != UINT8_C(0),
		   "dirty a page before explicit pageout") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "unlocked page can page out") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE, core, 1U) == MINIMM_OK &&
			   (core[0] & MINIMM_MINCORE_PRESENT) == UINT8_C(0) &&
			   (core[0] & MINIMM_MINCORE_RESIDENT) == UINT8_C(0) &&
			   (core[0] & MINIMM_MINCORE_DIRTY) == UINT8_C(0) &&
			   (core[0] & UINT8_C(1)) == UINT8_C(0),
		   "pageout clears residency, present diagnostics, and PTE dirty state") ||
	    !check(minimm_madvise(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				  MINIMM_MADV_WILLNEED) == MINIMM_OK,
		   "willneed accepts an anonymous advisory hint") ||
	    !check(minimm_mincore(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE, &core[1],
				  1U) == MINIMM_OK &&
			   (core[1] & MINIMM_MINCORE_PRESENT) == UINT8_C(0),
		   "willneed does not guarantee anonymous PTE population") ||
	    !check(minimm_write(space, address + MINIMM_PAGE_SIZE, &value, sizeof(value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "write page before dontneed") ||
	    !check(minimm_madvise(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				  MINIMM_MADV_DONTNEED) == MINIMM_OK,
		   "discard unlocked private page") ||
	    !check(minimm_mincore(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE, &core[1],
				  1U) == MINIMM_OK &&
			   (core[1] & MINIMM_MINCORE_PRESENT) == UINT8_C(0),
		   "dontneed removes the PTE") ||
	    !check(minimm_madvise(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				  MINIMM_MADV_WILLNEED) == MINIMM_OK,
		   "willneed accepts a discarded anonymous page") ||
	    !check(minimm_read(space, address + MINIMM_PAGE_SIZE, &read_back, sizeof(read_back),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(read_back) && read_back == UINT8_C(0),
		   "private anonymous dontneed refaults as zero")) {
		goto done;
	}

	if (!check(minimm_mincore(space, address + UINT64_C(1), MINIMM_PAGE_SIZE, core, 1U) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "range start must be page aligned") ||
	    !check(minimm_mincore(space, address, UINT64_C(0), NULL, 0U) == MINIMM_OK &&
			   minimm_msync(space, address, UINT64_C(0)) == MINIMM_OK &&
			   minimm_madvise(space, address, UINT64_C(0), MINIMM_MADV_NORMAL) ==
				   MINIMM_OK &&
			   minimm_mlock(space, address + UINT64_C(1), UINT64_C(0)) == MINIMM_OK &&
			   minimm_munlock(space, address + UINT64_C(1), UINT64_C(0)) == MINIMM_OK,
		   "Linux-style zero-length range operations are no-ops") ||
	    !check(minimm_munmap(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "create a VMA hole") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE * UINT64_C(2), core, 2U) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "range operations reject holes before processing")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool pte_metadata_matches(const minimm_page_info_t *left, const minimm_page_info_t *right)
{
	return left->page_address == right->page_address && left->protection == right->protection &&
	       left->dirty == right->dirty && left->accessed == right->accessed &&
	       left->cow == right->cow && left->shared == right->shared &&
	       left->locked == right->locked;
}

static bool test_mlock_restores_existing_pte(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t before = { 0 };
	minimm_page_info_t after = { 0 };
	minimm_page_info_t untouched = { 0 };
	minimm_system_stats_t system_stats = { 0 };
	minimm_space_stats_t stats = { 0 };
	minimm_pfn_t warmed_pfn = MINIMM_PFN_NONE;
	unsigned char value = UINT8_C(0);
	size_t completed = 0U;
	minimm_status_t internal_status = MINIMM_OK;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.note_offset = 0U,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create parent space") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map private pages") ||
	    !check(minimm_read(parent, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "fault the first page") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "make the first page COW") ||
	    !check(minimm_mprotect(parent, address, MINIMM_PAGE_SIZE * UINT64_C(2),
				   MINIMM_PROT_WRITE) == MINIMM_OK,
		   "make the COW mapping write-only")) {
		goto done;
	}

	(void)pthread_mutex_lock(&parent->lock);
	internal_status = minimm_page_table_update_flags(parent->page_table, address, 0U,
							 MINIMM_PTE_ACCESSED | MINIMM_PTE_DIRTY);
	minimm_tlb_invalidate_page(parent->tlb, address);
	(void)pthread_mutex_unlock(&parent->lock);
	if (!check(internal_status == MINIMM_OK, "prepare clean unaccessed COW state") ||
	    !check(minimm_madvise(parent, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "page out the existing COW frame") ||
	    !check(minimm_query_page(parent, address, &before) == MINIMM_OK && !before.present &&
			   !before.resident && before.pfn == MINIMM_PFN_NONE && before.cow &&
			   !before.dirty && !before.accessed && !before.locked &&
			   before.protection == MINIMM_PROT_NONE,
		   "capture the pre-mlock PTE state")) {
		goto done;
	}

	if (!check(minimm_mlock(parent, address, MINIMM_PAGE_SIZE * UINT64_C(2)) ==
			   MINIMM_ERROR_BUSY,
		   "later page makes mlock fail after a clean page-in") ||
	    !check(minimm_query_page(parent, address, &after) == MINIMM_OK &&
			   pte_metadata_matches(&before, &after) && after.present &&
			   after.resident && after.pfn != MINIMM_PFN_NONE &&
			   (warmed_pfn = after.pfn) != MINIMM_PFN_NONE,
		   "failed mlock restores metadata while retaining a warm translation") ||
	    !check(minimm_query_page(parent, address + MINIMM_PAGE_SIZE, &untouched) == MINIMM_OK &&
			   !untouched.present && untouched.pfn == MINIMM_PFN_NONE,
		   "failed mlock removes the later newly faulted PTE") ||
	    !check(minimm_space_get_stats(parent, &stats) == MINIMM_OK && stats.pte_count == 1U &&
			   stats.locked_count == 0U,
		   "failed mlock leaves no extra PTE or pin") ||
	    !check(minimm_system_get_stats(mm, &system_stats) == MINIMM_OK &&
			   system_stats.frame_count == 1U && system_stats.resident_count == 1U,
		   "failed mlock releases transient frames but may warm the cache") ||
	    !check(minimm_mlock(parent, address, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_query_page(parent, address, &after) == MINIMM_OK &&
			   after.pfn == warmed_pfn && after.present && after.resident &&
			   after.locked && after.cow && after.protection == MINIMM_PROT_NONE &&
			   !after.dirty && !after.accessed,
		   "write-only COW page is locked without a simulated access") ||
	    !check(minimm_munlock(parent, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "cleanly locked frame can be unlocked again")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

static bool test_note_msync(void)
{
	static const unsigned char update[] = "msync persisted bytes";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_note_t *note = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t alias_address = MINIMM_ADDRESS_AUTO;
	minimm_space_stats_t alias_stats = { 0 };
	FILE *file = NULL;
	int fd = -1;
	uint8_t core = UINT8_C(0);
	uint8_t alias_core = UINT8_C(0);
	unsigned char persisted[sizeof(update)] = { 0 };
	size_t completed = 0U;
	ssize_t read_length = -1;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(3),
		.note_offset = 0U,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_SHARED,
		.note = NULL,
	};
	bool passed = false;

	file = tmpfile();
	if (!check(file != NULL, "create note file")) {
		goto done;
	}
	fd = fileno(file);
	if (!check(fd >= 0, "get note descriptor") ||
	    !check(ftruncate(fd, (off_t)(MINIMM_PAGE_SIZE * UINT64_C(3))) == 0, "size note file") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create note system") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &note) == MINIMM_OK,
		   "open file-backed note") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create note address space")) {
		goto done;
	}

	args.note = note;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map shared note") ||
	    !check(minimm_mmap(space, &args, &alias_address) == MINIMM_OK,
		   "map an unfaulted shared-note alias") ||
	    !check(minimm_write(space, address, update, sizeof(update), &completed) == MINIMM_OK &&
			   completed == sizeof(update),
		   "dirty shared note mapping") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE, &core, 1U) == MINIMM_OK &&
			   (core & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_RESIDENT |
				    MINIMM_MINCORE_DIRTY | MINIMM_MINCORE_SHARED)) ==
				   (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_RESIDENT |
				    MINIMM_MINCORE_DIRTY | MINIMM_MINCORE_SHARED),
		   "mincore reports dirty resident shared page") ||
	    !check(minimm_mincore(space, alias_address, MINIMM_PAGE_SIZE, &alias_core, 1U) ==
				   MINIMM_OK &&
			   (alias_core & (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_DIRTY |
					  MINIMM_MINCORE_SHARED)) ==
				   (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_DIRTY |
				    MINIMM_MINCORE_SHARED) &&
			   (alias_core & MINIMM_MINCORE_PRESENT) == UINT8_C(0) &&
			   minimm_space_get_stats(space, &alias_stats) == MINIMM_OK &&
			   alias_stats.pte_count == 1U,
		   "mincore sees dirty resident page cache without materializing an alias PTE") ||
	    !check(minimm_msync(space, address, UINT64_C(0)) == MINIMM_OK &&
			   minimm_mincore(space, address, MINIMM_PAGE_SIZE, &core, 1U) ==
				   MINIMM_OK &&
			   (core & MINIMM_MINCORE_DIRTY) != UINT8_C(0),
		   "zero-length msync leaves dirty state unchanged") ||
	    !check(minimm_munmap(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "create an msync hole") ||
	    !check(minimm_msync(space, address, MINIMM_PAGE_SIZE * UINT64_C(3)) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "msync processes mapped pieces and reports a hole") ||
	    !check(minimm_mincore(space, address, MINIMM_PAGE_SIZE, &core, 1U) == MINIMM_OK &&
			   (core & MINIMM_MINCORE_DIRTY) == UINT8_C(0),
		   "msync clears local dirty accounting") ||
	    !check(minimm_mincore(space, alias_address, MINIMM_PAGE_SIZE, &alias_core, 1U) ==
				   MINIMM_OK &&
			   (alias_core & (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_SHARED)) ==
				   (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_SHARED) &&
			   (alias_core & (MINIMM_MINCORE_PRESENT | MINIMM_MINCORE_DIRTY)) ==
				   UINT8_C(0),
		   "msync clears backing dirty state without materializing an alias PTE")) {
		goto done;
	}

	read_length = pread(fd, persisted, sizeof(persisted), (off_t)0);
	if (!check(read_length == (ssize_t)sizeof(persisted) &&
			   memcmp(persisted, update, sizeof(update)) == 0,
		   "msync persists mapped bytes to the note file")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_note_release(note);
	minimm_destroy(mm);
	if (file != NULL) {
		(void)fclose(file);
	}
	return passed;
}

static bool test_shared_alias_dontneed_uses_local_lock_state(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t parent_page = { 0 };
	minimm_page_info_t child_page = { 0 };
	uint8_t child_core = UINT8_C(0);
	unsigned char value = UINT8_C(0x6d);
	unsigned char observed = UINT8_C(0);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create alias lock system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create alias lock parent") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map shared alias page") ||
	    !check(minimm_write(parent, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "fault shared alias page") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork shared alias") ||
	    !check(minimm_mlock(parent, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock only the parent PTE") ||
	    !check(minimm_madvise(child, address, MINIMM_PAGE_SIZE, MINIMM_MADV_DONTNEED) ==
			   MINIMM_OK,
		   "an unlocked alias may discard its PTE while the shared frame is pinned") ||
	    !check(minimm_query_page(child, address, &child_page) == MINIMM_OK &&
			   !child_page.present,
		   "alias discard removes only the child PTE") ||
	    !check(minimm_mincore(child, address, MINIMM_PAGE_SIZE, &child_core, 1U) == MINIMM_OK &&
			   (child_core & (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_DIRTY |
					  MINIMM_MINCORE_SHARED)) ==
				   (MINIMM_MINCORE_RESIDENT | MINIMM_MINCORE_DIRTY |
				    MINIMM_MINCORE_SHARED) &&
			   (child_core & MINIMM_MINCORE_PRESENT) == UINT8_C(0),
		   "discarded alias observes dirty backing without recreating a PTE") ||
	    !check(minimm_read(child, address, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == value &&
			   minimm_query_page(child, address, &child_page) == MINIMM_OK &&
			   child_page.present,
		   "discarded shared alias refaults from the preserved backing cache") ||
	    !check(minimm_query_page(parent, address, &parent_page) == MINIMM_OK &&
			   parent_page.present && parent_page.locked &&
			   minimm_read(parent, address, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == value,
		   "parent pin and shared bytes survive alias discard")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

typedef struct mlock_pageout_race_context {
	minimm_space_t *space;
	minimm_vaddr_t address;
	atomic_bool stop;
	atomic_bool failed;
} mlock_pageout_race_context_t;

static void *pageout_shared_alias(void *opaque)
{
	mlock_pageout_race_context_t *context = opaque;

	while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
		const minimm_status_t status = minimm_madvise(
			context->space, context->address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT);

		if (status != MINIMM_OK && status != MINIMM_ERROR_BUSY) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
	}
	return NULL;
}

static bool test_mlock_is_resident_when_pageout_races(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t page = { 0 };
	mlock_pageout_race_context_t context = { 0 };
	pthread_t pageout_thread = { 0 };
	unsigned char value = UINT8_C(0);
	size_t completed = 0U;
	size_t iteration = 0U;
	int thread_status = 0;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS,
	};
	bool passed = false;
	bool thread_started = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create mlock race system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create mlock race parent") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map mlock race page") ||
	    !check(minimm_read(parent, address, &value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "fault mlock race page") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK,
		   "create a shared pageout alias")) {
		goto done;
	}

	context.space = child;
	context.address = address;
	atomic_init(&context.stop, false);
	atomic_init(&context.failed, false);
	thread_status = pthread_create(&pageout_thread, NULL, pageout_shared_alias, &context);
	if (!check(thread_status == 0, "start shared alias pageout worker")) {
		goto done;
	}
	thread_started = true;

	for (iteration = 0U; iteration < 20000U; ++iteration) {
		if (minimm_mlock(parent, address, MINIMM_PAGE_SIZE) != MINIMM_OK ||
		    minimm_query_page(parent, address, &page) != MINIMM_OK || !page.locked ||
		    !page.resident ||
		    minimm_munlock(parent, address, MINIMM_PAGE_SIZE) != MINIMM_OK) {
			atomic_store_explicit(&context.failed, true, memory_order_release);
			break;
		}
	}

	atomic_store_explicit(&context.stop, true, memory_order_release);
	(void)pthread_join(pageout_thread, NULL);
	thread_started = false;
	if (!check(!atomic_load_explicit(&context.failed, memory_order_acquire),
		   "successful mlock stays resident while an alias requests pageout")) {
		goto done;
	}
	passed = true;

done:
	if (thread_started) {
		atomic_store_explicit(&context.stop, true, memory_order_release);
		(void)pthread_join(pageout_thread, NULL);
	}
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

static bool test_sparse_range_operations(void)
{
	const uint64_t sparse_length = UINT64_C(1) << 40U;
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_system_stats_t stats = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = sparse_length,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	bool passed = false;

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create sparse range system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create sparse range space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "map a one-TiB sparse range") ||
	    !check(minimm_madvise(space, address, sparse_length, MINIMM_MADV_NORMAL) == MINIMM_OK &&
			   minimm_madvise(space, address, sparse_length, MINIMM_MADV_WILLNEED) ==
				   MINIMM_OK &&
			   minimm_madvise(space, address, sparse_length, MINIMM_MADV_DONTNEED) ==
				   MINIMM_OK &&
			   minimm_madvise(space, address, sparse_length, MINIMM_MADV_PAGEOUT) ==
				   MINIMM_OK &&
			   minimm_munlock(space, address, sparse_length) == MINIMM_OK,
		   "empty sparse range operations scale with VMAs and PTEs") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK && stats.frame_count == 0U,
		   "sparse range hints do not materialize private anonymous pages") ||
	    !check(minimm_munmap(space, address + (sparse_length / UINT64_C(2)),
				 MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "punch a one-page hole in the sparse range") ||
	    !check(minimm_madvise(space, address, sparse_length, MINIMM_MADV_NORMAL) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "sparse validation finds a distant VMA hole") ||
	    !check(minimm_msync(space, address, sparse_length) == MINIMM_ERROR_NOT_FOUND,
		   "msync skips a huge hole-free span and reports the distant hole")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	return test_lock_and_advice() && test_mlock_restores_existing_pte() && test_note_msync() &&
			       test_shared_alias_dontneed_uses_local_lock_state() &&
			       test_mlock_is_resident_when_pageout_races() &&
			       test_sparse_range_operations() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
