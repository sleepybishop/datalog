# Fact

<a id="s_fact"></a>
```
typedef struct fact {
    Symbol s;
    Symbol p;
    Symbol o;
    /* Support and runtime metadata are declared in include/fact.h. */
} s_fact;
```

A single fact. A fact consists of a subject, a predicate and an
object.

---

<a id="fact_init"></a>
`void fact_init (s_fact *f, Symbol s, Symbol p, Symbol o)`

Initialize caller-owned storage with interned subject, predicate, and object
symbols. Keep those symbols alive for the lifetime of the fact. This function
does not allocate the fact or take ownership of its symbols.

---

<a id="new_fact"></a>
`s_fact * new_fact (Symbol s, Symbol p, Symbol o)`

Allocate a fact from the fact arena and initialize it with the given interned
symbols. Keep those symbols alive for the lifetime of the fact. The returned
fact must be released with [delete_fact](#delete_fact); do not pass it to
`free(3)`.

---

<a id="delete_fact"></a>
`void delete_fact (s_fact *f)`

Return a fact allocated by [new_fact](#new_fact) to the fact arena. Do not pass
stack storage, a `malloc(3)` allocation, or a borrowed database fact to this
function. It does not release the fact's symbols.

---

<a id="fact_compare_spo"></a>
`int fact_compare_spo (void *a, void *b)`

Compare two facts with subject, predicate, object order.

---

<a id="fact_compare_pos"></a>
`int fact_compare_pos (void *a, void *b)`

Compare two facts with predicate, object, subject order.

---

<a id="fact_compare_osp"></a>
`int fact_compare_osp (void *a, void *b)`

Compare two facts with object, subject, predicate order.
