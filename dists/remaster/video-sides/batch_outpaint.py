"""Overnight batch: widen every COMI cutscene to 16:9 with ARP's LTX outpainting, shot by shot.

For each .SAN video:
  1. decode all frames (palette PNGs, exact game pixels) with ffmpeg
  2. split into shots at hard cuts (so the outpainting never blends across a cut)
  3. encode each shot losslessly and run ARP's outpaint on it (ComfyUI must be running)
Resumable: finished shots are skipped. Progress goes to batch.log and status.json.
"""
import json, os, subprocess, sys, time, glob, shutil
import numpy as np
from PIL import Image

# Paths, set by generate_cutscenes.ps1: COMI_GAME = the installed game's folder (with its .SAN videos),
# COMI_ARP = the AI Remaster Pipeline install, COMI_VIDEO_WORK = working folder.
for _v in ("COMI_GAME", "COMI_ARP", "COMI_VIDEO_WORK"):
    if not os.environ.get(_v):
        sys.exit(f"{_v} is not set; run generate_cutscenes.cmd in the install folder.")
GAME = os.environ["COMI_GAME"]
ARP = os.environ["COMI_ARP"]
_W = os.environ["COMI_VIDEO_WORK"]
WORK = os.path.join(_W, "work")
LOG = os.path.join(_W, "batch.log")
STATUS = os.path.join(_W, "status.json")
LORA = "ltx-2.3-22b-ic-lora-outpaint.safetensors"
CUT = 30.0        # mean abs RGB difference (80x60 thumbnails) that counts as a hard cut
MIN_SHOT = 12     # frames; shorter pieces are merged into the previous shot


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def decode(san, out):
    os.makedirs(out, exist_ok=True)
    if glob.glob(os.path.join(out, "*.png")):
        return
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", san, "-fps_mode", "passthrough", os.path.join(out, "%05d.png")], check=True)


def shots(frames):
    prev, cuts = None, [0]
    for i, f in enumerate(frames):
        a = np.asarray(Image.open(f).convert("RGB").resize((80, 60))).astype(np.float32)
        if prev is not None and np.abs(a - prev).mean() > CUT and i - cuts[-1] >= MIN_SHOT:
            cuts.append(i)
        prev = a
    cuts.append(len(frames))
    return [(cuts[k], cuts[k + 1]) for k in range(len(cuts) - 1)]


def outpaint(clip):
    cmd = [os.path.join(ARP, "wrappers", "outpaint_video.bat"), "--source", clip, "--target-aspect", "16:9",
           "--target-height", "480", "--outpaint-lora", LORA]
    slim = "ltx-2.3-22b-textenc-only.safetensors"   # optional slimmed text encoder (less RAM); else ARP's default
    if os.path.exists(os.path.join(ARP, "tools", "comfyui", "models", "checkpoints", slim)):
        cmd += ["--text-encoder-checkpoint", slim]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    outs = [l.split(": ", 1)[1].strip() for l in r.stdout.splitlines() if l.startswith("Wrote outpainted video:")]
    return r.returncode, (outs[-1] if outs else None), (r.stdout + r.stderr)[-2000:]


# ComfyUI is restarted only when its memory has grown this far between shots. (Restarting every few shots was
# worse: each fresh start reloads every model at once, which is when free RAM dipped lowest. The prompt
# conditioning is cached in the ARP node, so the 15 GB text encoder no longer reloads every shot.)
RESTART_ABOVE_GB = 26
_comfy = None


def comfy_private_gb():
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "(Get-CimInstance Win32_Process -Filter \"Name='python.exe'\" | Where-Object { $_.CommandLine -match 'main\\.py' } | "
                        "Measure-Object PrivatePageCount -Maximum).Maximum"], capture_output=True, text=True)
    try:
        return float(r.stdout.strip() or 0) / 2 ** 30
    except ValueError:
        return 0.0


def comfy_stop():
    """Stop the ComfyUI we started, including its worker process (the venv launcher spawns a child)."""
    global _comfy
    if _comfy is not None:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(_comfy.pid)], capture_output=True)
        _comfy = None
        time.sleep(5)


def comfy_start():
    global _comfy
    comfy_stop()
    py = os.path.join(ARP, ".venv", "Scripts", "python.exe")
    out = open(os.path.join(ARP, "comfy-out.log"), "ab")
    _comfy = subprocess.Popen([py, "main.py", "--listen", "127.0.0.1", "--port", "8188", "--use-pytorch-cross-attention", "--fp8_e4m3fn-text-enc"],
                              cwd=os.path.join(ARP, "tools", "comfyui"), stdout=out, stderr=subprocess.STDOUT,
                              creationflags=subprocess.CREATE_NO_WINDOW)
    import urllib.request
    for _ in range(120):
        try:
            urllib.request.urlopen("http://127.0.0.1:8188/system_stats", timeout=2)
            return
        except Exception:
            time.sleep(2)
    log("ComfyUI did not come up")


def main():
    sans = sorted(glob.glob(os.path.join(GAME, "**", "*.SAN"), recursive=True), key=os.path.getsize)
    status = json.load(open(STATUS)) if os.path.exists(STATUS) else {}
    t_start = time.time()
    runs = 0
    comfy_start()
    for san in sans:
        name = os.path.splitext(os.path.basename(san))[0]
        vdir = os.path.join(WORK, name)
        fdir = os.path.join(vdir, "frames")
        decode(san, fdir)
        frames = sorted(glob.glob(os.path.join(fdir, "*.png")))
        sh = status.get(name, {}).get("shots")
        if not sh:
            sh = shots(frames)
            status.setdefault(name, {})["shots"] = sh
            json.dump(status, open(STATUS, "w"), indent=1)
        log(f"{name}: {len(frames)} frames, {len(sh)} shots")
        for k, (a, b) in enumerate(sh):
            key = f"{a:05d}_{b:05d}"
            done = status[name].setdefault("done", {})
            if key in done and os.path.exists(done[key]):
                continue
            clip = os.path.join(vdir, f"shot_{key}.mkv")
            if not os.path.exists(clip):
                subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", "12", "-start_number", str(a + 1),
                                "-i", os.path.join(fdir, "%05d.png"), "-frames:v", str(b - a),
                                "-c:v", "ffv1", "-pix_fmt", "bgr0", clip], check=True)
            if runs and comfy_private_gb() > RESTART_ABOVE_GB:
                log(f"  ComfyUI at {comfy_private_gb():.1f} GB, restarting it")
                comfy_start()
            runs += 1
            t0 = time.time()
            rc, out, tail = outpaint(clip)
            if rc != 0 or not out or not os.path.exists(out):
                log(f"  shot {k + 1}/{len(sh)} frames {a}-{b}: FAILED rc={rc}\n{tail}")
                status[name].setdefault("failed", {})[key] = tail[-500:]
                json.dump(status, open(STATUS, "w"), indent=1)
                continue
            keep = os.path.join(vdir, f"wide_{key}.mkv")
            shutil.copyfile(out, keep)
            done[key] = keep
            status[name].pop("failed", {}).pop(key, None) if "failed" in status[name] else None
            json.dump(status, open(STATUS, "w"), indent=1)
            log(f"  shot {k + 1}/{len(sh)} frames {a}-{b} ({(b - a) / 12:.1f} s) done in {time.time() - t0:.0f} s")
    comfy_stop()
    log(f"ALL DONE in {(time.time() - t_start) / 3600:.1f} h")


if __name__ == "__main__":
    try:
        main()
    finally:
        comfy_stop()
