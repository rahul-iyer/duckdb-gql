-- Microbenchmark the physical representation used by vertex-label predicates.
-- Run with:
--   ./build/release/duckdb -no-init < scripts/benchmark/benchmark_label_list_search.sql
--
-- The native case matches current LIST_COLUMN storage. The legacy case matches
-- the previous SCALAR_COLUMN plan, which split a semicolon-delimited VARCHAR
-- for every scanned row.

PRAGMA threads = 1;
PRAGMA preserve_insertion_order = false;

CREATE TABLE label_search AS
SELECT i,
       CASE WHEN i % 10 = 0
            THEN ['person', 'engineer', 'target']
            ELSE ['person', 'other']
       END::VARCHAR[] AS label_list,
       CASE WHEN i % 10 = 0
            THEN 'person;engineer;target'
            ELSE 'person;other'
       END AS label_text
FROM range(2000000) rows(i);

.print NATIVE_WARMUP
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');
.print LEGACY_WARMUP
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');

.timer on
.print NATIVE_1
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');
.print NATIVE_2
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');
.print NATIVE_3
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');
.print NATIVE_4
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');
.print NATIVE_5
SELECT count(*) FROM label_search WHERE list_contains(label_list, 'target');

.print LEGACY_1
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');
.print LEGACY_2
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');
.print LEGACY_3
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');
.print LEGACY_4
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');
.print LEGACY_5
SELECT count(*) FROM label_search
WHERE list_contains(string_split(label_text, ';'), 'target');
