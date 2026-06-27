#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lftj.h"
#include "facts.h"
#include "arena.h"
#include "hexastore.h"

static int compare_val(Symbol va, Symbol vb)
{
    return compare_symbol(va, vb);
}

static inline uint64_t get_symbol_id(Symbol ptr)
{
    if (ptr == P_FIRST)
        return 0;
    if (ptr == P_LAST)
        return 0xFFFFFFFFFFFFFFFFULL;
    return ptr->id;
}

static inline void encode_uint64_be(unsigned char *buf, uint64_t val)
{
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8) & 0xFF;
    buf[7] = val & 0xFF;
}

static inline Symbol fact_get_col(s_fact *f, int col)
{
    return (col == 0) ? f->s : ((col == 1) ? f->p : f->o);
}

void init_lftj_iterator(s_lftj_iterator *it, struct rax *symbols, int col1, int col2, int col3)
{
    it->symbols = symbols;
    it->depth = 0;
    it->col1 = col1;
    it->col2 = col2;
    it->col3 = col3;
    raxStart(&it->it, symbols);
}

s_lftj_iterator *new_lftj_iterator(struct rax *symbols, int col1, int col2, int col3)
{
    s_lftj_iterator *it = malloc(sizeof(s_lftj_iterator));
    if (it) {
        init_lftj_iterator(it, symbols, col1, col2, col3);
    }
    return it;
}

void delete_lftj_iterator(s_lftj_iterator *it)
{
    if (it) {
        raxStop(&it->it);
        free(it);
    }
}

Symbol iterator_key(s_lftj_iterator *it)
{
    if (raxEOF(&it->it))
        return NULL;
    if (it->depth == 1) {
        return it->val1;
    } else if (it->depth == 2) {
        return it->val2;
    } else if (it->depth == 3) {
        return it->val3;
    }
    return NULL;
}

int iterator_open(s_lftj_iterator *it)
{
    unsigned char query_key[24];
    memset(query_key, 0, 24);

    if (it->depth == 0) {
        raxSeek(&it->it, "^", NULL, 0);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        it->val1 = fact_get_col(f, it->col1);
        it->depth = 1;
        return 0;
    }
    if (it->depth == 1) {
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        raxSeek(&it->it, ">=", query_key, 24);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1)
            return -1;
        it->val2 = fact_get_col(f, it->col2);
        it->depth = 2;
        return 0;
    } else if (it->depth == 2) {
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(it->val2));
        raxSeek(&it->it, ">=", query_key, 24);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1 || fact_get_col(f, it->col2) != it->val2)
            return -1;
        it->val3 = fact_get_col(f, it->col3);
        it->depth = 3;
        return 0;
    }
    return -1;
}

int iterator_up(s_lftj_iterator *it)
{
    if (it->depth > 0) {
        it->depth--;
        return 0;
    }
    return -1;
}

int iterator_seek(s_lftj_iterator *it, Symbol key)
{
    unsigned char query_key[24];
    memset(query_key, 0, 24);

    if (it->depth == 1) {
        encode_uint64_be(query_key, get_symbol_id(key));
        raxSeek(&it->it, ">=", query_key, 24);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        it->val1 = fact_get_col(f, it->col1);
        return 0;
    } else if (it->depth == 2) {
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(key));
        raxSeek(&it->it, ">=", query_key, 24);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1)
            return -1;
        it->val2 = fact_get_col(f, it->col2);
        return 0;
    } else if (it->depth == 3) {
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(it->val2));
        encode_uint64_be(query_key + 16, get_symbol_id(key));
        raxSeek(&it->it, ">=", query_key, 24);
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1 || fact_get_col(f, it->col2) != it->val2)
            return -1;
        it->val3 = fact_get_col(f, it->col3);
        return 0;
    }
    return -1;
}

int iterator_next(s_lftj_iterator *it)
{
    if (it->depth == 1) {
        while (1) {
            if (raxEOF(&it->it))
                return -1;
            s_fact *f = it->it.data;
            Symbol current_val = fact_get_col(f, it->col1);
            if (current_val != it->val1) {
                it->val1 = current_val;
                return 0;
            }
            it->it.flags &= ~RAX_ITER_JUST_SEEKED;
            raxNext(&it->it);
        }
    } else if (it->depth == 2) {
        while (1) {
            if (raxEOF(&it->it))
                return -1;
            s_fact *f = it->it.data;
            if (fact_get_col(f, it->col1) != it->val1)
                return -1;
            Symbol current_val = fact_get_col(f, it->col2);
            if (current_val != it->val2) {
                it->val2 = current_val;
                return 0;
            }
            it->it.flags &= ~RAX_ITER_JUST_SEEKED;
            raxNext(&it->it);
        }
    } else if (it->depth == 3) {
        it->it.flags &= ~RAX_ITER_JUST_SEEKED;
        raxNext(&it->it);
        if (raxEOF(&it->it)) return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1 || fact_get_col(f, it->col2) != it->val2) return -1;
        it->val3 = fact_get_col(f, it->col3);
        return 0;
    }
    return -1;
}

/* Helper to sort iterators by current key */
static void sort_iterators(s_lftj_iterator **arr, int count)
{
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            Symbol key_a = iterator_key(arr[j]);
            Symbol key_b = iterator_key(arr[j + 1]);
            if (!key_a || (key_b && compare_val(key_a, key_b) > 0)) {
                s_lftj_iterator *tmp = arr[j];
                arr[j] = arr[j + 1];
                arr[j + 1] = tmp;
            }
        }
    }
}

/* Leapfrog intersection search */
static Symbol leapfrog_search(s_lftj_iterator **arr, int count)
{
    if (count == 0)
        return NULL;
    if (count == 1) {
        return iterator_key(arr[0]);
    }

    sort_iterators(arr, count);
    int p = 0;
    while (1) {
        Symbol max_key = iterator_key(arr[(p + count - 1) % count]);
        Symbol cur_key = iterator_key(arr[p]);
        if (!max_key || !cur_key)
            return NULL;
        if (compare_val(cur_key, max_key) == 0) {
            return cur_key;
        }
        if (iterator_seek(arr[p], max_key) < 0) {
            return NULL;
        }
        p = (p + 1) % count;
    }
}

typedef struct lftj_subgoal {
    Symbol s_sym;
    Symbol p_sym;
    Symbol o_sym;
    const char *s_var;
    const char *p_var;
    const char *o_var;
} s_lftj_subgoal;

static Symbol get_bound_var_sym(s_facts *facts, const char *var_name, s_binding *bindings)
{
    if (!var_name || var_name[0] != '?')
        return NULL;
    const char **val_ptr = bindings_get(bindings, var_name);
    if (val_ptr && *val_ptr) {
        return facts_find_symbol_str(facts, *val_ptr);
    }
    return NULL;
}

/* Recursive Leapfrog solver */
static int lftj_solve_rec(s_facts *facts, s_lftj_subgoal *subgoals, s_binding *bindings, int var_idx, const char **vars,
                          int vars_count, s_lftj_iterator **iterators, int subgoal_count, int iterator_cols[][3],
                          void (*cb)(s_binding *bindings, void *user_data), void *user_data)
{
    if (var_idx == vars_count) {
        if (cb) {
            cb(bindings, user_data);
        }
        return 1;
    }

    const char *var_name = vars[var_idx];
    const char **bound_slot = bindings_get(bindings, var_name);

    s_lftj_iterator *active_iters[32];
    int active_count = 0;
    int opened_levels[32];
    memset(opened_levels, 0, sizeof(opened_levels));

    for (int i = 0; i < subgoal_count; i++) {
        s_lftj_subgoal *sub = &subgoals[i];
        s_lftj_iterator *it = iterators[i];

        while (it->depth < 3) {
            int col = iterator_cols[i][it->depth];
            Symbol val_sym = (col == 0) ? sub->s_sym : ((col == 1) ? sub->p_sym : sub->o_sym);
            const char *val_var = (col == 0) ? sub->s_var : ((col == 1) ? sub->p_var : sub->o_var);

            Symbol bound_sym = NULL;
            if (val_var && val_var[0] == '?') {
                bound_sym = get_bound_var_sym(facts, val_var, bindings);
            }

            if (val_var == NULL) { /* Constant */
                int r1 = iterator_open(it);
                int r2 = r1 == 0 ? iterator_seek(it, val_sym) : -1;
                Symbol k = iterator_key(it);
                if (r1 != 0 || r2 != 0 || k != val_sym) {
                    goto backtrack_temp;
                }
                opened_levels[i]++;
            } else if (bound_sym != NULL) { /* Already bound variable -> treat as constant */
                if (iterator_open(it) != 0 || iterator_seek(it, bound_sym) != 0 || iterator_key(it) != bound_sym) {
                    goto backtrack_temp;
                }
                opened_levels[i]++;
            } else if (strcmp(val_var, var_name) == 0) {
                if (iterator_open(it) != 0) {
                    goto backtrack_temp;
                }
                opened_levels[i]++;
                active_iters[active_count++] = it;
                break;
            } else {
                break;
            }
        }
        continue;

    backtrack_temp:
        for (int k = 0; k <= i; k++) {
            while (opened_levels[k] > 0) {
                iterator_up(iterators[k]);
                opened_levels[k]--;
            }
        }
        return 0;
    }

    int found_solutions = 0;
    if (active_count > 0) {
        Symbol match_val = leapfrog_search(active_iters, active_count);
        while (match_val) {
            *bound_slot = symbol_to_str(match_val);

            found_solutions += lftj_solve_rec(facts, subgoals, bindings, var_idx + 1, vars, vars_count, iterators, subgoal_count,
                                              iterator_cols, cb, user_data);

            if (iterator_next(active_iters[0]) < 0) {
                break;
            }
            match_val = leapfrog_search(active_iters, active_count);
        }
    } else {
        found_solutions += lftj_solve_rec(facts, subgoals, bindings, var_idx + 1, vars, vars_count, iterators, subgoal_count,
                                          iterator_cols, cb, user_data);
    }

    for (int i = 0; i < subgoal_count; i++) {
        while (opened_levels[i] > 0) {
            iterator_up(iterators[i]);
            opened_levels[i]--;
        }
    }
    *bound_slot = NULL;
    return found_solutions;
}

/* Entrypoint for Leapfrog Triejoin query evaluation */
int facts_lftj_solve(s_facts *facts, p_spec spec, s_binding *bindings)
{
    int subgoal_count = spec_count_facts(spec);
    if (subgoal_count == 0)
        return 0;

    s_lftj_subgoal subgoals[32];
    for (int i = 0; i < subgoal_count; i++) {
        s_spec_fact *f = (s_spec_fact *)(spec + i * 4);

        subgoals[i].s_var = (f->s && f->s[0] == '?') ? f->s : NULL;
        subgoals[i].s_sym = subgoals[i].s_var ? NULL : facts_find_symbol_str(facts, f->s);

        subgoals[i].p_var = (f->p && f->p[0] == '?') ? f->p : NULL;
        subgoals[i].p_sym = subgoals[i].p_var ? NULL : facts_find_symbol_str(facts, f->p);

        subgoals[i].o_var = (f->o && f->o[0] == '?') ? f->o : NULL;
        subgoals[i].o_sym = subgoals[i].o_var ? NULL : facts_find_symbol_str(facts, f->o);

        /* If a constant is not found in the database symbols, the query has 0 solutions! */
        if ((!subgoals[i].s_var && !subgoals[i].s_sym) || (!subgoals[i].p_var && !subgoals[i].p_sym) ||
            (!subgoals[i].o_var && !subgoals[i].o_sym)) {
            return 0;
        }
    }

    const char *vars[128];
    int vars_count = 0;
    for (int i = 0; i < subgoal_count; i++) {
        s_lftj_subgoal *sub = &subgoals[i];
        if (sub->s_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->s_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->s_var;
        }
        if (sub->p_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->p_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->p_var;
        }
        if (sub->o_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->o_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->o_var;
        }
    }

    static const int trie_candidate_cols[3][3] = {
        {0, 1, 2}, /* trie_spo */
        {1, 2, 0}, /* trie_pos */
        {2, 0, 1}  /* trie_osp */
    };

    s_lftj_iterator iterators_storage[32];
    s_lftj_iterator *iterators[32];
    int iterator_cols[32][3];

    for (int i = 0; i < subgoal_count; i++) {
        s_lftj_subgoal *sub = &subgoals[i];
        int best_cand = -1;
        int max_consts = -1;

        for (int cand = 0; cand < 3; cand++) {
            const int *cols = trie_candidate_cols[cand];
            int last_var_priority = -1;
            int is_valid = 1;
            int constants_before_vars = 0;
            int seen_var = 0;

            for (int c = 0; c < 3; c++) {
                int col = cols[c];
                const char *val_var = (col == 0) ? sub->s_var : ((col == 1) ? sub->p_var : sub->o_var);
                if (val_var) {
                    seen_var = 1;
                    int priority = -1;
                    for (int v = 0; v < vars_count; v++) {
                        if (strcmp(vars[v], val_var) == 0) {
                            priority = v;
                            break;
                        }
                    }
                    if (priority < last_var_priority) {
                        is_valid = 0;
                        break;
                    }
                    last_var_priority = priority;
                } else {
                    if (!seen_var) {
                        constants_before_vars++;
                    }
                }
            }

            if (is_valid) {
                if (constants_before_vars > max_consts) {
                    max_consts = constants_before_vars;
                    best_cand = cand;
                }
            }
        }

        if (best_cand == -1) {
            best_cand = 0;
        }

        struct rax *t = (best_cand == 0) ? facts->hexastore->trie_spo
                                         : ((best_cand == 1) ? facts->hexastore->trie_pos : facts->hexastore->trie_osp);
        const int *cols = trie_candidate_cols[best_cand];
        iterators[i] = &iterators_storage[i];
        init_lftj_iterator(iterators[i], t, cols[0], cols[1], cols[2]);
        memcpy(iterator_cols[i], cols, 3 * sizeof(int));
    }

    int solutions =
        lftj_solve_rec(facts, subgoals, bindings, 0, vars, vars_count, iterators, subgoal_count, iterator_cols, NULL, NULL);

    for (int i = 0; i < subgoal_count; i++) {
        raxStop(&iterators_storage[i].it);
    }

    return solutions;
}

int facts_lftj_solve_multi(s_facts *facts, s_facts **dbs, p_spec spec, s_binding *bindings,
                           void (*cb)(s_binding *bindings, void *user_data), void *user_data)
{
    int subgoal_count = spec_count_facts(spec);
    if (subgoal_count == 0)
        return 0;

    s_lftj_subgoal subgoals[32];
    for (int i = 0; i < subgoal_count; i++) {
        s_spec_fact *f = (s_spec_fact *)(spec + i * 4);

        subgoals[i].s_var = (f->s && f->s[0] == '?') ? f->s : NULL;
        subgoals[i].s_sym = subgoals[i].s_var ? NULL : facts_find_symbol_str(facts, f->s);

        subgoals[i].p_var = (f->p && f->p[0] == '?') ? f->p : NULL;
        subgoals[i].p_sym = subgoals[i].p_var ? NULL : facts_find_symbol_str(facts, f->p);

        subgoals[i].o_var = (f->o && f->o[0] == '?') ? f->o : NULL;
        subgoals[i].o_sym = subgoals[i].o_var ? NULL : facts_find_symbol_str(facts, f->o);

        /* If a constant is not found in the database symbols, the query has 0 solutions! */
        if ((!subgoals[i].s_var && !subgoals[i].s_sym) || (!subgoals[i].p_var && !subgoals[i].p_sym) ||
            (!subgoals[i].o_var && !subgoals[i].o_sym)) {
            return 0;
        }
    }

    const char *vars[128];
    int vars_count = 0;
    for (int i = 0; i < subgoal_count; i++) {
        s_lftj_subgoal *sub = &subgoals[i];
        if (sub->s_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->s_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->s_var;
        }
        if (sub->p_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->p_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->p_var;
        }
        if (sub->o_var) {
            int exists = 0;
            for (int k = 0; k < vars_count; k++) {
                if (strcmp(vars[k], sub->o_var) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (!exists)
                vars[vars_count++] = sub->o_var;
        }
    }

    static const int trie_candidate_cols[3][3] = {
        {0, 1, 2}, /* trie_spo */
        {1, 2, 0}, /* trie_pos */
        {2, 0, 1}  /* trie_osp */
    };

    s_lftj_iterator iterators_storage[32];
    s_lftj_iterator *iterators[32];
    int iterator_cols[32][3];

    for (int i = 0; i < subgoal_count; i++) {
        s_lftj_subgoal *sub = &subgoals[i];
        int best_cand = -1;
        int max_consts = -1;

        for (int cand = 0; cand < 3; cand++) {
            const int *cols = trie_candidate_cols[cand];
            int last_var_priority = -1;
            int is_valid = 1;
            int constants_before_vars = 0;
            int seen_var = 0;

            for (int c = 0; c < 3; c++) {
                int col = cols[c];
                const char *val_var = (col == 0) ? sub->s_var : ((col == 1) ? sub->p_var : sub->o_var);
                if (val_var) {
                    seen_var = 1;
                    int priority = -1;
                    for (int v = 0; v < vars_count; v++) {
                        if (strcmp(vars[v], val_var) == 0) {
                            priority = v;
                            break;
                        }
                    }
                    if (priority < last_var_priority) {
                        is_valid = 0;
                        break;
                    }
                    last_var_priority = priority;
                } else {
                    if (!seen_var) {
                        constants_before_vars++;
                    }
                }
            }

            if (is_valid) {
                if (constants_before_vars > max_consts) {
                    max_consts = constants_before_vars;
                    best_cand = cand;
                }
            }
        }

        if (best_cand == -1) {
            best_cand = 0;
        }

        s_facts *target_db = dbs[i] ? dbs[i] : facts;
        struct rax *t = (best_cand == 0) ? target_db->hexastore->trie_spo
                                         : ((best_cand == 1) ? target_db->hexastore->trie_pos : target_db->hexastore->trie_osp);
        const int *cols = trie_candidate_cols[best_cand];
        iterators[i] = &iterators_storage[i];
        init_lftj_iterator(iterators[i], t, cols[0], cols[1], cols[2]);
        memcpy(iterator_cols[i], cols, 3 * sizeof(int));
    }

    int solutions =
        lftj_solve_rec(facts, subgoals, bindings, 0, vars, vars_count, iterators, subgoal_count, iterator_cols, cb, user_data);

    for (int i = 0; i < subgoal_count; i++) {
        raxStop(&iterators_storage[i].it);
    }

    return solutions;
}
