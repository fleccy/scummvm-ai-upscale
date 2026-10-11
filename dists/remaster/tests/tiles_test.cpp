// Stand-alone tests for the AI tile cache (engines/scumm/remaster_tiles.cpp): fingerprints and the cache itself.
//   g++ -O2 -std=c++11 -Idists/remaster/tests/stub -I. -Iengines dists/remaster/tests/tiles_test.cpp engines/scumm/remaster_tiles.cpp -o tiles_test && ./tiles_test
#include "engines/scumm/remaster_tiles.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Scumm;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

int main() {
	const int W = 64, H = 48;
	std::vector<uint32> img(W * H);
	srand(7);
	for (auto &p : img)
		p = (uint32)rand() * 2654435761u;

	// 1. Fingerprints
	{
		const uint64 a = remasterHashRect(img.data(), W, 4, 4, 35, 35, 1);
		const uint64 b = remasterHashRect(img.data(), W, 4, 4, 35, 35, 1);
		CHECK(a == b, "same pixels give the same fingerprint");
		std::vector<uint32> img2(img);
		img2[20 * W + 20] ^= 1;
		CHECK(remasterHashRect(img2.data(), W, 4, 4, 35, 35, 1) != a, "one changed pixel changes the fingerprint");
		img2 = img;
		img2[2 * W + 2] ^= 1;   // outside the area
		CHECK(remasterHashRect(img2.data(), W, 4, 4, 35, 35, 1) == a, "a change outside the area does not");
		CHECK(remasterHashRect(img.data(), W, 4, 4, 36, 35, 1) != a, "a different area size gives a different fingerprint");
		CHECK(remasterHashRect(img.data(), W, 4, 4, 35, 35, 2) != a, "a different seed (upscaler, scale) gives a different one");
		// the same content at another position (repeating pattern): same fingerprint, as intended
		std::vector<uint32> tiled(W * H);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++)
				tiled[y * W + x] = img[(y % 16) * W + (x % 16)];
		CHECK(remasterHashRect(tiled.data(), W, 0, 0, 15, 15, 3) == remasterHashRect(tiled.data(), W, 16, 16, 31, 31, 3),
		      "identical content elsewhere gives the same fingerprint");
	}

	// 2. The cache
	{
		const int TW = 24, TH = 16;
		RemasterTileCache *c = remasterTileCacheCreate((size_t)TW * TH * 4 * 8);   // room for 8 tiles
		std::vector<uint32> out(W * H, 0);
		CHECK(!remasterTileCacheGet(c, 42, out.data(), W, TW, TH), "an empty cache misses");
		remasterTileCachePut(c, 42, img.data() + 3 * W + 5, W, TW, TH);
		CHECK(remasterTileCacheGet(c, 42, out.data() + 10 * W + 20, W, TW, TH), "a stored tile is found");
		bool same = true;
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				same = same && out[(10 + y) * W + 20 + x] == img[(3 + y) * W + 5 + x];
		CHECK(same, "and comes back exactly, at the given place");
		CHECK(!remasterTileCacheGet(c, 42, out.data(), W, TW + 1, TH), "a different tile size misses");
		// fill well past the limit while using tile 42 all along: it must survive, the oldest others must go
		for (int k = 0; k < 40; k++) {
			remasterTileCachePut(c, 1000 + k, img.data(), W, TW, TH);
			remasterTileCacheGet(c, 42, out.data(), W, TW, TH);
		}
		uint64 hits, misses;
		size_t bytes;
		remasterTileCacheStats(c, hits, misses, bytes);
		CHECK(bytes <= (size_t)TW * TH * 4 * 8, "stays within its memory limit (%zu bytes)", bytes);
		CHECK(remasterTileCacheGet(c, 42, out.data(), W, TW, TH), "a tile in constant use survives eviction");
		CHECK(!remasterTileCacheGet(c, 1000, out.data(), W, TW, TH), "the least recently used ones are evicted");
		CHECK(remasterTileCacheGet(c, 1039, out.data(), W, TW, TH), "the newest is kept");
		remasterTileCacheStats(c, hits, misses, bytes);
		CHECK(hits == 2 && misses == 1, "statistics count hits and misses (%d, %d)", (int)hits, (int)misses);
		remasterTileCacheClear(c);
		remasterTileCacheStats(c, hits, misses, bytes);
		CHECK(bytes == 0 && !remasterTileCacheGet(c, 42, out.data(), W, TW, TH), "clearing empties it");
		remasterTileCacheDestroy(c);
		RemasterTileCache *off = remasterTileCacheCreate(0);
		remasterTileCachePut(off, 1, img.data(), W, TW, TH);
		CHECK(!remasterTileCacheGet(off, 1, out.data(), W, TW, TH), "a cache with no memory stores nothing");
		remasterTileCacheDestroy(off);
	}
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures ? 1 : 0;
}
