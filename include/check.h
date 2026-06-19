#ifndef MOCK_CHECK_H
#define MOCK_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

// Mock types to match Check API
typedef struct Suite Suite;
typedef struct TCase TCase;
typedef struct SRunner SRunner;

#define START_TEST(name)                                                                                                           \
    void name(void)                                                                                                                \
    {

#define END_TEST }

#define ck_assert(expr)                                                                                                            \
    do {                                                                                                                           \
        if (!(expr)) {                                                                                                             \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s' failed\n", __FILE__, __LINE__, #expr);                                    \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

#define ck_assert_int_eq(a, b)                                                                                                     \
    do {                                                                                                                           \
        long long _a = (long long)(a);                                                                                             \
        long long _b = (long long)(b);                                                                                             \
        if (_a != _b) {                                                                                                            \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s == %s' failed (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b);      \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

#define ck_assert_ptr_eq(a, b)                                                                                                     \
    do {                                                                                                                           \
        if ((a) != (b)) {                                                                                                          \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s == %s' (pointers) failed (%p != %p)\n", __FILE__, __LINE__, #a, #b,        \
                    (void *)(a), (void *)(b));                                                                                     \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

#define ck_assert_ptr_ne(a, b)                                                                                                     \
    do {                                                                                                                           \
        if ((a) == (b)) {                                                                                                          \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s != %s' (pointers) failed (%p == %p)\n", __FILE__, __LINE__, #a, #b,        \
                    (void *)(a), (void *)(b));                                                                                     \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

#define ck_assert_str_eq(a, b)                                                                                                     \
    do {                                                                                                                           \
        const char *_a = (a);                                                                                                      \
        const char *_b = (b);                                                                                                      \
        if (strcmp(_a, _b) != 0) {                                                                                                 \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s == %s' (strings) failed (\"%s\" != \"%s\")\n", __FILE__, __LINE__, #a, #b, \
                    _a, _b);                                                                                                       \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

#define ck_assert_double_eq_tol(a, b, tol)                                                                                         \
    do {                                                                                                                           \
        double _a = (a);                                                                                                           \
        double _b = (b);                                                                                                           \
        if (fabs(_a - _b) > (tol)) {                                                                                               \
            fprintf(stderr, "FAIL: %s:%d: Assertion '%s == %s' (double) failed (%f != %f within %f)\n", __FILE__, __LINE__, #a,    \
                    #b, _a, _b, (tol));                                                                                            \
            exit(1);                                                                                                               \
        }                                                                                                                          \
    } while (0)

typedef void (*test_fn_t)(void);
typedef void (*fixture_fn_t)(void);

struct TCase {
    const char *name;
    fixture_fn_t setup;
    fixture_fn_t teardown;
    test_fn_t tests[256];
    int test_count;
};

struct Suite {
    const char *name;
    TCase tcases[64];
    int tcase_count;
};

struct SRunner {
    Suite *suite;
    int failed_count;
};

static inline Suite *suite_create(const char *name)
{
    Suite *s = malloc(sizeof(Suite));
    if (s) {
        s->name = name;
        s->tcase_count = 0;
    }
    return s;
}

static inline TCase *tcase_create(const char *name)
{
    TCase *tc = malloc(sizeof(TCase));
    if (tc) {
        tc->name = name;
        tc->setup = NULL;
        tc->teardown = NULL;
        tc->test_count = 0;
    }
    return tc;
}

static inline void suite_add_tcase(Suite *s, TCase *tc)
{
    if (s && tc && s->tcase_count < 64) {
        s->tcases[s->tcase_count++] = *tc;
    }
    free(tc);
}

static inline void tcase_add_checked_fixture(TCase *tc, fixture_fn_t setup, fixture_fn_t teardown)
{
    if (tc) {
        tc->setup = setup;
        tc->teardown = teardown;
    }
}

static inline void _tcase_add_test(TCase *tc, test_fn_t test)
{
    if (tc && tc->test_count < 256) {
        tc->tests[tc->test_count++] = test;
    }
}

#define tcase_add_test(tc, test) _tcase_add_test(tc, test)

static inline SRunner *srunner_create(Suite *s)
{
    SRunner *sr = malloc(sizeof(SRunner));
    if (sr) {
        sr->suite = s;
        sr->failed_count = 0;
    }
    return sr;
}

static inline void srunner_free(SRunner *sr)
{
    if (sr) {
        free(sr->suite);
        free(sr);
    }
}

static inline int srunner_ntests_failed(SRunner *sr)
{
    return sr ? sr->failed_count : 0;
}

enum { CK_NORMAL = 0 };

static inline void srunner_run_all(SRunner *sr, int mode)
{
    (void)mode;
    if (!sr || !sr->suite)
        return;
    Suite *s = sr->suite;
    for (int i = 0; i < s->tcase_count; i++) {
        TCase *tc = &s->tcases[i];
        for (int j = 0; j < tc->test_count; j++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                exit(1);
            }
            if (pid == 0) {
                // Child process: run setup, test, and teardown
                if (tc->setup)
                    tc->setup();
                tc->tests[j]();
                if (tc->teardown)
                    tc->teardown();
                exit(0);
            } else {
                // Parent process: wait for child
                int status;
                waitpid(pid, &status, 0);
                if (WIFEXITED(status)) {
                    if (WEXITSTATUS(status) != 0) {
                        sr->failed_count++;
                    }
                } else {
                    sr->failed_count++;
                }
            }
        }
    }
}

#endif
