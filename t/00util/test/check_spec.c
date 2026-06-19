#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "spec.h"

START_TEST(test_spec_cursor_empty)
{
    s_spec_cursor c;
    s_spec_fact f;
    spec_cursor_init(&c, (const char *[]){NULL, NULL});
    ck_assert(!spec_cursor_next(&c, &f));
}
END_TEST

START_TEST(test_spec_cursor_one)
{
    s_spec_cursor c;
    s_spec_fact f;
    spec_cursor_init(&c, (const char *[]){"a", "b", "c", NULL, NULL});
    ck_assert(spec_cursor_next(&c, &f));
    ck_assert(!strcmp(f.s, "a"));
    ck_assert(!strcmp(f.p, "b"));
    ck_assert(!strcmp(f.o, "c"));
    ck_assert(!spec_cursor_next(&c, &f));
}
END_TEST

START_TEST(test_spec_cursor_two)
{
    s_spec_cursor c;
    s_spec_fact f;
    spec_cursor_init(&c, (const char *[]){"a", "b", "c", "d", "e", NULL, NULL});
    ck_assert(spec_cursor_next(&c, &f));
    ck_assert(!strcmp(f.s, "a"));
    ck_assert(!strcmp(f.p, "b"));
    ck_assert(!strcmp(f.o, "c"));
    ck_assert(spec_cursor_next(&c, &f));
    ck_assert(!strcmp(f.s, "a"));
    ck_assert(!strcmp(f.p, "d"));
    ck_assert(!strcmp(f.o, "e"));
    ck_assert(!spec_cursor_next(&c, &f));
    spec_cursor_init(&c, (const char *[]){"a", "b", "c", NULL, "b", "d", "e", NULL, NULL});
    ck_assert(spec_cursor_next(&c, &f));
    ck_assert(!strcmp(f.s, "a"));
    ck_assert(!strcmp(f.p, "b"));
    ck_assert(!strcmp(f.o, "c"));
    ck_assert(spec_cursor_next(&c, &f));
    ck_assert(!strcmp(f.s, "b"));
    ck_assert(!strcmp(f.p, "d"));
    ck_assert(!strcmp(f.o, "e"));
    ck_assert(!spec_cursor_next(&c, &f));
}
END_TEST

START_TEST(test_spec_count_bindings)
{
    ck_assert_int_eq(0, spec_count_bindings((const char *[]){NULL, NULL}));
    ck_assert_int_eq(0, spec_count_bindings((const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert_int_eq(2, spec_count_bindings((const char *[]){"?a", "b", "c", "d", "?e", NULL, NULL}));
}
END_TEST

START_TEST(test_spec_count_facts)
{
    ck_assert_int_eq(0, spec_count_facts((const char *[]){NULL, NULL}));
    ck_assert_int_eq(1, spec_count_facts((const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert_int_eq(2, spec_count_facts((const char *[]){"a", "b", "c", "d", "e", NULL, NULL}));
}
END_TEST

START_TEST(test_spec_expand)
{
    p_spec spec = (const char *[]){"a", "b", "c", "d", "e", NULL, NULL};
    p_spec expanded = spec_expand(spec);
    ck_assert(expanded);
    ck_assert_str_eq("a", expanded[0]);
    ck_assert_str_eq("b", expanded[1]);
    ck_assert_str_eq("c", expanded[2]);
    ck_assert(!expanded[3]);
    ck_assert_str_eq("a", expanded[4]);
    ck_assert_str_eq("d", expanded[5]);
    ck_assert_str_eq("e", expanded[6]);
    ck_assert(!expanded[7]);
    ck_assert(!expanded[8]);
    free(expanded);
}
END_TEST

START_TEST(test_spec_sort)
{
    const char *spec_data[] = {"?a", "b", "?c", NULL, "x", "y", "z", NULL, NULL};
    // Create a mutable copy of the spec since spec_sort modifies it in-place
    p_spec spec = malloc(sizeof(spec_data));
    memcpy(spec, spec_data, sizeof(spec_data));

    p_spec sorted = spec_sort(spec);
    ck_assert(sorted);
    // The 0-variable fact {"x", "y", "z"} should be sorted before the 2-variable fact {"?a", "b", "?c"}
    ck_assert_str_eq("x", sorted[0]);
    ck_assert_str_eq("y", sorted[1]);
    ck_assert_str_eq("z", sorted[2]);
    ck_assert(!sorted[3]);
    ck_assert_str_eq("?a", sorted[4]);
    ck_assert_str_eq("b", sorted[5]);
    ck_assert_str_eq("?c", sorted[6]);
    ck_assert(!sorted[7]);
    ck_assert(!sorted[8]);
    free(spec);
}
END_TEST

START_TEST(test_spec_bindings)
{
    p_spec spec = (const char *[]){"?a", "b", "c", NULL, NULL};
    s_binding *bindings = spec_bindings(spec);
    ck_assert(bindings);
    ck_assert_str_eq("?a", bindings[0].name);
    ck_assert(!bindings[1].name);
    free(bindings);
}
END_TEST

Suite *spec_suite(void)
{
    Suite *s;
    TCase *tc_cursor;
    TCase *tc_logic;
    s = suite_create("Spec");
    tc_cursor = tcase_create("Cursor");
    tcase_add_test(tc_cursor, test_spec_cursor_empty);
    tcase_add_test(tc_cursor, test_spec_cursor_one);
    tcase_add_test(tc_cursor, test_spec_cursor_two);
    suite_add_tcase(s, tc_cursor);

    tc_logic = tcase_create("Logic");
    tcase_add_test(tc_logic, test_spec_count_bindings);
    tcase_add_test(tc_logic, test_spec_count_facts);
    tcase_add_test(tc_logic, test_spec_expand);
    tcase_add_test(tc_logic, test_spec_sort);
    tcase_add_test(tc_logic, test_spec_bindings);
    suite_add_tcase(s, tc_logic);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = spec_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
