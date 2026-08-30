#include "minimm/minimm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

int main(void)
{
	const unsigned char value = UINT8_C(0x5a);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_fault_trace_t trace = { 0 };
	minimm_fault_trace_t child_trace = { 0 };
	minimm_fault_info_t fault = { 0 };
	minimm_space_stats_t stats = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};
	uint64_t sequence_before_clear = UINT64_C(0);
	unsigned char observed = UINT8_C(0);
	unsigned char partial[4] = { 0 };
	size_t completed = 0U;
	size_t iteration = 0U;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create trace system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create traced space") ||
	    !check(minimm_space_get_fault_trace(NULL, &trace) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null trace space") ||
	    !check(minimm_space_get_fault_trace(space, NULL) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null trace output") ||
	    !check(minimm_space_clear_fault_trace(NULL) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject clearing a null space") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 0U &&
			   trace.overwritten_count == UINT64_C(0),
		   "a new space has an empty fault trace") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map traced page") ||
	    !check(minimm_write(space, address + UINT64_C(17), &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(value),
		   "automatic write resolves demand-zero fault") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 1U &&
			   trace.overwritten_count == UINT64_C(0),
		   "automatic write records one fault") ||
	    !check(trace.events[0].sequence == UINT64_C(1) &&
			   trace.events[0].address == address + UINT64_C(17) &&
			   trace.events[0].page_address == address &&
			   trace.events[0].access == MINIMM_ACCESS_WRITE &&
			   trace.events[0].origin == MINIMM_FAULT_ORIGIN_ACCESS &&
			   trace.events[0].reason == MINIMM_FAULT_NOT_PRESENT &&
			   trace.events[0].resolution == MINIMM_FAULT_ZERO_FILLED &&
			   trace.events[0].status == MINIMM_OK,
		   "demand-zero trace describes the automatic write") ||
	    !check(minimm_read(space, address + UINT64_C(17), &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == value,
		   "read through the warm translation") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 1U &&
			   trace.events[0].sequence == UINT64_C(1),
		   "warm read does not add a fault event") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "page out the traced page") ||
	    !check(minimm_read(space, address + UINT64_C(17), &observed, sizeof(observed),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed) && observed == value,
		   "automatic read pages the translation back in") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 2U &&
			   trace.overwritten_count == UINT64_C(0),
		   "automatic page-in appends one fault") ||
	    !check(trace.events[1].sequence == UINT64_C(2) &&
			   trace.events[1].address == address + UINT64_C(17) &&
			   trace.events[1].page_address == address &&
			   trace.events[1].access == MINIMM_ACCESS_READ &&
			   trace.events[1].origin == MINIMM_FAULT_ORIGIN_ACCESS &&
			   trace.events[1].reason == MINIMM_FAULT_NOT_PRESENT &&
			   trace.events[1].resolution == MINIMM_FAULT_PAGE_IN &&
			   trace.events[1].status == MINIMM_OK,
		   "page-in trace describes the automatic read") ||
	    !check(minimm_handle_page_fault(space, address + UINT64_C(19), MINIMM_ACCESS_READ,
					    &fault) == MINIMM_OK,
		   "explicit resident fault completes without another resolution") ||
	    !check(fault.sequence == UINT64_C(3) && fault.origin == MINIMM_FAULT_ORIGIN_EXPLICIT &&
			   fault.reason == MINIMM_FAULT_NONE &&
			   fault.resolution == MINIMM_FAULT_NO_ACTION && fault.status == MINIMM_OK,
		   "explicit success returns a classified no-action result") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 3U &&
			   trace.events[2].sequence == fault.sequence &&
			   trace.events[2].origin == fault.origin &&
			   trace.events[2].resolution == fault.resolution,
		   "explicit success is appended to the trace") ||
	    !check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE, MINIMM_PROT_READ) == MINIMM_OK,
		   "make the traced page read-only") ||
	    !check(minimm_handle_page_fault(space, address + UINT64_C(23), MINIMM_ACCESS_WRITE,
					    &fault) == MINIMM_ERROR_PERMISSION,
		   "explicit write fault is denied") ||
	    !check(fault.sequence == UINT64_C(4) && fault.origin == MINIMM_FAULT_ORIGIN_EXPLICIT &&
			   fault.reason == MINIMM_FAULT_PERMISSION &&
			   fault.resolution == MINIMM_FAULT_DENIED &&
			   fault.status == MINIMM_ERROR_PERMISSION,
		   "explicit denial returns classified fault information") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 4U &&
			   trace.events[3].sequence == fault.sequence &&
			   trace.events[3].address == fault.address &&
			   trace.events[3].access == fault.access &&
			   trace.events[3].origin == fault.origin &&
			   trace.events[3].reason == fault.reason &&
			   trace.events[3].resolution == fault.resolution &&
			   trace.events[3].status == fault.status,
		   "explicit denial is appended to the trace")) {
		goto failure;
	}

	if (!check(minimm_write(space, address + UINT64_C(29), &value, sizeof(value), &completed) ==
				   MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "automatic write records a VMA permission fault") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 5U &&
			   trace.overwritten_count == UINT64_C(0),
		   "automatic permission denial appends one fault") ||
	    !check(trace.events[4].sequence == UINT64_C(5) &&
			   trace.events[4].address == address + UINT64_C(29) &&
			   trace.events[4].page_address == address &&
			   trace.events[4].access == MINIMM_ACCESS_WRITE &&
			   trace.events[4].origin == MINIMM_FAULT_ORIGIN_ACCESS &&
			   trace.events[4].reason == MINIMM_FAULT_PERMISSION &&
			   trace.events[4].resolution == MINIMM_FAULT_DENIED &&
			   trace.events[4].status == MINIMM_ERROR_PERMISSION,
		   "automatic permission trace is classified as access-origin") ||
	    !check(minimm_read(space, address + MINIMM_PAGE_SIZE - UINT64_C(2), partial,
			       sizeof(partial), &completed) == MINIMM_ERROR_NOT_FOUND &&
			   completed == 2U,
		   "cross-page read reports its prefix before an unmapped fault") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 6U &&
			   trace.overwritten_count == UINT64_C(0),
		   "automatic unmapped denial appends one fault") ||
	    !check(trace.events[5].sequence == UINT64_C(6) &&
			   trace.events[5].address == address + MINIMM_PAGE_SIZE &&
			   trace.events[5].page_address == address + MINIMM_PAGE_SIZE &&
			   trace.events[5].access == MINIMM_ACCESS_READ &&
			   trace.events[5].origin == MINIMM_FAULT_ORIGIN_ACCESS &&
			   trace.events[5].reason == MINIMM_FAULT_UNMAPPED &&
			   trace.events[5].resolution == MINIMM_FAULT_DENIED &&
			   trace.events[5].status == MINIMM_ERROR_NOT_FOUND,
		   "automatic unmapped trace is classified as access-origin") ||
	    !check(minimm_handle_page_fault(space, MINIMM_USER_ADDRESS_LIMIT, MINIMM_ACCESS_READ,
					    &fault) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject an invalid explicit fault address") ||
	    !check(minimm_handle_page_fault(space, address,
					    MINIMM_ACCESS_READ | MINIMM_ACCESS_WRITE,
					    &fault) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject an invalid explicit access mask") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 6U &&
			   trace.overwritten_count == UINT64_C(0) &&
			   trace.events[5].sequence == UINT64_C(6),
		   "invalid fault inputs do not append trace events") ||
	    !check(minimm_space_get_stats(space, &stats) == MINIMM_OK &&
			   stats.fault_sequence == UINT64_C(6),
		   "invalid fault inputs do not advance the sequence")) {
		goto failure;
	}

	sequence_before_clear = trace.events[5].sequence;
	if (!check(minimm_space_clear_fault_trace(space) == MINIMM_OK,
		   "clear the retained fault history") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 0U &&
			   trace.overwritten_count == UINT64_C(0),
		   "clear resets retained and overwritten counts") ||
	    !check(minimm_space_get_stats(space, &stats) == MINIMM_OK &&
			   stats.fault_sequence == sequence_before_clear,
		   "clear preserves the space fault sequence")) {
		goto failure;
	}

	for (iteration = 0U; iteration < (size_t)MINIMM_FAULT_TRACE_CAPACITY + 3U; ++iteration) {
		const minimm_vaddr_t fault_address =
			address + (minimm_vaddr_t)iteration + UINT64_C(1);

		if (!check(minimm_handle_page_fault(space, fault_address, MINIMM_ACCESS_WRITE,
						    &fault) == MINIMM_ERROR_PERMISSION,
			   "generate a permission fault for ring wrap")) {
			goto failure;
		}
	}

	if (!check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK &&
			   trace.count == (size_t)MINIMM_FAULT_TRACE_CAPACITY &&
			   trace.overwritten_count == UINT64_C(3),
		   "full trace reports three overwritten events")) {
		goto failure;
	}
	for (iteration = 0U; iteration < trace.count; ++iteration) {
		const uint64_t expected_sequence =
			sequence_before_clear + UINT64_C(4) + (uint64_t)iteration;
		const minimm_vaddr_t expected_address =
			address + UINT64_C(4) + (minimm_vaddr_t)iteration;
		const minimm_fault_info_t *event = &trace.events[iteration];

		if (!check(event->sequence == expected_sequence &&
				   event->address == expected_address &&
				   event->page_address == address &&
				   event->access == MINIMM_ACCESS_WRITE &&
				   event->origin == MINIMM_FAULT_ORIGIN_EXPLICIT &&
				   event->reason == MINIMM_FAULT_PERMISSION &&
				   event->resolution == MINIMM_FAULT_DENIED &&
				   event->status == MINIMM_ERROR_PERMISSION,
			   "wrapped trace is ordered from oldest to newest")) {
			goto failure;
		}
	}
	if (!check(minimm_space_get_stats(space, &stats) == MINIMM_OK &&
			   stats.fault_sequence == sequence_before_clear +
							   (uint64_t)MINIMM_FAULT_TRACE_CAPACITY +
							   UINT64_C(3),
		   "ring wrap does not change fault sequence accounting") ||
	    !check(minimm_space_fork(space, &child) == MINIMM_OK,
		   "fork a space after filling its trace") ||
	    !check(minimm_space_get_fault_trace(child, &child_trace) == MINIMM_OK &&
			   child_trace.count == 0U && child_trace.overwritten_count == UINT64_C(0),
		   "fork child starts with an empty fault trace") ||
	    !check(minimm_space_get_stats(child, &stats) == MINIMM_OK &&
			   stats.fault_sequence == UINT64_C(0),
		   "fork child starts with an independent fault sequence") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK &&
			   trace.count == (size_t)MINIMM_FAULT_TRACE_CAPACITY &&
			   trace.overwritten_count == UINT64_C(3),
		   "fork leaves the parent fault trace unchanged")) {
		goto failure;
	}

	minimm_space_destroy(child);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_SUCCESS;

failure:
	minimm_space_destroy(child);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_FAILURE;
}
