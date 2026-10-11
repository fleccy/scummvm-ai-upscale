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

// Dev check for true widescreen (remaster_offscreen_log=true): the wider view shows columns the original never showed
// at that moment. This logs, once per room and actor/object, what is drawn there:
//   WHOLE    an actor drawn wholly outside the original view (in the extra columns)
//   ENTER    that actor later reaching the original view (and how far the camera moved meanwhile)
//   UNREACH  an actor or object drawn where the original could never look (outside the room's camera limits, and
//            outside every view the camera has shown in this room)
//   OFFSUM   a summary per actor/object when the room is left
// It only reads state; nothing drawn changes.

#include "common/config-manager.h"
#include "scumm/scumm.h"
#include "scumm/actor.h"

namespace Scumm {

bool ScummEngine::remasterOffscreenOn() {
	if (_remasterOffLog < 0)
		_remasterOffLog = (_remasterLogicalWidth && ConfMan.hasKey("remaster_offscreen_log") && ConfMan.getBool("remaster_offscreen_log")) ? 1 : 0;
	return _remasterOffLog > 0;
}

bool ScummEngine::remasterOffscreenViews(int &origL, int &origR, int &reachL, int &reachR, int &shownL, int &shownR) {
	if (!_remasterLogicalWidth || _currentRoom <= 0 || _roomWidth <= 0 || _remasterVideoShown)
		return false;
	const int L = _remasterLogicalWidth;
	auto viewLeft = [&](int camX) { return _game.version >= 7 ? camX - L / 2 : (camX / 8 - L / 16) * 8; };
	origL = viewLeft(camera._cur.x);
	origR = origL + L;
	// The camera positions the original allows in this room (its limits; v5 cameraMoved keeps it inside the room)
	int lo = L / 2, hi = MAX(L / 2, _roomWidth - L / 2);
	if (VAR_CAMERA_MIN_X != 0xFF && VAR(VAR_CAMERA_MIN_X) > lo)
		lo = VAR(VAR_CAMERA_MIN_X);
	if (VAR_CAMERA_MAX_X != 0xFF && VAR(VAR_CAMERA_MAX_X) > 0 && VAR(VAR_CAMERA_MAX_X) < hi)
		hi = VAR(VAR_CAMERA_MAX_X);
	if (hi < lo)
		hi = lo;
	reachL = viewLeft(lo);
	reachR = viewLeft(hi) + L;
	if (lo != _remasterOffLastLo || hi != _remasterOffLastHi) {
		debug(1, "OFFLIMIT room %d frame %d camera limits %d-%d (original reach %d-%d of %d) cam %d", _currentRoom, _remasterOffFrame,
			lo, hi, MAX(0, reachL), MIN((int)_roomWidth, reachR), _roomWidth, camera._cur.x);
		_remasterOffLastLo = lo;
		_remasterOffLastHi = hi;
	}
	_remasterOffLimLo = MIN(_remasterOffLimLo, reachL);   // the camera limits' reach seen in this room (narrowest
	_remasterOffLimHi = MAX(_remasterOffLimHi, reachR);   // and widest are logged per room)
	_remasterOffLimLoMax = MAX(_remasterOffLimLoMax, reachL);
	_remasterOffLimHiMin = MIN(_remasterOffLimHiMin, reachR);
	if (remasterSingleScreenRoom())
		return false;   // only the original picture is shown
	// and every original view the camera has really shown here (scripts can place it past the limits)
	if (_remasterOffCamLo < _remasterOffCamHi) {
		reachL = MIN(reachL, _remasterOffCamLo);
		reachR = MAX(reachR, _remasterOffCamHi);
	}
	const VirtScreen &vs = _virtscr[kMainVirtScreen];
	shownL = MAX(0, (int)vs.xstart);
	shownR = MIN(_roomWidth, (int)vs.xstart + _screenWidth);
	return shownL < shownR;
}

static void offscreenEvaluate(ScummEngine *vm, ScummEngine::RemasterOffTrack &t, const char *kind, int id, int x0, int x1,
		int y0, int y1, int px, int py, int origL, int origR, int reachL, int reachR, int shownL, int shownR, int frame,
		int room, int camX, bool isObject) {
	const int w = x1 - x0;
	const int inOrig = MAX(0, MIN(x1, origR) - MAX(x0, origL));
	const int inReach = MAX(0, MIN(x1, reachR) - MAX(x0, reachL));
	t.seen++;
	t.minX = MIN(t.minX, px);
	t.maxX = MAX(t.maxX, px);
	if (inOrig == 0) {
		t.whole++;
		if (px == t.lastX && py == t.lastY)
			t.still++;
		if (t.firstWholeFrame < 0) {
			t.firstWholeFrame = frame;
			t.firstWholeCam = camX;
		}
		if (!t.loggedWhole && !isObject) {
			t.loggedWhole = true;
			debug(1, "OFFSCREEN WHOLE room %d frame %d %s %d cost %d pos %d,%d drawn x %d-%d y %d-%d orig %d-%d reach %d-%d shown %d-%d cam %d",
				room, frame, kind, id, t.costume, px, py, x0, x1, y0, y1, origL, origR, reachL, reachR, shownL, shownR, camX);
		}
	} else {
		if (inOrig < w)
			t.part++;
		t.orig++;
		if (t.firstWholeFrame >= 0 && t.enterFrame < 0) {
			t.enterFrame = frame;
			t.enterCamDelta = camX - t.firstWholeCam;
			if (!isObject)
				debug(1, "OFFSCREEN ENTER room %d frame %d %s %d cost %d pos %d,%d after %d frames, camera moved %d",
					room, frame, kind, id, t.costume, px, py, frame - t.firstWholeFrame, t.enterCamDelta);
		}
	}
	if (inReach < w) {
		t.unreach++;
		if (!t.loggedUnreach) {
			t.loggedUnreach = true;
			debug(1, "OFFSCREEN UNREACH room %d frame %d %s %d cost %d pos %d,%d drawn x %d-%d y %d-%d orig %d-%d reach %d-%d shown %d-%d cam %d",
				room, frame, kind, id, t.costume, px, py, x0, x1, y0, y1, origL, origR, reachL, reachR, shownL, shownR, camX);
		}
	}
	t.lastX = px;
	t.lastY = py;
}

void ScummEngine::remasterOffscreenNoteActor(int actor, int left, int right, int top, int bottom) {
	// called from markRectAsDirty while the actor draws: screen x -> room x
	const int xs = _virtscr[kMainVirtScreen].xstart;
	const Common::Rect r(left + xs, top, right + xs, bottom);
	if (r.left >= r.right || r.top >= r.bottom)
		return;
	Common::Rect &cur = _remasterOffActorRect[actor];
	if (_remasterOffActorStamp[actor] != _remasterOffFrame || cur.isEmpty())
		cur = r;
	else
		cur.extend(r);
	_remasterOffActorStamp[actor] = _remasterOffFrame;
}

void ScummEngine::remasterOffscreenNoteObject(int obj, int x, int w, int y, int h) {
	if (_remasterOffRoom != _currentRoom)
		return;
	int origL, origR, reachL, reachR, shownL, shownR;
	if (!remasterOffscreenViews(origL, origR, reachL, reachR, shownL, shownR))
		return;
	const int x0 = MAX(x, shownL), x1 = MIN(x + w, shownR);
	if (x0 >= x1)
		return;
	RemasterOffTrack &t = _remasterOffTracks[10000 + obj];
	offscreenEvaluate(this, t, "object", obj, x0, x1, y, y + h, x, y, origL, origR, reachL, reachR, shownL, shownR,
		_remasterOffFrame, _currentRoom, camera._cur.x, true);
}

void ScummEngine::remasterOffscreenCheck() {
	if (_remasterOffRoom != _currentRoom) {
		remasterOffscreenFlush();
		_remasterOffRoom = _currentRoom;
		_remasterOffWidth = _roomWidth;
		_remasterOffSingle = remasterSingleScreenRoom();
		_remasterOffLastLo = _remasterOffLastHi = -1;
		_remasterOffFrame = 0;
		_remasterOffCamLo = _remasterOffLimLo = _remasterOffLimHiMin = 32767;
		_remasterOffCamHi = _remasterOffLimHi = _remasterOffLimLoMax = -32768;
		_remasterOffTracks.clear();
		for (int i = 0; i < 256; i++) {
			_remasterOffActorRect[i] = Common::Rect();
			_remasterOffActorStamp[i] = -1;
		}
	}
	int origL, origR, reachL, reachR, shownL, shownR;
	const bool ok = remasterOffscreenViews(origL, origR, reachL, reachR, shownL, shownR);
	if (ok) {
		_remasterOffCamLo = MIN(_remasterOffCamLo, origL);
		_remasterOffCamHi = MAX(_remasterOffCamHi, origR);
		for (int i = 1; i < _numActors && i < 256; i++) {
			const Common::Rect &r = _remasterOffActorRect[i];
			if (r.isEmpty())
				continue;
			Actor *a = _actors[i];
			if (!a->isInCurrentRoom() || !a->_visible || !a->_costume)
				continue;
			const int x0 = MAX<int>(r.left, shownL), x1 = MIN<int>(r.right, shownR);
			if (x0 >= x1)
				continue;
			RemasterOffTrack &t = _remasterOffTracks[i];
			t.costume = a->_costume;
			offscreenEvaluate(this, t, "actor", i, x0, x1, r.top, r.bottom, a->getPos().x, a->getPos().y, origL, origR,
				reachL, reachR, shownL, shownR, _remasterOffFrame, _currentRoom, camera._cur.x, false);
		}
	}
	_remasterOffFrame++;
}

void ScummEngine::remasterOffscreenFlush() {
	if (_remasterOffRoom <= 0)
		return;
	debug(1, "OFFROOM room %d width %d frames %d camview %d-%d limits %d-%d (narrowest %d-%d) single %d tracks %d",
		_remasterOffRoom, _remasterOffWidth, _remasterOffFrame, _remasterOffCamLo, _remasterOffCamHi, _remasterOffLimLo,
		_remasterOffLimHi, _remasterOffLimLoMax, _remasterOffLimHiMin, _remasterOffSingle ? 1 : 0, _remasterOffTracks.size());
	for (auto &it : _remasterOffTracks) {
		const RemasterOffTrack &t = it._value;
		if (!t.whole && !t.unreach)
			continue;
		const bool obj = it._key >= 10000;
		debug(1, "OFFSUM room %d %s %d cost %d seen %d whole %d still %d part %d inorig %d unreach %d x %d..%d enter %d cam %d camview %d-%d frames %d",
			_remasterOffRoom, obj ? "object" : "actor", obj ? it._key - 10000 : it._key, t.costume, t.seen, t.whole, t.still,
			t.part, t.orig, t.unreach, t.minX, t.maxX, t.enterFrame, t.enterCamDelta, _remasterOffCamLo, _remasterOffCamHi,
			_remasterOffFrame);
	}
	_remasterOffTracks.clear();
}

} // End of namespace Scumm
