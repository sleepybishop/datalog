#ifndef HEXASTORE_H
#define HEXASTORE_H

#include <stdint.h>
#include "fact.h"

struct rax;

typedef struct hexastore {
    struct rax *trie_spo;
    struct rax *trie_pos;
    struct rax *trie_osp;
} s_hexastore;

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

s_hexastore *new_hexastore(void);
void delete_hexastore(s_hexastore *h);
int hexastore_insert(s_hexastore *h, struct fact *f);
void hexastore_remove(s_hexastore *h, struct fact *f);
void hexastore_compact(s_hexastore *h);

#endif
