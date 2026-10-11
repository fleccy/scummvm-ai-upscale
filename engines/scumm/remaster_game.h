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

#ifndef SCUMM_REMASTER_GAME_H
#define SCUMM_REMASTER_GAME_H

#include "common/scummsys.h"
#include "common/str.h"

namespace Scumm {

// What the remaster knows about a game: which rooms need special treatment and which features it supports.
// Room lists are comma-separated room numbers; each can be overridden in scummvm.ini (the key named next to it).
struct RemasterGameProfile {
	const char *name;
	bool widescreen;            // true widescreen (camera logic checked for this game)
	bool controller;            // direct walking with the stick, hotspot cycling
	const char *sideArtRooms;      // narrow rooms whose sides suit painted side art (remaster_live_sides_rooms)
	const char *singleScreenRooms; // wider than the screen but shown one 640-wide picture at a time (remaster_single_screen_rooms)
	const char *edgeStrictRooms;   // animated scenery visibly leaves the picture: any actor at the edge fades the side art (remaster_edge_strict_rooms)
	const char *liveEdgeRooms;     // animated scenery (water) continues into the side art (remaster_live_edge_rooms)
	const char *zoomSkipRooms;     // never zoomed to fill, e.g. menus (remaster_zoom_skip_rooms)
	const char *menuRooms;         // the game's own menu (remaster_live_sides_menu_rooms)
	bool hdActors;                 // HD characters on by default (remaster_hd_actors overrides)
	// Readable text drawn with Scale2x-style edges instead of smooth letters by default (remaster_sharp_text overrides):
	// for thin one-pixel fonts without outlines, whose diagonal strokes the smooth letters lose
	bool sharpText;
	// Grain guard on by default (remaster_grain_guard overrides): off for art dithered all over (most of the picture
	// would count as grain and be blurred)
	bool grainGuard;
	// Zoom to fill offered for narrow rooms (F12, settings menu, options): only where it has been checked; never for
	// the classic games, whose verb panel is part of the picture (remaster_zoom in the configuration file still works)
	bool zoomToFill;
	// True widescreen keeps the wider view inside what the room's camera limits (VAR_CAMERA_MIN_X/MAX_X) let the original
	// show; while the limits allow less than the wider view, only the original picture is shown (as in a single-screen
	// room). Scripts narrow the limits to keep part of a room out of a scene (remaster_limit_view overrides)
	bool limitView;
	// The AI models used unless the configuration file names one (profiles that leave them out get the defaults):
	// the real-time network (remaster_ai_net; nullptr: the standard one, "default") and the HD background model
	// (remaster_bg_net; nullptr: kRemasterDefaultBgNet). Performance mode still takes the lightest real-time network.
	const char *aiNet;
	const char *bgNet;
	// The game uses F9 itself (Loom: text on screen on/off, which it tells the player): the settings menu is on Alt+F9
	// (false unless a profile sets it)
	bool gameUsesF9;
	// The verb screen is drawn as pixel art (Scale2x edges) over the AI picture instead of through the AI (Loom: the
	// distaff's one-pixel staff lines and small notes blurred away); false unless a profile sets it
	bool pixelArtPanel;
};

// The HD background model of games whose profile names none.
extern const char *const kRemasterDefaultBgNet;

// The profile for a game, or nullptr when the remaster does not support it (yet).
const RemasterGameProfile *remasterGameProfile(byte gameId);

// Test aid: the remaster options the options dialog (Game tab) lists for a target, with their defaults
// ("key=0/1, ..."; metaengine.cpp).
Common::String remasterDescribeGameOptions(const Common::String &target);

// Whether room is in a comma-separated list of room numbers.
bool remasterRoomInList(const char *list, int room);

} // End of namespace Scumm

#endif
