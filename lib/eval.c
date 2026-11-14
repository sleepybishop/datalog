#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"
#include "facts_internal.h"
#include "justification.h"
#include "lftj.h"

static int program_has_negation(const s_datalog_program *prog);

typedef struct eval_cb_data {
    s_facts *target_db;
    const s_datalog_rule *rule;
    s_facts *main_facts;
    size_t derived_count;
    size_t rule_index;
    int *evaluation_error;
} s_eval_cb_data;

/*
 * Scratch databases are private to one evaluation, but queries over several
 * databases otherwise acquire their locks in data-dependent order. Hold each
 * scratch writer lock for its whole lifetime so nested operations are no-ops.
 * Transaction level stays zero, deliberately avoiding rollback-log references.
 */
static s_facts *new_eval_scratch(s_intern *symbols, unsigned long max)
{
    s_facts *facts = new_facts(symbols, max);
    if (!facts)
        return NULL;
    int already_owned = transaction_acquire_writer(&facts->tx);
    assert(!already_owned);
    (void)already_owned;
    return facts;
}

static void delete_eval_scratch(s_facts *facts)
{
    if (!facts)
        return;
    assert(transaction_writer_owned(&facts->tx));
    assert(facts->tx.level == 0);
    assert(facts->tx.rollback.size == 0);
    transaction_release_writer(&facts->tx, 0);
    delete_facts(facts);
}

static const char *resolve_term(const char *term, s_binding *bindings)
{
    if (!term)
        return NULL;
    if (term[0] == '?') {
        const char **val_ptr = bindings_get(bindings, term);
        return (val_ptr && *val_ptr) ? *val_ptr : NULL;
    }
    return term;
}

static void eval_solution_cb(s_binding *bindings, void *user_data)
{
    s_eval_cb_data *data = (s_eval_cb_data *)user_data;
    if (!data || !data->evaluation_error || !data->rule || !data->main_facts || !data->target_db)
        return;
    if (*data->evaluation_error)
        return;
    const s_datalog_rule *rule = data->rule;

    /* Check negated subgoals first */
    for (size_t i = 0; i < rule->body_count; i++) {
        if (rule->body[i].negated) {
            const char *s_val = resolve_term(rule->body[i].s, bindings);
            const char *p_val = resolve_term(rule->body[i].p, bindings);
            const char *o_val = resolve_term(rule->body[i].o, bindings);
            if (!s_val || !p_val || !o_val) {
                return;
            }
            if (facts_contains_spo(data->main_facts, s_val, p_val, o_val) > 0) {
                return;
            }
        }
    }

    const char *s_val = rule->head.s;
    if (s_val && s_val[0] == '?') {
        const char **val_ptr = bindings_get(bindings, s_val);
        s_val = (val_ptr && *val_ptr) ? *val_ptr : NULL;
    }

    const char *p_val = rule->head.p;
    if (p_val && p_val[0] == '?') {
        const char **val_ptr = bindings_get(bindings, p_val);
        p_val = (val_ptr && *val_ptr) ? *val_ptr : NULL;
    }

    const char *o_val = rule->head.o;
    if (o_val && o_val[0] == '?') {
        const char **val_ptr = bindings_get(bindings, o_val);
        o_val = (val_ptr && *val_ptr) ? *val_ptr : NULL;
    }

    if (!s_val || !p_val || !o_val) {
        return;
    }

    if (data->main_facts->justifications_staging) {
        size_t support_count = data->rule->body_count;
        s_justification_support *supports = support_count ? calloc(support_count, sizeof(*supports)) : NULL;
        if (support_count && !supports) {
            *data->evaluation_error = 1;
            return;
        }
        int valid = 1;
        for (size_t i = 0; i < support_count; i++) {
            supports[i].s = (char *)resolve_term(data->rule->body[i].s, bindings);
            supports[i].p = (char *)resolve_term(data->rule->body[i].p, bindings);
            supports[i].o = (char *)resolve_term(data->rule->body[i].o, bindings);
            supports[i].negated = data->rule->body[i].negated != NULL;
            if (!supports[i].s || !supports[i].p || !supports[i].o) {
                valid = 0;
                break;
            }
        }
        if (!valid || justification_graph_add(data->main_facts->justifications_staging, s_val, p_val, o_val, data->rule_index,
                                              supports, support_count) < 0)
            *data->evaluation_error = 1;
        free(supports);
        if (*data->evaluation_error)
            return;
    }

    /* an asserted fact may independently gain derived support. */
    s_fact_support support;
    int in_main = facts_get_support_spo(data->main_facts, s_val, p_val, o_val, &support);
    if ((in_main > 0 && support.derived) || facts_contains_spo(data->target_db, s_val, p_val, o_val) > 0)
        return;

    /* insert new unique fact */
    if (data->target_db) {
        if (facts_add_spo(data->target_db, s_val, p_val, o_val))
            data->derived_count++;
        else if (data->evaluation_error)
            *data->evaluation_error = 1;
    }
}

static int facts_merge_count(s_facts *dest, s_facts *src, size_t *count_out)
{
    size_t count = 0;
    int result = 0;
    s_facts_cursor c;
    facts_with_0(src, &c, NULL, NULL, NULL);
    s_fact *f;
    while ((f = facts_cursor_next(&c)) != NULL) {
        if (facts_add_spo(dest, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o)))
            count++;
        else {
            result = -1;
            break;
        }
    }
    if (facts_cursor_error(&c))
        result = -1;
    facts_cursor_stop(&c);
    if (count_out)
        *count_out = count;
    return result;
}

static int facts_merge_derived_count(s_facts *dest, s_facts *src, size_t *count_out)
{
    size_t count = 0;
    int result = 0;
    s_facts_cursor c;
    facts_with_0(src, &c, NULL, NULL, NULL);
    s_fact *f;
    while ((f = facts_cursor_next(&c)) != NULL) {
        s_fact_support before;
        int existed = facts_get_support_spo(dest, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o), &before);
        s_fact *added =
            facts_add_spo_origin(dest, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o), FACT_ORIGIN_DERIVED);
        if (!added) {
            result = -1;
            break;
        }
        if (existed <= 0 || before.derived == 0)
            count++;
    }
    if (facts_cursor_error(&c))
        result = -1;
    facts_cursor_stop(&c);
    if (count_out)
        *count_out = count;
    return result;
}

static int add_pred_to_list_eval(const char ***list, size_t *count, const char *pred)
{
    if (!pred)
        return 0;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*list)[i], pred) == 0) {
            return 0;
        }
    }
    if (*count == SIZE_MAX / sizeof(char *))
        return -1;
    const char **new_list = realloc(*list, (*count + 1) * sizeof(char *));
    if (!new_list)
        return -1;
    *list = new_list;
    (*list)[*count] = pred;
    (*count)++;
    return 0;
}

static p_spec compile_rule_body(const s_datalog_rule *rule)
{
    size_t pos_count = 0;
    for (size_t i = 0; i < rule->body_count; i++) {
        if (!rule->body[i].negated) {
            pos_count++;
        }
    }
    if (pos_count > (SIZE_MAX - 2) / 4 || pos_count * 4 + 2 > SIZE_MAX / sizeof(char *))
        return NULL;
    const char **spec = calloc(pos_count * 4 + 2, sizeof(char *));
    if (!spec)
        return NULL;
    size_t idx = 0;
    for (size_t i = 0; i < rule->body_count; i++) {
        if (!rule->body[i].negated) {
            spec[idx * 4] = rule->body[i].s;
            spec[idx * 4 + 1] = rule->body[i].p;
            spec[idx * 4 + 2] = rule->body[i].o;
            spec[idx * 4 + 3] = NULL;
            idx++;
        }
    }
    spec[pos_count * 4] = NULL;
    spec[pos_count * 4 + 1] = NULL;
    return (p_spec)spec;
}

static int prepare_rule_query(const s_datalog_rule *rule, p_spec *spec_out, s_binding **bindings_out, s_facts ***dbs_out)
{
    *spec_out = compile_rule_body(rule);
    *bindings_out = NULL;
    *dbs_out = NULL;
    if (!*spec_out)
        return -1;
    *bindings_out = (*spec_out)[0] ? spec_bindings(*spec_out) : calloc(1, sizeof(**bindings_out));
    if (!*bindings_out) {
        free(*spec_out);
        *spec_out = NULL;
        return -1;
    }
    size_t count = rule->body_count ? rule->body_count : 1;
    *dbs_out = calloc(count, sizeof(**dbs_out));
    if (!*dbs_out) {
        free(*bindings_out);
        free(*spec_out);
        *bindings_out = NULL;
        *spec_out = NULL;
        return -1;
    }
    return 0;
}

static int facts_datalog_eval_locked(s_facts *facts, const s_datalog_program *prog)
{
    assert(facts);
    assert(prog);

    if (prog->rule_count == 0)
        return 0;

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    if (!rule_strata) {
        return -1;
    }

    size_t total_derived = 0;
    int evaluation_error = 0;

    for (int s = 0; s < num_strata; s++) {
        /* 1. Identify all IDB predicates in stratum s */
        const char **idb_preds = NULL;
        size_t idb_preds_count = 0;
        int variable_head = 0;
        int variable_body = 0;
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] == s) {
                const s_datalog_rule *rule = &prog->rules[r];
                for (size_t j = 0; j < rule->body_count; j++) {
                    if (rule->body[j].p && rule->body[j].p[0] == '?')
                        variable_body = 1;
                }
                if (rule->head.p && rule->head.p[0] != '?') {
                    if (add_pred_to_list_eval(&idb_preds, &idb_preds_count, rule->head.p) != 0)
                        evaluation_error = 1;
                } else {
                    variable_head = 1;
                }
            }
        }

        if (evaluation_error) {
            free(idb_preds);
            break;
        }

        /* 2. Create query-local databases for stratum evaluation */
        s_facts *old_db = new_eval_scratch(facts->symbols, 256);
        s_facts *delta_db = new_eval_scratch(facts->symbols, 256);
        s_facts *new_db = new_eval_scratch(facts->symbols, 256);
        s_facts *old_plus_delta_db = new_eval_scratch(facts->symbols, 256);
        if (!old_db || !delta_db || !new_db || !old_plus_delta_db) {
            delete_eval_scratch(old_db);
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            delete_eval_scratch(old_plus_delta_db);
            free(idb_preds);
            free(rule_strata);
            return -1;
        }

        /* existing asserted and derived IDB tuples are part of the fixed point;
         * keep them in the old relation so only newly gained support enters delta. */
        if (variable_head || variable_body) {
            if (facts_merge_count(old_db, facts, NULL) != 0)
                evaluation_error = 1;
        } else {
            for (size_t i = 0; !evaluation_error && i < idb_preds_count; i++) {
                s_facts_cursor cursor;
                const char *subject = NULL, *object = NULL;
                facts_with_1_2(facts, &cursor, "?subject", idb_preds[i], "?object", &subject, NULL, &object);
                s_fact *fact;
                while ((fact = facts_cursor_next(&cursor)) != NULL) {
                    if (!facts_add_spo(old_db, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o))) {
                        evaluation_error = 1;
                        break;
                    }
                }
                if (facts_cursor_error(&cursor))
                    evaluation_error = 1;
                facts_cursor_stop(&cursor);
            }
        }
        if (!evaluation_error && facts_merge_count(old_plus_delta_db, old_db, NULL) != 0)
            evaluation_error = 1;

        /* 3. iteration 0: naive evaluation to seed delta_db */
        for (size_t r = 0; !evaluation_error && r < prog->rule_count; r++) {
            if (rule_strata[r] != s)
                continue;
            const s_datalog_rule *rule = &prog->rules[r];

            p_spec spec;
            s_binding *bindings;
            s_facts **dbs;
            if (prepare_rule_query(rule, &spec, &bindings, &dbs) != 0) {
                evaluation_error = 1;
                break;
            }

            size_t pos_idx = 0;
            for (size_t j = 0; j < rule->body_count; j++) {
                if (!rule->body[j].negated)
                    dbs[pos_idx++] = facts;
            }

            s_eval_cb_data cb_data;
            cb_data.target_db = delta_db;
            cb_data.rule = rule;
            cb_data.main_facts = facts;
            cb_data.derived_count = 0;

            cb_data.rule_index = r;
            cb_data.evaluation_error = &evaluation_error;

            if (!spec[0])
                eval_solution_cb(bindings, &cb_data);
            else if (facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data) < 0)
                evaluation_error = 1;

            free(dbs);
            free(spec);
            free(bindings);
        }

        if (evaluation_error) {
            delete_eval_scratch(old_db);
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            delete_eval_scratch(old_plus_delta_db);
            free(idb_preds);
            break;
        }

        size_t delta_size = facts_count(delta_db);
        if (delta_size == 0) {
            delete_eval_scratch(old_db);
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            delete_eval_scratch(old_plus_delta_db);
            free(idb_preds);
            continue;
        }

        /* merge delta_db into old_plus_delta_db and register with the main database */
        size_t merged_derived = 0;
        if (facts_merge_count(old_plus_delta_db, delta_db, NULL) != 0 ||
            facts_merge_derived_count(facts, delta_db, &merged_derived) != 0) {
            evaluation_error = 1;
        }
        total_derived += merged_derived;

        /* 4. semi-naive fixed-point loop */
        while (!evaluation_error) {
            if (facts_reset_local_db(new_db) != 0) {
                evaluation_error = 1;
                break;
            }

            size_t iter_derived = 0;

            for (size_t r = 0; r < prog->rule_count; r++) {
                if (rule_strata[r] != s)
                    continue;
                const s_datalog_rule *rule = &prog->rules[r];

                /* identify which subgoals are recursive (current stratum IDB predicates) */
                int recursive_subgoal_count = 0;
                int *recursive_subgoal_indices =
                    malloc((rule->body_count ? rule->body_count : 1) * sizeof(*recursive_subgoal_indices));
                if (!recursive_subgoal_indices) {
                    evaluation_error = 1;
                    break;
                }
                for (size_t j = 0; j < rule->body_count; j++) {
                    const s_spec_fact *sub = &rule->body[j];
                    if (sub->negated)
                        continue;
                    if (sub->p) {
                        int is_recursive = variable_head || sub->p[0] == '?';
                        for (size_t k = 0; k < idb_preds_count; k++) {
                            if (strcmp(idb_preds[k], sub->p) == 0) {
                                is_recursive = 1;
                                break;
                            }
                        }
                        if (is_recursive) {
                            recursive_subgoal_indices[recursive_subgoal_count++] = (int)j;
                        }
                    }
                }

                if (recursive_subgoal_count == 0) {
                    /* non-recursive rules are only run in iteration 0 */
                    free(recursive_subgoal_indices);
                    continue;
                }

                /* evaluate the rule version for each recursive subgoal */
                for (int v = 0; v < recursive_subgoal_count; v++) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, &spec, &bindings, &dbs) != 0) {
                        evaluation_error = 1;
                        break;
                    }

                    size_t pos_idx = 0;
                    for (size_t j = 0; j < rule->body_count; j++) {
                        if (rule->body[j].negated)
                            continue;

                        int rec_idx = -1;
                        for (int w = 0; w < recursive_subgoal_count; w++) {
                            if (recursive_subgoal_indices[w] == (int)j) {
                                rec_idx = w;
                                break;
                            }
                        }

                        if (rec_idx == -1) {
                            dbs[pos_idx++] = facts;
                        } else if (rec_idx == v) {
                            dbs[pos_idx++] = delta_db;
                        } else if (rec_idx < v) {
                            dbs[pos_idx++] = old_db;
                        } else {
                            dbs[pos_idx++] = old_plus_delta_db;
                        }
                    }

                    s_eval_cb_data cb_data;
                    cb_data.target_db = new_db;
                    cb_data.rule = rule;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;

                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;

                    if (!spec[0])
                        eval_solution_cb(bindings, &cb_data);
                    else if (facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data) < 0)
                        evaluation_error = 1;

                    iter_derived += cb_data.derived_count;

                    free(dbs);
                    free(spec);
                    free(bindings);
                }
                free(recursive_subgoal_indices);
            }

            if (evaluation_error || iter_derived == 0) {
                break;
            }

            /* update old_db and delta_db for next iteration, persist results to facts */
            if (facts_merge_count(old_db, delta_db, NULL) != 0 || facts_merge_count(old_plus_delta_db, new_db, NULL) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_reset_local_db(delta_db) != 0) {
                evaluation_error = 1;
                break;
            }
            merged_derived = 0;
            if (facts_merge_count(delta_db, new_db, NULL) != 0 || facts_merge_derived_count(facts, new_db, &merged_derived) != 0) {
                evaluation_error = 1;
                break;
            }
            total_derived += merged_derived;
        }

        delete_eval_scratch(old_db);
        delete_eval_scratch(delta_db);
        delete_eval_scratch(new_db);
        delete_eval_scratch(old_plus_delta_db);
        free(idb_preds);
        if (evaluation_error)
            break;
    }

    free(rule_strata);
    return evaluation_error ? -1 : (int)total_derived;
}

int facts_datalog_eval(s_facts *facts, const s_datalog_program *prog)
{
    if (!facts || !prog)
        return -1;

    int started_transaction = !transaction_writer_owned(&facts->tx);
    if (started_transaction && facts_transaction_begin(facts) != 0)
        return -1;

    int old_disable = facts->disable_listener;
    facts->disable_listener = 1;
    int result = facts_datalog_eval_locked(facts, prog);

    if (started_transaction) {
        facts->disable_listener = old_disable;
        if (result < 0) {
            if (facts_transaction_rollback(facts) != 0)
                result = -1;
        } else if (transaction_commit_silent(facts, &facts->tx) != 0) {
            result = -1;
        }
    } else {
        facts->disable_listener = old_disable;
    }
    return result;
}

static int program_head_matches_fact(const s_datalog_program *prog, const s_fact *fact)
{
    const char *values[] = {symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o)};
    for (size_t r = 0; r < prog->rule_count; r++) {
        const s_spec_fact *head = &prog->rules[r].head;
        const char *terms[] = {head->s, head->p, head->o};
        int matches = 1;
        for (size_t t = 0; t < 3 && matches; t++) {
            if (!terms[t]) {
                matches = 0;
            } else if (terms[t][0] != '?') {
                matches = strcmp(terms[t], values[t]) == 0;
            } else {
                for (size_t earlier = 0; earlier < t; earlier++) {
                    if (strcmp(terms[t], terms[earlier]) == 0 && strcmp(values[t], values[earlier]) != 0) {
                        matches = 0;
                        break;
                    }
                }
            }
        }
        if (matches)
            return 1;
    }
    return 0;
}

/* standalone evaluation has no per-program proof graph; recompute the supplied
 * program's output scope while treating derived facts outside its heads as
 * external inputs. preserve those inputs and unchanged live output facts;
 * managed programs use directed invalidation instead. */
static int facts_datalog_reconcile(s_facts *facts, const s_datalog_program *prog)
{
    s_facts *closure = new_eval_scratch(facts->symbols, 256);
    s_facts *removals = new_eval_scratch(facts->symbols, 256);
    int result = -1;
    if (!closure || !removals)
        goto done;
    s_facts_cursor cursor;
    facts_with_0(facts, &cursor, NULL, NULL, NULL);
    s_fact *fact;
    int failed = 0;
    while ((fact = facts_cursor_next(&cursor)) != NULL) {
        if ((fact->asserted_count || (fact->derived_count && !program_head_matches_fact(prog, fact))) &&
            !facts_add_spo(closure, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o))) {
            failed = 1;
            break;
        }
    }
    if (facts_cursor_error(&cursor))
        failed = 1;
    facts_cursor_stop(&cursor);
    if (failed || facts_datalog_eval_locked(closure, prog) < 0)
        goto done;
    facts_with_0(facts, &cursor, NULL, NULL, NULL);
    while ((fact = facts_cursor_next(&cursor)) != NULL) {
        if (!fact->derived_count || !program_head_matches_fact(prog, fact))
            continue;
        s_fact_support support;
        int found =
            facts_get_support_spo(closure, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o), &support);
        if ((found == 0 || !support.derived) &&
            !facts_add_spo(removals, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o))) {
            failed = 1;
            break;
        }
    }
    if (facts_cursor_error(&cursor))
        failed = 1;
    facts_cursor_stop(&cursor);
    if (failed)
        goto done;
    facts_with_0(removals, &cursor, NULL, NULL, NULL);
    while ((fact = facts_cursor_next(&cursor)) != NULL) {
        if (facts_remove_spo_origin(facts, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o),
                                    FACT_ORIGIN_DERIVED) < 0) {
            failed = 1;
            break;
        }
    }
    if (facts_cursor_error(&cursor))
        failed = 1;
    facts_cursor_stop(&cursor);
    if (failed)
        goto done;
    result = 0;
    facts_with_0(closure, &cursor, NULL, NULL, NULL);
    while ((fact = facts_cursor_next(&cursor)) != NULL) {
        if (!fact->derived_count)
            continue;
        s_fact_support support;
        int found = facts_get_support_spo(facts, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o), &support);
        if (found && support.derived)
            continue;
        if (!facts_add_spo_origin(facts, symbol_to_str(fact->s), symbol_to_str(fact->p), symbol_to_str(fact->o),
                                  FACT_ORIGIN_DERIVED)) {
            result = -1;
            break;
        }
        result++;
    }
    if (facts_cursor_error(&cursor))
        result = -1;
    facts_cursor_stop(&cursor);
done:
    delete_eval_scratch(closure);
    delete_eval_scratch(removals);
    return result;
}

static int facts_datalog_eval_incremental_locked(s_facts *facts, const s_datalog_program *prog, const s_rollback_entry *delta,
                                                 size_t delta_count)
{
    if (!facts || !prog || !delta || delta_count == 0)
        return 0;
    if (prog->rule_count == 0)
        return 0;
    if (!facts->justifications_staging) {
        if (program_has_negation(prog))
            return facts_datalog_reconcile(facts, prog);
        for (size_t i = 0; i < delta_count; i++) {
            if (delta[i].action == ROLLBACK_ADD || delta[i].action == ROLLBACK_STATE)
                return facts_datalog_reconcile(facts, prog);
        }
    }

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    if (!rule_strata)
        return -1;

    size_t total_derived = 0;
    int evaluation_error = 0;
    s_facts *cumulative_delta_db = new_eval_scratch(facts->symbols, 256);
    if (!cumulative_delta_db) {
        delete_eval_scratch(cumulative_delta_db);
        free(rule_strata);
        return -1;
    }

    for (size_t i = 0; i < delta_count; i++) {
        if (delta[i].action == ROLLBACK_REMOVE &&
            facts_contains_spo(facts, symbol_to_str(delta[i].fact.s), symbol_to_str(delta[i].fact.p),
                               symbol_to_str(delta[i].fact.o))) {
            if (!facts_add_spo(cumulative_delta_db, symbol_to_str(delta[i].fact.s), symbol_to_str(delta[i].fact.p),
                               symbol_to_str(delta[i].fact.o))) {
                evaluation_error = 1;
                break;
            }
        }
    }

    for (int s = 0; !evaluation_error && s < num_strata; s++) {
        s_facts *delta_db = new_eval_scratch(facts->symbols, 256);
        s_facts *new_db = new_eval_scratch(facts->symbols, 256);
        if (!delta_db || !new_db) {
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            evaluation_error = 1;
            break;
        }

        if (facts_merge_count(delta_db, cumulative_delta_db, NULL) != 0) {
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            evaluation_error = 1;
            break;
        }

        if (facts_count(delta_db) == 0) {
            delete_eval_scratch(delta_db);
            delete_eval_scratch(new_db);
            continue;
        }

        while (1) {
            if (facts_reset_local_db(new_db) != 0) {
                evaluation_error = 1;
                break;
            }

            size_t iter_derived = 0;
            for (size_t r = 0; r < prog->rule_count; r++) {
                if (rule_strata[r] != s)
                    continue;
                const s_datalog_rule *rule = &prog->rules[r];

                int target_count = 0;
                int *target_indices = malloc((rule->body_count ? rule->body_count : 1) * sizeof(*target_indices));
                if (!target_indices) {
                    evaluation_error = 1;
                    break;
                }
                for (size_t j = 0; j < rule->body_count; j++) {
                    if (!rule->body[j].negated) {
                        target_indices[target_count++] = (int)j;
                    }
                }

                if (target_count == 0) {
                    s_binding empty_binding = {0};
                    s_eval_cb_data cb_data = {.target_db = new_db,
                                              .rule = rule,
                                              .main_facts = facts,
                                              .rule_index = r,
                                              .evaluation_error = &evaluation_error};
                    eval_solution_cb(&empty_binding, &cb_data);
                    iter_derived += cb_data.derived_count;
                    free(target_indices);
                    continue;
                }

                for (int v = 0; v < target_count; v++) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, &spec, &bindings, &dbs) != 0) {
                        evaluation_error = 1;
                        break;
                    }

                    size_t pos_idx = 0;
                    for (size_t j = 0; j < rule->body_count; j++) {
                        if (rule->body[j].negated)
                            continue;

                        int rec_idx = -1;
                        for (int w = 0; w < target_count; w++) {
                            if (target_indices[w] == (int)j) {
                                rec_idx = w;
                                break;
                            }
                        }

                        if (rec_idx == v)
                            dbs[pos_idx] = delta_db;
                        else
                            dbs[pos_idx] = facts;

                        pos_idx++;
                    }

                    s_eval_cb_data cb_data;
                    cb_data.target_db = new_db;
                    cb_data.rule = rule;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;

                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;

                    if (!spec[0])
                        eval_solution_cb(bindings, &cb_data);
                    else if (facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data) < 0)
                        evaluation_error = 1;
                    iter_derived += cb_data.derived_count;
                    free(dbs);
                    free(spec);
                    free(bindings);
                }
                free(target_indices);
            }

            if (evaluation_error || iter_derived == 0)
                break;

            if (facts_reset_local_db(delta_db) != 0) {
                evaluation_error = 1;
                break;
            }
            size_t merged_derived = 0;
            if (facts_merge_count(delta_db, new_db, NULL) != 0 || facts_merge_count(cumulative_delta_db, new_db, NULL) != 0 ||
                facts_merge_derived_count(facts, new_db, &merged_derived) != 0) {
                evaluation_error = 1;
                break;
            }
            total_derived += merged_derived;
        }

        delete_eval_scratch(delta_db);
        delete_eval_scratch(new_db);
    }

    delete_eval_scratch(cumulative_delta_db);
    free(rule_strata);
    return evaluation_error ? -1 : (int)total_derived;
}

int facts_datalog_eval_incremental(s_facts *facts, const s_datalog_program *prog, const s_rollback_entry *delta, size_t delta_count)
{
    if (!facts || !prog || (delta_count && !delta))
        return -1;
    if (!delta_count)
        return 0;
    int started_transaction = !transaction_writer_owned(&facts->tx);
    if (started_transaction && facts_transaction_begin(facts) != 0)
        return -1;
    int old_disable = facts->disable_listener;
    facts->disable_listener = 1;
    int result = facts_datalog_eval_incremental_locked(facts, prog, delta, delta_count);
    facts->disable_listener = old_disable;
    if (started_transaction) {
        if (result < 0)
            facts_transaction_rollback(facts);
        else if (transaction_commit_silent(facts, &facts->tx) != 0)
            result = -1;
    }
    return result;
}

static int program_has_negation(const s_datalog_program *prog)
{
    if (!prog)
        return 0;
    for (size_t r = 0; r < prog->rule_count; r++) {
        for (size_t j = 0; j < prog->rules[r].body_count; j++) {
            if (prog->rules[r].body[j].negated)
                return 1;
        }
    }
    return 0;
}

typedef struct invalidation_event {
    char *s;
    char *p;
    char *o;
    int negated;
} s_invalidation_event;

static void invalidation_events_destroy(s_invalidation_event *events, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(events[i].s);
        free(events[i].p);
        free(events[i].o);
    }
    free(events);
}

static int invalidation_event_append(s_invalidation_event **events, size_t *count, const char *s, const char *p, const char *o,
                                     int negated)
{
    for (size_t i = 0; i < *count; i++) {
        if ((*events)[i].negated == negated && strcmp((*events)[i].s, s) == 0 && strcmp((*events)[i].p, p) == 0 &&
            strcmp((*events)[i].o, o) == 0)
            return 0;
    }
    if (*count == SIZE_MAX / sizeof(**events))
        return -1;
    s_invalidation_event *resized = realloc(*events, (*count + 1) * sizeof(**events));
    if (!resized)
        return -1;
    *events = resized;
    s_invalidation_event *event = &resized[*count];
    memset(event, 0, sizeof(*event));
    event->s = strdup(s);
    event->p = strdup(p);
    event->o = strdup(o);
    event->negated = negated;
    if (!event->s || !event->p || !event->o) {
        free(event->s);
        free(event->p);
        free(event->o);
        memset(event, 0, sizeof(*event));
        return -1;
    }
    (*count)++;
    return 0;
}

static int external_justification_support(const s_justification_support *support, void *user_data)
{
    s_facts *facts = user_data;
    s_fact_support current;
    int found = facts_get_support_spo(facts, support->s, support->p, support->o, &current);
    if (found < 0)
        return -1;
    return support->negated ? found == 0 : found > 0 && current.asserted != 0;
}

static int invalidation_collect_conclusions(s_justification_graph *graph, const char *s, const char *p, const char *o, int negated,
                                            s_invalidation_event **candidates, size_t *candidate_count)
{
    s_justification_support *conclusions = NULL;
    size_t conclusion_count = 0;
    int result = justification_graph_remove_support(graph, s, p, o, negated, &conclusions, &conclusion_count);
    for (size_t i = 0; result >= 0 && i < conclusion_count; i++) {
        const s_justification_support *conclusion = &conclusions[i];
        if (invalidation_event_append(candidates, candidate_count, conclusion->s, conclusion->p, conclusion->o, 0) != 0)
            result = -1;
    }
    justification_support_array_destroy(conclusions, conclusion_count);
    return result < 0 ? -1 : 0;
}

static int facts_invalidate_justifications(s_facts *facts, const s_rollback_entry *entries, size_t entry_count,
                                           int *requires_closure_scan)
{
    s_invalidation_event *events = NULL;
    size_t event_count = 0;
    s_invalidation_event *candidates = NULL;
    size_t candidate_count = 0;
    s_justification_grounding *grounding = NULL;
    int has_negation = program_has_negation(facts->prog);
    *requires_closure_scan = 0;
    for (size_t i = 0; i < entry_count; i++) {
        const char *s = symbol_to_str(entries[i].fact.s);
        const char *p = symbol_to_str(entries[i].fact.p);
        const char *o = symbol_to_str(entries[i].fact.o);
        s_fact_support support;
        int found = facts_get_support_spo(facts, s, p, o, &support);
        if (found < 0)
            goto error;
        if (entries[i].action == ROLLBACK_ADD && !found) {
            if (has_negation)
                *requires_closure_scan = 1;
            if (invalidation_event_append(&events, &event_count, s, p, o, 0) != 0)
                goto error;
        } else if (entries[i].action == ROLLBACK_REMOVE && found) {
            if (invalidation_event_append(&events, &event_count, s, p, o, 1) != 0)
                goto error;
        } else if (entries[i].action == ROLLBACK_STATE && entries[i].fact.asserted_count && found && !support.asserted) {
            /* losing asserted support can unground a recursive component even
             * while its tuples still carry derived support. */
            if (invalidation_event_append(&events, &event_count, s, p, o, 2) != 0)
                goto error;
        }
    }

    /* apply every base invalidation before computing a grounded closure; a
     * proof that refers to a removed tuple must not seed that closure merely
     * because some other proof for the removed tuple remains in the graph. */
    for (size_t cursor = 0; cursor < event_count; cursor++) {
        if (events[cursor].negated == 2) {
            if (invalidation_event_append(&candidates, &candidate_count, events[cursor].s, events[cursor].p, events[cursor].o, 0) !=
                0)
                goto error;
        } else if (invalidation_collect_conclusions(facts->justifications_staging, events[cursor].s, events[cursor].p,
                                                    events[cursor].o, events[cursor].negated, &candidates, &candidate_count) != 0) {
            goto error;
        }
    }

    if (candidate_count) {
        grounding = new_justification_grounding(facts->justifications_staging, external_justification_support, facts);
        if (!grounding)
            goto error;
    }
    for (size_t cursor = 0; cursor < candidate_count; cursor++) {
        const s_invalidation_event *conclusion = &candidates[cursor];
        if (justification_grounding_contains(grounding, conclusion->s, conclusion->p, conclusion->o))
            continue;
        s_fact_support support;
        int found = facts_get_support_spo(facts, conclusion->s, conclusion->p, conclusion->o, &support);
        if (found < 0)
            goto error;
        if (found > 0 && support.derived) {
            int disappears = support.asserted == 0;
            if (facts_remove_spo_origin(facts, conclusion->s, conclusion->p, conclusion->o, FACT_ORIGIN_DERIVED) < 0)
                goto error;
            if (disappears) {
                if (invalidation_collect_conclusions(facts->justifications_staging, conclusion->s, conclusion->p, conclusion->o, 0,
                                                     &candidates, &candidate_count) != 0)
                    goto error;
                /* positive cascades only discard ungrounded conclusions and
                 * proofs that use them, leaving this snapshot valid. with
                 * negation a disappearance can make a support true, so rebuild
                 * before testing another conclusion; snapshot keys are owned
                 * independently of the compacted proof array. */
                if (has_negation) {
                    *requires_closure_scan = 1;
                    delete_justification_grounding(grounding);
                    grounding = new_justification_grounding(facts->justifications_staging, external_justification_support, facts);
                    if (!grounding)
                        goto error;
                }
            }
        }
    }
    delete_justification_grounding(grounding);
    invalidation_events_destroy(candidates, candidate_count);
    invalidation_events_destroy(events, event_count);
    return 0;

error:
    delete_justification_grounding(grounding);
    invalidation_events_destroy(candidates, candidate_count);
    invalidation_events_destroy(events, event_count);
    return -1;
}

int reactive_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    (void)user_data;
    if (facts->disable_listener) {
        return 0;
    }
    if (!facts->prog) {
        return 0;
    }

    facts->disable_listener = 1;
    if (facts_justifications_stage(facts, 1) != 0) {
        facts->disable_listener = 0;
        return -1;
    }

    int result = 0;
    s_rollback_entry *owned_entries = NULL;
    const s_rollback_entry *pending = entries;
    size_t pending_count = entry_count;
    do {
        size_t produced_start = facts->tx.rollback.size;
        int requires_closure_scan = 0;
        result = facts_invalidate_justifications(facts, pending, pending_count, &requires_closure_scan);
        if (result == 0) {
            if (requires_closure_scan) {
                result = facts_datalog_eval(facts, facts->prog);
            } else {
                int has_insertions = 0;
                for (size_t i = 0; i < pending_count; i++) {
                    if (pending[i].action == ROLLBACK_REMOVE) {
                        has_insertions = 1;
                        break;
                    }
                }
                if (has_insertions)
                    result = facts_datalog_eval_incremental(facts, facts->prog, pending, pending_count);
            }
        }
        free(owned_entries);
        owned_entries = NULL;
        if (result < 0 || !program_has_negation(facts->prog))
            break;
        pending_count = facts->tx.rollback.size - produced_start;
        if (!pending_count)
            break;
        /* inferred additions may invalidate a negated support in a later
         * stratum; process their changes before publishing the transaction. */
        if (pending_count > SIZE_MAX / sizeof(*owned_entries)) {
            result = -1;
            break;
        }
        owned_entries = malloc(pending_count * sizeof(*owned_entries));
        if (!owned_entries) {
            result = -1;
            break;
        }
        memcpy(owned_entries, &facts->tx.rollback.entries[produced_start], pending_count * sizeof(*owned_entries));
        pending = owned_entries;
    } while (pending_count);
    free(owned_entries);
    facts->disable_listener = 0;
    if (result < 0)
        facts_justifications_discard(facts);
    return result < 0 ? -1 : 0;
}

int rete_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    return reactive_tx_listener(facts, entries, entry_count, user_data);
}
