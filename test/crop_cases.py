"""Checks plan_crop against the edges and awkward shapes of a selection."""

import importlib.util
import sys

spec = importlib.util.spec_from_file_location("ar", r"D:\Projects\LanPaint-diffusers\affinity_runner.py")
ar = importlib.util.module_from_spec(spec)
sys.modules["ar"] = ar
spec.loader.exec_module(ar)

LIMIT = 629145
TARGET = 1048576


def check(label, box, size):
    try:
        rect = ar.plan_crop(box, size, LIMIT, TARGET)
    except SystemExit as exc:
        print(f"{label:42s} refused: {exc}")
        return
    l, t, r, b = rect
    w, h = r - l, b - t
    problems = []
    if l < 0 or t < 0 or r > size[0] or b > size[1]:
        problems.append("outside the picture")
    if l > box[0] or t > box[1] or r < box[2] + 1 or b < box[3] + 1:
        problems.append("does not contain the selection")
    if w * h > TARGET * 1.2:
        problems.append("larger than the target")
    flag = "  <-- " + ", ".join(problems) if problems else ""
    print(f"{label:42s} {w}x{h} at {l},{t}  {w * h / 1e6:.2f} MP{flag}")


cases = [
    ("middle blob", (900, 700, 1199, 919), (2048, 1536)),
    ("top-left corner", (0, 0, 299, 219), (2048, 1536)),
    ("bottom-right corner", (1748, 1316, 2047, 1535), (2048, 1536)),
    ("touches left edge", (0, 600, 400, 900), (2048, 1536)),
    ("touches top edge, wide", (700, 0, 1300, 120), (2048, 1536)),
    ("full width strip at bottom", (0, 1500, 2047, 1535), (2048, 1536)),
    ("one pixel deep", (500, 500, 1500, 500), (2048, 1536)),
    ("single pixel", (1024, 768, 1024, 768), (2048, 1536)),
    ("whole picture under limit", (0, 0, 799, 749), (800, 750)),
    ("selection equals picture", (0, 0, 2047, 1535), (2048, 1536)),
    ("tiny picture", (0, 0, 19, 19), (20, 20)),
    ("picture 40x40", (0, 0, 39, 39), (40, 40)),
    ("portrait, bottom edge", (300, 3800, 500, 4095), (1000, 4096)),
    ("wide panorama", (3000, 100, 3500, 200), (6000, 500)),
]

for label, box, size in cases:
    check(label, box, size)
