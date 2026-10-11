/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "common/str.h"
#include "scumm/detection.h"
#include "scumm/remaster_game.h"

namespace Scumm {

// Profiles that name no HD background model keep Real-ESRGAN x4plus anime (runs everywhere); a game opts in to
// 4x UltraSharpV2 once it has been compared in that game (it keeps painted detail, but turns some flat dithered
// areas into scribbles). Where UltraSharpV2 cannot run (no DirectX 12 / ONNX Runtime, e.g. Linux) x4plus anime is used.
const char *const kRemasterDefaultBgNet = "x4anime";

static const RemasterGameProfile kComi = {
	"The Curse of Monkey Island",
	true,   // widescreen
	true,   // controller
	// narrow rooms with side art (73, the Monkey Island 1 screen, and 89, a close-up, keep the glow)
	"9,10,11,12,13,16,17,18,19,20,21,23,24,26,27,28,29,30,31,32,34,35,36,37,38,39,52,54,56,57,58,59,62,63,64,65,66,67,"
	"68,69,71,72,74,76,77,78,79,80,81,82,83,84,85,90,94",
	"25",        // the banjo duel
	"83,84,85",  // the island map's clouds, the diorama ride's cart
	"11,13",     // the ship's side, the island map
	"87",        // the difficulty sign at the start
	"92",        // save / load / options
	true,        // HD characters
	false,       // sharp text
	true,        // grain guard
	true,        // zoom to fill
	false,       // limit view: COMI's rooms were checked one by one for widescreen
	nullptr,          // real-time AI model: the standard one
	// HD background model: x4anime. UltraSharpV2 (F9) is more faithful but keeps more of the original's grain: a change
	// of style rather than the cleaner remastered look
	nullptr,
};

// The Dig (SCUMM v7) shares most of the renderer (AKOS costumes, NUT fonts, SMUSH cutscenes): true widescreen
// (424 x 200), no special rooms until it has been played through with the remaster.
static const RemasterGameProfile kDig = {
	"The Dig",
	true,   // widescreen
	true,   // controller
	"",     // side art rooms
	"23,39,83",   // single-screen rooms: several scenes side by side in one wide room
	"",     // edge-strict rooms
	"",     // live-edge rooms
	"",     // zoom-skip rooms
	"",     // menu rooms
	// HD characters off: its sprites are small (320 x 200) and the heavy network flattens their faces; the
	// real-time AI pass keeps them
	false,
	false,  // sharp text
	true,   // grain guard
	false,  // zoom to fill: not checked through the game (text near the bottom of close-ups can be left out of view)
	true,   // limit view
	"anisd",          // real-time AI model: AniSD stays closest to the soft low-resolution art
	"ultrasharpv2",   // HD background model
};

// Indiana Jones and the Fate of Atlantis (SCUMM v5, 320 x 200): classic costumes, charset text, the verb and
// inventory panel at the bottom: the real-time AI pass over the whole frame, true widescreen on request (the room is
// 424 wide, the panel keeps its width and is centred); no controller walking, no HD characters (built for AKOS
// costumes).
static const RemasterGameProfile kIndy4 = {
	"Indiana Jones and the Fate of Atlantis",
	true,   // widescreen (remaster_widescreen=wide)
	false,  // controller
	"",     // side art rooms
	// single-screen rooms: 58, the binocular view (a panorama behind a mask the original screen width); 62, the German
	// laboratory cutscene (the scientist waits just past the original's right edge before he walks in)
	"58,62",
	"",     // edge-strict rooms
	"",     // live-edge rooms
	"",     // zoom-skip rooms
	"",     // menu rooms
	false,  // HD characters (the AKOS cel path is wired to classic costumes too; remaster_hd_actors=true tries it)
	true,   // sharp text: the classic charset is thin, without outlines
	true,   // grain guard
	false,  // zoom to fill: the verb panel is part of the picture
	// limit view: room 15 holds two scenes side by side; while the ransacked study is played the camera limits keep the
	// original on its 320 columns (Indy waits just past the edge to walk in, the other room is beside it)
	true,
	// AI models: the standard ones. Compared in rooms 10, 19 and 64: AniScale2S turns the dithered costumes into speckle
	// (the standard network keeps them smooth); UltraSharpV2 paints roofs and pottery far better (room 19) but keeps
	// the dithering of dark streets and floors as grain (rooms 10, 64). Both are choices in the F9 menu
	nullptr,
	nullptr,
};

// Indiana Jones and the Last Crusade (SCUMM v3, 256-colour VGA, 320 x 200): the same bring-up as Fate of Atlantis.
// Its v3 charset is a one-bit font (drawBits1, with a drop shadow when the colour asks for one); room text is on the
// text layer, verb, sentence and inventory text in the verb screen. True widescreen as in Fate of Atlantis: the room
// is 424 wide; the verb panel and the message line at the top (where the game prints speech) keep their width and
// are centred.
static const RemasterGameProfile kIndy3 = {
	"Indiana Jones and the Last Crusade",
	true,   // widescreen (remaster_widescreen=wide)
	false,  // controller
	"",     // side art rooms
	"",     // single-screen rooms
	"",     // edge-strict rooms
	"",     // live-edge rooms
	"",     // zoom-skip rooms
	"",     // menu rooms
	false,  // HD characters
	true,   // sharp text: the 8 x 8 font is one pixel thin
	// grain guard off: the VGA art is dithered nearly everywhere (trees, walls, crowds, the credits), so the guard
	// blurred whole blocks of most rooms; the AI alone keeps them sharp
	false,
	false,  // zoom to fill: the verb panel is part of the picture
	true,   // limit view
	// AI models: the standard ones. Compared in rooms 15 and 73: AniScale2S keeps more of the faces but also the jagged
	// pixel steps; UltraSharpV2 keeps the architecture of Venice (room 15) but turns the flat dithered boxing ring of
	// the gym (room 73) into scribbles. Both are choices in the F9 menu
	nullptr,
	nullptr,
};

// Loom (the VGA CD talkie: SCUMM v4, 256 colours, 320 x 200, speech and music from the CD audio track): the same
// bring-up as the Indiana Jones games. Its distaff (the notes as pictures, the draft's letters as text) is the verb
// screen at the bottom; dialogue is charset text over the room.
static const RemasterGameProfile kLoom = {
	"Loom",
	true,   // widescreen (remaster_widescreen=wide)
	false,  // controller
	"",     // side art rooms
	"",     // single-screen rooms
	"",     // edge-strict rooms
	"",     // live-edge rooms
	"",     // zoom-skip rooms
	"",     // menu rooms
	false,  // HD characters
	true,   // sharp text: the charset is thin, without outlines
	// grain guard off: it faded the Loom's one-pixel threads (room 9, colour-cycled every frame) and sprinkled white dots
	// over the forge (36); the AI alone was closer to the original in every room checked
	false,
	false,  // zoom to fill: the distaff is part of the picture
	true,   // limit view
	nullptr,  // real-time AI model: the standard one (not compared for Loom yet)
	nullptr,  // HD background model: x4anime
	true,   // the game uses F9 (text on screen on/off; it says so at the start): the settings menu is on Alt+F9
	// the distaff (verb screen) as pixel art: the AI blurred its one-pixel staff lines into a grey band and the notes
	// into smudges
	true,
};

// Sam & Max Hit the Road (SCUMM v6, 320 x 200): classic costumes and charset text as in Fate of Atlantis, but no verb
// panel: the room is the whole screen (the verbs are the cursor, the inventory a box drawn over the room). True
// widescreen as in Fate of Atlantis; while the inventory box is open only the original picture is shown, centred.
static const RemasterGameProfile kSamnmax = {
	"Sam & Max Hit the Road",
	true,   // widescreen (remaster_widescreen=wide)
	false,  // controller
	"",     // side art rooms
	// single-screen rooms: 31 and 78, the coin-op viewers (a landscape behind a mask the original width); 70 and 91, the
	// dress-up pages (the next page lies beside the one shown); 77, the title strip; 84, the Wax Me booth
	"31,70,77,78,84,91",
	"",     // edge-strict rooms
	"",     // live-edge rooms
	"",     // zoom-skip rooms
	"",     // menu rooms
	// HD characters off, as in the other 320 x 200 games: the heavy network flattens the small sprites' faces
	false,
	false,  // sharp text: the talk font has a dark outline; the smooth letters keep it
	// grain guard off: it changed nothing measurable in the rooms checked, but took the dress-up screen's fine hatching
	// (room 91) for grain and blurred it all
	false,
	false,  // zoom to fill
	true,   // limit view
	"anisd",  // real-time AI model: keeps Sam's and Max's faces (eyes, mouths) where the standard one melts them
	nullptr,  // HD background model: x4anime
	false,  // the game does not use F9
	false,  // no verb panel
};

const RemasterGameProfile *remasterGameProfile(byte gameId) {
	switch (gameId) {
	case GID_CMI:
		return &kComi;
	case GID_DIG:
		return &kDig;
	case GID_INDY4:
		return &kIndy4;
	case GID_INDY3:
		return &kIndy3;
	case GID_LOOM:
		return &kLoom;
	case GID_SAMNMAX:
		return &kSamnmax;
	default:
		return nullptr;
	}
}

bool remasterRoomInList(const char *list, int room) {
	for (const char *p = list; p && *p;) {
		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (!*p)
			break;
		int r = 0;
		while (*p >= '0' && *p <= '9')
			r = r * 10 + (*p++ - '0');
		if (r == room)
			return true;
	}
	return false;
}

} // End of namespace Scumm
