import json,sys
e=json.load(open('work/ko/SCENARIO/SCE.json'))
a,b=int(sys.argv[1]),int(sys.argv[2])
for i in range(a,min(b,len(e))):
    x=e[i]; lines=[l for l in x['ko'].split('\n')]
    n=sum(1 for l in lines if l.strip())
    print(f"{i}|{n}|{x['where'][0]}|"+' / '.join(l.strip() for l in lines if l.strip()))
