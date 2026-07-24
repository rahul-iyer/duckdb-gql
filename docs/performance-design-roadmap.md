# DuckGQL performance design roadmap

## Status and purpose

This document ranks the important design changes needed to make DuckGQL
perform well on large analytical graphs. `P0`, `P1`, and `P2` are implementation
order, not optional scope cuts.

The ordering is based on the current implementation and repository benchmarks.
Expected improvements are hypotheses until the acceptance benchmark for each
item passes. Correctness and ISO GQL semantics remain mandatory.

## What the existing evidence says

DuckGQL does not currently have a general relational-query performance problem:

| Workload | Current evidence | Design implication |
|---|---|---|
| LiveJournal trusted bulk load | 19.876 s versus 19.710 s for equivalent native SQL | Keep the CTAS-based loader; do not replace it with a row-at-a-time importer |
| LiveJournal strict bulk load | 23.615 s; validation adds 3.739 s over trusted loading | Optimize validation separately from the fast load path |
| LiveJournal one-hop `MATCH` | 8.710 ms versus 7.830 ms native SQL | Reduce fixed planning overhead, but do not build a separate MATCH engine |
| LiveJournal exact two-hop `MATCH` | 64.841 ms versus 63.511 ms native SQL | Preserve native DuckDB join execution |
| LiveJournal long variable path | 1.266 s versus 1.235 s for matched SQL, with about 9.1 GiB peak RSS | Optimize recursive state types and memory before replacing recursive CTEs |
| Graphalytics datagen CSR build | 16.5M vertices and 82.1M stored arcs in 5.685 s | The two-pass streaming builder is viable; memory layout and reuse are now more important |
| Graphalytics datagen algorithms | BFS 4.830 s, PageRank 5.667 s, WCC 4.075 s, LCC 29.476 s | Parallel kernels and topology-projection reuse are the largest algorithm opportunities |

These numbers come from different benchmark workloads and should not be
compared as if they were one controlled experiment. Their shared conclusion is
still useful: preserve DuckDB-native relational execution and focus engineering
effort on recursive state, CSR representation, algorithm parallelism, and
derived-state lifecycle.

## Performance principles

1. Measure against an equivalent native DuckDB plan before adding a custom
   operator.
2. Keep typed wide tables as authoritative storage.
3. Push selective predicates into the earliest scan or recursive anchor.
4. Avoid materializing the same topology or label projection more than once.
5. Prefer compact ordinal arrays in hot loops.
6. Keep output streaming even when an algorithm requires blocking computation.
7. Make caches versioned, bounded, observable, and correctness-preserving.
8. Treat peak memory, build time, warm execution time, and result correctness as
   separate metrics.

## P0: highest-impact work

### P0.1 Establish performance regression gates

Before changing execution, turn the existing benchmark scripts into a stable
measurement contract.

Required suites:

- small deterministic SQLLogic correctness tests;
- LiveJournal bulk-load comparison;
- LiveJournal one-hop, two-hop, and long variable-path comparison;
- Graphalytics test-dataset correctness;
- wiki-Talk and datagen scale runs for CSR build and algorithms;
- a new mutation throughput suite;
- a new heterogeneous-label/property suite.

Every result should record:

- source commit and whether the extension is statically linked or loadable;
- DuckDB version, compiler, build type, operating system, architecture, thread
  count, and memory limit;
- warmups, measured runs, median, minimum, maximum, and peak RSS;
- input and output cardinality;
- physical plan or operator profile where available.

Acceptance gate:

- CI runs the small suites on every change;
- scheduled or manual scale runs produce comparable JSON artifacts;
- performance changes are not merged based on a single wall-clock measurement.

### P0.2 Keep recursive trail identities as `UBIGINT[]`

Current unbounded-path lowering casts every managed edge ID to `VARCHAR` and
stores the trail as `VARCHAR[]` so the recursive path implementation can remain
generic across possible key types. Managed DuckGQL edges already use
`UBIGINT`.

Change:

- specialize the managed-table recursive path to `UBIGINT[]`;
- use native `UBIGINT` in `list_contains` and `list_append`;
- retain a generic fallback only if noncanonical table attachment returns.

Why it matters:

- removes per-edge numeric-to-string conversion;
- reduces trail-state width and allocator pressure;
- improves comparison and membership operations;
- reduces the peak memory of deep or high-cardinality path enumeration.

The existing type-isolation experiment reduced matched handwritten recursive
SQL from 1.160 s with `VARCHAR[]` to 0.857 s with `UBIGINT[]`. That experiment
is evidence for prioritization, not a promised DuckGQL speedup.

Acceptance gate:

- identical trail semantics on cycles, parallel edges, and reverse traversal;
- lower median latency and peak RSS on the long LiveJournal VLP benchmark;
- no regression on fixed-pattern `MATCH`.

### P0.3 Compact the CSR representation

The current snapshot stores both outgoing and incoming adjacency with
`idx_t` ordinals, managed edge IDs, and label IDs. On a 64-bit build,
`idx_t` neighbors use eight bytes even when the graph has fewer than
`2^32` vertices. Dense managed graphs also retain identity structures that can
often be derived.

Change:

- use 32-bit neighbor ordinals when vertex count permits;
- retain 64-bit offsets for large edge counts;
- omit `ordinal_by_id` for dense managed IDs, as today;
- make `vertex_ids` implicit for the exact dense `1..N` case where the public
  output contract permits;
- separate topology-only arrays from optional edge-identity arrays;
- allocate incoming adjacency only for algorithms that require it, or cache it
  as a separately accounted projection;
- include hash-map, dictionary, and auxiliary-buffer overhead in memory
  reporting.

Why it matters:

- lowers peak RSS and improves cache locality;
- allows larger graphs under the same DuckDB memory limit;
- reduces memory bandwidth per algorithm iteration.

Acceptance gate:

- at least 25% lower reported CSR bytes on datagen-7_8-zf;
- exact algorithm-output parity;
- no material CSR-build regression;
- memory accounting within an agreed tolerance of process-level measurements.

### P0.4 Parallelize the core CSR kernels

Most current kernels perform their main computation while initializing one
global table-function state. Increasing `PRAGMA threads` does not make those
loops meaningfully parallel.

Implement in this order:

1. PageRank as a pull computation over incoming CSR, partitioned by target
   ordinal, with parallel reductions for dangling mass and convergence.
2. BFS and unweighted SSSP with partitioned frontiers and thread-local next
   frontiers.
3. WCC with a deterministic parallel connectivity strategy.
4. Triangle count and LCC over a reusable forward-oriented simple topology.
5. SCC after the simpler kernels establish the scheduler and scratch-buffer
   conventions.

Design requirements:

- use DuckDB's execution/task infrastructure rather than unmanaged background
  threads;
- use thread-local scratch buffers and deterministic reductions;
- check interruption between bounded units of work;
- avoid atomics in the innermost loops when partitioning can provide exclusive
  ownership;
- continue emitting results as DuckDB chunks after computation.

Acceptance gate:

- exact existing correctness and Graphalytics validation;
- speedup curves at 1, 2, 4, and 8 threads;
- no worse than 10% regression at one thread;
- bounded additional memory per worker;
- no nondeterministic component IDs or traversal outputs.

### P0.5 Reuse normalized simple-topology projections

Triangle count and LCC currently scan CSR, materialize edge pairs, sort, remove
duplicates, and construct new adjacency structures independently. LCC is the
slowest measured datagen kernel at 29.476 s.

Change:

- define a derived `SimpleUndirectedProjection` containing deduplicated,
  loop-free adjacency and degree order;
- build it once per graph version and label projection;
- share it between triangle count and LCC;
- account for it independently in the cache memory budget;
- discard it when no longer referenced or when its graph version becomes
  stale.

Acceptance gate:

- second-kernel warm execution does not repeat global edge sorting;
- materially lower combined `triangle_count` plus `lcc` latency;
- lower peak transient allocation than two independent materializations;
- identical directed-LCC and triangle semantics.

## P1: system-level scaling

### P1.1 Publish a database-wide, bounded CSR cache

The current cache is connection-local. A service with many DuckDB connections
must rebuild the same graph repeatedly.

Change:

- key immutable snapshots by database identity, graph ID, graph version, and
  projection options;
- publish a completed snapshot atomically after construction;
- coalesce concurrent builds of the same key;
- use reference counting plus an LRU or clock eviction policy;
- enforce configurable soft and hard memory budgets;
- expose cache bytes, build count, hit count, miss count, eviction count, and
  build duration;
- keep per-query scratch space outside the shared immutable snapshot.

Correctness prerequisite:

- all supported writes must advance `graph_version`;
- direct SQL writes must either be declared unsupported for cache correctness,
  routed through a versioned API, or detected by a reliable invalidation hook.

Acceptance gate:

- concurrent connections reuse one snapshot;
- stale graph versions are never returned;
- cache pressure evicts only unreferenced entries;
- failure or interruption during a build does not publish a partial snapshot.

### P1.2 Store labels in a native representation

Vertex labels are currently stored as a lowercase, semicolon-separated
`VARCHAR`. Every relational label predicate invokes `string_split` and
`list_contains`.

Change:

- move the physical label column to a normalized `VARCHAR[]` as the minimum
  improvement;
- evaluate dictionary-coded integer label IDs if profiling shows string
  comparison remains important;
- activate the existing `LIST_COLUMN` catalog mapping;
- migrate old graph storage through an explicit schema-version step;
- keep edge type as a scalar where the model permits exactly one type.

Acceptance gate:

- exact multi-label mutation and query semantics;
- no per-row string splitting in physical plans;
- lower label-filter CPU time on a mixed-label benchmark;
- acceptable storage size and mutation cost.

### P1.3 Add selective indexes only where benchmarks justify them

Candidate indexes include:

- a unique index on the named imported vertex `:ID` property, when present;
- a unique index on managed vertex and edge IDs if constraint enforcement and
  lookup performance justify the write cost;
- endpoint indexes on `__gql_source_id` and `__gql_target_id`;
- property indexes requested by an explicit graph schema.

Indexes must not be enabled blindly. DuckDB's scan and hash-join execution is
already fast for analytical workloads, while index construction increases load
time, database size, and mutation cost.

Acceptance gate:

- selective-anchor and point-mutation benchmarks improve materially;
- broad analytical scans do not regress materially;
- index build time, disk size, and write amplification are reported;
- the optimizer demonstrably uses the index in the target workload.

### P1.4 Make mutations set-oriented

Standalone and matched path inserts currently lower to multiple generated DML
statements inside one command envelope. This preserves correctness but is not
designed as a high-throughput ingestion API.

Change:

- lower all vertices for one statement into one typed batch;
- lower all edges into a second typed batch;
- allocate ID ranges rather than calling `nextval` once per element where
  transaction semantics permit;
- perform one set-oriented insert per element table;
- update `graph_version` once per successful statement;
- preserve the pre-mutation snapshot semantics of matched writes.

Acceptance gate:

- add 1, 100, 10K, and 1M element mutation benchmarks;
- near-linear scaling in batch size until storage bandwidth dominates;
- command-level rollback remains atomic;
- no identity collision under concurrent writers.

### P1.5 Remove avoidable CSR build work

The builder currently counts tables, orders vertices by ID, scans edges once
for degrees, and scans them again to scatter adjacency.

Potential improvements:

- place dense vertex labels directly by `__gql_id - 1` without a global sort;
- pass known row counts from catalog-maintained statistics when trustworthy;
- parallelize degree counting with thread-local degree arrays and a reduction;
- parallelize scatter with partitioned prefix ranges;
- avoid rebuilding identical label dictionaries for related projections.

Keep the two-pass final-allocation design. It avoids the much larger memory
cost of materializing and sorting all edges.

Acceptance gate:

- lower CSR-build CPU time on wiki-Talk and datagen;
- memory remains bounded by final arrays plus documented worker scratch;
- no dependence on physical table scan order.

## P2: broader architectural scaling

### P2.1 Support multiple typed element tables per graph

A single mixed vertex table becomes sparse when labels have very different
properties. It also prevents table-level pruning when a query requests one
label or edge type.

Change:

- allow multiple vertex and edge table mappings;
- bind label/type predicates to the smallest compatible table set;
- use static label mappings for tables that represent exactly one label;
- union compatible tables only when the query requires them;
- retain stable graph-wide element identities.

This is a substantial compiler, catalog, mutation, and schema-evolution change.
It should follow the simpler label and indexing improvements.

Acceptance gate:

- a heterogeneous benchmark with disjoint property sets;
- less scanned data and fewer null property values;
- correct cross-table edge endpoints and path matching;
- no regression for homogeneous one-table graphs.

### P2.2 Add incremental CSR maintenance

Full rebuilds are appropriate for analytical snapshots but expensive for
frequently updated graphs.

Change:

- record versioned topology deltas from supported mutations;
- answer small updates from a base snapshot plus bounded delta structures;
- compact deltas into a new immutable snapshot asynchronously or explicitly;
- preserve transaction visibility and never expose uncommitted topology.

Do not mutate shared CSR arrays in place. Immutable publication keeps readers
simple and safe.

Acceptance gate:

- update-to-query latency and rebuild amortization benchmarks;
- snapshot isolation under concurrent readers and writers;
- bounded delta lookup overhead;
- deterministic compaction and recovery after interruption.

### P2.3 Consider a specialized path operator only after profiling

Native recursive CTE execution is currently within 2.5% of matched handwritten
SQL when both use the same trail representation. A custom path operator is
therefore not justified merely because graph systems often have one.

Reconsider it only if typed recursive state, predicate pushdown, and DuckDB
optimizer improvements still leave a measured gap for:

- bidirectional shortest path;
- path modes requiring compact visited sets;
- quantified path materialization;
- cost-based frontier selection;
- high-depth traversal where row-based recursive state dominates.

Acceptance gate:

- a documented workload where the native recursive plan is the measured
  bottleneck;
- a prototype that improves latency or memory by at least an agreed threshold;
- full parity on trail, acyclic, simple, and shortest-path semantics.

### P2.4 Cache compiled and bound query programs

The one-hop benchmark exposes about 0.88 ms of GQL overhead relative to native
SQL. For interactive microqueries, parsing, binding, serialization, and
lowering can become visible.

Change:

- cache immutable parsed or bound programs by query text, parameter types,
  selected graph ID, graph schema version, and extension version;
- invalidate on schema or mapping changes;
- preserve source-located diagnostics for uncached failures;
- prefer DuckDB prepared-statement integration over an unrelated cache layer.

Acceptance gate:

- repeated parameterized microquery benchmark;
- lower planning latency without changing execution plans;
- bounded cache size and correct invalidation.

### P2.5 Add persisted or memory-mapped derived topology only if needed

Very large graphs may not fit comfortably in a process-local heap snapshot.
A persisted topology artifact could reduce repeated build cost and enable
memory mapping.

This should remain P2 because it introduces file lifecycle, portability,
version compatibility, and crash-consistency requirements.

Acceptance gate:

- a graph that cannot meet the target memory or startup budget with compact
  in-memory CSR;
- documented artifact format and versioning;
- checksum and graph-version validation;
- cold-start improvement large enough to justify the added storage system.

## Changes not recommended now

### Do not replace typed columns with EAV

EAV would increase row counts, lose native property types, and require repeated
property joins. It conflicts with the measured near-native relational path.

### Do not route every `MATCH` through CSR

CSR is excellent for dense topology algorithms. Relational `MATCH` also needs
typed property scans, joins, aggregation, optional semantics, and transaction
visibility. The current native plan already performs close to handwritten SQL.

### Do not add unconditional indexes to every structural column

Measure point-selectivity benefits against load time, database size, and
mutation cost.

### Do not build a second storage engine

DuckDB already supplies persistence, MVCC, vectorized execution, spill,
compression, and file-format integration. DuckGQL should specialize graph
semantics and derived topology, not duplicate those systems.

## Recommended implementation sequence

1. Add stable regression gates and the missing mutation/heterogeneous-label
   benchmarks.
2. Change managed recursive trail state from `VARCHAR[]` to `UBIGINT[]`.
3. Compact CSR ordinals and make optional arrays explicit.
4. Parallelize PageRank, then BFS/SSSP, WCC, and topology-intersection kernels.
5. Add the shared simple-undirected projection for triangle count and LCC.
6. Publish a bounded shared CSR cache with correct version invalidation.
7. Move labels to a native list representation.
8. Benchmark and selectively add indexes.
9. Batch mutation lowering and ID allocation.
10. Pursue multiple element tables, incremental CSR, or a custom path operator
    only when the preceding benchmark evidence justifies them.

## Source and benchmark map

| Area | Source or benchmark |
|---|---|
| Bulk import | [`src/gql_import.cpp`](../src/gql_import.cpp), [`docs/benchmarks/livejournal-copy-graph.md`](benchmarks/livejournal-copy-graph.md) |
| Fixed and recursive MATCH | [`src/gql_relational.cpp`](../src/gql_relational.cpp), [`docs/benchmarks/livejournal-query.md`](benchmarks/livejournal-query.md) |
| CSR construction | [`src/gql_csr.cpp`](../src/gql_csr.cpp) |
| Algorithms | [`src/gql_algorithms.cpp`](../src/gql_algorithms.cpp) |
| Native mutations | [`src/gql_mutation.cpp`](../src/gql_mutation.cpp) |
| Graphalytics validation | [`scripts/benchmark_graphalytics.py`](../scripts/benchmark_graphalytics.py), [`scripts/benchmark_graphalytics_scale.py`](../scripts/benchmark_graphalytics_scale.py) |
| Scale results | [`docs/benchmarks/graphalytics-wiki-talk.md`](benchmarks/graphalytics-wiki-talk.md), [`docs/benchmarks/graphalytics-datagen-7-8-zf.md`](benchmarks/graphalytics-datagen-7-8-zf.md) |
