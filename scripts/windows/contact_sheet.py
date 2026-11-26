import sys, glob, os
from PIL import Image, ImageDraw
d = sys.argv[1]; out = sys.argv[2]
files = sorted(glob.glob(os.path.join(d, "gpu_*.png")))
cols, w, h = 6, 256, 224
rows = (len(files) + cols - 1) // cols
sheet = Image.new("RGB", (cols * w, rows * h))
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB").resize((w, h))
    ImageDraw.Draw(im).text((4, 4), os.path.basename(f)[4:9], fill=(255, 0, 255))
    sheet.paste(im, ((i % cols) * w, (i // cols) * h))
sheet.save(out)
