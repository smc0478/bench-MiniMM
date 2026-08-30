#include "minimm/minimm.h"
#include "../src/page_table.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MINIMM_TEST_WRAP_CALLOC)
static atomic_bool fail_next_pte_allocation;
static atomic_bool pte_allocation_reached;
static atomic_bool pageout_attempt_complete;

void *__real_calloc(size_t element_count, size_t element_size);
void *__wrap_calloc(size_t element_count, size_t element_size);

void *__wrap_calloc(size_t element_count, size_t element_size)
{
	if (element_count == 1U && element_size == sizeof(minimm_pte_t) &&
	    atomic_exchange_explicit(&fail_next_pte_allocation, false, memory_order_acq_rel)) {
		atomic_store_explicit(&pte_allocation_reached, true, memory_order_release);
		while (!atomic_load_explicit(&pageout_attempt_complete, memory_order_acquire)) {
			(void)sched_yield();
		}
		return NULL;
	}
	return __real_calloc(element_count, element_size);
}
#endif

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

#if defined(MINIMM_TEST_WRAP_CALLOC)
typedef struct fixed_rollback_pageout_context {
	minimm_space_t *space;
	minimm_vaddr_t address;
	atomic_bool stop;
	minimm_status_t status;
} fixed_rollback_pageout_context_t;

static void *pageout_during_fixed_rollback(void *opaque)
{
	fixed_rollback_pageout_context_t *context = opaque;

	while (!atomic_load_explicit(&pte_allocation_reached, memory_order_acquire) &&
	       !atomic_load_explicit(&context->stop, memory_order_acquire)) {
		(void)sched_yield();
	}
	if (atomic_load_explicit(&pte_allocation_reached, memory_order_acquire)) {
		context->status = minimm_madvise(context->space, context->address, MINIMM_PAGE_SIZE,
						 MINIMM_MADV_PAGEOUT);
		atomic_store_explicit(&pageout_attempt_complete, true, memory_order_release);
	}
	return NULL;
}

static bool test_fixed_rollback_preserves_locked_residency(void)
{
	static const unsigned char source_values[2] = { UINT8_C(0x42), UINT8_C(0x81) };
	const minimm_vaddr_t source = UINT64_C(0x30000000);
	const minimm_vaddr_t destination = source + (MINIMM_PAGE_SIZE * UINT64_C(8));
	const unsigned char destination_value = UINT8_C(0xd7);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_space_t *alias = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t destination_before = { 0 };
	minimm_page_info_t destination_after = { 0 };
	minimm_page_info_t destination_hole = { 0 };
	minimm_mapping_info_t destination_mapping = { 0 };
	fixed_rollback_pageout_context_t context = { 0 };
	pthread_t pageout_thread = { 0 };
	unsigned char observed = UINT8_C(0);
	size_t completed = 0U;
	size_t index = 0U;
	minimm_status_t remap_status = MINIMM_OK;
	int thread_status = 0;
	bool thread_started = false;
	bool passed = false;
	minimm_mmap_args_t args = {
		.address_hint = source,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
	};

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 4U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create fixed rollback system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create fixed rollback space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK && address == source,
		   "map fixed rollback source")) {
		goto done;
	}
	for (index = 0U; index < 2U; ++index) {
		if (!check(minimm_write(space, source + ((minimm_vaddr_t)index * MINIMM_PAGE_SIZE),
					&source_values[index], sizeof(source_values[index]),
					&completed) == MINIMM_OK &&
				   completed == sizeof(source_values[index]),
			   "fault fixed rollback source page")) {
			goto done;
		}
	}

	args.address_hint = destination;
	args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK && address == destination,
		   "map sparse fixed rollback destination") ||
	    !check(minimm_write(space, destination, &destination_value, sizeof(destination_value),
				&completed) == MINIMM_OK &&
			   completed == sizeof(destination_value),
		   "fault the old fixed destination") ||
	    !check(minimm_mlock(space, destination, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_query_page(space, destination, &destination_before) ==
				   MINIMM_OK &&
			   destination_before.locked && destination_before.resident,
		   "lock the old fixed destination") ||
	    !check(minimm_space_fork(space, &alias) == MINIMM_OK,
		   "create a pageout alias for the old destination")) {
		goto done;
	}

	context.space = alias;
	context.address = destination;
	context.status = MINIMM_OK;
	atomic_init(&context.stop, false);
	atomic_store_explicit(&fail_next_pte_allocation, false, memory_order_relaxed);
	atomic_store_explicit(&pte_allocation_reached, false, memory_order_relaxed);
	atomic_store_explicit(&pageout_attempt_complete, false, memory_order_relaxed);
	thread_status =
		pthread_create(&pageout_thread, NULL, pageout_during_fixed_rollback, &context);
	if (!check(thread_status == 0, "start fixed rollback pageout worker")) {
		goto done;
	}
	thread_started = true;
	atomic_store_explicit(&fail_next_pte_allocation, true, memory_order_release);
	remap_status = minimm_mremap(space, source, MINIMM_PAGE_SIZE * UINT64_C(2),
				     MINIMM_PAGE_SIZE * UINT64_C(2),
				     MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_FIXED, destination,
				     &address);
	atomic_store_explicit(&context.stop, true, memory_order_release);
	(void)pthread_join(pageout_thread, NULL);
	thread_started = false;

	if (!check(atomic_load_explicit(&pte_allocation_reached, memory_order_acquire),
		   "inject failure while installing the second destination PTE") ||
	    !check(remap_status == MINIMM_ERROR_OUT_OF_MEMORY && address == MINIMM_ADDRESS_AUTO,
		   "fixed mremap reports the injected allocation failure") ||
	    !check(minimm_query_page(space, destination, &destination_after) == MINIMM_OK &&
			   destination_after.present && destination_after.locked &&
			   destination_after.resident &&
			   destination_after.pfn == destination_before.pfn,
		   "rollback restores a resident locked destination PTE") ||
	    !check(context.status == MINIMM_ERROR_BUSY,
		   "rollback barrier prevents concurrent pageout of the locked destination") ||
	    !check(minimm_query_page(space, destination + MINIMM_PAGE_SIZE, &destination_hole) ==
				   MINIMM_OK &&
			   !destination_hole.present,
		   "rollback removes partially installed destination PTEs") ||
	    !check(minimm_mapping_query(space, destination, &destination_mapping) == MINIMM_OK &&
			   destination_mapping.flags == MINIMM_MAP_SHARED,
		   "rollback preserves the destination VMA") ||
	    !check(minimm_read(space, destination, &observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) && observed == destination_value,
		   "rollback preserves the destination bytes")) {
		goto done;
	}

	passed = true;

done:
	if (thread_started) {
		atomic_store_explicit(&context.stop, true, memory_order_release);
		atomic_store_explicit(&pageout_attempt_complete, true, memory_order_release);
		(void)pthread_join(pageout_thread, NULL);
	}
	minimm_space_destroy(alias);
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return passed;
}
#else
static bool test_fixed_rollback_preserves_locked_residency(void)
{
	return true;
}
#endif

int main(void)
{
	static const char value[] = "mremap keeps this page";
	static const char overwritten[] = "old destination";
	const minimm_vaddr_t fixed = UINT64_C(0x20000000);
	const minimm_vaddr_t fixed_destination = fixed + (MINIMM_PAGE_SIZE * UINT64_C(8));
	const minimm_vaddr_t dontunmap_source = fixed + (MINIMM_PAGE_SIZE * UINT64_C(16));
	const minimm_vaddr_t dontunmap_fixed_source = fixed + (MINIMM_PAGE_SIZE * UINT64_C(24));
	const minimm_vaddr_t dontunmap_fixed_destination =
		fixed + (MINIMM_PAGE_SIZE * UINT64_C(32));
	const minimm_vaddr_t sparse_source = fixed + (MINIMM_PAGE_SIZE * UINT64_C(40));
	const minimm_vaddr_t sparse_destination = fixed + (MINIMM_PAGE_SIZE * UINT64_C(48));
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t blocker = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t moved = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t duplicate = MINIMM_ADDRESS_AUTO;
	minimm_page_info_t before = { 0 };
	minimm_page_info_t after = { 0 };
	char buffer[sizeof(value)] = { 0 };
	unsigned char zero = UINT8_C(0xff);
	unsigned char sparse_value = UINT8_C(0);
	const unsigned char sparse_first = UINT8_C(0x31);
	const unsigned char sparse_third = UINT8_C(0x73);
	const unsigned char old_destination = UINT8_C(0xcc);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = fixed,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
	};

	if (!test_fixed_rollback_preserves_locked_residency()) {
		return EXIT_FAILURE;
	}

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK && address == fixed,
		   "map fixed remap source") ||
	    !check(minimm_write(space, address, value, sizeof(value), &completed) == MINIMM_OK &&
			   minimm_query_page(space, address, &before) == MINIMM_OK,
		   "fault source page")) {
		goto failure;
	}

	args.address_hint = fixed + MINIMM_PAGE_SIZE;
	if (!check(minimm_mmap(space, &args, &blocker) == MINIMM_OK, "block in-place growth") ||
	    !check(minimm_mremap(space, address, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE * UINT64_C(2),
				 0U, MINIMM_ADDRESS_AUTO, &moved) == MINIMM_ERROR_ADDRESS_IN_USE &&
			   moved == MINIMM_ADDRESS_AUTO,
		   "growth without MAYMOVE preserves source") ||
	    !check(minimm_mremap(space, address, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE * UINT64_C(2),
				 MINIMM_MREMAP_MAYMOVE, MINIMM_ADDRESS_AUTO, &moved) == MINIMM_OK &&
			   moved != address,
		   "move mapping when adjacent range is occupied") ||
	    !check(minimm_query_page(space, moved, &after) == MINIMM_OK && after.pfn == before.pfn,
		   "move preserves the existing PTE and PFN") ||
	    !check(minimm_read(space, moved, buffer, sizeof(buffer), &completed) == MINIMM_OK &&
			   memcmp(buffer, value, sizeof(value)) == 0,
		   "move preserves bytes") ||
	    !check(minimm_read(space, address, buffer, 1U, &completed) == MINIMM_ERROR_NOT_FOUND,
		   "old address becomes unmapped") ||
	    !check(minimm_mremap(space, moved, MINIMM_PAGE_SIZE * UINT64_C(2),
				 MINIMM_PAGE_SIZE * UINT64_C(3), 0U, MINIMM_ADDRESS_AUTO,
				 &address) == MINIMM_OK &&
			   address == moved,
		   "grow moved mapping in place") ||
	    !check(minimm_write(space, moved + MINIMM_PAGE_SIZE * UINT64_C(2), value, sizeof(value),
				&completed) == MINIMM_OK,
		   "newly grown page demand faults") ||
	    !check(minimm_mremap(space, moved, MINIMM_PAGE_SIZE * UINT64_C(3), MINIMM_PAGE_SIZE, 0U,
				 MINIMM_ADDRESS_AUTO, &address) == MINIMM_OK &&
			   address == moved,
		   "shrink mapping in place") ||
	    !check(minimm_read(space, moved + MINIMM_PAGE_SIZE, buffer, 1U, &completed) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "shrink removes tail pages") ||
	    !check(minimm_mremap(space, moved, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				 MINIMM_MREMAP_MAYMOVE, fixed_destination,
				 &address) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "nonfixed mremap rejects the old custom destination hint") ||
	    !check(minimm_mremap(space, moved, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				 MINIMM_MREMAP_FIXED, fixed_destination,
				 &address) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "MREMAP_FIXED requires MAYMOVE")) {
		goto failure;
	}

	args.address_hint = fixed_destination;
	if (!check(minimm_mmap(space, &args, &blocker) == MINIMM_OK,
		   "map an occupied fixed destination") ||
	    !check(minimm_write(space, blocker, overwritten, sizeof(overwritten), &completed) ==
			   MINIMM_OK,
		   "fault the old destination") ||
	    !check(minimm_mlock(space, blocker, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "lock the destination replaced by fixed mremap") ||
	    !check(minimm_mremap(space, moved, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				 MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_FIXED, fixed_destination,
				 &address) == MINIMM_OK &&
			   address == fixed_destination,
		   "MREMAP_FIXED replaces the destination mapping") ||
	    !check(minimm_query_page(space, fixed_destination, &after) == MINIMM_OK &&
			   after.pfn == before.pfn && !after.locked,
		   "fixed move preserves the source PTE without the replaced destination lock") ||
	    !check(minimm_read(space, fixed_destination, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   memcmp(buffer, value, sizeof(value)) == 0,
		   "fixed move replaces old destination bytes") ||
	    !check(minimm_read(space, moved, buffer, 1U, &completed) == MINIMM_ERROR_NOT_FOUND,
		   "fixed move unmaps the old VMA") ||
	    !check(minimm_mremap(space, fixed_destination, UINT64_C(0), MINIMM_PAGE_SIZE,
				 MINIMM_MREMAP_MAYMOVE, MINIMM_ADDRESS_AUTO,
				 &address) == MINIMM_ERROR_UNSUPPORTED &&
			   address == MINIMM_ADDRESS_AUTO,
		   "old-size-zero shareable clone mode is explicitly unsupported")) {
		goto failure;
	}

	args.length = MINIMM_PAGE_SIZE * UINT64_C(2);
	args.address_hint = dontunmap_source;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK && address == dontunmap_source,
		   "map DONTUNMAP source") ||
	    !check(minimm_write(space, address, value, sizeof(value), &completed) == MINIMM_OK &&
			   minimm_mlock(space, address, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_query_page(space, address, &before) == MINIMM_OK && before.locked,
		   "fault and lock DONTUNMAP source") ||
	    !check(minimm_mremap(space, address, args.length, args.length,
				 MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_DONTUNMAP,
				 MINIMM_ADDRESS_AUTO, &duplicate) == MINIMM_OK &&
			   duplicate != address,
		   "DONTUNMAP chooses a free destination") ||
	    !check(minimm_query_page(space, duplicate, &after) == MINIMM_OK && after.present &&
			   after.pfn == before.pfn && after.locked,
		   "DONTUNMAP moves the PTE and lock state") ||
	    !check(minimm_query_page(space, address, &after) == MINIMM_OK && !after.present,
		   "DONTUNMAP retains an empty source VMA") ||
	    !check(minimm_read(space, duplicate, buffer, sizeof(buffer), &completed) == MINIMM_OK &&
			   memcmp(buffer, value, sizeof(value)) == 0,
		   "DONTUNMAP destination retains bytes") ||
	    !check(minimm_read(space, address, &zero, sizeof(zero), &completed) == MINIMM_OK &&
			   zero == UINT8_C(0),
		   "DONTUNMAP source refaults as zero") ||
	    !check(minimm_munmap(space, address, args.length) == MINIMM_OK,
		   "remove the retained DONTUNMAP source VMA") ||
	    !check((zero = UINT8_C(0xff), minimm_read(space, duplicate + MINIMM_PAGE_SIZE, &zero,
						      sizeof(zero), &completed)) == MINIMM_OK &&
			   completed == sizeof(zero) && zero == UINT8_C(0),
		   "destination binding survives source removal for later faults") ||
	    !check(minimm_munlock(space, duplicate, MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "moved lock can be released at the destination")) {
		goto failure;
	}

	args.length = MINIMM_PAGE_SIZE;
	args.address_hint = dontunmap_fixed_source;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK &&
			   minimm_write(space, address, value, sizeof(value), &completed) ==
				   MINIMM_OK,
		   "map fixed DONTUNMAP source")) {
		goto failure;
	}
	args.address_hint = dontunmap_fixed_destination;
	if (!check(minimm_mmap(space, &args, &blocker) == MINIMM_OK &&
			   minimm_write(space, blocker, overwritten, sizeof(overwritten),
					&completed) == MINIMM_OK,
		   "map fixed DONTUNMAP destination") ||
	    !check(minimm_mremap(space, address, MINIMM_PAGE_SIZE, MINIMM_PAGE_SIZE,
				 MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_FIXED |
					 MINIMM_MREMAP_DONTUNMAP,
				 dontunmap_fixed_destination, &duplicate) == MINIMM_OK &&
			   duplicate == dontunmap_fixed_destination,
		   "fixed DONTUNMAP replaces its destination") ||
	    !check(minimm_read(space, duplicate, buffer, sizeof(buffer), &completed) == MINIMM_OK &&
			   memcmp(buffer, value, sizeof(value)) == 0,
		   "fixed DONTUNMAP moves source bytes") ||
	    !check(minimm_query_page(space, address, &after) == MINIMM_OK && !after.present,
		   "fixed DONTUNMAP retains the source VMA without its PTE")) {
		goto failure;
	}

	args.length = MINIMM_PAGE_SIZE * UINT64_C(4);
	args.address_hint = sparse_source;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK && address == sparse_source,
		   "map a sparse fixed-move source") ||
	    !check(minimm_write(space, sparse_source, &sparse_first, sizeof(sparse_first),
				&completed) == MINIMM_OK &&
			   minimm_write(space, sparse_source + (MINIMM_PAGE_SIZE * UINT64_C(2)),
					&sparse_third, sizeof(sparse_third),
					&completed) == MINIMM_OK,
		   "fault nonadjacent sparse source pages")) {
		goto failure;
	}
	args.length = MINIMM_PAGE_SIZE * UINT64_C(5);
	args.address_hint = sparse_destination;
	if (!check(minimm_mmap(space, &args, &blocker) == MINIMM_OK &&
			   blocker == sparse_destination,
		   "map a larger fixed-move destination") ||
	    !check(minimm_write(space, sparse_destination + MINIMM_PAGE_SIZE, &old_destination,
				sizeof(old_destination), &completed) == MINIMM_OK &&
			   minimm_write(space,
					sparse_destination + (MINIMM_PAGE_SIZE * UINT64_C(4)),
					&old_destination, sizeof(old_destination),
					&completed) == MINIMM_OK,
		   "fault destination pages that source holes must replace") ||
	    !check(minimm_mremap(space, sparse_source, MINIMM_PAGE_SIZE * UINT64_C(4),
				 MINIMM_PAGE_SIZE * UINT64_C(5),
				 MINIMM_MREMAP_MAYMOVE | MINIMM_MREMAP_FIXED, sparse_destination,
				 &moved) == MINIMM_OK &&
			   moved == sparse_destination,
		   "fixed mremap grows and preserves a sparse PTE layout") ||
	    !check(minimm_read(space, sparse_source, &sparse_value, sizeof(sparse_value),
			       &completed) == MINIMM_ERROR_NOT_FOUND,
		   "sparse fixed move removes the complete source") ||
	    !check((sparse_value = UINT8_C(0), minimm_read(space, sparse_destination, &sparse_value,
							   sizeof(sparse_value), &completed)) ==
				   MINIMM_OK &&
			   sparse_value == sparse_first,
		   "sparse fixed move keeps the first source page") ||
	    !check((sparse_value = UINT8_C(0xff),
		    minimm_read(space, sparse_destination + MINIMM_PAGE_SIZE, &sparse_value,
				sizeof(sparse_value), &completed)) == MINIMM_OK &&
			   sparse_value == UINT8_C(0),
		   "source hole replaces an old destination PTE") ||
	    !check((sparse_value = UINT8_C(0),
		    minimm_read(space, sparse_destination + (MINIMM_PAGE_SIZE * UINT64_C(2)),
				&sparse_value, sizeof(sparse_value), &completed)) == MINIMM_OK &&
			   sparse_value == sparse_third,
		   "sparse fixed move keeps a distant source page") ||
	    !check((sparse_value = UINT8_C(0xff),
		    minimm_read(space, sparse_destination + (MINIMM_PAGE_SIZE * UINT64_C(4)),
				&sparse_value, sizeof(sparse_value), &completed)) == MINIMM_OK &&
			   sparse_value == UINT8_C(0),
		   "grown fixed-move tail replaces old destination PTEs with holes")) {
		goto failure;
	}

	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_SUCCESS;

failure:
	minimm_space_destroy(space);
	minimm_destroy(mm);
	return EXIT_FAILURE;
}
