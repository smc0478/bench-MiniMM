#include "minimm/minimm.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
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

static minimm_mmap_args_t private_anonymous_args(uint64_t length)
{
	const minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = length,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
		.note = NULL,
	};

	return args;
}

static bool write_byte(minimm_space_t *space, minimm_vaddr_t address, unsigned char value)
{
	size_t completed = 0U;

	return minimm_write(space, address, &value, sizeof(value), &completed) == MINIMM_OK &&
	       completed == sizeof(value);
}

static bool read_byte(minimm_space_t *space, minimm_vaddr_t address, unsigned char *out_value)
{
	size_t completed = 0U;

	return minimm_read(space, address, out_value, sizeof(*out_value), &completed) ==
		       MINIMM_OK &&
	       completed == sizeof(*out_value);
}

static bool test_cold_hint_and_target_zero(void)
{
	const unsigned char value = UINT8_C(0x5a);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(2));
	minimm_page_info_t page = { 0 };
	minimm_fault_trace_t trace = { 0 };
	minimm_reclaim_result_t result = { 0 };
	minimm_system_stats_t before = { 0 };
	minimm_system_stats_t after = { 0 };
	minimm_space_stats_t space_stats = { 0 };
	unsigned char observed = UINT8_C(0);
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_system_reclaim(NULL, 1U, &result) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject reclaim on a null system") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create cold-hint system") ||
	    !check(minimm_system_reclaim(mm, 0U, NULL) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reclaim requires a result output") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create cold-hint space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "map cold-hint anonymous pages") ||
	    !check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "read counters before cold hint") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_COLD) == MINIMM_OK,
		   "cold advice accepts an untouched anonymous page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && !page.present &&
			   !page.resident && !page.cold && page.pfn == MINIMM_PFN_NONE,
		   "cold advice does not materialize an untouched anonymous page") ||
	    !check(minimm_space_get_stats(space, &space_stats) == MINIMM_OK &&
			   space_stats.pte_count == 0U,
		   "cold advice does not install a PTE") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.page_in_count == before.page_in_count &&
			   after.page_out_count == before.page_out_count &&
			   after.reclaim_scan_count == before.reclaim_scan_count &&
			   after.reclaim_count == before.reclaim_count &&
			   after.refault_count == before.refault_count,
		   "cold advice does not change paging or reclaim counters") ||
	    !check(write_byte(space, address, value), "materialize the cold-hint page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && page.accessed && !page.cold,
		   "ordinary write creates a warm accessed page") ||
	    !check(minimm_space_clear_fault_trace(space) == MINIMM_OK,
		   "clear trace before resident cold transition") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_COLD) == MINIMM_OK,
		   "mark a resident page cold") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && !page.accessed && page.cold,
		   "cold advice clears accessed and records cold priority") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 0U,
		   "cold advice is not a page fault") ||
	    !check(read_byte(space, address, &observed) && observed == value,
		   "read a resident cold page") ||
	    !check(minimm_query_page(space, address, &page) == MINIMM_OK && page.present &&
			   page.resident && page.accessed && !page.cold,
		   "real access refills the invalidated TLB and warms the page") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 0U,
		   "resident warm-up is a refill rather than a fault") ||
	    !check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "read counters before zero-target reclaim")) {
		goto done;
	}

	result.scanned_count = SIZE_MAX;
	result.reclaimed_count = SIZE_MAX;
	if (!check(minimm_system_reclaim(mm, 0U, &result) == MINIMM_OK &&
			   result.scanned_count == 0U && result.reclaimed_count == 0U,
		   "zero-target reclaim is a successful no-op") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.page_in_count == before.page_in_count &&
			   after.page_out_count == before.page_out_count &&
			   after.reclaim_scan_count == before.reclaim_scan_count &&
			   after.reclaim_count == before.reclaim_count &&
			   after.refault_count == before.refault_count,
		   "zero-target reclaim leaves all counters unchanged")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_deterministic_reclaim_and_refault(void)
{
	const unsigned char values[3] = { UINT8_C(0x41), UINT8_C(0x42), UINT8_C(0x43) };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(3));
	minimm_page_info_t pages[3] = { 0 };
	minimm_pfn_t original_b_pfn = MINIMM_PFN_NONE;
	minimm_reclaim_result_t result = { 0 };
	minimm_system_stats_t stats = { 0 };
	minimm_system_stats_t before_hot_read = { 0 };
	minimm_fault_trace_t trace = { 0 };
	unsigned char observed = UINT8_C(0);
	size_t index = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 3U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create three-slot reclaim system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK,
		   "create deterministic reclaim space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "map deterministic reclaim pages")) {
		goto done;
	}
	for (index = 0U; index < 3U; ++index) {
		if (!check(write_byte(space, address + (minimm_vaddr_t)index * MINIMM_PAGE_SIZE,
				      values[index]),
			   "populate deterministic reclaim page") ||
		    !check(minimm_query_page(space,
					     address + (minimm_vaddr_t)index * MINIMM_PAGE_SIZE,
					     &pages[index]) == MINIMM_OK &&
				   pages[index].present && pages[index].resident &&
				   pages[index].pfn != MINIMM_PFN_NONE,
			   "capture resident reclaim page identity")) {
			goto done;
		}
	}
	original_b_pfn = pages[1].pfn;
	if (!check(minimm_mlock(space, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock page A before reclaim") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_COLD) ==
				   MINIMM_OK &&
			   minimm_madvise(space, address + MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
					  MINIMM_MADV_COLD) == MINIMM_OK,
		   "mark locked A and evictable B cold") ||
	    !check(minimm_query_page(space, address, &pages[0]) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &pages[1]) ==
				   MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2),
					     &pages[2]) == MINIMM_OK,
		   "query hot, cold, and locked pages") ||
	    !check(pages[0].resident && pages[0].locked && pages[0].cold && !pages[0].accessed &&
			   pages[1].resident && !pages[1].locked && pages[1].cold &&
			   !pages[1].accessed && pages[2].resident && !pages[2].cold &&
			   pages[2].accessed,
		   "cold and lock state is directly observable") ||
	    !check(minimm_space_clear_fault_trace(space) == MINIMM_OK,
		   "clear trace before policy reclaim") ||
	    !check(minimm_system_reclaim(mm, 1U, &result) == MINIMM_OK &&
			   result.scanned_count == 3U && result.reclaimed_count == 1U,
		   "first reclaim scans all pages and reclaims one") ||
	    !check(minimm_query_page(space, address, &pages[0]) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &pages[1]) ==
				   MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2),
					     &pages[2]) == MINIMM_OK,
		   "query first reclaim result") ||
	    !check(pages[0].resident && pages[0].locked && !pages[1].present &&
			   !pages[1].resident && pages[1].pfn == MINIMM_PFN_NONE &&
			   pages[2].resident,
		   "cold unlocked B is the exact first victim") ||
	    !check(minimm_system_reclaim(mm, 2U, &result) == MINIMM_OK &&
			   result.scanned_count == 3U && result.reclaimed_count == 1U,
		   "second reclaim reports a best-effort locked-page shortfall") ||
	    !check(minimm_query_page(space, address, &pages[0]) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2),
					     &pages[2]) == MINIMM_OK &&
			   pages[0].resident && pages[0].locked && !pages[2].present &&
			   !pages[2].resident,
		   "second reclaim skips locked A and evicts only C") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.page_in_count == UINT64_C(3) &&
			   stats.page_out_count == UINT64_C(2) &&
			   stats.reclaim_scan_count == UINT64_C(6) &&
			   stats.reclaim_count == UINT64_C(2) && stats.refault_count == UINT64_C(0),
		   "explicit reclaim counters include scans, victims, and shortfall")) {
		goto done;
	}

	if (!check(read_byte(space, address + MINIMM_PAGE_SIZE, &observed) && observed == values[1],
		   "refault B and restore its byte") ||
	    !check(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &pages[1]) == MINIMM_OK &&
			   pages[1].present && pages[1].resident && pages[1].accessed &&
			   !pages[1].cold && pages[1].pfn == original_b_pfn,
		   "refault restores B's stable PFN and warms it") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 1U &&
			   trace.events[0].reason == MINIMM_FAULT_NOT_PRESENT &&
			   trace.events[0].resolution == MINIMM_FAULT_PAGE_IN &&
			   trace.events[0].status == MINIMM_OK,
		   "refault is recorded as one automatic page-in") ||
	    !check(minimm_system_get_stats(mm, &before_hot_read) == MINIMM_OK &&
			   before_hot_read.page_in_count == UINT64_C(4) &&
			   before_hot_read.refault_count == UINT64_C(1),
		   "first policy refault is counted once") ||
	    !check(read_byte(space, address + MINIMM_PAGE_SIZE, &observed) && observed == values[1],
		   "read already-hot B again") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.page_in_count == before_hot_read.page_in_count &&
			   stats.refault_count == before_hot_read.refault_count,
		   "hot read does not count another refault") ||
	    !check(minimm_space_get_fault_trace(space, &trace) == MINIMM_OK && trace.count == 1U,
		   "hot read does not append a fault") ||
	    !check(minimm_munlock(space, address, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "unlock cold page A") ||
	    !check(minimm_query_page(space, address, &pages[0]) == MINIMM_OK && pages[0].resident &&
			   !pages[0].locked && pages[0].cold && !pages[0].accessed,
		   "unlock preserves cold priority without warming A") ||
	    !check(minimm_system_reclaim(mm, 1U, &result) == MINIMM_OK &&
			   result.scanned_count == 2U && result.reclaimed_count == 1U,
		   "post-unlock reclaim scans A and B") ||
	    !check(minimm_query_page(space, address, &pages[0]) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &pages[1]) ==
				   MINIMM_OK &&
			   !pages[0].present && !pages[0].resident &&
			   pages[0].pfn == MINIMM_PFN_NONE && pages[1].resident,
		   "unlocked cold A becomes the exact next victim") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK && stats.frame_count == 3U &&
			   stats.resident_count == 1U && stats.resident_limit == 3U &&
			   stats.page_in_count == UINT64_C(4) &&
			   stats.page_out_count == UINT64_C(3) &&
			   stats.reclaim_scan_count == UINT64_C(8) &&
			   stats.reclaim_count == UINT64_C(3) && stats.refault_count == UINT64_C(1),
		   "final deterministic reclaim accounting is exact")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_direct_pageout_classification(void)
{
	const unsigned char value = UINT8_C(0x7d);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE);
	minimm_page_info_t before_page = { 0 };
	minimm_page_info_t after_page = { 0 };
	minimm_system_stats_t before = { 0 };
	minimm_system_stats_t after = { 0 };
	unsigned char observed = UINT8_C(0);
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create direct-pageout system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create direct-pageout space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map direct-pageout page") ||
	    !check(write_byte(space, address, value), "write direct-pageout byte") ||
	    !check(minimm_query_page(space, address, &before_page) == MINIMM_OK &&
			   before_page.pfn != MINIMM_PFN_NONE,
		   "capture direct-pageout frame identity") ||
	    !check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "read counters before direct pageout") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_PAGEOUT) ==
			   MINIMM_OK,
		   "directly page out one frame") ||
	    !check(read_byte(space, address, &observed) && observed == value,
		   "page in directly paged-out data") ||
	    !check(minimm_query_page(space, address, &after_page) == MINIMM_OK &&
			   after_page.pfn == before_page.pfn && !after_page.cold,
		   "direct page-in preserves identity and returns warm") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.page_out_count == before.page_out_count + UINT64_C(1) &&
			   after.page_in_count == before.page_in_count + UINT64_C(1) &&
			   after.reclaim_scan_count == before.reclaim_scan_count &&
			   after.reclaim_count == before.reclaim_count &&
			   after.refault_count == before.refault_count,
		   "direct PAGEOUT is excluded from reclaim and refault counters")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_automatic_pressure_accounting(void)
{
	const unsigned char values[3] = { UINT8_C(0x11), UINT8_C(0x22), UINT8_C(0x33) };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(3));
	minimm_page_info_t first = { 0 };
	minimm_page_info_t second = { 0 };
	minimm_page_info_t third = { 0 };
	minimm_system_stats_t stats = { 0 };
	minimm_pfn_t first_pfn = MINIMM_PFN_NONE;
	unsigned char observed = UINT8_C(0);
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create pressure reclaim system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create pressure reclaim space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "map pressure reclaim pages") ||
	    !check(write_byte(space, address, values[0]) &&
			   write_byte(space, address + MINIMM_PAGE_SIZE, values[1]),
		   "fill both resident slots") ||
	    !check(minimm_query_page(space, address, &first) == MINIMM_OK &&
			   ((first_pfn = first.pfn) != MINIMM_PFN_NONE),
		   "capture first pressure frame identity") ||
	    !check(minimm_madvise(space, address, MINIMM_PAGE_SIZE, MINIMM_MADV_COLD) == MINIMM_OK,
		   "prioritize first page for pressure reclaim") ||
	    !check(write_byte(space, address + MINIMM_PAGE_SIZE * UINT64_C(2), values[2]),
		   "fault a third page under two-slot pressure") ||
	    !check(minimm_query_page(space, address, &first) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &second) ==
				   MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2),
					     &third) == MINIMM_OK &&
			   !first.resident && first.pfn == MINIMM_PFN_NONE && second.resident &&
			   third.resident,
		   "automatic pressure chooses the explicit cold victim") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.page_in_count == UINT64_C(3) &&
			   stats.page_out_count == UINT64_C(1) &&
			   stats.reclaim_scan_count == UINT64_C(2) &&
			   stats.reclaim_count == UINT64_C(1) && stats.refault_count == UINT64_C(0),
		   "automatic pressure contributes one policy scan and victim") ||
	    !check(read_byte(space, address, &observed) && observed == values[0],
		   "refault the automatically reclaimed page") ||
	    !check(minimm_query_page(space, address, &first) == MINIMM_OK &&
			   minimm_query_page(space, address + MINIMM_PAGE_SIZE, &second) ==
				   MINIMM_OK &&
			   first.resident && first.pfn == first_pfn && !second.resident,
		   "automatic refault restores A and evicts oldest hot B") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.page_in_count == UINT64_C(4) &&
			   stats.page_out_count == UINT64_C(2) &&
			   stats.reclaim_scan_count == UINT64_C(4) &&
			   stats.reclaim_count == UINT64_C(2) && stats.refault_count == UINT64_C(1),
		   "automatic refault and replacement accounting is exact") ||
	    !check(read_byte(space, address, &observed) && observed == values[0] &&
			   minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.refault_count == UINT64_C(1),
		   "hot access after automatic refault is not recounted")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_shared_alias_cold_state(void)
{
	const unsigned char value = UINT8_C(0x6b);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE);
	minimm_page_info_t parent_page = { 0 };
	minimm_page_info_t child_page = { 0 };
	unsigned char observed = UINT8_C(0);
	bool passed = false;

	args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS;
	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create shared-cold system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create shared-cold parent") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK,
		   "map shared anonymous page") ||
	    !check(write_byte(parent, address, value), "materialize shared frame") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork shared frame alias") ||
	    !check(read_byte(child, address, &observed) && observed == value,
		   "mark child alias accessed") ||
	    !check(minimm_query_page(parent, address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, address, &child_page) == MINIMM_OK &&
			   parent_page.accessed && child_page.accessed && !parent_page.cold &&
			   !child_page.cold && parent_page.pfn == child_page.pfn,
		   "both aliases begin accessed and warm") ||
	    !check(minimm_madvise(parent, address, MINIMM_PAGE_SIZE, MINIMM_MADV_COLD) == MINIMM_OK,
		   "mark a shared frame cold through the parent alias") ||
	    !check(minimm_query_page(parent, address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, address, &child_page) == MINIMM_OK &&
			   parent_page.cold && child_page.cold && !parent_page.accessed &&
			   child_page.accessed,
		   "cold is frame-global while accessed clearing is alias-local") ||
	    !check(read_byte(child, address, &observed) && observed == value,
		   "warm the shared frame through the untouched alias") ||
	    !check(minimm_query_page(parent, address, &parent_page) == MINIMM_OK &&
			   minimm_query_page(child, address, &child_page) == MINIMM_OK &&
			   !parent_page.cold && !child_page.cold && !parent_page.accessed &&
			   child_page.accessed,
		   "alias access warms the frame without changing another PTE's accessed bit")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

typedef struct reclaim_race_context {
	minimm_t *mm;
	minimm_space_t *space;
	minimm_vaddr_t address;
	unsigned char values[2];
	atomic_bool start;
	atomic_bool failed;
} reclaim_race_context_t;

static void *run_reclaim_race_reads(void *opaque)
{
	reclaim_race_context_t *context = opaque;
	size_t iteration = 0U;

	while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
		(void)sched_yield();
	}
	for (iteration = 0U; iteration < 2000U; ++iteration) {
		const size_t page_index = iteration % 2U;
		unsigned char observed = UINT8_C(0);

		if (!read_byte(context->space,
			       context->address + (minimm_vaddr_t)page_index * MINIMM_PAGE_SIZE,
			       &observed) ||
		    observed != context->values[page_index]) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
		(void)sched_yield();
	}
	return NULL;
}

static void *run_reclaim_race_scans(void *opaque)
{
	reclaim_race_context_t *context = opaque;
	size_t iteration = 0U;

	while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
		(void)sched_yield();
	}
	for (iteration = 0U; iteration < 2000U; ++iteration) {
		minimm_reclaim_result_t result = { 0 };

		if (minimm_system_reclaim(context->mm, 1U, &result) != MINIMM_OK ||
		    result.reclaimed_count > 1U || result.scanned_count < result.reclaimed_count) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
		(void)sched_yield();
	}
	return NULL;
}

static bool test_bounded_reclaim_read_race(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(2));
	reclaim_race_context_t context = { 0 };
	minimm_system_stats_t stats = { 0 };
	pthread_t reader;
	pthread_t reclaimer;
	unsigned char observed = UINT8_C(0);
	bool reader_started = false;
	bool reclaimer_started = false;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create reclaim-race system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create reclaim-race space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map reclaim-race pages") ||
	    !check(write_byte(space, address, UINT8_C(0xa5)) &&
			   write_byte(space, address + MINIMM_PAGE_SIZE, UINT8_C(0x3c)),
		   "initialize reclaim-race bytes")) {
		goto done;
	}

	context.mm = mm;
	context.space = space;
	context.address = address;
	context.values[0] = UINT8_C(0xa5);
	context.values[1] = UINT8_C(0x3c);
	atomic_init(&context.start, false);
	atomic_init(&context.failed, false);
	if (!check(pthread_create(&reader, NULL, run_reclaim_race_reads, &context) == 0,
		   "start reclaim-race reader")) {
		goto done;
	}
	reader_started = true;
	if (!check(pthread_create(&reclaimer, NULL, run_reclaim_race_scans, &context) == 0,
		   "start reclaim-race scanner")) {
		atomic_store_explicit(&context.start, true, memory_order_release);
		goto done;
	}
	reclaimer_started = true;
	atomic_store_explicit(&context.start, true, memory_order_release);
	(void)pthread_join(reader, NULL);
	reader_started = false;
	(void)pthread_join(reclaimer, NULL);
	reclaimer_started = false;

	if (!check(!atomic_load_explicit(&context.failed, memory_order_acquire),
		   "concurrent reclaim and reads complete without errors") ||
	    !check(read_byte(space, address, &observed) && observed == context.values[0],
		   "reclaim race preserves page A") ||
	    !check(read_byte(space, address + MINIMM_PAGE_SIZE, &observed) &&
			   observed == context.values[1],
		   "reclaim race preserves page B") ||
	    !check(minimm_system_get_stats(mm, &stats) == MINIMM_OK &&
			   stats.resident_count <= stats.resident_limit &&
			   stats.reclaim_count != UINT64_C(0) && stats.refault_count != UINT64_C(0),
		   "reclaim race records bounded victims and refaults")) {
		goto done;
	}

	passed = true;

done:
	if (reader_started || reclaimer_started) {
		atomic_store_explicit(&context.start, true, memory_order_release);
	}
	if (reader_started) {
		(void)pthread_join(reader, NULL);
	}
	if (reclaimer_started) {
		(void)pthread_join(reclaimer, NULL);
	}
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	return test_cold_hint_and_target_zero() && test_deterministic_reclaim_and_refault() &&
			       test_direct_pageout_classification() &&
			       test_automatic_pressure_accounting() &&
			       test_shared_alias_cold_state() && test_bounded_reclaim_read_race() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
