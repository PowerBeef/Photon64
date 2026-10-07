import json,sys
bad=[]; n=0; tot_prims=0
for l in open(sys.argv[1] if len(sys.argv)>1 else 'out/gpumulti.log'):
    if not l.startswith('[page] {'): continue
    r=json.loads(l[7:]); n+=1
    if r.get('error') or r.get('vi') or r.get('fb') or not r.get('sync'):
        bad.append((r['rom'], r.get('error'), r.get('vi'), r.get('bbox'), r.get('fb'), r.get('sync')))
print(n, 'roms;', len(bad), 'with differences')
for b in bad: print(b)
