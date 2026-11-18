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

#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "scumm/remaster_tiles.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace Scumm {

struct RemasterTileEntry {
	std::vector<uint32> px;
	int w = 0, h = 0;
	uint64 lastUse = 0;
};

class RemasterTileCache {
public:
	std::unordered_map<uint64, RemasterTileEntry> map;
	size_t maxBytes = 0, bytes = 0;
	uint64 clock = 0, hits = 0, misses = 0;

	void evict() {
		// drop the least recently used quarter (in one go, so this does not run on every insertion)
		std::vector<uint64> uses;
		uses.reserve(map.size());
		for (const auto &e : map)
			uses.push_back(e.second.lastUse);
		if (uses.empty())
			return;
		const size_t cut = uses.size() / 4;
		std::nth_element(uses.begin(), uses.begin() + cut, uses.end());
		const uint64 limit = uses[cut];
		for (auto it = map.begin(); it != map.end();) {
			if (it->second.lastUse <= limit) {
				bytes -= it->second.px.size() * 4;
				it = map.erase(it);
			} else {
				++it;
			}
		}
	}
};

RemasterTileCache *remasterTileCacheCreate(size_t maxBytes) {
	RemasterTileCache *c = new RemasterTileCache();
	c->maxBytes = maxBytes;
	return c;
}

void remasterTileCacheDestroy(RemasterTileCache *c) {
	delete c;
}

void remasterTileCacheClear(RemasterTileCache *c) {
	if (!c)
		return;
	c->map.clear();
	c->bytes = 0;
}

bool remasterTileCacheGet(RemasterTileCache *c, uint64 key, uint32 *dst, int pitch, int w, int h) {
	if (!c)
		return false;
	auto it = c->map.find(key);
	if (it == c->map.end() || it->second.w != w || it->second.h != h) {
		c->misses++;
		return false;
	}
	it->second.lastUse = ++c->clock;
	const uint32 *s = it->second.px.data();
	for (int y = 0; y < h; y++)
		memcpy(dst + (size_t)y * pitch, s + (size_t)y * w, (size_t)w * 4);
	c->hits++;
	return true;
}

void remasterTileCachePut(RemasterTileCache *c, uint64 key, const uint32 *src, int pitch, int w, int h) {
	if (!c || !c->maxBytes)
		return;
	RemasterTileEntry &e = c->map[key];
	if (e.px.empty())
		c->bytes += (size_t)w * h * 4;
	e.w = w;
	e.h = h;
	e.px.resize((size_t)w * h);
	for (int y = 0; y < h; y++)
		memcpy(e.px.data() + (size_t)y * w, src + (size_t)y * pitch, (size_t)w * 4);
	e.lastUse = ++c->clock;
	if (c->bytes > c->maxBytes)
		c->evict();
}

void remasterTileCacheStats(RemasterTileCache *c, uint64 &hits, uint64 &misses, size_t &bytes) {
	hits = c ? c->hits : 0;
	misses = c ? c->misses : 0;
	bytes = c ? c->bytes : 0;
	if (c)
		c->hits = c->misses = 0;
}

size_t remasterTileCacheBytes(const RemasterTileCache *c) {
	return c ? c->bytes : 0;
}

uint64 remasterHashRect(const uint32 *img, int pitch, int x0, int y0, int x1, int y1, uint64 seed) {
	// FNV-style mixing of 64-bit words; fast, and collisions are astronomically unlikely for this use
	uint64 h = seed ^ 0x9E3779B97F4A7C15ULL;
	h = (h ^ (uint64)(x1 - x0 + 1)) * 0x100000001B3ULL;
	h = (h ^ (uint64)(y1 - y0 + 1)) * 0x100000001B3ULL;
	for (int y = y0; y <= y1; y++) {
		const uint32 *r = img + (size_t)y * pitch + x0;
		const int n = x1 - x0 + 1;
		int x = 0;
		for (; x + 1 < n; x += 2) {
			uint64 v;
			memcpy(&v, r + x, 8);
			h = (h ^ v) * 0x100000001B3ULL;
			h ^= h >> 29;
		}
		if (x < n)
			h = (h ^ r[x]) * 0x100000001B3ULL;
	}
	return h;
}

} // End of namespace Scumm
