#include <stddef.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include "hexastore.h"
#include "rax.h"
#include "fact.h"
#include "intern.h"

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

int hexastore_insert(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];

    /* SPO key */
    encode_triple_key(key, f->s, f->p, f->o);
    if (raxInsert(h->trie_spo, key, 24, f, NULL) != 1)
        return -1;

    /* POS key */
    encode_triple_key(key, f->p, f->o, f->s);
    if (raxInsert(h->trie_pos, key, 24, f, NULL) != 1) {
        encode_triple_key(key, f->s, f->p, f->o);
        raxRemove(h->trie_spo, key, 24, NULL);
        return -1;
    }

    /* OSP key */
    encode_triple_key(key, f->o, f->s, f->p);
    if (raxInsert(h->trie_osp, key, 24, f, NULL) != 1) {
        encode_triple_key(key, f->p, f->o, f->s);
        raxRemove(h->trie_pos, key, 24, NULL);
        encode_triple_key(key, f->s, f->p, f->o);
        raxRemove(h->trie_spo, key, 24, NULL);
        return -1;
    }
    return 0;
}

void hexastore_remove(s_hexastore *h, s_fact *f)
{
    unsigned char key[24];

    /* SPO key */
    encode_triple_key(key, f->s, f->p, f->o);
    raxRemove(h->trie_spo, key, 24, NULL);

    /* POS key */
    encode_triple_key(key, f->p, f->o, f->s);
    raxRemove(h->trie_pos, key, 24, NULL);

    /* OSP key */
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

    /* keep the public rax object addresses stable because s_facts caches them. */
    replace_rax_contents(h->trie_spo, new_spo);
    replace_rax_contents(h->trie_pos, new_pos);
    replace_rax_contents(h->trie_osp, new_osp);
}
