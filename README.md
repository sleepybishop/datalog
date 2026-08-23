# datalog [![CI](https://github.com/sleepybishop/datalog/actions/workflows/ci.yml/badge.svg)](https://github.com/sleepybishop/datalog/actions/workflows/ci.yml)

Graph database in C.

An embedded graph database in C combining indexed triples, Datalog inference,
and Linda coordination.

It provides transactions, radix-tree indexes, Leapfrog Triejoin queries,
reactive stratified rules with provenance-aware retraction, Linda tuple-space
operations, and a small SPARQL REPL.


## Build

```sh
make
make check
```

See [runtime semantics](doc/SEMANTICS.md) for the Datalog/Linda contract and
concurrency rules.

Originally forked from `facts-db`, itself a port of [https://github.com/facts-db/cl-facts](https://github.com/facts-db/cl-facts).

## License

See [LICENSE](LICENSE).
