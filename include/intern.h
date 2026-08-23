#ifndef INTERN_H
#define INTERN_H

#include <stddef.h>
#include "set.h"

#include <stdint.h>

typedef struct symbol {
    uint64_t id;
    char data[];
} s_symbol;

typedef const s_symbol *Symbol;

#ifndef P_FIRST
#define P_FIRST ((Symbol)0)
#endif
#ifndef P_LAST
#define P_LAST ((Symbol) - 1)
#endif

static inline int compare_symbol(Symbol a, Symbol b)
{
    if (a == b)
        return 0;
    if (a == P_FIRST || b == P_LAST)
        return -1;
    if (a == P_LAST || b == P_FIRST)
        return 1;
    return (a->id < b->id) ? -1 : 1;
}

static inline const char *symbol_to_str(Symbol s)
{
    if (s == P_FIRST || s == P_LAST)
        return NULL;
    return s->data;
}

typedef struct intern_page {
    struct intern_page *next;
    size_t size;
    size_t capacity;
    char data[];
} s_intern_page;

struct rax;

typedef struct intern_table {
    struct rax *symbols;
    int symbols_delete;
    uint64_t next_id;
    s_intern_page *pages;
} s_intern;

void intern_init(s_intern *intern, unsigned long max);
int intern_init_checked(s_intern *intern, unsigned long max);
void intern_destroy(s_intern *intern);

s_intern *new_intern(unsigned long max);
void delete_intern(s_intern *intern);

s_set_item *intern_find_symbol(s_intern *intern, const char *string);
Symbol intern_find_symbol_str(s_intern *intern, const char *string);

Symbol intern_long(s_intern *intern, long l);
Symbol intern_double(s_intern *intern, double d);

long intern_get_long(s_intern *intern, const char *string);
double intern_get_double(s_intern *intern, const char *string);

Symbol intern_string(s_intern *intern, const char *string);
Symbol intern_string_view(s_intern *intern, const char *string, size_t len);
void intern_unstring(s_intern *intern, Symbol sym);

#endif
