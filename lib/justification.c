#include "justification.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void justification_destroy(s_justification *item)
{
    if (!item)
        return;
    free(item->s);
    free(item->p);
    free(item->o);
    for (size_t i = 0; i < item->support_count; i++) {
        free(item->supports[i].s);
        free(item->supports[i].p);
        free(item->supports[i].o);
    }
    free(item->supports);
    memset(item, 0, sizeof(*item));
}

s_justification_graph *new_justification_graph(void)
{
    return calloc(1, sizeof(s_justification_graph));
}

void delete_justification_graph(s_justification_graph *graph)
{
    if (!graph)
        return;
    for (size_t i = 0; i < graph->count; i++)
        justification_destroy(&graph->items[i]);
    free(graph->items);
    free(graph);
}

static int support_equal(const s_justification_support *a, const s_justification_support *b)
{
    return a->negated == b->negated && strcmp(a->s, b->s) == 0 && strcmp(a->p, b->p) == 0 && strcmp(a->o, b->o) == 0;
}

static int justification_equal(const s_justification *item, const char *s, const char *p, const char *o, size_t rule_index,
                               const s_justification_support *supports, size_t support_count)
{
    if (item->rule_index != rule_index || item->support_count != support_count || strcmp(item->s, s) != 0 ||
        strcmp(item->p, p) != 0 || strcmp(item->o, o) != 0)
        return 0;
    for (size_t i = 0; i < support_count; i++) {
        if (!support_equal(&item->supports[i], &supports[i]))
            return 0;
    }
    return 1;
}

static int support_copy(s_justification_support *dest, const s_justification_support *source)
{
    dest->s = strdup(source->s);
    dest->p = strdup(source->p);
    dest->o = strdup(source->o);
    dest->negated = source->negated;
    if (!dest->s || !dest->p || !dest->o) {
        free(dest->s);
        free(dest->p);
        free(dest->o);
        memset(dest, 0, sizeof(*dest));
        return -1;
    }
    return 0;
}

int justification_graph_add(s_justification_graph *graph, const char *s, const char *p, const char *o, size_t rule_index,
                            const s_justification_support *supports, size_t support_count)
{
    if (!graph || !s || !p || !o || (support_count && !supports))
        return -1;
    for (size_t i = 0; i < graph->count; i++) {
        if (justification_equal(&graph->items[i], s, p, o, rule_index, supports, support_count))
            return 0;
    }
    if (graph->count == graph->capacity) {
        size_t capacity = graph->capacity ? graph->capacity * 2 : 16;
        if (capacity < graph->capacity || capacity > SIZE_MAX / sizeof(*graph->items))
            return -1;
        s_justification *items = realloc(graph->items, capacity * sizeof(*items));
        if (!items)
            return -1;
        graph->items = items;
        graph->capacity = capacity;
    }
    s_justification item = {0};
    item.s = strdup(s);
    item.p = strdup(p);
    item.o = strdup(o);
    item.rule_index = rule_index;
    item.support_count = support_count;
    item.supports = support_count ? calloc(support_count, sizeof(*item.supports)) : NULL;
    if (!item.s || !item.p || !item.o || (support_count && !item.supports)) {
        justification_destroy(&item);
        return -1;
    }
    for (size_t i = 0; i < support_count; i++) {
        if (support_copy(&item.supports[i], &supports[i]) != 0) {
            justification_destroy(&item);
            return -1;
        }
    }
    graph->items[graph->count++] = item;
    return 1;
}

s_justification_graph *justification_graph_clone(const s_justification_graph *graph)
{
    s_justification_graph *copy = new_justification_graph();
    if (!copy)
        return NULL;
    if (!graph)
        return copy;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *item = &graph->items[i];
        if (justification_graph_add(copy, item->s, item->p, item->o, item->rule_index, item->supports, item->support_count) < 0) {
            delete_justification_graph(copy);
            return NULL;
        }
    }
    return copy;
}

size_t justification_graph_count(const s_justification_graph *graph, const char *s, const char *p, const char *o)
{
    if (!graph || !s || !p || !o)
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *item = &graph->items[i];
        if (strcmp(item->s, s) == 0 && strcmp(item->p, p) == 0 && strcmp(item->o, o) == 0)
            count++;
    }
    return count;
}
