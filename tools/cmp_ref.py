# Run test ROMs that ship with a reference screenshot; compare the amount of green (PASS) / red (FAIL+labels) / white text.
import subprocess, glob, os, sys
from PIL import Image
import numpy as np
root=sys.argv[1]; frames=sys.argv[2] if len(sys.argv)>2 else '120'
def counts(im):
    a=np.asarray(im.convert('RGB').resize((640,480))).astype(int)
    r,g,b=a[...,0],a[...,1],a[...,2]
    return np.array([((r>140)&(g>140)&(b>140)).sum(), ((g>100)&(r<90)&(b<90)).sum(), ((r>140)&(g<90)&(b<90)).sum()])
res=[]
out=sys.argv[3] if len(sys.argv)>3 else '/tmp/rsptest_'
for rom in sorted(glob.glob(root+'/**/*.N64',recursive=True)):
    base=rom[:-4]
    refs=[r for r in set([base+'.png']+glob.glob(glob.escape(base)+'*.png')) if os.path.exists(r)]
    if not refs: continue
    try: subprocess.run(['./out/native',rom,frames,'-o',out,'-e',frames],capture_output=True,timeout=60)
    except Exception as e: res.append((9.9,os.path.basename(rom),'timeout')); continue
    mc=counts(Image.open(out+'%05d.png'%int(frames)))
    best=None
    for r in refs:
        try: ref=Image.open(r)
        except Exception: continue
        rc=counts(ref)
        d=float(np.abs(mc-rc).sum())/max(1,rc.sum())
        if best is None or d<best[0]: best=(d,os.path.basename(r),mc.tolist(),rc.tolist())
    if best: res.append((best[0],os.path.basename(rom),best))
import statistics
rows=[(n,b) for s_,n,b in res if isinstance(b,tuple)]
gr=[b[2][1]/max(1,b[3][1]) for n,b in rows if b[3][1]>200]
rr=[b[2][2]/max(1,b[3][2]) for n,b in rows if b[3][2]>200]
mg=statistics.median(gr) if gr else 1; mr=statistics.median(rr) if rr else 1
bad=0
for n,b in rows:
    g=b[2][1]/max(1,b[3][1]); r=b[2][2]/max(1,b[3][2])
    if abs(g-mg)>0.04 or abs(r-mr)>0.06 or (b[3][1]<=200 and b[2][1]>200):
        print(f'{n:44s} green {b[2][1]:6d} vs ref {b[3][1]:6d} ({g:.2f})  red {b[2][2]:6d} vs ref {b[3][2]:6d} ({r:.2f})  [{b[1]}]'); bad+=1
print(f'{len(rows)} roms compared (median green ratio {mg:.3f}, red {mr:.3f}); {bad} outliers')
