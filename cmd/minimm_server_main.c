#define _POSIX_C_SOURCE 200809L

#include "minimm/server.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMM_SERVER_MAX_NOTE_SIZE ((uint64_t)INT64_MAX & ~(MINIMM_PAGE_SIZE - UINT64_C(1)))

enum {
	MINIMM_SERVER_PARSE_RUN = -1,
	MINIMM_SERVER_EXIT_SUCCESS = 0,
	MINIMM_SERVER_EXIT_OPERATION = 1,
	MINIMM_SERVER_EXIT_USAGE = 2
};

static void minimm_server_usage(FILE *stream, const char *program)
{
	(void)fprintf(stream,
		      "Usage: %s [OPTIONS]\n"
		      "Run the MiniMM note service in the foreground.\n"
		      "Options:\n"
		      "  --bind ADDRESS                Bind address (default: 127.0.0.1)\n"
		      "  --port PORT                    TCP port (default: 7331; 0: automatic)\n"
		      "  --max-clients COUNT            Concurrent client limit (default: 32)\n"
		      "  --max-notes COUNT              Live note limit (default: 1024)\n"
		      "  --max-note-size BYTES          Per-note limit (default: 67108864)\n"
		      "  --max-total-note-size BYTES    Total note limit (default: 268435456)\n"
		      "  --memory-pages COUNT           Resident page budget (default: 16384)\n"
		      "  --timeout-ms MS                Socket timeout (default: 30000)\n"
		      "  --enable-private-preview       Enable loopback-only private previews\n"
		      "  --enable-stack-expand          Enable loopback-only stack expansion\n"
		      "  --enable-page-remap            Enable loopback-only page remapping\n"
		      "  --enable-mseal-merge            Enable loopback-only mseal merging\n"
		      "  --enable-mglru-reparent          Enable loopback-only MGLRU reparenting\n"
		      "  --version\n"
		      "  --help\n",
		      program);
}

static bool minimm_server_parse_uint(const char *text, uintmax_t maximum, uintmax_t *out_value)
{
	const char *cursor = text;
	char *end = NULL;
	uintmax_t value = 0U;

	if (text == NULL || out_value == NULL || *text == '\0') {
		return false;
	}
	while (*cursor != '\0') {
		if (*cursor < '0' || *cursor > '9') {
			return false;
		}
		cursor += 1;
	}

	errno = 0;
	value = strtoumax(text, &end, 10);
	if (errno == ERANGE || end == text || *end != '\0' || value > maximum) {
		return false;
	}
	*out_value = value;
	return true;
}

static bool minimm_server_option_takes_value(const char *option)
{
	return strcmp(option, "--bind") == 0 || strcmp(option, "--port") == 0 ||
	       strcmp(option, "--max-clients") == 0 || strcmp(option, "--max-notes") == 0 ||
	       strcmp(option, "--max-note-size") == 0 ||
	       strcmp(option, "--max-total-note-size") == 0 ||
	       strcmp(option, "--memory-pages") == 0 || strcmp(option, "--timeout-ms") == 0;
}

static int minimm_server_usage_error(const char *message)
{
	(void)fprintf(stderr, "minimm-server: %s\n", message);
	return MINIMM_SERVER_EXIT_USAGE;
}

static int minimm_server_operation_error(const char *operation, minimm_status_t status)
{
	(void)fprintf(stderr, "minimm-server: %s: %s\n", operation, minimm_status_string(status));
	return MINIMM_SERVER_EXIT_OPERATION;
}

static int minimm_server_finish_output(int result)
{
	if (result == MINIMM_SERVER_EXIT_SUCCESS &&
	    (fflush(stdout) == EOF || ferror(stdout) != 0)) {
		(void)fprintf(stderr, "minimm-server: write output failed\n");
		return MINIMM_SERVER_EXIT_OPERATION;
	}
	return result;
}

static int minimm_server_system_error(const char *operation, int error_number)
{
	(void)fprintf(stderr, "minimm-server: %s: %s\n", operation, strerror(error_number));
	return MINIMM_SERVER_EXIT_OPERATION;
}

static int minimm_server_parse_options(int argc, char **argv, minimm_server_config_t *config)
{
	int index = 1;

	while (index < argc) {
		const char *option = argv[index];
		uintmax_t value = 0U;

		if (strcmp(option, "--help") == 0) {
			minimm_server_usage(stdout, argv[0]);
			return minimm_server_finish_output(MINIMM_SERVER_EXIT_SUCCESS);
		}
		if (strcmp(option, "--version") == 0) {
			if (printf("minimm-server %s\n", minimm_version()) < 0) {
				return minimm_server_operation_error("write output",
								     MINIMM_ERROR_IO);
			}
			return minimm_server_finish_output(MINIMM_SERVER_EXIT_SUCCESS);
		}
		if (strcmp(option, "--enable-private-preview") == 0) {
			config->enable_private_preview = true;
			index += 1;
			continue;
		}
		if (strcmp(option, "--enable-stack-expand") == 0) {
			config->enable_stack_expand = true;
			index += 1;
			continue;
		}
		if (strcmp(option, "--enable-page-remap") == 0) {
			config->enable_page_remap = true;
			index += 1;
			continue;
		}
		if (strcmp(option, "--enable-mseal-merge") == 0) {
			config->enable_mseal_merge = true;
			index += 1;
			continue;
		}
		if (strcmp(option, "--enable-mglru-reparent") == 0) {
			config->enable_mglru_reparent = true;
			index += 1;
			continue;
		}
		if (strncmp(option, "--", 2U) != 0) {
			minimm_server_usage(stderr, argv[0]);
			return minimm_server_usage_error("unexpected positional argument");
		}
		if (!minimm_server_option_takes_value(option)) {
			minimm_server_usage(stderr, argv[0]);
			return minimm_server_usage_error("unknown option");
		}
		if (index + 1 >= argc) {
			minimm_server_usage(stderr, argv[0]);
			return minimm_server_usage_error("missing option value");
		}

		if (strcmp(option, "--bind") == 0) {
			if (argv[index + 1][0] == '\0') {
				return minimm_server_usage_error("--bind must not be empty");
			}
			config->bind_address = argv[index + 1];
		} else if (strcmp(option, "--port") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1], UINT16_MAX, &value)) {
				return minimm_server_usage_error("invalid --port");
			}
			config->port = (uint16_t)value;
		} else if (strcmp(option, "--max-clients") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1], SIZE_MAX, &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --max-clients");
			}
			config->max_clients = (size_t)value;
		} else if (strcmp(option, "--max-notes") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1], SIZE_MAX, &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --max-notes");
			}
			config->max_notes = (size_t)value;
		} else if (strcmp(option, "--max-note-size") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1],
						      (uintmax_t)MINIMM_SERVER_MAX_NOTE_SIZE,
						      &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --max-note-size");
			}
			config->max_note_size = (uint64_t)value;
		} else if (strcmp(option, "--max-total-note-size") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1], UINT64_MAX, &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --max-total-note-size");
			}
			config->max_total_note_size = (uint64_t)value;
		} else if (strcmp(option, "--memory-pages") == 0) {
			const size_t page_size = (size_t)MINIMM_PAGE_SIZE;
			const uintmax_t maximum_pages = (uintmax_t)(SIZE_MAX / page_size);

			if (!minimm_server_parse_uint(argv[index + 1], maximum_pages, &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --memory-pages");
			}
			config->memory.physical_memory_size = (size_t)value * page_size;
		} else if (strcmp(option, "--timeout-ms") == 0) {
			if (!minimm_server_parse_uint(argv[index + 1], UINT32_MAX, &value) ||
			    value == 0U) {
				return minimm_server_usage_error("invalid --timeout-ms");
			}
			config->io_timeout_ms = (uint32_t)value;
		} else {
			return minimm_server_usage_error("unknown option");
		}
		index += 2;
	}
	if ((config->max_note_size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return minimm_server_usage_error("--max-note-size must be page aligned");
	}
	if ((config->max_total_note_size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return minimm_server_usage_error("--max-total-note-size must be page aligned");
	}
	if (config->max_total_note_size < config->max_note_size) {
		return minimm_server_usage_error(
			"--max-total-note-size must not be smaller than --max-note-size");
	}
	return MINIMM_SERVER_PARSE_RUN;
}

int main(int argc, char **argv)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	sigset_t termination_signals;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_SERVER_EXIT_SUCCESS;
	int signal_number = 0;
	int system_status = 0;

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		return minimm_server_system_error("ignore SIGPIPE", errno);
	}
	result = minimm_server_parse_options(argc, argv, &config);
	if (result != MINIMM_SERVER_PARSE_RUN) {
		return result;
	}
	result = MINIMM_SERVER_EXIT_SUCCESS;

	if (sigemptyset(&termination_signals) != 0 ||
	    sigaddset(&termination_signals, SIGINT) != 0 ||
	    sigaddset(&termination_signals, SIGTERM) != 0) {
		return minimm_server_system_error("prepare signal set", errno);
	}
	system_status = pthread_sigmask(SIG_BLOCK, &termination_signals, NULL);
	if (system_status != 0) {
		return minimm_server_system_error("block signals", system_status);
	}

	status = minimm_server_create(&config, &server);
	if (status != MINIMM_OK) {
		result = minimm_server_operation_error("create", status);
	} else {
		status = minimm_server_start(server);
		if (status != MINIMM_OK) {
			result = minimm_server_operation_error("start", status);
		} else {
			const uint16_t bound_port = minimm_server_bound_port(server);

			if (printf("listening address=%s port=%" PRIu16 "\n", config.bind_address,
				   bound_port) < 0 ||
			    fflush(stdout) != 0) {
				(void)fprintf(stderr, "minimm-server: write output failed\n");
				result = MINIMM_SERVER_EXIT_OPERATION;
			} else {
				system_status = sigwait(&termination_signals, &signal_number);
				if (system_status != 0) {
					result = minimm_server_system_error("wait for signal",
									    system_status);
				}
			}

			status = minimm_server_stop(server);
			if (status != MINIMM_OK && result == MINIMM_SERVER_EXIT_SUCCESS) {
				result = minimm_server_operation_error("stop", status);
			}
		}
	}

	if (server != NULL) {
		minimm_server_destroy(server);
	}
	return minimm_server_finish_output(result);
}
