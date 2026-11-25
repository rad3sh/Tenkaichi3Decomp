"""Compare the BT3_SHOT screenshots of several GPU back ends (scripts/windows/compare-backends.ps1).

usage: compare_shots.py <outdir> <reference backend> <backend> [...]

For every frame present in the reference and another back end: <outdir>/side_<backend>_NNNNN.png (reference |
backend | amplified difference) and a line with the mean difference and the share of pixels that differ by more
than 24 levels. Also converts each PPM to PNG so the pictures can be opened anywhere.
"""
import os
import re
import sys

import numpy as np
from PIL import Image


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out, backends = sys.argv[1], sys.argv[2:]
    shots = {}
    for b in backends:
        d = os.path.join(out, b)
        shots[b] = {}
        for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            m = re.match(r"gpu_(\d+)\.ppm$", name)
            if m:
                path = os.path.join(d, name)
                Image.open(path).save(path[:-4] + ".png")
                shots[b][int(m.group(1))] = path
    ref = backends[0]
    for b in backends[1:]:
        frames = sorted(set(shots[ref]) & set(shots[b]))
        print(f"{b} against {ref}: {len(frames)} common frame(s)")
        for f in frames:
            a, c = load(shots[ref][f]), load(shots[b][f])
            if a.shape != c.shape:
                h, w = min(a.shape[0], c.shape[0]), min(a.shape[1], c.shape[1])
                a = np.asarray(Image.fromarray(a.astype(np.uint8)).resize((w, h)), dtype=np.int16)
                c = np.asarray(Image.fromarray(c.astype(np.uint8)).resize((w, h)), dtype=np.int16)
            diff = np.abs(a - c).max(axis=2)
            mean = float(diff.mean())
            share = float((diff > 24).mean()) * 100.0
            amp = np.clip(diff * 4, 0, 255).astype(np.uint8)
            side = np.concatenate([a.astype(np.uint8), c.astype(np.uint8), np.stack([amp] * 3, axis=2)], axis=1)
            Image.fromarray(side).save(os.path.join(out, f"side_{b}_{f:05d}.png"))
            print(f"  frame {f:5d}: mean diff {mean:6.2f}, {share:5.1f}% of pixels differ")
    return 0


if __name__ == "__main__":
    sys.exit(main())
