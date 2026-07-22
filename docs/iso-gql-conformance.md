# ISO GQL Conformance Program

## Baseline

The release target is the complete language defined by [ISO/IEC 39075:2024](https://www.iso.org/standard/76120.html), amended by [Technical Corrigendum 1](https://www.iso.org/standard/93701.html) after its final publication. The next ISO edition is tracked independently so a revision cannot silently change an existing release's semantics.

The vendored [OpenGQL ANTLR grammar](https://github.com/opengql/grammar/tree/16ea71bd320ad07fd2c46a3066afbaef7d226922) is the executable syntax source. It contains 571 parser rules at the pinned commit. It is not a substitute for the standard's semantic, typing, transaction, and diagnostic requirements. Exact conformance work therefore requires an authorized copy of the applicable ISO text and corrigendum.

## Definition of support

A feature is `implemented` only if all applicable layers are complete:

1. Syntax and a typed, source-located AST.
2. Binding, scope, type inference/checking, and ISO semantic validation.
3. Logical lowering and correct physical execution.
4. Correct MVCC, transaction, persistence, and concurrency behavior.
5. Positive, negative, boundary, and diagnostic tests.

`partial` means at least one valid form executes but the feature family is incomplete. `planned` means the grammar may recognize it but execution is not implemented. Grammar recognition alone is never reported as language support.

The machine-readable source of record is [`../test/conformance/iso-gql-2024.tsv`](../test/conformance/iso-gql-2024.tsv). It deliberately uses TSV rather than JSON: rows are stable in code review, can be joined directly in DuckDB, and do not require nested document parsing.

CI validates its schema, unique feature IDs, coverage of ISO grammar sections 6 through 21, and the pinned grammar's 571-rule inventory:

```sh
python3 scripts/check_gql_conformance.py
```

The eventual release gate additionally rejects every applicable row that is not `implemented`:

```sh
python3 scripts/check_gql_conformance.py --release
```

## Current coverage summary

| Area | Current status | Completion requirement |
|---|---|---|
| Program/session/transactions | Partial | All session set/reset/close, parameters, transaction modes, commit/rollback, multi-statement flow |
| Catalog and graph types | Partial | Schema, graph, graph type, typed/open/closed/copy forms, qualification and replacement semantics |
| Data modification | Partial | Standalone node/directed-path insert, one fixed directed match-and-insert path, and core matched property set/remove and edge/node/detach delete use native DuckDB DML; multiple-path/clause and undirected insert, whole-map replacement, open-graph schema evolution, and complete label-set semantics remain |
| Query clauses | Partial | Match/optional, let, for, filter, calls, select/return/finish, grouping, ordering/paging, composition |
| Graph patterns and paths | Partial | Complete pattern algebra, all directions, predicates, quantifiers, modes, searches, shortest paths/groups |
| Expressions and types | Partial | Full value system, operators, predicates, functions, casts, coercion, null/unknown semantics |
| Procedures and parameters | Planned | Named/inline calls, argument binding, yield, graph/table/value parameters |
| Diagnostics and status | Partial | Stable standard-aligned status mapping and source ranges for every failure phase |

## Engineering gates

- Every manifest row must name its tests before it moves to `implemented`.
- Parser tests must exercise token comments and formatting so no clause depends on raw-source regex matching.
- Semantic tests must include syntactically valid but ill-typed/ill-scoped statements.
- Query tests must compare CSR-backed execution with a canonical relational reference implementation.
- Modification tests must prove same-transaction read-after-write, rollback, concurrent-reader isolation, and reopen persistence.
- “Full ISO GQL” is a release claim only when every applicable manifest row is `implemented` and the pinned revision's conformance suite is green.
