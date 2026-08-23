#ifndef LINDA_H
#define LINDA_H

#include "facts.h"

/*
 * Linda Coordination Tuplespace wrapper.
 * Integrates with our facts database and provides thread-safe concurrent coordination.
 */
#define LINDA_COND_PARTITIONS 64
#define LINDA_OK 0
#define LINDA_TIMEOUT 1
#define LINDA_CLOSED 2
#define LINDA_ERROR -1

typedef struct linda_pattern s_linda_pattern;
typedef struct linda_space s_linda_space;

/* Allocate and initialize a new Linda Tuplespace */
s_linda_space *new_linda_space(unsigned long max_symbols);

/*
 * Stop accepting new work and wake blocked rd/in calls. Closing is idempotent;
 * awakened and subsequent operations return LINDA_CLOSED. The space remains
 * valid until delete_linda_space() is called.
 */
int linda_space_close(s_linda_space *space);
int linda_space_is_closed(s_linda_space *space);

/* Close, wait for in-flight operations/workers, then free the tuplespace. */
void delete_linda_space(s_linda_space *space);

/*
 * Advanced interoperability escape hatch. The returned database is borrowed
 * and remains owned by space. Prefer Linda operations for coordination tuples.
 */
s_facts *linda_space_facts(s_linda_space *space);

/* Attach or detach a managed reactive program on the shared fact runtime. */
int linda_space_attach_program(s_linda_space *space, const s_datalog_program *program);
int linda_space_detach_program(s_linda_space *space);

/* Compile and own a reusable tuple pattern. Safe to use concurrently. */
s_linda_pattern *new_linda_pattern(const char *s, const char *p, const char *o);
void delete_linda_pattern(s_linda_pattern *pattern);

/*
 * Linda out(S, P, O) - Non-blocking insertion of one tuple occurrence.
 * Equal tuples are retained as distinct consumable occurrences. Facts carrying
 * derived support only are intentionally outside the Linda coordination view.
 */
int linda_out(s_linda_space *space, const char *s, const char *p, const char *o);

/* Linda rd(S, P, O, out_s, out_p, out_o) - Blocking read of a matching tuple */
int linda_rd(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o);

/* Linda in(S, P, O, out_s, out_p, out_o) - Blocking read and consume of a matching tuple */
int linda_in(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o);

/* Timed blocking operations. Return LINDA_TIMEOUT when timeout_ms elapses. */
int linda_rd_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
                   size_t max_p, char *out_o, size_t max_o, long timeout_ms);
int linda_in_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
                   size_t max_p, char *out_o, size_t max_o, long timeout_ms);

/* Linda rdp(S, P, O, out_s, out_p, out_o) - Non-blocking read (returns 1 if found, 0 if not) */
int linda_rdp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o);

/* Linda inp(S, P, O, out_s, out_p, out_o) - Non-blocking consume (returns 1 if found, 0 if not) */
int linda_inp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o);

int linda_rd_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                     char *out_o, size_t max_o);
int linda_in_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                     char *out_o, size_t max_o);
int linda_rdp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                      char *out_o, size_t max_o);
int linda_inp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                      char *out_o, size_t max_o);
int linda_rd_pattern_timed(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                           size_t max_p, char *out_o, size_t max_o, long timeout_ms);
int linda_in_pattern_timed(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                           size_t max_p, char *out_o, size_t max_o, long timeout_ms);

/* Linda eval() worker function type */
typedef void *(*f_linda_worker)(void *arg);

/* Linda eval(worker, arg) - Concurrent evaluation of a worker function */
int linda_eval(s_linda_space *space, f_linda_worker func, void *arg);

#endif
