#include "page_table.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct minimm_page_table_node {
	void *entries[MINIMM_PAGE_TABLE_ENTRIES];
} minimm_page_table_node_t;

struct minimm_page_table {
	minimm_page_table_node_t *root;
	size_t mapping_count;
	uint64_t generation;
};

static bool minimm_page_address_is_valid(minimm_vaddr_t address)
{
	return address < MINIMM_USER_ADDRESS_LIMIT && (address & (MINIMM_PAGE_SIZE - 1U)) == 0U;
}

static size_t minimm_page_table_index(minimm_vaddr_t address, unsigned level)
{
	const unsigned shift = MINIMM_PAGE_SHIFT + (level * 9U);

	return (size_t)((address >> shift) & UINT64_C(0x1ff));
}

static bool minimm_page_table_node_is_empty(const minimm_page_table_node_t *node)
{
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PAGE_TABLE_ENTRIES; ++index) {
		if (node->entries[index] != NULL) {
			return false;
		}
	}
	return true;
}

static void minimm_page_table_node_destroy(minimm_page_table_node_t *node, unsigned level)
{
	size_t index = 0U;

	if (node == NULL) {
		return;
	}

	for (index = 0U; index < MINIMM_PAGE_TABLE_ENTRIES; ++index) {
		if (node->entries[index] == NULL) {
			continue;
		}
		if (level == 0U) {
			minimm_pte_t *pte = node->entries[index];

			if ((pte->flags & MINIMM_PTE_LOCKED) != 0U) {
				minimm_frame_unpin(pte->frame);
			}
			minimm_frame_unmap(pte->frame);
			minimm_frame_release(pte->frame);
			free(pte);
		} else {
			minimm_page_table_node_destroy(node->entries[index], level - 1U);
		}
	}
	free(node);
}

static minimm_pte_t *minimm_page_table_lookup_internal(const minimm_page_table_t *table,
						       minimm_vaddr_t address)
{
	minimm_page_table_node_t *node = NULL;
	unsigned level = 0U;

	if (table == NULL || table->root == NULL || address >= MINIMM_USER_ADDRESS_LIMIT) {
		return NULL;
	}

	node = table->root;
	for (level = MINIMM_PAGE_TABLE_LEVELS - 1U; level > 0U; --level) {
		const size_t index = minimm_page_table_index(address, level);

		node = node->entries[index];
		if (node == NULL) {
			return NULL;
		}
	}

	return node->entries[minimm_page_table_index(address, 0U)];
}

static minimm_status_t minimm_page_table_visit_node(const minimm_page_table_node_t *node,
						    unsigned level, minimm_vaddr_t address_prefix,
						    minimm_page_table_visitor_t visitor,
						    void *context)
{
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PAGE_TABLE_ENTRIES; ++index) {
		minimm_status_t status = MINIMM_OK;
		minimm_vaddr_t entry_address = 0U;
		const unsigned shift = MINIMM_PAGE_SHIFT + (level * 9U);

		if (node->entries[index] == NULL) {
			continue;
		}
		entry_address = address_prefix | ((minimm_vaddr_t)index << shift);
		if (level == 0U) {
			status = visitor(entry_address, node->entries[index], context);
		} else {
			status = minimm_page_table_visit_node(node->entries[index], level - 1U,
							      entry_address, visitor, context);
		}
		if (status != MINIMM_OK) {
			return status;
		}
	}
	return MINIMM_OK;
}

static bool minimm_page_table_find_next_node(const minimm_page_table_node_t *node, unsigned level,
					     minimm_vaddr_t address_prefix, minimm_vaddr_t start,
					     minimm_vaddr_t end, minimm_vaddr_t *out_page_address)
{
	const unsigned shift = MINIMM_PAGE_SHIFT + (level * 9U);
	const minimm_vaddr_t entry_span = UINT64_C(1) << shift;
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PAGE_TABLE_ENTRIES; ++index) {
		const minimm_vaddr_t entry_address = address_prefix |
						     ((minimm_vaddr_t)index << shift);
		const minimm_vaddr_t entry_end = entry_address + entry_span;

		if (entry_end <= start || entry_address >= end || node->entries[index] == NULL) {
			continue;
		}
		if (level == 0U) {
			*out_page_address = entry_address;
			return true;
		}
		if (minimm_page_table_find_next_node(node->entries[index], level - 1U,
						     entry_address, start, end, out_page_address)) {
			return true;
		}
	}
	return false;
}

static uint32_t minimm_page_table_effective_protection(uint32_t protection,
						       minimm_pte_flags_t flags)
{
	if ((flags & MINIMM_PTE_COW) != 0U) {
		protection &= ~(uint32_t)(MINIMM_PROT_WRITE | MINIMM_PROT_EDIT);
	}
	return protection;
}

static minimm_status_t minimm_page_table_commit_attributes(minimm_page_table_t *table,
							   minimm_pte_t *pte, uint32_t protection,
							   minimm_pte_flags_t set_flags,
							   minimm_pte_flags_t clear_flags)
{
	const minimm_pte_flags_t updated_flags = (pte->flags | set_flags) & ~clear_flags;
	const uint32_t effective_protection =
		minimm_page_table_effective_protection(protection, updated_flags);

	if (pte->protection == effective_protection && pte->flags == updated_flags) {
		return MINIMM_OK;
	}
	if ((pte->flags & MINIMM_PTE_LOCKED) == 0U && (updated_flags & MINIMM_PTE_LOCKED) != 0U) {
		minimm_frame_pin(pte->frame);
	} else if ((pte->flags & MINIMM_PTE_LOCKED) != 0U &&
		   (updated_flags & MINIMM_PTE_LOCKED) == 0U) {
		minimm_frame_unpin(pte->frame);
	}
	pte->protection = effective_protection;
	pte->flags = updated_flags;
	table->generation += 1U;
	return MINIMM_OK;
}

minimm_status_t minimm_page_table_create(minimm_page_table_t **out_table)
{
	minimm_page_table_t *table = NULL;

	if (out_table == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_table = NULL;

	table = calloc(1U, sizeof(*table));
	if (table == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	table->root = calloc(1U, sizeof(*table->root));
	if (table->root == NULL) {
		free(table);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	table->generation = 1U;

	*out_table = table;
	return MINIMM_OK;
}

void minimm_page_table_destroy(minimm_page_table_t *table)
{
	if (table == NULL) {
		return;
	}
	minimm_page_table_node_destroy(table->root, MINIMM_PAGE_TABLE_LEVELS - 1U);
	free(table);
}

minimm_status_t minimm_page_table_map(minimm_page_table_t *table, minimm_vaddr_t page_address,
				      minimm_frame_t *frame, uint32_t protection,
				      minimm_pte_flags_t flags)
{
	minimm_page_table_node_t *node = NULL;
	minimm_page_table_node_t *created_nodes[MINIMM_PAGE_TABLE_LEVELS - 1U] = { NULL };
	minimm_page_table_node_t *created_parents[MINIMM_PAGE_TABLE_LEVELS - 1U] = { NULL };
	size_t created_indexes[MINIMM_PAGE_TABLE_LEVELS - 1U] = { 0U };
	size_t created_count = 0U;
	minimm_pte_t *pte = NULL;
	unsigned level = 0U;

	if (table == NULL || frame == NULL || !minimm_page_address_is_valid(page_address)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (minimm_page_table_lookup_internal(table, page_address) != NULL) {
		return MINIMM_ERROR_ADDRESS_IN_USE;
	}

	node = table->root;
	for (level = MINIMM_PAGE_TABLE_LEVELS - 1U; level > 0U; --level) {
		const size_t index = minimm_page_table_index(page_address, level);

		if (node->entries[index] == NULL) {
			minimm_page_table_node_t *new_node = calloc(1U, sizeof(*new_node));

			if (new_node == NULL) {
				goto allocation_failed;
			}
			node->entries[index] = new_node;
			created_nodes[created_count] = new_node;
			created_parents[created_count] = node;
			created_indexes[created_count] = index;
			created_count += 1U;
		}
		node = node->entries[index];
	}

	pte = calloc(1U, sizeof(*pte));
	if (pte == NULL) {
		goto allocation_failed;
	}
	minimm_frame_retain(frame);
	if ((flags & MINIMM_PTE_LOCKED) != 0U) {
		minimm_frame_pin(frame);
	}
	pte->frame = frame;
	pte->flags = flags | MINIMM_PTE_PRESENT;
	pte->protection = minimm_page_table_effective_protection(protection, pte->flags);
	node->entries[minimm_page_table_index(page_address, 0U)] = pte;
	minimm_frame_map(frame);
	table->mapping_count += 1U;
	table->generation += 1U;
	return MINIMM_OK;

allocation_failed:
	while (created_count != 0U) {
		created_count -= 1U;
		free(created_nodes[created_count]);
		created_parents[created_count]->entries[created_indexes[created_count]] = NULL;
	}
	return MINIMM_ERROR_OUT_OF_MEMORY;
}

minimm_status_t minimm_page_table_unmap(minimm_page_table_t *table, minimm_vaddr_t page_address)
{
	minimm_page_table_node_t *nodes[MINIMM_PAGE_TABLE_LEVELS] = { NULL };
	size_t indexes[MINIMM_PAGE_TABLE_LEVELS] = { 0U };
	minimm_pte_t *pte = NULL;
	unsigned level = 0U;

	if (table == NULL || !minimm_page_address_is_valid(page_address)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	nodes[MINIMM_PAGE_TABLE_LEVELS - 1U] = table->root;
	for (level = MINIMM_PAGE_TABLE_LEVELS - 1U; level > 0U; --level) {
		indexes[level] = minimm_page_table_index(page_address, level);
		nodes[level - 1U] = nodes[level]->entries[indexes[level]];
		if (nodes[level - 1U] == NULL) {
			return MINIMM_ERROR_NOT_FOUND;
		}
	}

	indexes[0] = minimm_page_table_index(page_address, 0U);
	pte = nodes[0]->entries[indexes[0]];
	if (pte == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	nodes[0]->entries[indexes[0]] = NULL;
	if ((pte->flags & MINIMM_PTE_LOCKED) != 0U) {
		minimm_frame_unpin(pte->frame);
	}
	minimm_frame_unmap(pte->frame);
	minimm_frame_release(pte->frame);
	free(pte);
	table->mapping_count -= 1U;
	table->generation += 1U;

	for (level = 0U; level < MINIMM_PAGE_TABLE_LEVELS - 1U; ++level) {
		if (!minimm_page_table_node_is_empty(nodes[level])) {
			break;
		}
		free(nodes[level]);
		nodes[level + 1U]->entries[indexes[level + 1U]] = NULL;
	}

	return MINIMM_OK;
}

minimm_pte_t *minimm_page_table_lookup(minimm_page_table_t *table, minimm_vaddr_t address)
{
	return minimm_page_table_lookup_internal(table, address);
}

const minimm_pte_t *minimm_page_table_lookup_const(const minimm_page_table_t *table,
						   minimm_vaddr_t address)
{
	return minimm_page_table_lookup_internal(table, address);
}

minimm_status_t minimm_page_table_protect(minimm_page_table_t *table, minimm_vaddr_t page_address,
					  uint32_t protection)
{
	return minimm_page_table_update_attributes(table, page_address, protection, 0U, 0U);
}

minimm_status_t minimm_page_table_update_attributes(minimm_page_table_t *table,
						    minimm_vaddr_t page_address,
						    uint32_t protection,
						    minimm_pte_flags_t set_flags,
						    minimm_pte_flags_t clear_flags)
{
	minimm_pte_t *pte = NULL;

	if (table == NULL || !minimm_page_address_is_valid(page_address) ||
	    ((set_flags | clear_flags) & MINIMM_PTE_PRESENT) != 0U ||
	    (set_flags & clear_flags) != 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	pte = minimm_page_table_lookup_internal(table, page_address);
	if (pte == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	return minimm_page_table_commit_attributes(table, pte, protection, set_flags, clear_flags);
}

minimm_status_t minimm_page_table_replace_frame(minimm_page_table_t *table,
						minimm_vaddr_t page_address, minimm_frame_t *frame,
						uint32_t protection, minimm_pte_flags_t set_flags,
						minimm_pte_flags_t clear_flags)
{
	minimm_pte_t *pte = NULL;
	minimm_frame_t *old_frame = NULL;
	minimm_pte_flags_t updated_flags = 0U;
	bool was_locked = false;
	bool will_be_locked = false;

	if (table == NULL || frame == NULL || !minimm_page_address_is_valid(page_address) ||
	    ((set_flags | clear_flags) & MINIMM_PTE_PRESENT) != 0U ||
	    (set_flags & clear_flags) != 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	pte = minimm_page_table_lookup_internal(table, page_address);
	if (pte == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}

	minimm_frame_retain(frame);
	old_frame = pte->frame;
	updated_flags = (pte->flags | set_flags) & ~clear_flags;
	was_locked = (pte->flags & MINIMM_PTE_LOCKED) != 0U;
	will_be_locked = (updated_flags & MINIMM_PTE_LOCKED) != 0U;
	if (will_be_locked) {
		minimm_frame_pin(frame);
	}
	if (frame != old_frame) {
		minimm_frame_map(frame);
	}
	pte->frame = frame;
	pte->protection = minimm_page_table_effective_protection(protection, updated_flags);
	pte->flags = updated_flags;
	table->generation += 1U;
	if (was_locked) {
		minimm_frame_unpin(old_frame);
	}
	if (frame != old_frame) {
		minimm_frame_unmap(old_frame);
	}
	minimm_frame_release(old_frame);
	return MINIMM_OK;
}

minimm_status_t minimm_page_table_update_flags(minimm_page_table_t *table,
					       minimm_vaddr_t page_address,
					       minimm_pte_flags_t set_flags,
					       minimm_pte_flags_t clear_flags)
{
	minimm_pte_t *pte = NULL;

	if (table == NULL || !minimm_page_address_is_valid(page_address) ||
	    ((set_flags | clear_flags) & MINIMM_PTE_PRESENT) != 0U ||
	    (set_flags & clear_flags) != 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	pte = minimm_page_table_lookup_internal(table, page_address);
	if (pte == NULL) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	return minimm_page_table_commit_attributes(table, pte, pte->protection, set_flags,
						   clear_flags);
}

minimm_status_t minimm_page_table_for_each(const minimm_page_table_t *table,
					   minimm_page_table_visitor_t visitor, void *context)
{
	if (table == NULL || visitor == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_page_table_visit_node(table->root, MINIMM_PAGE_TABLE_LEVELS - 1U, 0U, visitor,
					    context);
}

minimm_status_t minimm_page_table_find_next(const minimm_page_table_t *table, minimm_vaddr_t start,
					    minimm_vaddr_t end, minimm_vaddr_t *out_page_address)
{
	if (table == NULL || out_page_address == NULL || start > end ||
	    end > MINIMM_USER_ADDRESS_LIMIT || (start & (MINIMM_PAGE_SIZE - 1U)) != 0U ||
	    (end & (MINIMM_PAGE_SIZE - 1U)) != 0U) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_page_address = 0U;
	if (start == end) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	if (!minimm_page_table_find_next_node(table->root, MINIMM_PAGE_TABLE_LEVELS - 1U, 0U, start,
					      end, out_page_address)) {
		return MINIMM_ERROR_NOT_FOUND;
	}
	return MINIMM_OK;
}

size_t minimm_page_table_mapping_count(const minimm_page_table_t *table)
{
	return table == NULL ? 0U : table->mapping_count;
}

uint64_t minimm_page_table_generation(const minimm_page_table_t *table)
{
	return table == NULL ? 0U : table->generation;
}
