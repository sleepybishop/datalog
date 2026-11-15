#define _GNU_SOURCE
#include <check.h>
#include <errno.h>
#include "facts.h"
#include "io.h"

START_TEST(test_write_facts_reports_flush_failure)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    ck_assert(facts_add_spo(facts, "subject", "predicate", "object") != NULL);
    FILE *fp = fopen("/dev/full", "w");
    ck_assert(fp != NULL);
    char buffer[4096];
    ck_assert_int_eq(setvbuf(fp, buffer, _IOFBF, sizeof(buffer)), 0);
    ck_assert_int_eq(write_facts(facts, fp), -1);
    ck_assert(ferror(fp));
    fclose(fp);
    delete_facts(facts);
}
END_TEST

START_TEST(test_write_fact_log_reports_flush_failure)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    s_fact *fact = facts_add_spo(facts, "subject", "predicate", "object");
    ck_assert(fact != NULL);
    FILE *fp = fopen("/dev/full", "w");
    ck_assert(fp != NULL);
    char buffer[4096];
    ck_assert_int_eq(setvbuf(fp, buffer, _IOFBF, sizeof(buffer)), 0);
    ck_assert_int_eq(write_fact_log("add", fact, fp), -1);
    ck_assert(ferror(fp));
    fclose(fp);
    delete_facts(facts);
}
END_TEST

START_TEST(test_write_facts_round_trip)
{
    s_facts *facts = new_facts(NULL, 100);
    s_facts *loaded = new_facts(NULL, 100);
    ck_assert(facts != NULL && loaded != NULL);
    ck_assert(facts_add_spo(facts, "subject", "predicate", "quoted\" value\\\n") != NULL);
    ck_assert(facts_add_spo(facts, "empty_literal", "predicate", "") != NULL);
    ck_assert(facts_add_spo(facts, "", "", "") != NULL);
    FILE *fp = tmpfile();
    ck_assert(fp != NULL);
    ck_assert_int_eq(write_facts(facts, fp), 0);
    rewind(fp);
    ck_assert_int_eq(read_facts(loaded, fp), 0);
    ck_assert_int_eq(facts_contains_spo(loaded, "subject", "predicate", "quoted\" value\\\n"), 1);
    ck_assert_int_eq(facts_contains_spo(loaded, "empty_literal", "predicate", ""), 1);
    ck_assert_int_eq(facts_contains_spo(loaded, "", "", ""), 1);
    ck_assert_int_eq(facts_count(loaded), 3);
    ck_assert_int_eq(fclose(fp), 0);
    delete_facts(loaded);
    delete_facts(facts);
}
END_TEST

START_TEST(test_empty_literal_log_round_trip)
{
    s_facts *facts = new_facts(NULL, 100);
    s_facts *loaded = new_facts(NULL, 100);
    ck_assert(facts != NULL && loaded != NULL);
    s_fact *fact = facts_add_spo(facts, "subject", "predicate", "");
    ck_assert(fact != NULL);
    FILE *added = tmpfile();
    FILE *removed = tmpfile();
    ck_assert(added != NULL && removed != NULL);
    ck_assert_int_eq(write_fact_log("add", fact, added), 0);
    ck_assert_int_eq(write_fact_log("remove", fact, removed), 0);
    rewind(added);
    rewind(removed);
    ck_assert_int_eq(read_facts_log(loaded, added), 0);
    ck_assert_int_eq(facts_contains_spo(loaded, "subject", "predicate", ""), 1);
    ck_assert_int_eq(read_facts_log(loaded, removed), 0);
    ck_assert_int_eq(facts_count(loaded), 0);
    ck_assert_int_eq(fclose(removed), 0);
    ck_assert_int_eq(fclose(added), 0);
    delete_facts(loaded);
    delete_facts(facts);
}
END_TEST

typedef struct failing_input {
    const char *text;
    size_t offset;
} s_failing_input;

#if defined(__GLIBC__)
static ssize_t read_until_error(void *cookie, char *buf, size_t size)
{
    s_failing_input *input = cookie;
    size_t remaining = strlen(input->text) - input->offset;
    if (!remaining) {
        errno = EIO;
        return -1;
    }
    if (size > remaining)
        size = remaining;
    memcpy(buf, input->text + input->offset, size);
    input->offset += size;
    return (ssize_t)size;
}

static FILE *open_failing_stream(s_failing_input *input)
{
    cookie_io_functions_t callbacks = {.read = read_until_error};
    return fopencookie(input, "r", callbacks);
}
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
static int read_until_error(void *cookie, char *buf, int size)
{
    s_failing_input *input = cookie;
    size_t remaining = strlen(input->text) - input->offset;
    if (!remaining || size < 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)size > remaining)
        size = (int)remaining;
    memcpy(buf, input->text + input->offset, (size_t)size);
    input->offset += (size_t)size;
    return size;
}

static FILE *open_failing_stream(s_failing_input *input)
{
    return funopen(input, read_until_error, NULL, NULL, NULL);
}
#else
#define NO_CUSTOM_STDIO_STREAM 1
#endif

START_TEST(test_read_facts_reports_stream_errors)
{
#ifndef NO_CUSTOM_STDIO_STREAM
    const char *inputs[] = {"", "subject\npredicate\nobject\n\n"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        s_facts *facts = new_facts(NULL, 100);
        ck_assert(facts != NULL);
        s_failing_input input = {inputs[i], 0};
        FILE *fp = open_failing_stream(&input);
        ck_assert(fp != NULL);
        ck_assert_int_eq(read_facts(facts, fp), -1);
        ck_assert(ferror(fp));
        ck_assert_int_eq(facts_count(facts), i);
        ck_assert_int_eq(fclose(fp), 0);
        delete_facts(facts);
    }
#endif
}
END_TEST

START_TEST(test_read_facts_log_reports_stream_errors)
{
#ifndef NO_CUSTOM_STDIO_STREAM
    const char *inputs[] = {"", "add\nsubject\npredicate\nobject\n\n"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        s_facts *facts = new_facts(NULL, 100);
        ck_assert(facts != NULL);
        s_failing_input input = {inputs[i], 0};
        FILE *fp = open_failing_stream(&input);
        ck_assert(fp != NULL);
        ck_assert_int_eq(read_facts_log(facts, fp), -1);
        ck_assert(ferror(fp));
        ck_assert_int_eq(facts_count(facts), i);
        ck_assert_int_eq(fclose(fp), 0);
        delete_facts(facts);
    }
#endif
}
END_TEST

START_TEST(test_read_facts_log_reports_removal_failure)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    s_fact *fact = facts_add_spo(facts, "subject", "predicate", "object");
    ck_assert(fact != NULL);
    FILE *replay = tmpfile();
    FILE *log = fopen("/dev/full", "w");
    ck_assert(replay != NULL && log != NULL);
    ck_assert_int_eq(write_fact_log("remove", fact, replay), 0);
    rewind(replay);
    facts->log = log;
    ck_assert_int_eq(read_facts_log(facts, replay), -1);
    ck_assert(ferror(log));
    ck_assert_int_eq(facts_contains_spo(facts, "subject", "predicate", "object"), 1);
    facts->log = NULL;
    fclose(log);
    ck_assert_int_eq(fclose(replay), 0);
    delete_facts(facts);
}
END_TEST

START_TEST(test_logged_mutation_reports_flush_failure)
{
    s_facts *facts = new_facts(NULL, 100);
    ck_assert(facts != NULL);
    FILE *fp = fopen("/dev/full", "w");
    ck_assert(fp != NULL);
    char buffer[4096];
    ck_assert_int_eq(setvbuf(fp, buffer, _IOFBF, sizeof(buffer)), 0);
    facts->log = fp;
    ck_assert_int_eq(facts_transaction_begin(facts), 0);
    ck_assert(facts_add_spo(facts, "subject", "predicate", "object") == NULL);
    ck_assert_int_eq(facts_contains_spo(facts, "subject", "predicate", "object"), 0);
    ck_assert_int_eq(facts_transaction_commit(facts), -1);
    ck_assert_int_eq(facts_count(facts), 0);
    facts->log = NULL;
    fclose(fp);
    delete_facts(facts);
}
END_TEST

int main(void)
{
    Suite *suite = suite_create("IO");
    TCase *core = tcase_create("Core");
    tcase_add_test(core, test_write_facts_reports_flush_failure);
    tcase_add_test(core, test_write_fact_log_reports_flush_failure);
    tcase_add_test(core, test_write_facts_round_trip);
    tcase_add_test(core, test_empty_literal_log_round_trip);
    tcase_add_test(core, test_read_facts_reports_stream_errors);
    tcase_add_test(core, test_read_facts_log_reports_stream_errors);
    tcase_add_test(core, test_read_facts_log_reports_removal_failure);
    tcase_add_test(core, test_logged_mutation_reports_flush_failure);
    suite_add_tcase(suite, core);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
