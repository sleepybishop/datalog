#include <check.h>
#include <errno.h>
#include <pthread.h>
#include "facts.h"
#include "sparql.h"

/* fail only allocations in the thread executing the operation under test. */
static _Thread_local int fail_allocations;
static _Thread_local size_t fail_malloc_size;
static _Thread_local int failed_malloc;
static int count_slabs;
static size_t slab_allocations;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);

void *__wrap_malloc(size_t size)
{
    if (fail_allocations || (fail_malloc_size && size == fail_malloc_size)) {
        fail_malloc_size = 0;
        failed_malloc = 1;
        errno = ENOMEM;
        return NULL;
    }
    if (count_slabs && size == sizeof(void *) + 1024 * sizeof(s_fact))
        slab_allocations++;
    return __real_malloc(size);
}

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocations) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_calloc(count, size);
}

void *__wrap_realloc(void *pointer, size_t size)
{
    if (fail_allocations) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(pointer, size);
}

static pthread_mutex_t unlock_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unlock_cond = PTHREAD_COND_INITIALIZER;
static int unlock_paused;
static int unlock_resume;
static _Thread_local int pause_after_unlock;

int __real_pthread_rwlock_unlock(pthread_rwlock_t *lock);

int __wrap_pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
    int result = __real_pthread_rwlock_unlock(lock);
    if (pause_after_unlock) {
        pause_after_unlock = 0;
        pthread_mutex_lock(&unlock_gate);
        unlock_paused = 1;
        pthread_cond_signal(&unlock_cond);
        while (!unlock_resume)
            pthread_cond_wait(&unlock_cond, &unlock_gate);
        pthread_mutex_unlock(&unlock_gate);
    }
    return result;
}

START_TEST(test_rollback_restores_deleted_fact_without_allocation)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "subject", "predicate", "object") != NULL);
    ck_assert_int_eq(facts_transaction_begin(facts), 0);
    ck_assert_int_eq(facts_remove_spo(facts, "subject", "predicate", "object"), 1);
    fail_allocations = 1;
    int result = facts_transaction_rollback(facts);
    fail_allocations = 0;
    ck_assert_int_eq(result, 0);
    ck_assert_int_eq(facts_contains_spo(facts, "subject", "predicate", "object"), 1);
    ck_assert_int_eq(facts_count(facts), 1);
    delete_facts(facts);
}
END_TEST

START_TEST(test_nested_rollback_restores_deleted_facts_without_allocation)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "baseline", "predicate", "object") != NULL);
    ck_assert_int_eq(facts_transaction_begin(facts), 0);
    ck_assert(facts_add_spo(facts, "outer", "predicate", "object") != NULL);
    ck_assert_int_eq(facts_transaction_begin(facts), 0);
    ck_assert_int_eq(facts_remove_spo(facts, "baseline", "predicate", "object"), 1);
    ck_assert_int_eq(facts_remove_spo(facts, "outer", "predicate", "object"), 1);
    ck_assert(facts_add_spo(facts, "inner", "predicate", "object") != NULL);
    fail_allocations = 1;
    int result = facts_transaction_rollback(facts);
    fail_allocations = 0;
    ck_assert_int_eq(result, 0);
    ck_assert_int_eq(facts_contains_spo(facts, "baseline", "predicate", "object"), 1);
    ck_assert_int_eq(facts_contains_spo(facts, "outer", "predicate", "object"), 1);
    ck_assert_int_eq(facts_contains_spo(facts, "inner", "predicate", "object"), 0);
    ck_assert_int_eq(facts_count(facts), 2);
    fail_allocations = 1;
    result = facts_transaction_rollback(facts);
    fail_allocations = 0;
    ck_assert_int_eq(result, 0);
    ck_assert_int_eq(facts_contains_spo(facts, "baseline", "predicate", "object"), 1);
    ck_assert_int_eq(facts_contains_spo(facts, "outer", "predicate", "object"), 0);
    ck_assert_int_eq(facts_count(facts), 1);
    delete_facts(facts);
}
END_TEST

typedef struct {
    s_facts *facts;
    int result;
} s_thread_facts;

static void *add_and_pause(void *argument)
{
    s_thread_facts *args = argument;
    pause_after_unlock = 1;
    args->result = facts_add_spo(args->facts, "subject", "predicate", "object") != NULL;
    return NULL;
}

START_TEST(test_add_and_reset_are_atomic)
{
    alarm(10);
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    s_thread_facts args = {facts, 0};
    pthread_t thread;
    ck_assert_int_eq(pthread_create(&thread, NULL, add_and_pause, &args), 0);
    pthread_mutex_lock(&unlock_gate);
    while (!unlock_paused)
        pthread_cond_wait(&unlock_cond, &unlock_gate);
    pthread_mutex_unlock(&unlock_gate);
    /* the first unlock must happen only after the whole insertion is complete. */
    ck_assert_int_eq(facts_reset_checked(facts), 0);
    pthread_mutex_lock(&unlock_gate);
    unlock_resume = 1;
    pthread_cond_signal(&unlock_cond);
    pthread_mutex_unlock(&unlock_gate);
    ck_assert_int_eq(pthread_join(thread, NULL), 0);
    ck_assert_int_eq(args.result, 1);
    ck_assert_int_eq(facts_count(facts), 0);
    ck_assert_int_eq(facts_contains_symbol(facts, "subject"), 0);
    ck_assert(facts_add_spo(facts, "fresh", "predicate", "object") != NULL);
    ck_assert_int_eq(facts_contains_spo(facts, "fresh", "predicate", "object"), 1);
    delete_facts(facts);
    alarm(0);
}
END_TEST

static pthread_mutex_t attach_gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t attach_cond = PTHREAD_COND_INITIALIZER;
static int start_insert;
static int insert_staged;
static int attach_done;
static s_datalog_program *attach_program;

static void wait_for_concurrent_insert(s_facts *facts, void *unused)
{
    (void)facts;
    (void)unused;
    pthread_mutex_lock(&attach_gate);
    start_insert = 1;
    pthread_cond_broadcast(&attach_cond);
    while (!insert_staged)
        pthread_cond_wait(&attach_cond, &attach_gate);
    pthread_mutex_unlock(&attach_gate);
}

static int reject_concurrent_insert(s_facts *facts, const s_rollback_entry *entries, size_t count, void *unused)
{
    (void)facts;
    (void)entries;
    (void)count;
    (void)unused;
    pthread_mutex_lock(&attach_gate);
    insert_staged = 1;
    pthread_cond_broadcast(&attach_cond);
    while (!attach_done)
        pthread_cond_wait(&attach_cond, &attach_gate);
    pthread_mutex_unlock(&attach_gate);
    return -1;
}

static void *attach_with_interleaving(void *argument)
{
    s_thread_facts *args = argument;
    args->result = facts_attach_program(args->facts, attach_program);
    pthread_mutex_lock(&attach_gate);
    attach_done = 1;
    pthread_cond_broadcast(&attach_cond);
    pthread_mutex_unlock(&attach_gate);
    return NULL;
}

static void *insert_with_interleaving(void *argument)
{
    s_thread_facts *args = argument;
    pthread_mutex_lock(&attach_gate);
    while (!start_insert)
        pthread_cond_wait(&attach_cond, &attach_gate);
    pthread_mutex_unlock(&attach_gate);
    ck_assert_int_eq(facts_transaction_begin(args->facts), 0);
    ck_assert(facts_add_spo(args->facts, "b", "source", "yes") != NULL);
    args->result = facts_transaction_commit(args->facts);
    return NULL;
}

START_TEST(test_attach_does_not_commit_another_transactions_proofs)
{
    alarm(10);
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "a", "source", "yes") != NULL);
    attach_program = new_datalog_program();
    ck_assert(attach_program != NULL);
    ck_assert_int_eq(datalog_program_parse_rules(attach_program, "?x p yes :- ?x source yes ."), 0);
    facts_register_commit_observer(facts, wait_for_concurrent_insert, NULL);
    facts_register_tx_listener(facts, reject_concurrent_insert, NULL);
    s_thread_facts attaching = {facts, -1};
    s_thread_facts inserting = {facts, 0};
    pthread_t attach_thread, insert_thread;
    ck_assert_int_eq(pthread_create(&insert_thread, NULL, insert_with_interleaving, &inserting), 0);
    ck_assert_int_eq(pthread_create(&attach_thread, NULL, attach_with_interleaving, &attaching), 0);
    ck_assert_int_eq(pthread_join(attach_thread, NULL), 0);
    ck_assert_int_eq(pthread_join(insert_thread, NULL), 0);
    facts_register_commit_observer(facts, NULL, NULL);
    facts_register_tx_listener(facts, NULL, NULL);
    ck_assert_int_eq(attaching.result, 0);
    ck_assert_int_eq(inserting.result, -1);
    ck_assert_int_eq(facts_contains_spo(facts, "b", "source", "yes"), 0);
    ck_assert_int_eq(facts_contains_spo(facts, "b", "p", "yes"), 0);
    ck_assert_int_eq(facts_justification_count(facts, "b", "p", "yes"), 0);
    ck_assert_int_eq(facts_contains_spo(facts, "a", "p", "yes"), 1);
    ck_assert_int_eq(facts_justification_count(facts, "a", "p", "yes"), 1);
    delete_datalog_program(attach_program);
    delete_facts(facts);
    alarm(0);
}
END_TEST

START_TEST(test_destroyed_databases_recycle_fact_slabs)
{
    s_facts *anchor = new_facts(NULL, 100);
    ck_assert(anchor != NULL);
    size_t warmup_slabs = 0;
    count_slabs = 1;
    for (int iteration = 0; iteration < 20; iteration++) {
        s_facts *temporary = new_facts(NULL, 3000);
        ck_assert(temporary != NULL);
        for (int i = 0; i < 2048; i++) {
            char object[32];
            snprintf(object, sizeof(object), "%d", i);
            ck_assert(facts_add_spo(temporary, "subject", "predicate", object) != NULL);
        }
        delete_facts(temporary);
        if (iteration == 0) {
            warmup_slabs = slab_allocations;
            ck_assert(warmup_slabs > 0);
        } else {
            ck_assert_int_eq(slab_allocations, warmup_slabs);
        }
    }
    count_slabs = 0;
    delete_facts(anchor);
}
END_TEST

static void *insert_after_cursor_error(void *argument)
{
    s_thread_facts *args = argument;
    args->result = facts_add_spo(args->facts, "after", "cursor", "error") != NULL;
    return NULL;
}

START_TEST(test_negation_allocation_failure_releases_cursor_locks)
{
    alarm(10);
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "s", "p", "o") != NULL);
    s_facts_with_cursor cursor;
    s_binding *bindings = NULL;
    ck_assert_int_eq(facts_sparql_eval(facts, "SELECT ?s WHERE { ?s <p> <o> . NOT ?s <q> <v> . }", &cursor, &bindings), 0);
    size_t binding_count = 0;
    while (bindings[binding_count].name)
        binding_count++;
    ck_assert(binding_count > 0);
    failed_malloc = 0;
    fail_malloc_size = binding_count * sizeof(const char *);
    int found = facts_with_cursor_next(&cursor);
    fail_malloc_size = 0;
    ck_assert(failed_malloc);
    ck_assert_int_eq(found, 0);
    ck_assert_int_eq(facts_with_cursor_error(&cursor), -1);
    facts_with_cursor_destroy(&cursor);
    free(bindings);
    s_thread_facts args = {facts, 0};
    pthread_t writer;
    ck_assert_int_eq(pthread_create(&writer, NULL, insert_after_cursor_error, &args), 0);
    ck_assert_int_eq(pthread_join(writer, NULL), 0);
    ck_assert_int_eq(args.result, 1);
    delete_facts(facts);
    alarm(0);
}
END_TEST

START_TEST(test_failed_cursor_initialization_can_be_destroyed)
{
    alarm(10);
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    const char *value = NULL;
    s_binding bindings[] = {{"?s", &value}, {NULL, NULL}};
    const char *spec[] = {"?s", "predicate", "object", NULL, NULL};
    s_spec_fact triple = {"?s", "predicate", "object", NULL};
    const unsigned char variable_mask = 1;
    for (int typed = 0; typed < 2; typed++) {
        s_facts_with_cursor cursor;
        memset(&cursor, 0xa5, sizeof(cursor));
        fail_allocations = 1;
        int result = typed ? facts_with_typed_checked(facts, bindings, &cursor, &triple, &variable_mask, 1)
                           : facts_with_checked(facts, bindings, &cursor, spec);
        fail_allocations = 0;
        ck_assert_int_eq(result, -1);
        ck_assert_int_eq(facts_with_cursor_error(&cursor), -1);
        facts_with_cursor_destroy(&cursor);
    }
    s_thread_facts args = {facts, 0};
    pthread_t writer;
    ck_assert_int_eq(pthread_create(&writer, NULL, insert_after_cursor_error, &args), 0);
    ck_assert_int_eq(pthread_join(writer, NULL), 0);
    ck_assert_int_eq(args.result, 1);
    delete_facts(facts);
    alarm(0);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("Storage failures");
    TCase *core = tcase_create("Fault injection and concurrency");
    tcase_add_test(core, test_rollback_restores_deleted_fact_without_allocation);
    tcase_add_test(core, test_nested_rollback_restores_deleted_facts_without_allocation);
    tcase_add_test(core, test_add_and_reset_are_atomic);
    tcase_add_test(core, test_attach_does_not_commit_another_transactions_proofs);
    tcase_add_test(core, test_destroyed_databases_recycle_fact_slabs);
    tcase_add_test(core, test_negation_allocation_failure_releases_cursor_locks);
    tcase_add_test(core, test_failed_cursor_initialization_can_be_destroyed);
    suite_add_tcase(suite, core);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
