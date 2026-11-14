#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lftj.h"
#include "facts.h"
#include "hexastore.h"

static inline Symbol fact_get_col(s_fact *f, int col)
{
    return (col == 0) ? f->s : ((col == 1) ? f->p : f->o);
}

static void init_lftj_iterator(s_lftj_iterator *it, struct rax *symbols, int col1, int col2, int col3)
{
    it->symbols = symbols;
    it->depth = 0;
    it->error = 0;
    it->col1 = col1;
    it->col2 = col2;
    it->col3 = col3;
    it->val1 = NULL;
    it->val2 = NULL;
    it->val3 = NULL;
    raxStart(&it->it, symbols);
}

s_lftj_iterator *new_lftj_iterator(struct rax *symbols, int col1, int col2, int col3)
{
    s_lftj_iterator *it = malloc(sizeof(s_lftj_iterator));
    if (it)
        init_lftj_iterator(it, symbols, col1, col2, col3);
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
    if (!it)
        return NULL;
    if (it->depth == 1)
        return it->val1;
    if (it->depth == 2)
        return it->val2;
    if (it->depth == 3)
        return it->val3;
    return NULL;
}

static int iterator_skip_removed(s_lftj_iterator *it)
{
    while (!raxEOF(&it->it)) {
        s_fact *fact = it->it.data;
        if (fact->asserted_count || fact->derived_count)
            return 1;
        it->it.flags &= ~RAX_ITER_JUST_SEEKED;
        if (!raxNext(&it->it) && errno == ENOMEM) {
            it->error = 1;
            return 0;
        }
    }
    return 1;
}

static int iterator_rax_seek(s_lftj_iterator *it, const char *op, unsigned char *key, size_t len)
{
    if (!raxSeek(&it->it, op, key, len)) {
        it->error = 1;
        return 0;
    }
    return iterator_skip_removed(it);
}

static int iterator_rax_next(s_lftj_iterator *it)
{
    if (!raxNext(&it->it) && errno == ENOMEM) {
        it->error = 1;
        return 0;
    }
    return iterator_skip_removed(it);
}

int iterator_open(s_lftj_iterator *it)
{
    unsigned char query_key[24];
    memset(query_key, 0, sizeof(query_key));

    if (it->depth == 0) {
        if (!iterator_rax_seek(it, "^", NULL, 0))
            return -1;
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        it->val1 = fact_get_col(f, it->col1);
        it->val2 = NULL;
        it->val3 = NULL;
        it->depth = 1;
        return 0;
    }
    if (it->depth == 1) {
        if (!it->val1)
            return -1;
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        if (!iterator_rax_seek(it, ">=", query_key, 24))
            return -1;
        if (raxEOF(&it->it))
            return -1;
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1)
            return -1;
        it->val2 = fact_get_col(f, it->col2);
        it->val3 = NULL;
        it->depth = 2;
        return 0;
    }
    if (it->depth == 2) {
        if (!it->val1 || !it->val2)
            return -1;
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(it->val2));
        if (!iterator_rax_seek(it, ">=", query_key, 24))
            return -1;
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
        if (it->depth == 3)
            it->val3 = NULL;
        else if (it->depth == 2)
            it->val2 = NULL;
        else
            it->val1 = NULL;
        it->depth--;
        return 0;
    }
    return -1;
}

int iterator_seek(s_lftj_iterator *it, Symbol key)
{
    unsigned char query_key[24];
    memset(query_key, 0, sizeof(query_key));

    if (it->depth == 1) {
        encode_uint64_be(query_key, get_symbol_id(key));
        if (!iterator_rax_seek(it, ">=", query_key, 24) || raxEOF(&it->it)) {
            it->val1 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        it->val1 = fact_get_col(f, it->col1);
        return 0;
    }
    if (it->depth == 2) {
        if (!it->val1)
            return -1;
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(key));
        if (!iterator_rax_seek(it, ">=", query_key, 24) || raxEOF(&it->it)) {
            it->val2 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1) {
            it->val2 = NULL;
            return -1;
        }
        it->val2 = fact_get_col(f, it->col2);
        return 0;
    }
    if (it->depth == 3) {
        if (!it->val1 || !it->val2)
            return -1;
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, get_symbol_id(it->val2));
        encode_uint64_be(query_key + 16, get_symbol_id(key));
        if (!iterator_rax_seek(it, ">=", query_key, 24) || raxEOF(&it->it)) {
            it->val3 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1 || fact_get_col(f, it->col2) != it->val2) {
            it->val3 = NULL;
            return -1;
        }
        it->val3 = fact_get_col(f, it->col3);
        return 0;
    }
    return -1;
}

int iterator_next(s_lftj_iterator *it)
{
    unsigned char query_key[24];

    if (it->depth == 1) {
        if (!it->val1)
            return -1;
        uint64_t id1 = get_symbol_id(it->val1);
        if (id1 == UINT64_MAX) {
            it->val1 = NULL;
            return -1;
        }
        memset(query_key, 0, sizeof(query_key));
        encode_uint64_be(query_key, id1 + 1);
        if (!iterator_rax_seek(it, ">=", query_key, 24) || raxEOF(&it->it)) {
            it->val1 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        it->val1 = fact_get_col(f, it->col1);
        return 0;
    }
    if (it->depth == 2) {
        if (!it->val1 || !it->val2)
            return -1;
        uint64_t id2 = get_symbol_id(it->val2);
        if (id2 == UINT64_MAX) {
            it->val2 = NULL;
            return -1;
        }
        memset(query_key, 0, sizeof(query_key));
        encode_uint64_be(query_key, get_symbol_id(it->val1));
        encode_uint64_be(query_key + 8, id2 + 1);
        if (!iterator_rax_seek(it, ">=", query_key, 24) || raxEOF(&it->it)) {
            it->val2 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1) {
            it->val2 = NULL;
            return -1;
        }
        it->val2 = fact_get_col(f, it->col2);
        return 0;
    }
    if (it->depth == 3) {
        if (!it->val1 || !it->val2 || !it->val3)
            return -1;
        it->it.flags &= ~RAX_ITER_JUST_SEEKED;
        if (!iterator_rax_next(it) || raxEOF(&it->it)) {
            it->val3 = NULL;
            return -1;
        }
        s_fact *f = it->it.data;
        if (fact_get_col(f, it->col1) != it->val1 || fact_get_col(f, it->col2) != it->val2) {
            it->val3 = NULL;
            return -1;
        }
        it->val3 = fact_get_col(f, it->col3);
        return 0;
    }
    return -1;
}

static void sort_iterators(s_lftj_iterator **arr, int count)
{
    for (int i = 1; i < count; i++) {
        s_lftj_iterator *cur = arr[i];
        Symbol key_cur = iterator_key(cur);
        int j = i - 1;
        while (j >= 0) {
            Symbol key_j = iterator_key(arr[j]);
            if (key_j && (!key_cur || compare_symbol(key_j, key_cur) <= 0))
                break;
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = cur;
    }
}

static Symbol leapfrog_search(s_lftj_iterator **arr, int count)
{
    if (count == 0)
        return NULL;
    if (count == 1)
        return iterator_key(arr[0]);

    sort_iterators(arr, count);
    int p = 0;
    while (1) {
        Symbol max_key = iterator_key(arr[(p + count - 1) % count]);
        Symbol cur_key = iterator_key(arr[p]);
        if (!max_key || !cur_key)
            return NULL;
        if (cur_key == max_key || compare_symbol(cur_key, max_key) == 0)
            return cur_key;
        if (iterator_seek(arr[p], max_key) < 0)
            return NULL;
        p = (p + 1) % count;
    }
}

typedef struct lftj_subgoal {
    s_facts *db;
    Symbol sym[3];
    int var_id[3];
} s_lftj_subgoal;

typedef struct lftj_ctx {
    s_lftj_subgoal *subgoals;
    s_lftj_iterator **iterators;
    int (*iterator_cols)[3];
    int subgoal_count;
    int vars_count;
    Symbol *var_syms;
    const char ***var_slots;
    s_binding *bindings;
    void (*cb)(s_binding *bindings, void *user_data);
    void *user_data;
} s_lftj_ctx;

static int lftj_solve_rec(s_lftj_ctx *ctx, int var_idx)
{
    int n = ctx->subgoal_count;
    s_lftj_iterator *stack_active[32];
    int stack_opened[32];
    s_lftj_iterator **active_iters = (n <= 32) ? stack_active : malloc((size_t)n * sizeof(*active_iters));
    int *opened_levels = (n <= 32) ? stack_opened : calloc((size_t)n, sizeof(*opened_levels));
    if (!active_iters || !opened_levels) {
        if (active_iters != stack_active)
            free(active_iters);
        if (opened_levels != stack_opened)
            free(opened_levels);
        return -1;
    }
    if (n <= 32)
        memset(stack_opened, 0, (size_t)n * sizeof(*stack_opened));

    int active_count = 0;

    for (int i = 0; i < n; i++) {
        s_lftj_subgoal *sub = &ctx->subgoals[i];
        s_lftj_iterator *it = ctx->iterators[i];

        while (it->depth < 3) {
            int col = ctx->iterator_cols[i][it->depth];
            int vid = sub->var_id[col];
            if (vid < 0 || vid < var_idx) {
                Symbol target = (vid < 0) ? sub->sym[col] : ctx->var_syms[vid];
                if (iterator_open(it) != 0)
                    goto backtrack_temp;
                opened_levels[i]++;
                if (iterator_seek(it, target) != 0 || iterator_key(it) != target)
                    goto backtrack_temp;
            } else if (vid == var_idx) {
                if (iterator_open(it) != 0)
                    goto backtrack_temp;
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
                iterator_up(ctx->iterators[k]);
                opened_levels[k]--;
            }
        }
        if (active_iters != stack_active)
            free(active_iters);
        if (opened_levels != stack_opened)
            free(opened_levels);
        for (int k = 0; k < n; k++) {
            if (ctx->iterators[k]->error)
                return -1;
        }
        return 0;
    }

    if (var_idx == ctx->vars_count) {
        if (ctx->cb)
            ctx->cb(ctx->bindings, ctx->user_data);
        for (int i = 0; i < n; i++) {
            while (opened_levels[i] > 0) {
                iterator_up(ctx->iterators[i]);
                opened_levels[i]--;
            }
        }
        if (active_iters != stack_active)
            free(active_iters);
        if (opened_levels != stack_opened)
            free(opened_levels);
        return 1;
    }

    const char **bound_slot = ctx->var_slots[var_idx];
    const char *previous_binding = *bound_slot;
    int found_solutions = 0;

    if (active_count > 0) {
        Symbol match_val = leapfrog_search(active_iters, active_count);
        while (match_val) {
            ctx->var_syms[var_idx] = match_val;
            *bound_slot = symbol_to_str(match_val);

            int child_solutions = lftj_solve_rec(ctx, var_idx + 1);
            if (child_solutions < 0 || found_solutions > INT_MAX - child_solutions) {
                found_solutions = -1;
                break;
            }
            found_solutions += child_solutions;

            if (iterator_next(active_iters[0]) < 0)
                break;
            match_val = leapfrog_search(active_iters, active_count);
        }
    } else {
        found_solutions = lftj_solve_rec(ctx, var_idx + 1);
    }

    for (int i = 0; i < n; i++) {
        while (opened_levels[i] > 0) {
            iterator_up(ctx->iterators[i]);
            opened_levels[i]--;
        }
    }
    *bound_slot = previous_binding;
    ctx->var_syms[var_idx] = NULL;
    for (int i = 0; i < n; i++) {
        if (ctx->iterators[i]->error)
            found_solutions = -1;
    }
    if (active_iters != stack_active)
        free(active_iters);
    if (opened_levels != stack_opened)
        free(opened_levels);
    return found_solutions;
}

typedef struct multi_subgoal {
    s_facts *db;
    const char *terms[3];
    const char **slots[3];
    int negated;
} s_multi_subgoal;

static int naive_solve_rec(s_multi_subgoal *subgoals, unsigned char *visited, int subgoal_count, int remaining, s_binding *bindings,
                           void (*cb)(s_binding *bindings, void *user_data), void *user_data)
{
    if (remaining == 0) {
        if (cb)
            cb(bindings, user_data);
        return 1;
    }

    int best_idx = -1;
    uint64_t best_cost = UINT64_MAX;
    for (int i = 0; i < subgoal_count; i++) {
        if (visited[i])
            continue;
        s_multi_subgoal *cand = &subgoals[i];
        uint64_t db_size = cand->db->index.count - cand->db->retired_count;
        int bound_terms = 0;
        for (int t = 0; t < 3; t++) {
            if (cand->slots[t]) {
                if (*cand->slots[t])
                    bound_terms++;
            } else if (cand->terms[t]) {
                bound_terms++;
            }
        }
        uint64_t cost;
        if (cand->negated) {
            cost = (bound_terms == 3) ? 0 : (UINT64_MAX - 1);
        } else if (bound_terms == 3) {
            cost = 0;
        } else if (bound_terms == 2) {
            cost = db_size + 1;
        } else if (bound_terms == 1) {
            cost = db_size * 64 + 64;
        } else {
            cost = db_size * 4096 + 4096;
        }

        if (best_idx == -1 || cost < best_cost) {
            best_cost = cost;
            best_idx = i;
            if (cost == 0)
                break;
        }
    }

    visited[best_idx] = 1;
    s_multi_subgoal *sub = &subgoals[best_idx];
    s_facts *db = sub->db;
    int solutions = 0;
    const char *s_val = (sub->slots[0] && *sub->slots[0]) ? *sub->slots[0] : sub->terms[0];
    const char *p_val = (sub->slots[1] && *sub->slots[1]) ? *sub->slots[1] : sub->terms[1];
    const char *o_val = (sub->slots[2] && *sub->slots[2]) ? *sub->slots[2] : sub->terms[2];

    s_facts_cursor fc;
    const char *cursor_s = NULL, *cursor_p = NULL, *cursor_o = NULL;
    const char **var_s = sub->slots[0] && !*sub->slots[0] ? &cursor_s : NULL;
    const char **var_p = sub->slots[1] && !*sub->slots[1] ? &cursor_p : NULL;
    const char **var_o = sub->slots[2] && !*sub->slots[2] ? &cursor_o : NULL;
    if (var_s && var_p && var_o)
        facts_with_0(db, &fc, var_s, var_p, var_o);
    else if (var_s || var_p || var_o)
        facts_with_1_2(db, &fc, s_val, p_val, o_val, var_s, var_p, var_o);
    else
        facts_with_3(db, &fc, s_val, p_val, o_val);

    if (sub->negated) {
        s_fact *f = facts_cursor_next(&fc);
        int cur_err = facts_cursor_error(&fc);
        facts_cursor_stop(&fc);
        if (cur_err) {
            visited[best_idx] = 0;
            return -1;
        }
        if (!f)
            solutions = naive_solve_rec(subgoals, visited, subgoal_count, remaining - 1, bindings, cb, user_data);
        visited[best_idx] = 0;
        return solutions;
    }

    s_fact *f;
    while ((f = facts_cursor_next(&fc))) {
        const char *values[3] = {symbol_to_str(f->s), symbol_to_str(f->p), symbol_to_str(f->o)};
        const char **changed_slots[3];
        int changed_count = 0;
        int compatible = 1;
        for (int i = 0; i < 3; i++) {
            const char **slot = sub->slots[i];
            if (!slot) {
                if (sub->terms[i] && sub->terms[i][0] == '?') {
                    compatible = 0;
                    break;
                }
                continue;
            }
            if (*slot && strcmp(*slot, values[i]) != 0) {
                compatible = 0;
                break;
            }
            if (!*slot) {
                changed_slots[changed_count++] = slot;
                *slot = values[i];
            }
        }

        if (compatible) {
            int child_solutions = naive_solve_rec(subgoals, visited, subgoal_count, remaining - 1, bindings, cb, user_data);
            if (child_solutions < 0 || solutions > INT_MAX - child_solutions)
                solutions = -1;
            else
                solutions += child_solutions;
        }

        for (int i = 0; i < changed_count; i++)
            *changed_slots[i] = NULL;
        if (solutions < 0)
            break;
    }
    if (facts_cursor_error(&fc))
        solutions = -1;
    facts_cursor_stop(&fc);
    visited[best_idx] = 0;
    return solutions;
}

typedef struct lftj_var_info {
    const char *name;
    const char **slot;
    uint64_t min_db_size;
    int occurrences;
    int unlocks_prefix;
} s_lftj_var_info;

static int var_info_better(const s_lftj_var_info *a, const s_lftj_var_info *b)
{
    if (a->min_db_size != b->min_db_size)
        return a->min_db_size < b->min_db_size;
    if (a->occurrences != b->occurrences)
        return a->occurrences > b->occurrences;
    return a->unlocks_prefix > b->unlocks_prefix;
}

static int run_naive_fallback(s_facts **sub_dbs, const s_spec_fact *triples, int subgoal_count, s_binding *bindings,
                              void (*cb)(s_binding *bindings, void *user_data), void *user_data)
{
    s_multi_subgoal stack_subgoals[16];
    unsigned char stack_visited[16];
    s_multi_subgoal *subgoals = (subgoal_count <= 16) ? stack_subgoals : calloc((size_t)subgoal_count, sizeof(*subgoals));
    unsigned char *visited = (subgoal_count <= 16) ? stack_visited : calloc((size_t)subgoal_count, sizeof(*visited));
    if (!subgoals || !visited) {
        if (subgoals != stack_subgoals)
            free(subgoals);
        if (visited != stack_visited)
            free(visited);
        return -1;
    }
    if (subgoal_count <= 16)
        memset(stack_visited, 0, (size_t)subgoal_count);

    for (int i = 0; i < subgoal_count; i++) {
        subgoals[i].db = sub_dbs[i];
        subgoals[i].terms[0] = triples[i].s;
        subgoals[i].terms[1] = triples[i].p;
        subgoals[i].terms[2] = triples[i].o;
        subgoals[i].negated = triples[i].negated != NULL;
        for (int t = 0; t < 3; t++) {
            const char *term = subgoals[i].terms[t];
            subgoals[i].slots[t] = (term && term[0] == '?') ? bindings_get(bindings, term) : NULL;
        }
    }

    int solutions = naive_solve_rec(subgoals, visited, subgoal_count, subgoal_count, bindings, cb, user_data);
    if (subgoals != stack_subgoals)
        free(subgoals);
    if (visited != stack_visited)
        free(visited);
    return solutions;
}

int facts_lftj_solve_multi(s_facts *facts, s_facts **dbs, p_spec spec, s_binding *bindings,
                           void (*cb)(s_binding *bindings, void *user_data), void *user_data)
{
    if (!facts || !spec)
        return -1;
    size_t subgoal_count_sz = spec_count_facts(spec);
    if (subgoal_count_sz == 0)
        return 0;
    if (subgoal_count_sz > INT_MAX || subgoal_count_sz > SIZE_MAX / (3 * sizeof(s_lftj_var_info)))
        return -1;
    int subgoal_count = (int)subgoal_count_sz;

    s_spec_fact stack_triples[16];
    s_facts *stack_sub_dbs[16];
    s_facts *stack_unique_dbs[17];
    int stack_db_locks[17];

    s_spec_fact *triples = (subgoal_count <= 16) ? stack_triples : calloc(subgoal_count_sz, sizeof(*triples));
    s_facts **sub_dbs = (subgoal_count <= 16) ? stack_sub_dbs : calloc(subgoal_count_sz, sizeof(*sub_dbs));
    s_facts **unique_dbs = (subgoal_count <= 16) ? stack_unique_dbs : calloc(subgoal_count_sz + 1, sizeof(*unique_dbs));
    int *db_locks = (subgoal_count <= 16) ? stack_db_locks : calloc(subgoal_count_sz + 1, sizeof(*db_locks));
    if (!triples || !sub_dbs || !unique_dbs || !db_locks) {
        if (triples != stack_triples)
            free(triples);
        if (sub_dbs != stack_sub_dbs)
            free(sub_dbs);
        if (unique_dbs != stack_unique_dbs)
            free(unique_dbs);
        if (db_locks != stack_db_locks)
            free(db_locks);
        return -1;
    }

    s_spec_cursor sc;
    spec_cursor_init(&sc, spec);
    int parsed = 0;
    int unique_db_count = 0;
    unique_dbs[unique_db_count++] = facts;

    while (parsed < subgoal_count && spec_cursor_next(&sc, &triples[parsed])) {
        s_facts *db = (dbs && dbs[parsed]) ? dbs[parsed] : facts;
        if (!db) {
            parsed = -1;
            break;
        }
        sub_dbs[parsed] = db;
        int seen = 0;
        for (int k = 0; k < unique_db_count; k++) {
            if (unique_dbs[k] == db) {
                seen = 1;
                break;
            }
        }
        if (!seen)
            unique_dbs[unique_db_count++] = db;
        parsed++;
    }

    if (parsed != subgoal_count) {
        if (triples != stack_triples)
            free(triples);
        if (sub_dbs != stack_sub_dbs)
            free(sub_dbs);
        if (unique_dbs != stack_unique_dbs)
            free(unique_dbs);
        if (db_locks != stack_db_locks)
            free(db_locks);
        return -1;
    }

    /* acquire reader locks in deterministic address order */
    for (int i = 1; i < unique_db_count; i++) {
        s_facts *cur = unique_dbs[i];
        int j = i - 1;
        while (j >= 0 && (uintptr_t)unique_dbs[j] > (uintptr_t)cur) {
            unique_dbs[j + 1] = unique_dbs[j];
            j--;
        }
        unique_dbs[j + 1] = cur;
    }
    for (int i = 0; i < unique_db_count; i++)
        db_locks[i] = transaction_acquire_reader(&unique_dbs[i]->tx);

    int solutions = -1;
    int has_negation = 0;
    int shared_symbols = 1;
    for (int i = 0; i < subgoal_count; i++) {
        if (triples[i].negated) {
            has_negation = 1;
        } else if (sub_dbs[i]->index.count - sub_dbs[i]->retired_count == 0) {
            solutions = 0;
            goto release_locks;
        }
        if (sub_dbs[i]->symbols != facts->symbols)
            shared_symbols = 0;
    }

    if (has_negation || !shared_symbols) {
        solutions = run_naive_fallback(sub_dbs, triples, subgoal_count, bindings, cb, user_data);
        goto release_locks;
    }

    s_lftj_subgoal stack_subgoals[16];
    s_lftj_var_info stack_vars[48];
    s_lftj_iterator stack_iter_storage[16];
    s_lftj_iterator *stack_iters[16];
    int stack_iter_cols[16][3];
    Symbol stack_var_syms[48];
    const char **stack_var_slots[48];

    s_lftj_subgoal *subgoals = (subgoal_count <= 16) ? stack_subgoals : calloc(subgoal_count_sz, sizeof(*subgoals));
    s_lftj_var_info *vars = (subgoal_count <= 16) ? stack_vars : calloc(subgoal_count_sz * 3, sizeof(*vars));
    s_lftj_iterator *iterators_storage =
        (subgoal_count <= 16) ? stack_iter_storage : calloc(subgoal_count_sz, sizeof(*iterators_storage));
    s_lftj_iterator **iterators = (subgoal_count <= 16) ? stack_iters : calloc(subgoal_count_sz, sizeof(*iterators));
    int (*iterator_cols)[3] = (subgoal_count <= 16) ? stack_iter_cols : calloc(subgoal_count_sz, sizeof(*iterator_cols));
    Symbol *var_syms = (subgoal_count <= 16) ? stack_var_syms : calloc(subgoal_count_sz * 3, sizeof(*var_syms));
    const char ***var_slots = (subgoal_count <= 16) ? stack_var_slots : calloc(subgoal_count_sz * 3, sizeof(*var_slots));
    int initialized_iterators = 0;

    if (!subgoals || !vars || !iterators_storage || !iterators || !iterator_cols || !var_syms || !var_slots)
        goto lftj_cleanup;

    int vars_count = 0;
    for (int i = 0; i < subgoal_count; i++) {
        s_facts *db = sub_dbs[i];
        uint64_t db_size = db->index.count - db->retired_count;
        const char *raw_terms[3] = {triples[i].s, triples[i].p, triples[i].o};
        subgoals[i].db = db;

        int is_const[3] = {0, 0, 0};
        const char *unbound_names[3] = {NULL, NULL, NULL};
        const char **unbound_slots[3] = {NULL, NULL, NULL};

        for (int t = 0; t < 3; t++) {
            const char *term = raw_terms[t];
            if (!term) {
                solutions = -1;
                goto lftj_cleanup;
            }
            if (term[0] == '?') {
                const char **slot = bindings_get(bindings, term);
                if (!slot) {
                    /* unbound variable missing from bindings cannot match */
                    solutions = 0;
                    goto lftj_cleanup;
                }
                if (*slot) {
                    Symbol sym = intern_find_symbol_str(facts->symbols, *slot);
                    if (!sym) {
                        solutions = 0;
                        goto lftj_cleanup;
                    }
                    subgoals[i].sym[t] = sym;
                    subgoals[i].var_id[t] = -1;
                    is_const[t] = 1;
                } else {
                    subgoals[i].sym[t] = NULL;
                    unbound_names[t] = term;
                    unbound_slots[t] = slot;
                }
            } else {
                Symbol sym = intern_find_symbol_str(facts->symbols, term);
                if (!sym) {
                    solutions = 0;
                    goto lftj_cleanup;
                }
                subgoals[i].sym[t] = sym;
                subgoals[i].var_id[t] = -1;
                is_const[t] = 1;
            }
        }

        for (int t = 0; t < 3; t++) {
            if (!unbound_names[t])
                continue;
            int prev_col = (t + 2) % 3;
            int unlocks = is_const[prev_col];
            int vid = -1;
            for (int v = 0; v < vars_count; v++) {
                if (strcmp(vars[v].name, unbound_names[t]) == 0) {
                    vid = v;
                    break;
                }
            }
            if (vid < 0) {
                vid = vars_count++;
                vars[vid].name = unbound_names[t];
                vars[vid].slot = unbound_slots[t];
                vars[vid].min_db_size = db_size;
                vars[vid].occurrences = 1;
                vars[vid].unlocks_prefix = unlocks;
            } else {
                if (db_size < vars[vid].min_db_size)
                    vars[vid].min_db_size = db_size;
                vars[vid].occurrences++;
                vars[vid].unlocks_prefix += unlocks;
            }
        }
    }

    /* sort variables by selectivity (smaller db, join degree, leading-constant unlock) */
    for (int i = 1; i < vars_count; i++) {
        s_lftj_var_info cur = vars[i];
        int j = i - 1;
        while (j >= 0 && var_info_better(&cur, &vars[j])) {
            vars[j + 1] = vars[j];
            j--;
        }
        vars[j + 1] = cur;
    }

    for (int v = 0; v < vars_count; v++) {
        var_syms[v] = NULL;
        var_slots[v] = vars[v].slot;
    }

    /* map unbound subgoal columns to sorted variable indices */
    for (int i = 0; i < subgoal_count; i++) {
        const char *raw_terms[3] = {triples[i].s, triples[i].p, triples[i].o};
        for (int t = 0; t < 3; t++) {
            if (subgoals[i].sym[t])
                continue;
            int vid = -1;
            for (int v = 0; v < vars_count; v++) {
                if (strcmp(vars[v].name, raw_terms[t]) == 0) {
                    vid = v;
                    break;
                }
            }
            subgoals[i].var_id[t] = vid;
        }
    }

    static const int trie_candidate_cols[3][3] = {
        {0, 1, 2}, /* trie_spo */
        {1, 2, 0}, /* trie_pos */
        {2, 0, 1}  /* trie_osp */
    };

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
                int vid = sub->var_id[col];
                if (vid >= 0) {
                    seen_var = 1;
                    if (vid < last_var_priority) {
                        is_valid = 0;
                        break;
                    }
                    last_var_priority = vid;
                } else if (!seen_var) {
                    constants_before_vars++;
                }
            }

            if (is_valid && constants_before_vars > max_consts) {
                max_consts = constants_before_vars;
                best_cand = cand;
            }
        }

        if (best_cand == -1) {
            solutions = run_naive_fallback(sub_dbs, triples, subgoal_count, bindings, cb, user_data);
            goto lftj_cleanup;
        }

        s_hexastore *hex = sub->db->hexastore;
        struct rax *t = (best_cand == 0) ? hex->trie_spo : ((best_cand == 1) ? hex->trie_pos : hex->trie_osp);
        const int *cols = trie_candidate_cols[best_cand];
        iterators[i] = &iterators_storage[i];
        init_lftj_iterator(iterators[i], t, cols[0], cols[1], cols[2]);
        initialized_iterators++;
        memcpy(iterator_cols[i], cols, 3 * sizeof(int));
    }

    {
        s_lftj_ctx ctx = {
            .subgoals = subgoals,
            .iterators = iterators,
            .iterator_cols = iterator_cols,
            .subgoal_count = subgoal_count,
            .vars_count = vars_count,
            .var_syms = var_syms,
            .var_slots = var_slots,
            .bindings = bindings,
            .cb = cb,
            .user_data = user_data,
        };
        solutions = lftj_solve_rec(&ctx, 0);
    }

lftj_cleanup:
    for (int i = 0; i < initialized_iterators; i++)
        raxStop(&iterators_storage[i].it);
    if (var_slots != stack_var_slots)
        free(var_slots);
    if (var_syms != stack_var_syms)
        free(var_syms);
    if (iterator_cols != stack_iter_cols)
        free(iterator_cols);
    if (iterators != stack_iters)
        free(iterators);
    if (iterators_storage != stack_iter_storage)
        free(iterators_storage);
    if (vars != stack_vars)
        free(vars);
    if (subgoals != stack_subgoals)
        free(subgoals);

release_locks:
    for (int i = unique_db_count - 1; i >= 0; i--)
        transaction_release_reader(&unique_dbs[i]->tx, db_locks[i]);
    if (db_locks != stack_db_locks)
        free(db_locks);
    if (unique_dbs != stack_unique_dbs)
        free(unique_dbs);
    if (sub_dbs != stack_sub_dbs)
        free(sub_dbs);
    if (triples != stack_triples)
        free(triples);
    return solutions;
}

int facts_lftj_solve(s_facts *facts, p_spec spec, s_binding *bindings)
{
    return facts_lftj_solve_multi(facts, NULL, spec, bindings, NULL, NULL);
}
