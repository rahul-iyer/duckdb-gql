# DuckDB ISO GQL implementation plan

## Product target

Build a C++17 DuckDB extension that accepts GQL directly, stores graphs in managed native DuckDB tables, lowers the complete language through a typed compiler pipeline, and optionally substitutes prepared CSR operators for eligible analytical traversals.

The target is full ISO/IEC 39075:2024 conformance, not a permanently limited graph-query subset. The source of record is [`test/conformance/iso-gql-2024.tsv`](../test/conformance/iso-gql-2024.tsv), checked by `python3 scripts/check_gql_conformance.py --release`.

## Current status (2026-07-20)

- Direct OpenGQL parsing, owned AST nodes, initial binding/type checks, logical MATCH IR, and DuckDB plan lowering exist.
- `CREATE GRAPH`, `DROP GRAPH`, `SESSION SET GRAPH`, and atomic `COPY GRAPH` are implemented.
- Canonical graph rows live only in graph-owned wide DuckDB vertex and edge tables.
- The normalized EAV backend, legacy `gql_load_graph`, SQL introspection over EAV rows, matched EAV mutation lowerer, and public `gql_register_graph_tables` function have been removed.
- Fixed directed MATCH, filters, projections, optional matching, aggregation, ordering/paging, finite anonymous quantifiers, and one anonymous unbounded directed factor use native DuckDB plans.
- Native variable-length execution uses recursive CTEs with ordered used-edge identity for different-edge/trail semantics.
- Explicit connection-local CSR build and hinted unbounded-path execution are implemented for managed native graphs.
- Matched property `SET`, `REMOVE`, edge/node `DELETE`, and `DETACH DELETE` lower to ordinary DuckDB DML over managed native tables. Multiple items use one pre-mutation snapshot, participate in the caller transaction, and are atomic in autocommit. `INSERT` and whole-map replacement remain pending.
- A project-owned Cypher-compatible single-vertex `MERGE` lowers to DuckDB's native `MERGE INTO`; it is intentionally not recorded as ISO GQL conformance. Edge/path MERGE and `ON CREATE`/`ON MATCH` remain pending.

## Required compiler architecture

1. OpenGQL lexer/parser produces a concrete parse tree for the pinned grammar.
2. A typed, source-located C++ AST represents every statement, clause, expression, pattern, path, and type.
3. A GQL binder resolves catalogs, graphs, variables, labels, properties, procedures, scopes, overloads, nullability, and coercions.
4. A graph-relational logical IR represents scans, expansion, filtering, projection, grouping, paths, composition, and mutation independently of storage.
5. Native lowering constructs DuckDB logical plans; DuckDB chooses physical scans, joins, filters, aggregation, sorting, recursion, and parallel execution.
6. CSR is an optional derived physical alternative enabled only behind semantic parity tests.
7. Every modifying command participates in the caller's DuckDB transaction.

## P0: semantic and native execution foundation

1. Complete typed AST coverage with source ranges and no source-text reparsing.
2. Complete binder/type system for graph and scalar values, scopes, nullability, coercion, overloads, and diagnostics.
3. Complete storage-independent graph-relational logical IR.
4. Complete core query behavior: fixed patterns, optional matching, label expressions, predicates, projection, distinctness, grouping, aggregation, ordering, offset, and limit.
5. Complete core mutation directly over managed tables:
   - `INSERT` target-table selection and identity allocation;
   - typed property `SET` is implemented; whole-map replacement remains;
   - `REMOVE` is implemented for mapped nullable property columns;
   - edge/node `DELETE` and `DETACH DELETE` are implemented;
   - match-and-modify pipelines.
6. Caller-transaction correctness, including read-your-writes, rollback, persistence, and graph/CSR version publication.
7. Manifest-backed positive, negative, type, scope, transaction, diagnostic, and native-versus-CSR differential tests.

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
4. Cost-based native-versus-CSR planning using graph statistics and selectivity estimates.
5. Database-wide versioned CSR cache with memory budgets, bounded LRU eviction, parallel construction/traversal, and transaction-local deltas.
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
- `gql_build_csr(graph_name)` builds/refreshes a prepared CSR.
- `gql_neighbors(graph_name, vertex_id, direction)` inspects prepared adjacency.
- `gql_csr_stats(graph_name)` reports cached version, counts, memory, and build count.
- `MATCH /*+ CSR */` requests CSR only for an eligible query and otherwise fails explicitly.

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
- do not charge offline CSR construction to query latency;
- retain exact commands, dataset versions, thread count, warmups, repetitions, and machine metadata;
- use raw SQL baselines that match the GQL query rather than weaker reachability proxies.

## Pins

- DuckDB v1.5.4
- OpenGQL grammar v1.9.0
- ANTLR 4.13.2
- C++17
- Java 11+ only for parser regeneration
