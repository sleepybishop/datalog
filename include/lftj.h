#ifndef LFTJ_H
#define LFTJ_H

#include <stddef.h>
#include "spec.h"
#include "rax.h"

#include "intern.h"

struct facts;
struct fact;

// Iterator
typedef struct lftj_iterator {
    struct rax *symbols;
    raxIterator it;
    int depth; // 0, 1, 2, 3
    int col1, col2, col3;
    Symbol val1;
    Symbol val2;
    Symbol val3;
} s_lftj_iterator;

s_lftj_iterator *new_lftj_iterator(struct rax *symbols, int col1, int col2, int col3);
void delete_lftj_iterator(s_lftj_iterator *it);

Symbol iterator_key(s_lftj_iterator *it);
int iterator_next(s_lftj_iterator *it);
int iterator_seek(s_lftj_iterator *it, Symbol key);
int iterator_open(s_lftj_iterator *it);
int iterator_up(s_lftj_iterator *it);

// Leapfrog Triejoin Solver
int facts_lftj_solve(struct facts *facts, p_spec spec, s_binding *bindings);
int facts_lftj_solve_multi(struct facts *facts, struct facts **dbs, p_spec spec, s_binding *bindings,
                           void (*cb)(s_binding *bindings, void *user_data), void *user_data);

#endif
