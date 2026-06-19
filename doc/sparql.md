# Feasibility Analysis: Ragel SPARQL Parser

This document details the architectural feasibility and estimated effort to implement a **Ragel-based SPARQL parser** for the C Datalog query engine.

---

## 1. Architectural Fit: Ragel & Datalog

Ragel compiles finite state machines (FSM) directly into branch-heavy C code (`goto` statements or table-driven transitions). It is highly suitable for this codebase due to:
* **Zero Overhead**: No runtime dependencies, compiling down to a single `.c` file.
* **Stream-Friendly**: High-performance parsing over strings or buffers in a single pass.

The SPARQL parser would translate a query string into the engine's native query structures:

```
[ SPARQL Query String ] 
       │
       ▼ (Ragel Lexer & Parser)
[ AST / Query Struct ]
       │
       ▼ (Bridge / Compiler)
[ p_spec ] and [ s_binding ] ──► facts_with(...)
```

---

## 2. Implementation Breakdown

For a **Simplified SPARQL Parser** (supporting `SELECT`, `WHERE` blocks, variables `?var`, symbols `<uri>` or barewords, and triple patterns `?s ?p ?o .`):

### A. Lexer / Scanner (`sparql.rl`)
Write the Ragel scanner rules to recognize:
* Keywords: `SELECT`, `WHERE` (case-insensitive)
* Variables: `\?[a-zA-Z0-9_]+`
* Symbols/URIs: `<[^>]+>` or standard alphanumeric words
* Literals: String constants inside double quotes (`"[^"]*"`)
* Grammar punctuation: `{`, `}`, `.`

### B. Parser State Machine & AST
Because SPARQL has a nested structure (matching blocks inside `WHERE { ... }`), a pure FSM is not sufficient. A hybrid FSM with a stack (a Pushdown Automaton) is required to track bracket nesting `{ }`.
* Define AST structures in C to hold the parser's output:
  ```c
  typedef struct sparql_triple {
      char *s;
      char *p;
      char *o;
  } s_sparql_triple;

  typedef struct sparql_query {
      char **projection_vars; // Variables in SELECT
      s_sparql_triple *triples;
      size_t triple_count;
  } s_sparql_query;
  ```

### C. Compilation to Datalog Query
Translate the `s_sparql_query` AST into the engine's query structures:
1. Map the AST triples into a flat `const char *spec[]` array.
2. Initialize `s_binding bindings[]` where `bindings[i].name` is a projected variable from the `SELECT` list, and `bindings[i].value` points to a `const char *` variable to receive the query results.

### D. Build Integration
Add rules to the [Makefile](file:///home/joe/src/sleepybishop/datalog/Makefile) to compile Ragel files:
```makefile
%.c: %.rl
	ragel -G2 -o $@ $<
```
*Tip: Checking the generated `.c` file into Git allows users to compile the database engine without having `ragel` installed locally.*

---

## 3. Effort & Timeline Estimate (Simplified SPARQL)

| Phase | Tasks | Estimated Effort |
| :--- | :--- | :---: |
| **Phase 1: Lexer & Grammar** | Write `sparql.rl` to tokenize variables, symbols, strings, and bracket punctuation. | **1.0 Day** |
| **Phase 2: AST & Bracket Stack** | Write action hooks in Ragel to parse statements, manage bracket nesting using a simple C stack, and populate the AST. | **1.5 Days** |
| **Phase 3: Datalog Bridge** | Write translation code converting the AST to the [spec.h](file:///home/joe/src/sleepybishop/datalog/include/spec.h)/[facts.h](file:///home/joe/src/sleepybishop/datalog/include/facts.h) query structures. | **1.0 Day** |
| **Phase 4: Tests & Cleanup** | Add check unit tests validating parsed queries against database states. | **1.0 Day** |
| **Total** | | **~4.5 Days** |

---

## 4. Feature Gap Analysis & Implementation Roadmap

Below is a breakdown of the common SPARQL features not currently supported, along with the Level of Effort (LoE) and architectural impact to implement them in the datalog engine.

| Feature | Description | Status / LoE | Architectural Impact |
| :--- | :--- | :---: | :--- |
| **Prefixes (`PREFIX`)** | Namespace resolution during parsing. | **Completed** | **Low**. Namespace mapping table tracks prefixes and expands prefixed names to full URIs at parse time. |
| **Negation (`NOT`)** | Exclude matching subgraphs using negative subgoals. | **Completed** | **Low**. Handled in backtracking evaluator via `:not` negation level checks. |
| **Modifiers (`LIMIT` / `OFFSET`)** | Page query results by limiting output counts. | **Completed** | **Low**. Enforced during facts cursor traversal. |
| **Sorting (`ORDER BY`)** | Order bindings alphabetically (ASC/DESC). | **Completed** | **Medium**. Cached matches sorted before cursor traversal begins. |
| **Boolean Queries (`ASK`)** | Verify existence of patterns (returns true/false). | **Completed** | **Low**. Early-aborts cursor traversal on first match. |
| **Optional Patterns (`OPTIONAL`)** | Left outer joins where patterns can be missing. | **4.0 Days** | **High**. Requires extending the backtracking execution cursor to allow a level to succeed without binding variables if no matching facts are found. |
| **Filters (`FILTER`)** | Conditional expression constraints (e.g. range, inequality, regex). | **5.0 Days** | **High**. Needs a recursive-descent expression parser to build an expression tree, and an evaluator hook in the backtracking loop to validate bindings. |
| **Disjunctions (`UNION`)** | Logical disjunction of graph patterns. | **5.0 Days** | **High**. The backtracking engine must be modified to handle branching paths (i.e. returning a cursor that aggregates multiple execution cursors). |
| **Property Paths** | Transitive closures and deep graph traversal (e.g., `?a :parent+ ?b`). | **7.0+ Days** | **Critical**. Requires graph search algorithms (BFS/DFS with cycle detection) directly inside the index retrieval engine. |
