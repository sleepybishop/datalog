#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "io.h"

int write_string_quoted(const char *string, FILE *fp)
{
    size_t i;
    size_t string_len = strlen(string);
    if (putc_unlocked('"', fp) == EOF)
        return -1;
    for (i = 0; i < string_len; i++) {
        unsigned char c = (unsigned char)string[i];
        if (c == '"' || c == '\\') {
            if (putc_unlocked('\\', fp) == EOF)
                return -1;
        }
        if (putc_unlocked(c, fp) == EOF)
            return -1;
    }
    if (putc_unlocked('"', fp) == EOF || putc_unlocked('\n', fp) == EOF)
        return -1;
    return 0;
}

int write_string(const char *string, FILE *fp)
{
    if (string[0] == '"' || strchr(string, '\n') || strchr(string, '\\'))
        return write_string_quoted(string, fp);
    if (string[0]) {
        size_t len = strlen(string);
        if (fwrite(string, 1, len, fp) != len)
            return -1;
    }
    if (putc_unlocked('\n', fp) == EOF)
        return -1;
    return 0;
}

int read_string_quoted(char *buf, size_t len, FILE *fp)
{
    int ch = getc_unlocked(fp);
    if (ch == EOF)
        return -1;
    while (ch != '"') {
        if (len == 1)
            return -1;
        if (ch == '\\') {
            ch = getc_unlocked(fp);
            if (ch == EOF)
                return -1;
        }
        if (ch == '\0')
            return -1;
        *buf++ = (char)ch;
        len--;
        ch = getc_unlocked(fp);
        if (ch == EOF)
            return -1;
    }
    *buf = 0;
    ch = getc_unlocked(fp);
    if (ch != '\n')
        return -1;
    return 0;
}

int read_string(char *buf, size_t len, FILE *fp)
{
    assert(buf);
    assert(len > 0);
    int ch = getc_unlocked(fp);
    if (ch == EOF)
        return -1;
    if (ch == '"')
        return read_string_quoted(buf, len, fp);
    while (ch != '\n') {
        if (len == 1 || ch == '\0')
            return -1;
        *buf++ = (char)ch;
        len--;
        ch = getc_unlocked(fp);
        if (ch == EOF)
            return -1;
    }
    *buf = 0;
    return 0;
}

int write_fact(const s_fact *f, FILE *fp)
{
    assert(f);
    assert(fp);
    if (write_string(symbol_to_str(f->s), fp))
        return -1;
    if (write_string(symbol_to_str(f->p), fp))
        return -1;
    if (write_string(symbol_to_str(f->o), fp))
        return -1;
    if (putc_unlocked('\n', fp) == EOF)
        return -1;
    return 0;
}

int write_facts(s_facts *facts, FILE *fp)
{
    s_facts_cursor c;
    s_fact *f;
    const char *s;
    const char *p;
    const char *o;
    assert(facts);
    facts_with_0(facts, &c, &s, &p, &o);
    while ((f = facts_cursor_next(&c))) {
        if (write_fact(f, fp)) {
            facts_cursor_stop(&c);
            return -1;
        }
    }
    fflush(fp);
    return 0;
}

int read_fact(s_facts *facts, s_fact *f, char *buf, size_t buf_sz, FILE *fp)
{
    assert(facts);
    assert(f);
    assert(buf);
    assert(buf_sz > 0);
    f->negated = NULL;
    if (read_string(buf, buf_sz, fp))
        return -1;
    if (!buf[0])
        return -1;
    if (!(f->s = facts_intern(facts, buf)))
        return -1;
    if (read_string(buf, buf_sz, fp))
        return -1;
    if (!buf[0])
        return -1;
    if (!(f->p = facts_intern(facts, buf)))
        return -1;
    if (read_string(buf, buf_sz, fp))
        return -1;
    if (!buf[0])
        return -1;
    if (!(f->o = facts_intern(facts, buf)))
        return -1;
    if (getc_unlocked(fp) != '\n')
        return -1;
    return 0;
}

static int fpeek(FILE *fp)
{
    int c = getc_unlocked(fp);
    if (c >= 0)
        ungetc(c, fp);
    return c;
}

int read_facts(s_facts *facts, FILE *fp)
{
    s_fact f;
    char *buf;
    assert(facts);
    if (!(buf = calloc(FACTS_LOAD_BUFSZ, sizeof(char))))
        return -1;
    while (!feof(fp) && fpeek(fp) != EOF) {
        if (read_fact(facts, &f, buf, FACTS_LOAD_BUFSZ, fp)) {
            free(buf);
            return -1;
        }
        int added = facts_add_fact(facts, &f) != NULL;
        facts_unintern(facts, f.s);
        facts_unintern(facts, f.p);
        facts_unintern(facts, f.o);
        if (!added) {
            free(buf);
            return -1;
        }
    }
    free(buf);
    return 0;
}

int write_fact_log(const char *operation, const s_fact *f, FILE *fp)
{
    size_t len = strlen(operation);
    if (fwrite(operation, 1, len, fp) != len)
        return -1;
    if (putc_unlocked('\n', fp) == EOF)
        return -1;
    if (write_fact(f, fp))
        return -1;
    fflush(fp);
    return 0;
}

int read_facts_log(s_facts *facts, FILE *fp)
{
    char operation[32];
    int op = 0;
    s_fact f;
    char *buf;
    assert(facts);
    if (!(buf = calloc(FACTS_LOAD_BUFSZ, sizeof(char))))
        return -1;
    while (!feof(fp) && fpeek(fp) != EOF) {
        if (!fgets(operation, sizeof(operation), fp)) {
            free(buf);
            return -1;
        }
        size_t len = strlen(operation);
        if (len > 0 && operation[len - 1] != '\n') {
            int c;
            while ((c = getc_unlocked(fp)) != '\n' && c != EOF) {
            }
            free(buf);
            return -1;
        }
        if (!strcasecmp(operation, "add\n"))
            op = 1;
        else if (!strcasecmp(operation, "remove\n"))
            op = 2;
        else {
            fprintf(stderr,
                    "facts_load_log:"
                    " unknown operation: %s\n",
                    operation);
            free(buf);
            return -1;
        }
        if (read_fact(facts, &f, buf, FACTS_LOAD_BUFSZ, fp)) {
            free(buf);
            return -1;
        }
        if (op == 1) {
            int added = facts_add_fact(facts, &f) != NULL;
            facts_unintern(facts, f.s);
            facts_unintern(facts, f.p);
            facts_unintern(facts, f.o);
            if (!added) {
                free(buf);
                return -1;
            }
        } else if (op == 2) {
            int removed = facts_remove_fact(facts, &f);
            facts_unintern(facts, f.s);
            facts_unintern(facts, f.p);
            facts_unintern(facts, f.o);
            if (!removed) {
                free(buf);
                return -1;
            }
        }
    }
    free(buf);
    return 0;
}

int write_spec(p_spec spec, FILE *fp)
{
    s_spec_cursor c;
    s_spec_fact f;
    spec_cursor_init(&c, spec);
    while (spec_cursor_next(&c, &f)) {
        if (write_string(f.s, fp))
            return -1;
        if (write_string(f.p, fp))
            return -1;
        if (write_string(f.o, fp))
            return -1;
        if (putc_unlocked('\n', fp) == EOF)
            return -1;
    }
    return 0;
}
