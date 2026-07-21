# DuckDB ISO GQL extension

`gql` is a C++17 DuckDB extension targeting ISO GQL. The current implementation uses one canonical storage model: graph-owned native DuckDB vertex and edge tables. There is no normalized EAV compatibility backend and no manual table-registration API.

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
- Optional prepared CSR execution for eligible unbounded paths through `MATCH /*+ CSR */`.

Native DuckDB relations are authoritative. Fixed MATCH and the default variable-length path implementation lower to ordinary DuckDB scans, joins, filters, projections, aggregation, ordering, and recursive CTE operators.

Matched property `SET`, `REMOVE`, edge/node `DELETE`, and `DETACH DELETE` lower directly to managed native DuckDB tables. They use one pre-mutation MATCH snapshot, participate in explicit caller transactions, and retain command-level atomicity in autocommit. Standalone `INSERT`, whole-map replacement, and schema evolution for previously unmapped properties remain pending.

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

## CSR

Native DuckDB execution is the correctness default. CSR construction is explicit and offline:

```sql
SELECT * FROM gql_build_csr('social');

MATCH /*+ CSR */ (a:Person)-[:KNOWS]->+(b:Person)
WHERE a.name = 'Ada'
RETURN b.name;
```

The CSR cache is connection-local. Rebuild it after direct SQL topology changes. Explicit caller transactions are currently ineligible because transaction-local CSR deltas are not implemented.

Inspection functions:

```sql
SELECT * FROM gql_graphs();
SELECT * FROM gql_build_csr('social');
SELECT * FROM gql_neighbors('social', 1, 'out');
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
