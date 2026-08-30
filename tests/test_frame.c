#include "frame.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool test_descriptor_exhaustion_status(int source_fd)
{
	struct rlimit original_limit = { 0 };
	struct rlimit exhausted_limit = { 0 };
	minimm_file_backing_t *backing = NULL;
	minimm_status_t status = MINIMM_OK;
	bool output_was_null = false;
	int descriptor_zero_guard = -1;
	int restore_status = -1;

	if (fcntl(STDIN_FILENO, F_GETFD) < 0 && errno == EBADF) {
		descriptor_zero_guard = dup(source_fd);
		if (descriptor_zero_guard != STDIN_FILENO) {
			if (descriptor_zero_guard >= 0) {
				(void)close(descriptor_zero_guard);
			}
			return false;
		}
	}
	if (getrlimit(RLIMIT_NOFILE, &original_limit) != 0 || original_limit.rlim_max < 1U) {
		if (descriptor_zero_guard >= 0) {
			(void)close(descriptor_zero_guard);
		}
		return false;
	}
	exhausted_limit = original_limit;
	exhausted_limit.rlim_cur = 1U;
	if (setrlimit(RLIMIT_NOFILE, &exhausted_limit) != 0) {
		if (descriptor_zero_guard >= 0) {
			(void)close(descriptor_zero_guard);
		}
		return false;
	}
	status = minimm_file_backing_create(source_fd, &backing);
	restore_status = setrlimit(RLIMIT_NOFILE, &original_limit);
	output_was_null = backing == NULL;
	minimm_file_backing_release(backing);
	if (descriptor_zero_guard >= 0) {
		(void)close(descriptor_zero_guard);
	}
	return restore_status == 0 && status == MINIMM_ERROR_NO_SPACE && output_was_null;
}

static bool test_transient_resident_pin(minimm_frame_t *frame)
{
	bool page_out_was_blocked = false;

	if (!check(minimm_frame_try_pin_resident(frame), "transiently pin a resident frame")) {
		return false;
	}
	page_out_was_blocked = check(minimm_frame_page_out(frame) == MINIMM_ERROR_BUSY,
				     "transient resident pin blocks page-out");
	minimm_frame_unpin_resident(frame);
	return page_out_was_blocked &&
	       check(minimm_frame_page_out(frame) == MINIMM_OK, "unpinning restores page-out");
}

typedef struct page_in_context {
	minimm_frame_t *frame;
	minimm_status_t status;
	bool paged_in;
} page_in_context_t;

static void *run_page_in(void *opaque)
{
	page_in_context_t *context = opaque;

	context->status = minimm_frame_ensure_resident(context->frame, &context->paged_in);
	return NULL;
}

static bool wait_for_transient_waiters(minimm_frame_store_t *store, size_t expected)
{
	minimm_frame_store_stats_t stats = { 0 };
	size_t attempt = 0U;

	for (attempt = 0U; attempt < 1000000U; ++attempt) {
		minimm_frame_store_get_stats(store, &stats);
		if (stats.transient_waiter_count == expected) {
			return true;
		}
		(void)sched_yield();
	}
	return false;
}

static bool test_concurrent_same_frame_page_in(void)
{
	const unsigned char blocker_value = UINT8_C(0x5a);
	minimm_frame_store_t *store = NULL;
	minimm_frame_t *blocker = NULL;
	minimm_frame_t *target = NULL;
	minimm_frame_store_stats_t stats = { 0 };
	page_in_context_t first_context = { 0 };
	page_in_context_t second_context = { 0 };
	pthread_t first_thread;
	pthread_t second_thread;
	unsigned char observed = UINT8_C(0xff);
	bool blocker_pinned = false;
	bool first_started = false;
	bool second_started = false;
	bool passed = false;

	if (!check(minimm_frame_store_create(1U, &store) == MINIMM_OK,
		   "create concurrent page-in store") ||
	    !check(minimm_frame_create_zero(store, &blocker) == MINIMM_OK &&
			   minimm_frame_create_zero(store, &target) == MINIMM_OK,
		   "create concurrent page-in frames") ||
	    !check(minimm_frame_write(blocker, 0U, &blocker_value, sizeof(blocker_value)) ==
			   MINIMM_OK,
		   "make concurrent page-in blocker resident") ||
	    !check(minimm_frame_try_pin_resident(blocker), "pin concurrent page-in blocker")) {
		goto done;
	}
	blocker_pinned = true;
	first_context.frame = target;
	second_context.frame = target;
	if (!check(pthread_create(&first_thread, NULL, run_page_in, &first_context) == 0,
		   "start first same-frame page-in")) {
		goto done;
	}
	first_started = true;
	if (!check(pthread_create(&second_thread, NULL, run_page_in, &second_context) == 0,
		   "start second same-frame page-in")) {
		goto done;
	}
	second_started = true;
	if (!check(wait_for_transient_waiters(store, 2U),
		   "both same-frame page-ins wait behind the transient pin")) {
		goto done;
	}

	minimm_frame_unpin_resident(blocker);
	blocker_pinned = false;
	(void)pthread_join(first_thread, NULL);
	first_started = false;
	(void)pthread_join(second_thread, NULL);
	second_started = false;
	minimm_frame_store_get_stats(store, &stats);
	if (!check(first_context.status == MINIMM_OK && second_context.status == MINIMM_OK &&
			   first_context.paged_in != second_context.paged_in,
		   "same-frame waiters share exactly one successful page-in") ||
	    !check(stats.resident_count == 1U && stats.page_in_count == 2U &&
			   stats.page_out_count == 1U && stats.transient_waiter_count == 0U,
		   "same-frame page-in preserves arena accounting") ||
	    !check(minimm_frame_read(target, 0U, &observed, sizeof(observed)) == MINIMM_OK &&
			   observed == UINT8_C(0),
		   "same-frame page-in preserves target data")) {
		goto done;
	}
	passed = true;

done:
	if (blocker_pinned) {
		minimm_frame_unpin_resident(blocker);
	}
	if (first_started) {
		(void)pthread_join(first_thread, NULL);
	}
	if (second_started) {
		(void)pthread_join(second_thread, NULL);
	}
	minimm_frame_release(target);
	minimm_frame_release(blocker);
	minimm_frame_store_destroy(store);
	return passed;
}

int main(void)
{
	static const char first_value[] = "first page";
	static const char second_value[] = "second page";
	char buffer[sizeof(first_value)] = { 0 };
	minimm_frame_store_t *store = NULL;
	minimm_frame_t *first = NULL;
	minimm_frame_t *second = NULL;
	minimm_frame_store_stats_t stats = { 0 };
	minimm_frame_state_t state = { 0 };
	bool paged_in = false;
	minimm_frame_store_t *deferred_store = NULL;
	minimm_frame_t *surviving_frame = NULL;
	minimm_frame_store_t *arena_store = NULL;
	minimm_frame_t *arena_first = NULL;
	minimm_frame_t *arena_second = NULL;
	minimm_frame_store_t *file_store = NULL;
	minimm_frame_t *file_frame = NULL;
	minimm_file_backing_t *read_only_backing = NULL;
	char file_path[] = "/tmp/minimm-frame-test-XXXXXX";
	char error_path[] = "/tmp/minimm-frame-error-XXXXXX";
	unsigned char arena_value = UINT8_C(0);
	const unsigned char arena_first_value = UINT8_C(0x31);
	const unsigned char arena_second_value = UINT8_C(0x72);
	int file_fd = -1;
	int error_fd = -1;

	if (!check(minimm_frame_store_create(SIZE_MAX, &store) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   store == NULL,
		   "reject an overflowing resident arena") ||
	    !check(minimm_frame_store_create(1U, &store) == MINIMM_OK,
		   "create one-page resident store") ||
	    !check(minimm_frame_create_zero(store, &first) == MINIMM_OK, "create first frame")) {
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	if (!check(minimm_frame_read(first, MINIMM_PAGE_SIZE, NULL, 0U) == MINIMM_OK &&
			   minimm_frame_write(first, MINIMM_PAGE_SIZE, NULL, 0U) == MINIMM_OK,
		   "zero-length frame I/O is a no-op at the page end")) {
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_frame_store_get_stats(store, &stats);
	if (!check(!minimm_frame_is_resident(first) && !minimm_frame_is_dirty(first),
		   "zero-length frame I/O does not instantiate or dirty a page") ||
	    !check(stats.resident_count == 0U && stats.page_in_count == 0U,
		   "zero-length frame I/O leaves residency counters unchanged") ||
	    !check(minimm_frame_read(first, MINIMM_PAGE_SIZE + 1U, NULL, 0U) ==
			   MINIMM_ERROR_INVALID_ARGUMENT,
		   "zero-length frame I/O still validates its offset") ||
	    !check(minimm_frame_write(first, 0U, first_value, sizeof(first_value)) == MINIMM_OK,
		   "write first frame") ||
	    !check(minimm_frame_create_zero(store, &second) == MINIMM_OK, "create second frame") ||
	    !check(minimm_frame_write(second, 0U, second_value, sizeof(second_value)) == MINIMM_OK,
		   "write second frame and evict first") ||
	    !check(!minimm_frame_is_resident(first), "first frame was paged out") ||
	    !check(minimm_frame_ensure_resident(first, &paged_in) == MINIMM_OK && paged_in,
		   "atomically report a page-in") ||
	    !(minimm_frame_get_state(first, &state),
	      check(state.resident && !state.dirty, "snapshot resident state")) ||
	    !check(minimm_frame_read(first, 0U, buffer, sizeof(buffer)) == MINIMM_OK,
		   "page first frame back in") ||
	    !check(memcmp(buffer, first_value, sizeof(first_value)) == 0,
		   "temporary-file backing preserves bytes")) {
		minimm_frame_release(second);
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	minimm_frame_store_get_stats(store, &stats);
	if (!check(stats.frame_count == 2U, "store tracks frames") ||
	    !check(stats.resident_count == 1U, "resident limit is enforced") ||
	    !check(stats.page_in_count == 3U, "page-in events are counted") ||
	    !check(stats.page_out_count == 2U, "page-out events are counted") ||
	    !check(stats.swap_slot_count == 2U && stats.swap_slot_high_water == 2U,
		   "anonymous frames own bounded swap slots")) {
		minimm_frame_release(second);
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}

	minimm_frame_release(second);
	second = NULL;
	minimm_frame_release(first);
	first = NULL;
	minimm_frame_store_get_stats(store, &stats);
	if (!check(stats.frame_count == 0U, "released frames leave the store") ||
	    !check(stats.swap_slot_count == 0U && stats.swap_slot_high_water == 2U,
		   "released frames return swap slots to the store") ||
	    !check(minimm_frame_create_zero(store, &first) == MINIMM_OK &&
			   minimm_frame_write(first, 0U, first_value, sizeof(first_value)) ==
				   MINIMM_OK &&
			   minimm_frame_create_zero(store, &second) == MINIMM_OK &&
			   minimm_frame_write(second, 0U, second_value, sizeof(second_value)) ==
				   MINIMM_OK,
		   "create a second swap-slot generation")) {
		minimm_frame_release(second);
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_frame_store_get_stats(store, &stats);
	if (!check(stats.swap_slot_count == 1U && stats.swap_slot_high_water == 2U,
		   "swap-slot reuse does not grow the high-water mark")) {
		minimm_frame_release(second);
		minimm_frame_release(first);
		minimm_frame_store_destroy(store);
		return EXIT_FAILURE;
	}
	minimm_frame_release(second);
	second = NULL;
	minimm_frame_release(first);
	first = NULL;

	minimm_frame_store_destroy(store);
	store = NULL;

	if (!check(minimm_frame_store_create(2U, &arena_store) == MINIMM_OK,
		   "create two-slot mmap arena") ||
	    !check(minimm_frame_create_zero(arena_store, &arena_first) == MINIMM_OK,
		   "create first arena frame") ||
	    !check(minimm_frame_create_zero(arena_store, &arena_second) == MINIMM_OK,
		   "create second arena frame") ||
	    !check(minimm_frame_write(arena_first, 0U, &arena_first_value,
				      sizeof(arena_first_value)) == MINIMM_OK,
		   "write first arena slot") ||
	    !check(minimm_frame_write(arena_second, 0U, &arena_second_value,
				      sizeof(arena_second_value)) == MINIMM_OK,
		   "write second arena slot") ||
	    !check(minimm_frame_read(arena_first, 0U, &arena_value, sizeof(arena_value)) ==
				   MINIMM_OK &&
			   arena_value == arena_first_value,
		   "resident arena slots do not alias")) {
		minimm_frame_release(arena_second);
		minimm_frame_release(arena_first);
		minimm_frame_store_destroy(arena_store);
		return EXIT_FAILURE;
	}
	minimm_frame_store_get_stats(arena_store, &stats);
	if (!check(stats.resident_count == 2U, "both mmap arena slots are resident") ||
	    !test_transient_resident_pin(arena_first)) {
		minimm_frame_release(arena_second);
		minimm_frame_release(arena_first);
		minimm_frame_store_destroy(arena_store);
		return EXIT_FAILURE;
	}
	minimm_frame_release(arena_second);
	minimm_frame_release(arena_first);
	minimm_frame_store_destroy(arena_store);
	if (!test_concurrent_same_frame_page_in()) {
		return EXIT_FAILURE;
	}

	if (!check(minimm_frame_store_create(1U, &deferred_store) == MINIMM_OK,
		   "create store for deferred destruction") ||
	    !check(minimm_frame_create_zero(deferred_store, &surviving_frame) == MINIMM_OK,
		   "create frame that outlives its store owner") ||
	    !check(minimm_frame_write(surviving_frame, 0U, first_value, sizeof(first_value)) ==
			   MINIMM_OK,
		   "write surviving frame")) {
		minimm_frame_release(surviving_frame);
		minimm_frame_store_destroy(deferred_store);
		return EXIT_FAILURE;
	}

	minimm_frame_store_destroy(deferred_store);
	(void)memset(buffer, 0, sizeof(buffer));
	if (!check(minimm_frame_read(surviving_frame, 0U, buffer, sizeof(buffer)) == MINIMM_OK,
		   "frame remains usable until its last reference is released") ||
	    !check(memcmp(buffer, first_value, sizeof(first_value)) == 0,
		   "deferred store destruction preserves frame data")) {
		minimm_frame_release(surviving_frame);
		return EXIT_FAILURE;
	}
	minimm_frame_release(surviving_frame);

	file_fd = mkstemp(file_path);
	if (!check(file_fd >= 0, "create shared-frame test file")) {
		return EXIT_FAILURE;
	}
	(void)unlink(file_path);
	if (!check(ftruncate(file_fd, (off_t)MINIMM_PAGE_SIZE) == 0,
		   "size shared-frame test file") ||
	    !check(minimm_frame_store_create(1U, &file_store) == MINIMM_OK,
		   "create file-backed frame store") ||
	    !check(minimm_frame_create_file(file_store, file_fd, 0U, true, &file_frame) ==
			   MINIMM_OK,
		   "create shared file frame") ||
	    !check(minimm_frame_write(file_frame, 0U, first_value, sizeof(first_value)) ==
			   MINIMM_OK,
		   "dirty shared file frame")) {
		minimm_frame_release(file_frame);
		minimm_frame_store_destroy(file_store);
		(void)close(file_fd);
		return EXIT_FAILURE;
	}

	minimm_frame_release(file_frame);
	file_frame = NULL;
	(void)memset(buffer, 0, sizeof(buffer));
	if (!check(pread(file_fd, buffer, sizeof(buffer), (off_t)0) == (ssize_t)sizeof(buffer),
		   "read shared frame backing file") ||
	    !check(memcmp(buffer, first_value, sizeof(first_value)) == 0,
		   "last shared-frame release writes dirty data back")) {
		minimm_frame_store_destroy(file_store);
		(void)close(file_fd);
		return EXIT_FAILURE;
	}
	minimm_frame_store_destroy(file_store);
	(void)close(file_fd);

	error_fd = mkstemp(error_path);
	if (!check(error_fd >= 0, "create writeback-error file") ||
	    !check(ftruncate(error_fd, (off_t)MINIMM_PAGE_SIZE) == 0,
		   "size writeback-error file")) {
		if (error_fd >= 0) {
			(void)close(error_fd);
		}
		(void)unlink(error_path);
		return EXIT_FAILURE;
	}
	(void)close(error_fd);
	error_fd = open(error_path, O_RDONLY);
	(void)unlink(error_path);
	if (!check(error_fd >= 0, "reopen backing without write permission") ||
	    !check(minimm_frame_store_create(1U, &file_store) == MINIMM_OK,
		   "create writeback-error store") ||
	    !check(minimm_file_backing_create(error_fd, &read_only_backing) == MINIMM_OK,
		   "create shared read-only backing") ||
	    !check(minimm_frame_create_file_backing(file_store, read_only_backing, 0U, true,
						    &file_frame) == MINIMM_OK,
		   "create frame on read-only backing") ||
	    !check(minimm_frame_write(file_frame, 0U, first_value, sizeof(first_value)) ==
			   MINIMM_OK,
		   "dirty the read-only-backed frame") ||
	    !check(minimm_frame_sync(file_frame) == MINIMM_ERROR_IO,
		   "surface the first writeback failure") ||
	    !check(minimm_file_backing_sync(read_only_backing) == MINIMM_ERROR_IO,
		   "preserve the writeback failure on the shared backing")) {
		minimm_frame_release(file_frame);
		minimm_file_backing_release(read_only_backing);
		minimm_frame_store_destroy(file_store);
		if (error_fd >= 0) {
			(void)close(error_fd);
		}
		return EXIT_FAILURE;
	}
	minimm_frame_release(file_frame);
	minimm_file_backing_release(read_only_backing);
	minimm_frame_store_destroy(file_store);
	if (!check(test_descriptor_exhaustion_status(error_fd),
		   "descriptor exhaustion is reported as no space without leaking a backing")) {
		(void)close(error_fd);
		return EXIT_FAILURE;
	}
	(void)close(error_fd);
	return EXIT_SUCCESS;
}
