#define _POSIX_C_SOURCE 200809L

#include "minimm/client.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINIMM_CLIENT_DEFAULT_HOST "127.0.0.1"
#define MINIMM_CLIENT_DEFAULT_PORT UINT16_C(7331)
#define MINIMM_CLIENT_MAX_READ_SIZE ((size_t)16U * 1024U * 1024U)
#define MINIMM_CLIENT_PING_NONCE UINT64_C(0x6d696e694d4d0001)

enum {
	MINIMM_CLIENT_EXIT_SUCCESS = 0,
	MINIMM_CLIENT_EXIT_OPERATION = 1,
	MINIMM_CLIENT_EXIT_USAGE = 2
};

typedef struct minimm_client_cli_options {
	const char *host;
	uint16_t port;
	uint32_t timeout_ms;
} minimm_client_cli_options_t;

static void minimm_client_usage(FILE *stream, const char *program)
{
	(void)fprintf(stream,
		      "Usage: %s [--host HOST] [--port PORT] [--timeout-ms MS] COMMAND\n"
		      "Commands:\n"
		      "  ping\n"
		      "  create SIZE [RIGHTS]\n"
		      "  copy TOKEN [RIGHTS]\n"
		      "  stat TOKEN\n"
		      "  read TOKEN OFFSET LENGTH\n"
		      "  write TOKEN OFFSET TEXT\n"
		      "  edit TOKEN OFFSET TEXT\n"
		      "  preview TOKEN OFFSET TEXT\n"
		      "  stack-expand TOKEN OFFSET TEXT\n"
		      "  remap-page TOKEN OFFSET\n"
		      "  mseal-merge TOKEN\n"
		      "  mglru-reparent TOKEN\n"
		      "  resize TOKEN SIZE\n"
		      "  flush TOKEN\n"
		      "  delete TOKEN\n"
		      "Use TOKEN '-' to read the capability from standard input.\n"
		      "RIGHTS is a combination of r,w,e,s,z,d; edit requires write.\n",
		      program);
}

static bool minimm_client_parse_uint(const char *text, uintmax_t maximum, uintmax_t *out_value)
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

static bool minimm_client_parse_rights(const char *text, minimm_remote_rights_t *out_rights)
{
	const char *cursor = text;
	minimm_remote_rights_t rights = 0U;

	if (text == NULL || out_rights == NULL || *text == '\0') {
		return false;
	}
	while (*cursor != '\0') {
		minimm_remote_rights_t right = 0U;

		switch (*cursor) {
		case 'r':
			right = MINIMM_REMOTE_RIGHT_READ;
			break;
		case 'w':
			right = MINIMM_REMOTE_RIGHT_WRITE;
			break;
		case 'e':
			right = MINIMM_REMOTE_RIGHT_EDIT;
			break;
		case 's':
			right = MINIMM_REMOTE_RIGHT_SHARE;
			break;
		case 'z':
			right = MINIMM_REMOTE_RIGHT_RESIZE;
			break;
		case 'd':
			right = MINIMM_REMOTE_RIGHT_DELETE;
			break;
		default:
			return false;
		}
		if ((rights & right) != 0U) {
			return false;
		}
		rights |= right;
		cursor += 1;
	}
	if ((rights & MINIMM_REMOTE_RIGHT_EDIT) != 0U &&
	    (rights & MINIMM_REMOTE_RIGHT_WRITE) == 0U) {
		return false;
	}
	*out_rights = rights;
	return true;
}

static void minimm_client_format_rights(minimm_remote_rights_t rights, char output[7])
{
	size_t index = 0U;

	if ((rights & MINIMM_REMOTE_RIGHT_READ) != 0U) {
		output[index++] = 'r';
	}
	if ((rights & MINIMM_REMOTE_RIGHT_WRITE) != 0U) {
		output[index++] = 'w';
	}
	if ((rights & MINIMM_REMOTE_RIGHT_EDIT) != 0U) {
		output[index++] = 'e';
	}
	if ((rights & MINIMM_REMOTE_RIGHT_SHARE) != 0U) {
		output[index++] = 's';
	}
	if ((rights & MINIMM_REMOTE_RIGHT_RESIZE) != 0U) {
		output[index++] = 'z';
	}
	if ((rights & MINIMM_REMOTE_RIGHT_DELETE) != 0U) {
		output[index++] = 'd';
	}
	if (index == 0U) {
		output[index++] = '-';
	}
	output[index] = '\0';
}

static void minimm_client_format_protection(minimm_prot_t protection, char output[5])
{
	size_t index = 0U;

	if ((protection & MINIMM_PROT_READ) != 0U) {
		output[index++] = 'r';
	}
	if ((protection & MINIMM_PROT_WRITE) != 0U) {
		output[index++] = 'w';
	}
	if ((protection & MINIMM_PROT_EDIT) != 0U) {
		output[index++] = 'e';
	}
	if ((protection & MINIMM_PROT_EXEC) != 0U) {
		output[index++] = 'x';
	}
	if (index == 0U) {
		output[index++] = '-';
	}
	output[index] = '\0';
}

static int minimm_client_usage_error(const char *message)
{
	(void)fprintf(stderr, "minimm-client: %s\n", message);
	return MINIMM_CLIENT_EXIT_USAGE;
}

static int minimm_client_operation_error(const char *operation, minimm_status_t status)
{
	(void)fprintf(stderr, "minimm-client: %s: %s\n", operation, minimm_status_string(status));
	return MINIMM_CLIENT_EXIT_OPERATION;
}

static int minimm_client_finish_output(int result)
{
	if (result == MINIMM_CLIENT_EXIT_SUCCESS &&
	    (fflush(stdout) == EOF || ferror(stdout) != 0)) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		return MINIMM_CLIENT_EXIT_OPERATION;
	}
	return result;
}

static int minimm_client_connect_cli(const minimm_client_cli_options_t *options,
				     minimm_client_t **out_client)
{
	const minimm_status_t status = minimm_client_connect(options->host, options->port,
							     options->timeout_ms, out_client);

	if (status != MINIMM_OK) {
		return minimm_client_operation_error("connect", status);
	}
	return MINIMM_CLIENT_EXIT_SUCCESS;
}

static int minimm_client_parse_token(const char *text, minimm_capability_t *out_capability)
{
	char input[MINIMM_CAPABILITY_HEX_BUFFER_SIZE + 2U] = { 0 };
	const char *token_text = text;
	size_t input_length = 0U;

	if (text != NULL && strcmp(text, "-") == 0) {
		if (fgets(input, (int)sizeof(input), stdin) == NULL) {
			return minimm_client_usage_error(
				"could not read TOKEN from standard input");
		}
		input_length = strcspn(input, "\r\n");
		input[input_length] = '\0';
		token_text = input;
	}
	const minimm_status_t status = minimm_capability_parse(token_text, out_capability);

	if (status != MINIMM_OK) {
		return minimm_client_usage_error(
			"TOKEN must contain exactly 32 hexadecimal characters");
	}
	return MINIMM_CLIENT_EXIT_SUCCESS;
}

static int minimm_client_close_note(minimm_client_t *client, minimm_remote_note_t *note)
{
	const minimm_status_t status = minimm_client_note_close(client, note);

	if (status != MINIMM_OK) {
		return minimm_client_operation_error("close", status);
	}
	return MINIMM_CLIENT_EXIT_SUCCESS;
}

static int minimm_client_command_ping(const minimm_client_cli_options_t *options,
				      int argument_count)
{
	minimm_client_t *client = NULL;
	uint64_t response = 0U;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 0) {
		return minimm_client_usage_error("ping takes no arguments");
	}
	result = minimm_client_connect_cli(options, &client);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_ping(client, MINIMM_CLIENT_PING_NONCE, &response);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("ping", status);
	} else if (response != MINIMM_CLIENT_PING_NONCE) {
		(void)fprintf(stderr, "minimm-client: ping: nonce mismatch\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	} else if (puts("pong") == EOF) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_create(const minimm_client_cli_options_t *options,
					int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_ALL;
	minimm_remote_rights_t created_rights = 0U;
	uintmax_t size_value = 0U;
	uint64_t created_size = 0U;
	char token[MINIMM_CAPABILITY_HEX_BUFFER_SIZE] = { 0 };
	char rights_text[7] = { 0 };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;
	bool note_is_open = false;

	if (argument_count != 1 && argument_count != 2) {
		return minimm_client_usage_error("create requires SIZE [RIGHTS]");
	}
	if (!minimm_client_parse_uint(arguments[0], UINT64_MAX, &size_value)) {
		return minimm_client_usage_error("invalid create SIZE");
	}
	if (argument_count == 2 && !minimm_client_parse_rights(arguments[1], &rights)) {
		return minimm_client_usage_error("invalid RIGHTS");
	}
	if ((rights & (MINIMM_REMOTE_RIGHT_SHARE | MINIMM_REMOTE_RIGHT_DELETE)) !=
	    (MINIMM_REMOTE_RIGHT_SHARE | MINIMM_REMOTE_RIGHT_DELETE)) {
		return minimm_client_usage_error(
			"one-shot create requires the shared and delete rights 'sd'");
	}

	result = minimm_client_connect_cli(options, &client);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_create(client, (uint64_t)size_value, rights, &note);
	if (status == MINIMM_OK) {
		note_is_open = true;
		created_size = note.size;
		created_rights = note.rights;
		status = minimm_capability_format(&note.capability, token);
	}
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("create", status);
	} else {
		minimm_client_format_rights(created_rights, rights_text);
		if (printf("token=%s size=%" PRIu64 " rights=%s\n", token, created_size,
			   rights_text) < 0 ||
		    fflush(stdout) != 0) {
			(void)fprintf(stderr, "minimm-client: write output failed\n");
			(void)minimm_client_note_unlink(client, &note.capability);
			result = MINIMM_CLIENT_EXIT_OPERATION;
		} else {
			result = minimm_client_close_note(client, &note);
			if (result == MINIMM_CLIENT_EXIT_SUCCESS) {
				note_is_open = false;
			}
		}
	}
	if (note_is_open) {
		(void)minimm_client_note_close(client, &note);
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_open_token(const minimm_client_cli_options_t *options, const char *token,
				    minimm_remote_rights_t rights, minimm_client_t **out_client,
				    minimm_remote_note_t *out_note,
				    minimm_capability_t *out_capability)
{
	minimm_status_t status = MINIMM_OK;
	int result = minimm_client_parse_token(token, out_capability);

	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	result = minimm_client_connect_cli(options, out_client);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_open(*out_client, out_capability, rights, out_note);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("open", status);
		minimm_client_disconnect(*out_client);
		*out_client = NULL;
	}
	return result;
}

static int minimm_client_command_copy(const minimm_client_cli_options_t *options,
				      int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t source = { 0 };
	minimm_remote_note_t copy = { 0 };
	minimm_capability_t source_capability = { { 0 } };
	minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_ALL;
	char token[MINIMM_CAPABILITY_HEX_BUFFER_SIZE] = { 0 };
	char rights_text[7] = { 0 };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;
	bool source_is_open = false;
	bool copy_is_open = false;

	if (argument_count != 1 && argument_count != 2) {
		return minimm_client_usage_error("copy requires TOKEN [RIGHTS]");
	}
	if (argument_count == 2 && !minimm_client_parse_rights(arguments[1], &rights)) {
		return minimm_client_usage_error("invalid RIGHTS");
	}
	if ((rights & (MINIMM_REMOTE_RIGHT_SHARE | MINIMM_REMOTE_RIGHT_DELETE)) !=
	    (MINIMM_REMOTE_RIGHT_SHARE | MINIMM_REMOTE_RIGHT_DELETE)) {
		return minimm_client_usage_error(
			"one-shot copy requires the shared and delete rights 'sd'");
	}

	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_READ, &client,
					  &source, &source_capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	source_is_open = true;
	status = minimm_client_note_copy(client, source.handle, rights, &copy);
	if (status == MINIMM_OK) {
		copy_is_open = true;
		status = minimm_capability_format(&copy.capability, token);
	}
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("copy", status);
	} else {
		minimm_client_format_rights(copy.rights, rights_text);
		if (printf("token=%s size=%" PRIu64 " rights=%s\n", token, copy.size, rights_text) <
			    0 ||
		    fflush(stdout) != 0) {
			(void)fprintf(stderr, "minimm-client: write output failed\n");
			(void)minimm_client_note_unlink(client, &copy.capability);
			result = MINIMM_CLIENT_EXIT_OPERATION;
		}
	}
	if (copy_is_open) {
		const int close_result = minimm_client_close_note(client, &copy);

		if (close_result == MINIMM_CLIENT_EXIT_SUCCESS) {
			copy_is_open = false;
		} else if (result == MINIMM_CLIENT_EXIT_SUCCESS) {
			result = close_result;
		}
	}
	if (source_is_open) {
		const int close_result = minimm_client_close_note(client, &source);

		if (close_result == MINIMM_CLIENT_EXIT_SUCCESS) {
			source_is_open = false;
		} else if (result == MINIMM_CLIENT_EXIT_SUCCESS) {
			result = close_result;
		}
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_stat(const minimm_client_cli_options_t *options,
				      int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_remote_note_info_t info = { 0 };
	minimm_capability_t capability = { { 0 } };
	char rights_text[7] = { 0 };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 1) {
		return minimm_client_usage_error("stat requires TOKEN");
	}
	result = minimm_client_open_token(options, arguments[0], 0U, &client, &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_stat(client, note.handle, &info);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("stat", status);
	} else {
		minimm_client_format_rights(info.rights, rights_text);
		if (printf("size=%" PRIu64 " rights=%s flags=%" PRIu32 "\n", info.size, rights_text,
			   info.flags) < 0) {
			(void)fprintf(stderr, "minimm-client: write output failed\n");
			result = MINIMM_CLIENT_EXIT_OPERATION;
		}
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static bool minimm_client_stdout_write(const unsigned char *bytes, size_t length)
{
	size_t completed = 0U;

	while (completed < length) {
		const size_t written = fwrite(bytes + completed, 1U, length - completed, stdout);

		if (written == 0U) {
			return false;
		}
		completed += written;
	}
	return true;
}

static int minimm_client_command_read(const minimm_client_cli_options_t *options,
				      int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	unsigned char *buffer = NULL;
	uintmax_t offset_value = 0U;
	uintmax_t length_value = 0U;
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 3) {
		return minimm_client_usage_error("read requires TOKEN OFFSET LENGTH");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &offset_value)) {
		return minimm_client_usage_error("invalid read OFFSET");
	}
	if (!minimm_client_parse_uint(arguments[2], (uintmax_t)MINIMM_CLIENT_MAX_READ_SIZE,
				      &length_value)) {
		return minimm_client_usage_error("invalid read LENGTH (maximum is 16777216)");
	}
	if (length_value > UINT64_MAX - offset_value) {
		return minimm_client_usage_error("read range overflows uint64_t");
	}
	if (length_value != 0U) {
		buffer = malloc((size_t)length_value);
		if (buffer == NULL) {
			(void)fprintf(stderr, "minimm-client: allocate read buffer failed\n");
			return MINIMM_CLIENT_EXIT_OPERATION;
		}
	}

	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_READ, &client,
					  &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		free(buffer);
		return result;
	}
	status = minimm_client_note_read(client, note.handle, (uint64_t)offset_value, buffer,
					 (size_t)length_value, &completed);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("read", status);
	} else if (completed > (size_t)length_value) {
		(void)fprintf(stderr, "minimm-client: read: invalid byte count\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	} else if (completed != 0U && !minimm_client_stdout_write(buffer, completed)) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	free(buffer);
	return result;
}

static int minimm_client_command_write_like(const minimm_client_cli_options_t *options,
					    int argument_count, char **arguments, bool edit)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	const char *text = NULL;
	size_t text_length = 0U;
	size_t completed = 0U;
	uintmax_t offset_value = 0U;
	minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_WRITE;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 3) {
		return minimm_client_usage_error(edit ? "edit requires TOKEN OFFSET TEXT" :
							"write requires TOKEN OFFSET TEXT");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &offset_value)) {
		return minimm_client_usage_error(edit ? "invalid edit OFFSET" :
							"invalid write OFFSET");
	}
	text = arguments[2];
	text_length = strlen(text);
	if ((uintmax_t)text_length > UINT64_MAX - offset_value) {
		return minimm_client_usage_error(edit ? "edit range overflows uint64_t" :
							"write range overflows uint64_t");
	}
	if (edit) {
		rights |= MINIMM_REMOTE_RIGHT_EDIT;
	}

	result = minimm_client_open_token(options, arguments[0], rights, &client, &note,
					  &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = edit ? minimm_client_note_edit(client, note.handle, (uint64_t)offset_value, text,
						text_length, &completed) :
			minimm_client_note_write(client, note.handle, (uint64_t)offset_value, text,
						 text_length, &completed);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error(edit ? "edit" : "write", status);
	} else if (printf("completed=%zu\n", completed) < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_preview(const minimm_client_cli_options_t *options,
					 int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	const char *text = NULL;
	size_t text_length = 0U;
	size_t completed = 0U;
	uintmax_t offset_value = 0U;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 3) {
		return minimm_client_usage_error("preview requires TOKEN OFFSET TEXT");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &offset_value)) {
		return minimm_client_usage_error("invalid preview OFFSET");
	}
	text = arguments[2];
	text_length = strlen(text);
	if ((uintmax_t)text_length > UINT64_MAX - offset_value) {
		return minimm_client_usage_error("preview range overflows uint64_t");
	}

	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_READ, &client,
					  &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_preview(client, note.handle, (uint64_t)offset_value, text,
					    text_length, &completed);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("preview", status);
	} else if (printf("completed=%zu\n", completed) < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_stack_expand(const minimm_client_cli_options_t *options,
					      int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	const char *text = NULL;
	size_t text_length = 0U;
	size_t completed = 0U;
	uintmax_t offset_value = 0U;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 3) {
		return minimm_client_usage_error("stack-expand requires TOKEN OFFSET TEXT");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &offset_value)) {
		return minimm_client_usage_error("invalid stack-expand OFFSET");
	}
	text = arguments[2];
	text_length = strlen(text);
	if ((uintmax_t)text_length > UINT64_MAX - offset_value) {
		return minimm_client_usage_error("stack-expand range overflows uint64_t");
	}

	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_READ, &client,
					  &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_stack_expand(client, note.handle, (uint64_t)offset_value, text,
						 text_length, &completed);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("stack-expand", status);
	} else if (printf("completed=%zu\n", completed) < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_remap_page(const minimm_client_cli_options_t *options,
					    int argument_count, char **arguments)
{
	const minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_WRITE |
					      MINIMM_REMOTE_RIGHT_SHARE;
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	minimm_prot_t protection = MINIMM_PROT_NONE;
	uintmax_t offset_value = 0U;
	char protection_text[5] = { 0 };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 2) {
		return minimm_client_usage_error("remap-page requires TOKEN OFFSET");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &offset_value) ||
	    (offset_value & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return minimm_client_usage_error("invalid remap-page OFFSET");
	}

	result = minimm_client_open_token(options, arguments[0], rights, &client, &note,
					  &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_remap_page(client, note.handle, (uint64_t)offset_value,
					       &protection);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("remap-page", status);
	} else {
		minimm_client_format_protection(protection, protection_text);
		if (printf("protection=%s\n", protection_text) < 0) {
			(void)fprintf(stderr, "minimm-client: write output failed\n");
			result = MINIMM_CLIENT_EXIT_OPERATION;
		}
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_mseal_merge(const minimm_client_cli_options_t *options,
					     int argument_count, char **arguments)
{
	const minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_WRITE |
					      MINIMM_REMOTE_RIGHT_SHARE;
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_remote_mseal_merge_result_t merge = { 0 };
	minimm_capability_t capability = { { 0 } };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 1) {
		return minimm_client_usage_error("mseal-merge requires TOKEN");
	}
	result = minimm_client_open_token(options, arguments[0], rights, &client, &note,
					  &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_mseal_merge(client, note.handle, &merge);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("mseal-merge", status);
	} else if (printf("total_pages=%" PRIu32 " sealed_pages=%" PRIu32
			  " range_valid=%s update_start=%" PRIu64 " current_start=%" PRIu64 "\n",
			  merge.total_pages, merge.sealed_pages,
			  merge.range_valid ? "true" : "false", merge.update_start,
			  merge.current_start) < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_mglru_reparent(const minimm_client_cli_options_t *options,
						int argument_count, char **arguments)
{
	const minimm_remote_rights_t rights = MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_WRITE |
					      MINIMM_REMOTE_RIGHT_SHARE;
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_remote_mglru_reparent_result_t reparent = { 0 };
	minimm_capability_t capability = { { 0 } };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 1) {
		return minimm_client_usage_error("mglru-reparent requires TOKEN");
	}
	result = minimm_client_open_token(options, arguments[0], rights, &client, &note,
					  &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_mglru_reparent(client, note.handle, &reparent);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("mglru-reparent", status);
	} else if (printf("total_pages=%" PRIu32 " parent_old_pages=%" PRIu32
			  " parent_new_pages=%" PRIu32 " child_old_debt_pages=%" PRIu32
			  " child_new_credit_pages=%" PRIu32 " exit_clean=%s accounting_valid=%s\n",
			  reparent.total_pages, reparent.parent_old_pages,
			  reparent.parent_new_pages, reparent.child_old_debt_pages,
			  reparent.child_new_credit_pages, reparent.exit_clean ? "true" : "false",
			  reparent.accounting_valid ? "true" : "false") < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_resize(const minimm_client_cli_options_t *options,
					int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	uintmax_t size_value = 0U;
	uint64_t actual_size = 0U;
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 2) {
		return minimm_client_usage_error("resize requires TOKEN SIZE");
	}
	if (!minimm_client_parse_uint(arguments[1], UINT64_MAX, &size_value)) {
		return minimm_client_usage_error("invalid resize SIZE");
	}
	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_RESIZE,
					  &client, &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_resize(client, note.handle, (uint64_t)size_value, &actual_size);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("resize", status);
	} else if (printf("size=%" PRIu64 "\n", actual_size) < 0) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_flush(const minimm_client_cli_options_t *options,
				       int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 1) {
		return minimm_client_usage_error("flush requires TOKEN");
	}
	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_WRITE, &client,
					  &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_flush(client, note.handle);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("flush", status);
	} else if (puts("ok") == EOF) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_command_delete(const minimm_client_cli_options_t *options,
					int argument_count, char **arguments)
{
	minimm_client_t *client = NULL;
	minimm_remote_note_t note = { 0 };
	minimm_capability_t capability = { { 0 } };
	minimm_status_t status = MINIMM_OK;
	int result = MINIMM_CLIENT_EXIT_SUCCESS;

	if (argument_count != 1) {
		return minimm_client_usage_error("delete requires TOKEN");
	}
	result = minimm_client_open_token(options, arguments[0], MINIMM_REMOTE_RIGHT_DELETE,
					  &client, &note, &capability);
	if (result != MINIMM_CLIENT_EXIT_SUCCESS) {
		return result;
	}
	status = minimm_client_note_unlink(client, &capability);
	if (status != MINIMM_OK) {
		result = minimm_client_operation_error("delete", status);
	} else if (puts("ok") == EOF) {
		(void)fprintf(stderr, "minimm-client: write output failed\n");
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	if (minimm_client_close_note(client, &note) != MINIMM_CLIENT_EXIT_SUCCESS &&
	    result == MINIMM_CLIENT_EXIT_SUCCESS) {
		result = MINIMM_CLIENT_EXIT_OPERATION;
	}
	minimm_client_disconnect(client);
	return result;
}

static int minimm_client_dispatch(const minimm_client_cli_options_t *options, const char *command,
				  int argument_count, char **arguments)
{
	if (strcmp(command, "ping") == 0) {
		return minimm_client_command_ping(options, argument_count);
	}
	if (strcmp(command, "create") == 0) {
		return minimm_client_command_create(options, argument_count, arguments);
	}
	if (strcmp(command, "copy") == 0) {
		return minimm_client_command_copy(options, argument_count, arguments);
	}
	if (strcmp(command, "stat") == 0) {
		return minimm_client_command_stat(options, argument_count, arguments);
	}
	if (strcmp(command, "read") == 0) {
		return minimm_client_command_read(options, argument_count, arguments);
	}
	if (strcmp(command, "write") == 0) {
		return minimm_client_command_write_like(options, argument_count, arguments, false);
	}
	if (strcmp(command, "edit") == 0) {
		return minimm_client_command_write_like(options, argument_count, arguments, true);
	}
	if (strcmp(command, "preview") == 0) {
		return minimm_client_command_preview(options, argument_count, arguments);
	}
	if (strcmp(command, "stack-expand") == 0) {
		return minimm_client_command_stack_expand(options, argument_count, arguments);
	}
	if (strcmp(command, "remap-page") == 0) {
		return minimm_client_command_remap_page(options, argument_count, arguments);
	}
	if (strcmp(command, "mseal-merge") == 0) {
		return minimm_client_command_mseal_merge(options, argument_count, arguments);
	}
	if (strcmp(command, "mglru-reparent") == 0) {
		return minimm_client_command_mglru_reparent(options, argument_count, arguments);
	}
	if (strcmp(command, "resize") == 0) {
		return minimm_client_command_resize(options, argument_count, arguments);
	}
	if (strcmp(command, "flush") == 0) {
		return minimm_client_command_flush(options, argument_count, arguments);
	}
	if (strcmp(command, "delete") == 0) {
		return minimm_client_command_delete(options, argument_count, arguments);
	}
	return minimm_client_usage_error("unknown command");
}

int main(int argc, char **argv)
{
	minimm_client_cli_options_t options = {
		.host = MINIMM_CLIENT_DEFAULT_HOST,
		.port = MINIMM_CLIENT_DEFAULT_PORT,
		.timeout_ms = 0U,
	};
	int index = 1;

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		return minimm_client_operation_error("ignore SIGPIPE", MINIMM_ERROR_IO);
	}

	while (index < argc && strncmp(argv[index], "--", 2U) == 0) {
		uintmax_t value = 0U;

		if (strcmp(argv[index], "--help") == 0) {
			minimm_client_usage(stdout, argv[0]);
			return minimm_client_finish_output(MINIMM_CLIENT_EXIT_SUCCESS);
		}
		if (index + 1 >= argc) {
			minimm_client_usage(stderr, argv[0]);
			return minimm_client_usage_error("missing option value");
		}
		if (strcmp(argv[index], "--host") == 0) {
			if (argv[index + 1][0] == '\0') {
				return minimm_client_usage_error("--host must not be empty");
			}
			options.host = argv[index + 1];
		} else if (strcmp(argv[index], "--port") == 0) {
			if (!minimm_client_parse_uint(argv[index + 1], UINT16_MAX, &value) ||
			    value == 0U) {
				return minimm_client_usage_error("invalid --port");
			}
			options.port = (uint16_t)value;
		} else if (strcmp(argv[index], "--timeout-ms") == 0) {
			if (!minimm_client_parse_uint(argv[index + 1], UINT32_MAX, &value)) {
				return minimm_client_usage_error("invalid --timeout-ms");
			}
			options.timeout_ms = (uint32_t)value;
		} else {
			minimm_client_usage(stderr, argv[0]);
			return minimm_client_usage_error("unknown option");
		}
		index += 2;
	}

	if (index >= argc) {
		minimm_client_usage(stderr, argv[0]);
		return MINIMM_CLIENT_EXIT_USAGE;
	}
	return minimm_client_finish_output(
		minimm_client_dispatch(&options, argv[index], argc - index - 1, argv + index + 1));
}
