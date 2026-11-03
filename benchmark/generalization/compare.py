#!/usr/bin/env python3
"""Prints latency by query and any answers that differ, for each dataset that has results."""
import json, statistics, sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'build/benchmarks/generalization'
COLUMNS = ['before', 'duckgql', 'ladybug']


def cell(result):
    if result is None:
        return '-'
    return f"{result['ms']:.1f}" if result['status'] == 'ok' else result['status'].split(':')[0]


def normal(answer):
    return sorted(str([float(value) if isinstance(value, (int, float)) else value for value in row]) for row in answer)


def main():
    failures = 0
    for dataset in ('air', 'roadnet', 'livejournal'):
        runs = {label: json.loads((OUT / f'{dataset}-{label}.json').read_text())['results']
                for label in COLUMNS if (OUT / f'{dataset}-{label}.json').exists()}
        if not runs:
            continue
        print(f'\n{dataset}: median ms across seeds, slowest seed in brackets')
        keys = next(iter(runs.values())).keys()
        for query in dict.fromkeys(key.split('/')[0] for key in keys):
            row = []
            for label in COLUMNS:
                if label not in runs:
                    continue
                entries = [value for key, value in runs[label].items() if key.split('/')[0] == query]
                good = [entry['ms'] for entry in entries if entry['status'] == 'ok']
                bad = len(entries) - len(good)
                text = f'{statistics.median(good):.1f} [{max(good):.1f}]' if good else '-'
                row.append(f'{label} {text}' + (f' ({bad} failed: {next(e["status"] for e in entries if e["status"] != "ok").split(":")[0]})' if bad else ''))
            print(f'  {query:20}', ' | '.join(row))
        reference = runs.get('ladybug')
        for label in ('before', 'duckgql'):
            if label not in runs or not reference:
                continue
            same = compared = 0
            for key, entry in runs[label].items():
                other = reference[key]
                if entry['status'] != 'ok' or other['status'] != 'ok':
                    continue
                compared += 1
                if normal(entry['answer']) == normal(other['answer']):
                    same += 1
                else:
                    failures += 1
                    print('  DIFFERS', label, key, str(entry['answer'])[:60], str(other['answer'])[:60])
            print(f'  answers, {label} vs ladybug: {same} of {compared} match')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
