#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "transaction.h"
#include "facts.h"
#include "facts_internal.h"

void transaction_init(s_transaction *tx)
{
    if (transaction_init_checked(tx) != 0)
        abort();
}

int transaction_init_checked(s_transaction *tx)
{
    if (!tx)
        return -1;
    memset(tx, 0, sizeof(*tx));
    if (pthread_rwlock_init(&tx->rwlock, NULL) != 0)
        return -1;
    if (pthread_mutex_init(&tx->state_mutex, NULL) != 0) {
        pthread_rwlock_destroy(&tx->rwlock);
        return -1;
    }
    return 0;
}

void transaction_destroy(s_transaction *tx)
{
    assert(tx);
    if (tx->owner_valid || tx->level != 0 || tx->rollback.size != 0)
        abort();
    assert(!tx->owner_valid);
    assert(tx->level == 0);
    assert(tx->rollback.size == 0);
    pthread_rwlock_destroy(&tx->rwlock);
    pthread_mutex_destroy(&tx->state_mutex);
    free(tx->savepoints);
    tx->savepoints = NULL;
    tx->savepoint_capacity = 0;
    free(tx->rollback.entries);
    tx->rollback.entries = NULL;
    tx->rollback.capacity = 0;
    tx->rollback.size = 0;
}

int transaction_begin(s_facts *facts, s_transaction *tx)
{
    (void)facts;
    assert(tx);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    if (tx->owner_valid && pthread_equal(tx->owner, self)) {
        if (tx->level > 0) {
            size_t idx = (size_t)(tx->level - 1);
            if (idx >= tx->savepoint_capacity) {
                size_t new_cap = tx->savepoint_capacity ? tx->savepoint_capacity * 2 : 8;
                if (new_cap <= idx)
                    new_cap = idx + 1;
                if (new_cap > SIZE_MAX / sizeof(*tx->savepoints)) {
                    pthread_mutex_unlock(&tx->state_mutex);
                    return -1;
                }
                size_t *new_savepoints = realloc(tx->savepoints, new_cap * sizeof(*tx->savepoints));
                if (!new_savepoints) {
                    pthread_mutex_unlock(&tx->state_mutex);
                    return -1;
                }
                tx->savepoints = new_savepoints;
                tx->savepoint_capacity = new_cap;
            }
            tx->savepoints[idx] = tx->rollback.size;
        }
        tx->level++;
        pthread_mutex_unlock(&tx->state_mutex);
        return 0;
    }
    pthread_mutex_unlock(&tx->state_mutex);
    if (transaction_reader_owned(tx))
        return -1;
    pthread_rwlock_wrlock(&tx->rwlock);
    pthread_mutex_lock(&tx->state_mutex);
    tx->owner = self;
    tx->owner_valid = 1;
    tx->level = 1;
    tx->io_error = 0;
    pthread_mutex_unlock(&tx->state_mutex);
    return 0;
}

static int transaction_commit_impl(s_facts *facts, s_transaction *tx, int suppress_listener)
{
    assert(tx);
    pthread_t self = pthread_self();
    f_facts_tx_listener listener;
    void *listener_data;
    f_facts_tx_listener internal_listener;
    void *internal_listener_data;
    pthread_mutex_lock(&tx->state_mutex);
    if (tx->level <= 0 || !tx->owner_valid || !pthread_equal(tx->owner, self)) {
        pthread_mutex_unlock(&tx->state_mutex);
        return -1;
    }
    if (tx->level > 1) {
        tx->level--;
        pthread_mutex_unlock(&tx->state_mutex);
        return 0;
    }
    listener = tx->listener;
    listener_data = tx->listener_data;
    internal_listener = tx->internal_listener;
    internal_listener_data = tx->internal_listener_data;
    pthread_mutex_unlock(&tx->state_mutex);
    {
        int changed = 0;
        s_facts_commit_summary summary = {0, 0};
        f_facts_commit_observer observer;
        void *observer_data;
        f_facts_commit_summary_observer summary_observer;
        void *summary_observer_data;
        f_facts_commit_summary_observer internal_summary_observer;
        void *internal_summary_observer_data;
        if (tx->io_error) {
            transaction_rollback(facts, tx);
            return -1;
        }
        if (!suppress_listener && (internal_listener || listener) && tx->rollback.size > 0) {
            size_t entry_count = tx->rollback.size;
            s_rollback_entry *entries = malloc(entry_count * sizeof(*entries));
            if (!entries) {
                transaction_rollback(facts, tx);
                return -1;
            }
            memcpy(entries, tx->rollback.entries, entry_count * sizeof(*entries));
            int listener_result = 0;
            if (internal_listener)
                listener_result = internal_listener(facts, entries, entry_count, internal_listener_data);
            size_t listener_start = tx->rollback.size;
            if (listener_result == 0 && listener)
                listener_result = listener(facts, entries, entry_count, listener_data);
            free(entries);
            /* reconcile listener mutations into the closure before commit */
            if (listener_result == 0 && !tx->io_error && internal_listener && tx->rollback.size > listener_start) {
                size_t added_count = tx->rollback.size - listener_start;
                entries = malloc(added_count * sizeof(*entries));
                if (!entries) {
                    listener_result = -1;
                } else {
                    memcpy(entries, tx->rollback.entries + listener_start, added_count * sizeof(*entries));
                    listener_result = internal_listener(facts, entries, added_count, internal_listener_data);
                    free(entries);
                }
            }
            if (listener_result != 0 || tx->io_error) {
                transaction_rollback(facts, tx);
                return -1;
            }
        }
        facts_justifications_commit(facts);
        for (size_t i = 0; i < tx->rollback.size; i++) {
            /* support-only changes can make a derived tuple visible to Linda. */
            unsigned int partition = facts_commit_subject_partition(symbol_to_str(tx->rollback.entries[i].fact.s));
            changed = 1;
            summary.subject_partitions |= UINT64_C(1);
            summary.subject_partitions |= UINT64_C(1) << partition;
            if (tx->rollback.entries[i].action != ROLLBACK_STATE) {
                summary.physical_changes++;
            }
        }
        facts_reclaim_retired(facts);
        for (size_t i = 0; i < tx->rollback.size; i++) {
            facts_unintern(facts, tx->rollback.entries[i].fact.s);
            facts_unintern(facts, tx->rollback.entries[i].fact.p);
            facts_unintern(facts, tx->rollback.entries[i].fact.o);
        }
        tx->rollback.size = 0;
        pthread_mutex_lock(&tx->state_mutex);
        tx->level = 0;
        observer = tx->commit_observer;
        observer_data = tx->commit_observer_data;
        summary_observer = tx->commit_summary_observer;
        summary_observer_data = tx->commit_summary_observer_data;
        internal_summary_observer = tx->internal_commit_summary_observer;
        internal_summary_observer_data = tx->internal_commit_summary_observer_data;
        memset(&tx->owner, 0, sizeof(pthread_t));
        tx->owner_valid = 0;
        pthread_mutex_unlock(&tx->state_mutex);
        pthread_rwlock_unlock(&tx->rwlock);
        if (changed && summary_observer)
            summary_observer(facts, &summary, summary_observer_data);
        if (changed && internal_summary_observer)
            internal_summary_observer(facts, &summary, internal_summary_observer_data);
        if (changed && observer)
            observer(facts, observer_data);
    }
    return 0;
}

int transaction_commit(s_facts *facts, s_transaction *tx)
{
    return transaction_commit_impl(facts, tx, 0);
}

int transaction_commit_silent(s_facts *facts, s_transaction *tx)
{
    return transaction_commit_impl(facts, tx, 1);
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
    int level = tx->level;
    size_t target_size = (level > 1) ? tx->savepoints[level - 2] : 0;
    pthread_mutex_unlock(&tx->state_mutex);

    if (level == 1 || (facts->justifications_staging && level <= facts->justifications_staged_level))
        facts_justifications_discard(facts);

    while (tx->rollback.size > target_size) {
        s_rollback_entry entry = tx->rollback.entries[--tx->rollback.size];
        facts_apply_rollback_entry(facts, &entry);
        facts_unintern(facts, entry.fact.s);
        facts_unintern(facts, entry.fact.p);
        facts_unintern(facts, entry.fact.o);
    }

    if (level == 1)
        facts_reclaim_retired(facts);
    int io_error = tx->io_error;

    pthread_mutex_lock(&tx->state_mutex);
    if (level > 1) {
        tx->level = level - 1;
        pthread_mutex_unlock(&tx->state_mutex);
        return 0;
    }
    tx->level = 0;
    tx->io_error = 0;
    memset(&tx->owner, 0, sizeof(pthread_t));
    tx->owner_valid = 0;
    pthread_mutex_unlock(&tx->state_mutex);
    pthread_rwlock_unlock(&tx->rwlock);
    return io_error ? -1 : 0;
}

int transaction_rollback_push(s_facts *facts, s_transaction *tx, e_rollback_action action, const s_fact *fact)
{
    if (tx->level > 0) {
        if (tx->rollback.size >= tx->rollback.capacity) {
            size_t capacity = tx->rollback.capacity ? tx->rollback.capacity * 2 : 16;
            if (capacity < tx->rollback.capacity || capacity > SIZE_MAX / sizeof(s_rollback_entry))
                return -1;
            s_rollback_entry *entries = realloc(tx->rollback.entries, capacity * sizeof(*entries));
            if (!entries)
                return -1;
            tx->rollback.entries = entries;
            tx->rollback.capacity = capacity;
        }
        Symbol s_ref = facts_intern_ref(facts, fact->s);
        Symbol p_ref = facts_intern_ref(facts, fact->p);
        Symbol o_ref = facts_intern_ref(facts, fact->o);
        if (!s_ref || !p_ref || !o_ref) {
            if (s_ref)
                facts_unintern(facts, s_ref);
            if (p_ref)
                facts_unintern(facts, p_ref);
            if (o_ref)
                facts_unintern(facts, o_ref);
            return -1;
        }
        s_rollback_entry *entry = &tx->rollback.entries[tx->rollback.size++];
        entry->action = action;
        entry->fact = *fact;
    }
    return 0;
}

void facts_rollback_pop_discard(s_facts *facts)
{
    if (!facts || facts->tx.level <= 0 || facts->tx.rollback.size == 0)
        return;
    s_rollback_entry entry = facts->tx.rollback.entries[--facts->tx.rollback.size];
    facts_unintern(facts, entry.fact.s);
    facts_unintern(facts, entry.fact.p);
    facts_unintern(facts, entry.fact.o);
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

int transaction_writer_owned(s_transaction *tx)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&tx->state_mutex);
    int owned = tx->owner_valid && pthread_equal(tx->owner, self);
    pthread_mutex_unlock(&tx->state_mutex);
    return owned;
}

/*
 * a query cursor may hold a read lock while helper lookups acquire another read
 * section. recursively calling pthread_rwlock_rdlock is not portable and can
 * deadlock when a writer is waiting, so track read nesting per thread/DB.
 */
typedef struct transaction_reader_slot {
    s_transaction *tx;
    unsigned int depth;
} s_transaction_reader_slot;

#define TRANSACTION_READER_SLOTS 64
static _Thread_local s_transaction_reader_slot reader_slots[TRANSACTION_READER_SLOTS];

int transaction_acquire_reader(s_transaction *tx)
{
    if (transaction_writer_owned(tx))
        return 0;

    s_transaction_reader_slot *empty = NULL;
    for (size_t i = 0; i < TRANSACTION_READER_SLOTS; i++) {
        if (reader_slots[i].tx == tx) {
            reader_slots[i].depth++;
            return 1;
        }
        if (!reader_slots[i].tx && !empty)
            empty = &reader_slots[i];
    }

    /* more than 64 databases locked by one thread is a programming error. */
    if (!empty)
        abort();
    pthread_rwlock_rdlock(&tx->rwlock);
    empty->tx = tx;
    empty->depth = 1;
    return 1;
}

void transaction_release_reader(s_transaction *tx, int acquired)
{
    if (acquired <= 0)
        return;
    for (size_t i = 0; i < TRANSACTION_READER_SLOTS; i++) {
        if (reader_slots[i].tx == tx) {
            assert(reader_slots[i].depth > 0);
            if (--reader_slots[i].depth == 0) {
                reader_slots[i].tx = NULL;
                pthread_rwlock_unlock(&tx->rwlock);
            }
            return;
        }
    }
    assert(0 && "unbalanced transaction reader release");
}

int transaction_reader_owned(s_transaction *tx)
{
    for (size_t i = 0; i < TRANSACTION_READER_SLOTS; i++) {
        if (reader_slots[i].tx == tx)
            return reader_slots[i].depth > 0;
    }
    return 0;
}
