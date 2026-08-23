#ifndef FACTS_H
#define FACTS_H

#include <stdio.h>
#include "binding.h"
#include "fact.h"
#include "intern.h"
#include "rule.h"
#include "set.h"
#include "rax.h"
#include "spec.h"

#define FACTS_SKIPLIST_SPACING 2.7
#define FACTS_LOAD_BUFSZ (1024 * 1024)

#include <pthread.h>
#include "transaction.h"
#include "hexastore.h"

typedef struct facts {
    s_intern *symbols;
    size_t symbols_delete;
    s_set index;
    struct rax *index_spo;
    struct rax *index_pos;
    struct rax *index_osp;
    s_hexastore *hexastore;
    FILE *log;

    // Decoupled Transactions
    s_transaction tx;

    // Reactive Rete-like Rule Evaluation
    s_datalog_program *prog;
    int owns_prog;
    int disable_listener;
} s_facts;

typedef enum { FACT_ORIGIN_ASSERTED, FACT_ORIGIN_DERIVED } e_fact_origin;

typedef struct fact_support {
    size_t asserted;
    size_t derived;
} s_fact_support;

/* An owning, mutation-independent copy of a fact. */
typedef struct fact_snapshot {
    char *s;
    char *p;
    char *o;
    s_fact_support support;
} s_fact_snapshot;

/* Keeps borrowed pointers returned by legacy lookup APIs valid until end. */
typedef struct facts_read_guard {
    s_facts *facts;
    int acquired;
} s_facts_read_guard;

void facts_init(s_facts *facts, s_intern *symbols, unsigned long max);

void facts_destroy(s_facts *facts);
void facts_reset(s_facts *facts);

s_facts *new_facts(s_intern *symbols, unsigned long max);

void delete_facts(s_facts *facts);

int facts_transaction_begin(s_facts *facts);
int facts_transaction_commit(s_facts *facts);
int facts_transaction_rollback(s_facts *facts);

int facts_read_begin(s_facts *facts, s_facts_read_guard *guard);
void facts_read_end(s_facts_read_guard *guard);

int facts_contains_symbol(s_facts *facts, const char *string);

/* Borrowed pointers require an active facts_read_guard across their use. */
s_set_item *facts_find_symbol(s_facts *facts, const char *string);

/* Borrowed Symbol; keep a read guard active while retaining it. */
Symbol facts_find_symbol_str(s_facts *facts, const char *string);

const char *facts_long(s_facts *facts, long l);

const char *facts_double(s_facts *facts, double d);

long facts_get_long(s_facts *facts, const char *string);

double facts_get_double(s_facts *facts, const char *string);

const char *facts_anon(s_facts *facts, const char *name);

Symbol facts_intern(s_facts *facts, const char *string);
void facts_unintern(s_facts *facts, Symbol sym);

s_fact *facts_add_fact(s_facts *facts, s_fact *f);

s_fact *facts_add_fact_origin(s_facts *facts, s_fact *f, e_fact_origin origin);

s_fact *facts_add_spo(s_facts *facts, const char *s, const char *p, const char *o);

s_fact *facts_add_spo_origin(s_facts *facts, const char *s, const char *p, const char *o, e_fact_origin origin);

int facts_add(s_facts *facts, p_spec spec);

int facts_remove_fact(s_facts *facts, s_fact *f);

int facts_remove_fact_origin(s_facts *facts, s_fact *f, e_fact_origin origin);

int facts_remove_spo(s_facts *facts, const char *s, const char *p, const char *o);

int facts_remove_spo_origin(s_facts *facts, const char *s, const char *p, const char *o, e_fact_origin origin);

/* Returns 1 when the fact exists, 0 when it does not, and -1 for invalid arguments. */
int facts_get_support_spo(s_facts *facts, const char *s, const char *p, const char *o, s_fact_support *support);

/* Safe lookup APIs: no internal database pointer escapes the read section. */
int facts_contains_spo(s_facts *facts, const char *s, const char *p, const char *o);
/* Initialize or destroy snapshot before filling it; destroy successful results. */
int facts_get_spo_snapshot(s_facts *facts, const char *s, const char *p, const char *o, s_fact_snapshot *snapshot);
void facts_snapshot_destroy(s_fact_snapshot *snapshot);

int facts_remove(s_facts *facts, p_spec spec);

/* Borrowed result; keep a read guard active while retaining it. */
s_fact *facts_get_fact(s_facts *facts, s_fact *f);

/* Borrowed result; prefer facts_contains_spo or facts_get_spo_snapshot. */
s_fact *facts_get_spo(s_facts *facts, const char *s, const char *p, const char *o);

unsigned long facts_count(s_facts *facts);

typedef struct facts_cursor {
    raxIterator it;
    s_facts *facts;
    unsigned char end_key[24];
    int started;
    int locked;
    int index_type;
    const char **var_s;
    const char **var_p;
    const char **var_o;
} s_facts_cursor;

void facts_cursor_init(s_facts *facts, s_facts_cursor *c, struct rax *tree, s_fact *start, s_fact *end);

s_fact *facts_cursor_next(s_facts_cursor *c);

void facts_cursor_stop(s_facts_cursor *c);

void facts_with_3(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o);

void facts_with_0(s_facts *facts, s_facts_cursor *c, const char **var_s, const char **var_p, const char **var_o);

void facts_with_1_2(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o, const char **var_s,
                    const char **var_p, const char **var_o);

void facts_with_spo(s_facts *facts, s_binding *bindings, s_facts_cursor *c, const char *s, const char *p, const char *o);

typedef struct facts_with_cursor_level {
    s_facts_cursor c;
    s_fact *fact;
    p_spec spec;
    int spec_init;
} s_facts_with_cursor_level;

typedef struct facts_with_cursor {
    s_facts *facts;
    s_binding *bindings;
    size_t facts_count;
    s_facts_with_cursor_level *l;
    size_t level;
    p_spec spec;
    int locked;
    long limit;
    long offset;
    long result_count;
    int is_sorted;
    char *sort_var;
    int sort_desc;
    void *sorted_matches;
    size_t sorted_count;
    size_t sorted_pos;
} s_facts_with_cursor;

void facts_with(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, p_spec spec);
void facts_spec_sort(s_facts *facts, p_spec spec, size_t count);

void facts_with_cursor_destroy(s_facts_with_cursor *c);

int facts_with_cursor_next(s_facts_with_cursor *c);

/* Borrowed result; prefer facts_get_prop_copy outside a read guard. */
const char *facts_get_prop(s_facts *facts, const char *s, const char *p);

/* Allocates *value on success. The caller owns it and must free it. */
int facts_get_prop_copy(s_facts *facts, const char *s, const char *p, char **value);

long facts_get_prop_long(s_facts *facts, const char *s, const char *p);

double facts_get_prop_double(s_facts *facts, const char *s, const char *p);

s_fact *facts_set_prop(s_facts *facts, const char *s, const char *p, const char *o);

void facts_register_tx_listener(s_facts *facts, f_facts_tx_listener listener, void *user_data);

/* Called after a changed outer transaction is committed and its write lock is released. */
void facts_register_commit_observer(s_facts *facts, f_facts_commit_observer observer, void *user_data);

/* Detailed post-commit notification for targeted subject-based wakeups. */
unsigned int facts_commit_subject_partition(const char *subject);
void facts_register_commit_summary_observer(s_facts *facts, f_facts_commit_summary_observer observer, void *user_data);

/*
 * Atomically replace the reactive program with a private deep copy and
 * recompute derived support. Passing NULL detaches the current program.
 */
int facts_attach_program(s_facts *facts, const s_datalog_program *prog);
int facts_detach_program(s_facts *facts);

/* Internal transaction rollback hook. */
void facts_apply_rollback_entry(s_facts *facts, const s_rollback_entry *entry);

typedef struct entity {
    s_facts *facts;
    const char *subject;
    int in_transaction;
} s_entity;

s_entity *facts_entity_begin(s_facts *facts, const char *subject);
int facts_entity_add(s_entity *ent, const char *predicate, const char *object);
int facts_entity_add_long(s_entity *ent, const char *predicate, long value);
int facts_entity_add_double(s_entity *ent, const char *predicate, double value);
int facts_entity_commit(s_entity *ent);
void facts_entity_abort(s_entity *ent);

#endif
