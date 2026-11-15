# Runtime semantics

This document defines the boundaries between the fact database, Datalog rules,
reactive evaluation, and Linda coordination. These layers deliberately share a
storage engine, but they do not assign identical meaning to every stored fact.

## Fact identity and support

A fact is uniquely identified by its `(subject, predicate, object)` tuple. The
database is a set: adding the same fact and support class again is idempotent.
A fact may independently have asserted support, derived support, or both.
Removing one support class leaves the fact present while another class remains.

Derived support records that at least one currently evaluated rule conclusion
supports a fact. It is not a proof count and must not be interpreted as the
number of independent derivations.

## Transactions and reactive rules

The reactive engine is a semi-naive, delta-driven Datalog evaluator with a
grounded justification graph. It is inspired by Rete's incremental intent but
does not claim to maintain a classical persistent alpha/beta node network. The
legacy `rete_tx_listener()` symbol remains only for source compatibility;
`reactive_tx_listener()` names the implemented execution model.

An outer transaction is the atomic mutation and notification boundary. Reactive
rule evaluation runs before that transaction becomes visible to other threads.
Post-commit observers run after the write lock is released and see the final
physical change summary, including changes made by reactive evaluation.
Reactive and application transaction listeners are fallible. A listener error
rolls back the base mutation and every derived mutation produced during that
commit; observers are not called for the failed transaction.
The application listener sees the initial reactive closure. Any mutations it
makes are reconciled by the reactive engine before commit; the application
listener is called only once. Errors during reconciliation roll back the whole
transaction as well.

Reactive retraction is dependency-directed. The runtime removes proofs that
reference a disappeared positive support or a newly present negated support,
verifies that surviving proofs remain grounded in external support rather than a
self-supporting cycle, removes derived support from ungrounded conclusions, and
cascades further only when a conclusion has no remaining asserted support and
actually disappears. Unaffected derived facts are not removed and reinserted. A
closure pass may still be required to discover facts enabled by a deletion
through negation, but it does not churn the unaffected closure.

Attaching a managed Datalog program clones it into the database. Replacing or
detaching a program clears prior derived support and recomputes the new closure
atomically. The caller retains ownership of the source program.

## Linda coordination

Linda is a multiset even though the underlying fact database is a set. The
database's asserted support represents the first tuple occurrence; Linda keeps
an internal multiplicity entry for additional equal occurrences. Each `in` or
`inp` consumes exactly one occurrence.

Only asserted support is visible through Linda operations. A derived-only fact
is a logical result, not a coordination message. If a fact has both asserted and
derived support, consuming its final Linda occurrence removes asserted support
and leaves its derived support in the database, invisible to Linda.

Datalog observes the set projection of Linda's multiset: the first `out` makes
the asserted tuple true, further equal occurrences do not add logical support,
and consuming the final occurrence retracts that asserted tuple. Rules therefore
remain true while at least one equal coordination occurrence remains.

Direct asserted additions through the database returned by `linda_space_facts()`
remain visible to Linda and wake blocked operations after commit. This accessor
is an advanced, borrowed interoperability escape hatch; the Linda space itself
is opaque. Direct database removals are not a supported way to manage Linda
multiplicity, so coordination tuples should be inserted and consumed through the
Linda API.

Linda tuple mutations own their complete transaction boundary. Calling `out`,
`in`, `inp`, or blocking `rd` (including timed and compiled-pattern variants)
while the calling thread already holds a read guard or write lock/transaction on
the space's database returns `LINDA_ERROR` without changing tuples or
multiplicity. Non-blocking `rdp` remains usable under an active read guard or
transaction. An enclosing database transaction cannot roll back Linda's separate
multiplicity state.

The raw `linda_space_facts()` accessor is retained for source compatibility but
returns `NULL` after close and must not be retained across deletion. New code
should use `linda_space_facts_read_begin()`/`linda_space_facts_read_end()` for
lifecycle-safe borrowed reads. Those guards participate in Linda's active
operation count, so destruction waits for them.

`linda_space_close()` is the cancellation boundary. It is idempotent, rejects
new tuple operations and workers with `LINDA_CLOSED`, and broadcasts to every
wait partition so blocked `rd` and `in` calls return `LINDA_CLOSED` promptly.
Closing does not free storage. `delete_linda_space()` closes first, waits for
ordinary in-flight API calls and detached `eval` workers to finish, and only then
destroys synchronization objects and the fact database. As with other owning C
objects, callers must not initiate a new operation concurrently with or after
deletion; use close as the coordination signal before transferring final
destruction to one thread.

## Pointer lifetime

Legacy lookup functions returning internal facts, symbols, or strings expose
borrowed storage. Callers must keep a fact read guard active for the entire use.
Snapshot and copy APIs are the default choice across thread or transaction
boundaries.

Symbol tables may be shared by concurrent databases. Each table synchronizes
its own lookups and reference changes. Zero-reference symbols remain allocated
and reusable until table destruction, while lookups hide them until referenced
again. Storage therefore grows with distinct vocabulary, not repeated use of
the same tuples. Destroy a shared symbol table only after all its users finish.

## Allocation failures

Checked constructors (`intern_init_checked()`, `facts_init_checked()`,
`transaction_init_checked()`, and `set_init_checked()`) return errors without
publishing partially initialized objects. Fact insertion rolls back partially
built hash/trie indexes, and query cursors report allocation errors through
`facts_cursor_error()` and `facts_with_cursor_error()`. Inside an active
transaction, deleted facts are retired in place (`retired_facts`) and reclaimed
only when the outer transaction commits or finishes rolling back, so transaction
rollback restores deleted facts without allocating memory or mutating indexes.
`facts_reset_checked()` allocates a complete replacement index set before
modifying live state and is used internally by evaluation. Legacy void
initialization and reset entry points remain as compatibility wrappers for
callers that intentionally treat allocation failure as fatal.
