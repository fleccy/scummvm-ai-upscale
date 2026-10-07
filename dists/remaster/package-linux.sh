#!/bin/bash
# Package the Linux build (from build-linux.sh) into remaster-build-linux/<name>-linux-x86_64.tar.gz.
# Contains no game data: players use their own copy of the game.
#
#   bash dists/remaster/package-linux.sh [name]     # default name: scummvm-ai-upscale-<git describe>
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${REMASTER_BUILD_DIR:-$REPO/remaster-build-linux}"
NAME="${1:-scummvm-ai-upscale-$(git -C "$REPO" describe --tags --always --dirty)}"
STAGE="$WORK/package/$NAME"
SRC="$WORK/src"
[ -x "$WORK/out/scummvm" ] || { echo "run dists/remaster/build-linux.sh first"; exit 1; }
rm -rf "$STAGE"
mkdir -p "$STAGE/models" "$STAGE/licenses"

install -m 755 "$WORK/out/scummvm" "$STAGE/"
install -m 644 "$WORK/out/libSDL2-2.0.so.0" "$STAGE/"
install -m 755 "$REPO/dists/remaster/play.sh" "$STAGE/"
cp "$REPO/dists/remaster/models/realesr-animevideov3-x3."{param,bin} "$STAGE/models/"
cp "$REPO/dists/remaster/models/realesrgan-x4plus-anime."{param,bin} "$STAGE/models/"  # HD backgrounds
cp "$REPO/dists/remaster/README-linux.txt" "$STAGE/README.txt"
cp "$REPO/gui/themes/scummmodern.zip" "$STAGE/"   # GUI theme for the in-game menus

# Optional widescreen side art (REMASTER_SIDES_DIR=folder of 848-wide NNNN.png), see package.sh.
if [ -n "${REMASTER_SIDES_DIR:-}" ]; then
	python3 "$REPO/dists/remaster/sides/strip_middle.py" "$REMASTER_SIDES_DIR" "$STAGE/sides"
fi

# Licences of everything inside scummvm / libSDL2-2.0.so.0 / models.
cp "$REPO/COPYING"                                  "$STAGE/licenses/ScummVM-GPL-3.0.txt"
cp "$REPO/AUTHORS"                                  "$STAGE/licenses/ScummVM-AUTHORS.txt"
cp "$REPO/dists/remaster/models/LICENSE-Real-ESRGAN.txt" "$STAGE/licenses/Real-ESRGAN-BSD-3-Clause.txt"
cp "$SRC/SDL2-2.32.10/LICENSE.txt"                  "$STAGE/licenses/SDL2-zlib.txt"
cp "$SRC/libpng-1.6.44/LICENSE"                     "$STAGE/licenses/libpng.txt"
sed -n '/Copyright notice:/,/^$/p;/(C) 1995/,/madler/p' "$SRC/zlib-1.3.1/README" > "$STAGE/licenses/zlib.txt"
NCNN_SRC="$SRC/ncnn-20260526"
[ -f "$NCNN_SRC/LICENSE.txt" ] || NCNN_SRC="$(dirname "$(find "$SRC/ncnn-20260526" -mindepth 2 -maxdepth 2 -name LICENSE.txt -not -path "*/python/*" | head -1)")"
cp "$NCNN_SRC/LICENSE.txt"                          "$STAGE/licenses/ncnn-BSD-3-Clause.txt"
cp "$NCNN_SRC/glslang/LICENSE.txt"                  "$STAGE/licenses/glslang.txt"
# libstdc++/libgcc are linked statically: GPL-3.0 with the GCC Runtime Library Exception.
for f in /usr/share/doc/libstdc++6/copyright /usr/share/doc/gcc-*-base/copyright; do
	[ -f "$f" ] && { cp "$f" "$STAGE/licenses/GCC-runtime-libstdc++.txt"; break; }
done

tar -C "$WORK/package" --owner=0 --group=0 -czf "$WORK/$NAME-linux-x86_64.tar.gz" "$NAME"
ls -l "$WORK/$NAME-linux-x86_64.tar.gz"
ls "$STAGE" "$STAGE/licenses"
sha256sum "$WORK/$NAME-linux-x86_64.tar.gz"
python3 "$REPO/dists/remaster/check_package.py" "$WORK/$NAME-linux-x86_64.tar.gz"
