#include "minimm/client.h"

#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MINIMM_CLIENT_DEFAULT_TIMEOUT_MS UINT32_C(30000)

struct minimm_client {
	pthread_mutex_t lock;
	int socket_fd;
	uint64_t next_request_id;
	uint64_t max_note_size;
	uint32_t max_payload_size;
	bool broken;
};

static void minimm_client_break_locked(minimm_client_t *client)
{
	if (client->socket_fd >= 0) {
		(void)shutdown(client->socket_fd, SHUT_RDWR);
		(void)close(client->socket_fd);
		client->socket_fd = -1;
	}
	client->broken = true;
}

static uint64_t minimm_client_next_request_id_locked(minimm_client_t *client)
{
	const uint64_t request_id = client->next_request_id;

	client->next_request_id = request_id == UINT64_MAX ? UINT64_C(1) : request_id + UINT64_C(1);
	return request_id;
}

static bool minimm_client_wire_status_is_valid(uint32_t status)
{
	return status <= (uint32_t)MINIMM_PROTOCOL_STATUS_INTERNAL_ERROR;
}

static minimm_status_t
minimm_client_exchange_locked(minimm_client_t *client, minimm_protocol_opcode_t opcode,
			      const uint8_t *request_payload, uint32_t request_length,
			      uint8_t **out_response, uint32_t *out_response_length,
			      minimm_status_t *out_server_status)
{
	minimm_protocol_header_t request = {
		.magic = MINIMM_PROTOCOL_MAGIC,
		.major = MINIMM_PROTOCOL_VERSION_MAJOR,
		.minor = MINIMM_PROTOCOL_VERSION_MINOR,
		.header_size = MINIMM_PROTOCOL_HEADER_SIZE,
		.opcode = (uint16_t)opcode,
		.flags = UINT16_C(0),
		.wire_status = (uint32_t)MINIMM_PROTOCOL_STATUS_OK,
		.request_id = UINT64_C(0),
		.payload_length = request_length,
		.reserved = UINT32_C(0),
	};
	minimm_protocol_header_t response = { 0 };
	uint8_t request_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	uint8_t response_header[MINIMM_PROTOCOL_HEADER_SIZE] = { 0 };
	uint8_t *response_payload = NULL;
	minimm_protocol_io_result_t io_result = MINIMM_PROTOCOL_IO_OK;

	if (out_response == NULL || out_response_length == NULL || out_server_status == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_response = NULL;
	*out_response_length = UINT32_C(0);
	*out_server_status = MINIMM_ERROR_IO;
	if (client == NULL || (request_payload == NULL && request_length != UINT32_C(0)) ||
	    request_length > client->max_payload_size) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (client->broken || client->socket_fd < 0) {
		return MINIMM_ERROR_IO;
	}

	request.request_id = minimm_client_next_request_id_locked(client);
	if (!minimm_protocol_encode_header(&request, request_header)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	io_result =
		minimm_protocol_send_all(client->socket_fd, request_header, sizeof(request_header));
	if (io_result == MINIMM_PROTOCOL_IO_OK && request_length != UINT32_C(0)) {
		io_result = minimm_protocol_send_all(client->socket_fd, request_payload,
						     (size_t)request_length);
	}
	if (io_result != MINIMM_PROTOCOL_IO_OK) {
		minimm_client_break_locked(client);
		return MINIMM_ERROR_IO;
	}

	io_result = minimm_protocol_recv_exact(client->socket_fd, response_header,
					       sizeof(response_header));
	if (io_result != MINIMM_PROTOCOL_IO_OK ||
	    !minimm_protocol_decode_header(response_header, &response) ||
	    response.major != MINIMM_PROTOCOL_VERSION_MAJOR ||
	    response.minor != MINIMM_PROTOCOL_VERSION_MINOR ||
	    response.opcode != (uint16_t)opcode ||
	    response.flags != MINIMM_PROTOCOL_FLAG_RESPONSE ||
	    response.request_id != request.request_id ||
	    !minimm_client_wire_status_is_valid(response.wire_status) ||
	    response.payload_length > client->max_payload_size) {
		minimm_client_break_locked(client);
		return MINIMM_ERROR_IO;
	}

	if (response.payload_length != UINT32_C(0)) {
		response_payload = malloc((size_t)response.payload_length);
		if (response_payload == NULL) {
			minimm_client_break_locked(client);
			return MINIMM_ERROR_OUT_OF_MEMORY;
		}
		io_result = minimm_protocol_recv_exact(client->socket_fd, response_payload,
						       (size_t)response.payload_length);
		if (io_result != MINIMM_PROTOCOL_IO_OK) {
			free(response_payload);
			minimm_client_break_locked(client);
			return MINIMM_ERROR_IO;
		}
	}

	*out_server_status = minimm_protocol_status_to_minimm(
		(minimm_protocol_wire_status_t)response.wire_status);
	*out_response = response_payload;
	*out_response_length = response.payload_length;
	return MINIMM_OK;
}

static bool minimm_client_rights_are_valid(minimm_remote_rights_t rights)
{
	return (rights & ~(minimm_remote_rights_t)MINIMM_REMOTE_RIGHT_ALL) == 0U &&
	       ((rights & MINIMM_REMOTE_RIGHT_EDIT) == 0U ||
		(rights & MINIMM_REMOTE_RIGHT_WRITE) != 0U);
}

static bool minimm_client_protection_is_valid(minimm_prot_t protection)
{
	const minimm_prot_t allowed = MINIMM_PROT_READ | MINIMM_PROT_WRITE | MINIMM_PROT_EDIT |
				      MINIMM_PROT_EXEC;

	return (protection & ~allowed) == UINT32_C(0) &&
	       ((protection & MINIMM_PROT_EDIT) == UINT32_C(0) ||
		(protection & MINIMM_PROT_WRITE) != UINT32_C(0));
}

static bool minimm_client_capability_is_zero(const minimm_capability_t *capability)
{
	uint8_t combined = UINT8_C(0);
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		combined |= capability->bytes[index];
	}
	return combined == UINT8_C(0);
}

static bool minimm_client_note_size_is_valid(const minimm_client_t *client, uint64_t size)
{
	return size <= client->max_note_size &&
	       (size & (MINIMM_PAGE_SIZE - UINT64_C(1))) == UINT64_C(0);
}

static bool minimm_client_range_is_valid(uint64_t offset, size_t length)
{
	return (uint64_t)length <= UINT64_MAX - offset;
}

static minimm_status_t minimm_client_configure_socket(int socket_fd, uint32_t timeout_ms)
{
	const int no_delay = 1;
	struct timeval timeout = { 0 };

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

static bool minimm_client_make_deadline(uint32_t timeout_ms, struct timespec *out_deadline)
{
	struct timespec now = { 0 };
	const time_t seconds = (time_t)(timeout_ms / UINT32_C(1000));
	const long nanoseconds = (long)(timeout_ms % UINT32_C(1000)) * 1000000L;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return false;
	}
	out_deadline->tv_sec = now.tv_sec + seconds;
	out_deadline->tv_nsec = now.tv_nsec + nanoseconds;
	if (out_deadline->tv_nsec >= 1000000000L) {
		out_deadline->tv_sec += (time_t)1;
		out_deadline->tv_nsec -= 1000000000L;
	}
	return true;
}

static int minimm_client_deadline_remaining_ms(const struct timespec *deadline)
{
	struct timespec now = { 0 };
	time_t seconds = 0;
	long nanoseconds = 0L;
	uint64_t milliseconds = UINT64_C(0);

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	if (now.tv_sec > deadline->tv_sec ||
	    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
		return 0;
	}

	seconds = deadline->tv_sec - now.tv_sec;
	nanoseconds = deadline->tv_nsec - now.tv_nsec;
	if (nanoseconds < 0L) {
		seconds -= (time_t)1;
		nanoseconds += 1000000000L;
	}
	milliseconds = (uint64_t)seconds * UINT64_C(1000) +
		       ((uint64_t)nanoseconds + UINT64_C(999999)) / UINT64_C(1000000);
	if (milliseconds > (uint64_t)INT_MAX) {
		return INT_MAX;
	}
	return (int)milliseconds;
}

static bool minimm_client_wait_for_connect(int socket_fd, const struct timespec *deadline)
{
	struct pollfd descriptor = {
		.fd = socket_fd,
		.events = POLLOUT,
		.revents = 0,
	};

	for (;;) {
		const int remaining = minimm_client_deadline_remaining_ms(deadline);
		int poll_status = 0;
		int socket_error = 0;
		socklen_t error_size = (socklen_t)sizeof(socket_error);

		if (remaining <= 0) {
			errno = remaining == 0 ? ETIMEDOUT : EIO;
			return false;
		}
		poll_status = poll(&descriptor, (nfds_t)1U, remaining);
		if (poll_status == 0) {
			continue;
		}
		if (poll_status < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) != 0) {
			return false;
		}
		if (socket_error != 0) {
			errno = socket_error;
			return false;
		}
		return true;
	}
}

static bool minimm_client_connect_one(int socket_fd, const struct sockaddr *address,
				      socklen_t address_length, const struct timespec *deadline)
{
	const int original_flags = fcntl(socket_fd, F_GETFL);
	bool connected = false;
	int saved_error = 0;

	if (original_flags < 0 || fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
		return false;
	}
	if (connect(socket_fd, address, address_length) == 0) {
		connected = true;
	} else if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK ||
		   errno == EINTR) {
		connected = minimm_client_wait_for_connect(socket_fd, deadline);
	}
	saved_error = errno;
	if (fcntl(socket_fd, F_SETFL, original_flags) != 0) {
		return false;
	}
	errno = saved_error;
	return connected;
}

static int minimm_client_create_socket(int family, int type, int protocol)
{
	int socket_fd = -1;
	int descriptor_flags = 0;

#ifdef SOCK_CLOEXEC
	socket_fd = socket(family, type | SOCK_CLOEXEC, protocol);
	if (socket_fd >= 0 || errno != EINVAL) {
		return socket_fd;
	}
#endif
	socket_fd = socket(family, type, protocol);
	if (socket_fd < 0) {
		return -1;
	}
	do {
		descriptor_flags = fcntl(socket_fd, F_GETFD);
	} while (descriptor_flags < 0 && errno == EINTR);
	if (descriptor_flags >= 0) {
		int result = 0;

		do {
			result = fcntl(socket_fd, F_SETFD, descriptor_flags | FD_CLOEXEC);
		} while (result != 0 && errno == EINTR);
		if (result == 0) {
			return socket_fd;
		}
	}
	{
		const int error_number = errno;

		(void)close(socket_fd);
		errno = error_number;
	}
	return -1;
}

static minimm_status_t minimm_client_connect_socket(const char *host, uint16_t port,
						    uint32_t timeout_ms, int *out_socket_fd)
{
	struct addrinfo hints = { 0 };
	struct addrinfo *addresses = NULL;
	struct addrinfo *address = NULL;
	char service[6] = { 0 };
	struct timespec deadline = { 0 };
	int socket_fd = -1;
	int written = 0;

	if (host == NULL || host[0] == '\0' || port == UINT16_C(0) || out_socket_fd == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_socket_fd = -1;
	written = snprintf(service, sizeof(service), "%u", (unsigned)port);
	if (written <= 0 || (size_t)written >= sizeof(service)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if (getaddrinfo(host, service, &hints, &addresses) != 0) {
		return MINIMM_ERROR_IO;
	}
	if (!minimm_client_make_deadline(timeout_ms, &deadline)) {
		freeaddrinfo(addresses);
		return MINIMM_ERROR_IO;
	}

	for (address = addresses; address != NULL; address = address->ai_next) {
		socket_fd = minimm_client_create_socket(address->ai_family, address->ai_socktype,
							address->ai_protocol);
		if (socket_fd < 0) {
			continue;
		}
		if (minimm_client_connect_one(socket_fd, address->ai_addr, address->ai_addrlen,
					      &deadline) &&
		    minimm_client_configure_socket(socket_fd, timeout_ms) == MINIMM_OK) {
			*out_socket_fd = socket_fd;
			freeaddrinfo(addresses);
			return MINIMM_OK;
		}
		(void)close(socket_fd);
	}
	freeaddrinfo(addresses);
	return MINIMM_ERROR_IO;
}

static minimm_status_t minimm_client_handshake_locked(minimm_client_t *client)
{
	uint8_t request[MINIMM_PROTOCOL_HELLO_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;
	uint32_t negotiated_payload = UINT32_C(0);
	uint32_t max_handles = UINT32_C(0);
	uint32_t page_size = UINT32_C(0);
	uint32_t max_inflight = UINT32_C(0);
	uint64_t max_note_size = UINT64_C(0);

	request[0] = MINIMM_PROTOCOL_VERSION_MAJOR;
	request[1] = MINIMM_PROTOCOL_VERSION_MINOR;
	request[2] = MINIMM_PROTOCOL_VERSION_MAJOR;
	request[3] = MINIMM_PROTOCOL_VERSION_MINOR;
	minimm_protocol_put_u32(request + 4U, UINT32_C(0));
	minimm_protocol_put_u32(request + 8U, MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE);
	minimm_protocol_put_u32(request + 12U, UINT32_C(0));

	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_HELLO, request,
					       MINIMM_PROTOCOL_HELLO_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status != MINIMM_OK) {
		return status;
	}
	if (server_status != MINIMM_OK) {
		free(response);
		return server_status;
	}
	if (response_length != MINIMM_PROTOCOL_HELLO_RESPONSE_SIZE ||
	    response[0] != MINIMM_PROTOCOL_VERSION_MAJOR ||
	    response[1] != MINIMM_PROTOCOL_VERSION_MINOR ||
	    minimm_protocol_get_u16(response + 2U) != UINT16_C(0) ||
	    minimm_protocol_get_u32(response + 4U) != UINT32_C(0)) {
		free(response);
		minimm_client_break_locked(client);
		return MINIMM_ERROR_IO;
	}

	negotiated_payload = minimm_protocol_get_u32(response + 8U);
	max_handles = minimm_protocol_get_u32(response + 12U);
	page_size = minimm_protocol_get_u32(response + 16U);
	max_inflight = minimm_protocol_get_u32(response + 20U);
	max_note_size = minimm_protocol_get_u64(response + 24U);
	free(response);
	if (negotiated_payload < MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE ||
	    negotiated_payload > MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE ||
	    max_handles == UINT32_C(0) || page_size != (uint32_t)MINIMM_PAGE_SIZE ||
	    max_inflight == UINT32_C(0) || max_note_size == UINT64_C(0) ||
	    (max_note_size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		minimm_client_break_locked(client);
		return MINIMM_ERROR_IO;
	}
	client->max_payload_size = negotiated_payload;
	client->max_note_size = max_note_size;
	return MINIMM_OK;
}

minimm_status_t minimm_client_connect(const char *host, uint16_t port, uint32_t timeout_ms,
				      minimm_client_t **out_client)
{
	minimm_client_t *client = NULL;
	minimm_status_t status = MINIMM_OK;
	int socket_fd = -1;

	if (out_client == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_client = NULL;
	if (timeout_ms == UINT32_C(0)) {
		timeout_ms = MINIMM_CLIENT_DEFAULT_TIMEOUT_MS;
	}
	status = minimm_client_connect_socket(host, port, timeout_ms, &socket_fd);
	if (status != MINIMM_OK) {
		return status;
	}

	client = calloc(1U, sizeof(*client));
	if (client == NULL) {
		(void)close(socket_fd);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	if (pthread_mutex_init(&client->lock, NULL) != 0) {
		free(client);
		(void)close(socket_fd);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	client->socket_fd = socket_fd;
	client->next_request_id = UINT64_C(1);
	client->max_payload_size = MINIMM_PROTOCOL_HARD_MAX_PAYLOAD_SIZE;

	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_handshake_locked(client);
	(void)pthread_mutex_unlock(&client->lock);
	if (status != MINIMM_OK) {
		minimm_client_disconnect(client);
		return status;
	}

	*out_client = client;
	return MINIMM_OK;
}

void minimm_client_disconnect(minimm_client_t *client)
{
	if (client == NULL) {
		return;
	}
	(void)pthread_mutex_lock(&client->lock);
	minimm_client_break_locked(client);
	(void)pthread_mutex_unlock(&client->lock);
	(void)pthread_mutex_destroy(&client->lock);
	free(client);
}

uint32_t minimm_client_max_payload(const minimm_client_t *client)
{
	return client == NULL ? UINT32_C(0) : client->max_payload_size;
}

uint64_t minimm_client_max_note_size(const minimm_client_t *client)
{
	return client == NULL ? UINT64_C(0) : client->max_note_size;
}

minimm_status_t minimm_client_ping(minimm_client_t *client, uint64_t nonce, uint64_t *out_nonce)
{
	uint8_t request[MINIMM_PROTOCOL_PING_PAYLOAD_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_nonce == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_nonce = UINT64_C(0);
	if (client == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, nonce);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_PING, request,
					       MINIMM_PROTOCOL_PING_PAYLOAD_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != MINIMM_PROTOCOL_PING_PAYLOAD_SIZE) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			*out_nonce = minimm_protocol_get_u64(response);
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_create(minimm_client_t *client, uint64_t size,
					  minimm_remote_rights_t rights,
					  minimm_remote_note_t *out_note)
{
	uint8_t request[MINIMM_PROTOCOL_CREATE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_note, 0, sizeof(*out_note));
	if (client == NULL || !minimm_client_rights_are_valid(rights) ||
	    (size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    size > client->max_note_size) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, size);
	minimm_protocol_put_u32(request + 8U, rights);
	minimm_protocol_put_u32(request + 12U, UINT32_C(0));

	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_CREATE, request,
					       MINIMM_PROTOCOL_CREATE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		minimm_capability_t capability = { { 0 } };

		if (response_length == MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE) {
			(void)memcpy(capability.bytes, response + 8U,
				     MINIMM_PROTOCOL_CAPABILITY_SIZE);
		}
		if (response_length != MINIMM_PROTOCOL_CREATE_RESPONSE_SIZE ||
		    minimm_protocol_get_u64(response) == UINT64_C(0) ||
		    minimm_protocol_get_u64(response + 24U) != size ||
		    minimm_protocol_get_u32(response + 32U) != rights ||
		    minimm_protocol_get_u32(response + 36U) != UINT32_C(0) ||
		    (((rights & MINIMM_REMOTE_RIGHT_SHARE) != UINT32_C(0)) ==
		     minimm_client_capability_is_zero(&capability))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			out_note->handle = minimm_protocol_get_u64(response);
			out_note->capability = capability;
			out_note->size = size;
			out_note->rights = rights;
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_copy(minimm_client_t *client, uint64_t source_handle,
					minimm_remote_rights_t rights,
					minimm_remote_note_t *out_note)
{
	uint8_t request[MINIMM_PROTOCOL_COPY_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_note, 0, sizeof(*out_note));
	if (client == NULL || source_handle == UINT64_C(0) ||
	    !minimm_client_rights_are_valid(rights)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, source_handle);
	minimm_protocol_put_u32(request + 8U, rights);
	minimm_protocol_put_u32(request + 12U, UINT32_C(0));

	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_COPY, request,
					       MINIMM_PROTOCOL_COPY_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		minimm_capability_t capability = { { 0 } };
		uint64_t size = UINT64_C(0);

		if (response_length == MINIMM_PROTOCOL_COPY_RESPONSE_SIZE) {
			(void)memcpy(capability.bytes, response + 8U,
				     MINIMM_PROTOCOL_CAPABILITY_SIZE);
			size = minimm_protocol_get_u64(response + 24U);
		}
		if (response_length != MINIMM_PROTOCOL_COPY_RESPONSE_SIZE ||
		    minimm_protocol_get_u64(response) == UINT64_C(0) ||
		    minimm_protocol_get_u64(response) == source_handle ||
		    !minimm_client_note_size_is_valid(client, size) ||
		    minimm_protocol_get_u32(response + 32U) != rights ||
		    minimm_protocol_get_u32(response + 36U) != UINT32_C(0) ||
		    (((rights & MINIMM_REMOTE_RIGHT_SHARE) != UINT32_C(0)) ==
		     minimm_client_capability_is_zero(&capability))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			out_note->handle = minimm_protocol_get_u64(response);
			out_note->capability = capability;
			out_note->size = size;
			out_note->rights = rights;
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_open(minimm_client_t *client,
					const minimm_capability_t *capability,
					minimm_remote_rights_t rights,
					minimm_remote_note_t *out_note)
{
	minimm_capability_t capability_copy = { { 0 } };
	uint8_t request[MINIMM_PROTOCOL_OPEN_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_note == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (capability != NULL) {
		capability_copy = *capability;
	}
	(void)memset(out_note, 0, sizeof(*out_note));
	if (client == NULL || capability == NULL || !minimm_client_rights_are_valid(rights)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memcpy(request, capability_copy.bytes, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	minimm_protocol_put_u32(request + 16U, rights);
	minimm_protocol_put_u32(request + 20U, UINT32_C(0));

	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_OPEN, request,
					       MINIMM_PROTOCOL_OPEN_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != MINIMM_PROTOCOL_OPEN_RESPONSE_SIZE ||
		    minimm_protocol_get_u64(response) == UINT64_C(0) ||
		    !minimm_client_note_size_is_valid(client,
						      minimm_protocol_get_u64(response + 8U)) ||
		    minimm_protocol_get_u32(response + 16U) != rights ||
		    minimm_protocol_get_u32(response + 20U) != UINT32_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			out_note->handle = minimm_protocol_get_u64(response);
			out_note->size = minimm_protocol_get_u64(response + 8U);
			out_note->rights = rights;
			out_note->capability = capability_copy;
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_close(minimm_client_t *client, minimm_remote_note_t *note)
{
	uint8_t request[MINIMM_PROTOCOL_CLOSE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (client == NULL || note == NULL || note->handle == UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, note->handle);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_CLOSE, request,
					       MINIMM_PROTOCOL_CLOSE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != UINT32_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			(void)memset(note, 0, sizeof(*note));
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_stat(minimm_client_t *client, uint64_t handle,
					minimm_remote_note_info_t *out_info)
{
	uint8_t request[MINIMM_PROTOCOL_STAT_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_info == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_info, 0, sizeof(*out_info));
	if (client == NULL || handle == UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, handle);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_STAT, request,
					       MINIMM_PROTOCOL_STAT_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != MINIMM_PROTOCOL_STAT_RESPONSE_SIZE ||
		    !minimm_client_note_size_is_valid(client, minimm_protocol_get_u64(response)) ||
		    !minimm_client_rights_are_valid(minimm_protocol_get_u32(response + 8U)) ||
		    minimm_protocol_get_u32(response + 12U) != UINT32_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			out_info->size = minimm_protocol_get_u64(response);
			out_info->rights = minimm_protocol_get_u32(response + 8U);
			out_info->flags = minimm_protocol_get_u32(response + 12U);
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

static minimm_status_t minimm_client_note_read_locked(minimm_client_t *client, uint64_t handle,
						      uint64_t offset, uint8_t *destination,
						      size_t length, size_t *out_completed)
{
	const uint32_t max_data =
		client->max_payload_size - MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE;
	size_t total = 0U;
	minimm_status_t result = MINIMM_OK;
	/* Empty I/O still reaches the server to validate the handle and its rights. */
	bool request_pending = true;

	while (request_pending || total < length) {
		const size_t remaining = length - total;
		const size_t chunk = remaining < (size_t)max_data ? remaining : (size_t)max_data;
		uint8_t request[MINIMM_PROTOCOL_READ_REQUEST_SIZE] = { 0 };
		uint8_t *response = NULL;
		uint32_t response_length = UINT32_C(0);
		uint32_t completed = UINT32_C(0);
		minimm_status_t server_status = MINIMM_ERROR_IO;
		minimm_status_t status = MINIMM_OK;

		request_pending = false;
		minimm_protocol_put_u64(request, handle);
		minimm_protocol_put_u64(request + 8U, offset + (uint64_t)total);
		minimm_protocol_put_u32(request + 16U, (uint32_t)chunk);
		minimm_protocol_put_u32(request + 20U, UINT32_C(0));
		status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_READ, request,
						       MINIMM_PROTOCOL_READ_REQUEST_SIZE, &response,
						       &response_length, &server_status);
		if (status != MINIMM_OK) {
			result = status;
			free(response);
			break;
		}
		/* Error progress payloads are optional on the wire. */
		if (server_status != MINIMM_OK && response_length == UINT32_C(0)) {
			result = server_status;
			free(response);
			break;
		}
		if (response_length < MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			free(response);
			break;
		}
		completed = minimm_protocol_get_u32(response);
		if (minimm_protocol_get_u32(response + 4U) != UINT32_C(0) ||
		    completed > (uint32_t)chunk ||
		    response_length != MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE + completed) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			free(response);
			break;
		}
		if (completed != UINT32_C(0)) {
			(void)memcpy(destination + total,
				     response + MINIMM_PROTOCOL_READ_RESPONSE_FIXED_SIZE,
				     (size_t)completed);
			total += (size_t)completed;
		}
		free(response);
		if (server_status != MINIMM_OK) {
			result = server_status;
			break;
		}
		if (completed != (uint32_t)chunk) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			break;
		}
		if (length == 0U) {
			break;
		}
	}
	*out_completed = total;
	return result;
}

minimm_status_t minimm_client_note_read(minimm_client_t *client, uint64_t handle, uint64_t offset,
					void *destination, size_t length, size_t *out_completed)
{
	size_t ignored_completed = 0U;
	size_t *completed = out_completed != NULL ? out_completed : &ignored_completed;
	minimm_status_t status = MINIMM_OK;

	if (out_completed != NULL) {
		*out_completed = 0U;
	}
	if (client == NULL || handle == UINT64_C(0) || (destination == NULL && length != 0U) ||
	    !minimm_client_range_is_valid(offset, length)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_note_read_locked(client, handle, offset, destination, length,
						completed);
	(void)pthread_mutex_unlock(&client->lock);
	return status;
}

static minimm_status_t minimm_client_note_send_data_locked(minimm_client_t *client,
							   minimm_protocol_opcode_t opcode,
							   uint64_t handle, uint64_t offset,
							   const uint8_t *source, size_t length,
							   size_t *out_completed)
{
	const uint32_t max_data =
		client->max_payload_size - MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE;
	size_t total = 0U;
	minimm_status_t result = MINIMM_OK;
	/* Empty I/O still reaches the server to validate the handle and its rights. */
	bool request_pending = true;

	while (request_pending || total < length) {
		const size_t remaining = length - total;
		const size_t chunk = remaining < (size_t)max_data ? remaining : (size_t)max_data;
		const uint32_t payload_length =
			MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE + (uint32_t)chunk;
		uint8_t *request = malloc((size_t)payload_length);
		uint8_t *response = NULL;
		uint32_t response_length = UINT32_C(0);
		uint32_t completed = UINT32_C(0);
		minimm_status_t server_status = MINIMM_ERROR_IO;
		minimm_status_t status = MINIMM_OK;

		request_pending = false;
		if (request == NULL) {
			result = MINIMM_ERROR_OUT_OF_MEMORY;
			break;
		}
		minimm_protocol_put_u64(request, handle);
		minimm_protocol_put_u64(request + 8U, offset + (uint64_t)total);
		minimm_protocol_put_u32(request + 16U, (uint32_t)chunk);
		minimm_protocol_put_u32(request + 20U, UINT32_C(0));
		if (chunk != 0U) {
			(void)memcpy(request + MINIMM_PROTOCOL_WRITE_REQUEST_FIXED_SIZE,
				     source + total, chunk);
		}
		status = minimm_client_exchange_locked(client, opcode, request, payload_length,
						       &response, &response_length, &server_status);
		free(request);
		if (status != MINIMM_OK) {
			result = status;
			free(response);
			break;
		}
		/* Error progress payloads are optional on the wire. */
		if (server_status != MINIMM_OK && response_length == UINT32_C(0)) {
			result = server_status;
			free(response);
			break;
		}
		if (response_length != MINIMM_PROTOCOL_WRITE_RESPONSE_SIZE) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			free(response);
			break;
		}
		completed = minimm_protocol_get_u32(response);
		if (minimm_protocol_get_u32(response + 4U) != UINT32_C(0) ||
		    completed > (uint32_t)chunk) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			free(response);
			break;
		}
		total += (size_t)completed;
		free(response);
		if (server_status != MINIMM_OK) {
			result = server_status;
			break;
		}
		if (completed != (uint32_t)chunk) {
			minimm_client_break_locked(client);
			result = MINIMM_ERROR_IO;
			break;
		}
		if (length == 0U) {
			break;
		}
	}
	*out_completed = total;
	return result;
}

static minimm_status_t minimm_client_note_send_data(minimm_client_t *client,
						    minimm_protocol_opcode_t opcode,
						    uint64_t handle, uint64_t offset,
						    const void *source, size_t length,
						    size_t *out_completed)
{
	size_t ignored_completed = 0U;
	size_t *completed = out_completed != NULL ? out_completed : &ignored_completed;
	minimm_status_t status = MINIMM_OK;

	if (client == NULL || handle == UINT64_C(0) || (source == NULL && length != 0U) ||
	    !minimm_client_range_is_valid(offset, length)) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_note_send_data_locked(client, opcode, handle, offset, source, length,
						     completed);
	(void)pthread_mutex_unlock(&client->lock);
	return status;
}

minimm_status_t minimm_client_note_write(minimm_client_t *client, uint64_t handle, uint64_t offset,
					 const void *source, size_t length, size_t *out_completed)
{
	return minimm_client_note_send_data(client, MINIMM_PROTOCOL_OP_WRITE, handle, offset,
					    source, length, out_completed);
}

minimm_status_t minimm_client_note_edit(minimm_client_t *client, uint64_t handle, uint64_t offset,
					const void *source, size_t length, size_t *out_completed)
{
	return minimm_client_note_send_data(client, MINIMM_PROTOCOL_OP_EDIT, handle, offset, source,
					    length, out_completed);
}

minimm_status_t minimm_client_note_preview(minimm_client_t *client, uint64_t handle,
					   uint64_t offset, const void *source, size_t length,
					   size_t *out_completed)
{
	const uint64_t page_offset = offset & (MINIMM_PAGE_SIZE - UINT64_C(1));

	if (length == 0U || length > (size_t)MINIMM_PAGE_SIZE ||
	    (uint64_t)length > MINIMM_PAGE_SIZE - page_offset) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_client_note_send_data(client, MINIMM_PROTOCOL_OP_PREVIEW, handle, offset,
					    source, length, out_completed);
}

minimm_status_t minimm_client_note_stack_expand(minimm_client_t *client, uint64_t handle,
						uint64_t offset, const void *source, size_t length,
						size_t *out_completed)
{
	const uint64_t page_offset = offset & (MINIMM_PAGE_SIZE - UINT64_C(1));

	if (length == 0U || length > (size_t)MINIMM_PAGE_SIZE ||
	    (uint64_t)length > MINIMM_PAGE_SIZE - page_offset) {
		if (out_completed != NULL) {
			*out_completed = 0U;
		}
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	return minimm_client_note_send_data(client, MINIMM_PROTOCOL_OP_STACK_EXPAND, handle, offset,
					    source, length, out_completed);
}

minimm_status_t minimm_client_note_remap_page(minimm_client_t *client, uint64_t handle,
					      uint64_t note_offset, minimm_prot_t *out_protection)
{
	uint8_t request[MINIMM_PROTOCOL_REMAP_PAGE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_protection == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_protection = MINIMM_PROT_NONE;
	if (client == NULL || handle == UINT64_C(0) ||
	    (note_offset & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u64(request + 8U, note_offset);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_REMAP_PAGE, request,
					       MINIMM_PROTOCOL_REMAP_PAGE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const minimm_prot_t protection =
			response_length == MINIMM_PROTOCOL_REMAP_PAGE_RESPONSE_SIZE ?
				minimm_protocol_get_u32(response) :
				UINT32_MAX;

		if (response_length != MINIMM_PROTOCOL_REMAP_PAGE_RESPONSE_SIZE ||
		    minimm_protocol_get_u32(response + 4U) != UINT32_C(0) ||
		    !minimm_client_protection_is_valid(protection)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				*out_protection = protection;
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_mseal_merge(minimm_client_t *client, uint64_t handle,
					       minimm_remote_mseal_merge_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_MSEAL_MERGE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_MSEAL_MERGE, request,
					       MINIMM_PROTOCOL_MSEAL_MERGE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const uint32_t total_pages =
			response_length == MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE ?
				minimm_protocol_get_u32(response) :
				UINT32_C(0);
		const uint32_t sealed_pages =
			response_length == MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE ?
				minimm_protocol_get_u32(response + 4U) :
				UINT32_C(0);
		const uint32_t range_valid =
			response_length == MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE ?
				minimm_protocol_get_u32(response + 8U) :
				UINT32_MAX;

		if (response_length != MINIMM_PROTOCOL_MSEAL_MERGE_RESPONSE_SIZE ||
		    range_valid > UINT32_C(1) ||
		    minimm_protocol_get_u32(response + 12U) != UINT32_C(0) ||
		    sealed_pages > total_pages ||
		    (minimm_protocol_get_u64(response + 16U) & (MINIMM_PAGE_SIZE - UINT64_C(1))) !=
			    UINT64_C(0) ||
		    (minimm_protocol_get_u64(response + 24U) & (MINIMM_PAGE_SIZE - UINT64_C(1))) !=
			    UINT64_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->total_pages = total_pages;
				out_result->sealed_pages = sealed_pages;
				out_result->range_valid = range_valid != UINT32_C(0);
				out_result->update_start = minimm_protocol_get_u64(response + 16U);
				out_result->current_start = minimm_protocol_get_u64(response + 24U);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_mglru_reparent(minimm_client_t *client, uint64_t handle,
						  minimm_remote_mglru_reparent_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_MGLRU_REPARENT_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_MGLRU_REPARENT, request,
					       MINIMM_PROTOCOL_MGLRU_REPARENT_REQUEST_SIZE,
					       &response, &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const bool response_sized = response_length ==
					    MINIMM_PROTOCOL_MGLRU_REPARENT_RESPONSE_SIZE;
		const uint32_t total_pages = response_sized ? minimm_protocol_get_u32(response) :
							      UINT32_C(0);
		const uint32_t parent_old_pages =
			response_sized ? minimm_protocol_get_u32(response + 4U) : UINT32_C(0);
		const uint32_t parent_new_pages =
			response_sized ? minimm_protocol_get_u32(response + 8U) : UINT32_C(0);
		const uint32_t child_old_debt_pages =
			response_sized ? minimm_protocol_get_u32(response + 12U) : UINT32_C(0);
		const uint32_t child_new_credit_pages =
			response_sized ? minimm_protocol_get_u32(response + 16U) : UINT32_C(0);
		const uint32_t exit_clean =
			response_sized ? minimm_protocol_get_u32(response + 20U) : UINT32_MAX;
		const uint32_t accounting_valid =
			response_sized ? minimm_protocol_get_u32(response + 24U) : UINT32_MAX;

		if (!response_sized || exit_clean > UINT32_C(1) || accounting_valid > UINT32_C(1) ||
		    minimm_protocol_get_u32(response + 28U) != UINT32_C(0) ||
		    (server_status == MINIMM_OK &&
		     (total_pages == UINT32_C(0) || parent_old_pages > total_pages ||
		      parent_new_pages > total_pages || child_old_debt_pages > total_pages ||
		      child_new_credit_pages > total_pages ||
		      (uint64_t)parent_old_pages + (uint64_t)parent_new_pages !=
			      (uint64_t)total_pages ||
		      child_old_debt_pages != child_new_credit_pages ||
		      (accounting_valid != UINT32_C(0) && exit_clean == UINT32_C(0))))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->total_pages = total_pages;
				out_result->parent_old_pages = parent_old_pages;
				out_result->parent_new_pages = parent_new_pages;
				out_result->child_old_debt_pages = child_old_debt_pages;
				out_result->child_new_credit_pages = child_new_credit_pages;
				out_result->exit_clean = exit_clean != UINT32_C(0);
				out_result->accounting_valid = accounting_valid != UINT32_C(0);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_rmap_unmap(minimm_client_t *client, uint64_t handle,
					      uint32_t pte_capacity, uint32_t pte_index,
					      uint32_t folio_pages, uint32_t vma_remaining,
					      minimm_remote_rmap_unmap_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_RMAP_UNMAP_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0) || pte_capacity == UINT32_C(0) ||
	    pte_index >= pte_capacity || folio_pages == UINT32_C(0) ||
	    vma_remaining == UINT32_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u32(request + 8U, pte_capacity);
	minimm_protocol_put_u32(request + 12U, pte_index);
	minimm_protocol_put_u32(request + 16U, folio_pages);
	minimm_protocol_put_u32(request + 20U, vma_remaining);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_RMAP_UNMAP, request,
					       MINIMM_PROTOCOL_RMAP_UNMAP_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const bool response_sized = response_length ==
					    MINIMM_PROTOCOL_RMAP_UNMAP_RESPONSE_SIZE;
		const uint32_t requested = response_sized ? minimm_protocol_get_u32(response) :
							    UINT32_C(0);
		const uint32_t scanned = response_sized ? minimm_protocol_get_u32(response + 4U) :
							  UINT32_C(0);
		const uint32_t safe = response_sized ? minimm_protocol_get_u32(response + 8U) :
						       UINT32_C(0);
		const uint32_t first_invalid =
			response_sized ? minimm_protocol_get_u32(response + 12U) : UINT32_C(0);
		const uint32_t crossed = response_sized ? minimm_protocol_get_u32(response + 16U) :
							  UINT32_MAX;
		const uint32_t bounds_valid =
			response_sized ? minimm_protocol_get_u32(response + 20U) : UINT32_MAX;
		const uint32_t available = pte_capacity - pte_index;
		const uint32_t expected_safe = scanned < available ? scanned : available;

		if (!response_sized || crossed > UINT32_C(1) || bounds_valid > UINT32_C(1) ||
		    (server_status == MINIMM_OK &&
		     (requested != folio_pages || scanned > requested || scanned > vma_remaining ||
		      safe != expected_safe || crossed != (uint32_t)(scanned > safe) ||
		      bounds_valid != (uint32_t)(scanned == safe) ||
		      (crossed != UINT32_C(0) &&
		       (first_invalid != pte_index + safe || first_invalid != pte_capacity)) ||
		      (crossed == UINT32_C(0) && first_invalid != UINT32_MAX)))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->requested_pages = requested;
				out_result->scanned_pages = scanned;
				out_result->safe_pages = safe;
				out_result->first_invalid_index = first_invalid;
				out_result->crossed_pte_boundary = crossed != UINT32_C(0);
				out_result->bounds_valid = bounds_valid != UINT32_C(0);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_uffd_move(minimm_client_t *client, uint64_t handle,
					     uint32_t swap_entry, uint32_t source_folio,
					     uint32_t replacement_folio,
					     minimm_remote_uffd_move_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_UFFD_MOVE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0) || swap_entry == UINT32_C(0) ||
	    source_folio == UINT32_C(0) || replacement_folio == UINT32_C(0) ||
	    source_folio == replacement_folio) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u32(request + 8U, swap_entry);
	minimm_protocol_put_u32(request + 12U, source_folio);
	minimm_protocol_put_u32(request + 16U, replacement_folio);
	minimm_protocol_put_u32(request + 20U, UINT32_C(0));
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_UFFD_MOVE, request,
					       MINIMM_PROTOCOL_UFFD_MOVE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const bool response_sized = response_length ==
					    MINIMM_PROTOCOL_UFFD_MOVE_RESPONSE_SIZE;
		const uint32_t returned_entry = response_sized ? minimm_protocol_get_u32(response) :
								 UINT32_C(0);
		const uint32_t expected = response_sized ? minimm_protocol_get_u32(response + 4U) :
							   UINT32_C(0);
		const uint32_t moved = response_sized ? minimm_protocol_get_u32(response + 8U) :
							UINT32_C(0);
		const uint32_t pte_matches =
			response_sized ? minimm_protocol_get_u32(response + 12U) : UINT32_MAX;
		const uint32_t identity_valid =
			response_sized ? minimm_protocol_get_u32(response + 16U) : UINT32_MAX;
		const uint32_t accounting_valid =
			response_sized ? minimm_protocol_get_u32(response + 20U) : UINT32_MAX;

		if (!response_sized || pte_matches > UINT32_C(1) || identity_valid > UINT32_C(1) ||
		    accounting_valid > UINT32_C(1) ||
		    (server_status == MINIMM_OK &&
		     (returned_entry != swap_entry || expected != source_folio ||
		      pte_matches == UINT32_C(0) ||
		      (moved != source_folio && moved != replacement_folio) ||
		      identity_valid != (uint32_t)(moved == expected) ||
		      accounting_valid != identity_valid))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->swap_entry = returned_entry;
				out_result->expected_folio = expected;
				out_result->moved_folio = moved;
				out_result->pte_entry_matches = pte_matches != UINT32_C(0);
				out_result->folio_identity_valid = identity_valid != UINT32_C(0);
				out_result->accounting_valid = accounting_valid != UINT32_C(0);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t
minimm_client_note_hugetlb_reserve(minimm_client_t *client, uint64_t handle, uint32_t maximum_pages,
				   uint32_t minimum_pages, uint32_t used_before,
				   uint32_t requested_pages, uint32_t global_free_pages,
				   minimm_remote_hugetlb_reserve_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_HUGETLB_RESERVE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0) || maximum_pages == UINT32_C(0) ||
	    minimum_pages > maximum_pages || used_before > maximum_pages ||
	    requested_pages == UINT32_C(0) || requested_pages > maximum_pages - used_before) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u32(request + 8U, maximum_pages);
	minimm_protocol_put_u32(request + 12U, minimum_pages);
	minimm_protocol_put_u32(request + 16U, used_before);
	minimm_protocol_put_u32(request + 20U, requested_pages);
	minimm_protocol_put_u32(request + 24U, global_free_pages);
	minimm_protocol_put_u32(request + 28U, UINT32_C(0));
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_HUGETLB_RESERVE, request,
					       MINIMM_PROTOCOL_HUGETLB_RESERVE_REQUEST_SIZE,
					       &response, &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const bool response_sized = response_length ==
					    MINIMM_PROTOCOL_HUGETLB_RESERVE_RESPONSE_SIZE;
		const uint32_t requested = response_sized ? minimm_protocol_get_u32(response) :
							    UINT32_C(0);
		const uint32_t global_needed =
			response_sized ? minimm_protocol_get_u32(response + 4U) : UINT32_C(0);
		const uint32_t allocated = response_sized ? minimm_protocol_get_u32(response + 8U) :
							    UINT32_C(0);
		const uint32_t returned_used_before =
			response_sized ? minimm_protocol_get_u32(response + 12U) : UINT32_C(0);
		const uint32_t used_after =
			response_sized ? minimm_protocol_get_u32(response + 16U) : UINT32_C(0);
		const uint32_t rollback = response_sized ? minimm_protocol_get_u32(response + 20U) :
							   UINT32_C(0);
		const uint32_t succeeded =
			response_sized ? minimm_protocol_get_u32(response + 24U) : UINT32_MAX;
		const uint32_t accounting_valid =
			response_sized ? minimm_protocol_get_u32(response + 28U) : UINT32_MAX;
		const uint32_t temporary_used = used_before + requested_pages;
		const uint32_t global_before =
			used_before > minimum_pages ? used_before - minimum_pages : UINT32_C(0);
		const uint32_t global_after = temporary_used > minimum_pages ?
						      temporary_used - minimum_pages :
						      UINT32_C(0);
		const uint32_t expected_global_needed = global_after - global_before;

		if (!response_sized || succeeded > UINT32_C(1) || accounting_valid > UINT32_C(1) ||
		    (server_status == MINIMM_OK &&
		     (requested != requested_pages || returned_used_before != used_before ||
		      global_needed != expected_global_needed || allocated > requested ||
		      rollback > requested || used_after > maximum_pages ||
		      (succeeded != UINT32_C(0) &&
		       (global_needed > global_free_pages || allocated != requested ||
			rollback != UINT32_C(0) || used_after != used_before + requested ||
			accounting_valid == UINT32_C(0))) ||
		      (succeeded == UINT32_C(0) &&
		       (global_needed <= global_free_pages || allocated != UINT32_C(0) ||
			(rollback != requested - global_needed && rollback != requested) ||
			used_after != used_before + requested - rollback ||
			(accounting_valid != UINT32_C(0)) != (used_after == used_before)))))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->requested_pages = requested;
				out_result->global_needed_pages = global_needed;
				out_result->allocated_pages = allocated;
				out_result->used_before = returned_used_before;
				out_result->used_after = used_after;
				out_result->rollback_pages = rollback;
				out_result->reservation_succeeded = succeeded != UINT32_C(0);
				out_result->accounting_valid = accounting_valid != UINT32_C(0);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t
minimm_client_note_percpu_populate(minimm_client_t *client, uint64_t handle, uint32_t unit_count,
				   uint32_t unit_pages,
				   minimm_remote_percpu_populate_result_t *out_result)
{
	uint8_t request[MINIMM_PROTOCOL_PERCPU_POPULATE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_result == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memset(out_result, 0, sizeof(*out_result));
	if (client == NULL || handle == UINT64_C(0) || unit_count == UINT32_C(0) ||
	    unit_pages == UINT32_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u32(request + 8U, unit_count);
	minimm_protocol_put_u32(request + 12U, unit_pages);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_PERCPU_POPULATE, request,
					       MINIMM_PROTOCOL_PERCPU_POPULATE_REQUEST_SIZE,
					       &response, &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const bool response_sized = response_length ==
					    MINIMM_PROTOCOL_PERCPU_POPULATE_RESPONSE_SIZE;
		const uint32_t total = response_sized ? minimm_protocol_get_u32(response) :
							UINT32_C(0);
		const uint32_t capacity = response_sized ? minimm_protocol_get_u32(response + 4U) :
							   UINT32_C(0);
		const uint32_t mark_count =
			response_sized ? minimm_protocol_get_u32(response + 8U) : UINT32_C(0);
		const uint32_t first_invalid =
			response_sized ? minimm_protocol_get_u32(response + 12U) : UINT32_C(0);
		const uint32_t empty_after =
			response_sized ? minimm_protocol_get_u32(response + 16U) : UINT32_C(0);
		const uint32_t expected_empty =
			response_sized ? minimm_protocol_get_u32(response + 20U) : UINT32_C(0);
		const uint32_t bounds_valid =
			response_sized ? minimm_protocol_get_u32(response + 24U) : UINT32_MAX;
		const uint32_t accounting_valid =
			response_sized ? minimm_protocol_get_u32(response + 28U) : UINT32_MAX;
		const uint64_t expected_total = (uint64_t)unit_count * unit_pages;

		if (!response_sized || bounds_valid > UINT32_C(1) ||
		    accounting_valid > UINT32_C(1) ||
		    (server_status == MINIMM_OK &&
		     ((uint64_t)total != expected_total || capacity != unit_pages ||
		      (mark_count != capacity && mark_count != total) ||
		      empty_after != mark_count || expected_empty != capacity ||
		      bounds_valid != (uint32_t)(mark_count <= capacity) ||
		      (bounds_valid != UINT32_C(0) && first_invalid != UINT32_MAX) ||
		      (bounds_valid == UINT32_C(0) && first_invalid != capacity) ||
		      accounting_valid != (uint32_t)(empty_after == expected_empty)))) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			status = server_status;
			if (status == MINIMM_OK) {
				out_result->total_backing_pages = total;
				out_result->bitmap_capacity = capacity;
				out_result->mark_count = mark_count;
				out_result->first_invalid_index = first_invalid;
				out_result->empty_pages_after = empty_after;
				out_result->expected_empty_pages = expected_empty;
				out_result->bounds_valid = bounds_valid != UINT32_C(0);
				out_result->accounting_valid = accounting_valid != UINT32_C(0);
			}
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_resize(minimm_client_t *client, uint64_t handle,
					  uint64_t new_size, uint64_t *out_actual_size)
{
	uint8_t request[MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (out_actual_size == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_actual_size = UINT64_C(0);
	if (client == NULL || handle == UINT64_C(0) ||
	    (new_size & (MINIMM_PAGE_SIZE - UINT64_C(1))) != UINT64_C(0) ||
	    new_size > client->max_note_size) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, handle);
	minimm_protocol_put_u64(request + 8U, new_size);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_RESIZE, request,
					       MINIMM_PROTOCOL_RESIZE_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status != MINIMM_OK && response_length == UINT32_C(0)) {
		status = server_status;
	} else if (status == MINIMM_OK) {
		const uint64_t actual_size = response_length ==
							     MINIMM_PROTOCOL_RESIZE_RESPONSE_SIZE ?
						     minimm_protocol_get_u64(response) :
						     UINT64_MAX;

		if (response_length != MINIMM_PROTOCOL_RESIZE_RESPONSE_SIZE ||
		    !minimm_client_note_size_is_valid(client, actual_size) ||
		    (server_status == MINIMM_OK && actual_size != new_size)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		} else {
			*out_actual_size = actual_size;
			status = server_status;
		}
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

static minimm_status_t minimm_client_handle_empty_response(minimm_client_t *client,
							   minimm_protocol_opcode_t opcode,
							   uint64_t handle)
{
	uint8_t request[8] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (client == NULL || handle == UINT64_C(0)) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	minimm_protocol_put_u64(request, handle);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, opcode, request, (uint32_t)sizeof(request),
					       &response, &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != UINT32_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_client_note_flush(minimm_client_t *client, uint64_t handle)
{
	return minimm_client_handle_empty_response(client, MINIMM_PROTOCOL_OP_FLUSH, handle);
}

minimm_status_t minimm_client_note_unlink(minimm_client_t *client,
					  const minimm_capability_t *capability)
{
	uint8_t request[MINIMM_PROTOCOL_UNLINK_REQUEST_SIZE] = { 0 };
	uint8_t *response = NULL;
	uint32_t response_length = UINT32_C(0);
	minimm_status_t server_status = MINIMM_ERROR_IO;
	minimm_status_t status = MINIMM_OK;

	if (client == NULL || capability == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memcpy(request, capability->bytes, MINIMM_PROTOCOL_CAPABILITY_SIZE);
	(void)pthread_mutex_lock(&client->lock);
	status = minimm_client_exchange_locked(client, MINIMM_PROTOCOL_OP_UNLINK, request,
					       MINIMM_PROTOCOL_UNLINK_REQUEST_SIZE, &response,
					       &response_length, &server_status);
	if (status == MINIMM_OK && server_status == MINIMM_OK) {
		if (response_length != UINT32_C(0)) {
			minimm_client_break_locked(client);
			status = MINIMM_ERROR_IO;
		}
	} else if (status == MINIMM_OK) {
		status = server_status;
	}
	(void)pthread_mutex_unlock(&client->lock);
	free(response);
	return status;
}

minimm_status_t minimm_capability_format(const minimm_capability_t *capability,
					 char output[MINIMM_CAPABILITY_HEX_BUFFER_SIZE])
{
	static const char digits[] = "0123456789abcdef";
	minimm_capability_t input = { { 0 } };
	size_t index = 0U;

	if (capability == NULL || output == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	input = *capability;
	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		output[index * 2U] = digits[input.bytes[index] >> 4U];
		output[index * 2U + 1U] = digits[input.bytes[index] & UINT8_C(15)];
	}
	output[MINIMM_CAPABILITY_HEX_LENGTH] = '\0';
	return MINIMM_OK;
}

static int minimm_capability_hex_value(char character)
{
	if (character >= '0' && character <= '9') {
		return character - '0';
	}
	if (character >= 'a' && character <= 'f') {
		return character - 'a' + 10;
	}
	if (character >= 'A' && character <= 'F') {
		return character - 'A' + 10;
	}
	return -1;
}

minimm_status_t minimm_capability_parse(const char *text, minimm_capability_t *out_capability)
{
	char input[MINIMM_CAPABILITY_HEX_BUFFER_SIZE] = { 0 };
	size_t index = 0U;

	if (out_capability == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	if (text == NULL || strnlen(text, sizeof(input)) != MINIMM_CAPABILITY_HEX_LENGTH) {
		(void)memset(out_capability, 0, sizeof(*out_capability));
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	(void)memcpy(input, text, sizeof(input));
	(void)memset(out_capability, 0, sizeof(*out_capability));
	for (index = 0U; index < MINIMM_PROTOCOL_CAPABILITY_SIZE; ++index) {
		const int high = minimm_capability_hex_value(input[index * 2U]);
		const int low = minimm_capability_hex_value(input[index * 2U + 1U]);

		if (high < 0 || low < 0) {
			(void)memset(out_capability, 0, sizeof(*out_capability));
			return MINIMM_ERROR_INVALID_ARGUMENT;
		}
		out_capability->bytes[index] = (uint8_t)((high << 4) | low);
	}
	return MINIMM_OK;
}
