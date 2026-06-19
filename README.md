# facts_db

Graph database in C.

## Documentation

### [Binding](doc/binding.md) : variable bindings.
A binding associates a name to a value pointer.
Multiple bindings can be given in a NULL terminated array.

### [Fact](doc/fact.md) : a single fact.
Each fact is a triple which consists of subject, predicate, object.

### [Facts](doc/facts.md) : indexed facts database.
Facts database. Operations to add, remove and iterate on multiple facts.

### [Spec](doc/spec.md) : facts specification.
Specification for facts grouped by subject.

## License

See [LICENSE](LICENSE).
