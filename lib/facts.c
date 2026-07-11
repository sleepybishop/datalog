#include <assert.h>
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

void facts_rollback_push(s_facts *facts, e_rollback_action action, const s_fact *fact);

static inline uint64_t get_symbol_id(Symbol ptr)
{
    if (ptr == P_FIRST)
        return 0;
    if (ptr == P_LAST)
        return 0xFFFFFFFFFFFFFFFFULL;
    return ptr->id;
}

static inline void encode_uint64_be(unsigned char *buf, uint64_t val)
{
    buf[0] = (val >> 56) & 0xFF;
    buf[1] = (val >> 48) & 0xFF;
    buf[2] = (val >> 40) & 0xFF;
    buf[3] = (val >> 32) & 0xFF;
    buf[4] = (val >> 24) & 0xFF;
    buf[5] = (val >> 16) & 0xFF;
    buf[6] = (val >> 8) & 0xFF;
    buf[7] = val & 0xFF;
}

static void encode_cursor_key(unsigned char *key, const s_fact *f, int index_type)
{
    if (index_type == 0) { // SPO
        encode_uint64_be(key, get_symbol_id(f->s));
        encode_uint64_be(key + 8, get_symbol_id(f->p));
        encode_uint64_be(key + 16, get_symbol_id(f->o));
    } else if (index_type == 1) { // POS
        encode_uint64_be(key, get_symbol_id(f->p));
        encode_uint64_be(key + 8, get_symbol_id(f->o));
        encode_uint64_be(key + 16, get_symbol_id(f->s));
    } else { // OSP
        encode_uint64_be(key, get_symbol_id(f->o));
        encode_uint64_be(key + 8, get_symbol_id(f->s));
        encode_uint64_be(key + 16, get_symbol_id(f->p));
    }
}

void facts_init(s_facts *facts, s_intern *symbols, unsigned long max)
{
    assert(facts);
    arena_init();
    facts->symbols = symbols ? symbols : new_intern(max);
    facts->symbols_delete = !symbols;
    set_init(&facts->index, max);
    facts->hexastore = new_hexastore();
    facts->index_spo = facts->hexastore->trie_spo;
    facts->index_pos = facts->hexastore->trie_pos;
    facts->index_osp = facts->hexastore->trie_osp;
    facts->log = NULL;

    facts->prog = NULL;
    facts->disable_listener = 0;

    transaction_init(&facts->tx);
    facts_register_tx_listener(facts, rete_tx_listener, NULL);
}

void facts_destroy(s_facts *facts)
{
    delete_hexastore(facts->hexastore);
    set_destroy(&facts->index);
    if (facts->symbols_delete)
        delete_intern(facts->symbols);
    transaction_destroy(&facts->tx);
    arena_destroy();
}

void facts_reset(s_facts *facts)
{
    unsigned long max;
    s_set_cursor sc;
    s_set_item *si;
    assert(facts);

    // 1. Recycle all facts back into the fact arena pool
    set_cursor_init(&facts->index, &sc);
    while ((si = set_cursor_next(&sc))) {
        arena_free_fact(si->data);
    }

    // 2. Destroy and recreate the hexastore index
    delete_hexastore(facts->hexastore);
    facts->hexastore = new_hexastore();
    facts->index_spo = facts->hexastore->trie_spo;
    facts->index_pos = facts->hexastore->trie_pos;
    facts->index_osp = facts->hexastore->trie_osp;

    // 3. Destroy and recreate the facts index hash set
    max = facts->index.max;
    set_destroy(&facts->index);
    set_init(&facts->index, max);

    // 4. Clear the symbol interning table
    if (facts->symbols_delete) {
        delete_intern(facts->symbols);
        facts->symbols = new_intern(max);
    }

    // 5. Reset transaction data/state
    transaction_destroy(&facts->tx);
    transaction_init(&facts->tx);
    facts_register_tx_listener(facts, rete_tx_listener, NULL);
}

s_facts *new_facts(s_intern *symbols, unsigned long max)
{
    s_facts *facts = malloc(sizeof(s_facts));
    if (facts)
        facts_init(facts, symbols, max);
    return facts;
}

void delete_facts(s_facts *facts)
{
    facts_destroy(facts);
    free(facts);
}

s_set_item *facts_find_symbol(s_facts *facts, const char *string)
{
    return intern_find_symbol(facts->symbols, string);
}

Symbol facts_find_symbol_str(s_facts *facts, const char *string)
{
    return intern_find_symbol_str(facts->symbols, string);
}

const char *facts_long(s_facts *facts, long l)
{
    return symbol_to_str(intern_long(facts->symbols, l));
}

const char *facts_double(s_facts *facts, double d)
{
    return symbol_to_str(intern_double(facts->symbols, d));
}

long facts_get_long(s_facts *facts, const char *string)
{
    return intern_get_long(facts->symbols, string);
}

double facts_get_double(s_facts *facts, const char *string)
{
    return intern_get_double(facts->symbols, string);
}

Symbol facts_intern(s_facts *facts, const char *string)
{
    return intern_string(facts->symbols, string);
}

void facts_unintern(s_facts *facts, Symbol sym)
{
    intern_unstring(facts->symbols, sym);
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
        if (!facts_find_symbol(facts, buf))
            return symbol_to_str(facts_intern(facts, buf));
    }
    return NULL;
}

s_fact *facts_add_fact(s_facts *facts, s_fact *f)
{
    s_fact *found;
    s_fact *new;
    assert(facts);
    assert(f);
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_set_item *si = set_get(&facts->index, f, sizeof(Symbol) * 4);
    if (si) {
        found = (s_fact *)si->data;
        found->proof_count++;
        transaction_release_writer(&facts->tx, has_lock);
        return found;
    }
    if (facts->log)
        write_fact_log("add", f, facts->log);
    new = new_fact(f->s, f->p, f->o);
    assert(new);
    new->negated = f->negated;
    facts_intern(facts, symbol_to_str(new->s));
    facts_intern(facts, symbol_to_str(new->p));
    facts_intern(facts, symbol_to_str(new->o));
    hexastore_insert(facts->hexastore, new);
    set_add(&facts->index, new, sizeof(Symbol) * 4);
    facts_rollback_push(facts, ROLLBACK_REMOVE, new);
    transaction_release_writer(&facts->tx, has_lock);
    return new;
}

s_fact *facts_add_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    s_fact f;
    assert(facts);
    assert(s);
    assert(p);
    assert(o);
    f.s = facts_intern(facts, s);
    f.p = facts_intern(facts, p);
    f.o = facts_intern(facts, o);
    f.negated = NULL;
    s_fact *ret = facts_add_fact(facts, &f);
    facts_unintern(facts, f.s);
    facts_unintern(facts, f.p);
    facts_unintern(facts, f.o);
    return ret;
}

const char **spec_bindings_anon_assoc(s_facts *facts, p_spec spec)
{
    const char **b;
    const char **bindings;
    size_t count;
    assert(spec);
    count = spec_count_bindings(spec);
    bindings = calloc(count * 2 + 1, sizeof(char *));
    if (bindings) {
        size_t s = 0;
        b = bindings;
        while (spec[s] || spec[s + 1]) {
            if (spec[s] && spec[s][0] == '?') {
                *b++ = spec[s];
                *b++ = facts_anon(facts, spec[s]);
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

        s_fact db_fact;
        db_fact.s = facts_intern(facts, f.s);
        db_fact.p = facts_intern(facts, f.p);
        db_fact.o = facts_intern(facts, f.o);
        db_fact.negated = NULL;
        facts_add_fact(facts, &db_fact);
        facts_unintern(facts, db_fact.s);
        facts_unintern(facts, db_fact.p);
        facts_unintern(facts, db_fact.o);
    }
    free(anon);
    transaction_release_writer(&facts->tx, has_lock);
    return 0;
}

int facts_remove_fact(s_facts *facts, s_fact *f)
{
    s_fact *found;
    assert(facts);
    assert(f);
    int has_lock = transaction_acquire_writer(&facts->tx);
    s_set_item *si = set_get(&facts->index, f, sizeof(Symbol) * 4);
    if (si) {
        found = (s_fact *)si->data;
        if (found->proof_count > 1) {
            found->proof_count--;
            transaction_release_writer(&facts->tx, has_lock);
            return 1;
        }

        set_remove(&facts->index, si);
        if (facts->log)
            write_fact_log("remove", found, facts->log);
        facts_rollback_push(facts, ROLLBACK_ADD, found);
        hexastore_remove(facts->hexastore, found);
        facts_unintern(facts, found->s);
        facts_unintern(facts, found->p);
        facts_unintern(facts, found->o);
        delete_fact(found);
        transaction_release_writer(&facts->tx, has_lock);
        return 1;
    }
    transaction_release_writer(&facts->tx, has_lock);
    return 0;
}

int facts_remove_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    s_fact f;
    assert(facts);
    assert(s);
    assert(p);
    assert(o);
    Symbol s_sym = facts_find_symbol_str(facts, s);
    Symbol p_sym = facts_find_symbol_str(facts, p);
    Symbol o_sym = facts_find_symbol_str(facts, o);
    if (!s_sym || !p_sym || !o_sym) {
        return 0;
    }
    f.s = s_sym;
    f.p = p_sym;
    f.o = o_sym;
    f.negated = NULL;
    return facts_remove_fact(facts, &f);
}

int facts_remove(s_facts *facts, p_spec spec)
{
    s_facts_with_cursor wc;
    s_binding *bindings;
    s_fact_list *fl = NULL;
    s_fact_list *fli;
    int found = 0;
    int has_lock = transaction_acquire_writer(&facts->tx);
    bindings = spec_bindings(spec);
    facts_with(facts, bindings, &wc, spec);
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
            if (dbf) {
                fl = fact_list_intern(fl, dbf);
            }
        }
    }
    facts_with_cursor_destroy(&wc);
    free(bindings);
    fli = fl;
    while (fli) {
        if (facts_remove_fact(facts, fli->fact))
            found = 1;
        fli = fli->next;
    }
    delete_fact_list(fl);
    transaction_release_writer(&facts->tx, has_lock);
    return found;
}

s_fact *facts_get_fact(s_facts *facts, s_fact *f)
{
    assert(facts);
    assert(f);
    s_set_item *si = set_get(&facts->index, f, sizeof(Symbol) * 4);
    if (si)
        return (s_fact *)si->data;
    return NULL;
}

s_fact *facts_get_spo(s_facts *facts, const char *s, const char *p, const char *o)
{
    s_fact f;
    Symbol s_sym = facts_find_symbol_str(facts, s);
    Symbol p_sym = facts_find_symbol_str(facts, p);
    Symbol o_sym = facts_find_symbol_str(facts, o);
    if (!s_sym || !p_sym || !o_sym) {
        return NULL;
    }
    f.s = s_sym;
    f.p = p_sym;
    f.o = o_sym;
    f.negated = NULL;
    return facts_get_fact(facts, &f);
}

unsigned long facts_count(s_facts *facts)
{
    assert(facts);
    return raxSize(facts->index_spo);
}

void facts_cursor_init(s_facts *facts, s_facts_cursor *c, rax *tree, s_fact *start, s_fact *end)
{
    assert(facts);
    assert(c);
    assert(tree);

    c->index_type = 0;
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

    raxSeek(&c->it, ">=", start_key, 24);
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
    if (c->started == 0) {
        c->started = 1;
    } else {
        if (!raxEOF(&c->it)) {
            raxNext(&c->it);
        }
    }

    if (!raxEOF(&c->it)) {
        if (memcmp(c->it.key, c->end_key, 24) <= 0) {
            s_fact *f = (s_fact *)c->it.data;
            if (c->var_s)
                *c->var_s = symbol_to_str(f->s);
            if (c->var_p)
                *c->var_p = symbol_to_str(f->p);
            if (c->var_o)
                *c->var_o = symbol_to_str(f->o);
            return f;
        }
    }

    raxStop(&c->it);
    c->started = 2;

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
    }
}

void facts_with_3(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o)
{
    s_fact f;
    assert(facts);
    assert(c);
    assert(s);
    assert(p);
    assert(o);
    Symbol interned_s = facts_find_symbol_str(facts, s);
    Symbol interned_p = facts_find_symbol_str(facts, p);
    Symbol interned_o = facts_find_symbol_str(facts, o);
    if (!interned_s || !interned_p || !interned_o) {
        facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
        facts_cursor_stop(c);
        return;
    }
    f.s = interned_s;
    f.p = interned_p;
    f.o = interned_o;
    f.negated = NULL;
    facts_cursor_init(facts, c, facts->index_spo, &f, &f);
}

void facts_with_0(s_facts *facts, s_facts_cursor *c, const char **var_s, const char **var_p, const char **var_o)
{
    assert(facts);
    assert(c);
    facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
    c->var_s = var_s;
    c->var_p = var_p;
    c->var_o = var_o;
}

void facts_with_1_2(s_facts *facts, s_facts_cursor *c, const char *s, const char *p, const char *o, const char **var_s,
                    const char **var_p, const char **var_o)
{
    s_fact start;
    s_fact end;
    struct rax *tree;
    assert(facts);
    assert(c);
    assert(s);
    assert(p);
    assert(o);
    assert(var_s || var_p || var_o);
    Symbol interned_s = var_s ? NULL : facts_find_symbol_str(facts, s);
    Symbol interned_p = var_p ? NULL : facts_find_symbol_str(facts, p);
    Symbol interned_o = var_o ? NULL : facts_find_symbol_str(facts, o);
    if ((!var_s && !interned_s) || (!var_p && !interned_p) || (!var_o && !interned_o)) {
        facts_cursor_init(facts, c, facts->index_spo, NULL, NULL);
        facts_cursor_stop(c);
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
    size_t facts_count;
    assert(facts);
    assert(c);
    assert(spec);
    pthread_t self = pthread_self();
    int has_lock = pthread_equal(facts->tx.owner, self);
    if (!has_lock) {
        transaction_acquire_reader(&facts->tx);
        c->locked = 1;
    } else {
        c->locked = 0;
    }
    facts_count = spec_count_facts(spec);
    c->facts = facts;
    c->bindings = bindings;
    bindings_nullify(c->bindings);
    c->facts_count = facts_count;
    if (facts_count > 0) {
        size_t total_ptrs = 0;
        for (size_t i = 0; i < facts_count; i++) {
            total_ptrs += (facts_count - i) * 4 + 2;
        }
        size_t levels_sz = facts_count * sizeof(s_facts_with_cursor_level);
        size_t specs_sz = total_ptrs * sizeof(const char *);
        char *mem = calloc(1, levels_sz + specs_sz);
        assert(mem);
        c->l = (s_facts_with_cursor_level *)mem;
        const char **spec_ptr = (const char **)(mem + levels_sz);
        c->spec = spec_expand(spec);
        facts_spec_sort(facts, c->spec, facts_count);
        for (size_t i = 0; i < facts_count; i++) {
            size_t remaining = facts_count - i;
            c->l[i].spec = spec_ptr;
            spec_ptr += remaining * 4 + 2;
            c->l[i].spec_init = 0;
        }
    } else {
        c->l = NULL;
        c->spec = NULL;
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
}

void facts_with_cursor_destroy(s_facts_with_cursor *c)
{
    assert(c);
    if (c->l) {
        for (size_t i = 0; i < c->facts_count; i++) {
            if (c->l[i].spec_init) {
                facts_cursor_stop(&c->l[i].c);
            }
        }
        free(c->l);
    }
    if (c->spec)
        free(c->spec);
    if (c->locked && c->facts) {
        transaction_release_reader(&c->facts->tx);
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
    c->facts = NULL;
    c->bindings = NULL;
    c->facts_count = 0;
    c->l = NULL;
    c->level = 0;
    c->spec = NULL;
    c->locked = 0;
}

void spec_subst(p_spec spec, s_binding *bindings)
{
    size_t i = 0;
    if (spec && spec[0])
        while (spec[i] || spec[i + 1]) {
            if (spec[i] && spec[i][0] == '?') {
                const char **b = bindings_get(bindings, spec[i]);
                if (*b)
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

static void quicksort_matches(s_cached_match *arr, int low, int high, int idx, int desc)
{
    if (low < high) {
        s_cached_match pivot = arr[high];
        int i = low - 1;
        for (int j = low; j < high; j++) {
            if (compare_matches(&arr[j], &pivot, idx, desc) <= 0) {
                i++;
                swap_matches(&arr[i], &arr[j]);
            }
        }
        swap_matches(&arr[i + 1], &arr[high]);
        int pi = i + 1;
        quicksort_matches(arr, low, pi - 1, idx, desc);
        quicksort_matches(arr, pi + 1, high, idx, desc);
    }
}

int facts_with_cursor_next_inner(s_facts_with_cursor *c);

int facts_with_cursor_next(s_facts_with_cursor *c)
{
    if (c->is_sorted) {
        if (!c->sorted_matches && c->sorted_pos == 0) {
            size_t capacity = 16;
            c->sorted_matches = malloc(capacity * sizeof(s_cached_match));
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
                    capacity *= 2;
                    c->sorted_matches = realloc(c->sorted_matches, capacity * sizeof(s_cached_match));
                }
                s_cached_match *matches = (s_cached_match *)c->sorted_matches;
                matches[c->sorted_count].values = malloc(bindings_count * sizeof(const char *));
                for (int i = 0; i < bindings_count; i++) {
                    matches[c->sorted_count].values[i] = *c->bindings[i].value;
                }
                c->sorted_count++;
            }
            c->is_sorted = 1;

            c->limit = orig_limit;
            c->offset = orig_offset;
            c->result_count = 0;

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
                quicksort_matches((s_cached_match *)c->sorted_matches, 0, (int)c->sorted_count - 1, sort_idx, c->sort_desc);
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

int facts_with_cursor_next_inner(s_facts_with_cursor *c)
{
    assert(c);
    if (!c->facts_count)
        return 0;
    if (c->level == c->facts_count) {
        s_facts_with_cursor_level *l = c->l + (c->facts_count - 1);
        if (l->spec[3] && strcmp(l->spec[3], ":not") == 0) {
            facts_cursor_stop(&l->c);
            l->spec_init = 0;
            c->level--;
            if (!c->level) {
                c->facts_count = 0;
                return 0;
            }
            c->level--;
        } else {
            l->fact = facts_cursor_next(&l->c);
            if (l->fact)
                return 1;
            facts_cursor_stop(&l->c);
            l->spec_init = 0;
            c->level--;
            if (!c->level) {
                c->facts_count = 0;
                return 0;
            }
            c->level--;
        }
    }
    while (c->level < c->facts_count) {
        s_facts_with_cursor_level *l = c->l + c->level;
        if (!l->spec_init) {
            p_spec parent_spec = c->level ? c->l[c->level - 1].spec + 4 : c->spec;
            size_t remaining = c->facts_count - c->level;
            memcpy(l->spec, parent_spec, (remaining * 4 + 1) * sizeof(const char *));
            l->spec[remaining * 4 + 1] = NULL;
            spec_subst(l->spec, c->bindings);
            facts_with_spo(c->facts, c->bindings, &l->c, l->spec[0], l->spec[1], l->spec[2]);
            l->spec_init = 1;
        }

        if (l->spec[3] && strcmp(l->spec[3], ":not") == 0) {
            // Negated level:
            // Save bindings to isolate from potential variable matching
            int bindings_count = 0;
            while (c->bindings && c->bindings[bindings_count].name)
                bindings_count++;
            const char **saved_values = NULL;
            if (bindings_count > 0) {
                saved_values = malloc(bindings_count * sizeof(const char *));
                for (int i = 0; i < bindings_count; i++) {
                    saved_values[i] = *c->bindings[i].value;
                }
            }

            s_fact *found_fact = facts_cursor_next(&l->c);

            // Restore bindings
            if (bindings_count > 0) {
                for (int i = 0; i < bindings_count; i++) {
                    *c->bindings[i].value = saved_values[i];
                }
                free(saved_values);
            }

            if (found_fact) {
                // Fact exists in db, negation fails. Backtrack.
                facts_cursor_stop(&l->c);
                l->spec_init = 0;
                if (c->level > 0) {
                    c->level--;
                } else {
                    c->facts_count = 0;
                    return 0;
                }
            } else {
                // Fact does not exist, negation succeeds. Advance.
                c->level++;
            }
        } else {
            // Positive level:
            l->fact = facts_cursor_next(&l->c);
            if (l->fact) {
                c->level++;
            } else {
                facts_cursor_stop(&l->c);
                l->spec_init = 0;
                if (c->level > 0) {
                    c->level--;
                } else {
                    c->facts_count = 0;
                    return 0;
                }
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
    if (facts_cursor_next(&c))
        return o;
    return NULL;
}

long facts_get_prop_long(s_facts *facts, const char *s, const char *p)
{
    const char *o = facts_get_prop(facts, s, p);
    return o ? facts_get_long(facts, o) : 0;
}

double facts_get_prop_double(s_facts *facts, const char *s, const char *p)
{
    const char *o = facts_get_prop(facts, s, p);
    return o ? facts_get_double(facts, o) : 0.0;
}

s_fact *facts_set_prop(s_facts *facts, const char *s, const char *p, const char *o)
{
    int has_lock = transaction_acquire_writer(&facts->tx);
    facts_remove(facts, (const char *[]){s, p, "?o", NULL, NULL});
    s_fact *ret = facts_add_spo(facts, s, p, o);
    transaction_release_writer(&facts->tx, has_lock);
    return ret;
}

void facts_rollback_push(s_facts *facts, e_rollback_action action, const s_fact *fact)
{
    transaction_rollback_push(facts, &facts->tx, action, fact);
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

void facts_register_tx_listener(s_facts *facts, f_facts_tx_listener listener, void *user_data)
{
    assert(facts);
    facts->tx.listener = listener;
    facts->tx.listener_data = user_data;
}

s_entity *facts_entity_begin(s_facts *facts, const char *subject)
{
    assert(facts);
    assert(subject);
    s_entity *ent = malloc(sizeof(s_entity));
    if (ent) {
        ent->facts = facts;
        ent->subject = symbol_to_str(facts_intern(facts, subject));
        facts_transaction_begin(facts);
        ent->in_transaction = 1;
    }
    return ent;
}

int facts_entity_add(s_entity *ent, const char *predicate, const char *object)
{
    if (!ent || !ent->in_transaction)
        return -1;
    facts_add_spo(ent->facts, ent->subject, predicate, object);
    return 0;
}

int facts_entity_add_long(s_entity *ent, const char *predicate, long value)
{
    if (!ent || !ent->in_transaction)
        return -1;
    const char *val_str = facts_long(ent->facts, value);
    facts_add_spo(ent->facts, ent->subject, predicate, val_str);
    return 0;
}

int facts_entity_add_double(s_entity *ent, const char *predicate, double value)
{
    if (!ent || !ent->in_transaction)
        return -1;
    const char *val_str = facts_double(ent->facts, value);
    facts_add_spo(ent->facts, ent->subject, predicate, val_str);
    return 0;
}

int facts_entity_commit(s_entity *ent)
{
    if (!ent)
        return -1;
    int res = 0;
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
    if (ent->in_transaction) {
        facts_transaction_rollback(ent->facts);
        ent->in_transaction = 0;
    }
    free(ent);
}

void facts_spec_sort(s_facts *facts, p_spec spec, size_t count)
{
    if (count <= 1)
        return;

    const char *bound_vars[128];
    size_t bound_vars_count = 0;

    for (size_t i = 0; i < count; i++) {
        size_t best_idx = i;
        double best_cost = -1.0;

        for (size_t j = i; j < count; j++) {
            s_spec_fact *f = (s_spec_fact *)(spec + j * 4);
            double cost = 1.0;
            int has_unbound = 0;

            // Subject
            if (f->s[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], f->s) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, f->s);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            // Predicate
            if (f->p[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], f->p) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, f->p);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            // Object
            if (f->o[0] == '?') {
                int bound = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], f->o) == 0) {
                        bound = 1;
                        break;
                    }
                }
                if (!bound)
                    has_unbound = 1;
                cost *= bound ? 1.0 : 100000.0;
            } else {
                s_set_item *sym = intern_find_symbol(facts->symbols, f->o);
                cost *= sym ? (double)sym->usage : 0.1;
            }

            if (f->negated && strcmp(f->negated, ":not") == 0) {
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
            s_spec_fact *a = (s_spec_fact *)(spec + i * 4);
            s_spec_fact *b = (s_spec_fact *)(spec + best_idx * 4);
            s_spec_fact swap = *a;
            *a = *b;
            *b = swap;
        }

        s_spec_fact *chosen = (s_spec_fact *)(spec + i * 4);
        if (!(chosen->negated && strcmp(chosen->negated, ":not") == 0)) {
            if (chosen->s[0] == '?' && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], chosen->s) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = chosen->s;
            }
            if (chosen->p[0] == '?' && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], chosen->p) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = chosen->p;
            }
            if (chosen->o[0] == '?' && bound_vars_count < 128) {
                int exists = 0;
                for (size_t k = 0; k < bound_vars_count; k++) {
                    if (strcmp(bound_vars[k], chosen->o) == 0) {
                        exists = 1;
                        break;
                    }
                }
                if (!exists)
                    bound_vars[bound_vars_count++] = chosen->o;
            }
        }
    }
}
