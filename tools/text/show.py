import json,sys
for n in sys.argv[1:]:
    e=json.load(open(f'work/ko/CHAPDATA/VISUAL.{n}.json'))
    print(f"=== VISUAL.{n}")
    for i,x in enumerate(e): print(i, x['code'], ' '.join(x['ko'].split()))
