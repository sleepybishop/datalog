#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "intern.h"
#include "rax.h"

#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

static s_intern_page *new_page(size_t min_capacity)
{
    size_t page_capacity = 65536 - sizeof(s_intern_page);
    if (min_capacity > page_capacity)
        page_capacity = min_capacity;
    if (page_capacity > SIZE_MAX - sizeof(s_intern_page))
        return NULL;
    s_intern_page *page = malloc(sizeof(s_intern_page) + page_capacity);
    if (!page)
        return NULL;
    page->next = NULL;
    page->size = 0;
    page->capacity = page_capacity;
    return page;
}

static void *intern_alloc(s_intern *intern, size_t size, s_intern_page **out_page, size_t *out_prev_size)
{
    if (size > SIZE_MAX - 7)
        return NULL;
    size = ALIGN_UP(size, 8);
    s_intern_page *page = intern->pages;
    if (!page || size > page->capacity - page->size) {
        page = new_page(size);
        if (!page)
            return NULL;
        page->next = intern->pages;
        intern->pages = page;
    }
    *out_page = page;
    *out_prev_size = page->size;
    void *ptr = page->data + page->size;
    page->size += size;
    return ptr;
}

void intern_init(s_intern *intern, unsigned long max)
{
    if (intern_init_checked(intern, max) != 0)
        abort();
}

int intern_init_checked(s_intern *intern, unsigned long max)
{
    (void)max;
    if (!intern)
        return -1;
    memset(intern, 0, sizeof(*intern));
    rax *symbols = raxNew();
    if (!symbols)
        return -1;
    if (pthread_mutex_init(&intern->mutex, NULL) != 0) {
        raxFree(symbols);
        return -1;
    }
    intern->symbols = symbols;
    intern->symbols_delete = 1;
    intern->next_id = 1;
    intern->pages = NULL;
    return 0;
}

void intern_destroy(s_intern *intern)
{
    assert(intern);
    if (intern->symbols_delete && intern->symbols) {
        raxFree(intern->symbols);
        intern->symbols = NULL;
    }
    s_intern_page *page = intern->pages;
    while (page) {
        s_intern_page *next = page->next;
        free(page);
        page = next;
    }
    intern->pages = NULL;
    pthread_mutex_destroy(&intern->mutex);
}

s_intern *new_intern(unsigned long max)
{
    s_intern *intern = malloc(sizeof(s_intern));
    if (intern && intern_init_checked(intern, max) != 0) {
        free(intern);
        intern = NULL;
    }
    return intern;
}

void delete_intern(s_intern *intern)
{
    if (intern) {
        intern_destroy(intern);
        free(intern);
    }
}

s_set_item *intern_find_symbol(s_intern *intern, const char *string)
{
    assert(intern);
    assert(string);
    pthread_mutex_lock(&intern->mutex);
    void *item = NULL;
    s_set_item *result = NULL;
    if (raxFind(intern->symbols, (unsigned char *)string, strlen(string), &item)) {
        s_set_item *si = (s_set_item *)item;
        if (si->usage > 0)
            result = si;
    }
    pthread_mutex_unlock(&intern->mutex);
    return result;
}

Symbol intern_find_symbol_str(s_intern *intern, const char *string)
{
    s_set_item *i = intern_find_symbol(intern, string);
    return i ? (Symbol)i->data : NULL;
}

static Symbol intern_string_view_locked(s_intern *intern, const char *string, size_t len)
{
    void *res = NULL;
    s_set_item *i = NULL;
    if (!raxFind(intern->symbols, (unsigned char *)string, len, &res)) {
        if (len > SIZE_MAX - sizeof(s_symbol) - 1 || intern->next_id == UINT64_MAX)
            return NULL;
        s_intern_page *page = NULL;
        size_t prev_size = 0;
        s_symbol *sym = intern_alloc(intern, sizeof(s_symbol) + len + 1, &page, &prev_size);
        if (!sym)
            return NULL;
        sym->id = intern->next_id++;
        sym->item.data = sym;
        sym->item.len = len;
        sym->item.hash = 0;
        atomic_init(&sym->item.usage, 0);
        memcpy(sym->data, string, len);
        sym->data[len] = '\0';

        if (raxInsert(intern->symbols, (unsigned char *)sym->data, len, &sym->item, NULL) != 1) {
            page->size = prev_size;
            intern->next_id--;
            return NULL;
        }
        i = &sym->item;
    } else {
        i = (s_set_item *)res;
    }
    if (!i || i->usage == ULONG_MAX)
        return NULL;
    i->usage++;
    return (Symbol)i->data;
}

Symbol intern_string_view(s_intern *intern, const char *string, size_t len)
{
    assert(intern);
    assert(string);
    pthread_mutex_lock(&intern->mutex);
    Symbol result = intern_string_view_locked(intern, string, len);
    pthread_mutex_unlock(&intern->mutex);
    return result;
}

Symbol intern_string(s_intern *intern, const char *string)
{
    assert(intern);
    assert(string);
    return intern_string_view(intern, string, strlen(string));
}

Symbol intern_ref(s_intern *intern, Symbol sym)
{
    if (!intern || !sym || sym == P_LAST)
        return NULL;
    pthread_mutex_lock(&intern->mutex);
    s_symbol *mut = (s_symbol *)sym;
    Symbol result = NULL;
    if (mut->item.usage != ULONG_MAX) {
        mut->item.usage++;
        result = sym;
    }
    pthread_mutex_unlock(&intern->mutex);
    return result;
}

void intern_unstring(s_intern *intern, Symbol sym)
{
    assert(intern);
    assert(sym);
    if (sym == P_FIRST || sym == P_LAST)
        return;
    pthread_mutex_lock(&intern->mutex);
    s_symbol *mut = (s_symbol *)sym;
    if (mut->item.usage > 0)
        mut->item.usage--;
    /* arena storage lives until destruction; retain key for reuse */
    pthread_mutex_unlock(&intern->mutex);
}

Symbol intern_long(s_intern *intern, long l)
{
    char buf[32];
    assert(intern);
    snprintf(buf, sizeof(buf), "%ld", l);
    return intern_string(intern, buf);
}

Symbol intern_double(s_intern *intern, double d)
{
    char buf[64];
    assert(intern);
    snprintf(buf, sizeof(buf), "%.17g", d);
    return intern_string(intern, buf);
}

long intern_get_long(s_intern *intern, const char *string)
{
    assert(intern);
    assert(string);
    return intern_find_symbol(intern, string) ? strtol(string, NULL, 10) : 0;
}

double intern_get_double(s_intern *intern, const char *string)
{
    assert(intern);
    assert(string);
    return intern_find_symbol(intern, string) ? strtod(string, NULL) : 0.0;
}
