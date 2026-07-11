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
    memset(&tx->owner, 0, sizeof(pthread_t));
    tx->listener = NULL;
    tx->listener_data = NULL;
    urcu_init(&tx->rcu, 256);
}

void transaction_destroy(s_transaction *tx)
{
    assert(tx);
    pthread_rwlock_destroy(&tx->rwlock);
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
    if (tx->level > 0 && pthread_equal(tx->owner, self)) {
        tx->level++;
        return 0;
    }
    pthread_rwlock_wrlock(&tx->rwlock);
    tx->owner = self;
    tx->level = 1;
    return 0;
}

int transaction_commit(s_facts *facts, s_transaction *tx)
{
    assert(tx);
    if (tx->level <= 0) {
        return -1;
    }
    tx->level--;
    if (tx->level == 0) {
        if (tx->listener && tx->rollback.size > 0) {
            tx->listener(facts, tx->rollback.entries, tx->rollback.size, tx->listener_data);
        }
        for (size_t i = 0; i < tx->rollback.size; i++) {
            facts_unintern(facts, tx->rollback.entries[i].fact.s);
            facts_unintern(facts, tx->rollback.entries[i].fact.p);
            facts_unintern(facts, tx->rollback.entries[i].fact.o);
        }
        tx->rollback.size = 0;
        memset(&tx->owner, 0, sizeof(pthread_t));
        urcu_gc(&tx->rcu);
        pthread_rwlock_unlock(&tx->rwlock);
    }
    return 0;
}

int transaction_rollback(s_facts *facts, s_transaction *tx)
{
    assert(tx);
    if (tx->level <= 0) {
        return -1;
    }
    while (tx->rollback.size > 0) {
        s_rollback_entry entry = tx->rollback.entries[--tx->rollback.size];
        int saved_level = tx->level;
        tx->level = 0;
        if (entry.action == ROLLBACK_REMOVE) {
            facts_remove_fact(facts, &entry.fact);
        } else if (entry.action == ROLLBACK_ADD) {
            facts_add_fact(facts, &entry.fact);
        }
        tx->level = saved_level;
        facts_unintern(facts, entry.fact.s);
        facts_unintern(facts, entry.fact.p);
        facts_unintern(facts, entry.fact.o);
    }
    tx->level = 0;
    memset(&tx->owner, 0, sizeof(pthread_t));
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
    int has_lock = pthread_equal(tx->owner, self);
    if (!has_lock) {
        pthread_rwlock_wrlock(&tx->rwlock);
        tx->owner = self;
    }
    return has_lock;
}

void transaction_release_writer(s_transaction *tx, int has_lock)
{
    if (!has_lock) {
        memset(&tx->owner, 0, sizeof(pthread_t));
        pthread_rwlock_unlock(&tx->rwlock);
    }
}

void transaction_acquire_reader(s_transaction *tx)
{
    pthread_t self = pthread_self();
    if (!pthread_equal(tx->owner, self)) {
        urcu_read_lock(&tx->rcu);
    }
}

void transaction_release_reader(s_transaction *tx)
{
    pthread_t self = pthread_self();
    if (!pthread_equal(tx->owner, self)) {
        urcu_read_unlock(&tx->rcu);
    }
}
