# DuckDB ISO GQL implementation plan

## Product target

Build a C++17 DuckDB extension that accepts GQL directly, stores graphs in
managed native DuckDB tables, lowers the complete language through a typed
compiler pipeline, and provides a separate prepared-CSR subsystem for explicit
graph algorithm calls.

The target is full ISO/IEC 39075:2024 conformance, not a permanently limited graph-query subset. The source of record is [`test/conformance/iso-gql-2024.tsv`](../test/conformance/iso-gql-2024.tsv), checked by `python3 scripts/check_gql_conformance.py --release`.

## Current status (2026-07-22)

- Direct OpenGQL parsing, owned AST nodes, initial binding/type checks, logical MATCH IR, and DuckDB plan lowering exist.
- `CREATE GRAPH`, `DROP GRAPH`, `SESSION SET GRAPH`, and atomic `COPY GRAPH` are implemented.
- Canonical graph rows live only in graph-owned wide DuckDB vertex and edge tables.
- The normalized EAV backend, legacy `gql_load_graph`, SQL introspection over EAV rows, matched EAV mutation lowerer, and public `gql_register_graph_tables` function have been removed.
- Arbitrarily ordered fixed directed `MATCH` and `OPTIONAL MATCH` stages,
  clause-scoped filters and scalar `LET` boundaries, projections, aggregation,
  ordering/paging, finite anonymous quantifiers, and one anonymous unbounded
  directed factor use native DuckDB plans.
- Native variable-length execution uses recursive CTEs with ordered used-edge identity for different-edge/trail semantics.
- Explicit connection-local CSR build plus streaming BFS/DFS, dense PageRank,
  weak/strong components, and triangle counting are implemented for managed
  native graphs through DuckDB `CALL`. CSR is not a `MATCH` execution backend.
- Standalone node and directed fixed-path `INSERT`, fixed directed
  `MATCH`-and-`INSERT` pipelines, matched property `SET`, `REMOVE`, edge/node
  `DELETE`, and `DETACH DELETE` lower to ordinary DuckDB DML over managed
  native tables. Pipeline inserts reuse matched endpoints and evaluate
  properties per matched row. They participate in the caller transaction and
  are atomic in autocommit. Property-map replacement, explicit-map merge
  compatibility, node/edge label sets including compact compatibility chains,
  and order-independent constrained deletion share the same snapshot/command
  envelope. Fixed path-variable
  deletion and literal nested list/record targets expand into those same typed
  edge/node targets without runtime collection materialization. Multiple
  insert paths/clauses, undirected insertion, and runtime `LET`/`collect`/
  list-index-derived delete targets remain pending.
- A project-owned Cypher-compatible single-vertex `MERGE` lowers to DuckDB's native `MERGE INTO`; it is intentionally not recorded as ISO GQL conformance. Edge/path MERGE and `ON CREATE`/`ON MATCH` remain pending.

## Required compiler architecture

1. OpenGQL lexer/parser produces a concrete parse tree for the pinned grammar.
2. A typed, source-located C++ AST represents every statement, clause, expression, pattern, path, and type.
3. A GQL binder resolves catalogs, graphs, variables, labels, properties, procedures, scopes, overloads, nullability, and coercions.
4. A graph-relational logical IR represents scans, expansion, filtering, projection, grouping, paths, composition, and mutation independently of storage.
5. Native lowering constructs DuckDB logical plans; DuckDB chooses physical scans, joins, filters, aggregation, sorting, recursion, and parallel execution.
6. CSR is a derived, ordinal graph-algorithm substrate invoked explicitly via
   `CALL`; it does not participate in GQL query planning.
7. Every modifying command participates in the caller's DuckDB transaction.

## P0: semantic and native execution foundation

1. Complete typed AST coverage with source ranges and no source-text reparsing.
2. Complete binder/type system for graph and scalar values, scopes, nullability, coercion, overloads, and diagnostics.
3. Complete storage-independent graph-relational logical IR.
4. Complete core query behavior: fixed patterns, optional matching, label expressions, predicates, projection, distinctness, grouping, aggregation, ordering, offset, and limit.
5. Complete core mutation directly over managed tables:
   - standalone node and directed fixed-path `INSERT`, plus fixed directed
     match-and-insert pipelines, are implemented with target-table validation,
     sequence-backed identity allocation, and per-match-row expressions;
   - typed property `SET`, `SET element = {...}` replacement, and LET-backed
     record-map replacement/merge are implemented through the shared snapshot
     mutation envelope;
   - `REMOVE` is implemented for mapped nullable property columns;
   - edge/node `DELETE` and `DETACH DELETE` are implemented;
   - match-and-modify pipelines.
6. Caller-transaction correctness, including read-your-writes, rollback, persistence, and graph/CSR version publication.
7. Manifest-backed positive, negative, type, scope, transaction, diagnostic,
   and native-relational differential tests.

## P1: broad language and graph capability

1. Complete arithmetic, Boolean, comparison, null, label, endpoint, identity, cast, and parameter expressions.
2. Complete numeric, string, aggregate, list, element, temporal, duration, and path functions.
3. Complete pattern algebra: concatenation, alternation, union, parentheses, simplified patterns, general quantifiers, and optional factors.
4. Implement walk, trail, simple, and acyclic modes plus all/any/shortest searches, shortest groups, and `KEEP`.
5. Complete query composition: `LET`, `FOR`, ordinality, `SELECT`, `FINISH`, set operations, and `OTHERWISE`.
6. Add catalog/schema features: graph types, element types, inheritance, open/closed graphs, qualification, copy/replace, `AT SCHEMA`, and `USE GRAPH`.

## P2: final conformance and production hardening

1. Multi-statement programs, nested procedures, `NEXT`, `YIELD`, and named/inline calls.
2. Complete session behavior: graph/schema reset, time zones, parameters, and close.
3. ISO-aligned status conditions and precise source locations across every compiler/runtime phase.
4. Graph-specific degree, fanout, endpoint/label correlation, and path
   selectivity estimates for native recursive planning. Ordinary fixed MATCH
   regions lower as one native join graph and deliberately reuse DuckDB's table
   statistics, filter pushdown, and inner-join enumeration.
5. Database-wide versioned algorithm CSR cache with memory budgets, bounded LRU
   eviction, parallel construction/algorithms, and transaction-local deltas.
6. Concurrent reader/writer tests, crash/reopen tests, fuzzing, parser corpus testing, repeatable benchmarks, and memory-budget tests.
7. Move every applicable conformance manifest row to `implemented` and pass the release gate.

## Native storage and load work

The active physical model is documented in [table-backed-graph-architecture.md](table-backed-graph-architecture.md). Immediate storage work:

- native GQL DDL for explicit vertex/edge table schemas;
- multiple element tables and endpoint mappings per graph;
- composite keys, multiple/static/list labels, and open-graph extra properties;
- multi-file and incremental `COPY GRAPH`;
- schema evolution and mutation target selection;
- direct write tracking for graph version and CSR invalidation.

## Public interfaces

- Direct GQL statements are primary.
- `gql_graphs()` reports graph identity, version, and native row counts.
- `CALL gql_build_csr(graph_name)` builds/refreshes a prepared algorithm CSR.
- `CALL gql_neighbors(graph_name, vertex_id, direction)` inspects prepared adjacency.
- `gql_csr_stats(graph_name)` reports cached version, counts, memory, and build count.
- `CALL algo.bfs(...)` and `CALL algo.dfs(...)` stream deterministic traversal trees.
- `CALL algo.sssp(...)` streams one-source unweighted distances and predecessors.
- `CALL algo.pagerank(...)` returns normalized ranks, iteration counts, and convergence status.
- `CALL algo.wcc(...)` and `CALL algo.scc(...)` return stable component IDs and sizes.
- `CALL algo.triangle_count(...)` returns local/global triangle metrics and clustering coefficients.
- `CALL algo.lcc(...)` returns the direction-preserving LDBC Graphalytics local clustering coefficient.
- `CALL algo.degree(...)` returns incoming, outgoing, and total edge incidence counts.
- `CALL algo.closeness(...)` returns exact generalized closeness with reachability diagnostics.
- Every algorithm accepts optional vertex- and edge-label filtering. Vertex
  labels define an induced algorithm projection; omitted filters preserve the
  full-graph behavior.
- `CALL algo.*(...) YIELD ... RETURN ... ORDER BY ... LIMIT ...` is the typed
  customer composition surface; `system.algo` remains internal lowering.
- `MATCH -> CALL -> YIELD -> RETURN` is one plan. Procedure registry metadata
  selects `NONE`, future lateral `ROW`, or whole-input `BATCH` execution;
  BFS/DFS use `BATCH` for matched frontiers, SSSP uses a singleton-constrained
  batch source, and graph-global algorithms use `NONE` and execute once after
  consuming their sequencing child.
- `MATCH /*+ CSR */` is rejected because MATCH always uses native execution.

## Verification gates

A feature is supported only when parsing, binding, typing, execution, transaction behavior, persistence, diagnostics, and positive/negative tests agree.

```sh
make debug
./build/debug/test/unittest "test/sql/gql*"
python3 scripts/check_gql_conformance.py
```

Release builds additionally require:

```sh
python3 scripts/check_gql_conformance.py --release
```

Benchmark rules:

- compare equivalent physical types and semantics;
- report load and query phases separately;
- do not charge offline CSR construction to algorithm latency;
- retain exact commands, dataset versions, thread count, warmups, repetitions, and machine metadata;
- use raw SQL baselines that match the GQL query rather than weaker reachability proxies.

## Pins

- DuckDB v1.5.4
- OpenGQL grammar v1.9.0
- ANTLR 4.13.2
- C++17
- Java 11+ only for parser regeneration
