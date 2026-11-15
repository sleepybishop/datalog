#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "facts.h"
#include "intern.h"

START_TEST(test_intern_init_destroy)
{
    s_intern intern;
    intern_init(&intern, 100);
    ck_assert(intern.symbols != NULL);
    intern_destroy(&intern);
}
END_TEST

START_TEST(test_intern_new_delete)
{
    s_intern *intern = new_intern(100);
    ck_assert(intern != NULL);
    ck_assert(intern->symbols != NULL);
    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_string_basic)
{
    s_intern *intern = new_intern(100);
    Symbol s1 = intern_string(intern, "hello");
    Symbol s2 = intern_string(intern, "hello");
    Symbol s3 = intern_string(intern, "world");

    ck_assert_ptr_eq(s1, s2);
    ck_assert_ptr_ne(s1, s3);
    ck_assert_str_eq(symbol_to_str(s1), "hello");
    ck_assert_str_eq(symbol_to_str(s3), "world");

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_unstring_basic)
{
    s_intern *intern = new_intern(100);
    Symbol s1 = intern_string(intern, "hello");

    /* find symbol should work */
    s_set_item *item1 = intern_find_symbol(intern, "hello");
    ck_assert(item1 != NULL);
    ck_assert_int_eq(item1->usage, 1);

    /* intern again increases usage */
    Symbol s2 = intern_string(intern, "hello");
    ck_assert_ptr_eq(s1, s2);
    ck_assert_int_eq(item1->usage, 2);

    /* unintern once decreases usage */
    intern_unstring(intern, s1);
    ck_assert_int_eq(item1->usage, 1);

    /* unintern again hides the retained entry from lookup */
    intern_unstring(intern, s1);
    ck_assert(intern_find_symbol(intern, "hello") == NULL);

    /* interning again reuses the same stable symbol entry */
    Symbol s3 = intern_string(intern, "hello");
    ck_assert_ptr_eq(s1, s3);
    ck_assert_str_eq(symbol_to_str(s3), "hello");
    s_set_item *item2 = intern_find_symbol(intern, "hello");
    ck_assert(item2 != NULL);
    ck_assert_int_eq(item2->usage, 1);

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_long_basic)
{
    s_intern *intern = new_intern(100);
    Symbol s1 = intern_long(intern, 42);
    Symbol s2 = intern_long(intern, 42);
    Symbol s3 = intern_long(intern, -100);

    ck_assert_ptr_eq(s1, s2);
    ck_assert_ptr_ne(s1, s3);
    ck_assert_str_eq(symbol_to_str(s1), "42");
    ck_assert_str_eq(symbol_to_str(s3), "-100");

    ck_assert_int_eq(intern_get_long(intern, "42"), 42);
    ck_assert_int_eq(intern_get_long(intern, "-100"), -100);

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_double_basic)
{
    s_intern *intern = new_intern(100);
    Symbol s1 = intern_double(intern, 3.14159);
    Symbol s2 = intern_double(intern, 3.14159);
    Symbol s3 = intern_double(intern, -0.007);

    ck_assert_ptr_eq(s1, s2);
    ck_assert_ptr_ne(s1, s3);
    /* use strtod to verify the string is parsed back to the same double */
    double d1 = strtod(symbol_to_str(s1), NULL);
    double d3 = strtod(symbol_to_str(s3), NULL);
    ck_assert_double_eq_tol(d1, 3.14159, 1e-9);
    ck_assert_double_eq_tol(d3, -0.007, 1e-9);

    ck_assert_double_eq_tol(intern_get_double(intern, symbol_to_str(s1)), 3.14159, 1e-9);
    ck_assert_double_eq_tol(intern_get_double(intern, symbol_to_str(s3)), -0.007, 1e-9);

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_find_symbol_str)
{
    s_intern *intern = new_intern(100);
    ck_assert(intern_find_symbol_str(intern, "test") == NULL);

    Symbol s1 = intern_string(intern, "test");
    Symbol found = intern_find_symbol_str(intern, "test");
    ck_assert_ptr_eq(s1, found);

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_string_view)
{
    s_intern *intern = new_intern(100);
    const char *orig = "hello world";
    Symbol s1 = intern_string_view(intern, orig, 5);
    Symbol s2 = intern_string(intern, "hello");
    Symbol s3 = intern_string_view(intern, orig + 6, 5);
    Symbol s4 = intern_string(intern, "world");

    ck_assert_ptr_eq(s1, s2);
    ck_assert_ptr_eq(s3, s4);
    ck_assert_str_eq(symbol_to_str(s1), "hello");
    ck_assert_str_eq(symbol_to_str(s3), "world");

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_ref_and_churn)
{
    s_intern *intern = new_intern(32);
    Symbol s = intern_string(intern, "ephemeral");
    ck_assert_ptr_eq(s, intern_ref(intern, s));
    s_set_item *item = intern_find_symbol(intern, "ephemeral");
    ck_assert(item != NULL);
    ck_assert_int_eq(item->usage, 2);
    intern_unstring(intern, s);
    ck_assert_int_eq(item->usage, 1);
    intern_unstring(intern, s);
    ck_assert(intern_find_symbol(intern, "ephemeral") == NULL);

    for (int i = 0; i < 500; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "sym_%d", i);
        Symbol tmp = intern_string(intern, buf);
        ck_assert(tmp != NULL);
        intern_unstring(intern, tmp);
        ck_assert(intern_find_symbol(intern, buf) == NULL);
    }

    delete_intern(intern);
}
END_TEST

START_TEST(test_intern_repeated_vocabulary_reuses_storage)
{
    const char *words[] = {"subject", "predicate", "object", "message", "ready", "done", "sender", "receiver"};
    Symbol symbols[sizeof(words) / sizeof(words[0])];
    s_intern *intern = new_intern(32);
    ck_assert(intern != NULL);
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        symbols[i] = intern_string(intern, words[i]);
        ck_assert(symbols[i] != NULL);
        intern_unstring(intern, symbols[i]);
    }
    s_intern_page *page = intern->pages;
    size_t used = page->size;
    uint64_t next_id = intern->next_id;
    for (size_t i = 0; i < 100000; i++) {
        size_t word = i % (sizeof(words) / sizeof(words[0]));
        Symbol symbol = intern_string(intern, words[word]);
        ck_assert_ptr_eq(symbol, symbols[word]);
        intern_unstring(intern, symbol);
        ck_assert(intern_find_symbol(intern, words[word]) == NULL);
    }
    ck_assert_ptr_eq(intern->pages, page);
    ck_assert_int_eq(page->size, used);
    ck_assert_int_eq(intern->next_id, next_id);
    ck_assert_ptr_eq(intern_ref(intern, symbols[0]), symbols[0]);
    ck_assert_ptr_eq(intern_find_symbol_str(intern, words[0]), symbols[0]);
    intern_unstring(intern, symbols[0]);
    delete_intern(intern);
}
END_TEST

typedef struct shared_intern_worker {
    s_facts *facts;
    pthread_barrier_t *start;
    unsigned int index;
} s_shared_intern_worker;

static void *exercise_shared_intern(void *argument)
{
    s_shared_intern_worker *worker = argument;
    const char *spec[] = {"?subject", "relation", "shared", NULL, "anchor", "relation", "?object", NULL, NULL};
    s_binding *bindings = spec_bindings(spec);
    ck_assert(bindings != NULL);
    int result = pthread_barrier_wait(worker->start);
    ck_assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned int i = 0; i < 2000; i++) {
        char subject[64];
        snprintf(subject, sizeof(subject), "worker_%u_value_%u", worker->index, i);
        ck_assert(facts_add_spo(worker->facts, subject, "relation", "shared") != NULL);
        ck_assert_int_eq(facts_contains_spo(worker->facts, subject, "relation", "shared"), 1);
        s_facts_with_cursor cursor;
        ck_assert_int_eq(facts_with_checked(worker->facts, bindings, &cursor, spec), 0);
        size_t rows = 0;
        while (facts_with_cursor_next(&cursor))
            rows++;
        ck_assert_int_eq(facts_with_cursor_error(&cursor), 0);
        facts_with_cursor_destroy(&cursor);
        ck_assert_int_eq(rows, 2);
        ck_assert_int_eq(facts_remove_spo(worker->facts, subject, "relation", "shared"), 1);
        ck_assert_int_eq(facts_contains_spo(worker->facts, subject, "relation", "shared"), 0);
    }
    free(bindings);
    return NULL;
}

START_TEST(test_intern_shared_databases_concurrent_queries_and_mutations)
{
    enum { worker_count = 4 };
    s_intern *intern = new_intern(32);
    ck_assert(intern != NULL);
    pthread_barrier_t start;
    ck_assert_int_eq(pthread_barrier_init(&start, NULL, worker_count), 0);
    s_shared_intern_worker workers[worker_count];
    pthread_t threads[worker_count];
    for (unsigned int i = 0; i < worker_count; i++) {
        workers[i] = (s_shared_intern_worker){new_facts(intern, 32), &start, i};
        ck_assert(workers[i].facts != NULL);
        ck_assert(facts_add_spo(workers[i].facts, "anchor", "relation", "shared") != NULL);
        ck_assert_int_eq(pthread_create(&threads[i], NULL, exercise_shared_intern, &workers[i]), 0);
    }
    for (size_t i = 0; i < worker_count; i++) {
        ck_assert_int_eq(pthread_join(threads[i], NULL), 0);
        ck_assert_int_eq(facts_count(workers[i].facts), 1);
        delete_facts(workers[i].facts);
    }
    ck_assert(intern_find_symbol(intern, "anchor") == NULL);
    ck_assert(intern_find_symbol(intern, "relation") == NULL);
    ck_assert(intern_find_symbol(intern, "shared") == NULL);
    ck_assert_int_eq(pthread_barrier_destroy(&start), 0);
    delete_intern(intern);
}
END_TEST

Suite *intern_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Intern");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_intern_init_destroy);
    tcase_add_test(tc_core, test_intern_new_delete);
    tcase_add_test(tc_core, test_intern_string_basic);
    tcase_add_test(tc_core, test_intern_unstring_basic);
    tcase_add_test(tc_core, test_intern_long_basic);
    tcase_add_test(tc_core, test_intern_double_basic);
    tcase_add_test(tc_core, test_intern_find_symbol_str);
    tcase_add_test(tc_core, test_intern_string_view);
    tcase_add_test(tc_core, test_intern_ref_and_churn);
    tcase_add_test(tc_core, test_intern_repeated_vocabulary_reuses_storage);
    tcase_add_test(tc_core, test_intern_shared_databases_concurrent_queries_and_mutations);
    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = intern_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
