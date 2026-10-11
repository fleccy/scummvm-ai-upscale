#!/bin/sh
# Start The Curse of Monkey Island with real-time AI upscaling (scummvm-ai-upscale, Linux / Steam Deck).
#
#   ./play.sh [options] [GAME_FOLDER]
#
#   GAME_FOLDER        folder containing COMI.LA0 (only needed once; it is remembered in scummvm.ini).
#                      Without it the first run searches the usual Steam/GOG locations, then asks.
#   --performance      lighter AI upscaling, recommended on Steam Deck (remembered)
#   --no-performance   full quality AI upscaling (remembered)
#   --windowed         start in a 1280x720 window instead of fullscreen
#   --reset            rewrite scummvm.ini with the defaults (saves are kept)
#
# Settings live in scummvm.ini next to this script, saves in saves/, the log in logs/scummvm.log.
set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
INI="$DIR/scummvm.ini"
MODEL="$DIR/models/realesr-animevideov3-x3"
PERF=""
WINDOWED=0
RESET=0
GAME=""

while [ $# -gt 0 ]; do
	case "$1" in
		--performance) PERF=true ;;
		--no-performance) PERF=false ;;
		--windowed) WINDOWED=1 ;;
		--reset) RESET=1 ;;
		-h|--help) sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		-*) echo "unknown option: $1 (see ./play.sh --help)" >&2; exit 2 ;;
		*) GAME="$1" ;;
	esac
	shift
done

has_data() { [ -n "$(find "$1" -maxdepth 1 -iname 'comi.la0' 2>/dev/null | head -1)" ]; }

# Accept the game folder itself or a parent (the Steam release keeps the data in ScummVM/monkey3).
resolve_game() {
	[ -d "$1" ] || return 1
	if has_data "$1"; then (cd "$1" && pwd); return 0; fi
	f="$(find "$1" -maxdepth 4 -iname 'comi.la0' 2>/dev/null | head -1)"
	[ -n "$f" ] && (cd "$(dirname "$f")" && pwd)
}

autodetect() {
	for base in \
		"$HOME/.local/share/Steam/steamapps/common/The Curse of Monkey Island" \
		"$HOME/.steam/steam/steamapps/common/The Curse of Monkey Island" \
		"$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/The Curse of Monkey Island" \
		/run/media/*/steamapps/common/"The Curse of Monkey Island" \
		/run/media/*/*/steamapps/common/"The Curse of Monkey Island" \
		/run/media/*/SteamLibrary/steamapps/common/"The Curse of Monkey Island" \
		"$HOME/GOG Games/The Curse of Monkey Island" \
		"$HOME/Games/gog/the-curse-of-monkey-island" \
		"$HOME/Games/The Curse of Monkey Island"; do
		resolve_game "$base" && return 0
	done
	return 1
}

ini_get() { # key -> value in [comi]
	awk -F= -v k="$1" '/^\[/{s=($0=="[comi]")} s && $1==k {sub(/^[^=]*=/,""); print; exit}' "$INI"
}

ini_set() { # section key value -> set key in [section] (added if missing)
	awk -v sec="[$1]" -v k="$2" -v v="$3" '
		/^\[/ { if (s && !done) { print k "=" v; done=1 } s=($0==sec) }
		s && index($0, k "=") == 1 { if (!done) print k "=" v; done=1; next }
		{ print }
		END { if (!done) { if (!s) print sec; print k "=" v } }' "$INI" > "$INI.tmp" && mv "$INI.tmp" "$INI"
}

mkdir -p "$DIR/saves" "$DIR/logs"

if [ -n "$GAME" ]; then
	G="$(resolve_game "$GAME")" || { echo "No Curse of Monkey Island data (COMI.LA0) found in: $GAME" >&2; exit 1; }
	GAME="$G"
fi

if [ ! -f "$INI" ] || [ "$RESET" = 1 ]; then
	if [ -z "$GAME" ] && [ -f "$INI" ]; then GAME="$(ini_get path)"; fi
	if [ -z "$GAME" ]; then GAME="$(autodetect)" || GAME=""; fi
	if [ -z "$GAME" ]; then
		if [ -t 0 ]; then
			printf 'Folder of your Curse of Monkey Island game (contains COMI.LA0): '
			read -r answer
			GAME="$(resolve_game "$answer")" || { echo "No COMI.LA0 found in: $answer" >&2; exit 1; }
		else
			echo "Game not found. Run once with the game folder: ./play.sh \"/path/to/The Curse of Monkey Island\"" >&2
			exit 1
		fi
	fi
	cat > "$INI" <<EOF
[scummvm]
gui_theme=scummmodern
themepath=$DIR
gui_scale=200
lastselectedgame=comi
fullscreen=true
gfx_mode=opengl
stretch_mode=fit
filtering=true
savepath=$DIR/saves
screenshotpath=$DIR/logs
updates_check=0

[comi]
gameid=comi
engineid=scumm
description=The Curse of Monkey Island (AI upscaling)
path=$GAME
savepath=$DIR/saves
subtitles=true
remaster_ai_model=$MODEL
remaster_ai_scale=3
remaster_widescreen=wide
remaster_ambient_sides=false
EOF
	echo "Wrote $INI (game: $GAME)"
else
	if [ -n "$GAME" ]; then ini_set comi path "$GAME"; fi
	# Keep the bundled paths valid if this folder was moved.
	ini_set scummvm themepath "$DIR"
	ini_set comi remaster_ai_model "$MODEL"
fi
if [ -n "$PERF" ]; then ini_set comi remaster_performance "$PERF"; fi
# Optional extras (separate release downloads, extracted into this folder): used whenever they are present.
if [ -d "$DIR/sides" ]; then ini_set comi remaster_sides_path "$DIR/sides"; fi
if [ -d "$DIR/video-sides" ]; then ini_set comi remaster_video_sides_path "$DIR/video-sides"; fi

set -- --config="$INI" --logfile="$DIR/logs/scummvm.log"
if [ "$WINDOWED" = 1 ]; then set -- "$@" --no-fullscreen --window-size=1280,720; fi
cd "$DIR/logs"
exec "$DIR/scummvm" "$@" comi
