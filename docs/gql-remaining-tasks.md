# GQL Remaining Task List

All tasks in this document are required for full ISO GQL support. P0, P1, and
P2 describe implementation order, not optionality.

The conformance source of truth remains
[`test/conformance/iso-gql-2024.tsv`](../test/conformance/iso-gql-2024.tsv).
When completing a task, add positive and negative tests, update the applicable
manifest rows, and run the verification commands at the end of this document.

## Current baseline

- Branch: `rewrite/native-tables`
- Native DuckDB wide-table graph storage; no EAV fallback
- Vectorized CSV and Parquet loading through `COPY GRAPH`
- 1,910 assertions passing across 14 GQL test cases
- 93 imported clause feature files and 827 source scenarios
- 93 verified executable source scenarios; 734 unreviewed
- 532 of 548 fixture setups adapted
- ISO manifest: 23 partial and 13 planned feature families
- Full release conformance gate does not pass yet

## P0 — Correct and complete the core

### Repository checkpoint

- [x] Commit the native-table rewrite and current compatibility suite (`1e206a5`).
- [x] Confirm the checkpoint passes the complete GQL test suite.

### Native INSERT

- [x] Implement native node `INSERT` over managed graph tables.
- [ ] Implement directed and undirected native edge `INSERT`. Directed fixed
      paths are implemented; undirected edges remain.
- [x] Support multiple elements in one insert graph pattern.
- [x] Support one fixed directed match-and-insert path, including matched
      endpoints, per-match-row property expressions, cardinality preservation,
      and command rollback. Multiple paths/clauses remain.
- [x] Make generated insert operations command-atomic in autocommit mode.
- [x] Verify read-your-writes and rollback inside caller transactions.

### Element and path values

- [x] Return nodes as typed DuckDB `STRUCT` values.
- [x] Return edges as typed DuckDB `STRUCT` values.
- [x] Return named fixed paths as typed DuckDB `STRUCT` values.
- [ ] Return quantified/VLP paths as typed values.
- [x] Define stable DuckDB result representations for node and edge values.
- [x] Define the stable DuckDB result representation for fixed path values.
- [x] Extend the compatibility candidate generator to validate unambiguous node,
      edge, and fixed-path result cells against adapted fixture identities.

### Element access functions

- [ ] Implement edge type and element-label access.
- [ ] Implement source-node access for edges.
- [ ] Implement destination-node access for edges.
- [ ] Implement property-map access where required by GQL.
- [ ] Complete element identity functions beyond the current `ELEMENT_ID`
      subset.

### Binder and type system

- [ ] Complete graph, binding-table, node, edge, path, scalar, list, record,
      temporal, and duration types.
- [ ] Track nullability through patterns, expressions, projections, and
      mutations.
- [ ] Implement numeric and property-value coercion rules.
- [ ] Implement function overload resolution.
- [ ] Add semantic validation for every supported expression and predicate.
- [ ] Preserve precise source ranges in all new typed AST and IR nodes.

### Parameters and casts

- [ ] Parse and bind query parameters.
- [ ] Connect parameters to session state.
- [ ] Implement supported scalar casts.
- [ ] Implement temporal, list, and compatible property casts.
- [ ] Add negative tests for invalid parameter and cast combinations.

### MATCH and OPTIONAL MATCH correctness

- [x] Preserve arbitrary ordered sequences of mandatory and optional match
      stages.
- [x] Support mandatory `MATCH` after `OPTIONAL MATCH` without reordering
      semantics.
- [x] Support optional stages that introduce no new bindings.
- [ ] Support optional stages combined with the remaining path forms.
- [ ] Add differential tests against equivalent native DuckDB joins.

### Mutation completeness

- [x] Implement `SET n = {...}` property-map replacement for managed node and
      edge tables, including empty maps, snapshot evaluation, and command
      rollback.
- [x] Implement explicit property-map merge compatibility with
      `SET n += {...}`, including overwrite, retained properties, NULL removal,
      snapshot expressions, and empty-map no-op behavior.
- [x] Complete ISO node and edge label setting/removal for `IS` and colon
      spellings with multiple comma-separated items and set-valued matching.
- [x] Complete bound node/edge deletion constraints: explicit edge targets are
      removed before node validation, unlisted incident edges reject plain and
      `NODETACH` deletion, and failures restore prior edge deletions.
- [x] Expand fixed path-variable `DELETE` and `DETACH DELETE` targets into their
      typed node/edge bindings, including repeated elements and rollback when
      an outside incident edge violates plain deletion.
- [x] Expand project-owned compact node/edge label chains such as
      `SET n:Foo:Bar` and `REMOVE n:Foo:Bar` into ordinary typed label
      mutations through a quote/comment-aware, source-offset-preserving parser
      adapter.
- [x] Extend compatibility property replacement/merge beyond inline map
      constructors to ordered record-valued `LET` bindings. The binder resolves
      nested record-field selections and expands their fields into the existing
      snapshot mutation program without runtime map materialization.
- [ ] Complete general runtime map expressions from parameters, functions, and
      independently materialized record values once those value layers exist.
- [x] Preserve literal list/record constructors in the typed AST and expand
      recursively nested `DELETE` targets, record-field selections, path
      leaves, duplicates, and NULL leaves into the existing typed mutation
      program without runtime collection materialization.
- [ ] Complete runtime list/record-derived `DELETE` selection through `LET`,
      `collect`, parameters, and list indexing; promote the applicable
      imported Delete5 scenarios only after those language layers exist.
- [x] Verify multiple mutation items use the required statement snapshot.
- [x] Verify generated mutation failures roll back the complete autocommit
      command, including failures after property-map clearing.

### MERGE completeness

- [ ] Implement edge and path `MERGE`.
- [ ] Implement `ON CREATE` actions.
- [ ] Implement `ON MATCH` actions.
- [ ] Support trailing query and mutation clauses.
- [ ] Define concurrency-safe match-or-create behavior.

### Transaction correctness

- [ ] Implement ISO transaction-start modes.
- [ ] Complete commit and rollback behavior for every graph command.
- [ ] Test read-your-writes across query, insert, mutation, and merge commands.
- [ ] Test isolation with concurrent graph readers and writers.
- [ ] Verify graph catalog and native tables commit atomically.

### Diagnostics

- [ ] Define stable GQL status conditions for parsing, binding, typing,
      execution, and transactions.
- [ ] Return precise source locations for all frontend errors.
- [ ] Document mappings from GQL conditions to DuckDB exceptions.
- [ ] Add negative conformance tests for every implemented feature family.

### Compatibility corpus

- [ ] Adapt the remaining 16 fixture setups.
- [ ] Classify all 734 currently unreviewed source scenarios.
- [ ] Promote every passing source-equivalent scenario into the active suite.
- [ ] Keep unsupported scenarios explicit and tied to concrete implementation
      gaps.

## P1 — Complete the language and graph surface

### Expressions and predicates

- [ ] Complete arithmetic, Boolean, comparison, null, and normalized
      expressions.
- [ ] Implement source/destination predicates.
- [ ] Implement same/different element predicates.
- [ ] Implement property-existence predicates.
- [ ] Complete label-expression wildcard, negation, conjunction, disjunction,
      and grouping semantics.

### Functions and values

- [ ] Complete numeric functions.
- [ ] Complete string functions.
- [ ] Complete aggregate functions.
- [ ] Implement list functions and list values.
- [ ] Implement record values.
- [ ] Implement temporal functions and values.
- [ ] Implement duration functions and values.
- [ ] Implement path functions.
- [ ] Implement graph and binding-table values.

### Variable-length paths

- [ ] Support multiple quantified factors in one query.
- [ ] Combine quantified paths with arbitrary fixed patterns.
- [ ] Support bound relationship and group variables.
- [ ] Implement correct zero-hop identity-path semantics.
- [ ] Support all finite and unbounded general quantifier forms.
- [ ] Remove the current single-anonymous-edge-factor restriction.

### Pattern algebra

- [ ] Implement general path concatenation.
- [ ] Implement path alternation and union.
- [ ] Implement parenthesized path expressions.
- [ ] Implement optional path factors.
- [ ] Implement simplified path patterns.
- [ ] Implement multiple general paths in one graph pattern.

### Path modes and searches

- [ ] Implement walk semantics.
- [ ] Implement trail semantics.
- [ ] Implement simple-path semantics.
- [ ] Implement acyclic-path semantics.
- [ ] Implement `ALL` and `ANY` path searches.
- [ ] Implement shortest-path and shortest-group searches.
- [ ] Implement `KEEP`.
- [x] Keep path-query execution native and reserve CSR for explicit graph
      algorithms.

### Query composition

- [ ] Complete `LET`; ordered deterministic scalar/record aliases used by
      projections, filters, and mutation expressions are inlined today, while
      independently materialized values, declared types, and broader value
      categories remain.
- [ ] Implement `FOR` and ordinality.
- [ ] Implement `SELECT` and `FINISH`.
- [ ] Implement `UNION`.
- [ ] Implement `EXCEPT`.
- [ ] Implement `INTERSECT`.
- [ ] Implement `OTHERWISE`.

### Catalog, schemas, and graph types

- [ ] Implement schema qualification.
- [ ] Implement `AT SCHEMA`.
- [ ] Implement `USE GRAPH` and complete focus reset behavior.
- [ ] Implement graph types and element types.
- [ ] Implement element-type inheritance.
- [ ] Implement open and closed graph semantics.
- [ ] Implement graph copy and replace semantics.
- [ ] Complete schema and graph lifecycle persistence tests.

## P2 — Advanced execution and production hardening

### Programs and procedures

- [ ] Implement multi-statement programs.
- [ ] Implement nested procedures and statement blocks.
- [ ] Implement `NEXT`.
- [ ] Implement `YIELD`.
- [ ] Implement named procedure calls with arguments and scopes.
- [ ] Implement inline procedure calls.

### Session behavior

- [ ] Implement schema reset.
- [ ] Implement graph reset.
- [ ] Implement session time zones.
- [ ] Complete session parameter behavior.
- [ ] Implement session close behavior.

### Physical optimization

- [x] Implement a connection-local ordinal CSR plus streaming BFS/DFS and
  dense iterative PageRank behind explicit `CALL` interfaces.
- [x] Add deterministic weak/strong component decomposition and degree-oriented
  triangle counting over the algorithm CSR.
- [x] Add streaming unweighted single-source shortest paths, exact in/out/total
  degree, and exact generalized outbound closeness over the algorithm CSR.
- [x] Add independently optional vertex- and edge-label filtering to every CSR
      algorithm, using induced vertex projections and compact CSR label IDs.
- [x] Add typed customer-facing algorithm `CALL ... YIELD ... RETURN` lowering
      with projection, aliases, ordering, and static paging.
- [x] Model algorithm `CALL` as a generic logical/physical procedure node with
      metadata-driven `NONE`, reserved `ROW`, and `BATCH` input modes. Lower
      `MATCH -> CALL -> YIELD -> RETURN` as one DuckDB table-in/out plan;
      BFS/DFS consume matched IDs as one frontier while global algorithms run
      once after their child is exhausted.
- [x] Replace row-at-a-time, double-sort CSR construction with chunk streaming,
  dense managed-ID lookup, exact allocation, and linear count/prefix/scatter.
- [x] Lower fixed mandatory regions as one native DuckDB join graph so DuckDB
  owns base-table statistics, filter pushdown, and inner-join enumeration.
- [ ] Collect only graph-specific statistics DuckDB cannot derive: degree and
  expansion fanout, endpoint/label correlation, and path-length distributions.
- [ ] Implement selectivity estimates for native recursive/path expansion.
- [ ] Implement a database-wide versioned algorithm CSR cache.
- [ ] Add bounded LRU eviction for CSR entries.
- [ ] Implement parallel CSR construction and PageRank/algorithm execution.
- [ ] Add typed aligned CSR edge-weight arrays and weighted Dijkstra/SSSP.
- [ ] Add parallel/approximate closeness for graphs where exact all-source BFS
      is not operationally appropriate.

### Concurrency and hardening

- [ ] Test concurrent graph readers and writers.
- [ ] Implement safe CSR construction and publication under concurrent writes.
- [ ] Add parser corpus fuzzing.
- [ ] Add planner and execution differential testing.
- [ ] Add crash/reopen and persistence testing.
- [ ] Add repeatable performance benchmarks.
- [ ] Add memory-budget and eviction tests.

### Release conformance

- [ ] Move every applicable row in `test/conformance/iso-gql-2024.tsv` to
      `implemented`.
- [ ] Pass `python3 scripts/check_gql_conformance.py --release`.
- [ ] Publish a feature-to-test traceability report.
- [ ] Track ISO GQL Corrigendum 1 and later revisions separately from the 2024
      baseline.

## Recommended next sequence

1. Implement element type, label, source, and destination functions.
2. Materialize quantified/VLP path values.
3. Complete multiple-path/clause and undirected `INSERT`.
4. Regenerate and promote compatibility candidates.

## Verification commands

```sh
cmake --build build/debug --target unittest gql_loadable_extension -j2
./build/debug/test/unittest "test/sql/gql*"
python3 scripts/check_gql_conformance.py
python3 scripts/check_gql_conformance.py --release
git diff --check
```
