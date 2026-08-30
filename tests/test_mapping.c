#include "minimm/minimm.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool test_high_hint_does_not_exhaust(minimm_t *mm)
{
	minimm_space_t *space = NULL;
	minimm_vaddr_t high = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t automatic = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t fallback = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_READ,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};

	if (!check(minimm_space_create(mm, &space) == MINIMM_OK,
		   "create high-hint address space") ||
	    !check(minimm_mmap(space, &args, &high) == MINIMM_OK,
		   "accept a valid high advisory hint") ||
	    !check(high == MINIMM_USER_ADDRESS_LIMIT - MINIMM_PAGE_SIZE, "high hint is honored")) {
		minimm_space_destroy(space);
		return false;
	}

	args.address_hint = MINIMM_ADDRESS_AUTO;
	if (!check(minimm_mmap(space, &args, &automatic) == MINIMM_OK,
		   "automatic mapping still finds low free space") ||
	    !check(automatic < high, "automatic mapping did not exhaust VA space")) {
		minimm_space_destroy(space);
		return false;
	}

	args.address_hint = high;
	if (!check(minimm_mmap(space, &args, &fallback) == MINIMM_OK,
		   "colliding high hint wraps to the low search base") ||
	    !check(fallback < high, "hint fallback returns low free space")) {
		minimm_space_destroy(space);
		return false;
	}

	minimm_space_destroy(space);
	return true;
}

static bool test_linux_style_default_permissions(minimm_t *mm)
{
	minimm_space_t *space = NULL;
	minimm_note_t *note = NULL;
	minimm_vaddr_t private_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t shared_address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t rejected = MINIMM_ADDRESS_AUTO;
	minimm_mapping_info_t info = { 0 };
	unsigned char value = UINT8_C(0x5a);
	size_t completed = 0U;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_NONE,
		.flags = MINIMM_MAP_PRIVATE,
	};
	bool passed = false;

	if (!check(minimm_space_create(mm, &space) == MINIMM_OK,
		   "create permission address space") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE,
				      MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_SHARE,
				      &note) == MINIMM_OK,
		   "create readable shareable note")) {
		goto done;
	}

	args.note = note;
	if (!check(minimm_mmap(space, &args, &private_address) == MINIMM_OK,
		   "private file mapping needs only read right") ||
	    !check(minimm_mapping_query(space, private_address, &info) == MINIMM_OK &&
			   (info.maximum_protection &
			    (MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EXEC)) ==
				   (MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EXEC) &&
			   (info.maximum_protection & MINIMM_PROT_EDIT) == 0U,
		   "default private maximum models VM_MAY permissions") ||
	    !check(minimm_mprotect(space, private_address, MINIMM_PAGE_SIZE,
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_OK,
		   "private mapping may become writable") ||
	    !check(minimm_write(space, private_address, &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(value),
		   "private writable mapping resolves through COW")) {
		goto done;
	}

	args.flags = MINIMM_MAP_SHARED;
	args.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE;
	if (!check(minimm_mmap(space, &args, &rejected) == MINIMM_ERROR_PERMISSION &&
			   rejected == MINIMM_ADDRESS_AUTO,
		   "shared writable mapping still needs write right")) {
		goto done;
	}
	args.protection = MINIMM_PROT_READ;
	if (!check(minimm_mmap(space, &args, &shared_address) == MINIMM_OK,
		   "read-only shared mapping is allowed") ||
	    !check(minimm_mapping_query(space, shared_address, &info) == MINIMM_OK &&
			   (info.maximum_protection & MINIMM_PROT_WRITE) == 0U,
		   "shared default maximum respects backing write right") ||
	    !check(minimm_mprotect(space, shared_address, MINIMM_PAGE_SIZE,
				   MINIMM_PROT_READ | MINIMM_PROT_WRITE) == MINIMM_ERROR_PERMISSION,
		   "shared mapping cannot exceed its VM_MAY ceiling") ||
	    !check(minimm_mprotect(space, shared_address, UINT64_C(0), MINIMM_PROT_NONE) ==
				   MINIMM_OK &&
			   minimm_mapping_query(space, shared_address, &info) == MINIMM_OK &&
			   info.protection == MINIMM_PROT_READ,
		   "zero-length mprotect is a no-op")) {
		goto done;
	}

	passed = true;

done:
	minimm_note_release(note);
	minimm_space_destroy(space);
	return passed;
}

enum {
	MODEL_SLOT_COUNT = 64,
	MODEL_ITERATIONS = 6000,
	RCU_READER_COUNT = 2,
	RCU_UPDATE_COUNT = 4000
};

typedef struct model_slot {
	unsigned char value;
	bool mapped;
	bool writable;
} model_slot_t;

static uint64_t test_next_random(uint64_t *state)
{
	uint64_t value = *state;

	value ^= value << 13U;
	value ^= value >> 7U;
	value ^= value << 17U;
	*state = value;
	return value;
}

static bool test_model_range_is_mapped(const model_slot_t *slots, size_t start, size_t count)
{
	size_t index = 0U;

	for (index = start; index < start + count; ++index) {
		if (!slots[index].mapped) {
			return false;
		}
	}
	return true;
}

static bool test_model_range_has_mapping(const model_slot_t *slots, size_t start, size_t count)
{
	size_t index = 0U;

	for (index = start; index < start + count; ++index) {
		if (slots[index].mapped) {
			return true;
		}
	}
	return false;
}

static bool test_validate_mapping_model(minimm_space_t *space, minimm_vaddr_t base,
					const model_slot_t *slots)
{
	size_t index = 0U;

	for (index = 0U; index < MODEL_SLOT_COUNT; ++index) {
		const minimm_vaddr_t address = base + ((minimm_vaddr_t)index * MINIMM_PAGE_SIZE);
		minimm_mapping_info_t info = { 0 };
		unsigned char observed = UINT8_C(0xff);
		size_t completed = SIZE_MAX;
		const minimm_status_t query_status = minimm_mapping_query(space, address, &info);

		if (!slots[index].mapped) {
			if (!check(query_status == MINIMM_ERROR_NOT_FOUND,
				   "state model finds every unmapped slot")) {
				return false;
			}
			continue;
		}
		if (!check(query_status == MINIMM_OK && info.start <= address &&
				   address < info.end &&
				   info.protection == (slots[index].writable ? (MINIMM_PROT_READ |
										MINIMM_PROT_WRITE) :
									       MINIMM_PROT_READ),
			   "state model observes coherent VMA protection") ||
		    !check(minimm_read(space, address + UINT64_C(23), &observed, sizeof(observed),
				       &completed) == MINIMM_OK &&
				   completed == sizeof(observed) && observed == slots[index].value,
			   "state model observes the expected private page byte")) {
			return false;
		}
	}
	return true;
}

static bool test_mapping_state_model(minimm_t *mm)
{
	const minimm_vaddr_t base = UINT64_C(0x20000000);
	model_slot_t slots[MODEL_SLOT_COUNT] = { 0 };
	minimm_space_t *space = NULL;
	uint64_t random_state = UINT64_C(0x4d595df4d0f33173);
	size_t iteration = 0U;
	bool passed = false;

	if (!check(minimm_space_create(mm, &space) == MINIMM_OK, "create state-model space") ||
	    !check(minimm_munmap(space, UINT64_C(0), MINIMM_PAGE_SIZE) == MINIMM_OK,
		   "munmap accepts an unmapped low canonical range") ||
	    !check(minimm_mprotect(space, UINT64_C(0), MINIMM_PAGE_SIZE, MINIMM_PROT_READ) ==
			   MINIMM_ERROR_NOT_FOUND,
		   "mprotect reports an unmapped low canonical range")) {
		goto done;
	}

	for (iteration = 0U; iteration < MODEL_ITERATIONS; ++iteration) {
		const uint64_t random = test_next_random(&random_state);
		const size_t start = (size_t)(random % MODEL_SLOT_COUNT);
		size_t count = (size_t)((random >> 8U) % 4U) + 1U;
		const unsigned operation = (unsigned)((random >> 16U) % 7U);
		const minimm_vaddr_t address = base + ((minimm_vaddr_t)start * MINIMM_PAGE_SIZE);
		minimm_status_t status = MINIMM_OK;
		size_t index = 0U;

		if (count > MODEL_SLOT_COUNT - start) {
			count = MODEL_SLOT_COUNT - start;
		}
		if (operation == 0U || operation == 6U) {
			const bool noreplace = operation == 6U;
			const bool occupied = test_model_range_has_mapping(slots, start, count);
			minimm_vaddr_t mapped = MINIMM_ADDRESS_AUTO;
			minimm_mmap_args_t args = {
				.address_hint = address,
				.length = (uint64_t)count * MINIMM_PAGE_SIZE,
				.protection = noreplace ? MINIMM_PROT_READ :
							  MINIMM_PROT_READ | MINIMM_PROT_WRITE,
				.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
				.flags =
					MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS |
					(noreplace ? MINIMM_MAP_FIXED_NOREPLACE : MINIMM_MAP_FIXED),
			};

			status = minimm_mmap(space, &args, &mapped);
			if (noreplace && occupied) {
				if (!check(status == MINIMM_ERROR_ADDRESS_IN_USE &&
						   mapped == MINIMM_ADDRESS_AUTO,
					   "state model preserves an occupied noreplace range")) {
					goto done;
				}
			} else {
				if (!check(status == MINIMM_OK && mapped == address,
					   "state model installs the requested fixed range")) {
					goto done;
				}
				for (index = start; index < start + count; ++index) {
					slots[index].mapped = true;
					slots[index].writable = !noreplace;
					slots[index].value = UINT8_C(0);
				}
			}
		} else if (operation == 1U) {
			status = minimm_munmap(space, address, (uint64_t)count * MINIMM_PAGE_SIZE);
			if (!check(status == MINIMM_OK, "state model unmaps any canonical range")) {
				goto done;
			}
			for (index = start; index < start + count; ++index) {
				slots[index] = (model_slot_t){ 0 };
			}
		} else if (operation == 2U) {
			const bool mapped = test_model_range_is_mapped(slots, start, count);
			const bool writable = (random & (UINT64_C(1) << 24U)) != 0U;
			const minimm_prot_t protection =
				writable ? MINIMM_PROT_READ | MINIMM_PROT_WRITE : MINIMM_PROT_READ;

			status = minimm_mprotect(space, address, (uint64_t)count * MINIMM_PAGE_SIZE,
						 protection);
			if (!check(status == (mapped ? MINIMM_OK : MINIMM_ERROR_NOT_FOUND),
				   "state model applies mprotect transactionally")) {
				goto done;
			}
			if (mapped) {
				for (index = start; index < start + count; ++index) {
					slots[index].writable = writable;
				}
			}
		} else if (operation == 3U) {
			const unsigned char value = (unsigned char)(random >> 32U);
			size_t completed = SIZE_MAX;
			const minimm_status_t expected =
				!slots[start].mapped  ? MINIMM_ERROR_NOT_FOUND :
				slots[start].writable ? MINIMM_OK :
							MINIMM_ERROR_PERMISSION;

			status = minimm_write(space, address + UINT64_C(23), &value, sizeof(value),
					      &completed);
			if (!check(status == expected &&
					   completed ==
						   (expected == MINIMM_OK ? sizeof(value) : 0U),
				   "state model enforces write access")) {
				goto done;
			}
			if (status == MINIMM_OK) {
				slots[start].value = value;
			}
		} else if (operation == 4U) {
			unsigned char observed = UINT8_C(0xff);
			size_t completed = SIZE_MAX;
			const minimm_status_t expected =
				slots[start].mapped ? MINIMM_OK : MINIMM_ERROR_NOT_FOUND;

			status = minimm_read(space, address + UINT64_C(23), &observed,
					     sizeof(observed), &completed);
			if (!check(status == expected &&
					   completed == (expected == MINIMM_OK ? sizeof(observed) :
										 0U) &&
					   (expected != MINIMM_OK ||
					    observed == slots[start].value),
				   "state model enforces reads and preserves bytes")) {
				goto done;
			}
		} else {
			const bool mapped = test_model_range_is_mapped(slots, start, count);

			status = minimm_madvise(space, address, (uint64_t)count * MINIMM_PAGE_SIZE,
						MINIMM_MADV_DONTNEED);
			if (!check(status == (mapped ? MINIMM_OK : MINIMM_ERROR_NOT_FOUND),
				   "state model validates DONTNEED before discarding PTEs")) {
				goto done;
			}
			if (mapped) {
				for (index = start; index < start + count; ++index) {
					slots[index].value = UINT8_C(0);
				}
			}
		}

		if ((iteration % 97U) == 0U && !test_validate_mapping_model(space, base, slots)) {
			goto done;
		}
	}
	passed = test_validate_mapping_model(space, base, slots);

done:
	minimm_space_destroy(space);
	return passed;
}

typedef struct mapping_rcu_context {
	minimm_space_t *space;
	minimm_vaddr_t address;
	atomic_bool stop;
	atomic_bool failed;
} mapping_rcu_context_t;

static void *test_mapping_rcu_reader(void *opaque)
{
	mapping_rcu_context_t *context = opaque;

	while (!atomic_load_explicit(&context->stop, memory_order_acquire)) {
		minimm_mapping_info_t info = { 0 };
		const minimm_status_t status =
			minimm_mapping_query(context->space, context->address, &info);

		if (status == MINIMM_OK) {
			if (info.start > context->address || context->address >= info.end ||
			    info.mapping_cookie == UINT64_C(0) ||
			    info.flags != MINIMM_MAP_PRIVATE ||
			    (info.protection != MINIMM_PROT_READ &&
			     info.protection != (MINIMM_PROT_READ | MINIMM_PROT_WRITE))) {
				atomic_store_explicit(&context->failed, true, memory_order_release);
				break;
			}
		} else if (status != MINIMM_ERROR_NOT_FOUND) {
			atomic_store_explicit(&context->failed, true, memory_order_release);
			break;
		}
	}
	return NULL;
}

static bool test_mapping_rcu_publish(minimm_t *mm)
{
	const minimm_vaddr_t address = UINT64_C(0x30000000);
	mapping_rcu_context_t context = { .space = NULL, .address = address };
	pthread_t readers[RCU_READER_COUNT] = { 0 };
	size_t started = 0U;
	size_t index = 0U;
	bool passed = false;

	atomic_init(&context.stop, false);
	atomic_init(&context.failed, false);
	if (!check(minimm_space_create(mm, &context.space) == MINIMM_OK,
		   "create concurrent mapping-query space")) {
		goto done;
	}
	for (started = 0U; started < RCU_READER_COUNT; ++started) {
		if (!check(pthread_create(&readers[started], NULL, test_mapping_rcu_reader,
					  &context) == 0,
			   "start concurrent mapping-query reader")) {
			goto stop;
		}
	}

	for (index = 0U; index < RCU_UPDATE_COUNT; ++index) {
		if ((index % 3U) == 0U) {
			if (!check(minimm_munmap(context.space, address, MINIMM_PAGE_SIZE) ==
					   MINIMM_OK,
				   "publish an unmapped RCU snapshot")) {
				goto stop;
			}
		} else {
			minimm_vaddr_t mapped = MINIMM_ADDRESS_AUTO;
			minimm_mmap_args_t args = {
				.address_hint = address,
				.length = MINIMM_PAGE_SIZE,
				.protection = (index & 1U) != 0U ?
						      MINIMM_PROT_READ :
						      MINIMM_PROT_READ | MINIMM_PROT_WRITE,
				.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
				.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS |
					 MINIMM_MAP_FIXED,
			};

			if (!check(minimm_mmap(context.space, &args, &mapped) == MINIMM_OK &&
					   mapped == address,
				   "publish a replaced RCU mapping snapshot")) {
				goto stop;
			}
		}
		if (atomic_load_explicit(&context.failed, memory_order_acquire)) {
			goto stop;
		}
	}
	passed = true;

stop:
	atomic_store_explicit(&context.stop, true, memory_order_release);
	for (index = 0U; index < started; ++index) {
		if (pthread_join(readers[index], NULL) != 0) {
			passed = false;
		}
	}
	if (atomic_load_explicit(&context.failed, memory_order_acquire)) {
		passed = false;
	}

done:
	minimm_space_destroy(context.space);
	return passed;
}

static bool test_mmap_args_output_alias(minimm_t *mm)
{
	const minimm_vaddr_t requested = UINT64_C(0x30000000);
	minimm_space_t *space = NULL;
	minimm_mapping_info_t info = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = requested,
		.length = MINIMM_PAGE_SIZE,
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_READ,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE,
	};
	bool passed = false;

	if (!check(minimm_space_create(mm, &space) == MINIMM_OK,
		   "create mmap alias address space") ||
	    !check(minimm_mmap(space, &args, &args.address_hint) == MINIMM_OK &&
			   args.address_hint == requested,
		   "mmap snapshots arguments before clearing an aliased output") ||
	    !check(minimm_mapping_query(space, requested, &info) == MINIMM_OK &&
			   info.start == requested && info.end == requested + MINIMM_PAGE_SIZE,
		   "aliased mmap output installs the requested fixed mapping")) {
		goto done;
	}
	passed = true;

done:
	minimm_space_destroy(space);
	return passed;
}

int main(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_vaddr_t replacement = MINIMM_ADDRESS_AUTO;
	minimm_mapping_info_t info = { 0 };
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE * UINT64_C(2),
		.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE,
		.maximum_protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT,
		.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS,
	};

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create address space") ||
	    !check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map pages") ||
	    !check(address % MINIMM_PAGE_SIZE == 0U, "mapping is page aligned") ||
	    !check(minimm_mapping_query(space, address + 17U, &info) == MINIMM_OK,
		   "query mapping") ||
	    !check(info.start == address && info.end == address + (MINIMM_PAGE_SIZE * UINT64_C(2)),
		   "query returns the mapped range") ||
	    !check(info.flags == MINIMM_MAP_PRIVATE,
		   "stored mapping keeps normalized private semantics")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	args.address_hint = address;
	args.length = MINIMM_PAGE_SIZE;
	args.flags = MINIMM_MAP_PRIVATE | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED_NOREPLACE;
	if (!check(minimm_mmap(space, &args, &replacement) == MINIMM_ERROR_ADDRESS_IN_USE,
		   "fixed-noreplace rejects an overlap") ||
	    !check(replacement == MINIMM_ADDRESS_AUTO, "failed mmap clears its output")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	args.address_hint = address + MINIMM_PAGE_SIZE;
	args.protection = MINIMM_PROT_READ;
	args.maximum_protection = MINIMM_PROT_READ;
	args.flags = MINIMM_MAP_SHARED | MINIMM_MAP_ANONYMOUS | MINIMM_MAP_FIXED;
	if (!check(minimm_mmap(space, &args, &replacement) == MINIMM_OK,
		   "fixed mmap replaces an existing subrange") ||
	    !check(replacement == address + MINIMM_PAGE_SIZE,
		   "fixed mmap uses the requested address") ||
	    !check(minimm_mapping_query(space, replacement, &info) == MINIMM_OK &&
			   info.protection == MINIMM_PROT_READ && info.flags == MINIMM_MAP_SHARED,
		   "replacement has independent rights and sharing mode") ||
	    !check(minimm_mprotect(space, address, MINIMM_PAGE_SIZE, MINIMM_PROT_READ) == MINIMM_OK,
		   "lower mapping protection") ||
	    !check(minimm_mapping_query(space, address, &info) == MINIMM_OK &&
			   info.protection == MINIMM_PROT_READ,
		   "protection update is visible") ||
	    !check(minimm_page_protect(space, replacement, MINIMM_PROT_READ | MINIMM_PROT_WRITE) ==
			   MINIMM_ERROR_PERMISSION,
		   "protection cannot exceed its ceiling")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	if (!check(minimm_munmap(space, address, MINIMM_PAGE_SIZE * UINT64_C(2)) == MINIMM_OK,
		   "unmap range") ||
	    !check(minimm_mapping_query(space, address, &info) == MINIMM_ERROR_NOT_FOUND,
		   "unmapped address is absent") ||
	    !check(minimm_mapping_query(space, replacement, &info) == MINIMM_ERROR_NOT_FOUND,
		   "replacement is also unmapped")) {
		minimm_space_destroy(space);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(space);
	if (!test_high_hint_does_not_exhaust(mm) || !test_linux_style_default_permissions(mm) ||
	    !test_mapping_state_model(mm) || !test_mapping_rcu_publish(mm) ||
	    !test_mmap_args_output_alias(mm)) {
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}
	minimm_destroy(mm);
	return EXIT_SUCCESS;
}
