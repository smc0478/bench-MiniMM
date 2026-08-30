#ifndef MINIMM_SERVER_H
#define MINIMM_SERVER_H

#include "minimm/minimm.h"
#include "minimm/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct minimm_server minimm_server_t;

typedef struct minimm_server_config {
	const char *bind_address;
	uint16_t port;
	size_t max_clients;
	size_t max_notes;
	uint64_t max_note_size;
	uint64_t max_total_note_size;
	uint32_t max_payload_size;
	uint32_t io_timeout_ms;
	bool enable_private_preview;
	bool enable_stack_expand;
	bool enable_page_remap;
	bool enable_mseal_merge;
	bool enable_mglru_reparent;
	bool enable_rmap_unmap;
	bool enable_uffd_move;
	bool enable_hugetlb_reserve;
	bool enable_percpu_populate;
	minimm_config_t memory;
} minimm_server_config_t;

/* Return a loopback-only, resource-bounded configuration suitable for v1. */
minimm_server_config_t minimm_server_config_default(void);

minimm_status_t minimm_server_create(const minimm_server_config_t *config,
				     minimm_server_t **out_server);

/* Start accepting clients. A port of zero selects an ephemeral TCP port. */
minimm_status_t minimm_server_start(minimm_server_t *server);

/* Zero means the server has not successfully bound a listener yet. */
uint16_t minimm_server_bound_port(const minimm_server_t *server);

/* Stop is idempotent. Existing client handles are drained before it returns. */
minimm_status_t minimm_server_stop(minimm_server_t *server);

/* No public server API may race with destruction of the server handle. */
void minimm_server_destroy(minimm_server_t *server);

#ifdef __cplusplus
}
#endif

#endif
