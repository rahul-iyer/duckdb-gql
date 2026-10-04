#!/usr/bin/env python3
"""Check directed shortest paths against Python BFS and optionally compare builds.

Uses local CLI binaries and generated graphs only. Timings include query planning;
cold calls include CSR construction. Baseline and candidate run in alternating order.
"""
import argparse
import csv
import io
import hashlib
import platform
import json
import random
import re
import statistics
import subprocess
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def quote(value):
    return "'" + str(value).replace("'", "''") + "'"


def fixture(directory, name, count, edges):
    nodes, arcs = directory / f'{name}_nodes.csv', directory / f'{name}_edges.csv'
    with nodes.open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow(['id:ID', ':LABEL'])
        writer.writerows((v, 'node;selected' if v % 3 else 'node') for v in range(1, count + 1))
    with arcs.open('w', newline='') as stream:
        writer = csv.writer(stream)
        writer.writerow([':START_ID', ':END_ID', ':TYPE'])
        writer.writerows(edges)
    return (f'CREATE GRAPH {name} ANY;\nCOPY GRAPH {name} FROM '
            f'(VERTICES {quote(nodes)}, EDGES {quote(arcs)}) FORMAT GRAPH;\nSESSION SET GRAPH {name};\n')


def run(shell, sql):
    result = subprocess.run([str(shell), '-unsigned', '-batch', '-bail', '-csv', '-noheader'],
                            input='SET threads=1;\n' + sql,
                            text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


def query(name, source, target, vertex_label='', edge_label=''):
    return (f'MATCH (s:node), (t:node) WHERE element_id(s) = {source} AND element_id(t) = {target}\n'
            f'CALL algo.shortest_path_length({quote(name)}, {source}, {target}, '
            f'{quote(vertex_label)}, {quote(edge_label)}) YIELD distance RETURN distance;\n')


def reference(count, edges, source, vertex_label, edge_label):
    adjacency = [[] for _ in range(count + 1)]
    for a, b, label in edges:
        if (not edge_label or label == edge_label) and (not vertex_label or (a % 3 and b % 3)):
            adjacency[a].append(b)
    distance = [-1] * (count + 1)
    distance[source] = 0
    frontier = deque([source])
    while frontier:
        a = frontier.popleft()
        for b in adjacency[a]:
            if distance[b] == -1:
                distance[b] = distance[a] + 1
                frontier.append(b)
    return distance


def validate(shell, directory):
    rng = random.Random(20260908)
    sql, expected = [], []
    for index, edge_count in enumerate([12, 40, 160, 400]):
        count = 20
        # Reserve vertex 20 as isolated; loops and parallel edges are intentional.
        edges = [(rng.randrange(1, count), rng.randrange(1, count), rng.choice(['route', 'other']))
                 for _ in range(edge_count)]
        name = f'check_paths_{index}'
        sql += ['.output /dev/null\n', fixture(directory, name, count, edges), '.output\n']
        for vertex_label, edge_label in [('', ''), ('', 'route'), ('selected', 'route'), ('', 'missing')]:
            vertices = [v for v in range(1, count + 1) if not vertex_label or v % 3]
            for source in vertices:
                distances = reference(count, edges, source, vertex_label, edge_label)
                for target in vertices:
                    sql.append(query(name, source, target, vertex_label, edge_label))
                    expected.append(distances[target])
    actual = [int(row[0]) for row in csv.reader(io.StringIO(run(shell, ''.join(sql))))]
    assert actual == expected, next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                                   ('row count', len(actual), len(expected)))
    return len(expected)


def benchmark(shells, directory, count, repeats):
    shapes = {
        'chain': ([(i, i + 1, 'route') for i in range(1, count)], 1, count),
        'out_hub': ([(1, i, 'route') for i in range(2, count)] + [(count - 1, count, 'route')], 1, count),
        'in_hub': ([(i, count, 'route') for i in range(2, count)] + [(1, 2, 'route')], 1, count),
        'tree': ([(i // 2, i, 'route') for i in range(2, count + 1)], 1, count),
        'unreachable': ([(1, i, 'route') for i in range(2, count)], 1, count),
        'cycle': ([(i, i + 1, 'route') for i in range(1, count)] + [(count, 1, 'route')], 1, count // 2),
    }
    report = []
    for name, (edges, source, target) in shapes.items():
        setup = fixture(directory, name, count, edges)
        expected = reference(count, edges, source, '', '')[target]
        samples = {label: {'cold_ms': [], 'warm_ms': [], 'csr_memory_bytes': []} for label in shells}
        for round_index in range(4):
            order = list(shells.items())
            if round_index % 2:
                order.reverse()
            for label, shell in order:
                sql = '.output /dev/null\n' + setup + '.output\n.timer on\n'
                sql += query(name, source, target) * (repeats + 2)
                sql += f".timer off\nSELECT memory_bytes FROM gql_csr_stats('{name}');\n"
                output = run(shell, sql)
                times = [float(x) * 1000 for x in re.findall(r'Run Time[^\n]*real ([0-9.]+)', output)]
                rows = [int(line) for line in output.splitlines() if re.fullmatch(r'-?\d+', line)]
                assert len(times) == repeats + 2 and rows[:-1] == [expected] * (repeats + 2), output
                samples[label]['cold_ms'].append(times[0])
                samples[label]['warm_ms'].extend(times[2:])
                samples[label]['csr_memory_bytes'].append(rows[-1])
        for sample in samples.values():
            sample['warm_median_ms'] = statistics.median(sample['warm_ms'])
            sample['cold_median_ms'] = statistics.median(sample['cold_ms'])
        report.append({'shape': name, 'vertices': count, 'edges': len(edges), 'distance': expected,
                       'measurements': samples})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shell', type=Path, default=ROOT / 'build/release/duckdb')
    parser.add_argument('--baseline-shell', type=Path, help='Separate CLI with baseline DuckGQL statically linked')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--vertices', type=int, default=100000)
    parser.add_argument('--repeats', type=int, default=9)
    args = parser.parse_args()
    if args.vertices < 4 or args.repeats < 1:
        parser.error('vertices must be >= 4 and repeats must be >= 1')
    args.output.mkdir(parents=True, exist_ok=True)
    shells = {'candidate': args.shell}
    if args.baseline_shell:
        shells['baseline'] = args.baseline_shell
    hashes = {label: hashlib.sha256(path.read_bytes()).hexdigest() for label, path in shells.items()}
    if len(shells) > 1 and len(set(hashes.values())) != len(shells):
        parser.error('baseline and candidate must be different binaries')
    report = {'binaries': {label: {'path': str(path.resolve()), 'sha256': hashes[label]}
                           for label, path in shells.items()},
              'platform': platform.platform(),
              'correctness_queries': {label: validate(path, args.output)
                                      for label, path in shells.items()},
              'benchmarks': benchmark(shells, args.output, args.vertices, args.repeats),
              'measurement': 'Single thread; CLI wall time at 1 ms precision; four alternating rounds; '
                             'one cold call and one warmup per round; warm timings include planning. '
                             'CSR bytes exclude process memory and traversal buffers.'}
    (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
