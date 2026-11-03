#!/usr/bin/env python3
"""Measures latency and peak memory of each query in its own process, on DuckGQL and Ladybug.

Peak memory is the process's peak resident set from the OS, which is comparable across
engines but includes the Python interpreter, the engine's buffer pool and any CSR. "Ready"
is that peak after opening the database and preparing to read, before the query runs.

    python3 benchmark/generalization/memory.py --ladybug-python VENV/bin/python
"""
import argparse, importlib.util, json, resource, statistics, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/benchmarks/generalization'
SF1 = ROOT / 'build/benchmarks/ldbc-complex-sf1'
UPSTREAM = Path('/Users/rahuliyer/graph-benchmark-ldbc-complex')
GRAPHS = ['air', 'roadnet', 'livejournal']
THREADS = 4


def peak_bytes():
    # Bytes on macOS, kilobytes on Linux.
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return peak if sys.platform == 'darwin' else peak * 1024


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def timed(function, runs):
    samples, status = [], 'ok'
    for _ in range(runs):
        started = time.perf_counter()
        try:
            function()
        except Exception as error:
            text = str(error)
            status = 'timeout' if 'nterrupt' in text or 'timeout' in text.lower() else 'error: ' + text[:120]
            break
        samples.append((time.perf_counter() - started) * 1000)
    return samples, status


def duckgql_child(dataset, query, runs, timeout):
    sys.path.insert(0, str(ROOT / 'benchmark/ldbc_complex_sf1'))
    from run import DB, PARAMETERS, parameterized
    if dataset == 'sf1':
        db = DB(SF1 / 'sf1.duckdb', THREADS, '8GB')
        db.query('SESSION SET GRAPH snb_interactive')
        db.query("SET gql_optimizer='cost'")
        db.query("CALL gql_build_csr('snb_interactive')")
        ready = peak_bytes()
        number = int(query[2:])
        text = (ROOT / f'benchmark/ldbc_complex_sf1/queries/ic{number}.gql').read_text()
        statement = db.prepare(parameterized(number, text))
        for _, name, value in PARAMETERS[number]:
            db.bind(statement, name, value)
        samples, status = timed(lambda: db.query(prepared=statement), runs)
        return ready, peak_bytes(), samples, status
    gen = load_module('generalization_run', HERE / 'run.py')
    d = gen.DATASETS[dataset]
    seeds = json.loads((OUT / f'{dataset}-seeds.json').read_text())
    db = DB(OUT / f'{dataset}-duckgql/graph.duckdb', THREADS, '16GB')
    db.query('SESSION SET GRAPH g')
    db.query("SET gql_optimizer='cost'")
    db.query("CALL gql_build_csr('g')")
    ready = peak_bytes()
    template = gen.gql_queries(d)[query]
    medians, status = [], 'ok'
    for seed in seeds:
        sql = gen.substitute(template, seed, "'")
        samples, seed_status = timed(lambda: db.query(sql, timeout), runs)
        if samples:
            medians.append(statistics.median(samples))
        status = status if seed_status == 'ok' else seed_status
    return ready, peak_bytes(), medians, status


def ladybug_child(dataset, query, runs, timeout):
    import ladybug
    if dataset == 'sf1':
        db = ladybug.Database(str(SF1 / 'ladybug-sf1.lbdb'), buffer_pool_size=8_000_000_000, max_num_threads=THREADS)
        conn = ladybug.Connection(db)
        conn.execute('ANALYZE').close()
        queries = load_module('ladybug_queries', UPSTREAM / 'ladybugdb/query.py')
        ready = peak_bytes()
        function = queries.QUERIES[int(query[2:])]
        samples, status = timed(lambda: function(conn), runs)
        return ready, peak_bytes(), samples, status
    gen = load_module('generalization_run', HERE / 'run.py')
    d = gen.DATASETS[dataset]
    seeds = json.loads((OUT / f'{dataset}-seeds.json').read_text())
    db = ladybug.Database(str(OUT / f'{dataset}-ladybug/graph.lbdb'), buffer_pool_size=16_000_000_000, max_num_threads=THREADS)
    conn = ladybug.Connection(db)
    conn.set_query_timeout(int(timeout * 1000))
    ready = peak_bytes()
    template = gen.cypher_queries(d)[query]

    def run(sql):
        result = conn.execute(sql)
        while result.has_next():
            result.get_next()

    medians, status = [], 'ok'
    for seed in seeds:
        sql = gen.substitute(template, seed, '')
        samples, seed_status = timed(lambda: run(sql), runs)
        if samples:
            medians.append(statistics.median(samples))
        status = status if seed_status == 'ok' else seed_status
    return ready, peak_bytes(), medians, status


def child(args):
    function = duckgql_child if args.engine == 'duckgql' else ladybug_child
    ready, peak, samples, status = function(args.dataset, args.query, args.runs, args.timeout)
    print('RESULT ' + json.dumps({'ready_bytes': ready, 'peak_bytes': peak, 'status': status,
                                  'median_ms': statistics.median(samples) if samples else None,
                                  'max_ms': max(samples) if samples else None, 'finished': len(samples)}))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ladybug-python', default=str(UPSTREAM / '.venv/bin/python'))
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--timeout', type=float, default=15)
    parser.add_argument('--child', action='store_true')
    parser.add_argument('--engine')
    parser.add_argument('--dataset')
    parser.add_argument('--query')
    args = parser.parse_args()
    if args.child:
        return child(args)
    gen = load_module('generalization_run', HERE / 'run.py')
    names = list(gen.gql_queries(gen.DATASETS['air']))
    cases = [('sf1', f'ic{number}') for number in range(1, 15)] + [(graph, name) for graph in GRAPHS for name in names]
    report = {}
    for dataset, query in cases:
        for engine in ('duckgql', 'ladybug'):
            python = [sys.executable] if engine == 'duckgql' else [args.ladybug_python, '-I']
            command = python + [__file__, '--child', '--engine', engine, '--dataset', dataset, '--query', query,
                                '--runs', str(args.runs), '--timeout', str(args.timeout)]
            try:
                done = subprocess.run(command, capture_output=True, text=True, timeout=600)
                lines = [line for line in done.stdout.splitlines() if line.startswith('RESULT ')]
                entry = json.loads(lines[-1][7:]) if lines else {'status': 'crashed: ' + done.stderr[-160:]}
            except subprocess.TimeoutExpired:
                entry = {'status': 'process timeout'}
            report[f'{dataset}/{query}/{engine}'] = entry
            print(dataset, query, engine, {k: (round(v / 1e6) if k.endswith('bytes') else round(v, 1) if isinstance(v, float) else v) for k, v in entry.items()}, flush=True)
            (OUT / 'memory.json').write_text(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
