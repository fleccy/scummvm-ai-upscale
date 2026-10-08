#!/bin/bash
# Build the remaster fork of ScummVM for 64-bit Windows, cross-compiled on Linux (Ubuntu/WSL tested).
#
#   bash dists/remaster/build-windows.sh            # build (dependencies are cached)
#   bash dists/remaster/build-windows.sh clean      # reconfigure ScummVM from scratch
#
# Host requirements: a C toolchain for the host (build-essential), cmake >= 3.16, make, curl, xz-utils,
# python3. Everything else is downloaded from its official release, checked against a SHA-256 digest and
# built into remaster-build/ (git-ignored):
#   llvm-mingw 20260616 (Windows cross compiler), zlib 1.3.1, libpng 1.6.44, SDL2 2.32.10,
#   ncnn 20260526 (GPU neural network inference via Vulkan, for the real-time AI upscaler),
#   ONNX Runtime 1.24.4 + DirectML 1.15.4 (headers; their DLLs are shipped and loaded at run time).
# Result: remaster-build/out/scummvm.exe and SDL2.dll. Package with dists/remaster/package.sh.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${REMASTER_BUILD_DIR:-$REPO/remaster-build}"
DL="$WORK/downloads"
SRC="$WORK/src"
PREFIX="$WORK/prefix"          # zlib, libpng, SDL2, ncnn for x86_64-w64-mingw32
OUT="$WORK/out"
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$DL" "$SRC" "$PREFIX" "$OUT"

for tool in cmake make curl xz python3 sha256sum; do
	command -v "$tool" >/dev/null || { echo "missing host tool: $tool"; exit 1; }
done

step() { echo; echo "==> $*"; }

fetch() { # url sha256 file
	local url="$1" sha="$2" file="$DL/$3"
	if [ ! -f "$file" ] || ! echo "$sha  $file" | sha256sum -c --quiet - 2>/dev/null; then
		echo "downloading $3"
		curl -fL --retry 3 -o "$file.part" "$url"
		mv "$file.part" "$file"
	fi
	echo "$sha  $file" | sha256sum -c --quiet -
}

unpack() { # file dir-name
	[ -d "$SRC/$2" ] && return 0
	case "$1" in
		*.tar.xz|*.tar.gz) tar -xf "$DL/$1" -C "$SRC" ;;
		*.zip) mkdir -p "$SRC/$2"; python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$DL/$1" "$SRC/$2" ;;
	esac
	[ -d "$SRC/$2" ] || { echo "unexpected archive layout: $1"; exit 1; }
}

# ── Downloads ─────────────────────────────────────────────────────────────────────────────────────
step "Fetching dependencies"
LLVM=llvm-mingw-20260616-ucrt-ubuntu-22.04-x86_64
fetch "https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/$LLVM.tar.xz" \
	534b92e067b22a6b4441f48ae9240a3341b17825d04d577eab0cf85c44b4deda "$LLVM.tar.xz"
fetch "https://zlib.net/fossils/zlib-1.3.1.tar.gz" \
	9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib-1.3.1.tar.gz
fetch "https://download.sourceforge.net/libpng/libpng-1.6.44.tar.gz" \
	8c25a7792099a0089fa1cc76c94260d0bb3f1ec52b93671b572f8bb61577b732 libpng-1.6.44.tar.gz
fetch "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz" \
	5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 SDL2-2.32.10.tar.gz
fetch "https://github.com/Tencent/ncnn/releases/download/20260526/ncnn-20260526-full-source.zip" \
	754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b ncnn-20260526-full-source.zip
# ONNX Runtime + DirectML (Microsoft, MIT / DirectML redistributable licence): optional DirectML upscalers. Only the
# C headers are used at build time; the DLLs are loaded at run time if present next to scummvm.exe.
fetch "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/microsoft.ml.onnxruntime.directml.1.24.4.nupkg" \
	57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681 onnxruntime-directml-1.24.4.zip
fetch "https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg" \
	4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9 directml-1.15.4.zip
unpack "$LLVM.tar.xz" "$LLVM"
unpack zlib-1.3.1.tar.gz zlib-1.3.1
unpack libpng-1.6.44.tar.gz libpng-1.6.44
unpack SDL2-2.32.10.tar.gz SDL2-2.32.10
unpack ncnn-20260526-full-source.zip ncnn-20260526
unpack onnxruntime-directml-1.24.4.zip onnxruntime-directml-1.24.4
unpack directml-1.15.4.zip directml-1.15.4
mkdir -p "$PREFIX/include/onnxruntime"
cp "$SRC/onnxruntime-directml-1.24.4/build/native/include/"*.h "$PREFIX/include/onnxruntime/"
cp "$SRC/onnxruntime-directml-1.24.4/runtimes/win-x64/native/onnxruntime.dll" \
	"$SRC/onnxruntime-directml-1.24.4/runtimes/win-x64/native/onnxruntime_providers_shared.dll" \
	"$SRC/directml-1.15.4/bin/x64-win/DirectML.dll" "$OUT/"

TC="$SRC/$LLVM/bin"
HOST=x86_64-w64-mingw32
export PATH="$TC:$PATH"
CMAKE_CROSS=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64
	-DCMAKE_C_COMPILER="$TC/$HOST-gcc" -DCMAKE_CXX_COMPILER="$TC/$HOST-g++" -DCMAKE_RC_COMPILER="$TC/$HOST-windres"
	-DCMAKE_FIND_ROOT_PATH="$PREFIX" -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
	-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
	-DCMAKE_INSTALL_PREFIX="$PREFIX")

# ── Libraries ─────────────────────────────────────────────────────────────────────────────────────
if [ ! -f "$PREFIX/lib/libz.a" ]; then
	step "Building zlib"
	make -C "$SRC/zlib-1.3.1" -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a >/dev/null
	mkdir -p "$PREFIX/lib" "$PREFIX/include"
	cp "$SRC/zlib-1.3.1/libz.a" "$PREFIX/lib/"
	cp "$SRC/zlib-1.3.1/zlib.h" "$SRC/zlib-1.3.1/zconf.h" "$PREFIX/include/"
fi
if [ ! -f "$PREFIX/lib/libpng16.a" ]; then
	step "Building libpng"
	cmake -S "$SRC/libpng-1.6.44" -B "$SRC/libpng-1.6.44/build" "${CMAKE_CROSS[@]}" \
		-DZLIB_ROOT="$PREFIX" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DSKIP_INSTALL_EXECUTABLES=ON >/dev/null
	cmake --build "$SRC/libpng-1.6.44/build" -j"$JOBS" >/dev/null
	cmake --install "$SRC/libpng-1.6.44/build" >/dev/null
fi
if [ ! -f "$PREFIX/bin/SDL2.dll" ]; then
	step "Building SDL2 (default features)"
	cmake -S "$SRC/SDL2-2.32.10" -B "$SRC/SDL2-2.32.10/build" "${CMAKE_CROSS[@]}" >/dev/null
	cmake --build "$SRC/SDL2-2.32.10/build" -j"$JOBS" >/dev/null
	cmake --install "$SRC/SDL2-2.32.10/build" >/dev/null
fi
if [ ! -f "$PREFIX/lib/libncnn.a" ]; then
	step "Building ncnn (Vulkan, loads vulkan-1.dll from the GPU driver at runtime)"
	NCNN_SRC="$SRC/ncnn-20260526"
	[ -f "$NCNN_SRC/CMakeLists.txt" ] || NCNN_SRC="$(dirname "$(find "$SRC/ncnn-20260526" -maxdepth 2 -name CMakeLists.txt | head -1)")"
	cmake -S "$NCNN_SRC" -B "$NCNN_SRC/build" "${CMAKE_CROSS[@]}" \
		-DNCNN_VULKAN=ON -DNCNN_SIMPLEVK=ON -DNCNN_OPENMP=OFF -DNCNN_SHARED_LIB=OFF -DNCNN_BUILD_TOOLS=OFF \
		-DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_PYTHON=OFF >/dev/null
	cmake --build "$NCNN_SRC/build" -j"$JOBS" >/dev/null
	cmake --install "$NCNN_SRC/build" >/dev/null
fi

# ── ScummVM ───────────────────────────────────────────────────────────────────────────────────────
BUILD="$WORK/scummvm-windows"
[ "${1:-}" = "clean" ] && rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f config.mk ]; then
	step "Configuring ScummVM (SCUMM engine only)"
	SDL_CONFIG="$PREFIX/bin/sdl2-config" CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
	PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
	"$REPO/configure" --host=$HOST --with-zlib-prefix="$PREFIX" --with-png-prefix="$PREFIX" \
		--with-sdl-prefix="$PREFIX" --opengl-mode=gl --disable-nasm --enable-optimizations \
		--disable-all-engines --enable-engine=scumm,scumm-7-8 > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
	grep -qE "^USE_PNG = 1" config.mk || { echo "configure did not find libpng"; exit 1; }
	cat >> config.mk <<EOF
# Real-time AI upscaling (engines/scumm/remaster_ai.cpp)
DEFINES += -DUSE_REMASTER_AI
DEFINES += -DUSE_REMASTER_ORT
INCLUDES += -I$PREFIX/include
LIBS += -L$PREFIX/lib -lncnn -lglslang -lSPIRV -lMachineIndependent -lOSDependent -lGenericCodeGen -lglslang-default-resource-limits -Wl,-Bstatic -lpthread -Wl,-Bdynamic
EOF
fi
step "Building ScummVM"
make -j"$JOBS" > make.log 2>&1 || { grep -E ": error:|\*\*\*" make.log | head -30; exit 1; }
$HOST-strip scummvm.exe -o "$OUT/scummvm.exe"
cp "$PREFIX/bin/SDL2.dll" "$OUT/"
step "Done"
ls -l "$OUT"
sha256sum "$OUT/scummvm.exe" "$OUT/SDL2.dll"
