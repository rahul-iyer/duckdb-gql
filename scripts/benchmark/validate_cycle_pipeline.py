"""Compare cycle-first MATCH pipelines with native SQL, including exact bags."""
import argparse
import csv
import hashlib
import itertools
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', type=Path, default=ROOT / 'build/release/duckdb')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    nodes = [(1, 'Country'), (2, 'Country'), (3, 'City'), (4, 'City'),
             (5, 'Person'), (6, 'Person'), (7, 'Person'), (8, 'Country;City;Person')]
    edges = [(a, b, 'LOC') for a, b in [(5, 3), (5, 3), (6, 3), (7, 4), (8, 8), (5, 4)]]
    edges += [(a, b, 'PART') for a, b in [(3, 1), (3, 2), (4, 1), (8, 8)]]
    edges += [(a, b, 'K') for a, b in [(5, 6), (6, 7), (7, 5), (6, 5), (5, 5), (5, 6),
                                     (8, 8), (8, 8), (8, 8)]]
    for name, header, rows in [('nodes', [':ID', ':LABEL'], nodes),
                               ('edges', [':START_ID', ':END_ID', ':TYPE'], edges)]:
        with (out / f'{name}.csv').open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(header)
            writer.writerows(rows)
    sql = (f".output /dev/null\nCREATE GRAPH controls ANY; COPY GRAPH controls FROM "
           f"(VERTICES {literal(out / 'nodes.csv')}, EDGES {literal(out / 'edges.csv')}) "
           "FORMAT GRAPH; SESSION SET GRAPH controls;\n")
    edge_table = 'gql_data.graph_1_edges'
    vertex_table = 'gql_data.graph_1_vertices'
    orientations = list(itertools.product([False, True], repeat=3)) + [None]
    cases = list(itertools.product(orientations, [False, True], [False, True], [False, True]))
    for case, (directions, reverse_loc, reverse_part, filtered) in enumerate(cases):
        gql = 'MATCH (country:Country)\n'
        filters = ["list_contains(country.__gql_label, 'country')"]
        tables = [f'{vertex_table} country']
        bindings = ['country']
        for i in range(3):
            loc = f'<-[l{i}:LOC]-' if reverse_loc else f'-[l{i}:LOC]->'
            part = f'<-[r{i}:PART]-' if reverse_part else f'-[r{i}:PART]->'
            gql += f'MATCH (p{i}:Person){loc}(c{i}:City){part}(country)\n'
            tables += [f'{vertex_table} p{i}', f'{vertex_table} c{i}',
                       f'{edge_table} l{i}', f'{edge_table} r{i}']
            bindings += [f'p{i}', f'c{i}', f'l{i}', f'r{i}']
            ls, lt = ('target', 'source') if reverse_loc else ('source', 'target')
            rs, rt = ('target', 'source') if reverse_part else ('source', 'target')
            filters += [f"list_contains(p{i}.__gql_label, 'person')",
                        f"list_contains(c{i}.__gql_label, 'city')",
                        f"l{i}.__gql_type='loc'", f"r{i}.__gql_type='part'",
                        f'l{i}.__gql_{ls}_id=p{i}.__gql_id', f'l{i}.__gql_{lt}_id=c{i}.__gql_id',
                        f'r{i}.__gql_{rs}_id=c{i}.__gql_id', f'r{i}.__gql_{rt}_id=country.__gql_id',
                        f'l{i}.__gql_edge_id<>r{i}.__gql_edge_id']
        gql += 'MATCH (p0)'
        for i in range(3):
            direction = directions[i] if directions is not None else None
            arc = f'<-[e{i}:K]-' if direction else f'-[e{i}:K]->'
            if directions is None:
                arc = f'-[e{i}:K]-'
            gql += arc + f'(p{(i + 1) % 3})'
            source, target = ('target', 'source') if direction else ('source', 'target')
            tables.append(f'walk e{i}' if directions is None else f'{edge_table} e{i}')
            bindings.append(f'e{i}')
            filters += [f"e{i}.__gql_type='k'", f'e{i}.__gql_{source}_id=p{i}.__gql_id',
                        f'e{i}.__gql_{target}_id=p{(i + 1) % 3}.__gql_id']
            filters += [f'e{i}.__gql_edge_id<>e{j}.__gql_edge_id' for j in range(i)]
        if filtered:
            gql += '\nWHERE element_id(p0) <> element_id(p1)'
            filters.append('p0.__gql_id<>p1.__gql_id')
        gql += '\nRETURN ' + ', '.join(f'element_id({b})' for b in bindings) + ';'
        # UNION ALL retains the two orientations of self-loops, as GQL does.
        prefix = (f'WITH walk AS (SELECT * FROM {edge_table} UNION ALL SELECT __gql_edge_id, '
                  f'__gql_target_id, __gql_source_id, __gql_type FROM {edge_table}) ')
        columns = [f'{b}.__gql_' + ('edge_id' if b[0] in 'lre' else 'id') for b in bindings]
        native = (prefix + 'SELECT ' + ', '.join(columns) + ' FROM ' + ', '.join(tables)
                  + ' WHERE ' + ' AND '.join(filters) + ';')
        for kind, query in [('gql', gql), ('sql', native)]:
            sql += f'.once {literal(out / f"{case}-{kind}.csv")}\n{query}\n'
    (out / 'controls.sql').write_text(sql)
    result = subprocess.run([str(args.cli.resolve()), '-unsigned', '-no-init', '-batch', '-bail',
                             '-csv', '-noheader'], input=sql, text=True, capture_output=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr)
    checked = 0
    for case in range(len(cases)):
        def rows(kind):
            with (out / f'{case}-{kind}.csv').open() as stream:
                return sorted(csv.reader(stream))
        actual, expected = rows('gql'), rows('sql')
        assert actual == expected, (case, len(actual), len(expected))
        checked += len(actual)
    report = {'binary_sha256': hashlib.sha256(args.cli.read_bytes()).hexdigest(), 'cases': len(cases), 'matched_rows': checked, 'status': 'verified',
              'scope': 'Exact bags of seven vertex and nine edge IDs; multiple location mappings, '
                       'parallel edges, self-loops, mixed labels, reversed and undirected cycles; '
                       'native SQL retains all vertex joins and per-path trail inequalities.'}
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report)


if __name__ == '__main__':
    main()
