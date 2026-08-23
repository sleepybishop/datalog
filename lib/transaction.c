#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "transaction.h"
#include "facts.h"

void transaction_init(s_transaction *tx)
{
    assert(tx);
    tx->level = 0;
    tx->rollback.entries = NULL;
    tx->rollback.capacity = 0;
    tx->rollback.size = 0;
    pthread_rwlock_init(&tx->rwlock, NULL);
    pthread_mutex_init(&tx->state_mutex, NULL);
    memset(&tx->owner, 0, sizeof(pthread_t));
    tx->owner_valid = 0;
    tx->listener = NULL;
    tx->listener_data = NULL;
    tx->commit_observer = NULL;
    tx->commit_observer_data = NULL;
    urcu_init(&tx->rcu, 256);
}

void transaction_destroy(s_transaction *tx)
{
    assert(tx);
    pthread_rwlock_destroy(&tx->rwlock);
    pthread_mutex_destroy(&tx->state_mutex);
    free(tx->rollback.entries);
    tx->rollback.entries = NULL;
    tx->rollback.capacity = 0;
    tx->rollback.size = 0;
    urcu_destroy(&tx->rcu);
}

int transaction_begin(s_facts *facts, s_transaction *tx)
{
    (void)facts;
    assert(tx);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    if (tx->owner_valid && pthread_equal(tx->owner, self)) {
        tx->level++;
        pthread_mutex_unlock(&tx->state_mutex);
        return 0;
    }
    pthread_mutex_unlock(&tx->state_mutex);
    pthread_rwlock_wrlock(&tx->rwlock);
    pthread_mutex_lock(&tx->state_mutex);
    tx->owner = self;
    tx->owner_valid = 1;
    tx->level = 1;
    pthread_mutex_unlock(&tx->state_mutex);
    return 0;
}

int transaction_commit(s_facts *facts, s_transaction *tx)
{
    assert(tx);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    if (tx->level <= 0 || !tx->owner_valid || !pthread_equal(tx->owner, self)) {
        pthread_mutex_unlock(&tx->state_mutex);
        return -1;
    }
    tx->level--;
    if (tx->level > 0) {
        pthread_mutex_unlock(&tx->state_mutex);
        return 0;
    }
    pthread_mutex_unlock(&tx->state_mutex);
    {
        int changed = 0;
        f_facts_commit_observer observer;
        void *observer_data;
        for (size_t i = 0; i < tx->rollback.size; i++) {
            if (tx->rollback.entries[i].action != ROLLBACK_STATE) {
                changed = 1;
                break;
            }
        }
        if (tx->listener && tx->rollback.size > 0) {
            tx->listener(facts, tx->rollback.entries, tx->rollback.size, tx->listener_data);
        }
        for (size_t i = 0; i < tx->rollback.size; i++) {
            facts_unintern(facts, tx->rollback.entries[i].fact.s);
            facts_unintern(facts, tx->rollback.entries[i].fact.p);
            facts_unintern(facts, tx->rollback.entries[i].fact.o);
        }
        tx->rollback.size = 0;
        urcu_gc(&tx->rcu);
        pthread_mutex_lock(&tx->state_mutex);
        observer = tx->commit_observer;
        observer_data = tx->commit_observer_data;
        memset(&tx->owner, 0, sizeof(pthread_t));
        tx->owner_valid = 0;
        pthread_mutex_unlock(&tx->state_mutex);
        pthread_rwlock_unlock(&tx->rwlock);
        if (changed && observer)
            observer(facts, observer_data);
    }
    return 0;
}

int transaction_rollback(s_facts *facts, s_transaction *tx)
{
    assert(tx);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    if (tx->level <= 0 || !tx->owner_valid || !pthread_equal(tx->owner, self)) {
        pthread_mutex_unlock(&tx->state_mutex);
        return -1;
    }
    pthread_mutex_unlock(&tx->state_mutex);
    while (tx->rollback.size > 0) {
        s_rollback_entry entry = tx->rollback.entries[--tx->rollback.size];
        facts_apply_rollback_entry(facts, &entry);
        facts_unintern(facts, entry.fact.s);
        facts_unintern(facts, entry.fact.p);
        facts_unintern(facts, entry.fact.o);
    }
    pthread_mutex_lock(&tx->state_mutex);
    tx->level = 0;
    memset(&tx->owner, 0, sizeof(pthread_t));
    tx->owner_valid = 0;
    pthread_mutex_unlock(&tx->state_mutex);
    pthread_rwlock_unlock(&tx->rwlock);
    return 0;
}

void transaction_rollback_push(s_facts *facts, s_transaction *tx, e_rollback_action action, const s_fact *fact)
{
    if (tx->level > 0) {
        if (tx->rollback.size >= tx->rollback.capacity) {
            tx->rollback.capacity = tx->rollback.capacity ? tx->rollback.capacity * 2 : 16;
            tx->rollback.entries = realloc(tx->rollback.entries, tx->rollback.capacity * sizeof(s_rollback_entry));
            assert(tx->rollback.entries);
        }
        s_rollback_entry *entry = &tx->rollback.entries[tx->rollback.size++];
        entry->action = action;
        entry->fact = *fact;
        facts_intern(facts, symbol_to_str(fact->s));
        facts_intern(facts, symbol_to_str(fact->p));
        facts_intern(facts, symbol_to_str(fact->o));
    }
}

int transaction_acquire_writer(s_transaction *tx)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    int has_lock = tx->owner_valid && pthread_equal(tx->owner, self);
    pthread_mutex_unlock(&tx->state_mutex);
    if (!has_lock) {
        pthread_rwlock_wrlock(&tx->rwlock);
        pthread_mutex_lock(&tx->state_mutex);
        tx->owner = self;
        tx->owner_valid = 1;
        pthread_mutex_unlock(&tx->state_mutex);
    }
    return has_lock;
}

void transaction_release_writer(s_transaction *tx, int has_lock)
{
    if (!has_lock) {
        pthread_mutex_lock(&tx->state_mutex);
        memset(&tx->owner, 0, sizeof(pthread_t));
        tx->owner_valid = 0;
        pthread_mutex_unlock(&tx->state_mutex);
        pthread_rwlock_unlock(&tx->rwlock);
    }
}

int transaction_acquire_reader(s_transaction *tx)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    int owns_writer = tx->owner_valid && pthread_equal(tx->owner, self);
    pthread_mutex_unlock(&tx->state_mutex);
    if (owns_writer)
        return 0;
    pthread_rwlock_rdlock(&tx->rwlock);
    return 1;
}

void transaction_release_reader(s_transaction *tx, int acquired)
{
    if (acquired)
        pthread_rwlock_unlock(&tx->rwlock);
}
