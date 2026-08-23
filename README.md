# datalog [![CI](https://github.com/sleepybishop/datalog/actions/workflows/ci.yml/badge.svg)](https://github.com/sleepybishop/datalog/actions/workflows/ci.yml)

Graph database in C.

**Note:** This project is a fork of [https://github.com/facts-db/facts-db](https://github.com/facts-db/facts-db) (which no longer exists), which itself was a port of [https://github.com/facts-db/cl-facts](https://github.com/facts-db/cl-facts).

## Architecture

The underlying engine has been extensively refactored for improved performance and memory density:

- **String Interning**: Strings are globally interned into opaque `Symbol` handles using `wyhash`. This enables fast $O(1)$ ID comparisons during query execution rather than character-by-character string matching.
- **Radix Tree Indexing**: The database utilizes a Hexastore (SPO, POS, etc.) backed by highly compressed Radix Trees (`rax`).
- **Leapfrog Triejoin Solver**: Queries are evaluated using a Leapfrog Triejoin (LFTJ) algorithm, providing optimal worst-case bounds for multi-way joins compared to naive binary backtracking.
- **Memory Arenas**: Fact and trie node allocations use thread-safe memory arenas with compaction support to eliminate fragmentation and improve cache locality.
- **Transactions**: Supports nested thread-safe transactions with full rollback capabilities, protected by a multiple-reader/single-writer lock.
- **Reactive Datalog**: Committed base-fact changes maintain a stratified derived layer. Positive insertions use semi-naive incremental evaluation; deletions and negation-sensitive changes rebuild derived facts to preserve correctness across alternate derivations.
- **Explicit Provenance**: Facts independently record asserted and derived support, so retracting one support class does not erase a tuple that remains true through the other. `facts_get_support_spo()` exposes this summary.
- **Linda Coordination**: The same fact store can act as a Linda tuplespace with blocking, non-blocking, and timed operations. Equal tuples remain distinct consumable occurrences, derived-only facts stay outside the coordination view, reusable compiled patterns remove binding allocation from hot coordination loops, and post-commit subject summaries wake only wildcard and affected waiter partitions. The cross-layer contract is specified in [doc/SEMANTICS.md](doc/SEMANTICS.md).
- **Safe Concurrent Reads**: Containment checks, owning snapshots, copied properties, and nestable read guards avoid leaking mutable storage across write transactions.
- **Managed Rule Programs**: Atomic deep-copy attachment recomputes the derived layer without exposing caller-owned rule memory.
- **SPARQL REPL**: Features a basic Ragel-compiled SPARQL query parser (SELECT/ASK/INSERT) and an interactive CLI REPL powered by (`linenoise`).

## Reactive usage

`facts_attach_program()` takes a deep copy and atomically rebuilds the derived layer, so the input program may be freed immediately. Base changes become reactive when their outer transaction commits:

```c
s_facts *db = new_facts(NULL, 1024);
s_datalog_program *program = new_datalog_program();
datalog_program_parse_rules(program, "?x <path> ?y :- ?x <edge> ?y .");
facts_attach_program(db, program);
delete_datalog_program(program);

facts_transaction_begin(db);
facts_add_spo(db, "a", "edge", "b");
facts_transaction_commit(db);

if (facts_contains_spo(db, "a", "path", "b") > 0) {
    /* The derived tuple is visible here. */
}
```

Use `facts_get_spo_snapshot()` or `facts_get_prop_copy()` when a value must outlive a read call. Legacy pointer-returning lookups remain available, but retaining their results requires a `facts_read_guard`.

## License

See [LICENSE](LICENSE).
