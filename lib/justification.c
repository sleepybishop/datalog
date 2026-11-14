#include "justification.h"
#include "wyhash.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint64_t hash_triple(const char *s, const char *p, const char *o)
{
    uint64_t h = wyhash(s, strlen(s), 0, _wyp);
    h = wyhash(p, strlen(p), h, _wyp);
    h = wyhash(o, strlen(o), h, _wyp);
    return h;
}

static uint64_t hash_justification(uint64_t conclusion_hash, size_t rule_index, const s_justification_support *supports,
                                   size_t support_count)
{
    uint64_t h = wyhash(&rule_index, sizeof(rule_index), conclusion_hash, _wyp);
    for (size_t i = 0; i < support_count; i++) {
        h = wyhash(supports[i].s, strlen(supports[i].s), h, _wyp);
        h = wyhash(supports[i].p, strlen(supports[i].p), h, _wyp);
        h = wyhash(supports[i].o, strlen(supports[i].o), h, _wyp);
        h = wyhash(&supports[i].negated, sizeof(supports[i].negated), h, _wyp);
    }
    return h;
}

static void justification_destroy(s_justification *item)
{
    if (!item)
        return;
    free(item->s);
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

static int justification_equal(const s_justification *item, uint64_t conclusion_hash, uint64_t hash, const char *s, const char *p,
                               const char *o, size_t rule_index, const s_justification_support *supports, size_t support_count)
{
    if (item->hash != hash || item->conclusion_hash != conclusion_hash || item->rule_index != rule_index ||
        item->support_count != support_count || strcmp(item->s, s) != 0 || strcmp(item->p, p) != 0 || strcmp(item->o, o) != 0)
        return 0;
    for (size_t i = 0; i < support_count; i++) {
        if (!support_equal(&item->supports[i], &supports[i]))
            return 0;
    }
    return 1;
}

static int justification_init(s_justification *item, uint64_t conclusion_hash, uint64_t hash, const char *s, const char *p,
                              const char *o, size_t rule_index, const s_justification_support *supports, size_t support_count)
{
    memset(item, 0, sizeof(*item));
    size_t s_len = strlen(s) + 1;
    size_t p_len = strlen(p) + 1;
    size_t o_len = strlen(o) + 1;
    size_t str_bytes = s_len + p_len + o_len;
    for (size_t i = 0; i < support_count; i++) {
        if (!supports[i].s || !supports[i].p || !supports[i].o)
            return -1;
        str_bytes += strlen(supports[i].s) + 1 + strlen(supports[i].p) + 1 + strlen(supports[i].o) + 1;
    }
    char *str_buf = malloc(str_bytes);
    s_justification_support *sup_buf = support_count ? calloc(support_count, sizeof(*sup_buf)) : NULL;
    if (!str_buf || (support_count && !sup_buf)) {
        free(str_buf);
        free(sup_buf);
        return -1;
    }
    char *cursor = str_buf;
    item->s = cursor;
    memcpy(cursor, s, s_len);
    cursor += s_len;
    item->p = cursor;
    memcpy(cursor, p, p_len);
    cursor += p_len;
    item->o = cursor;
    memcpy(cursor, o, o_len);
    cursor += o_len;

    for (size_t i = 0; i < support_count; i++) {
        size_t ss_len = strlen(supports[i].s) + 1;
        size_t sp_len = strlen(supports[i].p) + 1;
        size_t so_len = strlen(supports[i].o) + 1;
        sup_buf[i].s = cursor;
        memcpy(cursor, supports[i].s, ss_len);
        cursor += ss_len;
        sup_buf[i].p = cursor;
        memcpy(cursor, supports[i].p, sp_len);
        cursor += sp_len;
        sup_buf[i].o = cursor;
        memcpy(cursor, supports[i].o, so_len);
        cursor += so_len;
        sup_buf[i].negated = supports[i].negated;
    }

    item->rule_index = rule_index;
    item->supports = sup_buf;
    item->support_count = support_count;
    item->conclusion_hash = conclusion_hash;
    item->hash = hash;
    return 0;
}

int justification_graph_add(s_justification_graph *graph, const char *s, const char *p, const char *o, size_t rule_index,
                            const s_justification_support *supports, size_t support_count)
{
    if (!graph || !s || !p || !o || (support_count && !supports))
        return -1;
    for (size_t i = 0; i < support_count; i++) {
        if (!supports[i].s || !supports[i].p || !supports[i].o)
            return -1;
    }
    uint64_t conclusion_hash = hash_triple(s, p, o);
    uint64_t hash = hash_justification(conclusion_hash, rule_index, supports, support_count);
    for (size_t i = 0; i < graph->count; i++) {
        if (justification_equal(&graph->items[i], conclusion_hash, hash, s, p, o, rule_index, supports, support_count))
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
    s_justification item;
    if (justification_init(&item, conclusion_hash, hash, s, p, o, rule_index, supports, support_count) != 0)
        return -1;
    graph->items[graph->count++] = item;
    return 1;
}

s_justification_graph *justification_graph_clone(const s_justification_graph *graph)
{
    s_justification_graph *copy = new_justification_graph();
    if (!copy)
        return NULL;
    if (!graph || graph->count == 0)
        return copy;
    copy->items = calloc(graph->count, sizeof(*copy->items));
    if (!copy->items) {
        delete_justification_graph(copy);
        return NULL;
    }
    copy->capacity = graph->count;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *item = &graph->items[i];
        if (justification_init(&copy->items[i], item->conclusion_hash, item->hash, item->s, item->p, item->o, item->rule_index,
                               item->supports, item->support_count) != 0) {
            delete_justification_graph(copy);
            return NULL;
        }
        copy->count++;
    }
    return copy;
}

size_t justification_graph_count(const s_justification_graph *graph, const char *s, const char *p, const char *o)
{
    if (!graph || !s || !p || !o)
        return 0;
    uint64_t conclusion_hash = hash_triple(s, p, o);
    size_t count = 0;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *item = &graph->items[i];
        if (item->conclusion_hash == conclusion_hash && strcmp(item->s, s) == 0 && strcmp(item->p, p) == 0 &&
            strcmp(item->o, o) == 0)
            count++;
    }
    return count;
}

void justification_support_array_destroy(s_justification_support *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(items[i].s);
        free(items[i].p);
        free(items[i].o);
    }
    free(items);
}

static int justification_uses_support(const s_justification *item, const char *s, const char *p, const char *o, int negated)
{
    for (size_t i = 0; i < item->support_count; i++) {
        const s_justification_support *support = &item->supports[i];
        if (support->negated == negated && strcmp(support->s, s) == 0 && strcmp(support->p, p) == 0 && strcmp(support->o, o) == 0)
            return 1;
    }
    return 0;
}

static int conclusion_append(s_justification_support **items, size_t *count, const s_justification *proof)
{
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*items)[i].s, proof->s) == 0 && strcmp((*items)[i].p, proof->p) == 0 && strcmp((*items)[i].o, proof->o) == 0)
            return 0;
    }
    if (*count == SIZE_MAX / sizeof(**items))
        return -1;
    s_justification_support *resized = realloc(*items, (*count + 1) * sizeof(**items));
    if (!resized)
        return -1;
    *items = resized;
    s_justification_support *item = &resized[*count];
    memset(item, 0, sizeof(*item));
    item->s = strdup(proof->s);
    item->p = strdup(proof->p);
    item->o = strdup(proof->o);
    if (!item->s || !item->p || !item->o) {
        free(item->s);
        free(item->p);
        free(item->o);
        memset(item, 0, sizeof(*item));
        return -1;
    }
    (*count)++;
    return 0;
}

int justification_graph_remove_support(s_justification_graph *graph, const char *s, const char *p, const char *o, int negated,
                                       s_justification_support **conclusions_out, size_t *conclusion_count_out)
{
    if (!graph || !s || !p || !o || !conclusions_out || !conclusion_count_out)
        return -1;
    *conclusions_out = NULL;
    *conclusion_count_out = 0;

    for (size_t i = 0; i < graph->count; i++) {
        if (justification_uses_support(&graph->items[i], s, p, o, negated) &&
            conclusion_append(conclusions_out, conclusion_count_out, &graph->items[i]) != 0) {
            justification_support_array_destroy(*conclusions_out, *conclusion_count_out);
            *conclusions_out = NULL;
            *conclusion_count_out = 0;
            return -1;
        }
    }

    int removed = 0;
    size_t write_idx = 0;
    for (size_t i = 0; i < graph->count; i++) {
        if (!justification_uses_support(&graph->items[i], s, p, o, negated)) {
            if (write_idx != i)
                graph->items[write_idx] = graph->items[i];
            write_idx++;
            continue;
        }
        justification_destroy(&graph->items[i]);
        removed++;
    }
    if (write_idx < graph->count)
        memset(&graph->items[write_idx], 0, (graph->count - write_idx) * sizeof(*graph->items));
    graph->count = write_idx;
    return removed;
}
