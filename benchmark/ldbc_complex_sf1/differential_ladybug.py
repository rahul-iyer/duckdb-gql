import sys,json,re,importlib.util,types
from pathlib import Path
from decimal import Decimal
import ladybug as lb
S=Path(sys.argv[1]); U=Path('/Users/rahuliyer/graph-benchmark-ldbc-complex'); O=Path('/Users/rahuliyer/duckdb-gql/build/benchmarks/ldbc-complex-sf1')
spec=importlib.util.spec_from_file_location('golden',U/'conftest.py'); check=importlib.util.module_from_spec(spec); spec.loader.exec_module(check)
src=(U/'ladybugdb/query.py').read_text()
db=lb.Database(str(O/'ladybug-sf1.lbdb'),buffer_pool_size=8_000_000_000,max_num_threads=4); conn=lb.Connection(db); conn.set_query_timeout(60000)
res={}
for r in json.loads((S/'roots.json').read_text()):
    m=types.ModuleType('q'); m.__dict__['__name__']='q'; exec(compile(re.sub(r'(person1?Id["\']\s*:\s*)1131',lambda x:x[1]+str(r),src),'q','exec'),m.__dict__)
    for n in range(1,15):
        try: res[f'{r}/ic{n}']=[list(x.values()) for x in check.canonical_rows(m.QUERIES[n](conn))]
        except Exception as e: res[f'{r}/ic{n}']='ERR '+str(e)[:200]
    print('root',r,flush=True)
(S/'lb.json').write_text(json.dumps(res,default=lambda x:int(x) if isinstance(x,Decimal) and x==x.to_integral_value() else float(x) if isinstance(x,Decimal) else str(x)))
conn.close(); db.close()
