#include "justification.h"
#include "wyhash.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct justification_payload {
    _Atomic size_t refcount;
    size_t rule_index;
    size_t support_count;
    uint64_t conclusion_hash;
    uint64_t hash;
    char *s;
    char *p;
    char *o;
    s_justification_support supports[];
} s_justification_payload;

typedef struct conc_entry {
    s_justification_payload *key_owner;
    const char *s;
    const char *p;
    const char *o;
    uint64_t hash;
    size_t count;
} s_conc_entry;

typedef struct sup_entry {
    s_justification_payload *key_owner;
    const char *s;
    const char *p;
    const char *o;
    int negated;
    uint64_t hash;
    size_t head_edge;
} s_sup_entry;

typedef struct sup_edge {
    size_t proof_idx;
    size_t next;
} s_sup_edge;

struct justification_graph {
    s_justification_payload **items;
    size_t item_count;
    size_t item_cap;
    size_t live_count;

    /* open-addressed proof table: 0 = empty, SIZE_MAX = tombstone, idx + 1 = items[idx] */
    size_t *proof_slots;
    size_t proof_slot_cap;
    size_t proof_slot_used;

    /* open-addressed conclusion -> proof count table */
    s_conc_entry *conc_entries;
    size_t conc_count;
    size_t conc_cap;
    size_t *conc_slots;
    size_t conc_slot_cap;

    /* open-addressed support -> edge list table */
    s_sup_entry *sup_entries;
    size_t sup_count;
    size_t sup_cap;
    size_t *sup_slots;
    size_t sup_slot_cap;

    s_sup_edge *sup_edges;
    size_t sup_edge_count;
    size_t sup_edge_cap;
};

static uint64_t hash_triple(const char *s, const char *p, const char *o)
{
    uint64_t h = wyhash(s, strlen(s), 0, _wyp);
    h = wyhash(p, strlen(p), h, _wyp);
    h = wyhash(o, strlen(o), h, _wyp);
    return h;
}

static uint64_t hash_support(const char *s, const char *p, const char *o, int negated)
{
    int norm_neg = negated ? 1 : 0;
    uint64_t h = hash_triple(s, p, o);
    return wyhash(&norm_neg, sizeof(norm_neg), h, _wyp);
}

static uint64_t hash_justification(uint64_t conclusion_hash, size_t rule_index, const s_justification_support *supports,
                                   size_t support_count)
{
    uint64_t h = wyhash(&rule_index, sizeof(rule_index), conclusion_hash, _wyp);
    for (size_t i = 0; i < support_count; i++) {
        int norm_neg = supports[i].negated ? 1 : 0;
        h = wyhash(supports[i].s, strlen(supports[i].s), h, _wyp);
        h = wyhash(supports[i].p, strlen(supports[i].p), h, _wyp);
        h = wyhash(supports[i].o, strlen(supports[i].o), h, _wyp);
        h = wyhash(&norm_neg, sizeof(norm_neg), h, _wyp);
    }
    return h;
}

static s_justification_payload *payload_ref(s_justification_payload *p)
{
    if (p)
        atomic_fetch_add_explicit(&p->refcount, 1, memory_order_relaxed);
    return p;
}

static void payload_unref(s_justification_payload *p)
{
    if (p && atomic_fetch_sub_explicit(&p->refcount, 1, memory_order_acq_rel) == 1)
        free(p);
}

static s_justification_payload *payload_create(uint64_t conclusion_hash, uint64_t hash, const char *s, const char *p, const char *o,
                                               size_t rule_index, const s_justification_support *supports, size_t support_count)
{
    size_t s_len = strlen(s) + 1;
    size_t p_len = strlen(p) + 1;
    size_t o_len = strlen(o) + 1;
    if (s_len > SIZE_MAX - p_len || s_len + p_len > SIZE_MAX - o_len)
        return NULL;
    size_t str_bytes = s_len + p_len + o_len;
    for (size_t i = 0; i < support_count; i++) {
        if (!supports[i].s || !supports[i].p || !supports[i].o)
            return NULL;
        size_t l1 = strlen(supports[i].s) + 1;
        size_t l2 = strlen(supports[i].p) + 1;
        size_t l3 = strlen(supports[i].o) + 1;
        if (l1 > SIZE_MAX - l2 || l1 + l2 > SIZE_MAX - l3 || str_bytes > SIZE_MAX - (l1 + l2 + l3))
            return NULL;
        str_bytes += l1 + l2 + l3;
    }
    if (support_count > (SIZE_MAX - sizeof(s_justification_payload)) / sizeof(s_justification_support))
        return NULL;
    size_t header_bytes = sizeof(s_justification_payload) + support_count * sizeof(s_justification_support);
    if (header_bytes > SIZE_MAX - str_bytes)
        return NULL;

    s_justification_payload *item = malloc(header_bytes + str_bytes);
    if (!item)
        return NULL;
    atomic_init(&item->refcount, 1);
    item->rule_index = rule_index;
    item->support_count = support_count;
    item->conclusion_hash = conclusion_hash;
    item->hash = hash;

    char *cursor = (char *)item + header_bytes;
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
        size_t l1 = strlen(supports[i].s) + 1;
        size_t l2 = strlen(supports[i].p) + 1;
        size_t l3 = strlen(supports[i].o) + 1;
        item->supports[i].s = cursor;
        memcpy(cursor, supports[i].s, l1);
        cursor += l1;
        item->supports[i].p = cursor;
        memcpy(cursor, supports[i].p, l2);
        cursor += l2;
        item->supports[i].o = cursor;
        memcpy(cursor, supports[i].o, l3);
        cursor += l3;
        item->supports[i].negated = supports[i].negated ? 1 : 0;
    }
    return item;
}

s_justification_graph *new_justification_graph(void)
{
    return calloc(1, sizeof(s_justification_graph));
}

static void graph_clear_contents(s_justification_graph *graph)
{
    for (size_t i = 0; i < graph->item_count; i++)
        payload_unref(graph->items[i]);
    for (size_t i = 0; i < graph->conc_count; i++)
        payload_unref(graph->conc_entries[i].key_owner);
    for (size_t i = 0; i < graph->sup_count; i++)
        payload_unref(graph->sup_entries[i].key_owner);
    free(graph->items);
    free(graph->proof_slots);
    free(graph->conc_entries);
    free(graph->conc_slots);
    free(graph->sup_entries);
    free(graph->sup_slots);
    free(graph->sup_edges);
    memset(graph, 0, sizeof(*graph));
}

void delete_justification_graph(s_justification_graph *graph)
{
    if (!graph)
        return;
    graph_clear_contents(graph);
    free(graph);
}

static int support_equal(const s_justification_support *a, const s_justification_support *b)
{
    int na = a->negated ? 1 : 0;
    int nb = b->negated ? 1 : 0;
    return na == nb && strcmp(a->s, b->s) == 0 && strcmp(a->p, b->p) == 0 && strcmp(a->o, b->o) == 0;
}

static int payload_equal_raw(const s_justification_payload *item, uint64_t conclusion_hash, uint64_t hash, const char *s,
                             const char *p, const char *o, size_t rule_index, const s_justification_support *supports,
                             size_t support_count)
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

static int reserve_proof_slots(s_justification_graph *graph, size_t needed_used)
{
    if (graph->proof_slot_cap > 0 && needed_used * 2 <= graph->proof_slot_cap)
        return 0;
    size_t new_cap;
    if (graph->proof_slot_cap >= 32 && (graph->live_count + 1) * 4 <= graph->proof_slot_cap)
        new_cap = graph->proof_slot_cap;
    else
        new_cap = graph->proof_slot_cap ? graph->proof_slot_cap * 2 : 32;
    while (new_cap < (graph->live_count + 1) * 2) {
        if (new_cap > SIZE_MAX / 2)
            return -1;
        new_cap *= 2;
    }
    if (new_cap > SIZE_MAX / sizeof(*graph->proof_slots))
        return -1;
    size_t *new_slots = calloc(new_cap, sizeof(*new_slots));
    if (!new_slots)
        return -1;
    size_t used = 0;
    for (size_t i = 0; i < graph->item_count; i++) {
        s_justification_payload *p = graph->items[i];
        if (!p)
            continue;
        size_t slot = (size_t)p->hash & (new_cap - 1);
        while (new_slots[slot])
            slot = (slot + 1) & (new_cap - 1);
        new_slots[slot] = i + 1;
        used++;
    }
    free(graph->proof_slots);
    graph->proof_slots = new_slots;
    graph->proof_slot_cap = new_cap;
    graph->proof_slot_used = used;
    return 0;
}

static int reserve_conc_table(s_justification_graph *graph, size_t needed)
{
    if (needed > graph->conc_cap) {
        size_t new_cap = graph->conc_cap ? graph->conc_cap * 2 : 16;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*graph->conc_entries))
            return -1;
        s_conc_entry *entries = realloc(graph->conc_entries, new_cap * sizeof(*entries));
        if (!entries)
            return -1;
        graph->conc_entries = entries;
        graph->conc_cap = new_cap;
    }
    if (graph->conc_slot_cap == 0 || needed * 2 > graph->conc_slot_cap) {
        size_t new_cap = graph->conc_slot_cap ? graph->conc_slot_cap * 2 : 32;
        while (new_cap < needed * 2) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*graph->conc_slots))
            return -1;
        size_t *new_slots = calloc(new_cap, sizeof(*new_slots));
        if (!new_slots)
            return -1;
        for (size_t i = 0; i < graph->conc_count; i++) {
            size_t slot = (size_t)graph->conc_entries[i].hash & (new_cap - 1);
            while (new_slots[slot])
                slot = (slot + 1) & (new_cap - 1);
            new_slots[slot] = i + 1;
        }
        free(graph->conc_slots);
        graph->conc_slots = new_slots;
        graph->conc_slot_cap = new_cap;
    }
    return 0;
}

static int reserve_sup_table(s_justification_graph *graph, size_t needed_entries, size_t needed_edges)
{
    if (needed_entries > graph->sup_cap) {
        size_t new_cap = graph->sup_cap ? graph->sup_cap * 2 : 16;
        while (new_cap < needed_entries) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*graph->sup_entries))
            return -1;
        s_sup_entry *entries = realloc(graph->sup_entries, new_cap * sizeof(*entries));
        if (!entries)
            return -1;
        graph->sup_entries = entries;
        graph->sup_cap = new_cap;
    }
    if (graph->sup_slot_cap == 0 || needed_entries * 2 > graph->sup_slot_cap) {
        size_t new_cap = graph->sup_slot_cap ? graph->sup_slot_cap * 2 : 32;
        while (new_cap < needed_entries * 2) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*graph->sup_slots))
            return -1;
        size_t *new_slots = calloc(new_cap, sizeof(*new_slots));
        if (!new_slots)
            return -1;
        for (size_t i = 0; i < graph->sup_count; i++) {
            size_t slot = (size_t)graph->sup_entries[i].hash & (new_cap - 1);
            while (new_slots[slot])
                slot = (slot + 1) & (new_cap - 1);
            new_slots[slot] = i + 1;
        }
        free(graph->sup_slots);
        graph->sup_slots = new_slots;
        graph->sup_slot_cap = new_cap;
    }
    if (needed_edges > graph->sup_edge_cap) {
        size_t new_cap = graph->sup_edge_cap ? graph->sup_edge_cap * 2 : 32;
        while (new_cap < needed_edges) {
            if (new_cap > SIZE_MAX / 2)
                return -1;
            new_cap *= 2;
        }
        if (new_cap > SIZE_MAX / sizeof(*graph->sup_edges))
            return -1;
        s_sup_edge *edges = realloc(graph->sup_edges, new_cap * sizeof(*edges));
        if (!edges)
            return -1;
        graph->sup_edges = edges;
        graph->sup_edge_cap = new_cap;
    }
    return 0;
}

static size_t find_conc_slot(const s_justification_graph *graph, uint64_t hash, const char *s, const char *p, const char *o)
{
    size_t slot = (size_t)hash & (graph->conc_slot_cap - 1);
    while (graph->conc_slots[slot]) {
        const s_conc_entry *e = &graph->conc_entries[graph->conc_slots[slot] - 1];
        if (e->hash == hash && strcmp(e->s, s) == 0 && strcmp(e->p, p) == 0 && strcmp(e->o, o) == 0)
            break;
        slot = (slot + 1) & (graph->conc_slot_cap - 1);
    }
    return slot;
}

static size_t find_sup_slot(const s_justification_graph *graph, uint64_t hash, const char *s, const char *p, const char *o,
                            int negated)
{
    int norm_neg = negated ? 1 : 0;
    size_t slot = (size_t)hash & (graph->sup_slot_cap - 1);
    while (graph->sup_slots[slot]) {
        const s_sup_entry *e = &graph->sup_entries[graph->sup_slots[slot] - 1];
        if (e->hash == hash && e->negated == norm_neg && strcmp(e->s, s) == 0 && strcmp(e->p, p) == 0 && strcmp(e->o, o) == 0)
            break;
        slot = (slot + 1) & (graph->sup_slot_cap - 1);
    }
    return slot;
}

static int graph_insert_payload(s_justification_graph *graph, s_justification_payload *payload)
{
    if (graph->item_count == graph->item_cap) {
        size_t new_cap = graph->item_cap ? graph->item_cap * 2 : 16;
        if (new_cap < graph->item_cap || new_cap > SIZE_MAX / sizeof(*graph->items))
            return -1;
        s_justification_payload **items = realloc(graph->items, new_cap * sizeof(*items));
        if (!items)
            return -1;
        graph->items = items;
        graph->item_cap = new_cap;
    }
    if (payload->support_count > SIZE_MAX - graph->sup_count || payload->support_count > SIZE_MAX - graph->sup_edge_count)
        return -1;
    if (reserve_proof_slots(graph, graph->proof_slot_used + 1) != 0)
        return -1;
    if (reserve_conc_table(graph, graph->conc_count + 1) != 0)
        return -1;
    if (reserve_sup_table(graph, graph->sup_count + payload->support_count, graph->sup_edge_count + payload->support_count) != 0)
        return -1;

    size_t idx = graph->item_count++;
    graph->items[idx] = payload_ref(payload);
    graph->live_count++;

    /* 1. insert into proof_slots */
    size_t pslot = (size_t)payload->hash & (graph->proof_slot_cap - 1);
    size_t first_tombstone = SIZE_MAX;
    while (graph->proof_slots[pslot]) {
        if (graph->proof_slots[pslot] == SIZE_MAX && first_tombstone == SIZE_MAX)
            first_tombstone = pslot;
        pslot = (pslot + 1) & (graph->proof_slot_cap - 1);
    }
    if (first_tombstone != SIZE_MAX) {
        graph->proof_slots[first_tombstone] = idx + 1;
    } else {
        graph->proof_slots[pslot] = idx + 1;
        graph->proof_slot_used++;
    }

    /* 2. increment conclusion count */
    size_t cslot = find_conc_slot(graph, payload->conclusion_hash, payload->s, payload->p, payload->o);
    if (graph->conc_slots[cslot]) {
        graph->conc_entries[graph->conc_slots[cslot] - 1].count++;
    } else {
        size_t cidx = graph->conc_count++;
        s_conc_entry *ce = &graph->conc_entries[cidx];
        ce->key_owner = payload_ref(payload);
        ce->s = payload->s;
        ce->p = payload->p;
        ce->o = payload->o;
        ce->hash = payload->conclusion_hash;
        ce->count = 1;
        graph->conc_slots[cslot] = cidx + 1;
    }

    /* 3. add support reverse edges (skipping duplicate supports in the same proof) */
    for (size_t i = 0; i < payload->support_count; i++) {
        const s_justification_support *sup = &payload->supports[i];
        int dup = 0;
        for (size_t j = 0; j < i; j++) {
            if (support_equal(sup, &payload->supports[j])) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        uint64_t shash = hash_support(sup->s, sup->p, sup->o, sup->negated);
        size_t sslot = find_sup_slot(graph, shash, sup->s, sup->p, sup->o, sup->negated);
        size_t sidx;
        if (graph->sup_slots[sslot]) {
            sidx = graph->sup_slots[sslot] - 1;
        } else {
            sidx = graph->sup_count++;
            s_sup_entry *se = &graph->sup_entries[sidx];
            se->key_owner = payload_ref(payload);
            se->s = sup->s;
            se->p = sup->p;
            se->o = sup->o;
            se->negated = sup->negated ? 1 : 0;
            se->hash = shash;
            se->head_edge = SIZE_MAX;
            graph->sup_slots[sslot] = sidx + 1;
        }
        size_t eidx = graph->sup_edge_count++;
        graph->sup_edges[eidx].proof_idx = idx;
        graph->sup_edges[eidx].next = graph->sup_entries[sidx].head_edge;
        graph->sup_entries[sidx].head_edge = eidx;
    }
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

    if (graph->proof_slot_cap > 0) {
        size_t slot = (size_t)hash & (graph->proof_slot_cap - 1);
        while (graph->proof_slots[slot]) {
            size_t val = graph->proof_slots[slot];
            if (val != SIZE_MAX) {
                const s_justification_payload *item = graph->items[val - 1];
                if (item && payload_equal_raw(item, conclusion_hash, hash, s, p, o, rule_index, supports, support_count))
                    return 0;
            }
            slot = (slot + 1) & (graph->proof_slot_cap - 1);
        }
    }

    s_justification_payload *payload = payload_create(conclusion_hash, hash, s, p, o, rule_index, supports, support_count);
    if (!payload)
        return -1;
    int rc = graph_insert_payload(graph, payload);
    payload_unref(payload);
    return rc == 0 ? 1 : -1;
}

static void justification_graph_maybe_compact(s_justification_graph *graph)
{
    if (graph->item_count < 64 || graph->live_count * 2 >= graph->item_count)
        return;
    s_justification_graph *fresh = new_justification_graph();
    if (!fresh)
        return;
    for (size_t i = 0; i < graph->item_count; i++) {
        if (graph->items[i] && graph_insert_payload(fresh, graph->items[i]) != 0) {
            delete_justification_graph(fresh);
            return;
        }
    }
    s_justification_graph tmp = *graph;
    *graph = *fresh;
    *fresh = tmp;
    delete_justification_graph(fresh);
}

s_justification_graph *justification_graph_clone(const s_justification_graph *graph)
{
    s_justification_graph *copy = new_justification_graph();
    if (!copy)
        return NULL;
    if (!graph || graph->live_count == 0)
        return copy;

    /* if the source has accumulated tombstones, clone compactly */
    if (graph->live_count * 2 < graph->item_count) {
        for (size_t i = 0; i < graph->item_count; i++) {
            if (graph->items[i] && graph_insert_payload(copy, graph->items[i]) != 0) {
                delete_justification_graph(copy);
                return NULL;
            }
        }
        return copy;
    }

    copy->items = malloc(graph->item_count * sizeof(*copy->items));
    copy->proof_slots = malloc(graph->proof_slot_cap * sizeof(*copy->proof_slots));
    copy->conc_entries = malloc(graph->conc_count * sizeof(*copy->conc_entries));
    copy->conc_slots = malloc(graph->conc_slot_cap * sizeof(*copy->conc_slots));
    copy->sup_entries = graph->sup_count ? malloc(graph->sup_count * sizeof(*copy->sup_entries)) : NULL;
    copy->sup_slots = graph->sup_slot_cap ? malloc(graph->sup_slot_cap * sizeof(*copy->sup_slots)) : NULL;
    copy->sup_edges = graph->sup_edge_count ? malloc(graph->sup_edge_count * sizeof(*copy->sup_edges)) : NULL;

    if (!copy->items || !copy->proof_slots || !copy->conc_entries || !copy->conc_slots ||
        (graph->sup_count && !copy->sup_entries) || (graph->sup_slot_cap && !copy->sup_slots) ||
        (graph->sup_edge_count && !copy->sup_edges)) {
        free(copy->items);
        free(copy->proof_slots);
        free(copy->conc_entries);
        free(copy->conc_slots);
        free(copy->sup_entries);
        free(copy->sup_slots);
        free(copy->sup_edges);
        free(copy);
        return NULL;
    }

    memcpy(copy->items, graph->items, graph->item_count * sizeof(*copy->items));
    copy->item_count = graph->item_count;
    copy->item_cap = graph->item_count;
    copy->live_count = graph->live_count;
    for (size_t i = 0; i < copy->item_count; i++)
        payload_ref(copy->items[i]);

    memcpy(copy->proof_slots, graph->proof_slots, graph->proof_slot_cap * sizeof(*copy->proof_slots));
    copy->proof_slot_cap = graph->proof_slot_cap;
    copy->proof_slot_used = graph->proof_slot_used;

    memcpy(copy->conc_entries, graph->conc_entries, graph->conc_count * sizeof(*copy->conc_entries));
    copy->conc_count = graph->conc_count;
    copy->conc_cap = graph->conc_count;
    for (size_t i = 0; i < copy->conc_count; i++)
        payload_ref(copy->conc_entries[i].key_owner);

    memcpy(copy->conc_slots, graph->conc_slots, graph->conc_slot_cap * sizeof(*copy->conc_slots));
    copy->conc_slot_cap = graph->conc_slot_cap;

    if (graph->sup_count) {
        memcpy(copy->sup_entries, graph->sup_entries, graph->sup_count * sizeof(*copy->sup_entries));
        copy->sup_count = graph->sup_count;
        copy->sup_cap = graph->sup_count;
        for (size_t i = 0; i < copy->sup_count; i++)
            payload_ref(copy->sup_entries[i].key_owner);
    }
    if (graph->sup_slot_cap) {
        memcpy(copy->sup_slots, graph->sup_slots, graph->sup_slot_cap * sizeof(*copy->sup_slots));
        copy->sup_slot_cap = graph->sup_slot_cap;
    }
    if (graph->sup_edge_count) {
        memcpy(copy->sup_edges, graph->sup_edges, graph->sup_edge_count * sizeof(*copy->sup_edges));
        copy->sup_edge_count = graph->sup_edge_count;
        copy->sup_edge_cap = graph->sup_edge_count;
    }

    return copy;
}

size_t justification_graph_count(const s_justification_graph *graph, const char *s, const char *p, const char *o)
{
    if (!graph || !s || !p || !o || graph->conc_slot_cap == 0)
        return 0;
    uint64_t hash = hash_triple(s, p, o);
    size_t slot = find_conc_slot(graph, hash, s, p, o);
    if (!graph->conc_slots[slot])
        return 0;
    return graph->conc_entries[graph->conc_slots[slot] - 1].count;
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

typedef struct out_conc_set {
    s_justification_support *items;
    uint64_t *hashes;
    size_t count;
    size_t cap;
    size_t *slots;
    size_t slot_cap;
} s_out_conc_set;

static int out_conc_append(s_out_conc_set *out, const s_justification_payload *proof)
{
    if (out->slot_cap > 0) {
        size_t slot = (size_t)proof->conclusion_hash & (out->slot_cap - 1);
        while (out->slots[slot]) {
            size_t idx = out->slots[slot] - 1;
            if (out->hashes[idx] == proof->conclusion_hash && strcmp(out->items[idx].s, proof->s) == 0 &&
                strcmp(out->items[idx].p, proof->p) == 0 && strcmp(out->items[idx].o, proof->o) == 0)
                return 0;
            slot = (slot + 1) & (out->slot_cap - 1);
        }
    } else {
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->items[i].s, proof->s) == 0 && strcmp(out->items[i].p, proof->p) == 0 &&
                strcmp(out->items[i].o, proof->o) == 0)
                return 0;
        }
    }

    if (out->count == out->cap) {
        size_t new_cap = out->cap ? out->cap * 2 : 8;
        if (new_cap < out->cap || new_cap > SIZE_MAX / sizeof(*out->items) || new_cap > SIZE_MAX / sizeof(*out->hashes))
            return -1;
        s_justification_support *items = realloc(out->items, new_cap * sizeof(*items));
        if (!items)
            return -1;
        out->items = items;
        uint64_t *hashes = realloc(out->hashes, new_cap * sizeof(*hashes));
        if (!hashes)
            return -1;
        out->hashes = hashes;
        out->cap = new_cap;
    }

    if (out->count >= 8 && (out->slot_cap == 0 || (out->count + 1) * 2 > out->slot_cap)) {
        size_t new_scap = out->slot_cap ? out->slot_cap * 2 : 32;
        if (new_scap > SIZE_MAX / sizeof(*out->slots))
            return -1;
        size_t *slots = calloc(new_scap, sizeof(*slots));
        if (!slots)
            return -1;
        for (size_t i = 0; i < out->count; i++) {
            size_t s = (size_t)out->hashes[i] & (new_scap - 1);
            while (slots[s])
                s = (s + 1) & (new_scap - 1);
            slots[s] = i + 1;
        }
        free(out->slots);
        out->slots = slots;
        out->slot_cap = new_scap;
    }

    s_justification_support *item = &out->items[out->count];
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
    out->hashes[out->count] = proof->conclusion_hash;
    if (out->slot_cap > 0) {
        size_t s = (size_t)proof->conclusion_hash & (out->slot_cap - 1);
        while (out->slots[s])
            s = (s + 1) & (out->slot_cap - 1);
        out->slots[s] = out->count + 1;
    }
    out->count++;
    return 0;
}

int justification_graph_remove_support(s_justification_graph *graph, const char *s, const char *p, const char *o, int negated,
                                       s_justification_support **conclusions_out, size_t *conclusion_count_out)
{
    if (!graph || !s || !p || !o || !conclusions_out || !conclusion_count_out)
        return -1;
    *conclusions_out = NULL;
    *conclusion_count_out = 0;

    if (graph->sup_slot_cap == 0 || graph->live_count == 0)
        return 0;

    uint64_t shash = hash_support(s, p, o, negated);
    size_t sslot = find_sup_slot(graph, shash, s, p, o, negated);
    if (!graph->sup_slots[sslot])
        return 0;
    s_sup_entry *se = &graph->sup_entries[graph->sup_slots[sslot] - 1];
    if (se->head_edge == SIZE_MAX)
        return 0;

    s_out_conc_set out = {0};
    for (size_t e = se->head_edge; e != SIZE_MAX; e = graph->sup_edges[e].next) {
        const s_justification_payload *proof = graph->items[graph->sup_edges[e].proof_idx];
        if (!proof)
            continue;
        if (out_conc_append(&out, proof) != 0) {
            justification_support_array_destroy(out.items, out.count);
            free(out.hashes);
            free(out.slots);
            return -1;
        }
    }
    free(out.hashes);
    free(out.slots);

    int removed = 0;
    for (size_t e = se->head_edge; e != SIZE_MAX; e = graph->sup_edges[e].next) {
        size_t pidx = graph->sup_edges[e].proof_idx;
        s_justification_payload *proof = graph->items[pidx];
        if (!proof)
            continue;

        /* mark proof slot tombstone */
        size_t pslot = (size_t)proof->hash & (graph->proof_slot_cap - 1);
        while (graph->proof_slots[pslot]) {
            if (graph->proof_slots[pslot] == pidx + 1) {
                graph->proof_slots[pslot] = SIZE_MAX;
                break;
            }
            pslot = (pslot + 1) & (graph->proof_slot_cap - 1);
        }

        /* decrement conclusion count */
        size_t cslot = find_conc_slot(graph, proof->conclusion_hash, proof->s, proof->p, proof->o);
        if (graph->conc_slots[cslot]) {
            s_conc_entry *ce = &graph->conc_entries[graph->conc_slots[cslot] - 1];
            if (ce->count > 0)
                ce->count--;
        }

        graph->items[pidx] = NULL;
        graph->live_count--;
        payload_unref(proof);
        removed++;
    }
    se->head_edge = SIZE_MAX;

    justification_graph_maybe_compact(graph);

    *conclusions_out = out.items;
    *conclusion_count_out = out.count;
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
    if (!grounding || !graph->live_count)
        return grounding;
    size_t proof_count = graph->live_count;
    if (proof_count > SIZE_MAX / sizeof(*grounding->nodes) || proof_count > SIZE_MAX / sizeof(size_t) || proof_count > SIZE_MAX / 2)
        goto error;
    size_t slots = 2;
    while (slots < proof_count * 2) {
        if (slots > SIZE_MAX / 2)
            goto error;
        slots *= 2;
    }
    if (slots > SIZE_MAX / sizeof(*grounding->slots))
        goto error;
    grounding->slot_count = slots;
    grounding->nodes = calloc(proof_count, sizeof(*grounding->nodes));
    grounding->slots = calloc(slots, sizeof(*grounding->slots));
    remaining = calloc(proof_count, sizeof(*remaining));
    conclusions = malloc(proof_count * sizeof(*conclusions));
    queue = malloc(proof_count * sizeof(*queue));
    if (!grounding->nodes || !grounding->slots || !remaining || !conclusions || !queue)
        goto error;

    size_t support_count = 0;
    size_t live_idx = 0;
    for (size_t raw = 0; raw < graph->item_count; raw++) {
        const s_justification_payload *proof = graph->items[raw];
        if (!proof)
            continue;
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
        conclusions[live_idx++] = grounding->slots[slot] - 1;
    }
    if (support_count > SIZE_MAX / sizeof(*edges))
        goto error;
    edges = support_count ? malloc(support_count * sizeof(*edges)) : NULL;
    if (support_count && !edges)
        goto error;

    size_t edge_count = 0, queued = 0;
    live_idx = 0;
    for (size_t raw = 0; raw < graph->item_count; raw++) {
        const s_justification_payload *proof = graph->items[raw];
        if (!proof)
            continue;
        size_t i = live_idx++;
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
