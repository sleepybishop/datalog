#include "facts.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>

int main()
{
    s_intern *sym = new_intern(10000);
    s_facts *db = new_facts(sym, 10000);
    db->prog = new_datalog_program();

    const char *rules = "?X grandparent ?Z :- ?X parent ?Y, ?Y parent ?Z .\n"
                        "?X ancestor ?Y :- ?X parent ?Y .\n"
                        "?X ancestor ?Z :- ?X parent ?Y, ?Y ancestor ?Z .\n";

    if (datalog_program_parse_rules(db->prog, rules) < 0) {
        printf("FAILED TO PARSE RULES\n");
    }
    datalog_program_print(db->prog, stdout);

    facts_transaction_begin(db);
    facts_add_spo(db, "Alice", "parent", "Bob");
    facts_add_spo(db, "Bob", "parent", "Charlie");
    facts_transaction_commit(db);

    printf("Database count after insert: %lu (Expected: 6)\n", facts_count(db));

    s_set_cursor sc;
    set_cursor_init(&db->index, &sc);
    s_set_item *si;
    while ((si = set_cursor_next(&sc))) {
        s_fact *f = (s_fact *)si->data;
        printf("FACT: %s %s %s (proof=%zu)\n", symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o), f->proof_count);
    }

    s_fact *f = facts_get_spo(db, "Alice", "grandparent", "Charlie");
    if (f)
        printf("SUCCESS: Found Alice grandparent Charlie\n");
    else
        printf("FAILURE: Alice grandparent Charlie missing\n");

    f = facts_get_spo(db, "Alice", "ancestor", "Charlie");
    if (f)
        printf("SUCCESS: Found Alice ancestor Charlie\n");
    else
        printf("FAILURE: Alice ancestor Charlie missing\n");

    printf("\n--- RETRACTING Alice parent Bob ---\n");
    facts_transaction_begin(db);
    facts_remove_spo(db, "Alice", "parent", "Bob");
    facts_transaction_commit(db);

    printf("Database count after retract: %lu\n", facts_count(db));

    f = facts_get_spo(db, "Alice", "grandparent", "Charlie");
    if (!f)
        printf("SUCCESS: Alice grandparent Charlie retracted\n");
    else
        printf("FAILURE: Alice grandparent Charlie still exists\n");

    f = facts_get_spo(db, "Alice", "ancestor", "Charlie");
    if (!f)
        printf("SUCCESS: Alice ancestor Charlie retracted\n");
    else
        printf("FAILURE: Alice ancestor Charlie still exists\n");

    delete_datalog_program(db->prog);
    delete_facts(db);
    delete_intern(sym);
    return 0;
}
