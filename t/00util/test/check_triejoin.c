#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "facts.h"
#include "lftj.h"

START_TEST(test_facts_spec_sort)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    /* populate database with facts to establish symbol usage counts */
    facts_add_spo(facts, "alice", "friend", "bob");
    facts_add_spo(facts, "alice", "friend", "charlie");
    facts_add_spo(facts, "bob", "friend", "charlie");
    facts_add_spo(facts, "bob", "friend", "charlie2");
    facts_add_spo(facts, "bob", "friend", "charlie3");

    /* "friend" has usage = 5, "alice" has usage = 2, "bob" has usage = 4 */

    /* query: (?a, "friend", ?b), ("alice", "friend", ?b); constant clause sorts first */
    p_spec spec = malloc(2 * 4 * sizeof(char *) + 2 * sizeof(char *));
    spec[0] = "?a";
    spec[1] = "friend";
    spec[2] = "?b";
    spec[3] = NULL;
    spec[4] = "alice";
    spec[5] = "friend";
    spec[6] = "?b";
    spec[7] = NULL;
    spec[8] = NULL;
    spec[9] = NULL;

    facts_spec_sort(facts, spec, 2);

    /* assert that the selective constant-based clause was sorted first */
    ck_assert_str_eq(spec[0], "alice");
    ck_assert_str_eq(spec[4], "?a");

    free(spec);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_more_than_32_subgoals)
{
    const size_t subgoal_count = 33;
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "alice", "friend", "bob") != NULL);

    p_spec spec = calloc(subgoal_count * 4 + 2, sizeof(*spec));
    ck_assert(spec != NULL);
    for (size_t i = 0; i < subgoal_count; i++) {
        spec[i * 4] = "?s";
        spec[i * 4 + 1] = "friend";
        spec[i * 4 + 2] = "?o";
    }
    s_binding *bindings = spec_bindings(spec);
    ck_assert(bindings != NULL);
    ck_assert_int_eq(1, facts_lftj_solve(facts, spec, bindings));

    free(bindings);
    free(spec);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_iterator_basic)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    facts_add_spo(facts, "s1", "p1", "o1");
    facts_add_spo(facts, "s1", "p1", "o2");
    facts_add_spo(facts, "s1", "p2", "o3");
    facts_add_spo(facts, "s2", "p1", "o4");

    s_lftj_iterator *it = new_lftj_iterator(facts->hexastore->trie_spo, 0, 1, 2);
    ck_assert(it != NULL);

    /* open first level */
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "s1");

    /* open second level */
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p1");

    /* open third level */
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o1");

    /* next at depth 3 */
    ck_assert_int_eq(iterator_next(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o2");

    /* next again (no more elements for s1 p1) */
    ck_assert_int_eq(iterator_next(it), -1);

    /* up and verify parent key survived deeper EOF */
    ck_assert_int_eq(iterator_up(it), 0);
    ck_assert(iterator_key(it) != NULL);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p1");
    ck_assert_int_eq(iterator_next(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p2");

    /* open level 3 under p2 */
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o3");

    delete_lftj_iterator(it);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_solve)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    facts_add_spo(facts, "alice", "friend", "bob");
    facts_add_spo(facts, "bob", "friend", "charlie");
    facts_add_spo(facts, "charlie", "friend", "daniel");

    /* join query: (?a, "friend", ?b), (?b, "friend", ?c) -> 2 chains */
    p_spec spec = malloc(2 * 4 * sizeof(char *) + 2 * sizeof(char *));
    spec[0] = "?a";
    spec[1] = "friend";
    spec[2] = "?b";
    spec[3] = NULL;
    spec[4] = "?b";
    spec[5] = "friend";
    spec[6] = "?c";
    spec[7] = NULL;
    spec[8] = NULL;
    spec[9] = NULL;

    s_binding *bindings = spec_bindings(spec);
    ck_assert(bindings != NULL);

    int solutions = facts_lftj_solve(facts, spec, bindings);
    ck_assert_int_eq(solutions, 2);

    free(bindings);
    free(spec);
    delete_facts(facts);
}
END_TEST

START_TEST(test_hexastore_compaction)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    facts_add_spo(facts, "s1", "p1", "o1");
    facts_add_spo(facts, "s1", "p1", "o2");
    facts_add_spo(facts, "s1", "p2", "o3");
    facts_add_spo(facts, "s2", "p1", "o4");

    hexastore_compact(facts->hexastore);
    ck_assert(facts_get_spo(facts, "s2", "p1", "o4") != NULL);

    s_lftj_iterator *it = new_lftj_iterator(facts->hexastore->trie_spo, 0, 1, 2);
    ck_assert(it != NULL);

    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "s1");

    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p1");

    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o1");

    ck_assert_int_eq(iterator_next(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o2");

    delete_lftj_iterator(it);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_solve_repeated_vars_and_trailing_constants)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    facts_add_spo(facts, "a", "edge", "a");
    facts_add_spo(facts, "a", "edge", "b");
    facts_add_spo(facts, "b", "edge", "b");

    /* subgoal with repeated variable (?x, "edge", ?x) */
    const char *x = NULL;
    s_binding b1[] = {{"?x", &x}, {NULL, NULL}};
    p_spec s1 = spec_expand((p_spec)(const char *[]){"?x", "edge", "?x", NULL, NULL});
    ck_assert_int_eq(facts_lftj_solve(facts, s1, b1), 2);
    free(s1);

    /* subgoal with all constants */
    s_binding b2[] = {{NULL, NULL}};
    p_spec s2 = spec_expand((p_spec)(const char *[]){"a", "edge", "b", NULL, NULL});
    ck_assert_int_eq(facts_lftj_solve(facts, s2, b2), 1);
    free(s2);

    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_failed_bound_seek_restores_depth)
{
    s_facts *facts = new_facts(NULL, 100);
    facts_add_spo(facts, "a", "edge", "b");
    facts_add_spo(facts, "c", "edge", "a");
    facts_add_spo(facts, "d", "edge", "d");
    const char *spec[] = {"?x", "edge", "?x", NULL, NULL};
    s_binding *bindings = spec_bindings(spec);
    ck_assert_int_eq(1, facts_lftj_solve(facts, spec, bindings));
    ck_assert_int_eq(1, facts_lftj_solve_multi(facts, NULL, spec, bindings, NULL, NULL));
    free(bindings);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_ignores_transactionally_removed_facts)
{
    s_facts *facts = new_facts(NULL, 100);
    facts_add_spo(facts, "a", "edge", "a");
    facts_add_spo(facts, "b", "edge", "b");
    const char *spec[] = {"?x", "edge", "?x", NULL, NULL};
    s_binding *bindings = spec_bindings(spec);
    ck_assert_int_eq(0, facts_transaction_begin(facts));
    ck_assert_int_eq(1, facts_remove_spo(facts, "a", "edge", "a"));
    ck_assert_int_eq(1, facts_lftj_solve(facts, spec, bindings));
    ck_assert_int_eq(1, facts_lftj_solve_multi(facts, NULL, spec, bindings, NULL, NULL));
    ck_assert_int_eq(0, facts_transaction_rollback(facts));
    ck_assert_int_eq(2, facts_lftj_solve(facts, spec, bindings));
    free(bindings);
    delete_facts(facts);
}
END_TEST

START_TEST(test_lftj_negation_preserves_repeated_variable_equality)
{
    const char *patterns[][3] = {
        {"?x", "edge", "?x"},
        {"?x", "?x", "target"},
        {"source", "?x", "?x"},
        {"?x", "?x", "?x"},
    };
    const char *nonmatches[][3] = {
        {"a", "edge", "b"},
        {"a", "b", "target"},
        {"source", "a", "b"},
        {"a", "a", "b"},
    };
    const char *matches[][3] = {
        {"a", "edge", "a"},
        {"a", "a", "target"},
        {"source", "a", "a"},
        {"a", "a", "a"},
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        s_facts *facts = new_facts(NULL, 100);
        ck_assert(facts != NULL);
        ck_assert(facts_add_spo(facts, nonmatches[i][0], nonmatches[i][1], nonmatches[i][2]) != NULL);
        const char *spec[] = {":not", patterns[i][0], patterns[i][1], patterns[i][2], NULL, NULL};
        const char *x = NULL;
        s_binding bindings[] = {{"?x", &x}, {NULL, NULL}};
        ck_assert_int_eq(1, facts_lftj_solve(facts, spec, bindings));
        ck_assert(x == NULL);
        ck_assert(facts_add_spo(facts, matches[i][0], matches[i][1], matches[i][2]) != NULL);
        ck_assert_int_eq(0, facts_lftj_solve(facts, spec, bindings));
        ck_assert(x == NULL);
        delete_facts(facts);
    }
}
END_TEST

START_TEST(test_lftj_compact_spec_and_multi_db)
{
    s_intern *sym = new_intern(100);
    s_facts *db = new_facts(sym, 100);
    s_facts *delta = new_facts(sym, 100);

    facts_add_spo(db, "alice", "tag", "x");
    facts_add_spo(db, "alice", "rel", "bob");
    facts_add_spo(db, "bob", "friend", "charlie");
    facts_add_spo(delta, "bob", "step", "daniel");

    const char *compact_spec[] = {"?a", "tag", "x", "rel", "?b", NULL, "?b", "friend", "?c", NULL, NULL};
    s_binding *b1 = spec_bindings(compact_spec);
    ck_assert(b1 != NULL);
    ck_assert_int_eq(1, facts_lftj_solve(db, compact_spec, b1));
    ck_assert_int_eq(1, facts_lftj_solve_multi(db, NULL, compact_spec, b1, NULL, NULL));
    free(b1);

    const char *multi_spec[] = {"?a", "tag", "x", "rel", "?b", NULL, "?b", "step", "?d", NULL, NULL};
    s_facts *sources[] = {db, db, delta};
    s_binding *b2 = spec_bindings(multi_spec);
    ck_assert(b2 != NULL);
    ck_assert_int_eq(1, facts_lftj_solve_multi(db, sources, multi_spec, b2, NULL, NULL));
    free(b2);

    delete_facts(delta);
    delete_facts(db);
    delete_intern(sym);
}
END_TEST

Suite *triejoin_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Triejoin");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_facts_spec_sort);
    tcase_add_test(tc_core, test_lftj_iterator_basic);
    tcase_add_test(tc_core, test_lftj_solve);
    tcase_add_test(tc_core, test_lftj_more_than_32_subgoals);
    tcase_add_test(tc_core, test_hexastore_compaction);
    tcase_add_test(tc_core, test_lftj_solve_repeated_vars_and_trailing_constants);
    tcase_add_test(tc_core, test_lftj_failed_bound_seek_restores_depth);
    tcase_add_test(tc_core, test_lftj_ignores_transactionally_removed_facts);
    tcase_add_test(tc_core, test_lftj_negation_preserves_repeated_variable_equality);
    tcase_add_test(tc_core, test_lftj_compact_spec_and_multi_db);
    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = triejoin_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
