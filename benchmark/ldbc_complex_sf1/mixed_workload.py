#!/usr/bin/env python3
"""Replay LDBC's update stream between reads on DuckGQL or Ladybug.

An engineering comparison, not an official LDBC driver run. Each run works on a
fresh copy of the database. It records cold start, per-operation write latency,
and read latency with no writes and straight after a batch of writes. It finally
saves query answers so mixed_workload_compare.py can check both engines agree.

    python3 benchmark/ldbc_complex_sf1/mixed_workload.py --engine duckgql
    VENV/bin/python -I benchmark/ldbc_complex_sf1/mixed_workload.py --engine ladybug
    python3 benchmark/ldbc_complex_sf1/mixed_workload_compare.py
"""
import argparse, heapq, json, re, shutil, statistics, sys, time, types
from datetime import datetime, timedelta, timezone
from decimal import Decimal
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/benchmarks/ldbc-complex-sf1'
UPSTREAM = Path('/Users/rahuliyer/graph-benchmark-ldbc-complex')
STREAMS = UPSTREAM / 'social_network-sf1-CsvBasic-StringDateFormatter'
READS = [2, 8, 10, 11, 12, 1]
ANSWER_QUERIES = list(range(1, 15))
ANSWER_PEOPLE = [1131, 2199023262543, 13194139538102, 19791209305691]


def stream_operations(limit):
    """Yields operations of both streams in scheduled order, as LDBC replays them."""
    def rows(name):
        with (STREAMS / f'updateStream_0_0_{name}.csv').open() as stream:
            for line in stream:
                fields = line.rstrip('\n').split('|')
                yield int(fields[0]), int(fields[2]), fields[3:]
    for index, operation in enumerate(heapq.merge(rows('person'), rows('forum'), key=lambda row: row[0])):
        if index == limit:
            return
        yield operation


def timestamp(milliseconds):
    return (datetime(1970, 1, 1) + timedelta(milliseconds=int(milliseconds))).isoformat(sep=' ', timespec='milliseconds')


def ids(text):
    return [int(part) for part in text.split(';') if part]


def pairs(text):
    return [tuple(int(value) for value in part.split(',')) for part in text.split(';') if part]


def properties(values):
    """Empty fields are absent, as they are after the bulk CSV load."""
    return {name: value for name, value in values.items() if value != ''}


def describe(kind, fields):
    """Returns a new vertex, if any, and its edges as engine-neutral values.

    An edge is (source label, source id, relationship, target label, target id, properties).
    Relationship names are the DuckGQL types; Ladybug resolves them by endpoint below.
    """
    if kind == 1:
        person, first, last, gender, birthday, created, ip, browser, city = fields[:9]
        person = int(person)
        tags, study, work = fields[11], fields[12], fields[13]
        vertex = ('Person', person, properties({
            'firstName': first, 'lastName': last, 'gender': gender,
            'birthday': (datetime(1970, 1, 1) + timedelta(milliseconds=int(birthday))).date().isoformat(),
            'creationDate': timestamp(created), 'locationIP': ip, 'browserUsed': browser}))
        edges = [('Person', person, 'IS_LOCATED_IN', 'Place', int(city), {})]
        edges += [('Person', person, 'HAS_INTEREST', 'Tag', tag, {}) for tag in ids(tags)]
        edges += [('Person', person, 'STUDY_AT', 'Organisation', org, {'classYear': year}) for org, year in pairs(study)]
        edges += [('Person', person, 'WORK_AT', 'Organisation', org, {'workFrom': year}) for org, year in pairs(work)]
        return vertex, edges
    if kind in (2, 3):
        target = 'Post' if kind == 2 else 'Comment'
        return None, [('Person', int(fields[0]), 'LIKES', target, int(fields[1]), {'creationDate': timestamp(fields[2])})]
    if kind == 4:
        forum, title, created, moderator, tags = int(fields[0]), fields[1], fields[2], int(fields[3]), fields[4]
        vertex = ('Forum', forum, {'title': title, 'creationDate': timestamp(created)})
        edges = [('Forum', forum, 'HAS_MODERATOR', 'Person', moderator, {})]
        edges += [('Forum', forum, 'HAS_TAG', 'Tag', tag, {}) for tag in ids(tags)]
        return vertex, edges
    if kind == 5:
        return None, [('Forum', int(fields[0]), 'HAS_MEMBER', 'Person', int(fields[1]), {'joinDate': timestamp(fields[2])})]
    if kind == 6:
        post, image, created, ip, browser, language, content, length, person, forum, country, tags = fields[:12]
        post = int(post)
        vertex = ('Post', post, properties({
            'imageFile': image, 'creationDate': timestamp(created), 'locationIP': ip, 'browserUsed': browser,
            'language': language, 'content': content, 'length': int(length)}))
        edges = [('Post', post, 'HAS_CREATOR', 'Person', int(person), {}),
                 ('Forum', int(forum), 'CONTAINER_OF', 'Post', post, {}),
                 ('Post', post, 'IS_LOCATED_IN', 'Place', int(country), {})]
        edges += [('Post', post, 'HAS_TAG', 'Tag', tag, {}) for tag in ids(tags)]
        return vertex, edges
    if kind == 7:
        comment, created, ip, browser, content, length, person, country, reply_post, reply_comment, tags = fields[:11]
        comment = int(comment)
        vertex = ('Comment', comment, properties({
            'creationDate': timestamp(created), 'locationIP': ip, 'browserUsed': browser,
            'content': content, 'length': int(length)}))
        edges = [('Comment', comment, 'HAS_CREATOR', 'Person', int(person), {}),
                 ('Comment', comment, 'IS_LOCATED_IN', 'Place', int(country), {})]
        if int(reply_post) >= 0:
            edges.append(('Comment', comment, 'REPLY_OF', 'Post', int(reply_post), {}))
        if int(reply_comment) >= 0:
            edges.append(('Comment', comment, 'REPLY_OF', 'Comment', int(reply_comment), {}))
        edges += [('Comment', comment, 'HAS_TAG', 'Tag', tag, {}) for tag in ids(tags)]
        return vertex, edges
    if kind == 8:
        return None, [('Person', int(fields[0]), 'KNOWS', 'Person', int(fields[1]), {'creationDate': timestamp(fields[2])})]
    raise ValueError(f'unknown LDBC update {kind}')


def fresh_directory(path):
    """A leftover write-ahead log would replay a previous run's writes onto the new copy."""
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir(parents=True)
    return path


class DuckGQL:
    name = 'duckgql'
    LABELS = {'Post': 'Post&Message', 'Comment': 'Comment&Message'}

    def __init__(self, work, optimizer='cost'):
        sys.path.insert(0, str(HERE))
        from run import DB
        directory = fresh_directory(work / 'duckgql')
        # Graph metadata records the catalog name, which comes from the file name.
        self.path = directory / 'sf1.duckdb'
        shutil.copy(OUT / 'sf1.duckdb', self.path)
        started = time.perf_counter()
        self.db = DB(self.path, 4)
        self.db.query('SESSION SET GRAPH snb_interactive')
        self.db.query(f"SET gql_optimizer='{optimizer}'")
        self.open_ms = (time.perf_counter() - started) * 1000

    def prepare_for_reads(self):
        started = time.perf_counter()
        self.db.query("CALL gql_build_csr('snb_interactive')")
        return (time.perf_counter() - started) * 1000

    def delta_state(self):
        row = self.db.query("SELECT delta_vertex_count, delta_edge_count, build_count FROM gql_csr_stats('snb_interactive')")[0]
        return dict(zip(('delta_vertices', 'delta_edges', 'build_count'), row.values()))

    def literal(self, value):
        return str(value) if isinstance(value, int) else "'" + str(value).replace("'", "''") + "'"

    def assignments(self, values):
        return '{' + ', '.join(f'{name}: {self.literal(value)}' for name, value in values.items()) + '}'

    def edge(self, edge):
        source_label, source, relationship, target_label, target, values = edge
        body = f' {self.assignments(values)}' if values else ''
        return (f"MATCH (s:{source_label}), (t:{target_label}) WHERE s.id = {source} AND t.id = {target} "
                f"INSERT (s)-[:{relationship}{body}]->(t)")

    def apply(self, kind, fields):
        vertex, edges = describe(kind, fields)
        statements = []
        if vertex:
            label, identifier, values = vertex
            statements.append(f"INSERT (:{self.LABELS.get(label, label)} {self.assignments({'id': identifier, **values})})")
        for edge in edges:
            statements.append(self.edge(edge))
            if edge[2] == 'KNOWS':
                # The bulk load stores friendship in both directions.
                statements.append(self.edge(('Person', edge[4], 'KNOWS', 'Person', edge[1], edge[5])))
        if len(statements) > 1:
            self.db.query('BEGIN')
        for statement in statements:
            self.db.query(statement)
        if len(statements) > 1:
            self.db.query('COMMIT')

    def text(self, number, person):
        return (HERE / f'queries/ic{number}.gql').read_text().replace('1131', str(person))

    def read(self, number, person=1131):
        self.db.query(self.text(number, person), 60)

    def answer(self, number, person):
        return [list(row.values()) for row in self.db.query(self.text(number, person), 120)]


class Ladybug:
    name = 'ladybug'
    RELATIONSHIPS = {
        ('Person', 'IS_LOCATED_IN'): 'personIsLocatedIn', ('Post', 'IS_LOCATED_IN'): 'postIsLocatedIn',
        ('Comment', 'IS_LOCATED_IN'): 'commentIsLocatedIn', ('Person', 'HAS_INTEREST'): 'hasInterest',
        ('Person', 'STUDY_AT'): 'studyAt', ('Person', 'WORK_AT'): 'workAt', ('Forum', 'HAS_MODERATOR'): 'hasModerator',
        ('Forum', 'HAS_TAG'): 'forumHasTag', ('Post', 'HAS_TAG'): 'postHasTag', ('Comment', 'HAS_TAG'): 'commentHasTag',
        ('Forum', 'HAS_MEMBER'): 'hasMember', ('Post', 'HAS_CREATOR'): 'postHasCreator',
        ('Comment', 'HAS_CREATOR'): 'commentHasCreator', ('Forum', 'CONTAINER_OF'): 'containerOf',
        ('Person', 'KNOWS'): 'knows'}

    def __init__(self, work):
        import ladybug
        import importlib.util
        directory = fresh_directory(work / 'ladybug')
        self.path = directory / 'ladybug-sf1.lbdb'
        shutil.copy(OUT / 'ladybug-sf1.lbdb', self.path)
        spec = importlib.util.spec_from_file_location('golden_checks', UPSTREAM / 'conftest.py')
        self.check = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.check)
        self.source = (UPSTREAM / 'ladybugdb/query.py').read_text()
        self.modules = {}
        started = time.perf_counter()
        self.database = ladybug.Database(str(self.path), buffer_pool_size=8_000_000_000, max_num_threads=4)
        self.conn = ladybug.Connection(self.database)
        self.conn.execute('RETURN 1').close()
        self.open_ms = (time.perf_counter() - started) * 1000

    def prepare_for_reads(self):
        return 0.0

    def relationship(self, source_label, relationship, target_label):
        if relationship == 'LIKES':
            return 'likePost' if target_label == 'Post' else 'likeComment'
        if relationship == 'REPLY_OF':
            return 'replyOfPost' if target_label == 'Post' else 'replyOfComment'
        return self.RELATIONSHIPS[(source_label, relationship)]

    @staticmethod
    def coerce(name, value):
        if name in ('creationDate', 'joinDate'):
            return datetime.fromisoformat(value)
        if name == 'birthday':
            return datetime.fromisoformat(value).date()
        return value

    def run(self, statement, parameters):
        self.conn.execute(statement, parameters).close()

    def apply(self, kind, fields):
        vertex, edges = describe(kind, fields)
        multiple = bool(vertex) + len(edges) > 1
        if multiple:
            self.conn.execute('BEGIN TRANSACTION').close()
        if vertex:
            label, identifier, values = vertex
            body = ', '.join(f'{name}: ${name}' for name in values)
            self.run(f'CREATE (:{label} {{ID: $ID, {body}}})',
                     {'ID': identifier, **{name: self.coerce(name, value) for name, value in values.items()}})
        for source_label, source, relationship, target_label, target, values in edges:
            name = self.relationship(source_label, relationship, target_label)
            body = ' {' + ', '.join(f'{key}: ${key}' for key in values) + '}' if values else ''
            self.run(f'MATCH (s:{source_label} {{ID: $source}}), (t:{target_label} {{ID: $target}}) '
                     f'CREATE (s)-[:{name}{body}]->(t)',
                     {'source': source, 'target': target, **{key: self.coerce(key, value) for key, value in values.items()}})
        if multiple:
            self.conn.execute('COMMIT').close()

    def queries(self, person):
        if person not in self.modules:
            module = types.ModuleType('ladybug_queries')
            module.__dict__['__name__'] = 'ladybug_queries'
            text = re.sub(r'(person1?Id["\']\s*:\s*)1131', lambda match: match[1] + str(person), self.source)
            exec(compile(text, 'ladybug_queries', 'exec'), module.__dict__)
            self.modules[person] = module
        return self.modules[person].QUERIES

    def read(self, number, person=1131):
        self.queries(person)[number](self.conn)

    def answer(self, number, person):
        rows = self.check.canonical_rows(self.queries(person)[number](self.conn))
        return [list(row.values()) for row in rows]


def timed(function, *arguments):
    started = time.perf_counter()
    function(*arguments)
    return (time.perf_counter() - started) * 1000


def summary(samples):
    return {'median_ms': statistics.median(samples), 'mean_ms': statistics.mean(samples), 'max_ms': max(samples),
            'count': len(samples)}


def read_round(engine, samples, skip=()):
    for number in READS:
        if number not in skip:
            samples.setdefault(f'ic{number}', []).append(timed(engine.read, number))


def serialize(value):
    if isinstance(value, Decimal):
        return int(value) if value == value.to_integral_value() else float(value)
    return str(value)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--engine', choices=['duckgql', 'ladybug'], required=True)
    parser.add_argument('--work', type=Path, default=OUT / 'mixed')
    parser.add_argument('--write-ops', type=int, default=1000, help='operations timed on their own')
    parser.add_argument('--batch', type=int, default=20, help='writes before each read round')
    parser.add_argument('--rounds', type=int, default=20)
    parser.add_argument('--optimizer', choices=['cost', 'legacy'], default='cost', help='DuckGQL planner')
    parser.add_argument('--skip-stale', type=int, nargs='*', default=[],
                        help='queries not to run without a CSR, recorded as skipped: an internal '
                             'error invalidates the database instance and would end the run')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)

    report = {'engine': args.engine, 'date': datetime.now(timezone.utc).isoformat(), 'config': vars(args).copy(),
              'threads': 4, 'phases': {}}
    report['config']['work'] = str(args.work)
    phases = report['phases']
    total = args.write_ops + (2 * args.rounds + 1) * args.batch
    operations = stream_operations(total)

    started = time.perf_counter()
    engine = DuckGQL(args.work, args.optimizer) if args.engine == 'duckgql' else Ladybug(args.work)
    label = args.engine if args.engine == 'ladybug' else f'duckgql-{args.optimizer}'
    prepare_ms = engine.prepare_for_reads()
    first_ms = timed(engine.read, 8)
    phases['cold_start'] = {'open_ms': engine.open_ms, 'prepare_ms': prepare_ms, 'first_query_ms': first_ms,
                            'open_to_first_answer_ms': engine.open_ms + prepare_ms + first_ms,
                            'note': 'database file already in the OS cache'}
    print('cold start', phases['cold_start'], flush=True)

    baseline = {}
    for _ in range(9):
        read_round(engine, baseline)
    phases['reads_without_writes'] = {name: summary(samples[1:]) for name, samples in baseline.items()}
    print('reads without writes', {k: round(v['median_ms'], 1) for k, v in phases['reads_without_writes'].items()}, flush=True)

    by_kind = {}
    written = 0
    for _, kind, fields in (next(operations) for _ in range(args.write_ops)):
        by_kind.setdefault(kind, []).append(timed(engine.apply, kind, fields))
        written += 1
    every = [sample for samples in by_kind.values() for sample in samples]
    phases['writes'] = {'all': summary(every), **{f'op{kind}': summary(samples) for kind, samples in sorted(by_kind.items())}}
    print('writes', {k: (round(v['median_ms'], 2), v['count']) for k, v in phases['writes'].items()}, flush=True)

    def mixed(phase, after_batch=None):
        reads, extra = {}, []
        batch_ms = []
        for _ in range(args.rounds):
            started_batch = time.perf_counter()
            for _ in range(args.batch):
                _, kind, fields = next(operations)
                engine.apply(kind, fields)
            batch_ms.append((time.perf_counter() - started_batch) * 1000)
            if after_batch:
                extra.append(timed(after_batch))
            read_round(engine, reads)
        phases[phase] = {'batch_writes': summary(batch_ms), 'reads': {name: summary(samples) for name, samples in reads.items()}}
        if extra:
            phases[phase]['after_batch'] = summary(extra)
        print(phase, {k: round(v['median_ms'], 1) for k, v in phases[phase]['reads'].items()},
              'after batch', round(statistics.median(extra), 1) if extra else None, flush=True)

    # With no maintenance, then with what DuckGQL needs to recover its fast path.
    builds_before = engine.delta_state()['build_count'] if args.engine == 'duckgql' else None
    mixed('mixed_no_maintenance')
    if args.engine == 'duckgql':
        phases['mixed_no_maintenance']['delta'] = engine.delta_state()
        phases['mixed_no_maintenance']['csr_builds'] = phases['mixed_no_maintenance']['delta']['build_count'] - builds_before
        print('after no maintenance', phases['mixed_no_maintenance']['delta'], 'builds', phases['mixed_no_maintenance']['csr_builds'], flush=True)
    mixed('mixed_with_maintenance', engine.prepare_for_reads if args.engine == 'duckgql' else None)

    answers = {}
    def collect(state, skip=(), before_each=None):
        for person in ANSWER_PEOPLE:
            for number in ANSWER_QUERIES:
                key = f'{state}/{person}/ic{number}'
                if number in skip:
                    answers[key] = 'SKIPPED'
                    continue
                if before_each:
                    before_each()
                try:
                    answers[key] = engine.answer(number, person)
                except Exception as error:
                    answers[key] = 'ERR ' + str(error)[:200]
    for _ in range(args.batch):
        _, kind, fields = next(operations)
        engine.apply(kind, fields)
    if args.engine == 'duckgql':
        # The inserts above are in a delta over the CSR, until the rebuild.
        phases['delta_before_answers'] = engine.delta_state()
        print('delta', phases['delta_before_answers'], flush=True)
        collect('delta')
        engine.prepare_for_reads()
    collect('fast')
    if args.engine == 'duckgql':
        # A write that changes no rows still leaves the CSR stale, so these answers
        # come from the plans a reader gets between a write and the next rebuild.
        # Repeated per query, because a path search rebuilds it.
        collect('stale', args.skip_stale,
                lambda: engine.db.query('UPDATE gql_data.graph_1_vertices SET length = length WHERE id = -1'))
    report['total_seconds'] = time.perf_counter() - started
    report['applied_operations'] = total
    report['skipped_while_stale'] = args.skip_stale
    (OUT / f'mixed-{label}.json').write_text(json.dumps(report, indent=2) + '\n')
    (OUT / f'mixed-{label}-answers.json').write_text(json.dumps(answers, default=serialize))
    print('total seconds', round(report['total_seconds'], 1), flush=True)


if __name__ == '__main__':
    main()
