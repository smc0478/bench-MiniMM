#include "minimm/minimm.h"

#include "internal.h"

#include <stdlib.h>

#ifndef MINIMM_VERSION_STRING
#define MINIMM_VERSION_STRING "0.0.0-unknown"
#endif

minimm_config_t minimm_config_default(void)
{
	minimm_config_t config = {
		.physical_memory_size = (size_t)64U * 1024U * 1024U,
		.page_size = (size_t)4096U,
		.tlb_entries = 64U,
	};

	return config;
}

minimm_status_t minimm_create(const minimm_config_t *config, minimm_t **out_mm)
{
	minimm_t *mm = NULL;

	if (out_mm == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_mm = NULL;

	if (config == NULL || config->physical_memory_size == 0U ||
	    config->page_size != MINIMM_PAGE_SIZE || config->tlb_entries == 0U ||
	    config->physical_memory_size % config->page_size != 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	mm = malloc(sizeof(*mm));
	if (mm == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	mm->physical_memory_size = config->physical_memory_size;
	mm->page_size = config->page_size;
	mm->page_count = config->physical_memory_size / config->page_size;
	mm->tlb_entries = config->tlb_entries;
	atomic_init(&mm->references, 1U);
	atomic_init(&mm->closing, false);

	{
		const minimm_status_t status =
			minimm_frame_store_create(mm->page_count, &mm->frame_store);

		if (status != MINIMM_OK) {
			free(mm);
			return status;
		}
	}

	*out_mm = mm;
	return MINIMM_OK;
}

void minimm_destroy(minimm_t *mm)
{
	if (mm == NULL) {
		return;
	}
	atomic_store_explicit(&mm->closing, true, memory_order_release);
	minimm_system_release(mm);
}

minimm_status_t minimm_system_reclaim(minimm_t *mm, size_t target_pages,
				      minimm_reclaim_result_t *out_result)
{
	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	out_result->scanned_count = 0U;
	out_result->reclaimed_count = 0U;
	if (mm == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_frame_store_reclaim(mm->frame_store, target_pages, out_result);
}

bool minimm_system_try_retain(minimm_t *mm)
{
	size_t references = 0U;

	if (mm == NULL || atomic_load_explicit(&mm->closing, memory_order_acquire)) {
		return false;
	}

	references = atomic_load_explicit(&mm->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&mm->references, &references,
							  references + 1U, memory_order_acq_rel,
							  memory_order_relaxed)) {
			if (atomic_load_explicit(&mm->closing, memory_order_acquire)) {
				minimm_system_release(mm);
				return false;
			}
			return true;
		}
	}
	return false;
}

void minimm_system_release(minimm_t *mm)
{
	if (mm == NULL) {
		return;
	}
	if (atomic_fetch_sub_explicit(&mm->references, 1U, memory_order_acq_rel) == 1U) {
		minimm_frame_store_destroy(mm->frame_store);
		free(mm);
	}
}

size_t minimm_physical_memory_size(const minimm_t *mm)
{
	return mm == NULL ? 0U : mm->physical_memory_size;
}

size_t minimm_page_size(const minimm_t *mm)
{
	return mm == NULL ? 0U : mm->page_size;
}

size_t minimm_page_count(const minimm_t *mm)
{
	return mm == NULL ? 0U : mm->page_count;
}

const char *minimm_version(void)
{
	return MINIMM_VERSION_STRING;
}

const char *minimm_status_string(minimm_status_t status)
{
	switch (status) {
	case MINIMM_OK:
		return "ok";
	case MINIMM_ERROR_INVALID_ARGUMENT:
		return "invalid argument";
	case MINIMM_ERROR_OUT_OF_MEMORY:
		return "out of memory";
	case MINIMM_ERROR_NO_SPACE:
		return "no space available";
	case MINIMM_ERROR_IO:
		return "I/O error";
	case MINIMM_ERROR_BUSY:
		return "resource busy";
	case MINIMM_ERROR_NOT_FOUND:
		return "not found";
	case MINIMM_ERROR_PERMISSION:
		return "permission denied";
	case MINIMM_ERROR_ADDRESS_IN_USE:
		return "address already in use";
	case MINIMM_ERROR_UNSUPPORTED:
		return "unsupported operation";
	default:
		return "unknown status";
	}
}
