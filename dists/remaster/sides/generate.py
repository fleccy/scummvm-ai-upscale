"""Generate widescreen side art from YOUR OWN copy of The Curse of Monkey Island, on your own PC.

Nothing derived from the game's art is distributed with scummvm-ai-upscale; this makes it locally instead:
  1. extract  - the game saves each narrow room's background (remaster_extract_dir mode, one short minimised run per
                room, about 5 s each)
  2. paint    - an AI model continues each background outwards to 16:9:
                  cloud: Google Gemini through your own OpenRouter account (best results; about 7 US cents per room;
                         the room backgrounds are sent to OpenRouter/Google)
                  local: Stable Diffusion XL inpainting on your NVIDIA GPU (free, offline, ~7 GB model download;
                         simpler results)
  3. compose  - aligns the painted sides with the room and blends the seam (compose.py), writes sides\\NNNN.png
Resumable: rooms already done are skipped. Run it again to continue or to redo rooms you deleted.

usage: generate.py --install <install folder> [--method cloud|local] [--rooms 9,10,...] [--yes]
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
                generator=torch.Generator("cuda").manual_seed(7)).images[0]
    img.resize((WW * 2, H * 2), Image.LANCZOS).save(out_png)
    return 0.0


# ── 3. compose ──────────────────────────────────────────────────────────────────────────────────────────────────
def compose_room(work, room, sides_dir):
    import compose
    big = Image.open(os.path.join(work, f"room_{room:04d}.png")).convert("RGB")
    big.resize((big.width * 2, big.height * 2), Image.LANCZOS).save(os.path.join(work, f"room2x_{room:04d}.png"))
    # compose.py reads room_NNNN.png (2x) and wide_NNNN.png from one folder and writes <folder>/sides2/NNNN.png
    cdir = os.path.join(work, "compose")
    os.makedirs(cdir, exist_ok=True)
    os.replace(os.path.join(work, f"room2x_{room:04d}.png"), os.path.join(cdir, f"room_{room:04d}.png"))
    wide = os.path.join(work, f"wide_{room:04d}.png")
    Image.open(wide).save(os.path.join(cdir, f"wide_{room:04d}.png"))
    r = compose.compose(cdir, f"{room:04d}")
    if r is None:
        return False
    os.makedirs(sides_dir, exist_ok=True)
    os.replace(os.path.join(cdir, "sides2", f"{room:04d}.png"), os.path.join(sides_dir, f"{room:04d}.png"))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True, help="the scummvm-ai-upscale install folder")
    ap.add_argument("--method", choices=["cloud", "local"])
    ap.add_argument("--rooms", help="comma-separated room numbers (default: all narrow rooms)")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    a = ap.parse_args()
    install = os.path.abspath(a.install)
    work = os.path.join(install, "sides-work")
    sides_dir = os.path.join(install, "sides")
    os.makedirs(work, exist_ok=True)
    rooms = [int(x) for x in a.rooms.split(",")] if a.rooms else ROOMS
    todo = [r for r in rooms if not os.path.exists(os.path.join(sides_dir, f"{r:04d}.png"))]
    if not todo:
        say(f"Side art is already there for all {len(rooms)} rooms ({sides_dir}). Delete a room's file to redo it.")
        return
    method = a.method
    if not method:
        say("How should the sides be painted?")
        say("  1  cloud: Google Gemini through your own OpenRouter account. Best results, about $0.07 per room")
        say(f"     (about ${0.07 * len(todo):.2f} for {len(todo)} rooms). The room backgrounds are sent to OpenRouter/Google.")
        say("  2  local: Stable Diffusion XL on your NVIDIA graphics card. Free and offline; about 7 GB download,")
        say("     about 15 s per room on a fast card; simpler results.")
        method = "local" if input("Choose 1 or 2 [1]: ").strip() == "2" else "cloud"
    key = ""
    if method == "cloud":
        key = openrouter_key()
        if not key:
            say("An OpenRouter API key is needed (https://openrouter.ai/keys). It stays on this PC and is only sent to OpenRouter.")
            key = input("Paste your OpenRouter key (or set OPENROUTER_API_KEY): ").strip()
            if not key:
                sys.exit("No key; nothing done.")
    if not a.yes:
        what = f"{len(todo)} rooms with the {method} method"
        if input(f"Generate side art for {what}? [Y/n]: ").strip().lower() in ("n", "no"):
            sys.exit("Nothing done.")
    t0, cost, made, failed = time.time(), 0.0, 0, []
    extract(install, work, todo)
    for k, r in enumerate(todo):
        room_png = os.path.join(work, f"room_{r:04d}.png")
        if not os.path.exists(room_png):
            failed.append(r)
            continue
        if Image.open(room_png).width > 640:
            say(f"[paint] room {r} is wider than the screen, skipped (it shows more of the room instead)")
            continue
        wide = os.path.join(work, f"wide_{r:04d}.png")
        say(f"[paint] room {r} ({k + 1}/{len(todo)}, {method})")
        try:
            if not os.path.exists(wide):
                cost += paint_cloud(room_png, wide, key) if method == "cloud" else paint_local(room_png, wide)
            if compose_room(work, r, sides_dir):
                made += 1
            else:
                failed.append(r)
                say(f"[compose] room {r}: the painted picture could not be aligned; try again later (it is redone)")
                os.remove(wide)
        except (urllib.error.URLError, RuntimeError, OSError) as e:
            failed.append(r)
            say(f"[paint] room {r} failed: {e}")
    say(f"Done in {(time.time() - t0) / 60:.0f} min: {made} rooms painted" + (f", cost about ${cost:.2f}" if cost else "") +
        (f"; failed: {', '.join(map(str, failed))} (run again to retry)" if failed else "") + f". Side art: {sides_dir}")


if __name__ == "__main__":
    main()
