#include "protocol.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

void minimm_protocol_put_u16(uint8_t *destination, uint16_t value)
{
	destination[0] = (uint8_t)(value >> 8U);
	destination[1] = (uint8_t)value;
}

void minimm_protocol_put_u32(uint8_t *destination, uint32_t value)
{
	destination[0] = (uint8_t)(value >> 24U);
	destination[1] = (uint8_t)(value >> 16U);
	destination[2] = (uint8_t)(value >> 8U);
	destination[3] = (uint8_t)value;
}

void minimm_protocol_put_u64(uint8_t *destination, uint64_t value)
{
	minimm_protocol_put_u32(destination, (uint32_t)(value >> 32U));
	minimm_protocol_put_u32(destination + 4U, (uint32_t)value);
}

uint16_t minimm_protocol_get_u16(const uint8_t *source)
{
	return (uint16_t)(((uint16_t)source[0] << 8U) | (uint16_t)source[1]);
}

uint32_t minimm_protocol_get_u32(const uint8_t *source)
{
	return ((uint32_t)source[0] << 24U) | ((uint32_t)source[1] << 16U) |
	       ((uint32_t)source[2] << 8U) | (uint32_t)source[3];
}

uint64_t minimm_protocol_get_u64(const uint8_t *source)
{
	return ((uint64_t)minimm_protocol_get_u32(source) << 32U) |
	       (uint64_t)minimm_protocol_get_u32(source + 4U);
}

static bool minimm_protocol_header_is_valid(const minimm_protocol_header_t *header)
{
	if (header->magic != MINIMM_PROTOCOL_MAGIC ||
	    header->header_size != MINIMM_PROTOCOL_HEADER_SIZE ||
	    (header->flags & ~MINIMM_PROTOCOL_FLAG_RESPONSE) != UINT16_C(0) ||
	    header->payload_length > MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE ||
	    header->request_id == UINT64_C(0) || header->reserved != UINT32_C(0)) {
		return false;
	}
	return (header->flags & MINIMM_PROTOCOL_FLAG_RESPONSE) != UINT16_C(0) ||
	       header->wire_status == (uint32_t)MINIMM_PROTOCOL_STATUS_OK;
}

bool minimm_protocol_encode_header(const minimm_protocol_header_t *header,
				   uint8_t output[MINIMM_PROTOCOL_HEADER_SIZE])
{
	minimm_protocol_header_t input = { 0 };

	if (header == NULL || output == NULL) {
		return false;
	}
	input = *header;
	if (!minimm_protocol_header_is_valid(&input)) {
		return false;
	}

	minimm_protocol_put_u32(output, input.magic);
	output[4] = input.major;
	output[5] = input.minor;
	minimm_protocol_put_u16(output + 6U, input.header_size);
	minimm_protocol_put_u16(output + 8U, input.opcode);
	minimm_protocol_put_u16(output + 10U, input.flags);
	minimm_protocol_put_u32(output + 12U, input.wire_status);
	minimm_protocol_put_u64(output + 16U, input.request_id);
	minimm_protocol_put_u32(output + 24U, input.payload_length);
	minimm_protocol_put_u32(output + 28U, input.reserved);
	return true;
}

bool minimm_protocol_decode_header(const uint8_t input[MINIMM_PROTOCOL_HEADER_SIZE],
				   minimm_protocol_header_t *out_header)
{
	minimm_protocol_header_t header = { 0 };

	if (input == NULL || out_header == NULL) {
		return false;
	}

	header.magic = minimm_protocol_get_u32(input);
	header.major = input[4];
	header.minor = input[5];
	header.header_size = minimm_protocol_get_u16(input + 6U);
	header.opcode = minimm_protocol_get_u16(input + 8U);
	header.flags = minimm_protocol_get_u16(input + 10U);
	header.wire_status = minimm_protocol_get_u32(input + 12U);
	header.request_id = minimm_protocol_get_u64(input + 16U);
	header.payload_length = minimm_protocol_get_u32(input + 24U);
	header.reserved = minimm_protocol_get_u32(input + 28U);
	if (!minimm_protocol_header_is_valid(&header)) {
		return false;
	}

	*out_header = header;
	return true;
}

minimm_protocol_wire_status_t minimm_protocol_status_from_minimm(minimm_status_t status)
{
	switch (status) {
	case MINIMM_OK:
		return MINIMM_PROTOCOL_STATUS_OK;
	case MINIMM_ERROR_INVALID_ARGUMENT:
		return MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT;
	case MINIMM_ERROR_OUT_OF_MEMORY:
		return MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY;
	case MINIMM_ERROR_NO_SPACE:
		return MINIMM_PROTOCOL_STATUS_NO_SPACE;
	case MINIMM_ERROR_IO:
		return MINIMM_PROTOCOL_STATUS_IO_ERROR;
	case MINIMM_ERROR_BUSY:
		return MINIMM_PROTOCOL_STATUS_BUSY;
	case MINIMM_ERROR_NOT_FOUND:
		return MINIMM_PROTOCOL_STATUS_NOT_FOUND;
	case MINIMM_ERROR_PERMISSION:
		return MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED;
	case MINIMM_ERROR_ADDRESS_IN_USE:
		return MINIMM_PROTOCOL_STATUS_ADDRESS_IN_USE;
	case MINIMM_ERROR_UNSUPPORTED:
		return MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE;
	default:
		return MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR;
	}
}

minimm_status_t minimm_protocol_status_to_minimm(minimm_protocol_wire_status_t status)
{
	switch (status) {
	case MINIMM_PROTOCOL_STATUS_OK:
		return MINIMM_OK;
	case MINIMM_PROTOCOL_STATUS_MALFORMED_MESSAGE:
	case MINIMM_PROTOCOL_STATUS_INVALID_ARGUMENT:
		return MINIMM_ERROR_INVALID_ARGUMENT;
	case MINIMM_PROTOCOL_STATUS_UNSUPPORTED_VERSION:
	case MINIMM_PROTOCOL_STATUS_UNSUPPORTED_OPCODE:
		return MINIMM_ERROR_UNSUPPORTED;
	case MINIMM_PROTOCOL_STATUS_NOT_FOUND:
		return MINIMM_ERROR_NOT_FOUND;
	case MINIMM_PROTOCOL_STATUS_PERMISSION_DENIED:
		return MINIMM_ERROR_PERMISSION;
	case MINIMM_PROTOCOL_STATUS_OUT_OF_MEMORY:
		return MINIMM_ERROR_OUT_OF_MEMORY;
	case MINIMM_PROTOCOL_STATUS_NO_SPACE:
	case MINIMM_PROTOCOL_STATUS_LIMIT_EXCEEDED:
		return MINIMM_ERROR_NO_SPACE;
	case MINIMM_PROTOCOL_STATUS_IO_ERROR:
	case MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR:
		return MINIMM_ERROR_IO;
	case MINIMM_PROTOCOL_STATUS_BUSY:
		return MINIMM_ERROR_BUSY;
	case MINIMM_PROTOCOL_STATUS_ADDRESS_IN_USE:
		return MINIMM_ERROR_ADDRESS_IN_USE;
	default:
		return MINIMM_ERROR_IO;
	}
}

static size_t minimm_protocol_io_chunk(size_t remaining)
{
	const size_t maximum = (size_t)SSIZE_MAX;

	return remaining < maximum ? remaining : maximum;
}

minimm_protocol_io_result_t minimm_protocol_recv_exact(int socket_fd, void *buffer, size_t length)
{
	uint8_t *bytes = buffer;
	size_t completed = 0U;

	if (socket_fd < 0 || (buffer == NULL && length != 0U)) {
		errno = EINVAL;
		return MINIMM_PROTOCOL_IO_ERROR;
	}

	while (completed < length) {
		const size_t chunk = minimm_protocol_io_chunk(length - completed);
		const ssize_t result = recv(socket_fd, bytes + completed, chunk, 0);

		if (result > 0) {
			completed += (size_t)result;
			continue;
		}
		if (result == 0) {
			if (completed == 0U) {
				return MINIMM_PROTOCOL_IO_CLOSED;
			}
			errno = ECONNRESET;
			return MINIMM_PROTOCOL_IO_ERROR;
		}
		if (errno != EINTR) {
			return MINIMM_PROTOCOL_IO_ERROR;
		}
	}
	return MINIMM_PROTOCOL_IO_OK;
}

minimm_protocol_io_result_t minimm_protocol_send_all(int socket_fd, const void *buffer,
						     size_t length)
{
	const uint8_t *bytes = buffer;
	size_t completed = 0U;
	int flags = 0;

	if (socket_fd < 0 || (buffer == NULL && length != 0U)) {
		errno = EINVAL;
		return MINIMM_PROTOCOL_IO_ERROR;
	}

#ifdef MSG_NOSIGNAL
	flags = MSG_NOSIGNAL;
#endif
	while (completed < length) {
		const size_t chunk = minimm_protocol_io_chunk(length - completed);
		const ssize_t result = send(socket_fd, bytes + completed, chunk, flags);

		if (result > 0) {
			completed += (size_t)result;
			continue;
		}
		if (result == 0) {
			errno = EPIPE;
			return MINIMM_PROTOCOL_IO_ERROR;
		}
		if (errno != EINTR) {
			return MINIMM_PROTOCOL_IO_ERROR;
		}
	}
	return MINIMM_PROTOCOL_IO_OK;
}
