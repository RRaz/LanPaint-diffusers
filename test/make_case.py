"""Builds a test case for affinity_runner.py from the example image.

The centre of the picture is marked for replacement (alpha 0); the rest keeps
(alpha maximum), matching the convention the filter uses.
"""

import sys

import numpy as np
from PIL import Image, ImageDraw

src = sys.argv[1] if len(sys.argv) > 1 else "test/Original_No_Mask.png"
out = sys.argv[2] if len(sys.argv) > 2 else "test/case.rgba16"
size = int(sys.argv[3]) if len(sys.argv) > 3 else 512

image = Image.open(src).convert("RGB").resize((size, size), Image.LANCZOS)

mask = Image.new("L", (size, size), 255)
draw = ImageDraw.Draw(mask)
draw.rectangle([size // 4, size // 4, size * 3 // 4, size * 3 // 4], fill=0)
mask.save(out.replace(".rgba16", "-mask.png"))

rgb16 = ((np.asarray(image, dtype=np.uint32) * 32767) // 255).astype(np.uint16)
alpha16 = ((np.asarray(mask, dtype=np.uint32) * 32767) // 255).astype(np.uint16)

layer = np.dstack([rgb16, alpha16])
layer.tofile(out)
print(f"written {out}: {size}x{size}, {layer.nbytes} bytes")
