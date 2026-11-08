#!/usr/bin/env python3
"""Line up the test page's swatches across the pipeline.   python3 -I swatches.py PULL_DIR [--shot IMG.png --shot-scale S]

For each swatch of testpage/index.html found in a capture (the guest's half-float picture "src", and "post", the bytes Henge hands
Core Animation), print: the CSS value it was asked to be, the half-float value the guest wrote, that value as sRGB bytes, and the post
bytes. With --shot (a screenshot of the phone), also the pixel values the screenshot holds at the same place.
Swatches are found as flat single-colour rectangles in row-major order; the page's black swatch cannot be told from its background."""
import glob, json, os, sys
import numpy as np
from PIL import Image

EXPECT = ["000000", "101010", "202020", "303030", "404040", "505050", "606060", "707070", "808080", "909090", "a0a0a0", "b0b0b0",
          "c0c0c0", "d0d0d0", "e0e0e0", "f0f0f0", "ffffff", "ff0000", "00ff00", "0000ff", "00ffff", "ff00ff", "ffff00", "ffffff",
          "ff8000", "8000ff", "0080ff", "band 808080", "band ffffff", "band ff0000"]


def srgb_decode(v):
    v = np.asarray(v, dtype=np.float64)
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def srgb_encode(x):
    x = np.clip(np.asarray(x, dtype=np.float64), 0, 1)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * x ** (1 / 2.4) - 0.055)


def components(key, min_side=24):
    """Flat rectangles: for each frequent colour, split its pixels into row groups then column groups."""
    out = []
    vals, counts = np.unique(key, return_counts=True)
    for v, c in zip(vals, counts):
        if c < min_side * min_side or c > key.size * 0.5:
            continue
        ys, xs = np.where(key == v)
        order = np.argsort(ys)
        ys, xs = ys[order], xs[order]
        cuts = np.where(np.diff(ys) > 6)[0] + 1
        for yg, xg in zip(np.split(ys, cuts), np.split(xs, cuts)):
            o = np.argsort(xg)
            yg, xg = yg[o], xg[o]
            for ya, xa in zip(np.split(yg, np.where(np.diff(xg) > 6)[0] + 1), np.split(xg, np.where(np.diff(xg) > 6)[0] + 1)):
                w, h = xa.max() - xa.min() + 1, ya.max() - ya.min() + 1
                if w >= min_side and h >= min_side and len(xa) > 0.6 * w * h:
                    out.append((int(ya.min()), int(xa.min()), (int(xa.min()) + int(xa.max())) // 2, (int(ya.min()) + int(ya.max())) // 2, w, h, v))
    return sorted(out, key=lambda t: (t[0] // 30, t[1]))


def main():
    """Label-free test. The page's swatches are whole CSS bytes (16, 32, 128 ...). If the guest wrote those bytes/255 into the half-float
    surface, half*255 is a whole number for every swatch; if it wrote linear light, sRGB-decoded numbers are not. Count both."""
    d = sys.argv[1]
    metas = sorted(glob.glob(os.path.join(d, "frame-*.json")))
    m = json.load(open(metas[-1]))
    w, h = m["width"], m["height"]
    src = np.fromfile(os.path.join(d, m["files"]["src"]), dtype="<f2").reshape(h, w, 4).astype(np.float32)
    post = np.fromfile(os.path.join(d, m["files"]["post"]), dtype=np.uint8).reshape(h, w, 4)
    h16 = np.ascontiguousarray(src[..., :3].astype(np.float16)).view(np.uint16).reshape(h, w, 3)
    key = (h16[..., 0].astype(np.uint64) << 32) | (h16[..., 1].astype(np.uint64) << 16) | h16[..., 2].astype(np.uint64)
    comps = [c for c in components(key, min_side=40) if c[0] > 100 and c[4] < 1900 and not (c[4] > 900 and c[5] > 500)]
    whole = lambda x: np.abs(x - np.rint(x)) < 0.12
    gamma_hits = linear_hits = n = 0
    post_pass = post_enc = 0
    print(f"capture {m['seq']:03d}: {len(comps)} flat swatches (browser chrome included; the test is on mid-range components only)\n")
    print("guest half-float R,G,B      half*255            sRGB-encode(half)*255   post bytes     whole-number test")
    seen = set()
    for (y0, x0, cx, cy, bw, bh, v) in comps:
        r, g, b = (np.uint16((int(v) >> s_) & 0xffff).view(np.float16).astype(np.float32) for s_ in (32, 16, 0))
        if (float(r), float(g), float(b)) in seen:
            continue
        seen.add((float(r), float(g), float(b)))
        rgb = np.array([r, g, b], dtype=np.float64)
        mid = [i for i in range(3) if 0.02 < rgb[i] < 0.98]
        if not mid:
            continue  # 0 and 1 look the same whichever way the value was made
        a = 255 * rgb
        e = 255 * srgb_encode(rgb)
        g_ok = all(whole(a[i]) for i in mid)
        l_ok = all(whole(e[i]) for i in mid)
        p = post[cy, cx]
        n += 1
        near = lambda a, b_: all(abs(float(x) - float(y)) <= 1.5 for x, y in zip(a, b_))
        pb = (int(p[2]), int(p[1]), int(p[0]))
        post_pass += near(pb, a) and not near(pb, e)
        post_enc += near(pb, e) and not near(pb, a)
        gamma_hits += g_ok and not l_ok
        linear_hits += l_ok and not g_ok
        print(f"{r:7.4f} {g:7.4f} {b:7.4f}   {a[0]:6.2f},{a[1]:6.2f},{a[2]:6.2f}   {e[0]:6.2f},{e[1]:6.2f},{e[2]:6.2f}   {p[2]:3d},{p[1]:3d},{p[0]:3d}   "
              f"{'whole bytes as written (gamma-encoded)' if g_ok and not l_ok else 'whole after sRGB-encode (linear)' if l_ok and not g_ok else 'inconclusive'}")
    print(f"\n{n} distinct mid-range colours: {gamma_hits} are whole bytes as written (gamma-encoded), {linear_hits} are whole only after an sRGB encode (linear)")
    print(f"what Henge did to them: {post_pass} swatches came out unchanged (pass-through), {post_enc} came out sRGB-encoded"
          f"{'  (pointer-covered swatches excluded by tolerance)' if post_pass + post_enc < n else ''}")
    print("-> the half-float surface holds DISPLAY-ENCODED values; the screen_linear shader encodes them a second time (too light, paler)"
          if gamma_hits > linear_hits else
          "-> the half-float surface holds LINEAR light; the screen_linear shader's sRGB encode is right" if linear_hits > gamma_hits else "-> inconclusive")


if __name__ == "__main__":
    main()
