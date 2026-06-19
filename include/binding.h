#ifndef BINDING_H
#define BINDING_H

typedef struct binding {
    const char *name;
    const char **value;
} s_binding;

const char **bindings_get(s_binding *bindings, const char *name);

const char **bindings_get_or_die(s_binding *bindings, const char *name);

/* set all bindings values to NULL */
void bindings_nullify(s_binding *bindings);

int bindings_resolve(s_binding *bindings, const char **pstr);

#endif
