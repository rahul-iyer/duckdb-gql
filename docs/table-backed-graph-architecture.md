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
CALL gql_build_csr(...)
    -> prepared connection-local ordinal CSR snapshot
CALL algo.bfs(...) / algo.dfs(...) / algo.pagerank(...)
    -> dedicated graph algorithm execution
```

There is no EAV row store, no normalized execution fallback, and no `gql_register_graph_tables` API. A graph without managed tables cannot be queried; it must first be loaded with `COPY GRAPH`.

## Design goals

1. Keep graph data in typed, vectorized DuckDB columns.
2. Use DuckDB scans, joins, filters, projections, aggregates, ordering, and recursive CTEs as the default execution engine.
3. Make graph catalog metadata small and storage-independent.
4. Make bulk load close to equivalent native DuckDB CTAS performance.
5. Preserve a typed, source-located GQL AST and a storage-independent logical IR.
6. Treat CSR as a derived graph-algorithm structure, never canonical storage or
   a `MATCH` physical backend.
7. Keep graph creation, loading, querying, and dropping explicit and atomic.

## Catalog state machine

`gql_internal.graph_storage.storage_mode` has two values:

| State | Meaning | Legal operations |
|---|---|---|
| `EMPTY` | Graph exists but has no element tables | `COPY GRAPH`, `DROP GRAPH`, catalog inspection |
| `TABLE_BACKED` | Graph owns native vertex and edge tables | `MATCH`, CSR algorithm build/use, `DROP GRAPH` |

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

Ordered fixed-pattern pipelines retain their clause structure in the logical
IR: mandatory stages use `INNER_APPLY`, optional stages use `LEFT_APPLY`, and a
leading optional stage is anchored by `UNIT`. Mandatory stages may follow and
consume optional bindings without clause reordering; null-extended bindings
therefore disappear when a later mandatory correlation cannot match. An
optional stage whose variables are all already bound is an identity when its
condition fails because it has no newly introduced value to null-extend.
`LET` is a clause boundary even when its scalar expression is inlined, so a
subsequent `FILTER` is post-join rather than owned by the preceding optional
right-hand side. Optimizer predicate movement preserves `LEFT_APPLY` barriers.

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
the native recursive operators materialize every traversed node and edge,
rather than only endpoints and used-edge identities.

An unbounded anonymous directed factor lowers to a recursive CTE. Its state contains start identity, current endpoint, depth, and ordered used-edge identity. The edge list enforces different-edge/trail semantics and preserves distinct parallel-edge paths. Source-only restrictions can be pushed into the anchor.

There is deliberately no normalized fallback. Binding a MATCH against an `EMPTY` graph fails with a `COPY GRAPH` instruction.

## CSR algorithm execution

CSR is a derived, connection-local snapshot:

- a zero-map dense managed-ID fast path with sparse-ID fallback;
- outgoing offsets, ordinal neighbors, edge IDs, and label IDs;
- incoming offsets, ordinal neighbors, edge IDs, and label IDs;
- graph version and memory accounting.

Construction streams DuckDB result chunks, counts both degree vectors, computes
prefix sums, allocates every final array exactly once, and scatters each edge
into outgoing and incoming storage in one linear pass. Managed `COPY GRAPH`
IDs bypass per-vertex and per-edge validation hash tables; sparse vertex IDs
fall back to an ordinal map. Algorithm calls reuse the cached graph identity
and perform only a lightweight graph-version lookup rather than reloading the
full table/property binding.

The lifecycle is explicit:

```gql
CALL gql_build_csr('graph_name');

CALL algo.bfs('graph_name', 1, direction := 'out', max_depth := 4);
CALL algo.dfs('graph_name', 1, target_vertex_id := 42);
CALL algo.sssp('graph_name', 1);
CALL algo.pagerank('graph_name', damping := 0.85, tolerance := 1e-8);
CALL algo.wcc('graph_name');
CALL algo.scc('graph_name');
CALL algo.degree('graph_name', vertex_label := 'person');
CALL algo.closeness('graph_name', direction := 'out');
CALL algo.lcc('graph_name')
YIELD vertex_id, local_clustering_coefficient
RETURN vertex_id, local_clustering_coefficient;
CALL algo.triangle_count('graph_name')
YIELD vertex_id, triangle_count, local_clustering_coefficient
RETURN vertex_id, triangle_count, local_clustering_coefficient
ORDER BY triangle_count DESC
LIMIT 20;
```

`MATCH` never consults this snapshot, and CSR execution hints are rejected.
BFS and DFS stream deterministic traversal-tree rows and stop early under a
target, depth bound, interruption, or downstream limit. PageRank performs dense
ordinal iterations with dangling-node redistribution. WCC uses union/find over
the undirected projection, while SCC uses two linear iterative CSR traversals;
both expose the smallest stored
vertex ID as a stable component ID. Triangle counting canonicalizes a simple
undirected projection, then uses degree-oriented forward adjacency so each
triangle is visited once. Every algorithm optionally accepts a vertex label,
an edge label, both, or neither. A vertex label creates an induced vertex
projection; the optional edge label is applied inside that projection. Compact
per-vertex label offsets and IDs are built once into the CSR, while each call
materializes a byte mask for constant-time hot-loop membership checks. Direct
SQL topology changes require an explicit rebuild. CSR algorithms
are not eligible inside explicit caller transactions until transaction-local
snapshots/deltas exist.

The additional analytics follow the same immutable-array boundary:

- unweighted SSSP is a streaming BFS with an exactly-one-source contract and
  emits distance, predecessor edge, and settled order in `O(V+E)`;
- degree reads outgoing/incoming offset differences in `O(V)` without filters
  and scans induced adjacency in `O(V+E)` when either label filter is active;
- exact closeness reuses timestamped BFS scratch space for every vertex and
  reports generalized normalized outbound closeness in `O(V(V+E))`.

The current CSR has no edge-weight array. Weighted Dijkstra/SSSP therefore
remains intentionally unsupported until CSR construction accepts a typed,
non-negative weight property and publishes aligned outgoing/incoming weight
buffers.

The public composition boundary is typed GQL `CALL ... YIELD ... RETURN`, with
GQL ordering and static paging applied after the procedure result. `CALL` is a
generic logical node whose registry entry declares its arguments, output
schema, read-only/blocking properties, and input mode:

- `NONE` consumes its child as a sequencing dependency and executes once;
- `BATCH` collects declared input expressions across all child rows and invokes
  the procedure once (BFS/DFS use a matched frontier; SSSP requires a singleton
  batch source);
- `ROW` is reserved for future lateral, row-correlated procedures.

The lowerer maps this node to DuckDB's table-in/out operator with the lowered
`MATCH` query as its physical child. This is one physical plan and one statement
transaction, not a temporary table or a client-side sequence of queries. A
blocking call replaces the upstream relation, so the post-call binder scope
contains only declared `YIELD` names. The internal `gql_algorithm_call` table
function is an execution detail and is not part of the customer API.

Production CSR work still needed:

- database-wide versioned cache;
- bounded LRU eviction and memory budgets;
- reliable invalidation after native table writes;
- transaction-local deltas;
- parallel chunk processing and PageRank/traversal execution;
- repeatable algorithm benchmarks against established graph engines.

## Mutations

The former EAV mutation implementation has been replaced for the core matched forms. The compiler materializes one pre-mutation MATCH snapshot and emits ordinary DuckDB DML against the selected graph's managed base tables.

The native mutation lowering currently implements:

- standalone node and directed fixed-path `INSERT` -> sequence-backed inserts
  into the mapped vertex and edge tables;
- one fixed directed `MATCH`-and-`INSERT` path -> a materialized match snapshot
  plus set-at-a-time inserts, reusing matched node identities and evaluating
  inserted property expressions per matched row;
- `SET n.property = expression` -> typed DuckDB `UPDATE`;
- `SET n = {...}` -> schema-adaptive property clearing followed by typed
  assignments from the shared pre-mutation snapshot;
- compatibility `SET n += {...}` -> typed assignments without clearing omitted
  properties; NULL removes a property and an empty map does not advance the
  graph version;
- `LET patch = {...} SET n = patch` / compatibility `SET n += patch` -> ordered
  scalar/record aliases resolved by the binder, including nested record-field
  selection, then expanded into the same shared-snapshot clear/assignment
  mutations; parameters, map-producing functions, and independently
  materialized record values remain future value-system work;
- node and edge label `SET`/`REMOVE` -> idempotent set operations over the
  managed label/type column, with both `IS` and colon spellings; compact
  compatibility chains are expanded into the same typed mutations before
  binding while retaining original source offsets;
- `REMOVE n.property` -> nullable-column update or schema-defined absence handling;
- `DELETE edge` -> delete from the mapped edge table;
- `DELETE vertex` / `NODETACH DELETE vertex` -> delete explicit edge targets
  first, then reject when any unlisted incident edge remains;
- `DELETE path` / `DETACH DELETE path` -> binder expansion into the path's
  ordered typed edge/node identities, followed by the same constraint and
  command-rollback machinery;
- literal list/record `DELETE` targets -> recursive binder expansion of nested
  element/path leaves and record-field selections; NULL leaves disappear and
  no runtime list, record, or `UNNEST` operator is introduced;
- `DETACH DELETE vertex` -> delete incident edges then vertices atomically;
- caller-transaction rollback and an autocommit command envelope across multiple generated statements.

For one `INSERT` path, the lowerer allocates every vertex and edge identity in
one temporary single-row relation. Each generated DuckDB `MERGE INTO` then
inserts one element from that row, so edges use the exact keys allocated for
their endpoints. The temporary relation, all element writes, and the graph
version update share the mutation command envelope. Consequently, a failure
while converting or inserting a later element rolls back earlier elements in
autocommit, while an explicit caller transaction retains normal
read-your-writes and rollback behavior. A match-driven insert similarly
materializes the pre-mutation match result, allocates all new identities once
per snapshot row, and emits set-at-a-time `MERGE INTO` statements. This
preserves match cardinality, permits inserted properties to reference matched
bindings, and reuses bound node identities as path endpoints. The current
INSERT boundary is one fixed directed path per statement; standalone inserts
use literal mapped properties, while match-driven inserts accept bound scalar
expressions.

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

Remaining ISO mutation work includes multiple-path/clause `INSERT`, undirected
edge insertion, runtime `LET`/`collect`/parameter/list-index-derived delete
targets, writable-column metadata enforcement, native schema evolution for
open graphs, richer label storage, and complete ISO diagnostics. Compatibility
mutation work still includes arbitrary map expressions;
compatibility `MERGE` still needs edges/paths and `ON CREATE`/`ON MATCH` actions.

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
- explicit CSR build, algorithm reuse, refresh, and stale-snapshot diagnostics;
- deterministic BFS/DFS trees and PageRank convergence/reference comparisons;
- persistence and crash/reopen behavior;
- differential raw SQL versus GQL result tests;
- load/query throughput benchmarks plus separate algorithm benchmarks that do
  not charge CSR construction to algorithm latency.

The active SQL tests use `CREATE GRAPH` and `COPY GRAPH`. Tests that depended on EAV `INSERT`, normalized catalog tables, legacy import, or manual registration were removed with the corresponding backend.

## Near-term execution order

1. Complete native mutation coverage: standalone and match-driven fixed
   directed-path `INSERT`, plus matched `SET`, `REMOVE`, `DELETE`, and
   `DETACH DELETE`, now lower directly to managed tables; multiple-path/clause
   and undirected `INSERT`, whole-map replacement, and broader label-set
   storage remain.
2. Materialize quantified path values from native recursive execution.
3. Add native graph DDL for explicit element-table schemas instead of manual registration.
4. Expand the native conformance suite across expressions, optional matching, aggregation, and finite paths.
5. Add transaction-local write/version tracking, database-wide publication,
   bounded eviction, and parallel execution for the CSR algorithm cache.
