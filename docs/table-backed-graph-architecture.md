# Native Table-Backed Graph Architecture

## Status

- **State:** proposed target architecture
- **Date:** 2026-07-19
- **Scope:** persistent property-graph storage, bulk loading, GQL binding, native DuckDB execution, mutations, and optional CSR acceleration
- **Primary decision:** ordinary graphs are metadata views over native DuckDB vertex and edge tables. They are not copied into a shared entity-attribute-value store.

This document defines the target architecture for the native DuckDB backend. The normalized `gql_internal.objects`, `object_labels`, and `object_properties` representation remains the current implementation and a compatibility backend during migration. It is not the intended primary representation for bulk analytical graphs.

## Executive summary

Users should be able to load data with normal DuckDB operations and register the resulting relations as a property graph:

```sql
CREATE TABLE airports AS
SELECT * FROM read_parquet('airports.parquet');

CREATE TABLE routes AS
SELECT * FROM read_parquet('routes.parquet');
```

Graph creation then records how those tables represent vertices, edges, labels, endpoints, and properties. It does not copy their rows:

```text
air_routes graph
  vertex source -> main.airports
  edge source   -> main.routes
  vertex key    -> airports.id
  edge key      -> routes.edge_id
  source        -> routes.source_id = airports.id
  destination   -> routes.target_id = airports.id
```

The GQL compiler binds patterns against that metadata and lowers them into ordinary DuckDB table scans, joins, filters, projections, aggregations, recursive CTEs, and DML. A CSR snapshot is a derived physical alternative for traversal-heavy OLAP queries; it is never the authoritative graph state.

The architectural result is:

- CSV and Parquet loading retain DuckDB's vectorized and parallel ingestion path.
- Wide tabular properties remain typed columns instead of expanding into one indexed row per property.
- SQL and GQL observe the same MVCC-managed data.
- The native DuckDB plan is the semantic reference implementation.
- CSR construction is paid only when a query or workload benefits from it.

## Goals

1. Make graph loading approximately as fast as loading the same vertex and edge data into normal DuckDB tables.
2. Avoid duplicating wide vertex and edge datasets in normalized EAV tables.
3. Lower supported GQL queries to native DuckDB operators over the registered relations.
4. Preserve DuckDB transactions, read-your-writes, rollback, persistence, and concurrency behavior.
5. Support multiple vertex tables and multiple edge tables in one graph.
6. Preserve stable vertex and edge identity independently of physical row position.
7. Support both fixed-schema properties and open/dynamic properties.
8. Keep CSR optional, versioned, rebuildable, and storage-independent.
9. Preserve a migration path for existing normalized graphs and current tests.
10. Keep the logical GQL IR independent of either table-backed, normalized, or CSR physical storage.

## Non-goals

- Replacing DuckDB's storage engine, optimizer, MVCC, or file format.
- Making CSR authoritative or directly user-mutable.
- Depending on DuckDB `rowid` for persistent GQL element identity.
- Requiring every graph to use one universal vertex table or one universal edge table.
- Silently accepting lossy endpoint casts or non-unique element keys.
- Reporting grammar recognition as ISO GQL semantic conformance.

## Current architecture

The current implementation creates one database-wide normalized schema:

```text
gql_internal.graphs
    +-- gql_internal.objects
          +-- gql_internal.object_labels --> gql_internal.labels
          +-- gql_internal.object_properties --> gql_internal.property_keys
```

`objects` contains both vertices and edges. `kind=0` identifies vertices; `kind=1` identifies edges, with `source_id` and `target_id` referencing vertex object IDs. Labels and property names are dictionary encoded. Every property value is a row in `object_properties` containing a tagged `gql_internal.property_value` union.

This representation is useful as a uniform compatibility store, but it amplifies bulk loads:

```text
one wide input row
    -> one objects row
    -> zero or more object_labels rows
    -> one object_properties row for every non-null property
```

It also makes a property reference such as `airport.country` require dictionary and EAV joins. The current native lowerer therefore uses DuckDB operators, but those operators scan and join the normalized representation rather than the original wide tables.

The current implementation is defined primarily in:

- `src/gql_storage.cpp`: normalized schema, lifecycle, and standalone mutations.
- `src/gql_import.cpp`: CSV/Parquet staging followed by normalized writes.
- `src/gql_relational.cpp`: MATCH lowering over normalized tables.
- `src/gql_mutation.cpp`: matched DML over normalized tables.
- `src/gql_csr.cpp`: derived adjacency snapshots.

## Target architecture

### System overview

```text
GQL source
   |
   v
ANTLR parser -> typed AST -> binder and type system
                               |
                               v
                      graph-relational logical IR
                               |
               +---------------+----------------+
               |                                |
               v                                v
      native relational lowerer          CSR/path lowerer
               |                                |
               v                                v
      DuckDB scans/joins/DML          derived snapshot operators
               |                                |
               +---------------+----------------+
                               |
                               v
                  native DuckDB vertex/edge tables
```

The binder resolves a selected graph into a `BoundGraph` containing table sources, keys, labels, endpoints, property columns, mutability, and logical types. Logical graph operators refer to bound graph elements rather than physical `gql_internal` table names. Physical lowering chooses a storage provider only after semantic binding is complete.

### Storage providers

The migration uses two providers behind one logical contract:

| Provider | Authority | Primary purpose |
|---|---|---|
| `TABLE_BACKED` | Registered DuckDB vertex/edge relations | Default native and OLAP graphs |
| `NORMALIZED` | Existing `gql_internal` EAV tables | Compatibility, migration, and differential reference |

CSR is not a third authority provider. It is a derived physical index built from a transactionally visible provider snapshot.

The provider contract must support:

- enumerating vertex and edge sources;
- resolving stable element identity;
- resolving labels and properties;
- describing endpoint joins;
- producing native scan and mutation targets;
- exposing snapshot/version information for derived indexes.

## Graph catalog

Graph metadata belongs in `gql_internal`, while graph rows remain in their registered DuckDB tables. The catalog stores relation names and column mappings, not raw pointers to DuckDB catalog objects.

### `graphs`

The existing table remains the root:

```sql
gql_internal.graphs(
    graph_id       UBIGINT PRIMARY KEY,
    graph_name     VARCHAR NOT NULL UNIQUE,
    graph_version  UBIGINT NOT NULL,
    created_at     TIMESTAMP NOT NULL
)
```

`graph_version` records graph-aware catalog changes and GQL mutations. Native relational queries do not depend on it for visibility. CSR reuse does.

### `graph_storage`

```sql
gql_internal.graph_storage(
    graph_id             UBIGINT PRIMARY KEY,
    storage_mode         VARCHAR NOT NULL,  -- TABLE_BACKED or NORMALIZED
    default_catalog      VARCHAR,
    default_schema       VARCHAR,
    schema_version       UBIGINT NOT NULL,
    csr_policy           VARCHAR NOT NULL   -- DISABLED, MANUAL, AUTO
)
```

Keeping storage mode in a separate table allows existing `graphs` databases to migrate without redefining the current root table immediately.

### `graph_element_tables`

Each registered vertex or edge relation has one row:

```sql
gql_internal.graph_element_tables(
    element_table_id          UBIGINT PRIMARY KEY,
    graph_id                  UBIGINT NOT NULL,
    element_kind              VARCHAR NOT NULL, -- VERTEX or EDGE
    catalog_name              VARCHAR NOT NULL,
    schema_name               VARCHAR NOT NULL,
    table_name                VARCHAR NOT NULL,
    key_columns               VARCHAR[] NOT NULL,
    ownership                 VARCHAR NOT NULL, -- REFERENCED or MANAGED
    access_mode               VARCHAR NOT NULL, -- READ_ONLY or READ_WRITE
    extra_properties_column   VARCHAR,
    UNIQUE(graph_id, catalog_name, schema_name, table_name)
)
```

The catalog persists names because DuckDB catalog pointers are process-local. At bind time the extension resolves the current `TableCatalogEntry`, validates its schema, and registers normal DuckDB dependencies so cached plans invalidate after relevant DDL.

Initial implementation restrictions:

- Base tables are supported first; views are read-only sources later.
- One scalar key column is sufficient for the first vertical slice.
- Composite keys are represented by `key_columns` and added without changing the catalog layout.
- Keys must be non-null and unique. The extension validates an existing primary/unique constraint or performs explicit validation during registration.
- Persistent identity must never use `rowid`.

### `graph_edge_endpoints`

```sql
gql_internal.graph_edge_endpoints(
    edge_table_id             UBIGINT PRIMARY KEY,
    source_vertex_table_id    UBIGINT NOT NULL,
    target_vertex_table_id    UBIGINT NOT NULL,
    source_columns            VARCHAR[] NOT NULL,
    target_columns            VARCHAR[] NOT NULL,
    source_key_columns        VARCHAR[] NOT NULL,
    target_key_columns        VARCHAR[] NOT NULL
)
```

For air-routes, `routes.source_id` joins `airports.id` and `routes.target_id` joins `airports.id`. Endpoint types must be identical or connected by a binder-approved lossless coercion.

An edge table initially has one fixed source vertex table and one fixed target vertex table. Polymorphic endpoints require either separate edge registrations or a future discriminator mapping.

### `graph_label_mappings`

```sql
gql_internal.graph_label_mappings(
    label_mapping_id  UBIGINT PRIMARY KEY,
    element_table_id  UBIGINT NOT NULL,
    label_name        VARCHAR,
    mapping_kind      VARCHAR NOT NULL, -- STATIC, SCALAR_COLUMN, LIST_COLUMN
    column_name       VARCHAR,
    CHECK (
        (mapping_kind = 'STATIC' AND label_name IS NOT NULL AND column_name IS NULL) OR
        (mapping_kind IN ('SCALAR_COLUMN', 'LIST_COLUMN') AND
         label_name IS NULL AND column_name IS NOT NULL)
    )
)
```

Supported representations are:

- `STATIC`: every row in the source has a catalog-defined label or edge type.
- `SCALAR_COLUMN`: one label/type is read from a `VARCHAR` column.
- `LIST_COLUMN`: zero or more labels are read from a `VARCHAR[]` column.

Static labels enable source pruning before a table scan. Dynamic label columns lower to ordinary column predicates.
Registration rejects duplicate mappings for the same element source.

### `graph_property_mappings`

```sql
gql_internal.graph_property_mappings(
    element_table_id  UBIGINT NOT NULL,
    property_name     VARCHAR NOT NULL,
    column_name       VARCHAR NOT NULL,
    gql_type          VARCHAR NOT NULL,
    nullable          BOOLEAN NOT NULL,
    writable          BOOLEAN NOT NULL,
    PRIMARY KEY(element_table_id, property_name)
)
```

Known properties resolve directly to native columns. Their DuckDB logical types are bound to GQL types without wrapping every value in a generic union.

An optional `extra_properties_column` supports open graphs. Its physical type is:

```sql
MAP(VARCHAR, gql_internal.property_value)
```

Known, frequently queried properties remain native columns. Properties not represented by mapped columns live in the extra map. A property name cannot simultaneously exist in both locations for one element table; mapped columns take catalog precedence and mutation code prevents duplicates.

## Physical vertex and edge contract

### Example air-routes tables

```sql
CREATE TABLE air_routes_vertices(
    id BIGINT PRIMARY KEY,
    labels VARCHAR[],
    type VARCHAR,
    code VARCHAR,
    icao VARCHAR,
    description VARCHAR,
    region VARCHAR,
    runways BIGINT,
    longest BIGINT,
    elevation BIGINT,
    country VARCHAR,
    city VARCHAR,
    latitude DOUBLE,
    longitude DOUBLE,
    author VARCHAR,
    generated_at VARCHAR,
    __gql_extra_properties MAP(VARCHAR, gql_internal.property_value)
);

CREATE TABLE air_routes_edges(
    edge_id BIGINT PRIMARY KEY,
    source_id BIGINT NOT NULL,
    target_id BIGINT NOT NULL,
    edge_type VARCHAR NOT NULL,
    distance BIGINT,
    __gql_extra_properties MAP(VARCHAR, gql_internal.property_value)
);
```

The wide tables remain directly queryable from SQL. GQL metadata identifies structural columns and maps all other declared property columns.

### Stable element identity

A table-backed element identity is logically:

```text
(element_table_id, key_value_1, ..., key_value_n)
```

This identity is used for:

- repeated GQL variables;
- node/edge equality and difference predicates;
- different-edge and trail semantics;
- mutation targeting;
- result element values;
- CSR snapshot identity maps.

The executor may use an optimized fixed-width representation for a single integer key, but the logical IR must not assume all keys are `UBIGINT`.

Edges require their own stable key. Endpoint pairs are not sufficient because parallel edges are valid. Importers must preserve an external edge ID or generate and persist a dedicated key column once during table creation.

### Null and missing properties

- A null mapped column represents an absent/null property according to the bound GQL operation.
- An absent key in `extra_properties_column` represents a missing dynamic property.
- `REMOVE n.p` sets a mapped nullable column to null or deletes the key from the extra map.
- Mapped non-null columns cannot be removed; the binder reports a semantic error before execution.
- Exact ISO distinctions between null, unknown, and absent element properties remain the responsibility of the binder/type system and conformance tests.

## Graph definition interface

The exact public grammar must follow the supported ISO GQL catalog slice. The following syntax is illustrative and defines the required semantics:

```gql
CREATE GRAPH air_routes
  VERTEX TABLE main.air_routes_vertices
    KEY (id)
    LABELS FROM labels
    PROPERTIES ALL COLUMNS EXCEPT (id, labels, __gql_extra_properties)
    EXTRA PROPERTIES __gql_extra_properties
  EDGE TABLE main.air_routes_edges
    KEY (edge_id)
    SOURCE (source_id) REFERENCES main.air_routes_vertices (id)
    DESTINATION (target_id) REFERENCES main.air_routes_vertices (id)
    TYPE FROM edge_type
    PROPERTIES ALL COLUMNS EXCEPT
      (edge_id, source_id, target_id, edge_type, __gql_extra_properties)
    EXTRA PROPERTIES __gql_extra_properties;
```

Until the catalog grammar slice is implemented, a SQL table function can expose the same typed registration model. The SQL API must be a temporary adapter over the same catalog operations, not a separate storage implementation.

Registration is atomic and validates:

1. The graph name is unique.
2. Every referenced relation and column exists.
3. Element keys are non-null and unique.
4. Edge endpoint mappings have compatible arity and types.
5. Every edge endpoint resolves to an existing registered vertex key when requested by the validation mode.
6. Property names are unique per element source after GQL identifier normalization.
7. Writable mappings target writable base-table columns.
8. Open graphs have a correctly typed extra-properties column.

## Bulk loading

### User-managed tables

The fastest path is ordinary DuckDB ingestion followed by metadata registration:

```sql
CREATE TABLE air_routes_vertices AS
SELECT
    id::BIGINT AS id,
    string_split(label, ';') AS labels,
    type,
    code,
    runways::BIGINT AS runways,
    country,
    lat::DOUBLE AS latitude,
    lon::DOUBLE AS longitude
FROM read_csv('nodes.csv', header = true, all_varchar = true);

CREATE TABLE air_routes_edges AS
SELECT
    edge_id::BIGINT AS edge_id,
    source_id::BIGINT AS source_id,
    target_id::BIGINT AS target_id,
    edge_type,
    distance::BIGINT AS distance
FROM read_csv('edges.csv', header = true, all_varchar = true);
```

DuckDB owns CSV block scheduling, parallel parsing, vectorized casts, compression, and persistence. Graph registration then writes only catalog metadata.

### Managed `gql_load_graph`

`gql_load_graph` remains useful as a format adapter. In table-backed mode it must:

1. Parse Neo4j-compatible semantic headers.
2. Derive clean physical column names and DuckDB types.
3. Create one managed wide vertex table and one managed wide edge table.
4. Populate each table with one vectorized CTAS or `INSERT ... SELECT` pipeline.
5. Persist a generated edge key if the input does not supply one.
6. Validate vertex-key uniqueness and edge endpoints.
7. Register the new tables in the graph catalog.
8. Increment `graph_version` once and commit everything in one caller-visible transaction.

It must not create one `object_properties` row per input property. CSV and Parquet adapters share the same registration path after physical tables exist.

Managed physical names should be derived from immutable numeric IDs rather than untrusted graph names, for example:

```text
gql_managed.vertex_<graph_id>_<source_id>
gql_managed.edge_<graph_id>_<source_id>
```

`DROP GRAPH` drops `MANAGED` relations but never drops `REFERENCED` user tables.

## Compiler and binder

### Binding model

The binder resolves the selected graph into storage-independent descriptors:

```cpp
struct BoundGraph;
struct BoundVertexSource;
struct BoundEdgeSource;
struct BoundElementIdentity;
struct BoundLabelMapping;
struct BoundPropertyMapping;
struct BoundEndpointMapping;
```

These are conceptual interfaces, not required public C++ names. They carry:

- resolved DuckDB catalog entries and dependencies;
- stable catalog metadata IDs;
- key and endpoint expressions;
- native DuckDB logical types and bound GQL types;
- label/property resolution rules;
- nullability and mutability;
- source-pruning information.

The typed AST never contains table names inferred from source text. The binder performs name resolution once, emits source-located semantic errors, and produces typed logical operators.

### Logical IR

The graph-relational IR should expose at least:

```text
GraphScan
VertexScan
EdgeScan
Expand
PathExpand
Filter
Project
Aggregate
Distinct
Order
Offset
Limit
Union / Intersect / Except
InsertElements
UpdateProperties
RemoveProperties
DeleteElements
```

`VertexScan`, `EdgeScan`, and `Expand` refer to bound graph sources and identities, not `gql_internal.objects`. This lets the same logical plan lower to table-backed relations, normalized compatibility tables, or eligible CSR operators.

## Native relational lowering

### Fixed MATCH

For:

```gql
MATCH (a:airport)-[r:route]->(b:airport)
WHERE a.country = 'US' AND r.distance < 500
RETURN a.code, b.code
```

the table-backed lowerer should produce the equivalent native relation:

```sql
SELECT a.code, b.code
FROM air_routes_vertices a
JOIN air_routes_edges r
  ON r.source_id = a.id
JOIN air_routes_vertices b
  ON r.target_id = b.id
WHERE list_contains(a.labels, 'airport')
  AND list_contains(b.labels, 'airport')
  AND r.edge_type = 'route'
  AND a.country = 'US'
  AND r.distance < 500;
```

Important properties of this plan:

- Known properties are direct column references.
- Static labels prune irrelevant source tables before scanning.
- Dynamic labels are normal predicates.
- Edge direction is expressed by choosing the source or destination endpoint mapping.
- Multiple patterns reuse binding identities and become normal correlated joins.
- `OPTIONAL MATCH` becomes left-outer relational composition with correct null extension.
- Projection, aggregation, sorting, offset, limit, and scalar functions remain native DuckDB expressions/operators.

### Multiple source tables

A label may resolve to several vertex sources. The lowerer constructs compatible branches and combines them with `UNION ALL`, adding `element_table_id` to the internal identity. Source pruning occurs before union construction whenever static labels or edge types make a branch impossible.

### Variable-length paths

Native correctness lowering uses recursive CTEs over registered edge relations. The recursive state carries stable vertex identity, edge identity history where required, depth, and path output state. WALK, TRAIL, SIMPLE, and ACYCLIC semantics must be represented explicitly in logical IR rather than inferred by the physical backend.

CSR may replace eligible `PathExpand` or `Expand` operators only after the native plan implements the same semantics and differential tests prove equivalence.

### Element values

Returning a node, edge, or path should use a logical element reference containing graph ID, source ID, stable key, labels, and lazily accessible properties. The engine should not eagerly assemble a full property map when the query only needs selected columns.

## Mutations

All generated DML executes on the caller connection and within the caller transaction.

### `SET`

- A mapped property lowers to `UPDATE <table> SET <column> = <typed expression>`.
- A dynamic property lowers to a map update on `extra_properties_column`.
- The binder rejects writes to read-only relations, generated columns, or incompatible types.
- `SET n = {...}` updates all writable mapped columns, clearing omitted nullable columns, and replaces the extra-property map.

### `REMOVE`

- A mapped nullable property becomes `SET column = NULL`.
- An extra property is removed from the map.
- Removing a non-nullable mapped property is a semantic error.

### `DELETE` and `DETACH DELETE`

- Edge deletion targets the edge table using its stable key.
- Vertex deletion checks every registered edge source that can reference that vertex source.
- `DELETE` fails if attached edges remain.
- `DETACH DELETE` deletes matching edges from all applicable edge tables before deleting vertices.
- Match bindings are materialized once so multiple generated DML statements use one snapshot of target identities.

### `INSERT`

- Labels and edge type choose one writable target source.
- Ambiguous target sources are a binder error unless the statement explicitly identifies an element type/source.
- Required mapped columns must be supplied or have DuckDB defaults.
- Generated/default keys are read back as stable element identity.
- Unknown properties require an open extra-property map.
- Match-and-insert uses the same caller transaction and observes preceding writes.

### Direct SQL writes

Because native plans scan registered tables, direct SQL inserts, updates, and deletes are immediately visible to later GQL statements under normal DuckDB MVCC rules. Direct SQL is not forced through graph APIs.

Direct SQL can violate graph invariants if constraints are absent. Registration should prefer or create enforceable uniqueness constraints. Endpoint integrity validation can be strict at registration/import and diagnostic at query time until DuckDB can express all cross-table graph constraints directly.

## Transactions and concurrency

Table-backed graphs inherit DuckDB transaction behavior:

- Snapshot isolation comes from the underlying relations.
- GQL and SQL share the same transaction and read-your-writes behavior.
- Registration, managed table creation, validation, and catalog publication are atomic.
- Generated multi-statement mutations use one materialized binding table and one caller transaction.
- Rollback restores graph data and graph catalog state together.
- Concurrent readers use DuckDB snapshots; concurrent writers use DuckDB conflict detection.

An internal `Connection(*context.db)` must not establish independent transaction boundaries for caller-owned GQL mutations or managed imports. Internal connections are acceptable only for read-only background work that explicitly operates on a committed snapshot.

## CSR acceleration

### Role

CSR is a derived, versioned physical structure for expansion and path operators. It contains dense snapshot-local ordinals mapped from stable table-backed element identities.

```text
registered vertex/edge tables
          |
          v
transactionally visible edge scan
          |
          v
identity dictionary + outgoing/incoming CSR
          |
          v
eligible Expand / PathExpand operators
```

### Correctness rules

1. The native table-backed plan remains available for every supported query.
2. CSR never becomes the source of truth.
3. A CSR snapshot is used only when its table dependencies and transaction snapshot are valid.
4. Transaction-local writes require a delta overlay or force native execution.
5. Parallel edges and self-loops preserve distinct stable edge identities.
6. Path-mode and shortest-path semantics live in logical operators, not in an implicit CSR traversal convention.

### Invalidation

GQL mutations can increment `graph_version`, but direct SQL writes do not automatically pass through the GQL layer. Therefore persistent CSR reuse for table-backed graphs requires one of:

- reliable DuckDB table mutation/version dependencies;
- transaction commit hooks that publish affected graph versions;
- an explicit user-managed refresh contract.

Until reliable dependency tracking exists, table-backed CSR must be statement-local, manually refreshed, or conservatively disabled. It must never return stale results merely because direct SQL bypassed a GQL version counter.

### Planner selection and hints

The eventual cost model considers:

- edge and vertex cardinality;
- label/type selectivity;
- endpoint-key statistics;
- requested path bounds and modes;
- whether a valid CSR already exists;
- CSR build cost and memory budget;
- transaction-local mutations.

The optimizer chooses native or CSR execution by default. A query hint or session setting may force `NATIVE` or request `CSR` for diagnosis and benchmarking, but hints cannot bypass semantic eligibility or snapshot validation.

## Catalog and schema changes

At bind time the extension validates registered sources. Expected behavior is:

| Change | Behavior |
|---|---|
| Add unrelated column | Graph remains valid; `PROPERTIES ALL` refresh requires explicit catalog update |
| Rename/drop mapped column | Binding fails with a source-located invalid-graph diagnostic |
| Change mapped type | Binding revalidates coercion and may fail |
| Drop referenced table | Graph remains cataloged but invalid until repaired or dropped |
| Rename referenced table | Explicit graph catalog update is required unless DuckDB dependency APIs can propagate it safely |
| Drop graph with referenced tables | Metadata only is removed |
| Drop graph with managed tables | Metadata and owned tables are removed atomically |

Prepared plans must register DuckDB catalog dependencies so DDL invalidates stale bindings.

## Security and correctness boundaries

- Persist catalog/schema/table/column names as values; quote identifiers only through DuckDB AST/catalog APIs.
- Never concatenate unvalidated user identifiers into executable SQL.
- Bind relation and column references through DuckDB catalog resolution.
- Validate endpoint casts as lossless.
- Reject duplicate keys and unresolved endpoints during strict registration/import.
- Do not expose `gql_internal` writes as a supported public loading API.
- Do not use hidden row IDs as durable graph IDs.
- Preserve precise source ranges from GQL AST through catalog, type, and mutation diagnostics.

## Migration plan

### Phase 0: measurement contract

- Add a Release-build benchmark for plain DuckDB table load, table-backed graph registration, normalized import, fixed MATCH, and variable-length traversal.
- Measure CSV and Parquet separately.
- Record cold/warm cache, thread count, database storage version, input rows/bytes, wall time, CPU time, peak memory, and database size.
- Use the same source files and typed columns for every compared path.

### Phase 1: read-only catalog vertical slice

- Add graph storage/catalog tables.
- Register one vertex table and one directed edge table with scalar keys.
- Bind static/dynamic labels, direct properties, and endpoints.
- Lower fixed `MATCH`, `WHERE`/`FILTER`, and `RETURN` to native scans and joins.
- Keep normalized lowering selectable for differential tests.

### Phase 2: table-backed bulk loader

- Change `gql_load_graph` to create managed wide tables.
- Parse Neo4j-compatible headers into clean typed columns.
- Perform one vectorized load per vertex/edge relation.
- Register metadata without EAV expansion.
- Validate air-routes end to end.

### Phase 3: broad query semantics

- Multiple vertex/edge sources.
- Static and column-backed labels/types.
- OPTIONAL MATCH, multiple patterns, aggregation, ordering, composition, and finite/unbounded paths.
- Element identity and value construction across heterogeneous tables.

### Phase 4: mutations and open properties

- Direct mapped-column SET/REMOVE.
- Extra-property maps for open graphs.
- DELETE/DETACH across registered edge sources.
- INSERT target-source selection and match-and-insert.
- Caller-transaction and direct-SQL interoperability tests.

### Phase 5: CSR and cost-based planning

- Build CSR from bound table sources.
- Add safe dependency/version validation.
- Add transaction-local deltas or native fallback.
- Add cost-based choice and diagnostic hints.

### Phase 6: retire normalized default

- Make table-backed storage the default for new bulk graphs.
- Retain normalized graph reading for compatibility or provide an atomic migration command.
- Remove normalized assumptions from binder, logical IR, mutation targeting, and public documentation.

## Source-level implementation map

Recommended components:

| Component | Responsibility |
|---|---|
| `gql_catalog.*` | Persistent graph/table/property metadata and registration validation |
| `gql_graph_binding.*` | Resolve metadata to DuckDB catalog entries and typed bound descriptors |
| `gql_storage_provider.*` | Provider-neutral scan, identity, label, property, and mutation contract |
| `gql_table_storage.*` | Table-backed provider implementation |
| `gql_normalized_storage.*` | Adapter for the current EAV representation during migration |
| `gql_import.cpp` | Format adapter and managed wide-table loading |
| `gql_binder.cpp` | GQL scopes/types plus graph source/property resolution |
| `gql_ir.*` | Storage-independent graph-relational operators |
| `gql_relational.cpp` | Native table-backed DuckDB plan construction |
| `gql_mutation.cpp` | Provider-aware caller-transaction DML |
| `gql_csr.cpp` | Derived snapshot construction and eligible physical operators |

The first refactor should remove literal `objects`, `object_labels`, `labels`, `property_keys`, and `object_properties` assumptions from the relational lowerer. They should be isolated inside the normalized provider.

## Testing strategy

### Semantic tests

- Single and multiple vertex/edge sources.
- Static, scalar-column, and list-column labels.
- Directed endpoints, self-loops, and parallel edges.
- Repeated variables and different-edge semantics.
- Native typed property predicates and projections.
- Null, missing, nullable, and extra-map properties.
- Composite identity preparation even if MVP exposes scalar keys only.
- OPTIONAL MATCH and null extension.
- Bounded and unbounded paths.

### Mutation tests

- Known and dynamic SET.
- REMOVE on mapped and extra properties.
- SET-all replacement.
- DELETE attached-node rejection.
- DETACH DELETE across multiple edge sources.
- INSERT source selection and defaults.
- Explicit transaction read-your-writes and rollback.
- Direct SQL write followed by GQL read in the same transaction.

### Catalog tests

- Duplicate/non-null key validation.
- Missing endpoint validation.
- Incompatible endpoint types.
- Dropped/renamed table or column.
- Read-only source mutation rejection.
- Referenced versus managed DROP GRAPH behavior.
- Prepared-plan invalidation after DDL.

### Differential tests

Run identical logical graphs through:

1. table-backed native lowering;
2. normalized compatibility lowering;
3. CSR lowering when eligible.

Compare rows, values, nullability, path identity/order where specified, errors, and transaction outcomes.

## Performance acceptance criteria

All published numbers use a Release build; sanitizer Debug timings are diagnostic only.

For a graph already loaded into DuckDB tables:

- Registration should be metadata-bound and should not scale with total property count, except for optional validation scans.
- Non-validating registration should complete in approximately constant time relative to graph row count.
- Database size after registration should not contain a second copy of vertex/edge properties.

For managed CSV/Parquet loading:

- Graph load wall time target: at most `1.10x` the equivalent typed DuckDB table load for Parquet and `1.20x` for CSV, excluding explicitly requested endpoint/key validation.
- Property width should affect scan/cast cost, not multiply the number of DML statements or indexed rows.
- Peak memory must remain bounded by DuckDB's configured memory limit and support spilling where native operators support it.

For native MATCH:

- `EXPLAIN` for direct properties must show scans of registered relations and no `object_properties`/`property_keys` joins.
- One-hop fixed MATCH should be competitive with the equivalent handwritten SQL join.
- Result equivalence, not speed, is the release gate when native and CSR plans differ in performance.

The benchmark suite must report:

```text
DuckDB revision and extension revision
Release/debug configuration
sanitizer state
storage compatibility version
threads and memory limit
input format, bytes, vertices, edges, and non-null properties
cold/warm state
load, registration, validation, checkpoint, and query timings
database size and peak RSS
```

## Acceptance criteria for the first vertical slice

The architecture is proven when all of the following hold:

1. Existing DuckDB `airports` and `routes` tables can be registered without copying their rows.
2. `MATCH (a)-[r]->(b) RETURN ...` lowers to scans and joins over those exact tables.
3. Labels, endpoint direction, and typed property filters are correct.
4. SQL writes are visible to GQL under normal DuckDB transaction rules.
5. A graph can be dropped without dropping referenced tables.
6. The same logical dataset produces identical results through the normalized reference backend.
7. `EXPLAIN` contains no normalized EAV property joins for mapped columns.
8. Release-build registration is effectively metadata-only.
9. Managed air-routes loading is within the stated CSV target relative to plain DuckDB loading.
10. CSR remains disabled unless snapshot validity can be proved.

## Architectural decisions

1. **Native tables are authoritative.** Table-backed graphs reference DuckDB relations rather than copying graph objects into EAV storage.
2. **Stable keys define identity.** Physical row position and `rowid` are never persistent graph identity.
3. **Wide properties stay wide.** Known properties bind to native typed columns; open properties use one optional map column.
4. **Native execution is the correctness backend.** Every supported query must have a native DuckDB plan before CSR substitution.
5. **CSR is derived and versioned.** Unsafe reuse falls back to native execution.
6. **Transactions belong to the caller.** Registration, loading, queries, and mutations use DuckDB MVCC and caller transaction boundaries.
7. **Normalized storage is transitional.** It remains a compatibility and differential-testing provider, not the bulk/OLAP default.
8. **Performance is part of correctness for the loading surface.** The release benchmark compares against equivalent native DuckDB table loading and reports validation separately.
