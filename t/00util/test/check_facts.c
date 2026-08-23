#include <check.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "facts.h"
#include "io.h"
#include "sparql.h"

s_facts *g_f;

static Symbol ti(const void *s)
{
    if (!s || s == (const void *)P_FIRST || s == (const void *)P_LAST)
        return (Symbol)s;
    if (g_f && g_f->symbols)
        return intern_string(g_f->symbols, (const char *)s);
    return (Symbol)s;
}

#undef fact_init
#define fact_init(f, s, p, o) fact_init(f, ti(s), ti(p), ti(o))

START_TEST(test_facts_init_destroy)
{
    s_facts f;
    facts_init(&f, NULL, 100);
    ck_assert(!facts_count(&f));
    facts_destroy(&f);
}
END_TEST

START_TEST(test_shared_symbol_references_are_released)
{
    s_intern *symbols = new_intern(100);
    s_facts *facts = new_facts(symbols, 100);
    ck_assert(symbols != NULL);
    ck_assert(facts != NULL);

    ck_assert(facts_add_spo(facts, "shared-subject", "shared-predicate", "shared-object"));
    ck_assert(intern_find_symbol(symbols, "shared-subject") != NULL);
    facts_reset(facts);
    ck_assert(intern_find_symbol(symbols, "shared-subject") == NULL);

    ck_assert(facts_add_spo(facts, "destroy-subject", "destroy-predicate", "destroy-object"));
    ck_assert(intern_find_symbol(symbols, "destroy-subject") != NULL);
    delete_facts(facts);
    ck_assert(intern_find_symbol(symbols, "destroy-subject") == NULL);
    delete_intern(symbols);
}
END_TEST

static void *foreign_commit(void *arg)
{
    int *result = malloc(sizeof(*result));
    if (result)
        *result = facts_transaction_commit(arg);
    return result;
}

START_TEST(test_transaction_rejects_foreign_commit)
{
    ck_assert_int_eq(0, facts_transaction_begin(g_f));
    ck_assert(facts_add_spo(g_f, "owned", "by", "main") != NULL);

    pthread_t thread;
    ck_assert_int_eq(0, pthread_create(&thread, NULL, foreign_commit, g_f));
    int *result = NULL;
    ck_assert_int_eq(0, pthread_join(thread, (void **)&result));
    ck_assert(result != NULL);
    ck_assert_int_eq(-1, *result);
    free(result);

    ck_assert_int_eq(0, facts_transaction_rollback(g_f));
    ck_assert(facts_get_spo(g_f, "owned", "by", "main") == NULL);
}
END_TEST

typedef struct guarded_writer_data {
    s_facts *facts;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int started;
    int result;
} s_guarded_writer_data;

static void *guarded_writer(void *arg)
{
    s_guarded_writer_data *data = arg;
    pthread_mutex_lock(&data->mutex);
    data->started = 1;
    pthread_cond_signal(&data->cond);
    pthread_mutex_unlock(&data->mutex);
    data->result = facts_remove_spo(data->facts, "guarded", "value", "alive");
    return NULL;
}

START_TEST(test_safe_read_apis)
{
    ck_assert(facts_add_spo(g_f, "guarded", "value", "alive"));
    ck_assert_int_eq(facts_contains_symbol(g_f, "guarded"), 1);
    ck_assert_int_eq(facts_contains_spo(g_f, "guarded", "value", "alive"), 1);

    s_fact_snapshot snapshot;
    ck_assert_int_eq(facts_get_spo_snapshot(g_f, "guarded", "value", "alive", &snapshot), 1);
    ck_assert_str_eq(snapshot.s, "guarded");
    ck_assert_str_eq(snapshot.p, "value");
    ck_assert_str_eq(snapshot.o, "alive");
    ck_assert_int_eq(snapshot.support.asserted, 1);

    s_facts_read_guard outer = {0};
    s_facts_read_guard inner = {0};
    ck_assert_int_eq(facts_read_begin(g_f, &outer), 0);
    ck_assert_int_eq(facts_read_begin(g_f, &inner), 0);
    s_fact *borrowed = facts_get_spo(g_f, "guarded", "value", "alive");
    ck_assert(borrowed);

    s_guarded_writer_data data = {.facts = g_f, .started = 0, .result = -1};
    pthread_mutex_init(&data.mutex, NULL);
    pthread_cond_init(&data.cond, NULL);
    pthread_t writer;
    ck_assert_int_eq(pthread_create(&writer, NULL, guarded_writer, &data), 0);
    pthread_mutex_lock(&data.mutex);
    while (!data.started)
        pthread_cond_wait(&data.cond, &data.mutex);
    pthread_mutex_unlock(&data.mutex);

    ck_assert_str_eq(symbol_to_str(borrowed->s), "guarded");
    ck_assert_int_eq(facts_contains_spo(g_f, "guarded", "value", "alive"), 1);
    facts_read_end(&inner);
    facts_read_end(&outer);

    ck_assert_int_eq(pthread_join(writer, NULL), 0);
    ck_assert_int_eq(data.result, 1);
    pthread_cond_destroy(&data.cond);
    pthread_mutex_destroy(&data.mutex);
    ck_assert_int_eq(facts_contains_spo(g_f, "guarded", "value", "alive"), 0);

    /* Owning snapshots remain valid after the underlying tuple is removed. */
    ck_assert_str_eq(snapshot.s, "guarded");
    ck_assert_str_eq(snapshot.o, "alive");
    facts_snapshot_destroy(&snapshot);

    ck_assert(facts_add_spo(g_f, "entity", "name", "copy me"));
    char *value = NULL;
    ck_assert_int_eq(facts_get_prop_copy(g_f, "entity", "name", &value), 1);
    ck_assert(facts_remove_spo(g_f, "entity", "name", "copy me"));
    ck_assert_str_eq(value, "copy me");
    free(value);
}
END_TEST

static int summary_calls;
static s_facts_commit_summary last_summary;

static void commit_summary_observer(s_facts *facts, const s_facts_commit_summary *summary, void *user_data)
{
    (void)facts;
    (void)user_data;
    summary_calls++;
    last_summary = *summary;
}

START_TEST(test_commit_summary_partitions)
{
    const char *subjects[] = {"partition-alpha", "partition-beta"};
    uint64_t expected = UINT64_C(1);
    for (size_t i = 0; i < 2; i++)
        expected |= UINT64_C(1) << facts_commit_subject_partition(subjects[i]);

    summary_calls = 0;
    memset(&last_summary, 0, sizeof(last_summary));
    facts_register_commit_summary_observer(g_f, commit_summary_observer, NULL);
    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_add_spo(g_f, subjects[0], "p", "o"));
    ck_assert(facts_add_spo(g_f, subjects[1], "p", "o"));
    ck_assert_int_eq(facts_transaction_commit(g_f), 0);

    ck_assert_int_eq(summary_calls, 1);
    ck_assert_int_eq(last_summary.physical_changes, 2);
    ck_assert(last_summary.subject_partitions == expected);
    facts_register_commit_summary_observer(g_f, NULL, NULL);
}
END_TEST

START_TEST(test_facts_reset)
{
    s_facts *f = new_facts(NULL, 100);
    ck_assert(f);
    ck_assert_int_eq(facts_count(f), 0);

    facts_add_spo(f, "alice", "friend", "bob");
    facts_add_spo(f, "bob", "friend", "charlie");
    ck_assert_int_eq(facts_count(f), 2);
    ck_assert(facts_find_symbol(f, "alice") != NULL);

    facts_reset(f);
    ck_assert_int_eq(facts_count(f), 0);
    ck_assert(facts_find_symbol(f, "alice") == NULL);

    facts_add_spo(f, "alice", "friend", "bob");
    ck_assert_int_eq(facts_count(f), 1);
    ck_assert(facts_find_symbol(f, "alice") != NULL);

    delete_facts(f);
}
END_TEST

START_TEST(test_facts_reset_rejects_active_transaction)
{
    s_facts *f = new_facts(NULL, 100);
    ck_assert(f);
    ck_assert_int_eq(facts_transaction_begin(f), 0);
    ck_assert(facts_add_spo(f, "still", "in", "transaction"));
    ck_assert_int_eq(facts_reset_checked(f), -1);
    ck_assert_int_eq(facts_contains_spo(f, "still", "in", "transaction"), 1);
    ck_assert_int_eq(facts_transaction_rollback(f), 0);
    delete_facts(f);
}
END_TEST

START_TEST(test_facts_new_delete)
{
    s_facts *f;
    f = new_facts(NULL, 100);
    ck_assert(f);
    ck_assert(!facts_count(f));
    delete_facts(f);
}
END_TEST

void setup_add_fact(void)
{
    g_f = new_facts(NULL, 100);
}

void teardown_add_fact(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_add_fact_one)
{
    s_fact a, aa;
    fact_init(&a, "a", "b", "c");
    fact_init(&aa, "a", "b", "c");
    s_fact *ia;
    ck_assert(facts_count(g_f) == 0);
    ck_assert((ia = facts_add_fact(g_f, &a)));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(ia == facts_add_fact(g_f, &a));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(ia == facts_add_fact(g_f, &aa));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_fact_two)
{
    s_fact a, aa, b, bb;
    fact_init(&a, "a", "b", "c");
    fact_init(&aa, "a", "b", "c");
    fact_init(&b, "b", "c", "d");
    fact_init(&bb, "b", "c", "d");
    s_fact *ia;
    s_fact *ib;
    ck_assert(facts_count(g_f) == 0);
    ck_assert((ia = facts_add_fact(g_f, &a)));
    ck_assert(facts_count(g_f) == 1);
    ck_assert((ib = facts_add_fact(g_f, &b)));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_fact(g_f, &a) == ia);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_fact(g_f, &b) == ib);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_fact(g_f, &aa) == ia);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_fact(g_f, &bb) == ib);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_fact_ten)
{
    s_fact a, b, c, d, e, f, g, h, i, j;
    fact_init(&a, "a", "b", "c");
    fact_init(&b, "b", "c", "d");
    fact_init(&c, "c", "d", "e");
    fact_init(&d, "d", "e", "f");
    fact_init(&e, "e", "f", "g");
    fact_init(&f, "f", "g", "h");
    fact_init(&g, "g", "h", "i");
    fact_init(&h, "h", "i", "j");
    fact_init(&i, "i", "j", "k");
    fact_init(&j, "j", "k", "l");
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_fact(g_f, &a));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(facts_add_fact(g_f, &b));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_fact(g_f, &c));
    ck_assert(facts_count(g_f) == 3);
    ck_assert(facts_add_fact(g_f, &d));
    ck_assert(facts_count(g_f) == 4);
    ck_assert(facts_add_fact(g_f, &e));
    ck_assert(facts_count(g_f) == 5);
    ck_assert(facts_add_fact(g_f, &f));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(facts_add_fact(g_f, &g));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(facts_add_fact(g_f, &h));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_add_fact(g_f, &i));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_add_fact(g_f, &j));
    ck_assert(facts_count(g_f) == 10);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

void setup_add_spo(void)
{
    g_f = new_facts(NULL, 100);
}

void teardown_add_spo(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_add_spo_one)
{
    s_fact *ia;
    ck_assert(facts_count(g_f) == 0);
    ck_assert((ia = facts_add_spo(g_f, "a", "b", "c")));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(ia == facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_spo_two)
{
    s_fact *ia;
    s_fact *ib;
    ck_assert(facts_count(g_f) == 0);
    ck_assert((ia = facts_add_spo(g_f, "a", "b", "c")));
    ck_assert(facts_count(g_f) == 1);
    ck_assert((ib = facts_add_spo(g_f, "b", "c", "d")));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_spo(g_f, "a", "b", "c") == ia);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_spo(g_f, "b", "c", "d") == ib);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_spo_ten)
{
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_add_spo(g_f, "c", "d", "e"));
    ck_assert(facts_count(g_f) == 3);
    ck_assert(facts_add_spo(g_f, "d", "e", "f"));
    ck_assert(facts_count(g_f) == 4);
    ck_assert(facts_add_spo(g_f, "e", "f", "g"));
    ck_assert(facts_count(g_f) == 5);
    ck_assert(facts_add_spo(g_f, "f", "g", "h"));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(facts_add_spo(g_f, "g", "h", "i"));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(facts_add_spo(g_f, "h", "i", "j"));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_add_spo(g_f, "i", "j", "k"));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_add_spo(g_f, "j", "k", "l"));
    ck_assert(facts_count(g_f) == 10);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

void setup_add(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_add(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_add_one)
{
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_two)
{
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", "d", "e", NULL, NULL}));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", "d", "e", NULL, NULL}));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_ten)
{
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", "d", "e", NULL, "b", "c", "d",  "e", "f", "g", "h",  NULL, "i",
                                               "j", "k", "l", "m", "n", "o",  "p", "q", NULL, "r", "s", "t", NULL, NULL}));
    ck_assert(facts_count(g_f) == 10);
    ck_assert(!facts_add(g_f, (const char *[]){"a", "b", "c", "d", "e", NULL, "b", "c", "d",  "e", "f", "g", "h",  NULL, "i",
                                               "j", "k", "l", "m", "n", "o",  "p", "q", NULL, "r", "s", "t", NULL, NULL}));
    ck_assert(facts_count(g_f) == 10);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "a"));
}
END_TEST

START_TEST(test_facts_add_anon)
{
    FILE *fp = fopen("/tmp/test_write_facts_add_anon", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!facts_add(g_f, (const char *[]){"?a", "b", "c", "d", "?e", "?f", "g", "?h", "?i", NULL, "i", "j", "?k", "?l", "m",
                                               "?n", "?o", NULL, NULL}));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(!facts_add(g_f, (const char *[]){"?a", "b", "c", "d", "?e", "?f", "g", "?h", "?i", NULL, "i", "j", "?k", "?l", "m",
                                               "?n", "?o", NULL, NULL}));
    ck_assert(facts_count(g_f) == 14);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(facts_find_symbol(g_f, "b"));
    ck_assert(!write_facts(g_f, fp));
}
END_TEST

void setup_remove_fact(void)
{
    g_f = new_facts(NULL, 100);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "b", "c", "d");
    facts_add_spo(g_f, "c", "d", "e");
    facts_add_spo(g_f, "d", "e", "f");
    facts_add_spo(g_f, "e", "f", "g");
    facts_add_spo(g_f, "f", "g", "h");
    facts_add_spo(g_f, "g", "h", "i");
    facts_add_spo(g_f, "h", "i", "j");
    facts_add_spo(g_f, "i", "j", "k");
    facts_add_spo(g_f, "j", "k", "l");
}

void teardown_remove_fact(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_remove_fact_one)
{
    s_fact a;
    fact_init(&a, "a", "b", "c");
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_fact(g_f, &a));
    facts_unintern(g_f, a.s);
    facts_unintern(g_f, a.p);
    facts_unintern(g_f, a.o);
    ck_assert(facts_count(g_f) == 9);
    ck_assert(!facts_remove_fact(g_f, &a));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(!facts_find_symbol(g_f, "a"));
    ck_assert(facts_find_symbol(g_f, "j"));
}
END_TEST

START_TEST(test_facts_remove_fact_two)
{
    s_fact a, b;
    fact_init(&a, "a", "b", "c");
    fact_init(&b, "b", "c", "d");
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_fact(g_f, &a));
    facts_unintern(g_f, a.s);
    facts_unintern(g_f, a.p);
    facts_unintern(g_f, a.o);
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_remove_fact(g_f, &b));
    facts_unintern(g_f, b.s);
    facts_unintern(g_f, b.p);
    facts_unintern(g_f, b.o);
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_remove_fact(g_f, &a));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_remove_fact(g_f, &b));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(!facts_find_symbol(g_f, "a"));
    ck_assert(!facts_find_symbol(g_f, "b"));
    ck_assert(facts_find_symbol(g_f, "j"));
}
END_TEST

START_TEST(test_facts_remove_fact_ten)
{
    s_fact a, b, c, d, e, f, g, h, i, j;
    fact_init(&a, "a", "b", "c");
    fact_init(&b, "b", "c", "d");
    fact_init(&c, "c", "d", "e");
    fact_init(&d, "d", "e", "f");
    fact_init(&e, "e", "f", "g");
    fact_init(&f, "f", "g", "h");
    fact_init(&g, "g", "h", "i");
    fact_init(&h, "h", "i", "j");
    fact_init(&i, "i", "j", "k");
    fact_init(&j, "j", "k", "l");
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_fact(g_f, &a));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_remove_fact(g_f, &b));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_remove_fact(g_f, &c));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(facts_remove_fact(g_f, &d));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(facts_remove_fact(g_f, &e));
    ck_assert(facts_count(g_f) == 5);
    ck_assert(facts_remove_fact(g_f, &f));
    ck_assert(facts_count(g_f) == 4);
    ck_assert(facts_remove_fact(g_f, &g));
    ck_assert(facts_count(g_f) == 3);
    ck_assert(facts_remove_fact(g_f, &h));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_remove_fact(g_f, &i));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(facts_remove_fact(g_f, &j));
    ck_assert(facts_count(g_f) == 0);
    facts_unintern(g_f, a.s);
    facts_unintern(g_f, a.p);
    facts_unintern(g_f, a.o);
    facts_unintern(g_f, b.s);
    facts_unintern(g_f, b.p);
    facts_unintern(g_f, b.o);
    facts_unintern(g_f, c.s);
    facts_unintern(g_f, c.p);
    facts_unintern(g_f, c.o);
    facts_unintern(g_f, d.s);
    facts_unintern(g_f, d.p);
    facts_unintern(g_f, d.o);
    facts_unintern(g_f, e.s);
    facts_unintern(g_f, e.p);
    facts_unintern(g_f, e.o);
    facts_unintern(g_f, f.s);
    facts_unintern(g_f, f.p);
    facts_unintern(g_f, f.o);
    facts_unintern(g_f, g.s);
    facts_unintern(g_f, g.p);
    facts_unintern(g_f, g.o);
    facts_unintern(g_f, h.s);
    facts_unintern(g_f, h.p);
    facts_unintern(g_f, h.o);
    facts_unintern(g_f, i.s);
    facts_unintern(g_f, i.p);
    facts_unintern(g_f, i.o);
    facts_unintern(g_f, j.s);
    facts_unintern(g_f, j.p);
    facts_unintern(g_f, j.o);
    ck_assert(!facts_find_symbol(g_f, "0"));
    ck_assert(!facts_find_symbol(g_f, "a"));
    ck_assert(!facts_find_symbol(g_f, "b"));
    ck_assert(!facts_find_symbol(g_f, "c"));
    ck_assert(!facts_find_symbol(g_f, "d"));
    ck_assert(!facts_find_symbol(g_f, "e"));
    ck_assert(!facts_find_symbol(g_f, "f"));
    ck_assert(!facts_find_symbol(g_f, "g"));
    ck_assert(!facts_find_symbol(g_f, "h"));
    ck_assert(!facts_find_symbol(g_f, "i"));
    ck_assert(!facts_find_symbol(g_f, "j"));
}
END_TEST

void setup_remove_spo(void)
{
    g_f = new_facts(NULL, 100);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "b", "c", "d");
    facts_add_spo(g_f, "c", "d", "e");
    facts_add_spo(g_f, "d", "e", "f");
    facts_add_spo(g_f, "e", "f", "g");
    facts_add_spo(g_f, "f", "g", "h");
    facts_add_spo(g_f, "g", "h", "i");
    facts_add_spo(g_f, "h", "i", "j");
    facts_add_spo(g_f, "i", "j", "k");
    facts_add_spo(g_f, "j", "k", "l");
}

void teardown_remove_spo(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_remove_spo_one)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(!facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 9);
}
END_TEST

START_TEST(test_facts_remove_spo_two)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(facts_count(g_f) == 8);
}
END_TEST

START_TEST(test_facts_remove_spo_ten)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_remove_spo(g_f, "c", "d", "e"));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(facts_remove_spo(g_f, "d", "e", "f"));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(facts_remove_spo(g_f, "e", "f", "g"));
    ck_assert(facts_count(g_f) == 5);
    ck_assert(facts_remove_spo(g_f, "f", "g", "h"));
    ck_assert(facts_count(g_f) == 4);
    ck_assert(facts_remove_spo(g_f, "g", "h", "i"));
    ck_assert(facts_count(g_f) == 3);
    ck_assert(facts_remove_spo(g_f, "h", "i", "j"));
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_remove_spo(g_f, "i", "j", "k"));
    ck_assert(facts_count(g_f) == 1);
    ck_assert(facts_remove_spo(g_f, "j", "k", "l"));
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

void setup_remove(void)
{
    g_f = new_facts(NULL, 10);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "b", "c", "d");
    facts_add_spo(g_f, "c", "d", "e");
    facts_add_spo(g_f, "d", "e", "f");
    facts_add_spo(g_f, "e", "f", "g");
    facts_add_spo(g_f, "f", "g", "h");
    facts_add_spo(g_f, "g", "h", "i");
    facts_add_spo(g_f, "h", "i", "j");
    facts_add_spo(g_f, "i", "j", "k");
    facts_add_spo(g_f, "j", "k", "l");
}

void teardown_remove(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_remove_one)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove(g_f, (const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(!facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
    ck_assert(!facts_remove(g_f, (const char *[]){"a", "b", "c", NULL, NULL}));
    ck_assert(facts_count(g_f) == 9);
    ck_assert(facts_remove(g_f, (const char *[]){"?b", "c", "?d", NULL, NULL}));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_remove(g_f, (const char *[]){"?b", "c", "?d", NULL, NULL}));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_remove(g_f, (const char *[]){"?s", "?p", "?o", NULL, NULL}));
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!facts_remove(g_f, (const char *[]){"?s", "?p", "?o", NULL, NULL}));
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_facts_remove_two)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove(g_f, (const char *[]){"a", "b", "c", NULL, "b", "c", "d", NULL, NULL}));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(!facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
    ck_assert(!facts_remove(g_f, (const char *[]){"a", "b", "c", NULL, "b", "c", "d", NULL, NULL}));
    ck_assert(facts_count(g_f) == 8);
    ck_assert(facts_remove(g_f, (const char *[]){"c", "?d", "?e", NULL, "?f", "?g", "f", NULL, NULL}));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(!facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(!facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(!facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
    ck_assert(!facts_remove(g_f, (const char *[]){"c", "?d", "?e", NULL, "?f", "?g", "f", NULL, NULL}));
    ck_assert(facts_count(g_f) == 6);
    ck_assert(facts_remove(g_f, (const char *[]){"e", "?f", "?g", NULL, "?f", "?g", "?h", NULL, NULL}));
    ck_assert(facts_count(g_f) == 4);
    ck_assert(!facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(!facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(!facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(!facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(!facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
    ck_assert(facts_count(g_f) == 4);
}
END_TEST

START_TEST(test_facts_remove_ten)
{
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_remove(g_f, (const char *[]){"a", "b",  "c", NULL, "b", "c",  "d", NULL, "c", "d",  "e", NULL, "d", "e",
                                                 "f", NULL, "e", "f",  "g", NULL, "f", "g",  "h", NULL, "g", "h",  "i", NULL,
                                                 "h", "i",  "j", NULL, "i", "j",  "k", NULL, "j", "k",  "l", NULL, NULL}));
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

void setup_with_spo(void)
{
    g_f = new_facts(NULL, 100);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "a", "b", "d");
    facts_add_spo(g_f, "a", "e", "d");
    facts_add_spo(g_f, "g", "b", "c");
    facts_add_spo(g_f, "h", "i", "c");
}

void teardown_with_spo(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

int fact_equal(s_fact *f, const char *s, const char *p, const char *o)
{
    return (!strcmp(symbol_to_str(f->s), s) && !strcmp(symbol_to_str(f->p), p) && !strcmp(symbol_to_str(f->o), o));
}

START_TEST(test_facts_with_spo_0)
{
    s_fact f;
    const char *s;
    const char *p;
    const char *o;
    s_binding bindings[] = {{"?s", &s}, {"?p", &p}, {"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "?s", "?p", "?o");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(fact_equal(&f, s, p, o));
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(fact_equal(&f, s, p, o));
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(fact_equal(&f, s, p, o));
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(fact_equal(&f, s, p, o));
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(fact_equal(&f, s, p, o));
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_3)
{
    s_fact f;
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, NULL, &c, "a", "a", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "b", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "b", "c");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "b", "d");
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "b", "e");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "e", "d");
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "a", "e", "g");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "g", "b", "c");
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "h", "i", "c");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, NULL, &c, "i", "j", "k");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_s)
{
    s_fact f;
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "?s", "a", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "b", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "b", "c");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "b", "d");
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "b", "e");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "i", "b");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "i", "c");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "i", "d");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "j", "k");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_p)
{
    s_fact f;
    const char *p;
    s_binding bindings[] = {{"?p", &p}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "a", "?p", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "?p", "b");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "?p", "c");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "?p", "d");
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "?p", "e");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "g", "?p", "c");
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "?p", "b");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "?p", "c");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "?p", "d");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "i", "?p", "a");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_o)
{
    s_fact f;
    const char *o;
    s_binding bindings[] = {{"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "a", "a", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "b", "?o");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "c", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "e", "?o");
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "f", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "b", "c", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "g", "a", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "g", "b", "?o");
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "g", "c", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "a", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "i", "?o");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "i", "j", "?o");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_sp)
{
    s_fact f;
    const char *s;
    const char *p;
    s_binding bindings[] = {{"?s", &s}, {"?p", &p}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "?s", "?p", "a");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "?p", "c");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "?p", "d");
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "?p", "e");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_po)
{
    s_fact f;
    const char *p;
    const char *o;
    s_binding bindings[] = {{"?p", &p}, {"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "0", "?p", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "a", "?p", "?o");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "b", "?p", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "g", "?p", "?o");
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "h", "?p", "?o");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.p), p));
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "i", "?p", "?o");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

START_TEST(test_facts_with_spo_os)
{
    s_fact f;
    const char *o;
    const char *s;
    s_binding bindings[] = {{"?o", &o}, {"?s", &s}, {NULL, NULL}};
    s_facts_cursor c;
    ck_assert(facts_count(g_f) == 5);
    facts_with_spo(g_f, bindings, &c, "?s", "a", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "b", "?o");
    fact_init(&f, "a", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    fact_init(&f, "g", "b", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    fact_init(&f, "a", "b", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "c", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "e", "?o");
    fact_init(&f, "a", "e", "d");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "f", "?o");
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "i", "?o");
    fact_init(&f, "h", "i", "c");
    ck_assert(fact_compare_spo(&f, facts_cursor_next(&c)) == 0);
    ck_assert(!strcmp(symbol_to_str(f.o), o));
    ck_assert(!strcmp(symbol_to_str(f.s), s));
    ck_assert(!facts_cursor_next(&c));
    facts_with_spo(g_f, bindings, &c, "?s", "j", "?o");
    ck_assert(!facts_cursor_next(&c));
}
END_TEST

void setup_write(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_write(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_write_facts_empty)
{
    FILE *fp = fopen("/tmp/test_write_facts_empty", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!write_facts(g_f, fp));
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_one)
{
    FILE *fp = fopen("/tmp/test_write_facts_one", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(!write_facts(g_f, fp));
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_two)
{
    FILE *fp = fopen("/tmp/test_write_facts_two", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(!write_facts(g_f, fp));
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_ten)
{
    FILE *fp = fopen("/tmp/test_write_facts_ten", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_add_spo(g_f, "c", "d", "e"));
    ck_assert(facts_add_spo(g_f, "d", "e", "f"));
    ck_assert(facts_add_spo(g_f, "e", "f", "g"));
    ck_assert(facts_add_spo(g_f, "f", "g", "h"));
    ck_assert(facts_add_spo(g_f, "g", "h", "i"));
    ck_assert(facts_add_spo(g_f, "h", "i", "j"));
    ck_assert(facts_add_spo(g_f, "i", "j", "k"));
    ck_assert(facts_add_spo(g_f, "j", "k", "l"));
    ck_assert(!write_facts(g_f, fp));
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_escapes)
{
    FILE *fp = fopen("/tmp/test_write_facts_escapes", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(facts_add_spo(g_f, "\\", "\"", "\n"));
    ck_assert(facts_add_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(facts_add_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(facts_add_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(facts_add_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(facts_add_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(facts_add_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
    ck_assert(facts_count(g_f) == 7);
    ck_assert(!write_facts(g_f, fp));
    fclose(fp);
}
END_TEST

void setup_read(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_read(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_read_facts_empty)
{
    FILE *fp = fopen("test_facts_empty", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_one)
{
    FILE *fp = fopen("test_facts_one", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 1);
    ck_assert(facts_get_spo(g_f, "a", "b", "c"));
}
END_TEST

START_TEST(test_read_facts_two)
{
    FILE *fp = fopen("test_facts_two", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 2);
    ck_assert(facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(facts_get_spo(g_f, "b", "c", "d"));
}
END_TEST

START_TEST(test_read_facts_ten)
{
    FILE *fp = fopen("test_facts_ten", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 10);
    ck_assert(facts_get_spo(g_f, "a", "b", "c"));
    ck_assert(facts_get_spo(g_f, "b", "c", "d"));
    ck_assert(facts_get_spo(g_f, "c", "d", "e"));
    ck_assert(facts_get_spo(g_f, "d", "e", "f"));
    ck_assert(facts_get_spo(g_f, "e", "f", "g"));
    ck_assert(facts_get_spo(g_f, "f", "g", "h"));
    ck_assert(facts_get_spo(g_f, "g", "h", "i"));
    ck_assert(facts_get_spo(g_f, "h", "i", "j"));
    ck_assert(facts_get_spo(g_f, "i", "j", "k"));
    ck_assert(facts_get_spo(g_f, "j", "k", "l"));
}
END_TEST

START_TEST(test_read_facts_escapes)
{
    FILE *fp = fopen("test_facts_escapes", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 7);
    ck_assert(facts_get_spo(g_f, "\\", "\"", "\n"));
    ck_assert(facts_get_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(facts_get_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(facts_get_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(facts_get_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(facts_get_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(facts_get_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
}
END_TEST

void setup_write_facts_log(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_write_facts_log(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_write_facts_log_one)
{
    FILE *fp = fopen("/tmp/test_write_facts_log_one", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    g_f->log = fp;
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_count(g_f) == 0);
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_log_two)
{
    FILE *fp = fopen("/tmp/test_write_facts_log_two", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    g_f->log = fp;
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(!facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(facts_count(g_f) == 0);
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_log_ten)
{
    FILE *fp = fopen("/tmp/test_write_facts_log_ten", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    g_f->log = fp;
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_add_spo(g_f, "c", "d", "e"));
    ck_assert(facts_add_spo(g_f, "d", "e", "f"));
    ck_assert(facts_add_spo(g_f, "e", "f", "g"));
    ck_assert(facts_add_spo(g_f, "f", "g", "h"));
    ck_assert(facts_add_spo(g_f, "g", "h", "i"));
    ck_assert(facts_add_spo(g_f, "h", "i", "j"));
    ck_assert(facts_add_spo(g_f, "i", "j", "k"));
    ck_assert(facts_add_spo(g_f, "j", "k", "l"));
    ck_assert(facts_add_spo(g_f, "a", "b", "c"));
    ck_assert(facts_add_spo(g_f, "b", "c", "d"));
    ck_assert(facts_add_spo(g_f, "c", "d", "e"));
    ck_assert(facts_add_spo(g_f, "d", "e", "f"));
    ck_assert(facts_add_spo(g_f, "e", "f", "g"));
    ck_assert(facts_add_spo(g_f, "f", "g", "h"));
    ck_assert(facts_add_spo(g_f, "g", "h", "i"));
    ck_assert(facts_add_spo(g_f, "h", "i", "j"));
    ck_assert(facts_add_spo(g_f, "i", "j", "k"));
    ck_assert(facts_add_spo(g_f, "j", "k", "l"));
    ck_assert(facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(facts_remove_spo(g_f, "c", "d", "e"));
    ck_assert(facts_remove_spo(g_f, "d", "e", "f"));
    ck_assert(facts_remove_spo(g_f, "e", "f", "g"));
    ck_assert(facts_remove_spo(g_f, "f", "g", "h"));
    ck_assert(facts_remove_spo(g_f, "g", "h", "i"));
    ck_assert(facts_remove_spo(g_f, "h", "i", "j"));
    ck_assert(facts_remove_spo(g_f, "i", "j", "k"));
    ck_assert(facts_remove_spo(g_f, "j", "k", "l"));
    ck_assert(!facts_remove_spo(g_f, "a", "b", "c"));
    ck_assert(!facts_remove_spo(g_f, "b", "c", "d"));
    ck_assert(!facts_remove_spo(g_f, "c", "d", "e"));
    ck_assert(!facts_remove_spo(g_f, "d", "e", "f"));
    ck_assert(!facts_remove_spo(g_f, "e", "f", "g"));
    ck_assert(!facts_remove_spo(g_f, "f", "g", "h"));
    ck_assert(!facts_remove_spo(g_f, "g", "h", "i"));
    ck_assert(!facts_remove_spo(g_f, "h", "i", "j"));
    ck_assert(!facts_remove_spo(g_f, "i", "j", "k"));
    ck_assert(!facts_remove_spo(g_f, "j", "k", "l"));
    ck_assert(facts_count(g_f) == 0);
    fclose(fp);
}
END_TEST

START_TEST(test_write_facts_log_escapes)
{
    FILE *fp = fopen("/tmp/test_write_facts_log_escapes", "w");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    g_f->log = fp;
    ck_assert(facts_add_spo(g_f, "\\", "\"", "\n"));
    ck_assert(facts_add_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(facts_add_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(facts_add_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(facts_add_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(facts_add_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(facts_add_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
    ck_assert(facts_add_spo(g_f, "\\", "\"", "\n"));
    ck_assert(facts_add_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(facts_add_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(facts_add_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(facts_add_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(facts_add_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(facts_add_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
    ck_assert(facts_remove_spo(g_f, "\\", "\"", "\n"));
    ck_assert(facts_remove_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(facts_remove_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(facts_remove_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(facts_remove_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(facts_remove_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(facts_remove_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
    ck_assert(!facts_remove_spo(g_f, "\\", "\"", "\n"));
    ck_assert(!facts_remove_spo(g_f, "a\\", "a\\a", "a\\\\a"));
    ck_assert(!facts_remove_spo(g_f, "a\"", "a\"a", "a\"\"a"));
    ck_assert(!facts_remove_spo(g_f, "a\n", "a\na", "a\n\na"));
    ck_assert(!facts_remove_spo(g_f, "\\\"\n", "\\\"\n\\\"\n", "\\\"\n\\\"\n\\\"\n"));
    ck_assert(!facts_remove_spo(g_f, "\\a", "\\b", "\\c"));
    ck_assert(!facts_remove_spo(g_f, "a\\a", "a\\aa", "a\\a\\aa"));
    ck_assert(facts_count(g_f) == 0);
    fclose(fp);
}
END_TEST

void setup_read_facts_log(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_read_facts_log(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_read_facts_log_empty)
{
    FILE *fp = fopen("test_facts_log_empty", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts_log(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_log_one)
{
    FILE *fp = fopen("test_facts_log_one", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts_log(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_log_two)
{
    FILE *fp = fopen("test_facts_log_two", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts_log(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_log_ten)
{
    FILE *fp = fopen("test_facts_log_ten", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts_log(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_log_escapes)
{
    FILE *fp = fopen("test_facts_log_escapes", "r");
    ck_assert(fp);
    ck_assert(facts_count(g_f) == 0);
    ck_assert(!read_facts_log(g_f, fp));
    fclose(fp);
    ck_assert(facts_count(g_f) == 0);
}
END_TEST

START_TEST(test_read_facts_log_malformed)
{
    FILE *fp = fopen("test_facts_log_malformed", "w");
    ck_assert(fp);
    fprintf(fp, "this_is_a_very_long_operation_name_without_newline");
    fclose(fp);
    fp = fopen("test_facts_log_malformed", "r");
    ck_assert(fp);
    ck_assert(read_facts_log(g_f, fp) == -1);
    fclose(fp);
    remove("test_facts_log_malformed");
}
END_TEST

void setup_anon(void)
{
    g_f = new_facts(NULL, 10);
}

void teardown_anon(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_anon)
{
    const char *anon;
    ck_assert((anon = facts_anon(g_f, NULL)));
    ck_assert(!strncmp(anon, "anon-", 5));
    ck_assert(strlen(anon) == 15);
    ck_assert((anon = facts_anon(g_f, "")));
    ck_assert(!strncmp(anon, "anon-", 5));
    ck_assert(strlen(anon) == 15);
    ck_assert((anon = facts_anon(g_f, "?")));
    ck_assert(!strncmp(anon, "anon-", 5));
    ck_assert(strlen(anon) == 15);
    ck_assert((anon = facts_anon(g_f, "a")));
    ck_assert(!strncmp(anon, "a-", 2));
    ck_assert(strlen(anon) == 12);
    ck_assert((anon = facts_anon(g_f, "?a")));
    ck_assert(!strncmp(anon, "a-", 2));
    ck_assert(strlen(anon) == 12);
}
END_TEST

void setup_with(void)
{
    g_f = new_facts(NULL, 10);
    facts_add_spo(g_f, "a", "b", "c");
    facts_add_spo(g_f, "a", "b", "d");
    facts_add_spo(g_f, "a", "e", "d");
    facts_add_spo(g_f, "g", "b", "c");
    facts_add_spo(g_f, "h", "i", "c");
}

void teardown_with(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_with_empty)
{
    s_facts *facts = new_facts(NULL, 10);
    s_facts_with_cursor c;
    ck_assert(facts);
    int rc;

    rc = facts_sparql(facts, NULL, &c, "SELECT ?dummy WHERE { }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(facts, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . }");
    ck_assert_int_eq(0, rc);
    printf("a\n");
    ck_assert(!facts_with_cursor_next(&c));
    printf("b\n");
    facts_with_cursor_destroy(&c);
    printf("c\n");

    rc = facts_sparql(facts, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . <b> <c> <d> . }");
    ck_assert_int_eq(0, rc);
    printf("d\n");
    ck_assert(!facts_with_cursor_next(&c));
    printf("e\n");
    facts_with_cursor_destroy(&c);
    printf("f\n");

    rc = facts_sparql(facts, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . <a> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
    delete_facts(facts);

    rc = facts_sparql(g_f, NULL, &c, "SELECT ?dummy WHERE { }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . <b> <c> <d> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);

    rc = facts_sparql(g_f, NULL, &c, "SELECT ?dummy WHERE { <a> <a> <a> . <a> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(!facts_with_cursor_next(&c));
    facts_with_cursor_destroy(&c);
}
END_TEST

START_TEST(test_facts_with_zero)
{
    s_facts_with_cursor cur;
    int rc;

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . <a> <b> <d> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <e> <d> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . <g> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, NULL, &cur,
                      "SELECT ?dummy WHERE { <a> <b> <c> . <a> <b> <d> . <a> <e> <d> . <g> <b> <c> . <h> <i> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

START_TEST(test_facts_with_one)
{
    const char *a;
    const char *b;
    const char *c;
    const char *d;
    s_binding bindings[] = {{"?a", &a}, {"?b", &b}, {"?c", &c}, {"?d", &d}, {NULL, NULL}};
    s_facts_with_cursor cur;
    int rc;

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?a WHERE { ?a <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "g"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?b WHERE { <a> ?b <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?c WHERE { <a> <b> ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(c, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(c, "d"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

START_TEST(test_facts_with_two)
{
    const char *a;
    const char *b;
    const char *c;
    const char *d;
    s_binding bindings[] = {{"?a", &a}, {"?b", &b}, {"?c", &c}, {"?d", &d}, {NULL, NULL}};
    s_facts *facts = new_facts(NULL, 10);
    s_facts_with_cursor cur;
    int rc;
    ck_assert(facts);
    ck_assert(facts_count(facts) == 0);
    ck_assert(facts_add_spo(facts, "a", "b", "c"));
    ck_assert(facts_count(facts) == 1);

    rc = facts_sparql(facts, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(facts, bindings, &cur, "SELECT ?a ?b ?c WHERE { ?a ?b ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(facts, bindings, &cur, "SELECT ?a WHERE { ?a <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(facts, bindings, &cur, "SELECT ?b WHERE { <a> ?b <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(facts, bindings, &cur, "SELECT ?c WHERE { <a> <b> ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
    delete_facts(facts);

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, NULL, &cur, "SELECT ?dummy WHERE { <a> <b> <c> . <a> <b> <d> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?b ?c WHERE { <a> <b> <c> . <g> ?b ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

START_TEST(test_facts_with_three)
{
    const char *a;
    const char *b;
    const char *c;
    const char *d;
    const char *e;
    const char *f;
    s_binding bindings[] = {{"?a", &a}, {"?b", &b}, {"?c", &c}, {"?d", &d}, {"?e", &e}, {"?f", &f}, {NULL, NULL}};
    s_facts_with_cursor cur;
    int rc;

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?a ?b ?c WHERE { ?a ?b ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "e"));
    ck_assert(!strcmp(c, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "g"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "h"));
    ck_assert(!strcmp(b, "i"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?a ?b ?c ?d ?e ?f WHERE { ?a ?b ?c . ?d ?e ?f . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "e"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "g"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "h"));
    ck_assert(!strcmp(e, "i"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "d"));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "e"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "g"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "h"));
    ck_assert(!strcmp(e, "i"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "a"));
    ck_assert(!strcmp(b, "e"));
    ck_assert(!strcmp(c, "d"));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "e"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "g"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "h"));
    ck_assert(!strcmp(e, "i"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "g"));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "e"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "g"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "h"));
    ck_assert(!strcmp(e, "i"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(a, "h"));
    ck_assert(!strcmp(b, "i"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "a"));
    ck_assert(!strcmp(e, "e"));
    ck_assert(!strcmp(f, "d"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "g"));
    ck_assert(!strcmp(e, "b"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(d, "h"));
    ck_assert(!strcmp(e, "i"));
    ck_assert(!strcmp(f, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

START_TEST(test_facts_with_bindings)
{
    const char *a;
    const char *b;
    const char *c;
    const char *d;
    s_binding bindings[] = {{"?a", &a}, {"?b", &b}, {"?c", &c}, {"?d", &d}, {NULL, NULL}};
    s_facts_with_cursor cur;
    int rc;

    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?b ?c WHERE { <a> ?b ?c . <g> ?b ?c . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert(!strcmp(b, "b"));
    ck_assert(!strcmp(c, "c"));
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

START_TEST(test_facts_with_negation)
{
    const char *s;
    s_binding bindings[] = {{"?s", &s}, {NULL, NULL}};
    s_facts_with_cursor cur;
    int rc;

    // Query 1: ?s has property (b, c) but NOT property (e, d) -> should be only 'g'
    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?s WHERE { ?s <b> <c> . NOT ?s <e> <d> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert_str_eq("g", s);
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);

    // Query 2: ?s has property (b, c) but NOT property (e, x) -> should return both 'a' and 'g'
    rc = facts_sparql(g_f, bindings, &cur, "SELECT ?s WHERE { ?s <b> <c> . NOT ?s <e> <x> . }");
    ck_assert_int_eq(0, rc);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert_str_eq("a", s);
    ck_assert(facts_with_cursor_next(&cur));
    ck_assert_str_eq("g", s);
    ck_assert(!facts_with_cursor_next(&cur));
    facts_with_cursor_destroy(&cur);
}
END_TEST

void setup_prop(void)
{
    g_f = new_facts(NULL, 100);
}

void teardown_prop(void)
{
    delete_facts(g_f);
    g_f = NULL;
}

START_TEST(test_facts_helpers)
{
    // Test long conversion
    const char *l_str = facts_long(g_f, 123456789L);
    ck_assert(l_str);
    ck_assert_int_eq(123456789L, facts_get_long(g_f, l_str));

    // Test double conversion
    const char *d_str = facts_double(g_f, 3.14159265);
    ck_assert(d_str);
    double got_d = facts_get_double(g_f, d_str);
    ck_assert(got_d > 3.14 && got_d < 3.15);
}
END_TEST

START_TEST(test_facts_properties)
{
    // Test setting and getting string property
    ck_assert(facts_set_prop(g_f, "s1", "name", "John"));
    ck_assert_str_eq("John", facts_get_prop(g_f, "s1", "name"));

    // Test updating string property
    ck_assert(facts_set_prop(g_f, "s1", "name", "Joe"));
    ck_assert_str_eq("Joe", facts_get_prop(g_f, "s1", "name"));

    // Test long property
    const char *age_str = facts_long(g_f, 30);
    ck_assert(facts_set_prop(g_f, "s1", "age", age_str));
    ck_assert_int_eq(30, facts_get_prop_long(g_f, "s1", "age"));

    // Test double property
    const char *pi_str = facts_double(g_f, 3.14159);
    ck_assert(facts_set_prop(g_f, "s1", "pi", pi_str));
    double got_pi = facts_get_prop_double(g_f, "s1", "pi");
    ck_assert(got_pi > 3.14 && got_pi < 3.15);

    // Test getting non-existent property
    ck_assert(!facts_get_prop(g_f, "s1", "non_existent"));
    ck_assert_int_eq(0, facts_get_prop_long(g_f, "s1", "non_existent"));
    ck_assert(facts_get_prop_double(g_f, "s1", "non_existent") == 0.0);
}
END_TEST

START_TEST(test_facts_transaction)
{
    // 1. Test basic commit:
    facts_transaction_begin(g_f);
    ck_assert(facts_add_spo(g_f, "tx", "status", "active"));
    ck_assert_str_eq("active", facts_get_prop(g_f, "tx", "status"));
    facts_transaction_commit(g_f);
    // Verifying it is still there after commit:
    ck_assert_str_eq("active", facts_get_prop(g_f, "tx", "status"));

    // 2. Test basic rollback:
    facts_transaction_begin(g_f);
    ck_assert(facts_set_prop(g_f, "tx", "status", "mutated"));
    ck_assert_str_eq("mutated", facts_get_prop(g_f, "tx", "status"));
    facts_transaction_rollback(g_f);
    // Verifying it rolled back to "active":
    ck_assert_str_eq("active", facts_get_prop(g_f, "tx", "status"));

    // 3. Test rollback of deletion:
    facts_transaction_begin(g_f);
    ck_assert(facts_remove_spo(g_f, "tx", "status", "active"));
    ck_assert(!facts_get_prop(g_f, "tx", "status"));
    facts_transaction_rollback(g_f);
    // Verifying the deletion was rolled back (added back):
    ck_assert_str_eq("active", facts_get_prop(g_f, "tx", "status"));

    // 4. Test nested transactions:
    facts_transaction_begin(g_f);
    ck_assert(facts_add_spo(g_f, "tx", "step", "1"));

    facts_transaction_begin(g_f);
    ck_assert(facts_add_spo(g_f, "tx", "step", "2"));

    // Commit the inner one:
    facts_transaction_commit(g_f);

    // Rollback the outer one:
    facts_transaction_rollback(g_f);

    // Both step 1 and step 2 should be rolled back!
    ck_assert(!facts_get_prop(g_f, "tx", "step"));
}
END_TEST

static int g_listener_called = 0;
static size_t g_listener_entry_count = 0;

static int my_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    (void)facts;
    (void)entries;
    int *called = (int *)user_data;
    *called = 1;
    g_listener_entry_count = entry_count;
    return 0;
}

static int failing_tx_listener(s_facts *facts, const s_rollback_entry *entries, size_t entry_count, void *user_data)
{
    (void)entries;
    (void)entry_count;
    (void)user_data;
    /* Prove that listener-side mutations are part of the same rollback unit. */
    if (!facts_add_spo_origin(facts, "partial", "derived", "result", FACT_ORIGIN_DERIVED))
        return -1;
    return -1;
}

START_TEST(test_failing_tx_listener_rolls_back_base_and_derived_changes)
{
    facts_register_tx_listener(g_f, failing_tx_listener, NULL);
    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_add_spo(g_f, "base", "change", "pending"));
    ck_assert_int_eq(facts_transaction_commit(g_f), -1);
    ck_assert_int_eq(facts_contains_spo(g_f, "base", "change", "pending"), 0);
    ck_assert_int_eq(facts_contains_spo(g_f, "partial", "derived", "result"), 0);
}
END_TEST

START_TEST(test_facts_support_origin_and_rollback)
{
    s_fact_support support;
    ck_assert(facts_add_spo(g_f, "supported", "by", "both"));
    ck_assert_int_eq(facts_get_support_spo(g_f, "supported", "by", "both", &support), 1);
    ck_assert(support.asserted == 1);
    ck_assert(support.derived == 0);

    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_add_spo_origin(g_f, "supported", "by", "both", FACT_ORIGIN_DERIVED));
    ck_assert_int_eq(facts_transaction_rollback(g_f), 0);
    ck_assert_int_eq(facts_get_support_spo(g_f, "supported", "by", "both", &support), 1);
    ck_assert(support.asserted == 1);
    ck_assert(support.derived == 0);

    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_add_spo_origin(g_f, "supported", "by", "both", FACT_ORIGIN_DERIVED));
    ck_assert_int_eq(facts_transaction_commit(g_f), 0);
    ck_assert_int_eq(facts_get_support_spo(g_f, "supported", "by", "both", &support), 1);
    ck_assert(support.asserted == 1);
    ck_assert(support.derived == 1);

    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_remove_spo(g_f, "supported", "by", "both"));
    ck_assert_int_eq(facts_transaction_rollback(g_f), 0);
    ck_assert_int_eq(facts_get_support_spo(g_f, "supported", "by", "both", &support), 1);
    ck_assert(support.asserted == 1);
    ck_assert(support.derived == 1);

    ck_assert(facts_remove_spo(g_f, "supported", "by", "both"));
    ck_assert(facts_get_spo(g_f, "supported", "by", "both"));
    ck_assert_int_eq(facts_get_support_spo(g_f, "supported", "by", "both", &support), 1);
    ck_assert(support.asserted == 0);
    ck_assert(support.derived == 1);
    ck_assert(facts_remove_spo_origin(g_f, "supported", "by", "both", FACT_ORIGIN_DERIVED));
    ck_assert(!facts_get_spo(g_f, "supported", "by", "both"));
}
END_TEST

START_TEST(test_facts_log_tracks_asserted_support_only)
{
    FILE *log = tmpfile();
    ck_assert(log != NULL);
    g_f->log = log;
    ck_assert_int_eq(facts_transaction_begin(g_f), 0);
    ck_assert(facts_add_spo(g_f, "logged", "support", "fact"));
    ck_assert(facts_add_spo_origin(g_f, "logged", "support", "fact", FACT_ORIGIN_DERIVED));
    ck_assert_int_eq(facts_transaction_rollback(g_f), 0);
    ck_assert_int_eq(fflush(log), 0);
    rewind(log);
    char line[1024];
    int operations = 0;
    while (fgets(line, sizeof(line), log)) {
        if (strcmp(line, "add\n") == 0 || strcmp(line, "remove\n") == 0)
            operations++;
    }
    ck_assert_int_eq(operations, 2); /* asserted add plus its rollback compensation */
    g_f->log = NULL;
    fclose(log);
}
END_TEST

START_TEST(test_facts_tx_listener_and_entity_builder)
{
    // Test Transaction Listener:
    g_listener_called = 0;
    g_listener_entry_count = 0;
    facts_register_tx_listener(g_f, my_tx_listener, &g_listener_called);

    facts_transaction_begin(g_f);
    facts_add_spo(g_f, "tx", "status", "active");
    facts_transaction_commit(g_f);

    ck_assert_int_eq(g_listener_called, 1);
    ck_assert_int_eq(g_listener_entry_count, 1);

    // Test Entity Builder:
    s_entity *ent = facts_entity_begin(g_f, "movie:1");
    ck_assert(ent);
    ck_assert_int_eq(facts_entity_add(ent, "title", "Inception"), 0);
    ck_assert_int_eq(facts_entity_add_long(ent, "year", 2010), 0);
    ck_assert_int_eq(facts_entity_add_double(ent, "rating", 8.8), 0);

    g_listener_called = 0;
    g_listener_entry_count = 0;
    ck_assert_int_eq(facts_entity_commit(ent), 0);

    // Verifying properties are set:
    ck_assert_str_eq(facts_get_prop(g_f, "movie:1", "title"), "Inception");
    ck_assert_int_eq(facts_get_prop_long(g_f, "movie:1", "year"), 2010);
    ck_assert_double_eq_tol(facts_get_prop_double(g_f, "movie:1", "rating"), 8.8, 1e-9);

    // Listener should have been called with 3 entries:
    ck_assert_int_eq(g_listener_called, 1);
    ck_assert_int_eq(g_listener_entry_count, 3);
}
END_TEST

Suite *facts_suite(void)
{
    Suite *s;
    TCase *tc_init;
    TCase *tc_add_fact;
    TCase *tc_add_spo;
    TCase *tc_add;
    TCase *tc_remove_fact;
    TCase *tc_remove_spo;
    TCase *tc_remove;
    TCase *tc_with_spo;
    TCase *tc_write;
    TCase *tc_read;
    TCase *tc_write_log;
    TCase *tc_read_log;
    TCase *tc_anon;
    TCase *tc_with;
    TCase *tc_prop;
    s = suite_create("Facts");
    tc_init = tcase_create("Init");
    tcase_add_test(tc_init, test_facts_init_destroy);
    tcase_add_test(tc_init, test_shared_symbol_references_are_released);
    tcase_add_test(tc_init, test_facts_new_delete);
    tcase_add_test(tc_init, test_facts_reset);
    tcase_add_test(tc_init, test_facts_reset_rejects_active_transaction);
    suite_add_tcase(s, tc_init);
    tc_add_fact = tcase_create("Add fact");
    tcase_add_checked_fixture(tc_add_fact, setup_add_fact, teardown_add_fact);
    tcase_add_test(tc_add_fact, test_facts_add_fact_one);
    tcase_add_test(tc_add_fact, test_facts_add_fact_two);
    tcase_add_test(tc_add_fact, test_facts_add_fact_ten);
    suite_add_tcase(s, tc_add_fact);
    tc_add_spo = tcase_create("Add SPO");
    tcase_add_checked_fixture(tc_add_spo, setup_add_spo, teardown_add_spo);
    tcase_add_test(tc_add_spo, test_facts_add_spo_one);
    tcase_add_test(tc_add_spo, test_facts_add_spo_two);
    tcase_add_test(tc_add_spo, test_facts_add_spo_ten);
    suite_add_tcase(s, tc_add_spo);
    tc_add = tcase_create("Add");
    tcase_add_checked_fixture(tc_add, setup_add, teardown_add);
    tcase_add_test(tc_add, test_facts_add_one);
    tcase_add_test(tc_add, test_facts_add_two);
    tcase_add_test(tc_add, test_facts_add_ten);
    tcase_add_test(tc_add, test_facts_add_anon);
    suite_add_tcase(s, tc_add);
    tc_remove_fact = tcase_create("Remove fact");
    tcase_add_checked_fixture(tc_remove_fact, setup_remove_fact, teardown_remove_fact);
    tcase_add_test(tc_remove_fact, test_facts_remove_fact_one);
    tcase_add_test(tc_remove_fact, test_facts_remove_fact_two);
    tcase_add_test(tc_remove_fact, test_facts_remove_fact_ten);
    suite_add_tcase(s, tc_remove_fact);
    tc_remove_spo = tcase_create("Remove SPO");
    tcase_add_checked_fixture(tc_remove_spo, setup_remove_spo, teardown_remove_spo);
    tcase_add_test(tc_remove_spo, test_facts_remove_spo_one);
    tcase_add_test(tc_remove_spo, test_facts_remove_spo_two);
    tcase_add_test(tc_remove_spo, test_facts_remove_spo_ten);
    suite_add_tcase(s, tc_remove_spo);
    tc_remove = tcase_create("Remove");
    tcase_add_checked_fixture(tc_remove, setup_remove, teardown_remove);
    tcase_add_test(tc_remove, test_facts_remove_one);
    tcase_add_test(tc_remove, test_facts_remove_two);
    tcase_add_test(tc_remove, test_facts_remove_ten);
    suite_add_tcase(s, tc_remove);
    tc_with_spo = tcase_create("With SPO");
    tcase_add_checked_fixture(tc_with_spo, setup_with_spo, teardown_with_spo);
    tcase_add_test(tc_with_spo, test_facts_with_spo_0);
    tcase_add_test(tc_with_spo, test_facts_with_spo_3);
    tcase_add_test(tc_with_spo, test_facts_with_spo_s);
    tcase_add_test(tc_with_spo, test_facts_with_spo_p);
    tcase_add_test(tc_with_spo, test_facts_with_spo_o);
    tcase_add_test(tc_with_spo, test_facts_with_spo_sp);
    tcase_add_test(tc_with_spo, test_facts_with_spo_po);
    tcase_add_test(tc_with_spo, test_facts_with_spo_os);
    suite_add_tcase(s, tc_with_spo);
    tc_write = tcase_create("Write");
    tcase_add_checked_fixture(tc_write, setup_write, teardown_write);
    tcase_add_test(tc_write, test_write_facts_empty);
    tcase_add_test(tc_write, test_write_facts_one);
    tcase_add_test(tc_write, test_write_facts_two);
    tcase_add_test(tc_write, test_write_facts_ten);
    tcase_add_test(tc_write, test_write_facts_escapes);
    suite_add_tcase(s, tc_write);
    tc_read = tcase_create("Read");
    tcase_add_checked_fixture(tc_read, setup_read, teardown_read);
    tcase_add_test(tc_read, test_read_facts_empty);
    tcase_add_test(tc_read, test_read_facts_one);
    tcase_add_test(tc_read, test_read_facts_two);
    tcase_add_test(tc_read, test_read_facts_ten);
    tcase_add_test(tc_read, test_read_facts_escapes);
    suite_add_tcase(s, tc_read);
    tc_write_log = tcase_create("Write log");
    tcase_add_checked_fixture(tc_write_log, setup_write_facts_log, teardown_write_facts_log);
    tcase_add_test(tc_write_log, test_write_facts_log_one);
    tcase_add_test(tc_write_log, test_write_facts_log_two);
    tcase_add_test(tc_write_log, test_write_facts_log_ten);
    tcase_add_test(tc_write_log, test_write_facts_log_escapes);
    suite_add_tcase(s, tc_write_log);
    tc_read_log = tcase_create("Read log");
    tcase_add_checked_fixture(tc_read_log, setup_read_facts_log, teardown_read_facts_log);
    tcase_add_test(tc_read_log, test_read_facts_log_empty);
    tcase_add_test(tc_read_log, test_read_facts_log_one);
    tcase_add_test(tc_read_log, test_read_facts_log_two);
    tcase_add_test(tc_read_log, test_read_facts_log_ten);
    tcase_add_test(tc_read_log, test_read_facts_log_escapes);
    tcase_add_test(tc_read_log, test_read_facts_log_malformed);
    suite_add_tcase(s, tc_read_log);
    tc_anon = tcase_create("Anon");
    tcase_add_checked_fixture(tc_anon, setup_anon, teardown_anon);
    tcase_add_test(tc_anon, test_facts_anon);
    suite_add_tcase(s, tc_anon);
    tc_with = tcase_create("With");
    tcase_add_checked_fixture(tc_with, setup_with, teardown_with);
    tcase_add_test(tc_with, test_facts_with_empty);
    tcase_add_test(tc_with, test_facts_with_zero);
    tcase_add_test(tc_with, test_facts_with_one);
    tcase_add_test(tc_with, test_facts_with_two);
    tcase_add_test(tc_with, test_facts_with_three);
    tcase_add_test(tc_with, test_facts_with_bindings);
    tcase_add_test(tc_with, test_facts_with_negation);
    suite_add_tcase(s, tc_with);
    tc_prop = tcase_create("Properties");
    tcase_add_checked_fixture(tc_prop, setup_prop, teardown_prop);
    tcase_add_test(tc_prop, test_facts_helpers);
    tcase_add_test(tc_prop, test_facts_properties);
    tcase_add_test(tc_prop, test_facts_transaction);
    tcase_add_test(tc_prop, test_transaction_rejects_foreign_commit);
    tcase_add_test(tc_prop, test_safe_read_apis);
    tcase_add_test(tc_prop, test_commit_summary_partitions);
    tcase_add_test(tc_prop, test_facts_support_origin_and_rollback);
    tcase_add_test(tc_prop, test_facts_log_tracks_asserted_support_only);
    tcase_add_test(tc_prop, test_facts_tx_listener_and_entity_builder);
    tcase_add_test(tc_prop, test_failing_tx_listener_rolls_back_base_and_derived_changes);
    suite_add_tcase(s, tc_prop);
    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = facts_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return (number_failed == 0) ? 0 : 1;
}
