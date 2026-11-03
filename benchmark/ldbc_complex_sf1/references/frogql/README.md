# Pinned external GQL reference

The fourteen TOML files are copied unchanged from [pleiad/frogql](https://github.com/pleiad/frogql/tree/58089e09774ccce3cd907654eca1d59fc96334b1/bench/ldbc-queries), commit `58089e09774ccce3cd907654eca1d59fc96334b1`. The upstream MIT license is preserved. `source.json` records checksums.

These are formulation references, not SF1 validation evidence. IC10 is explicitly blocked in this corpus. DuckGQL adaptations live in ../../queries and are checked against graph-benchmark-ldbc-complex expected JSON. IC7 uses OPTIONAL MATCH anti-selection rather than VALUE/RECORD subqueries; IC12 uses a bounded hierarchy traversal and distinct comment counts. IC10 uses the imported ISO date string representation for month/day selection. Its fixed May/June window belongs to the upstream SF1 fixture.

IC7 follows the reference numeric duration convention: CAST(duration AS FLOAT) produces milliseconds for fixed durations. This is a DuckGQL compatibility extension, not a claim of universal ISO GQL duration-cast semantics. Calendar-month intervals are rejected.
