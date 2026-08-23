#define _GNU_SOURCE
#include <stdint.h>
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
        if (prog->rules[i].head.p && strcmp(prog->rules[i].head.p, pred) == 0)
            return 1;
    }
    return 0;
}

static int enqueue_adorned(s_adorned_pred **queue, size_t *count, size_t *capacity, const char *name,
                           const char *adornment)
{
    if (!queue || !count || !capacity || !name || !adornment || strlen(adornment) != 2)
        return -1;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*queue)[i].name, name) == 0 && strcmp((*queue)[i].adornment, adornment) == 0)
            return 0;
    }
    if (*count >= *capacity) {
        size_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        if (new_capacity < *capacity || new_capacity > SIZE_MAX / sizeof(s_adorned_pred))
            return -1;
        s_adorned_pred *resized = realloc(*queue, new_capacity * sizeof(*resized));
        if (!resized)
            return -1;
        *queue = resized;
        *capacity = new_capacity;
    }
    char *owned_name = strdup(name);
    if (!owned_name)
        return -1;
    (*queue)[*count].name = owned_name;
    memcpy((*queue)[*count].adornment, adornment, 3);
    (*count)++;
    return 0;
}

static void free_spec_fact_fields(s_spec_fact *fact)
{
    if (!fact)
        return;
    free((char *)fact->s);
    free((char *)fact->p);
    free((char *)fact->o);
    free((char *)fact->negated);
    memset(fact, 0, sizeof(*fact));
}

static int copy_string(const char *source, const char **destination)
{
    *destination = source ? strdup(source) : NULL;
    return !source || *destination ? 0 : -1;
}

static char *format_name(const char *prefix, const char *name, const char *adornment)
{
    char *buf = NULL;
    if (!name || !adornment || asprintf(&buf, "%s%s_%s", prefix, name, adornment) < 0)
        return NULL;
    return buf;
}

static int is_term_bound(const char *term, const char **bound_vars, size_t bound_count)
{
    if (!term)
        return 0;
    if (term[0] != '?')
        return 1;
    for (size_t i = 0; i < bound_count; i++) {
        if (strcmp(bound_vars[i], term) == 0)
            return 1;
    }
    return 0;
}

static int add_var_to_bound(const char ***list, size_t *count, const char *var)
{
    if (!var || var[0] != '?')
        return 0;
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*list)[i], var) == 0)
            return 0;
    }
    if (*count == SIZE_MAX / sizeof(char *))
        return -1;
    const char **resized = realloc(*list, (*count + 1) * sizeof(char *));
    if (!resized)
        return -1;
    *list = resized;
    (*list)[(*count)++] = var;
    return 0;
}

static int make_magic_fact(s_spec_fact *fact, const char *pred_name, const char *adornment, const char *s_val,
                           const char *o_val)
{
    memset(fact, 0, sizeof(*fact));
    fact->p = format_name("magic_", pred_name, adornment);
    if (!fact->p)
        return -1;
    if (strcmp(adornment, "bf") == 0) {
        if (copy_string(s_val, &fact->s) != 0 || copy_string("true", &fact->o) != 0)
            goto error;
    } else if (strcmp(adornment, "fb") == 0) {
        if (copy_string(o_val, &fact->s) != 0 || copy_string("true", &fact->o) != 0)
            goto error;
    } else if (strcmp(adornment, "bb") == 0) {
        if (copy_string(s_val, &fact->s) != 0 || copy_string(o_val, &fact->o) != 0)
            goto error;
    } else {
        goto error;
    }
    if (!fact->s || !fact->o)
        goto error;
    return 0;

error:
    free_spec_fact_fields(fact);
    return -1;
}

static int make_adorned_subgoal(s_spec_fact *dest, const s_spec_fact *src, int is_idb, const char *adornment)
{
    memset(dest, 0, sizeof(*dest));
    if (copy_string(src->s, &dest->s) != 0)
        goto error;
    if (is_idb) {
        dest->p = format_name("", src->p, adornment);
        if (!dest->p)
            goto error;
    } else if (copy_string(src->p, &dest->p) != 0) {
        goto error;
    }
    if (copy_string(src->o, &dest->o) != 0 || copy_string(src->negated, &dest->negated) != 0)
        goto error;
    return 0;

error:
    free_spec_fact_fields(dest);
    return -1;
}

static int copy_magic_support(s_spec_fact *dest, const s_spec_fact *source)
{
    memset(dest, 0, sizeof(*dest));
    if (copy_string(source->s, &dest->s) != 0 || copy_string(source->p, &dest->p) != 0 ||
        copy_string(source->o, &dest->o) != 0)
        goto error;
    return 0;

error:
    free_spec_fact_fields(dest);
    return -1;
}

static void free_spec_fact_array(s_spec_fact *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        free_spec_fact_fields(&items[i]);
    free(items);
}

static int transform_rule(s_datalog_program *magic_prog, const s_datalog_program *prog,
                          const s_datalog_rule *orig_rule, const s_adorned_pred *current,
                          s_adorned_pred **queue, size_t *queue_count, size_t *queue_capacity)
{
    const char **bound_vars = NULL;
    size_t bound_count = 0;
    char (*sub_adornments)[3] = NULL;
    int *is_sub_idb = NULL;
    s_spec_fact magic_head_fact = {0};
    int has_magic_head = strcmp(current->adornment, "ff") != 0;
    int result = -1;

    if ((current->adornment[0] == 'b' && add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.s) != 0) ||
        (current->adornment[1] == 'b' && add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.o) != 0))
        goto cleanup;

    size_t body_slots = orig_rule->body_count ? orig_rule->body_count : 1;
    if (body_slots > SIZE_MAX / sizeof(*sub_adornments) || body_slots > SIZE_MAX / sizeof(*is_sub_idb))
        goto cleanup;
    sub_adornments = calloc(body_slots, sizeof(*sub_adornments));
    is_sub_idb = calloc(body_slots, sizeof(*is_sub_idb));
    if (!sub_adornments || !is_sub_idb)
        goto cleanup;

    for (size_t j = 0; j < orig_rule->body_count; j++) {
        sub_adornments[j][0] = is_term_bound(orig_rule->body[j].s, bound_vars, bound_count) ? 'b' : 'f';
        sub_adornments[j][1] = is_term_bound(orig_rule->body[j].o, bound_vars, bound_count) ? 'b' : 'f';
        sub_adornments[j][2] = '\0';
        is_sub_idb[j] = is_idb_predicate(prog, orig_rule->body[j].p);
        if ((is_sub_idb[j] && enqueue_adorned(queue, queue_count, queue_capacity, orig_rule->body[j].p,
                                              sub_adornments[j]) != 0) ||
            add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].s) != 0 ||
            add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].o) != 0)
            goto cleanup;
    }

    if (has_magic_head && make_magic_fact(&magic_head_fact, current->name, current->adornment,
                                           orig_rule->head.s, orig_rule->head.o) != 0)
        goto cleanup;

    free(bound_vars);
    bound_vars = NULL;
    bound_count = 0;
    if ((current->adornment[0] == 'b' && add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.s) != 0) ||
        (current->adornment[1] == 'b' && add_var_to_bound(&bound_vars, &bound_count, orig_rule->head.o) != 0))
        goto cleanup;

    for (size_t j = 0; j < orig_rule->body_count; j++) {
        if (is_sub_idb[j] && strcmp(sub_adornments[j], "ff") != 0) {
            s_spec_fact magic_rule_head = {0};
            size_t magic_body_count = (has_magic_head ? 1 : 0) + j;
            s_spec_fact *magic_body = magic_body_count ? calloc(magic_body_count, sizeof(*magic_body)) : NULL;
            size_t initialized = 0;
            if ((magic_body_count && !magic_body) ||
                make_magic_fact(&magic_rule_head, orig_rule->body[j].p, sub_adornments[j],
                                orig_rule->body[j].s, orig_rule->body[j].o) != 0)
                goto magic_rule_error;
            if (has_magic_head) {
                if (copy_magic_support(&magic_body[initialized], &magic_head_fact) != 0)
                    goto magic_rule_error;
                initialized++;
            }
            for (size_t k = 0; k < j; k++) {
                if (make_adorned_subgoal(&magic_body[initialized], &orig_rule->body[k], is_sub_idb[k],
                                         sub_adornments[k]) != 0)
                    goto magic_rule_error;
                initialized++;
            }
            if (!datalog_program_add_rule(magic_prog, &magic_rule_head, magic_body, magic_body_count))
                goto magic_rule_error;
            free_spec_fact_fields(&magic_rule_head);
            free_spec_fact_array(magic_body, initialized);
            goto magic_rule_done;

magic_rule_error:
            free_spec_fact_fields(&magic_rule_head);
            free_spec_fact_array(magic_body, initialized);
            goto cleanup;
        }
magic_rule_done:
        if (add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].s) != 0 ||
            add_var_to_bound(&bound_vars, &bound_count, orig_rule->body[j].o) != 0)
            goto cleanup;
    }

    s_spec_fact rewritten_head = {0};
    s_spec_fact *rewritten_body = NULL;
    size_t rewritten_count = (has_magic_head ? 1 : 0) + orig_rule->body_count;
    size_t initialized = 0;
    if (copy_string(orig_rule->head.s, &rewritten_head.s) != 0 ||
        !(rewritten_head.p = format_name("", current->name, current->adornment)) ||
        copy_string(orig_rule->head.o, &rewritten_head.o) != 0 ||
        copy_string(orig_rule->head.negated, &rewritten_head.negated) != 0)
        goto rewritten_error;
    rewritten_body = rewritten_count ? calloc(rewritten_count, sizeof(*rewritten_body)) : NULL;
    if (rewritten_count && !rewritten_body)
        goto rewritten_error;
    if (has_magic_head) {
        if (copy_magic_support(&rewritten_body[initialized], &magic_head_fact) != 0)
            goto rewritten_error;
        initialized++;
    }
    for (size_t j = 0; j < orig_rule->body_count; j++) {
        if (make_adorned_subgoal(&rewritten_body[initialized], &orig_rule->body[j], is_sub_idb[j],
                                 sub_adornments[j]) != 0)
            goto rewritten_error;
        initialized++;
    }
    if (!datalog_program_add_rule(magic_prog, &rewritten_head, rewritten_body, rewritten_count))
        goto rewritten_error;
    result = 0;

rewritten_error:
    free_spec_fact_fields(&rewritten_head);
    free_spec_fact_array(rewritten_body, initialized);

cleanup:
    free_spec_fact_fields(&magic_head_fact);
    free(sub_adornments);
    free(is_sub_idb);
    free(bound_vars);
    return result;
}

s_datalog_program *datalog_program_magic_transform(const s_datalog_program *prog, const s_spec_fact *query_goal)
{
    if (!prog || !query_goal || !query_goal->s || !query_goal->p || !query_goal->o || is_variable(query_goal->p))
        return NULL;

    char initial_adornment[3] = {
        is_variable(query_goal->s) ? 'f' : 'b',
        is_variable(query_goal->o) ? 'f' : 'b',
        '\0'
    };
    s_adorned_pred *queue = NULL;
    size_t queue_count = 0;
    size_t queue_capacity = 0;
    s_datalog_program *magic_prog = NULL;

    if (enqueue_adorned(&queue, &queue_count, &queue_capacity, query_goal->p, initial_adornment) != 0)
        goto error;
    magic_prog = new_datalog_program();
    if (!magic_prog)
        goto error;

    for (size_t processed = 0; processed < queue_count; processed++) {
        s_adorned_pred current = queue[processed];
        for (size_t r = 0; r < prog->rule_count; r++) {
            const s_datalog_rule *rule = &prog->rules[r];
            if (rule->head.p && strcmp(rule->head.p, current.name) == 0 &&
                transform_rule(magic_prog, prog, rule, &current, &queue, &queue_count, &queue_capacity) != 0)
                goto error;
        }
    }

    for (size_t i = 0; i < queue_count; i++)
        free(queue[i].name);
    free(queue);
    return magic_prog;

error:
    for (size_t i = 0; i < queue_count; i++)
        free(queue[i].name);
    free(queue);
    delete_datalog_program(magic_prog);
    return NULL;
}
