"""Exact bag comparison of fixed trails against native SQL with every ID inequality."""
import argparse,csv,itertools,json,random,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cli', type=Path, default=root/'build/release/duckdb')
parser.add_argument('--output', type=Path, default=root/'build/benchmarks/lsqb/trail-filter/relational-controls')
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
rng=random.Random(907)
nodes=[(str(i),'N' if i < 6 else 'Other') for i in range(1,8)]
edges=[(str(rng.randrange(1,8)),str(rng.randrange(1,8)),rng.choice(['R','S','T'])) for _ in range(30)]
edges += [('1','1','R'),('1','1','S'),('1','1','R'),('1','2','R'),('1','2','R')]
for name,header,rows in [('nodes',[':ID',':LABEL'],nodes),('edges',[':START_ID',':END_ID',':TYPE'],edges)]:
 with (out/f'{name}.csv').open('w') as f:
  w=csv.writer(f);w.writerow(header);w.writerows(rows)
sql=f".output /dev/null\nCREATE GRAPH controls ANY; COPY GRAPH controls FROM (VERTICES '{out}/nodes.csv', EDGES '{out}/edges.csv') FORMAT GRAPH; SESSION SET GRAPH controls;\n"
cases=[]
for labels in itertools.product(['R','S','T',''],repeat=3):cases.append((labels,(False,False,False)))
for rev in itertools.product([False,True],repeat=3):
 for labels in [('R','S','T'),('R','S','R'),('R','R','S'),('S','R','R'),('','',''),('','R','S')]:cases.append((labels,rev))
cases = [(labels, rev, closing, filtered) for labels, rev in cases for closing in [False, True] for filtered in [False, True]]
for i,(labels,rev,closing,filtered) in enumerate(cases):
 pattern='(n0:N)' if filtered else '(n0)'
 for j,(label,reverse) in enumerate(zip(labels,rev)):
  edge=f'[e{j}'+(':'+label if label else '')+']'
  pattern+=('<-'+edge+'-' if reverse else '-'+edge+'->')+f'(n{j+1}'+(':N)' if filtered else ')')
 projection=', '.join([f'element_id(e{j})' for j in range(3)]+[f'element_id(n{j})' for j in range(4)])
 if closing: pattern += ', (n0)-[closure]->(n3)'
 gql=f'MATCH {pattern} '+('WHERE element_id(n0) <> element_id(n2) ' if filtered else '')+f'RETURN {projection};'
 table='gql_data.graph_1_edges';sources=[f'e{j}.__gql_'+('target' if rev[j] else 'source')+'_id' for j in range(3)];targets=[f'e{j}.__gql_'+('source' if rev[j] else 'target')+'_id' for j in range(3)]
 filters=[f'{targets[j]}={sources[j+1]}' for j in range(2)]
 filters += [f'e{j}.__gql_type=\'{label.lower()}\'' for j,label in enumerate(labels) if label]
 filters += [f'e{a}.__gql_edge_id<>e{b}.__gql_edge_id' for a,b in itertools.combinations(range(3),2)]
 if closing: filters += [f'eclose.__gql_source_id={sources[0]}', f'eclose.__gql_target_id={targets[2]}']
 vertex_ids=[sources[0]]+targets
 for j in range(4):
  filters += [f'v{j}.__gql_id={vertex_ids[j]}']
  if filtered:filters += [f"list_contains(v{j}.__gql_label, 'n')"]
 if filtered:filters += [f'{sources[0]} <> {targets[1]}']
 native='SELECT '+', '.join([f'e{j}.__gql_edge_id' for j in range(3)]+vertex_ids)+' FROM '+', '.join(f'{table} e{j}' for j in range(3))+(', '+table+' eclose' if closing else '')+', '+', '.join(f'gql_data.graph_1_vertices v{j}' for j in range(4))+' WHERE '+' AND '.join(filters)+';'
 for name,q in [('gql',gql),('sql',native)]:sql+=f'.once {out}/{i}-{name}.csv\n{q}\n'
(out/'controls.sql').write_text(sql)
# Disposable in-memory database; SQL always keeps the complete uniqueness check.
r=subprocess.run([str(args.cli.resolve()),'-unsigned','-no-init','-batch','-bail','-csv','-noheader'],input=sql,text=True,capture_output=True,timeout=60)
if r.returncode:raise RuntimeError(r.stderr)
rows_checked=0
for i,case in enumerate(cases):
 def rows(kind):return sorted(csv.reader((out/f'{i}-{kind}.csv').open()))
 actual,expected=rows('gql'),rows('sql')
 assert actual==expected,(i,case,actual[:5],expected[:5])
 rows_checked+=len(actual)
report={'cases':len(cases),'matched_rows':rows_checked,'status':'verified','scope':'Exact output bags of three edge IDs and four vertex IDs; all directed/reversed label combinations above; native SQL retains all three trail inequalities; seeded graph includes parallel edges and self loops; half the cases add an independent closing edge which may reuse a path edge; half apply vertex label and identity filters.'}
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(report)
