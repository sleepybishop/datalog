#include <check.h>
#include <stdlib.h>
#include <string.h>
#include "sparql.h"

s_facts *g_f;
s_intern *g_sym;

void setup(void)
{
    g_sym = new_intern(100);
    g_f = new_facts(g_sym, 100);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "a", "b", "d");
    facts_add_spo(g_f, "g", "b", "c");
    facts_add_spo(g_f, "h", "i", "c");
    facts_add_spo(g_f, "a", "e", "d");
}

void teardown(void)
{
    delete_facts(g_f);
    delete_intern(g_sym);
}

START_TEST(test_sparql_parse_error)
{
    s_facts_with_cursor c;
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};

    ck_assert_int_eq(facts_sparql(g_f, bindings, &c, "SELECT WHERE { ?s <b> <c> . }"), -1);
    ck_assert_int_eq(facts_sparql(g_f, bindings, &c, "SELECT ?s WHERE { ?s <b> . }"), -1);
    ck_assert_int_eq(facts_sparql(g_f, bindings, &c, "SELECT ?s WHERE ?s <b> <c> . }"), -1);
    ck_assert_int_eq(facts_sparql(g_f, bindings, &c, "SELECT ?s WHERE { ?s <b> <c>"), -1);
}
END_TEST

START_TEST(test_sparql_insert_is_atomic)
{
    int rc = facts_sparql_insert(g_f, "INSERT DATA { <atomic-s> <atomic-p> <atomic-o> . <incomplete> }");
    ck_assert_int_eq(-1, rc);
    ck_assert(facts_get_spo(g_f, "atomic-s", "atomic-p", "atomic-o") == NULL);
}
END_TEST

START_TEST(test_sparql_single_triple)
{
    s_facts_with_cursor c;
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};

    int rc = facts_sparql(g_f, bindings, &c, "SELECT ?s WHERE { ?s <b> <c> . }");
    ck_assert_int_eq(rc, 0);

    int count = 0;
    while (facts_with_cursor_next(&c)) {
        ck_assert(strcmp(s, "a") == 0 || strcmp(s, "g") == 0);
        count++;
    }
    ck_assert_int_eq(count, 2);
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_multiple_triples)
{
    s_facts_with_cursor c;
    const char *s;
    const char *o;
    s_binding bindings[] = {{"?s", &s}, {"?o", &o}, {NULL, NULL}};

    int rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . ?s <e> <d> . }");
    ck_assert_int_eq(rc, 0);

    int count = 0;
    while (facts_with_cursor_next(&c)) {
        ck_assert_str_eq(s, "a");
        ck_assert(strcmp(o, "c") == 0 || strcmp(o, "d") == 0);
        count++;
    }
    ck_assert_int_eq(count, 2);
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_negation)
{
    s_facts_with_cursor c;
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};

    int rc = facts_sparql(g_f, bindings, &c, "SELECT ?s WHERE { ?s <b> <c> . NOT ?s <e> <d> . }");
    ck_assert_int_eq(rc, 0);

    int count = 0;
    while (facts_with_cursor_next(&c)) {
        ck_assert_str_eq(s, "g");
        count++;
    }
    ck_assert_int_eq(count, 1);
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_clue_murder_mystery)
{
    s_intern *sym = new_intern(100);
    s_facts *facts = new_facts(sym, 100);
    ck_assert(facts != NULL);

    // Setup the Clue mystery facts
    facts_add_spo(facts, "mustard", "location", "library");
    facts_add_spo(facts, "plum", "location", "study");
    facts_add_spo(facts, "scarlett", "location", "kitchen");
    facts_add_spo(facts, "victim", "found_in", "library");
    facts_add_spo(facts, "dagger", "found_in", "library");
    facts_add_spo(facts, "mustard", "owns", "dagger");
    facts_add_spo(facts, "plum", "owns", "candlestick");
    facts_add_spo(facts, "scarlett", "owns", "lead_pipe");

    // SPARQL query to solve the mystery:
    // Find the suspect who was in the same room where the victim was found,
    // where the weapon was also found, and the suspect owns/had access to that weapon.
    const char *suspect;
    const char *weapon;
    const char *room;
    s_binding bindings[] = {{"?suspect", &suspect}, {"?weapon", &weapon}, {"?room", &room}, {NULL, NULL}};
    s_facts_with_cursor cur;

    int rc = facts_sparql(facts, bindings, &cur,
                          "SELECT ?suspect ?weapon ?room WHERE { "
                          "  ?suspect <location> ?room . "
                          "  <victim> <found_in> ?room . "
                          "  ?weapon <found_in> ?room . "
                          "  ?suspect <owns> ?weapon . "
                          "}");

    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert_str_eq(suspect, "mustard");
    ck_assert_str_eq(weapon, "dagger");
    ck_assert_str_eq(room, "library");
    ck_assert(!facts_with_cursor_next(&cur));

    facts_with_cursor_destroy(&cur);
    delete_facts(facts);
    delete_intern(sym);
}
END_TEST

START_TEST(test_sparql_prefix)
{
    s_facts_with_cursor c;
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};

    facts_add_spo(g_f, "http://example.org/alice", "http://xmlns.com/foaf/0.1/name", "Alice");
    facts_add_spo(g_f, "http://example.org/bob", "http://xmlns.com/foaf/0.1/name", "Bob");

    int rc = facts_sparql(g_f, bindings, &c,
                          "PREFIX ex: <http://example.org/> "
                          "PREFIX foaf: <http://xmlns.com/foaf/0.1/> "
                          "SELECT ?s WHERE { ?s foaf:name \"Alice\" . }");
    ck_assert_int_eq(rc, 0);

    int count = 0;
    while (facts_with_cursor_next(&c)) {
        ck_assert_str_eq(s, "http://example.org/alice");
        count++;
    }
    ck_assert_int_eq(count, 1);
    facts_with_cursor_destroy(&c);

    // Test with default/empty prefix
    rc = facts_sparql(g_f, bindings, &c,
                      "PREFIX : <http://example.org/> "
                      "PREFIX foaf: <http://xmlns.com/foaf/0.1/> "
                      "SELECT ?s WHERE { :alice foaf:name ?s . }");
    ck_assert_int_eq(rc, 0);

    count = 0;
    while (facts_with_cursor_next(&c)) {
        ck_assert_str_eq(s, "Alice");
        count++;
    }
    ck_assert_int_eq(count, 1);
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_limit_offset)
{
    s_facts_with_cursor c;
    const char *s;
    const char *o;
    s_binding bindings[] = {{"?s", &s}, {"?o", &o}, {NULL, NULL}};

    int rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } LIMIT 2");
    ck_assert_int_eq(rc, 0);
    int count = 0;
    while (facts_with_cursor_next(&c)) {
        count++;
    }
    ck_assert_int_eq(count, 2);
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } OFFSET 1");
    ck_assert_int_eq(rc, 0);
    count = 0;
    while (facts_with_cursor_next(&c)) {
        count++;
    }
    ck_assert_int_eq(count, 2);
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } LIMIT 1 OFFSET 1");
    ck_assert_int_eq(rc, 0);
    count = 0;
    while (facts_with_cursor_next(&c)) {
        count++;
    }
    ck_assert_int_eq(count, 1);
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_ask)
{
    s_facts_with_cursor c;
    s_binding *bindings = NULL;

    int is_ask = sparql_query_is_ask(g_f, "ASK WHERE { ?s <b> <c> . }");
    ck_assert_int_eq(is_ask, 1);

    int rc = facts_sparql_eval(g_f, "ASK WHERE { ?s <b> <c> . }", &c, &bindings);
    ck_assert_int_eq(rc, 0);
    ck_assert(facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);

    rc = facts_sparql_eval(g_f, "ASK WHERE { ?s <b> <nonexistent> . }", &c, &bindings);
    ck_assert_int_eq(rc, 0);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);

    is_ask = sparql_query_is_ask(g_f, "SELECT ?s WHERE { ?s <b> <c> . }");
    ck_assert_int_eq(is_ask, 0);
}
END_TEST

START_TEST(test_sparql_orderby)
{
    s_facts_with_cursor c;
    const char *s;
    const char *o;
    s_binding bindings[] = {{"?s", &s}, {"?o", &o}, {NULL, NULL}};

    int rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } ORDER BY ?s");
    ck_assert_int_eq(rc, 0);

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "a");

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "a");

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "g");

    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } ORDER BY DESC(?s)");
    ck_assert_int_eq(rc, 0);

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "g");

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "a");

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "a");

    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, bindings, &c, "SELECT ?s ?o WHERE { ?s <b> ?o . } ORDER BY ?s LIMIT 1 OFFSET 2");
    ck_assert_int_eq(rc, 0);

    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(s, "g");

    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_rule_engine)
{
    s_facts_with_cursor c;
    const char *rule;
    const char *action;
    const char *src;
    const char *proto;
    const char *port;
    s_binding bindings[] = {{"?rule", &rule},   {"?action", &action}, {"?src", &src},
                            {"?proto", &proto}, {"?port", &port},     {NULL, NULL}};

    // 1. Setup Rule database facts
    facts_add_spo(g_f, "rule1", "source", "ANY");
    facts_add_spo(g_f, "rule1", "proto", "TCP");
    facts_add_spo(g_f, "rule1", "port", "80");
    facts_add_spo(g_f, "rule1", "action", "ALLOW");

    facts_add_spo(g_f, "rule2", "source", "untrusted");
    facts_add_spo(g_f, "rule2", "proto", "ANY");
    facts_add_spo(g_f, "rule2", "port", "22");
    facts_add_spo(g_f, "rule2", "action", "BLOCK");

    facts_add_spo(g_f, "rule3", "source", "malicious");
    facts_add_spo(g_f, "rule3", "proto", "ANY");
    facts_add_spo(g_f, "rule3", "port", "ANY");
    facts_add_spo(g_f, "rule3", "action", "DROP");

    // 2. Setup incoming Packet properties and their matching capabilities
    // Packet 1: source="untrusted", proto="TCP", port="22"
    facts_add_spo(g_f, "pkt1", "matches_source", "untrusted");
    facts_add_spo(g_f, "pkt1", "matches_source", "ANY");
    facts_add_spo(g_f, "pkt1", "matches_proto", "TCP");
    facts_add_spo(g_f, "pkt1", "matches_proto", "ANY");
    facts_add_spo(g_f, "pkt1", "matches_port", "22");
    facts_add_spo(g_f, "pkt1", "matches_port", "ANY");

    // Packet 2: source="malicious", proto="UDP", port="53"
    facts_add_spo(g_f, "pkt2", "matches_source", "malicious");
    facts_add_spo(g_f, "pkt2", "matches_source", "ANY");
    facts_add_spo(g_f, "pkt2", "matches_proto", "UDP");
    facts_add_spo(g_f, "pkt2", "matches_proto", "ANY");
    facts_add_spo(g_f, "pkt2", "matches_port", "53");
    facts_add_spo(g_f, "pkt2", "matches_port", "ANY");

    // 3. Query matching rules for Packet 1
    int rc = facts_sparql(g_f, bindings, &c,
                          "SELECT ?rule ?action WHERE { "
                          "  ?rule <action> ?action . "
                          "  ?rule <source> ?src . "
                          "  <pkt1> <matches_source> ?src . "
                          "  ?rule <proto> ?proto . "
                          "  <pkt1> <matches_proto> ?proto . "
                          "  ?rule <port> ?port . "
                          "  <pkt1> <matches_port> ?port . "
                          "}");
    ck_assert_int_eq(rc, 0);
    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(rule, "rule2");
    ck_assert_str_eq(action, "BLOCK");
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    /* 4. Query matching rules for Packet 2 */
    rc = facts_sparql(g_f, bindings, &c,
                      "SELECT ?rule ?action WHERE { "
                      "  ?rule <action> ?action . "
                      "  ?rule <source> ?src . "
                      "  <pkt2> <matches_source> ?src . "
                      "  ?rule <proto> ?proto . "
                      "  <pkt2> <matches_proto> ?proto . "
                      "  ?rule <port> ?port . "
                      "  <pkt2> <matches_port> ?port . "
                      "}");
    ck_assert_int_eq(rc, 0);
    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq(rule, "rule3");
    ck_assert_str_eq(action, "DROP");
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_sparql_does_not_leak_symbols_or_break_escaped_quotes)
{
    s_facts_with_cursor c;
    s_binding *bindings = NULL;

    int rc = facts_sparql_eval(g_f, "SELECT ?never_interned_var WHERE { ?never_interned_var <missing_pred> \"missing_obj\" . }", &c,
                               &bindings);
    ck_assert_int_eq(rc, 0);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);

    ck_assert(facts_find_symbol_str(g_f, "?never_interned_var") == NULL);
    ck_assert(facts_find_symbol_str(g_f, "missing_pred") == NULL);
    ck_assert(facts_find_symbol_str(g_f, "missing_obj") == NULL);

    rc = facts_sparql_insert(g_f, "INSERT DATA { <quote_node> <label> \"hello \\\"world\\\"\" . }");
    ck_assert_int_eq(rc, 1);
    ck_assert_int_eq(facts_contains_spo(g_f, "quote_node", "label", "hello \"world\""), 1);

    rc = facts_sparql_eval(g_f, "ASK WHERE { <quote_node> <label> \"hello \\\"world\\\"\" . }", &c, &bindings);
    ck_assert_int_eq(rc, 0);
    ck_assert(facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);
}
END_TEST

START_TEST(test_sparql_preserves_constant_term_kinds)
{
    ck_assert_int_eq(5,
                     facts_sparql_insert(g_f, "PREFIX ex: <http://example.org/> INSERT DATA { "
                                              "<literal> <label> \"ex:literal\" . <iri> <label> <ex:literal> . "
                                              "<question> <label> \"?s\" . <:not> <label> \"sentinel\" . <?s> <link> <end> . }"));
    ck_assert_int_eq(1, facts_contains_spo(g_f, "literal", "label", "ex:literal"));
    ck_assert_int_eq(1, facts_contains_spo(g_f, "iri", "label", "ex:literal"));
    ck_assert_int_eq(0, facts_contains_spo(g_f, "literal", "label", "http://example.org/literal"));

    s_facts_with_cursor c;
    s_binding *bindings = NULL;
    ck_assert_int_eq(
        0, facts_sparql_eval(g_f, "PREFIX ex: <http://example.org/> SELECT ?s WHERE { ?s <label> \"ex:literal\" }", &c, &bindings));
    int count = 0;
    while (facts_with_cursor_next(&c))
        count++;
    ck_assert_int_eq(2, count);
    facts_with_cursor_destroy(&c);
    free(bindings);

    ck_assert_int_eq(0, facts_sparql_eval(g_f, "SELECT ?s WHERE { ?s <label> \"?s\" }", &c, &bindings));
    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq("question", *bindings_get(bindings, "?s"));
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);

    ck_assert_int_eq(0, facts_sparql_eval(g_f, "SELECT ?s ?x WHERE { ?s <label> ?x . ?x <link> <end> }", &c, &bindings));
    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq("question", *bindings_get(bindings, "?s"));
    ck_assert_str_eq("?s", *bindings_get(bindings, "?x"));
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);

    ck_assert_int_eq(0, facts_sparql_eval(g_f, "ASK WHERE { <:not> <label> \"sentinel\" }", &c, &bindings));
    ck_assert(facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);
}
END_TEST

START_TEST(test_sparql_repeated_variables_require_equal_terms)
{
    facts_add_spo(g_f, "left", "edge", "right");
    facts_add_spo(g_f, "self", "edge", "self");
    s_facts_with_cursor c;
    s_binding *bindings = NULL;
    ck_assert_int_eq(0, facts_sparql_eval(g_f, "SELECT ?x WHERE { ?x <edge> ?x }", &c, &bindings));
    ck_assert(facts_with_cursor_next(&c));
    ck_assert_str_eq("self", *bindings_get(bindings, "?x"));
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);
    ck_assert_int_eq(0, facts_sparql_eval(g_f, "ASK WHERE { NOT ?x <b> ?x }", &c, &bindings));
    ck_assert(facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    free(bindings);
}
END_TEST

START_TEST(test_sparql_statement_classification)
{
    ck_assert_int_eq(SPARQL_STATEMENT_INSERT,
                     sparql_statement_kind("PREFIX ex: <http://example.org/> INSERT DATA { ex:s ex:p \"x:-y\" }"));
    ck_assert_int_eq(SPARQL_STATEMENT_INSERT,
                     sparql_statement_kind("PREFIX ex: <http://example.org/> INSERT DATA { ex:-subject ex:p ex:o }"));
    ck_assert_int_eq(SPARQL_STATEMENT_INSERT,
                     sparql_statement_kind("PREFIX : <http://example/> INSERT DATA { :-subject :p :o . :- :p :o }"));
    ck_assert_int_eq(SPARQL_STATEMENT_ASK,
                     sparql_statement_kind("PREFIX : <http://example/> ASK WHERE { :-subject :p :o . :- :p :o }"));
    ck_assert_int_eq(SPARQL_STATEMENT_SELECT, sparql_statement_kind("SELECT ?x WHERE { ?x <p> \"x:-y\" }"));
    ck_assert_int_eq(SPARQL_STATEMENT_ASK, sparql_statement_kind("ASK WHERE { <x:-y> <p> <o> }"));
    ck_assert_int_eq(SPARQL_STATEMENT_RULE, sparql_statement_kind("(?s result ?o) :- (?s source ?o)."));
    const char *keyword_rules[] = {
        "SELECT <derived> <yes> :- <a> <source> <yes> .",   "ASK <derived> <yes> :- <a> <source> <yes> .",
        "INSERT <derived> <yes> :- <a> <source> <yes> .",   "\"INSERT\" <derived> <yes> :- <a> <source> <yes> .",
        "<INSERT> <derived> <yes> :- <a> <source> <yes> .", "?x <derived> ?o:- ?x <source> ?o .",
    };
    for (size_t i = 0; i < sizeof(keyword_rules) / sizeof(keyword_rules[0]); i++)
        ck_assert_int_eq(SPARQL_STATEMENT_RULE, sparql_statement_kind(keyword_rules[i]));
    ck_assert_int_eq(SPARQL_STATEMENT_UNKNOWN, sparql_statement_kind("\"x:-y\""));
}
END_TEST

Suite *sparql_suite(void)
{
    Suite *s;
    TCase *tc_core;
    s = suite_create("Sparql");
    tc_core = tcase_create("Core");
    tcase_add_checked_fixture(tc_core, setup, teardown);
    tcase_add_test(tc_core, test_sparql_parse_error);
    tcase_add_test(tc_core, test_sparql_insert_is_atomic);
    tcase_add_test(tc_core, test_sparql_single_triple);
    tcase_add_test(tc_core, test_sparql_multiple_triples);
    tcase_add_test(tc_core, test_sparql_negation);
    tcase_add_test(tc_core, test_clue_murder_mystery);
    tcase_add_test(tc_core, test_sparql_prefix);
    tcase_add_test(tc_core, test_sparql_limit_offset);
    tcase_add_test(tc_core, test_sparql_ask);
    tcase_add_test(tc_core, test_sparql_orderby);
    tcase_add_test(tc_core, test_sparql_rule_engine);
    tcase_add_test(tc_core, test_sparql_does_not_leak_symbols_or_break_escaped_quotes);
    tcase_add_test(tc_core, test_sparql_preserves_constant_term_kinds);
    tcase_add_test(tc_core, test_sparql_repeated_variables_require_equal_terms);
    tcase_add_test(tc_core, test_sparql_statement_classification);
    suite_add_tcase(s, tc_core);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = sparql_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
