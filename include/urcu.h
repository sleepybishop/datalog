/*
 * urcu.h - Userspace Read-Copy-Update (RCU) / Epoch-Based Reclamation (EBR)
 *
 * A lightweight, zero-dependency, header-only implementation of Userspace RCU
 * using Epoch-Based Reclamation (EBR).
 *
 * To use this library:
 *   1. Include this header in any file that needs to call the API.
 *   2. In exactly ONE C/C++ source file, define URCU_IMPLEMENTATION before
 *      including this file to compile the implementation logic.
 *
 * Example:
 *   #define URCU_IMPLEMENTATION
 *   #include "urcu.h"
 */

#ifndef URCU_H
#define URCU_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define URCU_MAX_THREADS 64

#if __STDC_VERSION__ >= 201112L
#define URCU_THREAD_LOCAL _Thread_local
#else
#define URCU_THREAD_LOCAL __thread
#endif

typedef struct {
    uint64_t epoch;
    bool active;
    /* Prevent false sharing by padding to CPU cache-line size */
    char pad[64 - sizeof(uint64_t) - sizeof(bool)];
} urcu_reader_t;

typedef struct {
    void *ptr;
    void (*free_fn)(void *);
    uint64_t epoch;
} urcu_retired_t;

typedef struct {
    uint64_t global_epoch;
    urcu_reader_t readers[URCU_MAX_THREADS];
    pthread_mutex_t registry_mutex;

    /* Retired objects waiting for reclamation (Single-Writer Model) */
    urcu_retired_t *retired_queue;
    int retired_count;
    int retired_capacity;
} urcu_t;

/* Thread-local registration index */
extern URCU_THREAD_LOCAL int urcu_thread_idx;

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the RCU context */
void urcu_init(urcu_t *rcu, int initial_capacity);

/* Destroy the RCU context, freeing all remaining retired pointers */
void urcu_destroy(urcu_t *rcu);

/* Register the calling thread as an active RCU reader */
int urcu_register_thread(urcu_t *rcu);

/* Unregister the calling thread from RCU registry */
void urcu_unregister_thread(urcu_t *rcu);

/* Enter an RCU read-side critical section (lock-free) */
static inline void urcu_read_lock(urcu_t *rcu)
{
    int idx = urcu_thread_idx;
    if (idx < 0) {
        idx = urcu_register_thread(rcu);
    }
    if (idx >= 0) {
        uint64_t g_epoch = __atomic_load_n(&rcu->global_epoch, __ATOMIC_ACQUIRE);
        __atomic_store_n(&rcu->readers[idx].epoch, g_epoch, __ATOMIC_RELEASE);
        __atomic_store_n(&rcu->readers[idx].active, true, __ATOMIC_RELEASE);
    }
}

/* Exit an RCU read-side critical section (lock-free) */
static inline void urcu_read_unlock(urcu_t *rcu)
{
    int idx = urcu_thread_idx;
    if (idx >= 0) {
        __atomic_store_n(&rcu->readers[idx].active, false, __ATOMIC_RELEASE);
    }
}

/* Defer reclamation of a memory pointer until all current readers exit */
void urcu_retire(urcu_t *rcu, void *ptr, void (*free_fn)(void *));

/* Reclaim retired objects that are no longer accessed by any active reader */
void urcu_gc(urcu_t *rcu);

/* Wait synchronously until all current reader threads have finished */
void urcu_synchronize(urcu_t *rcu);

#ifdef __cplusplus
}
#endif

#endif /* URCU_H */

#ifdef URCU_IMPLEMENTATION

URCU_THREAD_LOCAL int urcu_thread_idx = -1;

void urcu_init(urcu_t *rcu, int initial_capacity)
{
    rcu->global_epoch = 1;
    for (int i = 0; i < URCU_MAX_THREADS; i++) {
        rcu->readers[i].epoch = 0;
        rcu->readers[i].active = false;
    }
    pthread_mutex_init(&rcu->registry_mutex, NULL);
    rcu->retired_count = 0;
    rcu->retired_capacity = initial_capacity;
    if (initial_capacity > 0) {
        rcu->retired_queue = (urcu_retired_t *)malloc(initial_capacity * sizeof(urcu_retired_t));
    } else {
        rcu->retired_queue = NULL;
    }
}

void urcu_destroy(urcu_t *rcu)
{
    if (rcu->retired_queue) {
        for (int i = 0; i < rcu->retired_count; i++) {
            if (rcu->retired_queue[i].free_fn) {
                rcu->retired_queue[i].free_fn(rcu->retired_queue[i].ptr);
            } else {
                free(rcu->retired_queue[i].ptr);
            }
        }
        free(rcu->retired_queue);
    }
    pthread_mutex_destroy(&rcu->registry_mutex);
}

int urcu_register_thread(urcu_t *rcu)
{
    pthread_mutex_lock(&rcu->registry_mutex);
    int idx = -1;
    for (int i = 0; i < URCU_MAX_THREADS; i++) {
        bool active = __atomic_load_n(&rcu->readers[i].active, __ATOMIC_ACQUIRE);
        if (!active && rcu->readers[i].epoch == 0) {
            idx = i;
            break;
        }
    }
    if (idx != -1) {
        rcu->readers[idx].epoch = 1; /* Allocate slot */
        __atomic_store_n(&rcu->readers[idx].active, false, __ATOMIC_RELEASE);
        urcu_thread_idx = idx;
    }
    pthread_mutex_unlock(&rcu->registry_mutex);
    return idx;
}

void urcu_unregister_thread(urcu_t *rcu)
{
    int idx = urcu_thread_idx;
    if (idx >= 0) {
        pthread_mutex_lock(&rcu->registry_mutex);
        __atomic_store_n(&rcu->readers[idx].active, false, __ATOMIC_RELEASE);
        rcu->readers[idx].epoch = 0; /* Free slot */
        urcu_thread_idx = -1;
        pthread_mutex_unlock(&rcu->registry_mutex);
    }
}

void urcu_retire(urcu_t *rcu, void *ptr, void (*free_fn)(void *))
{
    if (!ptr) {
        return;
    }
    uint64_t current = __atomic_load_n(&rcu->global_epoch, __ATOMIC_ACQUIRE);

    if (rcu->retired_count >= rcu->retired_capacity) {
        int new_cap = rcu->retired_capacity == 0 ? 64 : rcu->retired_capacity * 2;
        urcu_retired_t *new_queue = (urcu_retired_t *)realloc(rcu->retired_queue, new_cap * sizeof(urcu_retired_t));
        if (!new_queue) {
            if (free_fn) {
                free_fn(ptr);
            } else {
                free(ptr);
            }
            return;
        }
        rcu->retired_queue = new_queue;
        rcu->retired_capacity = new_cap;
    }

    rcu->retired_queue[rcu->retired_count++] = (urcu_retired_t){.ptr = ptr, .free_fn = free_fn, .epoch = current};

    __atomic_add_fetch(&rcu->global_epoch, 1, __ATOMIC_ACQ_REL);
}

void urcu_gc(urcu_t *rcu)
{
    uint64_t global_epoch = __atomic_load_n(&rcu->global_epoch, __ATOMIC_ACQUIRE);
    uint64_t oldest_active = global_epoch;

    for (int i = 0; i < URCU_MAX_THREADS; i++) {
        if (rcu->readers[i].epoch != 0) {
            bool active = __atomic_load_n(&rcu->readers[i].active, __ATOMIC_ACQUIRE);
            if (active) {
                uint64_t r_epoch = __atomic_load_n(&rcu->readers[i].epoch, __ATOMIC_ACQUIRE);
                if (r_epoch < oldest_active) {
                    oldest_active = r_epoch;
                }
            }
        }
    }

    int write_idx = 0;
    for (int i = 0; i < rcu->retired_count; i++) {
        if (rcu->retired_queue[i].epoch < oldest_active) {
            if (rcu->retired_queue[i].free_fn) {
                rcu->retired_queue[i].free_fn(rcu->retired_queue[i].ptr);
            } else {
                free(rcu->retired_queue[i].ptr);
            }
        } else {
            rcu->retired_queue[write_idx++] = rcu->retired_queue[i];
        }
    }
    rcu->retired_count = write_idx;
}

void urcu_synchronize(urcu_t *rcu)
{
    uint64_t target_epoch = __atomic_add_fetch(&rcu->global_epoch, 1, __ATOMIC_ACQ_REL);
    while (1) {
        bool pending_readers = false;
        for (int i = 0; i < URCU_MAX_THREADS; i++) {
            if (rcu->readers[i].epoch != 0) {
                bool active = __atomic_load_n(&rcu->readers[i].active, __ATOMIC_ACQUIRE);
                if (active) {
                    uint64_t r_epoch = __atomic_load_n(&rcu->readers[i].epoch, __ATOMIC_ACQUIRE);
                    if (r_epoch < target_epoch) {
                        pending_readers = true;
                        break;
                    }
                }
            }
        }
        if (!pending_readers) {
            break;
        }
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }
}

#endif /* URCU_IMPLEMENTATION */
