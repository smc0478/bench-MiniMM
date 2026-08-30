#include <minimm/minimm.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *minimm_tour_bool(bool value)
{
	return value ? "yes" : "no";
}

static const char *minimm_tour_fault_reason(minimm_fault_reason_t reason)
{
	switch (reason) {
	case MINIMM_FAULT_NONE:
		return "none";
	case MINIMM_FAULT_UNMAPPED:
		return "unmapped";
	case MINIMM_FAULT_NOT_PRESENT:
		return "not-present";
	case MINIMM_FAULT_PERMISSION:
		return "permission";
	case MINIMM_FAULT_COW:
		return "cow";
	case MINIMM_FAULT_BACKING_IO:
		return "backing-io";
	case MINIMM_FAULT_NO_FRAME:
		return "no-frame";
	}
	return "unknown";
}

static const char *minimm_tour_fault_resolution(minimm_fault_resolution_t resolution)
{
	switch (resolution) {
	case MINIMM_FAULT_UNRESOLVED:
		return "unresolved";
	case MINIMM_FAULT_NO_ACTION:
		return "no-action";
	case MINIMM_FAULT_ZERO_FILLED:
		return "zero-filled";
	case MINIMM_FAULT_PAGE_IN:
		return "page-in";
	case MINIMM_FAULT_COW_COPIED:
		return "cow-copied";
	case MINIMM_FAULT_DENIED:
		return "denied";
	}
	return "unknown";
}

static const char *minimm_tour_fault_origin(minimm_fault_origin_t origin)
{
	switch (origin) {
	case MINIMM_FAULT_ORIGIN_NONE:
		return "none";
	case MINIMM_FAULT_ORIGIN_EXPLICIT:
		return "explicit";
	case MINIMM_FAULT_ORIGIN_ACCESS:
		return "access";
	}
	return "unknown";
}

static bool minimm_tour_check(bool condition, const char *scenario, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "minimm-mm-tour: %s: check failed: %s\n", scenario, message);
	}
	return condition;
}

static bool minimm_tour_check_status(minimm_status_t actual, minimm_status_t expected,
				     const char *scenario, const char *operation)
{
	if (actual != expected) {
		(void)fprintf(stderr, "minimm-mm-tour: %s: %s: expected %s, received %s\n",
			      scenario, operation, minimm_status_string(expected),
			      minimm_status_string(actual));
		return false;
	}
	return true;
}

static void minimm_tour_print_page(const char *step, const char *page_name,
				   const minimm_page_info_t *page, const char *pfn_relation)
{
	(void)printf("  step=%s page=%s present=%s resident=%s pfn=%s cow=%s dirty=%s "
		     "accessed=%s locked=%s cold=%s\n",
		     step, page_name, minimm_tour_bool(page->present),
		     minimm_tour_bool(page->resident), pfn_relation, minimm_tour_bool(page->cow),
		     minimm_tour_bool(page->dirty), minimm_tour_bool(page->accessed),
		     minimm_tour_bool(page->locked), minimm_tour_bool(page->cold));
}

static void minimm_tour_print_trace(const char *step, const minimm_fault_trace_t *trace)
{
	if (trace->count == 0U) {
		(void)printf("  trace=%s events=0 overwritten=%" PRIu64 " latest=none\n", step,
			     trace->overwritten_count);
		return;
	}

	const minimm_fault_info_t *event = &trace->events[trace->count - 1U];

	(void)printf("  trace=%s events=%zu overwritten=%" PRIu64
		     " latest=%s/%s origin=%s status=%s\n",
		     step, trace->count, trace->overwritten_count,
		     minimm_tour_fault_reason(event->reason),
		     minimm_tour_fault_resolution(event->resolution),
		     minimm_tour_fault_origin(event->origin), minimm_status_string(event->status));
}

static void minimm_tour_print_stats(const char *step, const minimm_system_stats_t *stats)
{
	(void)printf("  stats=%s frames=%zu resident=%zu/%zu page-in=%" PRIu64 " page-out=%" PRIu64
		     " reclaim-scan=%" PRIu64 " reclaim=%" PRIu64 " refault=%" PRIu64 "\n",
		     step, stats->frame_count, stats->resident_count, stats->resident_limit,
		     stats->page_in_count, stats->page_out_count, stats->reclaim_scan_count,
		     stats->reclaim_count, stats->refault_count);
}

static void minimm_tour_print_reclaim(const char *step, const minimm_reclaim_result_t *result)
{
	(void)printf("  reclaim=%s scanned=%zu reclaimed=%zu\n", step, result->scanned_count,
		     result->reclaimed_count);
}

static bool minimm_tour_print_snapshot(const char *step, const minimm_space_snapshot_t *snapshot,
				       const char *scenario)
{
	minimm_space_stats_t stats = { 0 };
	size_t index = 0U;

	if (!minimm_tour_check_status(minimm_space_snapshot_get_stats(snapshot, &stats), MINIMM_OK,
				      scenario, "read captured space statistics")) {
		return false;
	}
	(void)printf("  snapshot=%s vmas=%zu ptes=%zu vma-gen=%" PRIu64 " pt-gen=%" PRIu64
		     " faults=%" PRIu64 "\n",
		     step, minimm_space_snapshot_mapping_count(snapshot),
		     minimm_space_snapshot_page_count(snapshot),
		     minimm_space_snapshot_vma_generation(snapshot),
		     minimm_space_snapshot_page_table_generation(snapshot), stats.fault_sequence);

	for (index = 0U; index < minimm_space_snapshot_mapping_count(snapshot); ++index) {
		minimm_mapping_info_t mapping = { 0 };

		if (!minimm_tour_check_status(minimm_space_snapshot_get_mapping(snapshot, index,
										&mapping),
					      MINIMM_OK, scenario, "read captured VMA")) {
			return false;
		}
		(void)printf("    vma[%zu]=0x%" PRIx64 "-0x%" PRIx64 " cookie=%" PRIu64
			     " prot=0x%" PRIx32 " flags=0x%" PRIx32 "\n",
			     index, mapping.start, mapping.end, mapping.mapping_cookie,
			     mapping.protection, mapping.flags);
	}
	for (index = 0U; index < minimm_space_snapshot_page_count(snapshot); ++index) {
		minimm_space_snapshot_page_t page = { 0 };

		if (!minimm_tour_check_status(minimm_space_snapshot_get_page(snapshot, index,
									     &page),
					      MINIMM_OK, scenario, "read captured PTE")) {
			return false;
		}
		(void)printf("    pte[%zu]=0x%" PRIx64 " map=%" PRIu64 " frame=%" PRIu64
			     " mapcount=%zu"
			     " present=%s resident=%s cow=%s dirty=%s accessed=%s cold=%s\n",
			     index, page.page.page_address, page.mapping_cookie, page.frame_cookie,
			     page.frame_mapping_count, minimm_tour_bool(page.page.present),
			     minimm_tour_bool(page.page.resident), minimm_tour_bool(page.page.cow),
			     minimm_tour_bool(page.page.dirty),
			     minimm_tour_bool(page.page.accessed),
			     minimm_tour_bool(page.page.cold));
	}
	return true;
}

static bool minimm_tour_trace_matches(const minimm_fault_trace_t *trace, size_t expected_count,
				      minimm_access_t access, minimm_fault_origin_t origin,
				      minimm_fault_reason_t reason,
				      minimm_fault_resolution_t resolution, minimm_status_t status,
				      const char *scenario, const char *message)
{
	const minimm_fault_info_t *event = NULL;

	if (!minimm_tour_check(trace->count == expected_count, scenario, message) ||
	    !minimm_tour_check(trace->overwritten_count == UINT64_C(0), scenario,
			       "the short tour must not overflow the fault trace")) {
		return false;
	}
	event = &trace->events[trace->count - 1U];
	return minimm_tour_check(event->access == access && event->origin == origin &&
					 event->reason == reason &&
					 event->resolution == resolution && event->status == status,
				 scenario, message);
}

static minimm_mmap_args_t minimm_tour_private_anonymous_args(uint64_t length)
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

static bool minimm_tour_fault(void)
{
	static const char scenario[] = "fault";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = minimm_tour_private_anonymous_args(MINIMM_PAGE_SIZE);
	minimm_page_info_t initial = { 0 };
	minimm_page_info_t demand_zero = { 0 };
	minimm_page_info_t paged_out = { 0 };
	minimm_page_info_t paged_in = { 0 };
	minimm_fault_trace_t trace = { 0 };
	minimm_system_stats_t stats = { 0 };
	minimm_pfn_t original_pfn = MINIMM_PFN_NONE;
	unsigned char observed = UINT8_C(0xff);
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	(void)printf("scenario=fault demand-zero -> page-out -> page-in\n");
	if (!minimm_tour_check_status(minimm_create(&config, &mm), MINIMM_OK, scenario,
				      "create one-frame system") ||
	    !minimm_tour_check_status(minimm_space_create(mm, &space), MINIMM_OK, scenario,
				      "create address space") ||
	    !minimm_tour_check_status(minimm_mmap(space, &args, &address), MINIMM_OK, scenario,
				      "map one private anonymous page") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &initial), MINIMM_OK,
				      scenario, "query untouched page") ||
	    !minimm_tour_check(!initial.present && !initial.resident &&
				       initial.pfn == MINIMM_PFN_NONE,
			       scenario, "an untouched mapping must have no resident PTE") ||
	    !minimm_tour_check_status(minimm_space_clear_fault_trace(space), MINIMM_OK, scenario,
				      "clear fault trace")) {
		goto done;
	}
	minimm_tour_print_page("mapped", "anonymous", &initial, "none");

	if (!minimm_tour_check_status(minimm_read(space, address, &observed, sizeof(observed),
						  &completed),
				      MINIMM_OK, scenario, "read untouched page") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == UINT8_C(0), scenario,
			       "demand-zero read must return one zero byte") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &demand_zero), MINIMM_OK,
				      scenario, "query demand-zero page") ||
	    !minimm_tour_check(demand_zero.present && demand_zero.resident &&
				       demand_zero.pfn != MINIMM_PFN_NONE && demand_zero.accessed,
			       scenario,
			       "demand-zero fault must install an accessed resident PTE") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read demand-zero fault trace") ||
	    !minimm_tour_trace_matches(&trace, 1U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
				       MINIMM_FAULT_NOT_PRESENT, MINIMM_FAULT_ZERO_FILLED,
				       MINIMM_OK, scenario,
				       "the first read must record one zero-fill fault")) {
		goto done;
	}
	original_pfn = demand_zero.pfn;
	minimm_tour_print_page("read", "anonymous", &demand_zero, "allocated");
	minimm_tour_print_trace("demand-zero", &trace);

	if (!minimm_tour_check_status(minimm_madvise(space, address, MINIMM_PAGE_SIZE,
						     MINIMM_MADV_PAGEOUT),
				      MINIMM_OK, scenario, "page out resident page") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &paged_out), MINIMM_OK,
				      scenario, "query paged-out page") ||
	    !minimm_tour_check(
		    !paged_out.present && !paged_out.resident && paged_out.pfn == MINIMM_PFN_NONE,
		    scenario, "page-out must hide a nonresident frame from translation")) {
		goto done;
	}
	minimm_tour_print_page("page-out", "anonymous", &paged_out, "none");

	observed = UINT8_C(0xff);
	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(space, address, &observed, sizeof(observed),
						  &completed),
				      MINIMM_OK, scenario, "read paged-out page") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == UINT8_C(0), scenario,
			       "page-in must restore the original byte") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &paged_in), MINIMM_OK,
				      scenario, "query paged-in page") ||
	    !minimm_tour_check(paged_in.present && paged_in.resident &&
				       paged_in.pfn == original_pfn,
			       scenario, "page-in must restore the same logical frame") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read page-in fault trace") ||
	    !minimm_tour_trace_matches(&trace, 2U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
				       MINIMM_FAULT_NOT_PRESENT, MINIMM_FAULT_PAGE_IN, MINIMM_OK,
				       scenario, "the second read must record one page-in fault") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &stats), MINIMM_OK, scenario,
				      "read final system statistics") ||
	    !minimm_tour_check(
		    stats.frame_count == 1U && stats.resident_count == 1U &&
			    stats.resident_limit == 1U && stats.page_in_count == UINT64_C(2) &&
			    stats.page_out_count == UINT64_C(1),
		    scenario, "fault tour accounting must match two page-ins and one page-out")) {
		goto done;
	}
	minimm_tour_print_page("read-again", "anonymous", &paged_in, "same-as-before");
	minimm_tour_print_trace("page-in", &trace);
	minimm_tour_print_stats("final", &stats);
	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	(void)printf("result=%s scenario=%s\n", passed ? "PASS" : "FAIL", scenario);
	return passed;
}

static bool minimm_tour_cow(void)
{
	static const char scenario[] = "cow";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = minimm_tour_private_anonymous_args(MINIMM_PAGE_SIZE);
	minimm_page_info_t before_fork = { 0 };
	minimm_page_info_t parent_shared = { 0 };
	minimm_page_info_t child_shared = { 0 };
	minimm_page_info_t parent_split = { 0 };
	minimm_page_info_t child_split = { 0 };
	minimm_fault_trace_t trace = { 0 };
	minimm_system_stats_t stats = { 0 };
	const unsigned char parent_value = (unsigned char)'P';
	const unsigned char child_value = (unsigned char)'C';
	unsigned char parent_observed = UINT8_C(0);
	unsigned char child_observed = UINT8_C(0);
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	(void)printf("scenario=cow fork shared frame -> child copy-on-write split\n");
	if (!minimm_tour_check_status(minimm_create(&config, &mm), MINIMM_OK, scenario,
				      "create two-frame system") ||
	    !minimm_tour_check_status(minimm_space_create(mm, &parent), MINIMM_OK, scenario,
				      "create parent space") ||
	    !minimm_tour_check_status(minimm_mmap(parent, &args, &address), MINIMM_OK, scenario,
				      "map parent private page") ||
	    !minimm_tour_check_status(minimm_write(parent, address, &parent_value,
						   sizeof(parent_value), &completed),
				      MINIMM_OK, scenario, "write parent byte") ||
	    !minimm_tour_check(completed == sizeof(parent_value), scenario,
			       "parent write must complete") ||
	    !minimm_tour_check_status(minimm_query_page(parent, address, &before_fork), MINIMM_OK,
				      scenario, "query parent before fork") ||
	    !minimm_tour_check(before_fork.present && before_fork.resident &&
				       before_fork.pfn != MINIMM_PFN_NONE && !before_fork.cow,
			       scenario, "parent must own one writable frame before fork") ||
	    !minimm_tour_check_status(minimm_space_fork(parent, &child), MINIMM_OK, scenario,
				      "fork parent") ||
	    !minimm_tour_check_status(minimm_query_page(parent, address, &parent_shared), MINIMM_OK,
				      scenario, "query forked parent") ||
	    !minimm_tour_check_status(minimm_query_page(child, address, &child_shared), MINIMM_OK,
				      scenario, "query forked child") ||
	    !minimm_tour_check(parent_shared.present && child_shared.present &&
				       parent_shared.resident && child_shared.resident &&
				       parent_shared.pfn == child_shared.pfn && parent_shared.cow &&
				       child_shared.cow,
			       scenario, "fork must share one frame behind two COW PTEs") ||
	    !minimm_tour_check_status(minimm_space_clear_fault_trace(child), MINIMM_OK, scenario,
				      "clear child fault trace")) {
		goto done;
	}
	minimm_tour_print_page("before-fork", "parent", &before_fork, "private");
	minimm_tour_print_page("after-fork", "parent", &parent_shared, "shared");
	minimm_tour_print_page("after-fork", "child", &child_shared, "shared");

	completed = 0U;
	if (!minimm_tour_check_status(minimm_write(child, address, &child_value,
						   sizeof(child_value), &completed),
				      MINIMM_OK, scenario, "write child through COW") ||
	    !minimm_tour_check(completed == sizeof(child_value), scenario,
			       "child COW write must complete") ||
	    !minimm_tour_check_status(minimm_query_page(parent, address, &parent_split), MINIMM_OK,
				      scenario, "query parent after child write") ||
	    !minimm_tour_check_status(minimm_query_page(child, address, &child_split), MINIMM_OK,
				      scenario, "query child after child write") ||
	    !minimm_tour_check(
		    parent_split.pfn == before_fork.pfn && child_split.pfn != MINIMM_PFN_NONE &&
			    child_split.pfn != parent_split.pfn && parent_split.cow &&
			    !child_split.cow && child_split.dirty,
		    scenario, "child write must split the PFNs and leave parent isolated") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(child, &trace), MINIMM_OK,
				      scenario, "read child COW fault trace") ||
	    !minimm_tour_trace_matches(&trace, 1U, MINIMM_ACCESS_WRITE, MINIMM_FAULT_ORIGIN_ACCESS,
				       MINIMM_FAULT_COW, MINIMM_FAULT_COW_COPIED, MINIMM_OK,
				       scenario, "child write must record one COW-copy fault")) {
		goto done;
	}
	minimm_tour_print_page("child-write", "parent", &parent_split, "split-parent");
	minimm_tour_print_page("child-write", "child", &child_split, "split-child");
	minimm_tour_print_trace("child-cow", &trace);

	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(parent, address, &parent_observed,
						  sizeof(parent_observed), &completed),
				      MINIMM_OK, scenario, "read parent after child write") ||
	    !minimm_tour_check(completed == sizeof(parent_observed), scenario,
			       "parent read must complete")) {
		goto done;
	}
	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(child, address, &child_observed,
						  sizeof(child_observed), &completed),
				      MINIMM_OK, scenario, "read child after COW") ||
	    !minimm_tour_check(completed == sizeof(child_observed), scenario,
			       "child read must complete") ||
	    !minimm_tour_check(parent_observed == parent_value && child_observed == child_value,
			       scenario, "COW must preserve distinct parent and child bytes") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(child, &trace), MINIMM_OK,
				      scenario, "confirm warm child read is not a fault") ||
	    !minimm_tour_check(trace.count == 1U, scenario,
			       "warm reads must not append another fault event") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &stats), MINIMM_OK, scenario,
				      "read final system statistics") ||
	    !minimm_tour_check(stats.frame_count == 2U && stats.resident_count == 2U &&
				       stats.resident_limit == 2U,
			       scenario, "COW split must leave two resident frames")) {
		goto done;
	}
	(void)printf("  bytes=isolated parent=%c child=%c\n", (int)parent_observed,
		     (int)child_observed);
	minimm_tour_print_stats("final", &stats);
	passed = true;

done:
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	(void)printf("result=%s scenario=%s\n", passed ? "PASS" : "FAIL", scenario);
	return passed;
}

static bool minimm_tour_reclaim(void)
{
	static const char scenario[] = "reclaim";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t second_address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args =
		minimm_tour_private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(2));
	minimm_page_info_t locked = { 0 };
	minimm_page_info_t blocked = { 0 };
	minimm_page_info_t evicted = { 0 };
	minimm_page_info_t replacement = { 0 };
	minimm_page_info_t restored = { 0 };
	minimm_page_info_t evicted_again = { 0 };
	minimm_fault_trace_t trace = { 0 };
	minimm_system_stats_t stats = { 0 };
	minimm_pfn_t original_pfn = MINIMM_PFN_NONE;
	const unsigned char original = (unsigned char)'A';
	unsigned char observed = UINT8_C(0xff);
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	(void)printf("scenario=reclaim mlock pressure -> BUSY -> unlock and evict\n");
	if (!minimm_tour_check_status(minimm_create(&config, &mm), MINIMM_OK, scenario,
				      "create one-frame system") ||
	    !minimm_tour_check_status(minimm_space_create(mm, &space), MINIMM_OK, scenario,
				      "create address space") ||
	    !minimm_tour_check_status(minimm_mmap(space, &args, &address), MINIMM_OK, scenario,
				      "map two private anonymous pages")) {
		goto done;
	}
	second_address = address + MINIMM_PAGE_SIZE;
	if (!minimm_tour_check_status(
		    minimm_write(space, address, &original, sizeof(original), &completed),
		    MINIMM_OK, scenario, "write a nonzero byte to the first page") ||
	    !minimm_tour_check(completed == sizeof(original), scenario,
			       "first-page write must complete") ||
	    !minimm_tour_check_status(minimm_mlock(space, address, MINIMM_PAGE_SIZE), MINIMM_OK,
				      scenario, "lock the only resident page") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &locked), MINIMM_OK,
				      scenario, "query locked page") ||
	    !minimm_tour_check(locked.present && locked.resident && locked.locked, scenario,
			       "the first page must be resident and locked") ||
	    !minimm_tour_check_status(minimm_space_clear_fault_trace(space), MINIMM_OK, scenario,
				      "clear trace before pressure fault")) {
		goto done;
	}
	original_pfn = locked.pfn;
	minimm_tour_print_page("mlock", "A", &locked, "only-resident");

	observed = UINT8_C(0xff);
	completed = 0U;
	status = minimm_read(space, second_address, &observed, sizeof(observed), &completed);
	if (!minimm_tour_check_status(status, MINIMM_ERROR_BUSY, scenario,
				      "read second page while every victim is locked") ||
	    !minimm_tour_check(completed == 0U, scenario,
			       "a blocked first-byte read must report zero completed bytes") ||
	    !minimm_tour_check_status(minimm_query_page(space, second_address, &blocked), MINIMM_OK,
				      scenario, "query blocked second page") ||
	    !minimm_tour_check(!blocked.present && !blocked.resident &&
				       blocked.pfn == MINIMM_PFN_NONE,
			       scenario, "failed reclaim must not install the second PTE") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read pressure fault trace") ||
	    !minimm_tour_trace_matches(
		    &trace, 1U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
		    MINIMM_FAULT_NO_FRAME, MINIMM_FAULT_UNRESOLVED, MINIMM_ERROR_BUSY, scenario,
		    "locked pressure must record one unresolved no-frame fault")) {
		goto done;
	}
	minimm_tour_print_page("blocked", "B", &blocked, "none");
	minimm_tour_print_trace("locked-pressure", &trace);

	if (!minimm_tour_check_status(minimm_munlock(space, address, MINIMM_PAGE_SIZE), MINIMM_OK,
				      scenario, "unlock first page") ||
	    !minimm_tour_check_status(minimm_space_clear_fault_trace(space), MINIMM_OK, scenario,
				      "clear trace before successful reclaim")) {
		goto done;
	}
	observed = UINT8_C(0xff);
	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(space, second_address, &observed,
						  sizeof(observed), &completed),
				      MINIMM_OK, scenario, "read second page after unlock") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == UINT8_C(0), scenario,
			       "second page must fault successfully after unlock") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &evicted), MINIMM_OK,
				      scenario, "query evicted first page") ||
	    !minimm_tour_check_status(minimm_query_page(space, second_address, &replacement),
				      MINIMM_OK, scenario, "query resident second page") ||
	    !minimm_tour_check(!evicted.present && !evicted.resident &&
				       evicted.pfn == MINIMM_PFN_NONE && replacement.present &&
				       replacement.resident && replacement.pfn != MINIMM_PFN_NONE,
			       scenario,
			       "unlock must allow B to replace A in the sole resident slot") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read successful reclaim trace") ||
	    !minimm_tour_trace_matches(
		    &trace, 1U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
		    MINIMM_FAULT_NOT_PRESENT, MINIMM_FAULT_ZERO_FILLED, MINIMM_OK, scenario,
		    "post-unlock read must record one successful zero-fill fault")) {
		goto done;
	}
	minimm_tour_print_page("evicted", "A", &evicted, "none");
	minimm_tour_print_page("resident", "B", &replacement, "replacement");
	minimm_tour_print_trace("after-unlock", &trace);

	observed = UINT8_C(0);
	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(space, address, &observed, sizeof(observed),
						  &completed),
				      MINIMM_OK, scenario, "page the first page back in") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == original, scenario,
			       "reclaimed page A must preserve its nonzero byte") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &restored), MINIMM_OK,
				      scenario, "query restored first page") ||
	    !minimm_tour_check_status(minimm_query_page(space, second_address, &evicted_again),
				      MINIMM_OK, scenario, "query second page after A returns") ||
	    !minimm_tour_check(restored.present && restored.resident &&
				       restored.pfn == original_pfn && !evicted_again.present &&
				       !evicted_again.resident &&
				       evicted_again.pfn == MINIMM_PFN_NONE,
			       scenario, "page-in must restore A and evict B from the sole slot") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read preserved page-in trace") ||
	    !minimm_tour_trace_matches(&trace, 2U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
				       MINIMM_FAULT_NOT_PRESENT, MINIMM_FAULT_PAGE_IN, MINIMM_OK,
				       scenario, "reading A again must record a page-in fault") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &stats), MINIMM_OK, scenario,
				      "read final system statistics") ||
	    !minimm_tour_check(stats.frame_count == 2U && stats.resident_count == 1U &&
				       stats.resident_limit == 1U &&
				       stats.page_in_count == UINT64_C(3) &&
				       stats.page_out_count == UINT64_C(2),
			       scenario, "two replacements must preserve the one-frame limit")) {
		goto done;
	}
	minimm_tour_print_page("read-again", "A", &restored, "same-as-before");
	minimm_tour_print_page("evicted-again", "B", &evicted_again, "none");
	minimm_tour_print_trace("preserved-page-in", &trace);
	(void)printf("  bytes=preserved page=A value=%c\n", (int)observed);
	minimm_tour_print_stats("final", &stats);
	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	(void)printf("result=%s scenario=%s\n", passed ? "PASS" : "FAIL", scenario);
	return passed;
}

static bool minimm_tour_working_set(void)
{
	static const char scenario[] = "working-set";
	const unsigned char values[3] = { (unsigned char)'A', (unsigned char)'B',
					  (unsigned char)'C' };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args =
		minimm_tour_private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(3));
	minimm_page_info_t page_a = { 0 };
	minimm_page_info_t page_b = { 0 };
	minimm_page_info_t page_c = { 0 };
	minimm_fault_trace_t trace = { 0 };
	minimm_reclaim_result_t reclaim = { 0 };
	minimm_system_stats_t stats = { 0 };
	minimm_system_stats_t before_hot_read = { 0 };
	minimm_pfn_t original_b_pfn = MINIMM_PFN_NONE;
	unsigned char observed = UINT8_C(0);
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 3U;
	(void)printf("scenario=working-set cold priority -> locked shortfall -> refault\n");
	if (!minimm_tour_check_status(minimm_create(&config, &mm), MINIMM_OK, scenario,
				      "create three-frame system") ||
	    !minimm_tour_check_status(minimm_space_create(mm, &space), MINIMM_OK, scenario,
				      "create working-set space") ||
	    !minimm_tour_check_status(minimm_mmap(space, &args, &address), MINIMM_OK, scenario,
				      "map three private anonymous pages") ||
	    !minimm_tour_check_status(minimm_write(space, address, &values[0], sizeof(values[0]),
						   &completed),
				      MINIMM_OK, scenario, "write page A") ||
	    !minimm_tour_check(completed == sizeof(values[0]), scenario,
			       "page A write must complete") ||
	    !minimm_tour_check_status(minimm_write(space, address + MINIMM_PAGE_SIZE, &values[1],
						   sizeof(values[1]), &completed),
				      MINIMM_OK, scenario, "write page B") ||
	    !minimm_tour_check(completed == sizeof(values[1]), scenario,
			       "page B write must complete") ||
	    !minimm_tour_check_status(minimm_write(space, address + MINIMM_PAGE_SIZE * UINT64_C(2),
						   &values[2], sizeof(values[2]), &completed),
				      MINIMM_OK, scenario, "write page C") ||
	    !minimm_tour_check(completed == sizeof(values[2]), scenario,
			       "page C write must complete") ||
	    !minimm_tour_check_status(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &page_b),
				      MINIMM_OK, scenario, "capture page B identity") ||
	    !minimm_tour_check(page_b.pfn != MINIMM_PFN_NONE, scenario,
			       "resident page B must expose a PFN") ||
	    !minimm_tour_check_status(minimm_mlock(space, address, MINIMM_PAGE_SIZE), MINIMM_OK,
				      scenario, "lock page A") ||
	    !minimm_tour_check_status(minimm_madvise(space, address, MINIMM_PAGE_SIZE,
						     MINIMM_MADV_COLD),
				      MINIMM_OK, scenario, "mark locked page A cold") ||
	    !minimm_tour_check_status(minimm_madvise(space, address + MINIMM_PAGE_SIZE,
						     MINIMM_PAGE_SIZE, MINIMM_MADV_COLD),
				      MINIMM_OK, scenario, "mark page B cold") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &page_a), MINIMM_OK,
				      scenario, "query cold locked A") ||
	    !minimm_tour_check_status(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &page_b),
				      MINIMM_OK, scenario, "query cold B") ||
	    !minimm_tour_check_status(
		    minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2), &page_c),
		    MINIMM_OK, scenario, "query hot C") ||
	    !minimm_tour_check(page_a.resident && page_a.locked && page_a.cold &&
				       !page_a.accessed && page_b.resident && !page_b.locked &&
				       page_b.cold && !page_b.accessed && page_c.resident &&
				       !page_c.cold && page_c.accessed,
			       scenario, "A and B must be cold while only A is unevictable") ||
	    !minimm_tour_check_status(minimm_space_clear_fault_trace(space), MINIMM_OK, scenario,
				      "clear trace before reclaim")) {
		goto done;
	}
	original_b_pfn = page_b.pfn;
	minimm_tour_print_page("prepared", "A", &page_a, "resident");
	minimm_tour_print_page("prepared", "B", &page_b, "resident");
	minimm_tour_print_page("prepared", "C", &page_c, "resident");

	if (!minimm_tour_check_status(minimm_system_reclaim(mm, 1U, &reclaim), MINIMM_OK, scenario,
				      "reclaim one cold frame") ||
	    !minimm_tour_check(reclaim.scanned_count == 3U && reclaim.reclaimed_count == 1U,
			       scenario, "first reclaim must inspect all three frames") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &page_a), MINIMM_OK,
				      scenario, "query A after first reclaim") ||
	    !minimm_tour_check_status(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &page_b),
				      MINIMM_OK, scenario, "query B after first reclaim") ||
	    !minimm_tour_check_status(
		    minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2), &page_c),
		    MINIMM_OK, scenario, "query C after first reclaim") ||
	    !minimm_tour_check(page_a.resident && page_a.locked && !page_b.present &&
				       !page_b.resident && page_b.pfn == MINIMM_PFN_NONE &&
				       page_c.resident,
			       scenario, "locked A must be skipped and cold B selected")) {
		goto done;
	}
	minimm_tour_print_reclaim("cold-victim", &reclaim);
	minimm_tour_print_page("first-victim", "B", &page_b, "none");

	if (!minimm_tour_check_status(minimm_system_reclaim(mm, 2U, &reclaim), MINIMM_OK, scenario,
				      "request two more frames") ||
	    !minimm_tour_check(reclaim.scanned_count == 3U && reclaim.reclaimed_count == 1U,
			       scenario,
			       "locked shortfall must still return a successful result") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &page_a), MINIMM_OK,
				      scenario, "query locked A after shortfall") ||
	    !minimm_tour_check_status(
		    minimm_query_page(space, address + MINIMM_PAGE_SIZE * UINT64_C(2), &page_c),
		    MINIMM_OK, scenario, "query C after shortfall") ||
	    !minimm_tour_check(page_a.resident && page_a.locked && !page_c.present &&
				       !page_c.resident,
			       scenario, "only C may be reclaimed while A stays locked")) {
		goto done;
	}
	minimm_tour_print_reclaim("locked-shortfall", &reclaim);
	minimm_tour_print_page("unevictable", "A", &page_a, "resident");
	minimm_tour_print_page("second-victim", "C", &page_c, "none");

	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(space, address + MINIMM_PAGE_SIZE, &observed,
						  sizeof(observed), &completed),
				      MINIMM_OK, scenario, "refault page B") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == values[1], scenario,
			       "refault must preserve page B's byte") ||
	    !minimm_tour_check_status(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &page_b),
				      MINIMM_OK, scenario, "query refaulted B") ||
	    !minimm_tour_check(page_b.present && page_b.resident && page_b.accessed &&
				       !page_b.cold && page_b.pfn == original_b_pfn,
			       scenario, "refault must restore and warm the same logical frame") ||
	    !minimm_tour_check_status(minimm_space_get_fault_trace(space, &trace), MINIMM_OK,
				      scenario, "read refault trace") ||
	    !minimm_tour_trace_matches(&trace, 1U, MINIMM_ACCESS_READ, MINIMM_FAULT_ORIGIN_ACCESS,
				       MINIMM_FAULT_NOT_PRESENT, MINIMM_FAULT_PAGE_IN, MINIMM_OK,
				       scenario, "B refault must record one page-in") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &before_hot_read), MINIMM_OK,
				      scenario, "read counters after refault") ||
	    !minimm_tour_check(before_hot_read.reclaim_scan_count == UINT64_C(6) &&
				       before_hot_read.reclaim_count == UINT64_C(2) &&
				       before_hot_read.refault_count == UINT64_C(1),
			       scenario, "one policy-refault must be counted")) {
		goto done;
	}
	minimm_tour_print_page("refault", "B", &page_b, "same-as-before");
	minimm_tour_print_trace("refault", &trace);

	completed = 0U;
	if (!minimm_tour_check_status(minimm_read(space, address + MINIMM_PAGE_SIZE, &observed,
						  sizeof(observed), &completed),
				      MINIMM_OK, scenario, "read hot B again") ||
	    !minimm_tour_check(completed == sizeof(observed) && observed == values[1], scenario,
			       "hot B read must preserve its byte") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &stats), MINIMM_OK, scenario,
				      "read counters after hot access") ||
	    !minimm_tour_check(stats.page_in_count == before_hot_read.page_in_count &&
				       stats.refault_count == before_hot_read.refault_count,
			       scenario, "hot access must not count another refault") ||
	    !minimm_tour_check_status(minimm_munlock(space, address, MINIMM_PAGE_SIZE), MINIMM_OK,
				      scenario, "unlock cold A") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &page_a), MINIMM_OK,
				      scenario, "query unlocked A") ||
	    !minimm_tour_check(page_a.resident && !page_a.locked && page_a.cold && !page_a.accessed,
			       scenario, "unlock must preserve A's cold priority") ||
	    !minimm_tour_check_status(minimm_system_reclaim(mm, 1U, &reclaim), MINIMM_OK, scenario,
				      "reclaim after unlocking A") ||
	    !minimm_tour_check(reclaim.scanned_count == 2U && reclaim.reclaimed_count == 1U,
			       scenario, "unlocked A must become reclaimable") ||
	    !minimm_tour_check_status(minimm_query_page(space, address, &page_a), MINIMM_OK,
				      scenario, "query final victim A") ||
	    !minimm_tour_check_status(minimm_query_page(space, address + MINIMM_PAGE_SIZE, &page_b),
				      MINIMM_OK, scenario, "query surviving B") ||
	    !minimm_tour_check(!page_a.present && !page_a.resident &&
				       page_a.pfn == MINIMM_PFN_NONE && page_b.resident,
			       scenario, "cold A must be selected before warm B") ||
	    !minimm_tour_check_status(minimm_system_get_stats(mm, &stats), MINIMM_OK, scenario,
				      "read final working-set counters") ||
	    !minimm_tour_check(
		    stats.frame_count == 3U && stats.resident_count == 1U &&
			    stats.resident_limit == 3U && stats.page_in_count == UINT64_C(4) &&
			    stats.page_out_count == UINT64_C(3) &&
			    stats.reclaim_scan_count == UINT64_C(8) &&
			    stats.reclaim_count == UINT64_C(3) &&
			    stats.refault_count == UINT64_C(1),
		    scenario, "working-set counters must match every scan and transition")) {
		goto done;
	}
	minimm_tour_print_reclaim("unlocked-cold-victim", &reclaim);
	minimm_tour_print_page("final-victim", "A", &page_a, "none");
	minimm_tour_print_page("survivor", "B", &page_b, "resident");
	minimm_tour_print_stats("final", &stats);
	passed = true;

done:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	(void)printf("result=%s scenario=%s\n", passed ? "PASS" : "FAIL", scenario);
	return passed;
}

static bool minimm_tour_inspect(void)
{
	static const char scenario[] = "inspect";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_space_snapshot_t *vma_only = NULL;
	minimm_space_snapshot_t *faulted = NULL;
	minimm_space_snapshot_t *parent_shared = NULL;
	minimm_space_snapshot_t *child_shared = NULL;
	minimm_space_snapshot_t *parent_split = NULL;
	minimm_space_snapshot_t *child_split = NULL;
	minimm_space_snapshot_t *child_paged_out = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args =
		minimm_tour_private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(2));
	minimm_mapping_info_t mapping = { 0 };
	minimm_space_snapshot_page_t first = { 0 };
	minimm_space_snapshot_page_t parent_page = { 0 };
	minimm_space_snapshot_page_t child_page = { 0 };
	minimm_space_snapshot_page_t parent_after = { 0 };
	minimm_space_snapshot_page_t child_after = { 0 };
	minimm_space_snapshot_page_t paged_out = { 0 };
	minimm_space_snapshot_page_t frozen = { 0 };
	const unsigned char parent_value = (unsigned char)'P';
	const unsigned char child_value = (unsigned char)'C';
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	(void)printf("scenario=inspect immutable VMA/PTE snapshots -> COW split -> page-out\n");
	if (!minimm_tour_check_status(minimm_create(&config, &mm), MINIMM_OK, scenario,
				      "create two-frame system") ||
	    !minimm_tour_check_status(minimm_space_create(mm, &parent), MINIMM_OK, scenario,
				      "create parent address space") ||
	    !minimm_tour_check_status(minimm_mmap(parent, &args, &address), MINIMM_OK, scenario,
				      "map two private anonymous pages") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(parent, &vma_only), MINIMM_OK,
				      scenario, "capture VMA-only state") ||
	    !minimm_tour_check(minimm_space_snapshot_mapping_count(vma_only) == 1U &&
				       minimm_space_snapshot_page_count(vma_only) == 0U,
			       scenario, "an untouched anonymous mapping must have no PTEs") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_mapping(vma_only, 0U, &mapping),
				      MINIMM_OK, scenario, "read the captured mapping") ||
	    !minimm_tour_check(mapping.start == address &&
				       mapping.end == address + (MINIMM_PAGE_SIZE * UINT64_C(2)),
			       scenario, "the snapshot must contain the complete VMA") ||
	    !minimm_tour_print_snapshot("vma-only", vma_only, scenario)) {
		goto done;
	}

	if (!minimm_tour_check_status(minimm_write(parent, address, &parent_value,
						   sizeof(parent_value), &completed),
				      MINIMM_OK, scenario, "fault one sparse page") ||
	    !minimm_tour_check(completed == sizeof(parent_value), scenario,
			       "parent write must complete") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(parent, &faulted), MINIMM_OK,
				      scenario, "capture one installed PTE") ||
	    !minimm_tour_check(minimm_space_snapshot_page_count(faulted) == 1U, scenario,
			       "only the touched page must appear in the sparse PTE list") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(faulted, 0U, &first),
				      MINIMM_OK, scenario, "read the sparse PTE") ||
	    !minimm_tour_check(
		    first.page.page_address == address && first.page.present &&
			    first.page.resident && first.page.dirty && first.page.accessed &&
			    first.mapping_cookie == mapping.mapping_cookie &&
			    first.frame_cookie != UINT64_C(0) && first.frame_mapping_count == 1U,
		    scenario, "the PTE snapshot must connect VMA and physical frame state") ||
	    !minimm_tour_print_snapshot("one-pte", faulted, scenario)) {
		goto done;
	}

	if (!minimm_tour_check_status(minimm_space_fork(parent, &child), MINIMM_OK, scenario,
				      "fork the address space") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(parent, &parent_shared),
				      MINIMM_OK, scenario, "capture parent COW state") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(child, &child_shared),
				      MINIMM_OK, scenario, "capture child COW state") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(parent_shared, 0U,
								     &parent_page),
				      MINIMM_OK, scenario, "read parent COW PTE") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(child_shared, 0U, &child_page),
				      MINIMM_OK, scenario, "read child COW PTE") ||
	    !minimm_tour_check(parent_page.page.cow && child_page.page.cow &&
				       parent_page.frame_cookie == child_page.frame_cookie &&
				       parent_page.frame_mapping_count == 2U &&
				       child_page.frame_mapping_count == 2U,
			       scenario, "fork snapshots must expose the shared COW frame") ||
	    !minimm_tour_print_snapshot("fork-parent", parent_shared, scenario) ||
	    !minimm_tour_print_snapshot("fork-child", child_shared, scenario)) {
		goto done;
	}

	completed = 0U;
	if (!minimm_tour_check_status(minimm_write(child, address, &child_value,
						   sizeof(child_value), &completed),
				      MINIMM_OK, scenario, "split the child COW page") ||
	    !minimm_tour_check(completed == sizeof(child_value), scenario,
			       "child write must complete") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(parent, &parent_split),
				      MINIMM_OK, scenario, "capture parent after split") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(child, &child_split), MINIMM_OK,
				      scenario, "capture child after split") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(parent_split, 0U,
								     &parent_after),
				      MINIMM_OK, scenario, "read split parent PTE") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(child_split, 0U, &child_after),
				      MINIMM_OK, scenario, "read split child PTE") ||
	    !minimm_tour_check(parent_after.frame_cookie != child_after.frame_cookie &&
				       !child_after.page.cow &&
				       child_after.frame_mapping_count == 1U,
			       scenario, "the child write must create a distinct frame") ||
	    !minimm_tour_print_snapshot("split-parent", parent_split, scenario) ||
	    !minimm_tour_print_snapshot("split-child", child_split, scenario)) {
		goto done;
	}

	if (!minimm_tour_check_status(minimm_madvise(child, address, MINIMM_PAGE_SIZE,
						     MINIMM_MADV_PAGEOUT),
				      MINIMM_OK, scenario, "page out the split child frame") ||
	    !minimm_tour_check_status(minimm_space_snapshot_capture(child, &child_paged_out),
				      MINIMM_OK, scenario, "capture the nonresident PTE") ||
	    !minimm_tour_check_status(minimm_space_snapshot_get_page(child_paged_out, 0U,
								     &paged_out),
				      MINIMM_OK, scenario, "read the nonresident PTE") ||
	    !minimm_tour_check(
		    !paged_out.page.present && !paged_out.page.resident &&
			    paged_out.page.pfn == MINIMM_PFN_NONE &&
			    paged_out.frame_cookie == child_after.frame_cookie,
		    scenario,
		    "page-out must retain frame identity while hiding the resident PFN") ||
	    !minimm_tour_print_snapshot("child-pageout", child_paged_out, scenario)) {
		goto done;
	}

	minimm_space_destroy(child);
	child = NULL;
	if (!minimm_tour_check_status(minimm_space_snapshot_get_page(child_split, 0U, &frozen),
				      MINIMM_OK, scenario,
				      "read snapshot after source destruction") ||
	    !minimm_tour_check(
		    frozen.page.resident && frozen.frame_cookie == child_after.frame_cookie &&
			    minimm_space_snapshot_page_count(vma_only) == 0U,
		    scenario,
		    "older snapshots must remain immutable and independent of the space")) {
		goto done;
	}
	(void)printf("  frozen=child-before-pageout resident=%s frame=%" PRIu64
		     " source-space=destroyed\n",
		     minimm_tour_bool(frozen.page.resident), frozen.frame_cookie);
	passed = true;

done:
	minimm_space_snapshot_destroy(child_paged_out);
	minimm_space_snapshot_destroy(child_split);
	minimm_space_snapshot_destroy(parent_split);
	minimm_space_snapshot_destroy(child_shared);
	minimm_space_snapshot_destroy(parent_shared);
	minimm_space_snapshot_destroy(faulted);
	minimm_space_snapshot_destroy(vma_only);
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	(void)printf("result=%s scenario=%s\n", passed ? "PASS" : "FAIL", scenario);
	return passed;
}

static int minimm_tour_usage(const char *program)
{
	(void)fprintf(stderr, "Usage: %s fault|cow|reclaim|working-set|inspect|all\n", program);
	return 2;
}

int main(int argument_count, char **arguments)
{
	bool passed = false;

	if (argument_count != 2) {
		return minimm_tour_usage(arguments[0]);
	}
	if (strcmp(arguments[1], "fault") == 0) {
		passed = minimm_tour_fault();
	} else if (strcmp(arguments[1], "cow") == 0) {
		passed = minimm_tour_cow();
	} else if (strcmp(arguments[1], "reclaim") == 0) {
		passed = minimm_tour_reclaim();
	} else if (strcmp(arguments[1], "working-set") == 0) {
		passed = minimm_tour_working_set();
	} else if (strcmp(arguments[1], "inspect") == 0) {
		passed = minimm_tour_inspect();
	} else if (strcmp(arguments[1], "all") == 0) {
		passed = minimm_tour_fault();
		passed = minimm_tour_cow() && passed;
		passed = minimm_tour_reclaim() && passed;
		passed = minimm_tour_working_set() && passed;
		passed = minimm_tour_inspect() && passed;
		(void)printf("result=%s scenario=all\n", passed ? "PASS" : "FAIL");
	} else {
		return minimm_tour_usage(arguments[0]);
	}
	return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
