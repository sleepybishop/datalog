#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "facts.h"

#define FACT_COUNT 1000
#define SAMPLES 40
#define P95_LIMIT_NS 500000000LL
#define RSS_LIMIT_KB (256L * 1024L)

static size_t physical_changes;

static void summary_observer(s_facts *facts, const s_facts_commit_summary *summary, void *user_data)
{
    (void)facts;
    (void)user_data;
    physical_changes = summary->physical_changes;
}

static long long monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int compare_ns(const void *a, const void *b)
{
    long long av = *(const long long *)a;
    long long bv = *(const long long *)b;
    return (av > bv) - (av < bv);
}

int main(void)
{
    s_facts *facts = new_facts(NULL, FACT_COUNT * 4);
    s_datalog_program *program = new_datalog_program();
    if (!facts || !program ||
        datalog_program_parse_rules(program, "?X <derived> yes :- ?X <source> yes .\n") != 0)
        return 1;

    char subject[32];
    for (int i = 0; i < FACT_COUNT; i++) {
        snprintf(subject, sizeof(subject), "item-%d", i);
        if (!facts_add_spo(facts, subject, "source", "yes"))
            return 1;
    }
    if (facts_attach_program(facts, program) != 0)
        return 1;
    delete_datalog_program(program);
    facts_register_commit_summary_observer(facts, summary_observer, NULL);

    long long samples[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        snprintf(subject, sizeof(subject), "item-%d", i);
        physical_changes = 0;
        long long start = monotonic_ns();
        if (start < 0 || facts_transaction_begin(facts) != 0 ||
            facts_remove_spo(facts, subject, "source", "yes") != 1 || facts_transaction_commit(facts) != 0)
            return 1;
        samples[i] = monotonic_ns() - start;
        if (samples[i] < 0 || physical_changes != 2 ||
            facts_contains_spo(facts, subject, "derived", "yes") != 0)
            return 1;
        if (facts_transaction_begin(facts) != 0 || !facts_add_spo(facts, subject, "source", "yes") ||
            facts_transaction_commit(facts) != 0)
            return 1;
    }

    qsort(samples, SAMPLES, sizeof(samples[0]), compare_ns);
    long long p50 = samples[SAMPLES / 2];
    long long p95 = samples[(SAMPLES * 95) / 100];
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 1;
    printf("reactive retraction: facts=%d samples=%d p50=%.3fms p95=%.3fms peak_rss=%ldKB\n", FACT_COUNT,
           SAMPLES, p50 / 1000000.0, p95 / 1000000.0, usage.ru_maxrss);

    int failed = p95 > P95_LIMIT_NS || usage.ru_maxrss > RSS_LIMIT_KB;
    delete_facts(facts);
    return failed ? 1 : 0;
}
