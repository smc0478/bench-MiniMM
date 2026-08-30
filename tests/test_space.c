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
	const minimm_config_t config = minimm_config_default();
	minimm_t *mm = NULL;
	minimm_space_t *first = NULL;
	minimm_space_t *second = NULL;
	minimm_space_t *invalid_output = NULL;

	if (!check(minimm_create(&config, &mm) == MINIMM_OK, "create system") ||
	    !check(minimm_space_create(mm, &first) == MINIMM_OK, "create first address space") ||
	    !check(minimm_space_create(mm, &second) == MINIMM_OK, "create second address space")) {
		minimm_space_destroy(second);
		minimm_space_destroy(first);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	invalid_output = first;
	if (!check(minimm_space_create(NULL, &invalid_output) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null system") ||
	    !check(invalid_output == NULL, "invalid space creation clears output")) {
		minimm_space_destroy(second);
		minimm_space_destroy(first);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}
	invalid_output = second;
	if (!check(minimm_space_fork(NULL, &invalid_output) == MINIMM_ERROR_INVALID_ARGUMENT,
		   "reject a null fork parent") ||
	    !check(invalid_output == NULL, "invalid fork clears output")) {
		minimm_space_destroy(second);
		minimm_space_destroy(first);
		minimm_destroy(mm);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(first);
	first = NULL;

	/* A live address space keeps the system resources alive. */
	minimm_destroy(mm);
	if (!check(minimm_space_create(mm, &first) == MINIMM_ERROR_BUSY,
		   "closing system rejects new address spaces") ||
	    !check(first == NULL, "failed space creation clears output")) {
		minimm_space_destroy(second);
		return EXIT_FAILURE;
	}

	minimm_space_destroy(second);
	return EXIT_SUCCESS;
}
