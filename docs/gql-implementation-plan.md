# DuckDB ISO GQL Extension

## Summary

Build a C++17 DuckDB extension named `gql` that accepts GQL statements directly, stores property graphs canonically in DuckDB tables, and accelerates traversal through a rebuildable in-memory CSR index.

The current normalized EAV schema remains the implemented compatibility backend. The target bulk/native architecture is now [native table-backed graphs](table-backed-graph-architecture.md): graph metadata registers existing wide DuckDB vertex and edge relations, GQL lowers directly over those relations, and CSR remains a derived optional physical index.

The product target is full ISO GQL support, not a permanently limited graph-query subset. The pinned conformance baseline is ISO/IEC 39075:2024 plus Technical Corrigendum 1 once its final text is published. Revision work for the next edition is tracked as a separate compatibility delta. Normal SQL continues to work unchanged. Neither building nor using the extension requires Java; Java 11+ is needed only to regenerate checked-in C++ parser sources.

## Current Status (2026-07-19)

The bootstrap and initial storage checkpoints are implemented. Before the typed `MATCH` parser migration, the focused suite passed 209 assertions across seven SQLLogicTest files; current verification counts are recorded in the implementation handoff rather than hard-coded here.

- Direct parsing, persistent graph lifecycle, session graph selection, single-vertex insert, and directed chained path insert are implemented.
- Raw storage is normalized and dictionary encoded. Property values persist as native typed `UNION` members; there is no JSON property payload.
- Multiple comma-separated fixed `MATCH` patterns and arbitrary explicit multi-hop directed paths are supported. Exact anonymous edge quantifiers and positive finite ranges with bounds through `64` are represented in the typed AST. The binder expands each permitted finite length into the same native fixed-pattern IR, and the lowerer combines range alternatives with native `UNION ALL`. A standalone unbounded anonymous edge factor supports `*`, `+`, and `{n,}` through a native DuckDB recursive CTE with a zero-hop vertex anchor, a lower-depth predicate, and an ordered used-edge list for terminating different-edge/trail semantics. Each edge occurrence carries its own left/right direction; repeated variables share binding slots, anonymous elements receive internal slots, and label/property filters and projections remain ordinary relational operators outside recursion. The `MATCH -> FILTER* -> PROJECT` graph-relational IR lowers through the parser-override path to normal DuckDB query nodes, so scans, joins, recursion, typed filters, projections, and finite-range unions use native DuckDB planning and execution; regex-based clause and literal classification has been removed.
- A connection-local CSR snapshot supports outgoing/incoming expansion, deterministic order, parallel edges, self-loops, lazy reuse, and `graph_version` invalidation.
- Matched `SET`, `REMOVE`, `DELETE`, and `DETACH DELETE` pipelines lower to native DuckDB DML on the caller connection. They support comma-separated items, dynamically typed property expressions, and `SET variable = {...}` replacement. MATCH bindings are materialized once, and the generated statements are atomic in both autocommit and explicit caller transactions. Standalone `INSERT` still uses an internal connection.
- SQL interoperability is available through `gql_graphs()`, `gql_vertices()`, `gql_edges()`, `gql_properties()`, `gql_neighbors(...)`, and `gql_csr_stats(...)`.
- Full conformance is not yet implemented. Unbounded paths cannot yet compose with other path factors or comma-separated patterns; zero-inclusive finite upper bounds, optional paths, quantified group variables, path modes/searches, undirected matching, most ISO statements and expressions, full element values, and match-and-insert remain planned work. The native DuckDB backend is the required correctness implementation for every query feature before any CSR physical alternative is enabled.

## Implementation Changes

### Extension and parser

- Bootstrap from DuckDB's extension template pinned to DuckDB v1.5.4.
- Vendor OpenGQL `GQL.g4` v1.9.0 and its Apache-2.0 license.
- Generate C++ lexer, parser, and visitor sources with ANTLR 4.13.2; commit them under `src/parser/generated`.
- Link `antlr4_static` from DuckDB's vcpkg baseline. Remove the template OpenSSL dependency.
- Add a regeneration script pinned to `antlr-4.13.2-complete.jar`, SHA-256 `eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76`.
- Register a DuckDB parser extension. Native SQL has precedence; supported GQL forms are converted into a handwritten C++ AST. Read-only `MATCH` returns a normal `SelectStatement` through DuckDB's fallback parser-override path. Matched mutations use the same path to return native DuckDB DML statements; catalog/session and standalone insert commands continue through the extension-statement planner.
- Keep ANTLR behind `GqlTransformer`: `gql_ast.hpp` owns statement, pattern, projection, identifier, literal, and recursive expression nodes; `gql_transformer.cpp` is the only parse-tree-to-AST conversion layer. `gql_parser.cpp` invokes `GqlBinder` and the backend lowerer only after the ANTLR tree has been destroyed.
- The initial binder resolves fixed-pattern node/edge variables, assigns graph/scalar/property types with nullability, validates expression operands and Boolean predicates, and produces storage-independent `GqlLogicalMatch`, `GqlLogicalFilter`, and `GqlLogicalProject` operators. The relational adapter consumes serialized bound-expression programs and constructs DuckDB parsed-plan nodes rather than source text. The existing CSR matcher remains available as backend code for later cost-based selection and differential testing.
- Classify literal kinds from grammar contexts (`exactNumericLiteral`, `approximateNumericLiteral`, `generalLiteral`, and signed-expression nodes), not from token-text heuristics. Preserve token text only as the already-classified scalar payload until typed DuckDB binding.
- CI regenerates the parser and fails if committed generated sources differ.

### Canonical raw storage

Create private schema `gql_internal` with schema version `1`:

- `graphs(graph_id UBIGINT PRIMARY KEY, graph_name VARCHAR UNIQUE, graph_version UBIGINT, created_at TIMESTAMP)`
- `objects(object_id, graph_id, kind, source_id, target_id)` stores stable object identity and topology only.
- `labels(label_id, graph_id, label_name)` and `object_labels(graph_id, object_id, label_id)` dictionary-encode labels.
- `property_keys(key_id, graph_id, key_name)` dictionary-encodes property keys per graph.
- `object_properties(graph_id, object_id, key_id, value)` stores one typed property per key. `value` is a named DuckDB `UNION` covering Boolean, signed/unsigned integer, decimal, double, string, blob, date, time, timestamp, timestamp-with-time-zone, and interval values.
- Global sequences allocate stable graph and object IDs; gaps after rollback are allowed.
- `kind=0` represents vertices with null endpoints; `kind=1` represents edges with valid source and target vertex IDs.
- Label/property mapping primary keys deduplicate assignments. Edge direction follows `source_id -> target_id`.
- Graph mutations and `graph_version` updates occur in the same DuckDB transaction.
- Raw tables remain the source of truth and are persisted in the DuckDB database. CSR data is never persisted as authoritative state.

### CSR traversal index

- The current checkpoint maintains a connection-local cache keyed by `graph_id` and validates `graph_version` on every access. The production target is a per-database LRU keyed by `(graph_id, graph_version)`.
- Each CSR snapshot contains stable-ID-to-dense-ordinal mapping, vertex IDs, outgoing offsets/neighbors/edge IDs, and incoming offsets/neighbors/edge IDs.
- Sort adjacency deterministically by neighbor ordinal and edge ID; preserve parallel edges and self-loops.
- Build CSR lazily from `gql_internal.objects` on the first explicit CSR traversal or inspection query for a graph version. Fixed-pattern `MATCH` currently stays relational.
- When database-wide sharing is added, a transaction that has modified a graph must use a transaction-local CSR snapshot that is never published before commit.
- After commit, later readers observe the new `graph_version`, making the old cached CSR ineligible; the new snapshot is rebuilt lazily.
- Planned: evict unused database-wide snapshots using `gql_csr_cache_size`, default `256MB`; setting it to zero disables cross-query caching.
- Property filtering operates directly on dictionary key IDs and typed `UNION` vectors. CSR stores connectivity and object IDs, not duplicate label or property data.

### Full ISO GQL conformance target

“Supported” means more than accepting the grammar. A feature is complete only when parsing, binding, type checking, execution, transaction behavior, diagnostics, persistence, and positive/negative conformance tests agree with the pinned standard.

- Program, session, and transaction statements, including multi-statement procedures and `NEXT`/`YIELD` data flow.
- Catalog and graph-type DDL, graph construction/copying, schema qualification, and graph focus.
- All graph modifications: `INSERT`, `SET`, `REMOVE`, `DELETE`, and `DETACH DELETE`, including match-and-modify pipelines.
- Linear and composite queries: `MATCH`, `OPTIONAL MATCH`, `LET`, `FOR`, `FILTER`, `CALL`, `SELECT`, `RETURN`, `FINISH`, ordering/paging, grouping/aggregation, and set operators.
- The complete graph-pattern algebra: multiple patterns, label expressions, predicates, direction variants, quantified and simplified patterns, path modes, path searches, shortest paths/groups, `KEEP`, and match modes.
- The standard value system, expressions, predicates, functions, casts, null/unknown behavior, graph/element/path/binding-table values, and standard type rules.
- Standard status and diagnostic behavior wherever DuckDB's extension API can represent it; documented mappings are required where the host API imposes a different envelope.

The living coverage inventory is [`iso-gql-conformance.md`](iso-gql-conformance.md); its diff-friendly source of record is `test/conformance/iso-gql-2024.tsv`. No release may describe itself as fully conforming while an applicable row remains `partial` or `planned`.

### Compiler architecture required to reach full support

1. OpenGQL lexer/parser produces a concrete parse tree for the complete pinned grammar.
2. A typed, source-located C++ AST represents every statement, clause, pattern, expression, and type. Source text is never reparsed with clause regexes.
3. A GQL binder resolves catalogs, graphs, variables, labels, property keys, procedures, and types, and applies ISO scope and nullability rules.
4. A graph-relational logical IR represents binding tables, graph patterns, paths, updates, and result shaping independently of storage.
5. Lowering first implements every query feature with native DuckDB operators, including fixed expansion, quantified paths, path modes/searches, filtering, grouping, and result shaping.
6. Only after native semantic coverage and differential tests exist may the physical layer substitute a CSR-backed implementation. CSR remains an optional derived, versioned index over canonical MVCC state, never a prerequisite for query correctness.
7. Every modifying statement executes on the caller's DuckDB transaction; no internal connection may create a second transaction boundary.

## Public Interfaces

- Direct GQL statements are the primary API; no `gql_query(string)` wrapper is required.
- `gql_graphs()` exposes graph name, ID, version, vertex count, and edge count to SQL.
- `gql_vertices()`, `gql_edges()`, and `gql_properties()` provide read-only SQL interoperability over canonical storage. Graph-name arguments remain a planned overload.
- `gql_csr_stats(graph_name)` reports cached version, vertices, edges, memory usage, build count, and cache state.
- Planned: `gql_csr_cache_size` controls the database-wide CSR memory budget.
- GQL parser errors include line, column, offending token, and a concise expected-form message.

## Delivery and Tests

1. **Conformance foundation:** pin the normative revision, maintain the exhaustive feature manifest, remove all source-text clause parsing, and represent the complete grammar as a typed AST with source locations.
2. **Binder and logical IR:** implement ISO scopes, name resolution, graph/value/path types, coercion, nullability, binding-table schemas, semantic diagnostics, and storage-independent graph-relational operators.
3. **Core language breadth:** complete all query clauses, expressions, functions, graph patterns, path modes/search, result shaping, and composite queries.
4. **Mutation and catalog breadth:** complete schema/graph/graph-type DDL, procedures, session/transaction statements, and all data modification inside the caller transaction.
5. **Physical graph engine:** complete native DuckDB execution for expansion and path queries first; then add cost-based relational-versus-CSR substitution, transaction-local deltas, database-wide versioned CSR caching, concurrent publication, and bounded eviction behind differential parity gates.
6. **Conformance and hardening:** require positive and negative tests per manifest row, CSR-versus-relational differential tests, transaction/persistence/concurrency suites, parser corpus tests, fuzzing, and repeatable performance benchmarks.

## Assumptions and Pins

- The repository was empty when this plan was written, so there is no migration or backward-compatibility requirement.
- The current checkpoint supports one selected graph per connection and unqualified graph names; this is an implementation limitation, not the final language boundary.
- Canonical storage prioritizes correctness and SQL interoperability; CSR is strictly a derived acceleration structure.
- Parser regeneration requires Java 11+, but checked-in C++ sources ensure users and normal builders need no JVM.
- DuckDB: [v1.5.4](https://github.com/duckdb/duckdb/releases/tag/v1.5.4).
- DuckDB extension template: commit [`cfaf3e236008e782d27f4341b0ee036002d0a449`](https://github.com/duckdb/extension-template/commit/cfaf3e236008e782d27f4341b0ee036002d0a449).
- OpenGQL grammar: commit [`16ea71bd320ad07fd2c46a3066afbaef7d226922`](https://github.com/opengql/grammar/commit/16ea71bd320ad07fd2c46a3066afbaef7d226922), version 1.9.0.
- ANTLR: [4.13.2](https://www.antlr.org/download.html).
- vcpkg baseline: `84bab45d415d22042bd0b9081aea57f362da3f35`.
