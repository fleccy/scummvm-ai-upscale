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

// Remaster tooling: dump the current room's game data (walk boxes, objects) as JSON so that new
// room art can be checked against it and, later, drawn by the remaster renderer.
// Enabled with the config key remaster_dump_dir=<folder>; one file per room: room_NNNN.json.

#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "graphics/surface.h"
#include "image/png.h"

#include "scumm/boxes.h"
#include "scumm/object.h"
#include "scumm/scumm.h"

namespace Scumm {

static Common::String remasterJsonEscape(const byte *s) {
	Common::String out;
	for (; s && *s; s++) {
		if (*s == '"' || *s == '\\')
			out += '\\';
		if (*s >= 32 && *s < 127)
			out += (char)*s;
	}
	return out;
}

void ScummEngine::remasterExtractTick() {
	// remaster_extract_dir: write this room's background (the room with its objects, no characters), as the game
	// draws it from the player's own copy, to <dir>/room_NNNN.png in the room's own colours, then quit. Used by the
	// side-art generator (dists/remaster/sides/generate.py), which starts the game once per room.
	if (!ConfMan.hasKey("remaster_extract_dir") || _currentRoom <= 0)
		return;
	const uint32 now = _system->getMillis();
	if (_currentRoom != _remasterExtractRoom) {
		_remasterExtractRoom = _currentRoom;
		_remasterExtractSince = now;
		return;
	}
	if (now - _remasterExtractSince < 1500)
		return;   // let scripts set the room up
	Common::FSNode dir(Common::Path(ConfMan.get("remaster_extract_dir"), Common::Path::kNativeSeparator));
	const bool late = now - _remasterExtractSince >= 15000;
	if (!remasterWriteRoomBackground(dir.getChild(Common::String::format("room_%04d.png", _currentRoom)), late))
		return;
	warning("remaster: extracted room %d", _currentRoom);
	quitGame();
}

bool ScummEngine::remasterWriteRoomBackground(const Common::FSNode &file, bool allowIncomplete) {
	VirtScreen *vs = &_virtscr[kMainVirtScreen];
	if (!_remasterRoomBg || !_remasterRoomBgValid || _remasterRoomBgRoom != _currentRoom || !vs->getBasePtr(0, 0))
		return false;
	const int pitch = vs->pitch, vh = vs->h, w = MIN<int>(_roomWidth, pitch);
	if ((int64)pitch * vh > _remasterRoomBgSize || w <= 0)
		return false;
	bool drawn = true;
	for (int x = 0; x < w && drawn; x++)
		for (int y = 1; y < vh && drawn; y++)
			drawn = _remasterRoomBgValid[(size_t)y * pitch + x] != 0;
	if (!drawn && !allowIncomplete)
		return false;
	const byte *pal = getPalettePtr(_curPalIndex, _roomResource);   // undarkened: no fade-in or lighting effects
	if (!pal)
		pal = _remasterPal;
	Graphics::Surface s;
	s.create(w, vh, Graphics::PixelFormat(3, 8, 8, 8, 0, 0, 8, 16, 0));   // bytes R, G, B in memory
	for (int y = 0; y < vh; y++) {
		byte *d = (byte *)s.getBasePtr(0, y);
		for (int x = 0; x < w; x++) {
			const byte ix = _remasterRoomBg[(size_t)(y ? y : 1) * pitch + x];   // row 0 may be undrawn (see remasterBgUpdate)
			d[x * 3] = pal[ix * 3];
			d[x * 3 + 1] = pal[ix * 3 + 1];
			d[x * 3 + 2] = pal[ix * 3 + 2];
		}
	}
	// (readers only take files that have not changed for a moment: generate.py --serve)
	bool ok;
	{
		Common::DumpFile out;
		ok = out.open(file.getPath()) && Image::writePNG(out, s);
		out.finalize();
	}
	s.free();
	if (!ok)
		warning("remaster: could not write %s", file.getPath().toString().c_str());
	return true;
}

static const char *const kRemasterLiveSidesRooms =   // the narrow rooms whose sides suit painting (as generate.py)
	"9,10,11,12,13,16,17,18,19,20,21,23,24,26,27,28,29,30,31,32,34,35,36,37,38,39,52,54,56,57,58,59,62,63,64,65,66,67,"
	"68,69,71,72,74,76,77,78,79,80,81,82,83,84,85,90,94";

bool ScummEngine::remasterLiveSidesRoom(int room) const {
	const Common::String list = ConfMan.hasKey("remaster_live_sides_rooms") ? ConfMan.get("remaster_live_sides_rooms") : kRemasterLiveSidesRooms;
	for (const char *p = list.c_str(); *p;) {
		char *end;
		const long r = strtol(p, &end, 10);
		if (end == p) {
			p++;
			continue;
		}
		if (r == room)
			return true;
		p = end;
	}
	return false;
}

Common::FSNode ScummEngine::remasterLiveDir() const {
	if (ConfMan.hasKey("remaster_live_sides_dir"))
		return Common::FSNode(Common::Path(ConfMan.get("remaster_live_sides_dir"), Common::Path::kNativeSeparator));
	// default: <sides folder>/../sides-work/live
	Common::FSNode sides(Common::Path(ConfMan.get("remaster_sides_path"), Common::Path::kNativeSeparator));
	return sides.getParent().getChild("sides-work").getChild("live");
}

static void remasterWriteSmallFile(const Common::FSNode &file, const Common::String &text) {
	Common::DumpFile out;
	if (out.open(file.getPath()))
		out.writeString(text);
}

void ScummEngine::remasterLiveSidesTick() {
	if (!_remasterEnabled || !ConfMan.hasKey("remaster_live_sides") || !ConfMan.getBool("remaster_live_sides") ||
	    !ConfMan.hasKey("remaster_sides_path") || !_remasterLogicalWidth)
		return;
	const uint32 now = _system->getMillis();
	Common::FSNode live = remasterLiveDir();
	if (!live.exists())
		return;   // made by the helper (or the installer); the game does not create folders
	if (now - _remasterLiveAliveT >= 2000 || !_remasterLiveAliveT) {
		_remasterLiveAliveT = now;
		// "<room> play|idle": idle after a minute without input (the helper paints then; while the game is paused the
		// heartbeat stops, which it also reads as a break)
		// the game's own menu (save, load, options: room 92) is a break too, mouse or not
		const Common::String menus = ConfMan.hasKey("remaster_live_sides_menu_rooms") ? ConfMan.get("remaster_live_sides_menu_rooms") : "92";
		bool inMenu = false;
		for (const char *p = menus.c_str(); *p && !inMenu;) {
			char *end;
			const long r = strtol(p, &end, 10);
			if (end == p) {
				p++;
				continue;
			}
			inMenu = r == _currentRoom;
			p = end;
		}
		const bool idle = inMenu || now - _remasterLastInputT > 60000;
		remasterWriteSmallFile(live.getChild("alive"), Common::String::format("%d %s\n", _currentRoom, idle ? "idle" : "play"));
	}
	if (_currentRoom != _remasterLiveSidesAt) {
		_remasterLiveSidesAt = _currentRoom;
		_remasterLiveSince = now;
		_remasterLiveAsked = false;
		_remasterLiveFade = 256;
		return;
	}
	const bool narrow = _currentRoom > 0 && _roomWidth >= 320 && _roomWidth <= 640 && remasterMargin() > 0 && !_remasterVideoShown;
	if (!narrow || !remasterLiveSidesRoom(_currentRoom) || now - _remasterLiveSince < 1500)
		return;
	if (_remasterSidesRoom == _currentRoom && _remasterSides.getPixels())
		return;   // side art already there
	if (!_remasterLiveAsked) {
		// hand the room over (once per visit): its background, then "this room next"
		const Common::String name = Common::String::format("room_%04d.png", _currentRoom);
		if (!remasterWriteRoomBackground(live.getChild(name), now - _remasterLiveSince >= 15000))
			return;
		remasterWriteSmallFile(live.getChild("want"), Common::String::format("%d\n", _currentRoom));
		_remasterLiveAsked = true;
		debug(1, "remaster: live side art requested for room %d", _currentRoom);
	}
	if (now - _remasterLivePollT < 1000)
		return;
	_remasterLivePollT = now;
	Common::FSNode sides(Common::Path(ConfMan.get("remaster_sides_path"), Common::Path::kNativeSeparator));
	if (!sides.getChild(Common::String::format("%04d.png", _currentRoom)).exists())
		return;
	_remasterSidesRoom = -1;   // load it now
	if (remasterLoadSides()) {
		_remasterLiveFade = 0;   // crossfade from the glow
		_remasterFrameDirty = true;
		debug(1, "remaster: live side art arrived for room %d", _currentRoom);
	}
}

void ScummEngine::remasterDumpRoomTick() {
	if (!ConfMan.hasKey("remaster_dump_dir"))
		return;
	if (_currentRoom != _remasterDumpRoom) {
		_remasterDumpRoom = _currentRoom;
		_remasterDumpFrames = 0;
		_remasterDumpDone = false;
	}
	// Wait about a second after entering so that scripts have set object states and positions.
	if (_remasterDumpDone || ++_remasterDumpFrames < 30 || _currentRoom == 0)
		return;
	_remasterDumpDone = true;

	Common::FSNode dir(Common::Path(ConfMan.get("remaster_dump_dir"), Common::Path::kNativeSeparator));
	Common::FSNode file = dir.getChild(Common::String::format("room_%04d.json", _currentRoom));
	Common::DumpFile out;
	if (!out.open(file.getPath())) {
		warning("remaster: cannot write %s", file.getPath().toString().c_str());
		return;
	}

	out.writeString(Common::String::format("{\n  \"room\": %d,\n  \"width\": %d,\n  \"height\": %d,\n  \"camera_x\": %d,\n",
		_currentRoom, _roomWidth, _roomHeight, _virtscr[kMainVirtScreen].xstart));

	out.writeString("  \"boxes\": [\n");
	const int numBoxes = getNumBoxes();
	for (int b = 0; b < numBoxes; b++) {
		BoxCoords c = getBoxCoordinates(b);
		out.writeString(Common::String::format(
			"    {\"id\": %d, \"flags\": %d, \"points\": [[%d, %d], [%d, %d], [%d, %d], [%d, %d]]}%s\n",
			b, getBoxFlags(b), c.ul.x, c.ul.y, c.ur.x, c.ur.y, c.lr.x, c.lr.y, c.ll.x, c.ll.y,
			b + 1 < numBoxes ? "," : ""));
	}
	out.writeString("  ],\n  \"objects\": [\n");
	bool first = true;
	for (int i = 1; i < _numLocalObjects; i++) {
		const ObjectData &o = _objs[i];
		if (o.obj_nr == 0)
			continue;
		out.writeString(Common::String::format(
			"%s    {\"obj\": %d, \"name\": \"%s\", \"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d, \"walk\": [%d, %d], \"state\": %d, \"parent\": %d}",
			first ? "" : ",\n", o.obj_nr, remasterJsonEscape(getObjOrActorName(o.obj_nr)).c_str(),
			o.x_pos, o.y_pos, o.width, o.height, o.walk_x, o.walk_y, o.state, o.parent));
		first = false;
	}
	out.writeString("\n  ],\n");

	// Walk-behind masks (z-planes) for the whole room width, not just the visible strips. V8 stores per
	// 8-pixel strip a 32-bit offset table (after an 8-byte header) to a run-length encoded column of mask
	// bytes, one byte per row, 1 bit per pixel, MSB = leftmost (see Gdi::decodeMask/decompressMaskImg).
	// Written as binary PGM (0 = free, 255 = in front of actors) next to the JSON.
	int numZ = 0;
	if (_game.version == 8) {
		const byte *room = getResourceAddress(rtRoom, _roomResource);
		const byte *zplanes[9] = {};
		if (room)
			numZ = _gdi->remasterGetZPlanes(room + _IM00_offs, zplanes);
		const int strips = _roomWidth / 8;
		for (int z = 1; z < numZ; z++) {
			if (!zplanes[z])
				continue;
			Common::Array<byte> img;
			img.resize(_roomWidth * _roomHeight);
			memset(img.data(), 0, img.size());
			for (int s = 0; s < strips; s++) {
				const uint32 offs = READ_LE_UINT32(zplanes[z] + s * 4 + 8);
				if (!offs)
					continue;
				const byte *src = zplanes[z] + offs;
				int y = 0;
				while (y < _roomHeight) {
					byte b = *src++;
					const bool run = (b & 0x80) != 0;
					b &= 0x7F;
					const byte runValue = run ? *src++ : 0;
					do {
						const byte bits = run ? runValue : *src++;
						for (int bit = 0; bit < 8; bit++)
							if (bits & (0x80 >> bit))
								img[y * _roomWidth + s * 8 + bit] = 255;
						y++;
					} while (--b && y < _roomHeight);
				}
			}
			Common::DumpFile pgm;
			Common::FSNode pf = dir.getChild(Common::String::format("room_%04d_z%d.pgm", _currentRoom, z));
			if (pgm.open(pf.getPath())) {
				pgm.writeString(Common::String::format("P5\n%d %d\n255\n", _roomWidth, _roomHeight));
				pgm.write(img.data(), img.size());
				pgm.finalize();
			}
		}
	}
	out.writeString(Common::String::format("  \"zplanes\": %d\n}\n", MAX(0, numZ - 1)));
	out.finalize();
	warning("remaster: room %d dumped (%d boxes, %d z-planes) to %s", _currentRoom, numBoxes, MAX(0, numZ - 1),
		file.getPath().toString().c_str());
}

} // End of namespace Scumm
