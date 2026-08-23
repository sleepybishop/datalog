#ifndef RULE_H
#define RULE_H

#include <stdio.h>
#include <stddef.h>
#include "spec.h"

/*
 * Represents a single Datalog rule:
 * Head :- Body_1, Body_2, ..., Body_n.
 *
 * In a triple-store context, the head is a single triple pattern,
 * and the body is an array of triple patterns (which may be negated).
 */
typedef struct datalog_rule {
    s_spec_fact head;
    s_spec_fact *body;
    size_t body_count;
} s_datalog_rule;

/*
 * A collection of Datalog rules.
 */
typedef struct datalog_program {
    s_datalog_rule *rules;
    size_t rule_count;
} s_datalog_program;

/*
 * Allocate a new, empty Datalog program.
 */
s_datalog_program *new_datalog_program(void);

/* Return a deep copy, or NULL on allocation failure. */
s_datalog_program *datalog_program_clone(const s_datalog_program *prog);

/*
 * Free all memory associated with a Datalog program.
 */
void delete_datalog_program(s_datalog_program *prog);

/*
 * Add a rule to the program. The strings for head and body patterns
 * are duplicated using strdup.
 */
s_datalog_rule *datalog_program_add_rule(s_datalog_program *prog, const s_spec_fact *head, const s_spec_fact *body,
                                         size_t body_count);

/*
 * Parse a string of rules (separated by periods '.') and add them to the program.
 * Performs safety checks on each rule.
 * Returns 0 on success, or -1 on parse/safety error.
 */
int datalog_program_parse_rules(s_datalog_program *prog, const char *rules_str);

/*
 * Calculate stratification for the Datalog program.
 * Allocates and returns an array of size prog->rule_count containing the stratum
 * of each rule.
 * Also returns the number of strata in *num_strata_out.
 * If the program is not stratified (i.e. contains a negative cycle), returns NULL
 * and sets *num_strata_out = 0.
 * The returned array must be freed by the caller.
 */
int *datalog_program_stratify(const s_datalog_program *prog, int *num_strata_out);

/*
 * Validate that a rule is safe:
 * 1. Every variable in the head must appear in at least one positive subgoal.
 * 2. Every variable in a negated subgoal must appear in at least one positive subgoal.
 *
 * Returns 1 if safe, 0 if unsafe, and -1 on allocation failure.
 */
int datalog_rule_validate(const s_datalog_rule *rule);

/*
 * Print a rule to the specified file descriptor.
 */
void datalog_rule_print(const s_datalog_rule *rule, FILE *fp);

/*
 * Print all rules in a program.
 */
void datalog_program_print(const s_datalog_program *prog, FILE *fp);

#endif
