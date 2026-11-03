#!/usr/bin/env python3
"""Capture native DuckDB plans and profiles for the measured GQL queries."""
import json
from pathlib import Path
from run import DB,lit
root=Path(__file__).resolve().parents[2]; out=root/'build/benchmarks/ldbc-complex-sf1'; profiles=out/'profiles'; profiles.mkdir(exist_ok=True)
db=DB(out/'sf1.duckdb'); db.query('SESSION SET GRAPH snb_interactive'); db.query("CALL gql_build_csr('snb_interactive')")
for n in [2,3,4,5,6,8,9,11,13]:
    q=(out/f'ic{n}.gql').read_text()
    plan=db.query('EXPLAIN '+q)
    (profiles/f'ic{n}-plan.txt').write_text('\n'.join(str(v) for row in plan for v in row.values()))
    db.query("PRAGMA enable_profiling='json'"); db.query(f'SET profiling_output={lit(profiles/f"ic{n}.json")}'); db.query("SET profiling_mode='detailed'")
    db.query(q); db.query('PRAGMA disable_profiling')
    result=json.loads((profiles/f'ic{n}.json').read_text())
    query=result.get('query',result)
    print('IC',n,'latency',query.get('total_time'),'cpu',query.get('cpu_time'),'scanned',query.get('total_rows_scanned'),'cardinality',query.get('total_intermediate_rows'),flush=True)
    def visit(node):
        yield node
        for c in node.get('children',[]): yield from visit(c)
    nodes=[node for tree in result.get('operator',[]) for node in visit(tree)]
    for x in sorted(nodes,key=lambda x:x.get('timing',0),reverse=True)[:7]:
        print(x.get('type'),x.get('timing'),x.get('intermediate_rows'),x.get('extra_info'))
db.close()
