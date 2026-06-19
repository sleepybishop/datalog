#include <stdio.h>
#include <string.h>
#include <assert.h>
#define START_TEST(fn) void fn(void)
#define END_TEST
#define ck_assert(x) assert(x);

struct tcase_fxt {
    char *name;
    void (*before)(void);
    void (*after)(void);
};

static struct tcase_fxt fxt;

#define TCase char
#define Suite char
#define SRunner char

#define CK_NORMAL 0
#define srunner_create(s) s
#define srunner_run_all(sr, mode)
#define srunner_ntests_failed(sr) (0)
#define srunner_free(sr) strlen(sr);
#define suite_create(s) s;
#define tcase_create(n) (n);
#define tcase_add_checked_fixture(tc, b, a)                                                                                        \
    do {                                                                                                                           \
        fxt.before = (b);                                                                                                          \
        fxt.after = (a);                                                                                                           \
    } while (0);
#define suite_add_tcase(s, n) fprintf(stderr, "%s->%s\n", s, n);
#define tcase_add_test(p, fn)                                                                                                      \
    do {                                                                                                                           \
        if (fxt.before)                                                                                                            \
            fxt.before();                                                                                                          \
        fn();                                                                                                                      \
        if (fxt.after)                                                                                                             \
            fxt.after();                                                                                                           \
    } while (0)
