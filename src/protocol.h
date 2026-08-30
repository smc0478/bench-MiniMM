#ifndef MINIMM_PROTOCOL_INTERNAL_H
#define MINIMM_PROTOCOL_INTERNAL_H

#include "minimm/minimm.h"
#include "minimm/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct minimm_protocol_header {
	uint32_t magic;
	uint8_t major;
	uint8_t minor;
	uint16_t header_size;
	uint16_t opcode;
	uint16_t flags;
	uint32_t wire_status;
	uint64_t request_id;
	uint32_t payload_length;
	uint32_t reserved;
} minimm_protocol_header_t;

typedef enum minimm_protocol_io_result {
	MINIMM_PROTOCOL_IO_OK = 0,
	MINIMM_PROTOCOL_IO_CLOSED,
	MINIMM_PROTOCOL_IO_ERROR
} minimm_protocol_io_result_t;

void minimm_protocol_put_u16(uint8_t *destination, uint16_t value);
void minimm_protocol_put_u32(uint8_t *destination, uint32_t value);
void minimm_protocol_put_u64(uint8_t *destination, uint64_t value);
uint16_t minimm_protocol_get_u16(const uint8_t *source);
uint32_t minimm_protocol_get_u32(const uint8_t *source);
uint64_t minimm_protocol_get_u64(const uint8_t *source);

/* Encode/decode validate the fixed v1 framing invariants, but not opcodes. */
bool minimm_protocol_encode_header(const minimm_protocol_header_t *header,
				   uint8_t output[MINIMM_PROTOCOL_HEADER_SIZE]);
bool minimm_protocol_decode_header(const uint8_t input[MINIMM_PROTOCOL_HEADER_SIZE],
				   minimm_protocol_header_t *out_header);

minimm_protocol_wire_status_t minimm_protocol_status_from_minimm(minimm_status_t status);
minimm_status_t minimm_protocol_status_to_minimm(minimm_protocol_wire_status_t status);

/* These helpers operate on blocking sockets. */
minimm_protocol_io_result_t minimm_protocol_recv_exact(int socket_fd, void *buffer, size_t length);
minimm_protocol_io_result_t minimm_protocol_send_all(int socket_fd, const void *buffer,
						     size_t length);

#endif
