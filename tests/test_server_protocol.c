#include "minimm/server.h"

#include "protocol.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static int connect_loopback(uint16_t port)
{
	struct sockaddr_in address = { 0 };
	struct timeval timeout = {
		.tv_sec = 5,
		.tv_usec = 0,
	};
	int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (socket_fd < 0) {
		return -1;
	}
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1 ||
	    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, (socklen_t)sizeof(timeout)) !=
		    0 ||
	    connect(socket_fd, (const struct sockaddr *)&address, (socklen_t)sizeof(address)) !=
		    0) {
		(void)close(socket_fd);
		return -1;
	}
	return socket_fd;
}

static bool send_bytes(int socket_fd, const uint8_t *bytes, size_t length, bool fragmented)
{
	size_t offset = 0U;

	if (!fragmented) {
		return minimm_protocol_send_all(socket_fd, bytes, length) == MINIMM_PROTOCOL_IO_OK;
	}
	while (offset < length) {
		if (minimm_protocol_send_all(socket_fd, bytes + offset, 1U) !=
		    MINIMM_PROTOCOL_IO_OK) {
			return false;
		}
		offset += 1U;
	}
	return true;
}

static bool send_header(int socket_fd, const minimm_protocol_header_t *header, bool fragmented)
{
	uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };

	return minimm_protocol_encode_header(header, wire_header) &&
	       send_bytes(socket_fd, wire_header, sizeof(wire_header), fragmented);
}

static bool send_request(int socket_fd, const minimm_protocol_header_t *header,
			 const uint8_t *payload, bool fragmented)
{
	return send_header(socket_fd, header, fragmented) &&
	       (header->payload_length == UINT32_C(0) ||
		send_bytes(socket_fd, payload, (size_t)header->payload_length, fragmented));
}

static bool receive_response(int socket_fd, minimm_protocol_header_t *out_header, uint8_t *payload,
			     size_t payload_capacity)
{
	uint8_t wire_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };

	if (minimm_protocol_recv_exact(socket_fd, wire_header, sizeof(wire_header)) !=
		    MINIMM_PROTOCOL_IO_OK ||
	    !minimm_protocol_decode_header(wire_header, out_header) ||
	    out_header->flags != MINIMM_PROTOCOL_FLAG_RESPONSE ||
	    (size_t)out_header->payload_length > payload_capacity) {
		return false;
	}
	return out_header->payload_length == UINT32_C(0) ||
	       minimm_protocol_recv_exact(socket_fd, payload, (size_t)out_header->payload_length) ==
		       MINIMM_PROTOCOL_IO_OK;
}

static bool raw_hello(int socket_fd, uint64_t request_id, uint32_t receive_maximum, bool fragmented,
		      minimm_protocol_header_t *out_response)
{
	const minimm_protocol_header_t request = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = (uint16_t)MINIMM_PROTOCOL_OP_HELLO,
		.flags = UINT16_C(0),
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_OK,
		.request_id = request_id,
		.payload_length = MINIMM_PROTOCOL_HELLO_REQUEST_SIZE,
		.reserved = UINT32_C(0),
	};
	uint8_t request_payload[MINIMM_PROTOCOL_HELLO_REQUEST_SIZE] = { 0 };
	uint8_t response_payload[MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE] = { 0 };

	request_payload[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	request_payload[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	request_payload[2] = MINIMM_PROTOCOL_VERSION_MAJOR;
	request_payload[3] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u32(request_payload + 8U, receive_maximum);
	return send_request(socket_fd, &request, request_payload, fragmented) &&
	       receive_response(socket_fd, out_response, response_payload,
				sizeof(response_payload)) &&
	       out_response->opcode == request.opcode &&
	       out_response->request_id == request.request_id &&
	       out_response->wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_OK &&
	       out_response->payload_length == MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE &&
	       response_payload[0] == MINIMM_PROTOCOL_VERSION_MAJOR &&
	       response_payload[1] == MINIMM_PROTOCOL_VERSION_MINOR;
}

static bool test_reject_feature_bits(void)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	minimm_protocol_header_t request = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = (uint16_t)MINIMM_PROTOCOL_OP_HELLO,
		.flags = UINT16_C(0),
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_OK,
		.request_id = UINT64_C(42),
		.payload_length = MINIMM_PROTOCOL_HELLO_REQUEST_SIZE,
		.reserved = UINT32_C(0),
	};
	minimm_protocol_header_t response = { 0 };
	uint8_t request_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	uint8_t response_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	uint8_t payload[MINIMM_PROTOCOL_HELLO_REQUEST_SIZE] = { 0 };
	uint8_t byte = UINT8_C(0);
	int socket_fd = -1;
	bool success = false;

	config.port = UINT16_C(0);
	config.max_clients = 1U;
	config.max_notes = 1U;
	config.max_note_size = MINIMM_PAGE_SIZE;
	config.max_total_note_size = MINIMM_PAGE_SIZE;
	config.io_timeout_ms = UINT32_C(5000);
	config.memory.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;

	if (!check(minimm_server_create(&config, &server) == MINIMM_OK &&
			   minimm_server_start(server) == MINIMM_OK,
		   "start raw protocol server")) {
		goto cleanup;
	}
	socket_fd = connect_loopback(minimm_server_bound_port(server));
	payload[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	payload[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	payload[2] = MINIMM_PROTOCOL_VERSION_MAJOR;
	payload[3] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u32(payload + 4U, UINT32_C(1));
	minimm_protocol_put_u32(payload + 8U, MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE);

	if (!check(socket_fd >= 0, "connect raw protocol peer") ||
	    !check(minimm_protocol_encode_header(&request, request_header), "encode raw HELLO") ||
	    !check(minimm_protocol_send_all(socket_fd, request_header, sizeof(request_header)) ==
				   MINIMM_PROTOCOL_IO_OK &&
			   minimm_protocol_send_all(socket_fd, payload, sizeof(payload)) ==
				   MINIMM_PROTOCOL_IO_OK,
		   "send HELLO with unsupported feature bits") ||
	    !check(minimm_protocol_recv_exact(socket_fd, response_header,
					      sizeof(response_header)) == MINIMM_PROTOCOL_IO_OK &&
			   minimm_protocol_decode_header(response_header, &response),
		   "receive feature rejection") ||
	    !check(response.opcode == request.opcode &&
			   response.flags == MINIMM_PROTOCOL_FLAG_RESPONSE &&
			   response.request_id == request.request_id &&
			   response.wire_status ==
				   (uint32_t)MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT &&
			   response.payload_length == UINT32_C(0),
		   "return a framed invalid-argument response") ||
	    !check(minimm_protocol_recv_exact(socket_fd, &byte, 1U) == MINIMM_PROTOCOL_IO_CLOSED,
		   "close after a rejected mandatory HELLO")) {
		goto cleanup;
	}
	success = true;

cleanup:
	if (socket_fd >= 0) {
		(void)close(socket_fd);
	}
	if (server != NULL) {
		(void)minimm_server_stop(server);
		minimm_server_destroy(server);
	}
	return success;
}

static bool test_fragmented_state_machine(void)
{
	minimm_server_config_t config = minimm_server_config_default();
	minimm_server_t *server = NULL;
	minimm_protocol_header_t request = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = UINT16_C(0x7fff),
		.flags = UINT16_C(0),
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_OK,
		.request_id = UINT64_C(2),
		.payload_length = UINT32_C(0),
		.reserved = UINT32_C(0),
	};
	minimm_protocol_header_t response = { 0 };
	uint8_t write_payload[MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE + UINT32_C(1)] = { 0 };
	uint8_t response_payload[MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE] = { 0 };
	uint8_t ping_payload[MINIMM_PROTOCOL_PING_PAYLOAD_SIZE] = { 0 };
	uint8_t resize_payload[MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE] = { 0 };
	uint8_t hello_payload[MINIMM_PROTOCOL_HELLO_REQUEST_SIZE] = { 0 };
	uint8_t byte = UINT8_C(0);
	int socket_fd = -1;
	bool success = false;

	config.port = UINT16_C(0);
	config.max_clients = 4U;
	config.max_notes = 2U;
	config.max_note_size = MINIMM_PAGE_SIZE;
	config.max_total_note_size = MINIMM_PAGE_SIZE * UINT64_C(2);
	config.max_payload_size = MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE;
	config.io_timeout_ms = UINT32_C(5000);
	config.memory.physical_memory_size = (size_t)MINIMM_PAGE_SIZE;

	if (!check(minimm_server_create(&config, &server) == MINIMM_OK &&
			   minimm_server_start(server) == MINIMM_OK,
		   "start fragmented state-machine server")) {
		goto cleanup;
	}
	socket_fd = connect_loopback(minimm_server_bound_port(server));
	if (!check(socket_fd >= 0, "connect fragmented raw peer") ||
	    !check(raw_hello(socket_fd, UINT64_C(1), MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE, true,
			     &response),
		   "complete a byte-fragmented HELLO")) {
		goto cleanup;
	}

	if (!check(send_request(socket_fd, &request, NULL, true) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "exchange an unknown opcode") ||
	    !check(response.opcode == request.opcode && response.request_id == request.request_id &&
			   response.wire_status ==
				   (uint32_t)MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE &&
			   response.payload_length == UINT32_C(0),
		   "return unsupported-opcode without losing framing")) {
		goto cleanup;
	}

	request.opcode = (uint16_t)MINIMM_PROTOCOL_OP_WRITE;
	request.request_id = UINT64_C(3);
	request.payload_length = (uint32_t)sizeof(write_payload);
	minimm_protocol_put_u64(write_payload, UINT64_C(1));
	minimm_protocol_put_u32(write_payload + 16U, UINT32_C(2));
	if (!check(send_request(socket_fd, &request, write_payload, true) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "exchange a fragmented WRITE with a mismatched inner length") ||
	    !check(response.wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE &&
			   response.payload_length == MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE &&
			   minimm_protocol_get_u32(response_payload) == UINT32_C(0) &&
			   minimm_protocol_get_u32(response_payload + 4U) == UINT32_C(0),
		   "reject malformed WRITE with zero progress")) {
		goto cleanup;
	}

	request.opcode = (uint16_t)MINIMM_PROTOCOL_OP_RESIZE;
	request.request_id = UINT64_C(4);
	request.payload_length = MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE;
	minimm_protocol_put_u64(resize_payload, UINT64_C(999));
	minimm_protocol_put_u64(resize_payload + 8U, MINIMM_PAGE_SIZE);
	if (!check(send_request(socket_fd, &request, resize_payload, true) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "exchange a RESIZE for an unknown handle") ||
	    !check(response.wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_NOT_FOUND &&
			   response.payload_length == UINT32_C(0),
		   "omit misleading resize progress before an operation starts")) {
		goto cleanup;
	}

	request.opcode = (uint16_t)MINIMM_PROTOCOL_OP_PING;
	request.request_id = UINT64_C(5);
	request.payload_length = MINIMM_PROTOCOL_PING_PAYLOAD_SIZE;
	minimm_protocol_put_u64(ping_payload, UINT64_C(0x0102030405060708));
	if (!check(send_request(socket_fd, &request, ping_payload, true) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "ping after malformed operation") ||
	    !check(response.wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_OK &&
			   response.payload_length == MINIMM_PROTOCOL_PING_PAYLOAD_SIZE &&
			   memcmp(response_payload, ping_payload, sizeof(ping_payload)) == 0,
		   "preserve the connection after an operation error")) {
		goto cleanup;
	}

	hello_payload[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	hello_payload[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	hello_payload[2] = MINIMM_PROTOCOL_VERSION_MAJOR;
	hello_payload[3] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u32(hello_payload + 8U, MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE);
	request.opcode = (uint16_t)MINIMM_PROTOCOL_OP_HELLO;
	request.request_id = UINT64_C(6);
	request.payload_length = MINIMM_PROTOCOL_HELLO_REQUEST_SIZE;
	if (!check(send_request(socket_fd, &request, hello_payload, true) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "send a duplicate HELLO") ||
	    !check(response.wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE &&
			   response.payload_length == UINT32_C(0),
		   "reject a duplicate HELLO") ||
	    !check(minimm_protocol_recv_exact(socket_fd, &byte, 1U) == MINIMM_PROTOCOL_IO_CLOSED,
		   "close after a duplicate HELLO")) {
		goto cleanup;
	}
	(void)close(socket_fd);
	socket_fd = -1;

	socket_fd = connect_loopback(minimm_server_bound_port(server));
	if (!check(socket_fd >= 0 &&
			   raw_hello(socket_fd, UINT64_C(7), MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE,
				     false, &response),
		   "negotiate the minimum payload limit")) {
		goto cleanup;
	}
	request.opcode = (uint16_t)MINIMM_PROTOCOL_OP_PING;
	request.request_id = UINT64_C(8);
	request.payload_length = MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE + UINT32_C(1);
	if (!check(send_header(socket_fd, &request, false) &&
			   receive_response(socket_fd, &response, response_payload,
					    sizeof(response_payload)),
		   "send only an over-limit request header") ||
	    !check(response.wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED &&
			   response.payload_length == UINT32_C(0),
		   "reject the negotiated-limit violation without waiting for payload") ||
	    !check(minimm_protocol_recv_exact(socket_fd, &byte, 1U) == MINIMM_PROTOCOL_IO_CLOSED,
		   "close after a negotiated-limit violation")) {
		goto cleanup;
	}
	success = true;

cleanup:
	if (socket_fd >= 0) {
		(void)close(socket_fd);
	}
	if (server != NULL) {
		(void)minimm_server_stop(server);
		minimm_server_destroy(server);
	}
	return success;
}

int main(void)
{
	return test_reject_feature_bits() && test_fragmented_state_machine() ? EXIT_SUCCESS :
									       EXIT_FAILURE;
}
