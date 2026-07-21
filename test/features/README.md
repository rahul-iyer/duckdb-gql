# GQL clause feature corpus

This directory is the feature-level breadth corpus for `duckdb-gql`. The
`clauses` subtree is derived from the Apache-2.0 openCypher Technology
Compatibility Kit at the exact revision recorded in `clauses/SOURCE.json`.
Every derived feature retains the upstream notices, identifies its source, and
carries a prominent modification notice.

The corpus is not an ISO conformance claim and is not executed as Gherkin. The
generated `clauses/manifest.tsv` records every source scenario as
`ported_unverified`. Reviewed scenarios are ported into DuckDB SQLLogicTests
under `test/sql`; durable promotions belong in `execution.tsv`, outside the
regenerated subtree. An entry can move to `executable` only after it has a GQL
semantic review, fixture adaptation for managed DuckDB graph tables, and a
passing SQLLogicTest assertion.

Every source feature also has a generated counterpart under
`sqllogic/clauses`. These files retain the full GQL-adapted scenario
specification and its source mapping in SQLLogic `.test` files. They are kept
outside `test/sql` until their fixtures and assertions are executable, so a
pending port cannot be mistaken for a passing test. `sqllogic/manifest.tsv`
provides the one-to-one file inventory and per-file executable/pending counts.
`sqllogic/fixtures.tsv` separately records whether each source `having
executed` setup can be materialized through native `COPY GRAPH`, including
vertex/edge counts and a precise reason for setups that still require work.

## INSERT fixture adaptation

`scripts/gql_fixture_adapter.py` compiles deterministic literal `INSERT`
patterns into DuckDB `COPY` statements, typed Neo4j-format node and edge CSVs,
`CREATE GRAPH`, `COPY GRAPH`, and `SESSION SET GRAPH`. It supports scalar node
and relationship properties, directed paths, and variables bound within the
same setup. It deliberately rejects query-dependent setup, loops, lists,
multiple labels, undirected insertion, and dynamically incompatible property
types because the current native table representation cannot preserve those
semantics exactly.

## SQLLogicTest porting convention

- Use `test/sql/gql_feature_<clause>.test` for executable clause ports.
- Add `# Source: <source_path>:<source_line> scenario [n]` immediately before
  each port so failures remain traceable to the pinned upstream scenario.
- Keep each test graph isolated. `COPY GRAPH` may replace source setup steps
  until equivalent native GQL data-creation syntax is implemented.
- A mutating source query with a trailing result may be split into a mutation
  statement and a post-state `MATCH` assertion when the supported local syntax
  cannot return from the mutation yet.
- Record the port in `execution.tsv` only after the concrete `.test` passes.
- Passing imported scenarios measures compatibility breadth, not ISO GQL
  conformance. The ISO release gate remains separately manifest-backed.

## Mechanical query mappings

The sync script applies only these spelling-level mappings inside query doc
strings:

| Source spelling | GQL corpus spelling | Qualification |
|---|---|---|
| `CREATE pattern` | `INSERT pattern` | `ON CREATE` in compatibility `MERGE` is retained |
| `UNWIND expression AS variable` | `FOR variable IN expression` | Requires GQL `FOR` implementation |
| `SKIP count` | `OFFSET count` | Standardized spelling |
| `id(element)` | `element_id(element)` | GQL element identity function |
| `WITH projection` | `LET` bindings | Syntax-only translation; scope and aggregation require review |

Cypher `WITH` has no one-token semantic equivalent: its scope projection,
aggregation, ordering, and filtering behavior ultimately requires a manual
translation into GQL query composition. The generated `LET` form makes the
query vocabulary GQL-shaped without claiming semantic equivalence; synthetic
`__gql_with_scope_*` bindings keep those scenarios marked with the
`with_scope_translation` blocker. The same review rule applies to procedure
fixtures, value-rendering conventions, `MERGE` action clauses, and other
non-isomorphic behavior.

## Reproduce and validate

```bash
git clone https://github.com/opencypher/openCypher.git /tmp/openCypher
git -C /tmp/openCypher checkout 677cbafabb8c3c5eed458fd3b1ec0daec8d67d23
python3 scripts/sync_gql_clause_features.py --source /tmp/openCypher
python3 scripts/gql_fixture_adapter.py --self-test
python3 scripts/generate_gql_clause_sqllogic.py
python3 scripts/check_gql_feature_corpus.py
```

The executable DuckDB SQLLogicTests remain under `test/sql`; no Gherkin or TCK
runtime is required. SQLLogicTests create isolated managed graph fixtures,
execute GQL statements, compare typed result tables, and verify side effects.

`execution.tsv` is the reviewed overlay keyed by upstream source path and line.
An `executable` row must name the local test artifact that enforces the
scenario. An `unsupported` row must explain the deliberate incompatibility in
`notes`; neither status is inferred by the generator.
