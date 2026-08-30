#ifndef MINIMM_RCU_H
#define MINIMM_RCU_H

#include "minimm/minimm.h"

#include <stddef.h>

typedef struct minimm_rcu_domain minimm_rcu_domain_t;

typedef void (*minimm_rcu_reclaim_fn)(void *object);

/*
 * This is a deliberately conservative user-space RCU-style domain. Readers
 * must enter before loading a protected pointer and leave only after their
 * final access to the pointed-to object. An updater must remove an object from
 * the published structure before passing it to minimm_rcu_retire().
 */
minimm_status_t minimm_rcu_domain_create(minimm_rcu_domain_t **out_domain);

/* No other thread may use the domain while it is being destroyed. */
void minimm_rcu_domain_destroy(minimm_rcu_domain_t *domain);

void minimm_rcu_read_lock(minimm_rcu_domain_t *domain);
void minimm_rcu_read_unlock(minimm_rcu_domain_t *domain);

minimm_status_t minimm_rcu_retire(minimm_rcu_domain_t *domain, void *object,
				  minimm_rcu_reclaim_fn reclaim);

/*
 * Reclaim queued objects only if the domain is currently quiescent. The
 * return value is the number of callbacks invoked.
 */
size_t minimm_rcu_poll(minimm_rcu_domain_t *domain);

/*
 * Wait for a quiescent state and reclaim everything queued before that state.
 * Because the domain has one global counter, newly arriving readers can extend
 * the wait until the counter is observed at zero.
 */
size_t minimm_rcu_synchronize(minimm_rcu_domain_t *domain);

#endif
