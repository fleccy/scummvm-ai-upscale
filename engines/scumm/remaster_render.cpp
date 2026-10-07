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

// COMI remaster renderer.
//
// Enabled with the config key remaster_path (COMI only). The engine keeps its unmodified 8-bit pipeline;
// the screen is opened in a 32-bit format and every 8-bit blit (drawStripToScreen, transitions, SMUSH)
// goes through remasterBlit():
//  - normally the pixels are converted with the current palette, which looks exactly like stock ScummVM;
//  - in rooms with <remaster_path>/rooms/NNNN/background.png, the frame is composed instead:
//      * pixels that still show the plain room background (captured in redrawBGStrip) show the remaster
//        art, sampled at any scale (image width / room width) with nearest neighbour;
//      * pixels the costume renderers wrote (hdNoteActorPixel) are actors; they are reduced to the
//        remaster pixel grid, aligned to room coordinates so nothing shimmers while the camera scrolls;
//        shadow pixels darken the remaster art by the ratio the original shadow table applied;
//      * everything else (text, verbs, inventory, changed objects) keeps the game's own pixels.
// Both references are keyed by the main virtual screen's buffer offset, so camera scrolling cannot
// invalidate them.

#include "common/algorithm.h"
#include "scumm/remaster_palette.h"
#include "base/version.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/fs.h"
#include "engines/util.h"
#include "graphics/cursorman.h"
#include "image/png.h"

#include "scumm/remaster_ai.h"
#include "scumm/scumm.h"
#include "scumm/actor.h"
#include "scumm/object.h"

namespace Scumm {

void ScummEngine::remasterInitGraphics(int w, int h) {
	Graphics::PixelFormat fmt;
	bool found = false;
	Common::List<Graphics::PixelFormat> formats = _system->getSupportedFormats();
	for (Common::List<Graphics::PixelFormat>::const_iterator f = formats.begin(); f != formats.end(); ++f) {
		if (f->bytesPerPixel == 4) {
			fmt = *f;
			found = true;
			break;
		}
	}
	// Optional real-time AI upscaling of every frame. The AI works on bytes in memory order R, G, B, A.
	_remasterScale = 1;
	if (found && ConfMan.hasKey("remaster_ai_model")) {
		const bool rgbaBytes = fmt.rShift == 0 && fmt.gShift == 8 && fmt.bShift == 16;
		Common::String info;
		if (!rgbaBytes)
			info = "screen format " + fmt.toString() + " is not R,G,B,A in memory";
		else
			_remasterAI = remasterAICreateBackend(ConfMan.hasKey("remaster_ai_backend") ? ConfMan.get("remaster_ai_backend") : "ncnn",
			                                      ConfMan.get("remaster_ai_model"), info, w, h);
			if (!_remasterAI && ConfMan.hasKey("remaster_ai_backend")) {
				warning("remaster: upscaler %s unavailable (%s), using the standard one", ConfMan.get("remaster_ai_backend").c_str(), info.c_str());
				_remasterAI = remasterAICreate(ConfMan.get("remaster_ai_model"), info);
			}
		if (_remasterAI) {
			_remasterScale = ConfMan.hasKey("remaster_ai_scale") ? CLIP(ConfMan.getInt("remaster_ai_scale"), 2, 4) : 3;
			if (ConfMan.hasKey("remaster_performance") && ConfMan.getBool("remaster_performance"))
				_remasterScale = 2; // performance mode: about half the AI work; the backend scales the picture up
			_remasterAIOn = !ConfMan.hasKey("remaster_ai_enabled") || ConfMan.getBool("remaster_ai_enabled");
			if (!ConfMan.hasKey("remaster_hd_backgrounds") || ConfMan.getBool("remaster_hd_backgrounds")) {
				Common::String bgModel;
				if (ConfMan.hasKey("remaster_bg_model")) {
					bgModel = ConfMan.get("remaster_bg_model");
				} else {
					bgModel = ConfMan.get("remaster_ai_model");
					const char *sl = MAX(strrchr(bgModel.c_str(), '/'), strrchr(bgModel.c_str(), '\\'));
					bgModel = Common::String(bgModel.c_str(), sl ? sl + 1 : bgModel.c_str()) + "realesrgan-x4plus-anime";
				}
				Common::String bgInfo;
				_remasterBgAI = remasterAICreate(bgModel, bgInfo);
				_remasterBgScale = ConfMan.hasKey("remaster_bg_scale") ? CLIP(ConfMan.getInt("remaster_bg_scale"), 2, 4) : 4;
				warning("remaster: HD backgrounds %s (%s)", _remasterBgAI ? "with" : "unavailable:", _remasterBgAI ? bgModel.c_str() : bgInfo.c_str());
			}
			warning("remaster: upscaling x%d with %s, %s (%s)", _remasterScale, remasterAIBackendName(_remasterAI), ConfMan.get("remaster_ai_model").c_str(), info.c_str());
			_remasterBackendShown = remasterAIBackendName(_remasterAI);
		} else {
			// Keep the large screen anyway: original and retro display modes work without a GPU.
			_remasterScale = ConfMan.hasKey("remaster_ai_scale") ? CLIP(ConfMan.getInt("remaster_ai_scale"), 2, 4) : 3;
			warning("remaster: AI upscaling unavailable (%s), continuing without it", info.c_str());
		}
		_remasterMode = _remasterAI ? kRemasterAI : kRemasterOriginal;
		if (ConfMan.hasKey("remaster_ai_enabled") && !ConfMan.getBool("remaster_ai_enabled"))
			_remasterMode = kRemasterOriginal;
		if (ConfMan.hasKey("remaster_display_mode"))
			_remasterMode = CLIP(ConfMan.getInt("remaster_display_mode"), 0, (int)kRemasterModeCount - 1);
		if (_remasterMode == kRemasterAI && !_remasterAI)
			_remasterMode = kRemasterOriginal;
		_remasterCrt = ConfMan.hasKey("remaster_crt") && ConfMan.getBool("remaster_crt");
		_remasterSharpText = ConfMan.hasKey("remaster_sharp_text") && ConfMan.getBool("remaster_sharp_text");
		_remasterSmoothText = !ConfMan.hasKey("remaster_smooth_text") || ConfMan.getBool("remaster_smooth_text");
		_remasterHDActors = !ConfMan.hasKey("remaster_hd_actors") || ConfMan.getBool("remaster_hd_actors");
		_remasterCompare = ConfMan.hasKey("remaster_compare") && ConfMan.getBool("remaster_compare");
		_remasterCompareX = _screenWidth / 2;
		_remasterAIStrength = ConfMan.hasKey("remaster_ai_strength") ? CLIP(ConfMan.getInt("remaster_ai_strength"), 0, 100) : 100;
		_remasterHDBgOn = !ConfMan.hasKey("remaster_hd_backgrounds") || ConfMan.getBool("remaster_hd_backgrounds");
		if (ConfMan.hasKey("remaster_performance") && ConfMan.getBool("remaster_performance")) {
			_remasterHDActors = false;
			_remasterGrainOn = false;
		}
	}
	// Optional 16:9 output: the 4:3 picture stays centred, the side panels are filled by remasterAmbientSides().
	_remasterOutH = h * _remasterScale;
	_remasterOutW = w * _remasterScale;
	if (ConfMan.hasKey("remaster_widescreen") && ConfMan.get("remaster_widescreen") == "ambient")
		_remasterOutW = MAX(_remasterOutW, ((_remasterOutH * 16 / 9) + 1) & ~1);
	_remasterSide = (_remasterOutW - w * _remasterScale) / 2;
	_remasterAmbientOn = !ConfMan.hasKey("remaster_ambient_sides") || ConfMan.getBool("remaster_ambient_sides");
	_remasterSidesOn = !ConfMan.hasKey("remaster_side_art") || ConfMan.getBool("remaster_side_art");
	if (found) {
		initGraphics(_remasterOutW, _remasterOutH, &fmt);
		found = _system->getScreenFormat() == fmt;
	}
	if (!found) {
		warning("remaster: no 32-bit screen format available, running without the remaster renderer");
		remasterAIDestroy(_remasterAI);
		_remasterAI = nullptr;
		_remasterScale = 1;
		_remasterSide = 0;
		initGraphics(w, h);
		return;
	}
	_remasterFormat = fmt;
	_remasterScreen8 = (byte *)calloc(w * h, 1);
	_remasterRgb = (uint32 *)calloc(w * h, 4);
	if (_remasterScale > 1)
		_remasterBig = (uint32 *)calloc(w * h * _remasterScale * _remasterScale, 4);
	if (remasterDeferred())
		_remasterOut = (uint32 *)calloc(_remasterOutW * _remasterOutH, 4);
	if (_remasterScale > 1) {
		_remasterProtect = (byte *)calloc(w * h, 1);
		_remasterOwner = (byte *)calloc(w * h, 1);
		_remasterGrainOn = !ConfMan.hasKey("remaster_grain_guard") || ConfMan.getBool("remaster_grain_guard");
	}
	_remasterEnabled = _remasterScreen8 && _remasterRgb && (_remasterScale == 1 || _remasterBig) && (!remasterDeferred() || _remasterOut);
	// The 8-bit cursor is shown on an RGB screen, so it needs its own palette.
	CursorMan.disableCursorPalette(false);
	warning("remaster: enabled, screen %dx%d %s%s", _remasterOutW, _remasterOutH, fmt.toString().c_str(), _remasterSide ? " (16:9 with ambient side panels)" : "");
	remasterUpdateCheckStart();
}

bool ScummEngine::remasterAIIncremental(double *ms) {
	// Run the AI only on the parts of the frame that changed since the last AI frame (each grown by a margin wider
	// than the network's receptive field, so the result matches a full-frame run); keep the rest from the previous
	// output. Changes are tracked on a grid of 32x32 cells and merged into rectangles, so separate animations (people
	// walking on opposite sides of a room) do not force one huge box.
	// The pure AI output lives in _remasterAIOut; _remasterBig gets a copy that later passes may modify.
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	const size_t n = (size_t)W * H;
	remasterBackendAnnounce();   // e.g. TensorRT finished preparing
	if (remasterAISetVideo(_remasterAI, _remasterVideoShown))
		_remasterAIPrev.clear();   // Ultra cutscene: redo the picture with the heavy network
	remasterPhotoPoll();
	if (_remasterAIOut.size() != n * S * S) {
		_remasterAIOut.resize(n * S * S);
		_remasterAIPrev.clear();
	}
	// Governor level 3 (very heavy rooms): refresh the AI picture on every other frame only; in between the last AI
	// picture is shown again (the cursor is separate and stays responsive).
	if (_remasterGovLevel >= 1 && _remasterAIPrev.size() == n && (++_remasterGovSkip & 1)) {
		*ms = 0;
		memcpy(_remasterBig, _remasterAIOut.data(), n * S * S * 4);
		return true;
	}
	*ms = 0;
	// Camera scrolling (rooms wider than the screen): everything moves, so the frame differs everywhere. Shift the
	// previous AI picture (and its input) by the scroll instead, so only the newly shown strip, the columns that were
	// at the old screen edge (upscaled without their right-hand context) and real animation need the AI.
	{
		const VirtScreen &mvs = _virtscr[kMainVirtScreen];
		const int xs = mvs.xstart, top = _screenTop;
		const int dx = xs - _remasterAIPrevXs;
		const bool scrollOk = _remasterAIPrev.size() == n && dx != 0 && ABS(dx) < W / 2 && top == _remasterAIPrevTop &&
			_remasterAIPrevRoom == _currentRoom;
		if (scrollOk) {
			const uint32 poison = 0x00010203u;   // never a real colour (alpha 0): marks columns that must be redone
			const int band = 24;                 // receptive field margin at the old edge
			for (int y = 0; y < H; y++) {
				uint32 *pr = &_remasterAIPrev[(size_t)y * W];
				if (dx > 0) {
					memmove(pr, pr + dx, (W - dx) * 4);
					for (int x = MAX(0, W - dx - band); x < W; x++) pr[x] = poison;
				} else {
					memmove(pr - dx, pr, (W + dx) * 4);
					for (int x = 0; x < MIN(W, -dx + band); x++) pr[x] = poison;
				}
			}
			for (int y = 0; y < H * S; y++) {
				uint32 *po = &_remasterAIOut[(size_t)y * BW];
				if (dx > 0)
					memmove(po, po + dx * S, (BW - dx * S) * 4);
				else
					memmove(po - dx * S, po, (BW + dx * S) * 4);
			}
			_remasterAIScrolls++;
		} else if (dx != 0 || top != _remasterAIPrevTop || _remasterAIPrevRoom != _currentRoom) {
			_remasterAIPrev.clear();   // jumped: start over
		}
		_remasterAIPrevXs = xs;
		_remasterAIPrevTop = top;
		_remasterAIPrevRoom = _currentRoom;
	}
	bool full = _remasterAIPrev.size() != n || (ConfMan.hasKey("remaster_ai_incremental") && !ConfMan.getBool("remaster_ai_incremental"));
	if (!full && _remasterAISettle > 0 && --_remasterAISettle == 0)
		full = true;   // periodic full refresh after large partial updates (no residue can build up)
	const int G = 32, GW = (W + G - 1) / G, GH = (H + G - 1) / G;
	Common::Array<byte> dirty;
	int dirtyCells = 0;
	if (!full) {
		dirty.resize(GW * GH);
		memset(dirty.data(), 0, dirty.size());
		for (int y = 0; y < H; y++) {
			const uint32 *a = _remasterRgb + y * W, *b = &_remasterAIPrev[y * W];
			if (!memcmp(a, b, W * 4))
				continue;
			for (int gx = 0; gx < GW; gx++) {
				byte &d = dirty[(y / G) * GW + gx];
				if (d)
					continue;
				const int x0 = gx * G, x1 = MIN(W, x0 + G);
				if (memcmp(a + x0, b + x0, (x1 - x0) * 4)) {
					d = 1;
					dirtyCells++;
				}
			}
		}
		if (!dirtyCells) {
			_remasterAISkipped++;
			memcpy(_remasterBig, _remasterAIOut.data(), n * S * S * 4);
			return true;
		}
	}
	// Rectangles: runs of dirty cells per grid row, merged downwards with identical runs below.
	struct R { int gx0, gx1, gy0, gy1; };
	Common::Array<R> rects;
	if (!full) {
		Common::Array<byte> used(GW * GH, 0);
		for (int gy = 0; gy < GH; gy++)
			for (int gx = 0; gx < GW; gx++) {
				if (!dirty[gy * GW + gx] || used[gy * GW + gx])
					continue;
				int gx1 = gx;
				while (gx1 + 1 < GW && dirty[gy * GW + gx1 + 1] && !used[gy * GW + gx1 + 1])
					gx1++;
				int gy1 = gy;
				for (;;) {
					if (gy1 + 1 >= GH)
						break;
					bool all = true;
					for (int k = gx; k <= gx1 && all; k++)
						all = dirty[(gy1 + 1) * GW + k] && !used[(gy1 + 1) * GW + k];
					if (!all)
						break;
					gy1++;
				}
				for (int yy = gy; yy <= gy1; yy++)
					for (int k = gx; k <= gx1; k++)
						used[yy * GW + k] = 1;
				rects.push_back({gx, gx1, gy, gy1});
			}
		// Work estimate with margins: several rectangles, one box around all of them, or the full frame.
		size_t area = 0;
		int bx0 = GW, by0 = GH, bx1 = -1, by1 = -1;
		for (uint k = 0; k < rects.size(); k++) {
			area += (size_t)MIN(W, (rects[k].gx1 - rects[k].gx0 + 1) * G + 72) * MIN(H, (rects[k].gy1 - rects[k].gy0 + 1) * G + 72);
			bx0 = MIN(bx0, rects[k].gx0); by0 = MIN(by0, rects[k].gy0); bx1 = MAX(bx1, rects[k].gx1); by1 = MAX(by1, rects[k].gy1);
		}
		area += rects.size() * 2000;   // per-run overhead
		const size_t box = (size_t)MIN(W, (bx1 - bx0 + 1) * G + 72) * MIN(H, (by1 - by0 + 1) * G + 72);
		if (box < area) {
			rects.clear();
			rects.push_back({bx0, bx1, by0, by1});
			area = box;
		}
		if (area * 10 >= n * 7)
			full = true;
	}
	if (full) {
		if (!remasterAIRun(_remasterAI, (const byte *)_remasterRgb, W, H, S, (byte *)_remasterAIOut.data(), ms)) {
			_remasterAIPrev.clear();
			return false;
		}
		_remasterAISettle = 0;
		_remasterAIPrev.resize(n);
	} else {
		// Output pixels up to INF px away from a changed input pixel change too (the network looks at a
		// neighbourhood), so each updated area is its changed box grown by INF; the AI input is grown by another
		// CTX px of context so the updated area matches a full-frame run.
		const int INF = 16, CTX = 20;
		Common::Array<uint32> in, out;
		for (uint k = 0; k < rects.size(); k++) {
			int x0 = rects[k].gx0 * G, y0 = rects[k].gy0 * G;
			int x1 = MIN(W, (rects[k].gx1 + 1) * G) - 1, y1 = MIN(H, (rects[k].gy1 + 1) * G) - 1;
			x0 = MAX(0, x0 - INF); y0 = MAX(0, y0 - INF); x1 = MIN(W - 1, x1 + INF); y1 = MIN(H - 1, y1 + INF);
			const int px0 = MAX(0, x0 - CTX), py0 = MAX(0, y0 - CTX), px1 = MIN(W - 1, x1 + CTX), py1 = MIN(H - 1, y1 + CTX);
			const int rw = px1 - px0 + 1, rh = py1 - py0 + 1;
			in.resize((size_t)rw * rh);
			out.resize((size_t)rw * rh * S * S);
			for (int y = 0; y < rh; y++)
				memcpy(&in[(size_t)y * rw], _remasterRgb + (py0 + y) * W + px0, rw * 4);
			double one = 0;
			if (!remasterAIRun(_remasterAI, (const byte *)in.data(), rw, rh, S, (byte *)out.data(), &one)) {
				_remasterAIPrev.clear();
				return false;
			}
			*ms += one;
			remasterSmoothCheckpoint("AI regions");   // smooth scrolling: keep gliding between regions
			// Copy back the changed area; the margin is only used where it lies on the frame border.
			const int cx0 = (x0 - CTX <= 0) ? 0 : x0 - px0, cy0 = (y0 - CTX <= 0) ? 0 : y0 - py0;
			const int cx1 = (x1 + CTX >= W - 1) ? rw - 1 : x1 - px0, cy1 = (y1 + CTX >= H - 1) ? rh - 1 : y1 - py0;
			for (int y = cy0 * S; y < (cy1 + 1) * S; y++)
				memcpy(&_remasterAIOut[(size_t)(py0 * S + y) * BW + (px0 + cx0) * S], &out[(size_t)y * rw * S + cx0 * S], (cx1 - cx0 + 1) * S * 4);
		}
		_remasterAIPartial++;
		if (dirtyCells * 100 >= GW * GH * 3)
			_remasterAISettle = 10;
	}
	memcpy(_remasterAIPrev.data(), _remasterRgb, n * 4);
	memcpy(_remasterBig, _remasterAIOut.data(), n * S * S * 4);
	return true;
}

void ScummEngine::remasterUpscaleFrame() {
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale;
	const uint32 *center = _remasterRgb;
	uint32 *savedRgb = _remasterRgb;
	byte *savedProtect = _remasterProtect;
	byte *savedOwner = _remasterOwner;
	_remasterProtectFull = _remasterProtect;
	if (_remasterLogicalWidth && remasterMargin() > 0)
		remasterCentreView();   // swaps in centred copies of the frame and protect mask for this present
	const uint32 govStart = _system->getMillis();
	uint32 *withText = _remasterRgb;
	const bool centred = _remasterRgb == _remasterView.data();
	const bool textApart = S > 1 && _remasterProtect &&
		(centred ? _remasterViewClean.size() == (uint)(W * H) : _remasterClean.size() == (uint)(W * H));
	byte *protectWithText = _remasterProtect;
	if (textApart) {
		// the scene passes see the text-free frame, and its pixel kinds (a glyph pixel counts as what it covers)
		_remasterRgb = centred ? _remasterViewClean.data() : _remasterClean.data();
		_remasterProtect = centred ? _remasterViewCleanProtect.data() : _remasterCleanProtect.data();
	}
	if (S > 1) {
		double ms = 0;
		if (_remasterMode >= kRemasterVGA) {
			remasterRetro();
		} else if (_remasterMode != kRemasterAI || remasterAIOffInRoom() || !remasterAIIncremental(&ms)) {
			// AI switched off or failed: nearest neighbour.
			for (int y = 0; y < H * S; y++) {
				const uint32 *s = _remasterRgb + (y / S) * W;
				uint32 *d = _remasterBig + y * W * S;
				for (int x = 0; x < W * S; x++)
					d[x] = s[x / S];
			}
		} else {
			remasterSmoothCheckpoint("grain guard (after AI)");
			if (_remasterGrainOn && _remasterProtect)
				remasterGrainGuard();
			if (_remasterHDBgOn && _remasterBgAI && !_remasterVideoShown && _remasterGovLevel < 2) {
				const uint32 tb = _system->getMillis();
				remasterBgUpdate();
				remasterHDBackground();
				_remasterHDBgMs += _system->getMillis() - tb;
			}
			remasterSmoothCheckpoint("HD characters (after HD backgrounds)");
			_remasterAIMs += ms;
			if (++_remasterAIFrames % 300 == 0) {
				warning("remaster: AI %.1f ms, grain guard %.1f ms, HD actors %.1f ms, HD backgrounds %.1f ms per frame on average over 300 frames (partial %d, unchanged %d, scrolled %d; characters refined %d, waiting %d)",
					_remasterAIMs / 300, _remasterGrainMs / 300, _remasterHDMs / 300, _remasterHDBgMs / 300, _remasterAIPartial, _remasterAISkipped, _remasterAIScrolls, _remasterCelRefined, (int)_remasterCelQueue.size());
				_remasterAIScrolls = 0;
				_remasterHDBgMs = 0;
				_remasterAIPartial = _remasterAISkipped = 0;
				_remasterHDMs = 0;
				_remasterAIMs = 0;
				_remasterGrainMs = 0;
			}
		}
		if (_remasterMode == kRemasterAI && _remasterHDActors && !_remasterVideoShown && _remasterGovLevel < 3)
			remasterHDActors();
		remasterSmoothCheckpoint("output (after HD characters)");

		_remasterGovWork += _system->getMillis() - govStart;
		if (_remasterMode == kRemasterAI && _remasterAIStrength < 100)
			remasterApplyStrength();
		if (_remasterCompare)
			remasterCompareOverlay();
		if (_remasterCrt)
			remasterCrtOverlay();
		_remasterRgb = withText;
		_remasterProtect = protectWithText;
		if (textApart)
			remasterTextOverlay();   // readable text last: not upscaled, averaged or scanlined
		center = _remasterBig;
	}
	// Compose the output: centred picture plus side panels.
	for (int y = 0; y < _remasterOutH; y++)
		memcpy(_remasterOut + y * _remasterOutW + _remasterSide, center + y * W * S, W * S * 4);
	if (_remasterSide > 0)
		remasterAmbientSides();
	remasterSmoothNewFrame();
	if (_remasterSmActive) {
		remasterSmoothCompose(_remasterSmStart);   // still where the picture was
		_system->copyRectToScreen(_remasterSmOut.data(), _remasterOutW * 4, 0, 0, _remasterOutW, _remasterOutH);
		_remasterSmShown = _remasterSmOut.data();
	} else {
		_system->copyRectToScreen(_remasterOut, _remasterOutW * 4, 0, 0, _remasterOutW, _remasterOutH);
		_remasterSmShown = _remasterOut;
	}
	_remasterRgb = savedRgb;
	_remasterProtect = savedProtect;
	_remasterOwner = savedOwner;
	_remasterFrameDirty = false;
}

void ScummEngine::remasterAmbientSides() {
	// Soft "ambient" panels: a blurred, mirrored and darkened continuation of the picture's own edges, like
	// a TV/phone video background. The frame is reduced to a tiny thumbnail and sampled bilinearly, which
	// is what makes it smooth; it follows the scene's colours live and costs a few milliseconds.
	const int side = _remasterSide, outW = _remasterOutW, outH = _remasterOutH;
	if (!_remasterAmbientOn) {
		for (int y = 0; y < outH; y++) {
			memset(_remasterOut + y * outW, 0, side * 4);
			memset(_remasterOut + y * outW + outW - side, 0, side * 4);
		}
		return;
	}
	const int W = _screenWidth, H = _screenHeight;
	enum { TW = 32, TH = 24 };
	float tiny[TH][TW][3];
	const int cw = W / TW, ch = H / TH;
	for (int ty = 0; ty < TH; ty++) {
		for (int tx = 0; tx < TW; tx++) {
			uint32 sum[3] = {0, 0, 0};
			for (int y = ty * ch; y < (ty + 1) * ch; y++) {
				const uint32 *row = _remasterRgb + y * W + tx * cw;
				for (int x = 0; x < cw; x++) {
					uint8 r, g, b;
					_remasterFormat.colorToRGB(row[x], r, g, b);
					sum[0] += r; sum[1] += g; sum[2] += b;
				}
			}
			for (int c = 0; c < 3; c++)
				tiny[ty][tx][c] = sum[c] / float(cw * ch);
		}
	}
	const float reach = TW * 0.22f; // how much of the picture's edge is mirrored into a panel
	Common::Array<float> rowBuf;
	rowBuf.resize(TW * 3);
	for (int y = 0; y < outH; y++) {
		// Vertical bilinear blend of two thumbnail rows.
		float fy = (y + 0.5f) / outH * TH - 0.5f;
		fy = CLIP(fy, 0.0f, float(TH - 1));
		const int y0 = (int)fy, y1 = MIN(y0 + 1, (int)TH - 1);
		const float wy = fy - y0;
		for (int tx = 0; tx < TW; tx++)
			for (int c = 0; c < 3; c++)
				rowBuf[tx * 3 + c] = tiny[y0][tx][c] * (1 - wy) + tiny[y1][tx][c] * wy;
		uint32 *left = _remasterOut + y * outW;
		uint32 *right = _remasterOut + y * outW + outW - side;
		for (int x = 0; x < side; x++) {
			const float u = (side - x - 0.5f) / side; // 0 at the picture edge, 1 at the outer screen edge
			const float shade = 0.85f - 0.45f * u; // brightest next to the picture, fading outwards
			for (int pass = 0; pass < 2; pass++) {
				float fx = pass == 0 ? u * reach - 0.5f : (TW - u * reach) - 0.5f;
				fx = CLIP(fx, 0.0f, float(TW - 1));
				const int x0 = (int)fx, x1 = MIN(x0 + 1, (int)TW - 1);
				const float wx = fx - x0;
				uint8 rgb[3];
				for (int c = 0; c < 3; c++)
					rgb[c] = (uint8)((rowBuf[x0 * 3 + c] * (1 - wx) + rowBuf[x1 * 3 + c] * wx) * shade);
				const uint32 col = _remasterFormat.RGBToColor(rgb[0], rgb[1], rgb[2]);
				if (pass == 0)
					left[x] = col;
				else
					right[side - 1 - x] = col;
			}
		}
	}
}

bool ScummEngine::remasterAIOffInRoom() const {
	// remaster_ai_off_rooms=25,60: rooms where the AI is skipped entirely.
	if (!ConfMan.hasKey("remaster_ai_off_rooms"))
		return false;
	const Common::String list = ConfMan.get("remaster_ai_off_rooms");
	const char *p = list.c_str();
	while (*p) {
		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (!*p)
			break;
		if (atoi(p) == _currentRoom)
			return true;
		while (*p >= '0' && *p <= '9')
			p++;
	}
	return false;
}

void ScummEngine::remasterGrainGuard() {
	// Ordinary rooms are rechecked now and then. Right after entering a room the screen is often still fading in
	// (dark, so nothing looks grainy), so check often during the first seconds.
	if (_remasterGrainRoom != _currentRoom)
		_remasterGrainRoomFrames = 0;
	_remasterGrainRoomFrames++;
	const int recheck = _remasterGrainRoomFrames < 600 ? 20 : 300;
	if (_remasterGrainRoom == _currentRoom && !_remasterGrainScene && (_remasterGrainRoomFrames % recheck) != 0)
		return; // ordinary room: checked recently and not grainy
	_remasterGrainRoom = _currentRoom;
	// Grain guard. The AI sharpens fine dithering/grain into harsh speckle. Find grainy areas in the
	// 640x480 frame (pixel noise that a 3x3 median removes but edges survive), keep actors/text/UI on the AI
	// (_remasterProtect) and blend a lightly denoised bilinear upscale into the AI frame there.
	const uint32 t0 = _system->getMillis();
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S, BH = H * S;
	const byte *rgb = (const byte *)_remasterRgb; // bytes R, G, B, A (required for the AI path)

	Common::Array<uint8> lum, noise, mask;
	lum.resize(W * H);
	noise.resize(W * H);
	mask.resize(W * H);
	for (int i = 0; i < W * H; i++)
		lum[i] = (uint8)((rgb[i * 4] * 77 + rgb[i * 4 + 1] * 150 + rgb[i * 4 + 2] * 29) >> 8);

	// |lum - median3x3|, borders 0.
	memset(noise.data(), 0, noise.size());
	for (int y = 1; y < H - 1; y++) {
		for (int x = 1; x < W - 1; x++) {
			uint8 v[9];
			int k = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
					v[k++] = lum[(y + dy) * W + x + dx];
			// Partial selection sort up to the median (index 4).
			for (int i = 0; i <= 4; i++)
				for (int j = i + 1; j < 9; j++)
					if (v[j] < v[i]) { uint8 t = v[i]; v[i] = v[j]; v[j] = t; }
			const int d = (int)lum[y * W + x] - v[4];
			noise[y * W + x] = (uint8)MIN(255, ABS(d));
		}
	}

	// Box sums via an integral image.
	Common::Array<uint32> integ;
	integ.resize((W + 1) * (H + 1));
	auto buildIntegral = [&](const Common::Array<uint8> &src) {
		memset(integ.data(), 0, (W + 1) * sizeof(uint32));
		for (int y = 0; y < H; y++) {
			uint32 rowSum = 0;
			integ[(y + 1) * (W + 1)] = 0;
			for (int x = 0; x < W; x++) {
				rowSum += src[y * W + x];
				integ[(y + 1) * (W + 1) + x + 1] = integ[y * (W + 1) + x + 1] + rowSum;
			}
		}
	};
	auto boxMean = [&](int x, int y, int r) -> uint32 {
		const int x0 = MAX(0, x - r), y0 = MAX(0, y - r), x1 = MIN(W, x + r + 1), y1 = MIN(H, y + r + 1);
		const uint32 s = integ[y1 * (W + 1) + x1] - integ[y0 * (W + 1) + x1] - integ[y1 * (W + 1) + x0] + integ[y0 * (W + 1) + x0];
		return s / ((x1 - x0) * (y1 - y0));
	};

	// Grain density -> 0..255 (0 below 6 levels of average speckle, 255 above 12); protected pixels (dilated by
	// 2 px so outlines next to them stay crisp) get 0.
	buildIntegral(noise);
	bool any = false;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int m = ((int)boxMean(x, y, 4) - 6) * 255 / 6;
			m = CLIP(m, 0, 255);
			if (m) {
				for (int dy = -2; dy <= 2 && m; dy++)
					for (int dx = -2; dx <= 2 && m; dx++) {
						const int yy = CLIP(y + dy, 0, H - 1), xx = CLIP(x + dx, 0, W - 1);
						// Characters, their shadows and text keep the AI; animated scenery (kind 1, e.g. the banjo
						// duel's moving swirl) may be grainy itself and is softened like the background.
						const byte k = _remasterProtect[yy * W + xx];
						if (k >= 2)
							m = 0;
					}
			}
			mask[y * W + x] = (uint8)m;
			any |= m != 0;
		}
	}
	if (!any) {
		_remasterGrainMs += _system->getMillis() - t0;
		return;
	}
	// Only large grainy areas count (a dithered backdrop), not small detailed patches such as a face in a portrait:
	// keep a pixel only if most of its 25 x 25 neighbourhood is grainy too.
	buildIntegral(mask);
	any = false;
	int grainy = 0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (mask[y * W + x] && boxMean(x, y, 12) < 150)
				mask[y * W + x] = 0;
			if (mask[y * W + x]) {
				any = true;
				grainy++;
			}
		}
	}
	// Only scenes that are grainy overall (dithered backdrops such as the banjo duel, ~45 % of the frame) get the
	// treatment; in ordinary rooms the detector also fires on fine detail (scattered hair, papers) and would blur it.
	_remasterGrainScene = grainy >= W * H / 5;
	if (!_remasterGrainScene)
		any = false;
	if (!any) {
		_remasterGrainMs += _system->getMillis() - t0;
		return;
	}
	// Soften the mask edges so there are no seams.
	buildIntegral(mask);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			noise[y * W + x] = (uint8)boxMean(x, y, 2);   // reuse as soft mask

	// Denoised SD frame (3x3 mean, twice) for the fallback.
	Common::Array<uint8> soft;
	soft.resize(W * H * 3);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			int sum[3] = {0, 0, 0}, n = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					const int yy = y + dy, xx = x + dx;
					if (yy < 0 || yy >= H || xx < 0 || xx >= W)
						continue;
					for (int c = 0; c < 3; c++)
						sum[c] += rgb[(yy * W + xx) * 4 + c];
					n++;
				}
			for (int c = 0; c < 3; c++)
				soft[(y * W + x) * 3 + c] = (uint8)(sum[c] / n);
		}
	}
	// A second 3x3 pass (together a 5x5 tent): coarse dithering such as the banjo duel's swirl otherwise still shows
	// through as speckle. Only grainy scenes get here, so normal rooms are never softened.
	{
		Common::Array<uint8> tmp(soft);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				int sum[3] = {0, 0, 0}, n = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const int yy = y + dy, xx = x + dx;
						if (yy < 0 || yy >= H || xx < 0 || xx >= W)
							continue;
						for (int c = 0; c < 3; c++)
							sum[c] += tmp[(yy * W + xx) * 3 + c];
						n++;
					}
				for (int c = 0; c < 3; c++)
					soft[(y * W + x) * 3 + c] = (uint8)(sum[c] / n);
			}
	}

	// Blend into the AI frame: bilinear samples of the soft mask and the denoised frame. Sample positions and
	// weights are the same for every row/column, so they are computed once; SD rows without grain are skipped.
	Common::Array<int> cx0, cx1, cwx;
	cx0.resize(BW); cx1.resize(BW); cwx.resize(BW);
	for (int bx = 0; bx < BW; bx++) {
		const int fx = (bx * 2 + 1) * 128 / S - 128;  // source x in 1/256 px, pixel centres aligned
		cx0[bx] = CLIP(fx >> 8, 0, W - 1);
		cx1[bx] = MIN(cx0[bx] + 1, W - 1);
		cwx[bx] = CLIP(fx - (cx0[bx] << 8), 0, 256);
	}
	Common::Array<uint8> rowAny;
	rowAny.resize(H);
	for (int y = 0; y < H; y++) {
		rowAny[y] = 0;
		for (int x = 0; x < W && !rowAny[y]; x++)
			rowAny[y] = noise[y * W + x] != 0;
	}
	byte *big = (byte *)_remasterBig;
	for (int by = 0; by < BH; by++) {
		const int fy = (by * 2 + 1) * 128 / S - 128;
		const int sy0 = CLIP(fy >> 8, 0, H - 1), sy1 = MIN(sy0 + 1, H - 1);
		if (!rowAny[sy0] && !rowAny[sy1])
			continue;
		const int wy = CLIP(fy - (sy0 << 8), 0, 256), iwy = 256 - wy;
		const uint8 *m0 = &noise[sy0 * W], *m1 = &noise[sy1 * W];
		const uint8 *s0 = &soft[sy0 * W * 3], *s1 = &soft[sy1 * W * 3];
		byte *o = big + by * BW * 4;
		for (int bx = 0; bx < BW; bx++, o += 4) {
			const int a0 = cx0[bx], a1 = cx1[bx];
			const int mm = m0[a0] | m0[a1] | m1[a0] | m1[a1];
			if (!mm)
				continue;
			const int wx = cwx[bx], iwx = 256 - wx;
			const int a = ((m0[a0] * iwx + m0[a1] * wx) * iwy + (m1[a0] * iwx + m1[a1] * wx) * wy) >> 16; // 0..255
			if (!a)
				continue;
			const uint8 *p00 = s0 + a0 * 3, *p01 = s0 + a1 * 3, *p10 = s1 + a0 * 3, *p11 = s1 + a1 * 3;
			for (int ch = 0; ch < 3; ch++) {
				const int v = ((p00[ch] * iwx + p01[ch] * wx) * iwy + (p10[ch] * iwx + p11[ch] * wx) * wy) >> 16;
				o[ch] = (byte)(o[ch] + (((v - o[ch]) * a) >> 8));
			}
		}
	}	_remasterGrainMs += _system->getMillis() - t0;
}

void ScummEngine::remasterToggleGrain() {
	_remasterGrainOn = !_remasterGrainOn;
	ConfMan.setBool("remaster_grain_guard", _remasterGrainOn);
	ConfMan.flushToDisk();
	_remasterFrameDirty = true;
	_system->displayMessageOnOSD(Common::U32String(_remasterGrainOn ? "Grain guard: on (Shift+F11)" : "Grain guard: off (Shift+F11)"));
}
void ScummEngine::remasterCentreView() {
	// Copy the frame (and protect mask) with the content area moved to the centre, then point _remasterRgb /
	// _remasterProtect at the copies until the frame has been presented.
	const int W = _screenWidth, H = _screenHeight, m = remasterMargin(), sx = remasterViewSrcX();
	_remasterView.resize(W * H);
	_remasterViewProtect.resize(W * H);
	for (int y = 0; y < H; y++) {
		memcpy(&_remasterView[y * W + m], _remasterRgb + y * W + sx, (W - 2 * m) * 4);
		if (_remasterProtect) {
			memset(&_remasterViewProtect[y * W], 0, W);
			memcpy(&_remasterViewProtect[y * W + m], _remasterProtect + y * W + sx, W - 2 * m);
		}
	}
	if (_remasterOwner) {
		_remasterViewOwner.resize(W * H);
		for (int y = 0; y < H; y++) {
			memset(&_remasterViewOwner[y * W], 0, W);
			memcpy(&_remasterViewOwner[y * W + m], _remasterOwner + y * W + sx, W - 2 * m);
		}
	}
	_remasterRgb = _remasterView.data();
	if (_remasterProtect)
		_remasterProtect = _remasterViewProtect.data();
	if (_remasterOwner)
		_remasterOwner = _remasterViewOwner.data();
	remasterFillMargins();
	if (_remasterClean.size() == (uint)(W * H) && _remasterProtect) {
		// The centred frame exactly as just made (remasterFillMargins also blends the room's outer columns into the
		// side art), with only the glyph pixels replaced by the scene under them.
		_remasterViewClean = _remasterView;
		_remasterViewCleanProtect = _remasterViewProtect;
		for (int y = 0; y < H; y++)
			for (int x = m; x < W - m; x++) {
				const int v = y * W + x, s = y * W + sx + (x - m);
				if (_remasterViewProtect[v] == 3) {
					_remasterViewClean[v] = _remasterClean[s];
					_remasterViewCleanProtect[v] = _remasterCleanProtect[s];
				}
			}
	}
}

bool ScummEngine::remasterSingleScreenRoom() const {
	if (!_remasterLogicalWidth || _roomWidth <= _screenWidth)
		return false;
	// Called often (mouse mapping, every cel): the setting is parsed once per room.
	if (_remasterSingleRoomCached == _currentRoom)
		return _remasterSingleRoomResult;
	_remasterSingleRoomCached = _currentRoom;
	_remasterSingleRoomResult = remasterSingleScreenRoomParse();
	return _remasterSingleRoomResult;
}

bool ScummEngine::remasterSingleScreenRoomParse() const {
	Common::String list = ConfMan.hasKey("remaster_single_screen_rooms") ? ConfMan.get("remaster_single_screen_rooms") : "25";
	const char *q = list.c_str();
	while (*q) {
		while (*q && (*q < '0' || *q > '9'))
			q++;
		if (!*q)
			break;
		if (atoi(q) == _currentRoom)
			return true;
		while (*q >= '0' && *q <= '9')
			q++;
	}
	return false;
}

int ScummEngine::remasterViewSrcX() const {
	if (_remasterVideoShown || !remasterSingleScreenRoom())
		return 0;
	// The game's own 640-wide picture is centred on the camera; the wider view starts at xstart.
	const int left = camera._cur.x - _remasterLogicalWidth / 2 - _virtscr[kMainVirtScreen].xstart;
	return CLIP(left, 0, _screenWidth - _remasterLogicalWidth);
}

bool ScummEngine::remasterEdgeStrictRoom() const {
	// Rooms where animated scenery visibly leaves the picture (COMI: the island map's clouds, the diorama ride's
	// cart), so any actor at the edge triggers the edge guard. remaster_edge_strict_rooms overrides the list.
	Common::String list = ConfMan.hasKey("remaster_edge_strict_rooms") ? ConfMan.get("remaster_edge_strict_rooms") : "83,84,85";
	const char *q = list.c_str();
	while (*q) {
		while (*q && (*q < '0' || *q > '9'))
			q++;
		if (!*q)
			break;
		if (atoi(q) == _currentRoom)
			return true;
		while (*q >= '0' && *q <= '9')
			q++;
	}
	return false;
}

bool ScummEngine::remasterSideArtShown() {
	return _remasterLogicalWidth && _remasterSidesOn && remasterLoadSides() && _remasterEdgeVis[0] >= 256 && _remasterEdgeVis[1] >= 256;
}

bool ScummEngine::remasterLiveEdgeRoom() const {
	// Rooms whose animated scenery (water) meets the edges: the moving rows continue into the side art.
	const Common::String list = ConfMan.hasKey("remaster_live_edge_rooms") ? ConfMan.get("remaster_live_edge_rooms") : "11,13";
	const char *q = list.c_str();
	while (*q) {
		while (*q && (*q < '0' || *q > '9'))
			q++;
		if (!*q)
			break;
		if (atoi(q) == _currentRoom)
			return true;
		while (*q >= '0' && *q <= '9')
			q++;
	}
	return false;
}

void ScummEngine::remasterLiveEdges(int m, int contentW) {
	// Rows where the room's edge animates (measured over time) get the live edge mirrored outwards into the side
	// art, fading into the painting further out, so e.g. water keeps moving past the room's edge.
	const int W = _screenWidth, H = _screenHeight, BAND = 24;
	if (_remasterLiveRoom != _currentRoom || (int)_remasterLiveMotion.size() != 2 * H) {
		_remasterLiveRoom = _currentRoom;
		_remasterLiveMotion.clear();
		_remasterLiveMotion.resize(2 * H);
		_remasterLivePrev.clear();
		_remasterLivePrev.resize(2 * H * BAND);
		for (uint k = 0; k < _remasterLiveMotion.size(); k++) _remasterLiveMotion[k] = 0;
		_remasterLiveFrames = 0;
	}
	auto luma = [&](uint32 c) { uint8 r, g, b; _remasterFormat.colorToRGB(c, r, g, b); return (r * 77 + g * 150 + b * 29) >> 8; };
	for (int side = 0; side < 2; side++) {
		const int x0 = side ? m + contentW - BAND : m;
		for (int y = 0; y < H; y++) {
			int diff = 0;
			for (int k = 0; k < BAND; k++) {
				const int l = luma(_remasterRgb[y * W + x0 + k]);
				uint8 &pv = _remasterLivePrev[(side * H + y) * BAND + k];
				diff += ABS(l - (int)pv);
				pv = (uint8)l;
			}
			float &mo = _remasterLiveMotion[side * H + y];
			mo = _remasterLiveFrames > 0 && diff > BAND * 3 ? 1.0f : mo * 0.995f;
		}
	}
	_remasterLiveFrames++;
	// Sprites that are there from the moment the room appears (its animated water) are scenery and mirrored; sprites
	// that arrive later (floating debris, Murray) and Guybrush are not.
	const uint32 now = _system->getMillis();
	if (_remasterLiveSeenRoom != _currentRoom) {
		_remasterLiveSeenRoom = _currentRoom;
		_remasterLiveRoomSince = now;
		for (int k = 0; k < 256; k++)
			_remasterLiveScenery[k] = 0;   // 0 unseen, 1 scenery, 2 not
	}
	if (_remasterCels.empty() && now - _remasterLiveRoomSince < 20000)
		_remasterLiveRoomSince = now;   // the clock starts with the first sprite (rooms fade in first)
	for (uint k = 0; k < _remasterCels.size(); k++) {
		const int a = _remasterCels[k].actor;
		if (a > 0 && a < 256 && !_remasterLiveScenery[a])
			_remasterLiveScenery[a] = (now - _remasterLiveRoomSince < 1500 && (VAR_EGO == 0xFF || a != VAR(VAR_EGO))) ? 1 : 2;
	}
	bool smallActor[256];
	for (int k = 0; k < 256; k++)
		smallActor[k] = _remasterLiveScenery[k] == 2;   // unseen (not a recorded sprite, e.g. the water) = scenery
	// Learn the edge's own colours (palette indices in the animated rows) during the first seconds the room is shown,
	// once per session per room: later visitors to the edge (debris floating by) are then not mirrored.
	const int srcX8 = remasterViewSrcX();
	Common::Array<bool> &learned = _remasterLiveColours[_currentRoom];
	if (learned.size() != 256) {
		learned.resize(256);
		for (int k = 0; k < 256; k++)
			learned[k] = false;
		_remasterLiveLearnUntil[_currentRoom] = now + 4000;
	}
	const bool learning = now < _remasterLiveLearnUntil[_currentRoom];
	const bool learnedReady = !learning;
	if (learning) {
		for (int side = 0; side < 2; side++)
			for (int y = 0; y < H; y++) {
				if (_remasterLiveMotion[side * H + y] < 0.5f)
					continue;
				for (int d = 0; d < m && d < contentW; d++) {
					const int rx = side ? m + contentW - 1 - d : m + d;
					const int sx8 = rx - m + srcX8;
					if (sx8 >= 0 && sx8 < W && (!_remasterProtect || _remasterProtect[y * W + rx] != 3))
						learned[_remasterScreen8[y * W + sx8]] = true;
				}
			}
	}
	for (int side = 0; side < 2; side++) {
		// Vertical smoothing of the motion mask (rows between ripples belong to the water too).
		Common::Array<float> sm;
		sm.resize(H);
		for (int y = 0; y < H; y++) {
			float acc = 0;
			int n = 0;
			for (int dy = -12; dy <= 12; dy++) {
				const int yy = y + dy;
				if (yy < 0 || yy >= H) continue;
				acc = MAX(acc, _remasterLiveMotion[side * H + yy] * (1.0f - ABS(dy) / 13.0f));
				n++;
			}
			sm[y] = acc;
		}
		for (int y = 0; y < H; y++) {
			const float wr = sm[y];
			if (wr <= 0.01f)
				continue;
			uint32 *row = _remasterRgb + y * W;
			for (int d = 1; d <= m; d++) {
				const int sx = side ? m + contentW - 1 + d : m - d;          // side pixel
				const int rx = side ? m + contentW - d : m + d - 1;          // mirrored room pixel
				if (rx < m || rx >= m + contentW)
					break;
				const float wgt = wr * CLIP(1.0f - (float)(d - 1) / (m * 0.85f), 0.0f, 1.0f);
				if (wgt <= 0.01f)
					break;
				// Only scenery is mirrored: characters, shadows and subtitles (and the pixels around text, its outline)
				// would otherwise appear back-to-front on the side; the painted side art stays there.
				if (_remasterProtect) {
					bool fg = false;
					for (int dy = -1; dy <= 1 && !fg; dy++)
						for (int dx = -1; dx <= 1 && !fg; dx++) {
							const int yy = y + dy, xx = rx + dx;
							if (yy < 0 || yy >= H || xx < 0 || xx >= W)
								continue;
							const byte k = _remasterProtect[yy * W + xx];
							const byte own = _remasterOwner ? _remasterOwner[yy * W + xx] : 0;
							fg = k == 3 || (k == 2 && own && smallActor[own]);   // text, or a sprite that arrived later
						}
					// Anything not in the colours the edge had when the room was first seen (floating debris, Murray)
					if (!fg && learnedReady) {
						const int sx8 = rx - m + srcX8;
						if (sx8 >= 0 && sx8 < W && !learned[_remasterScreen8[y * W + sx8]])
							fg = true;
						}
					if (fg)
						continue;
				}
				uint8 r0, g0, b0, r1, g1, b1;
				_remasterFormat.colorToRGB(row[sx], r0, g0, b0);
				_remasterFormat.colorToRGB(row[rx], r1, g1, b1);
				row[sx] = _remasterFormat.RGBToColor((uint8)(r0 + (r1 - r0) * wgt), (uint8)(g0 + (g1 - g0) * wgt), (uint8)(b0 + (b1 - b0) * wgt));
			}
		}
	}
}

bool ScummEngine::remasterLoadSides() {
	// Side art for the current narrow room, loaded once per room. Expected size: 848 x room height, with the
	// original room in the middle (generated by dists/remaster/sides/outpaint.py).
	if (!ConfMan.hasKey("remaster_sides_path"))
		return false;
	if (_remasterSidesRoom == _currentRoom)
		return _remasterSides.getPixels() != nullptr;
	_remasterSidesRoom = _currentRoom;
	_remasterSides.free();
	Common::FSNode root(Common::Path(ConfMan.get("remaster_sides_path"), Common::Path::kNativeSeparator));
	Common::FSNode file = root.getChild(Common::String::format("%04d.png", _currentRoom));
	if (!file.exists())
		return false;
	Common::SeekableReadStream *stream = file.createReadStream();
	Image::PNGDecoder png;
	const bool ok = stream && png.loadStream(*stream);
	delete stream;
	const Graphics::Surface *src = ok ? png.getSurface() : nullptr;
	if (!src || src->w != _screenWidth || src->h != _roomHeight ||
	    (src->format.bytesPerPixel != 3 && src->format.bytesPerPixel != 4)) {
		if (src)
			warning("remaster: %s is %dx%d, expected %dx%d", file.getPath().toString().c_str(), src->w, src->h, _screenWidth, _roomHeight);
		return false;
	}
	_remasterSides.create(src->w, src->h, _remasterFormat);
	for (int y = 0; y < src->h; y++) {
		const byte *sp = (const byte *)src->getBasePtr(0, y);
		uint32 *d = (uint32 *)_remasterSides.getBasePtr(0, y);
		for (int x = 0; x < src->w; x++, sp += src->format.bytesPerPixel) {
			const uint32 c = src->format.bytesPerPixel == 4 ? READ_UINT32(sp) : READ_UINT24(sp);
			uint8 a, r, g, b;
			src->format.colorToARGB(c, a, r, g, b);
			d[x] = _remasterFormat.RGBToColor(r, g, b);
		}
	}
	return true;
}

void ScummEngine::remasterFillMargins() {
	// Widescreen margins next to a centred narrow room or video: a soft, darkened continuation of the picture's
	// edge colours (per row average of the outer 24 columns, smoothed vertically, fading outwards).
	const int W = _screenWidth, H = _screenHeight, m = remasterMargin();
	if (m <= 0)
		return;
	const int contentW = W - 2 * m;
	// Narrow room with side art: copy the matching rows (the view can scroll vertically in tall rooms).
	if (!_remasterVideoShown && _remasterSidesOn && remasterLoadSides()) {
		// Edge guard: characters are only drawn inside the original room, so one walking off the edge would be cut
		// by an invisible line in front of the side art. While an actor touches a room edge, that side's art fades
		// to black (like the original screen edge) and stays black until the room changes, so actors that keep
		// crossing the edges (e.g. drifting clouds) cannot make it flicker.
		if (_remasterEdgeRoom != _currentRoom) {
			_remasterEdgeRoom = _currentRoom;
			memset(_remasterEdgeActor, 0, sizeof(_remasterEdgeActor));
			_remasterEdgeVis[0] = _remasterEdgeVis[1] = 256;
			_remasterEdgeHold[0] = _remasterEdgeHold[1] = 0;
		}
		_remasterEdgeAnimating = false;
		for (int side = 0; side < 2; side++) {
			bool touch = false;
			// Only characters that walk (actors using walk boxes, e.g. Guybrush and other people) count; animated
			// scenery drawn as actors (waves, mist, sparks, clouds) ignores walk boxes and may cross the edge.
			for (int a = 1; a < 256 && !touch; a++) {
				if (!_remasterEdgeActor[side][a])
					continue;
				_remasterEdgeActor[side][a] = 0;
				Actor *act = (a < _numActors) ? _actors[a] : nullptr;
				if (!act || !act->isInCurrentRoom() || !act->_visible)
					continue;
				debug(1, "remaster: edge actor %d (costume %d, ignoreBoxes %d) at %s edge of room %d", a, act->_costume,
					act->_ignoreBoxes ? 1 : 0, side ? "right" : "left", _currentRoom);
				if (_remasterMode == kRemasterAI && _remasterHDActors && _remasterCelActor[a] == _remasterCelFrame)
					continue; // drawn by the HD actor renderer, which continues it onto the side art
				if (act->_ignoreBoxes && remasterLiveEdgeRoom())
					continue; // animated scenery continues through the live edges
				if (!act->_ignoreBoxes || remasterEdgeStrictRoom())
					touch = true;
			}
			if (touch && ConfMan.hasKey("remaster_edge_guard") && !ConfMan.getBool("remaster_edge_guard"))
				touch = false; // edge guard switched off
			if (touch && !_remasterEdgeHold[side]) {
				_remasterEdgeHold[side] = 1;                // sticky for the rest of this room visit (no flicker)
				debug(1, "remaster: edge guard, room %d, %s edge touched", _currentRoom, side ? "right" : "left");
			}
		}
		for (int side = 0; side < 2; side++) {
			// Symmetric: if either side was triggered, both sides fade (one painted, one black side looks lopsided).
			const int target = (_remasterEdgeHold[0] > 0 || _remasterEdgeHold[1] > 0) ? 0 : 256;
			int &v = _remasterEdgeVis[side];
			if (v != target) {
				v = target > v ? MIN(target, v + 64) : MAX(target, v - 64);   // ~4 frames
				_remasterEdgeAnimating = true;
			}
		}
		// Palette fades (the room fading in from black, darkening): the painted side art follows the room's brightness,
		// measured as the shown palette against the room's own palette (the one darkenPalette scales).
		// A fade scales every colour alike; lighting effects only change some, so the median ratio of the colours is
		// used (and ratios within 3% of full count as no fade).
		int palFade = 256;
		if (const byte *ref = getPalettePtr(_curPalIndex, _roomResource)) {
			int ratios[240], nr = 0;
			for (int i = 0; i < 240; i++) {   // the top entries are the cursor's
				const int r = ref[i * 3] + ref[i * 3 + 1] + ref[i * 3 + 2];
				if (r >= 60)
					ratios[nr++] = MIN(512, (_remasterPal[i * 3] + _remasterPal[i * 3 + 1] + _remasterPal[i * 3 + 2]) * 256 / r);
			}
			if (nr >= 16) {
				Common::sort(ratios, ratios + nr);
				palFade = CLIP(ratios[nr / 2], 0, 256);
				if (palFade >= 248)
					palFade = 256;
			}
		}
		if (palFade != _remasterSidesFade) {
			_remasterSidesFade = palFade;
			_remasterEdgeAnimating = true;   // keep presenting while the fade runs
		}
		const int top = CLIP<int>(_screenTop, 0, _remasterSides.h - H);
		for (int y = 0; y < H; y++) {
			const uint32 *sp = (const uint32 *)_remasterSides.getBasePtr(0, CLIP(top + y, 0, (int)_remasterSides.h - 1));
			memcpy(_remasterRgb + y * W, sp, m * 4);
			memcpy(_remasterRgb + y * W + m + contentW, sp + m + contentW, (W - m - contentW) * 4);
			for (int side = 0; side < 2; side++) {
				const int vis = _remasterEdgeVis[side] * palFade >> 8;
				if (vis >= 256)
					continue;
				uint32 *row = _remasterRgb + y * W + (side ? m + contentW : 0);
				const int n = side ? W - m - contentW : m;
				for (int x = 0; x < n; x++) {
					uint8 r, g, b;
					_remasterFormat.colorToRGB(row[x], r, g, b);
					row[x] = _remasterFormat.RGBToColor((r * vis) >> 8, (g * vis) >> 8, (b * vis) >> 8);
				}
			}
			// Hide the seam: blend 24 px of the room edge towards the side art (whose edge band holds the side
			// painting's own aligned version of the room), leaving actors and text untouched.
			for (int i = 0; i < 24; i++) {
				for (int side = 0; side < 2; side++) {
					const int k = (256 * (24 - i) / 25) * _remasterEdgeVis[side] >> 8;
					const int x = side ? m + contentW - 1 - i : m + i;
					if (_remasterProtect && _remasterProtect[y * W + x] >= 2)
						continue;
					uint8 r0, g0, b0, r1, g1, b1;
					_remasterFormat.colorToRGB(_remasterRgb[y * W + x], r0, g0, b0);
					_remasterFormat.colorToRGB(sp[x], r1, g1, b1);
					r1 = (uint8)(r1 * palFade >> 8); g1 = (uint8)(g1 * palFade >> 8); b1 = (uint8)(b1 * palFade >> 8);
					_remasterRgb[y * W + x] = _remasterFormat.RGBToColor((r0 * (256 - k) + r1 * k) >> 8, (g0 * (256 - k) + g1 * k) >> 8, (b0 * (256 - k) + b1 * k) >> 8);
				}
			}
		}
		if (remasterLiveEdgeRoom())
			remasterLiveEdges(m, contentW);
		return;
	}
	if (_remasterVideoShown && ConfMan.hasKey("remaster_video_sides") && ConfMan.get("remaster_video_sides") == "extend") {
		// Cutscenes: the video's own edges continue outwards live, mirrored, increasingly blurred and darkened.
		Common::Array<int> pre;
		pre.resize((W + 1) * 3);
		for (int y = 0; y < H; y++) {
			uint32 *row = _remasterRgb + y * W;
			pre[0] = pre[1] = pre[2] = 0;
			for (int x = 0; x < W; x++) {
				uint8 r, g, b;
				_remasterFormat.colorToRGB(row[x], r, g, b);
				pre[(x + 1) * 3] = pre[x * 3] + r; pre[(x + 1) * 3 + 1] = pre[x * 3 + 1] + g; pre[(x + 1) * 3 + 2] = pre[x * 3 + 2] + b;
			}
			auto avg = [&](int x0, int x1, int c) { x0 = CLIP(x0, m, m + contentW - 1); x1 = CLIP(x1, m, m + contentW - 1); return (pre[(x1 + 1) * 3 + c] - pre[x0 * 3 + c]) / (x1 - x0 + 1); };
			for (int d = 1; d <= m; d++) {
				const int rad = 1 + d / 5;
				const float shade = 0.85f - 0.6f * d / m;
				for (int side = 0; side < 2; side++) {
					const int src = side ? m + contentW - d : m + d - 1;
					const int dst = side ? m + contentW - 1 + d : m - d;
					if (dst < 0 || dst >= W)
						continue;
					row[dst] = _remasterFormat.RGBToColor((uint8)(avg(src - rad, src + rad, 0) * shade), (uint8)(avg(src - rad, src + rad, 1) * shade),
						(uint8)(avg(src - rad, src + rad, 2) * shade));
				}
			}
		}
		return;
	}
	if (!_remasterAmbientOn) { // F12: black bars
		for (int y = 0; y < H; y++) {
			memset(_remasterRgb + y * W, 0, m * 4);
			memset(_remasterRgb + y * W + m + contentW, 0, (W - m - contentW) * 4);
		}
		return;
	}
	const Graphics::PixelFormat &fmt = _remasterFormat;
	Common::Array<int> edge;
	edge.resize(H * 6);
	for (int y = 0; y < H; y++) {
		for (int side = 0; side < 2; side++) {
			int sum[3] = {0, 0, 0};
			const int x0 = side == 0 ? m : m + contentW - 24;
			for (int x = x0; x < x0 + 24; x++) {
				uint8 r, g, b;
				fmt.colorToRGB(_remasterRgb[y * W + x], r, g, b);
				sum[0] += r; sum[1] += g; sum[2] += b;
			}
			for (int c = 0; c < 3; c++)
				edge[y * 6 + side * 3 + c] = sum[c] / 24;
		}
	}
	for (int y = 0; y < H; y++) {
		int acc[6] = {0, 0, 0, 0, 0, 0}, n = 0;
		for (int dy = -20; dy <= 20; dy++) {
			const int yy = CLIP(y + dy, 0, H - 1);
			for (int k = 0; k < 6; k++)
				acc[k] += edge[yy * 6 + k];
			n++;
		}
		uint32 *row = _remasterRgb + y * W;
		for (int x = 0; x < m; x++) {
			const int shade = 150 + 90 * x / m; // darker towards the screen edge
			row[x] = fmt.RGBToColor(acc[0] / n * shade / 256, acc[1] / n * shade / 256, acc[2] / n * shade / 256);
			const int rx = W - 1 - x;
			if (rx >= m + contentW)
				row[rx] = fmt.RGBToColor(acc[3] / n * shade / 256, acc[4] / n * shade / 256, acc[5] / n * shade / 256);
		}
		for (int x = m + contentW; x < W - m; x++)  // odd leftover columns
			row[x] = row[W - 1 - (x - (m + contentW))];
	}
}
void ScummEngine::remasterToggleAmbient() {
	// F12 cycles the side panels: AI side art (widescreen narrow rooms, when available) -> ambient -> black.
	const bool sidesAvailable = _remasterLogicalWidth && ConfMan.hasKey("remaster_sides_path");
	const char *msg;
	if (sidesAvailable && _remasterSidesOn) {
		_remasterSidesOn = false;
		_remasterAmbientOn = true;
		msg = "Side panels: ambient (F12)";
	} else if (_remasterAmbientOn) {
		_remasterAmbientOn = false;
		msg = "Side panels: black (F12)";
	} else if (sidesAvailable) {
		_remasterSidesOn = true;
		_remasterAmbientOn = true;
		msg = "Side panels: AI side art (F12)";
	} else {
		_remasterAmbientOn = true;
		msg = "Side panels: ambient (F12)";
	}
	ConfMan.setBool("remaster_ambient_sides", _remasterAmbientOn);
	ConfMan.setBool("remaster_side_art", _remasterSidesOn);
	ConfMan.flushToDisk();
	_remasterFrameDirty = true;
	_system->displayMessageOnOSD(Common::U32String(msg));
}

void ScummEngine::remasterToggleAI() {
	remasterSetMode(_remasterMode == kRemasterAI ? kRemasterOriginal : kRemasterAI, true);
}

void ScummEngine::remasterSetMode(int mode, bool announce) {
	static const char *const names[kRemasterModeCount] = {
		"AI HD", "Original", "VGA (1992)", "EGA (1990)", "Amiga (32 colours)", "Modern pixel art", "Classic 320x200 (experimental)"
	};
	if (mode == kRemasterAI && !_remasterAI)
		mode = kRemasterOriginal;
	_remasterMode = mode;
	_remasterAIOn = mode == kRemasterAI;
	ConfMan.setInt("remaster_display_mode", mode);
	ConfMan.setBool("remaster_ai_enabled", mode == kRemasterAI);   // keeps the options checkbox in step
	ConfMan.flushToDisk();
	_remasterFrameDirty = true;
	updateCursor();
	if (announce)
		_system->displayMessageOnOSD(Common::U32String(Common::String::format("Display: %s (F10)", names[mode])));
}

// Scale an 8-bit cursor with the classic Scale2x / Scale3x pixel-art algorithms (on palette indices, so the
// transparent colour stays exact): diagonal edges become smooth while the cursor stays sharp. Scale 4 = 2x twice.
static void scale2x(const byte *src, int w, int h, Common::Array<byte> &out) {
	out.resize(w * 2 * h * 2);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const byte E = src[y * w + x];
			const byte B = src[MAX(y - 1, 0) * w + x], H = src[MIN(y + 1, h - 1) * w + x];
			const byte D = src[y * w + MAX(x - 1, 0)], F = src[y * w + MIN(x + 1, w - 1)];
			byte e0 = E, e1 = E, e2 = E, e3 = E;
			if (B != H && D != F) {
				e0 = D == B ? D : E;
				e1 = B == F ? F : E;
				e2 = D == H ? D : E;
				e3 = H == F ? F : E;
			}
			byte *o = &out[(y * 2) * w * 2 + x * 2];
			o[0] = e0; o[1] = e1; o[w * 2] = e2; o[w * 2 + 1] = e3;
		}
	}
}

static void scale3x(const byte *src, int w, int h, Common::Array<byte> &out) {
	out.resize(w * 3 * h * 3);
	auto at = [&](int x, int y) { return src[CLIP(y, 0, h - 1) * w + CLIP(x, 0, w - 1)]; };
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const byte A = at(x - 1, y - 1), B = at(x, y - 1), C = at(x + 1, y - 1);
			const byte D = at(x - 1, y), E = at(x, y), F = at(x + 1, y);
			const byte G = at(x - 1, y + 1), H = at(x, y + 1), I = at(x + 1, y + 1);
			byte e[9] = {E, E, E, E, E, E, E, E, E};
			if (B != H && D != F) {
				e[0] = D == B ? D : E;
				e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
				e[2] = B == F ? F : E;
				e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
				e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
				e[6] = D == H ? D : E;
				e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
				e[8] = H == F ? F : E;
			}
			for (int k = 0; k < 9; k++)
				out[(y * 3 + k / 3) * w * 3 + x * 3 + k % 3] = e[k];
		}
	}
}

void ScummEngine::remasterScaleCursor(const byte *src, int w, int h, int scale, Common::Array<byte> &out) {
	if (scale == 2) {
		scale2x(src, w, h, out);
	} else if (scale == 3) {
		scale3x(src, w, h, out);
	} else if (scale == 4) {
		Common::Array<byte> tmp;
		scale2x(src, w, h, tmp);
		scale2x(tmp.data(), w * 2, h * 2, out);
	} else {
		out.resize(w * scale * h * scale);
		for (int y = 0; y < h * scale; y++)
			for (int x = 0; x < w * scale; x++)
				out[y * w * scale + x] = src[(y / scale) * w + x / scale];
	}
}

void ScummEngine::remasterSharpText() {
	// remaster_sharp_text: glyph pixels drawn with Scale3x (x3), Scale2x (x2) or Scale2x with every result pixel doubled
	// (x4) instead of plain blocks: rounder diagonals, same letters. Only the glyphs' own blocks are written, so the
	// picture beside the text is left alone (see remasterTextOverlay).
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	if (!_remasterProtect || S < 2 || S > 4)
		return;
	auto at = [&](int x, int y) { return _remasterRgb[CLIP(y, 0, H - 1) * W + CLIP(x, 0, W - 1)]; };
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			if (_remasterProtect[y * W + x] != 3)
				continue;
			const uint32 A = at(x - 1, y - 1), B = at(x, y - 1), C = at(x + 1, y - 1);
			const uint32 D = at(x - 1, y), E = at(x, y), F = at(x + 1, y);
			const uint32 G = at(x - 1, y + 1), Hh = at(x, y + 1), I = at(x + 1, y + 1);
			uint32 *o = _remasterBig + (y * S) * BW + x * S;
			if (S == 3) {
				uint32 e[9] = {E, E, E, E, E, E, E, E, E};
				if (B != Hh && D != F) {
					e[0] = D == B ? D : E;
					e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
					e[2] = B == F ? F : E;
					e[3] = (D == B && E != G) || (D == Hh && E != A) ? D : E;
					e[5] = (B == F && E != I) || (Hh == F && E != C) ? F : E;
					e[6] = D == Hh ? D : E;
					e[7] = (D == Hh && E != I) || (Hh == F && E != G) ? Hh : E;
					e[8] = Hh == F ? F : E;
				}
				for (int k = 0; k < 9; k++)
					o[(k / 3) * BW + k % 3] = e[k];
			} else {
				uint32 e[4] = {E, E, E, E};
				if (B != Hh && D != F) {
					e[0] = D == B ? D : E; e[1] = B == F ? F : E; e[2] = D == Hh ? D : E; e[3] = Hh == F ? F : E;
				}
				const int k = S / 2;   // x2: 1x1 per Scale2x pixel, x4: 2x2
				for (int q = 0; q < 4; q++)
					for (int dy = 0; dy < k; dy++)
						for (int dx = 0; dx < k; dx++)
							o[((q >> 1) * k + dy) * BW + (q & 1) * k + dx] = e[q];
			}
		}
	}
}

void ScummEngine::remasterBeginActorDraw(int actor) {
	if (_remasterCelRoom != _currentRoom) {
		_remasterCels.clear();
		_remasterCelRoom = _currentRoom;
		_remasterCelFrame++;
	}
	for (uint i = 0; i < _remasterCels.size();) {
		if (_remasterCels[i].actor == actor)
			_remasterCels.remove_at(i);
		else
			i++;
	}
}

void ScummEngine::remasterRecordCel(const byte *src, int w, int h, byte mask, byte shr, const uint16 *palette,
		byte shadowMode, bool mirror, const Common::Rect &rect, int xOff) {
	if (!_remasterProtect || _remasterScale < 2 || w <= 0 || h <= 0 || w > 1024 || h > 1024 || _remasterDrawActor <= 0)
		return;
	// Edge guard: remember actors whose sprite really continues beyond the room's edge (it is then drawn onto
	// the side art); sprites whose art stops at the edge still fade the side art.
	if (_remasterDrawActor > 0 && _remasterDrawActor < 256 && (rect.left - xOff < 0 || rect.right - xOff > _roomWidth))
		_remasterCelActor[_remasterDrawActor] = _remasterCelFrame;
	RemasterCel cel;
	cel.actor = _remasterDrawActor;
	cel.seq = ++_remasterCelSeq;
	cel.w = w;
	cel.h = h;
	cel.mirror = mirror;
	if (rect.right <= rect.left || rect.bottom <= rect.top)
		return; // empty or inverted (fully clipped) cel
	cel.rect = Common::Rect(rect.left - xOff, rect.top - _screenTop, rect.right - xOff, rect.bottom - _screenTop);
	if (cel.rect.width() <= 0 || cel.rect.height() <= 0)
		return;
	cel.px.resize(w * h);
	// byleRLE: column-major runs of (colour, length); colour 0 is transparent.
	int col = 0, row = 0;
	while (col < w) {
		byte len = *src++;
		const byte color = len >> shr;
		len &= mask;
		int n = len ? len : *src++;
		int16 v = -1;
		if (color) {
			const uint16 pc = palette[color];
			const bool shadow = (shadowMode == 3 && pc < 8) || (shadowMode == 1 && pc == 13);
			v = shadow ? -2 : (int16)(pc & 0xFF);
		}
		while (n-- > 0 && col < w) {
			cel.px[row * w + col] = v;
			if (++row == h) {
				row = 0;
				col++;
			}
		}
	}
	_remasterCels.push_back(Common::move(cel));
}

void ScummEngine::remasterCelAlpha(const Common::Array<uint8> &a, int cw, int ch, int S, uint32 *rgba) {
	// Alpha: bilinear from the cel's mask, sharpened a little so edges stay crisp.
	const int W = cw * S, H = ch * S;
	for (int y = 0; y < H; y++) {
		const float fy = CLIP((y + 0.5f) / S - 0.5f, 0.0f, (float)ch - 1);
		const int y0 = (int)fy, y1 = MIN(y0 + 1, ch - 1);
		const float ay = fy - y0;
		for (int x = 0; x < W; x++) {
			const float fx = CLIP((x + 0.5f) / S - 0.5f, 0.0f, (float)cw - 1);
			const int x0 = (int)fx, x1 = MIN(x0 + 1, cw - 1);
			const float ax = fx - x0;
			float v = (a[y0 * cw + x0] * (1 - ax) + a[y0 * cw + x1] * ax) * (1 - ay) + (a[y1 * cw + x0] * (1 - ax) + a[y1 * cw + x1] * ax) * ay;
			v = CLIP((v - 128.0f) * 2.0f + 128.0f, 0.0f, 255.0f);
			rgba[y * W + x] = (rgba[y * W + x] & 0x00FFFFFFu) | ((uint32)v << 24);
		}
	}
}

void ScummEngine::remasterCelCacheTrim() {
	const size_t budget = (size_t)(ConfMan.hasKey("remaster_hd_actor_cache_mb") ? MAX(64, ConfMan.getInt("remaster_hd_actor_cache_mb")) : 768) << 20;
	size_t total = 0;
	for (Common::HashMap<uint32, RemasterCelHD>::const_iterator it = _remasterCelCache.begin(); it != _remasterCelCache.end(); ++it)
		total += it->_value.bytes();
	if (total <= budget)
		return;
	// least recently used first, down to 3/4 of the budget
	Common::Array<Common::Pair<uint32, uint32> > ages;   // (last use, key)
	for (Common::HashMap<uint32, RemasterCelHD>::const_iterator it = _remasterCelCache.begin(); it != _remasterCelCache.end(); ++it)
		ages.push_back(Common::Pair<uint32, uint32>(it->_value.lastUse, it->_key));
	Common::sort(ages.begin(), ages.end(), [](const Common::Pair<uint32, uint32> &a, const Common::Pair<uint32, uint32> &b) { return a.first < b.first; });
	const uint32 now = _system->getMillis();
	for (uint i = 0; i < ages.size() && total > budget / 4 * 3; i++) {
		if (now - ages[i].first < 2000)
			break;   // in use right now
		total -= _remasterCelCache[ages[i].second].bytes();
		_remasterCelCache.erase(ages[i].second);
	}
}

void ScummEngine::remasterCelRefinePoll() {
	if (_remasterCelJob) {
		if (!remasterAIJobDone(_remasterCelJob))
			return;
		const RemasterCelPending &pc = _remasterCelJobCel;
		const int BS = _remasterBgScale, S = MIN(BS, 3);   // kept at the real-time scale (characters never show larger)
		if (const byte *res = remasterAIJobResult(_remasterCelJob)) {
			if (_remasterCelCache.contains(pc.key)) {
				RemasterCelHD hd;
				hd.w = pc.w * S;
				hd.h = pc.h * S;
				hd.rgba.resize(hd.w * hd.h);
				// area-average BS -> S
				const int iw = pc.w * BS;
				const float f = (float)BS / S;
				for (int y = 0; y < hd.h; y++) {
					const float y0 = y * f, y1 = y0 + f;
					for (int x = 0; x < hd.w; x++) {
						const float x0 = x * f, x1 = x0 + f;
						float acc[3] = {0, 0, 0}, wsum = 0;
						for (int yy = (int)y0; yy < (int)ceilf(y1) && yy < pc.h * BS; yy++) {
							const float wy = MIN<float>(y1, yy + 1) - MAX<float>(y0, yy);
							for (int xx = (int)x0; xx < (int)ceilf(x1) && xx < iw; xx++) {
								const float wgt = wy * (MIN<float>(x1, xx + 1) - MAX<float>(x0, xx));
								const byte *q = res + ((size_t)yy * iw + xx) * 4;
								acc[0] += q[0] * wgt; acc[1] += q[1] * wgt; acc[2] += q[2] * wgt;
								wsum += wgt;
							}
						}
						const float k = wsum > 0 ? 1.0f / wsum : 0;
						hd.rgba[y * hd.w + x] = (uint32)(acc[0] * k + 0.5f) | ((uint32)(acc[1] * k + 0.5f) << 8) | ((uint32)(acc[2] * k + 0.5f) << 16) | 0xFF000000u;
					}
				}
				remasterCelAlpha(pc.a, pc.w, pc.h, S, hd.rgba.data());
				hd.lastUse = _remasterCelCache[pc.key].lastUse;
				_remasterCelCache[pc.key] = hd;   // the resampling cache starts empty, so it is redone at screen size
				_remasterCelRefined++;
			}
		}
		remasterAIJobDestroy(_remasterCelJob);
		_remasterCelJob = nullptr;
	}
	if (_remasterCelQueue.empty() || !_remasterBgAI)
		return;
	// A background trickle: the heavy network shares the GPU with the real-time one, so after each cel wait a few
	// game frames, and not at all while frames are late (walking produces a new cel almost every frame).
	if (_remasterCelCooldown > 0) {
		_remasterCelCooldown--;
		return;
	}
	if (_remasterGovHist & 3)
		return;
	_remasterCelCooldown = 3;
	// newest first: the cels on screen now
	_remasterCelJobCel = _remasterCelQueue.back();
	_remasterCelQueue.pop_back();
	if (!_remasterCelCache.contains(_remasterCelJobCel.key))
		return;
	_remasterCelJob = remasterAIJobStart(_remasterBgAI, (const byte *)_remasterCelJobCel.rgb.data(), _remasterCelJobCel.w, _remasterCelJobCel.h, _remasterBgScale, 96);   // small tiles: short GPU slices
}

const ScummEngine::RemasterCelHD *ScummEngine::remasterCelHD(const RemasterCel &c) {
	// Key: the cel's palette indices and size (palette changes give a new key through the indices' colours below).
	uint32 h = 2166136261u ^ (uint32)(c.w * 4099 + c.h);
	for (uint i = 0; i < c.px.size(); i++) {
		const int16 v = c.px[i];
		const uint32 col = v >= 0 ? _remasterLut[v] : (uint32)v;
		h = (h ^ col) * 16777619u;
	}
	if (_remasterCelCache.contains(h)) {
		RemasterCelHD &e = _remasterCelCache[h];
		e.lastUse = _system->getMillis();
		return &e;
	}
	const int S = 3;
	// Colour image with transparent pixels filled from opaque neighbours (no dark halos), alpha separately.
	Common::Array<uint32> rgb(c.w * c.h);
	Common::Array<uint8> a(c.w * c.h), filled(c.w * c.h);
	for (int i = 0; i < c.w * c.h; i++) {
		a[i] = c.px[i] >= 0 ? 255 : 0;
		filled[i] = a[i] ? 1 : 0;
		uint8 r = 0, g = 0, b = 0;
		if (a[i])
			_remasterFormat.colorToRGB(_remasterLut[c.px[i]], r, g, b);
		rgb[i] = r | (g << 8) | (b << 16) | 0xFF000000u;
	}
	for (int pass = 0; pass < 6; pass++) {
		Common::Array<uint8> nf = filled;
		for (int y = 0; y < c.h; y++)
			for (int x = 0; x < c.w; x++) {
				const int i = y * c.w + x;
				if (filled[i])
					continue;
				int sr = 0, sg = 0, sb = 0, n = 0;
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const int xx = x + dx, yy = y + dy;
						if (xx < 0 || yy < 0 || xx >= c.w || yy >= c.h || !filled[yy * c.w + xx])
							continue;
						const uint32 q = rgb[yy * c.w + xx];
						sr += q & 0xFF; sg += (q >> 8) & 0xFF; sb += (q >> 16) & 0xFF; n++;
					}
				if (n) {
					rgb[i] = (sr / n) | ((sg / n) << 8) | ((sb / n) << 16) | 0xFF000000u;
					nf[i] = 1;
				}
			}
		filled = nf;
	}
	RemasterCelHD hd;
	hd.w = c.w * S;
	hd.h = c.h * S;
	hd.rgba.resize(hd.w * hd.h);
	double ms;
	if (!_remasterAI || !remasterAIRun(_remasterAI, (const byte *)rgb.data(), c.w, c.h, S, (byte *)hd.rgba.data(), &ms)) {
		for (int y = 0; y < hd.h; y++)
			for (int x = 0; x < hd.w; x++)
				hd.rgba[y * hd.w + x] = rgb[(y / S) * c.w + x / S];
	}
	remasterCelAlpha(a, c.w, c.h, S, hd.rgba.data());
	if (_remasterBgAI && remasterAIBackendIsAI(_remasterAI) && (!ConfMan.hasKey("remaster_hd_actor_refine") || ConfMan.getBool("remaster_hd_actor_refine"))) {
		if (_remasterCelQueue.size() >= 96)
			_remasterCelQueue.remove_at(0);   // the oldest are probably gone from the screen
		RemasterCelPending pc;
		pc.key = h;
		pc.w = c.w;
		pc.h = c.h;
		pc.rgb = rgb;
		pc.a = a;
		_remasterCelQueue.push_back(pc);
	}
	hd.lastUse = _system->getMillis();
	_remasterCelCache[h] = hd;
	return &_remasterCelCache[h];
}

void ScummEngine::remasterHDActors() {
	// Paint recorded cels from AI-upscaled full-size sources straight to output resolution, over the AI frame.
	// A cel pixel is only used where the game's own frame shows this actor's costume, or where the cel barely
	// covers the game pixel (soft silhouette edge); anything else in front (scenery, other actors, text) wins.
	remasterCelRefinePoll();
	if ((++_remasterCelTrimTick & 31) == 0)
		remasterCelCacheTrim();
	if (_remasterCels.empty() || _remasterCelRoom != _currentRoom || !_remasterProtect || !_remasterOwner)
		return;
	const uint32 t0 = _system->getMillis();
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S, BH = H * S;
	const int margin = remasterMargin();
	// Narrow room with side art: sprites (characters, animated scenery) are drawn past the room edge onto the side
	// art from their full recorded cel, instead of being cut at the room edge.
	const bool overflow = margin > 0 && !_remasterVideoShown && remasterSideArtShown();
	Common::Array<uint32> order;
	for (uint i = 0; i < _remasterCels.size(); i++)
		order.push_back(i);
	Common::sort(order.begin(), order.end(), [&](uint32 a, uint32 b) { return _remasterCels[a].seq < _remasterCels[b].seq; });
	Common::Array<float> vis;
	Common::Array<uint32> stale;
	// smooth scrolling: coverage of the followed character
	const int egoId = VAR_EGO != 0xFF ? VAR(VAR_EGO) : -1;
	byte *egoAlpha = nullptr;
	if (_remasterSmOn) {
		_remasterSmEgoAlpha.resize((size_t)BW * BH);
		memset(_remasterSmEgoAlpha.data(), 0, _remasterSmEgoAlpha.size());
		egoAlpha = _remasterSmEgoAlpha.data();
	}
	for (uint oi = 0; oi < order.size(); oi++) {
		const RemasterCel &c = _remasterCels[order[oi]];
		const int gx0 = c.rect.left + remasterShift(), gy0 = c.rect.top, gw = c.rect.width(), gh = c.rect.height();
		const int ox0 = gx0 * S, oy0 = gy0 * S, ow = gw * S, oh = gh * S;
		if (ox0 >= BW || oy0 >= BH || ox0 + ow <= 0 || oy0 + oh <= 0)
			continue;
		// A cel is only replaced when its actor is drawn again. Once the game has erased an actor that is not
		// redrawn (e.g. the verb coin after it closes) none of its pixels remain; drop the cel, or its soft edge
		// keeps being painted as a faint dotted ghost.
		{
			bool owned = false;
			for (int gy = MAX(0, gy0); gy < MIN(H, gy0 + gh) && !owned; gy++)
				for (int gx = MAX(0, gx0); gx < MIN(W, gx0 + gw) && !owned; gx++)
					owned = _remasterOwner[gy * W + gx] == c.actor;
			if (!owned) {
				stale.push_back(order[oi]);
				continue;
			}
		}
		const RemasterCelHD *hd = remasterCelHD(c);
		if (hd->rsW != ow || hd->rsH != oh || hd->rsMirror != c.mirror || hd->rs.size() != (size_t)ow * oh * 4) {
			// Resample the HD cel to its on-screen size (kept while the actor keeps this size).
			hd->rs.resize(ow * oh * 4);
			uint8 *out = hd->rs.data();
			const float sx = (float)hd->w / ow, sy = (float)hd->h / oh;
			auto px = [hd, &c](int x, int y, float *o) {
				const uint32 q = hd->rgba[y * hd->w + (c.mirror ? hd->w - 1 - x : x)];
				const float al = (q >> 24) / 255.0f;
				o[0] = (q & 0xFF) * al; o[1] = ((q >> 8) & 0xFF) * al; o[2] = ((q >> 16) & 0xFF) * al; o[3] = al;
			};
			remasterParallel(oh, [&](int ya, int yb) {
				for (int oy = ya; oy < yb; oy++) {
					for (int ox = 0; ox < ow; ox++) {
						float acc[4] = {0, 0, 0, 0};
						if (sx >= 1.0f && sy >= 1.0f) {
							const float fx0 = ox * sx, fx1 = fx0 + sx, fy0 = oy * sy, fy1 = fy0 + sy;
							float wsum = 0;
							for (int y = (int)fy0; y < hd->h && y < fy1; y++) {
								const float wy = MIN<float>(fy1, y + 1) - MAX<float>(fy0, y);
								for (int x = (int)fx0; x < hd->w && x < fx1; x++) {
									const float wgt = wy * (MIN<float>(fx1, x + 1) - MAX<float>(fx0, x));
									float p[4];
									px(x, y, p);
									for (int k = 0; k < 4; k++) acc[k] += p[k] * wgt;
									wsum += wgt;
								}
							}
							if (wsum > 0) for (int k = 0; k < 4; k++) acc[k] /= wsum;
						} else {
							const float fx = CLIP((ox + 0.5f) * sx - 0.5f, 0.0f, (float)hd->w - 1), fy = CLIP((oy + 0.5f) * sy - 0.5f, 0.0f, (float)hd->h - 1);
							const int x0 = (int)fx, y0 = (int)fy, x1 = MIN(x0 + 1, hd->w - 1), y1 = MIN(y0 + 1, hd->h - 1);
							const float ax = fx - x0, ay = fy - y0;
							const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
							const float ws[4] = {(1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay};
							for (int q = 0; q < 4; q++) {
								float p[4];
								px(xs[q], ys[q], p);
								for (int k = 0; k < 4; k++) acc[k] += p[k] * ws[q];
							}
						}
						uint8 *d = &out[(oy * ow + ox) * 4];
						d[0] = (uint8)CLIP(acc[0] + 0.5f, 0.0f, 255.0f);
						d[1] = (uint8)CLIP(acc[1] + 0.5f, 0.0f, 255.0f);
						d[2] = (uint8)CLIP(acc[2] + 0.5f, 0.0f, 255.0f);
						d[3] = (uint8)CLIP(acc[3] * 255.0f + 0.5f, 0.0f, 255.0f);
					}
				}
			});
			// coverage per game pixel
			hd->rsCov.resize(gw * gh);
			for (int gy = 0; gy < gh; gy++)
				for (int gx = 0; gx < gw; gx++) {
					float a = 0;
					for (int yy = 0; yy < S; yy++)
						for (int xx = 0; xx < S; xx++)
							a += out[((gy * S + yy) * ow + gx * S + xx) * 4 + 3];
					hd->rsCov[gy * gw + gx] = a / (255.0f * S * S);
				}
			hd->rsW = ow;
			hd->rsH = oh;
			hd->rsMirror = c.mirror;
		}
		const uint8 *rgba = hd->rs.data();
		const float *cov = hd->rsCov.data();
		// Continued onto the side art only where the game shows the sprite at that room edge (cut by the edge). A
		// sprite that has drifted wholly past the edge (debris floating out of the cannon view) is invisible in the
		// original and stays so.
		bool cutL = false, cutR = false;
		if (overflow)
			for (int gy = MAX(0, gy0); gy < MIN(H, gy0 + gh) && !(cutL && cutR); gy++)
				for (int k = 0; k < 3; k++) {
					const int xl = margin + k, xr = W - margin - 1 - k;
					cutL = cutL || (xl >= gx0 && xl < gx0 + gw && _remasterOwner[gy * W + xl] == c.actor);
					cutR = cutR || (xr >= gx0 && xr < gx0 + gw && _remasterOwner[gy * W + xr] == c.actor);
				}
		// Visibility per game pixel: 1 where the game shows this actor, or the cel barely covers the pixel (soft
		// outer edge); 0 where something else is in front or text. Sampled bilinearly so the cut is smooth.
		vis.resize(gw * gh);
		for (int gy = 0; gy < gh; gy++)
			for (int gx = 0; gx < gw; gx++) {
				const int sx2 = gx0 + gx, sy2 = gy0 + gy;
				float v = 0;
				if (sx2 >= 0 && sx2 < W && sy2 >= 0 && sy2 < H) {
					const int sg = sy2 * W + sx2;
					if (overflow && (sx2 < margin || sx2 >= W - margin)) {
						// Beyond the room: the sprite continues over the side art. Scenery that hides it at the room's
						// edge (walk-behind areas) is carried outwards along the row, so it isn't cut in two.
						const int ex = sx2 < margin ? margin : W - margin - 1;
						const int egx = ex - gx0;
						v = (sx2 < margin ? cutL : cutR) ? 1.0f : 0.0f;
						if (v > 0 && egx >= 0 && egx < gw && cov[gy * gw + egx] > 0.35f) {
							const int se = sy2 * W + ex;
							v = (_remasterOwner[se] == c.actor) ? 1.0f : 0.0f;
						}
					}
					else if (_remasterProtect[sg] != 3) {
						// Shown by the game as this actor; or barely covered by the cel (its soft outer edge) AND next to a
						// pixel the game shows as this actor. Without the second condition small sprites (the beach's sea
						// sparkles) bleed through scenery in front of them.
						bool shown = _remasterOwner[sg] == c.actor;
						if (!shown && cov[gy * gw + gx] <= 0.35f) {
							for (int dy = -1; dy <= 1 && !shown; dy++)
								for (int dx = -1; dx <= 1 && !shown; dx++) {
									const int yy = sy2 + dy, xx = sx2 + dx;
									shown = yy >= 0 && yy < H && xx >= 0 && xx < W && _remasterOwner[yy * W + xx] == c.actor;
								}
						}
						v = shown ? 1.0f : 0.0f;
					}
				}
				vis[gy * gw + gx] = v;
			}
		const float *visp = vis.data();
		remasterParallel(oh, [&](int ya, int yb) {
			for (int oy = ya; oy < yb; oy++) {
				const int by = oy0 + oy;
				if (by < 0 || by >= BH)
					continue;
				const float fy = CLIP((oy + 0.5f) / S - 0.5f, 0.0f, (float)gh - 1);
				const int vy0 = (int)fy, vy1 = MIN(vy0 + 1, gh - 1);
				const float ay = fy - vy0;
				for (int ox = 0; ox < ow; ox++) {
					const int bx = ox0 + ox;
					if (bx < 0 || bx >= BW)
						continue;
					const uint8 *pb = &rgba[(oy * ow + ox) * 4];
					if (pb[3] <= 1)
						continue;
					const float p[4] = {(float)pb[0], (float)pb[1], (float)pb[2], pb[3] / 255.0f};
					const float fx = CLIP((ox + 0.5f) / S - 0.5f, 0.0f, (float)gw - 1);
					const int vx0 = (int)fx, vx1 = MIN(vx0 + 1, gw - 1);
					const float ax = fx - vx0;
					const float v = (visp[vy0 * gw + vx0] * (1 - ax) + visp[vy0 * gw + vx1] * ax) * (1 - ay) +
						(visp[vy1 * gw + vx0] * (1 - ax) + visp[vy1 * gw + vx1] * ax) * ay;
					if (v <= 0.01f)
						continue;
					uint32 &o = _remasterBig[by * BW + bx];
					uint8 r, g, b;
					_remasterFormat.colorToRGB(o, r, g, b);
					const float a = p[3] * v, ia = 1.0f - a;
					if (egoAlpha && c.actor == egoId) {
						byte &ea = egoAlpha[(size_t)by * BW + bx];
						ea = MAX<byte>(ea, (byte)CLIP(a * 255.0f + 0.5f, 0.0f, 255.0f));
					}
					o = _remasterFormat.RGBToColor((uint8)CLIP(p[0] * v + r * ia, 0.0f, 255.0f), (uint8)CLIP(p[1] * v + g * ia, 0.0f, 255.0f),
						(uint8)CLIP(p[2] * v + b * ia, 0.0f, 255.0f));
				}
			}
		});
	}
	if (!stale.empty()) {
		Common::sort(stale.begin(), stale.end(), [](uint32 a, uint32 b) { return a > b; });
		for (uint i = 0; i < stale.size(); i++)
			_remasterCels.remove_at(stale[i]);
	}
	_remasterHDMs += _system->getMillis() - t0;
}

bool ScummEngine::remasterAICursor(const byte *src, int w, int h, byte trans, Common::Array<uint32> &out, uint32 &key) {
	// Small cursors (crosshairs, thin lines) keep the crisp Scale3x look; the AI blurs one-pixel detail.
	if (!_remasterAI || _remasterMode != kRemasterAI || _remasterScale != 3 || MAX(w, h) < 40 || w > 512 || h > 512 ||
	    (ConfMan.hasKey("remaster_ai_cursor") && !ConfMan.getBool("remaster_ai_cursor")))
		return false;
	RemasterCel c;
	c.actor = 0;
	c.seq = 0;
	c.w = w;
	c.h = h;
	c.mirror = false;
	c.px.resize(w * h);
	for (int i = 0; i < w * h; i++)
		c.px[i] = src[i] == trans ? -1 : (int16)src[i];
	const RemasterCelHD *hd = remasterCelHD(c);
	if (!hd || hd->w != w * 3 || hd->h != h * 3)
		return false;
	key = _remasterFormat.RGBToColor(255, 0, 255);
	out.resize(hd->w * hd->h);
	for (int i = 0; i < hd->w * hd->h; i++) {
		const uint32 q = hd->rgba[i];
		if ((q >> 24) < 128) {
			out[i] = key;
			continue;
		}
		uint8 r = q & 0xFF, g = (q >> 8) & 0xFF, b = (q >> 16) & 0xFF;
		if (r == 255 && g == 0 && b == 255)
			b = 254; // never the key colour
		out[i] = _remasterFormat.RGBToColor(r, g, b);
	}
	if (ConfMan.hasKey("remaster_dump_dir")) { // testing: keep the last AI cursor and its Scale3x equivalent
		Common::FSNode dir(Common::Path(ConfMan.get("remaster_dump_dir"), Common::Path::kNativeSeparator));
		Graphics::Surface a;
		a.init(hd->w, hd->h, hd->w * 4, out.data(), _remasterFormat);
		Common::DumpFile f;
		if (f.open(dir.getChild(Common::String::format("cursor_%dx%d_ai.png", w, h)).getPath()))
			Image::writePNG(f, a);
		Common::Array<byte> sc;
		remasterScaleCursor(src, w, h, 3, sc);
		Common::Array<uint32> rgb(sc.size());
		for (uint i = 0; i < sc.size(); i++)
			rgb[i] = sc[i] == trans ? key : _remasterLut[sc[i]];
		Graphics::Surface b;
		b.init(w * 3, h * 3, w * 12, rgb.data(), _remasterFormat);
		Common::DumpFile g;
		if (g.open(dir.getChild(Common::String::format("cursor_%dx%d_scale3x.png", w, h)).getPath()))
			Image::writePNG(g, b);
	}
	return true;
}

void ScummEngine::remasterPadWalk() {
	// Called once per frame. While the right stick is deflected, keep sending the ego actor a short walk target in
	// that direction (the engine's walk boxes keep it on walkable ground); stop when the stick is released.
	if (_game.version != 8 || VAR_EGO == 0xFF)
		return;
	Actor *a = derefActorSafe(VAR(VAR_EGO), "remasterPadWalk");
	const float x = _remasterStickX / 32767.0f, y = _remasterStickY / 32767.0f;
	const float mag = sqrtf(x * x + y * y);
	const bool canControl = a && a->isInCurrentRoom() && _userPut > 0 && !isSmushActive() && !_mainMenuIsActive;
	if (mag < 0.35f || !canControl) {
		if (_remasterStickWalking && a && canControl)
			a->stopActorMoving();
		_remasterStickWalking = false;
		return;
	}
	// Re-issue the walk only when the stick direction changed noticeably, the actor stopped, or the target is
	// getting close; re-issuing every frame would keep restarting the walk. When the actor cannot go straight
	// (obstacle / edge of the walkable area), try directions turned by up to 90 degrees so it slides along.
	float dx = x / mag, dy = y / mag;
	const Common::Point pos = a->getRealPos();
	const bool dirChanged = !_remasterStickWalking || (dx * _remasterStickDirX + dy * _remasterStickDirY) < 0.92f;
	const bool nearTarget = ABS(pos.x - _remasterStickTX) + ABS(pos.y - _remasterStickTY) < 60;
	if (!dirChanged && a->_moving && !nearTarget)
		return;
	if (dirChanged) {
		_remasterStickDirX = dx;
		_remasterStickDirY = dy;
		_remasterStickStuck = 0;
	} else if (!a->_moving && pos == _remasterStickLastPos) {
		_remasterStickStuck++;
	} else {
		_remasterStickStuck = 0;
	}
	_remasterStickLastPos = pos;
	static const int turns[] = {0, 30, -30, 55, -55, 80, -80};
	const int t = turns[_remasterStickStuck % 7];
	const float ang = t * 3.14159265f / 180.0f;
	const float rx = dx * cosf(ang) - dy * sinf(ang), ry = dx * sinf(ang) + dy * cosf(ang);
	const int step = _remasterStickStuck >= 7 ? 90 : 220;
	const int tx = CLIP<int>(pos.x + (int)(rx * step), 0, _roomWidth - 1);
	const int ty = CLIP<int>(pos.y + (int)(ry * step * 0.6f), 0, _roomHeight - 1);
	a->startWalkActor(tx, ty, -1);
	_remasterStickTX = tx;
	_remasterStickTY = ty;
	_remasterStickWalking = true;
}

void ScummEngine::remasterHotspotCycle(int dir) {
	// Move the cursor to the next/previous interactive object or character on screen (ordered left to right).
	if (_game.version != 8 || _userPut <= 0)
		return;
	struct Spot { int x, y; };
	Common::Array<Spot> spots;
	const VirtScreen &vs = _virtscr[kMainVirtScreen];
	const int viewW = _remasterLogicalWidth ? MIN<int>(_screenWidth, _roomWidth) : _screenWidth;
	auto add = [&](int rx, int ry) {
		const int sx = rx - vs.xstart, sy = ry - _screenTop;
		if (sx >= 4 && sx < viewW - 4 && sy >= 4 && sy < _screenHeight - 4)
			spots.push_back(Spot{sx, sy});
	};
	for (int i = 1; i < _numLocalObjects; i++) {
		const ObjectData &od = _objs[i];
		if (!od.obj_nr || od.width == 0 || od.height == 0 || getClass(od.obj_nr, kObjectClassUntouchable))
			continue;
		const byte *name = getObjOrActorName(od.obj_nr);
		if (!name || !name[0])
			continue;
		add(od.x_pos + od.width / 2, od.y_pos + od.height / 2);
	}
	const int ego = (VAR_EGO != 0xFF) ? VAR(VAR_EGO) : -1;
	for (int i = 1; i < _numActors; i++) {
		Actor *a = _actors[i];
		if (!a || i == ego || !a->isInCurrentRoom() || !a->_visible || getClass(i, kObjectClassUntouchable))
			continue;
		const byte *name = getObjOrActorName(i);
		if (!name || !name[0] || a->_bottom <= a->_top)
			continue;
		add(a->getRealPos().x, (a->_top + a->_bottom) / 2);
	}
	if (spots.empty())
		return;
	Common::sort(spots.begin(), spots.end(), [](const Spot &l, const Spot &r) { return l.x != r.x ? l.x < r.x : l.y < r.y; });
	const int mx = _mouse.x, my = _mouse.y;
	int pick = -1;
	if (dir > 0) {
		for (uint i = 0; i < spots.size() && pick < 0; i++)
			if (spots[i].x > mx + 2 || (spots[i].x >= mx - 2 && spots[i].y > my + 2))
				pick = i;
		if (pick < 0)
			pick = 0;
	} else {
		for (int i = (int)spots.size() - 1; i >= 0 && pick < 0; i--)
			if (spots[i].x < mx - 2 || (spots[i].x <= mx + 2 && spots[i].y < my - 2))
				pick = i;
		if (pick < 0)
			pick = spots.size() - 1;
	}
	debug(1, "remaster: hotspot %d/%d -> %d,%d", pick + 1, spots.size(), spots[pick].x, spots[pick].y);
	remasterWarpMouse(spots[pick].x, spots[pick].y);
	_mouse.x = spots[pick].x;
	_mouse.y = spots[pick].y;
}

void ScummEngine::remasterCycleMode() {
	int m = (_remasterMode + 1) % kRemasterModeCount;
	if (m == kRemasterClassic)   // opt-in only (remaster_display_mode=6): F10 cycles the established modes
		m = kRemasterAI;
	if (m == kRemasterAI && !_remasterAI)
		m = kRemasterOriginal;
	remasterSetMode(m, true);
}

void ScummEngine::remasterToggleCrt() {
	_remasterCrt = !_remasterCrt;
	ConfMan.setBool("remaster_crt", _remasterCrt);
	ConfMan.flushToDisk();
	_remasterFrameDirty = true;
	_system->displayMessageOnOSD(Common::U32String(_remasterCrt ? "CRT scanlines: on (Ctrl+F10)" : "CRT scanlines: off (Ctrl+F10)"));
}
void ScummEngine::remasterRetro() {
	// Calculated "demaster" looks. The 640x480 frame is reduced to 320x240 (2x2 average), recoloured per mode
	// and scaled up with hard pixel edges. All modes are exact and stable from frame to frame.
	// The 2x2 grid is anchored to the room, not the screen: it moves with the camera, so scrolling shows the same
	// low-res pixels moved along instead of re-sampling the whole background on every odd-pixel step (shimmer).
	const bool centred = _remasterLogicalWidth && remasterMargin() > 0;
	const int phase = centred ? 0 : (_virtscr[kMainVirtScreen].xstart & 1);
	const int roomCell = centred ? 0 : (_virtscr[kMainVirtScreen].xstart - phase) / 2;   // anchors the EGA dither too
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, LW = (W + phase + 1) / 2, LH = H / 2, BW = W * S, BH = H * S;
	const Graphics::PixelFormat &fmt = _remasterFormat;
	static Common::Array<int> low;      // LW*LH*3, 0..255
	static Common::Array<uint32> lowCol; // LW*LH, screen format
	low.resize(LW * LH * 3);
	lowCol.resize(LW * LH);
	for (int ly = 0; ly < LH; ly++) {
		for (int lx = 0; lx < LW; lx++) {
			int sum[3] = {0, 0, 0}, n = 0;
			for (int dy = 0; dy < 2; dy++)
				for (int dx = 0; dx < 2; dx++) {
					const int x = lx * 2 + dx - phase;
					if (x < 0 || x >= W)
						continue;
					uint8 r, g, b;
					fmt.colorToRGB(_remasterRgb[(ly * 2 + dy) * W + x], r, g, b);
					sum[0] += r; sum[1] += g; sum[2] += b;
					n++;
				}
			for (int ch = 0; ch < 3; ch++)
				low[(ly * LW + lx) * 3 + ch] = sum[ch] / MAX(1, n);
		}
	}

	auto nearest = [](const byte *pal, int n, int r, int g, int b) -> int {
		return remasterNearestColor(pal, n, r, g, b);
	};
	if (!_remasterVgaMap) {
		_remasterVgaMap = new RemasterColorMap();
		_remasterAmigaMap = new RemasterColorMap();
	}

	if (_remasterPalDirty) {
		// Exact nearest colours (a colour that is in the palette stays itself), searched once per colour and cached
		// until the palette changes; the old 16x16x16 table changed about 60% of a typical frame's blocks.
		_remasterPalDirty = false;
		_remasterVgaMap->reset(_remasterPal, 256);
		// Amiga: 32-colour palette by median cut over the colours of the current frame, weighted by use. Subtitles
		// count as the picture under them, so a line of text does not change the palette.
		struct Box { int lo, hi; };
		Common::Array<uint32> hist;
		hist.resize(256);
		const bool textUnder = _remasterProtectFull && _remasterTextUnder.size() == (uint)(W * H);
		for (int i = 0; i < W * H; i++)
			hist[textUnder && _remasterProtectFull[i] == 3 ? _remasterTextUnder[i] : _remasterScreen8[i]]++;
		Common::Array<int> idx;
		for (int i = 0; i < 256; i++)
			if (hist[i])
				idx.push_back(i);
		Common::Array<Box> boxes;
		boxes.push_back(Box{0, (int)idx.size()});
		while (boxes.size() < 32) {
			int bestBox = -1, bestRange = 0, bestCh = 0;
			for (uint bi = 0; bi < boxes.size(); bi++) {
				if (boxes[bi].hi - boxes[bi].lo < 2)
					continue;
				for (int ch = 0; ch < 3; ch++) {
					int mn = 255, mx = 0;
					for (int k = boxes[bi].lo; k < boxes[bi].hi; k++) {
						const int v = _remasterPal[idx[k] * 3 + ch];
						mn = MIN(mn, v); mx = MAX(mx, v);
					}
					if (mx - mn > bestRange) { bestRange = mx - mn; bestBox = bi; bestCh = ch; }
				}
			}
			if (bestBox < 0)
				break;
			Box &bx = boxes[bestBox];
			for (int a = bx.lo; a < bx.hi; a++)          // sort the box by the chosen channel
				for (int b = a + 1; b < bx.hi; b++)
					if (_remasterPal[idx[b] * 3 + bestCh] < _remasterPal[idx[a] * 3 + bestCh]) { int t = idx[a]; idx[a] = idx[b]; idx[b] = t; }
			uint32 total = 0, acc = 0;
			for (int k = bx.lo; k < bx.hi; k++) total += hist[idx[k]];
			int split = bx.lo + 1;
			for (int k = bx.lo; k < bx.hi - 1; k++) { acc += hist[idx[k]]; if (acc * 2 >= total) { split = k + 1; break; } }
			Box nb = {split, bx.hi};
			bx.hi = split;
			boxes.push_back(nb);
		}
		memset(_remasterAmigaPal, 0, sizeof(_remasterAmigaPal));
		for (uint bi = 0; bi < boxes.size() && bi < 32; bi++) {
			uint64 sum[3] = {0, 0, 0}, n = 0;
			for (int k = boxes[bi].lo; k < boxes[bi].hi; k++) {
				for (int ch = 0; ch < 3; ch++) sum[ch] += (uint64)_remasterPal[idx[k] * 3 + ch] * hist[idx[k]];
				n += hist[idx[k]];
			}
			for (int ch = 0; ch < 3; ch++)
				_remasterAmigaPal[bi * 3 + ch] = n ? (byte)(sum[ch] / n) : 0;
		}
		_remasterAmigaMap->reset(_remasterAmigaPal, 32);
	}

	if (_remasterMode == kRemasterClassic) {
		remasterClassic();
		return;
	}
	static const byte ega[16 * 3] = {
		0, 0, 0,  0, 0, 170,  0, 170, 0,  0, 170, 170,  170, 0, 0,  170, 0, 170,  170, 85, 0,  170, 170, 170,
		85, 85, 85,  85, 85, 255,  85, 255, 85,  85, 255, 255,  255, 85, 85,  255, 85, 255,  255, 255, 85,  255, 255, 255
	};
	static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

	for (int ly = 0; ly < LH; ly++) {
		for (int lx = 0; lx < LW; lx++) {
			int *p = &low[(ly * LW + lx) * 3];
			int r = p[0], g = p[1], b = p[2];
			switch (_remasterMode) {
			case kRemasterVGA: {
				const byte *q = &_remasterPal[_remasterVgaMap->map(r, g, b) * 3];
				r = q[0]; g = q[1]; b = q[2];
				break;
			}
			case kRemasterEGA: {
				// EGA art used bold colours: lift brightness and saturation first, then a gentle ordered dither.
				const int lum = (r * 77 + g * 150 + b * 29) >> 8;
				const int er = lum + (r - lum) * 8 / 5 + 18, eg = lum + (g - lum) * 8 / 5 + 18, eb = lum + (b - lum) * 8 / 5 + 18;
				const int d = (bayer[ly & 3][(lx + roomCell) & 3] - 8) * 3;   // ordered dither, +-24 (room-anchored)
				const int k = nearest(ega, 16, CLIP(er + d, 0, 255), CLIP(eg + d, 0, 255), CLIP(eb + d, 0, 255));
				r = ega[k * 3]; g = ega[k * 3 + 1]; b = ega[k * 3 + 2];
				break;
			}
			case kRemasterAmiga: {
				const byte *q = &_remasterAmigaPal[_remasterAmigaMap->map(r, g, b) * 3];
				r = q[0]; g = q[1]; b = q[2];
				break;
			}
			default: { // modern pixel art: richer colour and a touch of local contrast
				const int lum = (r * 77 + g * 150 + b * 29) >> 8;
				r = lum + (r - lum) * 5 / 4; g = lum + (g - lum) * 5 / 4; b = lum + (b - lum) * 5 / 4;
				if (lx > 0 && lx < LW - 1 && ly > 0 && ly < LH - 1) {
					int avg[3] = {0, 0, 0};
					for (int ch = 0; ch < 3; ch++)
						avg[ch] = (low[((ly - 1) * LW + lx) * 3 + ch] + low[((ly + 1) * LW + lx) * 3 + ch] +
						           low[(ly * LW + lx - 1) * 3 + ch] + low[(ly * LW + lx + 1) * 3 + ch]) / 4;
					r += (p[0] - avg[0]) / 3; g += (p[1] - avg[1]) / 3; b += (p[2] - avg[2]) / 3;
				}
				break;
			}
			}
			lowCol[ly * LW + lx] = fmt.RGBToColor(CLIP(r, 0, 255), CLIP(g, 0, 255), CLIP(b, 0, 255));
		}
	}
	if (_remasterMode == kRemasterAmiga) {
		// Amiga ports on a TV/monitor looked a little soft: a light blend with the horizontal neighbours.
		for (int ly = 0; ly < LH; ly++) {
			uint32 prev = lowCol[ly * LW];
			for (int lx = 0; lx < LW; lx++) {
				const uint32 cur = lowCol[ly * LW + lx], next = lowCol[ly * LW + MIN(lx + 1, LW - 1)];
				uint8 r0, g0, b0, r1, g1, b1, r2, g2, b2;
				fmt.colorToRGB(prev, r0, g0, b0); fmt.colorToRGB(cur, r1, g1, b1); fmt.colorToRGB(next, r2, g2, b2);
				lowCol[ly * LW + lx] = fmt.RGBToColor((r0 + r1 * 6 + r2) / 8, (g0 + g1 * 6 + g2) / 8, (b0 + b1 * 6 + b2) / 8);
				prev = cur;
			}
		}
	}
	// Upscale with hard edges. Text is not part of this frame: remasterTextOverlay lays it over the finished picture at
	// full resolution, so subtitles stay readable without a reduced copy around them; in modern mode verbs/UI stay at
	// full resolution too (like Thimbleweed Park).
	const bool hiResUi = _remasterMode == kRemasterModern;
	for (int by = 0; by < BH; by++) {
		const uint32 *src = &lowCol[(by / (2 * S)) * LW];
		const int sy = by / S;
		uint32 *dst = _remasterBig + by * BW;
		for (int bx = 0; bx < BW; bx++) {
			const int sx = bx / S;
			const byte pr = _remasterProtect ? _remasterProtect[sy * W + sx] : 0;
			dst[bx] = (hiResUi && pr == 1) ? _remasterRgb[sy * W + sx] : src[(sx + phase) / 2];
		}
	}
}

void ScummEngine::remasterClassic() {
	// Classic demaster (experimental): an MI2-inspired 320x200 artwork raster (424x200 in widescreen) shown with 4:3
	// geometry - not an emulation of a real port. Built from the text-free frame and its pixel kinds:
	//  - columns are 2:1 and anchored to the room (the grid moves with the camera), so scrolling does not shimmer;
	//    rows are 480 -> 200 (2 or 3 game rows each, shown 7 or 8 output rows tall at x3);
	//  - each low-res pixel is either character or scene, whichever covers most of it, averaged from those pixels only:
	//    silhouettes stay clean instead of blending into the background;
	//  - colours: the exact nearest colour of the room's own 256-colour palette (stable ramps, no dithering);
	//  - readable subtitles are laid over the result at full resolution (remasterTextOverlay).
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S, BH = H * S;
	const int LH = remasterClassicRows();
	const Graphics::PixelFormat &fmt = _remasterFormat;
	const bool centred = _remasterLogicalWidth && remasterMargin() > 0;
	const int phase = centred ? 0 : (_virtscr[kMainVirtScreen].xstart & 1);
	const int LW = (W + phase + 1) / 2;
	static Common::Array<uint32> cell;
	static Common::Array<int> rowOf;
	cell.resize(LW * LH);
	rowOf.resize(H + 1);
	for (int ly = 0; ly <= LH; ly++)   // game rows of low-res row ly: [rowOf[ly], rowOf[ly + 1])
		rowOf[ly] = ly * H / LH;
	for (int ly = 0; ly < LH; ly++) {
		for (int lx = 0; lx < LW; lx++) {
			int sum[2][3] = {{0, 0, 0}, {0, 0, 0}}, cnt[2] = {0, 0};
			for (int y = rowOf[ly]; y < rowOf[ly + 1]; y++)
				for (int x = 2 * lx - phase; x < 2 * lx - phase + 2; x++) {
					if (x < 0 || x >= W)
						continue;
					const int k = _remasterProtect && _remasterProtect[y * W + x] == 2 ? 1 : 0;   // 1: character
					uint8 r, g, b;
					fmt.colorToRGB(_remasterRgb[y * W + x], r, g, b);
					sum[k][0] += r; sum[k][1] += g; sum[k][2] += b;
					cnt[k]++;
				}
			const int k = cnt[1] * 2 >= cnt[0] + cnt[1] && cnt[1] ? 1 : 0;
			const int n = MAX(1, cnt[k]);
			const byte *q = &_remasterPal[_remasterVgaMap->map(sum[k][0] / n, sum[k][1] / n, sum[k][2] / n) * 3];
			cell[ly * LW + lx] = fmt.RGBToColor(q[0], q[1], q[2]);
		}
	}
	for (int by = 0; by < BH; by++) {
		const int ly = (int)((int64)by * LH / BH);   // even 4:3 rows on the output
		const uint32 *src = &cell[ly * LW];
		uint32 *dst = _remasterBig + by * BW;
		for (int bx = 0; bx < BW; bx++)
			dst[bx] = src[(bx / S + phase) / 2];
	}
}

void ScummEngine::remasterCrtOverlay() {
	// Scanlines: darken the last output row(s) of every source line (320 lines in retro modes, 480 otherwise).
	const int S = _remasterScale, BW = _screenWidth * S, BH = _screenHeight * S;
	const int period = _remasterMode >= kRemasterVGA ? 2 * S : S;
	if (_remasterMode == kRemasterClassic) {
		// Classic: darken the last output row of each of the 200 low-res rows (they are 7 or 8 rows tall at x3)
		const int LH = remasterClassicRows();
		const uint32 aMask0 = _remasterFormat.aBits() ? ((uint32)(0xFF >> (8 - _remasterFormat.aBits())) << _remasterFormat.aShift) : 0;
		for (int by = 0; by < BH; by++) {
			if (by + 1 < BH && (int64)by * LH / BH == (int64)(by + 1) * LH / BH)
				continue;
			uint32 *row = _remasterBig + by * BW;
			for (int bx = 0; bx < BW; bx++) {
				const uint32 c = row[bx];
				const uint32 rb = (((c & 0x00FF00FF) * 150) >> 8) & 0x00FF00FF;
				const uint32 ga = ((((c >> 8) & 0x00FF00FF) * 150) & 0xFF00FF00);
				row[bx] = ((rb | ga) & ~aMask0) | (c & aMask0);
			}
		}
		return;
	}
	const uint32 aMask = _remasterFormat.aBits() ? ((uint32)(0xFF >> (8 - _remasterFormat.aBits())) << _remasterFormat.aShift) : 0;
	for (int by = 0; by < BH; by++) {
		const int phase = by % period;
		const int k = phase == period - 1 ? 150 : (period >= 4 && phase == period - 2 ? 215 : 256);
		if (k == 256)
			continue;
		uint32 *row = _remasterBig + by * BW;
		for (int bx = 0; bx < BW; bx++) {
			const uint32 c = row[bx];
			const uint32 rb = (((c & 0x00FF00FF) * k) >> 8) & 0x00FF00FF;
			const uint32 ga = ((((c >> 8) & 0x00FF00FF) * k) & 0xFF00FF00);
			row[bx] = ((rb | ga) & ~aMask) | (c & aMask);
		}
	}
}
void ScummEngine::remasterShowHelpOnce() {
	// First launch only: tell players about the switches (shown at about 4 s and 12 s of presented frames).
	if (ConfMan.hasKey("remaster_help_shown") || !_remasterEnabled)
		return;
	++_remasterHelpFrames;
	if (_remasterHelpFrames == 240 || _remasterHelpFrames == 720)
		_system->displayMessageOnOSD(Common::U32String("F10/RB display mode - F11/RT AI - F12/LT side panels - Ctrl+F5/Start options - Ctrl+F12/R3 report"));
	if (_remasterHelpFrames == 720) {
		ConfMan.setBool("remaster_help_shown", true);
		ConfMan.flushToDisk();
	}
}

void ScummEngine::remasterSmoothNewFrame() {
	const uint32 now = _system->getMillis();
	const int xs = _virtscr[kMainVirtScreen].xstart, S = _remasterScale;
	const size_t n = (size_t)_remasterOutW * _remasterOutH;
	// Only in AI HD with HD characters: keeping the followed character steady needs their coverage mask (egoAlpha);
	// without it the character was cut up while gliding. Elsewhere the game's own steps are shown.
	_remasterSmOn = ConfMan.hasKey("remaster_smooth_scroll") && ConfMan.getBool("remaster_smooth_scroll") &&
		_remasterMode == kRemasterAI && _remasterHDActors && _remasterGovLevel < 3;
	_remasterSmDebug = ConfMan.hasKey("remaster_smooth_debug") && ConfMan.getBool("remaster_smooth_debug");
	if (!_remasterSmOn) {   // off: no copies at all
		_remasterSmActive = false;
		_remasterSmShownValid = false;
		if (!_remasterSmCur.empty()) {
			_remasterSmCur.clear();
			_remasterSmPrev.clear();
			_remasterSmOut.clear();
		}
		_remasterSmXs = xs;
		_remasterSmRoom = _currentRoom;
		return;
	}
	const int dxg = xs - _remasterSmXs;
	if (_remasterSmRoom != _currentRoom || _remasterVideoShown)
		_remasterSmShownValid = false;
	const bool ok = _remasterSmOn && _remasterOut && _remasterSmRoom == _currentRoom && !_remasterVideoShown &&
		_remasterSmCur.size() == n && dxg != 0 && ABS(dxg) <= 24 && _remasterSide == 0;
	// keep this frame and the one before
	_remasterSmPrev.swap(_remasterSmCur);
	_remasterSmCur.resize(n);
	memcpy(_remasterSmCur.data(), _remasterOut, n * 4);
	const int target = xs * S;   // the new frame's camera position (output pixels)
	// The camera stopped but the picture has not caught up yet (the last glide was unfinished): glide the rest of
	// the way instead of jumping there.
	const int behind = _remasterSmShownValid ? target - _remasterSmShownCam : 0;
	const bool finish = !ok && dxg == 0 && behind != 0 && ABS(behind) <= 48 * S && _remasterSmOn && _remasterOut &&
		_remasterSmRoom == _currentRoom && !_remasterVideoShown && _remasterSmPrev.size() == n && _remasterSide == 0;
	if (finish) {
		_remasterSmActive = true;
		_remasterSmDx = 0;   // the previous frame showed the same camera position
		_remasterSmT0 = now;
		_remasterSmDur = CLIP<uint32>(_remasterSmMoveGap / 2, 20, 80);
		_remasterSmStart = behind;
		_remasterSmOut.resize(n);
		_remasterSmLastOffset = INT_MIN;
		_remasterSmFixOn = false;
		_remasterSmTarget = target;
		_remasterSmLastFrame = now;
		_remasterSmXs = xs;
		_remasterSmRoom = _currentRoom;
		return;
	}
	_remasterSmActive = ok;
	if (ok) {
		_remasterSmDx = dxg * S;
		_remasterSmT0 = now;
		// glide over the game's own frame period, starting from where the picture is now (an unfinished glide
		// continues instead of jumping)
		// the time between camera moves (an average while scrolling), which is what the glide must fill
		const uint32 gap = now - _remasterSmLastMove;
		if (gap >= 30 && gap <= 200)
			_remasterSmMoveGap = (_remasterSmMoveGap * 3 + gap) / 4;
		_remasterSmLastMove = now;
		_remasterSmDur = CLIP<uint32>(_remasterSmMoveGap, 30, 150);
		int start = _remasterSmShownValid ? target - _remasterSmShownCam : _remasterSmDx;
		const int lim = ABS(_remasterSmDx) * 2;
		_remasterSmStart = CLIP(start, -lim, lim);
		_remasterSmOut.resize(n);
		_remasterSmLastOffset = INT_MIN;
		_remasterSmShownCam = target - _remasterSmStart;
		remasterSmoothBuildLayers();
	} else {
		_remasterSmShownCam = target;
	}
	_remasterSmShownValid = true;
	_remasterSmTarget = target;
	_remasterSmLastFrame = now;
	_remasterSmXs = xs;
	_remasterSmRoom = _currentRoom;
}

void ScummEngine::remasterSmoothCheckpoint(const char *where) {
	// While the next frame is being composed (tens of ms while scrolling), keep the glide of the frame on screen
	// going: only finished pictures are shown, never the one being composed.
	if (_remasterSmActive && remasterSmoothPresent()) {
		remasterSmoothCapture(_remasterSmShown);
		_system->updateScreen();
		remasterSmoothGap(where);
	}
}

void ScummEngine::remasterSmoothGap(const char *where) {
	// remaster_smooth_debug: which stretch of work held the picture still for long while gliding
	const uint32 now = _system->getMillis();
	if (_remasterSmDebug && _remasterSmLastShow && now - _remasterSmLastShow > 20)
		warning("remaster: smooth gap %u ms before %s (since %s)", now - _remasterSmLastShow, where, _remasterSmLastWhere);
	_remasterSmLastShow = now;
	_remasterSmLastWhere = where;
}

void ScummEngine::remasterSmoothBuildLayers() {
	// Only while the camera follows the character (its screen position barely changes between frames); when the
	// camera pans past a standing character, everything glides together.
	_remasterSmFixOn = false;
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, OW = _remasterOutW;
	const int egoId = VAR_EGO != 0xFF ? VAR(VAR_EGO) : -1;
	// the character's position on screen (its room position minus the camera; the sprite's box changes with every
	// pose, so it is no use here)
	int egoLeft = INT_MIN;
	if (egoId > 0 && egoId < _numActors) {
		Actor *ego = derefActorSafe(egoId, "remasterSmoothBuildLayers");
		if (ego && ego->isInCurrentRoom())
			egoLeft = ego->getRealPos().x - _virtscr[kMainVirtScreen].xstart;
	}
	const int dxg = _remasterSmDx / S;
	const bool follows = egoLeft != INT_MIN && _remasterSmEgoLeft != INT_MIN && ABS(egoLeft - _remasterSmEgoLeft) * 2 < ABS(dxg);
	_remasterSmEgoLeft = egoLeft;
	if (!_remasterProtect || !_remasterOwner)
		return;
	// game pixels held still: the followed character (if it follows) and text, grown by one pixel
	Common::Array<byte> keep(W * H, 0);
	bool any = false;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			const int v = y * W + x;
			if (_remasterProtect[v] == 3 || (follows && egoId > 0 && _remasterOwner[v] == egoId)) {
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++) {
						const int yy = y + dy, xx = x + dx;
						if (yy >= 0 && yy < H && xx >= 0 && xx < W)
							keep[yy * W + xx] = 1;
					}
				any = true;
			}
		}
	if (!any)
		return;
	// clean frame: those pixels from the HD background (else as they are)
	const size_t n = (size_t)OW * _remasterOutH;
	_remasterSmClean.resize(n);
	memcpy(_remasterSmClean.data(), _remasterSmCur.data(), n * 4);
	_remasterSmFix.resize(n);
	memset(_remasterSmFix.data(), 0, n);
	const bool egoHD = follows && _remasterSmEgoAlpha.size() == n;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			if (!keep[y * W + x])
				continue;
			int ti, tx, ty;
			const bool plate = remasterBgPlate(y, x, ti, tx, ty);
			const bool text = _remasterProtect[y * W + x] == 3;
			const bool ego = follows && _remasterOwner[y * W + x] == egoId;
			for (int yy = 0; yy < S; yy++) {
				const size_t o = (size_t)(y * S + yy) * OW + x * S;
				if (plate) {
					const RemasterBgTile &t = _remasterBgTiles[ti];
					memcpy(&_remasterSmClean[o], &t.hd[(size_t)(ty * S + yy) * (t.hw * S) + tx * S], S * 4);
				}
				for (int xx = 0; xx < S; xx++) {
					// what stays still: text fully; the character by its HD coverage, or fully without HD characters
					byte a = 0;
					if (text)
						a = 255;
					if (ego)
						a = MAX<byte>(a, egoHD ? _remasterSmEgoAlpha[o + xx] : 255);
					if (!plate && a == 0)
						a = egoHD ? _remasterSmEgoAlpha[o + xx] : 0;
					_remasterSmFix[o + xx] = a;
				}
			}
		}
	_remasterSmFixOn = true;
}

void ScummEngine::remasterSmoothCompose(int offset) {
	// Show the current frame as if the camera were offset output pixels behind its new position; the strip that
	// comes into view is the previous frame's (it showed the camera's last position).
	const int OW = _remasterOutW, OH = _remasterOutH, dx = _remasterSmDx;
	const uint32 *cur = _remasterSmFixOn ? _remasterSmClean.data() : _remasterSmCur.data(), *prev = _remasterSmPrev.data();
	const uint32 *still = _remasterSmCur.data();
	const byte *fix = _remasterSmFixOn ? _remasterSmFix.data() : nullptr;
	uint32 *out = _remasterSmOut.data();
	remasterParallel(OH, [&](int ya, int yb) {
		for (int y = ya; y < yb; y++) {
			const uint32 *c = cur + (size_t)y * OW, *p = prev + (size_t)y * OW;
			uint32 *o = out + (size_t)y * OW;
			// o[x] = c[x - offset] where that exists, else p[x + dx - offset]
			const int a = MAX(0, offset), b = MIN(OW, OW + offset);
			if (b > a)
				memcpy(o + a, c + a - offset, (size_t)(b - a) * 4);
			for (int x = 0; x < a; x++) {
				const int px = x + dx - offset;
				o[x] = (px >= 0 && px < OW) ? p[px] : c[0];
			}
			for (int x = b; x < OW; x++) {
				const int px = x + dx - offset;
				o[x] = (px >= 0 && px < OW) ? p[px] : c[OW - 1];
			}
			if (fix) {
				// the followed character and text stay where they are
				const byte *f = fix + (size_t)y * OW;
				const uint32 *st = still + (size_t)y * OW;
				for (int x = 0; x < OW; x++) {
					const int a = f[x];
					if (!a)
						continue;
					if (a == 255) {
						o[x] = st[x];
						continue;
					}
					const byte *sb = (const byte *)&st[x];
					byte *ob = (byte *)&o[x];
					for (int k = 0; k < 3; k++)
						ob[k] = (byte)((sb[k] * a + ob[k] * (255 - a) + 127) / 255);
				}
			}
		}
	});
}

bool ScummEngine::remasterSmoothPresent() {
	if (!_remasterSmActive)
		return false;
	const uint32 el = _system->getMillis() - _remasterSmT0;
	if (el >= _remasterSmDur) {
		_remasterSmActive = false;
		_remasterSmShownCam = _remasterSmTarget;
		_system->copyRectToScreen(_remasterSmCur.data(), _remasterOutW * 4, 0, 0, _remasterOutW, _remasterOutH);
		_remasterSmShown = _remasterSmCur.data();
		return true;
	}
	const int offset = (int)((int64)_remasterSmStart * (int64)(_remasterSmDur - el) / (int64)_remasterSmDur);
	_remasterSmShownCam = _remasterSmTarget - offset;
	if (offset != _remasterSmLastOffset) {
		_remasterSmLastOffset = offset;
		remasterSmoothCompose(offset);
		_system->copyRectToScreen(_remasterSmOut.data(), _remasterOutW * 4, 0, 0, _remasterOutW, _remasterOutH);
		_remasterSmShown = _remasterSmOut.data();
	}
	return true;
}

void ScummEngine::remasterSmoothCapture(const uint32 *frame) {
	// Test aid (remaster_smooth_capture=<file>): the presented picture at 60 Hz, centre 640x360, raw RGBA.
	if (!ConfMan.hasKey("remaster_smooth_capture") || !frame)
		return;
	const uint32 now = _system->getMillis();
	if (now > _remasterSmCapUntil)
		return;   // started by the test command pad.request "<n> capture <seconds>"
	// a steady 60 Hz timeline: the picture on screen at each tick (repeated when nothing new was shown)
	if (!_remasterSmCapLast) {
		_remasterSmCapLast = now;
		_remasterSmCapFrames = 0;
	}
	const uint32 due = (now - _remasterSmCapLast) * 60 / 1000 + 1;
	if (!_remasterSmCap) {
		_remasterSmCap = new Common::DumpFile();
		if (!_remasterSmCap->open(Common::Path(ConfMan.get("remaster_smooth_capture"), Common::Path::kNativeSeparator))) {
			delete _remasterSmCap;
			_remasterSmCap = nullptr;
			ConfMan.removeKey("remaster_smooth_capture", ConfMan.getActiveDomainName());
			return;
		}
	}
	const int cw = MIN(640, _remasterOutW), ch = MIN(360, _remasterOutH);
	const int x0 = (_remasterOutW - cw) / 2, y0 = (_remasterOutH - ch) / 2;
	for (; _remasterSmCapFrames < due; _remasterSmCapFrames++)
		for (int y = 0; y < ch; y++)
			_remasterSmCap->write(frame + (size_t)(y0 + y) * _remasterOutW + x0, cw * 4);
}

void ScummEngine::remasterUpdateScreen() {
	if (_remasterEdgeAnimating)
		_remasterFrameDirty = true;   // edge guard fade in progress
	if (_remasterEnabled && remasterDeferred() && !_remasterFrameDirty && remasterSmoothPresent()) {
		remasterSmoothCapture(_remasterSmShown);
		_system->updateScreen();
		remasterSmoothGap("wait loop");
		return;
	}
	if (_remasterEnabled && remasterDeferred() && _remasterFrameDirty && _remasterSmActive)
		remasterSmoothGap("new frame (game logic and drawing before it)");
	if (_remasterEnabled && remasterDeferred() && _remasterFrameDirty)
		remasterUpscaleFrame();
	if (_remasterEnabled)
		remasterSmoothCapture(_remasterSmShown);
	remasterShowHelpOnce();
	_system->updateScreen();
}
void ScummEngine::remasterSetPalette(const byte *colors, uint first, uint num) {
	if (first >= 256)
		return;
	num = MIN<uint>(num, 256 - first);
	{
		const uint32 now = _system->getMillis();
		for (uint i = first; i < first + num; i++) {
			if (memcmp(_remasterPal + i * 3, colors + (i - first) * 3, 3) == 0)
				continue;
			// counts changes within ~2 s windows; a colour that keeps changing is cycling
			if (now - _remasterPalChangeT[i] > 2000)
				_remasterPalChangeN[i] = 0;
			if (_remasterPalChangeN[i] < 255)
				_remasterPalChangeN[i]++;
			_remasterPalChangeT[i] = now;
		}
	}
	if (memcmp(_remasterPal + first * 3, colors, num * 3) != 0)
		_remasterPalDirty = true;   // retro colour maps are rebuilt only when a colour really changed
	memcpy(_remasterPal + first * 3, colors, num * 3);
	for (uint i = first; i < first + num; i++)
		_remasterLut[i] = _remasterFormat.RGBToColor(_remasterPal[i * 3], _remasterPal[i * 3 + 1], _remasterPal[i * 3 + 2]);
	CursorMan.replaceCursorPalette(_remasterPal, 0, 256);
	if (_remasterCursorRGB)
		updateCursor();
	if (!_remasterEnabled)
		return;
	// The screen keeps palette indices only in the mirror; show everything again with the new colours.
	VirtScreen *vs = &_virtscr[kMainVirtScreen];
	if (remasterRoomActive() && vs->getPixels(0, 0) && vs->h >= _screenHeight)
		remasterBlit(vs->getPixels(0, _screenTop), vs->pitch, 0, vs->topline, _screenWidth, _screenHeight - vs->topline, vs);
	else
		remasterBlit(_remasterScreen8, _screenWidth, 0, 0, _screenWidth, _screenHeight, nullptr);
}

bool ScummEngine::remasterRoomActive() {
	if (!ConfMan.hasKey("remaster_path"))
		return false;
	if (_remasterBgRoom == _currentRoom)
		return _remasterBgValid;
	_remasterBgRoom = _currentRoom;
	_remasterBgValid = false;
	_remasterBg.free();

	Common::FSNode root(Common::Path(ConfMan.get("remaster_path"), Common::Path::kNativeSeparator));
	Common::FSNode file = root.getChild("rooms").getChild(Common::String::format("%04d", _currentRoom)).getChild("background.png");
	if (!file.exists())
		return false;
	Common::SeekableReadStream *stream = file.createReadStream();
	Image::PNGDecoder png;
	const bool ok = stream && png.loadStream(*stream);
	delete stream;
	const Graphics::Surface *src = ok ? png.getSurface() : nullptr;
	if (!src || (src->format.bytesPerPixel != 3 && src->format.bytesPerPixel != 4)) {
		warning("remaster: cannot use %s (needs an RGB or RGBA PNG)", file.getPath().toString().c_str());
		return false;
	}
	_remasterBg.create(src->w, src->h, _remasterFormat);
	for (int y = 0; y < src->h; y++) {
		const byte *s = (const byte *)src->getBasePtr(0, y);
		uint32 *d = (uint32 *)_remasterBg.getBasePtr(0, y);
		for (int x = 0; x < src->w; x++, s += src->format.bytesPerPixel) {
			const uint32 c = src->format.bytesPerPixel == 4 ? READ_UINT32(s) : READ_UINT24(s);
			uint8 a, r, g, b;
			src->format.colorToARGB(c, a, r, g, b);
			d[x] = _remasterFormat.RGBToColor(r, g, b);
		}
	}
	_remasterBgValid = true;
	warning("remaster: room %d uses %s (%dx%d, scale %.3f)", _currentRoom, file.getPath().toString().c_str(),
		_remasterBg.w, _remasterBg.h, (double)_remasterBg.w / MAX<int>(1, _roomWidth));
	return true;
}

void ScummEngine::remasterCaptureRoomStrips(int firstCol, int numCols) {
	// The main virtual screen is a sliding window only one strip wider than the screen: room pixel (x, y)
	// lives at buffer offset y * pitch + x, so columns past the pitch continue into the next row's memory.
	// Only one window is valid at a time and strips are redrawn whenever they scroll into view, so keying
	// the reference by buffer offset (exactly how drawBitmap addresses it) stays consistent.
	VirtScreen &vs = _virtscr[kMainVirtScreen];
	const int size = vs.pitch * vs.h;
	if (size <= 0)
		return;
	if (_remasterRoomBgSize != size) {
		_remasterRoomBg = (byte *)realloc(_remasterRoomBg, size);
		_remasterRoomBgValid = (byte *)realloc(_remasterRoomBgValid, size);
		_remasterRoomBgSize = size;
		_remasterRoomBgRoom = -1;
	}
	if (!_remasterRoomBg || !_remasterRoomBgValid)
		return;
	if (_remasterRoomBgRoom != _currentRoom) {
		memset(_remasterRoomBgValid, 0, size);
		_remasterRoomBgRoom = _currentRoom;
	}
	const byte *base = (const byte *)vs.getBasePtr(0, 0);
	for (int y = 0; y < vs.h; y++) {
		const int o0 = MAX(0, y * vs.pitch + firstCol);
		const int o1 = MIN(size, y * vs.pitch + firstCol + numCols);
		if (o1 <= o0)
			continue;
		memcpy(_remasterRoomBg + o0, base + o0, o1 - o0);
		memset(_remasterRoomBgValid + o0, 1, o1 - o0);
	}
}
void ScummEngine::hdNoteActorPixel(const byte *dst, byte under, byte value, bool shadow) {
	remasterNotePixel(dst, under, value, shadow ? 2 : 1);
}

void ScummEngine::remasterForgetMarks(const byte *dst, int pitch, int w, int h) {
	// The game restored the background here: whatever was drawn (costumes incl. the verb coin, shadows, text) is
	// gone, so its pixel marks must go too. Otherwise background pixels that happen to have the old colour still
	// count as e.g. costume, and the HD-actor / sharp-text passes paint a dotted ghost of it.
	if (!_remasterActorKind || _remasterActorRoom != _currentRoom)
		return;
	const byte *base = (const byte *)_virtscr[kMainVirtScreen].getBasePtr(0, 0);
	for (int y = 0; y < h; y++) {
		const int64 o = (int64)(dst + y * pitch - base);
		const int64 a = MAX<int64>(0, o), b = MIN<int64>(_remasterActorSize, o + w);
		if (b > a) {
			memset(_remasterActorKind + a, 0, b - a);
			if (_remasterActorOwner)
				memset(_remasterActorOwner + a, 0, b - a);
		}
	}
}

void ScummEngine::remasterVideoTextTarget(const byte *base, int pitch, int h) {
	if (base != _remasterVidTextBase || pitch != _remasterVidTextPitch || h != _remasterVidTextH) {
		_remasterVidTextBase = base;
		_remasterVidTextPitch = pitch;
		_remasterVidTextH = h;
		_remasterVidText.clear();
		if (base && pitch > 0 && h > 0)
			_remasterVidText.resize(pitch * h);   // zero: nothing recorded
	}
}

void ScummEngine::remasterNoteTextPixel(const byte *dst, byte under) {
	if (_remasterVidTextBase && dst >= _remasterVidTextBase && dst < _remasterVidTextBase + _remasterVidText.size()) {
		// cutscene subtitle: text over the previous frame's own text (a frame without a new picture) keeps the
		// picture's pixel recorded then
		uint32 &e = _remasterVidText[dst - _remasterVidTextBase];
		if (e && ((e >> 8) & 0xFF) == under)
			under = e & 0xFF;
		e = (uint32)_remasterVidTextGen << 16 | (uint32)*dst << 8 | under;
		return;
	}
	// Text drawn over text (overlapping outlines of neighbouring letters, a line drawn twice in one frame) keeps the
	// scene pixel recorded by the first glyph, not the other glyph's pixel.
	const byte *base = (const byte *)_virtscr[kMainVirtScreen].getBasePtr(0, 0);
	const int64 off = (int64)(dst - base);
	byte first = 0;
	if (base && off >= 0 && off < _remasterActorSize && remasterActorKind((int)off, under, &first) == 3)
		under = first;
	remasterNotePixel(dst, under, *dst, 3);
}

void ScummEngine::remasterCleanRect(int x0, int y0, int x1, int y1) {
	// The text-free frame for this rect: the presented pixels, except glyph pixels, which get the scene pixel under them.
	const int W = _screenWidth, H = _screenHeight;
	if (_remasterClean.size() != (uint)(W * H)) {
		_remasterClean.resize(W * H);
		memcpy(_remasterClean.data(), _remasterRgb, (size_t)W * H * 4);
		_remasterCleanProtect.resize(W * H);
		if (_remasterProtect)
			memcpy(_remasterCleanProtect.data(), _remasterProtect, W * H);
	}
	for (int y = MAX(0, y0); y < MIN(H, y1); y++)
		for (int x = MAX(0, x0); x < MIN(W, x1); x++) {
			const int i = y * W + x;
			const bool text = _remasterProtect && _remasterProtect[i] == 3;
			_remasterClean[i] = text ? _remasterLut[_remasterTextUnder[i]] : _remasterRgb[i];
			_remasterCleanProtect[i] = text ? _remasterTextUnderKind[i] : (_remasterProtect ? _remasterProtect[i] : 0);
		}
}

bool ScummEngine::remasterSmoothTextOverlay() {
	// The glyph pixels (outline included) drawn with smooth outlines: every output pixel takes 2x2 samples; each sample
	// is won by the colour (or "no text") with the most Gaussian-weighted votes among the nearest 5x5 game pixels, and
	// the samples are averaged (anti-aliased edges) over the text-free picture. Same letter shapes and colours as the
	// game, rounded at output resolution instead of stair-stepped. Returns false (pixel blocks) for unusual text with
	// more than 15 colours.
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	static Common::Array<byte> lab, near;
	lab.resize(W * H);
	near.resize(W * H);
	memset(lab.data(), 0, W * H);
	memset(near.data(), 0, W * H);
	uint32 cols[16];
	int nCols = 0;
	bool any = false;
	for (int i = 0; i < W * H; i++) {
		if (_remasterProtect[i] != 3)
			continue;
		const uint32 c = _remasterRgb[i];
		int l = 1;
		while (l <= nCols && cols[l - 1] != c)
			l++;
		if (l > nCols) {
			if (nCols == 15)
				return false;
			cols[nCols++] = c;
		}
		lab[i] = (byte)l;
		any = true;
	}
	if (!any)
		return true;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (lab[y * W + x])
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++) {
						const int yy = y + dy, xx = x + dx;
						if (yy >= 0 && yy < H && xx >= 0 && xx < W)
							near[yy * W + xx] = 1;
					}
	// separable weights: wt[a * 2 + s][di + 2] for output column a in the game pixel, subsample s, neighbour di
	const float sigma = 0.6f;
	float wt[8 * 2][5];
	for (int a = 0; a < S; a++)
		for (int s = 0; s < 2; s++) {
			const float f = (a + 0.25f + 0.5f * s) / S - 0.5f;   // sample position relative to the pixel centre
			for (int di = -2; di <= 2; di++)
				wt[a * 2 + s][di + 2] = expf(-(di - f) * (di - f) / (2 * sigma * sigma));
		}
	uint8 cr[16], cg[16], cb[16];
	for (int l = 0; l < nCols; l++)
		_remasterFormat.colorToRGB(cols[l], cr[l + 1], cg[l + 1], cb[l + 1]);
	const uint32 aMask = _remasterFormat.aBits() ? ((uint32)(0xFF >> (8 - _remasterFormat.aBits())) << _remasterFormat.aShift) : 0;
	remasterParallel(H, [&](int ya, int yb) {
		for (int gy = ya; gy < yb; gy++)
			for (int gx = 0; gx < W; gx++) {
				if (!near[gy * W + gx])
					continue;
				for (int b = 0; b < S; b++)
					for (int a = 0; a < S; a++) {
						int sum[3] = {0, 0, 0}, hits = 0;
						for (int sy = 0; sy < 2; sy++)
							for (int sx = 0; sx < 2; sx++) {
								float votes[16] = {0};
								const float *wx = wt[a * 2 + sx], *wy = wt[b * 2 + sy];
								for (int dj = -2; dj <= 2; dj++) {
									const int yy = CLIP(gy + dj, 0, H - 1);
									for (int di = -2; di <= 2; di++) {
										const int xx = CLIP(gx + di, 0, W - 1);
										votes[lab[yy * W + xx]] += wx[di + 2] * wy[dj + 2];
									}
								}
								int best = 0;
								for (int l = 1; l <= nCols; l++)
									if (votes[l] > votes[best])
										best = l;
								if (best) {
									sum[0] += cr[best]; sum[1] += cg[best]; sum[2] += cb[best];
									hits++;
								}
							}
						if (!hits)
							continue;
						uint32 &o = _remasterBig[(size_t)(gy * S + b) * BW + gx * S + a];
						uint8 r, g, bl;
						_remasterFormat.colorToRGB(o, r, g, bl);
						const int keep = 4 - hits;   // samples of the scene
						o = (_remasterFormat.RGBToColor((uint8)((sum[0] + r * keep) / 4), (uint8)((sum[1] + g * keep) / 4),
						                                (uint8)((sum[2] + bl * keep) / 4)) & ~aMask) | (o & aMask);
					}
			}
	});
	return true;
}

void ScummEngine::remasterTextOverlay() {
	// Readable text over the finished picture (after the AI, HD layers, retro looks, CRT and the comparison slider):
	// each glyph pixel, outline included, as an exact SxS block in the game's own colour. The picture under it was
	// made from the text-free frame. remaster_sharp_text draws the glyphs with Scale2x/Scale3x edges instead.
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	if (!_remasterProtect || S < 2)
		return;
	if (_remasterSharpText) {
		remasterSharpText();
		return;
	}
	if (_remasterMode == kRemasterAI && _remasterSmoothText && remasterSmoothTextOverlay())
		return;   // AI HD: smooth letters next to the smooth picture; the pixel modes keep exact pixel blocks
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			if (_remasterProtect[y * W + x] != 3)
				continue;
			const uint32 c = _remasterRgb[y * W + x];
			uint32 *o = _remasterBig + (y * S) * BW + x * S;
			for (int dy = 0; dy < S; dy++, o += BW)
				for (int dx = 0; dx < S; dx++)
					o[dx] = c;
		}
}

void ScummEngine::remasterNotePixel(const byte *dst, byte under, byte value, byte kind) {
	const VirtScreen &vs = _virtscr[kMainVirtScreen];
	const byte *base = (const byte *)vs.getBasePtr(0, 0);
	const int size = vs.pitch * vs.h;
	const int64 off = (int64)(dst - base);
	if (!base || off < 0 || off >= size)
		return; // e.g. drawn into the back buffer
	if (!_remasterActorKind || _remasterActorSize != size || _remasterActorRoom != _currentRoom) {
		if (_remasterActorSize != size) {
			_remasterActorValue = (byte *)realloc(_remasterActorValue, size);
			_remasterActorUnder = (byte *)realloc(_remasterActorUnder, size);
			_remasterActorKind = (byte *)realloc(_remasterActorKind, size);
			_remasterActorOwner = (byte *)realloc(_remasterActorOwner, size);
			_remasterActorSize = size;
		}
		if (_remasterActorOwner)
			memset(_remasterActorOwner, 0, size);
		if (!_remasterActorValue || !_remasterActorUnder || !_remasterActorKind)
			return;
		memset(_remasterActorKind, 0, size);
		_remasterActorRoom = _currentRoom;
	}
	_remasterActorValue[off] = value;
	_remasterActorUnder[off] = under;
	_remasterActorKind[off] = kind; // 1 costume, 2 shadow, 3 text
	if (_remasterActorOwner)
		_remasterActorOwner[off] = (kind == 1 && _remasterDrawActor > 0 && _remasterDrawActor < 256) ? (byte)_remasterDrawActor : 0;
	// Edge guard: which actors reach the first/last column of a narrow room (buffer column = room column there).
	if (_remasterDrawActor > 0 && _remasterDrawActor < 256 && _remasterLogicalWidth && _roomWidth < _screenWidth) {
		const int x = (int)(off % vs.pitch);
		if (x <= 1)
			_remasterEdgeActor[0][_remasterDrawActor] = 1;
		else if (x >= _roomWidth - 2 && x < _roomWidth)
			_remasterEdgeActor[1][_remasterDrawActor] = 1;
	}
}

void ScummEngine::remasterBlit(const byte *src, int pitch, int x, int y, int w, int h, VirtScreen *vs) {
	const int W = _screenWidth, H = _screenHeight;
	// Clip to the screen.
	if (x < 0) { src -= x; w += x; x = 0; }
	if (y < 0) { src -= y * pitch; h += y; y = 0; }
	w = MIN(w, W - x);
	h = MIN(h, H - y);
	VirtScreen *mainVs = &_virtscr[kMainVirtScreen];
	// Widescreen: narrow rooms and 640-wide videos are drawn at the left as usual and centred when the frame is
	// presented (remasterCentreView), so nothing here depends on it.
	if (_remasterLogicalWidth && (vs == mainVs || _remasterBlitIsVideo)) {
		if (_remasterVideoShown != _remasterBlitIsVideo)
			warning("remaster: cutscene %s", _remasterBlitIsVideo ? "started" : "ended");
		if (_remasterBlitIsVideo)
			_remasterVideoFrames++;
		_remasterVideoShown = _remasterBlitIsVideo;
		_remasterVideoFull = _remasterBlitIsVideo && x == 0 && w >= W;
	}
	if (w <= 0 || h <= 0)
		return;
	for (int r = 0; r < h; r++)
		memcpy(_remasterScreen8 + (y + r) * W + x, src + r * pitch, w);
	if (_remasterTextUnder.size() != (uint)(W * H)) {
		_remasterTextUnder.resize(W * H);
		_remasterTextUnderKind.resize(W * H);
	}

	if (_remasterProtect) {
		// Which pixels must keep the AI under the grain guard: actors, and everything on the main screen
		// that is not plain room background (text, verbs, inventory, changed objects). Other screens (banner)
		// are protected entirely; video frames (vs == nullptr) are left to the grain detector.
		const bool refOk = vs == mainVs && _remasterRoomBg && _remasterRoomBgRoom == _currentRoom;
		const byte *mbase = (const byte *)mainVs->getBasePtr(0, 0);
		for (int r = y; r < y + h; r++) {
			byte *p = _remasterProtect + r * W;
			if (_remasterOwner)
				memset(_remasterOwner + r * W + x, 0, w);
			if (!vs) {
				memset(p + x, 0, w);
				if (_remasterBlitIsVideo && _remasterVidTextShift >= 0 && !_remasterVidText.empty() && r < _remasterVidTextH) {
					// this frame's subtitle pixels (see remasterNoteTextPixel)
					for (int c = x; c < x + w; c++) {
						const int vx = c - _remasterVidTextShift;
						if (vx < 0 || vx >= _remasterVidTextPitch)
							continue;
						const uint32 e = _remasterVidText[r * _remasterVidTextPitch + vx];
						if ((e >> 16) == _remasterVidTextGen && ((e >> 8) & 0xFF) == _remasterScreen8[r * W + c]) {
							p[c] = 3;
							_remasterTextUnder[r * W + c] = e & 0xFF;
							_remasterTextUnderKind[r * W + c] = 0;
						}
					}
				}
				continue;
			}
			if (!refOk) {
				memset(p + x, 1, w);
				continue;
			}
			for (int c = x; c < x + w; c++) {
				const int off = (int)(mainVs->getPixels(c, r - mainVs->topline + _screenTop) - mbase);
				const byte cur = _remasterScreen8[r * W + c];
				const bool plainBg = off >= 0 && off < _remasterRoomBgSize && _remasterRoomBgValid[off] && _remasterRoomBg[off] == cur;
				byte textUnder = 0;
				const byte kind = remasterActorKind(off, cur, &textUnder);
				if (kind == 3) {
					_remasterTextUnder[r * W + c] = textUnder;
					_remasterTextUnderKind[r * W + c] = (off >= 0 && off < _remasterRoomBgSize && _remasterRoomBgValid[off] && _remasterRoomBg[off] == textUnder) ? 0 : 1;
				}
				p[c] = kind == 3 ? 3 : (kind == 1 ? 2 : (kind == 2 ? 4 : (!plainBg ? 1 : 0))); // 3 text, 2 costume, 4 shadow, 1 UI/objects, 0 bg
				if (_remasterOwner)
					_remasterOwner[r * W + c] = (kind == 1 && _remasterActorOwner && off >= 0 && off < _remasterActorSize) ? _remasterActorOwner[off] : 0;
			}
		}
	}

	if (vs != mainVs || !remasterRoomActive()) {
		for (int r = y; r < y + h; r++) {
			const byte *s = _remasterScreen8 + r * W;
			uint32 *d = _remasterRgb + r * W;
			for (int c = x; c < x + w; c++)
				d[c] = _remasterLut[s[c]];
		}
		remasterCleanRect(x, y, x + w, y + h);
		if (remasterDeferred())
			_remasterFrameDirty = true;
		else
			_system->copyRectToScreen(_remasterRgb + y * W + x, W * 4, x, y, w, h);
		return;
	}

	// ── Compose a remaster rect ────────────────────────────────────────────────────────────────────
	const double scaleX = (double)_remasterBg.w / MAX<int>(1, _roomWidth);
	const double scaleY = (double)_remasterBg.h / MAX<int>(1, _roomHeight);
	const int block = MAX(1, (int)(1.0 / scaleX + 0.5)); // screen pixels per remaster pixel
	const int xs = vs->xstart;
	const byte *base = (const byte *)vs->getBasePtr(0, 0);
	const bool bgRefOk = _remasterRoomBg && _remasterRoomBgRoom == _currentRoom;

	// Grow the rect to whole blocks (room aligned) so blocks are always computed from all their pixels.
	const int x0 = MAX(0, ((xs + x) / block) * block - xs);
	const int x1 = MIN(W, ((xs + x + w + block - 1) / block) * block - xs);
	const int y0 = MAX(0, (y / block) * block);
	const int y1 = MIN(H, ((y + h + block - 1) / block) * block);

	auto offsetOf = [&](int sx, int sy) -> int {
		return (int)(vs->getPixels(sx, sy - vs->topline + _screenTop) - base);
	};
	auto bgPixel = [&](int sx, int sy) -> uint32 {
		const int bx = CLIP((int)((xs + sx + 0.5) * scaleX), 0, _remasterBg.w - 1);
		const int by = CLIP((int)((sy - vs->topline + _screenTop + 0.5) * scaleY), 0, _remasterBg.h - 1);
		return *(const uint32 *)_remasterBg.getBasePtr(bx, by);
	};
	auto isRoomBg = [&](int off, byte cur) -> bool {
		if (!bgRefOk || off < 0 || off >= _remasterRoomBgSize)
			return false;
		return _remasterRoomBgValid[off] && _remasterRoomBg[off] == cur;
	};

	for (int by = y0; by < y1; by += block) {
		for (int bx = x0 - ((xs + x0) % block); bx < x1; bx += block) {
			int actor = 0, shadow = 0, body = 0;
			uint32 sumBody[3] = {0, 0, 0}, sumCur[3] = {0, 0, 0}, sumUnder[3] = {0, 0, 0};
			for (int sy = by; sy < MIN(H, by + block); sy++) {
				for (int sx = MAX(0, bx); sx < MIN(W, bx + block); sx++) {
					const int off = offsetOf(sx, sy);
					const byte cur = base[off];
					byte under = 0;
					const byte k = remasterActorKind(off, cur, &under);
					if (!k)
						continue;
					actor++;
					if (k == 2) {
						shadow++;
						for (int c = 0; c < 3; c++) {
							sumCur[c] += _remasterPal[cur * 3 + c];
							sumUnder[c] += _remasterPal[under * 3 + c];
						}
					} else {
						body++;
						for (int c = 0; c < 3; c++)
							sumBody[c] += _remasterPal[cur * 3 + c];
					}
				}
			}
			const bool actorBlock = actor * 2 >= block * block;
			for (int sy = MAX(y0, by); sy < MIN(y1, by + block); sy++) {
				for (int sx = MAX(x0, bx); sx < MIN(x1, bx + block); sx++) {
					const int off = offsetOf(sx, sy);
					const byte cur = base[off];
					// "other": neither an actor pixel nor plain room background (text, UI, changed objects).
					const bool other = !remasterActorKind(off, cur) && !isRoomBg(off, cur);
					uint32 &out = _remasterRgb[sy * W + sx];
					if (actorBlock && !other) {
						if (shadow * 2 >= actor) {
							uint8 r, g, b;
							_remasterFormat.colorToRGB(bgPixel(sx, sy), r, g, b);
							uint8 rgb[3] = {r, g, b};
							for (int c = 0; c < 3; c++)
								rgb[c] = (uint8)(rgb[c] * (sumCur[c] + 1) / (MAX(sumUnder[c], sumCur[c]) + 1));
							out = _remasterFormat.RGBToColor(rgb[0], rgb[1], rgb[2]);
						} else if (body) {
							out = _remasterFormat.RGBToColor(sumBody[0] / body, sumBody[1] / body, sumBody[2] / body);
						} else {
							out = bgPixel(sx, sy);
						}
					} else if (other) {
						out = _remasterLut[cur]; // text, UI, changed objects: the game's own pixels
					} else {
						out = bgPixel(sx, sy);
					}
				}
			}
		}
	}
	remasterCleanRect(x0, y0, x1, y1);
	if (remasterDeferred())
		_remasterFrameDirty = true;
	else
		_system->copyRectToScreen(_remasterRgb + y0 * W + x0, W * 4, x0, y0, x1 - x0, y1 - y0);
}

void ScummEngine::remasterTick() {

	remasterExtractTick();
	remasterUpdateCheckPoll();
	remasterPadWalk();
	if (!ConfMan.hasKey("remaster_dump_dir"))
		return;
	remasterDumpRoomTick();
	// Snapshot of the presented frame about 3 and 10 seconds after entering a room. Works even when the
	// desktop is locked and screen captures come back black.
	if (_currentRoom != _remasterSnapRoom) {
		_remasterSnapRoom = _currentRoom;
		_remasterSnapFrames = 0;
	}
	_remasterSnapFrames++;
	Common::FSNode dir(Common::Path(ConfMan.get("remaster_dump_dir"), Common::Path::kNativeSeparator));
	Common::String name;
	if (_remasterSnapFrames == 90 || _remasterSnapFrames == 300) {
		name = Common::String::format("snapshot_room_%04d_f%03d.png", _currentRoom, _remasterSnapFrames);
	} else if (_remasterSnapFrames % 5 == 0) {
		// On request: an external tool writes a number into snap.request; each new number produces
		// snap_<number>_room_NNNN.png once. Used for automated play-testing.
		Common::FSNode req = dir.getChild("snap.request");
		Common::File f;
		if (req.exists() && f.open(req)) {
			const int n = atoi(f.readLine().c_str());
			if (n > 0 && n != _remasterSnapRequest) {
				_remasterSnapRequest = n;
				name = Common::String::format("snap_%03d_room_%04d.png", n, _currentRoom);
			}
		}
	}
	if (_remasterSnapFrames % 5 == 0) {
		// Automated testing of the problem report (dump dir only): a new number in report.request triggers it.
		// Automated save tests (dump dir only): "<n>" in save.request saves to slot n once.
		Common::FSNode sq = dir.getChild("save.request");
		Common::File sf;
		if (sq.exists() && sf.open(sq)) {
			const int n = atoi(sf.readLine().c_str());
			if (n > 0 && n != _remasterSaveRequest) {
				_remasterSaveRequest = n;
				requestSave(n % 100, Common::String::format("remaster test %d", n));
			}
		}
		// Automated controller tests: pad.request = "<n> stick <x> <y>" or "<n> hot <dir>".
		Common::FSNode pq = dir.getChild("pad.request");
		Common::File pf;
		if (pq.exists() && pf.open(pq)) {
			const Common::String line = pf.readLine();
			int n = 0, a1 = 0, a2 = 0;
			char cmd[16] = {0};
			if (sscanf(line.c_str(), "%d %15s %d %d", &n, cmd, &a1, &a2) >= 3 && n != _remasterPadRequest) {
				_remasterPadRequest = n;
				if (!strcmp(cmd, "stick")) {
					_remasterStickX = (int16)a1;
					_remasterStickY = (int16)a2;
				} else if (!strcmp(cmd, "hot")) {
					remasterHotspotCycle(a1);
				} else if (!strcmp(cmd, "capture")) {
					_remasterSmCapUntil = _system->getMillis() + (uint32)a1 * 1000;
				} else if (!strcmp(cmd, "compare")) {
					remasterCompareCycle();   // as Ctrl+F11
				} else if (!strcmp(cmd, "photo")) {
					remasterPhoto();   // as Shift+F9
				} else if (!strcmp(cmd, "upscaler")) {
					remasterCycleBackend();   // as Ctrl+F9
				} else if (!strcmp(cmd, "darken")) {
					darkenPalette(a1, a1, a1, 0, 255);   // fade tests: as the game's own palette fades (scale 0..255)
				} else if (!strcmp(cmd, "cels")) {
					// character-style tests: the followed character's recorded full-size cels (palette colours, alpha 0 =
					// transparent, 128 = shadow) and their AI-upscaled versions, plus on-screen sizes, into the dump dir
					const int egoId = VAR_EGO != 0xFF ? VAR(VAR_EGO) : -1;
					Common::String info;
					int k = 0;
					for (uint ci = 0; ci < _remasterCels.size(); ci++) {
						const RemasterCel &c = _remasterCels[ci];
						if (c.actor != egoId || c.px.empty())
							continue;
						Common::Array<uint32> src;
						src.resize(c.w * c.h);
						for (int p = 0; p < c.w * c.h; p++) {
							const int16 v = c.px[p];
							src[p] = v >= 0 ? _remasterFormat.ARGBToColor(255, _remasterPal[v * 3], _remasterPal[v * 3 + 1], _remasterPal[v * 3 + 2])
							                : (v == -2 ? _remasterFormat.ARGBToColor(128, 0, 0, 0) : _remasterFormat.ARGBToColor(0, 0, 0, 0));
						}
						Graphics::Surface ss;
						ss.init(c.w, c.h, c.w * 4, src.data(), _remasterFormat);
						Common::DumpFile sf2;
						if (sf2.open(dir.getChild(Common::String::format("cel_%d_%d_src.png", n, k)).getPath()))
							Image::writePNG(sf2, ss);
						const RemasterCelHD *hd = remasterCelHD(c);
						if (hd && !hd->rgba.empty()) {
							Graphics::Surface hs;
							hs.init(hd->w, hd->h, hd->w * 4, (void *)hd->rgba.data(), Graphics::PixelFormat(4, 8, 8, 8, 8, 0, 8, 16, 24));
							Common::DumpFile hf;
							if (hf.open(dir.getChild(Common::String::format("cel_%d_%d_ai.png", n, k)).getPath()))
								Image::writePNG(hf, hs);
						}
						info += Common::String::format("%d: cel %dx%d, on screen %dx%d, mirror %d\n", k, c.w, c.h, c.rect.width(), c.rect.height(), c.mirror ? 1 : 0);
						k++;
					}
					Common::DumpFile inf;
					if (inf.open(dir.getChild(Common::String::format("cel_%d_info.txt", n)).getPath()))
						inf.write(info.c_str(), info.size());
				} else if (!strcmp(cmd, "say")) {
					_remasterSayRequest = a1 > 0 ? a1 : 1;   // subtitle tests: the ego speaks a fixed line (main loop)
				} else if (!strcmp(cmd, "walk") && VAR_EGO != 0xFF && VAR(VAR_EGO) > 0) {
					Actor *ego = derefActorSafe(VAR(VAR_EGO), "remaster walk test");
					if (ego)
						ego->startWalkActor(a1, a2, -1);   // room coordinates (camera follows)
				}
			}
		}
		// Automated video tests: video.request = "<n> <file.san> [start frame]" plays that cutscene (from the main loop).
		Common::FSNode vq = dir.getChild("video.request");
		Common::File vf;
		if (vq.exists() && vf.open(vq)) {
			const Common::String line = vf.readLine();
			int n = 0, start = 0;
			char file[64] = {0};
			if (sscanf(line.c_str(), "%d %63s %d", &n, file, &start) >= 2 && n != _remasterVideoRequestN) {
				_remasterVideoRequestN = n;
				_remasterVideoRequest = file;
				_remasterVideoRequestStart = start;
			}
		}
		Common::FSNode rq = dir.getChild("report.request");
		Common::File rf;
		if (rq.exists() && rf.open(rq)) {
			const int n = atoi(rf.readLine().c_str());
			if (n > 0 && n != _remasterReportRequest) {
				_remasterReportRequest = n;
				remasterReport();
			}
		}
	}
	if (name.empty())
		return;
	Graphics::Surface frame;
	frame.init(_screenWidth, _screenHeight, _screenWidth * 4, _remasterRgb, _remasterFormat);
	Common::FSNode file = dir.getChild(name);
	Common::DumpFile out;
	if (out.open(file.getPath()) && Image::writePNG(out, frame))
		warning("remaster: snapshot %s", file.getPath().toString().c_str());
	if (_remasterProtect && _remasterClean.size() == (uint)(_screenWidth * _screenHeight) && name.hasPrefix("snap_")) {
		// subtitle tests: the text-free frame and the pixel kinds (text = 255)
		Graphics::Surface clean;
		clean.init(_screenWidth, _screenHeight, _screenWidth * 4, _remasterClean.data(), _remasterFormat);
		Common::DumpFile co;
		if (co.open(dir.getChild(name.substr(0, name.size() - 4) + "_clean.png").getPath()))
			Image::writePNG(co, clean);
		Common::Array<uint32> kinds;
		kinds.resize(_screenWidth * _screenHeight);
		for (uint i = 0; i < kinds.size(); i++) {
			const byte v = _remasterProtect[i] == 3 ? 255 : _remasterProtect[i] * 40;
			kinds[i] = _remasterFormat.RGBToColor(v, v, v);
		}
		Graphics::Surface ks;
		ks.init(_screenWidth, _screenHeight, _screenWidth * 4, kinds.data(), _remasterFormat);
		Common::DumpFile ko;
		if (ko.open(dir.getChild(name.substr(0, name.size() - 4) + "_kinds.png").getPath()))
			Image::writePNG(ko, ks);
	}
	if (_remasterOut) {
		// The presented output frame (upscaled, with side panels; one frame behind the game snapshot).
		Graphics::Surface big;
		big.init(_remasterOutW, _remasterOutH, _remasterOutW * 4, _remasterOut, _remasterFormat);
		Common::FSNode bigFile = dir.getChild(name.substr(0, name.size() - 4) + "_ai.png");
		Common::DumpFile bigOut;
		if (bigOut.open(bigFile.getPath()))
			Image::writePNG(bigOut, big);
	}
}

static Common::String remasterUrlEncode(const Common::String &in) {
	Common::String out;
	for (uint i = 0; i < in.size(); i++) {
		const byte c = (byte)in[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
			out += Common::String::format("%%%02X", c);
	}
	return out;
}

static Common::FSNode remasterPhotoDir() {
	Common::FSNode base;
	if (ConfMan.hasKey("remaster_photo_dir"))
		base = Common::FSNode(Common::Path(ConfMan.get("remaster_photo_dir"), Common::Path::kNativeSeparator));
	else if (ConfMan.hasKey("savepath"))
		base = Common::FSNode(Common::Path(ConfMan.get("savepath"), Common::Path::kNativeSeparator)).getParent().getChild("photos");
	else
		base = Common::FSNode(Common::Path("photos"));
	if (!base.exists())
		base.createDirectory();
	return base;
}

void ScummEngine::remasterPhoto() {
	if (_remasterPhotoJob) {
		_system->displayMessageOnOSD(Common::U32String("Photo: still developing the last one..."));
		return;
	}
	TimeDate t;
	_system->getTimeAndDate(t);
	const Common::String stamp = Common::String::format("comi-%04d-%02d-%02d_%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1,
		t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	const Common::FSNode dir = remasterPhotoDir();
	// 1. The frame exactly as shown (side art, HD backgrounds and characters; no cursor)
	const uint32 *shown = _remasterOut ? _remasterOut : _remasterBig;
	const int sw = _remasterOut ? _remasterOutW : _screenWidth * _remasterScale, sh = _remasterOut ? _remasterOutH : _screenHeight * _remasterScale;
	if (shown) {
		Graphics::Surface big;
		big.init(sw, sh, sw * 4, (void *)shown, _remasterFormat);
		Common::DumpFile o;
		if (o.open(dir.getChild(stamp + "-screen.png").getPath()))
			Image::writePNG(o, big);
	}
	// 2. The room (without the side margins) upscaled by the heavy network, on a worker thread
	RemasterAI *ai = _remasterBgAI ? _remasterBgAI : _remasterAI;
	const int scale = _remasterBgAI ? _remasterBgScale : _remasterScale;
	if (ai && _remasterRgb) {
		// narrow rooms: only the room (in widescreen it sits at the view offset of the frame, see remasterCentreView)
		const int m = remasterMargin();
		const int src = (_remasterLogicalWidth && m > 0) ? remasterViewSrcX() : m;
		const int cw = _screenWidth - 2 * m, ch = _screenHeight;
		Common::Array<uint32> crop;
		crop.resize(cw * ch);
		for (int y = 0; y < ch; y++)
			memcpy(&crop[y * cw], (_remasterClean.size() == (uint)(_screenWidth * ch) ? _remasterClean.data() : _remasterRgb) + y * _screenWidth + src, cw * 4);   // without subtitles
		_remasterPhotoJob = remasterAIJobStart(ai, (const byte *)crop.data(), cw, ch, scale, 192);
		_remasterPhotoW = cw;
		_remasterPhotoH = ch;
		_remasterPhotoScale = scale;
		_remasterPhotoFile = dir.getChild(stamp + Common::String::format("-%dx.png", scale)).getPath().toString(Common::Path::kNativeSeparator);
	}
	warning("remaster: photo %s", stamp.c_str());
	_system->displayMessageOnOSD(Common::U32String(_remasterPhotoJob ? "Photo taken - developing the poster-size version..." : "Photo saved"));
}

void ScummEngine::remasterPhotoPoll() {
	if (!_remasterPhotoJob || !remasterAIJobDone(_remasterPhotoJob))
		return;
	if (const byte *res = remasterAIJobResult(_remasterPhotoJob)) {
		const int w = _remasterPhotoW * _remasterPhotoScale, h = _remasterPhotoH * _remasterPhotoScale;
		Graphics::Surface big;
		big.init(w, h, w * 4, (void *)res, _remasterFormat);
		Common::DumpFile o;
		if (o.open(Common::Path(_remasterPhotoFile, Common::Path::kNativeSeparator)) && Image::writePNG(o, big)) {
			warning("remaster: photo saved %s (%dx%d)", _remasterPhotoFile.c_str(), w, h);
			_system->displayMessageOnOSD(Common::U32String(Common::String::format("Photo saved (%dx%d) in the photos folder", w, h)));
		}
	}
	remasterAIJobDestroy(_remasterPhotoJob);
	_remasterPhotoJob = nullptr;
}

void ScummEngine::remasterReport() {
	// Ctrl+F12: save what is on screen (presented frame + the game's own frame) and the current settings to
	// <install>/reports/<date-time>/, open that folder, and open a pre-filled GitHub issue in the browser. No
	// account or key is involved; the player describes the problem and drags the screenshot into the issue.
	TimeDate t;
	_system->getTimeAndDate(t);
	const Common::String stamp = Common::String::format("%04d-%02d-%02d_%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1,
		t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
	Common::FSNode base;
	if (ConfMan.hasKey("remaster_report_dir"))
		base = Common::FSNode(Common::Path(ConfMan.get("remaster_report_dir"), Common::Path::kNativeSeparator));
	else if (ConfMan.hasKey("savepath"))
		base = Common::FSNode(Common::Path(ConfMan.get("savepath"), Common::Path::kNativeSeparator)).getParent().getChild("reports");
	else
		base = Common::FSNode(Common::Path("reports"));
	if (!base.exists())
		base.createDirectory();
	Common::FSNode dir = base.getChild(stamp);
	if (!dir.exists())
		dir.createDirectory();
	bool saved = false;
	if (_remasterOut) {
		Graphics::Surface big;
		big.init(_remasterOutW, _remasterOutH, _remasterOutW * 4, _remasterOut, _remasterFormat);
		Common::DumpFile o;
		if (o.open(dir.getChild("screen.png").getPath()))
			saved = Image::writePNG(o, big);
	}
	if (_remasterRgb) {
		Graphics::Surface frame;
		frame.init(_screenWidth, _screenHeight, _screenWidth * 4, _remasterRgb, _remasterFormat);
		Common::DumpFile o;
		if (o.open(dir.getChild("original-frame.png").getPath()))
			saved = Image::writePNG(o, frame) || saved;
	}
	static const char *const modes[kRemasterModeCount] = {"AI HD", "Original", "VGA", "EGA", "Amiga", "Modern pixel art", "Classic (experimental)"};
	Common::String details = Common::String::format(
		"- Room: %d\n- Display mode: %s%s\n- Widescreen: %s\n- Side art: %s\n- HD characters: %s\n- Grain guard: %s\n- GPU AI: %s\n- Build: %s\n- Report folder: %s",
		_currentRoom, modes[_remasterMode], _remasterCrt ? " + CRT" : "",
		_remasterLogicalWidth ? "16:9" : (_remasterSide ? "4:3 + ambient panels" : "4:3"),
		_remasterSidesOn ? "on" : "off", _remasterHDActors ? "on" : "off", _remasterGrainOn ? "on" : "off",
		_remasterAI ? "available" : "not available", gScummVMFullVersion, stamp.c_str());
	Common::DumpFile txt;
	if (txt.open(dir.getChild("report.txt").getPath()))
		txt.writeString("COMI remaster problem report\n\n" + details + "\n");
	const Common::String repo = ConfMan.hasKey("remaster_issue_repo") ? ConfMan.get("remaster_issue_repo") : "fleccy/scummvm-ai-upscale";
	const Common::String title = Common::String::format("[Room %d] ", _currentRoom);
	const Common::String body = "**What looks wrong?**\n\n(describe it here)\n\n**Screenshot:** drag `screen.png` from the folder that "
		"just opened into this box (`original-frame.png` shows the unprocessed game frame).\n\n**Details** (filled in by the game)\n" + details + "\n";
	const Common::String url = "https://github.com/" + repo + "/issues/new?title=" + remasterUrlEncode(title) + "&body=" + remasterUrlEncode(body);
	const bool noOpen = ConfMan.hasKey("remaster_report_no_open") && ConfMan.getBool("remaster_report_no_open"); // automated tests
	if (!noOpen)
		_system->openUrl(dir.getPath().toString(Common::Path::kNativeSeparator));
	const bool browser = !noOpen && _system->openUrl(url);
	if (noOpen) {
		Common::DumpFile u;
		if (u.open(dir.getChild("issue-url.txt").getPath()))
			u.writeString(url);
	}
	_system->displayMessageOnOSD(Common::U32String(saved ? (browser ? "Report saved - describe the problem in your browser" : "Report saved to the reports folder")
		: "Could not save the report"));
	warning("remaster: report %s (%s)", stamp.c_str(), dir.getPath().toString(Common::Path::kNativeSeparator).c_str());
}

} // End of namespace Scumm



namespace Scumm {

// ---------------------------------------------------------------------------------------------------------------
// HD backgrounds

namespace {

// Area-weighted resampling of R,G,B,A byte images (used to bring the 4x background model down to the game's scale).
void remasterAreaResample(const uint32 *src, int sw, int sh, uint32 *dst, int dw, int dh) {
	struct Tap { int i; float w; };
	auto taps = [](int sn, int dn, Common::Array<Common::Array<Tap> > &out) {
		out.resize(dn);
		const float r = (float)sn / dn;
		for (int d = 0; d < dn; d++) {
			const float a = d * r, b = a + r;
			for (int i = (int)a; i < sn && i < b; i++) {
				const float wgt = MIN<float>(b, i + 1) - MAX<float>(a, i);
				if (wgt > 0)
					out[d].push_back({i, wgt / r});
			}
		}
	};
	Common::Array<Common::Array<Tap> > tx, ty;
	taps(sw, dw, tx);
	taps(sh, dh, ty);
	Common::Array<float> row((size_t)dw * sh * 4);
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < dw; x++) {
			float acc[4] = {0, 0, 0, 0};
			for (const Tap &t : tx[x]) {
				const byte *p = (const byte *)&src[(size_t)y * sw + t.i];
				for (int k = 0; k < 4; k++)
					acc[k] += p[k] * t.w;
			}
			memcpy(&row[((size_t)y * dw + x) * 4], acc, sizeof(acc));
		}
	for (int y = 0; y < dh; y++)
		for (int x = 0; x < dw; x++) {
			float acc[4] = {0, 0, 0, 0};
			for (const Tap &t : ty[y])
				for (int k = 0; k < 4; k++)
					acc[k] += row[((size_t)t.i * dw + x) * 4 + k] * t.w;
			byte *o = (byte *)&dst[(size_t)y * dw + x];
			for (int k = 0; k < 4; k++)
				o[k] = (byte)CLIP<float>(acc[k] + 0.5f, 0.0f, 255.0f);
		}
}

} // anonymous namespace

void ScummEngine::remasterBgClear() {
	for (uint i = 0; i < _remasterBgTiles.size(); i++)
		remasterAIJobDestroy(_remasterBgTiles[i].job);
	_remasterBgTiles.clear();
	_remasterHDBgRoom = -1;
}

void ScummEngine::remasterBgUpdate() {
	// Keep each column tile's HD version in step with the room background: when a tile's background (in its
	// current colours) has been stable for a moment, upscale it again on the worker thread, one tile at a time.
	static const int TW = 160, CTX = 16;
	VirtScreen *vs = &_virtscr[kMainVirtScreen];
	if (!_remasterRoomBg || !_remasterRoomBgValid || _remasterRoomBgRoom != _currentRoom || !vs->getBasePtr(0, 0))
		return;
	const int pitch = vs->pitch, vh = vs->h, S = _remasterScale;
	if ((int64)pitch * vh > _remasterRoomBgSize)
		return;
	if (_remasterHDBgRoom != _currentRoom || _remasterBgPitch != pitch || _remasterBgH != vh) {
		remasterBgClear();
		_remasterHDBgRoom = _currentRoom;
		_remasterBgPitch = pitch;
		_remasterBgH = vh;
		_remasterBgRoomSince = _system->getMillis();
		for (int x0 = 0; x0 < pitch; x0 += TW) {
			RemasterBgTile t;
			t.x0 = x0;
			t.w = MIN(TW, pitch - x0);
			_remasterBgTiles.push_back(t);
		}
	}
	const uint32 now = _system->getMillis();
	bool busy = false;
	for (uint i = 0; i < _remasterBgTiles.size(); i++) {
		RemasterBgTile &t = _remasterBgTiles[i];
		if (t.fade > 0 && t.fade < 16)
			t.fade++;
		if (!t.job)
			continue;
		if (!remasterAIJobDone(t.job)) {
			busy = true;
			continue;
		}
		if (const byte *res = remasterAIJobResult(t.job)) {
			const int BS = _remasterBgScale, ow = t.jobW * BS, cw = t.jobCw;
			Common::Array<uint32> core((size_t)cw * BS * vh * BS);
			const int cx = (t.jobCx0 - t.jobX0) * BS;
			for (int y = 0; y < vh * BS; y++)
				memcpy(&core[(size_t)y * cw * BS], res + ((size_t)y * ow + cx) * 4, (size_t)cw * BS * 4);
			t.hd.resize((size_t)cw * S * vh * S);
			if (BS == S)
				memcpy(t.hd.data(), core.data(), core.size() * 4);
			else
				remasterAreaResample(core.data(), cw * BS, vh * BS, t.hd.data(), cw * S, vh * S);
			t.src.resize((size_t)cw * vh);
			for (int y = 0; y < vh; y++)
				memcpy(&t.src[(size_t)y * cw], &t.jobSrc[(size_t)y * t.jobW + (t.jobCx0 - t.jobX0)], (size_t)cw * 4);
			t.hx0 = t.jobCx0;
			t.hw = cw;
			t.hash = t.jobHash;
			t.fade = 1;
			remasterBgCacheSave(t);
		}
		remasterAIJobDestroy(t.job);
		t.job = nullptr;
		t.jobSrc.clear();
	}
	if (_remasterBgTiles.empty())
		return;
	// Look at one tile per frame; right after entering a room, at all of them (cached tiles then appear at once).
	const bool fresh = now - _remasterBgRoomSince < 1500;
	const uint n = fresh ? _remasterBgTiles.size() : 1;
	for (uint k = 0; k < n; k++) {
		RemasterBgTile &t = _remasterBgTiles[_remasterBgNext++ % _remasterBgTiles.size()];
		if (t.job)
			continue;
		// Use the columns that have been drawn: a tile at the edge of what has been shown (scrolling rooms, or rooms
		// wider than the screen that never scroll) is processed for its drawn part, with whatever context exists.
		// Rooms wider than the screen: the game's scroll buffer is one block in which the right part of the view
		// continues at the start of the next row, so in those buffer columns the first row is never drawn (it would
		// belong to the row above the screen). Ignore the first row for that reason.
		auto colDrawn = [&](int x) {
			for (int y = 1; y < vh; y++)
				if (!_remasterRoomBgValid[(size_t)y * pitch + x])
					return false;
			return true;
		};
		int ca = t.x0;
		while (ca < t.x0 + t.w && !colDrawn(ca))
			ca++;
		int cb = ca;
		while (cb < t.x0 + t.w && colDrawn(cb))
			cb++;
		t.incomplete = cb - ca < 24;
		if (t.incomplete)
			continue;
		// Context columns. Past the buffer's left/right edge the buffer continues in the row above/below (see
		// above), which in rooms wider than the screen is exactly the picture's neighbouring content.
		int a = ca, b = cb;
		while (a > ca - CTX && (a <= 0 || colDrawn(a - 1)))
			a--;
		while (b < cb + CTX && (b >= pitch || colDrawn(b)))
			b++;
		uint32 hash = 2166136261u ^ (uint32)(ca * 7919 + cb);
		const int64 bufLen = (int64)pitch * vh;
		auto bgAt = [&](int y, int x) -> byte {
			const int64 off = CLIP<int64>((int64)y * pitch + x, 0, bufLen - 1);
			return _remasterRoomBg[off];
		};
		for (int y = 0; y < vh; y++)
			for (int x = a; x < b; x++)
				{
					const byte ix = bgAt(y, x);
					hash = (hash ^ (remasterIndexCycles(ix) ? 0x01000000u + ix : _remasterLut[ix])) * 16777619u;
				}
		hash |= 1;
		if (hash == t.hash) {
			t.pendHash = hash;
			t.changedSince = 0;
			continue;
		}
		if (hash != t.pendHash) {
			t.pendHash = hash;
			t.pendSince = now;
		}
		// Made before (this or an earlier session)? Then it is ready at once. Looked up right after entering the room,
		// otherwise only once the tile has been stable for a moment (animated backgrounds change every frame).
		const bool stable = now - t.pendSince >= 400;
		if (hash != t.cacheMiss && (stable || fresh)) {
			if (remasterBgCacheLoad(t, ca, cb - ca, hash)) {
				t.fade = fresh && now - _remasterBgRoomSince < 600 ? 16 : 1;
				t.changedSince = 0;
				continue;
			}
			t.cacheMiss = hash;
		}
		// A tile that keeps changing (animation drawn into the background) is not remade over and over: each job
		// is a heavy GPU load that slows the real-time AI. Its animated pixels use the real-time AI anyway.
		if (t.jobs >= 2)
			continue;
		if (!t.changedSince)
			t.changedSince = now;
		// Make it when it is stable (fades, flashes), or regardless after a while (local animation: the per-pixel colour
		// check then keeps the animated pixels on the real-time AI). One job at a time.
		if (busy || (!stable && now - t.changedSince < 4000))
			continue;
		t.jobX0 = a;
		t.jobW = b - a;
		t.jobCx0 = ca;
		t.jobCw = cb - ca;
		t.jobHash = hash;
		t.jobSrc.resize((size_t)t.jobW * vh);
		for (int y = 0; y < vh; y++)
			for (int x = a; x < b; x++)
				t.jobSrc[(size_t)y * t.jobW + (x - a)] = _remasterLut[bgAt(y, x)];
		t.job = remasterAIJobStart(_remasterBgAI, (const byte *)t.jobSrc.data(), t.jobW, vh, _remasterBgScale, 128);
		t.jobs++;
		t.changedSince = 0;
		busy = true;
	}
	// Prewarm runs (remaster_bg_prewarm): quit once this room's HD background is complete, so a script can step
	// through every room to fill the disk cache.
	if (ConfMan.hasKey("remaster_bg_prewarm") && ConfMan.getBool("remaster_bg_prewarm") && now - _remasterBgRoomSince > 1500) {
		bool done = !busy;
		for (uint k = 0; k < _remasterBgTiles.size() && done; k++)
			done = !_remasterBgTiles[k].hd.empty() || _remasterBgTiles[k].incomplete;
		if (done || now - _remasterBgRoomSince > 40000) {
			warning("remaster: prewarm of room %d %s", _currentRoom, done ? "complete" : "timed out");
			quitGame();
		}
	}
}

Common::String ScummEngine::remasterBgCacheFile(int x0, int w, uint32 hash) const {
	return Common::String::format("r%03d_x%04d_w%03d_h%03d_s%d_%08x.png", _currentRoom, x0, w, _remasterBgH, _remasterScale, hash);
}

static Common::FSNode remasterBgCacheDir() {
	// remaster_bg_cache, or "hd-cache" next to the models folder (the install folder, which is per user).
	if (ConfMan.hasKey("remaster_bg_cache"))
		return Common::FSNode(Common::Path(ConfMan.get("remaster_bg_cache"), Common::Path::kNativeSeparator));
	// (the model path has no extension, so it is not an existing file node: work on the path itself)
	const Common::Path model(ConfMan.get("remaster_ai_model"), Common::Path::kNativeSeparator);
	return Common::FSNode(model.getParent().getParent().appendComponent("hd-cache"));
}

bool ScummEngine::remasterBgCacheLoad(RemasterBgTile &t, int x0, int w, uint32 hash) {
	if (ConfMan.hasKey("remaster_bg_cache_enabled") && !ConfMan.getBool("remaster_bg_cache_enabled"))
		return false;
	Common::FSNode f = remasterBgCacheDir().getChild(remasterBgCacheFile(x0, w, hash));
	if (!f.exists())
		return false;
	Common::SeekableReadStream *st = f.createReadStream();
	if (!st)
		return false;
	Image::PNGDecoder dec;
	const bool ok = dec.loadStream(*st);
	delete st;
	const int S = _remasterScale, vh = _remasterBgH, pitch = _remasterBgPitch;
	if (!ok || !dec.getSurface() || dec.getSurface()->w != w * S || dec.getSurface()->h != vh * S)
		return false;
	Graphics::Surface *conv = dec.getSurface()->convertTo(_remasterFormat);
	if (!conv)
		return false;
	t.hd.resize((size_t)w * S * vh * S);
	for (int y = 0; y < vh * S; y++)
		memcpy(&t.hd[(size_t)y * w * S], conv->getBasePtr(0, y), (size_t)w * S * 4);
	conv->free();
	delete conv;
	// The colours it was made from are, by its hash, the room's current ones.
	t.src.resize((size_t)w * vh);
	for (int y = 0; y < vh; y++)
		for (int x = 0; x < w; x++)
			t.src[(size_t)y * w + x] = _remasterLut[_remasterRoomBg[(size_t)y * pitch + x0 + x]];
	t.hx0 = x0;
	t.hw = w;
	t.hash = hash;
	return true;
}

void ScummEngine::remasterBgCacheSave(const RemasterBgTile &t) {
	if (ConfMan.hasKey("remaster_bg_cache_enabled") && !ConfMan.getBool("remaster_bg_cache_enabled"))
		return;
	Common::FSNode dir = remasterBgCacheDir();
	if (!dir.exists() && !dir.createDirectory())
		return;
	const int S = _remasterScale;
	Graphics::Surface surf;
	surf.init(t.hw * S, _remasterBgH * S, t.hw * S * 4, const_cast<uint32 *>(t.hd.data()), _remasterFormat);
	Common::DumpFile out;
	if (out.open(dir.getChild(remasterBgCacheFile(t.hx0, t.hw, t.hash)).getPath()))
		Image::writePNG(out, surf);
}

bool ScummEngine::remasterBgPlate(int r, int c, int &tile, int &tx, int &ty) {
	// The room's HD background pixel behind game screen pixel (r, c), whatever is drawn there now.
	if (_remasterBgTiles.empty() || _remasterHDBgRoom != _currentRoom || !_remasterRoomBg)
		return false;
	VirtScreen *vs = &_virtscr[kMainVirtScreen];
	const byte *base = (const byte *)vs->getBasePtr(0, 0);
	if (!base || vs->pitch != _remasterBgPitch || vs->h != _remasterBgH)
		return false;
	const int m = remasterMargin(), TW = 160;
	const int vrow = r - vs->topline + _screenTop;
	if (r < vs->topline || vrow < 0 || vrow >= vs->h || c < m || c >= _screenWidth - m)
		return false;
	const int64 off = (int64)((const byte *)vs->getPixels(0, vrow) - base) + (c - m) + remasterViewSrcX();
	if (off < 0 || off >= (int64)_remasterBgPitch * _remasterBgH)
		return false;
	const int bx = (int)(off % _remasterBgPitch), by = (int)(off / _remasterBgPitch), ti = bx / TW;
	if (ti >= (int)_remasterBgTiles.size())
		return false;
	const RemasterBgTile &t = _remasterBgTiles[ti];
	if (t.hd.empty() || bx < t.hx0 || bx >= t.hx0 + t.hw)
		return false;
	tile = ti;
	tx = bx - t.hx0;
	ty = by;
	return true;
}

void ScummEngine::remasterHDBackground() {
	// Use the HD background where the screen shows untouched room background in the same colours it was made from;
	// one game pixel of soft transition next to anything else (actors, objects, text), which keeps the real-time AI.
	if (_remasterBgTiles.empty() || _remasterHDBgRoom != _currentRoom || !_remasterProtect || !_remasterRoomBg)
		return;
	if (!remasterAIBackendIsAI(_remasterAI))
		return;   // the HD backgrounds are made by AI: hidden while a plain upscaler (FSR) is compared
	VirtScreen *vs = &_virtscr[kMainVirtScreen];
	const byte *base = (const byte *)vs->getBasePtr(0, 0);
	if (!base || vs->pitch != _remasterBgPitch || vs->h != _remasterBgH)
		return;
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S, m = remasterMargin();
	const int TW = 160;
	Common::Array<byte> &w8 = _remasterBgW8;
	Common::Array<int32> &tileOf = _remasterBgTileOf, &bxOf = _remasterBgBx, &byOf = _remasterBgBy;
	w8.resize((size_t)W * H);
	tileOf.resize((size_t)W * H);
	bxOf.resize((size_t)W * H);
	byOf.resize((size_t)W * H);
	memset(w8.data(), 0, w8.size());
	const int srcX = remasterViewSrcX();
	bool cycles[256];
	for (int k = 0; k < 256; k++)
		cycles[k] = remasterIndexCycles(k);
	Common::Array<byte> rowAny;
	rowAny.resize(H);
	memset(rowAny.data(), 0, H);
	remasterParallel(H, [&](int ra, int rb) {
	for (int r = ra; r < rb; r++) {
		const int vrow = r - vs->topline + _screenTop;
		if (r < vs->topline || vrow < 0 || vrow >= vs->h)
			continue;
		const int64 rowOff = (int64)((const byte *)vs->getPixels(0, vrow) - base);
		for (int c = m; c < W - m; c++) {
			const int64 off = rowOff + (c - m) + srcX;
			if (off < 0 || off >= (int64)_remasterBgPitch * _remasterBgH)
				continue;
			const int bx = (int)(off % _remasterBgPitch), by = (int)(off / _remasterBgPitch);
			const int ti = bx / TW;
			if (ti >= (int)_remasterBgTiles.size())
				continue;
			const RemasterBgTile &t = _remasterBgTiles[ti];
			const int v = r * W + c;
			if (cycles[base[off]])
				continue; // cycling colour (water, flames): keep the real-time AI, which follows the cycle
			if (t.hd.empty() || bx < t.hx0 || bx >= t.hx0 + t.hw || _remasterProtect[v] != 0 ||
			    t.src[(size_t)by * t.hw + (bx - t.hx0)] != _remasterRgb[v])
				continue;
			w8[v] = (byte)(t.fade * 16 > 255 ? 255 : t.fade * 16);
			tileOf[v] = ti;
			bxOf[v] = bx - t.hx0;
			byOf[v] = by;
			rowAny[r] = 1;
		}
	}
	});
	bool any = false;
	for (int r = 0; r < H && !any; r++)
		any = rowAny[r] != 0;
	if (!any)
		return;
	remasterParallel(H, [&](int ra, int rb) {
	for (int r = ra; r < rb; r++) {
		for (int c = m; c < W - m; c++) {
			const int v = r * W + c;
			if (!w8[v])
				continue;
			// soft edge: lower the weight next to pixels that keep the real-time AI
			int sum = 0, n = 0;
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++) {
					const int rr = r + dy, cc = c + dx;
					if (rr < 0 || rr >= H || cc < m || cc >= W - m)
						continue;
					sum += w8[rr * W + cc];
					n++;
				}
			const int wgt = MIN<int>(w8[v], sum / MAX(1, n));
			const RemasterBgTile &t = _remasterBgTiles[tileOf[v]];
			const int tw = t.hw * S;
			for (int yy = 0; yy < S; yy++) {
				uint32 *o = _remasterBig + (size_t)(r * S + yy) * BW + c * S;
				const uint32 *hp = &t.hd[(size_t)(byOf[v] * S + yy) * tw + bxOf[v] * S];
				if (wgt >= 255) {
					memcpy(o, hp, S * 4);
					continue;
				}
				for (int xx = 0; xx < S; xx++) {
					byte *ob = (byte *)&o[xx];
					const byte *hb = (const byte *)&hp[xx];
					for (int k = 0; k < 3; k++)
						ob[k] = (byte)((ob[k] * (255 - wgt) + hb[k] * wgt + 127) / 255);
				}
			}
		}
	}
	});
}

} // End of namespace Scumm

namespace Scumm {

void ScummEngine::remasterCompareCycle() {
	// Ctrl+F11: off -> original pixels -> AMD FSR -> standard AI -> off (a choice equal to the current upscaler is
	// skipped)
	const Common::String cur = ConfMan.hasKey("remaster_ai_backend") ? ConfMan.get("remaster_ai_backend") : "ncnn";
	static const char *const kWith[] = {nullptr, "fsr", "ncnn"};
	static const char *const kName[] = {"original pixels", "AMD FSR 1", "standard AI"};
	int next = _remasterCompare ? _remasterCompareWith + 1 : 0;
	while (next >= 1 && next <= 2 && cur == kWith[next])
		next++;
	remasterAIDestroy(_remasterCompareAI);
	_remasterCompareAI = nullptr;
	if (next > 2) {
		_remasterCompare = false;
		_system->displayMessageOnOSD(Common::U32String("Comparison off"));
		return;
	}
	if (next > 0) {
		Common::String info;
		_remasterCompareAI = remasterAICreateBackend(kWith[next], ConfMan.get("remaster_ai_model"), info);
		if (!_remasterCompareAI) {
			_remasterCompare = false;
			_system->displayMessageOnOSD(Common::U32String(Common::String::format("Comparison with %s unavailable", kName[next])));
			return;
		}
	}
	if (!_remasterCompare)
		_remasterCompareX = _screenWidth / 2;
	_remasterCompare = true;
	_remasterCompareWith = next;
	_system->displayMessageOnOSD(Common::U32String(Common::String::format("Comparison: %s | %s - hold Ctrl and move the mouse (Ctrl+F11: next)",
		remasterAIBackendName(_remasterAI), kName[next])));
}

void ScummEngine::remasterCompareOverlay() {
	// Right of the divider: the plain game pixels (nearest neighbour), or the frame upscaled by another upscaler
	// (without the HD layers); left: the current display mode; a divider line.
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	const int sx = CLIP(_remasterCompareX, 0, W) * S;
	bool other = false;
	int x0 = 0, rw = 0;
	if (_remasterCompareWith > 0 && _remasterCompareAI) {
		// the part right of the divider, with some context
		x0 = MAX(0, CLIP(_remasterCompareX, 0, W) - 24);
		rw = W - x0;
		if (rw > 0) {
			_remasterCompareIn.resize(rw * H);
			_remasterCompareOut.resize((size_t)rw * S * H * S);
			for (int y = 0; y < H; y++)
				memcpy(&_remasterCompareIn[y * rw], _remasterRgb + y * W + x0, rw * 4);
			double ms;
			other = remasterAIRun(_remasterCompareAI, (const byte *)_remasterCompareIn.data(), rw, H, S, (byte *)_remasterCompareOut.data(), &ms);
		}
	}
	const uint32 white = _remasterFormat.RGBToColor(255, 255, 255), dark = _remasterFormat.RGBToColor(0, 0, 0);
	for (int y = 0; y < H * S; y++) {
		uint32 *d = _remasterBig + (size_t)y * BW;
		if (other) {
			memcpy(d + sx, &_remasterCompareOut[(size_t)y * rw * S + (sx - x0 * S)], (size_t)(BW - sx) * 4);
		} else {
			const uint32 *src = _remasterRgb + (y / S) * W;
			for (int x = sx; x < BW; x++)
				d[x] = src[x / S];
		}
		for (int x = MAX(0, sx - 2); x < MIN(BW, sx + 2); x++)
			d[x] = white;
		if (sx - 3 >= 0) d[sx - 3] = dark;
		if (sx + 2 < BW) d[sx + 2] = dark;
	}
}

} // End of namespace Scumm

namespace Scumm {

void ScummEngine::remasterApplyStrength() {
	// Blend the finished AI picture with a plain bilinear upscale of the game frame: 100 % = all AI, 0 % = none.
	const int W = _screenWidth, H = _screenHeight, S = _remasterScale, BW = W * S;
	const int a = _remasterAIStrength * 256 / 100, b = 256 - a;
	for (int y = 0; y < H * S; y++) {
		const int fy = CLIP((y * 2 + 1) * 128 / S - 128, 0, (H - 1) * 256);
		const int y0 = fy >> 8, y1 = MIN(y0 + 1, H - 1), wy = fy & 255;
		const byte *r0 = (const byte *)(_remasterRgb + y0 * W), *r1 = (const byte *)(_remasterRgb + y1 * W);
		byte *d = (byte *)(_remasterBig + (size_t)y * BW);
		for (int x = 0; x < BW; x++) {
			const int fx = CLIP((x * 2 + 1) * 128 / S - 128, 0, (W - 1) * 256);
			const int x0 = fx >> 8, x1 = MIN(x0 + 1, W - 1), wx = fx & 255;
			for (int c = 0; c < 3; c++) {
				const int top = r0[x0 * 4 + c] * (256 - wx) + r0[x1 * 4 + c] * wx;
				const int bot = r1[x0 * 4 + c] * (256 - wx) + r1[x1 * 4 + c] * wx;
				const int lin = (top * (256 - wy) + bot * wy) >> 16;
				d[x * 4 + c] = (byte)((d[x * 4 + c] * a + lin * b) >> 8);
			}
		}
	}
}

void ScummEngine::remasterBackendAnnounce() {
	if (!_remasterAI)
		return;
	// prewarm.ps1 (remaster_trt_prewarm): quit once every TensorRT engine is built and cached
	if (ConfMan.hasKey("remaster_trt_prewarm") && ConfMan.getBool("remaster_trt_prewarm") && !shouldQuit()) {
		const int st = remasterAIPrepareState(_remasterAI);
		if (st != 0) {
			warning("remaster: TensorRT prewarm %s", st > 0 ? "complete" : "failed");
			quitGame();
		}
	}
	const Common::String name = remasterAIBackendName(_remasterAI);
	if (_remasterBackendShown.empty() || name == _remasterBackendShown)
		return;
	_remasterBackendShown = name;
	_remasterAIPrev.clear();     // upscale the next frame in full with the upscaler that is now active
	_remasterCelCache.clear();
	warning("remaster: upscaler now %s", name.c_str());
	_system->displayMessageOnOSD(Common::U32String(Common::String::format("Upscaler: %s", name.c_str())));
}

void ScummEngine::remasterCycleBackend() {
	// Ctrl+F9: switch between the available upscalers while playing.
	static const char *const kBackends[] = {"ncnn", "dml", "dml-ultra", "trt", "trt-ultra", "fsr"};
	const int n = ARRAYSIZE(kBackends);
	Common::String cur = ConfMan.hasKey("remaster_ai_backend") ? ConfMan.get("remaster_ai_backend") : "ncnn";
	int at = 0;
	for (int k = 0; k < n; k++)
		if (cur == kBackends[k])
			at = k;
	for (int step = 1; step < n; step++) {
		const char *next = kBackends[(at + step) % n];
		Common::String info;
		RemasterAI *ai = remasterAICreateBackend(next, ConfMan.get("remaster_ai_model"), info, _screenWidth, _screenHeight);
		if (!ai) {
			warning("remaster: upscaler %s unavailable (%s)", next, info.c_str());
			continue;
		}
		remasterAIDestroy(_remasterAI);
		_remasterAI = ai;
		_remasterAIPrev.clear();     // next frame is upscaled in full
		_remasterAISettle = 0;
		_remasterCelCache.clear();   // HD characters are upscaled again by the new upscaler
		ConfMan.set("remaster_ai_backend", next);
		ConfMan.flushToDisk();
		warning("remaster: upscaler now %s (%s)", remasterAIBackendName(_remasterAI), info.c_str());
		_system->displayMessageOnOSD(Common::U32String(Common::String::format("Upscaler: %s", remasterAIBackendName(_remasterAI))));
		_remasterBackendShown = remasterAIBackendName(_remasterAI);
		remasterUpdateScreen();
		return;
	}
	_system->displayMessageOnOSD(Common::U32String("No other upscaler available"));
}

void ScummEngine::remasterStepStrength(int delta) {
	_remasterAIStrength = CLIP(_remasterAIStrength + delta, 0, 100);
	ConfMan.setInt("remaster_ai_strength", _remasterAIStrength);
	ConfMan.flushToDisk();
	_system->displayMessageOnOSD(Common::U32String(Common::String::format("AI strength: %d%%", _remasterAIStrength)));
	remasterUpdateScreen();
}

} // End of namespace Scumm

namespace Scumm {

void ScummEngine::remasterGovernorFrame(bool late) {
	// Per room: once settled (3 s after entering), if most of the last 24 game frames (2 s at COMI's 12 Hz) were late
	// while the AI runs, step down: 1 the AI picture is refreshed on every other frame (the game itself animates at
	// about 12 fps, so this is the least visible saving), 2 + HD backgrounds off, 3 + HD characters off (the most
	// visible: distant characters become the AI's guess from the tiny game sprite).
	const uint32 now = _system->getMillis();
	if (_remasterGovRoom != _currentRoom) {
		_remasterGovRoom = _currentRoom;
		_remasterGovLevel = 0;
		remasterAISetLight(_remasterAI, false);   // Ultra: try the heavy network again in the new room
		_remasterGovLight = false;
		_remasterGovOver = 0;
		_remasterGovSince = now;
		_remasterGovHist = 0;
	}
	if (ConfMan.hasKey("remaster_governor") && !ConfMan.getBool("remaster_governor")) {
		_remasterGovLevel = 0;   // remaster_governor=false: always keep every effect
		return;
	}
	if (_remasterMode != kRemasterAI || _remasterVideoShown || now - _remasterGovSince < 2000 || _remasterGovLevel >= 3)
		return;
	if (_remasterCompare) {
		// the comparison slider runs a second upscaler: its cost must not take effects away from the picture
		// being compared
		_remasterGovOver = 0;
		_remasterGovHist = 0;
		return;
	}
	_remasterGovHist = (_remasterGovHist << 1) | (late ? 1 : 0);
	if (++_remasterGovOver < 12)
		return;
	int lateCount = 0;
	for (int k = 0; k < 12; k++)
		lateCount += (_remasterGovHist >> k) & 1;
	if (lateCount >= 8 && _remasterGovLevel == 0 && !_remasterGovLight && remasterAISetLight(_remasterAI, true)) {
		// Ultra: first fall back to the standard network in this room, keeping every effect
		_remasterGovLight = true;
		_remasterGovOver = 0;
		_remasterGovHist = 0;
		_remasterAISettle = 1;   // refresh the picture with the standard network soon
		warning("remaster: room %d is heavy for Ultra (%d of 12 frames late), standard AI for this room", _currentRoom, lateCount);
		return;
	}
	if (lateCount >= 8) {
		_remasterGovLevel++;
		_remasterGovOver = 0;
		_remasterGovHist = 0;
		warning("remaster: room %d is heavy (%d of 12 frames late), %s for this room", _currentRoom, lateCount,
			_remasterGovLevel == 1 ? "AI on every other frame" : _remasterGovLevel == 2 ? "HD backgrounds off too" : "HD characters off too");
	}
}

} // End of namespace Scumm

namespace Scumm {

bool ScummEngine::remasterIndexCycles(int i) const {
	return _remasterPalChangeN[i] >= 3 && _system->getMillis() - _remasterPalChangeT[i] < 2000;
}

} // End of namespace Scumm
