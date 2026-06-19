#ifndef HEXASTORE_H
#define HEXASTORE_H

struct rax;
struct fact;

struct rax_arena;

typedef struct hexastore {
    struct rax *trie_spo;
    struct rax *trie_pos;
    struct rax *trie_osp;
    struct rax_arena *arena;
} s_hexastore;

s_hexastore *new_hexastore(void);
void delete_hexastore(s_hexastore *h);
void hexastore_insert(s_hexastore *h, struct fact *f);
void hexastore_remove(s_hexastore *h, struct fact *f);
void hexastore_compact(s_hexastore *h);

#endif
