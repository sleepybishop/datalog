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

#endif
