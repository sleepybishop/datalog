#include <check.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "linda.h"

START_TEST(test_linda_basic_out_in)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);

    int rc = linda_out(space, "item", "1", "widget");
    ck_assert_int_eq(0, rc);

    char val[256] = {0};
    int found = linda_rdp(space, "item", "1", "?Val", NULL, 0, NULL, 0, val, sizeof(val));
    ck_assert_int_eq(1, found);
    ck_assert_str_eq("widget", val);

    char id[256] = {0};
    found = linda_inp(space, "item", "?Id", "widget", NULL, 0, id, sizeof(id), NULL, 0);
    ck_assert_int_eq(1, found);
    ck_assert_str_eq("1", id);

    found = linda_rdp(space, "item", "1", "widget", NULL, 0, NULL, 0, NULL, 0);
    ck_assert_int_eq(0, found);

    delete_linda_space(space);
}
END_TEST

START_TEST(test_linda_consumes_long_tuple)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);
    char *subject = malloc(1024);
    ck_assert(subject != NULL);
    memset(subject, 's', 1023);
    subject[1023] = '\0';

    ck_assert_int_eq(0, linda_out(space, subject, "predicate", "object"));
    ck_assert_int_eq(1, linda_inp(space, "?s", "predicate", "object", NULL, 0, NULL, 0, NULL, 0));
    ck_assert_int_eq(0, linda_rdp(space, subject, "predicate", "object", NULL, 0, NULL, 0, NULL, 0));

    free(subject);
    delete_linda_space(space);
}
END_TEST

static void *linda_producer(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    usleep(50000); /* 50ms sleep */
    linda_out(space, "msg", "from", "producer");
    return NULL;
}

START_TEST(test_linda_blocking_in)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);

    pthread_t thread;
    int rc = pthread_create(&thread, NULL, linda_producer, space);
    ck_assert_int_eq(0, rc);

    char payload[256] = {0};
    rc = linda_in(space, "msg", "from", "?Payload", NULL, 0, NULL, 0, payload, sizeof(payload));
    ck_assert_int_eq(0, rc);
    ck_assert_str_eq("producer", payload);

    pthread_join(thread, NULL);
    delete_linda_space(space);
}
END_TEST

static void *eval_worker(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    linda_out(space, "eval", "status", "done");
    return NULL;
}

START_TEST(test_linda_eval)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);

    int rc = linda_eval(space, eval_worker, space);
    ck_assert_int_eq(0, rc);

    char status[256] = {0};
    rc = linda_rd(space, "eval", "status", "?Status", NULL, 0, NULL, 0, status, sizeof(status));
    ck_assert_int_eq(0, rc);
    ck_assert_str_eq("done", status);

    delete_linda_space(space);
}
END_TEST

START_TEST(test_linda_timed_operations)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);
    ck_assert_int_eq(linda_rd_timed(space, "missing", "tuple", "?Value", NULL, 0, NULL, 0, NULL, 0, 20),
                     LINDA_TIMEOUT);
    ck_assert_int_eq(linda_in_timed(space, "missing", "tuple", "?Value", NULL, 0, NULL, 0, NULL, 0, 20),
                     LINDA_TIMEOUT);
    ck_assert_int_eq(linda_rd_timed(space, "missing", "tuple", "?Value", NULL, 0, NULL, 0, NULL, 0, -1),
                     LINDA_ERROR);
    delete_linda_space(space);
}
END_TEST

START_TEST(test_linda_reusable_pattern)
{
    s_linda_space *space = new_linda_space(1000);
    s_linda_pattern *pattern = new_linda_pattern("?Value", "equals", "?Value");
    ck_assert(space != NULL);
    ck_assert(pattern != NULL);
    ck_assert_int_eq(linda_out(space, "different", "equals", "other"), LINDA_OK);
    ck_assert_int_eq(linda_rdp_pattern(space, pattern, NULL, 0, NULL, 0, NULL, 0), 0);
    ck_assert_int_eq(linda_out(space, "same", "equals", "same"), LINDA_OK);
    char value[32] = {0};
    ck_assert_int_eq(linda_inp_pattern(space, pattern, value, sizeof(value), NULL, 0, NULL, 0), 1);
    ck_assert_str_eq(value, "same");
    delete_linda_pattern(pattern);
    delete_linda_space(space);
}
END_TEST

static void *direct_transaction_producer(void *arg)
{
    s_linda_space *space = (s_linda_space *)arg;
    usleep(50000);
    facts_transaction_begin(space->db);
    facts_add_spo(space->db, "direct", "commit", "visible");
    facts_transaction_commit(space->db);
    return NULL;
}

START_TEST(test_linda_wakes_after_direct_database_commit)
{
    s_linda_space *space = new_linda_space(1000);
    ck_assert(space != NULL);
    pthread_t thread;
    ck_assert_int_eq(pthread_create(&thread, NULL, direct_transaction_producer, space), 0);
    char value[32] = {0};
    ck_assert_int_eq(linda_rd_timed(space, "direct", "commit", "?Value", NULL, 0, NULL, 0, value, sizeof(value), 500),
                     LINDA_OK);
    ck_assert_str_eq(value, "visible");
    pthread_join(thread, NULL);
    delete_linda_space(space);
}
END_TEST

static void *slow_eval_worker(void *arg)
{
    int *finished = (int *)arg;
    usleep(50000);
    *finished = 1;
    return NULL;
}

START_TEST(test_linda_delete_waits_for_eval_workers)
{
    s_linda_space *space = new_linda_space(1000);
    int finished = 0;
    ck_assert(space != NULL);
    ck_assert_int_eq(linda_eval(space, slow_eval_worker, &finished), LINDA_OK);
    delete_linda_space(space);
    ck_assert_int_eq(finished, 1);
}
END_TEST

typedef struct {
    s_linda_space *space;
    int id;
    int failed;
} s_stress_args;

static void *stress_producer(void *arg)
{
    s_stress_args *args = (s_stress_args *)arg;
    for (int i = 0; i < 50; i++) {
        char subject[32];
        char value[32];
        snprintf(subject, sizeof(subject), "producer-%d", args->id);
        snprintf(value, sizeof(value), "item-%d-%d", args->id, i);
        if (linda_out(args->space, subject, "task", value) != LINDA_OK) {
            args->failed = 1;
            break;
        }
    }
    return NULL;
}

static void *stress_consumer(void *arg)
{
    s_stress_args *args = (s_stress_args *)arg;
    for (int i = 0; i < 50; i++) {
        if (linda_in_timed(args->space, "?Producer", "task", "?Item", NULL, 0, NULL, 0, NULL, 0, 5000) != LINDA_OK) {
            args->failed = 1;
            break;
        }
    }
    return NULL;
}

START_TEST(test_linda_parallel_producers_and_consumers)
{
    s_linda_space *space = new_linda_space(2000);
    ck_assert(space != NULL);
    pthread_t producers[4];
    pthread_t consumers[4];
    s_stress_args producer_args[4];
    s_stress_args consumer_args[4];
    for (int i = 0; i < 4; i++) {
        producer_args[i].space = space;
        producer_args[i].id = i;
        producer_args[i].failed = 0;
        consumer_args[i].space = space;
        consumer_args[i].id = i;
        consumer_args[i].failed = 0;
        ck_assert_int_eq(pthread_create(&producers[i], NULL, stress_producer, &producer_args[i]), 0);
        ck_assert_int_eq(pthread_create(&consumers[i], NULL, stress_consumer, &consumer_args[i]), 0);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(producers[i], NULL);
        pthread_join(consumers[i], NULL);
        ck_assert_int_eq(producer_args[i].failed, 0);
        ck_assert_int_eq(consumer_args[i].failed, 0);
    }
    ck_assert_int_eq(facts_count(space->db), 0);
    delete_linda_space(space);
}
END_TEST

Suite *linda_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Linda");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_linda_basic_out_in);
    tcase_add_test(tc_core, test_linda_consumes_long_tuple);
    tcase_add_test(tc_core, test_linda_blocking_in);
    tcase_add_test(tc_core, test_linda_eval);
    tcase_add_test(tc_core, test_linda_timed_operations);
    tcase_add_test(tc_core, test_linda_reusable_pattern);
    tcase_add_test(tc_core, test_linda_wakes_after_direct_database_commit);
    tcase_add_test(tc_core, test_linda_delete_waits_for_eval_workers);
    tcase_add_test(tc_core, test_linda_parallel_producers_and_consumers);

    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = linda_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
