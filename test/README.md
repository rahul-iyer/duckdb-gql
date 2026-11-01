# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the quack extension, this should probably be the goal too.

The `features` directory contains the source-pinned, GQL-adapted clause breadth
corpus plus one generated SQLLogic `.test` port specification for every source
feature. Reviewed executable scenarios live in `sql`; pending specifications do
not count as passing tests. See [`features/README.md`](features/README.md).

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

The real DuckLake identity-cache integration test requires a compatible signed
DuckLake extension. It uses an isolated catalog under the test directory and
skips when the extension path is absent:

```bash
DUCKGQL_DUCKLAKE_EXTENSION=/absolute/path/ducklake.duckdb_extension \
  build/release/test/unittest 'test/sql/gql_ducklake_identity.test'
```
