# DuckDB ISO GQL extension

`gql` is a C++17 DuckDB extension whose product target is full ISO GQL support. The current implementation is an early, explicitly incomplete checkpoint: it parses GQL directly, keeps canonical data in normalized DuckDB tables, lowers fixed patterns to DuckDB relational plans, and provides a versioned in-memory CSR snapshot for explicit traversal APIs.

It does not store graph properties as JSON. Labels and property keys are dictionary encoded, while values use a named DuckDB `UNION` of native scalar types.

## Current implementation checkpoint

```sql
CREATE GRAPH social ANY;
SESSION SET GRAPH social;

INSERT (:Person {name: 'Ada', age: 42})
       -[:KNOWS {since: 2020}]->
       (:Person {name: 'Grace'});

MATCH (a:Person)-[e:KNOWS]->(b:Person)
WHERE e.since >= 2020
RETURN a.name AS source_name,
       e.since AS since,
       b.name AS target_name;
```

Implemented:

- `CREATE GRAPH [IF NOT EXISTS] <name> ANY`
- `DROP GRAPH [IF EXISTS] <name>`
- `SESSION SET GRAPH <name>`
- Single-vertex and directed, chained path `INSERT`
- Boolean, integer, decimal, double, and string property literals
- Multiple comma-separated fixed `MATCH` patterns, including arbitrary explicit multi-hop paths with per-edge left/right direction, exact anonymous edge quantifiers `{1}` through `{64}`, finite ranges such as `{1,3}`, and one-factor unbounded paths using `*`, `+`, or `{n,}`; repeated variables share one binding and anonymous elements receive internal bindings
- Simple `OPTIONAL MATCH`, including one null-extended row when no match exists
- `:` and `IS` label constraints, `element_id(...)`, typed property projections, and aliases
- Graph-pattern `WHERE` and linear `FILTER` over fixed `MATCH`, with comparison, arithmetic, Boolean, and null predicates
- `RETURN DISTINCT`, grouping and `COUNT`/`SUM`/`AVG`/`MIN`/`MAX` aggregates, plus `ORDER BY`, static `OFFSET`, and static `LIMIT`
- Common scalar functions lowered to DuckDB operators: `LOWER`, `UPPER`, `LEFT`, `RIGHT`, `TRIM`, `LTRIM`, `RTRIM`, `CHAR_LENGTH`, `ABS`, `MOD`, `SQRT`, `FLOOR`, and `CEIL`, plus string concatenation with `||`
- Match-and-modify pipelines for multi-item property/label `SET` and `REMOVE`, whole-map replacement, dynamic property expressions, edge or isolated-node `DELETE`, and node `DETACH DELETE`
- Atomic Neo4j-header bulk import from CSV or Parquet through native DuckDB scans
- Native SQL alongside GQL

The OpenGQL parse tree is transformed immediately into an owned, source-located C++ AST. `MATCH` queries pass through a scope-aware binder and storage-independent `MATCH -> FILTER* -> PROJECT` logical IR. Each permitted finite positive length is expanded by the binder into an ordinary fixed pattern; finite ranges become native `UNION ALL` branches so distinct matches are not deduplicated. A single unbounded anonymous edge factor lowers to a native DuckDB recursive CTE. Its zero-hop vertex anchor implements `*`, its depth predicate implements `+` and `{n,}`, and its ordered used-edge list enforces terminating different-edge/trail semantics on cyclic graphs while preserving distinct parallel-edge paths. Endpoint label/property joins, filters, projections, optional null-extension, aggregation, distinctness, ordering, paging, and matched mutations stay in native DuckDB plans. Property predicates extract the required member from the persistent typed `UNION`; dynamic property projections are converted transiently to DuckDB `VARIANT`. The planner does not retain ANTLR nodes or reparse source text. Recognized unsupported forms fail explicitly rather than silently changing semantics. Current unbounded-path limitations are composition with additional pattern factors or comma-separated patterns, quantified group variables, and explicit path modes/searches. Other major gaps include compound optional-query composition, general undirected matching, the complete expression and function systems, match-and-insert, full element values, procedures, and typed graph schemas.

## Storage and CSR

Canonical persistent storage lives in the private `gql_internal` schema:

- `objects` stores stable IDs and vertex/edge topology.
- `labels` plus `object_labels` dictionary-encode label membership.
- `property_keys` plus `object_properties` store one native typed value per object/key.
- `graphs.graph_version` changes atomically with each successful GQL mutation.

CSR is derived and rebuildable. It is not used by the current `MATCH` backend; all implemented query semantics execute through native DuckDB operators. `gql_neighbors` and `gql_csr_stats` build CSR lazily only for explicit traversal and inspection. A snapshot maps stable vertex IDs to dense ordinals and contains separate outgoing and incoming `offsets`, neighbor IDs, and edge IDs. Adjacency is deterministic; parallel edges and self-loops are preserved. A graph-version mismatch invalidates the snapshot automatically. CSR may become an optional physical alternative only after native query coverage and differential parity tests are complete.

The current cache is connection-local. A database-wide memory-budgeted LRU remains a hardening milestone. Matched `SET`/`REMOVE`/`DELETE` statements lower to DuckDB DML on the caller connection, support comma-separated items, dynamically typed property expressions, and whole-property-map replacement, and participate in explicit caller `BEGIN`/`ROLLBACK` transactions. MATCH bindings and SET values are materialized once per command. In autocommit mode, the generated native statements are wrapped in one transaction and any intermediate failure rolls back the complete GQL command. Standalone `INSERT` still executes through an internal connection and does not yet participate in the caller transaction.

SQL interoperability functions are available:

```sql
SELECT * FROM gql_graphs();
SELECT * FROM gql_vertices();
SELECT * FROM gql_edges();
SELECT * FROM gql_properties();
SELECT * FROM gql_neighbors('social', 1, 'out');
SELECT * FROM gql_csr_stats('social');
```

## Neo4j-format bulk import

Create an empty graph, then load one node file and an optional relationship file with the same call:

```sql
CREATE GRAPH social ANY;

SELECT *
FROM gql_load_graph(
    'social',
    'nodes.csv',
    'relationships.csv',
    'csv'
);
```

Use `'parquet'` as the last argument for Parquet files, or pass `NULL` as the relationship path for a node-only import. CSV and Parquet use the same [Neo4j field-header convention](https://neo4j.com/docs/operations-manual/current/import/):

```text
personId:ID(People),name:string,age:int,:LABEL
p1,Ada,42,Person;Researcher
p2,Grace,37,Person
```

```text
:START_ID(People),:END_ID(People),:TYPE,since:int
p1,p2,KNOWS,2020
```

The importer currently supports one embedded-header node file, one embedded-header relationship file, one node `:ID` column, optional `:ID` groups, any number of `:LABEL` columns, one relationship `:TYPE`, and scalar properties. A named ID such as `personId:ID` is also persisted as the `personid` property; an anonymous `:ID` is import-only. GQL identifiers are normalized to lowercase, including imported label/type values and property names.

The target graph must already exist and be empty. Duplicate or missing node IDs, missing relationship endpoints, bad property casts, and unsupported types abort the complete command. Nodes, relationships, dictionaries, properties, and the single `graph_version` increment commit atomically.

Not yet supported by this initial compatibility slice: multiple files per category, separate CSV headers, Parquet name-mapping header files, composite IDs, incremental `:ACTION` imports, list properties, points, and durations.

## Build and test

The repository pins DuckDB v1.5.4, OpenGQL grammar v1.9.0, ANTLR 4.13.2, and its vcpkg baseline. The frontend and runtime are C++17. Generated C++ parser sources are checked in, so Java is not required to build or use the extension. Java 11+ is needed only to regenerate them.

```sh
make debug VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake"
```

For a focused incremental build and test run:

```sh
cmake --build build/debug --target unittest gql_loadable_extension -j2
./build/debug/test/unittest "test/sql/gql*"
python3 scripts/check_gql_conformance.py
```

Regenerate the parser with:

```sh
./scripts/regenerate_parser.sh
```

See [the implementation plan](docs/gql-implementation-plan.md) for the current compiler and conformance delivery checkpoints. [The native table-backed graph architecture](docs/table-backed-graph-architecture.md) defines the target storage, loading, native execution, mutation, and optional CSR design. [The conformance program](docs/iso-gql-conformance.md) defines what “full ISO GQL support” means and records current coverage without overclaiming it.
