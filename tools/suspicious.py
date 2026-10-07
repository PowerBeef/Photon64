# HD pixels with no similar colour in the native image's neighbourhood (candidates for upscaling artifacts)
import sys
from PIL import Image
import numpy as np
from scipy import ndimage
n=np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
for name in sys.argv[2:]:
    h=np.asarray(Image.open(name).convert('RGB')).astype(int)
    s=h.shape[1]//n.shape[1]
    H,W,_=h.shape
    up=np.repeat(np.repeat(n,s,axis=0),s,axis=1)
    best=np.full((H,W),1e9)
    for dy in (-1,0,1):
        for dx in (-3,-2,-1,0,1,2,3):
            sh=np.roll(np.roll(up,dy*s,axis=0),dx*s,axis=1)
            best=np.minimum(best,np.abs(h-sh).max(axis=2))
    bad=best>70
    bad[:, :10*s]=False; bad[:, -10*s:]=False; bad[-10*s:, :]=False; bad[:9*s,:]=False     # ignore the picture border
    lab,cnt=ndimage.label(bad)
    rows=[]
    for i,o in enumerate(ndimage.find_objects(lab)):
        ys,xs=o; rows.append((int((lab[o]==i+1).sum()),xs.start,ys.start,xs.stop-xs.start,ys.stop-ys.start))
    rows.sort(reverse=True)
    print(name,'suspicious px',int(bad.sum()),'clusters',cnt, rows[:10])
