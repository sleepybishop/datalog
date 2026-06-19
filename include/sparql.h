#ifndef SPARQL_H
#define SPARQL_H

#include "facts.h"

/*
 * Parse a basic SPARQL SELECT query string and execute it against facts.
 *
 * Supported query syntax:
 *   SELECT ?v1 ?v2 WHERE { ?v1 <pred> ?v2 . ?v2 "lit" <sym> . }
 *
 * Populates s_facts_with_cursor `c` which can be traversed with
 * facts_with_cursor_next and must be cleaned up using facts_with_cursor_destroy.
 *
 * Returns 0 on success, or -1 on syntax/parse errors.
 */
int facts_sparql(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, const char *query);

int facts_sparql_eval(s_facts *facts, const char *query, s_facts_with_cursor *c, s_binding **bindings_out);

char **sparql_projection_vars(s_facts *facts, const char *query, size_t *count);

int sparql_query_is_ask(s_facts *facts, const char *query);

/*
 * Parse a SPARQL INSERT DATA statement and add facts to the database.
 * Returns the number of successfully added facts, or -1 on error.
 */
int facts_sparql_insert(s_facts *facts, const char *query);

#endif
