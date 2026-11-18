#!/bin/bash
# Package the Windows build (from build-windows.sh) into remaster-build/<name>-windows-x64.zip.
# Contains no game data: players use their own copy of the game.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${REMASTER_BUILD_DIR:-$REPO/remaster-build}"
NAME="${1:-scummvm-ai-upscale-$(git -C "$REPO" describe --tags --always --dirty)}"
STAGE="$WORK/package/$NAME"
SRC="$WORK/src"
rm -rf "$STAGE"
mkdir -p "$STAGE/models" "$STAGE/licenses"

cp "$WORK/out/scummvm.exe" "$WORK/out/SDL2.dll" "$STAGE/"
cp "$WORK/out/onnxruntime.dll" "$WORK/out/onnxruntime_providers_shared.dll" "$WORK/out/DirectML.dll" "$STAGE/"   # DirectML upscalers
cp "$REPO/dists/remaster/models/realesr-animevideov3-x3."{param,bin} "$STAGE/models/"
cp "$REPO/dists/remaster/models/realesrgan-x4plus-anime."{param,bin} "$STAGE/models/"  # HD backgrounds
# other AI models for the live picture (remaster_ai_net), licences in dists/remaster/licenses
for m in realcugan-up3x-conservative 2x-Futsuu-Anime 2x-StarSample-V2.0-Lite ArtCNN_C4F32 2x-AniScale2S-Compact 2x-AniSD-G6i1b-Compact; do
	cp "$REPO/dists/remaster/models/$m."{param,bin} "$STAGE/models/"
done
# DirectML / TensorRT upscalers (same weights, converted by ncnn2onnx.py) and the UltraSharpV2 HD background model
# (ONNX Runtime only, export_onnx_fixed.py)
cp "$REPO/dists/remaster/models/"*.onnx "$STAGE/models/"
cp "$REPO/dists/remaster/install.ps1" "$REPO/dists/remaster/install.cmd" "$REPO/dists/remaster/install-gui.ps1" "$REPO/dists/remaster/prewarm.ps1" "$STAGE/"
# side-art generator (players make side art from their own copy; nothing generated is distributed)
cp "$REPO/dists/remaster/generate_sides.ps1" "$REPO/dists/remaster/generate_sides.cmd" "$STAGE/"
mkdir -p "$STAGE/sides-tools"
cp "$REPO/dists/remaster/sides/generate.py" "$REPO/dists/remaster/sides/compose.py" "$STAGE/sides-tools/"
mkdir -p "$STAGE/sides-tools/requirements"
cp "$REPO/dists/remaster/sides/requirements/"{cloud,local}.txt "$STAGE/sides-tools/requirements/"   # locked, hash-checked
cp "$REPO/dists/remaster/generate_cutscenes.ps1" "$REPO/dists/remaster/generate_cutscenes.cmd" "$STAGE/"
mkdir -p "$STAGE/video-tools"
cp "$REPO/dists/remaster/video-sides/batch_outpaint.py" "$REPO/dists/remaster/video-sides/pack_sides.py" "$STAGE/video-tools/"
cp "$REPO/dists/remaster/README.txt" "$STAGE/"
cp "$REPO/gui/themes/scummmodern.zip" "$STAGE/"   # GUI theme for the in-game menus

# Nothing derived from the game is ever packaged; check_package.py enforces an allowlist below.

# Licences of everything inside scummvm.exe / SDL2.dll / models.
cp "$REPO/COPYING"                                  "$STAGE/licenses/ScummVM-GPL-3.0.txt"
cp "$REPO/AUTHORS"                                  "$STAGE/licenses/ScummVM-AUTHORS.txt"
cp "$REPO/dists/remaster/models/LICENSE-Real-ESRGAN.txt" "$STAGE/licenses/Real-ESRGAN-BSD-3-Clause.txt"
cp "$SRC/SDL2-2.32.10/LICENSE.txt"                  "$STAGE/licenses/SDL2-zlib.txt"
cp "$SRC/libpng-1.6.44/LICENSE"                     "$STAGE/licenses/libpng.txt"
cp "$SRC/flac-1.4.3/COPYING.Xiph"                   "$STAGE/licenses/libFLAC-BSD-3-Clause.txt"
sed -n '/Copyright notice:/,/^$/p;/(C) 1995/,/madler/p' "$SRC/zlib-1.3.1/README" > "$STAGE/licenses/zlib.txt"
NCNN_SRC="$SRC/ncnn-20260526"
[ -f "$NCNN_SRC/LICENSE.txt" ] || NCNN_SRC="$(dirname "$(find "$SRC/ncnn-20260526" -mindepth 2 -maxdepth 2 -name LICENSE.txt -not -path "*/python/*" | head -1)")"
cp "$NCNN_SRC/LICENSE.txt"                          "$STAGE/licenses/ncnn-BSD-3-Clause.txt"
cp "$SRC/onnxruntime-directml-1.24.4/LICENSE"       "$STAGE/licenses/ONNX-Runtime-MIT.txt"
cp "$SRC/onnxruntime-directml-1.24.4/ThirdPartyNotices.txt" "$STAGE/licenses/ONNX-Runtime-ThirdPartyNotices.txt"
cp "$SRC/directml-1.15.4/LICENSE.txt"               "$STAGE/licenses/DirectML-licence.txt"
cp "$SRC/directml-1.15.4/ThirdPartyNotices.txt"     "$STAGE/licenses/DirectML-ThirdPartyNotices.txt"
cp "$NCNN_SRC/glslang/LICENSE.txt"                  "$STAGE/licenses/glslang.txt"
LLVM="$SRC/llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64"
for f in LICENSE.TXT; do [ -f "$LLVM/$f" ] && cp "$LLVM/$f" "$STAGE/licenses/llvm-mingw-runtime.txt"; done
cp "$REPO/dists/remaster/licenses/"*.txt "$STAGE/licenses/"   # mingw-w64 runtime + winpthreads (statically linked), FSR, AI models

(cd "$WORK/package" && python3 -c "
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + '-windows-x64.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk(name):
        for f in files:
            z.write(os.path.join(root, f))
" "$NAME")
mv "$WORK/package/$NAME-windows-x64.zip" "$WORK/"
ls -l "$WORK/$NAME-windows-x64.zip" "$STAGE/licenses"
sha256sum "$WORK/$NAME-windows-x64.zip"
python3 "$REPO/dists/remaster/check_package.py" "$WORK/$NAME-windows-x64.zip"

