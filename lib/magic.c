#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magic.h"

typedef struct adorned_pred {
    char *name;
    char adornment[3];
} s_adorned_pred;

static int is_variable(const char *str)
{
    return str && str[0] == '?';
}

static int is_idb_predicate(const s_datalog_program *prog, const char *pred)
{
    if (!pred)
        return 0;
    for (size_t i = 0; i < prog->rule_count; i++) {
        if (prog->rules[i].head.p && strcmp(prog->rules[i].head.p, pred) == 0) {
            return 1;
        }
    }
    return 0;
}

static void enqueue_adorned(s_adorned_pred **queue, size_t *count, size_t *capacity, const char *name, const char *adornment)
{
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*queue)[i].name, name) == 0 && strcmp((*queue)[i].adornment, adornment) == 0) {
            return;
        }
    }
    if (*count >= *capacity) {
        *capacity = *capacity == 0 ? 16 : *capacity * 2;
        *queue = realloc(*queue, *capacity * sizeof(s_adorned_pred));
        assert(*queue);
    }
    (*queue)[*count].name = strdup(name);
    strcpy((*queue)[*count].adornment, adornment);
    (*count)++;
}

static void free_spec_fact_fields(s_spec_fact *f)
{
    if (f->s)
        free((char *)f->s);
    if (f->p)
        free((char *)f->p);
    if (f->o)
        free((char *)f->o);
    if (f->negated)
        free((char *)f->negated);
}

static char *format_adorned_name(const char *name, const char *adornment)
{
    char *buf = NULL;
    int ret = asprintf(&buf, "%s_%s", name, adornment);
    assert(ret != -1);
    (void)ret;
    return buf;
}

static char *format_magic_name(const char *name, const char *adornment)
{
    char *buf = NULL;
    int ret = asprintf(&buf, "magic_%s_%s", name, adornment);
    assert(ret != -1);
    (void)ret;
    return buf;
}

static int is_term_bound(const char *term, const char **bound_vars, size_t bound_count)
{
    if (!term)
        return 0;
    if (term[0] != '?')
        return 1; /* constants are always bound */
    for (size_t i = 0; i < bound_count; i++) {
        if (strcmp(bound_vars[i], term) == 0) {
            return 1;
        }
    }
    return 0;
}

static void add_var_to_bound(const char ***list, size_t *count, const char *var)
{
    if (!var || var[0] != '?')
        return;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*list)[i], var) == 0) {
            return;
        }
    }
    const char **new_list = realloc(*list, (*count + 1) * sizeof(char *));
    assert(new_list);
    *list = new_list;
    (*list)[*count] = var;
    (*count)++;
}

static void make_magic_fact(s_spec_fact *fact, const char *pred_name, const char *adornment, const char *s_val, const char *o_val)
{
    char *magic_pred = format_magic_name(pred_name, adornment);
    if (strcmp(adornment, "bf") == 0) {
        fact->s = strdup(s_val);
        fact->p = magic_pred;
        fact->o = strdup("true");
    } else if (strcmp(adornment, "fb") == 0) {
        fact->s = strdup(o_val);
        fact->p = magic_pred;
        fact->o = strdup("true");
    } else if (strcmp(adornment, "bb") == 0) {
        fact->s = strdup(s_val);
        fact->p = magic_pred;
        fact->o = strdup(o_val);
    } else {
        fact->s = NULL;
        fact->p = NULL;
        fact->o = NULL;
    }
    fact->negated = NULL;
}

static void make_adorned_subgoal(s_spec_fact *dest, const s_spec_fact *src, int is_idb, const char *adornment)
{
    dest->s = src->s ? strdup(src->s) : NULL;
    if (is_idb) {
        dest->p = format_adorned_name(src->p, adornment);
    } else {
        dest->p = src->p ? strdup(src->p) : NULL;
    }
    dest->o = src->o ? strdup(src->o) : NULL;
    dest->negated = src->negated ? strdup(src->negated) : NULL;
}

s_datalog_program *datalog_program_magic_transform(const s_datalog_program *prog, const s_spec_fact *query_goal)
{
    assert(prog);
    assert(query_goal);

    /* Determine initial query adornment */
    char init_adornment[3];
    init_adornment[0] = is_variable(query_goal->s) ? 'f' : 'b';
    init_adornment[1] = is_variable(query_goal->o) ? 'f' : 'b';
    init_adornment[2] = '\0';

    s_adorned_pred *adorned_queue = NULL;
    size_t queue_count = 0;
    size_t queue_capacity = 0;

    enqueue_adorned(&adorned_queue, &queue_count, &queue_capacity, query_goal->p, init_adornment);

    s_datalog_program *magic_prog = new_datalog_program();

    size_t processed = 0;
    while (processed < queue_count) {
        s_adorned_pred curr = adorned_queue[processed];
        processed++;

        for (size_t r = 0; r < prog->rule_count; r++) {
            const s_datalog_rule *orig_rule = &prog->rules[r];
            if (!orig_rule->head.p || strcmp(orig_rule->head.p, curr.name) != 0) {
                continue;
            }

            /* Adorn the rule */
            const char **bound_vars = NULL;
            size_t bound_count = 0;

            if (curr.adornment[0] == 'b') {
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.s);
            }
            if (curr.adornment[1] == 'b') {
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.o);
            }

            char(*sub_adornments)[3] = calloc(orig_rule->body_count ? orig_rule->body_count : 1, sizeof(*sub_adornments));
            int *is_sub_idb = calloc(orig_rule->body_count ? orig_rule->body_count : 1, sizeof(*is_sub_idb));
            assert(sub_adornments);
            assert(is_sub_idb);
            for (size_t j = 0; j < orig_rule->body_count; j++) {
                sub_adornments[j][0] = is_term_bound(orig_rule->body[j].s, bound_vars, bound_count) ? 'b' : 'f';
                sub_adornments[j][1] = is_term_bound(orig_rule->body[j].o, bound_vars, bound_count) ? 'b' : 'f';
                sub_adornments[j][2] = '\0';

                is_sub_idb[j] = is_idb_predicate(prog, orig_rule->body[j].p);
                if (is_sub_idb[j]) {
                    enqueue_adorned(&adorned_queue, &queue_count, &queue_capacity, orig_rule->body[j].p, sub_adornments[j]);
                }

                add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].s);
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].o);
            }

            /* Has magic head if the head adornment has at least one 'b' */
            int has_magic_head = (strcmp(curr.adornment, "ff") != 0);

            s_spec_fact magic_head_fact;
            if (has_magic_head) {
                make_magic_fact(&magic_head_fact, curr.name, curr.adornment, orig_rule->head.s, orig_rule->head.o);
            }

            /* Reset bound variables for generating magic rules */
            free(bound_vars);
            bound_vars = NULL;
            bound_count = 0;

            if (curr.adornment[0] == 'b') {
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.s);
            }
            if (curr.adornment[1] == 'b') {
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.o);
            }

            /* 1. Generate magic rules for each IDB subgoal in the body */
            for (size_t j = 0; j < orig_rule->body_count; j++) {
                if (is_sub_idb[j] && strcmp(sub_adornments[j], "ff") != 0) {
                    s_spec_fact m_head;
                    make_magic_fact(&m_head, orig_rule->body[j].p, sub_adornments[j], orig_rule->body[j].s, orig_rule->body[j].o);

                    size_t m_body_count = (has_magic_head ? 1 : 0) + j;
                    s_spec_fact *m_body = calloc(m_body_count, sizeof(s_spec_fact));
                    assert(m_body);

                    size_t b_idx = 0;
                    if (has_magic_head) {
                        m_body[b_idx].s = strdup(magic_head_fact.s);
                        m_body[b_idx].p = strdup(magic_head_fact.p);
                        m_body[b_idx].o = strdup(magic_head_fact.o);
                        m_body[b_idx].negated = NULL;
                        b_idx++;
                    }
                    for (size_t k = 0; k < j; k++) {
                        make_adorned_subgoal(&m_body[b_idx], &orig_rule->body[k], is_sub_idb[k], sub_adornments[k]);
                        b_idx++;
                    }

                    datalog_program_add_rule(magic_prog, &m_head, m_body, m_body_count);

                    /* Cleanup magic rule memory */
                    free_spec_fact_fields(&m_head);
                    for (size_t k = 0; k < m_body_count; k++) {
                        free_spec_fact_fields(&m_body[k]);
                    }
                    free(m_body);
                }

                add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].s);
                add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].o);
            }

            /* 2. Generate rewritten adorned rule */
            s_spec_fact rew_head;
            rew_head.s = orig_rule->head.s ? strdup(orig_rule->head.s) : NULL;
            rew_head.p = format_adorned_name(curr.name, curr.adornment);
            rew_head.o = orig_rule->head.o ? strdup(orig_rule->head.o) : NULL;
            rew_head.negated = orig_rule->head.negated ? strdup(orig_rule->head.negated) : NULL;

            size_t rew_body_count = (has_magic_head ? 1 : 0) + orig_rule->body_count;
            s_spec_fact *rew_body = calloc(rew_body_count, sizeof(s_spec_fact));
            assert(rew_body);

            size_t b_idx = 0;
            if (has_magic_head) {
                rew_body[b_idx].s = strdup(magic_head_fact.s);
                rew_body[b_idx].p = strdup(magic_head_fact.p);
                rew_body[b_idx].o = strdup(magic_head_fact.o);
                rew_body[b_idx].negated = NULL;
                b_idx++;
            }
            for (size_t j = 0; j < orig_rule->body_count; j++) {
                make_adorned_subgoal(&rew_body[b_idx], &orig_rule->body[j], is_sub_idb[j], sub_adornments[j]);
                b_idx++;
            }

            datalog_program_add_rule(magic_prog, &rew_head, rew_body, rew_body_count);

            /* Cleanup rewritten rule memory */
            free_spec_fact_fields(&rew_head);
            for (size_t j = 0; j < rew_body_count; j++) {
                free_spec_fact_fields(&rew_body[j]);
            }
            free(rew_body);

            if (has_magic_head) {
                free_spec_fact_fields(&magic_head_fact);
            }
            free(sub_adornments);
            free(is_sub_idb);
            free(bound_vars);
        }
    }

    /* Free adorned queue */
    for (size_t i = 0; i < queue_count; i++) {
        free(adorned_queue[i].name);
    }
    free(adorned_queue);

    return magic_prog;
}
