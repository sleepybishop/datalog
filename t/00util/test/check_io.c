#include <check.h>
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
    FILE *fp = tmpfile();
    ck_assert(fp != NULL);
    ck_assert_int_eq(write_facts(facts, fp), 0);
    rewind(fp);
    ck_assert_int_eq(read_facts(loaded, fp), 0);
    ck_assert_int_eq(facts_contains_spo(loaded, "subject", "predicate", "quoted\" value\\\n"), 1);
    ck_assert_int_eq(fclose(fp), 0);
    delete_facts(loaded);
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
    tcase_add_test(core, test_logged_mutation_reports_flush_failure);
    suite_add_tcase(suite, core);
    SRunner *runner = srunner_create(suite);
    srunner_run_all(runner, CK_NORMAL);
    int failed = srunner_ntests_failed(runner);
    srunner_free(runner);
    return failed ? 1 : 0;
}
