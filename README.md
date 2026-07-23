# DuckGQL

`duckgql` is a C++17 extension providing experimental ISO GQL support for
DuckDB. The current implementation uses one canonical storage model:
graph-owned native DuckDB vertex and edge tables. There is no normalized EAV
compatibility backend and no manual table-registration API.

## Current workflow

Create an empty graph, bulk-load Neo4j-header CSV or Parquet files, select the graph, and query it:

```sql
CREATE GRAPH social ANY;

COPY GRAPH social FROM (
    VERTICES 'nodes.csv',
    EDGES 'relationships.csv'
) FORMAT NEO4J;

SESSION SET GRAPH social;

MATCH (a:Person)-[e:KNOWS]->(b:Person)
WHERE e.since >= 2020
RETURN a.name AS source_name,
       e.since AS since,
       b.name AS target_name;
```

`CREATE GRAPH` creates an `EMPTY` catalog entry. `COPY GRAPH` performs vectorized CSV/Parquet scans and CTAS operations, creates one managed wide vertex table and one managed wide edge table, records their mappings, and atomically moves the graph to `TABLE_BACKED`. `DROP GRAPH` removes both the metadata and the graph-owned tables.

## Implemented query surface

- Fixed directed `MATCH` patterns, including multiple comma-separated patterns and explicit multi-hop paths.
- `OPTIONAL MATCH`, `WHERE`, and linear `FILTER`.
- Label/type constraints, `element_id(...)`, mapped property access, aliases, and typed predicates.
- `DISTINCT`, grouping, aggregates, ordering, static offset, and static limit.
- Exact and ranged anonymous edge quantifiers plus one anonymous unbounded directed `*`, `+`, or `{n,}` factor.
- Native variable-length execution through DuckDB recursive CTEs with different-edge/trail semantics.
- Common numeric and string functions lowered to DuckDB expressions.
- Explicit CSR-backed BFS, DFS, PageRank, weak/strong components, and triangle
  counting exposed through native DuckDB `CALL` statements.

Native DuckDB relations are authoritative. Fixed MATCH and the default variable-length path implementation lower to ordinary DuckDB scans, joins, filters, projections, aggregation, ordering, and recursive CTE operators.

Standalone node and directed fixed-path `INSERT`, fixed directed
`MATCH`-and-`INSERT` pipelines, matched property `SET`, `REMOVE`, edge/node
`DELETE`, and `DETACH DELETE` lower directly to managed native DuckDB tables.
Pipeline inserts reuse matched nodes as endpoints and evaluate inserted
properties once per matched row. They participate in explicit caller
transactions and retain command-level atomicity in autocommit. Multiple insert
paths/clauses, undirected edge insertion, whole-map replacement, and schema
evolution for previously unmapped properties remain pending.

The project also provides a deliberately separate Cypher-compatible vertex
upsert extension:

```sql
MERGE (person:Person {person_id: 'p3', name: 'Linus'});
```

This initial `MERGE` form accepts one vertex with an optional variable, at most
one label, and a literal property map. It matches on every supplied label and
property and inserts into the managed native vertex table when no row matches.
It is not an ISO GQL conformance claim: ISO/IEC 39075:2024 has `INSERT`, `SET`,
`REMOVE`, and `DELETE`, but no `MERGE` statement. Edge/path patterns and
`ON CREATE`/`ON MATCH` actions remain pending.

## Managed table layout

For graph ID `N`, `COPY GRAPH` currently creates:

- `gql_data.graph_N_vertices`
  - `__gql_id UBIGINT`
  - `__gql_external_id VARCHAR`
  - `__gql_label VARCHAR`
  - one typed DuckDB column per imported property
- `gql_data.graph_N_edges`
  - `__gql_edge_id UBIGINT`
  - `__gql_source_id UBIGINT`
  - `__gql_target_id UBIGINT`
  - `__gql_type VARCHAR`
  - one typed DuckDB column per imported property

The private `gql_internal` schema stores graph and mapping metadata only:

- `graphs` and `graph_storage`
- `graph_element_tables`
- `graph_edge_endpoints`
- `graph_label_mappings`
- `graph_property_mappings`

It does not store graph vertices, edges, labels, or properties as EAV rows.

## Neo4j-format bulk import

Vertex input:

```text
personId:ID(People),name:string,age:int,:LABEL
p1,Ada,42,Person
p2,Grace,37,Person
```

Edge input:

```text
:START_ID(People),:END_ID(People),:TYPE,since:int
p1,p2,KNOWS,2020
```

The format is inferred independently from `.csv`, `.csv.gz`, `.csv.zst`, or `.parquet`. The loader supports one embedded-header vertex file, one edge file, one node ID, optional ID groups, at most one scalar node label, one relationship type, and scalar properties.

Validation is enabled by default and checks duplicate/missing node IDs and missing endpoints. Trusted inputs can skip those scans:

```sql
COPY GRAPH social FROM (
    VERTICES 'nodes.csv',
    EDGES 'relationships.csv'
) FORMAT NEO4J OPTIONS (VALIDATE FALSE);
```

## CSR algorithms

`MATCH` always uses native DuckDB relational or recursive execution. CSR is a
separate, derived algorithm substrate whose construction is explicit:

```gql
CALL gql_build_csr('social');

CALL algo.bfs('social', 1, direction := 'out', max_depth := 4);
CALL algo.dfs('social', 1, edge_label := 'knows');
CALL algo.sssp('social', 1, direction := 'out');
CALL algo.pagerank(
    'social', damping := 0.85, max_iterations := 100,
    tolerance := 1e-8, vertex_label := 'person', edge_label := 'knows'
);
CALL algo.wcc('social', edge_label := 'knows');
CALL algo.scc('social');
CALL algo.degree('social', vertex_label := 'person');
CALL algo.closeness('social', direction := 'out');
CALL algo.triangle_count('social')
YIELD vertex_id, triangle_count, local_clustering_coefficient
RETURN vertex_id, triangle_count, local_clustering_coefficient
ORDER BY triangle_count DESC
LIMIT 20;

MATCH (seed:Person)
FILTER seed.region = 'west'
CALL algo.bfs('social', element_id(seed), 'person')
YIELD vertex_id, depth, parent_vertex_id
RETURN vertex_id, depth, parent_vertex_id;
```

`algo.bfs` and `algo.dfs` return `vertex_id`, `depth`, `parent_vertex_id`,
`edge_id`, and `visit_order`. `algo.sssp` is the exactly-one-source,
unweighted shortest-path contract and returns `distance` plus its predecessor
tree and settled order. These traversals accept `target_vertex_id` for early
termination and `direction := 'in'|'out'|'both'`.

`algo.degree` returns exact outgoing, incoming, and total CSR edge-incidence
counts. Parallel edges count independently; a self-loop contributes once to
both incoming and outgoing degree. `algo.closeness` computes exact generalized
normalized closeness and returns `reachable_count` and `distance_sum` beside
the score. It defaults to outbound directed distances and accepts
`direction := 'in'|'out'|'both'`. Exact closeness costs `O(V(V+E))` on an
unweighted graph and is intended for graphs where that work is acceptable.

`algo.pagerank` returns each vertex's score plus the iteration count and
convergence flag. `algo.wcc` and `algo.scc` return deterministic component IDs
and sizes. `algo.triangle_count` returns per-vertex counts, simple-projection
degree, local clustering coefficient, and the graph-wide triangle count.
`algo.lcc` implements the direction-preserving LDBC Graphalytics coefficient:
neighbors are the unique union of incoming and outgoing neighbors, while arcs
between neighbors retain their direction. All ten algorithms accept
independently optional `vertex_label` and `edge_label`
filters. With neither filter, the full CSR is used. `vertex_label` forms an
induced vertex projection: only matching vertices are emitted and edges to
nonmatching vertices are excluded. `edge_label` then filters edges within that
projection. Standalone table-function calls use named arguments; the typed GQL
procedure pipeline accepts the optional vertex label as its trailing positional
configuration argument.

Algorithm result composition is a GQL `CALL ... YIELD ... RETURN` pipeline.
`CALL` is a generic logical operator with a declared input mode. `bfs` and
`dfs` use `BATCH`, so the matched element IDs become one deduplicated,
deterministically ordered frontier. SSSP also uses `BATCH` but enforces exactly
one distinct source. PageRank, WCC, SCC, degree, closeness, LCC, and triangle
counting use `NONE`: their upstream relation remains a sequencing child in the
same plan, but the algorithm executes once after that child is exhausted. The
compiler lowers both modes to one DuckDB table-in/out node, keeping `MATCH`,
algorithm execution, `YIELD`, and `RETURN` in one statement transaction.

The procedure boundary replaces the upstream row set. Only names listed by
`YIELD` remain visible after a blocking `CALL`; this avoids inventing a false
row-by-row correlation between a whole-frontier result and its input rows.
Customer queries never need the internal table function or SQL `SELECT FROM`
syntax.

The CSR cache is connection-local and version checked. Construction streams
DuckDB chunks into exactly allocated outgoing/incoming arrays through a linear
count/prefix/scatter build, with a dense managed-ID fast path. Rebuild it after
a graph mutation or direct SQL topology change. Explicit caller transactions
are currently ineligible because transaction-local CSR deltas are not
implemented. `MATCH /*+ CSR */` is rejected: CSR is not a query-plan backend.
Weighted SSSP is not yet exposed: it requires an explicitly configured edge
weight array in the immutable CSR snapshot rather than per-call property
lookups against canonical tables.

Inspection functions:

```sql
SELECT * FROM gql_graphs();
CALL gql_build_csr('social');
CALL gql_neighbors('social', 1, 'out');
SELECT * FROM gql_csr_stats('social');
```

## Build and test

The repository pins DuckDB v1.5.4, OpenGQL grammar v1.9.0, and ANTLR 4.13.2.

```sh
make debug
./build/debug/test/unittest "test/sql/gql*"
python3 scripts/check_gql_conformance.py
```

The conformance check also validates the source-pinned 93-file, 827-scenario
[GQL clause feature corpus](test/features/README.md). Those scenarios are a
porting inventory until individually promoted from `ported_unverified` to
`executable`; they do not inflate the ISO implementation status.

See [the implementation plan](docs/gql-implementation-plan.md), [the native graph architecture](docs/table-backed-graph-architecture.md), and the benchmark reports in [docs/benchmarks](docs/benchmarks).
