#!/usr/bin/env python3
"""Reports mixed_workload.py results, and whether both engines agree after the writes."""
import json, re, sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'build/benchmarks/ldbc-complex-sf1'


def canonical(value):
    if isinstance(value, list):
        if value and all(isinstance(item, list) for item in value):
            return sorted([str(part) for part in item] for item in value)
        return sorted(value) if value and all(isinstance(item, str) for item in value) else value
    if isinstance(value, float):
        return round(value, 6)
    if isinstance(value, str) and re.match(r'\d{4}-\d\d-\d\d[T ]', value):
        return value.replace(' ', 'T')[:23]
    return value


def rows(answer):
    return [[canonical(value) for value in row] for row in answer]


planner = sys.argv[1] if len(sys.argv) > 1 else 'cost'
print('DuckGQL planner:', planner)
duck = json.loads((OUT / f'mixed-duckgql-{planner}.json').read_text())['phases']
lady = json.loads((OUT / 'mixed-ladybug.json').read_text())['phases']
print('Cold start, ms (file in OS cache)')
for key in ('open_ms', 'prepare_ms', 'first_query_ms', 'open_to_first_answer_ms'):
    print(f"  {key:26}{duck['cold_start'][key]:10.1f}{lady['cold_start'][key]:10.1f}")
print('Single writes, median ms: operation, DuckGQL, Ladybug, count')
for key in duck['writes']:
    print(f"  {key:6}{duck['writes'][key]['median_ms']:9.2f}{lady['writes'][key]['median_ms']:9.2f}{duck['writes'][key]['count']:7}")
print('Read median ms: query, no writes D/L, right after writes D/L, D rebuilt after each batch')
for query in duck['reads_without_writes']:
    print(f"  {query:6}{duck['reads_without_writes'][query]['median_ms']:8.1f}{lady['reads_without_writes'][query]['median_ms']:8.1f}"
          f"{duck['mixed_no_maintenance']['reads'].get(query, {'median_ms': float('nan')})['median_ms']:10.1f}{lady['mixed_no_maintenance']['reads'][query]['median_ms']:8.1f}"
          f"{duck['mixed_with_maintenance']['reads'][query]['median_ms']:10.1f}")
print('DuckGQL CSR rebuild after a batch, median ms:', round(duck['mixed_with_maintenance']['after_batch']['median_ms'], 1))
print('Batch of writes, median ms: D, L:', round(duck['mixed_no_maintenance']['batch_writes']['median_ms'], 1),
      round(lady['mixed_no_maintenance']['batch_writes']['median_ms'], 1))

duck_answers = json.loads((OUT / f'mixed-duckgql-{planner}-answers.json').read_text())
lady_answers = json.loads((OUT / 'mixed-ladybug-answers.json').read_text())
failures = 0
for label in ('fast', 'stale'):
    matched = compared = skipped = 0
    for key, expected in lady_answers.items():
        actual = duck_answers[key.replace('fast/', label + '/', 1)]
        if actual == 'SKIPPED':
            skipped += 1
            continue
        compared += 1
        ordered = (lambda answer: sorted(rows(answer), key=str)) if key.endswith('/ic14') else rows
        if not isinstance(actual, str) and not isinstance(expected, str) and ordered(actual) == ordered(expected):
            matched += 1
        else:
            failures += 1
            if failures <= 12:
                print('  DIFFERS', label, key, 'duck', str(actual)[:170], '\n    lady', str(expected)[:170])
    print(f'Answers after writes, DuckGQL {label} vs Ladybug: {matched} of {compared} match, {skipped} skipped')
sys.exit(1 if failures else 0)
