#!/usr/bin/env python3
"""Prepared MATCH plans must never outlive what they depend on.

SQL PREPARE does not accept GQL, so sqllogictest cannot reach this path. Each check
compares a statement prepared before a change with a fresh query after it. Run from
the checkout after building the release library; exits non-zero on any failure.
"""
import copy, ctypes as C, sys, tempfile, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import DB

failures = []


def check(name, actual, expected):
    passed = actual == expected
    print(f"{'ok  ' if passed else 'FAIL'} {name}" + ('' if passed else f': got {actual}, expected {expected}'), flush=True)
    if not passed:
        failures.append(name)


def values(rows):
    return sorted(tuple(row.values()) for row in rows)


def connection(db):
    other = copy.copy(db)
    other.conn = C.c_void_p()
    assert db.lib.duckdb_connect(db.db, C.byref(other.conn)) == 0
    return other


class Prepared:
    def __init__(self, db, sql):
        self.db, self.sql, self.statement = db, sql, db.prepare(sql)

    def run(self):
        return values(self.db.query(prepared=self.statement))

    def fresh(self):
        return values(self.db.query(self.sql))

    def agrees(self, name):
        actual = self.run()
        check(name, actual, self.fresh())
        return actual


def load_graph(db, directory, name, rows, edges):
    db.query(f"COPY (SELECT * FROM (VALUES {rows}) v(\":ID\",\":LABEL\",\"id:int\",\"score:int\",\"tag:string\")) "
             f"TO '{directory}/{name}_v.csv' (HEADER)")
    db.query(f"COPY (SELECT * FROM (VALUES {edges}) e(\":START_ID\",\":END_ID\",\":TYPE\")) "
             f"TO '{directory}/{name}_e.csv' (HEADER)")
    db.query(f"CREATE GRAPH {name} ANY")
    db.query(f"COPY GRAPH {name} FROM (VERTICES '{directory}/{name}_v.csv', EDGES '{directory}/{name}_e.csv') FORMAT GRAPH")


def append_vertex(db, table, vertex_id, score):
    """Writes through the appender on a connection that never binds a statement."""
    lib = db.lib
    p = C.c_void_p
    lib.duckdb_appender_create.argtypes = [p, C.c_char_p, C.c_char_p, C.POINTER(p)]
    lib.duckdb_appender_flush.argtypes = lib.duckdb_appender_close.argtypes = lib.duckdb_appender_end_row.argtypes = [p]
    lib.duckdb_appender_destroy.argtypes = [C.POINTER(p)]
    lib.duckdb_append_null.argtypes = [p]
    lib.duckdb_append_uint64.argtypes = [p, C.c_uint64]
    lib.duckdb_append_int32.argtypes = [p, C.c_int32]
    columns = [row['name'] for row in db.query(f"SELECT name FROM pragma_table_info('gql_data.{table}') ORDER BY cid")]
    writer = connection(db)
    appender = p()
    assert lib.duckdb_appender_create(writer.conn, b'gql_data', table.encode(), C.byref(appender)) == 0
    for column in columns:
        if column == '__gql_id':
            lib.duckdb_append_uint64(appender, vertex_id)
        elif column == 'score':
            lib.duckdb_append_int32(appender, score)
        else:
            lib.duckdb_append_null(appender)
    assert lib.duckdb_appender_end_row(appender) == 0
    assert lib.duckdb_appender_flush(appender) == 0
    lib.duckdb_appender_close(appender)
    lib.duckdb_appender_destroy(C.byref(appender))
    lib.duckdb_disconnect(C.byref(writer.conn))


class Parameterized(Prepared):
    def __call__(self, **values):
        for name, value in values.items():
            self.db.bind(self.statement, name, value)
        return self.run()


def rejected(db, sql, expected):
    try:
        db.prepare(sql)
    except RuntimeError as error:
        check(f'rejected: {sql}', expected in str(error), True)
    else:
        check(f'rejected: {sql}', 'accepted', 'error')


def check_parameters(db, directory):
    # 3,000 vertices share one indexed value, more than one output chunk holds.
    db.query(f"COPY (SELECT i AS \":ID\", CASE WHEN i % 2 = 0 THEN 'P' ELSE 'Q' END AS \":LABEL\", i AS \"id:int\", "
             f"CASE WHEN i <= 3000 THEN 7 ELSE i END AS \"bucket:int\", 'n' || i AS \"name:string\", "
             f"i * 10 AS \"stamp:int\" "
             f"FROM range(1,4001) t(i)) TO '{directory}/params_v.csv' (HEADER)")
    db.query(f"COPY (SELECT i AS \":START_ID\", i+1 AS \":END_ID\", 'LINKS' AS \":TYPE\" FROM range(1,4000) t(i)) "
             f"TO '{directory}/params_e.csv' (HEADER)")
    db.query('CREATE GRAPH params ANY')
    db.query(f"COPY GRAPH params FROM (VERTICES '{directory}/params_v.csv', EDGES '{directory}/params_e.csv') FORMAT GRAPH")
    db.query('SESSION SET GRAPH params')
    for column in ('id', 'bucket'):
        db.query(f"CALL gql_create_property_index('params', '{column}')")
    db.query("CALL gql_build_csr('params')")
    for optimizer in ('cost', 'legacy'):
        db.query(f"SET gql_optimizer='{optimizer}'")
        tag = f'[{optimizer}] '
        by_id = Parameterized(db, "MATCH (a) WHERE a.id = $wanted RETURN a.name")
        check(tag + 'one plan, first value', by_id(wanted=5), [('n5',)])
        check(tag + 'one plan, second value', by_id(wanted=6), [('n6',)])
        check(tag + 'no matching vertex', by_id(wanted=999999), [])
        check(tag + 'NULL matches nothing', by_id(wanted=None), [])
        check(tag + 'after NULL', by_id(wanted=7), [('n7',)])
        labeled = Parameterized(db, "MATCH (a:P) WHERE a.id = $wanted RETURN a.name")
        check(tag + 'label still applies to a lookup', labeled(wanted=5), [])
        check(tag + 'label accepts its vertex', labeled(wanted=6), [('n6',)])
        many = Parameterized(db, "MATCH (a) WHERE a.bucket = $bucket RETURN count(*), count(DISTINCT a.id)")
        check(tag + 'lookup spanning several chunks', many(bucket=7), [(3000, 3000)])
        check(tag + 'rebound to a unique value', many(bucket=3500), [(1, 1)])
        hop = Parameterized(db, "MATCH (a)-[:LINKS]->(b:Q) WHERE a.id = $wanted RETURN b.id")
        check(tag + 'lookup seeds an expansion', hop(wanted=2), [(3,)])
        check(tag + 'seed with filtered neighbor', hop(wanted=3), [])
        ranged = Parameterized(db, "MATCH (a) WHERE a.id > $low AND a.id <= $high RETURN count(*)")
        check(tag + 'range bounds', ranged(low=10, high=20), [(10,)])
        check(tag + 'range rebound', ranged(low=0, high=4000), [(4000,)])
        unindexed = Parameterized(db, "MATCH (a) WHERE a.name = $name RETURN a.id")
        check(tag + 'unindexed property', unindexed(name='n42'), [(42,)])
        stamped = Parameterized(db, "MATCH (a) WHERE a.id = $wanted AND a.stamp < $before RETURN a.id")
        check(tag + 'second predicate passes', stamped(wanted=1, before=100), [(1,)])
        check(tag + 'second predicate rejects', stamped(wanted=1, before=5), [])
        optional = Parameterized(db, "MATCH (a) WHERE a.id = $wanted OPTIONAL MATCH (a)-[:LINKS]->(b) RETURN a.id, b.id")
        check(tag + 'OPTIONAL after lookup', optional(wanted=1), [(1, 2)])
        check(tag + 'OPTIONAL null-extends', optional(wanted=4000), [(4000, None)])
        positional = Parameterized(db, "MATCH (a) WHERE a.id = $1 AND a.bucket = $2 RETURN a.id")
        check(tag + 'positional parameters', positional(**{'1': 3, '2': 7}), [(3,)])
        reused = Parameterized(db, "MATCH (a) WHERE a.id = $x OR a.id = $x + 1 RETURN a.id")
        check(tag + 'one parameter, several uses', reused(x=10), [(10,), (11,)])
        two_types = Parameterized(db, "MATCH (a) WHERE a.id = $v OR a.name = $v RETURN a.id")
        # An integer converts the string column, exactly as the same literal does.
        for shape, value in (("MATCH (a) WHERE a.id = 9 OR a.name = 9 RETURN a.id", 9),):
            try:
                db.query(shape)
                literal_outcome = 'no error'
            except RuntimeError as error:
                literal_outcome = str(error).split(':')[0]
            try:
                two_types(v=value)
                parameter_outcome = 'no error'
            except RuntimeError as error:
                parameter_outcome = str(error).split(':')[0]
            check(tag + 'one parameter at two types behaves like literals', parameter_outcome, literal_outcome)
        check(tag + 'one parameter at two types, string', two_types(v='9'), [(9,)])
        hidden = Parameterized(db, "MATCH (a) WHERE a.id = $v AND CAST($v AS STRING) = a.name RETURN a.id")
        check(tag + 'second use under a cast finds nothing', hidden(v=9), [])
        casted = Parameterized(db, "MATCH (a) WHERE a.id = $v AND CAST($v AS STRING) = '9' RETURN a.id")
        check(tag + 'second use under a cast keeps its value', casted(v=9), [(9,)])
        check(tag + 'second use under a cast rebound', casted(v=10), [])
        twice = Parameterized(db, "MATCH (a) WHERE a.id = $v AND a.stamp = $w AND a.bucket = $v RETURN a.id")
        check(tag + 'repeated parameter, generic plan', twice(v=3500, w=35000), [(3500,)])
        check(tag + 'repeated parameter, rebound', twice(v=3501, w=35000), [])
        projected = Parameterized(db, "MATCH (a) WHERE a.id = 1 RETURN $shown")
        check(tag + 'parameter as a result', projected(shown='hello'), [('hello',)])
        check(tag + 'parameter as a result, other type', projected(shown=5), [(5,)])

    db.query("SET gql_optimizer='cost'")
    by_id = Parameterized(db, "MATCH (a) WHERE a.id = $wanted RETURN a.name")
    check('before writes', by_id(wanted=5), [('n5',)])
    db.query("UPDATE gql_data.graph_3_vertices SET id = 9005 WHERE id = 5")
    check('updated value leaves its old key', by_id(wanted=5), [])
    check('updated value joins its new key', by_id(wanted=9005), [('n5',)])
    db.query("DELETE FROM gql_data.graph_3_vertices WHERE id = 6")
    check('deleted vertex disappears', by_id(wanted=6), [])
    db.query("INSERT (:P {id: 7000, name: 'added'})")
    check('inserted vertex appears', by_id(wanted=7000), [('added',)])
    db.query('CHECKPOINT')
    check('after checkpoint', by_id(wanted=7000), [('added',)])
    db.query('BEGIN')
    db.query("INSERT (:P {id: 7001, name: 'pending'})")
    check('own uncommitted write is found', by_id(wanted=7001), [('pending',)])
    db.query('ROLLBACK')
    check('rolled back write is gone', by_id(wanted=7001), [])

    unbound = db.prepare("MATCH (a) WHERE a.id = $never RETURN a.id")
    try:
        db.query(prepared=unbound)
        check('missing value is an error', 'no error', 'error')
    except RuntimeError as error:
        check('missing value is an error', 'never' in str(error) or 'parameter' in str(error).lower(), True)
    try:
        db.bind(unbound, 'other', 1)
        check('unknown parameter name is an error', 'no error', 'error')
    except (KeyError, RuntimeError):
        check('unknown parameter name is an error', True, True)
    try:
        db.query("MATCH (a) WHERE a.id = $direct RETURN a.id")
        check('unprepared parameter is an error', 'no error', 'error')
    except RuntimeError as error:
        check('unprepared parameter is an error', bool(str(error)), True)
    rejected(db, "MATCH (a) WHERE a.id = $1 AND a.bucket = $named RETURN a.id", 'Mixing positional and named')
    rejected(db, "INSERT (:P {id: $p})", 'not implemented')
    rejected(db, "MATCH (a) WHERE a.id = 1 SET a.name = $p", 'only supported')
    db.query('SESSION SET GRAPH graph_a')
    db.query('DROP GRAPH params')


def main():
    directory = tempfile.mkdtemp(prefix='gql_prepared_reuse_')
    db = DB(f'{directory}/test.duckdb', threads=2)
    load_graph(db, directory, 'graph_a',
               "(1,'A',1,10,'x'),(2,'A',2,20,'y'),(3,'B',3,30,'z')", "(1,2,'R'),(2,3,'R')")
    load_graph(db, directory, 'graph_b', "(1,'A',1,500,'other')", "(1,1,'R')")
    db.query('SESSION SET GRAPH graph_a')
    db.query("CALL gql_create_property_index('graph_a', 'id')")
    db.query("CALL gql_build_csr('graph_a')")
    db.query("SET gql_optimizer='cost'")

    # Statistics can prove this filter empty, and the index fixes a row in the plan.
    pruned = Prepared(db, "MATCH (a:A) WHERE a.score > 100 RETURN a.score")
    pruned_any = Prepared(db, "MATCH (a) WHERE a.score > 100 RETURN a.score")
    indexed = Prepared(db, "MATCH (a:A) WHERE a.id = 2 RETURN a.score")
    chain = Prepared(db, "MATCH (a:A)-[:R]->(b) WHERE a.id = 1 RETURN b.score")
    counted = Prepared(db, "MATCH (a:A) RETURN count(*)")
    check('prepared result before any change', pruned.run(), [])
    check('stable across repeated executions', [indexed.run() for _ in range(5)], [[(20,)]] * 5)

    db.query("INSERT (:A {id: 4, score: 200})")
    check('insert defeats a statistics-pruned filter', pruned.agrees('insert: prepared equals fresh'), [(200,)])

    db.query("UPDATE gql_data.graph_1_vertices SET score = 21 WHERE id = 2")
    check('update reaches an index lookup', indexed.agrees('update: prepared equals fresh'), [(21,)])

    db.query("DELETE FROM gql_data.graph_1_vertices WHERE id = 4")
    check('delete is visible', pruned.agrees('delete: prepared equals fresh'), [])

    db.query("CALL gql_build_csr('graph_a')")
    chain.agrees('CSR rebuild: prepared equals fresh')
    db.query("INSERT (:A {id: 5, score: 50})")
    chain.agrees('write invalidating the CSR: prepared equals fresh')
    db.query("CALL gql_build_csr('graph_a')")

    other = connection(db)
    other.query('SESSION SET GRAPH graph_a')
    pruned.run()
    other.query("INSERT (:A {id: 6, score: 300})")
    check('write on another connection', pruned.agrees('other connection: prepared equals fresh'), [(300,)])

    # An uncommitted write is visible only here; a rollback must restore the old answer.
    before = counted.run()
    db.query('BEGIN')
    db.query("INSERT INTO gql_data.graph_1_vertices (__gql_id, __gql_label, id, score) VALUES (9000, ['a'], 7, 400)")
    check('explicit transaction sees its own write', counted.run(), [(before[0][0] + 1,)])
    db.query('ROLLBACK')
    check('rollback restores the answer', counted.run(), before)

    pruned_any.run()
    append_vertex(db, 'graph_1_vertices', 9001, 700)
    check('appender on a connection that never binds', sorted(pruned_any.run()), sorted(pruned_any.fresh()))
    check('appended row is visible', (700,) in pruned_any.run(), True)
    db.query("CALL gql_build_csr('graph_a')")

    db.query("SET gql_optimizer='legacy'")
    chain.agrees('planner change: prepared equals fresh')
    db.query("SET gql_optimizer='cost'")

    second_scores = Prepared(db, "MATCH (a:A) RETURN a.score")
    first_scores = second_scores.run()
    db.query('SESSION SET GRAPH graph_b')
    check('switching graph follows the session', second_scores.run(), [(500,)])
    db.query('SESSION SET GRAPH graph_a')
    check('switching back', second_scores.run(), first_scores)

    # Executions racing a writer never lose committed rows, and end in agreement.
    race_reader = connection(db)
    race_reader.query('SESSION SET GRAPH graph_a')
    race_reader.query("SET gql_optimizer='cost'")
    race_count = Prepared(race_reader, "MATCH (a:A) WHERE a.score >= 1000 RETURN count(*)")
    race_writer = connection(db)
    race_writer.query('SESSION SET GRAPH graph_a')
    done = threading.Event()
    errors = []

    def write():
        try:
            for value in range(40):
                race_writer.query(f"INSERT (:A {{id: {10000 + value}, score: {1000 + value}}})")
                time.sleep(0.002)
        except Exception as error:
            errors.append(str(error))
        done.set()

    seen, regressions = [], 0
    thread = threading.Thread(target=write)
    thread.start()
    while not done.is_set():
        try:
            current = race_count.run()[0][0]
        except RuntimeError as error:
            # A concurrent CSR invalidation may legitimately ask for a rebuild, not a wrong answer.
            errors.append(str(error))
            break
        regressions += bool(seen) and current < seen[-1]
        seen.append(current)
    thread.join()
    check('racing writer raised no errors', errors, [])
    check('committed rows never disappear under a racing writer', regressions, 0)
    check('agreement after the race', race_count.run(), [(40,)])

    check_parameters(db, directory)

    # Metadata changes, down to the graph no longer existing.
    gone = Prepared(other, "MATCH (a:A) RETURN count(*)")
    gone.run()
    db.query('DROP GRAPH graph_b')
    db.query('SESSION SET GRAPH graph_a')
    db.query('DROP GRAPH graph_a')
    try:
        gone.run()
        check('dropped graph is an error', 'no error', 'error')
    except RuntimeError as error:
        check('dropped graph is an error', 'does not exist' in str(error), True)

    db.close()
    print(f"{len(failures)} failure(s)" if failures else 'all passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
