#include "minimm/minimm.h"

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

int main(void)
{
	minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;

	if (!check(MINIMM_PAGE_SIZE == 4096U, "page size is fixed at 4 KiB") ||
	    !check(MINIMM_PAGE_TABLE_LEVELS == 4U,
		   "virtual addresses use four page-table levels") ||
	    !check(minimm_create(&config, &mm) == MINIMM_OK, "create context") ||
	    !check(mm != NULL, "context is returned") ||
	    !check(minimm_physical_memory_size(mm) == config.physical_memory_size,
		   "physical-memory size is preserved") ||
	    !check(minimm_page_size(mm) == config.page_size, "page size is preserved") ||
	    !check(config.tlb_entries == 64U, "default TLB has 64 entries") ||
	    !check(minimm_page_count(mm) == config.physical_memory_size / config.page_size,
		   "page count is derived from the configuration")) {
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_destroy(mm);
	mm = NULL;

	config.page_size = 0U;
	if (!check(minimm_create(&config, &mm) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "zero page size is rejected") ||
	    !check(mm == NULL, "failed creation leaves a null output")) {
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
