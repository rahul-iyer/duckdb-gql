import json,sys,re
from pathlib import Path
S=Path(sys.argv[1]); d=json.load(open(S/'duck.json')); l=json.load(open(S/'lb.json')); roots=json.load(open(S/'roots.json'))
def can(v):
    if isinstance(v,list):
        if v and all(isinstance(x,list) for x in v): return sorted([str(i) for i in x] for x in v)
        return sorted(v) if v and all(isinstance(x,str) for x in v) else v
    if isinstance(v,float): return round(v,6)
    if isinstance(v,str) and re.match(r'\d{4}-\d\d-\d\d[T ]',v): return v.replace(' ','T')[:23]
    return v
tot={};bad=[]
for mode in ['cost','legacy','cost-param','legacy-param']:
    ok=0;non=0
    for r in roots:
        for n in range(1,15):
            a=d[f'{mode}/{r}/ic{n}'];b=l[f'{r}/ic{n}']
            if isinstance(a,str) or isinstance(b,str): bad.append((mode,r,n,'ERR',str(a)[:120] if isinstance(a,str) else 'ok',str(b)[:120] if isinstance(b,str) else 'ok'));continue
            A=[[can(x) for x in row] for row in a];B=[[can(x) for x in row] for row in b]
            if n==14: A,B=sorted(A,key=str),sorted(B,key=str)
            if A==B: ok+=1; non+= len(A)>0
            else:
                i=next((i for i,(x,y) in enumerate(zip(A,B)) if x!=y),min(len(A),len(B)))
                bad.append((mode,r,n,f'rows {len(A)}/{len(B)} firstdiff@{i}',A[i] if i<len(A) else None,B[i] if i<len(B) else None))
    print(mode,'match',ok,'of',len(roots)*14,'non-empty matches',non)
for x in bad[:24]: print(str(x)[:330])
print('total mismatches',len(bad))
