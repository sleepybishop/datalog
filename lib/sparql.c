#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sparql.h"

typedef enum {
    TOKEN_SELECT,
    TOKEN_ASK,
    TOKEN_WHERE,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_DOT,
    TOKEN_VAR,
    TOKEN_CONST,
    TOKEN_NOT,
    TOKEN_PREFIX,
    TOKEN_LIMIT,
    TOKEN_OFFSET,
    TOKEN_ORDER,
    TOKEN_BY,
    TOKEN_ASC,
    TOKEN_DESC,
    TOKEN_EOF,
    TOKEN_ERROR
} e_token_type;

typedef struct {
    e_token_type type;
    char *value;
    int is_prefixed_name;
    int is_bare;
} s_token;

typedef struct {
    s_token *tokens;
    size_t count;
    size_t pos;
} s_token_stream;

typedef struct sparql_triple {
    char *s;
    char *p;
    char *o;
    int is_negated;
    unsigned char variable_mask;
} s_sparql_triple;

typedef struct {
    char *name;
    char *uri;
} s_sparql_prefix;

typedef struct sparql_query {
    char **projection_vars;
    size_t projection_count;
    s_sparql_triple *triples;
    size_t triple_count;
    s_sparql_prefix *prefixes;
    size_t prefix_count;
    long limit;
    long offset;
    int is_ask;
    char *sort_var;
    int sort_desc;
} s_sparql_query;

static s_token next_token(const char **p)
{
    s_token t;
    t.value = NULL;
    t.is_prefixed_name = 0;
    t.is_bare = 0;

    while (**p && (**p == ' ' || **p == '\t' || **p == '\r' || **p == '\n')) {
        (*p)++;
    }

    if (**p == '\0') {
        t.type = TOKEN_EOF;
        return t;
    }

    if (**p == '{') {
        t.type = TOKEN_LBRACE;
        t.value = strdup("{");
        if (!t.value)
            t.type = TOKEN_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '}') {
        t.type = TOKEN_RBRACE;
        t.value = strdup("}");
        if (!t.value)
            t.type = TOKEN_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '(') {
        t.type = TOKEN_LPAREN;
        t.value = strdup("(");
        if (!t.value)
            t.type = TOKEN_ERROR;
        (*p)++;
        return t;
    }

    if (**p == ')') {
        t.type = TOKEN_RPAREN;
        t.value = strdup(")");
        if (!t.value)
            t.type = TOKEN_ERROR;
        (*p)++;
        return t;
    }

    if (**p == '.') {
        t.type = TOKEN_DOT;
        t.value = strdup(".");
        if (!t.value)
            t.type = TOKEN_ERROR;
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
        t.type = TOKEN_VAR;
        t.value = malloc(len + 1);
        if (!t.value) {
            t.type = TOKEN_ERROR;
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
            t.type = TOKEN_CONST;
            t.value = malloc(len + 1);
            if (!t.value) {
                t.type = TOKEN_ERROR;
                return t;
            }
            memcpy(t.value, start, len);
            t.value[len] = '\0';
            (*p)++;
            return t;
        } else {
            t.type = TOKEN_ERROR;
            return t;
        }
    }

    if (**p == '"') {
        (*p)++;
        const char *scan = *p;
        size_t max_len = 0;
        while (*scan && *scan != '"') {
            if (*scan == '\\' && scan[1])
                scan++;
            scan++;
            max_len++;
        }
        if (*scan == '"') {
            t.type = TOKEN_CONST;
            t.value = malloc(max_len + 1);
            if (!t.value) {
                t.type = TOKEN_ERROR;
                return t;
            }
            size_t out = 0;
            while (**p && **p != '"') {
                if (**p == '\\' && (*p)[1])
                    (*p)++;
                t.value[out++] = *(*p)++;
            }
            t.value[out] = '\0';
            (*p)++;
            return t;
        } else {
            t.type = TOKEN_ERROR;
            return t;
        }
    }

    const char *start = *p;
    while (**p && ((**p >= 'a' && **p <= 'z') || (**p >= 'A' && **p <= 'Z') || (**p >= '0' && **p <= '9') || **p == '_' ||
                   **p == ':' || **p == '-')) {
        (*p)++;
    }
    size_t len = *p - start;
    if (len > 0) {
        char *word = malloc(len + 1);
        if (!word) {
            t.type = TOKEN_ERROR;
            return t;
        }
        memcpy(word, start, len);
        word[len] = '\0';

        if (strcasecmp(word, "SELECT") == 0) {
            t.type = TOKEN_SELECT;
            t.value = word;
        } else if (strcasecmp(word, "ASK") == 0) {
            t.type = TOKEN_ASK;
            t.value = word;
        } else if (strcasecmp(word, "WHERE") == 0) {
            t.type = TOKEN_WHERE;
            t.value = word;
        } else if (strcasecmp(word, "NOT") == 0) {
            t.type = TOKEN_NOT;
            t.value = word;
        } else if (strcasecmp(word, "PREFIX") == 0) {
            t.type = TOKEN_PREFIX;
            t.value = word;
        } else if (strcasecmp(word, "LIMIT") == 0) {
            t.type = TOKEN_LIMIT;
            t.value = word;
        } else if (strcasecmp(word, "OFFSET") == 0) {
            t.type = TOKEN_OFFSET;
            t.value = word;
        } else if (strcasecmp(word, "ORDER") == 0) {
            t.type = TOKEN_ORDER;
            t.value = word;
        } else if (strcasecmp(word, "BY") == 0) {
            t.type = TOKEN_BY;
            t.value = word;
        } else if (strcasecmp(word, "ASC") == 0) {
            t.type = TOKEN_ASC;
            t.value = word;
        } else if (strcasecmp(word, "DESC") == 0) {
            t.type = TOKEN_DESC;
            t.value = word;
        } else {
            t.type = TOKEN_CONST;
            t.value = word;
            t.is_prefixed_name = strchr(word, ':') != NULL;
            t.is_bare = 1;
        }
        return t;
    }

    t.type = TOKEN_ERROR;
    return t;
}

static s_token_stream tokenize(const char *query)
{
    s_token_stream stream;
    stream.tokens = NULL;
    stream.count = 0;
    stream.pos = 0;

    if (!query)
        return stream;

    const char *p = query;
    while (1) {
        s_token t = next_token(&p);
        s_token *new_tokens = realloc(stream.tokens, (stream.count + 1) * sizeof(s_token));
        if (!new_tokens) {
            free(t.value);
            for (size_t i = 0; i < stream.count; i++)
                free(stream.tokens[i].value);
            free(stream.tokens);
            stream.tokens = NULL;
            stream.count = 0;
            return stream;
        }
        stream.tokens = new_tokens;
        stream.tokens[stream.count++] = t;
        if (t.type == TOKEN_EOF || t.type == TOKEN_ERROR) {
            break;
        }
    }
    return stream;
}

static void free_token_stream(s_token_stream *stream)
{
    if (stream->tokens) {
        for (size_t i = 0; i < stream->count; i++) {
            if (stream->tokens[i].value) {
                free(stream->tokens[i].value);
            }
        }
        free(stream->tokens);
    }
}

static int statement_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':' || c == '-';
}

e_sparql_statement_kind sparql_statement_kind(const char *query)
{
    if (!query)
        return SPARQL_STATEMENT_UNKNOWN;
    s_token_stream stream = tokenize(query);
    size_t pos = 0;
    while (pos < stream.count && stream.tokens[pos].type == TOKEN_PREFIX) {
        if (pos + 2 >= stream.count || stream.tokens[pos + 1].type != TOKEN_CONST || stream.tokens[pos + 2].type != TOKEN_CONST)
            break;
        pos += 3;
    }
    e_sparql_statement_kind kind = SPARQL_STATEMENT_UNKNOWN;
    if (pos < stream.count) {
        s_token *token = &stream.tokens[pos];
        if (token->type == TOKEN_SELECT)
            kind = SPARQL_STATEMENT_SELECT;
        else if (token->type == TOKEN_ASK)
            kind = SPARQL_STATEMENT_ASK;
        else if (token->type == TOKEN_CONST && strcasecmp(token->value, "INSERT") == 0)
            kind = SPARQL_STATEMENT_INSERT;
    }
    free_token_stream(&stream);

    /* a rule head may itself be SELECT, ASK, or INSERT; its separator takes
       precedence, but delimiters inside literals and IRIs are ordinary data. */
    char quote = 0;
    int iri = 0;
    for (const char *p = query; *p; p++) {
        if (quote) {
            if (*p == '\\' && p[1])
                p++;
            else if (*p == quote)
                quote = 0;
        } else if (iri) {
            if (*p == '>')
                iri = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '<') {
            iri = 1;
        } else if (*p == '{') {
            /* datalog heads have no graph patterns; inside a SPARQL graph,
               even a token beginning with :- can be a default-prefixed name. */
            return kind;
        } else if (*p == ':' && p[1] == '-') {
            return SPARQL_STATEMENT_RULE;
        } else if (*p == '?') {
            while (statement_name_char(p[1]) && p[1] != ':' && p[1] != '-')
                p++;
        } else if (statement_name_char(*p)) {
            /* a prefixed name such as ex:-subject contains no separator. */
            while (statement_name_char(p[1]))
                p++;
        }
    }
    return kind;
}

static char *resolve_prefix_list(s_sparql_prefix *prefixes, size_t prefix_count, const s_token *token)
{
    const char *val = token->value;
    if (!val)
        return NULL;
    const char *colon = token->is_prefixed_name ? strchr(val, ':') : NULL;
    if (colon) {
        size_t prefix_len = colon - val;
        char *prefix_name = strndup(val, prefix_len);
        if (!prefix_name)
            return NULL;

        const char *uri = NULL;
        for (size_t i = 0; i < prefix_count; i++) {
            if (strcmp(prefixes[i].name, prefix_name) == 0) {
                uri = prefixes[i].uri;
                break;
            }
        }

        free(prefix_name);

        if (uri) {
            const char *suffix = colon + 1;
            char *resolved = NULL;
            if (asprintf(&resolved, "%s%s", uri, suffix) < 0) {
                return NULL;
            }
            return resolved;
        }
    }
    return strdup(val);
}

static void free_query(s_sparql_query *q)
{
    if (q) {
        for (size_t i = 0; i < q->projection_count; i++) {
            free(q->projection_vars[i]);
        }
        free(q->projection_vars);
        for (size_t i = 0; i < q->triple_count; i++) {
            free(q->triples[i].s);
            free(q->triples[i].p);
            free(q->triples[i].o);
        }
        free(q->triples);
        for (size_t i = 0; i < q->prefix_count; i++) {
            free(q->prefixes[i].name);
            free(q->prefixes[i].uri);
        }
        free(q->prefixes);
        if (q->sort_var) {
            free(q->sort_var);
        }
        free(q);
    }
}

static int parse_nonneg_long(const s_token *tok, long *out)
{
    if (!tok || tok->type != TOKEN_CONST || !tok->is_bare || !tok->value || tok->value[0] == '\0')
        return -1;
    for (const char *c = tok->value; *c; c++) {
        if (*c < '0' || *c > '9')
            return -1;
    }
    errno = 0;
    char *end = NULL;
    long val = strtol(tok->value, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || val < 0)
        return -1;
    *out = val;
    return 0;
}

static s_sparql_query *parse_query(s_token_stream *stream)
{
    s_sparql_query *q = calloc(1, sizeof(s_sparql_query));
    if (!q)
        return NULL;

    /* parse prefixes */
    while (stream->tokens[stream->pos].type == TOKEN_PREFIX) {
        stream->pos++;
        if (stream->tokens[stream->pos].type != TOKEN_CONST) {
            goto error;
        }
        char *pref_token = stream->tokens[stream->pos].value;
        size_t pref_len = strlen(pref_token);
        if (pref_len == 0 || pref_token[pref_len - 1] != ':') {
            goto error;
        }
        char *prefix_name = strndup(pref_token, pref_len - 1);
        if (!prefix_name)
            goto error;
        stream->pos++;

        if (stream->tokens[stream->pos].type != TOKEN_CONST) {
            free(prefix_name);
            goto error;
        }
        char *prefix_uri = strdup(stream->tokens[stream->pos].value);
        if (!prefix_uri) {
            free(prefix_name);
            goto error;
        }
        stream->pos++;

        s_sparql_prefix *new_prefixes = realloc(q->prefixes, (q->prefix_count + 1) * sizeof(s_sparql_prefix));
        if (!new_prefixes) {
            free(prefix_name);
            free(prefix_uri);
            goto error;
        }
        q->prefixes = new_prefixes;
        q->prefixes[q->prefix_count].name = prefix_name;
        q->prefixes[q->prefix_count].uri = prefix_uri;
        q->prefix_count++;
    }

    if (stream->tokens[stream->pos].type != TOKEN_SELECT && stream->tokens[stream->pos].type != TOKEN_ASK) {
        goto error;
    }
    if (stream->tokens[stream->pos].type == TOKEN_ASK) {
        q->is_ask = 1;
        stream->pos++;
    } else {
        q->is_ask = 0;
        stream->pos++;

        while (stream->tokens[stream->pos].type == TOKEN_VAR) {
            char **new_vars = realloc(q->projection_vars, (q->projection_count + 1) * sizeof(char *));
            if (!new_vars)
                goto error;
            q->projection_vars = new_vars;
            char *dup_var = strdup(stream->tokens[stream->pos].value);
            if (!dup_var)
                goto error;
            q->projection_vars[q->projection_count++] = dup_var;
            stream->pos++;
        }

        if (q->projection_count == 0) {
            goto error;
        }
    }

    if (stream->tokens[stream->pos].type != TOKEN_WHERE) {
        goto error;
    }
    stream->pos++;

    if (stream->tokens[stream->pos].type != TOKEN_LBRACE) {
        goto error;
    }
    stream->pos++;

    while (stream->tokens[stream->pos].type != TOKEN_RBRACE) {
        if (stream->tokens[stream->pos].type == TOKEN_EOF || stream->tokens[stream->pos].type == TOKEN_ERROR) {
            goto error;
        }

        int negated = 0;
        if (stream->tokens[stream->pos].type == TOKEN_NOT) {
            negated = 1;
            stream->pos++;
        }

        if (stream->tokens[stream->pos].type != TOKEN_VAR && stream->tokens[stream->pos].type != TOKEN_CONST) {
            goto error;
        }
        unsigned char variable_mask = stream->tokens[stream->pos].type == TOKEN_VAR ? 1 : 0;
        char *resolved_s = resolve_prefix_list(q->prefixes, q->prefix_count, &stream->tokens[stream->pos]);
        if (!resolved_s)
            goto error;
        stream->pos++;

        if (stream->tokens[stream->pos].type != TOKEN_VAR && stream->tokens[stream->pos].type != TOKEN_CONST) {
            free(resolved_s);
            goto error;
        }
        if (stream->tokens[stream->pos].type == TOKEN_VAR)
            variable_mask |= 2;
        char *resolved_p = resolve_prefix_list(q->prefixes, q->prefix_count, &stream->tokens[stream->pos]);
        if (!resolved_p) {
            free(resolved_s);
            goto error;
        }
        stream->pos++;

        if (stream->tokens[stream->pos].type != TOKEN_VAR && stream->tokens[stream->pos].type != TOKEN_CONST) {
            free(resolved_s);
            free(resolved_p);
            goto error;
        }
        if (stream->tokens[stream->pos].type == TOKEN_VAR)
            variable_mask |= 4;
        char *resolved_o = resolve_prefix_list(q->prefixes, q->prefix_count, &stream->tokens[stream->pos]);
        if (!resolved_o) {
            free(resolved_s);
            free(resolved_p);
            goto error;
        }
        stream->pos++;

        s_sparql_triple *new_triples = realloc(q->triples, (q->triple_count + 1) * sizeof(s_sparql_triple));
        if (!new_triples) {
            free(resolved_s);
            free(resolved_p);
            free(resolved_o);
            goto error;
        }
        q->triples = new_triples;
        q->triples[q->triple_count].s = resolved_s;
        q->triples[q->triple_count].p = resolved_p;
        q->triples[q->triple_count].o = resolved_o;
        q->triples[q->triple_count].is_negated = negated;
        q->triples[q->triple_count].variable_mask = variable_mask;
        q->triple_count++;

        if (stream->tokens[stream->pos].type == TOKEN_DOT) {
            stream->pos++;
        }
    }

    if (stream->tokens[stream->pos].type != TOKEN_RBRACE) {
        goto error;
    }
    stream->pos++;

    q->limit = -1;
    q->offset = 0;
    q->sort_var = NULL;
    q->sort_desc = 0;

    while (stream->tokens[stream->pos].type == TOKEN_LIMIT || stream->tokens[stream->pos].type == TOKEN_OFFSET ||
           stream->tokens[stream->pos].type == TOKEN_ORDER) {
        if (stream->tokens[stream->pos].type == TOKEN_LIMIT) {
            stream->pos++;
            if (parse_nonneg_long(&stream->tokens[stream->pos], &q->limit) != 0) {
                goto error;
            }
            stream->pos++;
        } else if (stream->tokens[stream->pos].type == TOKEN_OFFSET) {
            stream->pos++;
            if (parse_nonneg_long(&stream->tokens[stream->pos], &q->offset) != 0) {
                goto error;
            }
            stream->pos++;
        } else if (stream->tokens[stream->pos].type == TOKEN_ORDER) {
            stream->pos++;
            if (stream->tokens[stream->pos].type != TOKEN_BY) {
                goto error;
            }
            stream->pos++;
            free(q->sort_var);
            q->sort_var = NULL;
            if (stream->tokens[stream->pos].type == TOKEN_VAR) {
                q->sort_var = strdup(stream->tokens[stream->pos].value);
                if (!q->sort_var)
                    goto error;
                q->sort_desc = 0;
                stream->pos++;
            } else if (stream->tokens[stream->pos].type == TOKEN_ASC || stream->tokens[stream->pos].type == TOKEN_DESC) {
                int desc = (stream->tokens[stream->pos].type == TOKEN_DESC);
                stream->pos++;
                if (stream->tokens[stream->pos].type != TOKEN_LPAREN) {
                    goto error;
                }
                stream->pos++;
                if (stream->tokens[stream->pos].type != TOKEN_VAR) {
                    goto error;
                }
                q->sort_var = strdup(stream->tokens[stream->pos].value);
                if (!q->sort_var)
                    goto error;
                q->sort_desc = desc;
                stream->pos++;
                if (stream->tokens[stream->pos].type != TOKEN_RPAREN) {
                    goto error;
                }
                stream->pos++;
            } else {
                goto error;
            }
        }
    }

    if (stream->tokens[stream->pos].type != TOKEN_EOF) {
        goto error;
    }

    return q;

error:
    free_query(q);
    return NULL;
}

static s_spec_fact *compile_sparql_to_spec(s_sparql_query *q, unsigned char **masks_out)
{
    size_t count = q->triple_count ? q->triple_count : 1;
    if (count > SIZE_MAX / (sizeof(s_spec_fact) + sizeof(unsigned char)))
        return NULL;
    s_spec_fact *spec = calloc(count, sizeof(s_spec_fact) + sizeof(unsigned char));
    if (!spec)
        return NULL;
    unsigned char *masks = (unsigned char *)(spec + count);
    for (size_t i = 0; i < q->triple_count; i++) {
        spec[i] = (s_spec_fact){q->triples[i].s, q->triples[i].p, q->triples[i].o, q->triples[i].is_negated ? ":not" : NULL};
        masks[i] = q->triples[i].variable_mask;
    }
    *masks_out = masks;
    return spec;
}

int facts_sparql(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, const char *query)
{
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }

    s_sparql_query *q = parse_query(&stream);
    free_token_stream(&stream);
    if (!q) {
        return -1;
    }

    unsigned char *variable_masks;
    s_spec_fact *spec = compile_sparql_to_spec(q, &variable_masks);
    if (!spec) {
        free_query(q);
        return -1;
    }

    if (facts_with_typed_checked(facts, bindings, c, spec, variable_masks, q->triple_count) != 0) {
        free(spec);
        free_query(q);
        return -1;
    }
    if (q->is_ask) {
        c->limit = 1;
        c->offset = 0;
    } else {
        c->limit = q->limit;
        c->offset = q->offset;
    }

    if (q->sort_var) {
        c->sort_var = strdup(q->sort_var);
        if (!c->sort_var) {
            facts_with_cursor_destroy(c);
            free(spec);
            free_query(q);
            return -1;
        }
        c->is_sorted = 1;
        c->sort_desc = q->sort_desc;
    }

    free(spec);
    free_query(q);

    return 0;
}

char **sparql_projection_vars(s_facts *facts, const char *query, size_t *count)
{
    (void)facts;
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return NULL;
    }

    s_sparql_query *q = parse_query(&stream);
    free_token_stream(&stream);
    if (!q) {
        return NULL;
    }

    char **vars = q->projection_vars;
    *count = q->projection_count;
    q->projection_vars = NULL;
    q->projection_count = 0;

    if (q->is_ask && !vars) {
        vars = malloc(sizeof(char *));
        if (!vars) {
            free_query(q);
            return NULL;
        }
        vars[0] = NULL;
    }

    free_query(q);
    return vars;
}

int sparql_query_is_ask(s_facts *facts, const char *query)
{
    (void)facts;
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }
    s_sparql_query *q = parse_query(&stream);
    free_token_stream(&stream);
    if (!q) {
        return -1;
    }
    int is_ask = q->is_ask;
    free_query(q);
    return is_ask;
}

int facts_sparql_eval(s_facts *facts, const char *query, s_facts_with_cursor *c, s_binding **bindings_out)
{
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }

    s_sparql_query *q = parse_query(&stream);
    free_token_stream(&stream);
    if (!q) {
        return -1;
    }

    unsigned char *variable_masks;
    s_spec_fact *spec = compile_sparql_to_spec(q, &variable_masks);
    if (!spec) {
        free_query(q);
        return -1;
    }
    s_binding *bindings = spec_bindings_typed(spec, variable_masks, q->triple_count);
    if (!bindings) {
        free(spec);
        free_query(q);
        return -1;
    }

    if (facts_with_typed_checked(facts, bindings, c, spec, variable_masks, q->triple_count) != 0) {
        free(bindings);
        free(spec);
        free_query(q);
        return -1;
    }
    if (q->is_ask) {
        c->limit = 1;
        c->offset = 0;
    } else {
        c->limit = q->limit;
        c->offset = q->offset;
    }

    if (q->sort_var) {
        c->sort_var = strdup(q->sort_var);
        if (!c->sort_var) {
            facts_with_cursor_destroy(c);
            free(bindings);
            free(spec);
            free_query(q);
            return -1;
        }
        c->is_sorted = 1;
        c->sort_desc = q->sort_desc;
    }

    free(spec);
    free_query(q);

    *bindings_out = bindings;
    return 0;
}

int facts_sparql_insert(s_facts *facts, const char *query)
{
    typedef struct {
        char *s;
        char *p;
        char *o;
    } s_insert_triple;

    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }

    size_t pos = 0;

    /* parse prefixes */
    s_sparql_prefix *prefixes = NULL;
    size_t prefix_count = 0;
    s_insert_triple *triples = NULL;
    size_t triple_count = 0;
    size_t triple_capacity = 0;

    while (stream.tokens[pos].type == TOKEN_PREFIX) {
        pos++;
        if (stream.tokens[pos].type != TOKEN_CONST) {
            goto error;
        }
        char *pref_token = stream.tokens[pos].value;
        size_t pref_len = strlen(pref_token);
        if (pref_len == 0 || pref_token[pref_len - 1] != ':') {
            goto error;
        }
        char *prefix_name = strndup(pref_token, pref_len - 1);
        if (!prefix_name)
            goto error;
        pos++;

        if (stream.tokens[pos].type != TOKEN_CONST) {
            free(prefix_name);
            goto error;
        }
        char *prefix_uri = strdup(stream.tokens[pos].value);
        if (!prefix_uri) {
            free(prefix_name);
            goto error;
        }
        pos++;

        s_sparql_prefix *new_prefixes = realloc(prefixes, (prefix_count + 1) * sizeof(s_sparql_prefix));
        if (!new_prefixes) {
            free(prefix_name);
            free(prefix_uri);
            goto error;
        }
        prefixes = new_prefixes;
        prefixes[prefix_count].name = prefix_name;
        prefixes[prefix_count].uri = prefix_uri;
        prefix_count++;
    }

    /* check for "INSERT" */
    if (stream.tokens[pos].type != TOKEN_CONST || strcasecmp(stream.tokens[pos].value, "INSERT") != 0) {
        goto error;
    }
    pos++;

    /* check for "DATA" */
    if (stream.tokens[pos].type != TOKEN_CONST || strcasecmp(stream.tokens[pos].value, "DATA") != 0) {
        goto error;
    }
    pos++;

    /* check for "{" */
    if (stream.tokens[pos].type != TOKEN_LBRACE) {
        goto error;
    }
    pos++;

    while (stream.tokens[pos].type != TOKEN_RBRACE) {
        if (stream.tokens[pos].type == TOKEN_EOF || stream.tokens[pos].type == TOKEN_ERROR) {
            goto error;
        }

        /* triples to insert must not contain variables (must be constants) */
        if (stream.tokens[pos].type != TOKEN_CONST) {
            goto error;
        }
        char *resolved_s = resolve_prefix_list(prefixes, prefix_count, &stream.tokens[pos]);
        pos++;

        if (stream.tokens[pos].type != TOKEN_CONST) {
            free(resolved_s);
            goto error;
        }
        char *resolved_p = resolve_prefix_list(prefixes, prefix_count, &stream.tokens[pos]);
        pos++;

        if (stream.tokens[pos].type != TOKEN_CONST) {
            free(resolved_s);
            free(resolved_p);
            goto error;
        }
        char *resolved_o = resolve_prefix_list(prefixes, prefix_count, &stream.tokens[pos]);
        pos++;

        if (!resolved_s || !resolved_p || !resolved_o) {
            free(resolved_s);
            free(resolved_p);
            free(resolved_o);
            goto error;
        }

        if (triple_count == triple_capacity) {
            size_t new_capacity = triple_capacity ? triple_capacity * 2 : 8;
            s_insert_triple *new_triples = realloc(triples, new_capacity * sizeof(*triples));
            if (!new_triples) {
                free(resolved_s);
                free(resolved_p);
                free(resolved_o);
                goto error;
            }
            triples = new_triples;
            triple_capacity = new_capacity;
        }
        triples[triple_count++] = (s_insert_triple){resolved_s, resolved_p, resolved_o};

        if (stream.tokens[pos].type == TOKEN_DOT) {
            pos++;
        }
    }

    if (stream.tokens[pos].type != TOKEN_RBRACE) {
        goto error;
    }
    pos++;

    if (stream.tokens[pos].type != TOKEN_EOF) {
        goto error;
    }

    if (facts_transaction_begin(facts) != 0)
        goto error;

    int added_count = 0;
    for (size_t i = 0; i < triple_count; i++) {
        if (facts_contains_spo(facts, triples[i].s, triples[i].p, triples[i].o) <= 0) {
            if (!facts_add_spo(facts, triples[i].s, triples[i].p, triples[i].o)) {
                facts_transaction_rollback(facts);
                goto error;
            }
            added_count++;
        }
    }
    if (facts_transaction_commit(facts) != 0) {
        goto error;
    }

    for (size_t i = 0; i < triple_count; i++) {
        free(triples[i].s);
        free(triples[i].p);
        free(triples[i].o);
    }
    free(triples);
    for (size_t i = 0; i < prefix_count; i++) {
        free(prefixes[i].name);
        free(prefixes[i].uri);
    }
    free(prefixes);
    free_token_stream(&stream);
    return added_count;

error:
    for (size_t i = 0; i < triple_count; i++) {
        free(triples[i].s);
        free(triples[i].p);
        free(triples[i].o);
    }
    free(triples);
    for (size_t i = 0; i < prefix_count; i++) {
        free(prefixes[i].name);
        free(prefixes[i].uri);
    }
    free(prefixes);
    free_token_stream(&stream);
    return -1;
}
