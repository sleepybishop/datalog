#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "rule.h"

s_datalog_program *new_datalog_program(void)
{
    s_datalog_program *prog = calloc(1, sizeof(s_datalog_program));
    return prog;
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

static void datalog_program_truncate(s_datalog_program *prog, size_t rule_count)
{
    while (prog->rule_count > rule_count) {
        s_datalog_rule *rule = &prog->rules[--prog->rule_count];
        free_spec_fact_fields(&rule->head);
        for (size_t j = 0; j < rule->body_count; j++)
            free_spec_fact_fields(&rule->body[j]);
        free(rule->body);
    }
}

void delete_datalog_program(s_datalog_program *prog)
{
    if (!prog)
        return;
    for (size_t i = 0; i < prog->rule_count; i++) {
        s_datalog_rule *rule = &prog->rules[i];
        free_spec_fact_fields(&rule->head);
        for (size_t j = 0; j < rule->body_count; j++) {
            free_spec_fact_fields(&rule->body[j]);
        }
        free(rule->body);
    }
    free(prog->rules);
    free(prog);
}

static int clone_spec_fact_fields(s_spec_fact *dest, const s_spec_fact *src)
{
    dest->s = src->s ? strdup(src->s) : NULL;
    if (src->s && !dest->s)
        return 0;
    dest->p = src->p ? strdup(src->p) : NULL;
    if (src->p && !dest->p)
        goto error;
    dest->o = src->o ? strdup(src->o) : NULL;
    if (src->o && !dest->o)
        goto error;
    dest->negated = src->negated ? strdup(src->negated) : NULL;
    if (src->negated && !dest->negated)
        goto error;
    return 1;

error:
    free_spec_fact_fields(dest);
    memset(dest, 0, sizeof(*dest));
    return 0;
}

s_datalog_rule *datalog_program_add_rule(s_datalog_program *prog, const s_spec_fact *head, const s_spec_fact *body,
                                         size_t body_count)
{
    assert(prog);
    assert(head);
    assert(body || body_count == 0);

    if (prog->rule_count == SIZE_MAX || prog->rule_count + 1 > SIZE_MAX / sizeof(s_datalog_rule))
        return NULL;
    s_datalog_rule *new_rules = realloc(prog->rules, (prog->rule_count + 1) * sizeof(s_datalog_rule));
    if (!new_rules)
        return NULL;
    prog->rules = new_rules;

    s_datalog_rule *rule = &prog->rules[prog->rule_count];
    memset(rule, 0, sizeof(s_datalog_rule));
    if (!clone_spec_fact_fields(&rule->head, head))
        return NULL;

    if (body_count > 0) {
        rule->body = calloc(body_count, sizeof(s_spec_fact));
        if (!rule->body) {
            free_spec_fact_fields(&rule->head);
            memset(rule, 0, sizeof(*rule));
            return NULL;
        }
        rule->body_count = body_count;
        for (size_t i = 0; i < body_count; i++) {
            if (!clone_spec_fact_fields(&rule->body[i], &body[i])) {
                for (size_t j = 0; j < i; j++)
                    free_spec_fact_fields(&rule->body[j]);
                free(rule->body);
                free_spec_fact_fields(&rule->head);
                memset(rule, 0, sizeof(*rule));
                return NULL;
            }
        }
    }

    prog->rule_count++;
    return rule;
}

s_datalog_program *datalog_program_clone(const s_datalog_program *prog)
{
    if (!prog)
        return NULL;
    s_datalog_program *copy = new_datalog_program();
    if (!copy)
        return NULL;
    for (size_t i = 0; i < prog->rule_count; i++) {
        const s_datalog_rule *rule = &prog->rules[i];
        if (!datalog_program_add_rule(copy, &rule->head, rule->body, rule->body_count)) {
            delete_datalog_program(copy);
            return NULL;
        }
    }
    return copy;
}

static int is_variable(const char *str)
{
    return str && str[0] == '?';
}

static int add_var_to_list(const char ***list, size_t *count, const char *var)
{
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*list)[i], var) == 0) {
            return 0;
        }
    }
    if (*count == SIZE_MAX / sizeof(char *))
        return -1;
    const char **new_list = realloc(*list, (*count + 1) * sizeof(char *));
    if (!new_list)
        return -1;
    *list = new_list;
    (*list)[*count] = var;
    (*count)++;
    return 0;
}

static int list_contains(const char **list, size_t count, const char *var)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i], var) == 0) {
            return 1;
        }
    }
    return 0;
}

int datalog_rule_validate(const s_datalog_rule *rule)
{
    assert(rule);

    const char **pos_vars = NULL;
    size_t pos_vars_count = 0;

    // 1. Collect all variables in positive body subgoals
    for (size_t i = 0; i < rule->body_count; i++) {
        const s_spec_fact *sub = &rule->body[i];
        if (sub->negated == NULL) {
            if ((is_variable(sub->s) && add_var_to_list(&pos_vars, &pos_vars_count, sub->s) != 0) ||
                (is_variable(sub->p) && add_var_to_list(&pos_vars, &pos_vars_count, sub->p) != 0) ||
                (is_variable(sub->o) && add_var_to_list(&pos_vars, &pos_vars_count, sub->o) != 0)) {
                free(pos_vars);
                return -1;
            }
        }
    }

    // 2. Head check
    int safe = 1;
    if (is_variable(rule->head.s) && !list_contains(pos_vars, pos_vars_count, rule->head.s))
        safe = 0;
    if (is_variable(rule->head.p) && !list_contains(pos_vars, pos_vars_count, rule->head.p))
        safe = 0;
    if (is_variable(rule->head.o) && !list_contains(pos_vars, pos_vars_count, rule->head.o))
        safe = 0;

    // 3. Negated body subgoals check
    if (safe) {
        for (size_t i = 0; i < rule->body_count; i++) {
            const s_spec_fact *sub = &rule->body[i];
            if (sub->negated != NULL) {
                if (is_variable(sub->s) && !list_contains(pos_vars, pos_vars_count, sub->s)) {
                    safe = 0;
                    break;
                }
                if (is_variable(sub->p) && !list_contains(pos_vars, pos_vars_count, sub->p)) {
                    safe = 0;
                    break;
                }
                if (is_variable(sub->o) && !list_contains(pos_vars, pos_vars_count, sub->o)) {
                    safe = 0;
                    break;
                }
            }
        }
    }

    free(pos_vars);
    return safe;
}

void datalog_rule_print(const s_datalog_rule *rule, FILE *fp)
{
    assert(rule);
    assert(fp);

    fprintf(fp, "%s <%s> %s :- ", rule->head.s, rule->head.p, rule->head.o);
    for (size_t i = 0; i < rule->body_count; i++) {
        const s_spec_fact *sub = &rule->body[i];
        if (sub->negated) {
            fprintf(fp, "NOT( %s <%s> %s )", sub->s, sub->p, sub->o);
        } else {
            fprintf(fp, "%s <%s> %s", sub->s, sub->p, sub->o);
        }
        if (i < rule->body_count - 1) {
            fprintf(fp, ", ");
        }
    }
    fprintf(fp, " .\n");
}

void datalog_program_print(const s_datalog_program *prog, FILE *fp)
{
    assert(prog);
    assert(fp);
    for (size_t i = 0; i < prog->rule_count; i++) {
        datalog_rule_print(&prog->rules[i], fp);
    }
}

typedef enum {
    RULE_TOK_VAR,
    RULE_TOK_CONST,
    RULE_TOK_TURNSTILE, /* :- */
    RULE_TOK_COMMA,     /* , */
    RULE_TOK_DOT,       /* . */
    RULE_TOK_NOT,       /* NOT */
    RULE_TOK_LPAREN,    /* ( */
    RULE_TOK_RPAREN,    /* ) */
    RULE_TOK_EOF,
    RULE_TOK_ERROR
} e_rule_token;

typedef struct {
    e_rule_token type;
    char *value;
} s_rule_token;

static s_rule_token next_rule_token(const char **p)
{
    s_rule_token t;
    t.value = NULL;

    /* Skip whitespace */
    while (**p && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) {
        (*p)++;
    }

    if (**p == '\0') {
        t.type = RULE_TOK_EOF;
        return t;
    }

    if (strncmp(*p, ":-", 2) == 0) {
        t.type = RULE_TOK_TURNSTILE;
        t.value = strdup(":-");
        if (!t.value)
            t.type = RULE_TOK_ERROR;
        (*p) += 2;
        return t;
    }

    if (**p == ',') {
        t.type = RULE_TOK_COMMA;
        t.value = strdup(",");
        if (!t.value)
            t.type = RULE_TOK_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '.') {
        t.type = RULE_TOK_DOT;
        t.value = strdup(".");
        if (!t.value)
            t.type = RULE_TOK_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '(') {
        t.type = RULE_TOK_LPAREN;
        t.value = strdup("(");
        if (!t.value)
            t.type = RULE_TOK_ERROR;
        (*p)++;
        return t;
    }

    if (**p == ')') {
        t.type = RULE_TOK_RPAREN;
        t.value = strdup(")");
        if (!t.value)
            t.type = RULE_TOK_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '?') {
        const char *start = *p;
        (*p)++;
        while (**p && ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || (**p >= '0' && **p <= '9') || **p == '_')) {
            (*p)++;
        }
        size_t len = *p - start;
        t.type = RULE_TOK_VAR;
        t.value = malloc(len + 1);
        if (!t.value) {
            t.type = RULE_TOK_ERROR;
            return t;
        }
        memcpy(t.value, start, len);
        t.value[len] = '\0';
        return t;
    }

    if (**p == '<') {
        (*p)++;
        const char *start = *p;
        while (**p && **p != '>') {
            (*p)++;
        }
        if (**p == '>') {
            size_t len = *p - start;
            t.type = RULE_TOK_CONST;
            t.value = malloc(len + 1);
            if (!t.value) {
                t.type = RULE_TOK_ERROR;
                return t;
            }
            memcpy(t.value, start, len);
            t.value[len] = '\0';
            (*p)++;
            return t;
        } else {
            t.type = RULE_TOK_ERROR;
            return t;
        }
    }

    if (**p == '"') {
        (*p)++;
        const char *start = *p;
        while (**p && **p != '"') {
            (*p)++;
        }
        if (**p == '"') {
            size_t len = *p - start;
            t.type = RULE_TOK_CONST;
            t.value = malloc(len + 1);
            if (!t.value) {
                t.type = RULE_TOK_ERROR;
                return t;
            }
            memcpy(t.value, start, len);
            t.value[len] = '\0';
            (*p)++;
            return t;
        } else {
            t.type = RULE_TOK_ERROR;
            return t;
        }
    }

    /* Bareword or keyword */
    const char *start = *p;
    while (**p && ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || (**p >= '0' && **p <= '9') || **p == '_' ||
                   **p == ':' || **p == '-')) {
        (*p)++;
    }
    size_t len = *p - start;
    if (len > 0) {
        char *word = malloc(len + 1);
        if (!word) {
            t.type = RULE_TOK_ERROR;
            return t;
        }
        memcpy(word, start, len);
        word[len] = '\0';

        if (strcasecmp(word, "NOT") == 0) {
            t.type = RULE_TOK_NOT;
            t.value = word;
        } else {
            t.type = RULE_TOK_CONST;
            t.value = word;
        }
        return t;
    }

    t.type = RULE_TOK_ERROR;
    return t;
}

int datalog_program_parse_rules(s_datalog_program *prog, const char *rules_str)
{
    if (!prog || !rules_str)
        return -1;

    size_t original_rule_count = prog->rule_count;
    const char *p = rules_str;
    s_rule_token tok;

    size_t count = 0;
    size_t capacity = 16;
    s_rule_token *tokens = malloc(capacity * sizeof(s_rule_token));
    if (!tokens)
        return -1;

    while (1) {
        tok = next_rule_token(&p);
        if (count >= capacity) {
            capacity *= 2;
            s_rule_token *new_tokens = realloc(tokens, capacity * sizeof(s_rule_token));
            if (!new_tokens) {
                free(tok.value);
                for (size_t i = 0; i < count; i++)
                    free(tokens[i].value);
                free(tokens);
                return -1;
            }
            tokens = new_tokens;
        }
        tokens[count++] = tok;
        if (tok.type == RULE_TOK_EOF)
            break;
        if (tok.type == RULE_TOK_ERROR) {
            for (size_t i = 0; i < count; i++) {
                if (tokens[i].value)
                    free(tokens[i].value);
            }
            free(tokens);
            return -1;
        }
    }

    size_t pos = 0;
    while (tokens[pos].type != RULE_TOK_EOF) {
        /* Expect head: term term term */
        if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST)
            goto parse_error;
        const char *head_s = tokens[pos].value;
        pos++;

        if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST)
            goto parse_error;
        const char *head_p = tokens[pos].value;
        pos++;

        if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST)
            goto parse_error;
        const char *head_o = tokens[pos].value;
        pos++;

        s_spec_fact head = {head_s, head_p, head_o, NULL};

        /* Expect ':-' */
        if (tokens[pos].type != RULE_TOK_TURNSTILE)
            goto parse_error;
        pos++;

        /* Parse body subgoals */
        size_t body_capacity = 4;
        size_t body_count = 0;
        s_spec_fact *body = malloc(body_capacity * sizeof(s_spec_fact));
        if (!body)
            goto parse_error;

        while (1) {
            int is_negated = 0;
            if (tokens[pos].type == RULE_TOK_NOT) {
                is_negated = 1;
                pos++;
                if (tokens[pos].type == RULE_TOK_LPAREN) {
                    pos++;
                }
            }

            /* Expect subgoal terms: term term term */
            if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST) {
                free(body);
                goto parse_error;
            }
            const char *sub_s = tokens[pos].value;
            pos++;

            if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST) {
                free(body);
                goto parse_error;
            }
            const char *sub_p = tokens[pos].value;
            pos++;

            if (tokens[pos].type != RULE_TOK_VAR && tokens[pos].type != RULE_TOK_CONST) {
                free(body);
                goto parse_error;
            }
            const char *sub_o = tokens[pos].value;
            pos++;

            if (is_negated) {
                if (pos > 4 && tokens[pos - 4].type == RULE_TOK_LPAREN) {
                    if (tokens[pos].type != RULE_TOK_RPAREN) {
                        free(body);
                        goto parse_error;
                    }
                    pos++;
                }
            }

            if (body_count >= body_capacity) {
                body_capacity *= 2;
                s_spec_fact *new_body = realloc(body, body_capacity * sizeof(s_spec_fact));
                if (!new_body) {
                    free(body);
                    goto parse_error;
                }
                body = new_body;
            }
            body[body_count++] = (s_spec_fact){sub_s, sub_p, sub_o, is_negated ? ":not" : NULL};

            /* Expect either ',' or '.' */
            if (tokens[pos].type == RULE_TOK_COMMA) {
                pos++;
                continue;
            } else if (tokens[pos].type == RULE_TOK_DOT) {
                pos++;
                break;
            } else {
                free(body);
                goto parse_error;
            }
        }

        /* Validate before mutating the program. */
        s_datalog_rule candidate = {head, body, body_count};
        if (datalog_rule_validate(&candidate) != 1) {
            free(body);
            goto parse_error;
        }

        if (!datalog_program_add_rule(prog, &head, body, body_count)) {
            free(body);
            goto parse_error;
        }
        free(body);
    }

    for (size_t i = 0; i < count; i++) {
        if (tokens[i].value)
            free(tokens[i].value);
    }
    free(tokens);
    return 0;

parse_error:
    datalog_program_truncate(prog, original_rule_count);
    for (size_t i = 0; i < count; i++) {
        if (tokens[i].value)
            free(tokens[i].value);
    }
    free(tokens);
    return -1;
}

static int add_pred_to_list(const char ***list, size_t *count, const char *pred)
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

static int find_pred_idx(const char **preds, size_t pred_count, const char *pred)
{
    if (!pred)
        return -1;
    for (size_t i = 0; i < pred_count; i++) {
        if (strcmp(preds[i], pred) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int *datalog_program_stratify(const s_datalog_program *prog, int *num_strata_out)
{
    assert(prog);
    assert(num_strata_out);
    *num_strata_out = 0;

    if (prog->rule_count == 0) {
        return NULL;
    }

    /* 1. Collect all unique constant predicates */
    const char **preds = NULL;
    size_t pred_count = 0;
    for (size_t i = 0; i < prog->rule_count; i++) {
        const s_datalog_rule *rule = &prog->rules[i];
        if (rule->head.p && rule->head.p[0] != '?') {
            if (add_pred_to_list(&preds, &pred_count, rule->head.p) != 0)
                goto allocation_error;
        }
        for (size_t j = 0; j < rule->body_count; j++) {
            const s_spec_fact *sub = &rule->body[j];
            if (sub->p && sub->p[0] != '?') {
                if (add_pred_to_list(&preds, &pred_count, sub->p) != 0)
                    goto allocation_error;
            }
        }
    }

    if (pred_count == 0) {
        return NULL;
    }

    /* 2. Initialize stratum values to 0 for all predicates */
    int *stratum = calloc(pred_count, sizeof(int));
    if (!stratum)
        goto allocation_error;

    /* 3. Run fixed-point iteration for stratum propagation */
    /* We run it pred_count + 1 times. If stratum values change in the last iteration,
     * it means there is a cycle with negation, so the program is unstratified. */
    int changed = 0;
    for (size_t iter = 0; iter <= pred_count; iter++) {
        changed = 0;
        for (size_t r = 0; r < prog->rule_count; r++) {
            const s_datalog_rule *rule = &prog->rules[r];
            int head_idx = find_pred_idx(preds, pred_count, rule->head.p);
            if (head_idx < 0)
                continue;

            for (size_t b = 0; b < rule->body_count; b++) {
                const s_spec_fact *sub = &rule->body[b];
                int sub_idx = find_pred_idx(preds, pred_count, sub->p);
                if (sub_idx < 0)
                    continue;

                if (sub->negated == NULL) { /* Positive dependency */
                    if (stratum[head_idx] < stratum[sub_idx]) {
                        stratum[head_idx] = stratum[sub_idx];
                        changed = 1;
                    }
                } else { /* Negative dependency */
                    if (stratum[head_idx] <= stratum[sub_idx]) {
                        stratum[head_idx] = stratum[sub_idx] + 1;
                        changed = 1;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }

    /* If it changed in the (pred_count)-th iteration, there is a negative cycle */
    if (changed) {
        free(stratum);
        free(preds);
        return NULL;
    }

    /* Determine the number of strata and rule strata mapping */
    int max_stratum = 0;
    int *rule_strata = malloc(prog->rule_count * sizeof(int));
    if (!rule_strata) {
        free(stratum);
        free(preds);
        return NULL;
    }

    for (size_t r = 0; r < prog->rule_count; r++) {
        const s_datalog_rule *rule = &prog->rules[r];
        int head_idx = find_pred_idx(preds, pred_count, rule->head.p);
        int r_stratum = (head_idx >= 0) ? stratum[head_idx] : 0;
        rule_strata[r] = r_stratum;
        if (r_stratum > max_stratum) {
            max_stratum = r_stratum;
        }
    }

    *num_strata_out = max_stratum + 1;

    free(stratum);
    free(preds);
    return rule_strata;

allocation_error:
    free(preds);
    return NULL;
}
