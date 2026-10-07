# visualize an oracle diff image: mine | ref | where (grey = tiny, red = larger than a dither step)
import sys
from PIL import Image
import numpy as np
im=np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(int)
h=im.shape[0]
mine=im[:, :320]; ref=im[:, 320:640]
d=np.abs(mine-ref).max(axis=2); big=d>8
out=np.zeros((h,960,3),dtype=np.uint8); out[:, :320]=mine; out[:, 320:640]=ref
o=out[:, 640:]; o[:]=(mine*0.25).astype(np.uint8); o[d>0]=(0,255,0); o[big]=(255,0,0)
print('diff px', int((d>0).sum()), 'big', int(big.sum()))
if big.any(): print('big bbox (y,x)', np.argwhere(big).min(axis=0), np.argwhere(big).max(axis=0)); print('examples', [(int(y),int(x),mine[y,x].tolist(),ref[y,x].tolist()) for y,x in np.argwhere(big)[:6]])
Image.fromarray(out).resize((1920,h*2),Image.NEAREST).save(sys.argv[2])
