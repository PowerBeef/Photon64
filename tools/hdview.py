# View helper for high-resolution test dumps (640S x 240S, anamorphic): hdview.py in.png out.png [x y w h in 4:3 units of 1280x960] [zoom]
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
im = im.resize((1280, 960), Image.BILINEAR if im.width > 1280 else Image.NEAREST)
if len(sys.argv) > 6:
    x, y, w, h = map(int, sys.argv[3:7]); z = int(sys.argv[7]) if len(sys.argv) > 7 else 1
    im = im.crop((x, y, x + w, y + h)).resize((w * z, h * z), Image.NEAREST)
im.save(sys.argv[2])
