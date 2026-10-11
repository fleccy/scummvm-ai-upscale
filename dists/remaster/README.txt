scummvm-ai-upscale - real-time AI upscaling for The Curse of Monkey Island
=========================================================================

This is an unofficial fork of ScummVM (https://www.scummvm.org). It does NOT contain the game.
You need your own copy of The Curse of Monkey Island (Steam or GOG).

Install
-------
1. Extract this zip anywhere.
   (Side art or widened cutscenes you generated yourself: put the sides / video-sides folders here.)
2. Double-click install.cmd.
   A setup window opens: it finds your Steam or GOG copy (or lets you choose the folder),
   asks where to install (%LOCALAPPDATA%\ScummVM-AI-Upscale by default), how you want to play
   and which optional extras you want, then installs and creates a desktop shortcut.
   (Text version instead: "install.cmd /console", or install.ps1 with parameters.)
   Your game files are only read, never changed.
   On NVIDIA cards it also offers the optional TensorRT upscaler (about 1.5 GB download).
3. Start "The Curse of Monkey Island (AI upscaling)" from the desktop.

The Dig (experimental): if you also have The Dig (Steam or GOG), setup finds it and offers to set it
up too, or for The Dig alone ("install.ps1 -Games comi,dig", "-Games dig", "-DigPath <folder>").
It gets its own shortcut, "The Dig (AI upscaling)", and a [dig] section in scummvm.ini. AI upscaling,
HD backgrounds and true 16:9 work; Monkey Island's room-by-room extras (side art, widened cutscenes,
preparing the HD backgrounds in advance) are not made for it yet, and HD characters are off by default.

Indiana Jones and the Fate of Atlantis and Indiana Jones and the Last Crusade (experimental): found and
offered the same way (Steam or GOG; "-Games atlantis,indy3", "-AtlantisPath <folder>", "-Indy3Path <folder>").
Each gets its own shortcut and section ([atlantis], [indy3]) in scummvm.ini. AI upscaling, HD backgrounds and
readable text and true 16:9 work.

Loom (VGA CD talkie) and Sam & Max Hit the Road (CD) (experimental): found and offered the same way
("-Games loom,samnmax", "-LoomPath <folder>", "-SamnmaxPath <folder>"); targets [loom] and [samnmax]. AI upscaling,
readable text and true 16:9 work. Loom needs its CD audio track ripped next to the game files as FLAC
(track1.flac): speech and music come from it.
In Loom the remaster settings menu is on Alt+F9 (Loom uses F9 itself).

"Windows protected your PC"? This hobby project isn't code-signed, so Windows asks once when you start
install.cmd: click More info > Run anyway if you trust it (the source is on the project page). The
installer removes the download mark from the files it installed, so the game starts without a second prompt.

Requirements: 64-bit Windows 10/11 and a graphics card with an up-to-date Vulkan driver
(any recent NVIDIA, AMD or Intel GPU). Without Vulkan the game still runs, just without AI upscaling.

Keys
----
F9         remaster settings: everything below in one menu (controller: Back)
F10        cycle display mode: AI HD / Original / VGA / EGA / Amiga / Modern pixel art
Ctrl+F10   CRT scanlines on/off
F11        AI upscaling on/off
Ctrl+F9    upscaler: AI (ncnn) / AI (DirectML) / Ultra AI / TensorRT (optional download) / AMD FSR
Shift+F9   photo mode: the scene as shown plus a poster-size 4x version, in the photos folder
Shift+F11  grain guard on/off (softens grainy textures the AI would over-sharpen)
F12        narrow rooms: side art / ambient glow / zoom to fill / zoom part-way (experimental) / black bars
Ctrl+F12   report a problem (screenshot + pre-filled GitHub issue)
Alt+Enter  window / fullscreen (the window can be resized; the Start menu also has a windowed shortcut)
Ctrl+F5    ScummVM menu

Settings live in scummvm.ini in the install folder:
  remaster_widescreen=wide      true 16:9 view (ambient = 4:3 with side panels, remove = plain 4:3)
  remaster_ai_scale=3           2, 3 or 4 (3 fits 1440p screens exactly)

Licences: see the licenses folder. ScummVM and this fork are GPL-3.0-or-later; source code at the
project page. "The Curse of Monkey Island" is a trademark of Lucasfilm Ltd./Disney; this project is
not affiliated with or endorsed by them.
