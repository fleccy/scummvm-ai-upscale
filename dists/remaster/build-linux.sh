#!/bin/bash
# Build the remaster fork of ScummVM for 64-bit Linux (x86_64: Steam Deck / SteamOS, desktop distros),
# natively on Ubuntu 24.04 / Debian (WSL tested). No root needed.
#
#   bash dists/remaster/build-linux.sh            # build (dependencies are cached)
#   bash dists/remaster/build-linux.sh clean      # reconfigure ScummVM from scratch
#
# Host requirements: gcc/g++ (build-essential), make, curl, python3, apt-get + dpkg (only to fetch headers,
# see below). Everything else lands in remaster-build-linux/ (git-ignored):
#   cmake 3.31.8 (Kitware binary), zlib 1.3.1, libpng 1.6.44 (static), SDL2 2.32.10 (shared, bundled as
#   libSDL2-2.0.so.0 next to the binary), ncnn 20260526 + glslang (static, Vulkan via simplevk: libvulkan.so.1
#   is dlopen()ed from the GPU driver at runtime, so no Vulkan SDK is needed).
# Release tarballs are checked against SHA-256 digests. SDL2 needs X11/Wayland/ALSA/PulseAudio/udev
# *headers* only (it dlopen()s those libraries at runtime); when they are not installed system-wide they are
# fetched with `apt-get download` (signature-checked by apt, no root) and unpacked into a private sysroot.
# Result: remaster-build-linux/out/scummvm + libSDL2-2.0.so.0 (RUNPATH=$ORIGIN). Package: package-linux.sh.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${REMASTER_BUILD_DIR:-$REPO/remaster-build-linux}"
DL="$WORK/downloads"
SRC="$WORK/src"
PREFIX="$WORK/prefix"          # zlib, libpng, SDL2, ncnn (x86_64-linux-gnu)
SYSROOT="$WORK/sysroot"        # unpacked -dev .debs (headers + sonames for SDL's dynamic loading)
TOOLS="$WORK/tools"
OUT="$WORK/out"
JOBS="$(nproc 2>/dev/null || echo 4)"
MULTIARCH=x86_64-linux-gnu
mkdir -p "$DL" "$SRC" "$PREFIX" "$OUT" "$TOOLS/bin"

for tool in gcc g++ make curl python3 sha256sum objdump; do
	command -v "$tool" >/dev/null || { echo "missing host tool: $tool"; exit 1; }
done

step() { echo; echo "==> $*"; }

fetch() { # url sha256 file
	local url="$1" sha="$2" file="$DL/$3"
	if [ ! -f "$file" ] && [ -f "$REPO/remaster-build/downloads/$3" ]; then
		cp "$REPO/remaster-build/downloads/$3" "$file"   # reuse the Windows build's cache (re-verified below)
	fi
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
CMAKE_PKG=cmake-3.31.8-linux-x86_64
fetch "https://github.com/Kitware/CMake/releases/download/v3.31.8/$CMAKE_PKG.tar.gz" \
	630615d8e98ac33eba7fbe472626dff5c899c85af3c024585ae109166a6909d0 "$CMAKE_PKG.tar.gz"
fetch "https://zlib.net/fossils/zlib-1.3.1.tar.gz" \
	9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib-1.3.1.tar.gz
fetch "https://download.sourceforge.net/libpng/libpng-1.6.44.tar.gz" \
	8c25a7792099a0089fa1cc76c94260d0bb3f1ec52b93671b572f8bb61577b732 libpng-1.6.44.tar.gz
fetch "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz" \
	5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 SDL2-2.32.10.tar.gz
fetch "https://github.com/Tencent/ncnn/releases/download/20260526/ncnn-20260526-full-source.zip" \
	754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b ncnn-20260526-full-source.zip
unpack "$CMAKE_PKG.tar.gz" "$CMAKE_PKG"
unpack zlib-1.3.1.tar.gz zlib-1.3.1
unpack libpng-1.6.44.tar.gz libpng-1.6.44
unpack SDL2-2.32.10.tar.gz SDL2-2.32.10
unpack ncnn-20260526-full-source.zip ncnn-20260526
CMAKE="$SRC/$CMAKE_PKG/bin/cmake"

# ── Header sysroot for SDL2's video/audio/input back-ends ─────────────────────────────────────────
# Packages are Ubuntu 24.04 (noble) / Debian names. Runtime packages are included so the lib*.so
# symlinks resolve and SDL can read each library's SONAME for dlopen().
SYSPKGS=(
	pkgconf-bin libpkgconf3 libwayland-bin
	x11proto-dev libx11-dev libx11-6 libxext-dev libxext6 libxrandr-dev libxrandr2 libxrender-dev libxrender1
	libxi-dev libxi6 libxcursor-dev libxcursor1 libxfixes-dev libxfixes3 libxinerama-dev libxinerama1
	libxss-dev libxss1 libxcb1-dev libxcb1 libxau-dev libxau6 libxdmcp-dev libxdmcp6
	libffi-dev libffi8 libwayland-dev libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 wayland-protocols
	libxkbcommon-dev libxkbcommon0 libdecor-0-dev libdecor-0-0 libegl-dev libegl1 libgl-dev libgl1
	libasound2-dev libasound2t64 libpulse-dev libpulse0 libudev-dev libudev1 libdbus-1-dev libdbus-1-3
)
if [ "$(cat "$SYSROOT/.done" 2>/dev/null)" != "${SYSPKGS[*]}" ]; then
	step "Fetching SDL2 back-end headers (apt-get download, no root)"
	command -v apt-get >/dev/null && command -v dpkg >/dev/null \
		|| { echo "apt-get/dpkg not found: install the SDL2 build dependencies (X11, Wayland, ALSA, PulseAudio, udev headers, pkg-config) and adapt SYSPKGS"; exit 1; }
	rm -rf "$SYSROOT" "$DL/debs"; mkdir -p "$SYSROOT" "$DL/debs" "$WORK/apt/lists/partial" "$WORK/apt/cache/archives/partial"
	# Private package index (the system one may be stale and refreshing it needs root).
	APT_OPTS=(-o Dir::State::Lists="$WORK/apt/lists" -o Dir::Cache="$WORK/apt/cache" -o Debug::NoLocking=1)
	apt-get "${APT_OPTS[@]}" -qq update
	(cd "$DL/debs" && apt-get "${APT_OPTS[@]}" download "${SYSPKGS[@]}")
	for deb in "$DL/debs"/*.deb; do dpkg -x "$deb" "$SYSROOT"; done
	sha256sum "$DL/debs"/*.deb > "$SYSROOT/debs.sha256"
	echo "${SYSPKGS[*]}" > "$SYSROOT/.done"
fi
SYSLIB="$SYSROOT/usr/lib/$MULTIARCH"
# pkg-config / wayland-scanner run from the sysroot.
cat > "$TOOLS/bin/pkg-config" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH="$SYSLIB\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "$SYSROOT/usr/bin/pkgconf" "\$@"
EOF
cat > "$TOOLS/bin/wayland-scanner" <<EOF
#!/bin/sh
export LD_LIBRARY_PATH="$SYSLIB\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
exec "$SYSROOT/usr/bin/wayland-scanner" "\$@"
EOF
chmod +x "$TOOLS/bin/pkg-config" "$TOOLS/bin/wayland-scanner"
export PATH="$TOOLS/bin:$PATH"

CMAKE_COMMON=(-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_PREFIX_PATH="$PREFIX")

# ── Libraries ─────────────────────────────────────────────────────────────────────────────────────
if [ ! -f "$PREFIX/lib/libz.a" ]; then
	step "Building zlib (static)"
	(cd "$SRC/zlib-1.3.1" && CFLAGS="-O2 -fPIC" ./configure --static --prefix="$PREFIX" >/dev/null \
		&& make -j"$JOBS" libz.a >/dev/null && make install >/dev/null)
fi
if [ ! -f "$PREFIX/lib/libpng16.a" ]; then
	step "Building libpng (static)"
	"$CMAKE" -S "$SRC/libpng-1.6.44" -B "$SRC/libpng-1.6.44/build-linux" "${CMAKE_COMMON[@]}" \
		-DZLIB_ROOT="$PREFIX" -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DSKIP_INSTALL_EXECUTABLES=ON >/dev/null
	"$CMAKE" --build "$SRC/libpng-1.6.44/build-linux" -j"$JOBS" >/dev/null
	"$CMAKE" --install "$SRC/libpng-1.6.44/build-linux" >/dev/null
fi
if [ ! -f "$PREFIX/lib/libSDL2-2.0.so.0" ] || [ "$SYSROOT/.done" -nt "$PREFIX/lib/libSDL2-2.0.so.0" ]; then
	step "Building SDL2 (X11 + Wayland video, PulseAudio/PipeWire-pulse + ALSA audio, all dlopen()ed)"
	SDLB="$SRC/SDL2-2.32.10/build-linux"
	rm -rf "$SDLB"
	PKG_CONFIG_SYSROOT_DIR="$SYSROOT" PKG_CONFIG_LIBDIR="$SYSLIB/pkgconfig:$SYSROOT/usr/share/pkgconfig" \
	"$CMAKE" -S "$SRC/SDL2-2.32.10" -B "$SDLB" "${CMAKE_COMMON[@]}" \
		-DCMAKE_C_FLAGS="-I$SYSROOT/usr/include -I$SYSROOT/usr/include/$MULTIARCH" \
		-DCMAKE_LIBRARY_PATH="$SYSLIB" -DCMAKE_INCLUDE_PATH="$SYSROOT/usr/include" \
		-DPKG_CONFIG_EXECUTABLE="$TOOLS/bin/pkg-config" -DWAYLAND_SCANNER="$TOOLS/bin/wayland-scanner" \
		-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF \
		-DSDL_X11=ON -DSDL_X11_SHARED=ON -DSDL_WAYLAND=ON -DSDL_WAYLAND_SHARED=ON -DSDL_WAYLAND_LIBDECOR=ON \
		-DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO=ON -DSDL_PULSEAUDIO_SHARED=ON \
		-DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_KMSDRM=OFF -DSDL_LIBUDEV=ON \
		-DSDL_DBUS=ON -DSDL_IBUS=OFF -DSDL_OPENGL=ON -DSDL_OPENGLES=ON -DSDL_VULKAN=ON > "$WORK/sdl2-cmake.log"
	grep -hE "DYNAMIC|HAVE_DBUS" \
		"$SDLB/include/SDL_config.h" "$SDLB/include-config-release/SDL2/SDL_config.h" 2>/dev/null || true
	for need in SDL_VIDEO_DRIVER_X11_DYNAMIC SDL_VIDEO_DRIVER_WAYLAND_DYNAMIC SDL_AUDIO_DRIVER_PULSEAUDIO_DYNAMIC SDL_AUDIO_DRIVER_ALSA_DYNAMIC SDL_UDEV_DYNAMIC; do
		grep -rqs "#define $need " "$SDLB"/include* || { echo "SDL2 configured without $need (see $WORK/sdl2-cmake.log)"; exit 1; }
	done
	"$CMAKE" --build "$SDLB" -j"$JOBS" >/dev/null
	"$CMAKE" --install "$SDLB" >/dev/null
fi
if [ ! -f "$PREFIX/lib/libncnn.a" ]; then
	step "Building ncnn (Vulkan via simplevk, dlopen()s libvulkan.so.1 from the GPU driver at runtime)"
	NCNN_SRC="$SRC/ncnn-20260526"
	[ -f "$NCNN_SRC/CMakeLists.txt" ] || NCNN_SRC="$(dirname "$(find "$SRC/ncnn-20260526" -maxdepth 2 -name CMakeLists.txt | head -1)")"
	"$CMAKE" -S "$NCNN_SRC" -B "$NCNN_SRC/build-linux" "${CMAKE_COMMON[@]}" \
		-DNCNN_VULKAN=ON -DNCNN_SIMPLEVK=ON -DNCNN_OPENMP=OFF -DNCNN_SHARED_LIB=OFF -DNCNN_BUILD_TOOLS=OFF \
		-DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_PYTHON=OFF >/dev/null
	"$CMAKE" --build "$NCNN_SRC/build-linux" -j"$JOBS" > "$WORK/ncnn-build.log" 2>&1 || { tail -30 "$WORK/ncnn-build.log"; exit 1; }
	"$CMAKE" --install "$NCNN_SRC/build-linux" >/dev/null
fi

# ── ScummVM ───────────────────────────────────────────────────────────────────────────────────────
BUILD="$WORK/scummvm-linux"
[ "${1:-}" = "clean" ] && rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
if [ ! -f config.mk ]; then
	step "Configuring ScummVM (SCUMM engine only)"
	# PKG_CONFIG_LIBDIR limits auto-detection to our prefix, so no optional host library sneaks in.
	# SDL_syswm.h needs <X11/Xlib.h>: expose only the X11 headers of the sysroot, nothing else.
	mkdir -p "$WORK/x11-include"; ln -sfn "$SYSROOT/usr/include/X11" "$WORK/x11-include/X11"
	SDL_CONFIG="$PREFIX/bin/sdl2-config" CPPFLAGS="-I$PREFIX/include -I$WORK/x11-include" LDFLAGS="-L$PREFIX/lib" \
	PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" \
	"$REPO/configure" --with-zlib-prefix="$PREFIX" --with-png-prefix="$PREFIX" \
		--with-sdl-prefix="$PREFIX" --enable-optimizations \
		--disable-all-engines --enable-engine=scumm,scumm-7-8 > configure.log 2>&1 || { tail -30 configure.log; exit 1; }
	grep -qE "^USE_PNG = 1" config.mk || { echo "configure did not find libpng"; exit 1; }
	cat >> config.mk <<EOF
# Real-time AI upscaling (engines/scumm/remaster_ai.cpp)
DEFINES += -DUSE_REMASTER_AI
INCLUDES += -I$PREFIX/include
LIBS += -L$PREFIX/lib -lncnn -lglslang -lSPIRV -lMachineIndependent -lOSDependent -lGenericCodeGen -lglslang-default-resource-limits -lpthread -ldl
# Portable binary: libstdc++/libgcc linked in, bundled libSDL2-2.0.so.0 found next to the executable.
LDFLAGS += -static-libstdc++ -static-libgcc -Wl,-rpath,'\$\$ORIGIN' -Wl,--enable-new-dtags
EOF
fi
step "Building ScummVM"
make -j"$JOBS" > make.log 2>&1 || { grep -E ": error:|\*\*\*|undefined reference" make.log | head -30; exit 1; }
strip scummvm -o "$OUT/scummvm"
cp -L "$PREFIX/lib/libSDL2-2.0.so.0" "$OUT/"
strip --strip-unneeded "$OUT/libSDL2-2.0.so.0"
step "Done"
ls -l "$OUT"
sha256sum "$OUT/scummvm" "$OUT/libSDL2-2.0.so.0"
echo "Max glibc symbol version required:"
objdump -T "$OUT/scummvm" "$OUT/libSDL2-2.0.so.0" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
