#include <check.h>
#include <pthread.h>
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

Suite *linda_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Linda");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_linda_basic_out_in);
    tcase_add_test(tc_core, test_linda_blocking_in);
    tcase_add_test(tc_core, test_linda_eval);

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
