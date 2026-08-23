#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "magic.h"
#include "rule.h"

START_TEST(test_magic_sets_transformation)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <path> ?y :- ?x <edge> ?y .\n"
                            "?x <path> ?y :- ?x <edge> ?z , ?z <path> ?y .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);

    /* Query: alice <path> ?y (adornment bf) */
    s_spec_fact query_goal = {"alice", "path", "?y", NULL};

    s_datalog_program *magic_prog = datalog_program_magic_transform(prog, &query_goal);
    ck_assert(magic_prog);

    /* We expect:
     * 1. magic_path_bf(?z) :- magic_path_bf(?x), ?x <edge> ?z . (magic rule)
     * 2. ?x <path_bf> ?y :- ?x <magic_path_bf> "true", ?x <edge> ?y . (rewritten rule 1)
     * 3. ?x <path_bf> ?y :- ?x <magic_path_bf> "true", ?x <edge> ?z, ?z <path_bf> ?y . (rewritten rule 2)
     * So we expect exactly 3 rules.
     */
    ck_assert_int_eq(3, magic_prog->rule_count);

    /* Check validation of rewritten program */
    for (size_t i = 0; i < magic_prog->rule_count; i++) {
        ck_assert_int_eq(1, datalog_rule_validate(&magic_prog->rules[i]));
    }

    delete_datalog_program(magic_prog);
    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_magic_sets_rejects_invalid_goal)
{
    s_datalog_program *prog = new_datalog_program();
    ck_assert(prog);
    s_spec_fact variable_predicate = {"alice", "?predicate", "?object", NULL};
    s_spec_fact missing_subject = {NULL, "path", "?object", NULL};
    ck_assert_ptr_eq(datalog_program_magic_transform(prog, &variable_predicate), NULL);
    ck_assert_ptr_eq(datalog_program_magic_transform(prog, &missing_subject), NULL);
    ck_assert_ptr_eq(datalog_program_magic_transform(NULL, &variable_predicate), NULL);
    delete_datalog_program(prog);
}
END_TEST

Suite *magic_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Magic");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_magic_sets_transformation);
    tcase_add_test(tc_core, test_magic_sets_rejects_invalid_goal);

    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = magic_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
