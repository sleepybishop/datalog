#ifndef SPEC_H
#define SPEC_H

#include <stdio.h>
#include "binding.h"
#include "fact.h"

typedef const char **p_spec;

/* facts specification
   ===================

   A spec has the following pattern :
    (subject (predicate object)+ NULL)+ NULL
   For instance one fact {s1, p1.1, o1.1} has the spec :
     s1 p1.1 o1.1 NULL NULL
   And four facts of two subjects, each having two properties yield this
   spec :
     s1 p1.1 o1.1 p1.2 o1.2 NULL s2 p2.1 o2.1 p2.2 o2.2 NULL NULL
*/

size_t spec_count_bindings(p_spec spec);

size_t spec_count_facts(p_spec spec);

/* calls malloc to return a new p_spec */
p_spec spec_expand(p_spec spec);

p_spec spec_sort(p_spec spec);

typedef struct spec_fact {
    const char *s;
    const char *p;
    const char *o;
    const char *negated;
} s_spec_fact;

/* copy counted expanded triples, including literal :not subjects. */
p_spec spec_copy_expanded(const s_spec_fact *triples, size_t count);

typedef struct spec_cursor {
    p_spec spec;
    const char *s;
    unsigned long pos;
} s_spec_cursor;

void spec_cursor_init(s_spec_cursor *c, p_spec spec);

int spec_cursor_next(s_spec_cursor *c, s_spec_fact *f);

int spec_fact_bindings_resolve(s_spec_fact *f, s_binding *bindings);

s_binding *spec_bindings(p_spec spec);
s_binding *spec_bindings_typed(const s_spec_fact *triples, const unsigned char *variable_masks, size_t count);

#endif
