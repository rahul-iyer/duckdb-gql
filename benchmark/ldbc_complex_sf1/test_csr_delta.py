#!/usr/bin/env python3
"""Inserts reach CSR reads through a delta, with concurrent connections and prepared plans.

Covers what sqllogictest cannot: prepared MATCH statements, appenders, and threads. Run
from the checkout after building the release library; exits non-zero on any failure.
"""
import shutil, sys, tempfile, threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import DB
from test_prepared_reuse import append_vertex, check, connection, failures, Prepared, values

VERTICES = 50000
HUB = 1
WRITES = 150


def stats(db, *columns):
    return tuple(db.query(f"SELECT {', '.join(columns)} FROM gql_csr_stats('delta')")[0].values())


def load(db, directory):
    db.query(f"COPY (SELECT i AS \":ID\", 'A' AS \":LABEL\", i::INTEGER AS \"id:int\" FROM range(1, {VERTICES + 1}) t(i)) "
             f"TO '{directory}/v.csv' (HEADER)")
    db.query(f"COPY (SELECT i AS \":START_ID\", i % {VERTICES} + 1 AS \":END_ID\", 'R' AS \":TYPE\" FROM range(1, {VERTICES + 1}) t(i)) "
             f"TO '{directory}/e.csv' (HEADER)")
    db.query('CREATE GRAPH delta ANY')
    db.query(f"COPY GRAPH delta FROM (VERTICES '{directory}/v.csv', EDGES '{directory}/e.csv') FORMAT GRAPH")
    db.query('SESSION SET GRAPH delta')
    db.query("CALL gql_create_property_index('delta', 'id')")
    db.query("CALL gql_build_csr('delta')")


def check_prepared(db):
    for planner in ('cost', 'legacy'):
        db.query(f"SET gql_optimizer='{planner}'")
        builds = stats(db, 'build_count')
        out = Prepared(db, 'MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id')
        back = Prepared(db, 'MATCH (a:A)<-[:R]-(b) WHERE a.id = 7 RETURN b.id')
        two = Prepared(db, 'MATCH (a:A)-[:R]->(b)-[:R]->(c) WHERE a.id = 7 RETURN c.id')
        check(f'{planner}: plan expands over the CSR',
              'gql_csr_expand' in str(db.query('EXPLAIN MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id')).lower(), True)
        before = len(out.run()), len(back.run()), len(two.run())
        target = 1000 + len(planner) * 100 + before[0]
        db.query(f'MATCH (a:A), (b:A) WHERE a.id = 7 AND b.id = {target} INSERT (a)-[:R]->(b)')
        db.query(f'MATCH (a:A), (b:A) WHERE a.id = {target} AND b.id = 7 INSERT (a)-[:R]->(b)')
        check(f'{planner}: prepared reaches the new neighbor', (target,) in out.run(), True)
        check(f'{planner}: prepared follows against the direction', (target,) in back.run(), True)
        check(f'{planner}: prepared crosses CSR and delta', (7,) in two.run(), True)
        for name, statement in (('out', out), ('in', back), ('two hops', two)):
            statement.agrees(f'{planner}: prepared equals fresh, {name}')
        check(f'{planner}: no rebuild for those reads', stats(db, 'build_count'), builds)


def check_appender(db):
    vertices, delta = stats(db, 'vertex_count', 'delta_vertex_count')
    table = db.query("SELECT table_name FROM duckdb_tables() WHERE schema_name = 'gql_data' AND table_name LIKE '%vertices'")[0]['table_name']
    append_vertex(db, table, VERTICES + 100000, 0)
    check('appender reaches the delta', stats(db, 'vertex_count', 'delta_vertex_count'), (vertices + 1, delta + 1))


def check_concurrency(db):
    db.query("CALL gql_build_csr('delta')")
    errors, stop = [], threading.Event()
    def truth(reader):
        return reader.query(f"SELECT count(*) AS n FROM gql_data.graph_1_edges WHERE __gql_target_id = {HUB}")[0]['n']

    def read(reader):
        reader.query('SESSION SET GRAPH delta')
        latest, steps = 0, 0
        try:
            while not stop.is_set():
                # Both sides of one statement share a lease, so they read one version.
                row = reader.query(f"SELECT (SELECT count(*) FROM gql_neighbors('delta', {HUB}, 'in')) AS a, "
                                   f"(SELECT count(*) FROM gql_neighbors('delta', {HUB}, 'in')) AS b")[0]
                if row['a'] != row['b']:
                    errors.append(f"statement saw two versions: {row}")
                if row['a'] < latest:
                    errors.append(f"went backwards: {latest} then {row['a']}")
                latest = row['a']
                # Every neighbor this read finds has a row it can fetch.
                reader.query(f"MATCH (a:A)<-[:R]-(b) WHERE a.id = {HUB} RETURN b.id")
                steps += 1
        except Exception as error:
            errors.append(f'reader: {error}')
        counts.append(steps)

    def write(writer, base):
        writer.query('SESSION SET GRAPH delta')
        try:
            for index in range(WRITES):
                # GQL writers update one graph_version row, so concurrent ones conflict.
                while True:
                    try:
                        writer.query(f'MATCH (a:A), (b:A) WHERE a.id = {base + index} AND b.id = {HUB} INSERT (a)-[:R]->(b)')
                        break
                    except RuntimeError as error:
                        if 'Conflict' not in str(error):
                            raise
                if index % 10 == 0:
                    writer.query('BEGIN')
                    writer.query(f'INSERT (:A {{id: {-1 - index}}})')
                    writer.query('ROLLBACK')
        except Exception as error:
            errors.append(f'writer: {error}')

    def rebuild(builder):
        try:
            while not stop.is_set():
                builder.query("CALL gql_build_csr('delta')")
        except Exception as error:
            errors.append(f'builder: {error}')

    counts = []
    before = truth(db)
    connections = [connection(db) for _ in range(6)]
    readers = [threading.Thread(target=read, args=(reader,)) for reader in connections[:3]]
    writers = [threading.Thread(target=write, args=(connections[3], 10000)),
               threading.Thread(target=write, args=(connections[4], 20000))]
    builder = threading.Thread(target=rebuild, args=(connections[5],))
    for thread in readers + writers + [builder]:
        thread.start()
    for thread in writers:
        thread.join()
    stop.set()
    builder.join()
    for thread in readers:
        thread.join()
    check('concurrency: no errors or stale reads', errors[:5], [])
    check('concurrency: readers made progress', all(count > 0 for count in counts), True)
    check('concurrency: all writes committed', truth(db), before + 2 * WRITES)
    expected = truth(db)
    for index, other in enumerate(connections[:3]):
        check(f'concurrency: connection {index} ends on the truth',
              other.query(f"SELECT count(*) AS n FROM gql_neighbors('delta', {HUB}, 'in')")[0]['n'], expected)
    # With the writers done, the last CSR has caught up, and answers agree.
    actual = values(db.query(f'MATCH (a:A)<-[:R]-(b) WHERE a.id = {HUB} RETURN b.id'))
    truth_rows = values(db.query(f'SELECT __gql_source_id AS id FROM gql_data.graph_1_edges WHERE __gql_target_id = {HUB}'))
    check('concurrency: fresh MATCH equals the table', actual, truth_rows)


def check_renamed_file(db, directory):
    expected = values(db.query('MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id'))
    db.query('CHECKPOINT')
    db.close()
    shutil.move(f'{directory}/test.duckdb', f'{directory}/renamed.duckdb')
    renamed = DB(f'{directory}/renamed.duckdb', threads=4)
    renamed.query('SESSION SET GRAPH delta')
    check('renamed file: relational read', values(renamed.query('MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id')), expected)
    renamed.query("CALL gql_build_csr('delta')")
    check('renamed file: read over the CSR', values(renamed.query('MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id')), expected)
    renamed.query('MATCH (a:A), (b:A) WHERE a.id = 7 AND b.id = 9 INSERT (a)-[:R]->(b)')
    check('renamed file: write reaches the delta', stats(renamed, 'delta_edge_count'), (1,))
    check('renamed file: write visible', (9,) in values(renamed.query('MATCH (a:A)-[:R]->(b) WHERE a.id = 7 RETURN b.id')), True)
    renamed.close()


def main():
    directory = tempfile.mkdtemp(prefix='gql_csr_delta_')
    db = DB(f'{directory}/test.duckdb', threads=4)
    load(db, directory)
    check_prepared(db)
    check_appender(db)
    check_concurrency(db)
    check_renamed_file(db, directory)
    print('FAILED: ' + ', '.join(failures) if failures else 'all passed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
