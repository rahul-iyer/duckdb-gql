#!/usr/bin/env python3
"""Asks each engine the same questions on graphs unlike LDBC, with the same seeds.

An engineering comparison, not a published benchmark. Answers are saved so that
compare.py can check the engines agree, and a timeout or error is a result too.

    python3 benchmark/generalization/run.py --dataset roadnet --engine duckgql
    DUCKGQL_BUILD=/path/to/build python3 benchmark/generalization/run.py --dataset roadnet --engine duckgql --label before
    VENV/bin/python -I benchmark/generalization/run.py --dataset roadnet --engine ladybug
    python3 benchmark/generalization/compare.py
"""
import argparse, csv, json, os, shutil, statistics, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / 'build/benchmarks/generalization'
DATASETS = {
    'air': dict(nodes='data/air-routes/nodes.csv', edges='data/air-routes/edges.csv', label='airport', edge='route', key='id'),
    'roadnet': dict(nodes='data/roadnet-ca/graph-import/nodes.csv', edges='data/roadnet-ca/graph-import/edges.csv',
                    label='intersection', edge='road', key='external_id'),
    'livejournal': dict(nodes='data/livejournal/graph-import/nodes.csv', edges='data/livejournal/graph-import/edges.csv',
                        label='user', edge='follows', key='external_id'),
}


def gql_queries(d):
    a, t, k = d['label'], d['edge'], d['key']
    return {
        'one_hop': f"MATCH (a:{a})-[:{t}]->(b) WHERE a.{k} = $s RETURN count(*)",
        'two_hop_distinct': f"MATCH (a:{a})-[:{t}]->(b)-[:{t}]->(c) WHERE a.{k} = $s RETURN count(DISTINCT c.{k})",
        'path_1_3_distinct': f"MATCH (a:{a})-[:{t}]->{{1,3}}(b) WHERE a.{k} = $s RETURN count(DISTINCT b.{k})",
        'path_1_8_distinct': f"MATCH (a:{a})-[:{t}]->{{1,8}}(b) WHERE a.{k} = $s RETURN count(DISTINCT b.{k})",
        'triangle': f"MATCH (a:{a})-[:{t}]->(b)-[:{t}]->(c)-[:{t}]->(a) WHERE a.{k} = $s RETURN count(DISTINCT c.{k})",
        'optional_chain': f"MATCH (a:{a})-[:{t}]->(b) WHERE a.{k} = $s OPTIONAL MATCH (a)-[:{t}]->(x)-[:{t}]->(b) RETURN count(DISTINCT x.{k})",
        'shortest_length': f"MATCH (a:{a}) WHERE a.{k} = $s MATCH p = ANY SHORTEST (a)-[:{t}]->{{1,30}}(b) WHERE b.{k} = $t RETURN PATH_LENGTH(p)",
        'all_shortest_count': f"MATCH (a:{a}) WHERE a.{k} = $s MATCH (b:{a}) WHERE b.{k} = $t MATCH p = ALL SHORTEST (a)-[:{t}]->{{1,30}}(b) RETURN count(*)",
    }


def cypher_queries(d):
    a, t = d['label'], d['edge']
    return {
        'one_hop': f"MATCH (a:{a} {{id: $s}})-[:{t}]->(b) RETURN count(*)",
        'two_hop_distinct': f"MATCH (a:{a} {{id: $s}})-[:{t}]->(b)-[:{t}]->(c) RETURN count(DISTINCT c.id)",
        'path_1_3_distinct': f"MATCH (a:{a} {{id: $s}})-[:{t}*1..3]->(b) RETURN count(DISTINCT b.id)",
        'path_1_8_distinct': f"MATCH (a:{a} {{id: $s}})-[:{t}*1..8]->(b) RETURN count(DISTINCT b.id)",
        'triangle': f"MATCH (a:{a} {{id: $s}})-[:{t}]->(b)-[:{t}]->(c)-[:{t}]->(a) RETURN count(DISTINCT c.id)",
        'optional_chain': f"MATCH (a:{a} {{id: $s}})-[:{t}]->(b) OPTIONAL MATCH (a)-[:{t}]->(x)-[:{t}]->(b) RETURN count(DISTINCT x.id)",
        'shortest_length': f"MATCH (a:{a} {{id: $s}})-[e:{t}* SHORTEST 1..30]->(b:{a} {{id: $t}}) RETURN length(e)",
        'all_shortest_count': f"MATCH (a:{a} {{id: $s}})-[e:{t}* ALL SHORTEST 1..30]->(b:{a} {{id: $t}}) RETURN count(*)",
    }


def substitute(sql, seed, quote):
    return sql.replace('$s', f"{quote}{seed['source']}{quote}").replace('$t', f"{quote}{seed['target']}{quote}")


def pick_seeds(name, d):
    """Sources across the degree range, each with a target reached by a random walk."""
    path = OUT / f'{name}-seeds.json'
    if path.exists():
        return json.loads(path.read_text())
    import random
    adjacency, degree = {}, {}
    keep = set()
    with (ROOT / d['nodes']).open() as nodes:
        for row in csv.DictReader(nodes):
            if row[':LABEL'] == d['label']:
                keep.add(int(row[next(iter(row))]))
    with (ROOT / d['edges']).open() as edges:
        reader = csv.reader(edges)
        header = next(reader)
        source, target, kind = header.index(next(h for h in header if h.startswith(':START'))), header.index(next(h for h in header if h.startswith(':END'))), header.index(':TYPE')
        for row in reader:
            if row[kind] == d['edge']:
                s, e = int(row[source]), int(row[target])
                if s in keep and e in keep:
                    adjacency.setdefault(s, []).append(e)
    ordered = sorted(adjacency, key=lambda vertex: (len(adjacency[vertex]), vertex))
    rng = random.Random(562818)
    seeds = []
    for fraction in (0.05, 0.25, 0.5, 0.75, 0.95, 0.999):
        source = ordered[min(len(ordered) - 1, int(len(ordered) * fraction))]
        current = source
        for _ in range(rng.randint(3, 12)):
            current = rng.choice(adjacency.get(current) or [source])
        if current == source:
            current = rng.choice(adjacency[source])
        seeds.append({'source': source, 'target': current, 'degree': len(adjacency[source])})
    path.write_text(json.dumps(seeds))
    return seeds


class DuckGQL:
    # COPY GRAPH stores an untyped :ID column as VARCHAR. An integer literal would cast the column and skip its index.
    key_quote = "'"

    def __init__(self, name, d, threads, label):
        sys.path.insert(0, str(ROOT / 'benchmark/ldbc_complex_sf1'))
        from run import DB
        directory = OUT / f'{name}-{label}'
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir(parents=True)
        self.db = DB(directory / 'graph.duckdb', threads, '16GB')
        self.report = {}
        started = time.perf_counter()
        self.db.query('CREATE GRAPH g ANY')
        self.db.query(f"COPY GRAPH g FROM (VERTICES '{ROOT / d['nodes']}', EDGES '{ROOT / d['edges']}') FORMAT GRAPH")
        self.db.query('SESSION SET GRAPH g')
        self.db.query(f"CALL gql_create_property_index('g', '{d['key']}')")
        self.report['load_seconds'] = time.perf_counter() - started
        self.db.query("SET gql_optimizer='cost'")
        started = time.perf_counter()
        row = self.db.query("CALL gql_build_csr('g')")[0]
        self.report['csr_seconds'] = time.perf_counter() - started
        self.report['csr_memory_bytes'] = row['memory_bytes']
        self.queries = gql_queries(d)

    def run(self, sql, timeout):
        return [list(row.values()) for row in self.db.query(sql, timeout)]


class Ladybug:
    key_quote = ''

    def __init__(self, name, d, threads, label):
        import ladybug
        self.conn_module = ladybug
        directory = OUT / f'{name}-ladybug'
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir(parents=True)
        # Only the label and type being asked about, in the form COPY expects.
        nodes, edges = directory / 'nodes.csv', directory / 'edges.csv'
        keep = set()
        with (ROOT / d['nodes']).open() as source, nodes.open('w') as sink:
            sink.write('id\n')
            for row in csv.DictReader(source):
                if row[':LABEL'] == d['label']:
                    keep.add(key := row[next(iter(row))])
                    sink.write(key + '\n')
        with (ROOT / d['edges']).open() as source, edges.open('w') as sink:
            reader = csv.reader(source)
            header = next(reader)
            a, b, kind = header.index(next(h for h in header if h.startswith(':START'))), header.index(next(h for h in header if h.startswith(':END'))), header.index(':TYPE')
            sink.write('from,to\n')
            for row in reader:
                if row[kind] == d['edge'] and row[a] in keep and row[b] in keep:
                    sink.write(f'{row[a]},{row[b]}\n')
        self.db = ladybug.Database(str(directory / 'graph.lbdb'), buffer_pool_size=16_000_000_000, max_num_threads=threads)
        self.conn = ladybug.Connection(self.db)
        started = time.perf_counter()
        self.conn.execute(f"CREATE NODE TABLE {d['label']}(id INT64 PRIMARY KEY)")
        self.conn.execute(f"CREATE REL TABLE {d['edge']}(FROM {d['label']} TO {d['label']})")
        self.conn.execute(f"COPY {d['label']} FROM '{nodes}' (HEADER=true)")
        self.conn.execute(f"COPY {d['edge']} FROM '{edges}' (HEADER=true)")
        self.report = {'load_seconds': time.perf_counter() - started, 'version': ladybug.__version__}
        self.queries = cypher_queries(d)

    def run(self, sql, timeout):
        self.conn.set_query_timeout(int(timeout * 1000))
        result = self.conn.execute(sql)
        rows = []
        while result.has_next():
            rows.append(list(result.get_next()))
        return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dataset', choices=DATASETS, required=True)
    parser.add_argument('--engine', choices=['duckgql', 'ladybug'], required=True)
    parser.add_argument('--label', default=None, help='names this build in the results, such as before')
    parser.add_argument('--threads', type=int, default=4)
    parser.add_argument('--runs', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=60)
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    label = args.label or args.engine
    d = DATASETS[args.dataset]
    seeds = pick_seeds(args.dataset, d)
    engine = (DuckGQL if args.engine == 'duckgql' else Ladybug)(args.dataset, d, args.threads, label)
    print('loaded', engine.report, flush=True)
    results = {}
    for query, template in engine.queries.items():
        for index, seed in enumerate(seeds):
            sql, samples, answer, status = substitute(template, seed, engine.key_quote), [], None, 'ok'
            for _ in range(args.runs):
                started = time.perf_counter()
                try:
                    answer = engine.run(sql, args.timeout)
                except Exception as error:
                    text = str(error)
                    status = 'timeout' if 'nterrupt' in text or 'timeout' in text.lower() else 'error: ' + text[:200]
                    break
                samples.append((time.perf_counter() - started) * 1000)
            results[f'{query}/{index}'] = {'status': status, 'ms': statistics.median(samples) if samples else None,
                                           'answer': answer, 'degree': seed['degree']}
            print(query, index, 'degree', seed['degree'], status[:90], round(statistics.median(samples), 1) if samples else '-', str(answer)[:50], flush=True)
    (OUT / f'{args.dataset}-{label}.json').write_text(json.dumps({'engine': engine.report, 'seeds': seeds, 'results': results}, indent=1, default=str))


if __name__ == '__main__':
    main()
