#!/usr/bin/env python3
"""Measure upstream Ladybug queries locally with five rounds and golden checks."""
import argparse, gc, hashlib, importlib.metadata, importlib.util, json, platform, re
import statistics, subprocess, sys, time
from datetime import datetime, timezone
from decimal import Decimal
from pathlib import Path

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path); mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod); return mod

def main():
    root=Path(__file__).resolve().parents[2]
    p=argparse.ArgumentParser(); p.add_argument('--upstream',type=Path,default=Path('/Users/rahuliyer/graph-benchmark-ldbc-complex')); p.add_argument('--output',type=Path,default=root/'build/benchmarks/ldbc-complex-sf1'); p.add_argument('--threads',type=int,default=4); p.add_argument('--runs',type=int,default=5); a=p.parse_args()
    import ladybug as lb
    q=module('ladybug_queries',a.upstream/'ladybugdb/query.py'); check=module('golden_checks',a.upstream/'conftest.py'); ingest=module('ladybug_ingest',a.upstream/'ladybugdb/build_graph.py')
    out=a.output; out.mkdir(parents=True,exist_ok=True); path=out/'ladybug-sf1.lbdb'
    report={'date':datetime.now(timezone.utc).isoformat(),'engine':'ladybug','version':importlib.metadata.version('ladybug'),'platform':platform.platform(),'python':sys.version,'upstream_commit':subprocess.check_output(['git','-C',str(a.upstream),'rev-parse','HEAD'],text=True).strip(),'config':{'threads':a.threads,'buffer_pool_bytes':8_000_000_000,'runs':a.runs,'warmups':0,'timeout_seconds':q.QUERY_TIMEOUT_SECONDS,'timer':'perf_counter; upstream query execution and get_as_pl conversion; golden checks excluded','indexes':ingest.SECONDARY_INDEXES,'analyze':True},'queries':{}}
    report['versions']={n:importlib.metadata.version(n) for n in ['ladybug','polars','pyarrow','pytest','pytest-benchmark']}
    report['source_sha256']={str(p.relative_to(a.upstream)):hashlib.file_digest(p.open('rb'),'sha256').hexdigest() for p in [a.upstream/'ladybugdb/query.py',a.upstream/'ladybugdb/etl/schema.cypher',a.upstream/'ladybugdb/etl/copy.cypher']}
    def save(): (out/'ladybug-results.json').write_text(json.dumps(report,indent=2)+'\n')
    save(); exists=path.exists(); db=lb.Database(str(path),buffer_pool_size=8_000_000_000,max_num_threads=a.threads); conn=lb.Connection(db,num_threads=a.threads)
    marker=out/'ladybug-loaded.json'
    if not exists:
        t=time.perf_counter(); print('Ingesting Ladybug SF1',flush=True)
        for statement in (a.upstream/'ladybugdb/etl/schema.cypher').read_text().split(';'):
            if statement.strip(): conn.execute(statement).close()
        csvroot=a.upstream/'social_network-sf1-CsvBasic-StringDateFormatter'
        if not csvroot.exists(): csvroot=a.upstream/'csv'
        copy=(a.upstream/'ladybugdb/etl/copy.cypher').read_text().replace('../csv/',str(csvroot)+'/')
        for line in copy.splitlines():
            if line.strip(): conn.execute(line).close()
        ingest.create_secondary_indexes(conn)
        conn.execute('CHECKPOINT').close()
        marker.write_text(json.dumps({'load_and_index_seconds':time.perf_counter()-t,'database':str(path)},indent=2))
    elif not marker.exists(): raise RuntimeError('Database exists without completed-load marker; select a new output directory.')
    report['load']=json.loads(marker.read_text()); print(report['load'],flush=True)
    schema=(a.upstream/'ladybugdb/etl/schema.cypher').read_text(); counts={}
    for kind,name in re.findall(r'CREATE (NODE|REL) TABLE (\w+)',schema):
        response=conn.execute(f'MATCH (n:{name}) RETURN count(n) AS n' if kind=='NODE' else f'MATCH ()-[r:{name}]->() RETURN count(r) AS n')
        counts[name]={'kind':kind,'count':response.get_next()[0]}; response.close()
    report['counts']=counts; report['total_vertices']=sum(x['count'] for x in counts.values() if x['kind']=='NODE'); report['total_edges']=sum(x['count'] for x in counts.values() if x['kind']=='REL')
    t=time.perf_counter(); conn.execute('ANALYZE').close(); report['analyze_seconds']=time.perf_counter()-t
    conn.set_query_timeout(int(q.QUERY_TIMEOUT_SECONDS*1000)); save(); gc.disable()
    try:
        for n,fn in q.QUERIES.items():
            name=f'ic{n}'; entry={}; report['queries'][name]=entry; times=[]; print(name,flush=True)
            try:
                for _ in range(a.runs):
                    t=time.perf_counter(); result=fn(conn); times.append((time.perf_counter()-t)*1000)
                    actual=check.canonical_rows(result); (out/f'ladybug-{name}-actual.json').write_text(json.dumps(actual,indent=2,default=lambda x: int(x) if isinstance(x,Decimal) and x==x.to_integral_value() else float(x) if isinstance(x,Decimal) else str(x))+'\n'); check.assert_golden(name,result)
                entry.update(status='passed',rows=len(actual),samples_ms=times,mean_ms=statistics.mean(times),median_ms=statistics.median(times),min_ms=min(times),max_ms=max(times))
            except Exception as e: entry.update(status='wrong_answer' if isinstance(e,AssertionError) else 'timeout' if isinstance(e,TimeoutError) else 'error',error=str(e))
            print(entry,flush=True); save()
    finally: gc.enable(); conn.close(); db.close()
if __name__=='__main__': main()
