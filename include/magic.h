#ifndef MAGIC_H
#define MAGIC_H

#include "rule.h"

/*
 * Perform Magic Sets Transformation on a Datalog program for a given query goal.
 * Returns a new s_datalog_program representing the rewritten program,
 * or NULL on error/no-op.
 * The caller is responsible for calling delete_datalog_program on the returned program.
 */
s_datalog_program *datalog_program_magic_transform(const s_datalog_program *prog, const s_spec_fact *query_goal);

#endif
