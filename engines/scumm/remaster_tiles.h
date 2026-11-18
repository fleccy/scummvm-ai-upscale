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

#ifndef SCUMM_REMASTER_TILES_H
#define SCUMM_REMASTER_TILES_H

#include "common/scummsys.h"

namespace Scumm {

// COMI remaster: recently upscaled tiles, keyed by a fingerprint of their input (the tile plus the neighbourhood the
// network looks at). Identical input gives identical output, so animation that repeats (colour-cycled water, fire,
// looping scenery) is upscaled once per distinct picture instead of every frame. Least recently used tiles go first.
class RemasterTileCache;

RemasterTileCache *remasterTileCacheCreate(size_t maxBytes);
void remasterTileCacheDestroy(RemasterTileCache *c);
void remasterTileCacheClear(RemasterTileCache *c);
// On a hit, copies the w x h tile into dst (pitch in pixels) and returns true.
bool remasterTileCacheGet(RemasterTileCache *c, uint64 key, uint32 *dst, int pitch, int w, int h);
void remasterTileCachePut(RemasterTileCache *c, uint64 key, const uint32 *src, int pitch, int w, int h);
void remasterTileCacheStats(RemasterTileCache *c, uint64 &hits, uint64 &misses, size_t &bytes);   // resets the counts
size_t remasterTileCacheBytes(const RemasterTileCache *c);

// 64-bit fingerprint of the pixels [x0, x1] x [y0, y1] (inclusive) of an image with the given pitch.
uint64 remasterHashRect(const uint32 *img, int pitch, int x0, int y0, int x1, int y1, uint64 seed);

} // End of namespace Scumm

#endif
