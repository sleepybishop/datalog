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
- **Linda Coordination**: The same fact store can act as a Linda tuplespace with blocking, non-blocking, and timed operations. Reusable compiled patterns remove binding allocation from hot coordination loops, and waiters are notified only after a changed transaction commits.
- **SPARQL REPL**: Features a basic Ragel-compiled SPARQL query parser (SELECT/ASK/INSERT) and an interactive CLI REPL powered by (`linenoise`).

## License

See [LICENSE](LICENSE).
