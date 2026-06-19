#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "deps/linenoise/linenoise.h"
#include "facts.h"
#include "io.h"
#include "sparql.h"
#include "rule.h"
#include "eval.h"

static s_datalog_program *repl_program = NULL;

void clear_idb_facts(s_facts *facts, const s_datalog_program *prog)
{
    for (size_t i = 0; i < prog->rule_count; i++) {
        const char *pred = prog->rules[i].head.p;
        if (pred) {
            s_facts_cursor c;
            facts_with_0(facts, &c, NULL, NULL, NULL);
            s_fact *f;
            s_fact_list *fl = NULL;
            while ((f = facts_cursor_next(&c)) != NULL) {
                if (strcmp(symbol_to_str(f->p), pred) == 0) {
                    fl = fact_list_intern(fl, f);
                }
            }
            facts_cursor_stop(&c);
            s_fact_list *fli = fl;
            while (fli) {
                facts_remove_fact(facts, fli->fact);
                fli = fli->next;
            }
            delete_fact_list(fl);
        }
    }
}

void execute_query(s_facts *facts, const char *query_str)
{
    if (repl_program && repl_program->rule_count > 0) {
        clear_idb_facts(facts, repl_program);
        facts_datalog_eval(facts, repl_program);
    }
    int is_ask = sparql_query_is_ask(facts, query_str);
    if (is_ask < 0) {
        fprintf(stderr, "Error: failed to parse query\n");
        return;
    }

    if (is_ask) {
        s_facts_with_cursor c;
        s_binding *bindings = NULL;
        if (facts_sparql_eval(facts, query_str, &c, &bindings) == 0) {
            int has_match = facts_with_cursor_next(&c);
            printf("%s\n", has_match ? "true" : "false");
            facts_with_cursor_destroy(&c);
            free(bindings);
        } else {
            fprintf(stderr, "Error: query execution failed\n");
        }
        return;
    }

    size_t proj_count = 0;
    char **proj_vars = sparql_projection_vars(facts, query_str, &proj_count);
    if (!proj_vars) {
        fprintf(stderr, "Error: failed to parse query\n");
        return;
    }

    s_facts_with_cursor c;
    s_binding *bindings = NULL;
    if (facts_sparql_eval(facts, query_str, &c, &bindings) == 0) {
        // Print headers
        for (size_t i = 0; i < proj_count; i++) {
            printf("%s\t", proj_vars[i]);
        }
        printf("\n");
        for (size_t i = 0; i < proj_count; i++) {
            printf("-----\t");
        }
        printf("\n");

        // Print results
        int rows = 0;
        while (facts_with_cursor_next(&c)) {
            for (size_t i = 0; i < proj_count; i++) {
                const char **val_ptr = bindings_get(bindings, proj_vars[i]);
                printf("%s\t", (val_ptr && *val_ptr) ? *val_ptr : "NULL");
            }
            printf("\n");
            rows++;
        }
        printf("(%d rows)\n", rows);
        facts_with_cursor_destroy(&c);
        free(bindings);
    } else {
        fprintf(stderr, "Error: query execution failed\n");
    }

    // Cleanup
    for (size_t i = 0; i < proj_count; i++) {
        free(proj_vars[i]);
    }
    free(proj_vars);
}

void process_input(s_facts *facts, const char *line, const char *db_filename)
{
    // Skip leading whitespace
    while (*line && (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')) {
        line++;
    }
    if (*line == '\0') {
        return;
    }

    if (strcasecmp(line, "SHOW RULES") == 0) {
        datalog_program_print(repl_program, stdout);
    } else if (strcasecmp(line, "CLEAR RULES") == 0) {
        delete_datalog_program(repl_program);
        repl_program = new_datalog_program();
        printf("Rules cleared.\n");
    } else if (strstr(line, ":-") != NULL) {
        if (datalog_program_parse_rules(repl_program, line) == 0) {
            printf("Successfully added rule(s).\n");
        } else {
            fprintf(stderr, "Error: failed to parse or validate rule(s)\n");
        }
    } else if (strncasecmp(line, "INSERT", 6) == 0) {
        int added = facts_sparql_insert(facts, line);
        if (added >= 0) {
            printf("Successfully added %d facts.\n", added);
            // Persist changes if db_filename is provided
            if (db_filename) {
                FILE *fp = fopen(db_filename, "w");
                if (fp) {
                    if (write_facts(facts, fp) >= 0) {
                        printf("Persisted database changes to '%s'.\n", db_filename);
                    } else {
                        fprintf(stderr, "Error: failed to persist database changes to '%s'\n", db_filename);
                    }
                    fclose(fp);
                } else {
                    fprintf(stderr, "Error: could not open database file '%s' for writing\n", db_filename);
                }
            }
        } else {
            fprintf(stderr, "Error: failed to parse or execute INSERT statement\n");
        }
    } else if (strncasecmp(line, "SELECT", 6) == 0 || strncasecmp(line, "ASK", 3) == 0 || strncasecmp(line, "PREFIX", 6) == 0) {
        execute_query(facts, line);
    } else {
        fprintf(stderr, "Error: unknown statement type (only SELECT, ASK and INSERT DATA statements are supported)\n");
    }
}

int main(int argc, char **argv)
{
    repl_program = new_datalog_program();
    s_intern *sym = new_intern(100000);
    s_facts *facts = new_facts(sym, 100000);
    const char *db_filename = NULL;

    if (argc > 1) {
        db_filename = argv[1];
        if (access(db_filename, F_OK) == 0) {
            FILE *fp = fopen(db_filename, "r");
            if (!fp) {
                fprintf(stderr, "Error: could not open database file '%s'\n", db_filename);
                delete_facts(facts);
                delete_intern(sym);
                return 1;
            }
            if (read_facts(facts, fp) < 0) {
                fprintf(stderr, "Error: failed to read facts from '%s'\n", db_filename);
                fclose(fp);
                delete_facts(facts);
                delete_intern(sym);
                return 1;
            }
            fclose(fp);
            printf("Loaded facts from '%s'\n", db_filename);
        } else {
            printf("Database file '%s' does not exist. Starting with an empty database.\n", db_filename);
        }
    }

    // If STDIN is not a TTY, read all input and execute once
    if (!isatty(STDIN_FILENO)) {
        size_t capacity = 4096;
        size_t size = 0;
        char *buf = malloc(capacity);
        while (1) {
            size_t n = fread(buf + size, 1, capacity - size - 1, stdin);
            if (n == 0)
                break;
            size += n;
            if (size + 1 >= capacity) {
                capacity *= 2;
                buf = realloc(buf, capacity);
            }
        }
        buf[size] = '\0';
        process_input(facts, buf, db_filename);
        free(buf);
    } else {
        // Interactive REPL
        char *line;
        linenoiseHistoryLoad("history.txt");
        printf("SPARQL Query REPL (datalog engine)\n");
        printf("Type 'exit' or 'quit' to exit.\n\n");

        while ((line = linenoise("sparql> ")) != NULL) {
            if (line[0] != '\0') {
                if (strcasecmp(line, "exit") == 0 || strcasecmp(line, "quit") == 0) {
                    free(line);
                    break;
                }
                process_input(facts, line, db_filename);
                linenoiseHistoryAdd(line);
                linenoiseHistorySave("history.txt");
            }
            free(line);
        }
    }

    delete_facts(facts);
    delete_intern(sym);
    delete_datalog_program(repl_program);
    return 0;
}
