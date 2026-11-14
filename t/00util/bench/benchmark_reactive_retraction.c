#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "facts.h"
#include "justification.h"

#define FACT_COUNT 1000
#define SAMPLES 40
#define P95_LIMIT_NS 500000000LL
#define RSS_LIMIT_KB (256L * 1024L)
#define BULK_FACT_COUNT 2000
#define BULK_LIMIT_NS 5000000000LL
#define GROUNDING_CHAIN_LENGTH 1000

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

static size_t external_support_calls;

static int chain_external_support(const s_justification_support *support, void *user_data)
{
    external_support_calls++;
    return *(int *)user_data && !support->negated && strcmp(support->s, "root") == 0;
}

static int benchmark_grounding_chain(void)
{
    s_justification_graph *graph = new_justification_graph();
    if (!graph)
        return -1;
    /* store the root proof last, forcing a rescanning fixed-point algorithm to
     * make one pass per link; grounding should visit every support only once. */
    for (int i = 0; i < GROUNDING_CHAIN_LENGTH; i++) {
        char conclusion[32], dependency[32];
        snprintf(conclusion, sizeof(conclusion), "node-%d", i);
        snprintf(dependency, sizeof(dependency), "node-%d", i + 1);
        s_justification_support support = {i + 1 == GROUNDING_CHAIN_LENGTH ? "root" : dependency, "p", "yes", 0};
        if (justification_graph_add(graph, conclusion, "p", "yes", 0, &support, 1) < 0) {
            delete_justification_graph(graph);
            return -1;
        }
    }
    int root_present = 1;
    external_support_calls = 0;
    int grounded = justification_graph_is_grounded(graph, "node-0", "p", "yes", chain_external_support, &root_present);
    size_t rooted_calls = external_support_calls;
    root_present = 0;
    external_support_calls = 0;
    int ungrounded = justification_graph_is_grounded(graph, "node-0", "p", "yes", chain_external_support, &root_present);
    printf("grounding chain: proofs=%d rooted_support_checks=%zu unrooted_support_checks=%zu\n", GROUNDING_CHAIN_LENGTH,
           rooted_calls, external_support_calls);
    int failed = grounded != 1 || ungrounded != 0 || rooted_calls > GROUNDING_CHAIN_LENGTH * 2 ||
                 external_support_calls > GROUNDING_CHAIN_LENGTH * 2;
    delete_justification_graph(graph);
    return failed ? -1 : 0;
}

static int benchmark_alternate_proof_retraction(void)
{
    s_facts *facts = new_facts(NULL, BULK_FACT_COUNT * 8);
    s_datalog_program *program = new_datalog_program();
    if (!facts || !program ||
        datalog_program_parse_rules(program, "?X mid yes :- ?X source yes . "
                                             "?X derived yes :- ?X mid yes . "
                                             "?X derived yes :- ?X alt yes .") != 0)
        goto error;
    char subject[32];
    for (int i = 0; i < BULK_FACT_COUNT; i++) {
        snprintf(subject, sizeof(subject), "item-%d", i);
        if (!facts_add_spo(facts, subject, "source", "yes") || !facts_add_spo(facts, subject, "alt", "yes"))
            goto error;
    }
    if (facts_attach_program(facts, program) != 0)
        goto error;
    facts_register_commit_summary_observer(facts, summary_observer, NULL);
    physical_changes = 0;
    long long start = monotonic_ns();
    if (start < 0 || facts_transaction_begin(facts) != 0)
        goto error;
    for (int i = 0; i < BULK_FACT_COUNT; i++) {
        snprintf(subject, sizeof(subject), "item-%d", i);
        if (facts_remove_spo(facts, subject, "alt", "yes") != 1)
            goto error;
    }
    if (facts_transaction_commit(facts) != 0)
        goto error;
    long long elapsed = monotonic_ns() - start;
    if (elapsed < 0 || physical_changes != BULK_FACT_COUNT || facts_count(facts) != BULK_FACT_COUNT * 3)
        goto error;
    for (int i = 0; i < BULK_FACT_COUNT; i++) {
        snprintf(subject, sizeof(subject), "item-%d", i);
        if (!facts_contains_spo(facts, subject, "derived", "yes"))
            goto error;
    }
    printf("alternate-proof retraction: subjects=%d elapsed=%.3fms physical_changes=%zu\n", BULK_FACT_COUNT, elapsed / 1000000.0,
           physical_changes);
    delete_datalog_program(program);
    delete_facts(facts);
    return elapsed > BULK_LIMIT_NS ? -1 : 0;

error:
    if (facts) {
        facts_transaction_rollback(facts);
        delete_facts(facts);
    }
    delete_datalog_program(program);
    return -1;
}

int main(void)
{
    s_facts *facts = new_facts(NULL, FACT_COUNT * 4);
    s_datalog_program *program = new_datalog_program();
    if (!facts || !program || datalog_program_parse_rules(program, "?X <derived> yes :- ?X <source> yes .\n") != 0)
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
        if (start < 0 || facts_transaction_begin(facts) != 0 || facts_remove_spo(facts, subject, "source", "yes") != 1 ||
            facts_transaction_commit(facts) != 0)
            return 1;
        samples[i] = monotonic_ns() - start;
        if (samples[i] < 0 || physical_changes != 2 || facts_contains_spo(facts, subject, "derived", "yes") != 0)
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
    printf("reactive retraction: facts=%d samples=%d p50=%.3fms p95=%.3fms peak_rss=%ldKB\n", FACT_COUNT, SAMPLES, p50 / 1000000.0,
           p95 / 1000000.0, usage.ru_maxrss);

    int failed = p95 > P95_LIMIT_NS || usage.ru_maxrss > RSS_LIMIT_KB;
    delete_facts(facts);
    if (benchmark_grounding_chain() != 0 || benchmark_alternate_proof_retraction() != 0)
        failed = 1;
    return failed ? 1 : 0;
}
