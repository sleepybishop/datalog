#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <pthread.h>
#include <stddef.h>
#include "fact.h"
#include "urcu.h"

typedef struct facts s_facts;

typedef enum { ROLLBACK_ADD, ROLLBACK_REMOVE, ROLLBACK_STATE } e_rollback_action;

typedef struct rollback_entry {
    e_rollback_action action;
    s_fact fact;
} s_rollback_entry;

typedef struct rollback_stack {
    s_rollback_entry *entries;
    size_t capacity;
    size_t size;
} s_rollback_stack;

typedef void (*f_facts_tx_listener)(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data);
typedef void (*f_facts_commit_observer)(s_facts *facts, void *user_data);

typedef struct transaction {
    int level;
    s_rollback_stack rollback;
    pthread_rwlock_t rwlock;
    pthread_mutex_t state_mutex;
    pthread_t owner;
    int owner_valid;
    f_facts_tx_listener listener;
    void *listener_data;
    f_facts_commit_observer commit_observer;
    void *commit_observer_data;
    urcu_t rcu;
} s_transaction;

void transaction_init(s_transaction *tx);
void transaction_destroy(s_transaction *tx);

int transaction_begin(s_facts *facts, s_transaction *tx);
int transaction_commit(s_facts *facts, s_transaction *tx);
int transaction_rollback(s_facts *facts, s_transaction *tx);
void transaction_rollback_push(s_facts *facts, s_transaction *tx, e_rollback_action action, const s_fact *fact);

int transaction_acquire_writer(s_transaction *tx);
void transaction_release_writer(s_transaction *tx, int has_lock);

int transaction_acquire_reader(s_transaction *tx);
void transaction_release_reader(s_transaction *tx, int acquired);

#endif
