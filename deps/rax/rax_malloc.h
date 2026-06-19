/* Rax -- A radix tree implementation.
 *
 * Copyright (c) 2017-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#ifndef RAX_ALLOC_H
#define RAX_ALLOC_H

#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "arena.h"

static inline void *rax_malloc(size_t size)
{
    if (g_rax_current_arena) {
        size_t total_size = size + sizeof(size_t);
        void *mem = rax_arena_alloc(g_rax_current_arena, total_size);
        if (!mem)
            return NULL;
        *(size_t *)mem = total_size;
        return (char *)mem + sizeof(size_t);
    }
    return malloc(size);
}

static inline void *rax_realloc(void *ptr, size_t size)
{
    if (g_rax_current_arena) {
        if (!ptr)
            return rax_malloc(size);
        size_t total_size = size + sizeof(size_t);
        void *new_mem = rax_arena_alloc(g_rax_current_arena, total_size);
        if (!new_mem)
            return NULL;
        *(size_t *)new_mem = total_size;

        void *old_mem = (char *)ptr - sizeof(size_t);
        size_t old_total_size = *(size_t *)old_mem;
        size_t copy_size = (old_total_size - sizeof(size_t) < size) ? (old_total_size - sizeof(size_t)) : size;
        memcpy((char *)new_mem + sizeof(size_t), ptr, copy_size);
        return (char *)new_mem + sizeof(size_t);
    }
    return realloc(ptr, size);
}

static inline void rax_free(void *ptr)
{
    if (g_rax_current_arena) {
        return;
    }
    free(ptr);
}

static inline void *rax_malloc_usable(size_t size, size_t *usable)
{
    if (g_rax_current_arena) {
        void *ptr = rax_malloc(size);
        if (ptr && usable)
            *usable = size;
        return ptr;
    }
    void *ptr = malloc(size);
    if (ptr && usable)
        *usable = malloc_usable_size(ptr);
    return ptr;
}

static inline void *rax_realloc_usable(void *ptr, size_t size, size_t *usable, size_t *old_usable)
{
    if (g_rax_current_arena) {
        if (ptr && old_usable) {
            void *old_mem = (char *)ptr - sizeof(size_t);
            *old_usable = *(size_t *)old_mem - sizeof(size_t);
        }
        void *new_ptr = rax_realloc(ptr, size);
        if (new_ptr && usable)
            *usable = size;
        return new_ptr;
    }
    if (old_usable && ptr)
        *old_usable = malloc_usable_size(ptr);
    void *new_ptr = realloc(ptr, size);
    if (new_ptr && usable)
        *usable = malloc_usable_size(new_ptr);
    return new_ptr;
}

static inline void rax_free_usable(void *ptr, size_t *usable)
{
    if (g_rax_current_arena) {
        if (ptr && usable) {
            void *old_mem = (char *)ptr - sizeof(size_t);
            *usable = *(size_t *)old_mem - sizeof(size_t);
        }
        rax_free(ptr);
        return;
    }
    if (usable && ptr)
        *usable = malloc_usable_size(ptr);
    free(ptr);
}

#define rax_malloc_usable_size malloc_usable_size

#endif
