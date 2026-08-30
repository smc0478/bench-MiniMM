#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "note.h"

#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool test_temporary_note(void)
{
	static const unsigned char value[] = "a note crossing a page boundary";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	minimm_note_t *read_only = NULL;
	minimm_note_t *write_only = NULL;
	minimm_frame_t *first_frame = NULL;
	minimm_frame_t *same_frame = NULL;
	unsigned char buffer[sizeof(value)] = { 0 };
	unsigned char zeroes[32] = { 0 };
	size_t completed = 0U;
	bool mapping_attached = false;
	const uint64_t offset = MINIMM_PAGE_SIZE - UINT64_C(8);

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE * UINT64_C(2), MINIMM_NOTE_RIGHT_ALL,
				      &note) == MINIMM_OK,
		   "create temporary-file note") ||
	    !check(minimm_note_id(note) != UINT64_C(0), "assign note id") ||
	    !check(minimm_note_size(note) == MINIMM_PAGE_SIZE * UINT64_C(2), "report note size") ||
	    !check(minimm_note_rights(note) == MINIMM_NOTE_RIGHT_ALL, "report note rights") ||
	    !check(minimm_note_pwrite(note, offset, value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(value),
		   "write bytes across page boundary") ||
	    !check(minimm_note_pedit(note, offset, value, sizeof(value), &completed) == MINIMM_OK &&
			   completed == sizeof(value),
		   "edit bytes with explicit edit right") ||
	    !check(minimm_note_pread(note, offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer),
		   "read bytes across page boundary") ||
	    !check(memcmp(value, buffer, sizeof(value)) == 0, "preserve bytes") ||
	    !check(minimm_note_resize(note, MINIMM_PAGE_SIZE * UINT64_C(3)) == MINIMM_OK,
		   "grow note by one page") ||
	    !check(minimm_note_size(note) == MINIMM_PAGE_SIZE * UINT64_C(3),
		   "publish grown size") ||
	    !check(minimm_note_get_frame(note, 0U, &first_frame) == MINIMM_OK &&
			   minimm_note_get_frame(note, 0U, &same_frame) == MINIMM_OK &&
			   first_frame == same_frame,
		   "note page cache returns one shared frame") ||
	    !check(minimm_note_mapping_attach(note) == MINIMM_OK &&
			   (mapping_attached = true,
			    minimm_note_resize(note, MINIMM_PAGE_SIZE * UINT64_C(4)) == MINIMM_OK),
		   "mapped note can grow without invalidating its mapping") ||
	    !check(minimm_note_size(note) == MINIMM_PAGE_SIZE * UINT64_C(4),
		   "publish mapped growth") ||
	    !check(minimm_note_resize(note, MINIMM_PAGE_SIZE * UINT64_C(2)) == MINIMM_ERROR_BUSY,
		   "mapped note cannot shrink without SIGBUS invalidation") ||
	    !check(minimm_note_pread(note, MINIMM_PAGE_SIZE * UINT64_C(2), buffer, sizeof(zeroes),
				     &completed) == MINIMM_OK &&
			   completed == sizeof(zeroes),
		   "read newly allocated sparse bytes") ||
	    !check(memcmp(buffer, zeroes, sizeof(zeroes)) == 0, "grown page is zero filled") ||
	    !check(minimm_note_resize(note, MINIMM_PAGE_SIZE + UINT64_C(1)) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject unaligned resize") ||
	    !check(minimm_note_pwrite(note, MINIMM_PAGE_SIZE * UINT64_C(4) - UINT64_C(1), value, 2U,
				      &completed) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   completed == 0U,
		   "reject write past logical end") ||
	    !check(minimm_note_pread(note, UINT64_MAX - UINT64_C(1), buffer, sizeof(buffer),
				     &completed) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   completed == 0U,
		   "reject overflowing read range") ||
	    !check(minimm_note_flush(note) == MINIMM_OK, "flush temporary note") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_EDIT, &read_only) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "edit right requires write right") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_READ, &read_only) ==
			   MINIMM_OK,
		   "create read-only logical note") ||
	    !check(minimm_note_pwrite(read_only, 0U, value, sizeof(value), &completed) ==
				   MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "enforce write right") ||
	    !check(minimm_note_resize(read_only, MINIMM_PAGE_SIZE * UINT64_C(2)) ==
			   MINIMM_ERROR_PERMISSION,
		   "enforce resize right") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_WRITE, &write_only) ==
			   MINIMM_OK,
		   "create write-only logical note") ||
	    !check(minimm_note_pread(write_only, 0U, buffer, sizeof(buffer), &completed) ==
				   MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "enforce read right") ||
	    !check(minimm_note_pedit(write_only, 0U, value, sizeof(value), &completed) ==
				   MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "plain write right does not grant edit")) {
		if (mapping_attached) {
			minimm_note_mapping_detach(note);
		}
		minimm_frame_release(same_frame);
		minimm_frame_release(first_frame);
		minimm_note_release(write_only);
		minimm_note_release(read_only);
		minimm_note_release(note);
		minimm_destroy(mm);
		return false;
	}

	if (mapping_attached) {
		minimm_note_mapping_detach(note);
	}
	minimm_frame_release(same_frame);
	minimm_frame_release(first_frame);
	minimm_note_retain(note);
	minimm_note_release(note);
	minimm_note_release(write_only);
	minimm_note_release(read_only);
	minimm_note_release(note);
	minimm_destroy(mm);
	return true;
}

static bool test_note_write_completion_alias(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	unsigned char expected[sizeof(size_t)] = { 0 };
	unsigned char observed[sizeof(size_t)] = { 0 };
	size_t aliased = SIZE_MAX - 17U;
	size_t completed = 0U;
	bool passed = false;

	(void)memcpy(expected, &aliased, sizeof(expected));
	if (!check(minimm_create(&config, &mm) == MINIMM_OK,
		   "create completion-alias note system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_ALL, &note) ==
			   MINIMM_OK,
		   "create completion-alias note") ||
	    !check(minimm_note_pwrite(note, UINT64_C(0), &aliased, sizeof(aliased), &aliased) ==
				   MINIMM_OK &&
			   aliased == sizeof(aliased),
		   "allow note write source to alias its completion output") ||
	    !check(minimm_note_pread(note, UINT64_C(0), observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) &&
			   memcmp(observed, expected, sizeof(observed)) == 0,
		   "preserve aliased note write source bytes")) {
		goto done;
	}

	aliased = SIZE_MAX / 3U;
	(void)memcpy(expected, &aliased, sizeof(expected));
	if (!check(minimm_note_pedit(note, UINT64_C(64), &aliased, sizeof(aliased), &aliased) ==
				   MINIMM_OK &&
			   aliased == sizeof(aliased),
		   "allow note edit source to alias its completion output") ||
	    !check(minimm_note_pread(note, UINT64_C(64), observed, sizeof(observed), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(observed) &&
			   memcmp(observed, expected, sizeof(observed)) == 0,
		   "preserve aliased note edit source bytes")) {
		goto done;
	}
	passed = true;

done:
	minimm_note_release(note);
	minimm_destroy(mm);
	return passed;
}

static bool test_note_copy(void)
{
	static const unsigned char original[] = "dirty snapshot spanning a page boundary";
	static const unsigned char replacement[] = "source changes remain independent";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *source = NULL;
	minimm_note_t *read_only_copy = NULL;
	minimm_note_t *writable_copy = NULL;
	minimm_note_t *zero_source = NULL;
	minimm_note_t *zero_copy = NULL;
	minimm_note_t *write_only_source = NULL;
	minimm_note_t *invalid_result = NULL;
	unsigned char buffer[sizeof(original)] = { 0 };
	size_t completed = 0U;
	bool passed = false;
	const uint64_t offset = MINIMM_PAGE_SIZE - UINT64_C(8);
	const minimm_note_rights_t invalid_rights = MINIMM_NOTE_RIGHT_ALL | (UINT32_C(1) << 31);

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create copy test system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE * UINT64_C(2), MINIMM_NOTE_RIGHT_ALL,
				      &source) == MINIMM_OK,
		   "create copy source") ||
	    !check(minimm_note_pwrite(source, offset, original, sizeof(original), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(original),
		   "seed dirty source pages") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_READ, &read_only_copy) == MINIMM_OK,
		   "copy to read-only destination") ||
	    !check(minimm_note_size(read_only_copy) == minimm_note_size(source),
		   "copy preserves size") ||
	    !check(minimm_note_id(read_only_copy) != minimm_note_id(source),
		   "copy receives a new id") ||
	    !check(minimm_note_rights(read_only_copy) == MINIMM_NOTE_RIGHT_READ,
		   "copy receives requested rights") ||
	    !check(minimm_note_belongs_to(read_only_copy, mm), "copy belongs to source system") ||
	    !check(minimm_note_pread(read_only_copy, offset, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, original, sizeof(original)) == 0,
		   "copy captures dirty cached bytes") ||
	    !check(minimm_note_pwrite(read_only_copy, 0U, original, sizeof(original), &completed) ==
				   MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "copy creation bypasses destination rights only internally") ||
	    !check(minimm_note_pwrite(source, offset, replacement, sizeof(replacement),
				      &completed) == MINIMM_OK &&
			   completed == sizeof(replacement),
		   "modify source after copying") ||
	    !check(minimm_note_pread(read_only_copy, offset, buffer, sizeof(original),
				     &completed) == MINIMM_OK &&
			   completed == sizeof(original) &&
			   memcmp(buffer, original, sizeof(original)) == 0,
		   "source changes do not affect copy") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_READ | MINIMM_NOTE_RIGHT_WRITE,
				    &writable_copy) == MINIMM_OK,
		   "create writable copy") ||
	    !check(minimm_note_pwrite(writable_copy, offset, original, sizeof(original),
				      &completed) == MINIMM_OK &&
			   completed == sizeof(original),
		   "modify writable copy") ||
	    !check(minimm_note_pread(source, offset, buffer, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement) &&
			   memcmp(buffer, replacement, sizeof(replacement)) == 0,
		   "copy changes do not affect source") ||
	    !check(minimm_note_create(mm, UINT64_C(0), MINIMM_NOTE_RIGHT_READ, &zero_source) ==
			   MINIMM_OK,
		   "create zero-size source") ||
	    !check(minimm_note_copy(zero_source, MINIMM_NOTE_RIGHT_NONE, &zero_copy) == MINIMM_OK &&
			   minimm_note_size(zero_copy) == UINT64_C(0) &&
			   minimm_note_id(zero_copy) != minimm_note_id(zero_source) &&
			   minimm_note_rights(zero_copy) == MINIMM_NOTE_RIGHT_NONE,
		   "copy zero-size note") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_WRITE,
				      &write_only_source) == MINIMM_OK,
		   "create source without read right")) {
		goto done;
	}

	invalid_result = source;
	if (!check(minimm_note_copy(NULL, MINIMM_NOTE_RIGHT_READ, &invalid_result) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   invalid_result == NULL,
		   "reject null copy source") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_READ, NULL) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject null copy output") ||
	    !check(minimm_note_copy(source, invalid_rights, &invalid_result) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   invalid_result == NULL,
		   "reject unknown copy rights") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_EDIT, &invalid_result) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   invalid_result == NULL,
		   "validate copy destination rights") ||
	    !check(minimm_note_copy(write_only_source, MINIMM_NOTE_RIGHT_READ, &invalid_result) ==
				   MINIMM_ERROR_PERMISSION &&
			   invalid_result == NULL,
		   "copy requires source read right")) {
		goto done;
	}

	passed = true;

done:
	minimm_note_release(write_only_source);
	minimm_note_release(zero_copy);
	minimm_note_release(zero_source);
	minimm_note_release(writable_copy);
	minimm_note_release(read_only_copy);
	minimm_note_release(source);
	minimm_destroy(mm);
	return passed;
}

static bool test_note_copy_is_page_cow(void)
{
	const unsigned char original = UINT8_C(0x31);
	const unsigned char source_value = UINT8_C(0x72);
	const unsigned char copy_value = UINT8_C(0xa4);
	const uint64_t large_sparse_size = UINT64_C(1) << 30;
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *source = NULL;
	minimm_note_t *copy = NULL;
	minimm_note_t *large = NULL;
	minimm_note_t *large_copy = NULL;
	minimm_frame_t *source_frame = NULL;
	minimm_frame_t *copy_frame = NULL;
	minimm_system_stats_t before = { 0 };
	minimm_system_stats_t after = { 0 };
	unsigned char value = UINT8_C(0);
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create page-COW system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_ALL, &source) ==
			   MINIMM_OK,
		   "create page-COW source") ||
	    !check(minimm_note_pwrite(source, 0U, &original, sizeof(original), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(original),
		   "seed one source page") ||
	    !check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "snapshot stats before note copy") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_ALL, &copy) == MINIMM_OK,
		   "create lazy page-COW copy") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.frame_count == before.frame_count &&
			   after.resident_count == before.resident_count &&
			   after.page_in_count == before.page_in_count,
		   "copy creates no page frames or residency events") ||
	    !check(minimm_note_pread(copy, 0U, &value, sizeof(value), &completed) == MINIMM_OK &&
			   value == original,
		   "copy reads through immutable snapshot ancestry") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.frame_count == before.frame_count,
		   "copy read does not materialize an overlay") ||
	    !check(minimm_note_peek_frame(source, 0U, &source_frame) == MINIMM_OK &&
			   minimm_note_peek_frame(copy, 0U, &copy_frame) == MINIMM_OK &&
			   source_frame == copy_frame,
		   "source and copy initially resolve to one cached frame")) {
		goto done;
	}
	minimm_frame_release(copy_frame);
	copy_frame = NULL;
	minimm_frame_release(source_frame);
	source_frame = NULL;

	if (!check(minimm_note_pwrite(source, 0U, &source_value, sizeof(source_value),
				      &completed) == MINIMM_OK,
		   "first source write splits the snapshot page") ||
	    !check(minimm_note_peek_frame(source, 0U, &source_frame) == MINIMM_OK &&
			   minimm_note_peek_frame(copy, 0U, &copy_frame) == MINIMM_OK &&
			   source_frame != copy_frame,
		   "first write produces distinct cached frames") ||
	    !check(minimm_note_pread(copy, 0U, &value, sizeof(value), &completed) == MINIMM_OK &&
			   value == original,
		   "source write preserves copy bytes")) {
		goto done;
	}
	minimm_frame_release(copy_frame);
	copy_frame = NULL;
	minimm_frame_release(source_frame);
	source_frame = NULL;
	if (!check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "snapshot stats before destination write") ||
	    !check(minimm_note_pwrite(copy, 0U, &copy_value, sizeof(copy_value), &completed) ==
			   MINIMM_OK,
		   "write the already split destination page") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.frame_count == before.frame_count,
		   "destination write reuses its private overlay page") ||
	    !check(minimm_note_pread(source, 0U, &value, sizeof(value), &completed) == MINIMM_OK &&
			   value == source_value,
		   "destination write leaves source independent") ||
	    !check(minimm_note_create(mm, large_sparse_size, MINIMM_NOTE_RIGHT_ALL, &large) ==
			   MINIMM_OK,
		   "create a large sparse internal note") ||
	    !check(minimm_system_get_stats(mm, &before) == MINIMM_OK,
		   "snapshot sparse-copy stats") ||
	    !check(minimm_note_copy(large, MINIMM_NOTE_RIGHT_READ, &large_copy) == MINIMM_OK,
		   "copy a large sparse note in constant page work") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.frame_count == before.frame_count &&
			   after.resident_count == before.resident_count &&
			   after.page_in_count == before.page_in_count,
		   "large sparse copy allocates no page frames") ||
	    !check(minimm_note_resize(large, UINT64_C(0)) == MINIMM_OK,
		   "shrink a large sparse snapshot source") ||
	    !check(minimm_system_get_stats(mm, &after) == MINIMM_OK &&
			   after.frame_count == before.frame_count &&
			   after.resident_count == before.resident_count &&
			   after.page_in_count == before.page_in_count,
		   "sparse shrink walks cached pages rather than virtual pages") ||
	    !check(minimm_note_pread(large_copy, large_sparse_size - MINIMM_PAGE_SIZE, &value,
				     sizeof(value), &completed) == MINIMM_OK &&
			   value == UINT8_C(0),
		   "sparse child holes remain zero after source truncation")) {
		goto done;
	}

	passed = true;

done:
	minimm_frame_release(copy_frame);
	minimm_frame_release(source_frame);
	minimm_note_release(large_copy);
	minimm_note_release(large);
	minimm_note_release(copy);
	minimm_note_release(source);
	minimm_destroy(mm);
	return passed;
}

static bool test_note_copy_chain_and_resize(void)
{
	const unsigned char original = UINT8_C(0x5a);
	unsigned char value = UINT8_C(0xff);
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *source = NULL;
	minimm_note_t *middle = NULL;
	minimm_note_t *leaf = NULL;
	size_t completed = 0U;
	bool passed = false;
	const uint64_t third_page = MINIMM_PAGE_SIZE * UINT64_C(2);

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create chain system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE * UINT64_C(3), MINIMM_NOTE_RIGHT_ALL,
				      &source) == MINIMM_OK,
		   "create chain source") ||
	    !check(minimm_note_pwrite(source, third_page, &original, sizeof(original),
				      &completed) == MINIMM_OK,
		   "seed the source tail") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_ALL, &middle) == MINIMM_OK &&
			   minimm_note_copy(middle, MINIMM_NOTE_RIGHT_ALL, &leaf) == MINIMM_OK,
		   "create a two-level COW chain") ||
	    !check(minimm_note_resize(source, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_note_size(middle) == MINIMM_PAGE_SIZE * UINT64_C(3),
		   "source shrink does not truncate its child") ||
	    !check(minimm_note_pread(leaf, third_page, &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   value == original,
		   "source shrink preserves the descendant snapshot") ||
	    !check(minimm_note_resize(source, MINIMM_PAGE_SIZE * UINT64_C(3)) == MINIMM_OK &&
			   minimm_note_pread(source, third_page, &value, sizeof(value),
					     &completed) == MINIMM_OK &&
			   value == UINT8_C(0),
		   "source regrowth is zero-filled") ||
	    !check(minimm_note_resize(middle, MINIMM_PAGE_SIZE) == MINIMM_OK &&
			   minimm_note_size(source) == MINIMM_PAGE_SIZE * UINT64_C(3),
		   "child shrink leaves the source size intact") ||
	    !check(minimm_note_resize(middle, MINIMM_PAGE_SIZE * UINT64_C(3)) == MINIMM_OK &&
			   minimm_note_pread(middle, third_page, &value, sizeof(value),
					     &completed) == MINIMM_OK &&
			   value == UINT8_C(0),
		   "child regrowth does not resurrect truncated ancestor bytes") ||
	    !check(minimm_note_pread(leaf, third_page, &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   value == original,
		   "child resize preserves its leaf snapshot")) {
		goto done;
	}

	minimm_note_release(source);
	source = NULL;
	minimm_note_release(middle);
	middle = NULL;
	if (!check(minimm_note_pread(leaf, third_page, &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   value == original,
		   "leaf remains readable after external ancestor references are released")) {
		goto done;
	}
	passed = true;

done:
	minimm_note_release(leaf);
	minimm_note_release(middle);
	minimm_note_release(source);
	minimm_destroy(mm);
	return passed;
}

static void *release_note_chain(void *argument)
{
	minimm_note_release(argument);
	return NULL;
}

static bool test_deep_note_copy_chain_release(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *leaf = NULL;
	pthread_attr_t attributes;
	pthread_t thread;
	size_t depth = 0U;
	bool attributes_initialized = false;
	bool thread_started = false;
	bool passed = false;
	const size_t chain_depth = 768U;
	const size_t release_stack_size = 64U * 1024U;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create deep-chain system") ||
	    !check(minimm_note_create(mm, UINT64_C(0), MINIMM_NOTE_RIGHT_ALL, &leaf) == MINIMM_OK,
		   "create deep-chain root")) {
		goto done;
	}
	for (depth = 0U; depth < chain_depth; ++depth) {
		minimm_note_t *child = NULL;

		if (!check(minimm_note_copy(leaf, MINIMM_NOTE_RIGHT_ALL, &child) == MINIMM_OK,
			   "extend deep COW chain")) {
			goto done;
		}
		minimm_note_release(leaf);
		leaf = child;
	}
	if (!check(pthread_attr_init(&attributes) == 0, "initialize small-stack attributes")) {
		goto done;
	}
	attributes_initialized = true;
	if (!check(pthread_attr_setstacksize(&attributes, release_stack_size) == 0,
		   "select a bounded release stack") ||
	    !check(pthread_create(&thread, &attributes, release_note_chain, leaf) == 0,
		   "release deep chain on a bounded stack")) {
		goto done;
	}
	thread_started = true;
	leaf = NULL;
	if (!check(pthread_join(thread, NULL) == 0, "join deep-chain releaser")) {
		goto done;
	}
	thread_started = false;
	passed = true;

done:
	if (attributes_initialized) {
		(void)pthread_attr_destroy(&attributes);
	}
	if (thread_started) {
		(void)pthread_join(thread, NULL);
	}
	minimm_note_release(leaf);
	minimm_destroy(mm);
	return passed;
}

static size_t count_open_descriptors(void)
{
	DIR *directory = opendir("/proc/self/fd");
	struct dirent *entry = NULL;
	size_t count = 0U;

	if (directory == NULL) {
		return SIZE_MAX;
	}
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
			count += 1U;
		}
	}
	(void)closedir(directory);
	return count;
}

static bool test_note_pages_share_one_descriptor(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	unsigned char value = UINT8_C(0);
	size_t completed = 0U;
	size_t before = SIZE_MAX;
	size_t after = SIZE_MAX;
	uint64_t page = UINT64_C(0);
	bool passed = false;
	const uint64_t page_count = UINT64_C(128);

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create fd-sharing system") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE * page_count, MINIMM_NOTE_RIGHT_ALL,
				      &note) == MINIMM_OK,
		   "create multi-page note")) {
		goto done;
	}
	before = count_open_descriptors();
	for (page = UINT64_C(0); page < page_count; ++page) {
		if (!check(minimm_note_pread(note, page * MINIMM_PAGE_SIZE, &value, sizeof(value),
					     &completed) == MINIMM_OK,
			   "cache another file-backed note page")) {
			goto done;
		}
	}
	after = count_open_descriptors();
	if (!check(before != SIZE_MAX && after != SIZE_MAX && after <= before + 1U,
		   "cached pages retain one shared file descriptor")) {
		goto done;
	}
	passed = true;

done:
	minimm_note_release(note);
	minimm_destroy(mm);
	return passed;
}

static bool test_external_note_copy_is_stable_after_copy(void)
{
	const unsigned char original = UINT8_C(0x29);
	const unsigned char external_update = UINT8_C(0xe1);
	char path[] = "/tmp/minimm-note-copy-external-XXXXXX";
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *source = NULL;
	minimm_note_t *copy = NULL;
	minimm_frame_t *copied_frame = NULL;
	unsigned char value = UINT8_C(0);
	size_t completed = 0U;
	int fd = -1;
	bool passed = false;
	const uint64_t offset = MINIMM_PAGE_SIZE;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	fd = mkstemp(path);
	if (!check(fd >= 0, "create external snapshot file") ||
	    !check(ftruncate(fd, (off_t)(MINIMM_PAGE_SIZE * UINT64_C(2))) == 0,
		   "size external snapshot file") ||
	    !check(pwrite(fd, &original, sizeof(original), (off_t)offset) ==
			   (ssize_t)sizeof(original),
		   "seed an uncached external page") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create external snapshot system") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &source) == MINIMM_OK,
		   "open external snapshot note") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_READ, &copy) == MINIMM_OK,
		   "copy external note with eager fallback") ||
	    !check(minimm_note_peek_frame(copy, offset, &copied_frame) == MINIMM_OK,
		   "external fallback materializes every destination page") ||
	    !check(pwrite(fd, &external_update, sizeof(external_update), (off_t)offset) ==
			   (ssize_t)sizeof(external_update),
		   "modify the caller-owned file after copying") ||
	    !check(minimm_note_pread(copy, offset, &value, sizeof(value), &completed) ==
				   MINIMM_OK &&
			   value == original,
		   "external mutation cannot change the copied snapshot")) {
		goto done;
	}
	passed = true;

done:
	minimm_frame_release(copied_frame);
	minimm_note_release(copy);
	minimm_note_release(source);
	minimm_destroy(mm);
	if (fd >= 0) {
		(void)close(fd);
	}
	(void)unlink(path);
	return passed;
}

static bool test_note_copy_rejects_mapped_source(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *space = NULL;
	minimm_note_t *source = NULL;
	minimm_note_t *copy = source;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
	minimm_mmap_args_t args = {
		.address_hint = MINIMM_ADDRESS_AUTO,
		.length = MINIMM_PAGE_SIZE,
		.note_offset = UINT64_C(0),
		.protection = MINIMM_PROT_READ,
		.maximum_protection = MINIMM_PROT_READ,
		.flags = MINIMM_MAP_PRIVATE,
	};
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create mapped copy system") ||
	    !check(minimm_space_create(mm, &space) == MINIMM_OK, "create mapped copy space") ||
	    !check(minimm_note_create(mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_ALL, &source) ==
			   MINIMM_OK,
		   "create mapped copy source")) {
		goto done;
	}
	args.note = source;
	if (!check(minimm_mmap(space, &args, &address) == MINIMM_OK, "map the copy source") ||
	    !check(minimm_note_copy(source, MINIMM_NOTE_RIGHT_READ, &copy) == MINIMM_ERROR_BUSY &&
			   copy == NULL,
		   "reject a note copy while a mapping is attached")) {
		goto done;
	}
	passed = true;

done:
	minimm_note_release(copy);
	minimm_space_destroy(space);
	minimm_note_release(source);
	minimm_destroy(mm);
	return passed;
}

static bool test_open_fd(void)
{
	static const char initial[] = "external file contents";
	static const char append_replacement[] = "append flag isolated";
	static const char replacement[] = "updated through note";
	char path[] = "/tmp/minimm-note-test-XXXXXX";
	char buffer[sizeof(initial)] = { 0 };
	struct stat backing_information = { 0 };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	minimm_note_t *read_note = NULL;
	minimm_note_t *append_note = NULL;
	int fd = -1;
	int read_fd = -1;
	int append_fd = -1;
	int caller_status_flags = 0;
	size_t completed = 0U;
	uint64_t first_id = UINT64_C(0);

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	fd = mkstemp(path);
	if (!check(fd >= 0, "create external backing file") ||
	    !check(ftruncate(fd, (off_t)(MINIMM_PAGE_SIZE + UINT64_C(1))) == 0,
		   "make external file unaligned") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &note) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject unaligned external file") ||
	    !check(ftruncate(fd, (off_t)MINIMM_PAGE_SIZE) == 0, "align external file") ||
	    !check(pwrite(fd, initial, sizeof(initial), (off_t)0) == (ssize_t)sizeof(initial),
		   "seed external file")) {
		minimm_note_release(note);
		minimm_destroy(mm);
		if (fd >= 0) {
			(void)close(fd);
		}
		(void)unlink(path);
		return false;
	}

	read_fd = open(path, O_RDONLY);
	if (!check(read_fd >= 0, "open read-only external descriptor") ||
	    !check(minimm_note_open_fd(mm, read_fd, MINIMM_NOTE_RIGHT_WRITE, &read_note) ==
			   MINIMM_ERROR_PERMISSION,
		   "reject write right on read-only descriptor") ||
	    !check(minimm_note_open_fd(mm, read_fd, MINIMM_NOTE_RIGHT_READ, &read_note) ==
			   MINIMM_OK,
		   "open read-only note")) {
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		if (read_fd >= 0) {
			(void)close(read_fd);
		}
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	append_fd = open(path, O_RDWR | O_APPEND);
	if (!check(append_fd >= 0, "open append-mode external descriptor") ||
	    !check(minimm_note_open_fd(mm, append_fd, MINIMM_NOTE_RIGHT_WRITE, &append_note) ==
				   MINIMM_ERROR_PERMISSION &&
			   append_note == NULL,
		   "reject writable note access through an append-mode descriptor")) {
		minimm_note_release(append_note);
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		if (append_fd >= 0) {
			(void)close(append_fd);
		}
		(void)close(read_fd);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	(void)close(append_fd);
	append_fd = -1;
#ifdef O_PATH
	append_fd = open(path, O_PATH | O_CLOEXEC);
	if (!check(append_fd >= 0, "open path-only external descriptor") ||
	    !check(minimm_note_open_fd(mm, append_fd, MINIMM_NOTE_RIGHT_READ, &append_note) ==
				   MINIMM_ERROR_PERMISSION &&
			   append_note == NULL,
		   "reject a path-only descriptor before deferred page-in")) {
		minimm_note_release(append_note);
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		if (append_fd >= 0) {
			(void)close(append_fd);
		}
		(void)close(read_fd);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	(void)close(append_fd);
	append_fd = -1;
#endif
#ifdef __linux__
	append_fd = open(path, O_ACCMODE | O_CLOEXEC);
	if (!check(append_fd >= 0, "open Linux mode-3 external descriptor") ||
	    !check(minimm_note_open_fd(mm, append_fd, MINIMM_NOTE_RIGHT_READ, &append_note) ==
				   MINIMM_ERROR_PERMISSION &&
			   append_note == NULL,
		   "reject a descriptor that cannot perform regular file I/O")) {
		minimm_note_release(append_note);
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		if (append_fd >= 0) {
			(void)close(append_fd);
		}
		(void)close(read_fd);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	(void)close(append_fd);
	append_fd = -1;
#endif
	(void)close(read_fd);
	read_fd = -1;
	if (!check(minimm_note_pread(read_note, 0U, buffer, sizeof(buffer), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(buffer),
		   "duplicated read descriptor survives caller close") ||
	    !check(memcmp(buffer, initial, sizeof(initial)) == 0, "read seed") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &note) == MINIMM_OK,
		   "open read-write note")) {
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	first_id = minimm_note_id(note);
	minimm_note_release(note);
	note = NULL;
	if (!check(fcntl(fd, F_GETFD) >= 0, "note release preserves caller fd") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &note) == MINIMM_OK,
		   "reopen external note") ||
	    !check(minimm_note_id(note) != first_id, "assign unique note id") ||
	    !check(lseek(fd, (off_t)123, SEEK_SET) == (off_t)123, "set fd offset") ||
	    !check(minimm_note_pread(note, 0U, buffer, sizeof(buffer), &completed) == MINIMM_OK,
		   "read through duplicated descriptor") ||
	    !check(lseek(fd, (off_t)0, SEEK_CUR) == (off_t)123,
		   "positioned note I/O leaves caller offset unchanged")) {
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}
	caller_status_flags = fcntl(fd, F_GETFL);
	if (!check(caller_status_flags >= 0, "read caller descriptor status flags") ||
	    !check(fcntl(fd, F_SETFL, caller_status_flags | O_APPEND) == 0,
		   "enable append mode after opening note") ||
	    !check(minimm_note_pwrite(note, 0U, append_replacement, sizeof(append_replacement),
				      &completed) == MINIMM_OK &&
			   completed == sizeof(append_replacement),
		   "write after caller enables append mode") ||
	    !check(minimm_note_flush(note) == MINIMM_OK,
		   "flush after caller enables append mode") ||
	    !check(fstat(fd, &backing_information) == 0 &&
			   backing_information.st_size == (off_t)MINIMM_PAGE_SIZE,
		   "positioned writeback does not append a page") ||
	    !check((fcntl(fd, F_GETFL) & O_APPEND) != 0,
		   "note does not clear append mode on caller descriptor") ||
	    !check(pread(fd, buffer, sizeof(append_replacement), (off_t)0) ==
				   (ssize_t)sizeof(append_replacement) &&
			   memcmp(buffer, append_replacement, sizeof(append_replacement)) == 0,
		   "writeback updates the requested offset")) {
		minimm_note_release(read_note);
		minimm_note_release(note);
		minimm_destroy(mm);
		(void)close(fd);
		(void)unlink(path);
		return false;
	}

	minimm_note_release(read_note);
	read_note = NULL;
	(void)close(fd);
	fd = -1;
	(void)unlink(path);
	minimm_destroy(mm);
	mm = NULL;

	if (!check(minimm_note_pwrite(note, 0U, replacement, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement),
		   "note survives caller fd and system owner close") ||
	    !check(minimm_note_flush(note) == MINIMM_OK, "flush external note") ||
	    !check(minimm_note_pread(note, 0U, buffer, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement),
		   "read updated external note") ||
	    !check(memcmp(buffer, replacement, sizeof(replacement)) == 0,
		   "preserve external update")) {
		minimm_note_release(note);
		return false;
	}

	minimm_note_release(note);
	return true;
}

static bool test_open_unlinked_fd(void)
{
	static const char replacement[] = "unlinked external note";
	char path[] = "/tmp/minimm-note-unlinked-XXXXXX";
	char buffer[sizeof(replacement)] = { 0 };
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_note_t *note = NULL;
	int fd = -1;
	size_t completed = 0U;
	bool passed = false;

	config.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;
	fd = mkstemp(path);
	if (!check(fd >= 0, "create external file for unlink test") ||
	    !check(ftruncate(fd, (off_t)MINIMM_PAGE_SIZE) == 0,
		   "size external file for unlink test") ||
	    !check(unlink(path) == 0, "unlink external file before note open") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK,
		   "create system for unlinked external note") ||
	    !check(minimm_note_open_fd(mm, fd, MINIMM_NOTE_RIGHT_ALL, &note) == MINIMM_OK,
		   "open writable note after external file is unlinked")) {
		goto done;
	}
	(void)close(fd);
	fd = -1;
	if (!check(minimm_note_pwrite(note, 0U, replacement, sizeof(replacement), &completed) ==
				   MINIMM_OK &&
			   completed == sizeof(replacement),
		   "write unlinked external note") ||
	    !check(minimm_note_flush(note) == MINIMM_OK, "flush unlinked external note") ||
	    !check(minimm_note_pread(note, 0U, buffer, sizeof(buffer), &completed) == MINIMM_OK &&
			   completed == sizeof(buffer) &&
			   memcmp(buffer, replacement, sizeof(buffer)) == 0,
		   "read unlinked external note")) {
		goto done;
	}
	passed = true;

done:
	if (fd >= 0) {
		(void)close(fd);
	}
	(void)unlink(path);
	minimm_note_release(note);
	minimm_destroy(mm);
	return passed;
}

int main(void)
{
	return test_temporary_note() && test_note_write_completion_alias() && test_note_copy() &&
			       test_note_copy_is_page_cow() && test_note_copy_chain_and_resize() &&
			       test_deep_note_copy_chain_release() &&
			       test_note_pages_share_one_descriptor() &&
			       test_external_note_copy_is_stable_after_copy() &&
			       test_note_copy_rejects_mapped_source() && test_open_fd() &&
			       test_open_unlinked_fd() ?
		       EXIT_SUCCESS :
		       EXIT_FAILURE;
}
