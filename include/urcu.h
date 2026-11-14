#ifndef URCU_H
#define URCU_H

#include <stdlib.h>

typedef struct urcu_context urcu_t;

static inline void urcu_retire(urcu_t *rcu, void *ptr, void (*free_fn)(void *))
{
    (void)rcu;
    if (free_fn)
        free_fn(ptr);
    else
        free(ptr);
}

#endif /* URCU_H */
