#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "facts.h"
#include "lftj.h"

START_TEST(test_facts_spec_sort)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);

    // Populate database with facts to establish symbol usage counts
    facts_add_spo(facts, "alice", "friend", "bob");
    facts_add_spo(facts, "alice", "friend", "charlie");
    facts_add_spo(facts, "bob", "friend", "charlie");
    facts_add_spo(facts, "bob", "friend", "charlie2");
    facts_add_spo(facts, "bob", "friend", "charlie3");

    // "friend" has usage = 5, "alice" has usage = 2, "bob" has usage = 4

    // We create a spec that represents the query: (?a, "friend", ?b), ("alice", "friend", ?b)
    // In our cost-based selectivity sorting:
    // - ("alice", "friend", ?b) has a constant and should be sorted first.
    // - (?a, "friend", ?b) has two variables and should be sorted second.
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

    // Assert that the selective constant-based clause was sorted first
    ck_assert_str_eq(spec[0], "alice");
    ck_assert_str_eq(spec[4], "?a");

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

    // Open first level
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "s1");

    // Open second level
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p1");

    // Open third level
    ck_assert_int_eq(iterator_open(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o1");

    // Next at depth 3
    ck_assert_int_eq(iterator_next(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "o2");

    // Next again (no more elements for s1 p1)
    ck_assert_int_eq(iterator_next(it), -1);

    // Up and next
    ck_assert_int_eq(iterator_up(it), 0); // back to level 2 (p1)
    ck_assert_int_eq(iterator_next(it), 0);
    ck_assert_str_eq(symbol_to_str(iterator_key(it)), "p2");

    // Open level 3 under p2
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

    // Join query: (?a, "friend", ?b), (?b, "friend", ?c)
    // This should find two transitive friend chains:
    // 1. alice -> bob -> charlie
    // 2. bob -> charlie -> daniel
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

Suite *triejoin_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Triejoin");
    tc_core = tcase_create("Core");
    tcase_add_test(tc_core, test_facts_spec_sort);
    tcase_add_test(tc_core, test_lftj_iterator_basic);
    tcase_add_test(tc_core, test_lftj_solve);
    tcase_add_test(tc_core, test_hexastore_compaction);
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
