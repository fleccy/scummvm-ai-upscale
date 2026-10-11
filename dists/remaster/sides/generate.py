r"""Generate widescreen side art from YOUR OWN copy of The Curse of Monkey Island, on your own PC.

Nothing derived from the game's art is distributed with scummvm-ai-upscale; this makes it locally instead:
  1. extract  - the game saves each narrow room's background (remaster_extract_dir mode, one short minimised run per
                room, about 5 s each)
  2. paint    - an AI model continues each background outwards to 16:9:
                  cloud: Google Gemini through your own OpenRouter account (best results; about 7 US cents per room;
                         the room backgrounds are sent to OpenRouter/Google)
                  local: Stable Diffusion XL inpainting on your NVIDIA GPU (free, offline, ~7 GB model download;
                         simpler results)
                  comfyui: Qwen-Image-Edit in a ComfyUI you already run (http://127.0.0.1:8188, or %COMFYUI_URL%;
                         free, offline, needs its Qwen-Image-Edit models)
  3. compose  - aligns the painted sides with the room and blends the seam (compose.py), writes sides\\NNNN.png
Resumable: finished rooms are kept; a room whose sides\NNNN.png you delete (or --regenerate) is painted anew.

usage: generate.py --install <install folder> [--method cloud|local|comfyui] [--rooms 9,10,...] [--regenerate]
                   [--recompose] [--status] [--max-cost USD] [--yes]
       generate.py --install <install folder> --method ... --serve [--max-cost USD]
  --serve: live side art. Runs next to the game (remaster_live_sides=true) and paints in the background while you
  play: the room you are in first, then the others (backgrounds of rooms you have not visited are saved by short
  hidden runs of the game without AI). New side art fades in by itself. Stops when the game closes.
exit code: 0 all requested rooms made, 2 some rooms failed (listed; run again to retry), 1 nothing done.
  The OpenRouter key is read from %OPENROUTER_API_KEY% or %USERPROFILE%\\.openrouter_key (never stored elsewhere).
"""
import argparse
import base64
import io
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
M = 104
# Rooms 640 wide whose sides can be painted (wide rooms show more of the room instead; rooms with characters or
# animation crossing the edges keep the ambient glow; room 73, the Monkey Island 1 screen with its verb menu, and
# room 89, a close-up of a hand, are left as they are: a model paints fake menu text or more hand beside them).
ROOMS = [9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21, 23, 24, 26, 27, 28, 29, 30, 31, 32, 34, 35, 36, 37, 38, 39, 52, 54,
         56, 57, 58, 59, 62, 63, 64, 65, 66, 67, 68, 69, 71, 72, 74, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 90, 94]
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
            "remaster_bg_prewarm=", "remaster_widescreen=", "remaster_live_sides=", "remaster_ai_enabled=",
            "remaster_display_mode=", "remaster_hd_backgrounds=", "remaster_hd_actors=")
    lines = [l for l in lines if not l.startswith(drop)]
    i = lines.index("[comi]") if "[comi]" in lines else -1
    if i < 0:
        sys.exit("scummvm.ini has no [comi] section; run the installer first.")
    saves = os.path.join(work, "saves")
    os.makedirs(saves, exist_ok=True)
    # no AI and no live side art in these runs: they only need the room's background, and stay light next to the game
    lines[i + 1:i + 1] = ["fullscreen=false", "mute=true", "autosave_period=0", "remaster_update_check=false",
                          f"savepath={saves}", f"remaster_extract_dir={work}", "remaster_widescreen=wide",
                          "remaster_live_sides=false", "remaster_ai_enabled=false", "remaster_display_mode=1",
                          "remaster_hd_backgrounds=false", "remaster_hd_actors=false"]
    cfg = os.path.join(work, "extract.ini")
    open(cfg, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    todo = [r for r in rooms if not os.path.exists(os.path.join(work, f"room_{r:04d}.png"))]
    for k, r in enumerate(todo):
        say(f"[extract] room {r} ({k + 1}/{len(todo)})")
        flags = (subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == "nt" else 0
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


COMFY = os.environ.get("COMFYUI_URL", "http://127.0.0.1:8188").rstrip("/")


def comfy_call(path, data=None, ctype="application/json", timeout=60):
    req = urllib.request.Request(COMFY + path, data=data, headers={"Content-Type": ctype} if data is not None else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def comfy_models():
    """The Qwen-Image-Edit files this ComfyUI has (GGUF through ComfyUI-GGUF, or safetensors), or a reason."""
    try:
        info = json.loads(comfy_call("/object_info"))
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise RuntimeError(f"ComfyUI is not reachable at {COMFY} ({e}); start it first") from e
    def choices(node, field):
        try:
            return list(info[node]["input"]["required"][field][0])
        except (KeyError, IndexError, TypeError):
            return []
    gguf = [n for n in choices("UnetLoaderGGUF", "unet_name") if "qwen" in n.lower() and "edit" in n.lower()]
    safe = [n for n in choices("UNETLoader", "unet_name") if "qwen" in n.lower() and "edit" in n.lower()]
    te = [n for n in choices("CLIPLoader", "clip_name") if "qwen_2.5_vl" in n.lower()]
    vae = [n for n in choices("VAELoader", "vae_name") if "qwen_image" in n.lower()]
    lora = [n for n in choices("LoraLoaderModelOnly", "lora_name") if "qwen-image-edit" in n.lower() and "lightning" in n.lower()]
    if not (gguf or safe) or not te or not vae or "TextEncodeQwenImageEditPlus" not in info:
        raise RuntimeError("this ComfyUI has no complete Qwen-Image-Edit setup (model, qwen_2.5_vl text encoder, qwen_image_vae)")
    return {"gguf": sorted(gguf)[0] if gguf else None, "unet": None if gguf else sorted(safe)[0], "te": sorted(te)[0],
            "vae": sorted(vae)[0], "lora": sorted(lora)[0] if lora else None}


COMFY_PROMPT = ("Continue this hand-painted cartoon adventure game background outwards on the left and right: the same "
                "place, walls, scenery and materials extending naturally, matching perspective, line work, colours and "
                "lighting. Objects cut by the edges continue correctly and never repeat. No people, no text, no borders.")


class PaintInterrupted(Exception):
    """Live side art: the player is back (or the game closed) while a room was being painted."""


_keep_painting = None   # live side art: a function saying whether a running paint may continue


def comfy_free():
    """Ask ComfyUI to unload its models (they hold several GB of graphics memory the game's AI needs)."""
    try:
        comfy_call("/free", json.dumps({"unload_models": True, "free_memory": True}).encode(), timeout=10)
    except (urllib.error.URLError, OSError):
        pass


def paint_comfyui(room_png, out_png):
    """Inpainting with Qwen-Image-Edit: only the side strips may change (latent noise mask); they start from the
    room's mirrored edges so the model continues the scene instead of keeping placeholder bars."""
    m = comfy_models()
    orig = Image.open(room_png).convert("RGB")
    W, H = orig.size
    S = 2
    big = orig.resize((W * S, H * S), Image.LANCZOS)
    MS, WW = M * S, (W + 2 * M) * S
    canvas = Image.new("RGBA", (WW, H * S))
    canvas.paste(big, (MS, 0))
    canvas.paste(big.crop((0, 0, MS, H * S)).transpose(Image.FLIP_LEFT_RIGHT), (0, 0))
    canvas.paste(big.crop((W * S - MS, 0, W * S, H * S)).transpose(Image.FLIP_LEFT_RIGHT), (MS + W * S, 0))
    # blur the mirrored strips: they give the colours and light, the model draws the shapes (no mirror copies)
    from PIL import ImageFilter
    for x0 in (0, MS + W * S):
        canvas.paste(canvas.crop((x0, 0, x0 + MS, H * S)).filter(ImageFilter.GaussianBlur(28)), (x0, 0))
    # alpha 0 = may be painted (ComfyUI's LoadImage mask is 1 - alpha); a few pixels of overlap for the blend
    alpha = Image.new("L", canvas.size, 0)
    alpha.paste(255, (MS + 16, 0, MS + W * S - 16, H * S))
    canvas.putalpha(alpha)
    buf = io.BytesIO()
    canvas.save(buf, "PNG")
    bd = "----sides" + hashlib_hex(buf.getvalue())[:16]
    name = f"comi_sides_{os.path.basename(room_png)}"
    body = (f"--{bd}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"{name}\"\r\nContent-Type: image/png\r\n\r\n").encode() + \
        buf.getvalue() + f"\r\n--{bd}\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--{bd}--\r\n".encode()
    up = json.loads(comfy_call("/upload/image", body, f"multipart/form-data; boundary={bd}"))["name"]
    model = ["1", 0]
    g = {"1": {"class_type": "UnetLoaderGGUF", "inputs": {"unet_name": m["gguf"]}} if m["gguf"] else
              {"class_type": "UNETLoader", "inputs": {"unet_name": m["unet"], "weight_dtype": "default"}}}
    if m["lora"]:
        g["1b"] = {"class_type": "LoraLoaderModelOnly", "inputs": {"model": model, "lora_name": m["lora"], "strength_model": 1.0}}
        model = ["1b", 0]
    g.update({
        "1c": {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": model, "shift": 3.0}},
        "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": m["te"], "type": "qwen_image"}},
        "3": {"class_type": "VAELoader", "inputs": {"vae_name": m["vae"]}},
        "4": {"class_type": "LoadImage", "inputs": {"image": up}},
        "5": {"class_type": "VAEEncode", "inputs": {"pixels": ["4", 0], "vae": ["3", 0]}},
        "5m": {"class_type": "SetLatentNoiseMask", "inputs": {"samples": ["5", 0], "mask": ["4", 1]}},
        "6": {"class_type": "TextEncodeQwenImageEditPlus", "inputs": {"clip": ["2", 0], "prompt": COMFY_PROMPT, "vae": ["3", 0], "image1": ["4", 0]}},
        "7": {"class_type": "TextEncodeQwenImageEditPlus", "inputs": {"clip": ["2", 0], "prompt": "", "vae": ["3", 0], "image1": ["4", 0]}},
        "8": {"class_type": "KSampler", "inputs": {"model": ["1c", 0], "positive": ["6", 0], "negative": ["7", 0], "latent_image": ["5m", 0],
                                                  "seed": LOCAL_SEED, "steps": 4 if m["lora"] else 20, "cfg": 1.0 if m["lora"] else 2.5,
                                                  "sampler_name": "euler", "scheduler": "simple", "denoise": 1.0}},
        "9": {"class_type": "VAEDecode", "inputs": {"samples": ["8", 0], "vae": ["3", 0]}},
        "10": {"class_type": "SaveImage", "inputs": {"images": ["9", 0], "filename_prefix": "comi_sides"}},
    })
    pid = json.loads(comfy_call("/prompt", json.dumps({"prompt": g}).encode()))["prompt_id"]
    t0 = time.time()
    while time.time() - t0 < 900:
        if _keep_painting and not _keep_painting():
            try:
                comfy_call("/interrupt", b"{}", timeout=10)   # stop at once: the game needs the graphics card
            except (urllib.error.URLError, OSError):
                pass
            comfy_free()
            raise PaintInterrupted()
        h = json.loads(comfy_call(f"/history/{pid}", timeout=30))
        if pid in h:
            st = h[pid].get("status", {})
            if st.get("status_str") == "error":
                msgs = [x[1].get("exception_message", "") for x in st.get("messages", []) if x[0] == "execution_error"]
                raise RuntimeError("ComfyUI failed: " + (msgs[0].strip() if msgs else "unknown error")[:200])
            imgs = h[pid].get("outputs", {}).get("10", {}).get("images", [])
            if imgs:
                o = imgs[0]
                q = urllib.parse.urlencode({"filename": o["filename"], "subfolder": o["subfolder"], "type": o["type"]})
                Image.open(io.BytesIO(comfy_call(f"/view?{q}", timeout=60))).convert("RGB").save(out_png)
                return 0.0
        time.sleep(1)
    raise RuntimeError("ComfyUI took longer than 15 minutes")


def hashlib_hex(b):
    import hashlib
    return hashlib.sha256(b).hexdigest()


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
    fix_edge_columns(os.path.join(cdir, "sides2", f"{room:04d}.png"))
    os.replace(os.path.join(cdir, "sides2", f"{room:04d}.png"), os.path.join(sides_dir, f"{room:04d}.png"))
    return True


def fix_edge_columns(path, width=640):
    """Some rooms have a stray darker or lighter outermost column (it was the screen edge in 1997 and never seen).
    Next to side art it shows as a hairline, so the room's outer columns at each join that stand out from both
    neighbours are smoothed into them (the game blends its own edge towards these columns)."""
    im = Image.open(path).convert("RGB")
    a = np.asarray(im, dtype=np.float32).copy()
    w = a.shape[1]
    m = (w - width) // 2
    if m <= 2:
        return
    changed = False
    # at each join, a run of 1-3 columns that stands out from the columns on both sides of it (which agree with each
    # other) is replaced by the blend between those two
    for j in (m, m + width):
        best = None
        for start in range(j - 3, j + 2):
            for n in (1, 2, 3):
                lo, hi = start - 1, start + n
                if lo < 0 or hi >= w:
                    continue
                left, right = a[:, lo], a[:, hi]
                t = (np.arange(1, n + 1, dtype=np.float32) / (n + 1))[None, :, None]
                interp = left[:, None, :] * (1 - t) + right[:, None, :] * t
                dev = np.abs(a[:, start:start + n] - interp).mean()
                spread = np.abs(left - right).mean()
                if dev > 12 and dev > 2 * spread and (best is None or dev > best[0]):
                    best = (dev, start, n, interp)
        if best:
            _, start, n, interp = best
            a[:, start:start + n] = interp
            changed = True
    if changed:
        Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).save(path)


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
    if method == "comfyui":
        return {"method": "comfyui", "model": "qwen-image-edit-inpaint", "prompt": hashlib.sha256(COMFY_PROMPT.encode()).hexdigest()[:16], "seed": LOCAL_SEED}
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


def sides_painted(wide_png, room_png):
    """False when the painting's sides are still flat or placeholder grey (a model that ignored the request)."""
    with Image.open(wide_png) as im, Image.open(room_png) as rm:
        w = np.asarray(im.convert("RGB").resize((848, rm.height)), dtype=np.float32)
    for strip in (w[:, 8:M - 16], w[:, 848 - M + 16:840]):
        grey = np.abs(strip - 128).mean() < 12 and strip.std() < 18
        flat = strip.std(axis=(0, 1)).mean() < 6
        if grey or flat:
            return False
    return True


def make_room(work, sides_dir, r, method, key, params, state):
    """Paint (unless a matching painting waits) and compose one room. Returns (result, cost, reason): result is
    'new', 'skipped' (a wide room) or 'failed'."""
    room_png = os.path.join(work, f"room_{r:04d}.png")
    wide = os.path.join(work, f"wide_{r:04d}.png")
    if not os.path.exists(room_png):
        return "failed", 0.0, "the game did not save this room"
    with Image.open(room_png) as im:
        if im.width > 640:
            return "skipped", 0.0, "wider than the screen (it shows more of the room instead)"
    cost = 0.0
    try:
        if state != "painted":
            if os.path.exists(wide):
                os.remove(wide)
            paint = {"cloud": lambda: paint_cloud(room_png, wide, key), "local": lambda: paint_local(room_png, wide),
                     "comfyui": lambda: paint_comfyui(room_png, wide)}[method]
            cost = paint() or 0.0
            if not sides_painted(wide, room_png):
                os.remove(wide)
                return "failed", cost, "the model left the sides unpainted"
            write_meta(work, r, dict(params, source=sha256(room_png), cost=cost, made=time.strftime("%Y-%m-%d %H:%M"), composed=False))
        if compose_room(work, r, sides_dir):
            m = read_meta(work, r)
            m["composed"] = True
            write_meta(work, r, m)
            return "new", cost, ""
        if os.path.exists(wide):
            os.remove(wide)
        return "failed", cost, "the painted picture could not be aligned with the room"
    except (urllib.error.URLError, RuntimeError, OSError, ValueError, KeyError) as e:
        return "failed", cost, str(e)[:200]


def process_alive(pid):
    if os.name == "nt":
        import ctypes
        k = ctypes.windll.kernel32
        h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k.GetExitCodeProcess(h, ctypes.byref(code))
        k.CloseHandle(h)
        return bool(ok) and code.value == 259   # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def find_game(install):
    """Process id of this install's game (scummvm.exe / scummvm from the install folder), or 0."""
    want = os.path.normcase(os.path.abspath(os.path.join(install, "scummvm.exe" if os.name == "nt" else "scummvm")))
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        k, psapi = ctypes.windll.kernel32, ctypes.windll.psapi
        ids = (wintypes.DWORD * 4096)()
        got = wintypes.DWORD()
        if not psapi.EnumProcesses(ids, ctypes.sizeof(ids), ctypes.byref(got)):
            return 0
        for pid in ids[:got.value // ctypes.sizeof(wintypes.DWORD)]:
            h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                continue
            buf = ctypes.create_unicode_buffer(1024)
            n = wintypes.DWORD(1024)
            ok = k.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
            k.CloseHandle(h)
            if ok and os.path.normcase(buf.value) == want:
                return pid
        return 0
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                if os.path.normcase(os.path.realpath(f"/proc/{d}/exe")) == want:
                    return int(d)
            except OSError:
                pass
    return 0


def serve(install, work, sides_dir, method, key, max_cost, game_pid=0, while_playing=False):
    """Live side art: runs next to the game. The game writes live/alive every 2 s ("<room> play|idle"; it stops
    while the game is paused) and, for a narrow room without side art, live/room_NNNN.png and live/want. This paints
    that room first, then the others - only during breaks unless while_playing (painting shares the graphics card
    with the game's AI upscaling) - and stops when the game has closed."""
    live = os.path.join(work, "live")
    os.makedirs(live, exist_ok=True)
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)   # below normal
    alive, want_f = os.path.join(live, "alive"), os.path.join(live, "want")

    def heartbeat_age():
        try:
            return time.time() - os.path.getmtime(alive)
        except OSError:
            return 1e9

    def game_running():
        if game_pid:
            return process_alive(game_pid)
        return heartbeat_age() < 20

    def on_a_break():
        if while_playing:
            return True
        if heartbeat_age() > 6:
            # paused (ScummVM menu): the game's loop, and its heartbeat, stand still. Before the game's first
            # heartbeat of this session (it is still starting) it is not a break.
            return seen_heartbeat
        try:
            with open(alive, encoding="ascii") as f:
                return f.read().split()[1:2] == ["idle"]
        except (OSError, IndexError):
            return False

    def wanted():
        try:
            with open(want_f, encoding="ascii") as f:
                return int(f.read().strip() or 0)
        except (OSError, ValueError):
            return 0

    params = gen_params(method)
    tries, spent, t0 = {}, 0.0, time.time()
    say(f"[live] waiting for the game ({method} method)...")
    while not game_pid and time.time() - t0 < 60:
        game_pid = find_game(install)   # started by play_live_sides.cmd just before this
        if not game_pid:
            time.sleep(1)
    while not game_running() or (not game_pid and heartbeat_age() > 20):
        if game_pid and not process_alive(game_pid):
            say("[live] the game is not running; stopping")
            return 1
        if heartbeat_age() < 20:
            break
        if time.time() - t0 > 180:
            say("[live] the game did not start (no heartbeat in sides-work\\live); stopping")
            return 1
        time.sleep(1)
    seen_heartbeat = False
    while game_running() and not seen_heartbeat:
        seen_heartbeat = heartbeat_age() < 6 and os.path.getmtime(alive) > t0
        if not seen_heartbeat:
            time.sleep(1)
    say("[live] game running; painting side art in the background")
    idle, waiting = False, False
    while game_running():
        if not on_a_break():
            if not waiting:
                say("[live] you are playing; painting waits for a break (pause, or a minute without input)")
                waiting = True
                if method == "comfyui":
                    comfy_free()   # give the graphics memory back to the game's AI while you play
            time.sleep(2)
            continue
        waiting = False
        w = wanted()
        todo = [r for r in ([w] if w in ROOMS else []) + ROOMS
                if tries.get(r, 0) < 2 and room_state(work, sides_dir, r, params) != "done"]
        if not todo:
            if not idle:
                say("[live] all rooms have side art; idle until the game closes")
                idle = True
            time.sleep(2)
            continue
        idle = False
        r = todo[0]
        src_live = os.path.join(live, f"room_{r:04d}.png")
        room_png = os.path.join(work, f"room_{r:04d}.png")
        if os.path.exists(src_live) and time.time() - os.path.getmtime(src_live) > 2:
            shutil.copyfile(src_live, room_png)   # the room as the game shows it now (written completely)
        elif not os.path.exists(room_png):
            if r == w:
                time.sleep(1)                     # the game is about to hand it over
                continue
            extract(install, work, [r])           # a short hidden run of the game, no AI
            if not game_running():
                break
        if method == "cloud" and spent + 0.07 > max_cost:
            say(f"[live] spending limit ${max_cost:.2f} reached; painting stops (rooms you visit keep the glow)")
            while game_running():
                time.sleep(2)
            break
        state = room_state(work, sides_dir, r, params)
        say(f"[live] room {r}{' (you are here)' if r == w else ''}")
        global _keep_painting
        _keep_painting = lambda: game_running() and on_a_break()
        try:
            result, cost, why = make_room(work, sides_dir, r, method, key, params, state)
        except PaintInterrupted:
            say(f"[live] room {r}: you are playing again; stopped, continues at the next break")
            continue
        finally:
            _keep_painting = None
        spent += cost
        if result == "new":
            say(f"[live] room {r} ready")
        elif "not reachable" in why:
            say(f"[live] {why}; trying again in 30 s")   # ComfyUI not started yet: no room counts as failed
            time.sleep(30)
        else:
            tries[r] = 2 if result == "skipped" else tries.get(r, 0) + 1
            say(f"[live] room {r} {result}: {why}")
    say("[live] the game has closed; stopping")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", required=True, help="the scummvm-ai-upscale install folder")
    ap.add_argument("--method", choices=["cloud", "local", "comfyui"])
    ap.add_argument("--serve", action="store_true", help="live side art: paint in the background while the game runs")
    ap.add_argument("--game-pid", type=int, default=0, help="--serve: the game's process (default: found by itself)")
    ap.add_argument("--settings", action="store_true",
                    help="--serve: method, spending limit and --while-playing from sides-work/live/settings.json")
    ap.add_argument("--while-playing", action="store_true",
                    help="--serve: also paint while you play (by default only during breaks: game paused, idle for a minute)")
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

    if a.serve and a.settings:
        try:
            with open(os.path.join(work, "live", "settings.json"), encoding="utf-8-sig") as f:
                st = json.load(f)
        except (OSError, ValueError):
            sys.exit("No live side-art settings; run generate_sides.cmd and choose \"while I play\".")
        method = method or st.get("method")
        if a.max_cost is None and st.get("max_cost"):
            a.max_cost = float(st["max_cost"])
        a.while_playing = a.while_playing or bool(st.get("while_playing"))
    if a.serve:
        if not method:
            sys.exit("--serve needs --method")
        key = ""
        if method == "cloud":
            key = openrouter_key()
            if not key or a.max_cost is None:
                sys.exit("--serve with the cloud method needs an OpenRouter key and --max-cost")
        return serve(install, work, sides_dir, method, key, a.max_cost or 0.0, a.game_pid, a.while_playing)

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
    if method == "comfyui" and to_paint:
        try:
            comfy_models()   # reachable, with the Qwen-Image-Edit files: say so now, not once per room
        except RuntimeError as e:
            say(f"ComfyUI: {e}.")
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
        result, cost, why = make_room(work, sides_dir, r, method, key, params, states[r])
        summary["cost"] += cost
        if result == "new":
            summary["new"].append(r)
        else:
            summary["failed"][r] = why
            say(f"[paint] room {r} failed: {why}")
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
