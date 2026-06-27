#include "linda.h"
#include "spec.h"
#include "binding.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

s_linda_space *new_linda_space(unsigned long max_symbols)
{
    s_linda_space *space = malloc(sizeof(s_linda_space));
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

    if (pthread_mutex_init(&space->lock, NULL) != 0) {
        delete_facts(space->db);
        free(space);
        return NULL;
    }

    if (pthread_cond_init(&space->cond, NULL) != 0) {
        pthread_mutex_destroy(&space->lock);
        delete_facts(space->db);
        free(space);
        return NULL;
    }

    return space;
}

void delete_linda_space(s_linda_space *space)
{
    if (!space)
        return;

    pthread_cond_destroy(&space->cond);
    pthread_mutex_destroy(&space->lock);
    delete_facts(space->db);
    free(space);
}

int linda_out(s_linda_space *space, const char *s, const char *p, const char *o)
{
    if (!space || !s || !p || !o)
        return -1;

    pthread_mutex_lock(&space->lock);

    facts_transaction_begin(space->db);
    facts_add_spo(space->db, s, p, o);
    facts_transaction_commit(space->db);

    pthread_cond_broadcast(&space->cond);

    pthread_mutex_unlock(&space->lock);
    return 0;
}

static int match_and_extract(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, char *out_p,
                             char *out_o)
{
    const char *spec[5] = {s, p, o, NULL, NULL};
    s_binding *bindings = spec_bindings(spec);
    s_facts_with_cursor c;
    facts_with(space->db, bindings, &c, spec);
    int found = facts_with_cursor_next(&c);
    if (found) {
        if (out_s) {
            if (s[0] == '?') {
                const char **val = bindings_get(bindings, s);
                strcpy(out_s, (val && *val) ? *val : s);
            } else {
                strcpy(out_s, s);
            }
        }
        if (out_p) {
            if (p[0] == '?') {
                const char **val = bindings_get(bindings, p);
                strcpy(out_p, (val && *val) ? *val : p);
            } else {
                strcpy(out_p, p);
            }
        }
        if (out_o) {
            if (o[0] == '?') {
                const char **val = bindings_get(bindings, o);
                strcpy(out_o, (val && *val) ? *val : o);
            } else {
                strcpy(out_o, o);
            }
        }
    }
    facts_with_cursor_destroy(&c);
    free(bindings);
    return found;
}

int linda_rd(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, char *out_p, char *out_o)
{
    if (!space || !s || !p || !o)
        return -1;

    pthread_mutex_lock(&space->lock);

    while (1) {
        if (match_and_extract(space, s, p, o, out_s, out_p, out_o)) {
            pthread_mutex_unlock(&space->lock);
            return 0;
        }
        pthread_cond_wait(&space->cond, &space->lock);
    }
}

int linda_in(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, char *out_p, char *out_o)
{
    if (!space || !s || !p || !o)
        return -1;

    pthread_mutex_lock(&space->lock);

    while (1) {
        char match_s[256], match_p[256], match_o[256];
        if (match_and_extract(space, s, p, o, match_s, match_p, match_o)) {
            if (out_s)
                strcpy(out_s, match_s);
            if (out_p)
                strcpy(out_p, match_p);
            if (out_o)
                strcpy(out_o, match_o);

            facts_transaction_begin(space->db);
            facts_remove_spo(space->db, match_s, match_p, match_o);
            facts_transaction_commit(space->db);

            pthread_mutex_unlock(&space->lock);
            return 0;
        }
        pthread_cond_wait(&space->cond, &space->lock);
    }
}

int linda_rdp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, char *out_p, char *out_o)
{
    if (!space || !s || !p || !o)
        return -1;

    pthread_mutex_lock(&space->lock);
    int found = match_and_extract(space, s, p, o, out_s, out_p, out_o);
    pthread_mutex_unlock(&space->lock);
    return found;
}

int linda_inp(s_linda_space *space, const char *s, const char *p, const char *o, char *out_s, char *out_p, char *out_o)
{
    if (!space || !s || !p || !o)
        return -1;

    pthread_mutex_lock(&space->lock);
    char match_s[256], match_p[256], match_o[256];
    int found = match_and_extract(space, s, p, o, match_s, match_p, match_o);
    if (found) {
        if (out_s)
            strcpy(out_s, match_s);
        if (out_p)
            strcpy(out_p, match_p);
        if (out_o)
            strcpy(out_o, match_o);

        facts_transaction_begin(space->db);
        facts_remove_spo(space->db, match_s, match_p, match_o);
        facts_transaction_commit(space->db);
    }
    pthread_mutex_unlock(&space->lock);
    return found;
}

typedef struct {
    s_linda_space *space;
    f_linda_worker func;
    void *arg;
} s_linda_eval_args;

static void *linda_eval_thread_wrapper(void *raw_args)
{
    s_linda_eval_args *args = (s_linda_eval_args *)raw_args;
    args->func(args->arg);
    free(args);
    return NULL;
}

int linda_eval(s_linda_space *space, f_linda_worker func, void *arg)
{
    pthread_t thread;
    s_linda_eval_args *args = malloc(sizeof(s_linda_eval_args));
    if (!args)
        return -1;
    args->space = space;
    args->func = func;
    args->arg = arg;
    if (pthread_create(&thread, NULL, linda_eval_thread_wrapper, args) != 0) {
        free(args);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}
