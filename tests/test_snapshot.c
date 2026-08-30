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

static bool mapping_equals(const minimm_mapping_info_t *left, const minimm_mapping_info_t *right)
{
	return left->start == right->start && left->end == right->end &&
	       left->mapping_cookie == right->mapping_cookie &&
	       left->note_offset == right->note_offset && left->protection == right->protection &&
	       left->maximum_protection == right->maximum_protection && left->flags == right->flags;
}

static bool snapshot_page_equals(const minimm_space_snapshot_page_t *left,
				 const minimm_space_snapshot_page_t *right)
{
	return left->page.page_address == right->page.page_address &&
	       left->page.pfn == right->page.pfn &&
	       left->page.protection == right->page.protection &&
	       left->page.present == right->page.present &&
	       left->page.resident == right->page.resident &&
	       left->page.dirty == right->page.dirty &&
	       left->page.accessed == right->page.accessed && left->page.cow == right->page.cow &&
	       left->page.shared == right->page.shared && left->page.locked == right->page.locked &&
	       left->page.cold == right->page.cold &&
	       left->mapping_cookie == right->mapping_cookie &&
	       left->frame_cookie == right->frame_cookie &&
	       left->frame_mapping_count == right->frame_mapping_count;
}

static bool test_validation_empty_and_vma_only(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_snapshot_t *snapshot = NULL;
	minimm_space_snapshot_t *invalid_snapshot = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(3));
	minimm_mapping_info_t mapping = { 0 };
	minimm_space_snapshot_page_t page = { 0 };
	minimm_space_stats_t stats = { 0 };
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_space_snapshot_capture(NULL, &invalid_snapshot) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   invalid_snapshot == NULL,
		   "reject a null snapshot source") ||
	    !check(minimm_space_snapshot_capture(NULL, NULL) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject null snapshot arguments") ||
	    !check(minimm_space_snapshot_mapping_count(NULL) == 0U &&
			   minimm_space_snapshot_page_count(NULL) == 0U &&
			   minimm_space_snapshot_vma_generation(NULL) == UINT64_C(0) &&
			   minimm_space_snapshot_page_table_generation(NULL) == UINT64_C(0),
		   "null scalar snapshot accessors return zero") ||
	    !check(minimm_space_snapshot_get_stats(NULL, &stats) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   minimm_space_snapshot_get_mapping(NULL, 0U, &mapping) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   minimm_space_snapshot_get_page(NULL, 0U, &page) ==
				   MINIMM_ERROR_INVALID_ARGUMENT,
		   "value accessors reject a null snapshot") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create snapshot system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create snapshot space") ||
	    !check(minimm_space_snapshot_capture(space, NULL) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null snapshot output") ||
	    !check(minimm_space_snapshot_capture(space, &snapshot) == MINIMM_OK && snapshot != NULL,
		   "capture an empty space") ||
	    !check(minimm_space_snapshot_mapping_count(snapshot) == 0U &&
			   minimm_space_snapshot_page_count(snapshot) == 0U &&
			   minimm_space_snapshot_vma_generation(snapshot) != UINT64_C(0) &&
			   minimm_space_snapshot_page_table_generation(snapshot) != UINT64_C(0),
		   "empty snapshot retains nonzero generations") ||
	    !check(minimm_space_snapshot_get_stats(snapshot, &stats) == MINIMM_OK &&
			   stats.vma_count == 0U && stats.pte_count == 0U &&
			   stats.present_count == 0U && stats.resident_count == 0U,
		   "empty snapshot has empty statistics") ||
	    !check(minimm_space_snapshot_get_stats(snapshot, NULL) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   minimm_space_snapshot_get_mapping(snapshot, 0U, &mapping) ==
				   MINIMM_ERROR_NOT_FOUND &&
			   minimm_space_snapshot_get_mapping(snapshot, SIZE_MAX, &mapping) ==
				   MINIMM_ERROR_NOT_FOUND &&
			   minimm_space_snapshot_get_mapping(snapshot, 0U, NULL) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   minimm_space_snapshot_get_page(snapshot, 0U, &page) ==
				   MINIMM_ERROR_NOT_FOUND &&
			   minimm_space_snapshot_get_page(snapshot, SIZE_MAX, &page) ==
				   MINIMM_ERROR_NOT_FOUND &&
			   minimm_space_snapshot_get_page(snapshot, 0U, NULL) ==
				   MINIMM_ERROR_INVALID_ARGUMENT,
		   "snapshot value accessors distinguish null output from an invalid index")) {
		goto done;
	}

	minimm_space_snapshot_destroy(snapshot);
	snapshot = NULL;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "create a VMA without faulting a PTE") ||
	    !check(minimm_space_snapshot_capture(space, &snapshot) == MINIMM_OK,
		   "capture a VMA-only space") ||
	    !check(minimm_space_snapshot_mapping_count(snapshot) == 1U &&
			   minimm_space_snapshot_page_count(snapshot) == 0U,
		   "VMA-only snapshot contains no synthetic pages") ||
	    !check(minimm_space_snapshot_get_stats(snapshot, &stats) == MINIMM_OK &&
			   stats.vma_count == 1U && stats.pte_count == 0U &&
			   stats.present_count == 0U,
		   "VMA-only statistics match snapshot counts") ||
	    !check(minimm_space_snapshot_get_mapping(snapshot, 0U, &mapping) == MINIMM_OK &&
			   mapping.start == address &&
			   mapping.end == address + MINIMM_PAGE_SIZE * UINT64_C(3) &&
			   mapping.mapping_cookie != UINT64_C(0) &&
			   mapping.note_offset == UINT64_C(0) &&
			   mapping.protection == (MINIMM_PROT_READ | MINIMM_PROT_WRITE) &&
			   mapping.maximum_protection == (MINIMM_PROT_READ | MINIMM_PROT_WRITE) &&
			   mapping.flags == MINIMM_MAP_PRIVATE,
		   "VMA snapshot copies the public mapping value") ||
	    !check(minimm_space_snapshot_get_mapping(snapshot, 1U, &mapping) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "VMA-only snapshot rejects an out-of-range mapping index")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_snapshot_destroy(snapshot);
	minimm_space_snapshot_destroy(NULL);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_sparse_pageout_and_lifetime(void)
{
	static const size_t write_order[] = { 3U, 0U, 2U };
	static const unsigned char values[] = { UINT8_C(0x33), UINT8_C(0x10), UINT8_C(0x22) };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_snapshot_t *before = NULL;
	minimm_space_snapshot_t *after_pageout = NULL;
	minimm_space_snapshot_t *after_protect = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(4));
	minimm_mapping_info_t original_mapping = { 0 };
	minimm_mapping_info_t observed_mapping = { 0 };
	minimm_space_snapshot_page_t original_pages[3] = { 0 };
	minimm_space_snapshot_page_t observed_page = { 0 };
	minimm_space_stats_t stats = { 0 };
	size_t completed = 0U;
	size_t index = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create sparse snapshot system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create sparse snapshot space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map sparse snapshot range")) {
		goto done;
	}
	for (index = 0U; index < sizeof(write_order) / sizeof(write_order[0]); ++index) {
		const minimm_vaddr_t page_address =
			address + (minimm_vaddr_t)write_order[index] * MINIMM_PAGE_SIZE;

		if (!check(minimm_write(space, page_address, &values[index], sizeof(values[index]),
					&completed) == MINIMM_OK &&
				   completed == sizeof(values[index]),
			   "fault sparse pages in non-address order")) {
			goto done;
		}
	}
	if (!check(minimm_space_snapshot_capture(space, &before) == MINIMM_OK,
		   "capture sparse resident pages") ||
	    !check(minimm_space_snapshot_mapping_count(before) == 1U &&
			   minimm_space_snapshot_page_count(before) == 3U,
		   "sparse snapshot reports one VMA and three installed PTEs") ||
	    !check(minimm_space_snapshot_get_mapping(before, 0U, &original_mapping) == MINIMM_OK,
		   "copy sparse snapshot mapping") ||
	    !check(minimm_space_snapshot_get_stats(before, &stats) == MINIMM_OK &&
			   stats.vma_count == 1U && stats.pte_count == 3U &&
			   stats.present_count == 3U && stats.resident_count == 3U,
		   "sparse snapshot statistics match captured values")) {
		goto done;
	}
	for (index = 0U; index < 3U; ++index) {
		const size_t expected_page_index = index == 0U ? 0U : index + 1U;

		if (!check(minimm_space_snapshot_get_page(before, index, &original_pages[index]) ==
					   MINIMM_OK &&
				   original_pages[index].page.page_address ==
					   address + (minimm_vaddr_t)expected_page_index *
							     MINIMM_PAGE_SIZE &&
				   original_pages[index].page.present &&
				   original_pages[index].page.resident &&
				   original_pages[index].page.pfn != MINIMM_PFN_NONE &&
				   original_pages[index].frame_cookie ==
					   original_pages[index].page.pfn &&
				   original_pages[index].mapping_cookie ==
					   original_mapping.mapping_cookie &&
				   original_pages[index].frame_mapping_count == 1U,
			   "snapshot pages are address ordered value copies")) {
			goto done;
		}
	}

	if (!check(minimm_madvise(space, address + MINIMM_PAGE_SIZE * UINT64_C(2), MINIMM_PAGE_SIZE,
				  MINIMM_MADV_PAGEOUT) == MINIMM_OK,
		   "page out one installed sparse PTE") ||
	    !check(minimm_space_snapshot_capture(space, &after_pageout) == MINIMM_OK,
		   "capture sparse state after pageout") ||
	    !check(minimm_space_snapshot_page_count(after_pageout) == 3U,
		   "pageout preserves the installed sparse PTE") ||
	    !check(minimm_space_snapshot_get_page(after_pageout, 1U, &observed_page) == MINIMM_OK &&
			   observed_page.page.page_address ==
				   address + MINIMM_PAGE_SIZE * UINT64_C(2) &&
			   !observed_page.page.present && !observed_page.page.resident &&
			   observed_page.page.pfn == MINIMM_PFN_NONE &&
			   observed_page.frame_cookie == original_pages[1].frame_cookie &&
			   observed_page.frame_cookie != UINT64_C(0) &&
			   observed_page.frame_mapping_count == 1U,
		   "nonresident snapshot retains stable frame identity without exposing a PFN") ||
	    !check(minimm_space_snapshot_get_stats(after_pageout, &stats) == MINIMM_OK &&
			   stats.pte_count == 3U && stats.present_count == 2U &&
			   stats.resident_count == 2U,
		   "pageout snapshot statistics describe two resident pages") ||
	    !check(minimm_space_snapshot_get_page(before, 1U, &observed_page) == MINIMM_OK &&
			   snapshot_page_equals(&observed_page, &original_pages[1]),
		   "older resident snapshot is unchanged by pageout") ||
	    !check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE, MINIMM_PROT_READ) == MINIMM_OK,
		   "change the live VMA after earlier captures") ||
	    !check(minimm_space_snapshot_capture(space, &after_protect) == MINIMM_OK,
		   "capture changed VMA state") ||
	    !check(minimm_space_snapshot_vma_generation(after_protect) >
			   minimm_space_snapshot_vma_generation(before),
		   "new capture observes a later VMA generation") ||
	    !check(minimm_space_snapshot_get_mapping(before, 0U, &observed_mapping) == MINIMM_OK &&
			   mapping_equals(&observed_mapping, &original_mapping),
		   "older mapping snapshot is unchanged by mprotect")) {
		goto done;
	}

	minimm_space_destroy(space);
	space = NULL;
	minimm_destroy(mm);
	mm = NULL;
	if (!check(minimm_space_snapshot_mapping_count(before) == 1U &&
			   minimm_space_snapshot_page_count(before) == 3U &&
			   minimm_space_snapshot_get_mapping(before, 0U, &observed_mapping) ==
				   MINIMM_OK &&
			   mapping_equals(&observed_mapping, &original_mapping),
		   "snapshot mapping survives source system destruction") ||
	    !check(minimm_space_snapshot_get_page(before, 1U, &observed_page) == MINIMM_OK &&
			   snapshot_page_equals(&observed_page, &original_pages[1]),
		   "snapshot page survives source system destruction") ||
	    !check(minimm_space_snapshot_get_stats(before, &stats) == MINIMM_OK &&
			   stats.vma_count == 1U && stats.pte_count == 3U &&
			   stats.present_count == 3U,
		   "snapshot statistics survive source system destruction")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_snapshot_destroy(after_protect);
	minimm_space_snapshot_destroy(after_pageout);
	minimm_space_snapshot_destroy(before);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

static bool test_fork_cow_frame_identity(void)
{
	const unsigned char parent_value = UINT8_C(0x41);
	const unsigned char child_value = UINT8_C(0x63);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *parent = NULL;
	minimm_space_t *child = NULL;
	minimm_space_snapshot_t *parent_shared = NULL;
	minimm_space_snapshot_t *child_shared = NULL;
	minimm_space_snapshot_t *parent_split = NULL;
	minimm_space_snapshot_t *child_split = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE);
	minimm_space_snapshot_page_t parent_before = { 0 };
	minimm_space_snapshot_page_t child_before = { 0 };
	minimm_space_snapshot_page_t parent_after = { 0 };
	minimm_space_snapshot_page_t child_after = { 0 };
	minimm_space_snapshot_page_t old_page = { 0 };
	unsigned char observed_parent = UINT8_C(0);
	unsigned char observed_child = UINT8_C(0);
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 3U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create COW snapshot system") ||
	    !check(minimm_space_create(mm, &parent) == MINIMM_OK, "create COW parent") ||
	    !check(minimm_mmap(parent, &args, &address) == MINIMM_OK, "map COW snapshot page") ||
	    !check(minimm_write(parent, address, &parent_value, sizeof(parent_value), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(parent_value),
		   "materialize parent page before fork") ||
	    !check(minimm_space_fork(parent, &child) == MINIMM_OK, "fork snapshot parent") ||
	    !check(minimm_space_snapshot_capture(parent, &parent_shared) == MINIMM_OK &&
			   minimm_space_snapshot_capture(child, &child_shared) == MINIMM_OK,
		   "capture both sides of shared COW state") ||
	    !check(minimm_space_snapshot_get_page(parent_shared, 0U, &parent_before) == MINIMM_OK &&
			   minimm_space_snapshot_get_page(child_shared, 0U, &child_before) ==
				   MINIMM_OK,
		   "copy shared COW page values") ||
	    !check(parent_before.page.cow && child_before.page.cow &&
			   parent_before.frame_cookie != UINT64_C(0) &&
			   parent_before.frame_cookie == child_before.frame_cookie &&
			   parent_before.page.pfn == child_before.page.pfn &&
			   parent_before.frame_mapping_count == 2U &&
			   child_before.frame_mapping_count == 2U,
		   "fork snapshots expose one frame shared by two COW PTEs") ||
	    !check(minimm_write(child, address, &child_value, sizeof(child_value), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(child_value),
		   "split child frame on first COW write") ||
	    !check(minimm_space_snapshot_capture(parent, &parent_split) == MINIMM_OK &&
			   minimm_space_snapshot_capture(child, &child_split) == MINIMM_OK,
		   "capture both sides after COW split") ||
	    !check(minimm_space_snapshot_get_page(parent_split, 0U, &parent_after) == MINIMM_OK &&
			   minimm_space_snapshot_get_page(child_split, 0U, &child_after) ==
				   MINIMM_OK,
		   "copy split COW page values") ||
	    !check(parent_after.frame_cookie == parent_before.frame_cookie &&
			   child_after.frame_cookie != parent_after.frame_cookie &&
			   child_after.frame_cookie != UINT64_C(0) &&
			   parent_after.frame_mapping_count == 1U &&
			   child_after.frame_mapping_count == 1U && !child_after.page.cow &&
			   child_after.page.dirty,
		   "COW split snapshots expose two independently mapped frames") ||
	    !check(minimm_space_snapshot_get_page(parent_shared, 0U, &old_page) == MINIMM_OK &&
			   snapshot_page_equals(&old_page, &parent_before) &&
			   old_page.frame_mapping_count == 2U,
		   "pre-write parent snapshot keeps the old sharing count") ||
	    !check(minimm_space_snapshot_get_page(child_shared, 0U, &old_page) == MINIMM_OK &&
			   snapshot_page_equals(&old_page, &child_before) &&
			   old_page.frame_mapping_count == 2U,
		   "pre-write child snapshot keeps the old sharing count") ||
	    !check(minimm_read(parent, address, &observed_parent, sizeof(observed_parent),
			       &completed) == MINIMM_OK &&
			   completed == sizeof(observed_parent) &&
			   observed_parent == parent_value &&
			   minimm_read(child, address, &observed_child, sizeof(observed_child),
				       &completed) == MINIMM_OK &&
			   completed == sizeof(observed_child) && observed_child == child_value,
		   "COW frame split preserves isolated bytes")) {
		goto done;
	}

	passed = true;

done:
	minimm_space_snapshot_destroy(child_split);
	minimm_space_snapshot_destroy(parent_split);
	minimm_space_snapshot_destroy(child_shared);
	minimm_space_snapshot_destroy(parent_shared);
	minimm_space_destroy(child);
	minimm_space_destroy(parent);
	minimm_destroy(mm);
	return passed;
}

typedef struct snapshot_mutation_context {
	minimm_space_t *space;
	minimm_vaddr_t address;
	atomic_bool start;
	atomic_bool stop;
	atomic_bool failed;
	atomic_size_t mutation_count;
} snapshot_mutation_context_t;

static void *run_snapshot_mutations(void *opaque)
{
	snapshot_mutation_context_t *context = opaque;
	const minimm_vaddr_t middle = context->address + MINIMM_PAGE_SIZE;
	const uint64_t middle_length = MINIMM_PAGE_SIZE * UINT64_C(2);

	while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
		(void)sched_yield();
	}
	while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
		if (minimm_mprotect(context->space, middle, middle_length, MINIMM_PROT_READ) !=
		    MINIMM_OK) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
		(void)atomic_fetch_add_explicit(&context->mutation_count, 1U, memory_order_relaxed);
		(void)sched_yield();
		if (minimm_mprotect(context->space, middle, middle_length,
				    MINIMM_PROT_READ | MINIMM_PROT_WRITE) != MINIMM_OK) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
		(void)atomic_fetch_add_explicit(&context->mutation_count, 1U, memory_order_relaxed);
		(void)sched_yield();
	}
	return NULL;
}

static bool concurrent_snapshot_is_consistent(const minimm_space_snapshot_t *snapshot,
					      minimm_vaddr_t address)
{
	minimm_mapping_info_t mappings[3] = { 0 };
	minimm_space_snapshot_page_t page = { 0 };
	minimm_space_stats_t stats = { 0 };
	const minimm_vaddr_t end = address + MINIMM_PAGE_SIZE * UINT64_C(4);
	minimm_vaddr_t cursor = address;
	uint64_t mapping_cookie = UINT64_C(0);
	size_t mapping_count = minimm_space_snapshot_mapping_count(snapshot);
	size_t page_count = minimm_space_snapshot_page_count(snapshot);
	size_t mapping_index = 0U;
	size_t page_index = 0U;

	if (!check((mapping_count == 1U || mapping_count == 3U) && page_count == 4U,
		   "concurrent snapshot has one complete VMA view") ||
	    !check(minimm_space_snapshot_vma_generation(snapshot) != UINT64_C(0) &&
			   minimm_space_snapshot_page_table_generation(snapshot) != UINT64_C(0),
		   "concurrent snapshot has valid generations") ||
	    !check(minimm_space_snapshot_get_stats(snapshot, &stats) == MINIMM_OK &&
			   stats.vma_count == mapping_count && stats.pte_count == page_count &&
			   stats.present_count == 4U && stats.resident_count == 4U,
		   "concurrent snapshot statistics match its copied arrays")) {
		return false;
	}
	for (mapping_index = 0U; mapping_index < mapping_count; ++mapping_index) {
		minimm_mapping_info_t *mapping = &mappings[mapping_index];

		if (!check(minimm_space_snapshot_get_mapping(snapshot, mapping_index, mapping) ==
					   MINIMM_OK &&
				   mapping->start == cursor && mapping->end > mapping->start &&
				   mapping->end <= end && mapping->mapping_cookie != UINT64_C(0) &&
				   mapping->maximum_protection ==
					   (MINIMM_PROT_READ | MINIMM_PROT_WRITE) &&
				   (mapping->protection == MINIMM_PROT_READ ||
				    mapping->protection == (MINIMM_PROT_READ | MINIMM_PROT_WRITE)),
			   "concurrent snapshot mappings are ordered and complete")) {
			return false;
		}
		if (mapping_index == 0U) {
			mapping_cookie = mapping->mapping_cookie;
		} else if (!check(mapping->mapping_cookie == mapping_cookie,
				  "mprotect fragments retain one mapping identity")) {
			return false;
		}
		cursor = mapping->end;
	}
	if (!check(cursor == end, "concurrent mapping values cover the original VMA")) {
		return false;
	}

	for (page_index = 0U; page_index < page_count; ++page_index) {
		const minimm_vaddr_t page_address =
			address + (minimm_vaddr_t)page_index * MINIMM_PAGE_SIZE;
		const minimm_mapping_info_t *containing = NULL;

		if (!check(minimm_space_snapshot_get_page(snapshot, page_index, &page) ==
					   MINIMM_OK &&
				   page.page.page_address == page_address && page.page.present &&
				   page.page.resident && page.page.pfn == page.frame_cookie &&
				   page.frame_cookie != UINT64_C(0) &&
				   page.frame_mapping_count == 1U,
			   "concurrent snapshot pages are complete and address ordered")) {
			return false;
		}
		for (mapping_index = 0U; mapping_index < mapping_count; ++mapping_index) {
			if (mappings[mapping_index].start <= page_address &&
			    page_address < mappings[mapping_index].end) {
				containing = &mappings[mapping_index];
				break;
			}
		}
		if (!check(containing != NULL &&
				   page.mapping_cookie == containing->mapping_cookie &&
				   page.page.protection == containing->protection,
			   "each concurrent PTE agrees with its captured VMA")) {
			return false;
		}
	}
	return true;
}

static bool test_concurrent_mutation_and_capture(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = private_anonymous_args(MINIMM_PAGE_SIZE * UINT64_C(4));
	snapshot_mutation_context_t context = { 0 };
	pthread_t mutator;
	size_t completed = 0U;
	size_t index = 0U;
	bool thread_started = false;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create concurrent snapshot system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK,
		   "create concurrent snapshot space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK,
		   "map concurrent snapshot range")) {
		goto done;
	}
	for (index = 0U; index < 4U; ++index) {
		const unsigned char value = (unsigned char)(index + 1U);

		if (!check(minimm_write(space, address + (minimm_vaddr_t)index * MINIMM_PAGE_SIZE,
					&value, sizeof(value), &completed) == MINIMM_OK &&
				   completed == sizeof(value),
			   "populate concurrent snapshot page")) {
			goto done;
		}
	}

	context.space = space;
	context.address = address;
	atomic_init(&context.start, false);
	atomic_init(&context.stop, false);
	atomic_init(&context.failed, false);
	atomic_init(&context.mutation_count, 0U);
	if (!check(pthread_create(&mutator, NULL, run_snapshot_mutations, &context) == 0,
		   "start snapshot mutation thread")) {
		goto done;
	}
	thread_started = true;
	atomic_store_explicit(&context.start, true, memory_order_release);
	while (atomic_load_explicit(&context.mutation_count, memory_order_acquire) == 0U &&
	       !atomic_load_explicit(&context.failed, memory_order_acquire)) {
		(void)sched_yield();
	}
	if (!check(!atomic_load_explicit(&context.failed, memory_order_acquire),
		   "begin concurrent mutations successfully")) {
		goto done;
	}

	for (index = 0U; index < 256U; ++index) {
		minimm_space_snapshot_t *snapshot = NULL;

		if (!check(minimm_space_snapshot_capture(space, &snapshot) == MINIMM_OK &&
				   snapshot != NULL,
			   "capture while VMA protection changes") ||
		    !concurrent_snapshot_is_consistent(snapshot, address)) {
			minimm_space_snapshot_destroy(snapshot);
			goto done;
		}
		minimm_space_snapshot_destroy(snapshot);
		if (index == 127U) {
			const size_t mutation_checkpoint =
				atomic_load_explicit(&context.mutation_count, memory_order_acquire);

			while (atomic_load_explicit(&context.mutation_count,
						    memory_order_acquire) == mutation_checkpoint &&
			       !atomic_load_explicit(&context.failed, memory_order_acquire)) {
				(void)sched_yield();
			}
			if (!check(!atomic_load_explicit(&context.failed, memory_order_acquire),
				   "continue concurrent mutations successfully")) {
				goto done;
			}
		}
	}
	if (!check(atomic_load_explicit(&context.mutation_count, memory_order_acquire) > 1U &&
			   !atomic_load_explicit(&context.failed, memory_order_acquire),
		   "capture loop overlaps multiple successful mutations")) {
		goto done;
	}
	passed = true;

done:
	if (thread_started) {
		atomic_store_explicit(&context.stop, true, memory_order_release);
		(void)pthread_join(mutator, NULL);
		if (atomic_load_explicit(&context.failed, memory_order_acquire)) {
			passed = false;
		}
	}
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	return test_validation_empty_and_vma_only() && test_sparse_pageout_and_lifetime() &&
			       test_fork_cow_frame_identity() &&
			       test_concurrent_mutation_and_capture() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
