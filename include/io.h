#ifndef IO_H
#define IO_H

#include <stdio.h>
#include "fact.h"
#include "facts.h"
#include "spec.h"

int read_string_quoted(char *buf, size_t len, FILE *fp);

int write_string_quoted(const char *string, FILE *fp);

int read_string(char *buf, size_t len, FILE *fp);

int write_string(const char *string, FILE *fp);

int read_fact(s_facts *facts, s_fact *f, char *buf, size_t buf_sz, FILE *fp);

int write_fact(const s_fact *f, FILE *fp);

int read_facts(s_facts *facts, FILE *fp);

int write_facts(s_facts *facts, FILE *fp);

int read_facts_log(s_facts *facts, FILE *fp);

int write_fact_log(const char *operation, const s_fact *f, FILE *fp);

int write_spec(p_spec spec, FILE *fp);

#endif
