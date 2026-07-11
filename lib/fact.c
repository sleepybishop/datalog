#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "fact.h"
#include "intern.h"
#include "arena.h"

void fact_init(s_fact *f, Symbol s, Symbol p, Symbol o)
{
    f->s = s;
    f->p = p;
    f->o = o;
    f->negated = NULL;
    f->proof_count = 1;
}

s_fact *new_fact(Symbol s, Symbol p, Symbol o)
{
    s_fact *fact = arena_alloc_fact();
    if (fact) {
        fact->s = s;
        fact->p = p;
        fact->o = o;
        fact->negated = NULL;
        fact->proof_count = 1;
    }
    return fact;
}

void delete_fact(s_fact *f)
{
    arena_free_fact(f);
}

int fact_compare_spo(void *a, void *b)
{
    int cmp;
    s_fact *fa;
    s_fact *fb;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    fa = (s_fact *)a;
    fb = (s_fact *)b;
    cmp = compare_symbol(fa->s, fb->s);
    if (!cmp) {
        cmp = compare_symbol(fa->p, fb->p);
        if (!cmp) {
            cmp = compare_symbol(fa->o, fb->o);
        }
    }
    return cmp;
}

int fact_compare_pos(void *a, void *b)
{
    int cmp;
    s_fact *fa;
    s_fact *fb;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    fa = (s_fact *)a;
    fb = (s_fact *)b;
    cmp = compare_symbol(fa->p, fb->p);
    if (!cmp) {
        cmp = compare_symbol(fa->o, fb->o);
        if (!cmp) {
            cmp = compare_symbol(fa->s, fb->s);
        }
    }
    return cmp;
}

int fact_compare_osp(void *a, void *b)
{
    int cmp;
    s_fact *fa;
    s_fact *fb;
    if (a == b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    fa = (s_fact *)a;
    fb = (s_fact *)b;
    cmp = compare_symbol(fa->o, fb->o);
    if (!cmp) {
        cmp = compare_symbol(fa->s, fb->s);
        if (!cmp) {
            cmp = compare_symbol(fa->p, fb->p);
        }
    }
    return cmp;
}

s_fact_list *new_fact_list(s_fact *fact, s_fact_list *next)
{
    s_fact_list *fl = malloc(sizeof(s_fact_list));
    if (fl) {
        fl->fact = fact;
        fl->next = next;
    }
    return fl;
}

void delete_fact_list(s_fact_list *fl)
{
    while (fl) {
        s_fact_list *head = fl;
        fl = fl->next;
        free(head);
    }
}

s_fact_list *fact_list_find(s_fact_list *fl, s_fact *f)
{
    while (fl && fl->fact != f)
        fl = fl->next;
    return fl;
}

s_fact_list *fact_list_intern(s_fact_list *fl, s_fact *f)
{
    if (fact_list_find(fl, f))
        return fl;
    return new_fact_list(f, fl);
}
