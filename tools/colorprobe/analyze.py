#!/usr/bin/env python3
"""Read a colour-probe pull and say what it shows.  Usage: python3 -I analyze.py DIR [--regions]   (--regions lists flat swatches with positions)

For each capture (frame-NNN.json):
  * the geometry and the guest's gamma descriptor
  * the gamma table, read as float32 / float16, and which common curve it is closest to
  * what the guest wrote (src) against what Metal handed Core Animation (post): is the shader stage lossless?
  * the most common exact colours in src and post (use a page of known solid swatches to read the transfer function)
  * PNG previews of both, next to the raw files, so they can be looked at
Needs numpy and Pillow (python3 -I -m pip list | grep -i -E 'numpy|pillow').
"""
import glob, json, os, sys
import numpy as np
from PIL import Image


def srgb_encode(x):
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(np.maximum(x, 0), 1 / 2.4) - 0.055)


CURVES = {
    "identity": lambda x: x,
    "srgb_encode": srgb_encode,
    "gamma2.2_encode": lambda x: np.power(x, 1 / 2.2),
    "gamma2.2_decode": lambda x: np.power(x, 2.2),
}


def read_table(raw):
    """The gamma table, tried as planar float32 then float16; returns (name, entries, 3xN float array) candidates."""
    out = []
    for name, dt in (("f32", "<f4"), ("f16", "<f2")):
        size = np.dtype(dt).itemsize
        if len(raw) % (3 * size):
            continue
        with np.errstate(invalid="ignore", over="ignore"):
            a = np.frombuffer(raw, dtype=dt).astype(np.float64).reshape(3, -1)
        out.append((name, a.shape[1], a))
    return out


def describe_table(raw):
    for name, n, a in read_table(raw):
        if n < 8 or not np.all(np.isfinite(a)):
            print(f"    {name}: {n} entries/channel, contains non-finite values: unlikely layout")
            continue
        x = np.linspace(0, 1, n)
        print(f"    as planar {name}, {n} entries per channel; range [{a.min():.4f}, {a.max():.4f}]")
        for c, ch in enumerate("RGB"):
            errs = {k: float(np.max(np.abs(a[c] - f(x)))) for k, f in CURVES.items()}
            best = min(errs, key=errs.get)
            pts = " ".join(f"{a[c][int(p * (n - 1))]:.3f}" for p in (0, .1, .25, .5, .75, .9, 1))
            print(f"      {ch}: samples@0,10,25,50,75,90,100% = {pts}   closest curve: {best} (max error {errs[best]:.4f})"
                  f"   monotonic={bool(np.all(np.diff(a[c]) >= -1e-6))}")


def load_bgra(path, w, h):
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size != w * h * 4:
        return None
    return raw.reshape(h, w, 4)


def top_colors(img, k=10):
    rgb = (img[..., 2].astype(np.uint32) << 16) | (img[..., 1].astype(np.uint32) << 8) | img[..., 0]
    vals, counts = np.unique(rgb, return_counts=True)
    order = np.argsort(-counts)[:k]
    return [(f"{vals[i]:06x}", counts[i] / rgb.size) for i in order]


def regions(img, k=40, min_share=0.001):
    """Large single-colour areas, row-major by where they start: (rrggbb, share, bbox centre x,y, bbox w,h)."""
    rgb = (img[..., 2].astype(np.uint32) << 16) | (img[..., 1].astype(np.uint32) << 8) | img[..., 0]
    vals, counts = np.unique(rgb, return_counts=True)
    found = []
    for i in np.argsort(-counts)[:k]:
        if counts[i] / rgb.size < min_share:
            break
        ys, xs = np.where(rgb == vals[i])
        found.append((int(ys.min()) // 40, int(xs.min()), f"{vals[i]:06x}", counts[i] / rgb.size,
                      (int(xs.min()) + int(xs.max())) // 2, (int(ys.min()) + int(ys.max())) // 2,
                      int(xs.max()) - int(xs.min()) + 1, int(ys.max()) - int(ys.min()) + 1))
    return [(c, sh, cx, cy, bw, bh) for _, _, c, sh, cx, cy, bw, bh in sorted(found)]


def print_regions(tag, img):
    print(f"  {tag} flat regions (row-major; rrggbb share @centre x,y bbox WxH):")
    for c, sh, cx, cy, bw, bh in regions(img):
        print(f"      {c}  {sh*100:5.2f}%  @{cx},{cy}  {bw}x{bh}")



def load_half(path, w, h):
    raw = np.fromfile(path, dtype="<f2")
    if raw.size != w * h * 4:
        return None
    return raw.reshape(h, w, 4).astype(np.float32)


def shader_encode_u8(rgb, mode="linear"):
    """What the screen_linear shader writes for a half-float value: saturate, then (mode 'linear') the sRGB encode or (mode 'encoded') nothing."""
    v = np.clip(rgb, 0, 1)
    return np.rint(255 * (srgb_encode(v) if mode == "linear" else v)).astype(np.int32)


def half_report(src, post, mode="linear"):
    rgb = src[..., :3]
    finite = np.isfinite(rgb)
    print(f"  src half-float: per-channel min R/G/B {rgb[...,0][finite[...,0]].min():.4f} {rgb[...,1][finite[...,1]].min():.4f} {rgb[...,2][finite[...,2]].min():.4f}"
          f"   mean {rgb[...,0].mean():.4f} {rgb[...,1].mean():.4f} {rgb[...,2].mean():.4f}"
          f"   max {rgb[...,0].max():.4f} {rgb[...,1].max():.4f} {rgb[...,2].max():.4f}")
    print(f"  src values above 1.0: {100.0 * np.mean(rgb > 1.0):.3f}%   below 0: {100.0 * np.mean(rgb < 0.0):.3f}%   alpha min/max {src[...,3].min():.3f}/{src[...,3].max():.3f}")
    if post is not None:
        want = shader_encode_u8(rgb, mode)
        got = post[..., [2, 1, 0]].astype(np.int32)
        diff = np.abs(want - got).max(axis=2)
        bad = diff > 1
        print(f"  post vs what the shader should write in mode '{mode}' ({'sRGB encode of src' if mode == 'linear' else 'src passed through'}): max diff {diff.max()}, mean {diff.mean():.3f}, pixels over 1: {int(bad.sum())}")
        if bad.any():
            ys, xs = np.where(bad)
            box = (xs.max() - xs.min() + 1) * (ys.max() - ys.min() + 1)
            rest = diff.copy()
            rest[ys.min():ys.max() + 1, xs.min():xs.max() + 1] = 0
            small = box < 0.01 * diff.size
            print(f"    all of them lie in one box {xs.max()-xs.min()+1}x{ys.max()-ys.min()+1} at x{xs.min()},y{ys.min()}"
                  f"{' (small: the mouse pointer, which the presenter draws on top)' if small else ' (large: not just the pointer)'};"
                  f" outside it max diff {rest.max()}")
            verdict = rest.max() <= 1 and small
        else:
            verdict = True
        print(f"    -> {'the shader stage behaves as written (apart from the pointer)' if verdict else 'the shader stage does something else'}")


def half_regions(src, post, mode="linear", k=40, min_share=0.001):
    """Flat areas in the half-float surface. For each: its half-float value, the byte the shader makes of it,
    and the byte it WOULD be if the value were already gamma-encoded (value*255): the two readings of the same number."""
    h16 = np.ascontiguousarray(src[..., :3].astype(np.float16)).view(np.uint16).reshape(src.shape[0], src.shape[1], 3)
    key = (h16[..., 0].astype(np.uint64) << 32) | (h16[..., 1].astype(np.uint64) << 16) | h16[..., 2].astype(np.uint64)
    vals, counts = np.unique(key, return_counts=True)
    rows = []
    for i in np.argsort(-counts)[:k]:
        if counts[i] / key.size < min_share:
            break
        ys, xs = np.where(key == vals[i])
        r, g, b = (np.uint16((int(vals[i]) >> sh) & 0xffff).view(np.float16).astype(np.float32) for sh in (32, 16, 0))
        enc = shader_encode_u8(np.array([r, g, b], dtype=np.float32), mode)
        cx, cy = (int(xs.min()) + int(xs.max())) // 2, (int(ys.min()) + int(ys.max())) // 2
        seen = "" if post is None else f"{post[cy, cx, 2]:02x}{post[cy, cx, 1]:02x}{post[cy, cx, 0]:02x}"
        rows.append((int(ys.min()) // 40, int(xs.min()), (float(r), float(g), float(b)), counts[i] / key.size, cx, cy,
                     int(xs.max()) - int(xs.min()) + 1, int(ys.max()) - int(ys.min()) + 1, enc, seen))
    print("  src flat regions (row-major): half-float value R,G,B | share | centre | bbox | shader writes (hex) | if value were already gamma-encoded (hex) | post at centre")
    for _, _, (r, g, b), sh, cx, cy, bw, bh, enc, seen in sorted(rows):
        asis = np.rint(255 * np.clip(np.array([r, g, b]), 0, 1)).astype(int)
        print(f"      {r:7.4f} {g:7.4f} {b:7.4f} | {sh*100:5.2f}% | @{cx},{cy} | {bw}x{bh} | {enc[0]:02x}{enc[1]:02x}{enc[2]:02x} | {asis[0]:02x}{asis[1]:02x}{asis[2]:02x} | {seen}")


def save_half_preview(src, path, mode="linear"):
    Image.fromarray(shader_encode_u8(src[..., :3], mode).astype(np.uint8)).save(path)


def save_png(img_bgra, path):
    Image.fromarray(img_bgra[..., [2, 1, 0]]).save(path)


def analyse(meta_path):
    d = os.path.dirname(meta_path)
    m = json.load(open(meta_path))
    seq, w, h = m["seq"], m["width"], m["height"]
    g = m["gamma"]
    print(f"\n=== capture {seq:03d}  why={m['why']}  t={m['t']}s  mapping {m['mapping']}  {w}x{h}  {m['src_format']} ({m['fourcc']})")
    print(f"  gamma descriptor: offset={g['offset']} length={g['length']} entries={g['entries']} guest_sum={g['sum']} "
          f"found_in_surface={g['found']} (base={g['base']}, checksum_match={g['checksum_match']}: 0 none, 1 u32 sum, 2 byte sum)")
    gp = os.path.join(d, m["files"]["gamma"])
    if os.path.exists(gp):
        print("  gamma table:")
        describe_table(open(gp, "rb").read())
    else:
        print("  gamma table: not found in the surface")

    src_p, post_p = os.path.join(d, m["files"]["src"]), os.path.join(d, m["files"]["post"])
    src = load_bgra(src_p, w, h) if m["src_format"] == "BGRA8" and os.path.exists(src_p) else None
    if src is not None:
        print(f"  src  top colours (rrggbb:share): " + "  ".join(f"{c}:{s:.3f}" for c, s in top_colors(src)))
        print(f"  src  per-channel mean R/G/B: {src[...,2].mean():.2f} {src[...,1].mean():.2f} {src[...,0].mean():.2f}  alpha min/max {src[...,3].min()}/{src[...,3].max()}")
        save_png(src, src_p.replace(".raw", ".png"))
        if "--regions" in sys.argv: print_regions("src ", src)
    elif os.path.exists(src_p) and m["src_format"] == "RGBA16F":
        half = load_half(src_p, w, h)
        post_b = load_bgra(os.path.join(d, m["files"]["post"]), w, h) if os.path.exists(os.path.join(d, m["files"]["post"])) else None
        if half is None:
            print("  src: half-float file has an unexpected size")
        else:
            mode = m.get("color_mode", "linear")  # captures from before the fix (json v1) were all in the old linear behaviour
            print(f"  colour mode of this capture: {mode}")
            half_report(half, post_b, mode)
            half_regions(half, post_b, mode)
            save_half_preview(half, src_p.replace(".raw", ".png"), mode)
    if os.path.exists(post_p):
        post = load_bgra(post_p, w, h)
        if post is None:
            size = os.path.getsize(post_p)
            print(f"  post: {size} bytes, not {w}x{h}x4: the drawable has a different size than the surface")
        else:
            print(f"  post top colours (rrggbb:share): " + "  ".join(f"{c}:{s:.3f}" for c, s in top_colors(post)))
            save_png(post, post_p.replace(".raw", ".png"))
            if "--regions" in sys.argv: print_regions("post", post)
            if src is not None:
                diff = np.abs(src[..., :3].astype(int) - post[..., :3].astype(int))
                print(f"  src vs post (should be 0 when the screen shader is a pass-through): max diff B/G/R = "
                      f"{diff[...,0].max()}/{diff[...,1].max()}/{diff[...,2].max()}, mean {diff.mean():.3f}, "
                      f"pixels differing {100.0 * np.mean(diff.max(axis=2) > 0):.2f}%")
    else:
        print("  post: missing (the presenter never drew a frame after the trigger)")
    print(f"  previews: {src_p.replace('.raw', '.png')}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    metas = sorted(glob.glob(os.path.join(d, "frame-*.json")))
    if not metas:
        sys.exit(f"no frame-*.json in {d}")
    for p in metas:
        analyse(p)


if __name__ == "__main__":
    main()
