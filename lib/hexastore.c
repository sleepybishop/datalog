#include <stddef.h>
#include <assert.h>
#include "hexastore.h"
#include "rax.h"
#include "fact.h"
#include "intern.h"
#include <stdlib.h>
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
    s_hexastore *h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->trie_spo = raxNew();
    h->trie_pos = raxNew();
    h->trie_osp = raxNew();
    if (!h->trie_spo || !h->trie_pos || !h->trie_osp) {
        if (h->trie_spo)
            raxFree(h->trie_spo);
        if (h->trie_pos)
            raxFree(h->trie_pos);
        if (h->trie_osp)
            raxFree(h->trie_osp);
        free(h);
        return NULL;
    }
    return h;
}

void delete_hexastore(s_hexastore *h)
{
    if (h) {
        raxFree(h->trie_spo);
        raxFree(h->trie_pos);
        raxFree(h->trie_osp);
        free(h);
    }
}

void hexastore_insert(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];

    // SPO key
    encode_triple_key(key, f->s, f->p, f->o);
    raxInsert(h->trie_spo, key, 24, f, NULL);

    // POS key
    encode_triple_key(key, f->p, f->o, f->s);
    raxInsert(h->trie_pos, key, 24, f, NULL);

    // OSP key
    encode_triple_key(key, f->o, f->s, f->p);
    raxInsert(h->trie_osp, key, 24, f, NULL);
}

void hexastore_remove(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];

    // SPO key
    encode_triple_key(key, f->s, f->p, f->o);
    raxRemove(h->trie_spo, key, 24, NULL);

    // POS key
    encode_triple_key(key, f->p, f->o, f->s);
    raxRemove(h->trie_pos, key, 24, NULL);

    // OSP key
    encode_triple_key(key, f->o, f->s, f->p);
    raxRemove(h->trie_osp, key, 24, NULL);
}

static int insert_into_indexes(rax *spo, rax *pos, rax *osp, s_fact *f)
{
    unsigned char key[24];
    encode_triple_key(key, f->s, f->p, f->o);
    if (raxInsert(spo, key, sizeof(key), f, NULL) != 1)
        return 0;
    encode_triple_key(key, f->p, f->o, f->s);
    if (raxInsert(pos, key, sizeof(key), f, NULL) != 1)
        return 0;
    encode_triple_key(key, f->o, f->s, f->p);
    return raxInsert(osp, key, sizeof(key), f, NULL) == 1;
}

static void replace_rax_contents(rax *destination, rax *replacement)
{
    rax old = *destination;
    *destination = *replacement;
    *replacement = old;
    raxFree(replacement);
}

void hexastore_compact(s_hexastore *h)
{
    if (!h)
        return;

    rax *new_spo = raxNew();
    rax *new_pos = raxNew();
    rax *new_osp = raxNew();
    if (!new_spo || !new_pos || !new_osp) {
        if (new_spo)
            raxFree(new_spo);
        if (new_pos)
            raxFree(new_pos);
        if (new_osp)
            raxFree(new_osp);
        return;
    }

    int success = 1;
    raxIterator it;
    raxStart(&it, h->trie_spo);
    raxSeek(&it, "^", NULL, 0);
    while (success && raxNext(&it))
        success = insert_into_indexes(new_spo, new_pos, new_osp, it.data);
    raxStop(&it);

    if (!success) {
        raxFree(new_spo);
        raxFree(new_pos);
        raxFree(new_osp);
        return;
    }

    /* Keep the public rax object addresses stable because s_facts caches them. */
    replace_rax_contents(h->trie_spo, new_spo);
    replace_rax_contents(h->trie_pos, new_pos);
    replace_rax_contents(h->trie_osp, new_osp);
}
