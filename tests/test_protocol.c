#include "protocol.h"

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

static bool test_big_endian_integers(void)
{
	uint8_t storage[17] = { 0 };
	uint8_t *bytes = storage + 1U;
	static const uint8_t expected[14] = { 0x12U, 0x34U, 0x89U, 0xabU, 0xcdU, 0xefU, 0x01U,
					      0x23U, 0x45U, 0x67U, 0x89U, 0xabU, 0xcdU, 0xefU };

	minimm_protocol_put_u16(bytes, UINT16_C(0x1234));
	minimm_protocol_put_u32(bytes + 2U, UINT32_C(0x89abcdef));
	minimm_protocol_put_u64(bytes + 6U, UINT64_C(0x0123456789abcdef));
	return check(memcmp(bytes, expected, sizeof(expected)) == 0,
		     "encode unaligned big-endian integers") &&
	       check(minimm_protocol_get_u16(bytes) == UINT16_C(0x1234) &&
			     minimm_protocol_get_u32(bytes + 2U) == UINT32_C(0x89abcdef) &&
			     minimm_protocol_get_u64(bytes + 6U) == UINT64_C(0x0123456789abcdef),
		     "decode unaligned big-endian integers");
}

static bool test_golden_header(void)
{
	typedef union header_wire_overlap {
		minimm_protocol_header_t header;
		uint8_t wire[MINIMM_PROTOCOL_HEADER_SIZE];
	} header_wire_overlap_t;
	const minimm_protocol_header_t header = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = (uint16_t)MINIMM_PROTOCOL_OP_WRITE,
		.flags = MINIMM_PROTOCOL_FLAG_RESPONSE,
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_IO_ERROR,
		.request_id = UINT64_C(0x0102030405060708),
		.payload_length = MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE,
		.reserved = UINT32_C(0),
	};
	static const uint8_t golden[MINIMM_PROTOCOL_HEADER_SIZE] = {
		0x4dU, 0x49U, 0x4dU, 0x4dU, 0x01U, 0x00U, 0x00U, 0x20U, 0x01U, 0x05U, 0x00U,
		0x01U, 0x00U, 0x00U, 0x00U, 0x09U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U,
		0x07U, 0x08U, 0x00U, 0x10U, 0x00U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U
	};
	header_wire_overlap_t overlap = { .wire = { 0 } };
	uint8_t encoded[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	minimm_protocol_header_t decoded = { 0 };

	overlap.header = header;
	return check(minimm_protocol_encode_header(&header, encoded),
		     "encode valid response header") &&
	       check(memcmp(encoded, golden, sizeof(golden)) == 0,
		     "header matches the golden network-order vector") &&
	       check(minimm_protocol_encode_header(&overlap.header, overlap.wire),
		     "encode a header into overlapping storage") &&
	       check(memcmp(overlap.wire, golden, sizeof(golden)) == 0,
		     "preserve an overlapped header input") &&
	       check(minimm_protocol_decode_header(golden, &decoded),
		     "decode golden response header") &&
	       check(decoded.magic == header.magic && decoded.major == header.major &&
			     decoded.minor == header.minor &&
			     decoded.header_size == header.header_size &&
			     decoded.opcode == header.opcode && decoded.flags == header.flags &&
			     decoded.wire_status == header.wire_status &&
			     decoded.request_id == header.request_id &&
			     decoded.payload_length == header.payload_length &&
			     decoded.reserved == header.reserved,
		     "decoded header preserves every field");
}

static bool test_header_validation(void)
{
	minimm_protocol_header_t header = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = (uint16_t)MINIMM_PROTOCOL_OP_PING,
		.flags = UINT16_C(0),
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_OK,
		.request_id = UINT64_C(9),
		.payload_length = MINIMM_PROTOCOL_PING_PAYLOAD_SIZE,
		.reserved = UINT32_C(0),
	};
	uint8_t encoded[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	uint8_t malformed[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	minimm_protocol_header_t decoded = { 0 };

	if (!check(minimm_protocol_encode_header(&header, encoded),
		   "encode valid request header")) {
		return false;
	}

	(void)memcpy(malformed, encoded, sizeof(malformed));
	malformed[0] = 0U;
	if (!check(!minimm_protocol_decode_header(malformed, &decoded), "reject bad magic")) {
		return false;
	}
	(void)memcpy(malformed, encoded, sizeof(malformed));
	minimm_protocol_put_u32(malformed + 24U,
				MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE + UINT32_C(1));
	if (!check(!minimm_protocol_decode_header(malformed, &decoded),
		   "reject payload above the hard limit")) {
		return false;
	}
	(void)memcpy(malformed, encoded, sizeof(malformed));
	minimm_protocol_put_u32(malformed + 12U, (uint32_t)MINIMM_PROTOCOL_STATUS_IO_ERROR);
	if (!check(!minimm_protocol_decode_header(malformed, &decoded),
		   "reject a nonzero request status")) {
		return false;
	}
	(void)memcpy(malformed, encoded, sizeof(malformed));
	minimm_protocol_put_u64(malformed + 16U, UINT64_C(0));
	if (!check(!minimm_protocol_decode_header(malformed, &decoded),
		   "reject reserved request id zero")) {
		return false;
	}

	encoded[4] = UINT8_C(2);
	return check(minimm_protocol_decode_header(encoded, &decoded) &&
			     decoded.major == UINT8_C(2),
		     "decode unsupported versions for caller-generated error responses");
}

static bool test_status_mapping(void)
{
	static const struct {
		minimm_status_t minimm;
		minimm_protocol_wire_status_t wire;
	} cases[] = { { MINIMM_OK, MINIMM_PROTOCOL_STATUS_OK },
		      { MINIMM_ERROR_INVALID_ARGUMENT, MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT },
		      { MINIMM_ERROR_OUT_OF_MEMORY, MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY },
		      { MINIMM_ERROR_NO_SPACE, MINIMM_PROTOCOL_STATUS_NO_SPACE },
		      { MINIMM_ERROR_IO, MINIMM_PROTOCOL_STATUS_IO_ERROR },
		      { MINIMM_ERROR_BUSY, MINIMM_PROTOCOL_STATUS_BUSY },
		      { MINIMM_ERROR_NOT_FOUND, MINIMM_PROTOCOL_STATUS_NOT_FOUND },
		      { MINIMM_ERROR_PERMISSION, MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED },
		      { MINIMM_ERROR_ADDRESS_IN_USE, MINIMM_PROTOCOL_STATUS_ADDRESS_IN_USE },
		      { MINIMM_ERROR_UNSUPPORTED, MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE } };
	size_t index = 0U;

	for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		if (!check(minimm_protocol_status_from_minimm(cases[index].minimm) ==
				   cases[index].wire,
			   "map MiniMM status to stable wire status") ||
		    !check(minimm_protocol_status_to_minimm(cases[index].wire) ==
				   cases[index].minimm,
			   "map stable wire status to MiniMM status")) {
			return false;
		}
	}
	return check(minimm_protocol_status_to_minimm(MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE) ==
			     MINIMM_ERROR_INVALID_ARGUMENT,
		     "map malformed framing to invalid argument") &&
	       check(minimm_protocol_status_to_minimm(MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED) ==
			     MINIMM_ERROR_NO_SPACE,
		     "map a wire resource limit to no space");
}

static bool test_socket_io(void)
{
	static const uint8_t message[] = "fragmented protocol frame";
	int sockets[2] = { -1, -1 };
	uint8_t received[sizeof(message)] = { 0 };
	uint8_t truncated[5] = { 0 };
	size_t index = 0U;

	if (!check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
		   "create fragmentation socket pair")) {
		return false;
	}
	for (index = 0U; index < sizeof(message); ++index) {
		if (!check(minimm_protocol_send_all(sockets[0], message + index, 1U) ==
				   MINIMM_PROTOCOL_IO_OK,
			   "send one-byte fragment")) {
			(void)close(sockets[0]);
			(void)close(sockets[1]);
			return false;
		}
	}
	if (!check(minimm_protocol_recv_exact(sockets[1], received, sizeof(received)) ==
			   MINIMM_PROTOCOL_IO_OK,
		   "receive fragmented bytes exactly") ||
	    !check(memcmp(message, received, sizeof(message)) == 0,
		   "fragmented transfer preserves bytes")) {
		(void)close(sockets[0]);
		(void)close(sockets[1]);
		return false;
	}
	(void)close(sockets[0]);
	(void)close(sockets[1]);

	if (!check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
		   "create clean EOF socket pair")) {
		return false;
	}
	(void)close(sockets[0]);
	if (!check(minimm_protocol_recv_exact(sockets[1], truncated, 1U) ==
			   MINIMM_PROTOCOL_IO_CLOSED,
		   "distinguish clean EOF before any byte")) {
		(void)close(sockets[1]);
		return false;
	}
	(void)close(sockets[1]);

	if (!check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
		   "create truncated message socket pair") ||
	    !check(minimm_protocol_send_all(sockets[0], message, 3U) == MINIMM_PROTOCOL_IO_OK,
		   "send a truncated prefix")) {
		if (sockets[0] >= 0) {
			(void)close(sockets[0]);
		}
		if (sockets[1] >= 0) {
			(void)close(sockets[1]);
		}
		return false;
	}
	(void)shutdown(sockets[0], SHUT_WR);
	if (!check(minimm_protocol_recv_exact(sockets[1], truncated, sizeof(truncated)) ==
			   MINIMM_PROTOCOL_IO_ERROR,
		   "treat EOF after a partial message as truncation")) {
		(void)close(sockets[0]);
		(void)close(sockets[1]);
		return false;
	}
	(void)close(sockets[0]);
	(void)close(sockets[1]);

	return check(minimm_protocol_recv_exact(-1, NULL, 0U) == MINIMM_PROTOCOL_IO_ERROR,
		     "reject an invalid receive socket") &&
	       check(minimm_protocol_send_all(-1, NULL, 0U) == MINIMM_PROTOCOL_IO_ERROR,
		     "reject an invalid send socket");
}

static bool test_public_constants(void)
{
	return check(sizeof(minimm_protocol_capability_t) == MINIMM_PROTOCOL_CAPABILITY_SIZE &&
			     sizeof(minimm_capability_t) == MINIMM_PROTOCOL_CAPABILITY_SIZE,
		     "capability has exactly 16 wire bytes") &&
	       check(MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE == UINT32_C(1048600) &&
			     MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE == UINT32_C(24) &&
			     MINIMM_PROTOCOL_COPY_REQUEST_SIZE == UINT32_C(16) &&
			     MINIMM_PROTOCOL_COPY_RESPONSE_SIZE == UINT32_C(40),
		     "payload limits include the fixed write prefix") &&
	       check((uint32_t)MINIMM_PROTOCOL_RIGHT_READ == (uint32_t)MINIMM_NOTE_RIGHT_READ &&
			     (uint32_t)MINIMM_PROTOCOL_RIGHT_WRITE ==
				     (uint32_t)MINIMM_NOTE_RIGHT_WRITE &&
			     (uint32_t)MINIMM_PROTOCOL_RIGHT_EDIT ==
				     (uint32_t)MINIMM_NOTE_RIGHT_EDIT &&
			     (uint32_t)MINIMM_PROTOCOL_RIGHT_SHARE ==
				     (uint32_t)MINIMM_NOTE_RIGHT_SHARE &&
			     (uint32_t)MINIMM_PROTOCOL_RIGHT_RESIZE ==
				     (uint32_t)MINIMM_NOTE_RIGHT_RESIZE &&
			     MINIMM_PROTOCOL_RIGHT_DELETE == (UINT32_C(1) << 5) &&
			     MINIMM_REMOTE_RIGHT_READ == MINIMM_PROTOCOL_RIGHT_READ &&
			     MINIMM_REMOTE_RIGHT_DELETE == MINIMM_PROTOCOL_RIGHT_DELETE &&
			     MINIMM_REMOTE_RIGHT_ALL == MINIMM_PROTOCOL_RIGHT_ALL,
		     "remote rights match core rights and add delete") &&
	       check(MINIMM_PROTOCOL_OP_HELLO == 1 && MINIMM_PROTOCOL_OP_PING == 2 &&
			     MINIMM_PROTOCOL_OP_CREATE == 0x100 &&
			     MINIMM_PROTOCOL_OP_UNLINK == 0x109 && MINIMM_PROTOCOL_OP_COPY == 0x10a,
		     "opcode assignments remain stable");
}

int main(void)
{
	if (!test_big_endian_integers() || !test_golden_header() || !test_header_validation() ||
	    !test_status_mapping() || !test_socket_io() || !test_public_constants()) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
