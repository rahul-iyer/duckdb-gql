import sys,json,time,ctypes as C
from pathlib import Path
ROOT=Path('/Users/rahuliyer/duckdb-gql'); sys.path.insert(0,str(ROOT/'benchmark/ldbc_complex_sf1'))
from run import DB, PARAMETERS, parameterized
S=Path(sys.argv[1]); O=ROOT/'build/benchmarks/ldbc-complex-sf1'
db=DB(O/'sf1.duckdb',4)
d=db.query("""WITH deg AS (SELECT __gql_source_id s,count(*) c FROM gql_data.graph_1_edges WHERE __gql_type='knows' GROUP BY 1),
 r AS (SELECT v.id,d.c,row_number() OVER (ORDER BY d.c DESC,v.id) rn,count(*) OVER () n FROM deg d JOIN gql_data.graph_1_vertices v ON v.__gql_id=d.s)
 SELECT id,c,rn,n FROM r WHERE rn IN (1,2,greatest(1,n//10),n//4,n//2,3*n//4,9*n//10,n)""")
roots=[int(x['id']) for x in d]
ex=db.query("SELECT id FROM gql_data.graph_1_vertices WHERE id IN (7725,19791209305691) AND list_contains(__gql_label,'person')")
roots+= [int(x['id']) for x in ex if int(x['id']) not in roots]
print('roots',[(x['id'],x['c']) for x in d],'extra',ex,flush=True)
(S/'roots.json').write_text(json.dumps(roots))
db.query('SESSION SET GRAPH snb_interactive'); db.query("CALL gql_build_csr('snb_interactive')")
res={}
for mode in ['cost','legacy']:
    db.query(f"SET gql_optimizer='{mode}'")
    for r in roots:
        for n in range(2,14):
            q=(ROOT/f'benchmark/ldbc_complex_sf1/queries/ic{n}.gql').read_text().replace('1131',str(r))
            k=f'{mode}/{r}/ic{n}'
            try: res[k]=[list(x.values()) for x in db.query(q,20)]
            except Exception as e: res[k]='ERR '+str(e)[:200]
    print(mode,'done',flush=True)
# One generic plan per query, rebound to each person, must match every literal answer.
for mode in ['cost','legacy']:
    db.query(f"SET gql_optimizer='{mode}'")
    for n in range(2,14):
        statement=db.prepare(parameterized(n,(ROOT/f'benchmark/ldbc_complex_sf1/queries/ic{n}.gql').read_text()))
        for _,name,value in PARAMETERS[n]: db.bind(statement,name,value)
        person=PARAMETERS[n][0][1]
        for r in roots:
            db.bind(statement,person,r)
            k=f'{mode}-param/{r}/ic{n}'
            try: res[k]=[list(x.values()) for x in db.query(prepared=statement,timeout=20)]
            except Exception as e: res[k]='ERR '+str(e)[:200]
        db.lib.duckdb_destroy_prepare(C.byref(statement))
    print(mode,'parameterized done',flush=True)
(S/'duck.json').write_text(json.dumps(res))
