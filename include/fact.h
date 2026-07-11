#ifndef FACT_H
#define FACT_H

#include "binding.h"
#include "intern.h"

#ifndef P_FIRST
#define P_FIRST ((Symbol)0)
#endif
#ifndef P_LAST
#define P_LAST ((Symbol) - 1)
#endif

typedef struct fact {
    Symbol s;
    Symbol p;
    Symbol o;
    Symbol negated;
    size_t proof_count;
} s_fact;

typedef int (*f_fact)(s_fact *f);

void fact_init(s_fact *f, Symbol s, Symbol p, Symbol o);

s_fact *new_fact(Symbol s, Symbol p, Symbol o);

void delete_fact(s_fact *f);

int fact_compare_spo(void *a, void *b);

int fact_compare_pos(void *a, void *b);

int fact_compare_osp(void *a, void *b);

typedef struct fact_list s_fact_list;

struct fact_list {
    s_fact *fact;
    s_fact_list *next;
};

s_fact_list *new_fact_list(s_fact *fact, s_fact_list *next);

void delete_fact_list(s_fact_list *fl);

s_fact_list *fact_list_find(s_fact_list *fl, s_fact *f);

s_fact_list *fact_list_intern(s_fact_list *fl, s_fact *f);

#endif
