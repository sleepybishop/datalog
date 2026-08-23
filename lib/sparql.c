#define _GNU_SOURCE
#include <assert.h>
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
} s_token;

typedef struct {
    s_token *tokens;
    size_t count;
    size_t pos;
} s_token_stream;

typedef struct sparql_triple {
    const char *s;
    const char *p;
    const char *o;
    int is_negated;
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
        (*p)++;
        return t;
    }

    if (**p == '}') {
        t.type = TOKEN_RBRACE;
        t.value = strdup("}");
        (*p)++;
        return t;
    }

    if (**p == '(') {
        t.type = TOKEN_LPAREN;
        t.value = strdup("(");
        (*p)++;
        return t;
    }

    if (**p == ')') {
        t.type = TOKEN_RPAREN;
        t.value = strdup(")");
        (*p)++;
        return t;
    }

    if (**p == '.') {
        t.type = TOKEN_DOT;
        t.value = strdup(".");
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
        const char *start = *p;
        while (**p && **p != '"') {
            (*p)++;
        }
        if (**p == '"') {
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

static char *resolve_prefix_list(s_sparql_prefix *prefixes, size_t prefix_count, const char *val)
{
    if (!val)
        return NULL;
    if (val[0] == '?') {
        return strdup(val);
    }
    const char *colon = strchr(val, ':');
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

static s_sparql_query *parse_query(s_facts *facts, s_token_stream *stream)
{
    s_sparql_query *q = calloc(1, sizeof(s_sparql_query));
    if (!q)
        return NULL;

    // Parse prefixes
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
            q->projection_vars = realloc(q->projection_vars, (q->projection_count + 1) * sizeof(char *));
            q->projection_vars[q->projection_count++] = strdup(stream->tokens[stream->pos].value);
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
        char *resolved_s = resolve_prefix_list(q->prefixes, q->prefix_count, stream->tokens[stream->pos].value);
        if (!resolved_s)
            goto error;
        const char *s = symbol_to_str(facts_intern(facts, resolved_s));
        free(resolved_s);
        stream->pos++;

        if (stream->tokens[stream->pos].type != TOKEN_VAR && stream->tokens[stream->pos].type != TOKEN_CONST) {
            goto error;
        }
        char *resolved_p = resolve_prefix_list(q->prefixes, q->prefix_count, stream->tokens[stream->pos].value);
        if (!resolved_p)
            goto error;
        const char *p = symbol_to_str(facts_intern(facts, resolved_p));
        free(resolved_p);
        stream->pos++;

        if (stream->tokens[stream->pos].type != TOKEN_VAR && stream->tokens[stream->pos].type != TOKEN_CONST) {
            goto error;
        }
        char *resolved_o = resolve_prefix_list(q->prefixes, q->prefix_count, stream->tokens[stream->pos].value);
        if (!resolved_o)
            goto error;
        const char *o = symbol_to_str(facts_intern(facts, resolved_o));
        free(resolved_o);
        stream->pos++;

        q->triples = realloc(q->triples, (q->triple_count + 1) * sizeof(s_sparql_triple));
        q->triples[q->triple_count].s = s;
        q->triples[q->triple_count].p = p;
        q->triples[q->triple_count].o = o;
        q->triples[q->triple_count].is_negated = negated;
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
            if (stream->tokens[stream->pos].type != TOKEN_CONST) {
                goto error;
            }
            q->limit = atol(stream->tokens[stream->pos].value);
            stream->pos++;
        } else if (stream->tokens[stream->pos].type == TOKEN_OFFSET) {
            stream->pos++;
            if (stream->tokens[stream->pos].type != TOKEN_CONST) {
                goto error;
            }
            q->offset = atol(stream->tokens[stream->pos].value);
            stream->pos++;
        } else if (stream->tokens[stream->pos].type == TOKEN_ORDER) {
            stream->pos++;
            if (stream->tokens[stream->pos].type != TOKEN_BY) {
                goto error;
            }
            stream->pos++;
            if (stream->tokens[stream->pos].type == TOKEN_VAR) {
                q->sort_var = strdup(stream->tokens[stream->pos].value);
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

static p_spec compile_sparql_to_spec(s_sparql_query *q)
{
    const char **spec = malloc((q->triple_count * 5 + 2) * sizeof(const char *));
    size_t spec_pos = 0;

    int *visited = calloc(q->triple_count, sizeof(int));
    if (!spec || (q->triple_count > 0 && !visited)) {
        free(spec);
        free(visited);
        return NULL;
    }

    for (size_t i = 0; i < q->triple_count; i++) {
        if (visited[i])
            continue;

        if (q->triples[i].is_negated) {
            spec[spec_pos++] = ":not";
            spec[spec_pos++] = q->triples[i].s;
            spec[spec_pos++] = q->triples[i].p;
            spec[spec_pos++] = q->triples[i].o;
            spec[spec_pos++] = NULL;
            visited[i] = 1;
        } else {
            const char *subject = q->triples[i].s;
            spec[spec_pos++] = subject;

            for (size_t j = i; j < q->triple_count; j++) {
                if (!visited[j] && !q->triples[j].is_negated && strcmp(q->triples[j].s, subject) == 0) {
                    spec[spec_pos++] = q->triples[j].p;
                    spec[spec_pos++] = q->triples[j].o;
                    visited[j] = 1;
                }
            }
            spec[spec_pos++] = NULL;
        }
    }
    spec[spec_pos++] = NULL;

    free(visited);
    return spec;
}

int facts_sparql(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, const char *query)
{
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }

    s_sparql_query *q = parse_query(facts, &stream);
    free_token_stream(&stream);
    if (!q) {
        return -1;
    }

    p_spec spec = compile_sparql_to_spec(q);
    if (!spec) {
        free_query(q);
        return -1;
    }

    facts_with(facts, bindings, c, spec);
    if (q->is_ask) {
        c->limit = 1;
        c->offset = 0;
    } else {
        c->limit = q->limit;
        c->offset = q->offset;
    }

    if (q->sort_var) {
        c->is_sorted = 1;
        c->sort_var = strdup(q->sort_var);
        c->sort_desc = q->sort_desc;
    }

    free(spec);
    free_query(q);

    return 0;
}

char **sparql_projection_vars(s_facts *facts, const char *query, size_t *count)
{
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return NULL;
    }

    s_sparql_query *q = parse_query(facts, &stream);
    free_token_stream(&stream);
    if (!q) {
        return NULL;
    }

    char **vars = q->projection_vars;
    *count = q->projection_count;

    if (q->is_ask && !vars) {
        vars = malloc(sizeof(char *));
        if (!vars) {
            free_query(q);
            return NULL;
        }
        vars[0] = NULL;
    }

    free(q->triples);
    for (size_t i = 0; i < q->prefix_count; i++) {
        free(q->prefixes[i].name);
        free(q->prefixes[i].uri);
    }
    free(q->prefixes);
    free(q);

    return vars;
}

int sparql_query_is_ask(s_facts *facts, const char *query)
{
    s_token_stream stream = tokenize(query);
    if (stream.count == 0 || stream.tokens[0].type == TOKEN_ERROR) {
        free_token_stream(&stream);
        return -1;
    }
    s_sparql_query *q = parse_query(facts, &stream);
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

    s_sparql_query *q = parse_query(facts, &stream);
    free_token_stream(&stream);
    if (!q) {
        return -1;
    }

    p_spec spec = compile_sparql_to_spec(q);
    if (!spec) {
        free_query(q);
        return -1;
    }
    s_binding *bindings = spec_bindings(spec);
    if (!bindings) {
        free(spec);
        free_query(q);
        return -1;
    }

    facts_with(facts, bindings, c, spec);
    if (q->is_ask) {
        c->limit = 1;
        c->offset = 0;
    } else {
        c->limit = q->limit;
        c->offset = q->offset;
    }

    if (q->sort_var) {
        c->is_sorted = 1;
        c->sort_var = strdup(q->sort_var);
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

    // Parse prefixes
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

    // Check for "INSERT"
    if (stream.tokens[pos].type != TOKEN_CONST || strcasecmp(stream.tokens[pos].value, "INSERT") != 0) {
        goto error;
    }
    pos++;

    // Check for "DATA"
    if (stream.tokens[pos].type != TOKEN_CONST || strcasecmp(stream.tokens[pos].value, "DATA") != 0) {
        goto error;
    }
    pos++;

    // Check for "{"
    if (stream.tokens[pos].type != TOKEN_LBRACE) {
        goto error;
    }
    pos++;

    while (stream.tokens[pos].type != TOKEN_RBRACE) {
        if (stream.tokens[pos].type == TOKEN_EOF || stream.tokens[pos].type == TOKEN_ERROR) {
            goto error;
        }

        // Triples to insert must not contain variables (must be constants)
        if (stream.tokens[pos].type != TOKEN_CONST) {
            goto error;
        }
        char *resolved_s = resolve_prefix_list(prefixes, prefix_count, stream.tokens[pos].value);
        pos++;

        if (stream.tokens[pos].type != TOKEN_CONST) {
            free(resolved_s);
            goto error;
        }
        char *resolved_p = resolve_prefix_list(prefixes, prefix_count, stream.tokens[pos].value);
        pos++;

        if (stream.tokens[pos].type != TOKEN_CONST) {
            free(resolved_s);
            free(resolved_p);
            goto error;
        }
        char *resolved_o = resolve_prefix_list(prefixes, prefix_count, stream.tokens[pos].value);
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
        if (!facts_get_spo(facts, triples[i].s, triples[i].p, triples[i].o)) {
            if (!facts_add_spo(facts, triples[i].s, triples[i].p, triples[i].o)) {
                facts_transaction_rollback(facts);
                goto error;
            }
            added_count++;
        }
    }
    if (facts_transaction_commit(facts) != 0) {
        facts_transaction_rollback(facts);
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
