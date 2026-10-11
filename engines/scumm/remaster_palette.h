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

#ifndef SCUMM_REMASTER_PALETTE_H
#define SCUMM_REMASTER_PALETTE_H

// Nearest-colour mapping for the retro display modes, without engine dependencies (also built by the stand-alone
// test dists/remaster/tests/palette_test.cpp).

#include <string.h>

namespace Scumm {

// Weighted RGB distance used by the retro modes (green counts most, blue least).
inline int remasterColorDist(int r, int g, int b, const unsigned char *p) {
	const int dr = r - p[0], dg = g - p[1], db = b - p[2];
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

// Exact nearest palette entry (first of equals). A colour that is in the palette maps to an entry with that colour.
inline int remasterNearestColor(const unsigned char *pal, int n, int r, int g, int b) {
	int best = 0, bestD = 1 << 30;
	for (int i = 0; i < n; i++) {
		const int d = remasterColorDist(r, g, b, pal + i * 3);
		if (d < bestD) {
			bestD = d;
			best = i;
			if (!d)
				break;
		}
	}
	return best;
}

// The old approximation: a 16x16x16 table of the colours i * 17 per channel, addressed by value >> 4. Kept for the
// regression test only: it changes colours that are in the palette (e.g. 15,15,15 -> 0,0,0).
inline int remasterNearestColor4bit(const unsigned char *pal, int n, int r, int g, int b) {
	return remasterNearestColor(pal, n, (r >> 4) * 17, (g >> 4) * 17, (b >> 4) * 17);
}

// Exact nearest-colour lookups, cached per 24-bit colour (16 MB of indices plus a 2 MB "known" bitset, allocated on
// first use). reset() is called when the palette's colours change; each colour is then searched once.
class RemasterColorMap {
public:
	RemasterColorMap() {}
	~RemasterColorMap() {
		delete[] _idx;
		delete[] _known;
	}
	void reset(const unsigned char *pal, int n) {
		memcpy(_pal, pal, n * 3);
		_n = n;
		if (!_idx) {
			_idx = new unsigned char[1 << 24];
			_known = new unsigned int[1 << 19];
		}
		memset(_known, 0, (1 << 19) * sizeof(unsigned int));
		_misses = 0;
	}
	int map(int r, int g, int b) {
		const unsigned int k = (unsigned int)r << 16 | (unsigned int)g << 8 | (unsigned int)b;
		if (_known[k >> 5] & (1u << (k & 31)))
			return _idx[k];
		const int i = remasterNearestColor(_pal, _n, r, g, b);
		_idx[k] = (unsigned char)i;
		_known[k >> 5] |= 1u << (k & 31);
		_misses++;
		return i;
	}
	const unsigned char *color(int i) const { return _pal + i * 3; }
	int size() const { return _n; }
	unsigned int misses() const { return _misses; }   // searches since reset (for timing/tests)

private:
	RemasterColorMap(const RemasterColorMap &);
	RemasterColorMap &operator=(const RemasterColorMap &);
	unsigned char _pal[256 * 3] = {};
	int _n = 0;
	unsigned char *_idx = nullptr;
	unsigned int *_known = nullptr;
	unsigned int _misses = 0;
};

} // End of namespace Scumm

#endif
