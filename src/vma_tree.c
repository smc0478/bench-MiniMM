#include "vma_tree.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct minimm_vma_tree_node minimm_vma_tree_node_t;

struct minimm_vma_tree_node {
	minimm_vaddr_t max_end[MINIMM_VMA_TREE_FANOUT];
	union {
		const minimm_vma_t *ranges[MINIMM_VMA_TREE_FANOUT];
		minimm_vma_tree_node_t *children[MINIMM_VMA_TREE_FANOUT];
	} slots;
	size_t count;
	bool leaf;
};

struct minimm_vma_snapshot {
	minimm_vma_t *ranges;
	minimm_vma_tree_node_t *root;
	size_t count;
	uint64_t generation;
};

static bool minimm_vma_page_range_is_valid(minimm_vaddr_t start, minimm_vaddr_t end)
{
	const uint64_t page_mask = MINIMM_PAGE_SIZE - UINT64_C(1);

	return start < end && end <= MINIMM_USER_ADDRESS_LIMIT &&
	       (start & page_mask) == UINT64_C(0) && (end & page_mask) == UINT64_C(0);
}

static bool minimm_vma_protection_is_valid(minimm_vma_prot_t protection)
{
	const minimm_vma_prot_t allowed = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE |
					  MINIMM_VMA_PROT_EDIT | MINIMM_VMA_PROT_EXEC;

	return (protection & ~allowed) == UINT32_C(0) &&
	       ((protection & MINIMM_VMA_PROT_EDIT) == UINT32_C(0) ||
		(protection & MINIMM_VMA_PROT_WRITE) != UINT32_C(0));
}

static bool minimm_vma_is_valid(const minimm_vma_t *mapping)
{
	uint64_t length = 0U;

	if (mapping == NULL || !minimm_vma_page_range_is_valid(mapping->start, mapping->end) ||
	    (mapping->note_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    !minimm_vma_protection_is_valid(mapping->prot) ||
	    !minimm_vma_protection_is_valid(mapping->max_prot) ||
	    (mapping->flags != MINIMM_VMA_FLAG_PRIVATE &&
	     mapping->flags != MINIMM_VMA_FLAG_SHARED) ||
	    (mapping->prot & ~mapping->max_prot) != UINT32_C(0)) {
		return false;
	}

	length = mapping->end - mapping->start;
	return mapping->note_offset <= UINT64_MAX - length;
}

static void minimm_vma_tree_node_destroy(minimm_vma_tree_node_t *node)
{
	size_t index = 0U;

	if (node == NULL) {
		return;
	}
	if (!node->leaf) {
		for (index = 0U; index < node->count; ++index) {
			minimm_vma_tree_node_destroy(node->slots.children[index]);
		}
	}
	free(node);
}

static size_t minimm_vma_tree_child_capacity(unsigned height)
{
	size_t capacity = MINIMM_VMA_TREE_FANOUT;
	unsigned level = 1U;

	for (level = 1U; level < height; ++level) {
		capacity *= MINIMM_VMA_TREE_FANOUT;
	}
	return capacity;
}

static minimm_status_t minimm_vma_tree_build_node(const minimm_vma_t *ranges, size_t count,
						  unsigned height,
						  minimm_vma_tree_node_t **out_node)
{
	minimm_vma_tree_node_t *node = NULL;
	size_t index = 0U;

	*out_node = NULL;
	node = calloc(1U, sizeof(*node));
	if (node == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	node->leaf = height == 0U;
	if (node->leaf) {
		node->count = count;
		for (index = 0U; index < count; ++index) {
			node->slots.ranges[index] = &ranges[index];
			node->max_end[index] = ranges[index].end;
		}
	} else {
		const size_t child_capacity = minimm_vma_tree_child_capacity(height);
		size_t offset = 0U;

		while (offset < count) {
			const size_t remaining = count - offset;
			const size_t child_count = remaining < child_capacity ? remaining :
										child_capacity;
			minimm_status_t status = MINIMM_OK;

			status = minimm_vma_tree_build_node(ranges + offset, child_count,
							    height - 1U,
							    &node->slots.children[node->count]);
			if (status != MINIMM_OK) {
				minimm_vma_tree_node_destroy(node);
				return status;
			}
			node->max_end[node->count] = ranges[offset + child_count - 1U].end;
			node->count += 1U;
			offset += child_count;
		}
	}

	*out_node = node;
	return MINIMM_OK;
}

static minimm_status_t minimm_vma_tree_build(const minimm_vma_t *ranges, size_t count,
					     minimm_vma_tree_node_t **out_root)
{
	size_t capacity = MINIMM_VMA_TREE_FANOUT;
	unsigned height = 0U;

	*out_root = NULL;
	if (count == 0U) {
		return MINIMM_OK;
	}

	while (count > capacity) {
		if (capacity > SIZE_MAX / MINIMM_VMA_TREE_FANOUT) {
			return MINIMM_ERROR_NO_SPACE;
		}
		capacity *= MINIMM_VMA_TREE_FANOUT;
		height += 1U;
	}
	return minimm_vma_tree_build_node(ranges, count, height, out_root);
}

static bool minimm_vma_can_merge(const minimm_vma_t *left, const minimm_vma_t *right)
{
	const uint64_t left_length = left->end - left->start;

	return left->end == right->start && left->mapping_cookie == right->mapping_cookie &&
	       left->prot == right->prot && left->max_prot == right->max_prot &&
	       left->flags == right->flags && left->note_offset + left_length == right->note_offset;
}

static size_t minimm_vma_coalesce_ranges(minimm_vma_t *ranges, size_t count)
{
	size_t input = 0U;
	size_t output = 0U;

	for (input = 0U; input < count; ++input) {
		if (output != 0U && minimm_vma_can_merge(&ranges[output - 1U], &ranges[input])) {
			ranges[output - 1U].end = ranges[input].end;
			continue;
		}
		if (output != input) {
			ranges[output] = ranges[input];
		}
		output += 1U;
	}
	return output;
}

/* This function consumes ranges on both success and failure. */
static minimm_status_t minimm_vma_snapshot_from_ranges(minimm_vma_t *ranges, size_t count,
						       uint64_t generation,
						       minimm_vma_snapshot_t **out_snapshot)
{
	minimm_vma_snapshot_t *snapshot = NULL;
	minimm_status_t status = MINIMM_OK;

	*out_snapshot = NULL;
	snapshot = calloc(1U, sizeof(*snapshot));
	if (snapshot == NULL) {
		free(ranges);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	count = minimm_vma_coalesce_ranges(ranges, count);

	snapshot->ranges = ranges;
	snapshot->count = count;
	snapshot->generation = generation;
	status = minimm_vma_tree_build(ranges, count, &snapshot->root);
	if (status != MINIMM_OK) {
		free(ranges);
		free(snapshot);
		return status;
	}

	*out_snapshot = snapshot;
	return MINIMM_OK;
}

static minimm_status_t minimm_vma_allocate_ranges(size_t count, minimm_vma_t **out_ranges)
{
	*out_ranges = NULL;
	if (count == 0U) {
		return MINIMM_OK;
	}
	if (count > SIZE_MAX / sizeof(**out_ranges)) {
		return MINIMM_ERROR_NO_SPACE;
	}

	*out_ranges = calloc(count, sizeof(**out_ranges));
	if (*out_ranges == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_vma_next_generation(const minimm_vma_snapshot_t *snapshot,
						  uint64_t *out_generation)
{
	if (snapshot->generation == UINT64_MAX) {
		return MINIMM_ERROR_NO_SPACE;
	}
	*out_generation = snapshot->generation + UINT64_C(1);
	return MINIMM_OK;
}

static minimm_status_t minimm_vma_fragment(const minimm_vma_t *source, minimm_vaddr_t start,
					   minimm_vaddr_t end, minimm_vma_prot_t protection,
					   bool replace_protection, minimm_vma_t *out_mapping)
{
	const uint64_t offset_delta = start - source->start;

	if (source->note_offset > UINT64_MAX - offset_delta) {
		return MINIMM_ERROR_NO_SPACE;
	}

	*out_mapping = *source;
	out_mapping->start = start;
	out_mapping->end = end;
	out_mapping->note_offset = source->note_offset + offset_delta;
	if (replace_protection) {
		out_mapping->prot = protection;
	}
	return MINIMM_OK;
}

minimm_status_t minimm_vma_snapshot_create(minimm_vma_snapshot_t **out_snapshot)
{
	if (out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_vma_snapshot_from_ranges(NULL, 0U, UINT64_C(1), out_snapshot);
}

minimm_status_t minimm_vma_snapshot_clone(const minimm_vma_snapshot_t *snapshot,
					  minimm_vma_snapshot_t **out_snapshot)
{
	minimm_vma_t *ranges = NULL;
	minimm_status_t status = MINIMM_OK;

	if (snapshot == NULL || out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_snapshot = NULL;

	status = minimm_vma_allocate_ranges(snapshot->count, &ranges);
	if (status != MINIMM_OK) {
		return status;
	}
	if (snapshot->count != 0U) {
		(void)memcpy(ranges, snapshot->ranges, snapshot->count * sizeof(*ranges));
	}
	return minimm_vma_snapshot_from_ranges(ranges, snapshot->count, snapshot->generation,
					       out_snapshot);
}

void minimm_vma_snapshot_destroy(minimm_vma_snapshot_t *snapshot)
{
	if (snapshot == NULL) {
		return;
	}
	minimm_vma_tree_node_destroy(snapshot->root);
	free(snapshot->ranges);
	free(snapshot);
}

const minimm_vma_t *minimm_vma_snapshot_lookup(const minimm_vma_snapshot_t *snapshot,
					       minimm_vaddr_t address)
{
	const minimm_vma_tree_node_t *node = NULL;
	size_t low = 0U;
	size_t high = 0U;

	if (snapshot == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return NULL;
	}
	node = snapshot->root;
	if (node == NULL) {
		return NULL;
	}

	while (!node->leaf) {
		size_t index = 0U;

		while (index < node->count && address >= node->max_end[index]) {
			index += 1U;
		}
		if (index == node->count) {
			return NULL;
		}
		node = node->slots.children[index];
	}

	high = node->count;
	while (low < high) {
		const size_t middle = low + ((high - low) / 2U);

		if (node->slots.ranges[middle]->start <= address) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	if (low == 0U) {
		return NULL;
	}

	if (address < node->slots.ranges[low - 1U]->end) {
		return node->slots.ranges[low - 1U];
	}
	return NULL;
}

const minimm_vma_t *minimm_vma_snapshot_find_next(const minimm_vma_snapshot_t *snapshot,
						  minimm_vaddr_t address)
{
	size_t low = 0U;
	size_t high = 0U;

	if (snapshot == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return NULL;
	}
	high = snapshot->count;
	while (low < high) {
		const size_t middle = low + ((high - low) / 2U);

		if (snapshot->ranges[middle].end <= address) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	return low < snapshot->count ? &snapshot->ranges[low] : NULL;
}

size_t minimm_vma_snapshot_count(const minimm_vma_snapshot_t *snapshot)
{
	return snapshot == NULL ? 0U : snapshot->count;
}

uint64_t minimm_vma_snapshot_generation(const minimm_vma_snapshot_t *snapshot)
{
	return snapshot == NULL ? UINT64_C(0) : snapshot->generation;
}

bool minimm_vma_snapshot_contains_cookie(const minimm_vma_snapshot_t *snapshot,
					 uint64_t mapping_cookie)
{
	size_t index = 0U;

	if (snapshot == NULL || mapping_cookie == UINT64_C(0)) {
		return false;
	}
	for (index = 0U; index < snapshot->count; ++index) {
		if (snapshot->ranges[index].mapping_cookie == mapping_cookie) {
			return true;
		}
	}
	return false;
}

minimm_status_t minimm_vma_snapshot_insert(const minimm_vma_snapshot_t *snapshot,
					   const minimm_vma_t *mapping,
					   minimm_vma_snapshot_t **out_snapshot)
{
	minimm_vma_t *ranges = NULL;
	size_t low = 0U;
	size_t high = 0U;
	size_t position = 0U;
	size_t new_count = 0U;
	uint64_t generation = 0U;
	minimm_status_t status = MINIMM_OK;

	if (out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_snapshot = NULL;
	if (snapshot == NULL || !minimm_vma_is_valid(mapping)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	high = snapshot->count;
	while (low < high) {
		const size_t middle = low + ((high - low) / 2U);

		if (snapshot->ranges[middle].start < mapping->start) {
			low = middle + 1U;
		} else {
			high = middle;
		}
	}
	position = low;
	if ((position > 0U && snapshot->ranges[position - 1U].end > mapping->start) ||
	    (position < snapshot->count && snapshot->ranges[position].start < mapping->end)) {
		return MINIMM_ERROR_ADDRESS_IN_USE;
	}
	if (snapshot->count == SIZE_MAX) {
		return MINIMM_ERROR_NO_SPACE;
	}
	new_count = snapshot->count + 1U;

	status = minimm_vma_next_generation(snapshot, &generation);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_vma_allocate_ranges(new_count, &ranges);
	if (status != MINIMM_OK) {
		return status;
	}

	if (position > 0U) {
		(void)memcpy(ranges, snapshot->ranges, position * sizeof(*ranges));
	}
	ranges[position] = *mapping;
	if (position < snapshot->count) {
		(void)memcpy(ranges + position + 1U, snapshot->ranges + position,
			     (snapshot->count - position) * sizeof(*ranges));
	}

	return minimm_vma_snapshot_from_ranges(ranges, new_count, generation, out_snapshot);
}

minimm_status_t minimm_vma_snapshot_remove(const minimm_vma_snapshot_t *snapshot,
					   minimm_vaddr_t start, minimm_vaddr_t end,
					   minimm_vma_snapshot_t **out_snapshot)
{
	minimm_vma_t *ranges = NULL;
	size_t output_count = 0U;
	size_t output_index = 0U;
	size_t index = 0U;
	uint64_t generation = 0U;
	minimm_status_t status = MINIMM_OK;

	if (out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_snapshot = NULL;
	if (snapshot == NULL || !minimm_vma_page_range_is_valid(start, end)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	for (index = 0U; index < snapshot->count; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];
		size_t additions = 0U;

		if (mapping->end <= start || mapping->start >= end) {
			additions = 1U;
		} else {
			additions =
				(mapping->start < start ? 1U : 0U) + (mapping->end > end ? 1U : 0U);
		}
		if (output_count > SIZE_MAX - additions) {
			return MINIMM_ERROR_NO_SPACE;
		}
		output_count += additions;
	}

	status = minimm_vma_next_generation(snapshot, &generation);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_vma_allocate_ranges(output_count, &ranges);
	if (status != MINIMM_OK) {
		return status;
	}
	if (output_count == 0U) {
		return minimm_vma_snapshot_from_ranges(NULL, 0U, generation, out_snapshot);
	}

	for (index = 0U; index < snapshot->count; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];

		if (mapping->end <= start || mapping->start >= end) {
			ranges[output_index] = *mapping;
			output_index += 1U;
			continue;
		}
		if (mapping->start < start) {
			status = minimm_vma_fragment(mapping, mapping->start, start,
						     MINIMM_VMA_PROT_NONE, false,
						     &ranges[output_index]);
			if (status != MINIMM_OK) {
				free(ranges);
				return status;
			}
			output_index += 1U;
		}
		if (mapping->end > end) {
			status = minimm_vma_fragment(mapping, end, mapping->end,
						     MINIMM_VMA_PROT_NONE, false,
						     &ranges[output_index]);
			if (status != MINIMM_OK) {
				free(ranges);
				return status;
			}
			output_index += 1U;
		}
	}

	return minimm_vma_snapshot_from_ranges(ranges, output_count, generation, out_snapshot);
}

minimm_status_t minimm_vma_snapshot_protect(const minimm_vma_snapshot_t *snapshot,
					    minimm_vaddr_t start, minimm_vaddr_t end,
					    minimm_vma_prot_t protection,
					    minimm_vma_snapshot_t **out_snapshot)
{
	minimm_vma_t *ranges = NULL;
	minimm_vaddr_t covered_until = start;
	size_t output_count = 0U;
	size_t output_index = 0U;
	size_t index = 0U;
	uint64_t generation = 0U;
	minimm_status_t status = MINIMM_OK;

	if (out_snapshot == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_snapshot = NULL;
	if (snapshot == NULL || !minimm_vma_page_range_is_valid(start, end) ||
	    !minimm_vma_protection_is_valid(protection)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	for (index = 0U; index < snapshot->count && covered_until < end; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];

		if (mapping->end <= covered_until) {
			continue;
		}
		if (mapping->start > covered_until || mapping->start >= end) {
			return MINIMM_ERROR_NOT_FOUND;
		}
		if ((protection & ~mapping->max_prot) != UINT32_C(0)) {
			return MINIMM_ERROR_PERMISSION;
		}
		covered_until = mapping->end < end ? mapping->end : end;
	}
	if (covered_until != end) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	for (index = 0U; index < snapshot->count; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];
		size_t additions = 0U;

		if (mapping->end <= start || mapping->start >= end) {
			additions = 1U;
		} else {
			additions = 1U + (mapping->start < start ? 1U : 0U) +
				    (mapping->end > end ? 1U : 0U);
		}
		if (output_count > SIZE_MAX - additions) {
			return MINIMM_ERROR_NO_SPACE;
		}
		output_count += additions;
	}

	status = minimm_vma_next_generation(snapshot, &generation);
	if (status != MINIMM_OK) {
		return status;
	}
	status = minimm_vma_allocate_ranges(output_count, &ranges);
	if (status != MINIMM_OK) {
		return status;
	}

	for (index = 0U; index < snapshot->count; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];
		const minimm_vaddr_t middle_start = mapping->start > start ? mapping->start : start;
		const minimm_vaddr_t middle_end = mapping->end < end ? mapping->end : end;

		if (mapping->end <= start || mapping->start >= end) {
			ranges[output_index] = *mapping;
			output_index += 1U;
			continue;
		}
		if (mapping->start < middle_start) {
			status = minimm_vma_fragment(mapping, mapping->start, middle_start,
						     MINIMM_VMA_PROT_NONE, false,
						     &ranges[output_index]);
			if (status != MINIMM_OK) {
				free(ranges);
				return status;
			}
			output_index += 1U;
		}
		status = minimm_vma_fragment(mapping, middle_start, middle_end, protection, true,
					     &ranges[output_index]);
		if (status != MINIMM_OK) {
			free(ranges);
			return status;
		}
		output_index += 1U;
		if (mapping->end > middle_end) {
			status = minimm_vma_fragment(mapping, middle_end, mapping->end,
						     MINIMM_VMA_PROT_NONE, false,
						     &ranges[output_index]);
			if (status != MINIMM_OK) {
				free(ranges);
				return status;
			}
			output_index += 1U;
		}
	}

	return minimm_vma_snapshot_from_ranges(ranges, output_count, generation, out_snapshot);
}

static bool minimm_vma_align_up(minimm_vaddr_t value, uint64_t alignment, minimm_vaddr_t *out_value)
{
	const uint64_t mask = alignment - UINT64_C(1);

	if (value > UINT64_MAX - mask) {
		return false;
	}
	*out_value = (value + mask) & ~mask;
	return true;
}

minimm_status_t minimm_vma_snapshot_find_gap(const minimm_vma_snapshot_t *snapshot,
					     minimm_vaddr_t lower_bound, minimm_vaddr_t upper_bound,
					     uint64_t length, uint64_t alignment,
					     minimm_vaddr_t *out_address)
{
	minimm_vaddr_t candidate = 0U;
	size_t index = 0U;

	if (out_address == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_address = MINIMM_ADDRESS_AUTO;
	if (snapshot == NULL || lower_bound >= upper_bound ||
	    upper_bound > MINIMM_USER_ADDRESS_LIMIT || length == UINT64_C(0) ||
	    (length & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    alignment < MINIMM_PAGE_SIZE ||
	    (alignment & (alignment - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (!minimm_vma_align_up(lower_bound, alignment, &candidate)) {
		return MINIMM_ERROR_NO_SPACE;
	}

	for (index = 0U; index < snapshot->count; ++index) {
		const minimm_vma_t *mapping = &snapshot->ranges[index];

		if (mapping->end <= lower_bound) {
			continue;
		}
		if (mapping->start >= upper_bound) {
			break;
		}
		if (candidate < mapping->start && length <= mapping->start - candidate) {
			*out_address = candidate;
			return MINIMM_OK;
		}
		if (candidate < mapping->end &&
		    !minimm_vma_align_up(mapping->end, alignment, &candidate)) {
			return MINIMM_ERROR_NO_SPACE;
		}
	}

	if (candidate <= upper_bound && length <= upper_bound - candidate) {
		*out_address = candidate;
		return MINIMM_OK;
	}
	return MINIMM_ERROR_NO_SPACE;
}
