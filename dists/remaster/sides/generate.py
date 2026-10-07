r"""Generate widescreen side art from YOUR OWN copy of The Curse of Monkey Island, on your own PC.

Nothing derived from the game's art is distributed with scummvm-ai-upscale; this makes it locally instead:
  1. extract  - the game saves each narrow room's background (remaster_extract_dir mode, one short minimised run per
                room, about 5 s each)
  2. paint    - an AI model continues each background outwards to 16:9:
                  cloud: Google Gemini through your own OpenRouter account (best results; about 7 US cents per room;
                         the room backgrounds are sent to OpenRouter/Google)
                  local: Stable Diffusion XL inpainting on your NVIDIA GPU (free, offline, ~7 GB model download;
                         simpler results)
  3. compose  - aligns the painted sides with the room and blends the seam (compose.py), writes sides\\NNNN.png
Resumable: finished rooms are kept; a room whose sides\NNNN.png you delete (or --regenerate) is painted anew.

usage: generate.py --install <install folder> [--method cloud|local] [--rooms 9,10,...] [--regenerate]
                   [--recompose] [--status] [--max-cost USD] [--yes]
exit code: 0 all requested rooms made, 2 some rooms failed (listed; run again to retry), 1 nothing done.
  The OpenRouter key is read from %OPENROUTER_API_KEY% or %USERPROFILE%\\.openrouter_key (never stored elsewhere).
"""
import argparse
import base64
import io
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
M = 104
# Rooms 640 wide whose sides can be painted (wide rooms show more of the room instead; rooms with characters or
# animation crossing the edges keep the ambient glow). The list the original set was reviewed for.
ROOMS = [9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 39, 52, 54,
         56, 57, 58, 59, 62, 63, 64, 65, 66, 67, 68, 69, 71, 72, 73, 74, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 89, 90, 94]
CLOUD_MODEL = "google/gemini-3.1-flash-image"   # the model the reviewed set was made with
PROMPT = (
    "This is a 16:9 canvas with a hand-painted cartoon adventure game background in the middle and flat grey "
    "bars on the left and right. Paint ONLY the grey bars: continue the scene naturally outwards (walls, "
    "scenery, sky, ground) so the joins are invisible. Keep the middle picture exactly as it is - same position, "
    "scale, details and colours. Objects cut by the picture's edges must continue correctly and never appear "
    "twice. Do not mirror or copy parts of the picture. Same painted cartoon style, line work and lighting. "
    "No people, no characters, no text, no borders. Return the full 16:9 image.")


def say(msg):
    print(msg, flush=True)


# ── 1. extract ──────────────────────────────────────────────────────────────────────────────────────────────────
def extract(install, work, rooms):
    exe = os.path.join(install, "scummvm.exe" if os.name == "nt" else "scummvm")
    ini = os.path.join(install, "scummvm.ini")
    if not os.path.exists(exe) or not os.path.exists(ini):
        sys.exit(f"No installation found in {install} (scummvm.exe and scummvm.ini).")
    lines = [l.rstrip("\n") for l in open(ini, encoding="utf-8", errors="replace")]
    drop = ("fullscreen=", "mute=", "savepath=", "autosave_period=", "remaster_extract_dir=", "remaster_update_check=",
            "remaster_bg_prewarm=", "remaster_widescreen=")
    lines = [l for l in lines if not l.startswith(drop)]
    i = lines.index("[comi]") if "[comi]" in lines else -1
    if i < 0:
        sys.exit("scummvm.ini has no [comi] section; run the installer first.")
    saves = os.path.join(work, "saves")
    os.makedirs(saves, exist_ok=True)
    lines[i + 1:i + 1] = ["fullscreen=false", "mute=true", "autosave_period=0", "remaster_update_check=false",
                          f"savepath={saves}", f"remaster_extract_dir={work}", "remaster_widescreen=wide"]
    cfg = os.path.join(work, "extract.ini")
    open(cfg, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    todo = [r for r in rooms if not os.path.exists(os.path.join(work, f"room_{r:04d}.png"))]
    for k, r in enumerate(todo):
        say(f"[extract] room {r} ({k + 1}/{len(todo)})")
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        p = subprocess.Popen([exe, f"--config={cfg}", f"--logfile={os.path.join(work, 'extract.log')}", f"--boot-param={r}", "comi"],
                             cwd=work, creationflags=flags)
        try:
            p.wait(60)
        except subprocess.TimeoutExpired:
            p.kill()
            say(f"[extract] room {r}: timed out, skipped")


# ── 2. paint ────────────────────────────────────────────────────────────────────────────────────────────────────
def canvas_for(room_png, scale=2):
    orig = Image.open(room_png).convert("RGB")
    W, H = orig.size
    big = orig.resize((W * scale, H * scale), Image.LANCZOS)
    WW = (W + 2 * M) * scale
    canvas = Image.new("RGB", (WW, H * scale), (128, 128, 128))
    canvas.paste(big, (M * scale, 0))
    return orig, big, canvas


def openrouter_key():
    k = os.environ.get("OPENROUTER_API_KEY", "").strip()
    if not k:
        p = os.path.join(os.path.expanduser("~"), ".openrouter_key")
        if os.path.exists(p):
            k = open(p).read().strip()
    return k


def paint_cloud(room_png, out_png, key):
    _, _, canvas = canvas_for(room_png)
    buf = io.BytesIO()
    canvas.save(buf, "PNG")
    body = {"model": CLOUD_MODEL, "modalities": ["image", "text"],
            "messages": [{"role": "user", "content": [
                {"type": "text", "text": PROMPT},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()}}]}],
            "usage": {"include": True}}
    req = urllib.request.Request("https://openrouter.ai/api/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json",
                                          "X-Title": "scummvm-ai-upscale side art"})
    with urllib.request.urlopen(req, timeout=300) as r:
        resp = json.loads(r.read())
    msg = resp["choices"][0]["message"]
    images = msg.get("images") or []
    if not images:
        raise RuntimeError("no image returned: " + str(msg.get("content"))[:200])
    img = Image.open(io.BytesIO(base64.b64decode(images[0]["image_url"]["url"].split(",", 1)[1]))).convert("RGB")
    img.save(out_png)
    return (resp.get("usage") or {}).get("cost") or 0.0


_pipe = None


def paint_local(room_png, out_png):
    global _pipe
    import torch
    from diffusers import StableDiffusionXLInpaintPipeline
    from PIL import ImageFilter
    if _pipe is None:
        say("[paint] loading Stable Diffusion XL (first run downloads about 7 GB)...")
        _pipe = StableDiffusionXLInpaintPipeline.from_pretrained("stabilityai/stable-diffusion-xl-base-1.0", variant="fp16",
                                                                 torch_dtype=torch.float16)
        try:
            _pipe.enable_model_cpu_offload()   # less VRAM (needs the accelerate package)
        except ImportError:
            _pipe.to("cuda")                   # fits on 12 GB+ cards
        _pipe.vae.enable_tiling()
        _pipe.set_progress_bar_config(disable=True)
    orig = Image.open(room_png).convert("RGB")
    W, H = orig.size
    WW = W + 2 * M
    canvas = Image.new("RGB", (WW, H))
    canvas.paste(orig, (M, 0))
    canvas.paste(orig.crop((0, 0, M, H)).transpose(Image.FLIP_LEFT_RIGHT), (0, 0))
    canvas.paste(orig.crop((W - M, 0, W, H)).transpose(Image.FLIP_LEFT_RIGHT), (M + W, 0))
    mask = Image.new("L", (WW, H), 0)
    mask.paste(255, (0, 0, M + 8, H))
    mask.paste(255, (M + W - 8, 0, WW, H))
    gw, gh = WW * 2 // 8 * 8, H * 2 // 8 * 8
    img = _pipe(prompt="seamless continuation of the same hand-painted cartoon scene, the same objects, walls and materials "
                       "extended to the side, same lighting and colours, detailed 2D painting",
                negative_prompt="people, characters, text, letters, frame, border, blurry, photo, 3d render, new objects",
                image=canvas.resize((gw, gh), Image.BICUBIC), mask_image=mask.resize((gw, gh)).filter(ImageFilter.GaussianBlur(6)),
                width=gw, height=gh, strength=0.65, guidance_scale=6.5, num_inference_steps=35,
                generator=torch.Generator("cuda").manual_seed(LOCAL_SEED)).images[0]
    img.resize((WW * 2, H * 2), Image.LANCZOS).save(out_png)
    return 0.0


# ── 3. compose ──────────────────────────────────────────────────────────────────────────────────────────────────
def compose_room(work, room, sides_dir):
    import compose
    big = Image.open(os.path.join(work, f"room_{room:04d}.png")).convert("RGB")
    # compose.py reads room_NNNN.png (2x) and wide_NNNN.png from one folder and writes <folder>/sides2/NNNN.png
    cdir = os.path.join(work, "compose")
    os.makedirs(cdir, exist_ok=True)
    big.resize((big.width * 2, big.height * 2), Image.LANCZOS).save(os.path.join(cdir, f"room_{room:04d}.png"))
    Image.open(os.path.join(work, f"wide_{room:04d}.png")).save(os.path.join(cdir, f"wide_{room:04d}.png"))
    r = compose.compose(cdir, f"{room:04d}")
    if r is None:
        return False
    os.makedirs(sides_dir, exist_ok=True)
    os.replace(os.path.join(cdir, "sides2", f"{room:04d}.png"), os.path.join(sides_dir, f"{room:04d}.png"))
    return True


# ── bookkeeping ─────────────────────────────────────────────────────────────────────────────────────────────────
# sides-work\meta_NNNN.json records how a room's painting was made: method, model, prompt hash, seed, the source
# room's SHA-256, cost, and whether it was composed into sides\. A painting is reused only if it matches the current
# method and source; deleting a finished sides\NNNN.png means "make a new one".
LOCAL_SEED = 7


def sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def gen_params(method):
    import hashlib
    if method == "cloud":
        return {"method": "cloud", "model": CLOUD_MODEL, "prompt": hashlib.sha256(PROMPT.encode()).hexdigest()[:16], "seed": None}
    return {"method": "local", "model": "stabilityai/stable-diffusion-xl-base-1.0", "prompt": "sdxl-v1", "seed": LOCAL_SEED}


def meta_path(work, r):
    return os.path.join(work, f"meta_{r:04d}.json")


def read_meta(work, r):
    try:
        with open(meta_path(work, r), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def write_meta(work, r, m):
    with open(meta_path(work, r), "w", encoding="utf-8") as f:
        json.dump(m, f, indent=1)


def room_state(work, sides_dir, r, params):
    """'done', 'painted' (a matching painting waits to be composed), 'stale' (painting made differently or from another
    source), 'redo' (finished file was deleted), 'extracted' or 'new'."""
    done = os.path.exists(os.path.join(sides_dir, f"{r:04d}.png"))
    m = read_meta(work, r)
    wide = os.path.exists(os.path.join(work, f"wide_{r:04d}.png"))
    src = os.path.join(work, f"room_{r:04d}.png")
    if done:
        return "done"
    if wide and m.get("composed"):
        return "redo"
    if wide:
        same = all(m.get(k) == v for k, v in params.items()) and os.path.exists(src) and m.get("source") == sha256(src)
        return "painted" if same else "stale"
    return "extracted" if os.path.exists(src) else "new"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True, help="the scummvm-ai-upscale install folder")
    ap.add_argument("--method", choices=["cloud", "local"])
    ap.add_argument("--rooms", help="comma-separated room numbers (default: all narrow rooms)")
    ap.add_argument("--regenerate", action="store_true", help="paint the selected rooms again even if they are done")
    ap.add_argument("--recompose", action="store_true", help="only re-run the alignment/blend on existing paintings")
    ap.add_argument("--status", action="store_true", help="list each room's state and exit")
    ap.add_argument("--max-cost", type=float, default=None, help="cloud: stop before the spending passes this many USD")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    a = ap.parse_args()
    install = os.path.abspath(a.install)
    work = os.path.join(install, "sides-work")
    sides_dir = os.path.join(install, "sides")
    os.makedirs(work, exist_ok=True)
    rooms = [int(x) for x in a.rooms.split(",")] if a.rooms else ROOMS
    method = a.method

    if a.status:
        params = gen_params(method or "cloud")
        for r in rooms:
            m = read_meta(work, r)
            say(f"room {r:3d}: {room_state(work, sides_dir, r, params):9s} " + (f"({m.get('method')}, {m.get('model')})" if m else ""))
        return 0

    if a.recompose:
        n = 0
        for r in rooms:
            if os.path.exists(os.path.join(work, f"wide_{r:04d}.png")) and os.path.exists(os.path.join(work, f"room_{r:04d}.png")):
                n += compose_room(work, r, sides_dir)
        say(f"Recomposed {n} rooms.")
        return 0

    if not method:
        say("How should the sides be painted?")
        say("  1  cloud: Google Gemini through your own OpenRouter account. Best results, about $0.07 per room.")
        say("     The room backgrounds are sent to OpenRouter/Google.")
        say("  2  local: Stable Diffusion XL on your NVIDIA graphics card. Free and offline; about 7 GB download,")
        say("     about 15 s per room on a fast card; simpler results.")
        method = "local" if input("Choose 1 or 2 [1]: ").strip() == "2" else "cloud"
    params = gen_params(method)
    if a.regenerate:
        for r in rooms:
            for f in (os.path.join(sides_dir, f"{r:04d}.png"), os.path.join(work, f"wide_{r:04d}.png")):
                if os.path.exists(f):
                    os.remove(f)
    states = {r: room_state(work, sides_dir, r, params) for r in rooms}
    todo = [r for r in rooms if states[r] != "done"]
    to_paint = [r for r in todo if states[r] != "painted"]
    summary = {"done_before": len(rooms) - len(todo), "new": [], "skipped": [], "failed": {}, "cost": 0.0}
    if not todo:
        say(f"Side art is already there for all {len(rooms)} selected rooms ({sides_dir}). "
            "Use --regenerate to make new ones.")
        return 0
    key = ""
    if method == "cloud" and to_paint:
        key = openrouter_key()
        if not key:
            say("An OpenRouter API key is needed (https://openrouter.ai/keys). It stays on this PC and is only sent to OpenRouter.")
            key = input("Paste your OpenRouter key (or set OPENROUTER_API_KEY): ").strip()
            if not key:
                say("No key; nothing done.")
                return 1
    if not a.yes:
        est = f" (about ${0.07 * len(to_paint):.2f})" if method == "cloud" and to_paint else ""
        if input(f"Make side art for {len(todo)} rooms with the {method} method{est}? [Y/n]: ").strip().lower() in ("n", "no"):
            say("Nothing done.")
            return 1
    t0 = time.time()
    extract(install, work, [r for r in todo if not os.path.exists(os.path.join(work, f"room_{r:04d}.png"))])
    for k, r in enumerate(todo):
        room_png = os.path.join(work, f"room_{r:04d}.png")
        wide = os.path.join(work, f"wide_{r:04d}.png")
        if not os.path.exists(room_png):
            summary["failed"][r] = "the game did not save this room"
            continue
        with Image.open(room_png) as im:
            wide_room = im.width > 640
        if wide_room:
            say(f"[paint] room {r} is wider than the screen, skipped (it shows more of the room instead)")
            summary["skipped"].append(r)
            continue
        if method == "cloud" and a.max_cost is not None and states[r] != "painted" and summary["cost"] + 0.07 > a.max_cost:
            say(f"[paint] spending limit ${a.max_cost:.2f} reached; stopping (run again to continue)")
            summary["skipped"] += [x for x in todo[k:] if x not in summary["skipped"]]
            break
        say(f"[paint] room {r} ({k + 1}/{len(todo)}, {method})")
        try:
            if states[r] != "painted":
                if os.path.exists(wide):
                    os.remove(wide)
                c = paint_cloud(room_png, wide, key) if method == "cloud" else paint_local(room_png, wide)
                summary["cost"] += c or 0.0
                write_meta(work, r, dict(params, source=sha256(room_png), cost=c, made=time.strftime("%Y-%m-%d %H:%M"), composed=False))
            if compose_room(work, r, sides_dir):
                m = read_meta(work, r)
                m["composed"] = True
                write_meta(work, r, m)
                summary["new"].append(r)
            else:
                summary["failed"][r] = "the painted picture could not be aligned with the room"
                os.remove(wide)
        except (urllib.error.URLError, RuntimeError, OSError, ValueError, KeyError) as e:
            summary["failed"][r] = str(e)[:200]
            say(f"[paint] room {r} failed: {e}")
    summary["minutes"] = round((time.time() - t0) / 60, 1)
    with open(os.path.join(work, "last_run.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    say(f"Done in {summary['minutes']:.0f} min: {len(summary['new'])} rooms made" +
        (f" (cost about ${summary['cost']:.2f})" if summary["cost"] else "") +
        (f", {len(summary['skipped'])} skipped" if summary["skipped"] else "") +
        (f", {len(summary['failed'])} failed: " + ", ".join(f"{r} ({why})" for r, why in summary["failed"].items()) if summary["failed"] else "") +
        f". Side art: {sides_dir}")
    if summary["failed"]:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
