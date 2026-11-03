# SF1 complex-read comparison

Run from the DuckGQL checkout after building the release library, extension, tests, and shell (`cmake --build build/release --target duckdb unittest duckgql_loadable_extension shell -j 8`):

```sh
python3 benchmark/ldbc_complex_sf1/run.py --upstream /path/to/graph-benchmark-ldbc-complex
python3 benchmark/ldbc_complex_sf1/compare.py --upstream /path/to/graph-benchmark-ldbc-complex
```

Download and extract the upstream SF1 StringDateFormatter dataset first. The runner uses the standard library and the local DuckDB C API; no Python engine dependency is needed. It writes the database, graph Parquet files, fixed parameters, query text, actual rows, and results under `build/benchmarks/ldbc-complex-sf1/`. Subsequent runs reuse the loaded database; use a new `--output` directory to reload. The comparison script writes `comparison.md` beside `results.json`.

All 14 queries have native GQL adaptations in `queries/` and golden-answer checks, ordered except for IC14, as upstream's are, with collected lists compared sorted. The pinned external GQL corpus and provenance are in `references/frogql/`. Saved upstream engine results are compared with the local measurements and are not rerun. See the generated report for schema changes, timing boundaries, machine differences and the exact coverage boundary.

To measure Ladybug locally, install Ladybug 0.22.1, Polars, PyArrow and pytest in a virtual environment, then run:

```sh
/path/to/venv/bin/python benchmark/ldbc_complex_sf1/run_ladybug.py --upstream /path/to/graph-benchmark-ldbc-complex
python3 benchmark/ldbc_complex_sf1/compare.py --upstream /path/to/graph-benchmark-ldbc-complex
```

The Ladybug runner creates a separate database, imports the original upstream schema, builds its prescribed indexes, runs ANALYZE, and measures all 14 original queries. It uses four query threads, an 8,000,000,000-byte buffer pool and five timed executions without explicit warmups. Buffer pool size is not a total-process memory cap. Run engines sequentially to avoid benchmark contention. The comparison includes local Ladybug results when `ladybug-results.json` exists, while preserving the historical upstream table.

The optimizer experiment preserves its original baseline in `before-optimizer/results.json` and records the current outcome in `optimizer-changes.md`, with source hashes alongside it. `profile.py` captures current native plans and JSON profiles separately from the timed comparison. The default optimizer remains `legacy`; mapped fetch support is available to the experimental cost planner, while legacy correlated fetch eligibility stays conservative.

The following filter-pushdown step preserves the preceding build in `before-filter-pushdown/results.json`; its measurements and validation are documented in `filter-pushdown.md`. Current profiles describe the latest build, while the original IC4 profile for this step is preserved in `before-filter-pushdown/ic4-profile.json`.

`run.py` measures the cost planner by default; pass `--optimizer legacy` for the engine's current default. The `differential_*.py` scripts compare DuckGQL under both planners with Ladybug's original Cypher on other person IDs, outside benchmark timing; see `HANDOFF.md`.

`run.py --prepared` compiles each query once and reuses the plan, as Ladybug's client does implicitly, and writes `results-prepared.json`. `test_prepared_reuse.py` checks that prepared plans never outlive what they depend on. `test_csr_delta.py` checks that inserts reach reads without a rebuild, with prepared plans, appenders and concurrent connections.

`run.py --parameterized` also compiles once, and binds the upstream values as `$name` parameters instead of writing them as literals; it writes `results-parameterized.json`.
