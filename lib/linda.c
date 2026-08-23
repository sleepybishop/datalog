#include "linda.h"
#include "binding.h"
#include "facts_internal.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct linda_pattern {
    const char *terms[3];
    char *owned[3];
    const char *variables[3];
    int variable_index[3];
    size_t variable_count;
    unsigned int cond_index;
};

typedef struct linda_entry {
    Symbol s;
    Symbol p;
    Symbol o;
    size_t count;
} s_linda_entry;

struct linda_space {
    s_intern *sym;
    s_facts *db;
    /* Counts above the database's implicit single asserted occurrence. */
    s_set multiplicity;
    pthread_mutex_t locks[LINDA_COND_PARTITIONS];
    pthread_cond_t conds[LINDA_COND_PARTITIONS];
    pthread_mutex_t worker_lock;
    pthread_cond_t worker_cond;
    size_t active_workers;
    size_t active_operations;
    int shutting_down;
};

#define LINDA_ENTRY_KEY_SIZE (sizeof(Symbol) * 3)

_Static_assert(LINDA_COND_PARTITIONS == FACTS_COMMIT_PARTITIONS, "Linda and commit partitions must match");

static int linda_operation_begin(s_linda_space *space)
{
    if (!space)
        return LINDA_ERROR;
    pthread_mutex_lock(&space->worker_lock);
    if (space->shutting_down) {
        pthread_mutex_unlock(&space->worker_lock);
        return LINDA_CLOSED;
    }
    space->active_operations++;
    pthread_mutex_unlock(&space->worker_lock);
    return LINDA_OK;
}

static void linda_operation_end(s_linda_space *space)
{
    pthread_mutex_lock(&space->worker_lock);
    if (space->active_operations > 0)
        space->active_operations--;
    if (space->active_operations == 0)
        pthread_cond_broadcast(&space->worker_cond);
    pthread_mutex_unlock(&space->worker_lock);
}

static int linda_closed(s_linda_space *space)
{
    pthread_mutex_lock(&space->worker_lock);
    int closed = space->shutting_down;
    pthread_mutex_unlock(&space->worker_lock);
    return closed;
}

static unsigned int get_linda_cond_idx(const char *s)
{
    return facts_commit_subject_partition(s);
}

static s_set_item *linda_entry_item(s_linda_space *space, const s_fact *fact)
{
    s_linda_entry key = {.s = fact->s, .p = fact->p, .o = fact->o, .count = 0};
    return set_get(&space->multiplicity, &key, LINDA_ENTRY_KEY_SIZE);
}

static s_linda_entry *linda_entry_get(s_linda_space *space, const s_fact *fact)
{
    s_set_item *item = linda_entry_item(space, fact);
    return item ? (s_linda_entry *)item->data : NULL;
}

static s_linda_entry *linda_entry_add(s_linda_space *space, const s_fact *fact, size_t count)
{
    s_linda_entry *entry = malloc(sizeof(*entry));
    if (!entry)
        return NULL;
    entry->s = facts_intern(space->db, symbol_to_str(fact->s));
    entry->p = facts_intern(space->db, symbol_to_str(fact->p));
    entry->o = facts_intern(space->db, symbol_to_str(fact->o));
    entry->count = count;
    if (!entry->s || !entry->p || !entry->o) {
        if (entry->s)
            facts_unintern(space->db, entry->s);
        if (entry->p)
            facts_unintern(space->db, entry->p);
        if (entry->o)
            facts_unintern(space->db, entry->o);
        free(entry);
        return NULL;
    }
    if (!set_add(&space->multiplicity, entry, LINDA_ENTRY_KEY_SIZE)) {
        facts_unintern(space->db, entry->s);
        facts_unintern(space->db, entry->p);
        facts_unintern(space->db, entry->o);
        free(entry);
        return NULL;
    }
    return entry;
}

static void linda_entry_remove(s_linda_space *space, const s_fact *fact)
{
    s_set_item *item = linda_entry_item(space, fact);
    if (!item)
        return;
    s_linda_entry *entry = (s_linda_entry *)item->data;
    set_remove(&space->multiplicity, item);
    facts_unintern(space->db, entry->s);
    facts_unintern(space->db, entry->p);
    facts_unintern(space->db, entry->o);
    free(entry);
}

static void linda_signal_subject(s_linda_space *space, const char *subject)
{
    unsigned int partitions[2] = {0, get_linda_cond_idx(subject)};
    size_t count = partitions[1] == 0 ? 1 : 2;
    for (size_t i = 0; i < count; i++) {
        unsigned int partition = partitions[i];
        pthread_mutex_lock(&space->locks[partition]);
        pthread_cond_broadcast(&space->conds[partition]);
        pthread_mutex_unlock(&space->locks[partition]);
    }
}

static int linda_pattern_init(s_linda_pattern *pattern, const char *s, const char *p, const char *o, int copy_terms)
{
    if (!pattern || !s || !p || !o)
        return -1;
    memset(pattern, 0, sizeof(*pattern));
    const char *terms[3] = {s, p, o};
    for (size_t i = 0; i < 3; i++) {
        pattern->variable_index[i] = -1;
        if (copy_terms) {
            pattern->owned[i] = strdup(terms[i]);
            if (!pattern->owned[i]) {
                for (size_t j = 0; j < i; j++)
                    free(pattern->owned[j]);
                return -1;
            }
            pattern->terms[i] = pattern->owned[i];
        } else {
            pattern->terms[i] = terms[i];
        }
        if (terms[i][0] == '?') {
            size_t v = 0;
            while (v < pattern->variable_count && strcmp(pattern->variables[v], pattern->terms[i]) != 0)
                v++;
            if (v == pattern->variable_count)
                pattern->variables[pattern->variable_count++] = pattern->terms[i];
            pattern->variable_index[i] = (int)v;
        }
    }
    pattern->cond_index = get_linda_cond_idx(pattern->terms[0]);
    return 0;
}

s_linda_pattern *new_linda_pattern(const char *s, const char *p, const char *o)
{
    s_linda_pattern *pattern = malloc(sizeof(*pattern));
    if (!pattern)
        return NULL;
    if (linda_pattern_init(pattern, s, p, o, 1) != 0) {
        free(pattern);
        return NULL;
    }
    return pattern;
}

void delete_linda_pattern(s_linda_pattern *pattern)
{
    if (!pattern)
        return;
    for (size_t i = 0; i < 3; i++)
        free(pattern->owned[i]);
    free(pattern);
}

static void linda_commit_observer(s_facts *facts, const s_facts_commit_summary *summary, void *user_data)
{
    (void)facts;
    s_linda_space *space = (s_linda_space *)user_data;
    for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
        if (!(summary->subject_partitions & (UINT64_C(1) << i)))
            continue;
        pthread_mutex_lock(&space->locks[i]);
        pthread_cond_broadcast(&space->conds[i]);
        pthread_mutex_unlock(&space->locks[i]);
    }
}

s_linda_space *new_linda_space(unsigned long max_symbols)
{
    s_linda_space *space = malloc(sizeof(*space));
    if (!space)
        return NULL;
    space->sym = new_intern(max_symbols);
    if (!space->sym) {
        free(space);
        return NULL;
    }
    space->db = new_facts(space->sym, max_symbols);
    if (!space->db) {
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    pthread_condattr_t cond_attr;
    if (pthread_condattr_init(&cond_attr) != 0) {
        delete_facts(space->db);
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&cond_attr);
        delete_facts(space->db);
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    if (pthread_mutex_init(&space->worker_lock, NULL) != 0) {
        pthread_condattr_destroy(&cond_attr);
        delete_facts(space->db);
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    if (pthread_cond_init(&space->worker_cond, NULL) != 0) {
        pthread_mutex_destroy(&space->worker_lock);
        pthread_condattr_destroy(&cond_attr);
        delete_facts(space->db);
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    space->active_workers = 0;
    space->active_operations = 0;
    space->shutting_down = 0;
    for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
        if (pthread_mutex_init(&space->locks[i], NULL) != 0) {
            for (size_t j = 0; j < i; j++) {
                pthread_cond_destroy(&space->conds[j]);
                pthread_mutex_destroy(&space->locks[j]);
            }
            delete_facts(space->db);
            delete_intern(space->sym);
            pthread_cond_destroy(&space->worker_cond);
            pthread_mutex_destroy(&space->worker_lock);
            pthread_condattr_destroy(&cond_attr);
            free(space);
            return NULL;
        }
        if (pthread_cond_init(&space->conds[i], &cond_attr) != 0) {
            pthread_mutex_destroy(&space->locks[i]);
            for (size_t j = 0; j < i; j++) {
                pthread_cond_destroy(&space->conds[j]);
                pthread_mutex_destroy(&space->locks[j]);
            }
            delete_facts(space->db);
            delete_intern(space->sym);
            pthread_cond_destroy(&space->worker_cond);
            pthread_mutex_destroy(&space->worker_lock);
            pthread_condattr_destroy(&cond_attr);
            free(space);
            return NULL;
        }
    }
    pthread_condattr_destroy(&cond_attr);
    if (set_init_checked(&space->multiplicity, max_symbols) != 0) {
        for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
            pthread_cond_destroy(&space->conds[i]);
            pthread_mutex_destroy(&space->locks[i]);
        }
        pthread_cond_destroy(&space->worker_cond);
        pthread_mutex_destroy(&space->worker_lock);
        delete_facts(space->db);
        delete_intern(space->sym);
        free(space);
        return NULL;
    }
    facts_register_internal_commit_summary_observer(space->db, linda_commit_observer, space);
    return space;
}

s_facts *linda_space_facts(s_linda_space *space)
{
    return space && !linda_closed(space) ? space->db : NULL;
}

int linda_space_facts_read_begin(s_linda_space *space, s_linda_facts_guard *guard, const s_facts **facts_out)
{
    if (!space || !guard || !facts_out)
        return LINDA_ERROR;
    memset(guard, 0, sizeof(*guard));
    *facts_out = NULL;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    if (facts_read_begin(space->db, &guard->facts_guard) != 0) {
        linda_operation_end(space);
        return LINDA_ERROR;
    }
    guard->space = space;
    guard->active = 1;
    *facts_out = space->db;
    return LINDA_OK;
}

void linda_space_facts_read_end(s_linda_facts_guard *guard)
{
    if (!guard || !guard->active)
        return;
    s_linda_space *space = guard->space;
    facts_read_end(&guard->facts_guard);
    guard->space = NULL;
    guard->active = 0;
    linda_operation_end(space);
}

int linda_space_attach_program(s_linda_space *space, const s_datalog_program *program)
{
    if (!space || !program)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    result = facts_attach_program(space->db, program) == 0 ? LINDA_OK : LINDA_ERROR;
    linda_operation_end(space);
    return result;
}

int linda_space_detach_program(s_linda_space *space)
{
    if (!space)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    result = facts_detach_program(space->db) == 0 ? LINDA_OK : LINDA_ERROR;
    linda_operation_end(space);
    return result;
}

int linda_space_close(s_linda_space *space)
{
    if (!space)
        return LINDA_ERROR;
    pthread_mutex_lock(&space->worker_lock);
    int already_closed = space->shutting_down;
    space->shutting_down = 1;
    pthread_mutex_unlock(&space->worker_lock);

    if (!already_closed) {
        for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
            pthread_mutex_lock(&space->locks[i]);
            pthread_cond_broadcast(&space->conds[i]);
            pthread_mutex_unlock(&space->locks[i]);
        }
    }
    return LINDA_OK;
}

int linda_space_is_closed(s_linda_space *space)
{
    if (!space)
        return LINDA_ERROR;
    return linda_closed(space);
}

void delete_linda_space(s_linda_space *space)
{
    if (!space)
        return;
    linda_space_close(space);
    pthread_mutex_lock(&space->worker_lock);
    while (space->active_workers > 0 || space->active_operations > 0)
        pthread_cond_wait(&space->worker_cond, &space->worker_lock);
    pthread_mutex_unlock(&space->worker_lock);
    facts_register_internal_commit_summary_observer(space->db, NULL, NULL);
    s_set_cursor cursor;
    set_cursor_init(&space->multiplicity, &cursor);
    s_set_item *item;
    while ((item = set_cursor_next(&cursor)) != NULL) {
        s_linda_entry *entry = (s_linda_entry *)item->data;
        intern_unstring(space->sym, entry->s);
        intern_unstring(space->sym, entry->p);
        intern_unstring(space->sym, entry->o);
        free(entry);
    }
    set_destroy(&space->multiplicity);
    for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
        pthread_cond_destroy(&space->conds[i]);
        pthread_mutex_destroy(&space->locks[i]);
    }
    pthread_cond_destroy(&space->worker_cond);
    pthread_mutex_destroy(&space->worker_lock);
    delete_facts(space->db);
    delete_intern(space->sym);
    free(space);
}

int linda_out(s_linda_space *space, const char *s, const char *p, const char *o)
{
    if (!space || !s || !p || !o)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    if (facts_transaction_begin(space->db) != 0)
        goto error;
    s_fact_support support;
    int existed = facts_get_support_spo(space->db, s, p, o, &support);
    s_fact *fact = facts_add_spo(space->db, s, p, o);
    if (!fact) {
        facts_transaction_rollback(space->db);
        goto error;
    }
    s_linda_entry *entry = linda_entry_get(space, fact);
    if (existed > 0 && support.asserted) {
        if (entry) {
            if (entry->count == SIZE_MAX) {
                facts_transaction_rollback(space->db);
                goto error;
            }
            entry->count++;
        } else if (!linda_entry_add(space, fact, 2)) {
            facts_transaction_rollback(space->db);
            goto error;
        }
    } else if (entry) {
        /* Discard stale overlay state left by unsupported direct DB mutation. */
        linda_entry_remove(space, fact);
    }
    if (facts_transaction_commit(space->db) != 0) {
        facts_transaction_rollback(space->db);
        goto error;
    }
    /* Duplicate out() changes multiplicity without changing physical visibility. */
    linda_signal_subject(space, s);
    linda_operation_end(space);
    return LINDA_OK;

error:
    linda_operation_end(space);
    return LINDA_ERROR;
}

static void copy_output(char *out, size_t max, const char *value)
{
    if (out && max > 0) {
        strncpy(out, value, max - 1);
        out[max - 1] = '\0';
    }
}

static int match_and_extract(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                             size_t max_p, char *out_o, size_t max_o, const char **match_s, const char **match_p,
                             const char **match_o)
{
    const char *values[3] = {NULL, NULL, NULL};
    s_binding bindings[4];
    for (size_t i = 0; i < pattern->variable_count; i++) {
        bindings[i].name = pattern->variables[i];
        bindings[i].value = &values[i];
    }
    bindings[pattern->variable_count].name = NULL;
    bindings[pattern->variable_count].value = NULL;
    const char *spec[5] = {pattern->terms[0], pattern->terms[1], pattern->terms[2], NULL, NULL};
    s_facts_with_cursor cursor;
    if (facts_with_checked(space->db, bindings, &cursor, spec) != 0)
        return -1;
    int found = 0;
    const char *matched[3] = {NULL, NULL, NULL};
    while (facts_with_cursor_next(&cursor)) {
        s_fact *fact = cursor.l[0].fact;
        if (!fact->asserted_count)
            continue;
        s_linda_entry *entry = linda_entry_get(space, fact);
        if (entry && entry->count == 0)
            continue;
        matched[0] = symbol_to_str(fact->s);
        matched[1] = symbol_to_str(fact->p);
        matched[2] = symbol_to_str(fact->o);
        int valid = 1;
        for (size_t i = 0; i < 3 && valid; i++) {
            for (size_t j = i + 1; j < 3; j++) {
                if (pattern->variable_index[i] >= 0 && pattern->variable_index[i] == pattern->variable_index[j] &&
                    strcmp(matched[i], matched[j]) != 0) {
                    valid = 0;
                    break;
                }
            }
        }
        if (valid) {
            found = 1;
            break;
        }
    }
    if (found) {
        copy_output(out_s, max_s, matched[0]);
        copy_output(out_p, max_p, matched[1]);
        copy_output(out_o, max_o, matched[2]);
        if (match_s)
            *match_s = matched[0];
        if (match_p)
            *match_p = matched[1];
        if (match_o)
            *match_o = matched[2];
    }
    facts_with_cursor_destroy(&cursor);
    return facts_with_cursor_error(&cursor) ? -1 : found;
}

static int linda_consume_one(s_linda_space *space, const char *s, const char *p, const char *o)
{
    s_fact *fact = facts_get_spo(space->db, s, p, o);
    if (!fact || !fact->asserted_count)
        return 0;
    s_linda_entry *entry = linda_entry_get(space, fact);
    if (entry) {
        if (entry->count > 2) {
            entry->count--;
        } else {
            /* Count two becomes the database's implicit single occurrence. */
            linda_entry_remove(space, fact);
        }
        return 1;
    }
    return facts_remove_spo(space->db, s, p, o);
}

static int make_deadline(long timeout_ms, struct timespec *deadline)
{
    if (timeout_ms < 0 || clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
        return -1;
    deadline->tv_sec += timeout_ms / 1000;
    deadline->tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int linda_rd_wait(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                         char *out_o, size_t max_o, const struct timespec *deadline)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    unsigned int idx = pattern->cond_index;
    pthread_mutex_lock(&space->locks[idx]);
    for (;;) {
        if (linda_closed(space)) {
            result = LINDA_CLOSED;
            break;
        }
        int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL, NULL, NULL);
        if (found < 0) {
            result = LINDA_ERROR;
            break;
        }
        if (found) {
            result = LINDA_OK;
            break;
        }
        int rc = deadline ? pthread_cond_timedwait(&space->conds[idx], &space->locks[idx], deadline)
                          : pthread_cond_wait(&space->conds[idx], &space->locks[idx]);
        if (rc == ETIMEDOUT) {
            result = LINDA_TIMEOUT;
            break;
        }
        if (rc != 0) {
            result = LINDA_ERROR;
            break;
        }
    }
    pthread_mutex_unlock(&space->locks[idx]);
    linda_operation_end(space);
    return result;
}

static int linda_in_wait(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                         char *out_o, size_t max_o, const struct timespec *deadline)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    unsigned int idx = pattern->cond_index;
    pthread_mutex_lock(&space->locks[idx]);
    for (;;) {
        if (linda_closed(space)) {
            result = LINDA_CLOSED;
            break;
        }
        const char *match_s = NULL, *match_p = NULL, *match_o = NULL;
        if (facts_transaction_begin(space->db) != 0) {
            result = LINDA_ERROR;
            break;
        }
        int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &match_s, &match_p, &match_o);
        if (found < 0) {
            facts_transaction_rollback(space->db);
            result = LINDA_ERROR;
            break;
        }
        if (found) {
            int removed = linda_consume_one(space, match_s, match_p, match_o);
            pthread_mutex_unlock(&space->locks[idx]);
            if (!removed || facts_transaction_commit(space->db) != 0) {
                facts_transaction_rollback(space->db);
                result = LINDA_ERROR;
            } else {
                result = LINDA_OK;
            }
            linda_operation_end(space);
            return result;
        }
        if (facts_transaction_commit(space->db) != 0) {
            result = LINDA_ERROR;
            break;
        }
        int rc = deadline ? pthread_cond_timedwait(&space->conds[idx], &space->locks[idx], deadline)
                          : pthread_cond_wait(&space->conds[idx], &space->locks[idx]);
        if (rc == ETIMEDOUT) {
            result = LINDA_TIMEOUT;
            break;
        }
        if (rc != 0) {
            result = LINDA_ERROR;
            break;
        }
    }
    pthread_mutex_unlock(&space->locks[idx]);
    linda_operation_end(space);
    return result;
}

int linda_rd_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                     char *out_o, size_t max_o)
{
    return linda_rd_wait(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL);
}

int linda_in_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                     char *out_o, size_t max_o)
{
    return linda_in_wait(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL);
}

int linda_rd_pattern_timed(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                           size_t max_p, char *out_o, size_t max_o, long timeout_ms)
{
    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) != 0)
        return LINDA_ERROR;
    return linda_rd_wait(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &deadline);
}

int linda_in_pattern_timed(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                           size_t max_p, char *out_o, size_t max_o, long timeout_ms)
{
    struct timespec deadline;
    if (make_deadline(timeout_ms, &deadline) != 0)
        return LINDA_ERROR;
    return linda_in_wait(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &deadline);
}

int linda_rdp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                      char *out_o, size_t max_o)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    result = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL, NULL, NULL);
    linda_operation_end(space);
    return result;
}

int linda_inp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p, size_t max_p,
                      char *out_o, size_t max_o)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    int result = linda_operation_begin(space);
    if (result != LINDA_OK)
        return result;
    if (facts_transaction_begin(space->db) != 0) {
        linda_operation_end(space);
        return LINDA_ERROR;
    }
    const char *match_s = NULL, *match_p = NULL, *match_o = NULL;
    int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &match_s, &match_p, &match_o);
    if (found < 0) {
        facts_transaction_rollback(space->db);
        linda_operation_end(space);
        return LINDA_ERROR;
    }
    if (found && linda_consume_one(space, match_s, match_p, match_o) <= 0) {
        facts_transaction_rollback(space->db);
        linda_operation_end(space);
        return LINDA_ERROR;
    }
    if (facts_transaction_commit(space->db) != 0) {
        facts_transaction_rollback(space->db);
        linda_operation_end(space);
        return LINDA_ERROR;
    }
    linda_operation_end(space);
    return found;
}

#define WITH_PATTERN(call)                                                                                                         \
    s_linda_pattern pattern;                                                                                                       \
    if (linda_pattern_init(&pattern, s, p, o, 0) != 0)                                                                             \
        return LINDA_ERROR;                                                                                                        \
    return call

int linda_rd(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o)
{
    WITH_PATTERN(linda_rd_pattern(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o));
}

int linda_in(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
             size_t max_p, char *out_o, size_t max_o)
{
    WITH_PATTERN(linda_in_pattern(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o));
}

int linda_rd_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
                   size_t max_p, char *out_o, size_t max_o, long timeout_ms)
{
    WITH_PATTERN(linda_rd_pattern_timed(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o, timeout_ms));
}

int linda_in_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
                   size_t max_p, char *out_o, size_t max_o, long timeout_ms)
{
    WITH_PATTERN(linda_in_pattern_timed(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o, timeout_ms));
}

int linda_rdp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o)
{
    WITH_PATTERN(linda_rdp_pattern(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o));
}

int linda_inp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s, char *out_p,
              size_t max_p, char *out_o, size_t max_o)
{
    WITH_PATTERN(linda_inp_pattern(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o));
}

typedef struct {
    s_linda_space *space;
    f_linda_worker func;
    void *arg;
} s_linda_eval_args;

static void linda_eval_cleanup(void *raw_args)
{
    s_linda_eval_args *args = (s_linda_eval_args *)raw_args;
    pthread_mutex_lock(&args->space->worker_lock);
    args->space->active_workers--;
    pthread_cond_broadcast(&args->space->worker_cond);
    pthread_mutex_unlock(&args->space->worker_lock);
    free(args);
}

static void *linda_eval_thread_wrapper(void *raw_args)
{
    s_linda_eval_args *args = (s_linda_eval_args *)raw_args;
    pthread_cleanup_push(linda_eval_cleanup, args);
    args->func(args->arg);
    pthread_cleanup_pop(1);
    return NULL;
}

int linda_eval(s_linda_space *space, f_linda_worker func, void *arg)
{
    if (!space || !func)
        return LINDA_ERROR;
    pthread_t thread;
    s_linda_eval_args *args = malloc(sizeof(*args));
    if (!args)
        return LINDA_ERROR;
    pthread_mutex_lock(&space->worker_lock);
    if (space->shutting_down) {
        pthread_mutex_unlock(&space->worker_lock);
        free(args);
        return LINDA_CLOSED;
    }
    space->active_workers++;
    pthread_mutex_unlock(&space->worker_lock);
    args->space = space;
    args->func = func;
    args->arg = arg;
    if (pthread_create(&thread, NULL, linda_eval_thread_wrapper, args) != 0) {
        pthread_mutex_lock(&space->worker_lock);
        space->active_workers--;
        pthread_cond_broadcast(&space->worker_cond);
        pthread_mutex_unlock(&space->worker_lock);
        free(args);
        return LINDA_ERROR;
    }
    pthread_detach(thread);
    return LINDA_OK;
}
