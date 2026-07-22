# Native DuckDB graph architecture

## Status

This document describes the active architecture after retirement of the normalized EAV backend and the public manual table-registration function.

The canonical lifecycle is:

```text
CREATE GRAPH
    -> EMPTY catalog graph
COPY GRAPH
    -> vectorized CSV/Parquet scan
    -> managed wide DuckDB vertex and edge tables
    -> TABLE_BACKED catalog graph
MATCH
    -> GQL AST and binder
    -> graph-relational logical IR
    -> native DuckDB logical plan
    -> DuckDB physical operators
optional MATCH /*+ CSR */
    -> prepared connection-local CSR path scan
```

There is no EAV row store, no normalized execution fallback, and no `gql_register_graph_tables` API. A graph without managed tables cannot be queried; it must first be loaded with `COPY GRAPH`.

## Design goals

1. Keep graph data in typed, vectorized DuckDB columns.
2. Use DuckDB scans, joins, filters, projections, aggregates, ordering, and recursive CTEs as the default execution engine.
3. Make graph catalog metadata small and storage-independent.
4. Make bulk load close to equivalent native DuckDB CTAS performance.
5. Preserve a typed, source-located GQL AST and a storage-independent logical IR.
6. Treat CSR as an optional derived physical structure, not canonical storage.
7. Keep graph creation, loading, querying, and dropping explicit and atomic.

## Catalog state machine

`gql_internal.graph_storage.storage_mode` has two values:

| State | Meaning | Legal operations |
|---|---|---|
| `EMPTY` | Graph exists but has no element tables | `COPY GRAPH`, `DROP GRAPH`, catalog inspection |
| `TABLE_BACKED` | Graph owns native vertex and edge tables | `MATCH`, CSR build/use, `DROP GRAPH` |

`CREATE GRAPH` inserts `EMPTY`. `COPY GRAPH` requires `EMPTY` plus zero element-table mappings and transitions atomically to `TABLE_BACKED`. A failed load rolls back tables and metadata, leaving the graph `EMPTY`.

Legacy databases containing a different storage mode are rejected with an instruction to recreate and reload the graph. There is no implicit migration or compatibility read path.

## Physical data layout

For graph ID `N`, the current loader creates two managed tables in `gql_data`.

### Vertex table

```sql
CREATE TABLE gql_data.graph_N_vertices AS
SELECT
    row_number() OVER ()::UBIGINT AS __gql_id,
    CAST(input_id AS VARCHAR)     AS __gql_external_id,
    normalized_label             AS __gql_label,
    property_1,
    property_2,
    ...
FROM staged_vertices;
```

`__gql_id` is the stable internal identity used by joins and CSR. `__gql_external_id` preserves import identity. Properties retain native DuckDB types.

### Edge table

```sql
CREATE TABLE gql_data.graph_N_edges AS
SELECT
    row_number() OVER ()::UBIGINT AS __gql_edge_id,
    source.__gql_id               AS __gql_source_id,
    target.__gql_id               AS __gql_target_id,
    normalized_type               AS __gql_type,
    property_1,
    property_2,
    ...
FROM staged_edges
JOIN graph_N_vertices source ON ...
JOIN graph_N_vertices target ON ...;
```

The managed layout currently assumes one vertex table, one directed edge table, one scalar vertex label, one scalar edge type, and integer-compatible internal identities.

## Metadata layout

The private `gql_internal` schema contains metadata only.

### `graphs`

- stable graph ID and unique name;
- graph version;
- creation timestamp.

### `graph_storage`

- `EMPTY` or `TABLE_BACKED` state;
- default catalog/schema;
- schema version;
- CSR policy.

### `graph_element_tables`

- vertex/edge kind;
- fully qualified physical table name;
- key columns;
- `MANAGED` ownership;
- read/write capability.

### `graph_edge_endpoints`

- source and target vertex table mappings;
- edge endpoint columns;
- referenced vertex key columns.

### `graph_label_mappings`

- scalar label/type column mappings today;
- schema leaves room for static and list mappings later.

### `graph_property_mappings`

- GQL property name to physical column;
- DuckDB/GQL type text;
- nullability and writability.

The catalog never stores one row per vertex property or label membership.

## Bulk load

`COPY GRAPH` accepts Neo4j semantic headers in CSV/Parquet inputs. Loading uses DuckDB's native readers and set-at-a-time SQL:

1. Scan each file through `read_csv` or `read_parquet` into temporary staging tables.
2. Parse and validate semantic headers.
3. Optionally scan for null/duplicate node IDs and missing endpoints.
4. Create the vertex table with one CTAS.
5. Create the edge table with one CTAS and vectorized endpoint joins.
6. Record element, endpoint, label, and property mappings.
7. Transition the graph to `TABLE_BACKED` and commit.

`OPTIONS (VALIDATE FALSE)` skips uniqueness and referential-integrity validation scans for trusted data. It does not change the output layout or type conversions.

Potential loader improvements:

- fuse staging and final CTAS where header handling permits;
- avoid materializing the import row number when unused;
- build an endpoint lookup optimized for very large imports;
- accept multiple input files and separate header files;
- support list properties, multiple labels, composite IDs, and incremental actions;
- expose progress and per-phase profiling.

## Compiler and native execution

The execution spine is:

```text
OpenGQL parse tree
  -> owned typed AST with source ranges
  -> binder: scopes, bindings, labels, properties, types
  -> graph-relational logical IR
  -> DuckDB parsed logical plan
  -> DuckDB optimizer
  -> DuckDB physical operators
```

Fixed patterns lower to one scan alias per vertex/edge occurrence plus equality predicates for endpoints. Label and property predicates address mapped columns directly. DuckDB remains responsible for predicate pushdown, join ordering, aggregation, sorting, limits, parallel scans, and physical operator selection.

Direct node and edge projections are native DuckDB `STRUCT` values rather than
formatted graph strings. A node contains `__gql_id`, `__gql_labels`, and every
mapped property column. An edge contains `__gql_id`, `__gql_type`,
`__gql_source`, `__gql_target`, and every mapped property column. Property
fields are ordered case-insensitively by GQL property name so the struct type is
deterministic; values retain their native DuckDB types. An unmatched optional
binding produces SQL `NULL` for the complete struct. The `__gql_` prefix is
reserved by the managed-table importer, preventing property-name collisions.

A named fixed path projects as `STRUCT(nodes LIST<STRUCT>, edges LIST<STRUCT>)`.
Both lists preserve pattern traversal order, while an edge struct retains its
physical source and target identities even when the pattern traverses it in
reverse. A missing optional path is SQL `NULL`; a one-node path has an empty
edge list. Quantified/VLP path projection remains explicitly unsupported until
the recursive and CSR operators materialize every traversed node and edge,
rather than only endpoints and used-edge identities.

An unbounded anonymous directed factor lowers to a recursive CTE. Its state contains start identity, current endpoint, depth, and ordered used-edge identity. The edge list enforces different-edge/trail semantics and preserves distinct parallel-edge paths. Source-only restrictions can be pushed into the anchor.

There is deliberately no normalized fallback. Binding a MATCH against an `EMPTY` graph fails with a `COPY GRAPH` instruction.

## CSR execution

CSR is a derived, connection-local snapshot:

- dense vertex ordinal map;
- outgoing offsets, neighbors, edge IDs, and label IDs;
- incoming offsets, neighbors, edge IDs, and label IDs;
- graph version and memory accounting.

The lifecycle is explicit:

```sql
SELECT * FROM gql_build_csr('graph_name');

MATCH /*+ CSR */ (a:Label)-[:TYPE]->+(b:Label)
RETURN element_id(b);
```

The hint is accepted only for an eligible anonymous unbounded directed path. It never silently changes unsupported query shapes. Direct SQL topology changes require an explicit rebuild. CSR is not eligible inside explicit caller transactions until transaction-local snapshots/deltas exist.

Production CSR work still needed:

- database-wide versioned cache;
- bounded LRU eviction and memory budgets;
- reliable invalidation after native table writes;
- transaction-local deltas;
- parallel construction and traversal;
- cost-based choice between native recursion and CSR.

## Mutations

The former EAV mutation implementation has been replaced for the core matched forms. The compiler materializes one pre-mutation MATCH snapshot and emits ordinary DuckDB DML against the selected graph's managed base tables.

The native mutation lowering currently implements:

- standalone node and directed fixed-path `INSERT` -> sequence-backed inserts
  into the mapped vertex and edge tables;
- `SET n.property = expression` -> typed DuckDB `UPDATE`;
- `REMOVE n.property` -> nullable-column update or schema-defined absence handling;
- `DELETE edge` -> delete from the mapped edge table;
- `DELETE vertex` -> reject when incident edges exist;
- `DETACH DELETE vertex` -> delete incident edges then vertices atomically;
- caller-transaction rollback and an autocommit command envelope across multiple generated statements.

For one `INSERT` path, the lowerer allocates every vertex and edge identity in
one temporary single-row relation. Each generated DuckDB `MERGE INTO` then
inserts one element from that row, so edges use the exact keys allocated for
their endpoints. The temporary relation, all element writes, and the graph
version update share the mutation command envelope. Consequently, a failure
while converting or inserting a later element rolls back earlier elements in
autocommit, while an explicit caller transaction retains normal
read-your-writes and rollback behavior. The current INSERT boundary is one
fixed path with literal mapped properties and directed edges.

As a separate Cypher-compatibility surface, standalone single-vertex `MERGE`
lowers to DuckDB's native `MERGE INTO` operator. The match key is the complete
supplied label/property pattern. The insert branch writes the managed wide
vertex table directly, while a per-graph DuckDB sequence allocates concurrency-
safe internal identities. The sequence is initialized after `COPY GRAPH`, is
persistent, and is dropped with the graph. Like ordinary database sequences,
candidate values can be consumed by matched or rolled-back statements, so
internal IDs are unique but not gap-free.

This compatibility clause is intentionally outside the ISO conformance
manifest. Its current boundary is one vertex, zero or one label, mapped scalar
literal properties, and no `ON CREATE` or `ON MATCH` actions. Paths, edges,
action clauses, and schema evolution for unknown properties remain pending.

Remaining ISO mutation work includes match-driven and multi-path `INSERT`,
undirected edge insertion, whole-map replacement, writable-column metadata
enforcement, native schema evolution for open graphs, richer label storage,
and complete ISO diagnostics. Compatibility `MERGE` still needs edges/paths
and `ON CREATE`/`ON MATCH` actions.

## Transactions and ownership

- `COPY GRAPH` is currently autocommit-only and owns one transaction covering physical tables and metadata.
- Native MATCH observes ordinary DuckDB MVCC behavior.
- Native matched mutations use the caller's DuckDB transaction.
- Direct SQL changes to managed tables are immediately visible to native MATCH.
- CSR is outside caller transactions today.
- `DROP GRAPH` drops only `MANAGED` element tables, then removes metadata.

Because the public referenced-table registration path is gone, every current element table is graph-owned.

## Verification

Required native coverage includes:

- create/load/select/drop lifecycle and rollback on load failure;
- CSV and Parquet typed-property preservation;
- fixed MATCH, predicates, projection, optional, grouping, ordering, and limits;
- node/edge struct projection, including null optional bindings;
- fixed-path struct projection in forward, reverse, node-only, and optional forms;
- fixture-aware compatibility assertions for unambiguous node, edge, and fixed-path values;
- native recursive VLP on cycles, parallel edges, reverse direction, and zero-hop paths;
- explicit CSR build, reuse, refresh, and ineligible-hint diagnostics;
- persistence and crash/reopen behavior;
- differential raw SQL versus GQL result tests;
- load/query throughput benchmarks without charging CSR construction to query latency.

The active SQL tests use `CREATE GRAPH` and `COPY GRAPH`. Tests that depended on EAV `INSERT`, normalized catalog tables, legacy import, or manual registration were removed with the corresponding backend.

## Near-term execution order

1. Complete native mutation coverage: standalone directed-path `INSERT` and
   matched `SET`, `REMOVE`, `DELETE`, and `DETACH DELETE` now lower directly to
   managed tables; match-driven/multi-path/undirected `INSERT`, whole-map
   replacement, and broader label-set storage remain.
2. Materialize quantified path values from recursive/CSR execution.
3. Add native graph DDL for explicit element-table schemas instead of manual registration.
4. Expand the native conformance suite across expressions, optional matching, aggregation, and finite paths.
5. Add transaction-local write/version tracking for CSR invalidation, then introduce cost-based native-versus-CSR planning after both paths have comparable semantics.
