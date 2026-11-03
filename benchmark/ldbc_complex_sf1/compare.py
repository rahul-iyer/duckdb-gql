#!/usr/bin/env python3
"""Compare local engine runs and retain the upstream historical baselines."""
import argparse
import importlib.util
import json
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--upstream',type=Path,default=Path('/Users/rahuliyer/graph-benchmark-ldbc-complex'))
p.add_argument('--results',type=Path,default=Path(__file__).resolve().parents[2]/'build/benchmarks/ldbc-complex-sf1/results.json')
a=p.parse_args()
s=importlib.util.spec_from_file_location('upstream_compare',a.upstream/'results/compare.py')
m=importlib.util.module_from_spec(s); s.loader.exec_module(m)
r=json.loads(a.results.read_text())
lady_path=a.results.with_name('ladybug-results.json')
lady=json.loads(lady_path.read_text()) if lady_path.exists() else None
engines=['neo4j-2026.09.0','kuzu-0.11.3','ladybug-0.22.1','lance-csr-0.5.1-lazy','lance-csr-0.5.1-memory']
saved={e:m.parse_benchmark_file(a.upstream/f'results/{e}.txt')[0] for e in engines}
def cell(entry):
    return f"{entry['mean_ms']:.2f}" if entry and entry['status']=='passed' else entry.get('status','n/a') if entry else 'n/a'
passed=sum(q['status']=='passed' for q in r['queries'].values())
missing=', '.join(k.upper() for k,q in r['queries'].items() if q['status']!='passed')
lines=['# DuckGQL SF1 fixed-fixture comparison','', f'DuckGQL passes {passed} of 14 native GQL adaptations. Unmeasured or failing: {missing}. Passing queries were checked against upstream golden answers after every timed execution.','']
if lady:
    lines += ['## Same-machine measurements','', 'Both engines ran sequentially on this Apple M5 Pro, 24 GB, macOS 26.3.1, with four query threads. DuckGQL used an 8 GB DuckDB memory limit; Ladybug used an 8 GB buffer pool. These limits govern different resources and are not equal total-process memory caps. Values are mean milliseconds; five timed executions per query, no explicit warmups, garbage collection disabled, ten-second query timeout.','', '| Query | DuckGQL local | Ladybug 0.22.1 local | Ladybug / DuckGQL |','| --- | ---: | ---: | ---: |']
    for n in range(1,15):
        k=f'ic{n}'; d=r['queries'][k]; l=lady['queries'].get(k)
        ratio=f"{l['mean_ms']/d['mean_ms']:.2f}x" if d['status']=='passed' and l and l['status']=='passed' else 'n/a'
        lines.append(f'| IC{n} | {cell(d)} | {cell(l)} | {ratio} |')
    lines += ['', 'Ratios above 1 mean lower DuckGQL latency. Coverage is part of the result: missing query adaptations are not included in any speed summary. Ladybug uses the upstream queries without modifications and converts results to Polars inside timing. DuckGQL uses its native GQL adaptations through the C API and converts rows and collections to Python dictionaries inside timing. Golden comparisons and disk writes are outside both timed calls. These engine-specific conversions differ. Index construction, loading, DuckGQL CSR preparation, and Ladybug ANALYZE are outside query timing.','',f"Ladybug: {lady.get('total_vertices',0):,} vertices and {lady.get('total_edges',0):,} directed relationships before undirected query traversal. Indexes follow upstream build_graph.py. Its buffer pool and ANALYZE are shared across all 14 queries in one process. Versions, source hashes, resource settings and all sample timings are in ladybug-results.json.",'']
lines += ['## Historical upstream runs','', 'These saved runs used an Apple M5, 24 GB, macOS 26.6.2 and are not same-machine speedup measurements. Only Ladybug has also been measured locally when shown above. Upstream uses pytest-benchmark calibration and varying round counts.','', '| Query | DuckGQL local | '+' | '.join(engines)+' |','| --- | ---: |'+' ---: |'*len(engines)]
for n in range(1,15):
    k=f'ic{n}'
    lines.append(f'| IC{n} | {cell(r["queries"][k])} | '+' | '.join(f'{saved[e][k]:.2f}' if k in saved[e] else 'n/a' for e in engines)+' |')
lines += ['', '## DuckGQL run details','', f"Graph: {r['load']['counts']['vertices']:,} vertices and {r['load']['counts']['edges']:,} directed edges. Labels and edge types map to the existing DuckGQL SNB schema; Post and Comment share a Message label. Friendship edges are stored in both directions for directed GQL traversals. Other imported relations retain their direction. Property indexes: id, name, firstName, lastName, birthday, length. One database connection and one full CSR build before querying; later queries reuse the snapshot.", '', 'Adaptations are versioned in benchmark/ldbc_complex_sf1/queries with upstream SF1 parameters; nine originate in the SNB harness and three extend coverage using the pinned froGQL corpus as a formulation reference. IC5 uses strict joinDate > minDate; message content uses content before imageFile. IC13 calls the native shortest-path algorithm. Result columns map positionally to golden column names, then every row is compared in order; string collections are sorted as in the upstream oracle. Every passing fixture is nonempty. No SQL query substitutes are timed.', '', f"DuckGQL checkout: `{r['duckgql_commit']}`. Existing working-tree modifications, binary and extension hashes, dataset hash, configuration, sample timings and upstream commit are recorded in results.json. Incremental CMake build confirmed targets were current. This is an engineering comparison, not an official LDBC benchmark."]
out=a.results.with_name('comparison.md'); out.write_text('\n'.join(lines)+'\n'); print(out)
