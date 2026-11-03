# DuckGQL / Ladybug LDBC SF1 handoff

Updated: 2026-11-03 local time. Repository: `/Users/rahuliyer/duckdb-gql`.

## Graphs unlike LDBC (newest)

`benchmark/generalization/run.py` asks DuckGQL and Ladybug the same 8 questions from 6 seeds across the degree
range on Air Routes, RoadNet-CA and LiveJournal; `compare.py` prints times and checks answers (131/131 match).
`DUCKGQL_BUILD` points it, and `run.py`, at another build. Results are in `build/benchmarks/generalization/`.
COPY GRAPH stores an untyped `:ID` as VARCHAR, so keys must be quoted: an integer literal casts the column and
skips the index, which first looked like a per-query cost that grew with the graph.

Found and fixed: fetch was chosen from average degree, so a LiveJournal hub took 739 ms fetching 41K VARCHAR rows at
about 17 us each (0.3 us for typed LDBC columns). Now 7.1 ms. Seeds read their real degree through the property
index without endpoint statistics, using a new row ID lookup when IDs don't match row IDs, and a hop of the same
type as the edge that reached a vertex uses E[d^2]/E[d]. Applying that bias to every hop slowed IC10-12 by up to 25%.

Also fixed: ANY SHORTEST stops at a bound target, and a query observing only distinct endpoints from one hop on
runs a breadth-first search instead of enumerating trails, checked against SQL frontiers over the edge table.

Open, from those tables: optional chain on LiveJournal 25.6 vs 0.8 ms; one hop 1 ms vs 0.2 ms; all-shortest about
1 s at its slowest seed. The `before` LiveJournal results predate the key quoting, and the saved tables predate
these fixes. LSQB SF1 is not run yet. Delta size limit not tested at scale.

## IC14 under 100 ms

IC14 went from 318.8 to 65.5 ms (median, cost planner, parameterized; Ladybug 80.5). The four OPTIONAL branches
were joining about 1M rows each over the whole graph, because `a` and `b` had no cardinality estimate. Two changes
in `gql_optimizer_legacy.cpp`, which plans correlated stages for both planners:

- ALL SHORTEST between two bound vertices estimates `GQL_SHORTEST_PATH_STEPS_ESTIMATE` (64) rows, and a bound edge
  gives its endpoints its estimate. 64 is a constant, not a measurement: IC14's path search returns 232 step rows.
  At 256 the first expansion exceeds `CsrFrontierBeatsBulkScan`'s limit and the old plan returns.
- A correlated chain walks from one end. Expanding from both multiplied the two frontiers per outer row: 190M rows, 19 s.

Validation: 14/14 goldens on both planners, differential 140/140 per mode, mixed 56/56 in all three states on both
planners, SF10 16/16 with no time change beyond noise. Results in `OUT/after-ic14-under-100/`. Not tested: pairs with
a very large number of shortest paths, where the fixed estimate and plan could be worse than the old scan.
Next IC14 lead: `gql_csr_all_shortest` runs once per OPTIONAL branch, 4 times at about 10 ms each of CPU.

## Long paths, composed paths, read-only files

- Quantified edges over 4 hops or unbounded build a CSR before planning on managed graphs outside explicit
  transactions, as path searches do. A {1,30} path over 71 vertices took 1.25 s relationally.
- `(a)-[:R]->(b)-[:S]->{1,3}(c)` is split at shared vertices in the binder when edge types differ. Same-type
  edges would need trail checks across factors, and now say so instead of asking for `gql_build_csr`.
- Renamed database files work: managed tables resolve to the metadata's catalog when loaded.
- Read-only files work: `GqlEnsureStorage` no longer tries to create metadata in one.

SF10 on the delta build: 16/16 validated on both planners, and legacy no longer beats cost (14 timed queries
942 ms cost, 951 ms legacy; IC11 5.6 vs 6.6). IC1, IC7, IC10, IC12 and IC14 are still listed as unsupported in
that harness, whose text predates them.

Tried and reverted:

- Fetch cost at 24 + width/4 per row, from SF10 measurements of 17 units warm and 107 cold at 150 bytes. SF10
  IC2 172 -> 126 and IC4 69 -> 53, but SF10 IC11 5.6 -> 26.7, and SF1 IC10 13.3 -> 32.3, IC3 16.9 -> 33.8, IC9
  66.7 -> 78.2. The better lead: SF10 IC2 reads 30M rows, and a scan filtered by the join's IDs took 47 ms.
- `cost` as the default: 6 test files fail, mostly legacy plan shapes, and each stops at its first failure. The
  user's call, since it means pinning those tests to legacy.

The IC12 internal error is not root-caused. It needs 2+ threads, real writes, and that plan's join order
(`disabled_optimizers` of `join_order`, `build_side_probe_side` or `unused_columns` avoid it; perfect hash
and caching operators are not involved). **It is still reachable in an explicit transaction**, where no CSR is
available. Reproduce: 100 stream writes, `BEGIN`, IC12. It can invalidate the database.

Next: DuckLake writes, not started. Per `docs/ducklake-zero-copy-design.md`: `ACCESS_MODE 'READ_WRITE'`,
caller-supplied keys since DuckLake has no sequences, one DuckLake transaction per command, CSR freshness from
the snapshot ID. Referenced vertex IDs are dense and snapshot-local, so targets must be mapped to source keys
before anything is written, and mutation lowering needs non-canonical columns. Only the design survey is done.

## Incremental CSR (supersedes the stale-read figures in the next section)

Inserts reach reads through an immutable `GqlCsrDelta` over the immutable snapshot. Mixed workload on
the cost planner, right after writes, before -> after (ms): IC8 86.5 -> 4.9, IC10 919 -> 19.3,
IC11 97.8 -> 5.0, IC1 ~700 -> 16.8, IC12 errored -> 24.5; no CSR builds in 400 writes (351 vertices,
2,351 edges in the delta). Answers on the delta match Ladybug 56/56 on both planners. Reads with no
writes are unchanged. Results in `OUT/after-csr-delta/`.

How it holds together, all in `gql_csr.cpp`:

- The first acquisition in a statement catches up (`ResolveFullLease`) and the lease keeps that
  version for the statement. Algorithms still require an exact CSR and build their own.
- A scan is only needed when `GqlDatabaseWriteGeneration` moved. It is read before the scan, so a write
  committing during it causes one more scan rather than a miss.
- `ScanCsrDelta` reads rows from the row ID watermarks on, then compares table row counts. That check is
  what makes watermarks safe against out-of-order commits, moved row IDs and appenders; any failure rebuilds.
- These rebuild automatically: a label or edge type with no rows in the CSR, more than
  max(4096, edges/1024) delta rows, a schema version change, and an edge ID that doesn't match its row ID.
- Writes are classified in `gql_extension.cpp`. Delete, update, upsert and any prepared write still
  remove the CSR, and reads fall back to relational plans. Generations also advance at commit now, which
  closes a hole where a build between a write's plan and its commit recorded old rows as current.

Known limits: the optimizer plans from the snapshot's statistics, which lag by at most the delta limit. A
statement can see a commit made just after its transaction began. A rolled-back edge insert leaves a gap in
edge IDs, which turns off edge fetch for that graph until IDs match row IDs again, which they won't. Delta
memory is not in `memory_bytes`. Property updates remove the CSR even though they don't change topology.
Two concurrent GQL writers conflict on the `graph_version` row (found by the stress test, not new): one
gets `Conflict on update` and must retry.

Tests: `test/sql/gql_csr_delta.test`, `test_csr_delta.py` (prepared MATCH, appenders, 3 readers, 2 writers
and a rebuild loop). Two older tests asserted that any write removes the CSR: `gql_cost_join_domains.test`
now checks that with a delete, and `gql_cost_fanout.test` checks the delta count. `gql_csr_stats` gained
`delta_vertex_count` and `delta_edge_count`, and its totals include the delta.

## All 14 queries, and what a mixed workload shows

All 14 pass goldens natively under both planners, in fresh, prepared and parameterized modes.
Differential against Ladybug: 140/140 per mode across 10 people. Results in `OUT/after-ic1-ic14/`.
Added: `ANY SHORTEST` + `PATH_LENGTH`, `ALL SHORTEST`, `FOR step IN steps` over its group variable,
searched `CASE`, list values, and `[n IN NODES(p) | n.prop]` (a text rewrite; the vendored grammar is
unchanged). See `test/sql/gql_any_shortest_case_list.test` and `gql_all_shortest.test` for the limits.

`mixed_workload.py` replays LDBC's update stream. It records answers on the delta (`delta`), after a
rebuild (`fast`), and after a write that removes the CSR (`stale`, forced by an UPDATE matching no rows).

Open, in priority order:

1. **Internal error:** IC12, cost planner, no CSR. Outside transactions a long path now builds a CSR; see the
   newest section for what remains.
2. Incremental CSR: done, see above.
3. SF10: rerun, cost has caught up with legacy. Scans of 30M to 286M rows remain the largest gap there.

## GQL query parameters (newest; removes the "no parameters" limit noted below)

Binary SHA256 prefix `e9a607a2891b`, cost planner, 11 runs. `run.py --parameterized` writes
`results-parameterized.json`, binding the values in `run.py`'s `PARAMETERS` table. Ladybug figures
are from the run in the plan-reuse section, not rerun here. Results in `OUT/after-parameters/`.

| Query | Fresh compile | Literal prepared | Parameterized | Ladybug | Parameterized / Ladybug |
| --- | ---: | ---: | ---: | ---: | ---: |
| IC10 | 19.0 | 13.7 | 14.3 | 10.4 | 1.37x |
| IC11 | 4.4 | 2.1 | 4.3 | 1.6 | 2.61x |
| IC12 | 22.0 | 18.5 | 20.4 | 21.2 | 0.96x |

Syntax is `$name` or `$1`, not mixed, in read-only MATCH only: mutations, INSERT and procedure
configuration arguments reject them. Bind through the C API; SQL `PREPARE ... AS MATCH` still fails.

- IR: `GqlLiteralType::PARAMETER` on a LITERAL node, value = name, typed PROPERTY_VALUE in the binder.
- Lookup: equality on an indexed property lowers to `gql_vertex_lookup`, which probes the ART at
  execution and fetches rows. The MATCH predicate stays above it to recheck stale index entries.
  In an explicit transaction it lowers to a scan, because uncommitted rows are not in the shared index.
- DuckDB rejects parameters in table function arguments, so the value reaches the lookup laterally
  from a one-row subquery.
- DuckDB sets `rebind` whenever a parameter occurs twice. `ParametersFullyTyped` clears it only if
  every occurrence has the parameter's known type; otherwise it stays and the statement rebinds.
- **Deliberately not overridden:** `pushdown_get.cpp` invalidates a parameter filter that reaches a
  table scan, forcing a rebind so the filter can use zone maps. IC2, IC3, IC4, IC5, IC6, IC9 and IC11
  therefore replan each run and cost about the same as a fresh compile. Generic plans win only where
  no parameter filter reaches a scan: IC7, IC8, IC10, IC12, IC13. IC11's `work.workFrom < $workFromYear`
  is why it stays near 4 ms; overriding this would hurt large date-filtered scans.
- Plans are generic: they do not adapt to a value's selectivity.

Validation: `test_prepared_reuse.py` 95/95 (both planners; NULLs, multi-chunk lookups, updated keys,
checkpoint, uncommitted writes, one parameter at two types). GQL suite 4,424 assertions in 53 cases,
DuckLake skipped. Differential: one prepared plan rebound across 10 people, 120/120 under each planner,
plus 120/120 each for literals: 480 comparisons, 460 non-empty.

Next candidates: lower-cost edge property access so IC11's filter avoids a scan; SNB10 and DuckLake
runs; then making `cost` the default.

## Prepared plan reuse (newest)

Binary SHA256 prefix `07f840d6504f`, cost planner, 11 runs, both engines rerun sequentially.
`run.py --prepared` writes `results-prepared.json`: compile once, first timed run includes the
prepare, matching Ladybug's client, which implicitly caches a prepared statement per query text.

| Query | Fresh compile mean | Prepared mean | Ladybug mean | Prepared / Ladybug |
| --- | ---: | ---: | ---: | ---: |
| IC10 | 19.5 | 13.8 | 10.4 | 1.33x |
| IC11 | 5.2 | 2.2 | 1.6 | 1.31x |
| IC12 | 27.2 | 22.6 | 21.2 | 1.07x |

Full table: `OUT/after-plan-reuse/`. Fresh-compile figures vary between sessions (IC12 was 22.1 earlier).

`optimizer.cpp:578` makes DuckDB rebind every prepared statement that reads data, because plans
embed database state. `gql_plan_reuse.cpp` clears that flag only for read-only autocommit MATCH
statements over native sources that no binder marked for rebind (checked in a post-bind hook,
before the optimizer sets the flag). Execution rebinds when the write generation, selected graph,
CSR build count or `gql_optimizer` differs, or inside an explicit transaction.

- `GetLastCommit()` advances on read-only commits too, so it cannot be the token. It is used only
  to refuse capturing a plan while another commit landed.
- The write generation is `GqlDatabaseWriteGeneration`: advanced when a write is planned and after
  commit. The observer is registered on connection open so appenders are seen. An appender has no
  planning step, so a reader can overlap its commit for the instant before the post-commit advance.
- Any write anywhere in the database invalidates all prepared plans. A statement that has fallen
  behind rebinds on every later execution until re-prepared; the token is never refreshed.
- `benchmark/ldbc_complex_sf1/test_prepared_reuse.py` is required after touching this: SQL PREPARE
  rejects GQL, so sqllogictest cannot reach it. Negative control: with the token comparison disabled
  it reports 7 failures, including wrong answers and reads from the wrong graph.

## IC10/IC11/IC12 follow-up (newest; supersedes the table below for these)

Cost planner, 11 runs, binary SHA256 prefix `2ec0bf75c887`: IC10 19.0 ms (was 31.0, Ladybug
10.7), IC11 4.4 ms (was 4.8, Ladybug 1.8), IC12 22.1 ms (was 40.5, Ladybug 20.6). Results
in `OUT/after-ic10-12/`. Tests: 4,424 assertions in 53 cases, DuckLake still skipped.
Differential: 120/120 under each planner. New test: `gql_endpoint_elision_branches.test`.

1. Endpoint elision (`build_stage`): a vertex read only through its identity takes that
   identity from the edge, with all its labels tested in the expansion. Managed graphs only.
2. Independent OPTIONAL branches (`FindIndependentOptionalBranches`): lowers to
   `(X LEFT A) UNION ALL (X LEFT B)`. Conditions are in that function's comment. **Do not put
   the union inside one lateral join**: that correlates on all outer columns and doubled IC10
   by re-expanding sources per outer row. X is evaluated once per branch, adding compile time.
3. Do NOT raise the 4,096 fetch threshold. Measured crossover is about 4K-16K rows: fetching
   64K ids took 20.6 ms against 3.7 ms for scan and join. Batched fetch only helps small frontiers.

Remaining: IC11 is mostly compile time (about 2.8 of 4.2 ms). IC10 and IC12 spend most CPU
scanning vertices just for `.id`. `gql_optimizer_plan()` shows the planner's choice, not elision
or the branch rewrite, and the cost model does not account for either.

## Latest session: read this first (supersedes older numbers and advice below)

Still uncommitted. Binary SHA256 `dd578fabb23c7b9bd4d8616f55ad792ee5188d43b31b2ed77005728ece43c7e7`.
`run.py` now defaults to `--optimizer cost`, the planner the project is moving to; the
engine default is still `legacy`. Cost-planner correlated stages fall back to
`GqlOptimizeAccessPathsLegacy` (`gql_optimizer.cpp:932`), so that code still matters.

| Query | Session start (legacy) | Now (cost) | Ladybug 0.22.1, rerun | Now / Ladybug |
| --- | ---: | ---: | ---: | ---: |
| IC1 | unsupported | unsupported | 43.0 | - |
| IC2 | 32.7 | 23.1 | 10.4 | 2.23x |
| IC3 | 39.1 | 19.9 | 105.9 | 0.19x |
| IC4 | 43.6 | 15.0 | 5.7 | 2.63x |
| IC5 | 189.0 | 86.8 | 349.3 | 0.25x |
| IC6 | 22.2 | 8.1 | 9.2 | 0.88x |
| IC7 | 36.7 | 16.2 | 390.4 | 0.04x |
| IC8 | 14.8 | 4.9 | 1.4 | 3.55x |
| IC9 | 129.8 | 67.3 | 66.5 | 1.01x |
| IC10 | 122.5 | 31.0 | 10.7 | 2.89x |
| IC11 | 15.7 | 4.8 | 1.8 | 2.60x |
| IC12 | 205.5 | 40.5 | 20.6 | 1.96x |
| IC13 | 5.2 | 2.0 | 46.7 | 0.04x |
| IC14 | unsupported | unsupported | 87.0 | - |

Mean ms over five runs, both engines run sequentially in the same session. All 12 pass
goldens. Single-run noise reached about 20% on IC9, so compare repeated runs.

Changes:

1. Two-stage ANTLR parse (`gql_parser.cpp`): SLL, then LL only on failure. Full-context
   decisions are never cached in the DFA. Parse fell from about 2.5 to 0.4 ms.
2. Managed-graph metadata cache (`gql_catalog.cpp`): replaces per-statement catalog SQL.
   Any committed write to any database expires it, via `TransactionCommit` on the write
   observer. Referenced/DuckLake graphs are never cached.
3. `delim_join_as_cte=false` for the MATCH statement only (`gql_relational.cpp`), restored
   at QueryEnd, and an explicit user setting wins. **Upstream marks this setting deprecated**;
   its removal would be a compile error here. Avoiding delim joins is the durable fix.
4. Batched lateral execution (`gql_batched_inout.cpp`, `gql_csr.cpp`). DuckDB runs in-out
   functions with projected input row by row, giving one chunk per seed. A post-optimizer
   rule plans `gql_csr_expand` and element fetch without projected input, and they emit the
   passthrough columns themselves. `gql_csr_path_expand` is NOT batched yet.
5. Fetch is sorted per chunk. Keep the `emitted` handshake in `ElementFetchFunction`:
   constant-argument calls repeat until a call returns no rows, and removing it hangs.
6. Legacy planner: mapped row-ID fetch for correlated stages, and fetched vertices may seed
   CSR expansion. This reverses the older "do not enable mapped correlated fetch" advice,
   whose regression came from row-at-a-time fetch.
7. Neighbor-label filter: 5-field overload of `gql_csr_expand`, used only when
   `label_fanout_stats` shows it keeps under 90% of the edges (`GqlNeighborLabelIsSelective`).
   The 0.9 is a judgment call from one regression (IC9), not calibrated.

Validation: `build/release/test/unittest 'test/sql/gql*'` passes 4,196 assertions in 52 test
cases, with the DuckLake test skipped (`DUCKGQL_DUCKLAKE_EXTENSION` absent), so DuckLake is
unverified against these changes. New tests: `gql_statement_planning_state.test`,
`gql_labeled_expansion.test`. Differential against Ladybug's original Cypher on 10 person IDs
(friend degree 1 to 920), IC2-IC13: 120/120 ordered-row matches under each planner, 115
non-empty. Run `differential_duckgql.py DIR`, `differential_ladybug.py DIR` (Ladybug venv,
with `-I`), then `differential_compare.py DIR`. Rerun after any plan change. Not covered: the
invisible-row fallback in `ElementFetchFunction` has no direct test, and SNB10 was not rerun.

Suggested next steps, in order:

1. Run the SNB10 benchmarks and DuckLake tests, then consider making `cost` the default.
2. Give correlated stages real costing: they still use legacy fixed thresholds.
3. IC10: independent optional branches still form a 992K-row product before COUNT(DISTINCT).
4. IC8/IC11: about 2-3 ms of compile time remains against Ladybug totals under 2 ms.
5. Batch `gql_csr_path_expand`; IC1 and IC14 adapters remain open.

Artifacts: `OUT/after-labeled-expansion/` (results, comparison, source diff, differential
data); baseline in `OUT/before-compile-floor/`. `OUT/comparison.md` is regenerated.

## Objective and current status

The user asked to compare DuckGQL with the engines in
https://github.com/hyperdimensionalcomputingai/graph-benchmark-ldbc-complex,
measure Ladybug locally, close performance gaps, and investigate the supposedly
missing native GQL queries. The latest request is to leave this handoff for another
agent. Work is uncommitted; no PR or release has been made.

Current result: **12 of 14 native GQL SF1 fixed fixtures pass golden checks**.
IC1 and IC14 still lack complete adapters. Ladybug passes all 14 original queries.
The latest engine changes reduce IC10 from 1,058.10 to 122.47 ms and IC7 from
93.66 to 36.75 ms without changing their query text. Significant gaps remain.

This is a dirty-development, fixed-fixture engineering comparison, **not an
official LDBC driver benchmark or clean release result**.

## Workspace ownership and precautions

HEAD when work began: `549eb4f70e74b3cd18e7f6597f3c492e5edcc1c5`.
There were pre-existing modifications to `README.md`,
`scripts/benchmark/benchmark_snb_ab.py`, the `extension-ci-tools` submodule, and
untracked benchmark directories (`duckdb_versions`, `lsqb`, `snb10`, `vlp`,
`weighted_sssp`) and `tck`. Preserve these; do not reset the checkout.

Our work includes the new `benchmark/ldbc_complex_sf1/` directory, engine changes
listed below, five new SQL test files, and corrected unsupported-query descriptions
in `scripts/benchmark/benchmark_snb_gql_interactive.py`. Review diffs before staging.

There is no applicable root/home AGENTS.md found during this task. The DuckDB
submodule has its own AGENTS.md; we have not edited that submodule.

## Data, tools and reproducible commands

Existing upstream checkout: `/Users/rahuliyer/graph-benchmark-ldbc-complex`.
Reuse it; do not clone another copy. Its `.venv` has Python 3.13.14, Ladybug
0.22.1, Polars 1.37.1, PyArrow 23, pytest 9.0.2 and pytest-benchmark 5.2.3.

The downloaded upstream dataset is
`social_network-sf1-CsvBasic-StringDateFormatter/` in that checkout; the original
230 MB archive is also there. Original Cypher/schema/import definitions are in
`ladybugdb/query.py` and `ladybugdb/etl/`. Golden answers are in `expected/icN.json`;
upstream canonicalization is in `conftest.py`.

All commands below run from `/Users/rahuliyer/duckdb-gql`:

```sh
cmake --build build/release --target duckdb unittest duckgql_loadable_extension shell -j 8
build/release/test/unittest 'test/sql/gql*'
python3 benchmark/ldbc_complex_sf1/run.py
python3 benchmark/ldbc_complex_sf1/compare.py
/Users/rahuliyer/graph-benchmark-ldbc-complex/.venv/bin/python benchmark/ldbc_complex_sf1/run_ladybug.py
```

**Build trap:** target `duckdb` builds the shared library, not the CLI. Build
`shell` too when using `build/release/duckdb`. The shared C API library statically
embeds DuckGQL; rebuilding only the loadable extension does not update benchmark
behavior even though the runner executes LOAD. Build all four targets above.

Run tests and engine benchmarks sequentially, avoiding resource contention.
Catch's `'test/sql/gql*'` filter works; `'test/sql/gql_*.test'` did not match tests.

Main output directory: `build/benchmarks/ldbc-complex-sf1/` (called OUT below).
Databases: `OUT/sf1.duckdb` and `OUT/ladybug-sf1.lbdb`. Do not reload unnecessarily.
DuckGQL graph: `snb_interactive`, with 3,181,724 vertices and 17,436,661 edges.
Ladybug has 17,256,038 relationships; DuckGQL stores KNOWS in both directions.
The imported original numeric identifier is property `id`, distinct from internal
`__gql_id`. Post and Comment also have the Message label; Country/City also Place.

Machine: Apple M5 Pro, 15 cores, 24 GB, macOS 26.3.1. Four query threads, five timed
runs, no explicit warmup, ten-second timeout. DuckDB memory limit 8 GB versus
Ladybug buffer pool 8,000,000,000 bytes: these govern different resources.

DuckGQL timings include C API query execution and Python row/list conversion.
Ladybug timings include the upstream query and Polars conversion. Golden checks,
disk writes, loading, indexes, CSR build and Ladybug ANALYZE are outside timing.
One DuckGQL connection and one full CSR build are reused for all measured queries;
CSR is connection-local, not a persisted reusable index.

## Latest measured table

`OUT/results.json` is the authoritative latest DuckGQL run. All measured queries
passed the golden check after each of five timed executions. Ladybug values are
the earlier same-machine run in `OUT/ladybug-results.json`, not newly rerun after
each DuckGQL change. Values below are mean milliseconds.

| Query | Before latest optimizer work | Latest DuckGQL | Ladybug 0.22.1 |
| --- | ---: | ---: | ---: |
| IC1 | unsupported | unsupported | 53.298 |
| IC2 | 33.912 | 32.712 | 9.473 |
| IC3 | 47.676 | 39.055 | 112.599 |
| IC4 | 43.219 | 43.598 | 5.891 |
| IC5 | 186.321 | 189.008 | 388.888 |
| IC6 | 22.252 | 22.215 | 9.304 |
| IC7 | 93.661 | 36.747 | 410.313 |
| IC8 | 14.649 | 14.823 | 1.643 |
| IC9 | 128.105 | 129.793 | 70.020 |
| IC10 | 1058.104 | 122.470 | 10.963 |
| IC11 | 16.941 | 15.729 | 1.960 |
| IC12 | 239.650 | 205.468 | 21.587 |
| IC13 | 5.240 | 5.191 | 47.066 |
| IC14 | unsupported | unsupported | 85.156 |

Latest library SHA256 in results.json:
`3fb0dee42b97c8a08db290cc17559b36aeca2906e21fa32b0f0fa0fefde1d516`.
Pre-latest library:
`d4687f95c4ebb257393c9e08c2a401d755ae17822898efe8671e71056dfe2020`.

**Pending housekeeping:** `OUT/comparison.md` has NOT yet been regenerated after
the latest 122.47 ms run. Run `compare.py` before quoting that file. The prior
`OUT/native-adapters.md` and source hashes document the earlier 12-query build.
Do not present those older source hashes as the latest engine state.

## Work completed before the latest optimizer step

### Benchmark harness and native query coverage

New scripts: `run.py`, `run_ladybug.py`, `compare.py`, `profile.py` in this directory.
`run.py` uses stdlib ctypes against `build/release/src/libduckdb.dylib`, not a SQL
substitute for GQL. Query texts are versioned in `queries/icN.gql`, with fixed
upstream SF1 parameters. Result column names map positionally to golden keys;
row order is checked, and collected string lists are sorted as upstream does.

C API complex-result decoding uses chunk/vector APIs for lists rather than
parsing VARCHAR renderings. It handles nested lists, primitive values and UTF-8
strings. Explicitly checked inline/long strings, punctuation, nulls, numeric lists
and nested lists. Struct results are NOT implemented in this decoder yet.

The old harness excluded five queries with overly broad missing-feature claims.
Audits showed existing support for bounded hierarchy traversal, OPTIONAL MATCH,
aggregates and timestamp subtraction. Added complete adapters for:

- IC7: latest-like-per-liker via optional anti-selection; friendship optional
  match; FLOOR of elapsed minutes, with smaller message ID breaking equal-time ties.
- IC10: birthday window using YYYY-MM-DD string slicing; direct-friend exclusion;
  `2*COUNT(DISTINCT commonPost)-COUNT(DISTINCT post)` scoring.
- IC12: bounded 1..30 hierarchy traversal, null-aware self-class handling,
  COLLECT_LIST(DISTINCT tag.name), COUNT(DISTINCT comment.id).

Added generic unparameterized scalar CAST targets INTEGER/FLOAT/STRING/BOOLEAN
in `gql_transformer.cpp`, binder typing in `gql_binder.cpp`, and native lowering
in `gql_relational.cpp`. Added LIST type to `gql_ir.hpp` (appended enum member).
COLLECT_LIST lowers to null-filtered DuckDB list aggregation, supports DISTINCT,
and returns an empty list for empty/all-null inputs. `gql_cast_float` is registered
in `gql_extension.cpp`, declared in `gql_relational.hpp`, and marked SetFallible().
For a fixed duration, FLOAT conversion returns milliseconds following the
external formulation. Calendar-month intervals are rejected. This is an explicit
compatibility extension, not a claim of universal ISO duration-cast semantics.

Regression file: `test/sql/gql_cast_collect.test`.

### External corpus found on the web

Pinned all 14 TOMLs and MIT license from:
https://github.com/pleiad/frogql/tree/58089e09774ccce3cd907654eca1d59fc96334b1/bench/ldbc-queries

Files, README and checksum manifest are in `references/frogql/`. They are
formulation references, not validation evidence. External IC10 is itself marked
blocked for temporal predicates. IC1/IC14 formulations use features we do not yet
fully support, and need semantic checking even if parsed. Official LDBC v1 impls
did not have a native GQL directory when inspected.

### Earlier access-path and filtering improvements

Files: `gql_csr.cpp`, `gql_csr.hpp`, `gql_optimizer.cpp`,
`gql_optimizer_legacy.cpp`, `gql_relational.cpp`.

- Full managed CSR snapshots now optionally store compact vertex ordinal to
  physical row-ID mappings (`GQL_CSR_VERTEX_ROW_IDS`). Identity row IDs need no map.
  Handles reordered/sparse graph IDs and parallel-import ID/row-order differences.
- Fetch bind metadata comes from the snapshot instead of repeated graph metadata
  loads. Statement-local CSR leases avoid repeated validation; QueryEnd clears
  leases and generation checks preserve mutation/rebuild correctness.
- Cost planner can use HasVertexRowIds(). Legacy correlated fetch intentionally
  retains the stricter identity-rowid rule because broad enabling regressed IC8.
- Legacy fixed-chain access enumeration splits physical edge choices while
  preserving the original path for TRAIL and bag semantics. Initially this was
  limited to uncorrelated chains; the latest step expands that scope (below).
- MATCH-owned property/literal comparison conjuncts are applied before dependent
  CSR expansion using a local OFFSET 0 fence. Do not split OR or move predicates
  across optional boundaries without a semantic proof. IC4 improved roughly
  129 to 43 ms; IC3 also improved. IC11 improved roughly 42 to 15 ms through better
  chain access and scan reduction.

Tests: `gql_mapped_vertex_fetch.test`, `gql_fixed_chain_access.test`,
`gql_pre_expansion_predicates.test`.

## Latest optimizer work: implemented and verified

Latest changes are in `src/gql_optimizer_legacy.cpp`:

1. Enumerate fixed-edge CSR access alternatives inside correlated long MATCH
   chains, including OPTIONAL MATCH, while retaining the entire original path
   for uniqueness and the full optional boundary for null extension.
2. Guard these newly enabled correlated-chain choices using estimated expansion
   rows and existing edge statistics. `CsrFrontierBeatsBulkScan` requires a small
   fraction of the relation (threshold max(1024, edge_count/64)). Unknown estimates
   retain bulk access. This is a cost heuristic, not a semantic bound.
3. Permit independent small LABEL_POSTING_FETCH frontiers to use the snapshot's
   mapped physical row IDs. Eligibility uses `snapshot->HasVertexRowIds()` and
   MANAGED ownership, with the existing bounded posting-cardinality threshold.
   This does NOT broadly enable mapped correlated fetch. Only requested columns
   are fetched through the existing projected fetch operator.

An initial ungated correlated-chain experiment cut IC10 to ~129 ms and IC7 to
~46 ms, but regressed IC5 to ~254 ms. The fan-out guard restored IC5 (~189 ms
latest). Do not revert to unconditional correlated expansion.

Extended `gql_fixed_chain_access.test` to check correlated optional matches,
parallel-edge multiplicity, two captured endpoints, failure/null extension and
TRAIL uniqueness across the complete path. Added
`gql_mapped_label_postings.test` for small independent label frontiers with sparse
reordered IDs, projected properties, duplicate edges and write/rebuild behavior.

Validation completed on the final build:

- All four release targets rebuilt; log `build/ldbc-correlated-build.log`.
- **4,073 assertions passed across 50 executed GQL test cases**; one test skipped
  because DUCKGQL_DUCKLAKE_EXTENSION is absent. Log
  `build/ldbc-correlated-gql-tests.log`.
- All 12 implemented SF1 queries matched goldens on all five final timed runs.
  Log `build/ldbc-correlated-run.log`; authoritative `OUT/results.json`.
- `git diff --check` passed before handoff creation.

## Observations from profiles

Profiles are diagnostic executions, separate from benchmark timing. Operator CPU
times may overlap across threads; do not sum them as query wall time. Profile
files have mixed generations; record which build produced them.

Before correlated-chain improvement:

- IC10: 124.5 million intermediate rows; one inner join produced 103.3 million
  rows; 32.8 million physical rows scanned. Query returns 10 rows. Independent
  post branches and poor graph access create far more work than the result needs.
- IC12: 30.3 million physical rows scanned, 7.8 million intermediate rows to
  return 20. Broad property scans and late hierarchy restriction are expensive.
- IC8: existing profiles show broad vertex scans despite tiny neighborhoods;
  planning/binding overhead is material at a 1–2 ms target.
- IC9: ~1.4 million expansion rows, wide message content fetched before final
  top-20, and large DISTINCT input. Narrow ID frontiers and safe late properties
  remain candidates.

After the cost-guarded correlated-chain change, BEFORE the final mapped-posting
addition, IC10 profile was 9.30 million intermediate rows and 10.01 million rows
scanned (~140 ms diagnostic wall). Remaining hotspots included City/Post vertex
scans and aggregate DISTINCT work. This profile is currently `OUT/profiles/ic10.json`;
it is not a final-build profile. `ic7.json` is from the same intermediate build.

Saved Ladybug IC8/IC11/IC4 plans show SEMI_MASKER and selective SCAN_REL_TABLE /
SCAN_NODE_TABLE pipelines. They are stronger evidence than assuming adjacency
alone explains Ladybug's speed. DuckGQL already has CSR.

Research used for inspiration:

- Kùzu CIDR 2023 paper: https://www.vldb.org/cidrdb/papers/2023/p48-jin.pdf
  Accumulate-semijoin-probe passes discovered IDs to later scans and retains
  factorized intermediates instead of flattening all many-to-many combinations.
- Author explanation: https://kuzudb.github.io/blog/post/factorization/
- Ladybug documented architecture: https://docs.ladybugdb.com/

We implemented selective frontier access choices, NOT a factorized execution
engine or full ASP-Join. Be precise about that distinction.

## Artifacts and history

Under OUT:

- `before-optimizer/results.json`: original nine-query baseline.
- `before-filter-pushdown/`: preceding build and historical IC4 profile.
- `before-native-adapters.json`: nine-query results before three new adapters.
- `before-correlated-expansion/{results.json,comparison.md,ic10.json,ic12.json}`:
  baseline for the latest optimizer work. Preserve these.
- `results.json`: latest final 12-query results, binary/query/runner hashes.
- `ladybug-results.json`: original locally measured all-14 Ladybug run.
- `optimizer-changes.md`, `filter-pushdown.md`, `native-adapters.md`: historical
  stage-specific descriptions, NOT all updated to final build.
- `profiles/`: plans and profiles; `findings.md` explicitly describes an older
  baseline. `profile.py` currently covers only the original nine queries.
- `ic{5,7,10}-correlated-access.json`: access diagnostics from the initial ungated
  experiment. Do not treat as final access plans.
- `ic12-hierarchy-first.gql`: exploratory alternative, not adopted in queries/.

An IC12 hierarchy-first rewrite passed the fixed golden in one diagnostic run
at ~200 ms, but has not had repeated measurement or broader validation. A proposed
0..30 reverse hierarchy form was rejected: bounds currently require 1..64.
The shipped/timed IC12 text remains the original newly added adapter.

## Remaining work and suggested next steps

1. Regenerate comparison.md from latest results. Save final source hashes/diff
   and final IC7/IC10/IC12 profiles in a stage-specific report. This bookkeeping
   was pending when the user requested this handoff.
2. Validate changed access choices with more roots and parameters. Only the fixed
   upstream SF1 goldens plus synthetic semantic regression cases have been run.
   A planned extra differential check for IC7/10/12 with roots 7725 and
   19791209305691 against Ladybug was NOT implemented or executed. Read original
   queries/params via AST, change personId, compare canonical ordered rows. Run
   outside benchmark timings; avoid overwriting fixed goldens.
3. IC10 is still ~11.2x slower than Ladybug. Prioritize staged aggregation or a
   semantics-proven branch pre-aggregation rule, so independent optional post
   branches do not form a product before COUNT(DISTINCT). NEXT/aggregate-then-MATCH
   composition was unsupported in earlier audits. Implement reusable pipeline
   support or a guarded optimizer rule, not an IC10-specific SQL shortcut.
4. IC12 is still ~9.5x slower. Explore computing a small qualifying class/tag set
   first and passing it to comment/post scans. Preserve direct class matches,
   distinct comments/tags, zero-result groups, hierarchy limits and optional
   semantics. Current fixed-fixture rewrite evidence is not enough for a broad
   optimizer transformation.
5. Improve projected batched fetch vs bulk scan costing and semijoin filtering
   for IC8/IC9/IC11. Prior experiments with indiscriminate mapped correlated fetch
   or late Top-N regressed. Inspect actual projected columns, row counts and
   planning time before changing policy. Full graph values require all properties;
   ordinary property receivers should not accidentally request every column.
6. IC1 needs a complete shortest-distance selection plus nested university/employer
   collection composition. Many prerequisites already exist; do not claim all
   traversal/aggregate functionality is absent. Record/struct collection output
   and typed C API decoding may be needed.
7. IC14 needs ALL SHORTEST enumeration and quantified path values/path processing.
   Existing IC13 validates a native shortest-distance procedure, not all paths.
8. No clean-tag release benchmark, official LDBC workload, commit or PR yet.
   Preserve unrelated user work and name the exact binary/checkout in claims.

No long-running command was intentionally left active at handoff. The final
benchmark had completed before the interruption; results were re-read while
writing this file.
