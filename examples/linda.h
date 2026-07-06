#ifndef LINDA_H
#define LINDA_H

#include "facts.h"
#include <pthread.h>

/*
 * Linda Coordination Tuplespace wrapper.
 * Integrates with our facts database and provides thread-safe concurrent coordination.
 */
#define LINDA_COND_PARTITIONS 64

typedef struct {
    s_facts *db;
    s_intern *sym;
    pthread_mutex_t lock;
    pthread_cond_t conds[LINDA_COND_PARTITIONS];
} s_linda_space;

/* Allocate and initialize a new Linda Tuplespace */
s_linda_space *new_linda_space(unsigned long max_symbols);

/* Free the Linda Tuplespace and its internal database */
void delete_linda_space(s_linda_space *space);

/* Linda out(S, P, O) - Non-blocking insertion of a tuple */
int linda_out(s_linda_space *space, const char *s, const char *p, const char *o);

/* Linda rd(S, P, O, out_s, out_p, out_o) - Blocking read of a matching tuple */
int linda_rd(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o);

/* Linda in(S, P, O, out_s, out_p, out_o) - Blocking read and consume of a matching tuple */
int linda_in(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o);

/* Linda rdp(S, P, O, out_s, out_p, out_o) - Non-blocking read (returns 1 if found, 0 if not) */
int linda_rdp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o);

/* Linda inp(S, P, O, out_s, out_p, out_o) - Non-blocking consume (returns 1 if found, 0 if not) */
int linda_inp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o);

/* Linda eval() worker function type */
typedef void *(*f_linda_worker)(void *arg);

/* Linda eval(worker, arg) - Concurrent evaluation of a worker function */
int linda_eval(s_linda_space *space, f_linda_worker func, void *arg);

#endif
