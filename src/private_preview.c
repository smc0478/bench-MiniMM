#include "private_preview.h"

#include "fault.h"
#include "frame.h"
#include "note.h"
#include "page_table.h"
#include "space.h"
#include "tlb.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MINIMM_PRIVATE_PREVIEW_MAX_ATTEMPTS 3U

enum minimm_private_preview_follow_flag {
	MINIMM_PRIVATE_PREVIEW_FOLLOW_WRITE = 1U << 0,
	MINIMM_PRIVATE_PREVIEW_FOLLOW_FORCE = 1U << 1,
};

typedef struct minimm_private_preview_run {
	minimm_space_t *space;
	minimm_vaddr_t address;
	minimm_pfn_t backing_pfn;
	size_t within_page;
	pthread_mutex_t gate_lock;
	pthread_cond_t gate_changed;
	bool first_copy_complete;
	bool discard_complete;
	bool writer_complete;
	bool abort;
	minimm_status_t discard_status;
	atomic_bool discarder_exited;
} minimm_private_preview_run_t;

typedef struct minimm_private_preview_page {
	minimm_frame_t *frame;
	bool writable;
} minimm_private_preview_page_t;

typedef struct minimm_private_preview_pin {
	minimm_frame_t *frame;
	bool held;
} minimm_private_preview_pin_t;

static minimm_status_t minimm_private_preview_lock_space(minimm_private_preview_run_t *run)
{
	(void)pthread_mutex_lock(&run->space->lock);
	if (atomic_load_explicit(&run->space->closing, memory_order_acquire)) {
		(void)pthread_mutex_unlock(&run->space->lock);
		return MINIMM_ERROR_BUSY;
	}
	return MINIMM_OK;
}

static minimm_status_t
minimm_private_preview_load_page_locked(minimm_private_preview_run_t *run,
					minimm_private_preview_page_t *out_page)
{
	minimm_pte_t *pte = minimm_page_table_lookup(run->space->page_table, run->address);
	bool paged_in = false;
	minimm_status_t status = MINIMM_OK;

	out_page->frame = NULL;
	out_page->writable = false;
	if (pte == NULL) {
		status = minimm_populate_page_locked(run->space, run->address);
		if (status != MINIMM_OK) {
			return status;
		}
		pte = minimm_page_table_lookup(run->space->page_table, run->address);
	}
	if (pte == NULL || (pte->flags & MINIMM_PTE_PRESENT) == 0U) {
		return MINIMM_ERROR_IO;
	}
	status = minimm_frame_ensure_resident(pte->frame, &paged_in);
	if (status != MINIMM_OK) {
		return status;
	}
	(void)paged_in;
	out_page->frame = pte->frame;
	out_page->writable = (pte->protection & MINIMM_PROT_WRITE) != 0U;
	return MINIMM_OK;
}

static minimm_status_t minimm_private_preview_copy_page_locked(minimm_private_preview_run_t *run)
{
	minimm_pte_t *pte = minimm_page_table_lookup(run->space->page_table, run->address);
	minimm_pte_t *updated = NULL;
	minimm_frame_t *copy = NULL;
	minimm_pfn_t original_pfn = MINIMM_PFN_NONE;
	minimm_prot_t protection = MINIMM_PROT_NONE;
	minimm_status_t status = MINIMM_OK;
	bool valid_copy = false;

	if (pte == NULL || (pte->flags & MINIMM_PTE_COW) == 0U) {
		return MINIMM_ERROR_IO;
	}
	original_pfn = minimm_frame_id(pte->frame);
	protection = pte->protection;
	if (original_pfn != run->backing_pfn) {
		return MINIMM_ERROR_IO;
	}

	status = minimm_frame_copy(pte->frame, &copy);
	if (status == MINIMM_OK) {
		status = minimm_page_table_replace_frame(run->space->page_table, run->address, copy,
							 protection, MINIMM_PTE_DIRTY,
							 MINIMM_PTE_COW | MINIMM_PTE_SHARED);
	}
	if (status == MINIMM_OK) {
		minimm_tlb_invalidate_page(run->space->tlb, run->address);
		updated = minimm_page_table_lookup(run->space->page_table, run->address);
		valid_copy = updated != NULL && updated->frame == copy &&
			     minimm_frame_id(updated->frame) != original_pfn &&
			     (updated->flags & (MINIMM_PTE_PRESENT | MINIMM_PTE_DIRTY)) ==
				     (MINIMM_PTE_PRESENT | MINIMM_PTE_DIRTY) &&
			     (updated->flags & (MINIMM_PTE_COW | MINIMM_PTE_SHARED)) == 0U;
	}
	minimm_frame_release(copy);
	if (status != MINIMM_OK) {
		return status;
	}
	return valid_copy ? MINIMM_OK : MINIMM_ERROR_IO;
}

static bool minimm_private_preview_can_follow(const minimm_private_preview_page_t *page,
					      unsigned int flags)
{
	return (flags & MINIMM_PRIVATE_PREVIEW_FOLLOW_WRITE) == 0U || page->writable;
}

static minimm_status_t
minimm_private_preview_pin_backing_locked(minimm_private_preview_run_t *run,
					  const minimm_private_preview_page_t *page,
					  minimm_private_preview_pin_t *out_pin)
{
	minimm_pte_t *pte = minimm_page_table_lookup(run->space->page_table, run->address);

	out_pin->frame = NULL;
	out_pin->held = false;
	if (page->frame == NULL || pte == NULL || pte->frame != page->frame ||
	    (pte->flags & MINIMM_PTE_PRESENT) == 0U ||
	    minimm_frame_id(pte->frame) != run->backing_pfn) {
		return MINIMM_ERROR_IO;
	}
	if (!minimm_frame_try_pin_resident(pte->frame)) {
		return MINIMM_ERROR_BUSY;
	}
	minimm_frame_retain(pte->frame);
	out_pin->frame = pte->frame;
	out_pin->held = true;
	return MINIMM_OK;
}

static minimm_status_t minimm_private_preview_write_pin(minimm_private_preview_run_t *run,
							minimm_private_preview_pin_t *pin,
							const void *data, size_t length)
{
	minimm_status_t status = MINIMM_ERROR_INVALID_ARGUMENT;

	if (pin->frame != NULL && pin->held) {
		status = minimm_frame_write(pin->frame, run->within_page, data, length);
		minimm_frame_unpin_resident(pin->frame);
		minimm_frame_release(pin->frame);
		pin->frame = NULL;
		pin->held = false;
	}
	return status;
}

static minimm_status_t minimm_private_preview_wait_for_discard(minimm_private_preview_run_t *run)
{
	minimm_status_t status = MINIMM_OK;
	int wait_status = 0;

	(void)pthread_mutex_lock(&run->gate_lock);
	run->first_copy_complete = true;
	(void)pthread_cond_broadcast(&run->gate_changed);
	while (!run->discard_complete && !run->abort) {
		wait_status = pthread_cond_wait(&run->gate_changed, &run->gate_lock);
		if (wait_status != 0) {
			run->abort = true;
			(void)pthread_cond_broadcast(&run->gate_changed);
			break;
		}
	}
	if (wait_status != 0) {
		status = MINIMM_ERROR_IO;
	} else if (!run->discard_complete) {
		status = MINIMM_ERROR_BUSY;
	} else {
		status = run->discard_status;
	}
	(void)pthread_mutex_unlock(&run->gate_lock);
	return status;
}

static minimm_status_t minimm_private_preview_force_write(minimm_private_preview_run_t *run,
							  const void *data, size_t length,
							  size_t *out_completed)
{
	unsigned int flags = MINIMM_PRIVATE_PREVIEW_FOLLOW_WRITE |
			     MINIMM_PRIVATE_PREVIEW_FOLLOW_FORCE;
	unsigned int attempt = 0U;

	*out_completed = 0U;
	for (attempt = 0U; attempt < MINIMM_PRIVATE_PREVIEW_MAX_ATTEMPTS; ++attempt) {
		minimm_private_preview_page_t page = { 0 };
		minimm_private_preview_pin_t pin = { 0 };
		minimm_status_t status = minimm_private_preview_lock_space(run);

		if (status != MINIMM_OK) {
			return status;
		}
		status = minimm_private_preview_load_page_locked(run, &page);
		if (status == MINIMM_OK && minimm_private_preview_can_follow(&page, flags)) {
			status = minimm_private_preview_pin_backing_locked(run, &page, &pin);
			(void)pthread_mutex_unlock(&run->space->lock);
			if (status != MINIMM_OK) {
				return status;
			}
			status = minimm_private_preview_write_pin(run, &pin, data, length);
			if (status == MINIMM_OK) {
				*out_completed = length;
			}
			return status;
		}
		if (status == MINIMM_OK && (flags & MINIMM_PRIVATE_PREVIEW_FOLLOW_FORCE) != 0U) {
			status = minimm_private_preview_copy_page_locked(run);
			if (status == MINIMM_OK) {
				flags &= ~(unsigned int)MINIMM_PRIVATE_PREVIEW_FOLLOW_WRITE;
			}
		} else if (status == MINIMM_OK) {
			status = MINIMM_ERROR_PERMISSION;
		}
		(void)pthread_mutex_unlock(&run->space->lock);
		if (status != MINIMM_OK) {
			return status;
		}
		if (attempt == 0U) {
			status = minimm_private_preview_wait_for_discard(run);
			if (status != MINIMM_OK) {
				return status;
			}
		}
	}
	return MINIMM_ERROR_BUSY;
}

static void *minimm_private_preview_discard_main(void *context)
{
	minimm_private_preview_run_t *run = context;
	minimm_status_t status = MINIMM_OK;
	minimm_page_info_t page = { 0 };
	bool should_discard = false;
	int wait_status = 0;

	(void)pthread_mutex_lock(&run->gate_lock);
	while (!run->first_copy_complete && !run->writer_complete && !run->abort) {
		wait_status = pthread_cond_wait(&run->gate_changed, &run->gate_lock);
		if (wait_status != 0) {
			run->abort = true;
			(void)pthread_cond_broadcast(&run->gate_changed);
			break;
		}
	}
	should_discard = wait_status == 0 && run->first_copy_complete && !run->abort;
	(void)pthread_mutex_unlock(&run->gate_lock);

	if (wait_status != 0) {
		status = MINIMM_ERROR_IO;
	} else if (should_discard) {
		status = minimm_madvise(run->space, run->address, MINIMM_PAGE_SIZE,
					MINIMM_MADV_DONTNEED);
		if (status == MINIMM_OK) {
			status = minimm_query_page(run->space, run->address, &page);
		}
		if (status == MINIMM_OK && (page.present || page.pfn != MINIMM_PFN_NONE)) {
			status = MINIMM_ERROR_IO;
		}
	} else {
		status = MINIMM_ERROR_BUSY;
	}

	(void)pthread_mutex_lock(&run->gate_lock);
	run->discard_status = status;
	run->discard_complete = true;
	if (status != MINIMM_OK) {
		run->abort = true;
	}
	(void)pthread_cond_broadcast(&run->gate_changed);
	(void)pthread_mutex_unlock(&run->gate_lock);
	atomic_store_explicit(&run->discarder_exited, true, memory_order_release);
	return NULL;
}

static void minimm_private_preview_finish_writer(minimm_private_preview_run_t *run,
						 minimm_status_t status)
{
	(void)pthread_mutex_lock(&run->gate_lock);
	run->writer_complete = true;
	if (status != MINIMM_OK) {
		run->abort = true;
	}
	(void)pthread_cond_broadcast(&run->gate_changed);
	(void)pthread_mutex_unlock(&run->gate_lock);
}

static minimm_status_t minimm_private_preview_prepare(minimm_private_preview_run_t *run,
						      minimm_t *mm, minimm_note_t *note,
						      uint64_t page_offset, const void *data,
						      size_t length)
{
	unsigned char observed[MINIMM_PAGE_SIZE];
	minimm_mmap_args_t args = { 0 };
	minimm_page_info_t before = { 0 };
	minimm_page_info_t after = { 0 };
	size_t completed = 0U;
	minimm_status_t status = minimm_space_create(mm, &run->space);

	if (status != MINIMM_OK) {
		return status;
	}
	args.address_hint = MINIMM_ADDRESS_AUTO;
	args.length = MINIMM_PAGE_SIZE;
	args.note_offset = page_offset;
	args.protection = MINIMM_PROT_READ;
	args.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE;
	args.flags = MINIMM_MAP_PRIVATE;
	args.note = note;
	status = minimm_mmap(run->space, &args, &run->address);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_read(run->space, run->address + (minimm_vaddr_t)run->within_page, observed,
			     length, &completed);
	if (status != MINIMM_OK) {
		return status;
	}
	if (completed != length) {
		return MINIMM_ERROR_IO;
	}
	status = minimm_query_page(run->space, run->address, &before);
	if (status != MINIMM_OK) {
		return status;
	}
	if (!before.present || !before.resident || !before.cow || before.pfn == MINIMM_PFN_NONE ||
	    (before.protection & MINIMM_PROT_WRITE) != 0U) {
		return MINIMM_ERROR_IO;
	}
	run->backing_pfn = before.pfn;

	completed = 0U;
	status = minimm_write(run->space, run->address + (minimm_vaddr_t)run->within_page, data,
			      length, &completed);
	if (status != MINIMM_ERROR_PERMISSION || completed != 0U) {
		return status == MINIMM_OK ? MINIMM_ERROR_IO : status;
	}
	status = minimm_query_page(run->space, run->address, &after);
	if (status != MINIMM_OK) {
		return status;
	}
	if (!after.present || !after.resident || !after.cow || after.pfn != run->backing_pfn ||
	    (after.protection & MINIMM_PROT_WRITE) != 0U) {
		return MINIMM_ERROR_IO;
	}
	return MINIMM_OK;
}

minimm_status_t minimm_private_preview_apply(minimm_t *mm, minimm_note_t *note, uint64_t offset,
					     const void *data, size_t length, size_t *out_completed)
{
	const uint64_t page_mask = MINIMM_PAGE_SIZE - UINT64_C(1);
	const uint64_t note_size = note == NULL ? UINT64_C(0) : minimm_note_size(note);
	const uint64_t page_offset = offset & ~page_mask;
	const size_t within_page = (size_t)(offset & page_mask);
	unsigned char stable_data[MINIMM_PAGE_SIZE];
	minimm_private_preview_run_t run = { 0 };
	pthread_t discarder;
	minimm_status_t status = MINIMM_OK;
	size_t completed = 0U;
	bool gate_lock_initialized = false;
	bool gate_changed_initialized = false;
	bool discarder_created = false;
	int join_status = 0;

	if (out_completed != NULL) {
		*out_completed = 0U;
	}
	if (mm == NULL || note == NULL || data == NULL || length == 0U ||
	    length > (size_t)MINIMM_PAGE_SIZE || !minimm_note_belongs_to(note, mm) ||
	    (minimm_note_rights(note) & MINIMM_NOTE_RIGHT_READ) == 0U || offset > note_size ||
	    (uint64_t)length > note_size - offset ||
	    length > (size_t)MINIMM_PAGE_SIZE - within_page || page_offset > note_size ||
	    MINIMM_PAGE_SIZE > note_size - page_offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memcpy(stable_data, data, length);
	run.address = MINIMM_ADDRESS_AUTO;
	run.backing_pfn = MINIMM_PFN_NONE;
	run.within_page = within_page;
	run.discard_status = MINIMM_ERROR_BUSY;
	atomic_init(&run.discarder_exited, false);

	if (pthread_mutex_init(&run.gate_lock, NULL) != 0) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	gate_lock_initialized = true;
	if (pthread_cond_init(&run.gate_changed, NULL) != 0) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto cleanup;
	}
	gate_changed_initialized = true;
	status = minimm_private_preview_prepare(&run, mm, note, page_offset, stable_data, length);
	if (status != MINIMM_OK) {
		goto cleanup;
	}

	if (pthread_create(&discarder, NULL, minimm_private_preview_discard_main, &run) != 0) {
		status = MINIMM_ERROR_OUT_OF_MEMORY;
		goto cleanup;
	}
	discarder_created = true;
	status = minimm_private_preview_force_write(&run, stable_data, length, &completed);
	minimm_private_preview_finish_writer(&run, status);
	join_status = pthread_join(discarder, NULL);
	if (join_status != 0) {
		while (!atomic_load_explicit(&run.discarder_exited, memory_order_acquire)) {
			(void)sched_yield();
		}
		status = MINIMM_ERROR_IO;
	}
	discarder_created = false;
	if (status == MINIMM_OK && completed != length) {
		status = MINIMM_ERROR_IO;
	}
	if (status == MINIMM_OK && out_completed != NULL) {
		*out_completed = completed;
	}

cleanup:
	if (discarder_created) {
		minimm_private_preview_finish_writer(&run, status);
		(void)pthread_join(discarder, NULL);
	}
	if (run.space != NULL) {
		minimm_space_destroy(run.space);
	}
	if (gate_changed_initialized) {
		(void)pthread_cond_destroy(&run.gate_changed);
	}
	if (gate_lock_initialized) {
		(void)pthread_mutex_destroy(&run.gate_lock);
	}
	return status;
}
