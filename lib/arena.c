#include <pthread.h>
#include <stdlib.h>
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
    size_t live_items;
    size_t db_refs;
    pthread_mutex_t mutex;
} s_slab_pool;

static s_slab_pool fact_pool = {0, 0, NULL, NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER};

static void slab_pool_destroy_locked(s_slab_pool *pool)
{
    s_slab_page *page = pool->pages;
    while (page) {
        s_slab_page *next = page->next;
        free(page);
        page = next;
    }
    pool->pages = NULL;
    pool->free_list = NULL;
    pool->items_per_page = 0;
}

static int slab_pool_refill_locked(s_slab_pool *pool, size_t item_size)
{
    if (pool->items_per_page == 0) {
        pool->item_size = item_size < sizeof(void *) ? sizeof(void *) : item_size;
        pool->items_per_page = 1024;
    }
    size_t page_size = sizeof(s_slab_page) + pool->items_per_page * pool->item_size;
    s_slab_page *page = malloc(page_size);
    if (!page)
        return -1;
    page->next = pool->pages;
    pool->pages = page;

    char *ptr = page->data;
    for (size_t i = 0; i < pool->items_per_page - 1; i++) {
        *(void **)ptr = ptr + pool->item_size;
        ptr += pool->item_size;
    }
    *(void **)ptr = pool->free_list;
    pool->free_list = page->data;
    return 0;
}

void arena_init(void)
{
    pthread_mutex_lock(&fact_pool.mutex);
    fact_pool.db_refs++;
    pthread_mutex_unlock(&fact_pool.mutex);
}

void arena_destroy(void)
{
    pthread_mutex_lock(&fact_pool.mutex);
    if (fact_pool.db_refs > 0)
        fact_pool.db_refs--;
    if (fact_pool.db_refs == 0 && fact_pool.live_items == 0)
        slab_pool_destroy_locked(&fact_pool);
    pthread_mutex_unlock(&fact_pool.mutex);
}

void *arena_alloc_fact(void)
{
    pthread_mutex_lock(&fact_pool.mutex);
    if (!fact_pool.free_list && slab_pool_refill_locked(&fact_pool, sizeof(s_fact)) != 0) {
        pthread_mutex_unlock(&fact_pool.mutex);
        return NULL;
    }
    void *item = fact_pool.free_list;
    fact_pool.free_list = *(void **)item;
    fact_pool.live_items++;
    pthread_mutex_unlock(&fact_pool.mutex);
    return item;
}

void arena_free_fact(void *ptr)
{
    if (!ptr)
        return;
    pthread_mutex_lock(&fact_pool.mutex);
    *(void **)ptr = fact_pool.free_list;
    fact_pool.free_list = ptr;
    if (fact_pool.live_items > 0)
        fact_pool.live_items--;
    if (fact_pool.db_refs == 0 && fact_pool.live_items == 0)
        slab_pool_destroy_locked(&fact_pool);
    pthread_mutex_unlock(&fact_pool.mutex);
}

size_t arena_alloc_fact_batch(void **out_head, size_t max_count)
{
    *out_head = NULL;
    if (max_count == 0)
        return 0;
    pthread_mutex_lock(&fact_pool.mutex);
    if (!fact_pool.free_list && slab_pool_refill_locked(&fact_pool, sizeof(s_fact)) != 0) {
        pthread_mutex_unlock(&fact_pool.mutex);
        return 0;
    }
    void *head = fact_pool.free_list;
    void *tail = head;
    size_t got = 1;
    while (got < max_count && *(void **)tail) {
        tail = *(void **)tail;
        got++;
    }
    fact_pool.free_list = *(void **)tail;
    *(void **)tail = NULL;
    fact_pool.live_items += got;
    pthread_mutex_unlock(&fact_pool.mutex);
    *out_head = head;
    return got;
}

void arena_free_fact_list(void *head, size_t count)
{
    if (!head || count == 0)
        return;
    void *tail = head;
    size_t actual = 1;
    while (*(void **)tail) {
        tail = *(void **)tail;
        actual++;
    }
    pthread_mutex_lock(&fact_pool.mutex);
    *(void **)tail = fact_pool.free_list;
    fact_pool.free_list = head;
    if (fact_pool.live_items >= actual)
        fact_pool.live_items -= actual;
    else
        fact_pool.live_items = 0;
    if (fact_pool.db_refs == 0 && fact_pool.live_items == 0)
        slab_pool_destroy_locked(&fact_pool);
    pthread_mutex_unlock(&fact_pool.mutex);
}
