#!/usr/bin/env python3
"""SF1 fixed-fixture comparison; not an official LDBC benchmark.

Uses the local DuckDB C API with DuckGQL loaded. No SQL query substitute is
included in DuckGQL timings. Native query adaptations are versioned in queries/.
"""
import argparse, ast, ctypes as C, gc, hashlib, importlib.util, json, os, platform, re
import statistics, subprocess, sys, threading, time
from datetime import datetime, timezone, timedelta
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

def lit(x): return "'" + str(x).replace("'", "''") + "'"
def git(path, *args): return subprocess.check_output(['git','-C',str(path),*args],text=True).strip()

# Literal text in queries/icN.gql, its parameter name, and the upstream value bound to it.
# IC10's birthday window has no parameter: its month is built into two literal comparisons.
PARAMETERS={
    1:[('1131','personId',1131),("'Jose'",'firstName','Jose')],
    2:[('1131','personId',1131),("'2010-10-16 12:00:00'",'maxDate',datetime(2010,10,16,12))],
    3:[('1131','personId',1131),("'Panama'",'countryX','Panama'),("'South_Africa'",'countryY','South_Africa'),
       ("'2010-06-01 12:00:00'",'startDate',datetime(2010,6,1,12)),("'2010-06-29 12:00:00'",'endDate',datetime(2010,6,29,12))],
    4:[('1131','personId',1131),("'2010-06-01 00:00:00'",'startDate',datetime(2010,6,1)),("'2010-06-30 00:00:00'",'endDate',datetime(2010,6,30))],
    5:[('1131','personId',1131),("'2010-11-01 12:00:00'",'minDate',datetime(2010,11,1,12))],
    6:[('1131','personId',1131),("'Carl_Gustaf_Emil_Mannerheim'",'tagName','Carl_Gustaf_Emil_Mannerheim')],
    7:[('1131','personId',1131)],
    8:[('1131','personId',1131)],
    9:[('1131','personId',1131),("'2010-11-16 12:00:00'",'maxDate',datetime(2010,11,16,12))],
    10:[('1131','personId',1131)],
    11:[('1131','personId',1131),("'Hungary'",'countryName','Hungary'),('2011','workFromYear',2011)],
    12:[('1131','personId',1131),("'Monarch'",'tagClassName','Monarch')],
    13:[('1131','person1Id',1131),('19791209300775','person2Id',19791209300775)],
    14:[('1131','person1Id',1131),('19791209300775','person2Id',19791209300775)],
}

def parameterized(n, text):
    for literal,name,_ in PARAMETERS[n]:
        assert literal in text, f'ic{n} has no literal {literal}'
        text=text.replace(literal,'$'+name)
    return text

class Timestamp(C.Structure):
    _fields_=[('micros',C.c_int64)]

class Result(C.Structure):
    _fields_=[('column_count',C.c_uint64),('row_count',C.c_uint64),('rows_changed',C.c_uint64),('columns',C.c_void_p),('error',C.c_void_p),('internal',C.c_void_p)]

# DUCKGQL_BUILD selects another build, to compare it on the same queries.
BUILD = Path(os.environ.get('DUCKGQL_BUILD', ROOT/'build/release'))

class DB:
    def __init__(self, path, threads=4, memory='8GB'):
        self.lib = l = C.CDLL(str(BUILD/'src/libduckdb.dylib'))
        def fn(name, args, ret):
            f=getattr(l,name); f.argtypes=args; f.restype=ret; return f
        p=C.c_void_p; pp=C.POINTER(p); u=C.c_uint64
        fn('duckdb_create_config',[pp],C.c_int); fn('duckdb_set_config',[p,C.c_char_p,C.c_char_p],C.c_int)
        fn('duckdb_open_ext',[C.c_char_p,pp,p,pp],C.c_int); fn('duckdb_connect',[p,pp],C.c_int)
        fn('duckdb_destroy_config',[pp],None); fn('duckdb_query',[p,C.c_char_p,p],C.c_int)
        fn('duckdb_result_error',[p],C.c_char_p); fn('duckdb_destroy_result',[p],None)
        fn('duckdb_column_count',[p],u); fn('duckdb_row_count',[p],u)
        fn('duckdb_column_type',[p,u],C.c_int); fn('duckdb_column_name',[p,u],C.c_char_p)
        fn('duckdb_value_is_null',[p,u,u],C.c_bool); fn('duckdb_value_varchar',[p,u,u],p)
        fn('duckdb_result_chunk_count',[Result],u); fn('duckdb_result_get_chunk',[Result,u],p)
        fn('duckdb_data_chunk_get_size',[p],u); fn('duckdb_data_chunk_get_vector',[p,u],p)
        fn('duckdb_destroy_data_chunk',[pp],None)
        fn('duckdb_vector_get_data',[p],p); fn('duckdb_vector_get_validity',[p],p)
        fn('duckdb_vector_get_column_type',[p],p); fn('duckdb_get_type_id',[p],C.c_int)
        fn('duckdb_destroy_logical_type',[pp],None); fn('duckdb_list_vector_get_child',[p],p)
        fn('duckdb_free',[p],None); fn('duckdb_interrupt',[p],None)
        fn('duckdb_prepare',[p,C.c_char_p,pp],C.c_int); fn('duckdb_prepare_error',[p],C.c_char_p)
        fn('duckdb_execute_prepared',[p,p],C.c_int); fn('duckdb_destroy_prepare',[pp],None)
        fn('duckdb_bind_parameter_index',[p,C.POINTER(u),C.c_char_p],C.c_int); fn('duckdb_bind_int64',[p,u,C.c_int64],C.c_int)
        fn('duckdb_bind_varchar',[p,u,C.c_char_p],C.c_int); fn('duckdb_bind_timestamp',[p,u,Timestamp],C.c_int)
        fn('duckdb_bind_null',[p,u],C.c_int); fn('duckdb_bind_double',[p,u,C.c_double],C.c_int)
        fn('duckdb_disconnect',[pp],None); fn('duckdb_close',[pp],None)
        config=p(); l.duckdb_create_config(C.byref(config))
        l.duckdb_set_config(config,b'allow_unsigned_extensions',b'true')
        self.db=p(); err=p()
        if l.duckdb_open_ext(str(path).encode(),C.byref(self.db),config,C.byref(err)):
            raise RuntimeError(C.string_at(err).decode())
        l.duckdb_destroy_config(C.byref(config)); self.conn=p()
        assert l.duckdb_connect(self.db,C.byref(self.conn))==0
        self.query(f"LOAD {lit(BUILD/'extension/duckgql/duckgql.duckdb_extension')}")
        self.query(f"SET threads={threads}; SET memory_limit={lit(memory)}; SET preserve_insertion_order=false")
    def prepare(self, sql):
        statement=C.c_void_p()
        if self.lib.duckdb_prepare(self.conn,sql.encode(),C.byref(statement)):
            error=self.lib.duckdb_prepare_error(statement).decode(); self.lib.duckdb_destroy_prepare(C.byref(statement))
            raise RuntimeError(error)
        return statement
    def bind(self, statement, name, value):
        l=self.lib; index=C.c_uint64()
        if l.duckdb_bind_parameter_index(statement,C.byref(index),str(name).encode()): raise KeyError(name)
        index=index.value
        if value is None: status=l.duckdb_bind_null(statement,index)
        elif isinstance(value,datetime): status=l.duckdb_bind_timestamp(statement,index,Timestamp(int((value-datetime(1970,1,1)).total_seconds()*1000000)))
        elif isinstance(value,float): status=l.duckdb_bind_double(statement,index,value)
        elif isinstance(value,int): status=l.duckdb_bind_int64(statement,index,value)
        else: status=l.duckdb_bind_varchar(statement,index,value.encode())
        if status: raise RuntimeError(f'cannot bind {name}')
    def query(self, sql=None, timeout=None, prepared=None):
        l=self.lib; result=C.create_string_buffer(48)
        timer=threading.Timer(timeout, l.duckdb_interrupt,args=(self.conn,)) if timeout else None
        if timer: timer.start()
        try:
            status=l.duckdb_execute_prepared(prepared,result) if prepared is not None else l.duckdb_query(self.conn,sql.encode(),result)
            if status: raise RuntimeError(l.duckdb_result_error(result).decode())
            names=[l.duckdb_column_name(result,i).decode() for i in range(l.duckdb_column_count(result))]
            types=[l.duckdb_column_type(result,i) for i in range(len(names))]
            if 24 in types:
                rows=[]; value=Result.from_buffer(result)
                for chunk_idx in range(l.duckdb_result_chunk_count(value)):
                    chunk=C.c_void_p(l.duckdb_result_get_chunk(value,chunk_idx))
                    try:
                        vectors=[l.duckdb_data_chunk_get_vector(chunk,col) for col in range(len(names))]
                        rows.extend(dict(zip(names,[self.vector_value(v,row) for v in vectors]))
                                    for row in range(l.duckdb_data_chunk_get_size(chunk)))
                    finally: l.duckdb_destroy_data_chunk(C.byref(chunk))
                return rows
            rows=[]
            for row in range(l.duckdb_row_count(result)):
                cells=[]
                for col,typ in enumerate(types):
                    if l.duckdb_value_is_null(result,col,row): cells.append(None); continue
                    ptr=l.duckdb_value_varchar(result,col,row)
                    try: value=C.string_at(ptr).decode()
                    finally: l.duckdb_free(ptr)
                    if typ==1: value=value=='true'
                    elif typ in (2,3,4,5,6,7,8,9,16,32): value=int(value)
                    elif typ in (10,11,19): value=float(value)
                    elif typ in (12,20,21,22,31): value=datetime.fromisoformat(value).isoformat(timespec='milliseconds')
                    cells.append(value)
                rows.append(dict(zip(names,cells)))
            return rows
        finally:
            if timer: timer.cancel(); timer.join()
            l.duckdb_destroy_result(result)
    def vector_value(self, vector, row):
        l=self.lib; mask=l.duckdb_vector_get_validity(vector)
        if mask and not (C.c_uint64.from_address(mask+8*(row//64)).value & (1<<(row%64))): return None
        logical=C.c_void_p(l.duckdb_vector_get_column_type(vector))
        try: typ=l.duckdb_get_type_id(logical)
        finally: l.duckdb_destroy_logical_type(C.byref(logical))
        data=l.duckdb_vector_get_data(vector)
        if typ==24:
            offset=C.c_uint64.from_address(data+16*row).value
            size=C.c_uint64.from_address(data+16*row+8).value
            child=l.duckdb_list_vector_get_child(vector)
            return [self.vector_value(child,i) for i in range(offset,offset+size)]
        scalar={1:C.c_bool,2:C.c_int8,3:C.c_int16,4:C.c_int32,5:C.c_int64,
                6:C.c_uint8,7:C.c_uint16,8:C.c_uint32,9:C.c_uint64,10:C.c_float,11:C.c_double}
        if typ in scalar: return scalar[typ].from_address(data+row*C.sizeof(scalar[typ])).value
        if typ==17:
            address=data+16*row; size=C.c_uint32.from_address(address).value
            pointer=address+4 if size<=12 else C.c_void_p.from_address(address+8).value
            return C.string_at(pointer,size).decode()
        if typ==13: return (datetime(1970,1,1)+timedelta(days=C.c_int32.from_address(data+4*row).value)).date().isoformat()
        if typ==12: return (datetime(1970,1,1)+timedelta(microseconds=C.c_int64.from_address(data+8*row).value)).isoformat(timespec='milliseconds')
        raise TypeError(f'Unsupported vector result type {typ}')
    def close(self):
        self.lib.duckdb_disconnect(C.byref(self.conn)); self.lib.duckdb_close(C.byref(self.db))

def prepare(db, upstream, out):
    schema=(upstream/'ladybugdb/etl/schema.cypher').read_text()
    copies=(upstream/'ladybugdb/etl/copy.cypher').read_text()
    files=dict(re.findall(r"COPY (\w+) FROM '../csv/([^']+)'",copies))
    csvroot=upstream/'social_network-sf1-CsvBasic-StringDateFormatter'
    if not csvroot.exists(): csvroot=upstream/'csv'
    nodes=re.findall(r'CREATE NODE TABLE (\w+)\((.*?)\);',schema)
    ids={name:(i+1)*10**15 for i,(name,_) in enumerate(nodes)}
    def source(name): return f"read_csv({lit(csvroot/files[name])}, delim='|', header=true, all_varchar=true, quote='', escape='')"
    props={}
    for name,columns in nodes:
        for col,typ in re.findall(r'(\w+) (INT64|TIMESTAMP|DATE|STRING)',columns):
            if col!='ID': props[col]={'INT64':'BIGINT','TIMESTAMP':'TIMESTAMP','DATE':'DATE','STRING':'VARCHAR'}[typ]
    projections=[]
    for name,columns in nodes:
        present={x for x,_ in re.findall(r'(\w+) (INT64|TIMESTAMP|DATE|STRING)',columns)}
        label={'Post':'Post;Message','Comment':'Comment;Message','Tagclass':'TagClass'}.get(name,name)
        if name in ('Place','Organisation'): label_expr=f"{lit(label+';')} || upper(type[1]) || type[2:]"
        else: label_expr=lit(label)
        fields=[f'{ids[name]}+id::BIGINT AS ":ID"',f'{label_expr} AS ":LABEL"','id::BIGINT AS id']
        for col,typ in props.items(): fields.append(f'{chr(34)+col+chr(34) if col in present else "NULL"}::{typ} AS "{col}"')
        projections.append(f'SELECT {", ".join(fields)} FROM {source(name)}')
    vertices=out/'vertices.parquet'; edges=out/'edges.parquet'
    db.query(f'COPY ({" UNION ALL ".join(projections)}) TO {lit(vertices)} (FORMAT PARQUET, COMPRESSION ZSTD)')
    mapping={'containerOf':'CONTAINER_OF','hasInterest':'HAS_INTEREST','hasMember':'HAS_MEMBER','hasModerator':'HAS_MODERATOR','hasType':'HAS_TYPE','isPartOf':'IS_PART_OF','isSubclassOf':'IS_SUBCLASS_OF','knows':'KNOWS','studyAt':'STUDY_AT','workAt':'WORK_AT'}
    relations=[]; input_counts={}
    for name,src,dst,tail in re.findall(r'CREATE REL TABLE (\w+)\(FROM (\w+) TO (\w+)(.*?)\);',schema):
        label=mapping.get(name)
        if not label:
            label=next(v for k,v in [('HasCreator','HAS_CREATOR'),('HasTag','HAS_TAG'),('IsLocatedIn','IS_LOCATED_IN'),('like','LIKES'),('replyOf','REPLY_OF')] if k in name)
        header=(csvroot/files[name]).open().readline().strip().split('|')
        relprops={x:t for x,t in re.findall(r'(\w+) (INT64|TIMESTAMP)',tail)}
        extra=[]
        for col,typ in [('creationDate','TIMESTAMP'),('joinDate','TIMESTAMP'),('classYear','BIGINT'),('workFrom','BIGINT')]:
            extra.append(f'{chr(34)+col+chr(34) if col in relprops else "NULL"}::{typ} AS "{col}"')
        # Headers repeat for same-label edges; bind columns positionally via columns map.
        cols={f'c{i}':'VARCHAR' for i in range(len(header))}
        relsource=f"read_csv({lit(csvroot/files[name])}, delim='|', header=true, columns={str(cols)}, quote='', escape='')"
        extra=[re.sub(r'"(creationDate|joinDate|classYear|workFrom)"(?=::)',lambda m:f'c{header.index(m[1])}',x) for x in extra]
        def projection(reverse=False):
            a,b=('c1','c0') if reverse else ('c0','c1')
            return f'SELECT {ids[src]}+{a}::BIGINT AS ":START_ID", {ids[dst]}+{b}::BIGINT AS ":END_ID", {lit(label)} AS ":TYPE", {", ".join(extra)} FROM {relsource}'
        relations.append(projection())
        if name=='knows': relations.append(projection(True))
    db.query(f'COPY ({" UNION ALL ".join(relations)}) TO {lit(edges)} (FORMAT PARQUET, COMPRESSION ZSTD)')
    counts={x:db.query(f'SELECT count(*) AS n FROM read_parquet({lit(p)})')[0]['n'] for x,p in [('vertices',vertices),('edges',edges)]}
    db.query(f'CREATE GRAPH snb_interactive ANY; COPY GRAPH snb_interactive FROM (VERTICES {lit(vertices)}, EDGES {lit(edges)}) FORMAT GRAPH')
    for prop in ['id','name','firstName','lastName','birthday','length']:
        db.query(f"CALL gql_create_property_index('snb_interactive', {lit(prop)})")
    db.query('CHECKPOINT')
    return counts

def cases(upstream,out):
    # Extract upstream fixed parameters without importing its engine dependencies.
    tree=ast.parse((upstream/'ladybugdb/query.py').read_text()); params={}; original={}
    for f in tree.body:
        if isinstance(f,ast.FunctionDef) and f.name.startswith('run_query'):
            n=int(f.name[9:]); assignments={a.targets[0].id:a.value for a in f.body if isinstance(a,ast.Assign)}
            params[n]=eval(compile(ast.Expression(assignments['params']),'params','eval'),{'datetime':datetime,'timezone':timezone})
            original[n]=ast.literal_eval(assignments['query'])
    reference={'queries': {f'interactive-short-{n}': {'parameters': {'personId': 1131, 'messageId': 0}} for n in range(1,8)}}
    for n,p in params.items():
        p={k:int(v.timestamp()*1000) if isinstance(v,datetime) else v for k,v in p.items()}
        if n in (3,4): p['durationDays']=int((p['endDate']-p['startDate'])/86400000)
        reference['queries'][f'interactive-complex-{n}']={'parameters':p}
    paramfile=out/'parameters.json'; paramfile.write_text(json.dumps(reference,indent=2))
    unsupported={
    }
    queries={}
    for n in range(1,15):
        key=f'interactive-complex-{n}'
        path=HERE/'queries'/f'ic{n}.gql'
        if path.exists():
            queries[n]=path.read_text()
            unsupported.pop(key,None)
            (out/f'ic{n}.gql').write_text(queries[n])
        else:
            queries[n]=''
            (out/f'ic{n}.cypher').write_text(original[n])
    return queries,unsupported

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--upstream',type=Path,default=Path('/Users/rahuliyer/graph-benchmark-ldbc-complex'))
    parser.add_argument('--output',type=Path,default=ROOT/'build/benchmarks/ldbc-complex-sf1'); parser.add_argument('--threads',type=int,default=4)
    parser.add_argument('--runs',type=int,default=5); parser.add_argument('--timeout',type=float,default=10)
    parser.add_argument('--optimizer',choices=['cost','legacy'],default='cost')
    parser.add_argument('--prepared',action='store_true',help='compile each query once and reuse it, as Ladybug\'s client does; the first timed run includes the prepare')
    parser.add_argument('--setting',action='append',default=[],help='a SET statement to run before the queries')
    parser.add_argument('--saved-csr',action='store_true',help='read the CSR saved in the file instead of building it')
    parser.add_argument('--parameterized',action='store_true',help='like --prepared, binding upstream parameters instead of writing them as literals')
    args=parser.parse_args(); args.prepared=args.prepared or args.parameterized; out=args.output; out.mkdir(parents=True,exist_ok=True)
    report={'date':datetime.now(timezone.utc).isoformat(),'duckgql_commit':git(ROOT,'rev-parse','HEAD'),'duckgql_status':git(ROOT,'status','--short'),'upstream_commit':git(args.upstream,'rev-parse','HEAD'),'platform':platform.platform(),'config':{'optimizer':args.optimizer,'prepared':args.prepared,'parameterized':args.parameterized,'threads':args.threads,'memory_limit':'8GB','runs':args.runs,'warmups':0,'timeout_seconds':args.timeout,'timer':'perf_counter; C API execution and Python result conversion; no CLI overhead','csr':'one full snapshot before all queries; load/index/CSR excluded from query timings'},'queries':{}}
    report['binary_sha256']=hashlib.file_digest((ROOT/'build/release/src/libduckdb.dylib').open('rb'),'sha256').hexdigest()
    report['extension_sha256']=hashlib.file_digest((ROOT/'build/release/extension/duckgql/duckgql.duckdb_extension').open('rb'),'sha256').hexdigest()
    results_name='results-parameterized.json' if args.parameterized else 'results-prepared.json' if args.prepared else 'results.json'
    def save(): (out/results_name).write_text(json.dumps(report,indent=2)+'\n')
    save(); db=DB(out/'sf1.duckdb',args.threads)
    if not (out/'loaded.json').exists():
        print('Preparing and loading SF1',flush=True); t=time.perf_counter(); counts=prepare(db,args.upstream,out)
        (out/'loaded.json').write_text(json.dumps({'counts':counts,'seconds':time.perf_counter()-t},indent=2))
    report['machine']={'chip':'Apple M5 Pro','cores':15,'ram_gb':24,'os':'macOS 26.3.1'}
    archive=args.upstream/'social_network-sf1-CsvBasic-StringDateFormatter.tar.zst'
    report['dataset_sha256']=hashlib.file_digest(archive.open('rb'),'sha256').hexdigest() if archive.exists() else None
    report['runner_sha256']=hashlib.file_digest(Path(__file__).open('rb'),'sha256').hexdigest()
    report['load']=json.loads((out/'loaded.json').read_text()); print(report['load'],flush=True)
    for setting in args.setting: db.query(setting)
    db.query('SESSION SET GRAPH snb_interactive'); t=time.perf_counter(); db.query("SELECT build_count FROM gql_csr_stats('snb_interactive')") if args.saved_csr else db.query("CALL gql_build_csr('snb_interactive')")
    report['csr_seconds']=time.perf_counter()-t; print('CSR',report['csr_seconds'],flush=True)
    db.query(f"SET gql_optimizer={lit(args.optimizer)}")
    queries,unsupported=cases(args.upstream,out)
    gc.disable()
    for n,q in queries.items():
        key=f'ic{n}'; print(key,flush=True); entry={'adaptation':unsupported.get(f'interactive-complex-{n}','native GQL adapter: benchmark/ldbc_complex_sf1/queries/'+key+'.gql')}
        if q: entry['query_sha256']=hashlib.sha256(q.encode()).hexdigest()
        report['queries'][key]=entry; timings=[]
        if f'interactive-complex-{n}' in unsupported:
            entry.update(status='not_implemented',reason='No complete native GQL adaptation in the SF1 corpus; upstream Cypher is saved as .cypher for reference only.')
            print(entry,flush=True); save(); continue
        expected=json.loads((args.upstream/f'expected/{key}.json').read_text())
        try:
            statement=None
            for i in range(args.runs):
                t=time.perf_counter()
                if args.prepared and statement is None:
                    statement=db.prepare(parameterized(n,q) if args.parameterized else q)
                    for _,name,value in PARAMETERS[n] if args.parameterized else []: db.bind(statement,name,value)
                rows=db.query(q,args.timeout,prepared=statement); timings.append((time.perf_counter()-t)*1000)
                # GQL harness returns positional columns; rename to the golden schema.
                assert all(len(r)==len(expected[0]) for r in rows), 'projection arity differs from golden schema'
                actual=[dict(zip(expected[0].keys(),r.values())) for r in rows]
                (out/f'{key}-actual.json').write_text(json.dumps(actual,indent=2)+'\n')
                # Collected values have no order, but the IDs along a path do.
                actual=[{k:sorted(v) if isinstance(v,list) and any(isinstance(x,(str,list)) for x in v) else v for k,v in row.items()} for row in actual]
                if key=='ic14':
                    # Upstream compares IC14 unordered, since equal path weights can tie.
                    order=lambda row: json.dumps(row,sort_keys=True)
                    actual,expected=sorted(actual,key=order),sorted(expected,key=order)
                if actual!=expected: raise AssertionError(f'golden mismatch: {len(actual)} rows vs {len(expected)}; see {key}-actual.json')
            if statement is not None: db.lib.duckdb_destroy_prepare(C.byref(statement))
            entry.update(status='passed',rows=len(rows),samples_ms=timings,mean_ms=statistics.mean(timings),median_ms=statistics.median(timings),min_ms=min(timings),max_ms=max(timings))
        except Exception as e:
            entry.update(status='wrong_answer' if isinstance(e,AssertionError) else ('timeout' if 'Interrupt' in str(e) else 'unsupported' if f'interactive-complex-{n}' in unsupported else 'error'),error=str(e))
        print(entry,flush=True); save()
    gc.enable(); db.close()
if __name__=='__main__': main()
