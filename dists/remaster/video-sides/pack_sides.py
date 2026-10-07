"""Pack ARP-widened cutscene shots into one side-strip file per video for the game.

Output <out>/<NAME>.sides:
  'CMSD' magic, u16 version=1, u16 side width (104), u16 height (480), u16 reserved, u32 frame count N,
  u32 offsets[N + 1] (absolute; an empty entry means no sides for that frame),
  then per frame: zlib( left strip w*h palette indices, then right strip w*h ).
Frame n is ffmpeg's 0-based frame n (= the game's SMUSH _frame - 1).

Per shot and side, ARP's output is colour-matched to the real video next to that side (ARP grades its whole output, so the new sides are
otherwise a shade off next to the untouched original in the game), the innermost columns are feathered into the
real video's edge, and the strips are quantised to the colours that frame itself uses, so they follow the video's
palette fades.
"""
import glob, json, os, struct, subprocess, sys, zlib
import numpy as np
from PIL import Image

_W = os.environ.get("COMI_VIDEO_WORK") or sys.exit("COMI_VIDEO_WORK is not set; run generate_cutscenes.cmd.")   # see batch_outpaint.py
WORK = os.path.join(_W, "work")
STATUS = os.path.join(_W, "status.json")
SW, H, VW = 104, 480, 640
FEATHER = 16


def read_video(path):
    w = int(subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width",
                            "-of", "csv=p=0", path], capture_output=True, text=True).stdout.strip())
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                         capture_output=True).stdout
    v = np.frombuffer(raw, np.uint8).reshape(-1, H, w, 3)
    if w == 864:
        # ARP renders 16:9 at 864x480 (multiple of 32) but the picture is 854 wide: everything is stretched by
        # 864/854. Undo it, so the real video sits exactly at x 107..747 and the strips join without a step.
        v = np.stack([np.asarray(Image.fromarray(f).resize((854, H), Image.LANCZOS)) for f in v])
    return v


def colour_lut(wide_c, orig):
    """Per-channel 256-entry map from ARP's colours to the real video's, fitted on the shared middle."""
    lut = np.zeros((3, 256), np.float32)
    idx = np.arange(256)
    for ch in range(3):
        x = wide_c[..., ch].ravel()
        y = orig[..., ch].ravel()
        cnt = np.bincount(x, minlength=256)
        med = np.full(256, np.nan, np.float32)
        order = np.argsort(x, kind="stable")
        xs, ys = x[order], y[order]
        starts = np.searchsorted(xs, idx)
        ends = np.searchsorted(xs, idx, side="right")
        for v in np.nonzero(cnt > 20)[0]:
            med[v] = np.median(ys[starts[v]:ends[v]])
        good = ~np.isnan(med)
        if good.sum() < 2:
            lut[ch] = idx
        else:
            lut[ch] = np.interp(idx, idx[good], med[good])
    return lut


_lut_cache = {}
BITS = 6  # colour lookup grid: 64 levels per channel


def _palette_lut(pal, used):
    """Nearest allowed palette index for every point of a 64x64x64 RGB grid (cached: palettes rarely change)."""
    key = pal[used].tobytes() + used.tobytes()
    lut = _lut_cache.get(key)
    if lut is None:
        if len(_lut_cache) > 64:
            _lut_cache.clear()
        step = 256 // (1 << BITS)
        g = (np.arange(1 << BITS) * step + step // 2).astype(np.int32)
        grid = np.stack(np.meshgrid(g, g, g, indexing="ij"), -1).reshape(-1, 3)
        cols = pal[used].astype(np.int32)
        lut = np.empty(len(grid), np.uint8)
        for s in range(0, len(grid), 8192):
            d = ((grid[s:s + 8192, None, :] - cols[None, :, :]) ** 2 * np.array([3, 4, 2])).sum(2)
            lut[s:s + 8192] = used[d.argmin(1)]
        _lut_cache[key] = lut
    return lut


def quantise(rgb, pal, used):
    # Match only the distinct (6-bit per channel) colours present: fast even when the palette changes every frame
    # (fades, the opening credits), where a per-palette lookup table would be rebuilt constantly.
    q = (np.clip(rgb, 0, 255).astype(np.int32) >> (8 - BITS))
    codes = ((q[..., 0] << (2 * BITS)) | (q[..., 1] << BITS) | q[..., 2]).ravel()
    uniq, inv = np.unique(codes, return_inverse=True)
    step = 256 // (1 << BITS)
    ucol = np.stack([(uniq >> (2 * BITS)) & 63, (uniq >> BITS) & 63, uniq & 63], 1) * step + step // 2
    cols = pal[used].astype(np.int32)
    best = np.empty(len(uniq), np.uint8)
    for s in range(0, len(uniq), 4096):
        d = ((ucol[s:s + 4096, None, :] - cols[None, :, :]) ** 2 * np.array([3, 4, 2])).sum(2)
        best[s:s + 4096] = used[d.argmin(1)]
    return best[inv].reshape(rgb.shape[:2])


EDGE = 64  # original columns next to each join that define that side's colours and colour curve


def quantise_pref(rgb, pal, near, everything, tol=24):
    """Colours used near that edge where they fit (keeps water and sky in the right shades), otherwise any colour of
    the frame (objects such as the island that barely occur at the edge would otherwise turn into a mosaic)."""
    qa = quantise(rgb, pal, near)
    qb = quantise(rgb, pal, everything)
    err = np.abs(pal[qa].astype(np.int32) - np.clip(rgb, 0, 255).astype(np.int32)).max(-1)
    return np.where(err > tol, qb, qa)


def shot_blobs(wide, frame_paths):
    off = (wide.shape[2] - VW) // 2
    sample = list(range(0, len(frame_paths), max(1, len(frame_paths) // 12)))
    orig_s = np.stack([np.asarray(Image.open(frame_paths[i]).convert("RGB")) for i in sample])
    wide_c = wide[sample, :, off:off + VW].astype(np.int64)
    # colour curves fitted on the part of the picture next to each side (the water by the left edge is matched by the
    # water by the left edge, not by the sky in the middle)
    lut_l = colour_lut(wide_c[:, :, :200], orig_s[:, :, :200])
    lut_r = colour_lut(wide_c[:, :, -200:], orig_s[:, :, -200:])
    F = FEATHER
    alpha = np.array([0.75 * (1 - k / F) for k in range(F)], np.float32)[None, :, None]
    blobs = []
    for i in range(min(len(frame_paths), len(wide))):
        im = Image.open(frame_paths[i])
        pal = np.array(im.getpalette()[:768], np.uint8).reshape(256, 3)
        idx = np.asarray(im)
        orig = pal[idx].astype(np.float32)
        used_l = np.unique(idx[:, :EDGE])
        used_r = np.unique(idx[:, -EDGE:])
        wl = np.stack([lut_l[ch][wide[i, :, off - SW:off, ch]] for ch in range(3)], -1)
        wr = np.stack([lut_r[ch][wide[i, :, off + VW:off + VW + SW, ch]] for ch in range(3)], -1)
        wl[:, SW - F:] = wl[:, SW - F:] * (1 - alpha[:, ::-1]) + orig[:, F - 1::-1] * alpha[:, ::-1]
        wr[:, :F] = wr[:, :F] * (1 - alpha) + orig[:, VW - 1:VW - 1 - F:-1] * alpha
        used_all = np.unique(idx)
        ql = quantise_pref(np.clip(wl, 0, 255), pal, used_l, used_all)
        qr = quantise_pref(np.clip(wr, 0, 255), pal, used_r, used_all)
        blobs.append(zlib.compress(np.ascontiguousarray(ql).tobytes() + np.ascontiguousarray(qr).tobytes(), 9))
    return blobs


def write_file(path, blobs):
    n = len(blobs)
    head = b"CMSD" + struct.pack("<HHHHI", 1, SW, H, 0, n)
    offs, pos = [], len(head) + 4 * (n + 1)
    for bl in blobs:
        offs.append(pos)
        pos += len(bl)
    offs.append(pos)
    with open(path, "wb") as f:
        f.write(head + struct.pack("<%dI" % (n + 1), *offs))
        for bl in blobs:
            f.write(bl)


def pack(name, out_dir):
    st = json.load(open(STATUS))[name]
    frames = sorted(glob.glob(os.path.join(WORK, name, "frames", "*.png")))
    blobs = [b""] * len(frames)
    for key, wide_path in sorted(st.get("done", {}).items()):
        a, b = (int(x) for x in key.split("_"))
        for i, bl in enumerate(shot_blobs(read_video(wide_path), frames[a:b])):
            blobs[a + i] = bl
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, name.upper() + ".sides")
    write_file(path, blobs)
    have = sum(1 for bl in blobs if bl)
    print(f"{name}: {have}/{len(frames)} frames with sides, {os.path.getsize(path) / 1e6:.1f} MB", flush=True)


if __name__ == "__main__":
    out = sys.argv[1]
    names = sys.argv[2:] or sorted(json.load(open(STATUS)).keys())
    for nm in names:
        pack(nm, out)
