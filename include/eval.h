#ifndef EVAL_H
#define EVAL_H

#include "facts.h"
#include "rule.h"

/*
 * Evaluate a Datalog program bottom-up on the given facts database.
 * Computes the stratified fixed point of the program.
 * Derived facts are inserted directly into `facts`.
 * Returns the total number of new facts derived and inserted.
 * Returns -1 on error (e.g., if the program is unstratified).
 */
int facts_datalog_eval(s_facts *facts, const s_datalog_program *prog);
int facts_datalog_eval_incremental(s_facts *facts, const s_datalog_program *prog, const struct rollback_entry *delta,
                                   size_t delta_count);
int reactive_tx_listener(s_facts *facts, const struct rollback_entry *entries, size_t entry_count, void *user_data);
/* Backward-compatible name retained for callers of the early API. */
int rete_tx_listener(s_facts *facts, const struct rollback_entry *entries, size_t entry_count, void *user_data);

#endif
