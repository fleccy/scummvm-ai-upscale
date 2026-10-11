scummvm-ai-upscale - real-time AI upscaling for The Curse of Monkey Island (Linux x86_64 / Steam Deck)
======================================================================================================

This is an unofficial fork of ScummVM (https://www.scummvm.org). It does NOT contain the game.
You need your own copy of The Curse of Monkey Island (Steam or GOG).

Install
-------
1. Extract the archive anywhere in your home folder, e.g.
     tar xf scummvm-ai-upscale-*-linux-x86_64.tar.gz -C ~/Games
   Side art or widened cutscenes you generated yourself: put the sides/ or video-sides/ folder here;
   play.sh uses them when they are there.
2. Run it once from a terminal (Konsole in Steam Deck desktop mode):
     ~/Games/scummvm-ai-upscale-*/play.sh
   The first run looks for the game in the usual Steam / GOG folders (also on the SD card) and
   otherwise asks for it. You can also pass the folder that contains COMI.LA0 (or a parent of it):
     ./play.sh "/path/to/The Curse of Monkey Island"
   This writes scummvm.ini next to play.sh. Your game files are only read, never changed.

Steam Deck
----------
- Performance mode is recommended on the Deck:   ./play.sh --performance
  (remembered in scummvm.ini; ./play.sh --no-performance switches back to full quality).
- Game Mode: in desktop mode, Steam > Add a Game > Add a Non-Steam Game > Browse, pick play.sh.
  Run play.sh once from a terminal first so the game folder is known.
- Use the controller layout "Gamepad with Mouse Trackpad" (or "Keyboard (WASD) and Mouse") and map
  the keys below to back buttons as you like.

Options of play.sh
------------------
  --performance / --no-performance   lighter / full AI upscaling
  --windowed                         1280x720 window instead of fullscreen
  --reset                            rewrite scummvm.ini with the defaults (saves are kept)

Requirements: 64-bit Linux with glibc 2.38 or newer, X11 or Wayland, and a Vulkan driver
(Mesa RADV on the Steam Deck; any recent NVIDIA, AMD or Intel driver). Without Vulkan the game still
runs, just without AI upscaling. SDL2 is included (libSDL2-2.0.so.0 next to scummvm).

Keys
----
F10        cycle display mode: AI HD / Original / VGA / EGA / Amiga / Modern pixel art
Ctrl+F10   CRT scanlines on/off
F11        AI upscaling on/off
Shift+F11  grain guard on/off (softens grainy textures the AI would over-sharpen)
F12        side panels: AI side art (if generated) / ambient glow / black
Ctrl+F12   report a problem (screenshot + pre-filled GitHub issue)
Alt+Enter  window / fullscreen
Ctrl+F5    ScummVM menu

Settings live in scummvm.ini next to play.sh (saves in saves/, log in logs/scummvm.log):
  remaster_widescreen=wide      true 16:9 view (ambient = 4:3 with side panels, remove = plain 4:3)
  remaster_ai_scale=3           2, 3 or 4
  remaster_performance=true     lighter AI upscaling (Steam Deck)

Licences: see the licenses folder. ScummVM and this fork are GPL-3.0-or-later; source code at the
project page. "The Curse of Monkey Island" is a trademark of Lucasfilm Ltd./Disney; this project is
not affiliated with or endorsed by them.
