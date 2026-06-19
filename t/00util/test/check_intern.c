#include <check.h>
#include <stdlib.h>
#include <string.h>
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

    // Find symbol should work
    s_set_item *item1 = intern_find_symbol(intern, "hello");
    ck_assert(item1 != NULL);
    ck_assert_int_eq(item1->usage, 1);

    // Intern again increases usage
    Symbol s2 = intern_string(intern, "hello");
    ck_assert_ptr_eq(s1, s2);
    ck_assert_int_eq(item1->usage, 2);

    // Unintern once decreases usage
    intern_unstring(intern, s1);
    ck_assert_int_eq(item1->usage, 1);

    // Unintern again deletes from set
    intern_unstring(intern, s1);
    ck_assert(intern_find_symbol(intern, "hello") == NULL);

    // Interning again creates a new symbol entry
    Symbol s3 = intern_string(intern, "hello");
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
    // Use strtod to verify the string is parsed back to the same double
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
