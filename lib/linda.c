#include "linda.h"
#include "binding.h"
#include <errno.h>
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

static unsigned int get_linda_cond_idx(const char *s)
{
    if (!s || s[0] == '?')
        return 0;
    unsigned int hash = 5381;
    int c;
    while ((c = *s++))
        hash = ((hash << 5) + hash) + (unsigned int)c;
    return 1 + (hash % (LINDA_COND_PARTITIONS - 1));
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

static void linda_commit_observer(s_facts *facts, void *user_data)
{
    (void)facts;
    s_linda_space *space = (s_linda_space *)user_data;
    for (size_t i = 0; i < LINDA_COND_PARTITIONS; i++) {
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
    facts_register_commit_observer(space->db, linda_commit_observer, space);
    return space;
}

void delete_linda_space(s_linda_space *space)
{
    if (!space)
        return;
    pthread_mutex_lock(&space->worker_lock);
    space->shutting_down = 1;
    while (space->active_workers > 0)
        pthread_cond_wait(&space->worker_cond, &space->worker_lock);
    pthread_mutex_unlock(&space->worker_lock);
    facts_register_commit_observer(space->db, NULL, NULL);
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
    if (facts_transaction_begin(space->db) != 0)
        return LINDA_ERROR;
    if (!facts_add_spo(space->db, s, p, o)) {
        facts_transaction_rollback(space->db);
        return LINDA_ERROR;
    }
    if (facts_transaction_commit(space->db) != 0) {
        facts_transaction_rollback(space->db);
        return LINDA_ERROR;
    }
    return LINDA_OK;
}

static void copy_output(char *out, size_t max, const char *value)
{
    if (out && max > 0) {
        strncpy(out, value, max - 1);
        out[max - 1] = '\0';
    }
}

static int match_and_extract(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s,
                             char *out_p, size_t max_p, char *out_o, size_t max_o, const char **match_s,
                             const char **match_p, const char **match_o)
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
    facts_with(space->db, bindings, &cursor, spec);
    int found = 0;
    const char *matched[3] = {NULL, NULL, NULL};
    while (facts_with_cursor_next(&cursor)) {
        s_fact *fact = cursor.l[0].fact;
        matched[0] = symbol_to_str(fact->s);
        matched[1] = symbol_to_str(fact->p);
        matched[2] = symbol_to_str(fact->o);
        int valid = 1;
        for (size_t i = 0; i < 3 && valid; i++) {
            for (size_t j = i + 1; j < 3; j++) {
                if (pattern->variable_index[i] >= 0 &&
                    pattern->variable_index[i] == pattern->variable_index[j] && strcmp(matched[i], matched[j]) != 0) {
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
    return found;
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

static int linda_rd_wait(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                         size_t max_p, char *out_o, size_t max_o, const struct timespec *deadline)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    unsigned int idx = pattern->cond_index;
    pthread_mutex_lock(&space->locks[idx]);
    for (;;) {
        int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL, NULL, NULL);
        if (found < 0) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
        if (found) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_OK;
        }
        int rc = deadline ? pthread_cond_timedwait(&space->conds[idx], &space->locks[idx], deadline)
                          : pthread_cond_wait(&space->conds[idx], &space->locks[idx]);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_TIMEOUT;
        }
        if (rc != 0) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
    }
}

static int linda_in_wait(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                         size_t max_p, char *out_o, size_t max_o, const struct timespec *deadline)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    unsigned int idx = pattern->cond_index;
    pthread_mutex_lock(&space->locks[idx]);
    for (;;) {
        const char *match_s = NULL, *match_p = NULL, *match_o = NULL;
        if (facts_transaction_begin(space->db) != 0) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
        int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &match_s, &match_p,
                                      &match_o);
        if (found < 0) {
            facts_transaction_rollback(space->db);
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
        if (found) {
            int removed = facts_remove_spo(space->db, match_s, match_p, match_o);
            pthread_mutex_unlock(&space->locks[idx]);
            if (!removed || facts_transaction_commit(space->db) != 0) {
                facts_transaction_rollback(space->db);
                return LINDA_ERROR;
            }
            return LINDA_OK;
        }
        if (facts_transaction_commit(space->db) != 0) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
        int rc = deadline ? pthread_cond_timedwait(&space->conds[idx], &space->locks[idx], deadline)
                          : pthread_cond_wait(&space->conds[idx], &space->locks[idx]);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_TIMEOUT;
        }
        if (rc != 0) {
            pthread_mutex_unlock(&space->locks[idx]);
            return LINDA_ERROR;
        }
    }
}

int linda_rd_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                     size_t max_p, char *out_o, size_t max_o)
{
    return linda_rd_wait(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL);
}

int linda_in_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                     size_t max_p, char *out_o, size_t max_o)
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

int linda_rdp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                      size_t max_p, char *out_o, size_t max_o)
{
    if (!space || !pattern)
        return LINDA_ERROR;
    return match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, NULL, NULL, NULL);
}

int linda_inp_pattern(s_linda_space *space, const s_linda_pattern *pattern, char *out_s, size_t max_s, char *out_p,
                      size_t max_p, char *out_o, size_t max_o)
{
    if (!space || !pattern || facts_transaction_begin(space->db) != 0)
        return LINDA_ERROR;
    const char *match_s = NULL, *match_p = NULL, *match_o = NULL;
    int found = match_and_extract(space, pattern, out_s, max_s, out_p, max_p, out_o, max_o, &match_s, &match_p,
                                  &match_o);
    if (found < 0) {
        facts_transaction_rollback(space->db);
        return LINDA_ERROR;
    }
    if (found && !facts_remove_spo(space->db, match_s, match_p, match_o)) {
        facts_transaction_rollback(space->db);
        return LINDA_ERROR;
    }
    if (facts_transaction_commit(space->db) != 0) {
        facts_transaction_rollback(space->db);
        return LINDA_ERROR;
    }
    return found;
}

#define WITH_PATTERN(call)                                                                                             \
    s_linda_pattern pattern;                                                                                          \
    if (linda_pattern_init(&pattern, s, p, o, 0) != 0)                                                               \
        return LINDA_ERROR;                                                                                           \
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

int linda_rd_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s,
                   char *out_p, size_t max_p, char *out_o, size_t max_o, long timeout_ms)
{
    WITH_PATTERN(linda_rd_pattern_timed(space, &pattern, out_s, max_s, out_p, max_p, out_o, max_o, timeout_ms));
}

int linda_in_timed(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, size_t max_s,
                   char *out_p, size_t max_p, char *out_o, size_t max_o, long timeout_ms)
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
        return LINDA_ERROR;
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
