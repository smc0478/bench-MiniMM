#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "note.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct minimm_note_page {
	struct minimm_note_page *next;
	minimm_frame_t *frame;
	uint64_t page_offset;
} minimm_note_page_t;

typedef struct minimm_note_lineage {
	pthread_mutex_t lock;
	atomic_size_t references;
} minimm_note_lineage_t;

struct minimm_note {
	minimm_t *system;
	minimm_note_lineage_t *lineage;
	atomic_size_t references;
	atomic_uint_fast64_t size;
	uint64_t id;
	minimm_note_rights_t rights;
	minimm_file_backing_t *file_backing;
	size_t active_mappings;
	minimm_note_page_t *pages;
	minimm_note_t *cow_parent;
	minimm_note_t *first_child;
	minimm_note_t *next_sibling;
	uint64_t inherited_size;
	bool external_backing;
};

static atomic_uint_fast64_t minimm_next_note_id = ATOMIC_VAR_INIT(UINT64_C(1));

static minimm_status_t minimm_note_status_from_errno(int error_number)
{
	switch (error_number) {
	case ENOMEM:
		return MINIMM_ERROR_OUT_OF_MEMORY;
	case EACCES:
	case EPERM:
		return MINIMM_ERROR_PERMISSION;
	case EDQUOT:
	case EFBIG:
	case EMFILE:
	case ENFILE:
	case ENOSPC:
		return MINIMM_ERROR_NO_SPACE;
	case EBADF:
	case EINVAL:
		return MINIMM_ERROR_INVALID_ARGUMENT;
	default:
		return MINIMM_ERROR_IO;
	}
}

static minimm_status_t minimm_note_status_from_pthread(int error_number)
{
	if (error_number == ENOMEM) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (error_number == EAGAIN) {
		return MINIMM_ERROR_BUSY;
	}
	return MINIMM_ERROR_IO;
}

#if !defined(F_DUPFD_CLOEXEC) || !defined(O_CLOEXEC)
static int minimm_note_set_cloexec(int fd)
{
	int result = 0;

	do {
		result = fcntl(fd, F_SETFD, FD_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	return result;
}
#endif

static int minimm_note_unlink_retry(const char *path)
{
	int result = 0;

	do {
		result = unlink(path);
	} while (result != 0 && errno == EINTR);
	return result;
}

static int minimm_note_duplicate_fd(int fd)
{
#ifdef F_DUPFD_CLOEXEC
	int duplicate = -1;

	do {
		duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	} while (duplicate < 0 && errno == EINTR);
	return duplicate;
#else
	int duplicate = -1;

	do {
		duplicate = dup(fd);
	} while (duplicate < 0 && errno == EINTR);

	if (duplicate >= 0 && minimm_note_set_cloexec(duplicate) < 0) {
		const int error_number = errno;

		(void)close(duplicate);
		errno = error_number;
		return -1;
	}
	return duplicate;
#endif
}

#ifdef __linux__
/*
 * dup(2) and F_DUPFD_CLOEXEC retain the caller's open file description, so a
 * later F_SETFL(O_APPEND) on the caller descriptor would also affect MiniMM's
 * positioned writes. Linux has no fcntl operation that clones a descriptor
 * into a new open file description. Reopening the procfs descriptor link does
 * provide one; keep the original duplicate open so its descriptor number and
 * inode cannot be recycled while resolving the link.
 */
static int minimm_note_reopen_independent(int fd, const struct stat *expected,
					  struct stat *out_information)
{
	char descriptor_path[64];
	struct stat information = { 0 };
	int flags = O_RDWR;
	int length = 0;
	int reopened = -1;

	length = snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
	if (length < 0 || (size_t)length >= sizeof(descriptor_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
	do {
		reopened = open(descriptor_path, flags);
	} while (reopened < 0 && errno == EINTR);
	if (reopened < 0) {
		return -1;
	}
#ifndef O_CLOEXEC
	if (minimm_note_set_cloexec(reopened) < 0) {
		const int error_number = errno;

		(void)close(reopened);
		errno = error_number;
		return -1;
	}
#endif
	while (fstat(reopened, &information) != 0) {
		const int error_number = errno;

		if (error_number == EINTR) {
			continue;
		}
		(void)close(reopened);
		errno = error_number;
		return -1;
	}
	if (!S_ISREG(information.st_mode) || information.st_dev != expected->st_dev ||
	    information.st_ino != expected->st_ino) {
		(void)close(reopened);
		errno = ESTALE;
		return -1;
	}

	*out_information = information;
	return reopened;
}
#endif

static bool minimm_note_rights_are_valid(minimm_note_rights_t rights)
{
	return (rights & ~(minimm_note_rights_t)MINIMM_NOTE_RIGHT_ALL) == 0U &&
	       ((rights & MINIMM_NOTE_RIGHT_EDIT) == 0U ||
		(rights & MINIMM_NOTE_RIGHT_WRITE) != 0U);
}

static bool minimm_note_size_is_valid(uint64_t size)
{
	return (size & (MINIMM_PAGE_SIZE - UINT64_C(1))) == UINT64_C(0) &&
	       size <= (uint64_t)INT64_MAX;
}

static minimm_status_t minimm_note_allocate_id(uint64_t *out_id)
{
	uint_fast64_t candidate = atomic_load_explicit(&minimm_next_note_id, memory_order_relaxed);

	while (candidate != UINT64_MAX) {
		if (atomic_compare_exchange_weak_explicit(
			    &minimm_next_note_id, &candidate, candidate + UINT64_C(1),
			    memory_order_relaxed, memory_order_relaxed)) {
			*out_id = (uint64_t)candidate;
			return MINIMM_OK;
		}
	}
	return MINIMM_ERROR_NO_SPACE;
}

static minimm_status_t minimm_note_lineage_create(minimm_note_lineage_t **out_lineage)
{
	minimm_note_lineage_t *lineage = NULL;
	int lock_status = 0;

	lineage = calloc(1U, sizeof(*lineage));
	if (lineage == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	lock_status = pthread_mutex_init(&lineage->lock, NULL);
	if (lock_status != 0) {
		free(lineage);
		return minimm_note_status_from_pthread(lock_status);
	}
	atomic_init(&lineage->references, 1U);
	*out_lineage = lineage;
	return MINIMM_OK;
}

static void minimm_note_lineage_retain(minimm_note_lineage_t *lineage)
{
	size_t references = 0U;

	references = atomic_load_explicit(&lineage->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&lineage->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

static void minimm_note_lineage_release(minimm_note_lineage_t *lineage)
{
	if (atomic_fetch_sub_explicit(&lineage->references, 1U, memory_order_acq_rel) != 1U) {
		return;
	}
	(void)pthread_mutex_destroy(&lineage->lock);
	free(lineage);
}

static minimm_status_t minimm_note_adopt_fd(minimm_t *mm, int fd, uint64_t size,
					    minimm_note_rights_t rights,
					    minimm_note_lineage_t *shared_lineage,
					    bool external_backing, minimm_note_t **out_note)
{
	minimm_note_t *note = NULL;
	minimm_note_lineage_t *lineage = shared_lineage;
	minimm_status_t status = MINIMM_OK;

	if (!minimm_system_try_retain(mm)) {
		(void)close(fd);
		return MINIMM_ERROR_BUSY;
	}

	note = calloc(1U, sizeof(*note));
	if (note == NULL) {
		minimm_system_release(mm);
		(void)close(fd);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	status = minimm_file_backing_create(fd, &note->file_backing);
	(void)close(fd);
	if (status != MINIMM_OK) {
		free(note);
		minimm_system_release(mm);
		return status;
	}
	if (lineage == NULL) {
		status = minimm_note_lineage_create(&lineage);
	} else {
		minimm_note_lineage_retain(lineage);
	}
	if (status != MINIMM_OK) {
		minimm_file_backing_release(note->file_backing);
		free(note);
		minimm_system_release(mm);
		return status;
	}

	status = minimm_note_allocate_id(&note->id);
	if (status != MINIMM_OK) {
		minimm_note_lineage_release(lineage);
		minimm_file_backing_release(note->file_backing);
		free(note);
		minimm_system_release(mm);
		return status;
	}

	note->system = mm;
	note->lineage = lineage;
	atomic_init(&note->references, 1U);
	atomic_init(&note->size, size);
	note->rights = rights;
	note->external_backing = external_backing;
	*out_note = note;
	return MINIMM_OK;
}

static minimm_status_t minimm_note_validate_io(minimm_note_t *note, uint64_t offset,
					       const void *buffer, size_t length,
					       minimm_note_rights_t required_right)
{
	uint64_t size = UINT64_C(0);

	if (note == NULL || (buffer == NULL && length != 0U)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((note->rights & required_right) != required_right) {
		return MINIMM_ERROR_PERMISSION;
	}

	size = (uint64_t)atomic_load_explicit(&note->size, memory_order_acquire);
	if (offset > size || (uint64_t)length > size - offset) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return MINIMM_OK;
}

static minimm_note_page_t **minimm_note_find_page_link_locked(minimm_note_t *note,
							      uint64_t page_offset)
{
	minimm_note_page_t **link = &note->pages;

	while (*link != NULL && (*link)->page_offset < page_offset) {
		link = &(*link)->next;
	}
	return link;
}

static minimm_status_t minimm_note_cache_backing_page_locked(minimm_note_t *note,
							     uint64_t page_offset,
							     minimm_frame_t **out_frame)
{
	minimm_note_page_t **link = NULL;
	minimm_note_page_t *page = NULL;
	minimm_frame_t *frame = NULL;
	minimm_status_t status = MINIMM_OK;
	bool paged_in = false;

	link = minimm_note_find_page_link_locked(note, page_offset);
	if (*link != NULL && (*link)->page_offset == page_offset) {
		*out_frame = (*link)->frame;
		return MINIMM_OK;
	}

	page = malloc(sizeof(*page));
	if (page == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	status = minimm_frame_create_file_backing(note->system->frame_store, note->file_backing,
						  page_offset, true, &frame);
	if (status == MINIMM_OK) {
		status = minimm_frame_ensure_resident(frame, &paged_in);
	}
	if (status != MINIMM_OK) {
		minimm_frame_release(frame);
		free(page);
		return status;
	}

	(void)paged_in;
	page->page_offset = page_offset;
	page->frame = frame;
	page->next = *link;
	*link = page;
	*out_frame = frame;
	return MINIMM_OK;
}

static minimm_status_t minimm_note_cache_copy_locked(minimm_note_t *note, uint64_t page_offset,
						     minimm_frame_t *source,
						     minimm_frame_t **out_frame)
{
	minimm_note_page_t **link = minimm_note_find_page_link_locked(note, page_offset);
	minimm_note_page_t *page = NULL;
	minimm_frame_t *frame = NULL;
	minimm_status_t status = MINIMM_OK;

	if (*link != NULL && (*link)->page_offset == page_offset) {
		*out_frame = (*link)->frame;
		return MINIMM_OK;
	}
	page = malloc(sizeof(*page));
	if (page == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	status = minimm_frame_copy_file(source, note->file_backing, page_offset, &frame);
	if (status != MINIMM_OK) {
		free(page);
		return status;
	}
	page->page_offset = page_offset;
	page->frame = frame;
	page->next = *link;
	*link = page;
	*out_frame = frame;
	return MINIMM_OK;
}

/*
 * Read lookup walks immutable snapshot ancestors without allocating an overlay
 * page. The lineage mutex keeps the parent chain and all page lists stable.
 */
static minimm_status_t minimm_note_get_read_frame_locked(minimm_note_t *note, uint64_t page_offset,
							 minimm_frame_t **out_frame)
{
	const uint64_t size = (uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);
	minimm_note_t *owner = note;
	minimm_frame_t *frame = NULL;
	minimm_status_t status = MINIMM_OK;

	*out_frame = NULL;
	if ((page_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    page_offset >= size || size - page_offset < MINIMM_PAGE_SIZE) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	for (;;) {
		minimm_note_page_t **link = minimm_note_find_page_link_locked(owner, page_offset);

		if (*link != NULL && (*link)->page_offset == page_offset) {
			frame = (*link)->frame;
			break;
		}
		if (owner->cow_parent != NULL && page_offset < owner->inherited_size) {
			owner = owner->cow_parent;
			continue;
		}
		if (page_offset >=
		    (uint64_t)atomic_load_explicit(&owner->size, memory_order_relaxed)) {
			return MINIMM_ERROR_IO;
		}
		status = minimm_note_cache_backing_page_locked(owner, page_offset, &frame);
		if (status != MINIMM_OK) {
			return status;
		}
		break;
	}

	minimm_frame_retain(frame);
	*out_frame = frame;
	return MINIMM_OK;
}

static minimm_status_t minimm_note_preserve_children_locked(minimm_note_t *note,
							    uint64_t page_offset,
							    minimm_frame_t *snapshot)
{
	minimm_note_t *child = note->first_child;

	while (child != NULL) {
		if (page_offset < child->inherited_size) {
			minimm_frame_t *preserved = NULL;
			minimm_status_t status = minimm_note_cache_copy_locked(
				child, page_offset, snapshot, &preserved);

			if (status != MINIMM_OK) {
				return status;
			}
			(void)preserved;
		}
		child = child->next_sibling;
	}
	return MINIMM_OK;
}

/* Return a retained frame that may safely escape to a writer or mapping. */
static minimm_status_t minimm_note_get_private_frame_locked(minimm_note_t *note,
							    uint64_t page_offset,
							    minimm_frame_t **out_frame)
{
	minimm_note_page_t **link = NULL;
	minimm_frame_t *snapshot = NULL;
	minimm_frame_t *private_frame = NULL;
	minimm_status_t status = minimm_note_get_read_frame_locked(note, page_offset, &snapshot);

	*out_frame = NULL;
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_note_preserve_children_locked(note, page_offset, snapshot);
	if (status != MINIMM_OK) {
		minimm_frame_release(snapshot);
		return status;
	}

	link = minimm_note_find_page_link_locked(note, page_offset);
	if (*link != NULL && (*link)->page_offset == page_offset) {
		private_frame = (*link)->frame;
		minimm_frame_retain(private_frame);
	} else {
		status = minimm_note_cache_copy_locked(note, page_offset, snapshot, &private_frame);
		if (status == MINIMM_OK) {
			minimm_frame_retain(private_frame);
		}
	}
	minimm_frame_release(snapshot);
	if (status == MINIMM_OK) {
		*out_frame = private_frame;
	}
	return status;
}

static minimm_status_t minimm_note_sync_pages_locked(minimm_note_t *note, uint64_t offset,
						     uint64_t length)
{
	const uint64_t end = offset + length;
	minimm_note_page_t *page = note->pages;

	while (page != NULL) {
		if (page->page_offset >= end) {
			break;
		}
		if (page->page_offset + MINIMM_PAGE_SIZE > offset) {
			const minimm_status_t status = minimm_frame_sync(page->frame);

			if (status != MINIMM_OK) {
				return status;
			}
		}
		page = page->next;
	}
	return MINIMM_OK;
}

static void minimm_note_release_pages(minimm_note_page_t *page)
{
	while (page != NULL) {
		minimm_note_page_t *next = page->next;

		minimm_frame_release(page->frame);
		free(page);
		page = next;
	}
}

static minimm_note_page_t *minimm_note_detach_pages_from_locked(minimm_note_t *note,
								uint64_t page_offset)
{
	minimm_note_page_t **link = minimm_note_find_page_link_locked(note, page_offset);
	minimm_note_page_t *pages = *link;

	*link = NULL;
	return pages;
}

static minimm_status_t minimm_note_preserve_truncated_children_locked(minimm_note_t *note,
								      uint64_t new_size,
								      uint64_t old_size)
{
	minimm_note_t *child = note->first_child;
	minimm_note_t *owner = note;
	uint64_t preserve_end = new_size;
	uint64_t visible_limit = old_size;

	while (child != NULL) {
		if (child->inherited_size > preserve_end) {
			preserve_end = child->inherited_size;
		}
		child = child->next_sibling;
	}
	if (preserve_end > old_size) {
		preserve_end = old_size;
	}
	while (owner != NULL && visible_limit > new_size) {
		minimm_note_page_t *page = owner->pages;

		while (page != NULL && page->page_offset < preserve_end) {
			if (page->page_offset >= new_size && page->page_offset < visible_limit) {
				minimm_frame_t *snapshot = NULL;
				minimm_status_t status = minimm_note_get_read_frame_locked(
					note, page->page_offset, &snapshot);

				if (status == MINIMM_OK) {
					status = minimm_note_preserve_children_locked(
						note, page->page_offset, snapshot);
				}
				minimm_frame_release(snapshot);
				if (status != MINIMM_OK) {
					return status;
				}
			}
			page = page->next;
		}
		if (owner->cow_parent == NULL) {
			break;
		}
		if (visible_limit > owner->inherited_size) {
			visible_limit = owner->inherited_size;
		}
		owner = owner->cow_parent;
	}

	child = note->first_child;
	while (child != NULL) {
		if (child->inherited_size > new_size) {
			child->inherited_size = new_size;
		}
		child = child->next_sibling;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_note_copy_external_locked(minimm_note_t *source,
							minimm_note_t *destination, uint64_t size)
{
	uint64_t offset = UINT64_C(0);

	while (offset < size) {
		minimm_frame_t *source_frame = NULL;
		minimm_frame_t *destination_frame = NULL;
		minimm_status_t status =
			minimm_note_get_read_frame_locked(source, offset, &source_frame);

		if (status == MINIMM_OK) {
			status = minimm_note_cache_copy_locked(destination, offset, source_frame,
							       &destination_frame);
		}
		minimm_frame_release(source_frame);
		if (status != MINIMM_OK) {
			return status;
		}
		(void)destination_frame;
		offset += MINIMM_PAGE_SIZE;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_note_create_temporary(minimm_t *mm, uint64_t size,
						    minimm_note_rights_t rights,
						    minimm_note_lineage_t *lineage,
						    minimm_note_t **out_note)
{
	char path[] = "/tmp/minimm-note-XXXXXX";
	int fd = -1;

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_note = NULL;
	if (mm == NULL || !minimm_note_size_is_valid(size) ||
	    !minimm_note_rights_are_valid(rights)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

#ifdef O_CLOEXEC
	fd = mkostemp(path, O_CLOEXEC);
#else
	fd = mkstemp(path);
#endif
	if (fd < 0) {
		return minimm_note_status_from_errno(errno);
	}
	if (minimm_note_unlink_retry(path) != 0) {
		const int error_number = errno;

		(void)close(fd);
		return minimm_note_status_from_errno(error_number);
	}
#ifndef O_CLOEXEC
	if (minimm_note_set_cloexec(fd) < 0) {
		const int error_number = errno;

		(void)close(fd);
		return minimm_note_status_from_errno(error_number);
	}
#endif
	while (ftruncate(fd, (off_t)size) != 0) {
		const int error_number = errno;

		if (error_number == EINTR) {
			continue;
		}
		(void)close(fd);
		return minimm_note_status_from_errno(error_number);
	}

	return minimm_note_adopt_fd(mm, fd, size, rights, lineage, false, out_note);
}

minimm_status_t minimm_note_create(minimm_t *mm, uint64_t size, minimm_note_rights_t rights,
				   minimm_note_t **out_note)
{
	return minimm_note_create_temporary(mm, size, rights, NULL, out_note);
}

minimm_status_t minimm_note_copy(minimm_note_t *source, minimm_note_rights_t rights,
				 minimm_note_t **out_note)
{
	minimm_note_t *copy = NULL;
	minimm_status_t status = MINIMM_OK;
	uint64_t size = UINT64_C(0);

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_note = NULL;
	if (source == NULL || !minimm_note_rights_are_valid(rights)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((source->rights & MINIMM_NOTE_RIGHT_READ) == 0U) {
		return MINIMM_ERROR_PERMISSION;
	}

	(void)pthread_mutex_lock(&source->lineage->lock);
	if (source->active_mappings != 0U) {
		status = MINIMM_ERROR_BUSY;
	} else {
		size = (uint64_t)atomic_load_explicit(&source->size, memory_order_relaxed);
		status = minimm_note_create_temporary(
			source->system, size, rights,
			source->external_backing ? NULL : source->lineage, &copy);
		if (status == MINIMM_OK && source->external_backing) {
			(void)pthread_mutex_lock(&copy->lineage->lock);
			status = minimm_note_copy_external_locked(source, copy, size);
			(void)pthread_mutex_unlock(&copy->lineage->lock);
		} else if (status == MINIMM_OK) {
			copy->cow_parent = source;
			copy->inherited_size = size;
			copy->next_sibling = source->first_child;
			source->first_child = copy;
			minimm_note_retain(source);
		}
	}
	(void)pthread_mutex_unlock(&source->lineage->lock);

	if (status != MINIMM_OK) {
		minimm_note_release(copy);
		return status;
	}
	*out_note = copy;
	return MINIMM_OK;
}

minimm_status_t minimm_note_open_fd(minimm_t *mm, int fd, minimm_note_rights_t rights,
				    minimm_note_t **out_note)
{
	struct stat information = { 0 };
#ifdef __linux__
	struct stat reopened_information = { 0 };
#endif
	int duplicate_fd = -1;
	int access_mode = 0;
	uint64_t size = UINT64_C(0);

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_note = NULL;
	if (mm == NULL || fd < 0 || !minimm_note_rights_are_valid(rights)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	duplicate_fd = minimm_note_duplicate_fd(fd);
	if (duplicate_fd < 0) {
		return minimm_note_status_from_errno(errno);
	}
	while (fstat(duplicate_fd, &information) != 0) {
		const int error_number = errno;

		if (error_number == EINTR) {
			continue;
		}
		(void)close(duplicate_fd);
		return minimm_note_status_from_errno(error_number);
	}
	if (!S_ISREG(information.st_mode) || information.st_size < (off_t)0) {
		(void)close(duplicate_fd);
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	size = (uint64_t)information.st_size;
	if (!minimm_note_size_is_valid(size)) {
		(void)close(duplicate_fd);
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	do {
		access_mode = fcntl(duplicate_fd, F_GETFL);
	} while (access_mode < 0 && errno == EINTR);
	if (access_mode < 0) {
		const int error_number = errno;

		(void)close(duplicate_fd);
		return minimm_note_status_from_errno(error_number);
	}
#ifdef O_PATH
	if ((access_mode & O_PATH) != 0) {
		(void)close(duplicate_fd);
		return MINIMM_ERROR_PERMISSION;
	}
#endif
	/*
	 * Linux pwrite(2) appends instead of honoring its offset when O_APPEND is
	 * set. The validation duplicate still shares that status flag with the
	 * caller, so do not silently discard an append-mode contract. Accepted
	 * Linux writable descriptors are reopened independently below, which also
	 * isolates them from a later caller F_SETFL(O_APPEND).
	 */
	if ((access_mode & O_APPEND) != 0 &&
	    (rights & (MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_EDIT)) != 0U) {
		(void)close(duplicate_fd);
		return MINIMM_ERROR_PERMISSION;
	}
	access_mode &= O_ACCMODE;
	if ((access_mode != O_RDONLY && access_mode != O_RDWR) ||
	    ((rights & (MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_EDIT |
			MINIMM_NOTE_RIGHT_RESIZE)) != 0U &&
	     access_mode != O_RDWR)) {
		(void)close(duplicate_fd);
		return MINIMM_ERROR_PERMISSION;
	}

#ifdef __linux__
	if ((rights & MINIMM_NOTE_RIGHT_WRITE) != 0U) {
		const int reopened_fd = minimm_note_reopen_independent(duplicate_fd, &information,
								       &reopened_information);

		if (reopened_fd < 0) {
			const int error_number = errno;

			(void)close(duplicate_fd);
			return minimm_note_status_from_errno(error_number);
		}
		(void)close(duplicate_fd);
		duplicate_fd = reopened_fd;
		information = reopened_information;
		if (information.st_size < (off_t)0 ||
		    !minimm_note_size_is_valid((uint64_t)information.st_size)) {
			(void)close(duplicate_fd);
			return MINIMM_ERROR_INVALID_ARGUMENT;
		}
		size = (uint64_t)information.st_size;
	}
#endif

	return minimm_note_adopt_fd(mm, duplicate_fd, size, rights, NULL, true, out_note);
}

void minimm_note_retain(minimm_note_t *note)
{
	size_t references = 0U;

	if (note == NULL) {
		return;
	}
	references = atomic_load_explicit(&note->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&note->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

void minimm_note_release(minimm_note_t *note)
{
	/*
	 * A child owns one reference to its COW parent. Releasing a leaf can
	 * therefore make every externally released ancestor collectible. Walk
	 * that cascade iteratively so an arbitrarily deep COPY chain cannot turn
	 * into an equally deep C call stack.
	 */
	while (note != NULL &&
	       atomic_fetch_sub_explicit(&note->references, 1U, memory_order_acq_rel) == 1U) {
		minimm_t *mm = note->system;
		minimm_note_lineage_t *lineage = note->lineage;
		minimm_note_t *parent = NULL;
		minimm_note_page_t *pages = NULL;

		(void)pthread_mutex_lock(&lineage->lock);
		if (note->first_child != NULL) {
			(void)pthread_mutex_unlock(&lineage->lock);
			abort();
		}
		parent = note->cow_parent;
		if (parent != NULL) {
			minimm_note_t **link = &parent->first_child;

			while (*link != NULL && *link != note) {
				link = &(*link)->next_sibling;
			}
			if (*link != note) {
				(void)pthread_mutex_unlock(&lineage->lock);
				abort();
			}
			*link = note->next_sibling;
			note->cow_parent = NULL;
			note->next_sibling = NULL;
		}
		(void)minimm_note_sync_pages_locked(
			note, UINT64_C(0),
			(uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed));
		pages = note->pages;
		note->pages = NULL;
		(void)pthread_mutex_unlock(&lineage->lock);

		minimm_note_release_pages(pages);
		minimm_file_backing_release(note->file_backing);
		free(note);
		minimm_system_release(mm);
		minimm_note_lineage_release(lineage);
		note = parent;
	}
}

uint64_t minimm_note_id(const minimm_note_t *note)
{
	return note == NULL ? UINT64_C(0) : note->id;
}

uint64_t minimm_note_size(const minimm_note_t *note)
{
	return note == NULL ? UINT64_C(0) :
			      (uint64_t)atomic_load_explicit(&note->size, memory_order_acquire);
}

minimm_note_rights_t minimm_note_rights(const minimm_note_t *note)
{
	return note == NULL ? MINIMM_NOTE_RIGHT_NONE : note->rights;
}

minimm_status_t minimm_note_resize(minimm_note_t *note, uint64_t size)
{
	minimm_status_t status = MINIMM_OK;
	minimm_note_page_t *pages = NULL;
	uint64_t old_size = UINT64_C(0);

	if (note == NULL || !minimm_note_size_is_valid(size)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((note->rights & MINIMM_NOTE_RIGHT_RESIZE) == 0U) {
		return MINIMM_ERROR_PERMISSION;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	old_size = (uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);
	if (note->active_mappings != 0U && size < old_size) {
		(void)pthread_mutex_unlock(&note->lineage->lock);
		return MINIMM_ERROR_BUSY;
	}
	if (size < old_size) {
		status = minimm_note_preserve_truncated_children_locked(note, size, old_size);
		if (status == MINIMM_OK) {
			status = minimm_note_sync_pages_locked(note, UINT64_C(0), old_size);
		}
	}
	if (status == MINIMM_OK && size != old_size) {
		status = minimm_file_backing_resize(note->file_backing, size);
	}
	if (status == MINIMM_OK) {
		atomic_store_explicit(&note->size, size, memory_order_release);
		if (note->inherited_size > size) {
			note->inherited_size = size;
		}
		if (size < old_size) {
			pages = minimm_note_detach_pages_from_locked(note, size);
		}
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);
	minimm_note_release_pages(pages);
	return status;
}

minimm_status_t minimm_note_pread(minimm_note_t *note, uint64_t offset, void *destination,
				  size_t length, size_t *out_completed)
{
	unsigned char *bytes = destination;
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;

	if (out_completed != NULL) {
		*out_completed = 0U;
	}
	status = minimm_note_validate_io(note, offset, destination, length, MINIMM_NOTE_RIGHT_READ);

	if (status != MINIMM_OK || length == 0U) {
		return status;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	{
		const uint64_t current_size =
			(uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);

		if (offset > current_size || (uint64_t)length > current_size - offset) {
			status = MINIMM_ERROR_INVALID_ARGUMENT;
		}
	}
	while (status == MINIMM_OK && completed < length) {
		const uint64_t current = offset + (uint64_t)completed;
		const uint64_t page_offset = current & ~(MINIMM_PAGE_SIZE - UINT64_C(1));
		const size_t within_page = (size_t)(current - page_offset);
		const size_t remaining = length - completed;
		const size_t available = (size_t)MINIMM_PAGE_SIZE - within_page;
		const size_t chunk = remaining < available ? remaining : available;
		minimm_frame_t *frame = NULL;

		status = minimm_note_get_read_frame_locked(note, page_offset, &frame);
		if (status == MINIMM_OK) {
			status = minimm_frame_read(frame, within_page, bytes + completed, chunk);
		}
		minimm_frame_release(frame);
		if (status == MINIMM_OK) {
			completed += chunk;
		}
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);

	if (out_completed != NULL) {
		*out_completed = completed;
	}
	return status;
}

minimm_status_t minimm_note_pwrite(minimm_note_t *note, uint64_t offset, const void *source,
				   size_t length, size_t *out_completed)
{
	const unsigned char *bytes = source;
	size_t completed = 0U;
	minimm_status_t status =
		minimm_note_validate_io(note, offset, source, length, MINIMM_NOTE_RIGHT_WRITE);

	if (status != MINIMM_OK || length == 0U) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return status;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	{
		const uint64_t current_size =
			(uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);

		if (offset > current_size || (uint64_t)length > current_size - offset) {
			status = MINIMM_ERROR_INVALID_ARGUMENT;
		}
	}
	while (status == MINIMM_OK && completed < length) {
		const uint64_t current = offset + (uint64_t)completed;
		const uint64_t page_offset = current & ~(MINIMM_PAGE_SIZE - UINT64_C(1));
		const size_t within_page = (size_t)(current - page_offset);
		const size_t remaining = length - completed;
		const size_t available = (size_t)MINIMM_PAGE_SIZE - within_page;
		const size_t chunk = remaining < available ? remaining : available;
		minimm_frame_t *frame = NULL;

		status = minimm_note_get_private_frame_locked(note, page_offset, &frame);
		if (status == MINIMM_OK) {
			status = minimm_frame_write(frame, within_page, bytes + completed, chunk);
		}
		minimm_frame_release(frame);
		if (status == MINIMM_OK) {
			completed += chunk;
		}
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);

	if (out_completed != NULL) {
		*out_completed = completed;
	}
	return status;
}

minimm_status_t minimm_note_flush(minimm_note_t *note)
{
	minimm_status_t status = MINIMM_OK;

	if (note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	status = minimm_note_sync_pages_locked(
		note, UINT64_C(0),
		(uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed));
	if (status == MINIMM_OK) {
		status = minimm_file_backing_sync(note->file_backing);
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);
	return status;
}

minimm_status_t minimm_note_pedit(minimm_note_t *note, uint64_t offset, const void *source,
				  size_t length, size_t *out_completed)
{
	if (note == NULL || (source == NULL && length != 0U)) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if ((note->rights & (MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_EDIT)) !=
	    (MINIMM_NOTE_RIGHT_WRITE | MINIMM_NOTE_RIGHT_EDIT)) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_PERMISSION;
	}
	return minimm_note_pwrite(note, offset, source, length, out_completed);
}

minimm_status_t minimm_note_mapping_attach(minimm_note_t *note)
{
	if (note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	if (note->active_mappings == SIZE_MAX) {
		(void)pthread_mutex_unlock(&note->lineage->lock);
		return MINIMM_ERROR_NO_SPACE;
	}
	note->active_mappings += 1U;
	(void)pthread_mutex_unlock(&note->lineage->lock);
	return MINIMM_OK;
}

void minimm_note_mapping_detach(minimm_note_t *note)
{
	if (note == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&note->lineage->lock);
	if (note->active_mappings == 0U) {
		(void)pthread_mutex_unlock(&note->lineage->lock);
		abort();
	}
	note->active_mappings -= 1U;
	(void)pthread_mutex_unlock(&note->lineage->lock);
}

bool minimm_note_belongs_to(const minimm_note_t *note, const minimm_t *mm)
{
	return note != NULL && note->system == mm;
}

minimm_status_t minimm_note_get_frame(minimm_note_t *note, uint64_t page_offset,
				      minimm_frame_t **out_frame)
{
	minimm_status_t status = MINIMM_OK;

	if (note == NULL || out_frame == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;
	(void)pthread_mutex_lock(&note->lineage->lock);
	status = minimm_note_get_private_frame_locked(note, page_offset, out_frame);
	(void)pthread_mutex_unlock(&note->lineage->lock);
	return status;
}

minimm_status_t minimm_note_peek_frame(minimm_note_t *note, uint64_t page_offset,
				       minimm_frame_t **out_frame)
{
	minimm_note_t *owner = note;
	minimm_status_t status = MINIMM_ERROR_NOT_FOUND;
	uint64_t size = UINT64_C(0);

	if (note == NULL || out_frame == NULL ||
	    (page_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;
	(void)pthread_mutex_lock(&note->lineage->lock);
	size = (uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);
	if (page_offset >= size || size - page_offset < MINIMM_PAGE_SIZE) {
		status = MINIMM_ERROR_INVALID_ARGUMENT;
	} else {
		while (owner != NULL) {
			minimm_note_page_t **link =
				minimm_note_find_page_link_locked(owner, page_offset);

			if (*link != NULL && (*link)->page_offset == page_offset) {
				minimm_frame_retain((*link)->frame);
				*out_frame = (*link)->frame;
				status = MINIMM_OK;
				break;
			}
			if (owner->cow_parent == NULL || page_offset >= owner->inherited_size) {
				break;
			}
			owner = owner->cow_parent;
		}
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);
	return status;
}

minimm_status_t minimm_note_sync_range(minimm_note_t *note, uint64_t offset, uint64_t length)
{
	uint64_t size = UINT64_C(0);
	minimm_status_t status = MINIMM_OK;

	if (note == NULL || length == UINT64_C(0) ||
	    (offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    (length & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	(void)pthread_mutex_lock(&note->lineage->lock);
	size = (uint64_t)atomic_load_explicit(&note->size, memory_order_relaxed);
	if (offset > size || length > size - offset) {
		status = MINIMM_ERROR_INVALID_ARGUMENT;
	} else {
		status = minimm_note_sync_pages_locked(note, offset, length);
	}
	if (status == MINIMM_OK) {
		status = minimm_file_backing_sync(note->file_backing);
	}
	(void)pthread_mutex_unlock(&note->lineage->lock);
	return status;
}
