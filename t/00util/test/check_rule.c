#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "rule.h"

START_TEST(test_rule_create_and_validate_safe)
{
    s_datalog_program *prog = new_datalog_program();
    ck_assert(prog);

    // Rule: ?x <path> ?y :- ?x <edge> ?y .
    s_spec_fact head = {"?x", "path", "?y", NULL};
    s_spec_fact body[] = {{"?x", "edge", "?y", NULL}};

    s_datalog_rule *rule = datalog_program_add_rule(prog, &head, body, 1);
    ck_assert(rule);
    ck_assert_int_eq(1, datalog_rule_validate(rule));

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_validate_unsafe_head)
{
    s_datalog_program *prog = new_datalog_program();

    // Rule: ?x <path> ?y :- ?x <edge> ?z . (unsafe: ?y not in body)
    s_spec_fact head = {"?x", "path", "?y", NULL};
    s_spec_fact body[] = {{"?x", "edge", "?z", NULL}};

    s_datalog_rule *rule = datalog_program_add_rule(prog, &head, body, 1);
    ck_assert(rule);
    ck_assert_int_eq(0, datalog_rule_validate(rule));

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_validate_unsafe_negated)
{
    s_datalog_program *prog = new_datalog_program();

    // Rule: ?x <unreachable> ?y :- ?x <node> ?z, NOT( ?z <edge> ?y ) . (unsafe: ?y not in positive body subgoals)
    s_spec_fact head = {"?x", "unreachable", "?y", NULL};
    s_spec_fact body[] = {{"?x", "node", "?z", NULL}, {"?z", "edge", "?y", ":not"}};

    s_datalog_rule *rule = datalog_program_add_rule(prog, &head, body, 2);
    ck_assert(rule);
    ck_assert_int_eq(0, datalog_rule_validate(rule));

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_validate_safe_negated)
{
    s_datalog_program *prog = new_datalog_program();

    // Rule: ?x <unreachable> ?y :- ?x <node> ?z, ?z <node> ?y, NOT( ?z <edge> ?y ) . (safe: ?y is in positive body subgoal "?z
    // <node> ?y")
    s_spec_fact head = {"?x", "unreachable", "?y", NULL};
    s_spec_fact body[] = {{"?x", "node", "?z", NULL}, {"?z", "node", "?y", NULL}, {"?z", "edge", "?y", ":not"}};

    s_datalog_rule *rule = datalog_program_add_rule(prog, &head, body, 3);
    ck_assert(rule);
    ck_assert_int_eq(1, datalog_rule_validate(rule));

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_parse_success)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <path> ?y :- ?x <edge> ?y .\n"
                            "?x <path> ?y :- ?x <edge> ?z , ?z <path> ?y .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);
    ck_assert_int_eq(2, prog->rule_count);

    ck_assert_str_eq("?x", prog->rules[0].head.s);
    ck_assert_str_eq("path", prog->rules[0].head.p);
    ck_assert_str_eq("?y", prog->rules[0].head.o);
    ck_assert_int_eq(1, prog->rules[0].body_count);

    ck_assert_str_eq("?x", prog->rules[1].head.s);
    ck_assert_str_eq("path", prog->rules[1].head.p);
    ck_assert_str_eq("?y", prog->rules[1].head.o);
    ck_assert_int_eq(2, prog->rules[1].body_count);

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_parse_negated_success)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <unreachable> ?y :- ?x <node> ?z , ?z <node> ?y , NOT( ?z <edge> ?y ) .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);
    ck_assert_int_eq(1, prog->rule_count);
    ck_assert_int_eq(3, prog->rules[0].body_count);
    ck_assert_str_eq(":not", prog->rules[0].body[2].negated);

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_parse_failure)
{
    s_datalog_program *prog = new_datalog_program();

    // 1. Syntax error (missing head object)
    ck_assert_int_eq(-1, datalog_program_parse_rules(prog, "?x <path> :- ?x <edge> ?y ."));

    // 2. Safety error (unsafe rule head variable)
    ck_assert_int_eq(-1, datalog_program_parse_rules(prog, "?x <path> ?y :- ?x <edge> ?z ."));

    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_stratification_success)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <path> ?y :- ?x <edge> ?y .\n"
                            "?x <path> ?y :- ?x <edge> ?z , ?z <path> ?y .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    ck_assert(rule_strata);
    ck_assert_int_eq(1, num_strata);
    ck_assert_int_eq(0, rule_strata[0]);
    ck_assert_int_eq(0, rule_strata[1]);

    free(rule_strata);
    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_stratification_negation_success)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <p> ?y :- ?x <q> ?y .\n"
                            "?x <q> ?y :- ?x <r> ?y , NOT( ?x <s> ?y ) .\n"
                            "?x <s> ?y :- ?x <t> ?y .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    ck_assert(rule_strata);
    ck_assert_int_eq(2, num_strata);
    ck_assert_int_eq(0, rule_strata[2]); // s :- t is stratum 0
    ck_assert_int_eq(1, rule_strata[1]); // q :- r, NOT s is stratum 1
    ck_assert_int_eq(1, rule_strata[0]); // p :- q is stratum 1

    free(rule_strata);
    delete_datalog_program(prog);
}
END_TEST

START_TEST(test_rule_stratification_failure)
{
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <p> ?y :- ?x <q> ?y .\n"
                            "?x <q> ?y :- ?x <r> ?y , NOT( ?x <p> ?y ) .";

    int ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, ret);

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    ck_assert(rule_strata == NULL);
    ck_assert_int_eq(0, num_strata);

    delete_datalog_program(prog);
}
END_TEST

Suite *rule_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Rule");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_rule_create_and_validate_safe);
    tcase_add_test(tc_core, test_rule_validate_unsafe_head);
    tcase_add_test(tc_core, test_rule_validate_unsafe_negated);
    tcase_add_test(tc_core, test_rule_validate_safe_negated);
    tcase_add_test(tc_core, test_rule_parse_success);
    tcase_add_test(tc_core, test_rule_parse_negated_success);
    tcase_add_test(tc_core, test_rule_parse_failure);
    tcase_add_test(tc_core, test_rule_stratification_success);
    tcase_add_test(tc_core, test_rule_stratification_negation_success);
    tcase_add_test(tc_core, test_rule_stratification_failure);

    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = rule_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
