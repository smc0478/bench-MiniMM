#include "rcu.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

typedef struct minimm_rcu_retired {
	struct minimm_rcu_retired *next;
	void *object;
	minimm_rcu_reclaim_fn reclaim;
} minimm_rcu_retired_t;

struct minimm_rcu_domain {
	atomic_size_t readers;
	pthread_mutex_t lock;
	pthread_cond_t quiescent;
	minimm_rcu_retired_t *retired_head;
	minimm_rcu_retired_t *retired_tail;
};

static minimm_rcu_retired_t *minimm_rcu_take_retired_locked(minimm_rcu_domain_t *domain)
{
	minimm_rcu_retired_t *retired = domain->retired_head;

	domain->retired_head = NULL;
	domain->retired_tail = NULL;
	return retired;
}

static size_t minimm_rcu_reclaim_list(minimm_rcu_retired_t *retired)
{
	size_t reclaimed = 0U;

	while (retired != NULL) {
		minimm_rcu_retired_t *next = retired->next;

		retired->reclaim(retired->object);
		free(retired);
		retired = next;
		reclaimed += 1U;
	}
	return reclaimed;
}

minimm_status_t minimm_rcu_domain_create(minimm_rcu_domain_t **out_domain)
{
	minimm_rcu_domain_t *domain = NULL;
	int result = 0;

	if (out_domain == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}
	*out_domain = NULL;

	domain = calloc(1U, sizeof(*domain));
	if (domain == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	atomic_init(&domain->readers, 0U);

	result = pthread_mutex_init(&domain->lock, NULL);
	if (result != 0) {
		free(domain);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	result = pthread_cond_init(&domain->quiescent, NULL);
	if (result != 0) {
		(void)pthread_mutex_destroy(&domain->lock);
		free(domain);
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}

	*out_domain = domain;
	return MINIMM_OK;
}

void minimm_rcu_domain_destroy(minimm_rcu_domain_t *domain)
{
	if (domain == NULL) {
		return;
	}

	while (minimm_rcu_synchronize(domain) != 0U) {
		/* A reclaim callback may have retired another object. */
	}
	(void)pthread_cond_destroy(&domain->quiescent);
	(void)pthread_mutex_destroy(&domain->lock);
	free(domain);
}

void minimm_rcu_read_lock(minimm_rcu_domain_t *domain)
{
	size_t readers = 0U;

	if (domain == NULL) {
		return;
	}
	readers = atomic_load_explicit(&domain->readers, memory_order_seq_cst);
	while (readers != SIZE_MAX) {
		if (atomic_compare_exchange_weak_explicit(&domain->readers, &readers, readers + 1U,
							  memory_order_seq_cst,
							  memory_order_seq_cst)) {
			return;
		}
	}
	abort();
}

void minimm_rcu_read_unlock(minimm_rcu_domain_t *domain)
{
	size_t previous = 0U;

	if (domain == NULL) {
		return;
	}

	previous = atomic_load_explicit(&domain->readers, memory_order_seq_cst);
	while (previous != 0U &&
	       !atomic_compare_exchange_weak_explicit(&domain->readers, &previous, previous - 1U,
						      memory_order_seq_cst, memory_order_seq_cst)) {
	}
	if (previous == 0U) {
		abort();
	}
	if (previous == 1U) {
		(void)pthread_mutex_lock(&domain->lock);
		(void)pthread_cond_broadcast(&domain->quiescent);
		(void)pthread_mutex_unlock(&domain->lock);
	}
}

minimm_status_t minimm_rcu_retire(minimm_rcu_domain_t *domain, void *object,
				  minimm_rcu_reclaim_fn reclaim)
{
	minimm_rcu_retired_t *retired = NULL;

	if (domain == NULL || object == NULL || reclaim == NULL) {
		return MINIMM_ERROR_INVALID_ARGUMENT;
	}

	retired = malloc(sizeof(*retired));
	if (retired == NULL) {
		return MINIMM_ERROR_OUT_OF_MEMORY;
	}
	retired->next = NULL;
	retired->object = object;
	retired->reclaim = reclaim;

	(void)pthread_mutex_lock(&domain->lock);
	if (domain->retired_tail == NULL) {
		domain->retired_head = retired;
	} else {
		domain->retired_tail->next = retired;
	}
	domain->retired_tail = retired;
	(void)pthread_mutex_unlock(&domain->lock);
	return MINIMM_OK;
}

size_t minimm_rcu_poll(minimm_rcu_domain_t *domain)
{
	minimm_rcu_retired_t *retired = NULL;

	if (domain == NULL) {
		return 0U;
	}

	(void)pthread_mutex_lock(&domain->lock);
	if (atomic_load_explicit(&domain->readers, memory_order_seq_cst) == 0U) {
		retired = minimm_rcu_take_retired_locked(domain);
	}
	(void)pthread_mutex_unlock(&domain->lock);

	return minimm_rcu_reclaim_list(retired);
}

size_t minimm_rcu_synchronize(minimm_rcu_domain_t *domain)
{
	minimm_rcu_retired_t *retired = NULL;

	if (domain == NULL) {
		return 0U;
	}

	(void)pthread_mutex_lock(&domain->lock);
	while (atomic_load_explicit(&domain->readers, memory_order_seq_cst) != 0U) {
		(void)pthread_cond_wait(&domain->quiescent, &domain->lock);
	}
	retired = minimm_rcu_take_retired_locked(domain);
	(void)pthread_mutex_unlock(&domain->lock);

	return minimm_rcu_reclaim_list(retired);
}
