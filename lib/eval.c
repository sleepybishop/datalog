#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "eval.h"
#include "lftj.h"

typedef struct eval_cb_data {
    s_facts *target_db;
    const s_datalog_rule *rule;
    s_facts *old_db;
    s_facts *main_facts;
    size_t derived_count;
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
            if (facts_get_spo(data->main_facts, s_val, p_val, o_val)) {
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

    /* Check if the fact already exists in the main database, or in old_db, or in the target_db */
    if (facts_get_spo(data->main_facts, s_val, p_val, o_val)) {
        return;
    }
    if (facts_get_spo(data->old_db, s_val, p_val, o_val)) {
        return;
    }
    if (facts_get_spo(data->target_db, s_val, p_val, o_val)) {
        return;
    }

    /* Insert new unique fact */
    if (facts_add_spo(data->target_db, s_val, p_val, o_val)) {
        data->derived_count++;
    }
}

static size_t facts_merge_count(s_facts *dest, s_facts *src)
{
    size_t count = 0;
    s_facts_cursor c;
    facts_with_0(src, &c, NULL, NULL, NULL);
    s_fact *f;
    while ((f = facts_cursor_next(&c)) != NULL) {
        if (facts_add_spo(dest, symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o))) {
            count++;
        }
    }
    facts_cursor_stop(&c);
    return count;
}

static void add_pred_to_list_eval(const char ***list, size_t *count, const char *pred)
{
    if (!pred)
        return;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*list)[i], pred) == 0) {
            return;
        }
    }
    const char **new_list = realloc(*list, (*count + 1) * sizeof(char *));
    assert(new_list);
    *list = new_list;
    (*list)[*count] = pred;
    (*count)++;
}

static p_spec compile_positive_rule_body_to_spec(const s_datalog_rule *rule)
{
    size_t pos_count = 0;
    for (size_t i = 0; i < rule->body_count; i++) {
        if (!rule->body[i].negated) {
            pos_count++;
        }
    }
    if (pos_count == 0) {
        return NULL;
    }
    const char **spec = calloc(pos_count * 4 + 2, sizeof(char *));
    assert(spec);
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

int facts_datalog_eval(s_facts *facts, const s_datalog_program *prog)
{
    assert(facts);
    assert(prog);

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    if (!rule_strata) {
        return -1;
    }

    size_t total_derived = 0;

    for (int s = 0; s < num_strata; s++) {
        /* 1. Identify all IDB predicates in stratum s */
        const char **idb_preds = NULL;
        size_t idb_preds_count = 0;
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] == s) {
                const s_datalog_rule *rule = &prog->rules[r];
                if (rule->head.p && rule->head.p[0] != '?') {
                    add_pred_to_list_eval(&idb_preds, &idb_preds_count, rule->head.p);
                }
            }
        }

        if (idb_preds_count == 0) {
            continue;
        }

        /* 2. Create query-local databases for stratum evaluation */
        s_facts *old_db = new_facts(facts->symbols, 100000);
        s_facts *delta_db = new_facts(facts->symbols, 100000);
        s_facts *new_db = new_facts(facts->symbols, 100000);
        s_facts *old_plus_delta_db = new_facts(facts->symbols, 100000);

        /* 3. Iteration 0: Naive evaluation to seed delta_db */
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] != s)
                continue;
            const s_datalog_rule *rule = &prog->rules[r];

            p_spec spec = compile_positive_rule_body_to_spec(rule);
            s_binding *bindings = spec_bindings(spec);
            s_facts *dbs[32];

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

            facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);

            free(spec);
            free(bindings);
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
        facts_merge_count(old_db, delta_db);
        total_derived += facts_merge_count(facts, delta_db);

        /* 4. Semi-Naive Fixed-point loop */
        int iteration = 1;
        while (1) {
            facts_reset(new_db);
            facts_reset(old_plus_delta_db);
            facts_merge_count(old_plus_delta_db, old_db);
            facts_merge_count(old_plus_delta_db, delta_db);

            size_t iter_derived = 0;

            for (size_t r = 0; r < prog->rule_count; r++) {
                if (rule_strata[r] != s)
                    continue;
                const s_datalog_rule *rule = &prog->rules[r];

                /* Identify which subgoals are recursive (current stratum IDB predicates) */
                int recursive_subgoal_count = 0;
                int recursive_subgoal_indices[32];
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
                    continue;
                }

                /* Evaluate the rule version for each recursive subgoal */
                for (int v = 0; v < recursive_subgoal_count; v++) {
                    p_spec spec = compile_positive_rule_body_to_spec(rule);
                    s_binding *bindings = spec_bindings(spec);
                    s_facts *dbs[32];

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

                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);

                    iter_derived += cb_data.derived_count;

                    free(spec);
                    free(bindings);
                }
            }

            if (iter_derived == 0) {
                break;
            }

            /* Update old_db and delta_db for next iteration, persist results to facts */
            facts_merge_count(old_db, delta_db);
            facts_reset(delta_db);
            facts_merge_count(delta_db, new_db);
            total_derived += facts_merge_count(facts, new_db);

            iteration++;
        }

        delete_facts(old_db);
        delete_facts(delta_db);
        delete_facts(new_db);
        delete_facts(old_plus_delta_db);
        free(idb_preds);
    }

    free(rule_strata);
    return (int)total_derived;
}

int facts_datalog_eval_incremental(s_facts *facts, const s_datalog_program *prog, const s_rollback_entry *delta, size_t delta_count)
{
    if (!facts || !prog || !delta || delta_count == 0)
        return 0;

    int num_strata = 0;
    int *rule_strata = datalog_program_stratify(prog, &num_strata);
    if (!rule_strata)
        return -1;

    size_t total_derived = 0;
    s_facts *cumulative_delta_db = new_facts(facts->symbols, 100000);

    for (size_t i = 0; i < delta_count; i++) {
        if (delta[i].action == ROLLBACK_REMOVE) {
            facts_add_spo(cumulative_delta_db, symbol_to_str(delta[i].fact.s), symbol_to_str(delta[i].fact.p),
                          symbol_to_str(delta[i].fact.o));
        }
    }

    for (int s = 0; s < num_strata; s++) {
        const char **idb_preds = NULL;
        size_t idb_preds_count = 0;
        for (size_t r = 0; r < prog->rule_count; r++) {
            if (rule_strata[r] == s) {
                const s_datalog_rule *rule = &prog->rules[r];
                if (rule->head.p && rule->head.p[0] != '?') {
                    add_pred_to_list_eval(&idb_preds, &idb_preds_count, rule->head.p);
                }
            }
        }

        s_facts *old_db = new_facts(facts->symbols, 100000);
        s_facts *delta_db = new_facts(facts->symbols, 100000);
        s_facts *new_db = new_facts(facts->symbols, 100000);
        s_facts *old_plus_delta_db = new_facts(facts->symbols, 100000);

        facts_merge_count(delta_db, cumulative_delta_db);

        if (facts_count(delta_db) == 0) {
            delete_facts(old_db);
            delete_facts(delta_db);
            delete_facts(new_db);
            delete_facts(old_plus_delta_db);
            free(idb_preds);
            continue;
        }

        facts_merge_count(old_db, delta_db);

        while (1) {
            facts_reset(new_db);
            facts_reset(old_plus_delta_db);
            facts_merge_count(old_plus_delta_db, old_db);
            facts_merge_count(old_plus_delta_db, delta_db);

            size_t iter_derived = 0;
            for (size_t r = 0; r < prog->rule_count; r++) {
                if (rule_strata[r] != s)
                    continue;
                const s_datalog_rule *rule = &prog->rules[r];

                int target_count = 0;
                int target_indices[32];
                for (size_t j = 0; j < rule->body_count; j++) {
                    if (!rule->body[j].negated) {
                        target_indices[target_count++] = (int)j;
                    }
                }

                if (target_count == 0)
                    continue;

                for (int v = 0; v < target_count; v++) {
                    p_spec spec = compile_positive_rule_body_to_spec(rule);
                    s_binding *bindings = spec_bindings(spec);
                    s_facts *dbs[32];

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

                    facts_lftj_solve_multi(facts, dbs, spec, bindings, eval_solution_cb, &cb_data);
                    iter_derived += cb_data.derived_count;
                    free(spec);
                    free(bindings);
                }
            }

            if (iter_derived == 0)
                break;

            facts_merge_count(old_db, delta_db);
            facts_reset(delta_db);
            facts_merge_count(delta_db, new_db);
            facts_merge_count(cumulative_delta_db, new_db);
            total_derived += facts_merge_count(facts, new_db);
        }

        delete_facts(old_db);
        delete_facts(delta_db);
        delete_facts(new_db);
        delete_facts(old_plus_delta_db);
        free(idb_preds);
    }
    delete_facts(cumulative_delta_db);
    free(rule_strata);
    return (int)total_derived;
}

void rete_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    (void)user_data;
    if (facts->disable_listener) {
        return;
    }
    if (!facts->prog) {
        return;
    }

    facts->disable_listener = 1;
    facts_datalog_eval_incremental(facts, facts->prog, entries, entry_count);
    facts->disable_listener = 0;
}
