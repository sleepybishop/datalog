#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

void arena_init(void);
void arena_destroy(void);

void *arena_alloc_fact(void);
void arena_free_fact(void *ptr);

// Rax Arena Allocator
struct rax_arena {
    char *buf;
    size_t offset;
    size_t capacity;
    struct rax_arena *next;
    size_t chunk_size;
};
typedef struct rax_arena s_rax_arena;

s_rax_arena *rax_arena_create(size_t chunk_size);
void rax_arena_destroy(s_rax_arena *arena);
void *rax_arena_alloc(s_rax_arena *arena, size_t size);

extern _Thread_local s_rax_arena *g_rax_current_arena;

#endif
