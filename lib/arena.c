#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "arena.h"
#include "fact.h"

typedef struct slab_page {
    struct slab_page *next;
    char data[];
} s_slab_page;

typedef struct slab_pool {
    size_t item_size;
    size_t items_per_page;
    s_slab_page *pages;
    void *free_list;
    pthread_mutex_t mutex;
} s_slab_pool;

static s_slab_pool fact_pool = {0, 0, NULL, NULL, PTHREAD_MUTEX_INITIALIZER};

static int g_arena_ref_count = 0;
static pthread_mutex_t g_arena_ref_mutex = PTHREAD_MUTEX_INITIALIZER;

static void slab_pool_destroy(s_slab_pool *pool)
{
    pthread_mutex_lock(&pool->mutex);
    s_slab_page *page = pool->pages;
    while (page) {
        s_slab_page *next = page->next;
        free(page);
        page = next;
    }
    pool->pages = NULL;
    pool->free_list = NULL;
    pool->items_per_page = 0;
    pthread_mutex_unlock(&pool->mutex);
}

static void *slab_pool_alloc(s_slab_pool *pool, size_t item_size)
{
    pthread_mutex_lock(&pool->mutex);
    if (pool->items_per_page == 0) {
        pool->item_size = item_size < sizeof(void *) ? sizeof(void *) : item_size;
        pool->items_per_page = 1024;
    }
    if (!pool->free_list) {
        size_t page_size = sizeof(s_slab_page) + pool->items_per_page * pool->item_size;
        s_slab_page *page = malloc(page_size);
        if (!page) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        page->next = pool->pages;
        pool->pages = page;

        char *ptr = page->data;
        for (size_t i = 0; i < pool->items_per_page - 1; i++) {
            *(void **)ptr = ptr + pool->item_size;
            ptr += pool->item_size;
        }
        *(void **)ptr = NULL;
        pool->free_list = page->data;
    }
    void *item = pool->free_list;
    pool->free_list = *(void **)item;
    pthread_mutex_unlock(&pool->mutex);
    return item;
}

static void slab_pool_free(s_slab_pool *pool, void *item)
{
    if (!item)
        return;
    pthread_mutex_lock(&pool->mutex);
    *(void **)item = pool->free_list;
    pool->free_list = item;
    pthread_mutex_unlock(&pool->mutex);
}

void arena_init(void)
{
    pthread_mutex_lock(&g_arena_ref_mutex);
    g_arena_ref_count++;
    pthread_mutex_unlock(&g_arena_ref_mutex);
}

void arena_destroy(void)
{
    pthread_mutex_lock(&g_arena_ref_mutex);
    g_arena_ref_count--;
    if (g_arena_ref_count == 0) {
        slab_pool_destroy(&fact_pool);
    }
    pthread_mutex_unlock(&g_arena_ref_mutex);
}

void *arena_alloc_fact(void)
{
    return slab_pool_alloc(&fact_pool, sizeof(s_fact));
}

void arena_free_fact(void *ptr)
{
    slab_pool_free(&fact_pool, ptr);
}

_Thread_local s_rax_arena *g_rax_current_arena = NULL;

static s_rax_arena *rax_arena_new_chunk(size_t chunk_size)
{
    s_rax_arena *arena = malloc(sizeof(s_rax_arena));
    if (!arena)
        return NULL;
    arena->buf = malloc(chunk_size);
    if (!arena->buf) {
        free(arena);
        return NULL;
    }
    arena->offset = 0;
    arena->capacity = chunk_size;
    arena->next = NULL;
    arena->chunk_size = chunk_size;
    return arena;
}

s_rax_arena *rax_arena_create(size_t chunk_size)
{
    return rax_arena_new_chunk(chunk_size);
}

void rax_arena_destroy(s_rax_arena *arena)
{
    s_rax_arena *curr = arena;
    while (curr) {
        s_rax_arena *next = curr->next;
        free(curr->buf);
        free(curr);
        curr = next;
    }
}

void *rax_arena_alloc(s_rax_arena *arena, size_t size)
{
    size_t aligned_size = (size + 7) & ~7;
    s_rax_arena *curr = arena;
    while (curr) {
        if (curr->capacity - curr->offset >= aligned_size) {
            void *ptr = curr->buf + curr->offset;
            curr->offset += aligned_size;
            return ptr;
        }
        if (!curr->next) {
            size_t next_chunk_size = curr->chunk_size;
            if (aligned_size > next_chunk_size) {
                next_chunk_size = aligned_size;
            }
            curr->next = rax_arena_new_chunk(next_chunk_size);
            if (!curr->next)
                return NULL;
        }
        curr = curr->next;
    }
    return NULL;
}
