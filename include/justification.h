#ifndef JUSTIFICATION_H
#define JUSTIFICATION_H

#include <stddef.h>

typedef struct justification_support {
    char *s;
    char *p;
    char *o;
    int negated;
} s_justification_support;

typedef struct justification {
    char *s;
    char *p;
    char *o;
    size_t rule_index;
    s_justification_support *supports;
    size_t support_count;
} s_justification;

typedef struct justification_graph {
    s_justification *items;
    size_t count;
    size_t capacity;
} s_justification_graph;

s_justification_graph *new_justification_graph(void);
s_justification_graph *justification_graph_clone(const s_justification_graph *graph);
void delete_justification_graph(s_justification_graph *graph);

int justification_graph_add(s_justification_graph *graph, const char *s, const char *p, const char *o,
                            size_t rule_index, const s_justification_support *supports, size_t support_count);
size_t justification_graph_count(const s_justification_graph *graph, const char *s, const char *p, const char *o);

#endif
