"""Ocean shots: build the side strips' water from the video's own water.

Below the horizon each side strip is assembled from two pieces of the real frame (exact palette indices):
  - next to the join, the frame's edge mirrored outward (continuous with the picture at the join),
  - further out, a band of the frame's own water copied from further inside (same style, moves like the real
    water every frame).
Between the two, a per-frame minimum-error seam (dynamic programming, one column step per row) picks the least
visible boundary. Above the horizon the AI-painted sky from the existing strips is kept.
Usage: ocean_synth.py <NAME> <sides file> a:b [a:b ...]   (frame ranges = shots)
"""
import glob, os, struct, sys, zlib
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(__file__))
import pack_sides as P

SW, H, VW = P.SW, P.H, P.VW
ZONE = (56, 100)        # seam zone, in columns measured from the outer edge of the strip (left strip coords)


def water_rows(rgb):
    """Per row: is the outer edge water? (blue-ish, darker than the green sky)."""
    e = np.concatenate([rgb[:, :12], rgb[:, -12:]], 1).astype(np.float32)
    r, g, b = e[..., 0].mean(1), e[..., 1].mean(1), e[..., 2].mean(1)
    return (b >= g * 0.85) & (g < 120)


def horizon(rgb):
    w = water_rows(rgb)
    w = np.convolve(w.astype(np.float32), np.ones(9) / 9, mode="same") > 0.5
    rows = np.nonzero(w[100:])[0]
    return 100 + int(rows[0]) if len(rows) else H


def seam(cost):
    """Vertical seam through cost (h x w): column per row, moving at most one column per row."""
    h, w = cost.shape
    acc = cost.copy()
    for y in range(1, h):
        prev = acc[y - 1]
        best = np.minimum(prev, np.minimum(np.r_[np.inf, prev[:-1]], np.r_[prev[1:], np.inf]))
        acc[y] += best
    path = np.empty(h, np.int32)
    path[-1] = int(acc[-1].argmin())
    for y in range(h - 2, -1, -1):
        c = path[y + 1]
        lo, hi = max(0, c - 1), min(w, c + 2)
        path[y] = lo + int(acc[y, lo:hi].argmin())
    return path


def build_side(idx, pal, hz, dx, left):
    """Strip (H x SW) of palette indices for rows hz.. (rows above hz are left 0, filled by the caller)."""
    rows = idx[hz:]
    if left:
        mirror = rows[:, :SW][:, ::-1]                       # strip col x <- frame col SW-1-x
        copy = rows[:, dx - SW:dx]                           # strip col x <- frame col x + dx - SW
    else:
        mirror = rows[:, VW - SW:][:, ::-1]                  # strip col x <- frame col VW-1-x
        copy = rows[:, VW - dx:VW - dx + SW]                 # strip col x <- frame col VW - dx + x
        mirror, copy = mirror[:, ::-1], copy[:, ::-1]        # work in "outer edge first" order like the left
    a, b = ZONE
    cost = np.abs(pal[mirror[:, a:b]].astype(np.int32) - pal[copy[:, a:b]].astype(np.int32)).sum(2).astype(np.float64)
    cut = seam(cost) + a                                     # cols < cut: copy, cols >= cut: mirror
    cols = np.arange(SW)[None, :]
    out = np.where(cols < cut[:, None], copy, mirror)
    return out if left else out[:, ::-1]


def choose_dx(frames, a, b, hz, left):
    """Copy offset with the least mismatch against the mirror in the seam zone, over a few frames of the shot."""
    best = None
    sample = list(range(a, b, max(1, (b - a) // 8)))
    for dx in range(SW + 24, 420, 6):
        tot = 0.0
        for i in sample:
            im = Image.open(frames[i])
            pal = np.array(im.getpalette()[:768], np.uint8).reshape(256, 3).astype(np.int32)
            rows = np.asarray(im)[hz:]
            za, zb = ZONE
            if left:
                m = rows[:, :SW][:, ::-1][:, za:zb]
                c = rows[:, dx - SW:dx][:, za:zb]
            else:
                m = rows[:, VW - SW:][:, ::-1][:, ::-1][:, za:zb]
                c = rows[:, VW - dx:VW - dx + SW][:, ::-1][:, za:zb]
            tot += np.abs(pal[m] - pal[c]).sum(2).min(1).mean()
        if best is None or tot < best[0]:
            best = (tot, dx)
    return best[1]


def patch(sides_path, frames_dir, ranges):
    data = open(sides_path, "rb").read()
    n = struct.unpack("<I", data[12:16])[0]
    offs = struct.unpack("<%dI" % (n + 1), data[16:16 + 4 * (n + 1)])
    blobs = [data[offs[i]:offs[i + 1]] for i in range(n)]
    frames = sorted(glob.glob(os.path.join(frames_dir, "*.png")))
    for a, b in ranges:
        hz = horizon(np.asarray(Image.open(frames[(a + b) // 2]).convert("RGB")))
        dxl = choose_dx(frames, a, b, hz, True)
        dxr = choose_dx(frames, a, b, hz, False)
        for i in range(a, b):
            if not blobs[i]:
                continue
            q = np.frombuffer(zlib.decompress(blobs[i]), np.uint8)
            left = q[:SW * H].reshape(H, SW).copy()
            right = q[SW * H:].reshape(H, SW).copy()
            im = Image.open(frames[i])
            pal = np.array(im.getpalette()[:768], np.uint8).reshape(256, 3)
            idx = np.asarray(im)
            hzi = horizon(pal[idx])
            # Tilted camera: the edges' horizons differ; the synthesis assumes a level sea, keep the AI strips.
            rgbf = pal[idx]
            if abs(horizon(np.concatenate([rgbf[:, :24]] * 2, 1)) - horizon(np.concatenate([rgbf[:, -24:]] * 2, 1))) > 6:
                continue
            rgb = pal[idx].astype(np.float32)
            def pure_water(band):
                r, g, b = band[..., 0], band[..., 1], band[..., 2]
                return ((b >= g * 0.85) & (g < 120)).mean(1) > 0.97
            okl = pure_water(rgb[hzi:, :dxl])          # everything the left strip is built from is water
            okr = pure_water(rgb[hzi:, VW - dxr:])
            sl = build_side(idx, pal, hzi, dxl, True)
            sr = build_side(idx, pal, hzi, dxr, False)
            left[hzi:] = np.where(okl[:, None], sl, left[hzi:])
            right[hzi:] = np.where(okr[:, None], sr, right[hzi:])
            blobs[i] = zlib.compress(left.tobytes() + right.tobytes(), 9)
        print(f"shot {a}-{b}: horizon ~{hz}, copy offsets left {dxl} right {dxr}", flush=True)
    P.write_file(sides_path, blobs)


if __name__ == "__main__":
    name, path = sys.argv[1], sys.argv[2]
    patch(path, os.path.join(P.WORK, name, "frames"), [tuple(int(x) for x in r.split(":")) for r in sys.argv[3:]])
