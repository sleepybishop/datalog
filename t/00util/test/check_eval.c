#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"
#include "facts.h"

START_TEST(test_eval_transitive_closure)
{
    s_intern *sym = new_intern(1000);
    s_facts *facts = new_facts(sym, 1000);

    /* 1. Add base facts (graph edges) */
    facts_add_spo(facts, "a", "edge", "b");
    facts_add_spo(facts, "b", "edge", "c");
    facts_add_spo(facts, "c", "edge", "d");

    /* 2. Create rules program */
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <path> ?y :- ?x <edge> ?y .\n"
                            "?x <path> ?y :- ?x <edge> ?z , ?z <path> ?y .";
    int parse_ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, parse_ret);

    /* 3. Run evaluation */
    int derived = facts_datalog_eval(facts, prog);
    /* Derived paths should be:
     * a->b, b->c, c->d (3 direct paths)
     * a->c, b->d       (2 paths of length 2)
     * a->d             (1 path of length 3)
     * Total: 6 path facts.
     */
    ck_assert_int_eq(6, derived);

    /* 4. Verify derived facts exist in the database */
    ck_assert(facts_get_spo(facts, "a", "path", "b"));
    ck_assert(facts_get_spo(facts, "b", "path", "c"));
    ck_assert(facts_get_spo(facts, "c", "path", "d"));
    ck_assert(facts_get_spo(facts, "a", "path", "c"));
    ck_assert(facts_get_spo(facts, "b", "path", "d"));
    ck_assert(facts_get_spo(facts, "a", "path", "d"));

    /* Verify non-existent paths */
    ck_assert(!facts_get_spo(facts, "d", "path", "a"));

    delete_datalog_program(prog);
    delete_facts(facts);
    delete_intern(sym);
}
END_TEST

START_TEST(test_eval_stratified_negation)
{
    s_intern *sym = new_intern(1000);
    s_facts *facts = new_facts(sym, 1000);

    /* 1. Add base facts */
    facts_add_spo(facts, "a", "node", "true");
    facts_add_spo(facts, "b", "node", "true");
    facts_add_spo(facts, "c", "node", "true");
    facts_add_spo(facts, "a", "edge", "b");

    /* 2. Create rules program */
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <unreachable> ?y :- ?x <node> ?s , ?y <node> ?t , NOT( ?x <edge> ?y ) .";
    int parse_ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, parse_ret);

    /* 3. Run evaluation */
    int derived = facts_datalog_eval(facts, prog);
    /* There are 3 nodes (a, b, c). Total possible pairs is 3 * 3 = 9.
     * The only edge is a->b.
     * So 9 - 1 (a->b is reachable) = 8 unreachable pairs.
     * Wait, are identity pairs (a->a, b->b, c->c) unreachable? Yes, since there are no self loops.
     * So we expect 8 derived unreachable facts.
     */
    ck_assert_int_eq(8, derived);

    /* 4. Verify derived facts */
    ck_assert(facts_get_spo(facts, "a", "unreachable", "c"));
    ck_assert(facts_get_spo(facts, "b", "unreachable", "c"));
    ck_assert(facts_get_spo(facts, "c", "unreachable", "a"));
    ck_assert(facts_get_spo(facts, "c", "unreachable", "b"));
    ck_assert(facts_get_spo(facts, "a", "unreachable", "a")); // self loop missing

    /* Verify a->b is NOT unreachable */
    ck_assert(!facts_get_spo(facts, "a", "unreachable", "b"));

    delete_datalog_program(prog);
    delete_facts(facts);
    delete_intern(sym);
}
END_TEST

START_TEST(test_eval_same_generation_cousins)
{
    s_intern *sym = new_intern(1000);
    s_facts *facts = new_facts(sym, 1000);

    /* 1. Add base facts */
    facts_add_spo(facts, "alice", "parent", "bob");
    facts_add_spo(facts, "alice", "parent", "charlie");
    facts_add_spo(facts, "bob", "parent", "dave");
    facts_add_spo(facts, "charlie", "parent", "eve");

    /* 2. Create rules program */
    s_datalog_program *prog = new_datalog_program();
    const char *rules_str = "?x <sgc> ?y :- ?p <parent> ?x , ?p <parent> ?y .\n"
                            "?x <sgc> ?y :- ?a <parent> ?x , ?b <parent> ?y , ?a <sgc> ?b .";
    int parse_ret = datalog_program_parse_rules(prog, rules_str);
    ck_assert_int_eq(0, parse_ret);

    /* 3. Run evaluation */
    int derived = facts_datalog_eval(facts, prog);
    ck_assert_int_eq(8, derived);

    /* 4. Verify derived facts */
    ck_assert(facts_get_spo(facts, "dave", "sgc", "eve"));
    ck_assert(facts_get_spo(facts, "eve", "sgc", "dave"));
    ck_assert(facts_get_spo(facts, "bob", "sgc", "charlie"));

    delete_datalog_program(prog);
    delete_facts(facts);
    delete_intern(sym);
}
END_TEST

Suite *eval_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Eval");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_eval_transitive_closure);
    tcase_add_test(tc_core, test_eval_stratified_negation);
    tcase_add_test(tc_core, test_eval_same_generation_cousins);

    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = eval_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
