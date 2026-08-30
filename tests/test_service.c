#include "minimm/client.h"
#include "minimm/server.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool capability_is_zero(const minimm_capability_t *capability)
{
	uint8_t combined = UINT8_C(0);
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		combined |= capability->bytes[index];
	}
	return combined == UINT8_C(0);
}

static bool service_sockets_are_close_on_exec(uint16_t port)
{
	long descriptor_limit = sysconf(_SC_OPEN_MAX);
	int descriptor = 0;
	size_t connected_count = 0U;
	size_t listener_count = 0U;

	if (descriptor_limit < 0L || descriptor_limit > 4096L) {
		descriptor_limit = 4096L;
	}
	for (descriptor = 0; descriptor < (int)descriptor_limit; ++descriptor) {
		struct sockaddr_in peer = { 0 };
		struct sockaddr_in local = { 0 };
		socklen_t peer_size = (socklen_t)sizeof(peer);
		socklen_t local_size = (socklen_t)sizeof(local);
		socklen_t option_size = (socklen_t)sizeof(int);
		int accepts_connections = 0;
		int flags = 0;
		bool connected = false;
		bool listener = false;

		if (getsockname(descriptor, (struct sockaddr *)&local, &local_size) != 0 ||
		    local.sin_family != AF_INET) {
			continue;
		}
		connected = getpeername(descriptor, (struct sockaddr *)&peer, &peer_size) == 0 &&
			    peer.sin_family == AF_INET &&
			    (ntohs(peer.sin_port) == port || ntohs(local.sin_port) == port);
		listener = !connected && ntohs(local.sin_port) == port &&
			   getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &accepts_connections,
				      &option_size) == 0 &&
			   accepts_connections != 0;
		if (!connected && !listener) {
			continue;
		}
		flags = fcntl(descriptor, F_GETFD);
		if (flags < 0 || (flags & FD_CLOEXEC) == 0) {
			return false;
		}
		if (connected) {
			connected_count += 1U;
		} else {
			listener_count += 1U;
		}
	}
	/* Two connections contribute four endpoints; the server contributes its listener. */
	return connected_count >= 4U && listener_count >= 1U;
}

typedef struct scripted_server {
	int listener_fd;
	bool success;
} scripted_server_t;

static bool scripted_server_receive(int socket_fd, uint16_t expected_opcode,
				    uint32_t expected_length, minimm_protocol_header_t *out_header,
				    uint8_t *payload)
{
	uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };

	if (minimm_protocol_recv_exact(socket_fd, wire_header, sizeof(wire_header)) !=
		    MINIMM_PROTOCOL_IO_OK ||
	    !minimm_protocol_decode_header(wire_header, out_header) ||
	    out_header->flags != UINT16_C(0) || out_header->opcode != expected_opcode ||
	    out_header->payload_length != expected_length) {
		return false;
	}
	return expected_length == UINT32_C(0) ||
	       minimm_protocol_recv_exact(socket_fd, payload, (size_t)expected_length) ==
		       MINIMM_PROTOCOL_IO_OK;
}

static bool scripted_server_respond(int socket_fd, const minimm_protocol_header_t *request,
				    minimm_protocol_wire_status_t status, const uint8_t *payload,
				    uint32_t payload_length)
{
	const minimm_protocol_header_t response = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = request->opcode,
		.flags = MINIMM_PROTOCOL_FLAG_RESPONSE,
		.wire_status = (uint32_t)status,
		.request_id = request->request_id,
		.payload_length = payload_length,
		.reserved = UINT32_C(0),
	};
	uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };

	if (!minimm_protocol_encode_header(&response, wire_header) ||
	    minimm_protocol_send_all(socket_fd, wire_header, sizeof(wire_header)) !=
		    MINIMM_PROTOCOL_IO_OK) {
		return false;
	}
	return payload_length == UINT32_C(0) ||
	       minimm_protocol_send_all(socket_fd, payload, (size_t)payload_length) ==
		       MINIMM_PROTOCOL_IO_OK;
}

static void *serve_scripted_errors(void *argument)
{
	scripted_server_t *server = argument;
	minimm_protocol_header_t request = { 0 };
	uint8_t payload[MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE + UINT32_C(1)] = { 0 };
	uint8_t hello_response[MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE] = { 0 };
	uint8_t resize_response[MINIMM_PROTOCOL_RESIZE_RESPONSE_SIZE] = { 0 };
	int socket_fd = accept(server->listener_fd, NULL, NULL);

	server->success = false;
	if (socket_fd < 0 ||
	    !scripted_server_receive(socket_fd, (uint16_t)MINIMM_PROTOCOL_OP_HELLO,
				     MINIMM_PROTOCOL_HELLO_REQUEST_SIZE, &request, payload)) {
		goto cleanup;
	}
	hello_response[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	hello_response[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u32(hello_response + 8U, MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE);
	minimm_protocol_put_u32(hello_response + 12U, UINT32_C(8));
	minimm_protocol_put_u32(hello_response + 16U, (uint32_t)MINIMM_PAGE_SIZE);
	minimm_protocol_put_u32(hello_response + 20U, UINT32_C(1));
	minimm_protocol_put_u64(hello_response + 24U, MINIMM_PAGE_SIZE * UINT64_C(2));
	minimm_protocol_put_u64(resize_response, MINIMM_PAGE_SIZE);
	if (!scripted_server_respond(socket_fd, &request, MINIMM_PROTOCOL_STATUS_OK, hello_response,
				     (uint32_t)sizeof(hello_response)) ||
	    !scripted_server_receive(socket_fd, (uint16_t)MINIMM_PROTOCOL_OP_READ,
				     MINIMM_PROTOCOL_READ_REQUEST_SIZE, &request, payload) ||
	    !scripted_server_respond(socket_fd, &request, MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY,
				     NULL, UINT32_C(0)) ||
	    !scripted_server_receive(socket_fd, (uint16_t)MINIMM_PROTOCOL_OP_WRITE,
				     MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE + UINT32_C(1),
				     &request, payload) ||
	    !scripted_server_respond(socket_fd, &request, MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED,
				     NULL, UINT32_C(0)) ||
	    !scripted_server_receive(socket_fd, (uint16_t)MINIMM_PROTOCOL_OP_RESIZE,
				     MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE, &request, payload) ||
	    !scripted_server_respond(socket_fd, &request, MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED,
				     resize_response, (uint32_t)sizeof(resize_response)) ||
	    !scripted_server_receive(socket_fd, (uint16_t)MINIMM_PROTOCOL_OP_PING,
				     MINIMM_PROTOCOL_PING_PAYLOAD_SIZE, &request, payload) ||
	    !scripted_server_respond(socket_fd, &request, MINIMM_PROTOCOL_STATUS_OK, payload,
				     MINIMM_PROTOCOL_PING_PAYLOAD_SIZE)) {
		goto cleanup;
	}
	server->success = true;

cleanup:
	if (socket_fd >= 0) {
		(void)close(socket_fd);
	}
	return NULL;
}

static bool test_client_accepts_empty_error_progress(void)
{
	struct sockaddr_in address = { 0 };
	socklen_t address_length = (socklen_t)sizeof(address);
	scripted_server_t server = { .listener_fd = -1, .success = false };
	minimm_client_t *client = NULL;
	pthread_t thread;
	uint8_t byte = UINT8_C(0x5a);
	uint64_t nonce = UINT64_C(0x1020304050607080);
	uint64_t response_nonce = UINT64_C(0);
	uint64_t actual_size = UINT64_MAX;
	size_t completed = SIZE_MAX;
	int thread_status = 0;
	bool success = false;
	bool thread_created = false;

	server.listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	address.sin_family = AF_INET;
	address.sin_port = htons(UINT16_C(0));
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (!check(server.listener_fd >= 0 &&
			   bind(server.listener_fd, (const struct sockaddr *)&address,
				(socklen_t)sizeof(address)) == 0 &&
			   listen(server.listener_fd, 1) == 0 &&
			   getsockname(server.listener_fd, (struct sockaddr *)&address,
				       &address_length) == 0,
		   "start scripted client-error server")) {
		goto cleanup;
	}
	thread_status = pthread_create(&thread, NULL, serve_scripted_errors, &server);
	if (!check(thread_status == 0, "start scripted client-error worker")) {
		goto cleanup;
	}
	thread_created = true;
	if (!check(minimm_client_connect("127.0.0.1", ntohs(address.sin_port), UINT32_C(5000),
					 &client) == MINIMM_OK,
		   "connect to scripted client-error server") ||
	    !check(minimm_client_note_read(client, UINT64_C(77), UINT64_C(0), &byte, 1U,
					   &completed) == MINIMM_ERROR_OUT_OF_MEMORY &&
			   completed == 0U,
		   "preserve an empty-payload read error") ||
	    !check((completed = SIZE_MAX,
		    minimm_client_note_write(client, UINT64_C(77), UINT64_C(0), &byte, 1U,
					     &completed)) == MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "preserve an empty-payload write error") ||
	    !check(minimm_client_note_resize(client, UINT64_C(77), MINIMM_PAGE_SIZE * UINT64_C(2),
					     &actual_size) == MINIMM_ERROR_NO_SPACE &&
			   actual_size == MINIMM_PAGE_SIZE,
		   "preserve the server's actual size on a failed resize") ||
	    !check(minimm_client_ping(client, nonce, &response_nonce) == MINIMM_OK &&
			   response_nonce == nonce,
		   "keep the client usable after empty error payloads")) {
		goto cleanup;
	}
	success = true;

cleanup:
	minimm_client_disconnect(client);
	if (server.listener_fd >= 0) {
		(void)shutdown(server.listener_fd, SHUT_RDWR);
		(void)close(server.listener_fd);
		server.listener_fd = -1;
	}
	if (thread_created &&
	    !check(pthread_join(thread, NULL) == 0 && (!success || server.success),
		   "complete scripted client-error exchange")) {
		success = false;
	}
	return success;
}

typedef struct concurrent_writer {
	minimm_client_t *client;
	uint64_t handle;
	uint64_t offset;
	uint8_t value[64];
	minimm_status_t status;
} concurrent_writer_t;

static void *write_repeatedly(void *argument)
{
	concurrent_writer_t *writer = argument;
	size_t iteration = 0U;

	writer->status = MINIMM_OK;
	for (iteration = 0U; iteration < 64U; ++iteration) {
		size_t completed = 0U;

		writer->status = minimm_client_note_write(writer->client, writer->handle,
							  writer->offset, writer->value,
							  sizeof(writer->value), &completed);
		if (writer->status != MINIMM_OK || completed != sizeof(writer->value)) {
			writer->status = writer->status == MINIMM_OK ? MINIMM_ERROR_IO :
								       writer->status;
			break;
		}
	}
	return NULL;
}

static bool test_concurrent_writes(minimm_client_t *owner, uint64_t owner_handle,
				   minimm_client_t *peer, uint64_t peer_handle)
{
	concurrent_writer_t writers[2] = {
		{
			.client = owner,
			.handle = owner_handle,
			.offset = UINT64_C(1024),
			.value = { 0 },
			.status = MINIMM_OK,
		},
		{
			.client = peer,
			.handle = peer_handle,
			.offset = UINT64_C(2048),
			.value = { 0 },
			.status = MINIMM_OK,
		},
	};
	pthread_t threads[2];
	uint8_t observed[64] = { 0 };
	size_t index = 0U;
	size_t completed = 0U;
	int first_status = 0;
	int second_status = 0;

	for (index = 0U; index < sizeof(writers[0].value); ++index) {
		writers[0].value[index] = (uint8_t)(index + 1U);
		writers[1].value[index] = (uint8_t)(UINT8_C(255) - (uint8_t)index);
	}
	first_status = pthread_create(&threads[0], NULL, write_repeatedly, &writers[0]);
	if (first_status != 0) {
		return check(false, "start first concurrent writer");
	}
	second_status = pthread_create(&threads[1], NULL, write_repeatedly, &writers[1]);
	if (second_status != 0) {
		(void)pthread_join(threads[0], NULL);
		return check(false, "start second concurrent writer");
	}
	first_status = pthread_join(threads[0], NULL);
	second_status = pthread_join(threads[1], NULL);
	if (!check(first_status == 0 && second_status == 0, "join concurrent writers") ||
	    !check(writers[0].status == MINIMM_OK && writers[1].status == MINIMM_OK,
		   "complete concurrent writes") ||
	    !check(minimm_client_note_read(owner, owner_handle, writers[0].offset, observed,
					   sizeof(observed), &completed) == MINIMM_OK &&
			   completed == sizeof(observed) &&
			   memcmp(observed, writers[0].value, sizeof(observed)) == 0,
		   "preserve first concurrent writer bytes") ||
	    !check(minimm_client_note_read(peer, peer_handle, writers[1].offset, observed,
					   sizeof(observed), &completed) == MINIMM_OK &&
			   completed == sizeof(observed) &&
			   memcmp(observed, writers[1].value, sizeof(observed)) == 0,
		   "preserve second concurrent writer bytes")) {
		return false;
	}
	return true;
}

static bool test_capability_text(void)
{
	typedef union capability_text_overlap {
		minimm_capability_t capability;
		char text[MINIMM_CAPABILITY_HEX_BUFFER_SIZE];
	} capability_text_overlap_t;
	capability_text_overlap_t overlap = { .text = { 0 } };
	minimm_capability_t capability = { { 0 } };
	minimm_capability_t parsed = { { 0 } };
	char text[MINIMM_CAPABILITY_HEX_BUFFER_SIZE] = { 0 };
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		capability.bytes[index] = (uint8_t)(index * 13U + 7U);
	}
	if (!check(minimm_capability_format(&capability, text) == MINIMM_OK, "format capability") ||
	    !check(strlen(text) == MINIMM_CAPABILITY_HEX_LENGTH,
		   "format capability at fixed length") ||
	    !check(minimm_capability_parse(text, &parsed) == MINIMM_OK &&
			   memcmp(&parsed, &capability, sizeof(parsed)) == 0,
		   "parse formatted capability")) {
		return false;
	}

	overlap.capability = capability;
	if (!check(minimm_capability_format(&overlap.capability, overlap.text) == MINIMM_OK &&
			   strcmp(overlap.text, text) == 0,
		   "format a capability in overlapping storage")) {
		return false;
	}
	(void)memcpy(overlap.text, text, sizeof(text));
	if (!check(minimm_capability_parse(overlap.text, &overlap.capability) == MINIMM_OK &&
			   memcmp(&overlap.capability, &capability, sizeof(capability)) == 0,
		   "parse a capability from overlapping storage")) {
		return false;
	}

	return check(minimm_capability_parse("0000000000000000000000000000000g", &parsed) ==
				     MINIMM_ERROR_INVALID_ARGUMENT &&
			     capability_is_zero(&parsed),
		     "reject non-hex capability");
}

static bool test_failed_client_calls_clear_outputs(void)
{
	minimm_capability_t capability = { { 0 } };
	minimm_remote_note_t note;
	minimm_remote_note_info_t information;
	uint64_t value = UINT64_MAX;

	(void)memset(&note, 0xa5, sizeof(note));
	if (!check(minimm_client_note_create(NULL, UINT64_C(0), MINIMM_REMOTE_RIGHT_READ, &note) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   note.handle == UINT64_C(0) && note.size == UINT64_C(0) &&
			   note.rights == UINT32_C(0) && capability_is_zero(&note.capability),
		   "clear a failed CREATE output")) {
		return false;
	}
	(void)memset(&note, 0xa5, sizeof(note));
	if (!check(minimm_client_note_copy(NULL, UINT64_C(0), MINIMM_REMOTE_RIGHT_READ, &note) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   note.handle == UINT64_C(0) && note.size == UINT64_C(0) &&
			   note.rights == UINT32_C(0) && capability_is_zero(&note.capability),
		   "clear a failed COPY output")) {
		return false;
	}
	(void)memset(&note, 0xa5, sizeof(note));
	if (!check(minimm_client_note_open(NULL, &capability, MINIMM_REMOTE_RIGHT_READ, &note) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   note.handle == UINT64_C(0) && note.size == UINT64_C(0) &&
			   note.rights == UINT32_C(0) && capability_is_zero(&note.capability),
		   "clear a failed OPEN output")) {
		return false;
	}
	(void)memset(&information, 0xa5, sizeof(information));
	if (!check(minimm_client_note_stat(NULL, UINT64_C(0), &information) ==
				   MINIMM_ERROR_INVALID_ARGUMENT &&
			   information.size == UINT64_C(0) && information.rights == UINT32_C(0) &&
			   information.flags == UINT32_C(0),
		   "clear a failed STAT output")) {
		return false;
	}
	if (!check(minimm_client_ping(NULL, UINT64_C(1), &value) == MINIMM_ERROR_INVALID_ARGUMENT &&
			   value == UINT64_C(0),
		   "clear a failed PING output")) {
		return false;
	}
	value = UINT64_MAX;
	return check(minimm_client_note_resize(NULL, UINT64_C(0), UINT64_C(0), &value) ==
				     MINIMM_ERROR_INVALID_ARGUMENT &&
			     value == UINT64_C(0),
		     "clear a failed RESIZE output");
}

static bool test_client_write_completion_alias(minimm_client_t *client, uint64_t handle)
{
	unsigned char expected[sizeof(size_t)] = { 0 };
	unsigned char observed[sizeof(size_t)] = { 0 };
	size_t aliased = SIZE_MAX - 23U;
	size_t completed = 0U;

	(void)memcpy(expected, &aliased, sizeof(expected));
	if (!check(minimm_client_note_write(client, handle, UINT64_C(0), &aliased, sizeof(aliased),
					    &aliased) == MINIMM_OK &&
			   aliased == sizeof(aliased),
		   "allow remote write source to alias its completion output") ||
	    !check(minimm_client_note_read(client, handle, UINT64_C(0), observed, sizeof(observed),
					   &completed) == MINIMM_OK &&
			   completed == sizeof(observed) &&
			   memcmp(observed, expected, sizeof(observed)) == 0,
		   "preserve aliased remote write source bytes")) {
		return false;
	}

	aliased = SIZE_MAX / 5U;
	(void)memcpy(expected, &aliased, sizeof(expected));
	return check(minimm_client_note_edit(client, handle, UINT64_C(64), &aliased,
					     sizeof(aliased), &aliased) == MINIMM_OK &&
			     aliased == sizeof(aliased),
		     "allow remote edit source to alias its completion output") &&
	       check(minimm_client_note_read(client, handle, UINT64_C(64), observed,
					     sizeof(observed), &completed) == MINIMM_OK &&
			     completed == sizeof(observed) &&
			     memcmp(observed, expected, sizeof(observed)) == 0,
		     "preserve aliased remote edit source bytes");
}

static bool test_shared_note_service(void)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	minimm_client_t *owner = NULL;
	minimm_client_t *peer = NULL;
	minimm_remote_note_t owner_note = { 0 };
	minimm_remote_note_t peer_note = { 0 };
	minimm_remote_note_t read_only = { 0 };
	minimm_remote_note_t no_rights = { 0 };
	minimm_remote_note_t alias_note = { 0 };
	minimm_remote_note_t copied_note = { 0 };
	minimm_remote_note_t private_note = { 0 };
	minimm_remote_note_t missing_note = { 0 };
	minimm_remote_note_info_t information = { 0 };
	uint8_t source[300] = { 0 };
	uint8_t destination[300] = { 0 };
	uint8_t copied_bytes[300] = { 0 };
	uint8_t zeroes[32] = { 0 };
	static const uint8_t edit[] = "MiniMM";
	static const uint8_t changed[] = { UINT8_C(0xa5) };
	const uint64_t write_offset = MINIMM_PAGE_SIZE - UINT64_C(50);
	const uint64_t nonce = UINT64_C(0x0123456789abcdef);
	uint64_t ping_response = UINT64_C(0);
	uint64_t actual_size = UINT64_C(0);
	size_t completed = 0U;
	size_t index = 0U;
	bool success = false;

	config.port = UINT16_C(0);
	config.max_clients = 4U;
	config.max_notes = 8U;
	config.max_note_size = MINIMM_PAGE_SIZE * UINT64_C(4);
	config.max_total_note_size = MINIMM_PAGE_SIZE * UINT64_C(8);
	config.max_payload_size = UINT32_C(128);
	config.io_timeout_ms = UINT32_C(5000);
	config.memory.physical_memory_size = (size_t)MINIMM_PAGE_SIZE * 2U;

	for (index = 0U; index < sizeof(source); ++index) {
		source[index] = (uint8_t)(index * 37U + 11U);
	}

	if (!check(minimm_server_create(&config, &server) == MINIMM_OK, "create service") ||
	    !check(minimm_server_start(server) == MINIMM_OK &&
			   minimm_server_bound_port(server) != UINT16_C(0),
		   "start service on an ephemeral port") ||
	    !check(minimm_client_connect("127.0.0.1", minimm_server_bound_port(server),
					 UINT32_C(5000), &owner) == MINIMM_OK,
		   "connect owner") ||
	    !check(minimm_client_connect("127.0.0.1", minimm_server_bound_port(server),
					 UINT32_C(5000), &peer) == MINIMM_OK,
		   "connect peer") ||
	    !check(service_sockets_are_close_on_exec(minimm_server_bound_port(server)),
		   "mark service listener and connection descriptors close-on-exec") ||
	    !check(minimm_client_max_payload(owner) == config.max_payload_size &&
			   minimm_client_max_note_size(owner) == config.max_note_size,
		   "negotiate server limits") ||
	    !check(minimm_client_ping(owner, nonce, &ping_response) == MINIMM_OK &&
			   ping_response == nonce,
		   "round-trip ping") ||
	    !check(minimm_client_note_create(owner, MINIMM_PAGE_SIZE * UINT64_C(2),
					     MINIMM_REMOTE_RIGHT_ALL, &owner_note) == MINIMM_OK &&
			   owner_note.handle != UINT64_C(0) &&
			   !capability_is_zero(&owner_note.capability),
		   "create shared note") ||
	    !check(test_client_write_completion_alias(owner, owner_note.handle),
		   "preserve aliased remote data inputs") ||
	    !check(minimm_client_note_write(owner, owner_note.handle, write_offset, source,
					    sizeof(source), &completed) == MINIMM_OK &&
			   completed == sizeof(source),
		   "write a chunked cross-page value") ||
	    !check(minimm_client_note_open(peer, &owner_note.capability,
					   MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_WRITE |
						   MINIMM_REMOTE_RIGHT_EDIT,
					   &peer_note) == MINIMM_OK,
		   "open shared note from a second client") ||
	    !check(minimm_client_note_read(peer, peer_note.handle, write_offset, destination,
					   sizeof(destination), &completed) == MINIMM_OK &&
			   completed == sizeof(destination) &&
			   memcmp(destination, source, sizeof(source)) == 0,
		   "observe shared bytes from the peer") ||
	    !check((alias_note.capability = owner_note.capability,
		    minimm_client_note_open(peer, &alias_note.capability, MINIMM_REMOTE_RIGHT_READ,
					    &alias_note)) == MINIMM_OK,
		   "allow capability input to alias the output note") ||
	    !check(minimm_client_note_close(peer, &alias_note) == MINIMM_OK,
		   "close aliased open result") ||
	    !check((destination[0] = UINT8_C(0),
		    minimm_client_note_read(peer, peer_note.handle, write_offset, destination, 1U,
					    NULL)) == MINIMM_OK &&
			   destination[0] == source[0],
		   "allow an omitted completion count") ||
	    !check(minimm_client_note_edit(peer, peer_note.handle, write_offset + UINT64_C(33),
					   edit, sizeof(edit), &completed) == MINIMM_OK &&
			   completed == sizeof(edit),
		   "edit shared bytes") ||
	    !check(minimm_client_note_read(owner, owner_note.handle, write_offset + UINT64_C(33),
					   destination, sizeof(edit), &completed) == MINIMM_OK &&
			   completed == sizeof(edit) &&
			   memcmp(destination, edit, sizeof(edit)) == 0,
		   "observe peer edit from the owner") ||
	    !check(minimm_client_note_open(peer, &owner_note.capability, MINIMM_REMOTE_RIGHT_READ,
					   &read_only) == MINIMM_OK,
		   "derive a read-only handle") ||
	    !check(minimm_client_note_open(peer, &owner_note.capability, 0U, &no_rights) ==
			   MINIMM_OK,
		   "derive a handle without data rights") ||
	    !check((completed = SIZE_MAX,
		    minimm_client_note_read(peer, UINT64_MAX, UINT64_MAX, NULL, 0U, &completed)) ==
				   MINIMM_ERROR_NOT_FOUND &&
			   completed == 0U,
		   "validate a handle even for an empty read") ||
	    !check((completed = SIZE_MAX,
		    minimm_client_note_write(peer, read_only.handle, UINT64_MAX, NULL, 0U,
					     &completed)) == MINIMM_ERROR_PERMISSION &&
			   completed == 0U &&
			   (completed = SIZE_MAX,
			    minimm_client_note_read(peer, no_rights.handle, UINT64_MAX, NULL, 0U,
						    &completed)) == MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "enforce data rights even for empty I/O") ||
	    !check((completed = SIZE_MAX,
		    minimm_client_note_read(peer, read_only.handle, UINT64_C(0), NULL, 0U,
					    &completed)) == MINIMM_OK &&
			   completed == 0U &&
			   minimm_client_ping(peer, nonce, &ping_response) == MINIMM_OK &&
			   ping_response == nonce,
		   "keep an empty-I/O connection usable") ||
	    !check(minimm_client_note_write(peer, read_only.handle, 0U, edit, sizeof(edit),
					    &completed) == MINIMM_ERROR_PERMISSION &&
			   completed == 0U,
		   "enforce handle write permission") ||
	    !check(minimm_client_ping(peer, nonce, &ping_response) == MINIMM_OK &&
			   ping_response == nonce,
		   "keep the connection usable after a denied request") ||
	    !check((actual_size = UINT64_MAX,
		    minimm_client_note_resize(peer, no_rights.handle,
					      MINIMM_PAGE_SIZE * UINT64_C(2), &actual_size)) ==
				   MINIMM_ERROR_PERMISSION &&
			   actual_size == UINT64_C(0),
		   "clear resize progress when permission is denied before the operation") ||
	    !check(minimm_client_note_resize(owner, owner_note.handle,
					     MINIMM_PAGE_SIZE * UINT64_C(3),
					     &actual_size) == MINIMM_OK &&
			   actual_size == MINIMM_PAGE_SIZE * UINT64_C(3),
		   "resize shared note") ||
	    !check(minimm_client_note_stat(peer, peer_note.handle, &information) == MINIMM_OK &&
			   information.size == MINIMM_PAGE_SIZE * UINT64_C(3) &&
			   information.rights == peer_note.rights &&
			   information.flags == UINT32_C(0),
		   "stat resized note") ||
	    !check(minimm_client_note_read(peer, peer_note.handle, MINIMM_PAGE_SIZE * UINT64_C(2),
					   destination, sizeof(zeroes), &completed) == MINIMM_OK &&
			   completed == sizeof(zeroes) &&
			   memcmp(destination, zeroes, sizeof(zeroes)) == 0,
		   "read zero-filled grown page") ||
	    !check(minimm_client_note_flush(owner, owner_note.handle) == MINIMM_OK,
		   "flush note backing file") ||
	    !check(test_concurrent_writes(owner, owner_note.handle, peer, peer_note.handle),
		   "serialize concurrent clients safely") ||
	    !check(minimm_client_note_read(owner, owner_note.handle, write_offset, destination,
					   sizeof(destination), &completed) == MINIMM_OK &&
			   completed == sizeof(destination),
		   "capture bytes before copying") ||
	    !check(minimm_client_note_copy(peer, read_only.handle,
					   MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_SHARE |
						   MINIMM_REMOTE_RIGHT_DELETE,
					   &copied_note) == MINIMM_OK &&
			   copied_note.handle != UINT64_C(0) &&
			   copied_note.size == MINIMM_PAGE_SIZE * UINT64_C(3) &&
			   copied_note.rights ==
				   (MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_SHARE |
				    MINIMM_REMOTE_RIGHT_DELETE) &&
			   !capability_is_zero(&copied_note.capability) &&
			   memcmp(&copied_note.capability, &owner_note.capability,
				  sizeof(copied_note.capability)) != 0,
		   "copy a note into an independent read-only record") ||
	    !check(minimm_client_note_read(peer, copied_note.handle, write_offset, copied_bytes,
					   sizeof(copied_bytes), &completed) == MINIMM_OK &&
			   completed == sizeof(copied_bytes) &&
			   memcmp(copied_bytes, destination, sizeof(copied_bytes)) == 0,
		   "copy all source bytes") ||
	    !check(minimm_client_note_copy(peer, read_only.handle, MINIMM_REMOTE_RIGHT_ALL,
					   &missing_note) == MINIMM_ERROR_NO_SPACE &&
			   missing_note.handle == UINT64_C(0),
		   "enforce total-size quota while copying") ||
	    !check((alias_note.capability = owner_note.capability,
		    minimm_client_note_open(peer, &alias_note.capability, 0U, &alias_note)) ==
				   MINIMM_OK &&
			   minimm_client_note_copy(peer, alias_note.handle, MINIMM_REMOTE_RIGHT_ALL,
						   &missing_note) == MINIMM_ERROR_PERMISSION &&
			   minimm_client_note_close(peer, &alias_note) == MINIMM_OK,
		   "require read permission on the copy source") ||
	    !check(minimm_client_note_write(owner, owner_note.handle, write_offset, changed,
					    sizeof(changed), &completed) == MINIMM_OK &&
			   completed == sizeof(changed) &&
			   minimm_client_note_read(peer, copied_note.handle, write_offset,
						   copied_bytes, sizeof(changed),
						   &completed) == MINIMM_OK &&
			   completed == sizeof(changed) && copied_bytes[0] == destination[0] &&
			   minimm_client_note_write(peer, copied_note.handle, write_offset, changed,
						    sizeof(changed),
						    &completed) == MINIMM_ERROR_PERMISSION,
		   "keep the copy independent and enforce destination rights") ||
	    !check(minimm_client_note_unlink(owner, &owner_note.capability) == MINIMM_OK,
		   "unlink shared capability") ||
	    !check(minimm_client_note_open(peer, &owner_note.capability, MINIMM_REMOTE_RIGHT_READ,
					   &missing_note) == MINIMM_ERROR_NOT_FOUND,
		   "reject opening an unlinked capability") ||
	    !check(minimm_client_note_read(peer, peer_note.handle, write_offset, destination,
					   sizeof(destination), &completed) == MINIMM_OK &&
			   completed == sizeof(destination),
		   "preserve existing handles after unlink") ||
	    !check(minimm_client_note_create(owner, MINIMM_PAGE_SIZE,
					     MINIMM_REMOTE_RIGHT_READ | MINIMM_REMOTE_RIGHT_WRITE,
					     &private_note) == MINIMM_OK &&
			   capability_is_zero(&private_note.capability),
		   "create a session-private note without a capability") ||
	    !check(minimm_client_note_close(owner, &private_note) == MINIMM_OK,
		   "close private note") ||
	    !check(minimm_client_note_unlink(peer, &copied_note.capability) == MINIMM_OK &&
			   minimm_client_note_close(peer, &copied_note) == MINIMM_OK &&
			   minimm_client_note_close(peer, &no_rights) == MINIMM_OK &&
			   minimm_client_note_close(peer, &read_only) == MINIMM_OK &&
			   minimm_client_note_close(peer, &peer_note) == MINIMM_OK &&
			   minimm_client_note_close(owner, &owner_note) == MINIMM_OK,
		   "close shared handles")) {
		goto cleanup;
	}

	success = true;

cleanup:
	if (peer != NULL) {
		if (alias_note.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(peer, &alias_note);
		}
		if (read_only.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(peer, &read_only);
		}
		if (no_rights.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(peer, &no_rights);
		}
		if (copied_note.handle != UINT64_C(0)) {
			(void)minimm_client_note_unlink(peer, &copied_note.capability);
			(void)minimm_client_note_close(peer, &copied_note);
		}
		if (peer_note.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(peer, &peer_note);
		}
		minimm_client_disconnect(peer);
	}
	if (owner != NULL) {
		if (private_note.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(owner, &private_note);
		}
		if (owner_note.handle != UINT64_C(0)) {
			(void)minimm_client_note_close(owner, &owner_note);
		}
		minimm_client_disconnect(owner);
	}
	if (server != NULL) {
		if (minimm_server_stop(server) != MINIMM_OK) {
			success = false;
		}
		minimm_server_destroy(server);
	}
	return success;
}

static bool test_stop_drains_idle_client(void)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	minimm_client_t *client = NULL;
	minimm_remote_note_t root = { 0 };
	minimm_remote_note_t leaf = { 0 };
	uint64_t response = UINT64_C(0);
	bool success = false;

	config.port = UINT16_C(0);
	config.max_clients = 1U;
	config.max_notes = 2U;
	config.max_note_size = MINIMM_PAGE_SIZE;
	config.max_total_note_size = MINIMM_PAGE_SIZE * UINT64_C(2);
	config.io_timeout_ms = UINT32_C(5000);
	config.memory.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;

	if (!check(minimm_server_create(&config, &server) == MINIMM_OK &&
			   minimm_server_start(server) == MINIMM_OK,
		   "start shutdown test server") ||
	    !check(minimm_client_connect("127.0.0.1", minimm_server_bound_port(server),
					 UINT32_C(5000), &client) == MINIMM_OK,
		   "connect idle shutdown client") ||
	    !check(minimm_client_note_create(client, MINIMM_PAGE_SIZE, MINIMM_REMOTE_RIGHT_ALL,
					     &root) == MINIMM_OK &&
			   minimm_client_note_copy(client, root.handle, MINIMM_REMOTE_RIGHT_ALL,
						   &leaf) == MINIMM_OK &&
			   minimm_client_note_unlink(client, &root.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &root) == MINIMM_OK,
		   "leave a linked snapshot child with a hidden parent") ||
	    !check(minimm_server_stop(server) == MINIMM_OK &&
			   minimm_server_bound_port(server) == UINT16_C(0),
		   "stop server and drain idle client") ||
	    !check(minimm_client_ping(client, UINT64_C(1), &response) == MINIMM_ERROR_IO,
		   "surface stopped transport as an I/O error")) {
		goto cleanup;
	}
	minimm_client_disconnect(client);
	client = NULL;
	(void)memset(&root, 0, sizeof(root));
	(void)memset(&leaf, 0, sizeof(leaf));
	if (!check(minimm_server_start(server) == MINIMM_OK &&
			   minimm_client_connect("127.0.0.1", minimm_server_bound_port(server),
						 UINT32_C(5000), &client) == MINIMM_OK,
		   "restart the drained snapshot server") ||
	    !check(minimm_client_note_create(client, MINIMM_PAGE_SIZE, MINIMM_REMOTE_RIGHT_ALL,
					     &root) == MINIMM_OK &&
			   minimm_client_note_copy(client, root.handle, MINIMM_REMOTE_RIGHT_ALL,
						   &leaf) == MINIMM_OK,
		   "restore all record and byte quota after stop") ||
	    !check(minimm_client_note_unlink(client, &root.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &root) == MINIMM_OK &&
			   minimm_client_note_unlink(client, &leaf.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &leaf) == MINIMM_OK,
		   "retire notes created after restart")) {
		goto cleanup;
	}
	success = true;

cleanup:
	minimm_client_disconnect(client);
	if (server != NULL) {
		(void)minimm_server_stop(server);
		minimm_server_destroy(server);
	}
	return success;
}

static bool test_copy_lineage_quota(void)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	minimm_client_t *client = NULL;
	minimm_remote_note_t ancestor = { 0 };
	minimm_remote_note_t child = { 0 };
	minimm_remote_note_t rejected = { 0 };
	minimm_remote_note_t replacement = { 0 };
	size_t generation = 0U;
	bool success = false;

	config.port = UINT16_C(0);
	config.max_clients = 1U;
	config.max_notes = 64U;
	config.max_note_size = MINIMM_PAGE_SIZE;
	config.max_total_note_size = MINIMM_PAGE_SIZE * UINT64_C(2);
	config.io_timeout_ms = UINT32_C(5000);
	config.memory.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;

	if (!check(minimm_server_create(&config, &server) == MINIMM_OK &&
			   minimm_server_start(server) == MINIMM_OK,
		   "start copy-lineage quota server") ||
	    !check(minimm_client_connect("127.0.0.1", minimm_server_bound_port(server),
					 UINT32_C(5000), &client) == MINIMM_OK,
		   "connect copy-lineage quota client") ||
	    !check(minimm_client_note_create(client, MINIMM_PAGE_SIZE, MINIMM_REMOTE_RIGHT_ALL,
					     &ancestor) == MINIMM_OK &&
			   minimm_client_note_copy(client, ancestor.handle, MINIMM_REMOTE_RIGHT_ALL,
						   &child) == MINIMM_OK,
		   "create a full-size snapshot parent and child") ||
	    !check(minimm_client_note_unlink(client, &ancestor.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &ancestor) == MINIMM_OK,
		   "retire the visible full-size snapshot parent") ||
	    !check(minimm_client_note_copy(client, child.handle, MINIMM_REMOTE_RIGHT_ALL,
					   &rejected) == MINIMM_ERROR_NO_SPACE &&
			   rejected.handle == UINT64_C(0),
		   "charge a hidden snapshot ancestor against the byte quota") ||
	    !check(minimm_client_note_unlink(client, &child.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &child) == MINIMM_OK &&
			   minimm_client_note_create(client, MINIMM_PAGE_SIZE,
						     MINIMM_REMOTE_RIGHT_ALL,
						     &replacement) == MINIMM_OK,
		   "release ancestor byte quota with the last descendant") ||
	    !check(minimm_client_note_unlink(client, &replacement.capability) == MINIMM_OK &&
			   minimm_client_note_close(client, &replacement) == MINIMM_OK,
		   "retire the byte-quota replacement") ||
	    !check(minimm_client_note_create(client, UINT64_C(0), MINIMM_REMOTE_RIGHT_READ,
					     &ancestor) == MINIMM_OK,
		   "create a private zero-size snapshot lineage root")) {
		goto cleanup;
	}

	for (generation = 1U; generation < config.max_notes; ++generation) {
		if (!check(minimm_client_note_copy(client, ancestor.handle,
						   MINIMM_REMOTE_RIGHT_READ, &child) == MINIMM_OK &&
				   minimm_client_note_close(client, &ancestor) == MINIMM_OK,
			   "extend and close one private snapshot generation")) {
			goto cleanup;
		}
		ancestor = child;
		(void)memset(&child, 0, sizeof(child));
	}
	if (!check(minimm_client_note_copy(client, ancestor.handle, MINIMM_REMOTE_RIGHT_READ,
					   &rejected) == MINIMM_ERROR_NO_SPACE &&
			   rejected.handle == UINT64_C(0),
		   "charge a bounded hidden snapshot chain against the record quota") ||
	    !check(minimm_client_note_close(client, &ancestor) == MINIMM_OK &&
			   minimm_client_note_create(client, UINT64_C(0), MINIMM_REMOTE_RIGHT_READ,
						     &replacement) == MINIMM_OK,
		   "release an iterative ancestor chain with the last descendant") ||
	    !check(minimm_client_note_close(client, &replacement) == MINIMM_OK,
		   "retire the record-quota replacement")) {
		goto cleanup;
	}
	success = true;

cleanup:
	if (client != NULL) {
		if (rejected.handle != UINT64_C(0)) {
			(void)minimm_client_note_unlink(client, &rejected.capability);
			(void)minimm_client_note_close(client, &rejected);
		}
		if (replacement.handle != UINT64_C(0)) {
			(void)minimm_client_note_unlink(client, &replacement.capability);
			(void)minimm_client_note_close(client, &replacement);
		}
		if (child.handle != UINT64_C(0)) {
			(void)minimm_client_note_unlink(client, &child.capability);
			(void)minimm_client_note_close(client, &child);
		}
		if (ancestor.handle != UINT64_C(0)) {
			(void)minimm_client_note_unlink(client, &ancestor.capability);
			(void)minimm_client_note_close(client, &ancestor);
		}
		minimm_client_disconnect(client);
	}
	if (server != NULL) {
		if (minimm_server_stop(server) != MINIMM_OK) {
			success = false;
		}
		minimm_server_destroy(server);
	}
	return success;
}

int main(void)
{
	if (!test_capability_text() || !test_failed_client_calls_clear_outputs() ||
	    !test_client_accepts_empty_error_progress() || !test_shared_note_service() ||
	    !test_copy_lineage_quota() || !test_stop_drains_idle_client()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
