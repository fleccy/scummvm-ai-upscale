"""Release allowlist: fails if a package contains anything that is not an intended file of the program.
Nothing derived from the game (side art, cutscene strips, room images, captures) may ever be packaged.
usage: check_package.py <zip or tar.gz>"""
import fnmatch
import sys
import tarfile
import zipfile

ALLOWED = [
    # program and runtime
    "scummvm.exe", "SDL2.dll", "onnxruntime.dll", "onnxruntime_providers_shared.dll", "DirectML.dll",
    "scummvm", "libSDL2-2.0.so.0", "scummmodern.zip",
    # models (Real-ESRGAN, BSD-3-Clause)
    "models/realesr-animevideov3-x3.param", "models/realesr-animevideov3-x3.bin",
    "models/realesrgan-x4plus-anime.param", "models/realesrgan-x4plus-anime.bin",
    "models/realesr-animevideov3-x3-fp16.onnx", "models/realesrgan-x4plus-anime-x3-fp16.onnx",
    # scripts and docs
    "install.ps1", "install.cmd", "prewarm.ps1", "play.sh", "README.txt",
    "generate_sides.ps1", "generate_sides.cmd", "sides-tools/generate.py", "sides-tools/compose.py",
    "sides-tools/requirements/cloud.txt", "sides-tools/requirements/local.txt",
    "generate_cutscenes.ps1", "generate_cutscenes.cmd", "video-tools/batch_outpaint.py", "video-tools/pack_sides.py",
    "licenses/*.txt",
]
FORBIDDEN = ["*.png", "*.jpg", "*.webp", "*.sides", "*.san", "*.SAN", "*.mp4", "*.mkv", "*.json"]


def names(path):
    if path.endswith(".zip"):
        return [n for n in zipfile.ZipFile(path).namelist() if not n.endswith("/")]
    with tarfile.open(path) as t:
        return [m.name for m in t.getmembers() if m.isfile()]


def main():
    bad = []
    files = names(sys.argv[1])
    for n in files:
        rel = n.split("/", 1)[1] if "/" in n else n   # strip the top-level <name>/ folder
        if any(fnmatch.fnmatch(rel, p) for p in FORBIDDEN) or not any(fnmatch.fnmatch(rel, p) for p in ALLOWED):
            bad.append(rel)
    if bad:
        print(f"{sys.argv[1]}: NOT ALLOWED in a release:\n  " + "\n  ".join(bad))
        return 1
    print(f"{sys.argv[1]}: {len(files)} files, all on the allowlist")
    return 0


if __name__ == "__main__":
    sys.exit(main())
