#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "spec.h"

size_t spec_count_bindings(p_spec spec)
{
    size_t count = 0;
    size_t i = 0;
    if (spec && spec[0])
        while (spec[i] || spec[i + 1]) {
            if (spec[i] && spec[i][0] == '?')
                count++;
            i++;
        }
    return count;
}

size_t spec_count_facts(p_spec spec)
{
    s_spec_cursor c;
    size_t count = 0;
    s_spec_fact f;
    spec_cursor_init(&c, spec);
    while (spec_cursor_next(&c, &f))
        count++;
    return count;
}

/* calls malloc to return a new p_spec */
p_spec spec_expand(p_spec spec)
{
    size_t count;
    assert(spec);
    count = spec_count_facts(spec);
    if (count > 0) {
        s_spec_cursor c;
        s_spec_fact f;
        p_spec new = calloc(count * 4 + 2, sizeof(const char *));
        p_spec n = new;
        spec_cursor_init(&c, spec);
        while (spec_cursor_next(&c, &f)) {
            *n++ = f.s;
            *n++ = f.p;
            *n++ = f.o;
            *n++ = f.negated;
        }
        *n++ = NULL;
        *n = NULL;
        return new;
    }
    return NULL;
}

int fact_compare_bindings(s_spec_fact *a, s_spec_fact *b)
{
    int ba = 0;
    int bb = 0;
    assert(a);
    assert(b);
    if (a->s[0] == '?')
        ba++;
    if (a->p[0] == '?')
        ba++;
    if (a->o[0] == '?')
        ba++;
    if (b->s[0] == '?')
        bb++;
    if (b->p[0] == '?')
        bb++;
    if (b->o[0] == '?')
        bb++;
    if (ba < bb)
        return -1;
    if (ba > bb)
        return 1;
    return 0;
}

/* this only works on expanded specs : (s, p, o, NULL)*, NULL. */
p_spec spec_sort(p_spec spec)
{
    size_t count;
    size_t i;
    assert(spec);
    count = spec_count_facts(spec);
    if (count)
        for (i = 0; i < count - 1; i++) {
            size_t j;
            for (j = 0; j < count - i - 1; j++) {
                s_spec_fact *a = (s_spec_fact *)(spec + j * 4);
                s_spec_fact *b = (s_spec_fact *)(spec + (j + 1) * 4);
                if (fact_compare_bindings(a, b) > 0) {
                    s_spec_fact swap = *a;
                    *a = *b;
                    *b = swap;
                }
            }
        }
    return spec;
}

void spec_cursor_init(s_spec_cursor *c, p_spec spec)
{
    assert(c);
    assert(spec);
    c->spec = spec;
    c->s = spec[0];
    c->pos = 1;
}

int spec_cursor_next(s_spec_cursor *c, s_spec_fact *f)
{
    const char *p;
    const char *o;
    assert(c);
    assert(f);
    if (!c->s)
        return 0;

    if (strcmp(c->s, ":not") == 0) {
        const char *s = c->spec[c->pos];
        const char *p_val = c->spec[c->pos + 1];
        const char *o_val = c->spec[c->pos + 2];
        if (!s || !p_val || !o_val || c->spec[c->pos + 3] != NULL) {
            fprintf(stderr, "spec_cursor_next: invalid :not syntax\n");
            return 0;
        }
        f->s = s;
        f->p = p_val;
        f->o = o_val;
        f->negated = ":not";
        c->s = c->spec[c->pos + 4];
        c->pos += 5;
        return 1;
    }

    p = c->spec[c->pos];
    if (p) {
        o = c->spec[c->pos + 1];
        if (!o) {
            fprintf(stderr, "spec_cursor_next: NULL object\n");
            return 0;
        }
        c->pos += 2;
        f->s = c->s;
        f->p = p;
        f->o = o;
        f->negated = NULL;
        return 1;
    }
    c->s = c->spec[c->pos + 1];
    c->pos += 2;
    return spec_cursor_next(c, f);
}

int spec_fact_bindings_resolve(s_spec_fact *f, s_binding *bindings)
{
    int resolved = 0;
    assert(f);
    if (f->s && f->s[0] == '?')
        resolved += bindings_resolve(bindings, &f->s);
    if (f->p && f->p[0] == '?')
        resolved += bindings_resolve(bindings, &f->p);
    if (f->o && f->o[0] == '?')
        resolved += bindings_resolve(bindings, &f->o);
    return resolved;
}

s_binding *spec_bindings(p_spec spec)
{
    s_binding *bindings;
    size_t bindings_size;
    s_binding *b;
    const char **vars;
    size_t vars_size;
    const char **v;
    size_t count;
    if (!spec || !spec[0])
        return NULL;
    count = spec_count_bindings(spec);
    bindings_size = (count + 1) * sizeof(s_binding);
    vars_size = count * sizeof(char *);
    bindings = calloc(bindings_size + vars_size, 1);
    if (!bindings)
        return NULL;
    vars = (const char **)(((char *)bindings) + bindings_size);
    b = bindings;
    v = vars;
    while (spec[0] || spec[1]) {
        if (spec[0] && spec[0][0] == '?') {
            b->name = spec[0];
            b->value = v;
            b++;
            v++;
        }
        spec++;
    }
    return bindings;
}
