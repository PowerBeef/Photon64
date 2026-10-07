import json,sys
s=open(sys.argv[1] if len(sys.argv)>1 else 'out/gputest.log').read()
i=s.find('\n{')
if i<0: print("no result yet", s[-300:]); raise SystemExit
r=json.loads(s[i:])
for c in r['checks']:
    fb=c.get('fb')
    print(c['frame'], 'vi', c.get('viDiff'), c.get('viMax'), 'sync', c.get('sync'), 'prims', c.get('prims'), 'fb', fb and (fb['fmt'], fb['w'], fb['h'], fb['color']['dc'], fb['color']['dh'], fb['color']['first'], fb['depth']['dc'], fb['depth']['dh'], fb['depth']['first']), c.get('extra'))
print({k:r.get(k) for k in ('compileMs','msA','msB','stats','totalMs')})
