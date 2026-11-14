#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

void arena_init(void);
void arena_destroy(void);

void *arena_alloc_fact(void);
void arena_free_fact(void *ptr);

typedef struct rax_arena s_rax_arena;

static inline void *rax_arena_alloc(s_rax_arena *arena, size_t size)
{
    (void)arena;
    (void)size;
    return NULL;
}

#define g_rax_current_arena ((s_rax_arena *)NULL)

#endif
