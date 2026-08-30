#include "rcu.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct test_object test_object_t;

typedef struct test_control {
	minimm_rcu_domain_t *domain;
	test_object_t *object;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int reader_value;
	bool reader_entered;
	bool release_reader;
	atomic_bool reader_exited;
	atomic_bool reclaimed;
} test_control_t;

struct test_object {
	test_control_t *control;
	int value;
};

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static void *reader_main(void *argument)
{
	test_control_t *control = argument;

	minimm_rcu_read_lock(control->domain);
	(void)pthread_mutex_lock(&control->lock);
	control->reader_entered = true;
	(void)pthread_cond_broadcast(&control->changed);
	while (!control->release_reader) {
		(void)pthread_cond_wait(&control->changed, &control->lock);
	}
	(void)pthread_mutex_unlock(&control->lock);

	control->reader_value = control->object->value;
	atomic_store_explicit(&control->reader_exited, true, memory_order_release);
	minimm_rcu_read_unlock(control->domain);
	return NULL;
}

static void reclaim_object(void *object)
{
	test_object_t *retired = object;

	atomic_store_explicit(&retired->control->reclaimed, true, memory_order_release);
	free(retired);
}

int main(void)
{
	minimm_rcu_domain_t *domain = NULL;
	test_control_t control = { 0 };
	test_object_t *object = NULL;
	pthread_t reader = { 0 };
	bool thread_created = false;
	bool mutex_initialized = false;
	bool cond_initialized = false;
	int result = EXIT_FAILURE;

	if (!check(minimm_rcu_domain_create(&domain) == MINIMM_OK, "create RCU domain")) {
		return EXIT_FAILURE;
	}
	control.domain = domain;
	atomic_init(&control.reader_exited, false);
	atomic_init(&control.reclaimed, false);

	if (!check(pthread_mutex_init(&control.lock, NULL) == 0, "initialize test mutex")) {
		goto cleanup;
	}
	mutex_initialized = true;
	if (!check(pthread_cond_init(&control.changed, NULL) == 0, "initialize test condition")) {
		goto cleanup;
	}
	cond_initialized = true;

	object = malloc(sizeof(*object));
	if (!check(object != NULL, "allocate retired object")) {
		goto cleanup;
	}
	object->control = &control;
	object->value = 42;
	control.object = object;

	if (!check(pthread_create(&reader, NULL, reader_main, &control) == 0, "start reader")) {
		goto cleanup;
	}
	thread_created = true;

	(void)pthread_mutex_lock(&control.lock);
	while (!control.reader_entered) {
		(void)pthread_cond_wait(&control.changed, &control.lock);
	}
	(void)pthread_mutex_unlock(&control.lock);

	if (!check(object->value == 42, "retired object starts intact") ||
	    !check(minimm_rcu_retire(domain, object, reclaim_object) == MINIMM_OK,
		   "queue object for deferred reclaim")) {
		goto cleanup;
	}
	object = NULL;

	if (!check(minimm_rcu_poll(domain) == 0U,
		   "poll does not reclaim while a reader is active") ||
	    !check(!atomic_load_explicit(&control.reclaimed, memory_order_acquire),
		   "callback has not run before reader exit")) {
		goto cleanup;
	}

	(void)pthread_mutex_lock(&control.lock);
	control.release_reader = true;
	(void)pthread_cond_broadcast(&control.changed);
	(void)pthread_mutex_unlock(&control.lock);
	(void)pthread_join(reader, NULL);
	thread_created = false;

	if (!check(atomic_load_explicit(&control.reader_exited, memory_order_acquire),
		   "reader completed its critical section") ||
	    !check(control.reader_value == 42,
		   "reader can still access the retired object before unlock") ||
	    !check(minimm_rcu_poll(domain) == 1U, "quiescent poll invokes the queued callback") ||
	    !check(atomic_load_explicit(&control.reclaimed, memory_order_acquire),
		   "object is reclaimed after reader exit") ||
	    !check(minimm_rcu_synchronize(domain) == 0U,
		   "empty synchronize has nothing to reclaim")) {
		goto cleanup;
	}

	result = EXIT_SUCCESS;

cleanup:
	if (thread_created) {
		(void)pthread_mutex_lock(&control.lock);
		control.release_reader = true;
		(void)pthread_cond_broadcast(&control.changed);
		(void)pthread_mutex_unlock(&control.lock);
		(void)pthread_join(reader, NULL);
	}
	if (object != NULL) {
		free(object);
	}
	minimm_rcu_domain_destroy(domain);
	if (cond_initialized) {
		(void)pthread_cond_destroy(&control.changed);
	}
	if (mutex_initialized) {
		(void)pthread_mutex_destroy(&control.lock);
	}
	return result;
}
