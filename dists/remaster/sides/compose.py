"""Compose side art with LOCAL alignment at the joins.

Like compose.py (global scale/offset fit), then for each side: optical flow between the AI's rendition of the
room's edge band and the real edge band gives a per-row displacement at the join; the painted strip is warped by
that displacement (full at the join, fading to 0 over FADE px outwards), so lines meet their originals.

usage: compose2.py DIR ROOM [ROOM ...]   (DIR contains room_NNNN.png (2x) and wide_NNNN.png; writes DIR/sides2/)
"""
import os
import sys

import cv2
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
M, BAND, FADE = 104, 24, 70


def fit(orig, wide):
    og = cv2.cvtColor(orig, cv2.COLOR_RGB2GRAY).astype(np.float32)
    H, W = og.shape
    best = (-2, None)
    for k in np.arange(1.50, 2.15, 0.01):
        wk = cv2.resize(wide, (round(wide.shape[1] / k), round(wide.shape[0] / k)), interpolation=cv2.INTER_AREA)
        if wk.shape[0] < H or wk.shape[1] < W + 2 * M:
            continue
        r = cv2.matchTemplate(cv2.cvtColor(wk, cv2.COLOR_RGB2GRAY).astype(np.float32), og, cv2.TM_CCOEFF_NORMED)
        r[:, :M] = -2
        r[:, max(0, wk.shape[1] - W - M + 1):] = -2
        _, mx, _, loc = cv2.minMaxLoc(r)
        if mx > best[0]:
            best = (mx, (k, loc))
    return best


def compose(d, room):
    orig2 = np.asarray(Image.open(os.path.join(d, f"room_{room}.png")).convert("RGB"))
    orig = cv2.resize(orig2, (orig2.shape[1] // 2, orig2.shape[0] // 2), interpolation=cv2.INTER_AREA)
    wide = np.asarray(Image.open(os.path.join(d, f"wide_{room}.png")).convert("RGB"))
    H, W = orig.shape[:2]
    res = fit(orig, wide)
    if res[1] is None:
        return None
    score, (k, (lx, ly)) = res
    # Work at 2x for sub-pixel accuracy, then downscale at the end.
    S = 2
    wk = cv2.resize(wide, (round(wide.shape[1] / k * S), round(wide.shape[0] / k * S)), interpolation=cv2.INTER_CUBIC)
    o2 = cv2.resize(orig, (W * S, H * S), interpolation=cv2.INTER_CUBIC).astype(np.float32)
    HH, WW = H * S, (W + 2 * M) * S
    ys = np.clip(np.arange(HH) + ly * S, 0, wk.shape[0] - 1)
    xs = np.clip(np.arange(WW) - M * S + lx * S, 0, wk.shape[1] - 1)
    canvas = wk[ys][:, xs].astype(np.float32)
    out = canvas.copy()
    shifts = []
    for side in (0, 1):
        bw = 48 * S
        if side == 0:
            real = o2[:, :bw]
            ai = canvas[:, M * S:M * S + bw]
        else:
            real = o2[:, W * S - bw:]
            ai = canvas[:, (M + W) * S - bw:(M + W) * S]
        g1 = cv2.cvtColor(ai.astype(np.uint8), cv2.COLOR_RGB2GRAY)
        g2 = cv2.cvtColor(real.astype(np.uint8), cv2.COLOR_RGB2GRAY)
        flow = cv2.calcOpticalFlowFarneback(g2, g1, None, 0.5, 4, 31, 5, 7, 1.5, 0)  # real -> ai displacement
        # Displacement at the join: average over the 12 columns nearest the picture edge, smoothed vertically.
        cols = slice(0, 12 * S) if side == 0 else slice(bw - 12 * S, bw)
        dxy = flow[:, cols].mean(axis=1)
        dxy = cv2.GaussianBlur(dxy.reshape(HH, 1, 2), (1, 41), 0).reshape(HH, 2)
        shifts.append(float(np.abs(dxy).mean()) / S)
        # Warp the strip (and the band inside the edge) so AI content lands on the real positions.
        x0, x1 = (0, (M + 24) * S) if side == 0 else ((M + W - 24) * S, WW)
        xx, yy = np.meshgrid(np.arange(x0, x1, dtype=np.float32), np.arange(HH, dtype=np.float32))
        edge = M * S if side == 0 else (M + W) * S
        dist = np.abs(xx - edge)
        wgt = np.clip(1 - dist / (FADE * S), 0, 1)
        mapx = xx + dxy[:, 0:1] * wgt
        mapy = yy + dxy[:, 1:2] * wgt
        warped = cv2.remap(canvas, mapx, mapy, cv2.INTER_CUBIC, borderMode=cv2.BORDER_REFLECT)
        out[:, x0:x1] = warped
    # Join each side with (1) a minimum-error seam inside the room's edge band: AI pixels on the outside of the
    # seam, real pixels on the inside, no cross-fade (no ghosting); (2) an offset field: the per-row colour
    # difference along the seam is added to the AI side, fading out over 80 px (no brightness step).
    SB = 40 * S
    final = out.copy()
    final[:, M * S:(M + W) * S] = o2
    for side in (0, 1):
        if side == 0:
            b0 = M * S
            ai_b, real_b = out[:, b0:b0 + SB], o2[:, :SB]
        else:
            b0 = (M + W) * S - SB
            ai_b, real_b = out[:, b0:b0 + SB], o2[:, W * S - SB:]
        err = np.abs(ai_b - real_b).sum(axis=2)
        err = cv2.GaussianBlur(err, (5, 5), 0)
        # Seam must start near the outer edge of the band at the top? No: free column per row, smooth path (DP).
        cost = err.copy()
        back = np.zeros(cost.shape, np.int32)
        for y in range(1, HH):
            prev = cost[y - 1]
            l = np.r_[np.inf, prev[:-1]]
            r = np.r_[prev[1:], np.inf]
            stack = np.vstack([l, prev, r])
            i = stack.argmin(axis=0)
            back[y] = i - 1
            cost[y] += stack[i, np.arange(SB)]
        seam = np.zeros(HH, np.int32)
        seam[-1] = int(cost[-1].argmin())
        for y in range(HH - 1, 0, -1):
            seam[y - 1] = np.clip(seam[y] + back[y, seam[y]], 0, SB - 1)
        # Colour difference along the seam (real - ai), smoothed vertically.
        diff = np.array([real_b[y, seam[y]] - ai_b[y, seam[y]] for y in range(HH)], np.float32)
        diff = cv2.GaussianBlur(diff.reshape(HH, 1, 3), (1, 61), 0).reshape(HH, 3)
        # AI part: everything outside the seam (strip + band up to the seam).
        cols = np.arange(WW)
        for y in range(HH):
            xs = b0 + seam[y]
            if side == 0:
                sel = cols < xs
                dist = xs - cols[sel]
            else:
                sel = cols > xs
                dist = cols[sel] - xs
            fade = np.clip(1 - dist / (80.0 * S), 0, 1)[:, None]
            final[y, sel] = out[y, sel] + diff[y] * fade
    out = final
    res_img = cv2.resize(np.clip(out, 0, 255).astype(np.uint8), (W + 2 * M, H), interpolation=cv2.INTER_AREA)
    os.makedirs(os.path.join(d, "sides2"), exist_ok=True)
    Image.fromarray(res_img).save(os.path.join(d, "sides2", f"{room}.png"))
    return score, shifts


if __name__ == "__main__":
    d = sys.argv[1]
    for room in sys.argv[2:]:
        r = compose(d, room)
        if r is None:
            print(f"room {room}: fit failed")
        else:
            print(f"room {room}: fit score {r[0]:.3f}, join correction left {r[1][0]:.1f}px right {r[1][1]:.1f}px")
