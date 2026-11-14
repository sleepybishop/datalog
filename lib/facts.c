#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "facts.h"
#include "arena.h"
#include "random.h"
#include "io.h"
#include "lftj.h"
#include "eval.h"
#include "facts_internal.h"
#include "justification.h"

int facts_rollback_push(s_facts *facts, e_rollback_action action, const s_fact *fact);
static s_fact *facts_lookup_spo_locked(s_facts *facts, const char *s, const char *p, const char *o);

static int facts_log_change(s_facts *facts, const char *operation, const s_fact *fact)
{
    if (facts->log && write_fact_log(operation, fact, facts->log) != 0) {
        if (facts->tx.level > 0)
            facts->tx.io_error = 1;
        return -1;
    }
    return 0;
}

static void facts_set_support_locked(s_facts *facts, s_fact *fact, unsigned char asserted, unsigned char derived)
{
    int was_present = fact->asserted_count || fact->derived_count;
    int is_present = asserted || derived;
    if (was_present && !is_present) {
        facts->retired_count++;
        if (!fact->retirement_queued) {
            fact->retired_next = facts->retired_facts;
            fact->retirement_queued = 1;
            facts->retired_facts = fact;
        }
    } else if (!was_present && is_present) {
        assert(facts->retired_count > 0);
        facts->retired_count--;
    }
    fact->asserted_count = asserted;
    fact->derived_count = derived;
}

static void encode_cursor_key(unsigned char *key, const s_fact *f, int index_type)
{
    if (index_type == 0) { /* SPO */
        encode_triple_key(key, f->s, f->p, f->o);
    } else if (index_type == 1) { /* POS */
        encode_triple_key(key, f->p, f->o, f->s);
    } else { /* OSP */
        encode_triple_key(key, f->o, f->s, f->p);
    }
}

#define FACTS_LOCAL_BATCH 64
#define FACTS_LOCAL_FREE_MAX 256

static s_fact *facts_alloc_fact_locked(s_facts *facts, Symbol s, Symbol p, Symbol o)
{
    if (!facts->fact_free_list)
        facts->fact_free_count = arena_alloc_fact_batch(&facts->fact_free_list, FACTS_LOCAL_BATCH);
    if (!facts->fact_free_list)
        return NULL;
    s_fact *f = (s_fact *)facts->fact_free_list;
    facts->fact_free_list = *(void **)f;
    facts->fact_free_count--;
    memset(f, 0, sizeof(*f));
    f->s = s;
    f->p = p;
    f->o = o;
    return f;
}

static void facts_flush_fact_freelist(s_facts *facts)
{
    if (facts->fact_free_list) {
        arena_free_fact_list(facts->fact_free_list, facts->fact_free_count);
        facts->fact_free_list = NULL;
        facts->fact_free_count = 0;
    }
}

static void facts_free_fact_locked(s_facts *facts, s_fact *f)
{
    if (!f)
        return;
    if (facts->fact_free_count >= FACTS_LOCAL_FREE_MAX)
        facts_flush_fact_freelist(facts);
    *(void **)f = facts->fact_free_list;
    facts->fact_free_list = f;
    facts->fact_free_count++;
}

void facts_init(s_facts *facts, s_intern *symbols, unsigned long max)
{
    if (facts_init_checked(facts, symbols, max) != 0)
        abort();
}

int facts_init_checked(s_facts *facts, s_intern *symbols, unsigned long max)
{
    if (!facts || max == 0)
        return -1;
    memset(facts, 0, sizeof(*facts));
    arena_init();
    facts->symbols = symbols ? symbols : new_intern(max);
    facts->symbols_delete = !symbols;
    if (!facts->symbols)
        goto error;
    if (set_init_checked(&facts->index, max) != 0)
        goto error;
    facts->hexastore = new_hexastore();
    if (!facts->hexastore)
        goto error;
    facts->index_spo = facts->hexastore->trie_spo;
    facts->index_pos = facts->hexastore->trie_pos;
    facts->index_osp = facts->hexastore->trie_osp;
    facts->log = NULL;

    facts->prog = NULL;
    facts->owns_prog = 0;
    facts->disable_listener = 0;
    facts->justifications = new_justification_graph();
    if (!facts->justifications)
        goto error;
    facts->justifications_staging = NULL;

    if (transaction_init_checked(&facts->tx) != 0)
        goto error;
    return 0;

error:
    delete_justification_graph(facts->justifications);
    delete_hexastore(facts->hexastore);
    if (facts->index.items)
        set_destroy(&facts->index);
    if (facts->symbols_delete)
        delete_intern(facts->symbols);
    memset(facts, 0, sizeof(*facts));
    arena_destroy();
    return -1;
}

void facts_destroy(s_facts *facts)
{
    if (facts->owns_prog)
        delete_datalog_program(facts->prog);
    facts->prog = NULL;
    facts->owns_prog = 0;
    delete_justification_graph(facts->justifications_staging);
    delete_justification_graph(facts->justifications);
    facts->justifications_staging = NULL;
    facts->justifications = NULL;
    {
        s_set_cursor cursor;
        set_cursor_init(&facts->index, &cursor);
        s_set_item *item;
        while ((item = set_cursor_next(&cursor)) != NULL) {
            s_fact *fact = (s_fact *)item->data;
            if (!facts->symbols_delete) {
                intern_unstring(facts->symbols, fact->s);
                intern_unstring(facts->symbols, fact->p);
                intern_unstring(facts->symbols, fact->o);
            }
            *(void **)fact = facts->fact_free_list;
            facts->fact_free_list = fact;
            facts->fact_free_count++;
        }
        facts_flush_fact_freelist(facts);
    }
    delete_hexastore(facts->hexastore);
    set_destroy(&facts->index);
    if (facts->symbols_delete)
        delete_intern(facts->symbols);
    transaction_destroy(&facts->tx);
    arena_destroy();
}

void facts_reset(s_facts *facts)
{
    if (facts_reset_checked(facts) != 0)
        abort();
}

int facts_reset_checked(s_facts *facts)
{
    if (!facts || transaction_writer_owned(&facts->tx))
        return -1;
    int has_lock = transaction_acquire_writer(&facts->tx);
    int result = facts_reset_local_db(facts);
    transaction_release_writer(&facts->tx, has_lock);
    return result;
}

int facts_reset_local_db(s_facts *facts)
{
    s_set_cursor sc;
    s_set_item *si;
    s_set replacement_index = {0};
    s_hexastore *replacement_hexastore = NULL;
    s_justification_graph *replacement_justifications = NULL;
    s_intern *replacement_symbols = NULL;

    if (!facts || !transaction_writer_owned(&facts->tx) || facts->tx.level != 0 || facts->tx.rollback.size != 0)
        return -1;

    /* allocate every replacement before touching the live database. */
    if (set_init_checked(&replacement_index, facts->index.max) != 0)
        goto error;
    replacement_hexastore = new_hexastore();
    if (!replacement_hexastore)
        goto error;
    replacement_justifications = new_justification_graph();
    if (!replacement_justifications)
        goto error;
    if (facts->symbols_delete) {
        replacement_symbols = new_intern(facts->index.max);
        if (!replacement_symbols)
            goto error;
    }

    delete_justification_graph(facts->justifications_staging);
    delete_justification_graph(facts->justifications);
    facts->justifications_staging = NULL;
    facts->justifications_staged_level = 0;
    facts->justifications = replacement_justifications;
    replacement_justifications = NULL;

    /* 1. recycle all facts back into the fact arena pool */
    set_cursor_init(&facts->index, &sc);
    while ((si = set_cursor_next(&sc))) {
        s_fact *fact = (s_fact *)si->data;
        intern_unstring(facts->symbols, fact->s);
        intern_unstring(facts->symbols, fact->p);
        intern_unstring(facts->symbols, fact->o);
        *(void **)fact = facts->fact_free_list;
        facts->fact_free_list = fact;
        facts->fact_free_count++;
    }
    facts_flush_fact_freelist(facts);

    /* 2. replace the hexastore index. */
    delete_hexastore(facts->hexastore);

    facts->hexastore = replacement_hexastore;
    replacement_hexastore = NULL;
    facts->index_spo = facts->hexastore->trie_spo;
    facts->index_pos = facts->hexastore->trie_pos;
    facts->index_osp = facts->hexastore->trie_osp;

    /* 3. replace the facts index hash set. */
    set_destroy(&facts->index);
    facts->index = replacement_index;
    memset(&replacement_index, 0, sizeof(replacement_index));
    facts->retired_facts = NULL;
    facts->retired_count = 0;

    /* 4. replace an owned symbol table; shared tables were unreferenced above. */
    if (facts->symbols_delete) {
        delete_intern(facts->symbols);
        facts->symbols = replacement_symbols;
        replacement_symbols = NULL;
    }

    return 0;

error:
    delete_intern(replacement_symbols);
    delete_justification_graph(replacement_justifications);
    delete_hexastore(replacement_hexastore);
    if (replacement_index.items)
        set_destroy(&replacement_index);
    return -1;
}

s_facts *new_facts(s_intern *symbols, unsigned long max)
{
    s_facts *facts = malloc(sizeof(s_facts));
    if (facts && facts_init_checked(facts, symbols, max) != 0) {
        free(facts);
        facts = NULL;
    }
    return facts;
}

void delete_facts(s_facts *facts)
{
    if (!facts)
        return;
    facts_destroy(facts);
    free(facts);
}

s_set_item *facts_find_symbol(s_facts *facts, const char *string)
{
    int acquired = transaction_acquire_reader(&facts->tx);
    s_set_item *item = intern_find_symbol(facts->symbols, string);
    transaction_release_reader(&facts->tx, acquired);
    return item;
}

int facts_contains_symbol(s_facts *facts, const char *string)
{
    if (!facts || !string)
        return -1;
    int acquired = transaction_acquire_reader(&facts->tx);
    int found = intern_find_symbol(facts->symbols, string) != NULL;
    transaction_release_reader(&facts->tx, acquired);
    return found;
}

Symbol facts_find_symbol_str(s_facts *facts, const char *string)
{
    int acquired = transaction_acquire_reader(&facts->tx);
    Symbol symbol = intern_find_symbol_str(facts->symbols, string);
    transaction_release_reader(&facts->tx, acquired);
    return symbol;
}

const char *facts_long(s_facts *facts, long l)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    const char *value = symbol_to_str(intern_long(facts->symbols, l));
    transaction_release_writer(&facts->tx, has_lock);
    return value;
}

const char *facts_double(s_facts *facts, double d)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    const char *value = symbol_to_str(intern_double(facts->symbols, d));
    transaction_release_writer(&facts->tx, has_lock);
    return value;
}

long facts_get_long(s_facts *facts, const char *string)
{
    int acquired = transaction_acquire_reader(&facts->tx);
    long value = intern_get_long(facts->symbols, string);
    transaction_release_reader(&facts->tx, acquired);
    return value;
}

double facts_get_double(s_facts *facts, const char *string)
{
    int acquired = transaction_acquire_reader(&facts->tx);
    double value = intern_get_double(facts->symbols, string);
    transaction_release_reader(&facts->tx, acquired);
    return value;
}

Symbol facts_intern(s_facts *facts, const char *string)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    Symbol symbol = intern_string(facts->symbols, string);
    transaction_release_writer(&facts->tx, has_lock);
    return symbol;
}

Symbol facts_intern_ref(s_facts *facts, Symbol sym)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    Symbol symbol = intern_ref(facts->symbols, sym);
    transaction_release_writer(&facts->tx, has_lock);
    return symbol;
}

void facts_unintern(s_facts *facts, Symbol sym)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    intern_unstring(facts->symbols, sym);
    transaction_release_writer(&facts->tx, has_lock);
}

void random_id(char *buf, size_t len)
{
    static const char base64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "abcdefghijklmnopqrstuvwxyz"
                                    "0123456789-_";
    uint64_t r = xoshiro256_next();
    size_t bits_left = 64;
    while (len--) {
        if (bits_left < 6) {
            r = xoshiro256_next();
            bits_left = 64;
        }
        *buf++ = base64url[r & 63];
        r >>= 6;
        bits_left -= 6;
    }
}

const char *facts_anon(s_facts *facts, const char *name)
{
    char buf[1024];
    char *b = buf;
    int i = 0;
    assert(facts);
    if (name && name[0] == '?')
        name++;
    if (!name || !name[0])
        name = "anon";
    while ((unsigned)i < sizeof(buf) - 12 && name[i])
        *b++ = name[i++];
    *b++ = '-';
    while (1) {
        random_id(b, 10);
        b[10] = 0;
        if (!facts_contains_symbol(facts, buf))
            return symbol_to_str(facts_intern(facts, buf));
    }
    return NULL;
}

static void fact_set_origin(s_fact *fact, e_fact_origin origin)
{
    fact->asserted_count = origin == FACT_ORIGIN_ASSERTED ? 1 : 0;
    fact->derived_count = origin == FACT_ORIGIN_DERIVED ? 1 : 0;
}

static s_fact *facts_insert_fact_copy(s_facts *facts, const s_fact *f, int record_rollback, int same_intern)
{
    s_fact *new = facts_alloc_fact_locked(facts, f->s, f->p, f->o);
    if (!new)
        return NULL;
    new->asserted_count = f->asserted_count;
    new->derived_count = f->derived_count;
    Symbol s_ref = same_intern ? intern_ref(facts->symbols, new->s) : intern_string(facts->symbols, symbol_to_str(new->s));
    Symbol p_ref = same_intern ? intern_ref(facts->symbols, new->p) : intern_string(facts->symbols, symbol_to_str(new->p));
    Symbol o_ref = same_intern ? intern_ref(facts->symbols, new->o) : intern_string(facts->symbols, symbol_to_str(new->o));
    if (!s_ref || !p_ref || !o_ref) {
        if (s_ref)
            intern_unstring(facts->symbols, s_ref);
        if (p_ref)
            intern_unstring(facts->symbols, p_ref);
        if (o_ref)
            intern_unstring(facts->symbols, o_ref);
        facts_free_fact_locked(facts, new);
        return NULL;
    }
    new->s = s_ref;
    new->p = p_ref;
    new->o = o_ref;
    if (hexastore_insert(facts->hexastore, new) != 0) {
        intern_unstring(facts->symbols, new->s);
        intern_unstring(facts->symbols, new->p);
        intern_unstring(facts->symbols, new->o);
        facts_free_fact_locked(facts, new);
        return NULL;
    }
    s_set_item *index_item = set_add(&facts->index, new, FACT_KEY_SIZE);
    if (!index_item) {
        hexastore_remove(facts->hexastore, new);
        intern_unstring(facts->symbols, new->s);
        intern_unstring(facts->symbols, new->p);
        intern_unstring(facts->symbols, new->o);
        facts_free_fact_locked(facts, new);
        return NULL;
    }
    if (record_rollback && facts_rollback_push(facts, ROLLBACK_REMOVE, new) != 0) {
        set_remove(&facts->index, index_item);
        hexastore_remove(facts->hexastore, new);
        intern_unstring(facts->symbols, new->s);
        intern_unstring(facts->symbols, new->p);
        intern_unstring(facts->symbols, new->o);
        facts_free_fact_locked(facts, new);
        return NULL;
    }
    return new;
}

static s_fact *facts_add_fact_origin_locked(s_facts *facts, const s_fact *f, e_fact_origin origin, int same_intern)
{
    s_fact *found;
    s_fact lookup_key;
    if (same_intern) {
        lookup_key = *f;
    } else {
        lookup_key.s = intern_find_symbol_str(facts->symbols, symbol_to_str(f->s));
        lookup_key.p = intern_find_symbol_str(facts->symbols, symbol_to_str(f->p));
        lookup_key.o = intern_find_symbol_str(facts->symbols, symbol_to_str(f->o));
    }
    s_set_item *si = (lookup_key.s && lookup_key.p && lookup_key.o) ? set_get(&facts->index, &lookup_key, FACT_KEY_SIZE) : NULL;
    if (si) {
        found = (s_fact *)si->data;
        unsigned char *support = origin == FACT_ORIGIN_ASSERTED ? &found->asserted_count : &found->derived_count;
        if (*support == 0) {
            int was_present = found->asserted_count || found->derived_count;
            if (facts_rollback_push(facts, was_present ? ROLLBACK_STATE : ROLLBACK_REMOVE, found) != 0)
                return NULL;
            if (origin == FACT_ORIGIN_ASSERTED && facts_log_change(facts, "add", found) != 0) {
                facts_rollback_pop_discard(facts);
                return NULL;
            }
            facts_set_support_locked(facts, found, origin == FACT_ORIGIN_ASSERTED ? 1 : found->asserted_count,
                                     origin == FACT_ORIGIN_DERIVED ? 1 : found->derived_count);
        }
        return found;
    }
    s_fact copy = *f;
    fact_set_origin(&copy, origin);
    s_fact *new = facts_insert_fact_copy(facts, &copy, 1, same_intern);
    if (new && origin == FACT_ORIGIN_ASSERTED && facts_log_change(facts, "add", new) != 0) {
        facts_set_support_locked(facts, new, 0, 0);
        if (facts->tx.level == 0)
            facts_reclaim_retired(facts);
        new = NULL;
    }
    return new;
}

s_fact *facts_add_fact_origin(s_facts *facts, s_fact *f, e_fact_origin origin)
{
    assert(facts);
    assert(f);
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_fact *res = facts_add_fact_origin_locked(facts, f, origin, 0);
    transaction_release_writer(&facts->tx, has_lock);
    return res;
}

s_fact *facts_add_fact(s_facts *facts, s_fact *f)
{
    return facts_add_fact_origin(facts, f, FACT_ORIGIN_ASSERTED);
}

s_fact *facts_add_spo_origin(s_facts *facts, const char *s, const char *p, const char *o, e_fact_origin origin)
{
    s_fact f = {0};
    assert(facts);
    assert(s);
    assert(p);
    assert(o);
    int has_lock = transaction_acquire_writer(&facts->tx);
    f.s = intern_string(facts->symbols, s);
    f.p = intern_string(facts->symbols, p);
    f.o = intern_string(facts->symbols, o);
    if (!f.s || !f.p || !f.o) {
        if (f.s)
            intern_unstring(facts->symbols, f.s);
        if (f.p)
            intern_unstring(facts->symbols, f.p);
        if (f.o)
            intern_unstring(facts->symbols, f.o);
        transaction_release_writer(&facts->tx, has_lock);
        return NULL;
    }
    fact_set_origin(&f, origin);
    s_fact *ret = facts_add_fact_origin_locked(facts, &f, origin, 1);
    intern_unstring(facts->symbols, f.s);
    intern_unstring(facts->symbols, f.p);
    intern_unstring(facts->symbols, f.o);
    transaction_release_writer(&facts->tx, has_lock);
    return ret;
}

s_fact *facts_add_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    return facts_add_spo_origin(facts, s, p, o, FACT_ORIGIN_ASSERTED);
}

const char **spec_bindings_anon_assoc(s_facts *facts, p_spec spec)
{
    const char **b;
    const char **bindings;
    size_t count;
    assert(spec);
    count = spec_count_bindings(spec);
    if (count > (SIZE_MAX - 1) / 2 || count * 2 + 1 > SIZE_MAX / sizeof(char *))
        return NULL;
    bindings = calloc(count * 2 + 1, sizeof(char *));
    if (bindings) {
        size_t s = 0;
        b = bindings;
        while (spec[s] || spec[s + 1]) {
            if (spec[s] && spec[s][0] == '?') {
                *b++ = spec[s];
                *b = facts_anon(facts, spec[s]);
                if (!*b) {
                    free(bindings);
                    return NULL;
                }
                b++;
            }
            s++;
        }
        *b = NULL;
    }
    return bindings;
}

const char *assoc_get(const char **kv, const char *k)
{
    assert(kv);
    assert(k);
    while (*kv && strcmp(*kv, k))
        kv += 2;
    if (*kv)
        return kv[1];
    return NULL;
}

int facts_add(s_facts *facts, p_spec spec)
{
    const char **anon;
    s_spec_cursor c;
    s_spec_fact f;
    assert(facts);
    assert(spec);
    int has_lock = transaction_acquire_writer(&facts->tx);
    anon = spec_bindings_anon_assoc(facts, spec);
    if (!anon) {
        transaction_release_writer(&facts->tx, has_lock);
        return -1;
    }
    spec_cursor_init(&c, spec);
    while (spec_cursor_next(&c, &f)) {
        if (f.negated)
            continue;
        if (f.s[0] == '?')
            f.s = assoc_get(anon, f.s);
        if (f.p[0] == '?')
            f.p = assoc_get(anon, f.p);
        if (f.o[0] == '?')
            f.o = assoc_get(anon, f.o);

        if (!facts_add_spo(facts, f.s, f.p, f.o)) {
            free(anon);
            transaction_release_writer(&facts->tx, has_lock);
            return -1;
        }
    }
    free(anon);
    transaction_release_writer(&facts->tx, has_lock);
    return 0;
}

static int facts_remove_found(s_facts *facts, s_set_item *si, s_fact *found)
{
    if (!found->asserted_count && !found->derived_count) {
        assert(facts->retired_count > 0);
        facts->retired_count--;
    }
    set_remove(&facts->index, si);
    hexastore_remove(facts->hexastore, found);
    intern_unstring(facts->symbols, found->s);
    intern_unstring(facts->symbols, found->p);
    intern_unstring(facts->symbols, found->o);
    facts_free_fact_locked(facts, found);
    return 0;
}

void facts_reclaim_retired(s_facts *facts)
{
    s_fact *fact = facts->retired_facts;
    facts->retired_facts = NULL;
    while (fact) {
        s_fact *next = fact->retired_next;
        fact->retired_next = NULL;
        fact->retirement_queued = 0;
        if (!fact->asserted_count && !fact->derived_count) {
            s_set_item *item = set_get(&facts->index, fact, FACT_KEY_SIZE);
            assert(item != NULL);
            facts_remove_found(facts, item, fact);
        }
        fact = next;
    }
    assert(facts->retired_count == 0);
}

int facts_remove_fact_origin(s_facts *facts, s_fact *f, e_fact_origin origin)
{
    s_fact *found;
    assert(facts);
    assert(f);
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_set_item *si = set_get(&facts->index, f, FACT_KEY_SIZE);
    if (!si) {
        s_fact lookup_key = {
            .s = intern_find_symbol_str(facts->symbols, symbol_to_str(f->s)),
            .p = intern_find_symbol_str(facts->symbols, symbol_to_str(f->p)),
            .o = intern_find_symbol_str(facts->symbols, symbol_to_str(f->o)),
        };
        if (lookup_key.s && lookup_key.p && lookup_key.o)
            si = set_get(&facts->index, &lookup_key, FACT_KEY_SIZE);
    }
    if (si) {
        found = (s_fact *)si->data;
        unsigned char *support = origin == FACT_ORIGIN_ASSERTED ? &found->asserted_count : &found->derived_count;
        if (*support == 0) {
            transaction_release_writer(&facts->tx, has_lock);
            return 0;
        }
        e_rollback_action action = found->asserted_count + found->derived_count > 1 ? ROLLBACK_STATE : ROLLBACK_ADD;
        if (facts_rollback_push(facts, action, found) != 0) {
            transaction_release_writer(&facts->tx, has_lock);
            return -1;
        }
        if (origin == FACT_ORIGIN_ASSERTED && facts_log_change(facts, "remove", found) != 0) {
            facts_rollback_pop_discard(facts);
            transaction_release_writer(&facts->tx, has_lock);
            return -1;
        }
        if (action == ROLLBACK_STATE) {
            *support = 0;
        } else if (facts->tx.level > 0) {
            /* retain index paths and the fact until commit, so undo cannot fail. */
            facts_set_support_locked(facts, found, 0, 0);
        } else {
            facts_remove_found(facts, si, found);
        }
        transaction_release_writer(&facts->tx, has_lock);
        return 1;
    }
    transaction_release_writer(&facts->tx, has_lock);
    return 0;
}

int facts_remove_fact(s_facts *facts, s_fact *f)
{
    return facts_remove_fact_origin(facts, f, FACT_ORIGIN_ASSERTED);
}

int facts_remove_spo_origin(s_facts *facts, const char *s, const char *p, const char *o, e_fact_origin origin)
{
    if (!facts || !s || !p || !o)
        return -1;
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_fact *fact = facts_lookup_spo_locked(facts, s, p, o);
    int result = fact ? facts_remove_fact_origin(facts, fact, origin) : 0;
    transaction_release_writer(&facts->tx, has_lock);
    return result;
}

int facts_remove_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    return facts_remove_spo_origin(facts, s, p, o, FACT_ORIGIN_ASSERTED);
}

int facts_get_support_spo(s_facts *facts, const char *s, const char *p, const char *o, s_fact_support *support)
{
    if (!facts || !s || !p || !o || !support)
        return -1;
    support->asserted = 0;
    support->derived = 0;
    int acquired = transaction_acquire_reader(&facts->tx);
    Symbol s_sym = intern_find_symbol_str(facts->symbols, s);
    Symbol p_sym = intern_find_symbol_str(facts->symbols, p);
    Symbol o_sym = intern_find_symbol_str(facts->symbols, o);
    int found = 0;
    if (s_sym && p_sym && o_sym) {
        s_fact key = {.s = s_sym, .p = p_sym, .o = o_sym};
        s_set_item *si = set_get(&facts->index, &key, FACT_KEY_SIZE);
        if (si) {
            s_fact *fact = (s_fact *)si->data;
            support->asserted = fact->asserted_count;
            support->derived = fact->derived_count;
            found = support->asserted || support->derived;
        }
    }
    transaction_release_reader(&facts->tx, acquired);
    return found;
}

static s_fact *facts_lookup_spo_locked(s_facts *facts, const char *s, const char *p, const char *o)
{
    Symbol s_sym = intern_find_symbol_str(facts->symbols, s);
    Symbol p_sym = intern_find_symbol_str(facts->symbols, p);
    Symbol o_sym = intern_find_symbol_str(facts->symbols, o);
    if (!s_sym || !p_sym || !o_sym)
        return NULL;
    s_fact key = {.s = s_sym, .p = p_sym, .o = o_sym};
    s_set_item *si = set_get(&facts->index, &key, FACT_KEY_SIZE);
    s_fact *fact = si ? (s_fact *)si->data : NULL;
    return fact && (fact->asserted_count || fact->derived_count) ? fact : NULL;
}

int facts_contains_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    if (!facts || !s || !p || !o)
        return -1;
    int acquired = transaction_acquire_reader(&facts->tx);
    int found = facts_lookup_spo_locked(facts, s, p, o) != NULL;
    transaction_release_reader(&facts->tx, acquired);
    return found;
}

void facts_snapshot_destroy(s_fact_snapshot *snapshot)
{
    if (!snapshot)
        return;
    free(snapshot->s);
    free(snapshot->p);
    free(snapshot->o);
    memset(snapshot, 0, sizeof(*snapshot));
}

int facts_get_spo_snapshot(s_facts *facts, const char *s, const char *p, const char *o, s_fact_snapshot *snapshot)
{
    if (!facts || !s || !p || !o || !snapshot)
        return -1;
    memset(snapshot, 0, sizeof(*snapshot));
    int acquired = transaction_acquire_reader(&facts->tx);
    s_fact *fact = facts_lookup_spo_locked(facts, s, p, o);
    if (!fact) {
        transaction_release_reader(&facts->tx, acquired);
        return 0;
    }
    snapshot->s = strdup(symbol_to_str(fact->s));
    snapshot->p = strdup(symbol_to_str(fact->p));
    snapshot->o = strdup(symbol_to_str(fact->o));
    snapshot->support.asserted = fact->asserted_count;
    snapshot->support.derived = fact->derived_count;
    transaction_release_reader(&facts->tx, acquired);
    if (!snapshot->s || !snapshot->p || !snapshot->o) {
        facts_snapshot_destroy(snapshot);
        return -1;
    }
    return 1;
}

int facts_remove(s_facts *facts, p_spec spec)
{
    if (!facts || !spec)
        return -1;
    s_facts_with_cursor wc;
    s_binding *bindings;
    s_fact_list *fl = NULL;
    s_fact_list *fli;
    int found = 0;
    int has_lock = transaction_acquire_writer(&facts->tx);
    bindings = spec_bindings(spec);
    if (!bindings || facts_with_checked(facts, bindings, &wc, spec) != 0) {
        free(bindings);
        transaction_release_writer(&facts->tx, has_lock);
        return -1;
    }
    int error = 0;
    while (facts_with_cursor_next(&wc)) {
        s_spec_fact f;
        s_spec_cursor sc;
        spec_cursor_init(&sc, spec);
        while (spec_cursor_next(&sc, &f)) {
            if (f.negated)
                continue;
            s_fact *dbf;
            spec_fact_bindings_resolve(&f, bindings);
            dbf = facts_get_spo(facts, f.s, f.p, f.o);
            if (dbf && !fact_list_find(fl, dbf)) {
                s_fact_list *next = new_fact_list(dbf, fl);
                if (!next) {
                    error = 1;
                    break;
                }
                fl = next;
            }
        }
        if (error)
            break;
    }
    if (facts_with_cursor_error(&wc))
        error = 1;
    facts_with_cursor_destroy(&wc);
    free(bindings);
    fli = error ? NULL : fl;
    while (fli) {
        int removed = facts_remove_fact(facts, fli->fact);
        if (removed < 0) {
            error = 1;
            break;
        }
        if (removed)
            found = 1;
        fli = fli->next;
    }
    delete_fact_list(fl);
    transaction_release_writer(&facts->tx, has_lock);
    return error ? -1 : found;
}

s_fact *facts_get_fact(s_facts *facts, s_fact *f)
{
    assert(facts);
    assert(f);
    int acquired = transaction_acquire_reader(&facts->tx);
    s_set_item *si = set_get(&facts->index, f, FACT_KEY_SIZE);
    if (!si) {
        s_fact lookup_key = {
            .s = intern_find_symbol_str(facts->symbols, symbol_to_str(f->s)),
            .p = intern_find_symbol_str(facts->symbols, symbol_to_str(f->p)),
            .o = intern_find_symbol_str(facts->symbols, symbol_to_str(f->o)),
        };
        if (lookup_key.s && lookup_key.p && lookup_key.o)
            si = set_get(&facts->index, &lookup_key, FACT_KEY_SIZE);
    }
    s_fact *result = si ? (s_fact *)si->data : NULL;
    if (result && !result->asserted_count && !result->derived_count)
        result = NULL;
    transaction_release_reader(&facts->tx, acquired);
    return result;
}

s_fact *facts_get_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    if (!facts || !s || !p || !o)
        return NULL;
    int acquired = transaction_acquire_reader(&facts->tx);
    s_fact *result = facts_lookup_spo_locked(facts, s, p, o);
    transaction_release_reader(&facts->tx, acquired);
    return result;
}

unsigned long facts_count(s_facts *facts)
{
    assert(facts);
    int acquired = transaction_acquire_reader(&facts->tx);
    unsigned long count = facts->index.count - facts->retired_count;
    transaction_release_reader(&facts->tx, acquired);
    return count;
}

void facts_cursor_init(s_facts *facts, s_facts_cursor *c, rax *tree, s_fact *start, s_fact *end)
{
    assert(facts);
    assert(c);
    assert(tree);

    c->facts = facts;
    c->locked = transaction_acquire_reader(&facts->tx);
    c->index_type = 0;
    c->error = 0;
    if (tree == facts->index_pos) {
        c->index_type = 1;
    } else if (tree == facts->index_osp) {
        c->index_type = 2;
    }

    raxStart(&c->it, tree);

    unsigned char start_key[24];
    s_fact resolved_start, resolved_end;

    if (start) {
        resolved_start = *start;
    } else {
        resolved_start.s = P_FIRST;
        resolved_start.p = P_FIRST;
        resolved_start.o = P_FIRST;
    }

    if (end) {
        resolved_end = *end;
    } else {
        resolved_end.s = P_LAST;
        resolved_end.p = P_LAST;
        resolved_end.o = P_LAST;
    }

    encode_cursor_key(start_key, &resolved_start, c->index_type);
    encode_cursor_key(c->end_key, &resolved_end, c->index_type);

    if (!raxSeek(&c->it, ">=", start_key, 24))
        c->error = -1;
    c->it.flags &= ~RAX_ITER_JUST_SEEKED;

    c->started = 0;
    c->var_s = NULL;
    c->var_p = NULL;
    c->var_o = NULL;
}

s_fact *facts_cursor_next(s_facts_cursor *c)
{
    assert(c);
    if (c->started == 2) {
        return NULL;
    }
    while (!c->error) {
        if (c->started == 0) {
            c->started = 1;
        } else if (!raxEOF(&c->it) && !raxNext(&c->it) && errno == ENOMEM) {
            c->error = -1;
            break;
        }
        if (raxEOF(&c->it) || memcmp(c->it.key, c->end_key, 24) > 0)
            break;
        s_fact *f = (s_fact *)c->it.data;
        if ((!f->asserted_count && !f->derived_count) || (c->var_s && c->var_s == c->var_p && f->s != f->p) ||
            (c->var_s && c->var_s == c->var_o && f->s != f->o) || (c->var_p && c->var_p == c->var_o && f->p != f->o))
            continue;
        if (c->var_s)
            *c->var_s = symbol_to_str(f->s);
        if (c->var_p)
            *c->var_p = symbol_to_str(f->p);
        if (c->var_o)
            *c->var_o = symbol_to_str(f->o);
        return f;
    }

    facts_cursor_stop(c);

    if (c->var_s)
        *c->var_s = NULL;
    if (c->var_p)
        *c->var_p = NULL;
    if (c->var_o)
        *c->var_o = NULL;
    return NULL;
}

void facts_cursor_stop(s_facts_cursor *c)
{
    if (c && c->started != 2) {
        raxStop(&c->it);
        c->started = 2;
        transaction_release_reader(&c->facts->tx, c->locked);
        c->locked = 0;
    }
}

int facts_cursor_error(const s_facts_cursor *c)
{
    return c ? c->error : -1;
}

void facts_with_3(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o)
{
    s_fact f;
    assert(facts);
    assert(c);
    assert(s);
    assert(p);
    assert(o);
    int acquired = transaction_acquire_reader(&facts->tx);
    Symbol interned_s = intern_find_symbol_str(facts->symbols, s);
    Symbol interned_p = intern_find_symbol_str(facts->symbols, p);
    Symbol interned_o = intern_find_symbol_str(facts->symbols, o);
    if (!interned_s || !interned_p || !interned_o) {
        facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
        facts_cursor_stop(c);
        transaction_release_reader(&facts->tx, acquired);
        return;
    }
    f.s = interned_s;
    f.p = interned_p;
    f.o = interned_o;
    facts_cursor_init(facts, c, facts->index_spo, &f, &f);
    transaction_release_reader(&facts->tx, acquired);
}

void facts_with_0(s_facts *facts, s_facts_cursor *c, const char **var_s, const char **var_p, const char **var_o)
{
    assert(facts);
    assert(c);
    int acquired = transaction_acquire_reader(&facts->tx);
    facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
    c->var_s = var_s;
    c->var_p = var_p;
    c->var_o = var_o;
    transaction_release_reader(&facts->tx, acquired);
}

void facts_with_1_2(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o, const char **var_s,
                    const char **var_p, const char **var_o)
{
    s_fact start = {0};
    s_fact end = {0};
    struct rax *tree;
    assert(facts);
    assert(c);
    assert(s);
    assert(p);
    assert(o);
    assert(var_s || var_p || var_o);
    int acquired = transaction_acquire_reader(&facts->tx);
    Symbol interned_s = var_s ? NULL : intern_find_symbol_str(facts->symbols, s);
    Symbol interned_p = var_p ? NULL : intern_find_symbol_str(facts->symbols, p);
    Symbol interned_o = var_o ? NULL : intern_find_symbol_str(facts->symbols, o);
    if ((!var_s && !interned_s) || (!var_p && !interned_p) || (!var_o && !interned_o)) {
        facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
        facts_cursor_stop(c);
        transaction_release_reader(&facts->tx, acquired);
        return;
    }
    start.s = var_s ? P_FIRST : interned_s;
    start.p = var_p ? P_FIRST : interned_p;
    start.o = var_o ? P_FIRST : interned_o;
    end.s = var_s ? P_LAST : interned_s;
    end.p = var_p ? P_LAST : interned_p;
    end.o = var_o ? P_LAST : interned_o;
    tree = (!var_s && var_o) ? facts->index_spo : !var_p ? facts->index_pos : facts->index_osp;
    facts_cursor_init(facts, c, tree, &start, &end);
    c->var_s = var_s;
    c->var_p = var_p;
    c->var_o = var_o;
    transaction_release_reader(&facts->tx, acquired);
}

void facts_with_spo(s_facts *facts, s_binding *bindings, s_facts_cursor *c, const char *s, const char *p, const char *o)
{
    const char **var_s;
    const char **var_p;
    const char **var_o;
    assert(facts);
    assert(c);
    assert(s);
    assert(p);
    assert(o);
    var_s = (s[0] == '?') ? bindings_get(bindings, s) : NULL;
    var_p = (p[0] == '?') ? bindings_get(bindings, p) : NULL;
    var_o = (o[0] == '?') ? bindings_get(bindings, o) : NULL;
    if (var_s && var_p && var_o)
        facts_with_0(facts, c, var_s, var_p, var_o);
    else if (!(var_s || var_p || var_o))
        facts_with_3(facts, c, s, p, o);
    else
        facts_with_1_2(facts, c, s, p, o, var_s, var_p, var_o);
}

void facts_with(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, p_spec spec)
{
    (void)facts_with_checked(facts, bindings, c, spec);
}

static void facts_spec_sort_masked(s_facts *facts, p_spec spec, size_t count, unsigned char *variable_masks);

static int facts_with_initialize(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, p_spec expanded,
                                 const unsigned char *variable_masks, size_t facts_count)
{
    if (!facts || !c) {
        free(expanded);
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->locked = transaction_acquire_reader(&facts->tx);
    c->facts = facts;
    c->bindings = bindings;
    c->spec = expanded;
    bindings_nullify(c->bindings);
    c->facts_count = facts_count;
    if (facts_count > 0) {
        if (facts_count > SIZE_MAX / sizeof(s_facts_with_cursor_level) || facts_count > SIZE_MAX / 6)
            goto allocation_error;
        size_t total_ptrs = facts_count * 6;
        size_t levels_sz = facts_count * sizeof(s_facts_with_cursor_level);
        if (total_ptrs > SIZE_MAX / sizeof(const char *))
            goto allocation_error;
        size_t specs_sz = total_ptrs * sizeof(const char *);
        if (levels_sz > SIZE_MAX - specs_sz)
            goto allocation_error;
        char *mem = calloc(1, levels_sz + specs_sz);
        if (!mem)
            goto allocation_error;
        c->l = (s_facts_with_cursor_level *)mem;
        c->allocated_levels = facts_count;
        const char **spec_ptr = (const char **)(mem + levels_sz);
        c->variable_masks = malloc(facts_count);
        if (!c->variable_masks)
            goto allocation_error;
        for (size_t i = 0; i < facts_count; i++) {
            unsigned char mask = 0;
            for (size_t t = 0; t < 3; t++) {
                if (variable_masks ? (variable_masks[i] & (1u << t)) : c->spec[i * 4 + t][0] == '?') {
                    if (!bindings_get(bindings, c->spec[i * 4 + t]))
                        goto allocation_error;
                    mask |= 1u << t;
                }
            }
            c->variable_masks[i] = mask;
        }
        facts_spec_sort_masked(facts, c->spec, facts_count, c->variable_masks);
        for (size_t i = 0; i < facts_count; i++) {
            c->l[i].spec = spec_ptr;
            spec_ptr += 6;
            c->l[i].spec_init = 0;
        }
    }
    c->level = 0;
    c->limit = -1;
    c->offset = 0;
    c->result_count = 0;
    c->is_sorted = 0;
    c->sort_var = NULL;
    c->sort_desc = 0;
    c->sorted_matches = NULL;
    c->sorted_count = 0;
    c->sorted_pos = 0;
    c->error = 0;
    return 0;

allocation_error:
    facts_with_cursor_destroy(c);
    c->error = -1;
    return -1;
}

int facts_with_checked(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, p_spec spec)
{
    if (!facts || !c || !spec)
        return -1;
    memset(c, 0, sizeof(*c));
    c->error = -1;
    size_t count = spec_count_facts(spec);
    p_spec expanded = count ? spec_expand(spec) : NULL;
    if (count && !expanded)
        return -1;
    return facts_with_initialize(facts, bindings, c, expanded, NULL, count);
}

int facts_with_typed_checked(s_facts *facts, s_binding *bindings, s_facts_with_cursor *c, const s_spec_fact *triples,
                             const unsigned char *variable_masks, size_t count)
{
    if (!facts || !c || (count && (!triples || !variable_masks)))
        return -1;
    memset(c, 0, sizeof(*c));
    c->error = -1;
    p_spec expanded = spec_copy_expanded(triples, count);
    if (!expanded)
        return -1;
    return facts_with_initialize(facts, bindings, c, expanded, variable_masks, count);
}

int facts_with_cursor_error(const s_facts_with_cursor *c)
{
    return c ? c->error : -1;
}

void facts_with_cursor_destroy(s_facts_with_cursor *c)
{
    assert(c);
    if (c->l) {
        for (size_t i = 0; i < c->allocated_levels; i++) {
            if (c->l[i].spec_init) {
                facts_cursor_stop(&c->l[i].c);
            }
        }
        free(c->l);
    }
    if (c->spec)
        free(c->spec);
    free(c->variable_masks);
    if (c->locked && c->facts) {
        transaction_release_reader(&c->facts->tx, c->locked);
    }
    if (c->sorted_matches) {
        typedef struct {
            const char **values;
        } s_cached_match_local;
        s_cached_match_local *matches = (s_cached_match_local *)c->sorted_matches;
        for (size_t i = 0; i < c->sorted_count; i++) {
            free(matches[i].values);
        }
        free(matches);
    }
    if (c->sort_var) {
        free(c->sort_var);
    }
    free(c->saved_values);
    c->facts = NULL;
    c->bindings = NULL;
    c->facts_count = 0;
    c->allocated_levels = 0;
    c->l = NULL;
    c->level = 0;
    c->spec = NULL;
    c->variable_masks = NULL;
    c->locked = 0;
    c->sorted_matches = NULL;
    c->sort_var = NULL;
    c->sorted_count = 0;
    c->saved_values = NULL;
}

void spec_subst(p_spec spec, s_binding *bindings)
{
    size_t i = 0;
    if (spec && spec[0])
        while (spec[i] || spec[i + 1]) {
            if (spec[i] && spec[i][0] == '?') {
                const char **b = bindings_get(bindings, spec[i]);
                if (b && *b)
                    spec[i] = *b;
            }
            i++;
        }
}

int facts_with_cursor_next_inner(s_facts_with_cursor *c);

typedef struct {
    const char **values;
} s_cached_match;

static void swap_matches(s_cached_match *a, s_cached_match *b)
{
    s_cached_match tmp = *a;
    *a = *b;
    *b = tmp;
}

static int compare_matches(const s_cached_match *a, const s_cached_match *b, int idx, int desc)
{
    const char *val_a = a->values[idx];
    const char *val_b = b->values[idx];
    if (!val_a && !val_b)
        return 0;
    if (!val_a)
        return desc ? 1 : -1;
    if (!val_b)
        return desc ? -1 : 1;
    int cmp = strcmp(val_a, val_b);
    return desc ? -cmp : cmp;
}

static void sift_down_matches(s_cached_match *arr, size_t root, size_t count, int idx, int desc)
{
    if (count < 2)
        return;
    while (root <= (count - 2) / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare_matches(&arr[child], &arr[child + 1], idx, desc) < 0)
            child++;
        if (compare_matches(&arr[root], &arr[child], idx, desc) >= 0)
            return;
        swap_matches(&arr[root], &arr[child]);
        root = child;
    }
}

static void heapsort_matches(s_cached_match *arr, size_t count, int idx, int desc)
{
    if (count < 2)
        return;
    for (size_t start = count / 2; start > 0; start--)
        sift_down_matches(arr, start - 1, count, idx, desc);
    for (size_t end = count - 1; end > 0; end--) {
        swap_matches(&arr[0], &arr[end]);
        sift_down_matches(arr, 0, end, idx, desc);
    }
}

int facts_with_cursor_next_inner(s_facts_with_cursor *c);

int facts_with_cursor_next(s_facts_with_cursor *c)
{
    if (!c || c->error)
        return 0;
    if (c->is_sorted) {
        if (!c->sorted_matches && c->sorted_pos == 0) {
            size_t capacity = 16;
            c->sorted_matches = malloc(capacity * sizeof(s_cached_match));
            if (!c->sorted_matches) {
                c->error = -1;
                return 0;
            }
            c->sorted_count = 0;

            int bindings_count = 0;
            while (c->bindings && c->bindings[bindings_count].name) {
                bindings_count++;
            }

            long orig_limit = c->limit;
            long orig_offset = c->offset;
            c->limit = -1;
            c->offset = 0;

            c->is_sorted = 0;
            while (facts_with_cursor_next(c)) {
                if (c->sorted_count >= capacity) {
                    if (capacity > SIZE_MAX / 2 || capacity * 2 > SIZE_MAX / sizeof(s_cached_match)) {
                        c->error = -1;
                        break;
                    }
                    capacity *= 2;
                    void *resized = realloc(c->sorted_matches, capacity * sizeof(s_cached_match));
                    if (!resized) {
                        c->error = -1;
                        break;
                    }
                    c->sorted_matches = resized;
                }
                s_cached_match *matches = (s_cached_match *)c->sorted_matches;
                matches[c->sorted_count].values = bindings_count ? malloc((size_t)bindings_count * sizeof(const char *)) : NULL;
                if (bindings_count && !matches[c->sorted_count].values) {
                    c->error = -1;
                    break;
                }
                for (int i = 0; i < bindings_count; i++) {
                    matches[c->sorted_count].values[i] = *c->bindings[i].value;
                }
                c->sorted_count++;
            }
            c->is_sorted = 1;

            c->limit = orig_limit;
            c->offset = orig_offset;
            c->result_count = 0;

            if (c->error)
                return 0;

            int sort_idx = -1;
            if (c->sort_var) {
                for (int i = 0; i < bindings_count; i++) {
                    if (strcmp(c->bindings[i].name, c->sort_var) == 0) {
                        sort_idx = i;
                        break;
                    }
                }
            }

            if (sort_idx >= 0 && c->sorted_count > 1) {
                heapsort_matches((s_cached_match *)c->sorted_matches, c->sorted_count, sort_idx, c->sort_desc);
            }

            c->sorted_pos = 0;
        }

        while (c->offset > 0) {
            c->offset--;
            if (c->sorted_pos < c->sorted_count) {
                c->sorted_pos++;
            } else {
                return 0;
            }
        }

        if (c->limit >= 0 && c->result_count >= c->limit) {
            return 0;
        }

        if (c->sorted_pos < c->sorted_count) {
            s_cached_match *matches = (s_cached_match *)c->sorted_matches;
            int bindings_count = 0;
            while (c->bindings && c->bindings[bindings_count].name) {
                bindings_count++;
            }
            if (!matches || (bindings_count > 0 && !matches[c->sorted_pos].values)) {
                c->error = -1;
                return 0;
            }
            for (int i = 0; i < bindings_count; i++) {
                *c->bindings[i].value = matches[c->sorted_pos].values[i];
            }
            c->sorted_pos++;
            c->result_count++;
            return 1;
        }
        return 0;
    }

    if (c->limit >= 0 && c->result_count >= c->limit) {
        return 0;
    }

    while (c->offset > 0) {
        c->offset--;
        if (!facts_with_cursor_next_inner(c)) {
            return 0;
        }
    }

    int ok = facts_with_cursor_next_inner(c);
    if (ok) {
        c->result_count++;
    }
    return ok;
}

static int cursor_level_is_negated(const s_facts_with_cursor *c, size_t level)
{
    const char *neg = c->spec[level * 4 + 3];
    return neg && strcmp(neg, ":not") == 0;
}

static int cursor_backtrack_to_generator(s_facts_with_cursor *c, size_t cur)
{
    while (cur > 0) {
        cur--;
        s_facts_with_cursor_level *prev = &c->l[cur];
        if (cursor_level_is_negated(c, cur)) {
            if (prev->spec_init) {
                facts_cursor_stop(&prev->c);
                prev->spec_init = 0;
            }
            continue;
        }
        c->level = cur;
        return 1;
    }
    c->facts_count = 0;
    return 0;
}

int facts_with_cursor_next_inner(s_facts_with_cursor *c)
{
    assert(c);
    if (!c->facts_count)
        return 0;
    if (c->level == c->facts_count) {
        size_t last = c->facts_count - 1;
        s_facts_with_cursor_level *l = c->l + last;
        if (cursor_level_is_negated(c, last)) {
            if (l->spec_init) {
                facts_cursor_stop(&l->c);
                l->spec_init = 0;
            }
            if (!cursor_backtrack_to_generator(c, last))
                return 0;
        } else {
            l->fact = facts_cursor_next(&l->c);
            if (facts_cursor_error(&l->c)) {
                c->error = -1;
                return 0;
            }
            if (l->fact)
                return 1;
            facts_cursor_stop(&l->c);
            l->spec_init = 0;
            if (!cursor_backtrack_to_generator(c, last))
                return 0;
        }
    }
    while (c->level < c->facts_count) {
        s_facts_with_cursor_level *l = c->l + c->level;
        if (!l->spec_init) {
            memcpy(l->spec, c->spec + c->level * 4, 4 * sizeof(const char *));
            l->spec[4] = NULL;
            l->spec[5] = NULL;
            const char **slots[3] = {NULL, NULL, NULL};
            for (size_t t = 0; t < 3; t++) {
                if (c->variable_masks[c->level] & (1u << t)) {
                    slots[t] = bindings_get(c->bindings, l->spec[t]);
                    if (*slots[t]) {
                        l->spec[t] = *slots[t];
                        slots[t] = NULL;
                    }
                }
            }
            if (slots[0] && slots[1] && slots[2])
                facts_with_0(c->facts, &l->c, slots[0], slots[1], slots[2]);
            else if (slots[0] || slots[1] || slots[2])
                facts_with_1_2(c->facts, &l->c, l->spec[0], l->spec[1], l->spec[2], slots[0], slots[1], slots[2]);
            else
                facts_with_3(c->facts, &l->c, l->spec[0], l->spec[1], l->spec[2]);
            l->spec_init = 1;
            if (facts_cursor_error(&l->c)) {
                c->error = -1;
                return 0;
            }
        }

        if (cursor_level_is_negated(c, c->level)) {
            /* negated level: save bindings to isolate from variable matching */
            size_t bindings_count = 0;
            while (c->bindings && c->bindings[bindings_count].name)
                bindings_count++;
            if (bindings_count > 0) {
                if (!c->saved_values) {
                    c->saved_values = malloc(bindings_count * sizeof(const char *));
                    if (!c->saved_values) {
                        facts_cursor_stop(&l->c);
                        l->spec_init = 0;
                        c->error = -1;
                        c->facts_count = 0;
                        return 0;
                    }
                }
                for (size_t i = 0; i < bindings_count; i++) {
                    c->saved_values[i] = *c->bindings[i].value;
                }
            }

            s_fact *found_fact = facts_cursor_next(&l->c);
            int cur_err = facts_cursor_error(&l->c);
            facts_cursor_stop(&l->c);
            l->spec_init = 0;

            /* restore bindings */
            for (size_t i = 0; i < bindings_count; i++) {
                *c->bindings[i].value = c->saved_values[i];
            }

            if (cur_err) {
                c->error = -1;
                return 0;
            }

            if (found_fact) {
                /* fact exists in db, negation fails: backtrack to generator */
                if (!cursor_backtrack_to_generator(c, c->level))
                    return 0;
            } else {
                /* fact does not exist, negation succeeds: advance */
                c->level++;
            }
        } else {
            /* positive level */
            l->fact = facts_cursor_next(&l->c);
            if (facts_cursor_error(&l->c)) {
                c->error = -1;
                return 0;
            }
            if (l->fact) {
                c->level++;
            } else {
                facts_cursor_stop(&l->c);
                l->spec_init = 0;
                if (!cursor_backtrack_to_generator(c, c->level))
                    return 0;
            }
        }
    }
    return 1;
}

const char *facts_get_prop(s_facts *facts, const char *s, const char *p)
{
    const char *o = NULL;
    s_binding bindings[] = {{"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    facts_with_spo(facts, bindings, &c, s, p, "?o");
    if (facts_cursor_next(&c)) {
        facts_cursor_stop(&c);
        return o;
    }
    return NULL;
}

int facts_get_prop_copy(s_facts *facts, const char *s, const char *p, char **value)
{
    if (!facts || !s || !p || !value)
        return -1;
    *value = NULL;
    const char *o = NULL;
    s_binding bindings[] = {{"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    facts_with_spo(facts, bindings, &c, s, p, "?o");
    int found = facts_cursor_next(&c) != NULL;
    if (found)
        *value = strdup(o);
    facts_cursor_stop(&c);
    return found && !*value ? -1 : found;
}

long facts_get_prop_long(s_facts *facts, const char *s, const char *p)
{
    const char *o = NULL;
    s_binding bindings[] = {{"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    facts_with_spo(facts, bindings, &c, s, p, "?o");
    long value = facts_cursor_next(&c) ? strtol(o, NULL, 10) : 0;
    facts_cursor_stop(&c);
    return value;
}

double facts_get_prop_double(s_facts *facts, const char *s, const char *p)
{
    const char *o = NULL;
    s_binding bindings[] = {{"?o", &o}, {NULL, NULL}};
    s_facts_cursor c;
    facts_with_spo(facts, bindings, &c, s, p, "?o");
    double value = facts_cursor_next(&c) ? strtod(o, NULL) : 0.0;
    facts_cursor_stop(&c);
    return value;
}

s_fact *facts_set_prop(s_facts *facts, const char *s, const char *p, const char *o)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    if (facts_remove(facts, (const char *[]){s, p, "?o", NULL, NULL}) < 0) {
        transaction_release_writer(&facts->tx, has_lock);
        return NULL;
    }
    s_fact *ret = facts_add_spo(facts, s, p, o);
    transaction_release_writer(&facts->tx, has_lock);
    return ret;
}

int facts_rollback_push(s_facts *facts, e_rollback_action action, const s_fact *fact)
{
    return transaction_rollback_push(facts, &facts->tx, action, fact);
}

int facts_transaction_begin(s_facts *facts)
{
    return transaction_begin(facts, &facts->tx);
}

int facts_transaction_commit(s_facts *facts)
{
    return transaction_commit(facts, &facts->tx);
}

int facts_transaction_rollback(s_facts *facts)
{
    return transaction_rollback(facts, &facts->tx);
}

int facts_read_begin(s_facts *facts, s_facts_read_guard *guard)
{
    if (!guard)
        return -1;
    guard->facts = NULL;
    guard->acquired = 0;
    if (!facts)
        return -1;
    /* A read guard cannot outlive a write transaction owned by this thread. */
    if (transaction_writer_owned(&facts->tx))
        return -1;
    guard->facts = facts;
    guard->acquired = transaction_acquire_reader(&facts->tx);
    return 0;
}

void facts_read_end(s_facts_read_guard *guard)
{
    if (!guard || !guard->facts)
        return;
    transaction_release_reader(&guard->facts->tx, guard->acquired);
    guard->facts = NULL;
    guard->acquired = 0;
}

void facts_register_tx_listener(s_facts *facts, f_facts_tx_listener listener, void *user_data)
{
    assert(facts);
    pthread_mutex_lock(&facts->tx.state_mutex);
    facts->tx.listener = listener;
    facts->tx.listener_data = user_data;
    pthread_mutex_unlock(&facts->tx.state_mutex);
}

void facts_register_internal_tx_listener(s_facts *facts, f_facts_tx_listener listener, void *user_data)
{
    assert(facts);
    pthread_mutex_lock(&facts->tx.state_mutex);
    facts->tx.internal_listener = listener;
    facts->tx.internal_listener_data = user_data;
    pthread_mutex_unlock(&facts->tx.state_mutex);
}

void facts_register_commit_observer(s_facts *facts, f_facts_commit_observer observer, void *user_data)
{
    assert(facts);
    pthread_mutex_lock(&facts->tx.state_mutex);
    facts->tx.commit_observer = observer;
    facts->tx.commit_observer_data = user_data;
    pthread_mutex_unlock(&facts->tx.state_mutex);
}

unsigned int facts_commit_subject_partition(const char *subject)
{
    if (!subject || subject[0] == '?')
        return 0;
    uint32_t hash = 5381;
    const unsigned char *p = (const unsigned char *)subject;
    while (*p)
        hash = ((hash << 5) + hash) + *p++;
    return 1 + (hash % (FACTS_COMMIT_PARTITIONS - 1));
}

void facts_register_commit_summary_observer(s_facts *facts, f_facts_commit_summary_observer observer, void *user_data)
{
    assert(facts);
    pthread_mutex_lock(&facts->tx.state_mutex);
    facts->tx.commit_summary_observer = observer;
    facts->tx.commit_summary_observer_data = user_data;
    pthread_mutex_unlock(&facts->tx.state_mutex);
}

void facts_register_internal_commit_summary_observer(s_facts *facts, f_facts_commit_summary_observer observer, void *user_data)
{
    assert(facts);
    pthread_mutex_lock(&facts->tx.state_mutex);
    facts->tx.internal_commit_summary_observer = observer;
    facts->tx.internal_commit_summary_observer_data = user_data;
    pthread_mutex_unlock(&facts->tx.state_mutex);
}

int facts_justifications_stage(s_facts *facts, int preserve_existing)
{
    if (!facts || facts->justifications_staging)
        return -1;
    facts->justifications_staging =
        preserve_existing ? justification_graph_clone(facts->justifications) : new_justification_graph();
    if (!facts->justifications_staging)
        return -1;
    facts->justifications_staged_level = facts->tx.level;
    return 0;
}

void facts_justifications_commit(s_facts *facts)
{
    if (!facts || !facts->justifications_staging)
        return;
    delete_justification_graph(facts->justifications);
    facts->justifications = facts->justifications_staging;
    facts->justifications_staging = NULL;
    facts->justifications_staged_level = 0;
}

void facts_justifications_discard(s_facts *facts)
{
    if (!facts)
        return;
    delete_justification_graph(facts->justifications_staging);
    facts->justifications_staging = NULL;
    facts->justifications_staged_level = 0;
}

size_t facts_justification_count(s_facts *facts, const char *s, const char *p, const char *o)
{
    if (!facts || !s || !p || !o)
        return 0;
    int acquired = transaction_acquire_reader(&facts->tx);
    size_t count = justification_graph_count(facts->justifications, s, p, o);
    transaction_release_reader(&facts->tx, acquired);
    return count;
}

static int facts_remove_all_derived(s_facts *facts)
{
    size_t capacity = facts_count(facts);
    s_fact *derived = capacity ? malloc(capacity * sizeof(*derived)) : NULL;
    if (capacity && !derived)
        return -1;

    size_t count = 0;
    s_facts_cursor cursor;
    facts_with_0(facts, &cursor, NULL, NULL, NULL);
    s_fact *fact;
    while ((fact = facts_cursor_next(&cursor)) != NULL) {
        if (fact->derived_count) {
            if (count >= capacity) {
                facts_cursor_stop(&cursor);
                free(derived);
                return -1;
            }
            derived[count++] = *fact;
        }
    }
    facts_cursor_stop(&cursor);

    int result = 0;
    for (size_t i = 0; i < count; i++) {
        if (facts_remove_fact_origin(facts, &derived[i], FACT_ORIGIN_DERIVED) < 0) {
            result = -1;
            break;
        }
    }
    free(derived);
    return result;
}

int facts_attach_program(s_facts *facts, const s_datalog_program *prog)
{
    if (!facts)
        return -1;
    if (transaction_writer_owned(&facts->tx))
        return -1;

    s_datalog_program *copy = NULL;
    if (prog) {
        copy = datalog_program_clone(prog);
        if (!copy)
            return -1;
        if (copy->rule_count > 0) {
            int num_strata = 0;
            int *strata = datalog_program_stratify(copy, &num_strata);
            if (!strata) {
                delete_datalog_program(copy);
                return -1;
            }
            free(strata);
        }
    }

    if (facts_transaction_begin(facts) != 0) {
        delete_datalog_program(copy);
        return -1;
    }

    s_datalog_program *old_prog = facts->prog;
    int old_owns_prog = facts->owns_prog;
    int old_disable = facts->disable_listener;
    f_facts_tx_listener old_internal_listener = facts->tx.internal_listener;
    void *old_internal_listener_data = facts->tx.internal_listener_data;
    facts->disable_listener = 1;
    facts->prog = copy;
    facts->owns_prog = copy != NULL;
    facts_register_internal_tx_listener(facts, copy ? reactive_tx_listener : NULL, NULL);

    int result = facts_justifications_stage(facts, 0);
    if (result == 0)
        result = facts_remove_all_derived(facts);
    if (result == 0 && copy)
        result = facts_datalog_eval(facts, copy) < 0 ? -1 : 0;

    facts->disable_listener = old_disable;
    if (result != 0) {
        facts->prog = old_prog;
        facts->owns_prog = old_owns_prog;
        facts_register_internal_tx_listener(facts, old_internal_listener, old_internal_listener_data);
        facts_justifications_discard(facts);
        facts_transaction_rollback(facts);
        delete_datalog_program(copy);
        return -1;
    }

    if (transaction_commit_silent(facts, &facts->tx) != 0) {
        facts->prog = old_prog;
        facts->owns_prog = old_owns_prog;
        facts_register_internal_tx_listener(facts, old_internal_listener, old_internal_listener_data);
        facts_justifications_discard(facts);
        delete_datalog_program(copy);
        return -1;
    }
    if (old_owns_prog)
        delete_datalog_program(old_prog);
    return 0;
}

int facts_detach_program(s_facts *facts)
{
    return facts_attach_program(facts, NULL);
}

void facts_apply_rollback_entry(s_facts *facts, const s_rollback_entry *entry)
{
    assert(facts);
    assert(entry);
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_set_item *si = set_get(&facts->index, &entry->fact, FACT_KEY_SIZE);
    if (entry->action == ROLLBACK_REMOVE) {
        if (si) {
            if (((s_fact *)si->data)->asserted_count && facts->log)
                facts_log_change(facts, "remove", si->data);
            facts_set_support_locked(facts, si->data, 0, 0);
        }
    } else if (entry->action == ROLLBACK_ADD) {
        if (si) {
            if (entry->fact.asserted_count && facts->log)
                facts_log_change(facts, "add", &entry->fact);
            facts_set_support_locked(facts, si->data, entry->fact.asserted_count, entry->fact.derived_count);
        }
    } else if (si) {
        s_fact *fact = (s_fact *)si->data;
        if (facts->log && fact->asserted_count != entry->fact.asserted_count)
            facts_log_change(facts, entry->fact.asserted_count ? "add" : "remove", fact);
        facts_set_support_locked(facts, fact, entry->fact.asserted_count, entry->fact.derived_count);
    }
    transaction_release_writer(&facts->tx, has_lock);
}

s_entity *facts_entity_begin(s_facts *facts, const char *subject)
{
    if (!facts || !subject)
        return NULL;
    s_entity *ent = malloc(sizeof(s_entity));
    if (!ent)
        return NULL;
    if (facts_transaction_begin(facts) != 0) {
        free(ent);
        return NULL;
    }
    ent->facts = facts;
    ent->subject_sym = facts_intern(facts, subject);
    if (!ent->subject_sym) {
        facts_transaction_rollback(facts);
        free(ent);
        return NULL;
    }
    ent->subject = symbol_to_str(ent->subject_sym);
    ent->in_transaction = 1;
    return ent;
}

int facts_entity_add(s_entity *ent, const char *predicate, const char *object)
{
    if (!ent || !ent->in_transaction || !predicate || !object)
        return -1;
    return facts_add_spo(ent->facts, ent->subject, predicate, object) ? 0 : -1;
}

int facts_entity_add_long(s_entity *ent, const char *predicate, long value)
{
    if (!ent || !ent->in_transaction || !predicate)
        return -1;
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", value);
    return facts_add_spo(ent->facts, ent->subject, predicate, buf) ? 0 : -1;
}

int facts_entity_add_double(s_entity *ent, const char *predicate, double value)
{
    if (!ent || !ent->in_transaction || !predicate)
        return -1;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.17g", value);
    return facts_add_spo(ent->facts, ent->subject, predicate, buf) ? 0 : -1;
}

int facts_entity_commit(s_entity *ent)
{
    if (!ent)
        return -1;
    int res = 0;
    if (ent->subject_sym) {
        facts_unintern(ent->facts, ent->subject_sym);
        ent->subject_sym = NULL;
    }
    if (ent->in_transaction) {
        res = facts_transaction_commit(ent->facts);
        ent->in_transaction = 0;
    }
    free(ent);
    return res;
}

void facts_entity_abort(s_entity *ent)
{
    if (!ent)
        return;
    if (ent->subject_sym) {
        facts_unintern(ent->facts, ent->subject_sym);
        ent->subject_sym = NULL;
    }
    if (ent->in_transaction) {
        facts_transaction_rollback(ent->facts);
        ent->in_transaction = 0;
    }
    free(ent);
}

static void facts_spec_sort_masked(s_facts *facts, p_spec spec, size_t count, unsigned char *variable_masks)
{
    if (count <= 1)
        return;

    const char *bound_vars[128];
    size_t bound_vars_count = 0;

    for (size_t i = 0; i < count; i++) {
        size_t best_idx = i;
        double best_cost = -1.0;

        for (size_t j = i; j < count; j++) {
            const char *fs = spec[j * 4 + 0];
            const char *fp = spec[j * 4 + 1];
            const char *fo = spec[j * 4 + 2];
            const char *fneg = spec[j * 4 + 3];
            double cost = 1.0;
            int has_unbound = 0;

            /* subject */
            if (variable_masks ? (variable_masks[j] & 1) : fs[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], fs) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, fs);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            /* predicate */
            if (variable_masks ? (variable_masks[j] & 2) : fp[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], fp) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, fp);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            /* object */
            if (variable_masks ? (variable_masks[j] & 4) : fo[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], fo) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, fo);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            if (fneg && strcmp(fneg, ":not") == 0) {
                if (has_unbound) {
                    cost = 1e15;
                } else {
                    cost = 1.0;
                }
            }

            if (best_cost < 0 || cost < best_cost) {
                best_cost = cost;
                best_idx = j;
            }
        }

        if (best_idx != i) {
            const char *swap[4];
            memcpy(swap, spec + i * 4, sizeof(swap));
            memcpy(spec + i * 4, spec + best_idx * 4, sizeof(swap));
            memcpy(spec + best_idx * 4, swap, sizeof(swap));
            if (variable_masks) {
                unsigned char mask = variable_masks[i];
                variable_masks[i] = variable_masks[best_idx];
                variable_masks[best_idx] = mask;
            }
        }

        const char *cs = spec[i * 4 + 0];
        const char *cp = spec[i * 4 + 1];
        const char *co = spec[i * 4 + 2];
        const char *cneg = spec[i * 4 + 3];
        if (!(cneg && strcmp(cneg, ":not") == 0)) {
            if ((variable_masks ? (variable_masks[i] & 1) : cs[0] == '?') && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], cs) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = cs;
            }
            if ((variable_masks ? (variable_masks[i] & 2) : cp[0] == '?') && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], cp) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = cp;
            }
            if ((variable_masks ? (variable_masks[i] & 4) : co[0] == '?') && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], co) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = co;
            }
        }
    }
}

void facts_spec_sort(s_facts *facts, p_spec spec, size_t count)
{
    facts_spec_sort_masked(facts, spec, count, NULL);
}
