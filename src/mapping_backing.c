#include "mapping_backing.h"

#include "internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct minimm_backing_page {
	struct minimm_backing_page *next;
	minimm_frame_t *frame;
	uint64_t page_offset;
} minimm_backing_page_t;

struct minimm_mapping_backing {
	minimm_t *system;
	minimm_note_t *note;
	pthread_mutex_t lock;
	atomic_size_t references;
	minimm_backing_kind_t kind;
	minimm_backing_page_t *pages;
};

static void minimm_mapping_backing_release_pages(minimm_backing_page_t *page)
{
	while (page != NULL) {
		minimm_backing_page_t *next = page->next;

		minimm_frame_release(page->frame);
		free(page);
		page = next;
	}
}

minimm_status_t minimm_mapping_backing_create(minimm_t *system, minimm_note_t *note,
					      minimm_map_flags_t flags,
					      minimm_mapping_backing_t **out_backing)
{
	const bool anonymous = (flags & MINIMM_MAP_ANONYMOUS) != 0U;
	const bool shared = (flags & MINIMM_MAP_SHARED) != 0U;
	minimm_mapping_backing_t *backing = NULL;
	minimm_status_t status = MINIMM_OK;

	if (system == NULL || out_backing == NULL || (anonymous && note != NULL) ||
	    (!anonymous && note == NULL)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_backing = NULL;

	backing = calloc(1U, sizeof(*backing));
	if (backing == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&backing->lock, NULL) != 0) {
		free(backing);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	atomic_init(&backing->references, 1U);
	backing->system = system;
	backing->note = note;
	if (anonymous) {
		backing->kind = shared ? MINIMM_BACKING_ANON_SHARED : MINIMM_BACKING_ANON_PRIVATE;
	} else {
		status = minimm_note_mapping_attach(note);
		if (status != MINIMM_OK) {
			(void)pthread_mutex_destroy(&backing->lock);
			free(backing);
			return status;
		}
		minimm_note_retain(note);
		backing->kind = shared ? MINIMM_BACKING_NOTE_SHARED : MINIMM_BACKING_NOTE_PRIVATE;
	}

	*out_backing = backing;
	return MINIMM_OK;
}

void minimm_mapping_backing_retain(minimm_mapping_backing_t *backing)
{
	size_t references = 0U;

	if (backing == NULL) {
		return;
	}
	references = atomic_load_explicit(&backing->references, memory_order_relaxed);
	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&backing->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

void minimm_mapping_backing_release(minimm_mapping_backing_t *backing)
{
	if (backing == NULL ||
	    atomic_fetch_sub_explicit(&backing->references, 1U, memory_order_acq_rel) != 1U) {
		return;
	}

	minimm_mapping_backing_release_pages(backing->pages);
	if (backing->note != NULL) {
		minimm_note_mapping_detach(backing->note);
		minimm_note_release(backing->note);
	}
	(void)pthread_mutex_destroy(&backing->lock);
	free(backing);
}

minimm_backing_kind_t minimm_mapping_backing_kind(const minimm_mapping_backing_t *backing)
{
	return backing == NULL ? MINIMM_BACKING_ANON_PRIVATE : backing->kind;
}

minimm_note_t *minimm_mapping_backing_note(const minimm_mapping_backing_t *backing)
{
	return backing == NULL ? NULL : backing->note;
}

minimm_status_t minimm_mapping_backing_get_frame(minimm_mapping_backing_t *backing,
						 uint64_t page_offset, minimm_frame_t **out_frame,
						 bool *out_created)
{
	minimm_backing_page_t **link = NULL;
	minimm_backing_page_t *page = NULL;
	minimm_frame_t *frame = NULL;
	minimm_status_t status = MINIMM_OK;
	bool paged_in = false;

	if (backing == NULL || out_frame == NULL ||
	    (page_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;
	if (out_created != NULL) {
		*out_created = false;
	}
	if (backing->note != NULL) {
		return minimm_note_get_frame(backing->note, page_offset, out_frame);
	}
	if (backing->kind == MINIMM_BACKING_ANON_PRIVATE) {
		status = minimm_frame_create_zero(backing->system->frame_store, out_frame);
		if (status == MINIMM_OK && out_created != NULL) {
			*out_created = true;
		}
		return status;
	}

	(void)pthread_mutex_lock(&backing->lock);
	link = &backing->pages;
	while (*link != NULL && (*link)->page_offset < page_offset) {
		link = &(*link)->next;
	}
	if (*link != NULL && (*link)->page_offset == page_offset) {
		minimm_frame_retain((*link)->frame);
		*out_frame = (*link)->frame;
		(void)pthread_mutex_unlock(&backing->lock);
		return MINIMM_OK;
	}

	page = malloc(sizeof(*page));
	if (page == NULL) {
		(void)pthread_mutex_unlock(&backing->lock);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	status = minimm_frame_create_zero(backing->system->frame_store, &frame);
	if (status == MINIMM_OK) {
		status = minimm_frame_ensure_resident(frame, &paged_in);
	}
	if (status != MINIMM_OK) {
		minimm_frame_release(frame);
		free(page);
		(void)pthread_mutex_unlock(&backing->lock);
		return status;
	}

	(void)paged_in;
	page->page_offset = page_offset;
	page->frame = frame;
	page->next = *link;
	*link = page;
	minimm_frame_retain(frame);
	*out_frame = frame;
	if (out_created != NULL) {
		*out_created = true;
	}
	(void)pthread_mutex_unlock(&backing->lock);
	return MINIMM_OK;
}

minimm_status_t minimm_mapping_backing_peek_frame(minimm_mapping_backing_t *backing,
						  uint64_t page_offset, minimm_frame_t **out_frame)
{
	minimm_backing_page_t *page = NULL;

	if (backing == NULL || out_frame == NULL ||
	    (page_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_frame = NULL;
	if (backing->note != NULL) {
		return minimm_note_peek_frame(backing->note, page_offset, out_frame);
	}
	if (backing->kind == MINIMM_BACKING_ANON_PRIVATE) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	(void)pthread_mutex_lock(&backing->lock);
	page = backing->pages;
	while (page != NULL && page->page_offset < page_offset) {
		page = page->next;
	}
	if (page != NULL && page->page_offset == page_offset) {
		minimm_frame_retain(page->frame);
		*out_frame = page->frame;
	}
	(void)pthread_mutex_unlock(&backing->lock);
	return *out_frame == NULL ? MINIMM_ERROR_NOT_FOUND : MINIMM_OK;
}

minimm_status_t minimm_space_binding_create(minimm_mapping_backing_t *backing,
					    minimm_space_binding_t **out_binding)
{
	minimm_space_binding_t *binding = NULL;

	if (backing == NULL || out_binding == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_binding = NULL;
	binding = calloc(1U, sizeof(*binding));
	if (binding == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	binding->backing = backing;
	*out_binding = binding;
	return MINIMM_OK;
}

void minimm_space_binding_list_destroy(minimm_space_binding_t *bindings)
{
	while (bindings != NULL) {
		minimm_space_binding_t *next = bindings->next;

		minimm_mapping_backing_release(bindings->backing);
		free(bindings);
		bindings = next;
	}
}

minimm_space_binding_t *minimm_space_binding_find(minimm_space_binding_t *bindings, uint64_t cookie)
{
	while (bindings != NULL) {
		if (bindings->cookie == cookie) {
			return bindings;
		}
		bindings = bindings->next;
	}
	return NULL;
}

minimm_space_binding_t *minimm_space_binding_prune(minimm_space_binding_t **bindings,
						   const minimm_vma_snapshot_t *snapshot)
{
	minimm_space_binding_t **link = bindings;
	minimm_space_binding_t *removed = NULL;

	while (*link != NULL) {
		minimm_space_binding_t *binding = *link;

		if (minimm_vma_snapshot_contains_cookie(snapshot, binding->cookie)) {
			link = &binding->next;
		} else {
			*link = binding->next;
			binding->next = removed;
			removed = binding;
		}
	}
	return removed;
}

minimm_status_t minimm_space_binding_list_clone(const minimm_space_binding_t *bindings,
						minimm_space_binding_t **out_bindings)
{
	minimm_space_binding_t *head = NULL;
	minimm_space_binding_t **tail = &head;

	if (out_bindings == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_bindings = NULL;
	while (bindings != NULL) {
		minimm_space_binding_t *copy = calloc(1U, sizeof(*copy));

		if (copy == NULL) {
			minimm_space_binding_list_destroy(head);
			return MINIMM_ERROR_OUT_OF_MEMORY;
		}
		minimm_mapping_backing_retain(bindings->backing);
		copy->backing = bindings->backing;
		copy->cookie = bindings->cookie;
		*tail = copy;
		tail = &copy->next;
		bindings = bindings->next;
	}
	*out_bindings = head;
	return MINIMM_OK;
}
