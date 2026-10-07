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

#ifndef SCUMM_REMASTER_ORT_H
#define SCUMM_REMASTER_ORT_H

#include "common/scummsys.h"
#include "common/str.h"

namespace Scumm {

// ONNX Runtime upscalers (Windows). The model takes and returns RGBA bytes: [1, H, W, 4] uint8 ->
// [1, H*scale, W*scale, 4] uint8 (see dists/remaster/ncnn2onnx.py). Images of any size are accepted.
enum RemasterOrtProvider { kOrtDirectML = 0, kOrtTensorRT = 1 };
struct RemasterOrt;
// fullW x fullH: the whole-frame size, prepared up front (0 = none). TensorRT prepares on a worker thread: until
// remasterOrtState() is 1, remasterOrtRun() returns false.
RemasterOrt *remasterOrtCreate(RemasterOrtProvider provider, const Common::String &modelPath, int fullW, int fullH, Common::String &err);
int remasterOrtState(RemasterOrt *o, Common::String *failure = nullptr);   // 0 preparing, 1 ready, -1 failed
bool remasterOrtRun(RemasterOrt *o, const uint8 *in, int w, int h, int scale, uint8 *out);
void remasterOrtDestroy(RemasterOrt *o);

} // End of namespace Scumm

#endif
