#!/usr/bin/env python3
"""Run upstream LSQB SQL and equivalent DuckGQL MATCH queries with exact validation.

Requires an LSQB source checkout and its merged-FK dataset. Uses a local DuckDB
CLI with DuckGQL statically linked; no Python dependencies or implicit downloads.
Generated databases, converted CSVs, query plans, and reports stay in --output.
"""
import argparse
import csv
import hashlib
import json
import platform
import re
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERTICES = ['Company', 'University', 'Continent', 'Country', 'City', 'Forum',
            'Comment', 'Post', 'Person', 'Tag', 'TagClass']
# Source table, source type/id, destination type/id, edge label.
FOREIGN_KEYS = [
    ('Company', 'Company', 'CompanyId', 'Country', 'isLocatedIn_CountryId', 'IS_LOCATED_IN'),
    ('University', 'University', 'UniversityId', 'City', 'isLocatedIn_CityId', 'IS_LOCATED_IN'),
    ('Country', 'Country', 'CountryId', 'Continent', 'isPartOf_ContinentId', 'IS_PART_OF'),
    ('City', 'City', 'CityId', 'Country', 'isPartOf_CountryId', 'IS_PART_OF'),
    ('Forum', 'Forum', 'ForumId', 'Person', 'hasModerator_PersonId', 'HAS_MODERATOR'),
    ('Comment', 'Comment', 'CommentId', 'Person', 'hasCreator_PersonId', 'HAS_CREATOR'),
    ('Comment', 'Comment', 'CommentId', 'Country', 'isLocatedIn_CountryId', 'IS_LOCATED_IN'),
    ('Comment', 'Comment', 'CommentId', 'Post', 'replyOf_PostId', 'REPLY_OF'),
    ('Comment', 'Comment', 'CommentId', 'Comment', 'replyOf_CommentId', 'REPLY_OF'),
    ('Post', 'Post', 'PostId', 'Person', 'hasCreator_PersonId', 'HAS_CREATOR'),
    ('Post', 'Forum', 'Forum_containerOfId', 'Post', 'PostId', 'CONTAINER_OF'),
    ('Post', 'Post', 'PostId', 'Country', 'isLocatedIn_CountryId', 'IS_LOCATED_IN'),
    ('Person', 'Person', 'PersonId', 'City', 'isLocatedIn_CityId', 'IS_LOCATED_IN'),
    ('Tag', 'Tag', 'TagId', 'TagClass', 'hasType_TagClassId', 'HAS_TYPE'),
    ('TagClass', 'TagClass', 'TagClassId', 'TagClass', 'isSubclassOf_TagClassId', 'IS_SUBCLASS_OF'),
]
RELATIONS = ['Comment_hasTag_Tag', 'Post_hasTag_Tag', 'Forum_hasMember_Person',
             'Forum_hasTag_Tag', 'Person_hasInterest_Tag', 'Person_likes_Comment',
             'Person_likes_Post', 'Person_studyAt_University', 'Person_workAt_Company']


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(cli, database, sql, timeout):
    result = subprocess.run([str(cli), '-unsigned', '-no-init', '-batch', '-bail', '-csv', '-noheader', str(database)],
                            input=sql, text=True, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(result.stderr[-6000:])
    return result.stdout


def gql_query(upstream, index):
    query = (upstream / 'cypher' / f'q{index}.cypher').read_text().strip()
    # Cypher pattern predicates become GQL existential subqueries. Identity
    # comparisons explicitly use element IDs rather than materializing values.
    query = query.replace('WHERE NOT (comment)-[:HAS_TAG]->(tag1)',
                          'WHERE NOT EXISTS { MATCH (comment)-[:HAS_TAG]->(tag1) }')
    query = query.replace('WHERE NOT (person1)-[:KNOWS]-(person3)',
                          'WHERE NOT EXISTS { MATCH (person1)-[:KNOWS]-(person3) }')
    query = query.replace('AS count', 'AS match_count')
    query = query.replace('tag1 <> tag2', 'element_id(tag1) <> element_id(tag2)')
    query = query.replace('person1 <> person3', 'element_id(person1) <> element_id(person3)')
    return query.rstrip(';') + ';'


def load(args, database):
    sql = (args.upstream / 'sql/schema.sql').read_text()
    sql += (args.upstream / 'sql/snb-load.sql').read_text().replace('PATHVAR', str(args.data).replace("'", "''"))
    sql += (args.upstream / 'sql/views.sql').read_text()
    # The upstream SQL loader stores KNOWS in both directions. The graph must
    # store each input edge only once because its MATCH pattern is undirected.
    sql += f"CREATE TABLE raw_knows AS SELECT * FROM read_csv({literal(args.data / 'Person_knows_Person.csv')}, delim='|', header=true, columns={{'src':'BIGINT','dst':'BIGINT'}});\n"
    nodes = []
    for name in VERTICES:
        label = name + (';Message' if name in ('Comment', 'Post') else '')
        nodes.append(f"SELECT '{name}:' || {name}Id::VARCHAR AS \"id:ID\", '{label}' AS \":LABEL\" FROM {name}")
    edges = list(FOREIGN_KEYS)
    for table in RELATIONS:
        source, relation, target = table.split('_')
        label = re.sub(r'(?<!^)(?=[A-Z])', '_', relation).upper()
        edges.append((table, source, source + 'Id', target, target + 'Id', label))
    edges.append(('raw_knows', 'Person', 'src', 'Person', 'dst', 'KNOWS'))
    arcs = [f"SELECT '{source}:' || {source_id}::VARCHAR AS \":START_ID\", "
            f"'{target}:' || {target_id}::VARCHAR AS \":END_ID\", '{label}' AS \":TYPE\" "
            f'FROM {table} WHERE {source_id} IS NOT NULL AND {target_id} IS NOT NULL'
            for table, source, source_id, target, target_id, label in edges]
    nodes_path, edges_path = args.output / 'vertices.csv', args.output / 'edges.csv'
    sql += f"COPY ({' UNION ALL '.join(nodes)}) TO {literal(nodes_path)} (HEADER, FORMAT CSV);\n"
    sql += f"COPY ({' UNION ALL '.join(arcs)}) TO {literal(edges_path)} (HEADER, FORMAT CSV);\n"
    sql += f"CREATE GRAPH lsqb ANY;\nCOPY GRAPH lsqb FROM (VERTICES {literal(nodes_path)}, EDGES {literal(edges_path)}) FORMAT GRAPH;\nCHECKPOINT;\n"
    prefix = f"SET threads={args.threads}; SET memory_limit={literal(args.memory_limit)};\n"
    (args.output / 'load.sql').write_text(prefix + sql)
    started = time.perf_counter()
    run(args.cli, database, prefix + sql, args.timeout)
    return time.perf_counter() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--upstream', type=Path, required=True)
    parser.add_argument('--upstream-commit', required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--scale-factor', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cli', type=Path, default=ROOT / 'build/release/duckdb')
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--memory-limit', default='8GB')
    parser.add_argument('--max-spill', default='16GB')
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--rounds', type=int, default=2)
    parser.add_argument('--timeout', type=int, default=300, help='Timeout per process including warmup and all samples')
    parser.add_argument('--reuse', action='store_true', help='Reuse an already loaded output database')
    args = parser.parse_args()
    if min(args.threads, args.runs, args.rounds, args.timeout) < 1:
        parser.error('threads, runs, rounds, and timeout must be positive')
    for name in ['upstream', 'data', 'output', 'cli']:
        setattr(args, name, getattr(args, name).resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    database = args.output / 'lsqb.duckdb'
    expected = {}
    for row in csv.reader((args.upstream / 'expected-output/expected-output.csv').open(), delimiter='\t'):
        if row[2] == args.scale_factor:
            expected[int(row[3])] = int(row[5])
    if set(expected) != set(range(1, 10)):
        parser.error('upstream expected results must contain all nine queries for the scale factor')
    metadata = {'upstream_commit': args.upstream_commit, 'scale_factor': args.scale_factor,
                'platform': platform.platform(), 'threads': args.threads, 'memory_limit': args.memory_limit,
                'max_spill': args.max_spill, 'process_timeout_seconds': args.timeout,
                'cli': str(args.cli), 'cli_sha256': digest(args.cli),
                'data_sha256': {p.name: digest(p) for p in sorted(args.data.glob('*.csv'))},
                'upstream_sha256': {str(p.relative_to(args.upstream)): digest(p)
                                    for folder in ['sql', 'cypher', 'expected-output']
                                    for p in sorted((args.upstream / folder).glob('*')) if p.is_file()},
                'source_commit': subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip(),
                'source_diff_sha256': hashlib.sha256(subprocess.check_output(
                    ['git', '-C', str(ROOT), 'diff', '--', 'src'])).hexdigest(),
                'runs': args.runs, 'rounds': args.rounds,
                'scope': 'Managed DuckGQL vs upstream merged-FK DuckDB SQL; warm queries with JSON profiling; '
                         'one unmeasured warmup per query/variant/round; separate CLI processes share a persistent database; '
                         'variant order alternates by round; load and conversion excluded from query latency.'}
    if not args.reuse:
        if database.exists():
            parser.error('output database already exists; use a new output or --reuse')
        print('Loading official SQL schema and converting the same data to a managed graph', flush=True)
        metadata['load_and_conversion_seconds'] = load(args, database)
        (args.output / 'dataset.json').write_text(json.dumps(metadata, indent=2) + '\n')
    elif not database.exists():
        parser.error('--reuse requires an existing output database')
    else:
        previous = json.loads((args.output / 'dataset.json').read_text())
        for key in ['data_sha256', 'scale_factor', 'upstream_commit']:
            if previous[key] != metadata[key]:
                parser.error(f'reused dataset differs: {key}')
        metadata['load_and_conversion_seconds'] = previous['load_and_conversion_seconds']
    report = {'metadata': metadata, 'queries': {}}
    prefix = f"SET threads={args.threads}; SET memory_limit={literal(args.memory_limit)}; SET max_temp_directory_size={literal(args.max_spill)}; SESSION SET GRAPH lsqb;\n"
    for index in range(1, 10):
        record = {'expected_count': expected[index], 'variants': {}}
        report['queries'][f'q{index}'] = record
        variants = {'sql': (args.upstream / 'sql' / f'q{index}.sql').read_text().strip(),
                    'gql': gql_query(args.upstream, index)}
        for round_index in range(args.rounds):
            order = list(variants) if round_index % 2 == 0 else list(reversed(variants))
            for variant in order:
                entry = record['variants'].setdefault(variant, {'query': variants[variant], 'seconds': [], 'status': 'pending'})
                if entry['status'] in ('error', 'timeout', 'mismatch'):
                    continue
                stem = args.output / f'q{index}-{variant}-round{round_index}'
                sql = '.output /dev/null\n' + prefix
                sql += variants[variant] + '\n'
                for sample in range(args.runs):
                    profile = Path(str(stem) + f'-{sample}.json')
                    result = Path(str(stem) + f'-{sample}.csv')
                    sql += f"PRAGMA enable_profiling='json'; PRAGMA profiling_output={literal(profile)};\n"
                    sql += f'.once {literal(result)}\n{variants[variant]}\nPRAGMA disable_profiling;\n'
                Path(str(stem) + '.sql').write_text(sql)
                try:
                    plan = run(args.cli, database, prefix + 'EXPLAIN ' + variants[variant], args.timeout)
                    Path(str(stem) + '-explain.txt').write_text(plan)
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    entry['explain_error'] = str(error)
                started = time.perf_counter()
                try:
                    # Validate a separate first execution before spending time on
                    # repeated measurements. The measurement process still gets
                    # its own unmeasured warmup to preserve its connection cache.
                    if not entry.get('preflight_verified'):
                        result_text = run(args.cli, database, '.output /dev/null\n' + prefix + '.output\n' + variants[variant], args.timeout)
                        Path(str(stem) + '-preflight.csv').write_text(result_text)
                        rows = list(csv.reader(result_text.splitlines()))
                        if rows != [[str(expected[index])]]:
                            entry['status'] = 'mismatch'
                            entry['actual_rows'] = rows
                            raise ValueError(f'expected {expected[index]}, got {rows}')
                        entry['preflight_verified'] = True
                    run(args.cli, database, sql, args.timeout)
                    for sample in range(args.runs):
                        rows = list(csv.reader(Path(str(stem) + f'-{sample}.csv').open()))
                        if rows != [[str(expected[index])]]:
                            entry['status'] = 'mismatch'
                            entry['actual_rows'] = rows
                            raise ValueError(f'expected {expected[index]}, got {rows}')
                        profile = json.loads(Path(str(stem) + f'-{sample}.json').read_text())
                        entry['seconds'].append(profile['latency'])
                    entry['status'] = 'verified'
                    entry['median_ms'] = statistics.median(entry['seconds']) * 1000
                    print(f'q{index} {variant} round {round_index + 1}: {entry["median_ms"]:.3f} ms, count {expected[index]} verified', flush=True)
                except subprocess.TimeoutExpired:
                    entry.update(status='timeout', error=f'process exceeded {args.timeout} seconds')
                    print(f'q{index} {variant}: TIMEOUT', flush=True)
                except (RuntimeError, ValueError, OSError) as error:
                    if entry['status'] != 'mismatch':
                        entry['status'] = 'error'
                    entry['error'] = str(error)
                    print(f'q{index} {variant}: {entry["status"]}: {error}', flush=True)
                entry['last_process_seconds'] = time.perf_counter() - started
                (args.output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    verified = all(v['status'] == 'verified' for q in report['queries'].values() for v in q['variants'].values())
    print(f'All nine queries verified in both variants: {verified}', flush=True)
    return 0 if verified else 1


if __name__ == '__main__':
    raise SystemExit(main())
