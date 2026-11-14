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

typedef struct grounding_node {
    char *s;
    char *p;
    char *o;
    uint64_t hash;
    size_t dependents;
    int grounded;
} s_grounding_node;

struct justification_grounding {
    s_grounding_node *nodes;
    size_t node_count;
    size_t *slots;
    size_t slot_count;
};

typedef struct grounding_edge {
    size_t proof;
    size_t next;
} s_grounding_edge;

void delete_justification_grounding(s_justification_grounding *grounding)
{
    if (!grounding)
        return;
    for (size_t i = 0; i < grounding->node_count; i++)
        free(grounding->nodes[i].s);
    free(grounding->nodes);
    free(grounding->slots);
    free(grounding);
}

static size_t grounding_slot(const s_justification_grounding *grounding, uint64_t hash, const char *s, const char *p, const char *o)
{
    size_t slot = (size_t)hash & (grounding->slot_count - 1);
    while (grounding->slots[slot]) {
        const s_grounding_node *node = &grounding->nodes[grounding->slots[slot] - 1];
        if (node->hash == hash && strcmp(node->s, s) == 0 && strcmp(node->p, p) == 0 && strcmp(node->o, o) == 0)
            break;
        slot = (slot + 1) & (grounding->slot_count - 1);
    }
    return slot;
}

int justification_grounding_contains(const s_justification_grounding *grounding, const char *s, const char *p, const char *o)
{
    if (!grounding || !s || !p || !o || !grounding->node_count)
        return 0;
    size_t slot = grounding_slot(grounding, hash_triple(s, p, o), s, p, o);
    return grounding->slots[slot] && grounding->nodes[grounding->slots[slot] - 1].grounded;
}

s_justification_grounding *new_justification_grounding(const s_justification_graph *graph,
                                                       f_justification_external_support external_support, void *user_data)
{
    if (!graph || !external_support)
        return NULL;
    s_justification_grounding *grounding = calloc(1, sizeof(*grounding));
    size_t *remaining = NULL, *conclusions = NULL, *queue = NULL;
    s_grounding_edge *edges = NULL;
    if (!grounding || !graph->count)
        return grounding;
    if (graph->count > SIZE_MAX / sizeof(*grounding->nodes) || graph->count > SIZE_MAX / sizeof(size_t) ||
        graph->count > SIZE_MAX / 2)
        goto error;
    size_t slots = 2;
    while (slots < graph->count * 2) {
        if (slots > SIZE_MAX / 2)
            goto error;
        slots *= 2;
    }
    if (slots > SIZE_MAX / sizeof(*grounding->slots))
        goto error;
    grounding->slot_count = slots;
    grounding->nodes = calloc(graph->count, sizeof(*grounding->nodes));
    grounding->slots = calloc(slots, sizeof(*grounding->slots));
    remaining = calloc(graph->count, sizeof(*remaining));
    conclusions = malloc(graph->count * sizeof(*conclusions));
    queue = malloc(graph->count * sizeof(*queue));
    if (!grounding->nodes || !grounding->slots || !remaining || !conclusions || !queue)
        goto error;

    size_t support_count = 0;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *proof = &graph->items[i];
        if (proof->support_count > SIZE_MAX - support_count)
            goto error;
        support_count += proof->support_count;
        size_t slot = grounding_slot(grounding, proof->conclusion_hash, proof->s, proof->p, proof->o);
        if (!grounding->slots[slot]) {
            size_t s_len = strlen(proof->s) + 1;
            size_t p_len = strlen(proof->p) + 1;
            size_t o_len = strlen(proof->o) + 1;
            if (s_len > SIZE_MAX - p_len || s_len + p_len > SIZE_MAX - o_len)
                goto error;
            char *key = malloc(s_len + p_len + o_len);
            if (!key)
                goto error;
            s_grounding_node *node = &grounding->nodes[grounding->node_count];
            node->s = key;
            node->p = key + s_len;
            node->o = key + s_len + p_len;
            memcpy(node->s, proof->s, s_len);
            memcpy(node->p, proof->p, p_len);
            memcpy(node->o, proof->o, o_len);
            node->hash = proof->conclusion_hash;
            node->dependents = SIZE_MAX;
            grounding->slots[slot] = ++grounding->node_count;
        }
        conclusions[i] = grounding->slots[slot] - 1;
    }
    if (support_count > SIZE_MAX / sizeof(*edges))
        goto error;
    edges = support_count ? malloc(support_count * sizeof(*edges)) : NULL;
    if (support_count && !edges)
        goto error;

    size_t edge_count = 0, queued = 0;
    for (size_t i = 0; i < graph->count; i++) {
        const s_justification *proof = &graph->items[i];
        for (size_t j = 0; j < proof->support_count; j++) {
            const s_justification_support *support = &proof->supports[j];
            int supported = external_support(support, user_data);
            if (supported < 0)
                goto error;
            if (supported)
                continue;
            if (support->negated) {
                remaining[i] = SIZE_MAX;
                break;
            }
            size_t slot =
                grounding_slot(grounding, hash_triple(support->s, support->p, support->o), support->s, support->p, support->o);
            if (!grounding->slots[slot]) {
                remaining[i] = SIZE_MAX;
                break;
            }
            s_grounding_node *dependency = &grounding->nodes[grounding->slots[slot] - 1];
            edges[edge_count].proof = i;
            edges[edge_count].next = dependency->dependents;
            dependency->dependents = edge_count++;
            remaining[i]++;
        }
        if (!remaining[i] && !grounding->nodes[conclusions[i]].grounded) {
            grounding->nodes[conclusions[i]].grounded = 1;
            queue[queued++] = conclusions[i];
        }
    }

    /* a proof becomes grounded once all its positive dependencies do; cycles
     * without an external root never enter the queue. each edge is visited once. */
    for (size_t next = 0; next < queued; next++) {
        for (size_t edge = grounding->nodes[queue[next]].dependents; edge != SIZE_MAX; edge = edges[edge].next) {
            size_t proof = edges[edge].proof;
            if (remaining[proof] == SIZE_MAX || !remaining[proof])
                continue;
            if (--remaining[proof] == 0 && !grounding->nodes[conclusions[proof]].grounded) {
                grounding->nodes[conclusions[proof]].grounded = 1;
                queue[queued++] = conclusions[proof];
            }
        }
    }
    free(edges);
    free(queue);
    free(conclusions);
    free(remaining);
    return grounding;

error:
    free(edges);
    free(queue);
    free(conclusions);
    free(remaining);
    delete_justification_grounding(grounding);
    return NULL;
}

int justification_graph_is_grounded(const s_justification_graph *graph, const char *s, const char *p, const char *o,
                                    f_justification_external_support external_support, void *user_data)
{
    if (!s || !p || !o)
        return -1;
    s_justification_grounding *grounding = new_justification_grounding(graph, external_support, user_data);
    if (!grounding)
        return -1;
    int result = justification_grounding_contains(grounding, s, p, o);
    delete_justification_grounding(grounding);
    return result;
}
