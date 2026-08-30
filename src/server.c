#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "minimm/server.h"

#include "mglru_reparent.h"
#include "mseal_merge.h"
#include "page_remap.h"
#include "private_preview.h"
#include "protocol.h"
#include "stack_expand.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define MINIMM_SERVER_DEFAULT_ADDRESS "127.0.0.1"
#define MINIMM_SERVER_DEFAULT_PORT UINT16_C(7331)
#define MINIMM_SERVER_DEFAULT_CLIENTS 32U
#define MINIMM_SERVER_DEFAULT_NOTES 1024U
#define MINIMM_SERVER_DEFAULT_NOTE_SIZE (UINT64_C(64) * 1024U * 1024U)
#define MINIMM_SERVER_DEFAULT_TOTAL_SIZE (UINT64_C(256) * 1024U * 1024U)
#define MINIMM_SERVER_DEFAULT_TIMEOUT_MS UINT32_C(30000)
#define MINIMM_SERVER_MAX_HANDLES 256U
#define MINIMM_SERVER_ACCEPT_POLL_MS 250

typedef struct minimm_server_record minimm_server_record_t;
typedef struct minimm_server_client minimm_server_client_t;

typedef struct minimm_server_handle {
	minimm_server_record_t *record;
	uint64_t id;
	minimm_protocol_rights_t rights;
} minimm_server_handle_t;

struct minimm_server_record {
	minimm_server_record_t *registry_next;
	minimm_server_record_t *snapshot_parent;
	struct minimm_server *server;
	minimm_note_t *note;
	pthread_mutex_t op_lock;
	atomic_size_t references;
	minimm_protocol_capability_t capability;
	minimm_protocol_rights_t maximum_rights;
	uint64_t accounted_size;
};

struct minimm_server_client {
	minimm_server_client_t *active_next;
	struct minimm_server *server;
	int socket_fd;
	uint64_t next_handle;
	uint32_t maximum_payload;
	size_t handle_count;
	size_t handle_limit;
	bool hello_complete;
	minimm_server_handle_t handles[MINIMM_SERVER_MAX_HANDLES];
};

struct minimm_server {
	minimm_server_config_t config;
	char *bind_address;
	minimm_t *mm;
	pthread_mutex_t state_lock;
	pthread_cond_t state_changed;
	pthread_mutex_t registry_lock;
	minimm_server_client_t *active_clients;
	minimm_server_record_t *registry;
	pthread_t accept_thread;
	atomic_uint_fast16_t bound_port;
	atomic_size_t live_records;
	atomic_uint_fast64_t total_note_size;
	size_t active_client_count;
	int listener_fd;
	bool accept_thread_valid;
	bool started;
	bool stopping;
};

typedef struct minimm_server_response {
	minimm_protocol_wire_status_t status;
	uint32_t length;
	uint8_t fixed[MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE];
	uint8_t *dynamic;
	minimm_server_record_t *provisional_record;
	uint64_t provisional_handle;
} minimm_server_response_t;

static void minimm_server_record_release(minimm_server_record_t *record);

static bool minimm_server_rights_are_valid(minimm_protocol_rights_t rights)
{
	return (rights & ~(minimm_protocol_rights_t)MINIMM_PROTOCOL_RIGHT_ALL) == UINT32_C(0) &&
	       ((rights & MINIMM_PROTOCOL_RIGHT_EDIT) == UINT32_C(0) ||
		(rights & MINIMM_PROTOCOL_RIGHT_WRITE) != UINT32_C(0));
}

static bool minimm_server_has_rights(minimm_protocol_rights_t available,
				     minimm_protocol_rights_t required)
{
	return (available & required) == required;
}

static bool minimm_server_config_is_valid(const minimm_server_config_t *config)
{
	const uint64_t page_mask = MINIMM_PAGE_SIZE - UINT64_C(1);
	bool restricted_bind_is_valid = false;

	if (config == NULL || config->bind_address == NULL) {
		return false;
	}
	restricted_bind_is_valid = (!config->enable_private_preview &&
				    !config->enable_stack_expand && !config->enable_page_remap &&
				    !config->enable_mseal_merge &&
				    !config->enable_mglru_reparent) ||
				   strcmp(config->bind_address, "127.0.0.1") == 0 ||
				   strcmp(config->bind_address, "::1") == 0;

	return config->bind_address[0] != '\0' && restricted_bind_is_valid &&
	       config->max_clients != 0U && config->max_notes != 0U &&
	       config->max_note_size != UINT64_C(0) &&
	       config->max_note_size <= (uint64_t)INT64_MAX &&
	       (config->max_note_size & page_mask) == UINT64_C(0) &&
	       config->max_total_note_size >= config->max_note_size &&
	       (config->max_total_note_size & page_mask) == UINT64_C(0) &&
	       config->max_payload_size >= MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE &&
	       config->max_payload_size <= MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE &&
	       config->io_timeout_ms != UINT32_C(0);
}

minimm_server_config_t minimm_server_config_default(void)
{
	minimm_server_config_t config = {
		.bind_address = MINIMM_SERVER_DEFAULT_ADDRESS,
		.port = MINIMM_SERVER_DEFAULT_PORT,
		.max_clients = MINIMM_SERVER_DEFAULT_CLIENTS,
		.max_notes = MINIMM_SERVER_DEFAULT_NOTES,
		.max_note_size = MINIMM_SERVER_DEFAULT_NOTE_SIZE,
		.max_total_note_size = MINIMM_SERVER_DEFAULT_TOTAL_SIZE,
		.max_payload_size = MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE,
		.io_timeout_ms = MINIMM_SERVER_DEFAULT_TIMEOUT_MS,
		.enable_private_preview = false,
		.enable_stack_expand = false,
		.enable_page_remap = false,
		.enable_mseal_merge = false,
		.enable_mglru_reparent = false,
		.memory = { 0 },
	};

	config.memory = minimm_config_default();
	return config;
}

static bool minimm_server_reserve_record(minimm_server_t *server, uint64_t size)
{
	size_t records = atomic_load_explicit(&server->live_records, memory_order_relaxed);
	uint_fast64_t bytes = 0U;

	while (records < server->config.max_notes) {
		if (atomic_compare_exchange_weak_explicit(&server->live_records, &records,
							  records + 1U, memory_order_acq_rel,
							  memory_order_relaxed)) {
			break;
		}
	}
	if (records >= server->config.max_notes) {
		return false;
	}

	bytes = atomic_load_explicit(&server->total_note_size, memory_order_relaxed);
	while (size <= server->config.max_total_note_size - (uint64_t)bytes) {
		if (atomic_compare_exchange_weak_explicit(
			    &server->total_note_size, &bytes, bytes + (uint_fast64_t)size,
			    memory_order_acq_rel, memory_order_relaxed)) {
			return true;
		}
	}

	(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
	return false;
}

static bool minimm_server_reserve_bytes(minimm_server_t *server, uint64_t size)
{
	uint_fast64_t bytes = atomic_load_explicit(&server->total_note_size, memory_order_relaxed);

	while (size <= server->config.max_total_note_size - (uint64_t)bytes) {
		if (atomic_compare_exchange_weak_explicit(
			    &server->total_note_size, &bytes, bytes + (uint_fast64_t)size,
			    memory_order_acq_rel, memory_order_relaxed)) {
			return true;
		}
	}
	return false;
}

static void minimm_server_release_bytes(minimm_server_t *server, uint64_t size)
{
	(void)atomic_fetch_sub_explicit(&server->total_note_size, (uint_fast64_t)size,
					memory_order_acq_rel);
}

static void minimm_server_record_retain(minimm_server_record_t *record)
{
	size_t references = atomic_load_explicit(&record->references, memory_order_relaxed);

	while (references != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&record->references, &references,
							  references + 1U, memory_order_relaxed,
							  memory_order_relaxed)) {
			return;
		}
	}
	abort();
}

static void minimm_server_record_release(minimm_server_record_t *record)
{
	while (record != NULL &&
	       atomic_fetch_sub_explicit(&record->references, 1U, memory_order_acq_rel) == 1U) {
		minimm_server_record_t *snapshot_parent = record->snapshot_parent;
		minimm_server_t *server = record->server;
		const uint64_t size = record->accounted_size;

		minimm_note_release(record->note);
		(void)pthread_mutex_destroy(&record->op_lock);
		free(record);
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		record = snapshot_parent;
	}
}

static minimm_protocol_wire_status_t
minimm_server_record_create(minimm_server_t *server, uint64_t size,
			    minimm_protocol_rights_t maximum_rights,
			    minimm_server_record_t **out_record)
{
	minimm_server_record_t *record = NULL;
	minimm_status_t status = MINIMM_OK;

	*out_record = NULL;
	if (!minimm_server_reserve_record(server, size)) {
		return MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
	}

	record = calloc(1U, sizeof(*record));
	if (record == NULL) {
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&record->op_lock, NULL) != 0) {
		free(record);
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
	}

	status = minimm_note_create(server->mm, size, MINIMM_NOTE_RIGHT_ALL, &record->note);
	if (status != MINIMM_OK) {
		(void)pthread_mutex_destroy(&record->op_lock);
		free(record);
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return minimm_protocol_status_from_minimm(status);
	}

	record->server = server;
	record->maximum_rights = maximum_rights;
	record->accounted_size = size;
	atomic_init(&record->references, 1U);
	*out_record = record;
	return MINIMM_PROTOCOL_STATUS_OK;
}

/* The caller holds source->op_lock so size and bytes form one snapshot. */
static minimm_protocol_wire_status_t
minimm_server_record_copy(minimm_server_t *server, minimm_server_record_t *source,
			  minimm_protocol_rights_t maximum_rights,
			  minimm_server_record_t **out_record)
{
	const uint64_t size = source->accounted_size;
	minimm_server_record_t *record = NULL;
	minimm_status_t status = MINIMM_OK;

	*out_record = NULL;
	if (!minimm_server_reserve_record(server, size)) {
		return MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
	}

	record = calloc(1U, sizeof(*record));
	if (record == NULL) {
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&record->op_lock, NULL) != 0) {
		free(record);
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
	}

	status = minimm_note_copy(source->note, MINIMM_NOTE_RIGHT_ALL, &record->note);
	if (status != MINIMM_OK) {
		(void)pthread_mutex_destroy(&record->op_lock);
		free(record);
		minimm_server_release_bytes(server, size);
		(void)atomic_fetch_sub_explicit(&server->live_records, 1U, memory_order_acq_rel);
		return minimm_protocol_status_from_minimm(status);
	}

	record->server = server;
	record->maximum_rights = maximum_rights;
	record->accounted_size = size;
	atomic_init(&record->references, 1U);
	/* Keep the server-side quota owner alive as long as the lazy note parent is needed. */
	minimm_server_record_retain(source);
	record->snapshot_parent = source;
	*out_record = record;
	return MINIMM_PROTOCOL_STATUS_OK;
}

static bool minimm_server_capability_is_zero(const minimm_protocol_capability_t *capability)
{
	uint8_t combined = UINT8_C(0);
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		combined |= capability->bytes[index];
	}
	return combined == UINT8_C(0);
}

static bool minimm_server_capabilities_equal(const minimm_protocol_capability_t *left,
					     const minimm_protocol_capability_t *right)
{
	uint8_t difference = UINT8_C(0);
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		difference |= (uint8_t)(left->bytes[index] ^ right->bytes[index]);
	}
	return difference == UINT8_C(0);
}

static minimm_status_t minimm_server_random_capability(minimm_protocol_capability_t *capability)
{
	size_t completed = 0U;
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

	if (fd < 0) {
		return MINIMM_ERROR_IO;
	}
	while (completed < MINIMM_PROTOCOL_CAPABILITY_SIZE) {
		const ssize_t result = read(fd, capability->bytes + completed,
					    MINIMM_PROTOCOL_CAPABILITY_SIZE - completed);

		if (result > 0) {
			completed += (size_t)result;
		} else if (result < 0 && errno == EINTR) {
			continue;
		} else {
			(void)close(fd);
			return MINIMM_ERROR_IO;
		}
	}
	(void)close(fd);
	return MINIMM_OK;
}

static bool
minimm_server_registry_has_capability_locked(const minimm_server_t *server,
					     const minimm_protocol_capability_t *capability)
{
	const minimm_server_record_t *record = server->registry;

	while (record != NULL) {
		if (minimm_server_capabilities_equal(&record->capability, capability)) {
			return true;
		}
		record = record->registry_next;
	}
	return false;
}

static minimm_protocol_wire_status_t minimm_server_registry_link(minimm_server_t *server,
								 minimm_server_record_t *record)
{
	unsigned attempt = 0U;

	if (server == NULL || record == NULL) {
		return MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR;
	}

	for (attempt = 0U; attempt < 16U; ++attempt) {
		minimm_status_t status = minimm_server_random_capability(&record->capability);
		bool collision = false;

		if (status != MINIMM_OK) {
			return minimm_protocol_status_from_minimm(status);
		}
		if (minimm_server_capability_is_zero(&record->capability)) {
			continue;
		}

		(void)pthread_mutex_lock(&server->registry_lock);
		collision =
			minimm_server_registry_has_capability_locked(server, &record->capability);
		if (!collision) {
			minimm_server_record_retain(record);
			record->registry_next = server->registry;
			server->registry = record;
		}
		(void)pthread_mutex_unlock(&server->registry_lock);
		if (!collision) {
			return MINIMM_PROTOCOL_STATUS_OK;
		}
	}
	(void)memset(&record->capability, 0, sizeof(record->capability));
	return MINIMM_PROTOCOL_STATUS_NO_SPACE;
}

static minimm_protocol_wire_status_t
minimm_server_registry_open(minimm_server_t *server, const minimm_protocol_capability_t *capability,
			    minimm_protocol_rights_t requested_rights,
			    minimm_server_record_t **out_record)
{
	minimm_server_record_t *record = NULL;
	minimm_protocol_wire_status_t status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;

	*out_record = NULL;
	(void)pthread_mutex_lock(&server->registry_lock);
	record = server->registry;
	while (record != NULL) {
		if (minimm_server_capabilities_equal(&record->capability, capability)) {
			if (!minimm_server_has_rights(record->maximum_rights,
						      MINIMM_PROTOCOL_RIGHT_SHARE) ||
			    !minimm_server_has_rights(record->maximum_rights, requested_rights)) {
				status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
			} else {
				minimm_server_record_retain(record);
				*out_record = record;
				status = MINIMM_PROTOCOL_STATUS_OK;
			}
			break;
		}
		record = record->registry_next;
	}
	(void)pthread_mutex_unlock(&server->registry_lock);
	return status;
}

static minimm_protocol_wire_status_t
minimm_server_registry_unlink(minimm_server_t *server,
			      const minimm_protocol_capability_t *capability)
{
	minimm_server_record_t **link = NULL;
	minimm_server_record_t *removed = NULL;
	minimm_protocol_wire_status_t status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;

	(void)pthread_mutex_lock(&server->registry_lock);
	link = &server->registry;
	while (*link != NULL) {
		minimm_server_record_t *record = *link;

		if (minimm_server_capabilities_equal(&record->capability, capability)) {
			if (!minimm_server_has_rights(record->maximum_rights,
						      MINIMM_PROTOCOL_RIGHT_DELETE)) {
				status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
			} else {
				*link = record->registry_next;
				record->registry_next = NULL;
				removed = record;
				status = MINIMM_PROTOCOL_STATUS_OK;
			}
			break;
		}
		link = &record->registry_next;
	}
	(void)pthread_mutex_unlock(&server->registry_lock);
	minimm_server_record_release(removed);
	return status;
}

static minimm_server_handle_t *minimm_server_client_find_handle(minimm_server_client_t *client,
								uint64_t id)
{
	size_t index = 0U;

	if (id == UINT64_C(0)) {
		return NULL;
	}
	for (index = 0U; index < client->handle_limit; ++index) {
		if (client->handles[index].record != NULL && client->handles[index].id == id) {
			return &client->handles[index];
		}
	}
	return NULL;
}

/* Transfer the caller's record reference into a free session handle. */
static bool minimm_server_client_add_handle(minimm_server_client_t *client,
					    minimm_server_record_t *record,
					    minimm_protocol_rights_t rights, uint64_t *out_id)
{
	size_t index = 0U;

	if (client->handle_count >= client->handle_limit || client->next_handle == UINT64_MAX) {
		return false;
	}
	for (index = 0U; index < client->handle_limit; ++index) {
		if (client->handles[index].record == NULL) {
			client->handles[index].record = record;
			client->handles[index].id = client->next_handle;
			client->handles[index].rights = rights;
			*out_id = client->next_handle;
			client->next_handle += UINT64_C(1);
			client->handle_count += 1U;
			return true;
		}
	}
	return false;
}

static minimm_server_record_t *minimm_server_client_remove_handle(minimm_server_client_t *client,
								  uint64_t id)
{
	minimm_server_handle_t *handle = minimm_server_client_find_handle(client, id);
	minimm_server_record_t *record = NULL;

	if (handle == NULL) {
		return NULL;
	}
	record = handle->record;
	(void)memset(handle, 0, sizeof(*handle));
	client->handle_count -= 1U;
	return record;
}

static void minimm_server_client_release_handles(minimm_server_client_t *client)
{
	size_t index = 0U;

	for (index = 0U; index < client->handle_limit; ++index) {
		minimm_server_record_release(client->handles[index].record);
		(void)memset(&client->handles[index], 0, sizeof(client->handles[index]));
	}
	client->handle_count = 0U;
}

static void minimm_server_response_init(minimm_server_response_t *response)
{
	(void)memset(response, 0, sizeof(*response));
	response->status = MINIMM_PROTOCOL_STATUS_OK;
}

static uint8_t *minimm_server_response_allocate(minimm_server_response_t *response, uint32_t length)
{
	response->length = length;
	if (length <= (uint32_t)sizeof(response->fixed)) {
		(void)memset(response->fixed, 0, sizeof(response->fixed));
		return response->fixed;
	}
	response->dynamic = calloc((size_t)length, sizeof(*response->dynamic));
	if (response->dynamic == NULL) {
		response->length = UINT32_C(0);
	}
	return response->dynamic;
}

static const uint8_t *minimm_server_response_payload(const minimm_server_response_t *response)
{
	return response->dynamic != NULL ? response->dynamic : response->fixed;
}

static void minimm_server_response_destroy(minimm_server_response_t *response)
{
	free(response->dynamic);
	response->dynamic = NULL;
	response->length = UINT32_C(0);
}

static void minimm_server_registry_remove_record(minimm_server_t *server,
						 minimm_server_record_t *record)
{
	minimm_server_record_t **link = NULL;
	minimm_server_record_t *removed = NULL;

	(void)pthread_mutex_lock(&server->registry_lock);
	link = &server->registry;
	while (*link != NULL) {
		if (*link == record) {
			removed = *link;
			*link = removed->registry_next;
			removed->registry_next = NULL;
			break;
		}
		link = &(*link)->registry_next;
	}
	(void)pthread_mutex_unlock(&server->registry_lock);
	minimm_server_record_release(removed);
}

static void minimm_server_response_rollback(minimm_server_client_t *client,
					    minimm_server_response_t *response)
{
	minimm_server_record_t *handle_record = NULL;

	if (response->provisional_record == NULL) {
		return;
	}
	minimm_server_registry_remove_record(client->server, response->provisional_record);
	handle_record = minimm_server_client_remove_handle(client, response->provisional_handle);
	minimm_server_record_release(handle_record);
	response->provisional_record = NULL;
	response->provisional_handle = UINT64_C(0);
}

static bool minimm_server_version_is_at_most(uint8_t left_major, uint8_t left_minor,
					     uint8_t right_major, uint8_t right_minor)
{
	return left_major < right_major || (left_major == right_major && left_minor <= right_minor);
}

static void minimm_server_handle_hello(minimm_server_client_t *client, const uint8_t *payload,
				       uint32_t payload_length, minimm_server_response_t *response)
{
	uint32_t receive_maximum = UINT32_C(0);
	uint32_t negotiated_maximum = UINT32_C(0);
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_HELLO_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	receive_maximum = minimm_protocol_get_u32(payload + 8U);
	if (!minimm_server_version_is_at_most(payload[0], payload[1], payload[2], payload[3]) ||
	    !minimm_server_version_is_at_most(payload[0], payload[1], MINIMM_PROTOCOL_VERSION_MAJOR,
					      MINIMM_PROTOCOL_VERSION_MINOR) ||
	    !minimm_server_version_is_at_most(MINIMM_PROTOCOL_VERSION_MAJOR,
					      MINIMM_PROTOCOL_VERSION_MINOR, payload[2],
					      payload[3])) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_VERSION;
		return;
	}
	if (minimm_protocol_get_u32(payload + 4U) != UINT32_C(0) ||
	    minimm_protocol_get_u32(payload + 12U) != UINT32_C(0) ||
	    receive_maximum < MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}

	negotiated_maximum = receive_maximum < client->server->config.max_payload_size ?
				     receive_maximum :
				     client->server->config.max_payload_size;
	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	output[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	output[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u16(output + 2U, UINT16_C(0));
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	minimm_protocol_put_u32(output + 8U, negotiated_maximum);
	minimm_protocol_put_u32(output + 12U, (uint32_t)client->handle_limit);
	minimm_protocol_put_u32(output + 16U, (uint32_t)MINIMM_PAGE_SIZE);
	minimm_protocol_put_u32(output + 20U, UINT32_C(1));
	minimm_protocol_put_u64(output + 24U, client->server->config.max_note_size);
	client->maximum_payload = negotiated_maximum;
	client->hello_complete = true;
}

static void minimm_server_handle_ping(const uint8_t *payload, uint32_t payload_length,
				      minimm_server_response_t *response)
{
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_PING_PAYLOAD_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_PING_PAYLOAD_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	(void)memcpy(output, payload, MINIMM_PROTOCOL_PING_PAYLOAD_SIZE);
}

static void minimm_server_handle_create(minimm_server_client_t *client, const uint8_t *payload,
					uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_record_t *record = NULL;
	const uint64_t size = payload_length >= UINT32_C(8) ? minimm_protocol_get_u64(payload) :
							      UINT64_C(0);
	const minimm_protocol_rights_t rights = payload_length >= UINT32_C(12) ?
							minimm_protocol_get_u32(payload + 8U) :
							UINT32_C(0);
	uint64_t handle = UINT64_C(0);
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_CREATE_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	if (minimm_protocol_get_u32(payload + 12U) != UINT32_C(0) ||
	    !minimm_server_rights_are_valid(rights) ||
	    (size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    size > client->server->config.max_note_size) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	if (client->handle_count >= client->handle_limit || client->next_handle == UINT64_MAX) {
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}

	response->status = minimm_server_record_create(client->server, size, rights, &record);
	if (response->status != MINIMM_PROTOCOL_STATUS_OK || record == NULL) {
		if (response->status == MINIMM_PROTOCOL_STATUS_OK) {
			response->status = MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR;
		}
		return;
	}
	if (minimm_server_has_rights(rights, MINIMM_PROTOCOL_RIGHT_SHARE)) {
		response->status = minimm_server_registry_link(client->server, record);
		if (response->status != MINIMM_PROTOCOL_STATUS_OK) {
			minimm_server_record_release(record);
			return;
		}
	}
	if (!minimm_server_client_add_handle(client, record, rights, &handle)) {
		minimm_server_registry_remove_record(client->server, record);
		minimm_server_record_release(record);
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}

	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE);
	if (output == NULL) {
		minimm_server_registry_remove_record(client->server, record);
		minimm_server_record_release(minimm_server_client_remove_handle(client, handle));
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	minimm_protocol_put_u64(output, handle);
	(void)memcpy(output + 8U, record->capability.bytes, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	minimm_protocol_put_u64(output + 24U, size);
	minimm_protocol_put_u32(output + 32U, rights);
	minimm_protocol_put_u32(output + 36U, UINT32_C(0));
	response->provisional_record = record;
	response->provisional_handle = handle;
}

static void minimm_server_handle_copy(minimm_server_client_t *client, const uint8_t *payload,
				      uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_handle_t *source = NULL;
	minimm_server_record_t *record = NULL;
	minimm_protocol_rights_t rights = UINT32_C(0);
	uint64_t handle = UINT64_C(0);
	uint64_t size = UINT64_C(0);
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_COPY_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	rights = minimm_protocol_get_u32(payload + 8U);
	if (minimm_protocol_get_u32(payload + 12U) != UINT32_C(0) ||
	    !minimm_server_rights_are_valid(rights)) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	if (client->handle_count >= client->handle_limit || client->next_handle == UINT64_MAX) {
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}
	source = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (source == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(source->rights, MINIMM_PROTOCOL_RIGHT_READ)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}

	(void)pthread_mutex_lock(&source->record->op_lock);
	response->status =
		minimm_server_record_copy(client->server, source->record, rights, &record);
	(void)pthread_mutex_unlock(&source->record->op_lock);
	if (response->status != MINIMM_PROTOCOL_STATUS_OK || record == NULL) {
		if (response->status == MINIMM_PROTOCOL_STATUS_OK) {
			response->status = MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR;
		}
		return;
	}
	size = record->accounted_size;
	if (minimm_server_has_rights(rights, MINIMM_PROTOCOL_RIGHT_SHARE)) {
		response->status = minimm_server_registry_link(client->server, record);
		if (response->status != MINIMM_PROTOCOL_STATUS_OK) {
			minimm_server_record_release(record);
			return;
		}
	}
	if (!minimm_server_client_add_handle(client, record, rights, &handle)) {
		minimm_server_registry_remove_record(client->server, record);
		minimm_server_record_release(record);
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}

	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_COPY_RESPONSE_SIZE);
	if (output == NULL) {
		minimm_server_registry_remove_record(client->server, record);
		minimm_server_record_release(minimm_server_client_remove_handle(client, handle));
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	minimm_protocol_put_u64(output, handle);
	(void)memcpy(output + 8U, record->capability.bytes, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	minimm_protocol_put_u64(output + 24U, size);
	minimm_protocol_put_u32(output + 32U, rights);
	minimm_protocol_put_u32(output + 36U, UINT32_C(0));
	response->provisional_record = record;
	response->provisional_handle = handle;
}

static void minimm_server_handle_open(minimm_server_client_t *client, const uint8_t *payload,
				      uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_protocol_capability_t capability = { { 0 } };
	minimm_protocol_rights_t rights = UINT32_C(0);
	minimm_server_record_t *record = NULL;
	uint64_t handle = UINT64_C(0);
	uint64_t size = UINT64_C(0);
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_OPEN_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	(void)memcpy(capability.bytes, payload, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	rights = minimm_protocol_get_u32(payload + 16U);
	if (minimm_protocol_get_u32(payload + 20U) != UINT32_C(0) ||
	    !minimm_server_rights_are_valid(rights)) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	if (client->handle_count >= client->handle_limit || client->next_handle == UINT64_MAX) {
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}

	response->status =
		minimm_server_registry_open(client->server, &capability, rights, &record);
	if (response->status != MINIMM_PROTOCOL_STATUS_OK) {
		return;
	}
	if (!minimm_server_client_add_handle(client, record, rights, &handle)) {
		minimm_server_record_release(record);
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}
	(void)pthread_mutex_lock(&record->op_lock);
	size = record->accounted_size;
	(void)pthread_mutex_unlock(&record->op_lock);

	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_OPEN_RESPONSE_SIZE);
	if (output == NULL) {
		minimm_server_record_release(minimm_server_client_remove_handle(client, handle));
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	minimm_protocol_put_u64(output, handle);
	minimm_protocol_put_u64(output + 8U, size);
	minimm_protocol_put_u32(output + 16U, rights);
	minimm_protocol_put_u32(output + 20U, UINT32_C(0));
}

static void minimm_server_handle_close(minimm_server_client_t *client, const uint8_t *payload,
				       uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_record_t *record = NULL;

	if (payload_length != MINIMM_PROTOCOL_CLOSE_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	record = minimm_server_client_remove_handle(client, minimm_protocol_get_u64(payload));
	if (record == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	minimm_server_record_release(record);
}

static void minimm_server_handle_stat(minimm_server_client_t *client, const uint8_t *payload,
				      uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t size = UINT64_C(0);
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_STAT_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	(void)pthread_mutex_lock(&handle->record->op_lock);
	size = handle->record->accounted_size;
	(void)pthread_mutex_unlock(&handle->record->op_lock);

	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_STAT_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	minimm_protocol_put_u64(output, size);
	minimm_protocol_put_u32(output + 8U, handle->rights);
	minimm_protocol_put_u32(output + 12U, UINT32_C(0));
}

static void minimm_server_handle_read(minimm_server_client_t *client, const uint8_t *payload,
				      uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t offset = UINT64_C(0);
	uint32_t length = UINT32_C(0);
	uint32_t output_length = MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE;
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_READ_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	output =
		minimm_server_response_allocate(response, MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	offset = minimm_protocol_get_u64(payload + 8U);
	length = minimm_protocol_get_u32(payload + 16U);
	if (minimm_protocol_get_u32(payload + 20U) != UINT32_C(0) ||
	    length > MINIMM_PROTOCOL_MAX_DATA_SIZE || (uint64_t)length > UINT64_MAX - offset) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	if (length > client->maximum_payload - MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, MINIMM_PROTOCOL_RIGHT_READ)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}

	output_length += length;
	output = minimm_server_response_allocate(response, output_length);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_note_pread(handle->record->note, offset,
				   output + MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE,
				   (size_t)length, &completed);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, (uint32_t)completed);
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	response->length = MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE + (uint32_t)completed;
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_write_or_edit(minimm_server_client_t *client,
					       minimm_protocol_opcode_t opcode,
					       const uint8_t *payload, uint32_t payload_length,
					       minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t offset = UINT64_C(0);
	uint32_t length = UINT32_C(0);
	minimm_protocol_rights_t required = MINIMM_PROTOCOL_RIGHT_WRITE;
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (payload_length < MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}
	offset = minimm_protocol_get_u64(payload + 8U);
	length = minimm_protocol_get_u32(payload + 16U);
	if (length > MINIMM_PROTOCOL_MAX_DATA_SIZE ||
	    length > UINT32_MAX - MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE ||
	    payload_length != MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE + length) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	if (minimm_protocol_get_u32(payload + 20U) != UINT32_C(0) ||
	    (uint64_t)length > UINT64_MAX - offset) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (opcode == MINIMM_PROTOCOL_OP_EDIT) {
		required |= MINIMM_PROTOCOL_RIGHT_EDIT;
	}
	if (!minimm_server_has_rights(handle->rights, required)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	if (opcode == MINIMM_PROTOCOL_OP_EDIT) {
		status = minimm_note_pedit(handle->record->note, offset,
					   payload + MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE,
					   (size_t)length, &completed);
	} else {
		status = minimm_note_pwrite(handle->record->note, offset,
					    payload + MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE,
					    (size_t)length, &completed);
	}
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, (uint32_t)completed);
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_preview(minimm_server_client_t *client, const uint8_t *payload,
					 uint32_t payload_length,
					 minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t offset = UINT64_C(0);
	uint32_t length = UINT32_C(0);
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (!client->server->config.enable_private_preview) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		return;
	}
	if (payload_length < MINIMM_PROTOCOL_PREVIEW_REQUEST_FIXED_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	offset = minimm_protocol_get_u64(payload + 8U);
	length = minimm_protocol_get_u32(payload + 16U);
	if (length > UINT32_MAX - MINIMM_PROTOCOL_PREVIEW_REQUEST_FIXED_SIZE ||
	    payload_length != MINIMM_PROTOCOL_PREVIEW_REQUEST_FIXED_SIZE + length) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	if (length == UINT32_C(0) || length > MINIMM_PAGE_SIZE ||
	    minimm_protocol_get_u32(payload + 20U) != UINT32_C(0) ||
	    (uint64_t)length > UINT64_MAX - offset ||
	    (offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) + (uint64_t)length > MINIMM_PAGE_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, MINIMM_PROTOCOL_RIGHT_READ) ||
	    (handle->rights & (MINIMM_PROTOCOL_RIGHT_WRITE | MINIMM_PROTOCOL_RIGHT_EDIT)) !=
		    UINT32_C(0)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_PREVIEW_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_private_preview_apply(client->server->mm, handle->record->note, offset,
					      payload + MINIMM_PROTOCOL_PREVIEW_REQUEST_FIXED_SIZE,
					      (size_t)length, &completed);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, (uint32_t)completed);
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_stack_expand(minimm_server_client_t *client,
					      const uint8_t *payload, uint32_t payload_length,
					      minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t offset = UINT64_C(0);
	uint32_t length = UINT32_C(0);
	size_t completed = 0U;
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (!client->server->config.enable_stack_expand) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		return;
	}
	if (payload_length < MINIMM_PROTOCOL_STACK_EXPAND_REQUEST_FIXED_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	offset = minimm_protocol_get_u64(payload + 8U);
	length = minimm_protocol_get_u32(payload + 16U);
	if (length > UINT32_MAX - MINIMM_PROTOCOL_STACK_EXPAND_REQUEST_FIXED_SIZE ||
	    payload_length != MINIMM_PROTOCOL_STACK_EXPAND_REQUEST_FIXED_SIZE + length) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	if (length == UINT32_C(0) || length > MINIMM_PAGE_SIZE ||
	    minimm_protocol_get_u32(payload + 20U) != UINT32_C(0) ||
	    (uint64_t)length > UINT64_MAX - offset ||
	    (offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) + (uint64_t)length > MINIMM_PAGE_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, MINIMM_PROTOCOL_RIGHT_READ) ||
	    (handle->rights & (MINIMM_PROTOCOL_RIGHT_WRITE | MINIMM_PROTOCOL_RIGHT_EDIT)) !=
		    UINT32_C(0)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output = minimm_server_response_allocate(response,
						 MINIMM_PROTOCOL_STACK_EXPAND_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	status =
		minimm_stack_expand_apply(client->server->mm, handle->record->note, offset,
					  payload + MINIMM_PROTOCOL_STACK_EXPAND_REQUEST_FIXED_SIZE,
					  (size_t)length, &completed);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, (uint32_t)completed);
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_remap_page(minimm_server_client_t *client, const uint8_t *payload,
					    uint32_t payload_length,
					    minimm_server_response_t *response)
{
	const minimm_protocol_rights_t required = MINIMM_PROTOCOL_RIGHT_READ |
						  MINIMM_PROTOCOL_RIGHT_WRITE |
						  MINIMM_PROTOCOL_RIGHT_SHARE;
	minimm_server_handle_t *handle = NULL;
	uint64_t note_offset = UINT64_C(0);
	minimm_prot_t protection = MINIMM_PROT_NONE;
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (!client->server->config.enable_page_remap) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		return;
	}
	if (payload_length != MINIMM_PROTOCOL_REMAP_PAGE_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	note_offset = minimm_protocol_get_u64(payload + 8U);
	if ((note_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, required)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output =
		minimm_server_response_allocate(response, MINIMM_PROTOCOL_REMAP_PAGE_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_page_remap_apply(client->server->mm, handle->record->note, note_offset,
					 &protection);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, protection);
	minimm_protocol_put_u32(output + 4U, UINT32_C(0));
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_mseal_merge(minimm_server_client_t *client, const uint8_t *payload,
					     uint32_t payload_length,
					     minimm_server_response_t *response)
{
	const minimm_protocol_rights_t required = MINIMM_PROTOCOL_RIGHT_READ |
						  MINIMM_PROTOCOL_RIGHT_WRITE |
						  MINIMM_PROTOCOL_RIGHT_SHARE;
	minimm_server_handle_t *handle = NULL;
	minimm_mseal_merge_result_t result = { 0 };
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (!client->server->config.enable_mseal_merge) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		return;
	}
	if (payload_length != MINIMM_PROTOCOL_MSEAL_MERGE_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, required)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output = minimm_server_response_allocate(response,
						 MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_mseal_merge_apply(client->server->mm, handle->record->note, &result);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, result.total_pages);
	minimm_protocol_put_u32(output + 4U, result.sealed_pages);
	minimm_protocol_put_u32(output + 8U, result.range_valid ? UINT32_C(1) : UINT32_C(0));
	minimm_protocol_put_u32(output + 12U, UINT32_C(0));
	minimm_protocol_put_u64(output + 16U, result.update_start);
	minimm_protocol_put_u64(output + 24U, result.current_start);
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_mglru_reparent(minimm_server_client_t *client,
						const uint8_t *payload, uint32_t payload_length,
						minimm_server_response_t *response)
{
	const minimm_protocol_rights_t required = MINIMM_PROTOCOL_RIGHT_READ |
						  MINIMM_PROTOCOL_RIGHT_WRITE |
						  MINIMM_PROTOCOL_RIGHT_SHARE;
	minimm_server_handle_t *handle = NULL;
	minimm_mglru_reparent_result_t result = { 0 };
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (!client->server->config.enable_mglru_reparent) {
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		return;
	}
	if (payload_length != MINIMM_PROTOCOL_MGLRU_REPARENT_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, required)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output = minimm_server_response_allocate(response,
						 MINIMM_PROTOCOL_MGLRU_REPARENT_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_mglru_reparent_run(client->server->mm, handle->record->note, &result);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	minimm_protocol_put_u32(output, result.total_pages);
	minimm_protocol_put_u32(output + 4U, result.parent_old_pages);
	minimm_protocol_put_u32(output + 8U, result.parent_new_pages);
	minimm_protocol_put_u32(output + 12U, result.child_old_debt_pages);
	minimm_protocol_put_u32(output + 16U, result.child_new_credit_pages);
	minimm_protocol_put_u32(output + 20U, result.exit_clean ? UINT32_C(1) : UINT32_C(0));
	minimm_protocol_put_u32(output + 24U, result.accounting_valid ? UINT32_C(1) : UINT32_C(0));
	minimm_protocol_put_u32(output + 28U, UINT32_C(0));
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_resize(minimm_server_client_t *client, const uint8_t *payload,
					uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	uint64_t requested_size = UINT64_C(0);
	uint64_t old_size = UINT64_C(0);
	minimm_status_t status = MINIMM_OK;
	uint8_t *output = NULL;

	if (payload_length != MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	requested_size = minimm_protocol_get_u64(payload + 8U);
	if ((requested_size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    requested_size > client->server->config.max_note_size) {
		response->status = MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, MINIMM_PROTOCOL_RIGHT_RESIZE)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	output = minimm_server_response_allocate(response, MINIMM_PROTOCOL_RESIZE_RESPONSE_SIZE);
	if (output == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
		return;
	}

	(void)pthread_mutex_lock(&handle->record->op_lock);
	old_size = handle->record->accounted_size;
	minimm_protocol_put_u64(output, old_size);
	if (requested_size > old_size &&
	    !minimm_server_reserve_bytes(client->server, requested_size - old_size)) {
		response->status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
		(void)pthread_mutex_unlock(&handle->record->op_lock);
		return;
	}

	status = minimm_note_resize(handle->record->note, requested_size);
	if (status == MINIMM_OK) {
		handle->record->accounted_size = requested_size;
		if (requested_size < old_size) {
			minimm_server_release_bytes(client->server, old_size - requested_size);
		}
		minimm_protocol_put_u64(output, requested_size);
	} else if (requested_size > old_size) {
		minimm_server_release_bytes(client->server, requested_size - old_size);
	}
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_flush(minimm_server_client_t *client, const uint8_t *payload,
				       uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_server_handle_t *handle = NULL;
	minimm_status_t status = MINIMM_OK;

	if (payload_length != MINIMM_PROTOCOL_FLUSH_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	handle = minimm_server_client_find_handle(client, minimm_protocol_get_u64(payload));
	if (handle == NULL) {
		response->status = MINIMM_PROTOCOL_STATUS_NOT_FOUND;
		return;
	}
	if (!minimm_server_has_rights(handle->rights, MINIMM_PROTOCOL_RIGHT_WRITE)) {
		response->status = MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
		return;
	}
	(void)pthread_mutex_lock(&handle->record->op_lock);
	status = minimm_note_flush(handle->record->note);
	(void)pthread_mutex_unlock(&handle->record->op_lock);
	response->status = minimm_protocol_status_from_minimm(status);
}

static void minimm_server_handle_unlink(minimm_server_client_t *client, const uint8_t *payload,
					uint32_t payload_length, minimm_server_response_t *response)
{
	minimm_protocol_capability_t capability = { { 0 } };

	if (payload_length != MINIMM_PROTOCOL_UNLINK_REQUEST_SIZE) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return;
	}
	(void)memcpy(capability.bytes, payload, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	response->status = minimm_server_registry_unlink(client->server, &capability);
}

static bool minimm_server_dispatch(minimm_server_client_t *client, uint16_t opcode,
				   const uint8_t *payload, uint32_t payload_length,
				   minimm_server_response_t *response)
{
	if (!client->hello_complete) {
		if (opcode != (uint16_t)MINIMM_PROTOCOL_OP_HELLO) {
			response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
			return true;
		}
		minimm_server_handle_hello(client, payload, payload_length, response);
		return response->status != MINIMM_PROTOCOL_STATUS_OK;
	}
	if (opcode == (uint16_t)MINIMM_PROTOCOL_OP_HELLO) {
		response->status = MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE;
		return true;
	}

	switch (opcode) {
	case MINIMM_PROTOCOL_OP_PING:
		minimm_server_handle_ping(payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_CREATE:
		minimm_server_handle_create(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_COPY:
		minimm_server_handle_copy(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_OPEN:
		minimm_server_handle_open(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_CLOSE:
		minimm_server_handle_close(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_STAT:
		minimm_server_handle_stat(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_READ:
		minimm_server_handle_read(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_WRITE:
	case MINIMM_PROTOCOL_OP_EDIT:
		minimm_server_handle_write_or_edit(client, (minimm_protocol_opcode_t)opcode,
						   payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_PREVIEW:
		minimm_server_handle_preview(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_STACK_EXPAND:
		minimm_server_handle_stack_expand(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_REMAP_PAGE:
		minimm_server_handle_remap_page(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_MSEAL_MERGE:
		minimm_server_handle_mseal_merge(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_MGLRU_REPARENT:
		minimm_server_handle_mglru_reparent(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_RESIZE:
		minimm_server_handle_resize(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_FLUSH:
		minimm_server_handle_flush(client, payload, payload_length, response);
		break;
	case MINIMM_PROTOCOL_OP_UNLINK:
		minimm_server_handle_unlink(client, payload, payload_length, response);
		break;
	default:
		response->status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
		break;
	}
	return false;
}

static bool minimm_server_send_response(minimm_server_client_t *client,
					const minimm_protocol_header_t *request,
					const minimm_server_response_t *response)
{
	minimm_protocol_header_t header = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = request->opcode,
		.flags = MINIMM_PROTOCOL_FLAG_RESPONSE,
		.wire_status = (uint32_t)response->status,
		.request_id = request->request_id,
		.payload_length = response->length,
		.reserved = UINT32_C(0),
	};
	uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };

	if (response->length > client->maximum_payload ||
	    !minimm_protocol_encode_header(&header, wire_header) ||
	    minimm_protocol_send_all(client->socket_fd, wire_header, sizeof(wire_header)) !=
		    MINIMM_PROTOCOL_IO_OK) {
		return false;
	}
	return response->length == UINT32_C(0) ||
	       minimm_protocol_send_all(client->socket_fd, minimm_server_response_payload(response),
					(size_t)response->length) == MINIMM_PROTOCOL_IO_OK;
}

static void minimm_server_serve_client(minimm_server_client_t *client)
{
	for (;;) {
		minimm_protocol_header_t request = { 0 };
		minimm_server_response_t response;
		uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
		uint8_t *payload = NULL;
		minimm_protocol_io_result_t io_result = MINIMM_PROTOCOL_IO_OK;
		bool close_after_response = false;

		io_result = minimm_protocol_recv_exact(client->socket_fd, wire_header,
						       sizeof(wire_header));
		if (io_result != MINIMM_PROTOCOL_IO_OK ||
		    !minimm_protocol_decode_header(wire_header, &request) ||
		    (request.flags & MINIMM_PROTOCOL_FLAG_RESPONSE) != UINT16_C(0)) {
			break;
		}

		minimm_server_response_init(&response);
		if (request.major != MINIMM_PROTOCOL_VERSION_MAJOR ||
		    request.minor != MINIMM_PROTOCOL_VERSION_MINOR) {
			response.status = MINIMM_PROTOCOL_STATUS_UNSUPPORTED_VERSION;
			(void)minimm_server_send_response(client, &request, &response);
			minimm_server_response_destroy(&response);
			break;
		}
		if (request.payload_length > client->maximum_payload) {
			response.status = MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED;
			(void)minimm_server_send_response(client, &request, &response);
			minimm_server_response_destroy(&response);
			break;
		}

		if (request.payload_length != UINT32_C(0)) {
			payload = malloc((size_t)request.payload_length);
			if (payload == NULL) {
				response.status = MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
				(void)minimm_server_send_response(client, &request, &response);
				minimm_server_response_destroy(&response);
				break;
			}
			io_result = minimm_protocol_recv_exact(client->socket_fd, payload,
							       (size_t)request.payload_length);
			if (io_result != MINIMM_PROTOCOL_IO_OK) {
				free(payload);
				minimm_server_response_destroy(&response);
				break;
			}
		}

		close_after_response = minimm_server_dispatch(client, request.opcode, payload,
							      request.payload_length, &response);
		free(payload);
		if (!minimm_server_send_response(client, &request, &response)) {
			minimm_server_response_rollback(client, &response);
			minimm_server_response_destroy(&response);
			break;
		}
		minimm_server_response_destroy(&response);
		if (close_after_response) {
			break;
		}
	}
}

static void minimm_server_remove_active_client_locked(minimm_server_t *server,
						      minimm_server_client_t *client)
{
	minimm_server_client_t **link = &server->active_clients;

	while (*link != NULL) {
		if (*link == client) {
			*link = client->active_next;
			client->active_next = NULL;
			return;
		}
		link = &(*link)->active_next;
	}
}

static void *minimm_server_client_thread(void *argument)
{
	minimm_server_client_t *client = argument;
	minimm_server_t *server = client->server;

	minimm_server_serve_client(client);
	minimm_server_client_release_handles(client);

	(void)pthread_mutex_lock(&server->state_lock);
	minimm_server_remove_active_client_locked(server, client);
	(void)pthread_mutex_unlock(&server->state_lock);

	(void)shutdown(client->socket_fd, SHUT_RDWR);
	(void)close(client->socket_fd);
	client->socket_fd = -1;

	(void)pthread_mutex_lock(&server->state_lock);
	if (server->active_client_count != 0U) {
		server->active_client_count -= 1U;
	}
	(void)pthread_cond_broadcast(&server->state_changed);
	(void)pthread_mutex_unlock(&server->state_lock);
	free(client);
	return NULL;
}

static minimm_status_t minimm_server_set_descriptor_flags(int descriptor, bool nonblocking)
{
	int status_flags = fcntl(descriptor, F_GETFL);
	int descriptor_flags = 0;

	if (status_flags < 0) {
		return MINIMM_ERROR_IO;
	}
	if (nonblocking) {
		status_flags |= O_NONBLOCK;
	} else {
		status_flags &= ~O_NONBLOCK;
	}
	if (fcntl(descriptor, F_SETFL, status_flags) != 0) {
		return MINIMM_ERROR_IO;
	}
	descriptor_flags = fcntl(descriptor, F_GETFD);
	if (descriptor_flags < 0 ||
	    fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
		return MINIMM_ERROR_IO;
	}
	return MINIMM_OK;
}

static minimm_status_t minimm_server_configure_client_socket(int socket_fd, uint32_t timeout_ms)
{
	const int no_delay = 1;
	struct timeval timeout = { 0 };
	minimm_status_t status = minimm_server_set_descriptor_flags(socket_fd, false);

	if (status != MINIMM_OK) {
		return status;
	}
	timeout.tv_sec = (time_t)(timeout_ms / UINT32_C(1000));
	timeout.tv_usec = (suseconds_t)((timeout_ms % UINT32_C(1000)) * UINT32_C(1000));
	if (setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
		       (socklen_t)sizeof(no_delay)) != 0 ||
	    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) !=
		    0 ||
	    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, (socklen_t)sizeof(timeout)) !=
		    0) {
		return MINIMM_ERROR_IO;
	}
	return MINIMM_OK;
}

static bool minimm_server_is_stopping(minimm_server_t *server)
{
	bool stopping = false;

	(void)pthread_mutex_lock(&server->state_lock);
	stopping = server->stopping;
	(void)pthread_mutex_unlock(&server->state_lock);
	return stopping;
}

static void minimm_server_adopt_client(minimm_server_t *server, int socket_fd,
				       const pthread_attr_t *attributes)
{
	minimm_server_client_t *client = NULL;
	pthread_t thread = { 0 };
	int create_status = 0;

	if (minimm_server_configure_client_socket(socket_fd, server->config.io_timeout_ms) !=
	    MINIMM_OK) {
		(void)close(socket_fd);
		return;
	}
	client = calloc(1U, sizeof(*client));
	if (client == NULL) {
		(void)close(socket_fd);
		return;
	}
	client->server = server;
	client->socket_fd = socket_fd;
	client->next_handle = UINT64_C(1);
	client->maximum_payload = server->config.max_payload_size;
	client->handle_limit = server->config.max_notes < MINIMM_SERVER_MAX_HANDLES ?
				       server->config.max_notes :
				       MINIMM_SERVER_MAX_HANDLES;

	(void)pthread_mutex_lock(&server->state_lock);
	if (server->stopping || server->active_client_count >= server->config.max_clients) {
		(void)pthread_mutex_unlock(&server->state_lock);
		(void)close(socket_fd);
		free(client);
		return;
	}
	client->active_next = server->active_clients;
	server->active_clients = client;
	server->active_client_count += 1U;
	create_status = pthread_create(&thread, attributes, minimm_server_client_thread, client);
	if (create_status != 0) {
		minimm_server_remove_active_client_locked(server, client);
		server->active_client_count -= 1U;
		(void)pthread_cond_broadcast(&server->state_changed);
	}
	(void)pthread_mutex_unlock(&server->state_lock);

	if (create_status != 0) {
		(void)close(socket_fd);
		free(client);
	}
}

static int minimm_server_accept_socket(int listener_fd)
{
	int socket_fd = -1;

#if defined(__linux__) && defined(SOCK_CLOEXEC)
	socket_fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
	if (socket_fd >= 0 || (errno != EINVAL && errno != ENOSYS)) {
		return socket_fd;
	}
#endif
	socket_fd = accept(listener_fd, NULL, NULL);
	if (socket_fd >= 0 && minimm_server_set_descriptor_flags(socket_fd, false) != MINIMM_OK) {
		const int error_number = errno;

		(void)close(socket_fd);
		errno = error_number;
		return -1;
	}
	return socket_fd;
}

static void *minimm_server_accept_thread(void *argument)
{
	minimm_server_t *server = argument;
	pthread_attr_t attributes;
	bool attributes_valid = false;

	if (pthread_attr_init(&attributes) != 0) {
		return NULL;
	}
	attributes_valid = true;
	if (pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0) {
		(void)pthread_attr_destroy(&attributes);
		return NULL;
	}

	while (!minimm_server_is_stopping(server)) {
		struct pollfd descriptor = {
			.fd = server->listener_fd,
			.events = POLLIN,
			.revents = 0,
		};
		int poll_status = poll(&descriptor, (nfds_t)1U, MINIMM_SERVER_ACCEPT_POLL_MS);

		if (poll_status < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (poll_status == 0) {
			continue;
		}
		if (minimm_server_is_stopping(server)) {
			break;
		}
		if ((descriptor.revents & POLLIN) == 0) {
			if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
				break;
			}
			continue;
		}

		for (;;) {
			if (minimm_server_is_stopping(server)) {
				break;
			}
			int socket_fd = minimm_server_accept_socket(server->listener_fd);

			if (socket_fd >= 0) {
				minimm_server_adopt_client(server, socket_fd, &attributes);
				continue;
			}
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				break;
			}
			break;
		}
	}

	if (attributes_valid) {
		(void)pthread_attr_destroy(&attributes);
	}
	return NULL;
}

static int minimm_server_create_socket(int family, int type, int protocol)
{
	int socket_fd = -1;

#ifdef SOCK_CLOEXEC
	socket_fd = socket(family, type | SOCK_CLOEXEC, protocol);
	if (socket_fd >= 0 || errno != EINVAL) {
		return socket_fd;
	}
#endif
	socket_fd = socket(family, type, protocol);
	if (socket_fd >= 0 && minimm_server_set_descriptor_flags(socket_fd, false) != MINIMM_OK) {
		const int error_number = errno;

		(void)close(socket_fd);
		errno = error_number;
		return -1;
	}
	return socket_fd;
}

static minimm_status_t minimm_server_listener_port(int socket_fd, uint16_t *out_port)
{
	struct sockaddr_storage address = { 0 };
	socklen_t address_size = (socklen_t)sizeof(address);

	*out_port = UINT16_C(0);
	if (getsockname(socket_fd, (struct sockaddr *)&address, &address_size) != 0) {
		return MINIMM_ERROR_IO;
	}
	if (address.ss_family == AF_INET) {
		const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&address;

		*out_port = ntohs(ipv4->sin_port);
		return MINIMM_OK;
	}
	if (address.ss_family == AF_INET6) {
		const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)&address;

		*out_port = ntohs(ipv6->sin6_port);
		return MINIMM_OK;
	}
	return MINIMM_ERROR_IO;
}

static minimm_status_t minimm_server_create_listener(minimm_server_t *server, int *out_socket_fd,
						     uint16_t *out_port)
{
	struct addrinfo hints = { 0 };
	struct addrinfo *addresses = NULL;
	struct addrinfo *address = NULL;
	char service[6] = { 0 };
	int written = 0;
	int backlog = 0;
	bool address_in_use = false;
	int lookup_status = 0;

	*out_socket_fd = -1;
	*out_port = UINT16_C(0);
	written = snprintf(service, sizeof(service), "%u", (unsigned)server->config.port);
	if (written <= 0 || (size_t)written >= sizeof(service)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	lookup_status = getaddrinfo(server->bind_address, service, &hints, &addresses);
	if (lookup_status != 0) {
		return lookup_status == EAI_MEMORY ? MINIMM_ERROR_OUT_OF_MEMORY : MINIMM_ERROR_IO;
	}
	backlog = server->config.max_clients > (size_t)INT_MAX ? INT_MAX :
								 (int)server->config.max_clients;

	for (address = addresses; address != NULL; address = address->ai_next) {
		const int reuse_address = 1;
		int socket_fd = minimm_server_create_socket(
			address->ai_family, address->ai_socktype, address->ai_protocol);

		if (socket_fd < 0) {
			continue;
		}
		if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_address,
			       (socklen_t)sizeof(reuse_address)) != 0) {
			(void)close(socket_fd);
			continue;
		}
		if (bind(socket_fd, address->ai_addr, address->ai_addrlen) != 0) {
			if (errno == EADDRINUSE) {
				address_in_use = true;
			}
			(void)close(socket_fd);
			continue;
		}
		if (listen(socket_fd, backlog) != 0 ||
		    minimm_server_set_descriptor_flags(socket_fd, true) != MINIMM_OK ||
		    minimm_server_listener_port(socket_fd, out_port) != MINIMM_OK) {
			(void)close(socket_fd);
			continue;
		}
		*out_socket_fd = socket_fd;
		freeaddrinfo(addresses);
		return MINIMM_OK;
	}

	freeaddrinfo(addresses);
	return address_in_use ? MINIMM_ERROR_ADDRESS_IN_USE : MINIMM_ERROR_IO;
}

static minimm_server_record_t *minimm_server_detach_registry(minimm_server_t *server)
{
	minimm_server_record_t *records = NULL;

	(void)pthread_mutex_lock(&server->registry_lock);
	records = server->registry;
	server->registry = NULL;
	(void)pthread_mutex_unlock(&server->registry_lock);
	return records;
}

static void minimm_server_release_registry(minimm_server_record_t *records)
{
	while (records != NULL) {
		minimm_server_record_t *next = records->registry_next;

		records->registry_next = NULL;
		minimm_server_record_release(records);
		records = next;
	}
}

minimm_status_t minimm_server_create(const minimm_server_config_t *config,
				     minimm_server_t **out_server)
{
	minimm_server_t *server = NULL;
	minimm_status_t status = MINIMM_OK;

	if (out_server == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_server = NULL;
	if (!minimm_server_config_is_valid(config)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	server = calloc(1U, sizeof(*server));
	if (server == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	server->bind_address = strdup(config->bind_address);
	if (server->bind_address == NULL) {
		free(server);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	server->config = *config;
	server->config.bind_address = server->bind_address;
	server->listener_fd = -1;
	atomic_init(&server->bound_port, UINT16_C(0));
	atomic_init(&server->live_records, 0U);
	atomic_init(&server->total_note_size, UINT64_C(0));

	if (pthread_mutex_init(&server->state_lock, NULL) != 0) {
		free(server->bind_address);
		free(server);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_cond_init(&server->state_changed, NULL) != 0) {
		(void)pthread_mutex_destroy(&server->state_lock);
		free(server->bind_address);
		free(server);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&server->registry_lock, NULL) != 0) {
		(void)pthread_cond_destroy(&server->state_changed);
		(void)pthread_mutex_destroy(&server->state_lock);
		free(server->bind_address);
		free(server);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	status = minimm_create(&server->config.memory, &server->mm);
	if (status != MINIMM_OK) {
		(void)pthread_mutex_destroy(&server->registry_lock);
		(void)pthread_cond_destroy(&server->state_changed);
		(void)pthread_mutex_destroy(&server->state_lock);
		free(server->bind_address);
		free(server);
		return status;
	}
	*out_server = server;
	return MINIMM_OK;
}

minimm_status_t minimm_server_start(minimm_server_t *server)
{
	minimm_status_t status = MINIMM_OK;
	int listener_fd = -1;
	uint16_t port = UINT16_C(0);
	int thread_status = 0;

	if (server == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)pthread_mutex_lock(&server->state_lock);
	if (server->started || server->stopping) {
		(void)pthread_mutex_unlock(&server->state_lock);
		return MINIMM_ERROR_BUSY;
	}
	status = minimm_server_create_listener(server, &listener_fd, &port);
	if (status != MINIMM_OK) {
		(void)pthread_mutex_unlock(&server->state_lock);
		return status;
	}

	server->listener_fd = listener_fd;
	server->stopping = false;
	atomic_store_explicit(&server->bound_port, port, memory_order_release);
	thread_status =
		pthread_create(&server->accept_thread, NULL, minimm_server_accept_thread, server);
	if (thread_status != 0) {
		server->listener_fd = -1;
		atomic_store_explicit(&server->bound_port, UINT16_C(0), memory_order_release);
		(void)close(listener_fd);
		(void)pthread_mutex_unlock(&server->state_lock);
		return thread_status == EAGAIN ? MINIMM_ERROR_OUT_OF_MEMORY : MINIMM_ERROR_IO;
	}
	server->accept_thread_valid = true;
	server->started = true;
	(void)pthread_mutex_unlock(&server->state_lock);
	return MINIMM_OK;
}

uint16_t minimm_server_bound_port(const minimm_server_t *server)
{
	return server == NULL ?
		       UINT16_C(0) :
		       (uint16_t)atomic_load_explicit(&server->bound_port, memory_order_acquire);
}

minimm_status_t minimm_server_stop(minimm_server_t *server)
{
	minimm_server_client_t *client = NULL;
	minimm_server_record_t *records = NULL;
	pthread_t accept_thread = { 0 };
	int listener_fd = -1;
	bool join_accept_thread = false;

	if (server == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)pthread_mutex_lock(&server->state_lock);
	while (server->stopping) {
		(void)pthread_cond_wait(&server->state_changed, &server->state_lock);
	}
	if (!server->started) {
		(void)pthread_mutex_unlock(&server->state_lock);
		return MINIMM_OK;
	}
	server->stopping = true;
	listener_fd = server->listener_fd;
	accept_thread = server->accept_thread;
	join_accept_thread = server->accept_thread_valid;
	(void)pthread_mutex_unlock(&server->state_lock);

	if (listener_fd >= 0) {
		(void)shutdown(listener_fd, SHUT_RDWR);
	}
	if (join_accept_thread) {
		(void)pthread_join(accept_thread, NULL);
	}
	if (listener_fd >= 0) {
		(void)close(listener_fd);
	}

	(void)pthread_mutex_lock(&server->state_lock);
	for (client = server->active_clients; client != NULL; client = client->active_next) {
		(void)shutdown(client->socket_fd, SHUT_RDWR);
	}
	while (server->active_client_count != 0U) {
		(void)pthread_cond_wait(&server->state_changed, &server->state_lock);
	}
	(void)pthread_mutex_unlock(&server->state_lock);

	records = minimm_server_detach_registry(server);
	minimm_server_release_registry(records);

	(void)pthread_mutex_lock(&server->state_lock);
	server->listener_fd = -1;
	server->accept_thread_valid = false;
	server->started = false;
	server->stopping = false;
	atomic_store_explicit(&server->bound_port, UINT16_C(0), memory_order_release);
	(void)pthread_cond_broadcast(&server->state_changed);
	(void)pthread_mutex_unlock(&server->state_lock);
	return MINIMM_OK;
}

void minimm_server_destroy(minimm_server_t *server)
{
	if (server == NULL) {
		return;
	}
	(void)minimm_server_stop(server);
	minimm_destroy(server->mm);
	(void)pthread_mutex_destroy(&server->registry_lock);
	(void)pthread_cond_destroy(&server->state_changed);
	(void)pthread_mutex_destroy(&server->state_lock);
	free(server->bind_address);
	free(server);
}
