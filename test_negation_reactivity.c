#include "facts.h"
#include "eval.h"
#include <stdio.h>
#include <stdlib.h>

int main()
{
    s_intern *sym = new_intern(10000);
    s_facts *db = new_facts(sym, 10000);
    db->prog = new_datalog_program();

    const char *rules = "?X can_fly yes :- ?X is_bird yes, NOT ?X is_penguin yes .\n";

    if (datalog_program_parse_rules(db->prog, rules) < 0) {
        printf("FAILED TO PARSE RULES\n");
    }
    datalog_program_print(db->prog, stdout);

    printf("\n--- Inserting Tweety is_bird yes ---\n");
    facts_transaction_begin(db);
    facts_add_spo(db, "Tweety", "is_bird", "yes");
    facts_transaction_commit(db);

    s_fact *f = facts_get_spo(db, "Tweety", "can_fly", "yes");
    if (f)
        printf("SUCCESS: Tweety can fly\n");
    else
        printf("FAILURE: Tweety cannot fly\n");

    printf("\n--- Inserting Tweety is_penguin yes ---\n");
    facts_transaction_begin(db);
    facts_add_spo(db, "Tweety", "is_penguin", "yes");
    facts_transaction_commit(db);

    f = facts_get_spo(db, "Tweety", "can_fly", "yes");
    if (!f)
        printf("SUCCESS: Tweety can no longer fly (retracted due to negation)\n");
    else
        printf("FAILURE: Tweety can still fly (negation retraction failed)\n");

    printf("\n--- Retracting Tweety is_penguin yes ---\n");
    facts_transaction_begin(db);
    facts_remove_spo(db, "Tweety", "is_penguin", "yes");
    facts_transaction_commit(db);

    f = facts_get_spo(db, "Tweety", "can_fly", "yes");
    if (f)
        printf("SUCCESS: Tweety can fly again (asserted due to negation retraction)\n");
    else
        printf("FAILURE: Tweety cannot fly (negation assertion failed)\n");

    delete_datalog_program(db->prog);
    delete_facts(db);
    delete_intern(sym);
    return 0;
}
