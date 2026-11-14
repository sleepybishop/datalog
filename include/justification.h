#ifndef JUSTIFICATION_H
#define JUSTIFICATION_H

#include <stddef.h>
#include <stdint.h>

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
    uint64_t conclusion_hash;
    uint64_t hash;
} s_justification;

typedef struct justification_graph {
    s_justification *items;
    size_t count;
    size_t capacity;
} s_justification_graph;

s_justification_graph *new_justification_graph(void);
s_justification_graph *justification_graph_clone(const s_justification_graph *graph);
void delete_justification_graph(s_justification_graph *graph);

int justification_graph_add(s_justification_graph *graph, const char *s, const char *p, const char *o, size_t rule_index,
                            const s_justification_support *supports, size_t support_count);
size_t justification_graph_count(const s_justification_graph *graph, const char *s, const char *p, const char *o);

/*
 * Remove every proof depending on the grounded support. On success,
 * *conclusions_out owns a deduplicated array of affected conclusion triples;
 * release it with justification_support_array_destroy().
 */
int justification_graph_remove_support(s_justification_graph *graph, const char *s, const char *p, const char *o, int negated,
                                       s_justification_support **conclusions_out, size_t *conclusion_count_out);
void justification_support_array_destroy(s_justification_support *items, size_t count);

/* determine whether a conclusion has a finite proof rooted in external
 * support, rather than relying solely on a recursive cycle; the callback
 * returns 1 for asserted positive support or a satisfied negative support,
 * 0 otherwise, and -1 on error. returns 1/0/-1 on the same convention. */
typedef int (*f_justification_external_support)(const s_justification_support *support, void *user_data);
typedef struct justification_grounding s_justification_grounding;

/* owned snapshot of all grounded conclusions; rebuild after changes to proofs
 * or external support. triple keys remain valid if the source graph changes. */
s_justification_grounding *new_justification_grounding(const s_justification_graph *graph,
                                                       f_justification_external_support external_support, void *user_data);
void delete_justification_grounding(s_justification_grounding *grounding);
int justification_grounding_contains(const s_justification_grounding *grounding, const char *s, const char *p, const char *o);

int justification_graph_is_grounded(const s_justification_graph *graph, const char *s, const char *p, const char *o,
                                    f_justification_external_support external_support, void *user_data);

#endif
