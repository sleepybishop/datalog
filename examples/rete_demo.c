#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "facts.h"
#include "eval.h"
#include "sparql.h"
#include "rule.h"

int main()
{
    s_intern *sym = new_intern(10000);
    s_facts *db = new_facts(sym, 10000);

    // 1. Setup the Rete environment by assigning a program
    db->prog = new_datalog_program();

    // 2. Add our rule: grandparent(?X, ?Y) :- parent(?X, ?Z), parent(?Z, ?Y).
    const char *rule_str = "?X grandparent ?Y :- ?X parent ?Z, ?Z parent ?Y .";
    datalog_program_parse_rules(db->prog, rule_str);

    printf("Initial database count: %lu\n", facts_count(db));

    // 3. Insert initial facts (Alice -> Bob -> Charlie)
    facts_transaction_begin(db);
    facts_add_spo(db, "Alice", "parent", "Bob");
    facts_add_spo(db, "Bob", "parent", "Charlie");
    facts_transaction_commit(db);

    // Check that Rete engine automatically derived Alice is grandparent of Charlie
    printf("Database count after initial insert: %lu\n", facts_count(db));
    s_fact *f1 = facts_get_spo(db, "Alice", "grandparent", "Charlie");
    if (f1)
        printf("SUCCESS: Found derived fact: Alice is grandparent of Charlie\n");
    else
        printf("FAILURE: Derived fact not found!\n");

    // 4. Insert a new incremental fact (Charlie -> David)
    facts_transaction_begin(db);
    facts_add_spo(db, "Charlie", "parent", "David");
    facts_transaction_commit(db);

    // Check that Rete engine automatically derived Bob is grandparent of David
    printf("Database count after incremental insert: %lu\n", facts_count(db));
    s_fact *f2 = facts_get_spo(db, "Bob", "grandparent", "David");
    if (f2)
        printf("SUCCESS: Found derived fact: Bob is grandparent of David\n");
    else
        printf("FAILURE: Derived fact not found!\n");

    delete_datalog_program(db->prog);
    delete_facts(db);
    delete_intern(sym);
    return 0;
}
