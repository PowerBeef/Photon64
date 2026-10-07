# contact sheet of dumped frames:  sheet.py out.png cols width file...
import sys
from PIL import Image, ImageDraw
out, cols, w = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); files = sys.argv[4:]
h = w * 3 // 4; rows = (len(files) + cols - 1) // cols
im = Image.new('RGB', (cols * (w + 4), rows * (h + 4)), (40, 0, 40)); d = ImageDraw.Draw(im)
for i, f in enumerate(files):
    t = Image.open(f).convert('RGB').resize((w, h), Image.BILINEAR)
    x, y = (i % cols) * (w + 4), (i // cols) * (h + 4)
    im.paste(t, (x, y)); d.text((x + 4, y + 2), f.split('_')[-1].split('.')[0], fill=(255, 255, 0))
im.save(out)
