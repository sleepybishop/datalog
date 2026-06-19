#include <stddef.h>
#include <assert.h>
#include "hexastore.h"
#include "rax.h"
#include "fact.h"
#include "intern.h"
#include "arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline uint64_t get_symbol_id(Symbol ptr)
{
    if (ptr == P_FIRST)
        return 0;
    if (ptr == P_LAST)
        return 0xFFFFFFFFFFFFFFFFULL;
    return ptr->id;
}

static inline void encode_uint64_be(unsigned char *buf, uint64_t val)
{
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8) & 0xFF;
    buf[7] = val & 0xFF;
}

static inline void encode_triple_key(unsigned char *key, Symbol c1, Symbol c2, Symbol c3)
{
    encode_uint64_be(key, get_symbol_id(c1));
    encode_uint64_be(key + 8, get_symbol_id(c2));
    encode_uint64_be(key + 16, get_symbol_id(c3));
}

s_hexastore *new_hexastore(void)
{
    s_hexastore *h = malloc(sizeof(s_hexastore));
    if (h) {
        h->arena = rax_arena_create(1024 * 1024); // 1 MB chunks
        assert(h->arena);
        g_rax_current_arena = h->arena;
        h->trie_spo = raxNew();
        h->trie_pos = raxNew();
        h->trie_osp = raxNew();
        g_rax_current_arena = NULL;
    }
    return h;
}

void delete_hexastore(s_hexastore *h)
{
    if (h) {
        rax_arena_destroy(h->arena);
        free(h);
    }
}

void hexastore_insert(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];
    g_rax_current_arena = h->arena;

    // SPO key
    encode_triple_key(key, f->s, f->p, f->o);
    raxInsert(h->trie_spo, key, 24, f, NULL);

    // POS key
    encode_triple_key(key, f->p, f->o, f->s);
    raxInsert(h->trie_pos, key, 24, f, NULL);

    // OSP key
    encode_triple_key(key, f->o, f->s, f->p);
    raxInsert(h->trie_osp, key, 24, f, NULL);

    g_rax_current_arena = NULL;
}

void hexastore_remove(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];
    g_rax_current_arena = h->arena;

    // SPO key
    encode_triple_key(key, f->s, f->p, f->o);
    raxRemove(h->trie_spo, key, 24, NULL);

    // POS key
    encode_triple_key(key, f->p, f->o, f->s);
    raxRemove(h->trie_pos, key, 24, NULL);

    // OSP key
    encode_triple_key(key, f->o, f->s, f->p);
    raxRemove(h->trie_osp, key, 24, NULL);

    g_rax_current_arena = NULL;
}

#define raxPadding(nodesize) ((sizeof(void *) - (((nodesize) + 4) % sizeof(void *))) & (sizeof(void *) - 1))

static inline size_t rax_node_current_length(raxNode *n)
{
    return sizeof(raxNode) + n->size + raxPadding(n->size) + (n->iscompr ? sizeof(raxNode *) : sizeof(raxNode *) * n->size) +
           ((n->iskey && !n->isnull) ? sizeof(void *) : 0);
}

static inline raxNode **rax_node_last_child_ptr(raxNode *n)
{
    return (raxNode **)((char *)n + rax_node_current_length(n) - sizeof(raxNode *) -
                        ((n->iskey && !n->isnull) ? sizeof(void *) : 0));
}

static raxNode *rax_node_compact(s_rax_arena *new_arena, raxNode *n)
{
    if (!n)
        return NULL;

    size_t len = rax_node_current_length(n);
    size_t total_size = len + sizeof(size_t);
    void *mem = rax_arena_alloc(new_arena, total_size);
    if (!mem)
        return NULL;
    *(size_t *)mem = total_size;
    raxNode *new_node = (raxNode *)((char *)mem + sizeof(size_t));

    memcpy(new_node, n, len);

    int num_children = new_node->iscompr ? 1 : new_node->size;
    raxNode **new_last_child = rax_node_last_child_ptr(new_node);
    raxNode **old_last_child = rax_node_last_child_ptr(n);

    for (int i = 0; i < num_children; i++) {
        raxNode **new_child_slot = new_last_child - i;
        raxNode **old_child_slot = old_last_child - i;
        raxNode *old_child;
        memcpy(&old_child, old_child_slot, sizeof(old_child));

        if (old_child) {
            raxNode *new_child = rax_node_compact(new_arena, old_child);
            memcpy(new_child_slot, &new_child, sizeof(new_child));
        }
    }

    return new_node;
}

static rax *rax_compact(s_rax_arena *new_arena, rax *rt)
{
    if (!rt)
        return NULL;

    raxNode *new_head = rax_node_compact(new_arena, rt->head);
    if (!new_head)
        return NULL;

    size_t size = sizeof(rax);
    size_t total_size = size + sizeof(size_t);
    void *mem = rax_arena_alloc(new_arena, total_size);
    if (!mem)
        return NULL;
    *(size_t *)mem = total_size;
    rax *new_rt = (rax *)((char *)mem + sizeof(size_t));

    memcpy(new_rt, rt, sizeof(rax));
    new_rt->head = new_head;

    return new_rt;
}

void hexastore_compact(s_hexastore *h)
{
    if (!h || !h->arena)
        return;

    s_rax_arena *new_arena = rax_arena_create(h->arena->chunk_size);
    if (!new_arena)
        return;

    rax *new_spo = rax_compact(new_arena, h->trie_spo);
    rax *new_pos = rax_compact(new_arena, h->trie_pos);
    rax *new_osp = rax_compact(new_arena, h->trie_osp);

    if (new_spo && new_pos && new_osp) {
        h->trie_spo = new_spo;
        h->trie_pos = new_pos;
        h->trie_osp = new_osp;

        rax_arena_destroy(h->arena);
        h->arena = new_arena;
    } else {
        rax_arena_destroy(new_arena);
    }
}
