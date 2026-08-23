#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"
#include "facts_internal.h"
#include "justification.h"
#include "lftj.h"

typedef struct eval_cb_data {
    s_facts *target_db;
    const s_datalog_rule *rule;
    s_facts *old_db;
    s_facts *main_facts;
    size_t derived_count;
    int is_deletion;
    int skip_negated_check_idx;
    size_t rule_index;
    int *evaluation_error;
} s_eval_cb_data;

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
        if (rule->body[i].negated && (int)i != data->skip_negated_check_idx) {
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

    if (!data->is_deletion && data->main_facts->justifications_staging) {
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

    if (data->is_deletion) {
        /* During deletion, we WANT to find facts that exist in the main database so we can delete them.
           But we still don't want to insert duplicates into target_db. */
        if (facts_contains_spo(data->target_db, s_val, p_val, o_val) > 0) {
            return;
        }
    } else {
        /* An asserted fact may independently gain derived support. */
        s_fact_support support;
        int in_main = facts_get_support_spo(data->main_facts, s_val, p_val, o_val, &support);
        if (in_main > 0 && support.derived) {
            return;
        }
        if (facts_contains_spo(data->old_db, s_val, p_val, o_val) > 0) {
            return;
        }
        if (facts_contains_spo(data->target_db, s_val, p_val, o_val) > 0) {
            return;
        }
    }

    /* Insert new unique fact */
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

static p_spec compile_rule_body_with_trigger(const s_datalog_rule *rule, int trigger_idx)
{
    size_t pos_count = 0;
    for (size_t i = 0; i < rule->body_count; i++) {
        if (!rule->body[i].negated || (int)i == trigger_idx) {
            pos_count++;
        }
    }
    if (pos_count == 0) {
        return NULL;
    }
    if (pos_count > (SIZE_MAX - 2) / 4 || pos_count * 4 + 2 > SIZE_MAX / sizeof(char *))
        return NULL;
    const char **spec = calloc(pos_count * 4 + 2, sizeof(char *));
    if (!spec)
        return NULL;
    size_t idx = 0;
    for (size_t i = 0; i < rule->body_count; i++) {
        if (!rule->body[i].negated || (int)i == trigger_idx) {
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

static int prepare_rule_query(const s_datalog_rule *rule, int trigger_idx, p_spec *spec_out, s_binding **bindings_out,
                              s_facts ***dbs_out)
{
    *spec_out = compile_rule_body_with_trigger(rule, trigger_idx);
    *bindings_out = NULL;
    *dbs_out = NULL;
    if (!*spec_out)
        return -1;
    *bindings_out = spec_bindings(*spec_out);
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
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] == s) {
                const s_datalog_rule *rule = &prog->rules[r];
                if (rule->head.p && rule->head.p[0] != '?') {
                    if (add_pred_to_list_eval(&idb_preds, &idb_preds_count, rule->head.p) != 0)
                        evaluation_error = 1;
                }
            }
        }

        if (evaluation_error) {
            free(idb_preds);
            break;
        }

        if (idb_preds_count == 0) {
            continue;
        }

        /* 2. Create query-local databases for stratum evaluation */
        s_facts *old_db = new_facts(facts->symbols, 256);
        s_facts *delta_db = new_facts(facts->symbols, 256);
        s_facts *new_db = new_facts(facts->symbols, 256);
        s_facts *old_plus_delta_db = new_facts(facts->symbols, 256);
        if (!old_db || !delta_db || !new_db || !old_plus_delta_db) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            free(rule_strata);
            return -1;
        }

        /* 3. Iteration 0: Naive evaluation to seed delta_db */
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] != s)
                continue;
            const s_datalog_rule *rule = &prog->rules[r];

            p_spec spec;
            s_binding *bindings;
            s_facts **dbs;
            if (prepare_rule_query(rule, -1, &spec, &bindings, &dbs) != 0) {
                evaluation_error = 1;
                break;
            }

            size_t pos_idx = 0;
            for (size_t j = 0; j < rule->body_count; j++) {
                if (rule->body[j].negated)
                    continue;
                const s_spec_fact *sub = &rule->body[j];
                int is_idb = 0;
                for (size_t k = 0; k < idb_preds_count; k++) {
                    if (sub->p && strcmp(idb_preds[k], sub->p) == 0) {
                        is_idb = 1;
                        break;
                    }
                }
                dbs[pos_idx++] = is_idb ? old_db : facts;
            }

            s_eval_cb_data cb_data;
            cb_data.target_db = delta_db;
            cb_data.rule = rule;
            cb_data.old_db = old_db;
            cb_data.main_facts = facts;
            cb_data.derived_count = 0;
            cb_data.is_deletion = 0;
            cb_data.skip_negated_check_idx = -1;
            cb_data.rule_index = r;
            cb_data.evaluation_error = &evaluation_error;

            facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);

            free(dbs);
            free(spec);
            free(bindings);
        }

        if (evaluation_error) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            break;
        }

        size_t delta_size = facts_count(delta_db);
        if (delta_size == 0) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            continue;
        }

        /* Merge delta_db into old_db and register with the main database */
        size_t merged_derived = 0;
        if (facts_merge_count(old_db, delta_db, NULL) != 0 || facts_merge_derived_count(facts, delta_db, &merged_derived) != 0) {
            evaluation_error = 1;
        }
        total_derived += merged_derived;

        /* 4. Semi-Naive Fixed-point loop */
        while (1) {
            if (facts_reset_checked(new_db) != 0 || facts_reset_checked(old_plus_delta_db) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_merge_count(old_plus_delta_db, old_db, NULL) != 0 ||
                facts_merge_count(old_plus_delta_db, delta_db, NULL) != 0) {
                evaluation_error = 1;
                break;
            }

            size_t iter_derived = 0;

            for (size_t r = 0; r < prog->rule_count; r++) {
                if (rule_strata[r] != s)
                    continue;
                const s_datalog_rule *rule = &prog->rules[r];

                /* Identify which subgoals are recursive (current stratum IDB predicates) */
                int recursive_subgoal_count = 0;
                int *recursive_subgoal_indices =
                    malloc((rule->body_count ? rule->body_count : 1) * sizeof(*recursive_subgoal_indices));
                if (!recursive_subgoal_indices) {
                    evaluation_error = 1;
                    break;
                }
                for (size_t j = 0; j < rule->body_count; j++) {
                    const s_spec_fact *sub = &rule->body[j];
                    if (sub->p && sub->p[0] != '?') {
                        int is_recursive = 0;
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
                    /* Non-recursive rules are only run in Iteration 0 */
                    free(recursive_subgoal_indices);
                    continue;
                }

                /* Evaluate the rule version for each recursive subgoal */
                for (int v = 0; v < recursive_subgoal_count; v++) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, -1, &spec, &bindings, &dbs) != 0) {
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
                    cb_data.old_db = old_db;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;
                    cb_data.is_deletion = 0;
                    cb_data.skip_negated_check_idx = -1;
                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;

                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);

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

            /* Update old_db and delta_db for next iteration, persist results to facts */
            if (facts_merge_count(old_db, delta_db, NULL) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_reset_checked(delta_db) != 0) {
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

        delete_facts(old_db);
        delete_facts(delta_db);
        delete_facts(new_db);
        delete_facts(old_plus_delta_db);
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

int facts_datalog_eval_incremental(s_facts *facts, const s_datalog_program *prog, const s_rollback_entry *delta, size_t delta_count)
{
    if (!facts || !prog || !delta || delta_count == 0)
        return 0;
    if (prog->rule_count == 0)
        return 0;

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    if (!rule_strata)
        return -1;

    size_t total_derived = 0;
    int evaluation_error = 0;
    s_facts *cumulative_delta_db = new_facts(facts->symbols, 256);
    s_facts *cumulative_minus_db = new_facts(facts->symbols, 256);
    if (!cumulative_delta_db || !cumulative_minus_db) {
        delete_facts(cumulative_delta_db);
        delete_facts(cumulative_minus_db);
        free(rule_strata);
        return -1;
    }

    for (size_t i = 0; i < delta_count; i++) {
        if (delta[i].action == ROLLBACK_REMOVE) {
            if (!facts_add_spo(cumulative_delta_db, symbol_to_str(delta[i].fact.s), symbol_to_str(delta[i].fact.p),
                               symbol_to_str(delta[i].fact.o))) {
                evaluation_error = 1;
                break;
            }
        } else if (delta[i].action == ROLLBACK_ADD) {
            if (!facts_add_spo(cumulative_minus_db, symbol_to_str(delta[i].fact.s), symbol_to_str(delta[i].fact.p),
                               symbol_to_str(delta[i].fact.o))) {
                evaluation_error = 1;
                break;
            }
        }
    }

    /* Phase 1: Deletions */
    for (int s = 0; !evaluation_error && s < num_strata; s++) {
        s_facts *delta_db = new_facts(facts->symbols, 256);
        s_facts *new_db = new_facts(facts->symbols, 256);
        if (!delta_db || !new_db) {
            delete_facts(delta_db);
            delete_facts(new_db);
            evaluation_error = 1;
            break;
        }
        if (facts_merge_count(delta_db, cumulative_minus_db, NULL) != 0) {
            delete_facts(delta_db);
            delete_facts(new_db);
            evaluation_error = 1;
            break;
        }

        /* Negated insertions trigger deletions */
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] != s)
                continue;
            const s_datalog_rule *rule = &prog->rules[r];
            for (size_t j = 0; j < rule->body_count; j++) {
                if (rule->body[j].negated) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, (int)j, &spec, &bindings, &dbs) != 0) {
                        evaluation_error = 1;
                        break;
                    }
                    size_t pos_idx = 0;
                    for (size_t k = 0; k < rule->body_count; k++) {
                        if (!rule->body[k].negated || (int)k == (int)j) {
                            dbs[pos_idx++] = (k == j) ? cumulative_delta_db : facts;
                        }
                    }
                    s_eval_cb_data cb_data;
                    cb_data.target_db = new_db;
                    cb_data.rule = rule;
                    cb_data.old_db = new_db;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;
                    cb_data.is_deletion = 1;
                    cb_data.skip_negated_check_idx = (int)j;
                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;
                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);
                    free(dbs);
                    free(spec);
                    free(bindings);
                }
            }
        }

        if (facts_count(new_db) > 0) {
            s_facts_cursor fc;
            facts_cursor_init(new_db, &fc, new_db->index_spo, NULL, NULL);
            s_fact *f;
            while ((f = facts_cursor_next(&fc))) {
                facts_remove_spo_origin(facts, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o), FACT_ORIGIN_DERIVED);
            }
            facts_cursor_stop(&fc);

            if (facts_merge_count(delta_db, new_db, NULL) != 0)
                evaluation_error = 1;
            if (facts_reset_checked(new_db) != 0)
                evaluation_error = 1;
        }

        if (evaluation_error) {
            delete_facts(delta_db);
            delete_facts(new_db);
            break;
        }

        if (facts_count(delta_db) == 0) {
            delete_facts(delta_db);
            delete_facts(new_db);
            continue;
        }

        while (1) {
            if (facts_reset_checked(new_db) != 0) {
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
                    free(target_indices);
                    continue;
                }

                for (int v = 0; v < target_count; v++) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, -1, &spec, &bindings, &dbs) != 0) {
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
                    cb_data.old_db = new_db; /* Not used for deletions */
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;
                    cb_data.is_deletion = 1;
                    cb_data.skip_negated_check_idx = -1;
                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;

                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);
                    iter_derived += cb_data.derived_count;
                    free(dbs);
                    free(spec);
                    free(bindings);
                }
                free(target_indices);
            }

            if (evaluation_error || iter_derived == 0)
                break;

            s_facts_cursor fc;
            facts_cursor_init(new_db, &fc, new_db->index_spo, NULL, NULL);
            s_fact *f;
            while ((f = facts_cursor_next(&fc))) {
                facts_remove_spo_origin(facts, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o), FACT_ORIGIN_DERIVED);
            }
            facts_cursor_stop(&fc);

            if (facts_reset_checked(delta_db) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_merge_count(delta_db, new_db, NULL) != 0 || facts_merge_count(cumulative_minus_db, new_db, NULL) != 0) {
                evaluation_error = 1;
                break;
            }
        }
        delete_facts(delta_db);
        delete_facts(new_db);
    }
    for (int s = 0; !evaluation_error && s < num_strata; s++) {
        const char **idb_preds = NULL;
        size_t idb_preds_count = 0;
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] == s) {
                const s_datalog_rule *rule = &prog->rules[r];
                if (rule->head.p && rule->head.p[0] != '?') {
                    if (add_pred_to_list_eval(&idb_preds, &idb_preds_count, rule->head.p) != 0)
                        evaluation_error = 1;
                }
            }
        }

        if (evaluation_error) {
            free(idb_preds);
            break;
        }

        s_facts *old_db = new_facts(facts->symbols, 256);
        s_facts *delta_db = new_facts(facts->symbols, 256);
        s_facts *new_db = new_facts(facts->symbols, 256);
        s_facts *old_plus_delta_db = new_facts(facts->symbols, 256);
        if (!old_db || !delta_db || !new_db || !old_plus_delta_db) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            evaluation_error = 1;
            break;
        }

        if (facts_merge_count(delta_db, cumulative_delta_db, NULL) != 0) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            evaluation_error = 1;
            break;
        }

        /* Negated deletions trigger insertions */
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] != s)
                continue;
            const s_datalog_rule *rule = &prog->rules[r];
            for (size_t j = 0; j < rule->body_count; j++) {
                if (rule->body[j].negated) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, (int)j, &spec, &bindings, &dbs) != 0) {
                        evaluation_error = 1;
                        break;
                    }
                    size_t pos_idx = 0;
                    for (size_t k = 0; k < rule->body_count; k++) {
                        if (!rule->body[k].negated || (int)k == (int)j) {
                            dbs[pos_idx++] = (k == j) ? cumulative_minus_db : facts;
                        }
                    }
                    s_eval_cb_data cb_data;
                    cb_data.target_db = new_db;
                    cb_data.rule = rule;
                    cb_data.old_db = new_db;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;
                    cb_data.is_deletion = 0;
                    cb_data.skip_negated_check_idx = (int)j;
                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;
                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);
                    free(dbs);
                    free(spec);
                    free(bindings);
                }
            }
        }

        if (facts_count(new_db) > 0) {
            s_facts_cursor fc;
            facts_cursor_init(new_db, &fc, new_db->index_spo, NULL, NULL);
            s_fact *f;
            while ((f = facts_cursor_next(&fc))) {
                if (!facts_add_spo_origin(facts, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o),
                                          FACT_ORIGIN_DERIVED)) {
                    evaluation_error = 1;
                    break;
                }
            }
            facts_cursor_stop(&fc);

            if (facts_merge_count(delta_db, new_db, NULL) != 0 || facts_merge_count(cumulative_delta_db, new_db, NULL) != 0)
                evaluation_error = 1;
            if (facts_reset_checked(new_db) != 0)
                evaluation_error = 1;
        }

        if (evaluation_error) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            break;
        }

        if (facts_count(delta_db) == 0) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            continue;
        }

        if (facts_merge_count(old_db, delta_db, NULL) != 0) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            evaluation_error = 1;
            break;
        }
        while (1) {
            if (facts_reset_checked(new_db) != 0 || facts_reset_checked(old_plus_delta_db) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_merge_count(old_plus_delta_db, old_db, NULL) != 0 ||
                facts_merge_count(old_plus_delta_db, delta_db, NULL) != 0) {
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
                    free(target_indices);
                    continue;
                }

                for (int v = 0; v < target_count; v++) {
                    p_spec spec;
                    s_binding *bindings;
                    s_facts **dbs;
                    if (prepare_rule_query(rule, -1, &spec, &bindings, &dbs) != 0) {
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
                    cb_data.old_db = old_db;
                    cb_data.main_facts = facts;
                    cb_data.derived_count = 0;
                    cb_data.is_deletion = 0;
                    cb_data.skip_negated_check_idx = -1;
                    cb_data.rule_index = r;
                    cb_data.evaluation_error = &evaluation_error;

                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);
                    iter_derived += cb_data.derived_count;
                    free(dbs);
                    free(spec);
                    free(bindings);
                }
                free(target_indices);
            }

            if (evaluation_error || iter_derived == 0)
                break;

            if (facts_merge_count(old_db, delta_db, NULL) != 0) {
                evaluation_error = 1;
                break;
            }
            if (facts_reset_checked(delta_db) != 0) {
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

        delete_facts(old_db);
        delete_facts(delta_db);
        delete_facts(new_db);
        delete_facts(old_plus_delta_db);
        free(idb_preds);
    }

    delete_facts(cumulative_delta_db);
    delete_facts(cumulative_minus_db);
    free(rule_strata);
    return evaluation_error ? -1 : (int)total_derived;
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

static int invalidation_event_append(s_invalidation_event **events, size_t *count, const char *s, const char *p,
                                     const char *o, int negated)
{
    for (size_t i = 0; i < *count; i++) {
        if ((*events)[i].negated == negated && strcmp((*events)[i].s, s) == 0 &&
            strcmp((*events)[i].p, p) == 0 && strcmp((*events)[i].o, o) == 0)
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

static int facts_invalidate_justifications(s_facts *facts, const s_rollback_entry *entries, size_t entry_count,
                                           int *requires_closure_scan)
{
    s_invalidation_event *events = NULL;
    size_t event_count = 0;
    *requires_closure_scan = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].action == ROLLBACK_ADD) {
            *requires_closure_scan = 1;
            if (invalidation_event_append(&events, &event_count, symbol_to_str(entries[i].fact.s),
                                          symbol_to_str(entries[i].fact.p), symbol_to_str(entries[i].fact.o), 0) != 0)
                goto error;
        } else if (entries[i].action == ROLLBACK_REMOVE) {
            if (invalidation_event_append(&events, &event_count, symbol_to_str(entries[i].fact.s),
                                          symbol_to_str(entries[i].fact.p), symbol_to_str(entries[i].fact.o), 1) != 0)
                goto error;
        }
    }

    for (size_t cursor = 0; cursor < event_count; cursor++) {
        s_justification_support *conclusions = NULL;
        size_t conclusion_count = 0;
        int removed = justification_graph_remove_support(
            facts->justifications_staging, events[cursor].s, events[cursor].p, events[cursor].o,
            events[cursor].negated, &conclusions, &conclusion_count);
        if (removed < 0) {
            justification_support_array_destroy(conclusions, conclusion_count);
            goto error;
        }
        if (events[cursor].negated && removed > 0)
            *requires_closure_scan = 1;

        for (size_t i = 0; i < conclusion_count; i++) {
            s_justification_support *conclusion = &conclusions[i];
            if (justification_graph_count(facts->justifications_staging, conclusion->s, conclusion->p,
                                          conclusion->o) != 0)
                continue;
            s_fact_support support;
            int found = facts_get_support_spo(facts, conclusion->s, conclusion->p, conclusion->o, &support);
            if (found < 0) {
                justification_support_array_destroy(conclusions, conclusion_count);
                goto error;
            }
            if (found > 0 && support.derived) {
                int disappears = support.asserted == 0;
                if (facts_remove_spo_origin(facts, conclusion->s, conclusion->p, conclusion->o,
                                            FACT_ORIGIN_DERIVED) < 0) {
                    justification_support_array_destroy(conclusions, conclusion_count);
                    goto error;
                }
                if (disappears && invalidation_event_append(&events, &event_count, conclusion->s, conclusion->p,
                                                            conclusion->o, 0) != 0) {
                    justification_support_array_destroy(conclusions, conclusion_count);
                    goto error;
                }
            }
        }
        justification_support_array_destroy(conclusions, conclusion_count);
    }
    invalidation_events_destroy(events, event_count);
    return 0;

error:
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

    int requires_closure_scan = 0;
    int result = facts_invalidate_justifications(facts, entries, entry_count, &requires_closure_scan);
    if (result == 0) {
        result = requires_closure_scan ? facts_datalog_eval(facts, facts->prog)
                                       : facts_datalog_eval_incremental(facts, facts->prog, entries, entry_count);
    }
    facts->disable_listener = 0;
    if (result < 0)
        facts_justifications_discard(facts);
    return result < 0 ? -1 : 0;
}

int rete_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    return reactive_tx_listener(facts, entries, entry_count, user_data);
}
