#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "intern.h"
#include "rax.h"

#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

static s_intern_page *new_page(size_t min_capacity)
{
    size_t page_capacity = 65536 - sizeof(s_intern_page);
    if (min_capacity > page_capacity) {
        page_capacity = min_capacity;
    }
    s_intern_page *page = malloc(sizeof(s_intern_page) + page_capacity);
    assert(page);
    page->next = NULL;
    page->size = 0;
    page->capacity = page_capacity;
    return page;
}

static void *intern_alloc(s_intern *intern, size_t size)
{
    size = ALIGN_UP(size, 8);
    s_intern_page *page = intern->pages;
    if (!page || page->size + size > page->capacity) {
        page = new_page(size);
        page->next = intern->pages;
        intern->pages = page;
    }
    void *ptr = page->data + page->size;
    page->size += size;
    return ptr;
}

void intern_init(s_intern *intern, unsigned long max)
{
    (void)max;
    assert(intern);
    intern->symbols = raxNew();
    assert(intern->symbols);
    intern->symbols_delete = 1;
    intern->next_id = 1;
    intern->pages = NULL;
}

void intern_destroy(s_intern *intern)
{
    assert(intern);
    if (intern->symbols_delete && intern->symbols) {
        raxIterator it;
        raxStart(&it, intern->symbols);
        raxSeek(&it, "^", NULL, 0);
        while (raxNext(&it)) {
            s_set_item *item = it.data;
            free(item);
        }
        raxStop(&it);
        raxFree(intern->symbols);
    }
    s_intern_page *page = intern->pages;
    while (page) {
        s_intern_page *next = page->next;
        free(page);
        page = next;
    }
    intern->pages = NULL;
}

s_intern *new_intern(unsigned long max)
{
    s_intern *intern = malloc(sizeof(s_intern));
    if (intern) {
        intern_init(intern, max);
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
    void *item = NULL;
    int found = raxFind(intern->symbols, (unsigned char *)string, strlen(string), &item);
    if (!found)
        return NULL;
    return (s_set_item *)item;
}

Symbol intern_find_symbol_str(s_intern *intern, const char *string)
{
    s_set_item *i = intern_find_symbol(intern, string);
    if (i)
        return (Symbol)i->data;
    return NULL;
}

Symbol intern_string_view(s_intern *intern, const char *string, size_t len)
{
    assert(intern);
    assert(string);
    void *res = NULL;
    int found = raxFind(intern->symbols, (unsigned char *)string, len, &res);
    s_set_item *i;
    if (!found) {
        s_symbol *sym = intern_alloc(intern, sizeof(s_symbol) + len + 1);
        assert(sym);
        sym->id = intern->next_id++;
        memcpy(sym->data, string, len);
        sym->data[len] = '\0';

        i = malloc(sizeof(s_set_item));
        assert(i);
        i->data = (void *)sym;
        i->len = len;
        i->hash = 0;
        i->usage = 0;
        i->long_p = 0;
        i->long_value = 0;
        i->double_p = 0;
        i->double_value = 0.0;

        int ret = raxInsert(intern->symbols, (unsigned char *)string, len, i, NULL);
        (void)ret;
        assert(ret == 1);
    } else {
        i = (s_set_item *)res;
    }
    assert(i);
    i->usage++;
    return (Symbol)i->data;
}

Symbol intern_string(s_intern *intern, const char *string)
{
    assert(intern);
    assert(string);
    return intern_string_view(intern, string, strlen(string));
}

void intern_unstring(s_intern *intern, Symbol sym)
{
    assert(intern);
    assert(sym);
    s_set_item *i = intern_find_symbol(intern, sym->data);
    if (i) {
        i->usage--;
        if (!i->usage) {
            void *old;
            int ret = raxRemove(intern->symbols, (unsigned char *)sym->data, strlen(sym->data), &old);
            (void)ret;
            assert(ret == 1);
            assert(old == i);
            free(i);
        }
    }
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
    s_set_item *i;
    assert(intern);
    assert(string);
    i = intern_find_symbol(intern, string);
    if (i) {
        if (!i->long_p) {
            i->long_value = strtol(string, NULL, 10);
            i->long_p = 1;
        }
        return i->long_value;
    }
    return 0;
}

double intern_get_double(s_intern *intern, const char *string)
{
    s_set_item *i;
    assert(intern);
    assert(string);
    i = intern_find_symbol(intern, string);
    if (i) {
        if (!i->double_p) {
            i->double_value = strtod(string, NULL);
            i->double_p = 1;
        }
        return i->double_value;
    }
    return 0.0;
}
