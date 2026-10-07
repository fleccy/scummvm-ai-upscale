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
cp "$REPO/dists/remaster/models/"*.onnx "$STAGE/models/"   # DirectML upscalers (same weights, converted by ncnn2onnx.py)
cp "$REPO/dists/remaster/install.ps1" "$REPO/dists/remaster/install.cmd" "$REPO/dists/remaster/prewarm.ps1" "$STAGE/"
# side-art generator (players make side art from their own copy; nothing generated is distributed)
cp "$REPO/dists/remaster/generate_sides.ps1" "$REPO/dists/remaster/generate_sides.cmd" "$STAGE/"
mkdir -p "$STAGE/sides-tools"
cp "$REPO/dists/remaster/sides/generate.py" "$REPO/dists/remaster/sides/compose.py" "$STAGE/sides-tools/"
cp "$REPO/dists/remaster/generate_cutscenes.ps1" "$REPO/dists/remaster/generate_cutscenes.cmd" "$STAGE/"
mkdir -p "$STAGE/video-tools"
cp "$REPO/dists/remaster/video-sides/batch_outpaint.py" "$REPO/dists/remaster/video-sides/pack_sides.py" "$STAGE/video-tools/"
cp "$REPO/dists/remaster/README.txt" "$STAGE/"
cp "$REPO/gui/themes/scummmodern.zip" "$STAGE/"   # GUI theme for the in-game menus

# Generated art is NOT in the main zip but in separate, unversioned release assets, so either can be withdrawn
# on its own (the installer takes them from next to install.cmd or downloads them from the latest release):
#   REMASTER_SIDES_DIR=folder of 848-wide NNNN.png -> scummvm-ai-upscale-side-art.zip (sides/: only the painted
#     strips, the original room art in the middle is blanked, see dists/remaster/sides/strip_middle.py)
#   (widened cutscenes are not distributed: players generate them from their own copy, see dists/remaster/video-sides)
EXTRAS="$WORK/package/extras"
rm -rf "$EXTRAS"
if [ -n "${REMASTER_SIDES_DIR:-}" ]; then
	mkdir -p "$EXTRAS/side-art"
	python3 "$REPO/dists/remaster/sides/strip_middle.py" "$REMASTER_SIDES_DIR" "$EXTRAS/side-art/sides"
fi

# Licences of everything inside scummvm.exe / SDL2.dll / models.
cp "$REPO/COPYING"                                  "$STAGE/licenses/ScummVM-GPL-3.0.txt"
cp "$REPO/AUTHORS"                                  "$STAGE/licenses/ScummVM-AUTHORS.txt"
cp "$REPO/dists/remaster/models/LICENSE-Real-ESRGAN.txt" "$STAGE/licenses/Real-ESRGAN-BSD-3-Clause.txt"
cp "$SRC/SDL2-2.32.10/LICENSE.txt"                  "$STAGE/licenses/SDL2-zlib.txt"
cp "$SRC/libpng-1.6.44/LICENSE"                     "$STAGE/licenses/libpng.txt"
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
cp "$REPO/dists/remaster/licenses/"*.txt "$STAGE/licenses/"   # mingw-w64 runtime + winpthreads (statically linked)

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

for extra in side-art; do
	[ -d "$EXTRAS/$extra" ] || continue
	(cd "$EXTRAS/$extra" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk('.'):
        for f in files:
            z.write(os.path.join(root, f), os.path.relpath(os.path.join(root, f), '.'))
" "$WORK/scummvm-ai-upscale-$extra.zip")
	ls -l "$WORK/scummvm-ai-upscale-$extra.zip"
	sha256sum "$WORK/scummvm-ai-upscale-$extra.zip"
done
