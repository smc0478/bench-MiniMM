#include "vma_tree.h"

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

static minimm_vma_t make_mapping(minimm_vaddr_t start, minimm_vaddr_t end, uint64_t cookie,
				 uint64_t note_offset, minimm_vma_prot_t protection,
				 minimm_vma_prot_t maximum, minimm_vma_flags_t flags)
{
	minimm_vma_t mapping = { 0 };

	mapping.start = start;
	mapping.end = end;
	mapping.mapping_cookie = cookie;
	mapping.note_offset = note_offset;
	mapping.prot = protection;
	mapping.max_prot = maximum;
	mapping.flags = flags;
	return mapping;
}

static bool test_multilevel_and_rollback(void)
{
	const minimm_vaddr_t base = UINT64_C(0x100000);
	const minimm_vma_prot_t read_write = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE;
	minimm_vma_snapshot_t *snapshot = NULL;
	size_t index = 0U;

	if (!check((uint32_t)MINIMM_VMA_FLAG_SHARED == (uint32_t)MINIMM_MAP_SHARED &&
			   (uint32_t)MINIMM_VMA_FLAG_PRIVATE == (uint32_t)MINIMM_MAP_PRIVATE,
		   "VMA sharing flags match the public API") ||
	    !check(minimm_vma_snapshot_create(&snapshot) == MINIMM_OK,
		   "create empty VMA snapshot") ||
	    !check(minimm_vma_snapshot_count(snapshot) == 0U, "empty count") ||
	    !check(minimm_vma_snapshot_generation(snapshot) == UINT64_C(1), "initial generation") ||
	    !check(minimm_vma_snapshot_lookup(snapshot, base) == NULL, "empty lookup")) {
		minimm_vma_snapshot_destroy(snapshot);
		return false;
	}

	for (index = 0U; index < 20U; ++index) {
		const minimm_vaddr_t start = base + ((minimm_vaddr_t)index * UINT64_C(0x3000));
		const minimm_vma_t mapping =
			make_mapping(start, start + MINIMM_PAGE_SIZE, (uint64_t)index + UINT64_C(1),
				     (uint64_t)index * MINIMM_PAGE_SIZE, read_write,
				     read_write | MINIMM_VMA_PROT_EDIT, MINIMM_VMA_FLAG_PRIVATE);
		minimm_vma_snapshot_t *next = NULL;

		if (!check(minimm_vma_snapshot_insert(snapshot, &mapping, &next) == MINIMM_OK,
			   "insert sorted VMA")) {
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
		if (index == 0U && (!check(minimm_vma_snapshot_count(snapshot) == 0U,
					   "insert leaves source snapshot unchanged") ||
				    !check(minimm_vma_snapshot_lookup(snapshot, start) == NULL,
					   "source snapshot keeps old lookup view"))) {
			minimm_vma_snapshot_destroy(next);
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
		minimm_vma_snapshot_destroy(snapshot);
		snapshot = next;
	}

	if (!check(minimm_vma_snapshot_count(snapshot) == 20U,
		   "fanout overflow keeps all ranges") ||
	    !check(minimm_vma_snapshot_generation(snapshot) == UINT64_C(21),
		   "each transaction advances generation")) {
		minimm_vma_snapshot_destroy(snapshot);
		return false;
	}

	{
		minimm_vma_snapshot_t *clone = NULL;

		if (!check(minimm_vma_snapshot_clone(snapshot, &clone) == MINIMM_OK,
			   "clone immutable VMA snapshot") ||
		    !check(clone != snapshot && minimm_vma_snapshot_count(clone) == 20U &&
				   minimm_vma_snapshot_generation(clone) == UINT64_C(21),
			   "clone preserves independent metadata") ||
		    !check(minimm_vma_snapshot_contains_cookie(clone, UINT64_C(17)) &&
				   !minimm_vma_snapshot_contains_cookie(clone, UINT64_C(200)),
			   "mapping cookies are discoverable")) {
			minimm_vma_snapshot_destroy(clone);
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
		minimm_vma_snapshot_destroy(clone);
	}

	for (index = 0U; index < 20U; ++index) {
		const minimm_vaddr_t start = base + ((minimm_vaddr_t)index * UINT64_C(0x3000));
		const minimm_vma_t *found =
			minimm_vma_snapshot_lookup(snapshot, start + UINT64_C(97));

		if (!check(found != NULL, "multi-level tree lookup") ||
		    !check(found->mapping_cookie == (uint64_t)index + UINT64_C(1),
			   "lookup returns matching range metadata") ||
		    !check(minimm_vma_snapshot_lookup(snapshot, start + MINIMM_PAGE_SIZE) == NULL,
			   "half-open end and gap are not mapped")) {
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
	}

	{
		const uint64_t old_generation = minimm_vma_snapshot_generation(snapshot);
		const minimm_vma_t overlap =
			make_mapping(base + UINT64_C(0x3000), base + UINT64_C(0x5000),
				     UINT64_C(999), UINT64_C(0), MINIMM_VMA_PROT_READ,
				     MINIMM_VMA_PROT_READ, MINIMM_VMA_FLAG_PRIVATE);
		minimm_vma_snapshot_t *failed = NULL;

		if (!check(minimm_vma_snapshot_insert(snapshot, &overlap, &failed) ==
				   MINIMM_ERROR_ADDRESS_IN_USE,
			   "overlapping insert fails") ||
		    !check(failed == NULL, "failed transaction returns no snapshot") ||
		    !check(minimm_vma_snapshot_count(snapshot) == 20U &&
				   minimm_vma_snapshot_generation(snapshot) == old_generation,
			   "failed insert rolls back completely")) {
			minimm_vma_snapshot_destroy(failed);
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
	}

	{
		minimm_vma_t unaligned_offset =
			make_mapping(base + UINT64_C(0x50000), base + UINT64_C(0x51000),
				     UINT64_C(1000), UINT64_C(1), MINIMM_VMA_PROT_READ,
				     MINIMM_VMA_PROT_READ, MINIMM_VMA_FLAG_PRIVATE);
		minimm_vma_snapshot_t *failed = NULL;

		if (!check(minimm_vma_snapshot_insert(snapshot, &unaligned_offset, &failed) ==
				   MINIMM_ERROR_INVALID_ARGUMENT,
			   "file-backed offsets must be page aligned") ||
		    !check(failed == NULL, "invalid offset preserves the snapshot")) {
			minimm_vma_snapshot_destroy(failed);
			minimm_vma_snapshot_destroy(snapshot);
			return false;
		}
	}

	minimm_vma_snapshot_destroy(snapshot);
	return true;
}

static bool test_remove_split(void)
{
	const minimm_vma_prot_t maximum = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE |
					  MINIMM_VMA_PROT_EDIT;
	const minimm_vma_t mapping = make_mapping(UINT64_C(0x10000), UINT64_C(0x16000),
						  UINT64_C(71), UINT64_C(0x8000),
						  MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE,
						  maximum, MINIMM_VMA_FLAG_SHARED);
	minimm_vma_snapshot_t *empty = NULL;
	minimm_vma_snapshot_t *original = NULL;
	minimm_vma_snapshot_t *removed = NULL;
	minimm_vma_snapshot_t *removed_all = NULL;
	const minimm_vma_t *left = NULL;
	const minimm_vma_t *right = NULL;

	if (!check(minimm_vma_snapshot_create(&empty) == MINIMM_OK, "create remove fixture") ||
	    !check(minimm_vma_snapshot_insert(empty, &mapping, &original) == MINIMM_OK,
		   "insert remove fixture") ||
	    !check(minimm_vma_snapshot_remove(original, UINT64_C(0x12000), UINT64_C(0x14000),
					      &removed) == MINIMM_OK,
		   "remove middle of mapping")) {
		minimm_vma_snapshot_destroy(removed);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	left = minimm_vma_snapshot_lookup(removed, UINT64_C(0x11000));
	right = minimm_vma_snapshot_lookup(removed, UINT64_C(0x14000));
	if (!check(minimm_vma_snapshot_count(removed) == 2U, "remove splits VMA") ||
	    !check(left != NULL && left->end == UINT64_C(0x12000), "left split") ||
	    !check(right != NULL && right->start == UINT64_C(0x14000), "right split") ||
	    !check(right != NULL && right->note_offset == UINT64_C(0xc000),
		   "right split advances note offset") ||
	    !check(minimm_vma_snapshot_lookup(removed, UINT64_C(0x13000)) == NULL,
		   "removed middle is a hole") ||
	    !check(minimm_vma_snapshot_lookup(original, UINT64_C(0x13000)) != NULL,
		   "remove preserves original snapshot")) {
		minimm_vma_snapshot_destroy(removed);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}
	if (!check(minimm_vma_snapshot_remove(original, mapping.start, mapping.end, &removed_all) ==
			   MINIMM_OK,
		   "remove complete mapping") ||
	    !check(minimm_vma_snapshot_count(removed_all) == 0U &&
			   minimm_vma_snapshot_lookup(removed_all, mapping.start) == NULL,
		   "complete removal creates an empty snapshot")) {
		minimm_vma_snapshot_destroy(removed_all);
		minimm_vma_snapshot_destroy(removed);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	minimm_vma_snapshot_destroy(removed_all);
	minimm_vma_snapshot_destroy(removed);
	minimm_vma_snapshot_destroy(original);
	minimm_vma_snapshot_destroy(empty);
	return true;
}

static bool test_protect_split_and_rollback(void)
{
	const minimm_vma_prot_t read_write = MINIMM_VMA_PROT_READ | MINIMM_VMA_PROT_WRITE;
	const minimm_vma_t mapping = make_mapping(UINT64_C(0x20000), UINT64_C(0x26000),
						  UINT64_C(81), UINT64_C(0x1000), read_write,
						  read_write, MINIMM_VMA_FLAG_PRIVATE);
	minimm_vma_snapshot_t *empty = NULL;
	minimm_vma_snapshot_t *original = NULL;
	minimm_vma_snapshot_t *protected_snapshot = NULL;
	minimm_vma_snapshot_t *restored = NULL;
	minimm_vma_snapshot_t *failed = NULL;
	const minimm_vma_t *left = NULL;
	const minimm_vma_t *middle = NULL;
	const minimm_vma_t *right = NULL;

	if (!check(minimm_vma_snapshot_create(&empty) == MINIMM_OK, "create protect fixture") ||
	    !check(minimm_vma_snapshot_insert(empty, &mapping, &original) == MINIMM_OK,
		   "insert protect fixture") ||
	    !check(minimm_vma_snapshot_protect(original, UINT64_C(0x21000), UINT64_C(0x25000),
					       MINIMM_VMA_PROT_READ,
					       &protected_snapshot) == MINIMM_OK,
		   "protect middle of mapping")) {
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	left = minimm_vma_snapshot_lookup(protected_snapshot, UINT64_C(0x20000));
	middle = minimm_vma_snapshot_lookup(protected_snapshot, UINT64_C(0x22000));
	right = minimm_vma_snapshot_lookup(protected_snapshot, UINT64_C(0x25000));
	if (!check(minimm_vma_snapshot_count(protected_snapshot) == 3U,
		   "protect creates three ranges") ||
	    !check(left != NULL && left->prot == read_write, "left keeps rights") ||
	    !check(middle != NULL && middle->prot == MINIMM_VMA_PROT_READ &&
			   middle->start == UINT64_C(0x21000) && middle->end == UINT64_C(0x25000),
		   "middle receives new rights") ||
	    !check(middle != NULL && middle->note_offset == UINT64_C(0x2000),
		   "protected split advances note offset") ||
	    !check(right != NULL && right->prot == read_write &&
			   right->note_offset == UINT64_C(0x6000),
		   "right keeps rights and adjusted offset") ||
	    !check(minimm_vma_snapshot_lookup(original, UINT64_C(0x22000))->prot == read_write,
		   "protect leaves source rights unchanged")) {
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	if (!check(minimm_vma_snapshot_protect(protected_snapshot, UINT64_C(0x21000),
					       UINT64_C(0x22000), MINIMM_VMA_PROT_EDIT,
					       &failed) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "edit permission requires write permission") ||
	    !check(failed == NULL, "failed protect returns no snapshot") ||
	    !check(minimm_vma_snapshot_count(protected_snapshot) == 3U,
		   "failed protect rolls back")) {
		minimm_vma_snapshot_destroy(failed);
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	if (!check(minimm_vma_snapshot_protect(protected_snapshot, UINT64_C(0x21000),
					       UINT64_C(0x22000), read_write | MINIMM_VMA_PROT_EDIT,
					       &failed) == MINIMM_ERROR_PERMISSION,
		   "protect cannot exceed maximum rights") ||
	    !check(failed == NULL, "rights escalation returns no snapshot")) {
		minimm_vma_snapshot_destroy(failed);
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	if (!check(minimm_vma_snapshot_protect(original, UINT64_C(0x1f000), UINT64_C(0x21000),
					       MINIMM_VMA_PROT_READ,
					       &failed) == MINIMM_ERROR_NOT_FOUND,
		   "protect requires complete mapped coverage") ||
	    !check(failed == NULL, "coverage failure is transactional")) {
		minimm_vma_snapshot_destroy(failed);
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	if (!check(minimm_vma_snapshot_protect(protected_snapshot, UINT64_C(0x21000),
					       UINT64_C(0x25000), read_write,
					       &restored) == MINIMM_OK,
		   "restore split protection") ||
	    !check(minimm_vma_snapshot_count(restored) == 1U,
		   "compatible adjacent VMAs coalesce") ||
	    !check(minimm_vma_snapshot_lookup(restored, UINT64_C(0x22000))->start ==
				   UINT64_C(0x20000) &&
			   minimm_vma_snapshot_lookup(restored, UINT64_C(0x22000))->end ==
				   UINT64_C(0x26000),
		   "coalesced VMA restores original range")) {
		minimm_vma_snapshot_destroy(restored);
		minimm_vma_snapshot_destroy(protected_snapshot);
		minimm_vma_snapshot_destroy(original);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	minimm_vma_snapshot_destroy(restored);
	minimm_vma_snapshot_destroy(protected_snapshot);
	minimm_vma_snapshot_destroy(original);
	minimm_vma_snapshot_destroy(empty);
	return true;
}

static bool test_aligned_first_fit(void)
{
	const minimm_vma_t first = make_mapping(UINT64_C(0x1000), UINT64_C(0x3000), UINT64_C(1),
						UINT64_C(0), MINIMM_VMA_PROT_READ,
						MINIMM_VMA_PROT_READ, MINIMM_VMA_FLAG_PRIVATE);
	const minimm_vma_t second = make_mapping(UINT64_C(0x5000), UINT64_C(0x6000), UINT64_C(2),
						 UINT64_C(0), MINIMM_VMA_PROT_READ,
						 MINIMM_VMA_PROT_READ, MINIMM_VMA_FLAG_PRIVATE);
	minimm_vma_snapshot_t *empty = NULL;
	minimm_vma_snapshot_t *one = NULL;
	minimm_vma_snapshot_t *two = NULL;
	const minimm_vma_t *next = NULL;
	minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;

	if (!check(minimm_vma_snapshot_create(&empty) == MINIMM_OK, "create gap fixture") ||
	    !check(minimm_vma_snapshot_insert(empty, &first, &one) == MINIMM_OK,
		   "insert first gap boundary") ||
	    !check(minimm_vma_snapshot_insert(one, &second, &two) == MINIMM_OK,
		   "insert second gap boundary") ||
	    !check(minimm_vma_snapshot_find_gap(two, UINT64_C(0x1000), UINT64_C(0x10000),
						UINT64_C(0x2000), UINT64_C(0x4000),
						&address) == MINIMM_OK,
		   "find aligned first-fit gap") ||
	    !check(address == UINT64_C(0x8000), "alignment skips an insufficient earlier gap") ||
	    !check(minimm_vma_snapshot_find_gap(two, UINT64_C(0x9000), UINT64_C(0x10000),
						UINT64_C(0x8000), MINIMM_PAGE_SIZE,
						&address) == MINIMM_ERROR_NO_SPACE,
		   "gap search reports exhaustion") ||
	    !check(address == MINIMM_ADDRESS_AUTO, "failed gap search clears output") ||
	    !check((next = minimm_vma_snapshot_find_next(two, UINT64_C(0x2000))) != NULL &&
			   next->mapping_cookie == first.mapping_cookie,
		   "next lookup returns a mapping containing the cursor") ||
	    !check((next = minimm_vma_snapshot_find_next(two, UINT64_C(0x3000))) != NULL &&
			   next->mapping_cookie == second.mapping_cookie,
		   "next lookup skips directly across a hole") ||
	    !check(minimm_vma_snapshot_find_next(two, second.end) == NULL,
		   "next lookup reports exhaustion after the final mapping")) {
		minimm_vma_snapshot_destroy(two);
		minimm_vma_snapshot_destroy(one);
		minimm_vma_snapshot_destroy(empty);
		return false;
	}

	minimm_vma_snapshot_destroy(two);
	minimm_vma_snapshot_destroy(one);
	minimm_vma_snapshot_destroy(empty);
	return true;
}

int main(void)
{
	if (!test_multilevel_and_rollback() || !test_remove_split() ||
	    !test_protect_split_and_rollback() || !test_aligned_first_fit()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
