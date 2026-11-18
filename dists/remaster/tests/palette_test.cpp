// Stand-alone regression test for the retro modes' colour mapping (engines/scumm/remaster_palette.h).
//   g++ -O2 -std=c++11 -I. dists/remaster/tests/palette_test.cpp -o palette_test && ./palette_test [frame.txt]
// frame.txt (optional, from a captured game frame): "P r g b" palette lines and "A r g b" 2x2 block averages; it
// measures how often the old 4-bit table and the exact mapping disagree on real data.
#include "engines/scumm/remaster_palette.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Scumm;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static bool sameColor(const unsigned char *a, const unsigned char *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2]; }

int main(int argc, char **argv) {
	// 1. The reported case: black, (15,15,15), white; a uniform (15,15,15) patch must stay (15,15,15).
	{
		const unsigned char pal[] = {0, 0, 0, 15, 15, 15, 255, 255, 255};
		const int old = remasterNearestColor4bit(pal, 3, 15, 15, 15);
		RemasterColorMap m;
		m.reset(pal, 3);
		const int exact = m.map(15, 15, 15);
		printf("fixture (15,15,15): old table -> entry %d (%d,%d,%d), exact -> entry %d (%d,%d,%d)\n", old, pal[old * 3],
		       pal[old * 3 + 1], pal[old * 3 + 2], exact, pal[exact * 3], pal[exact * 3 + 1], pal[exact * 3 + 2]);
		CHECK(exact == 1, "(15,15,15) must map to itself");
		CHECK(old != 1, "the old table was expected to reproduce the reported bug");
	}
	// 2. Every palette entry maps to its own colour (random palettes, and one of smooth ramps).
	{
		srand(1);
		for (int t = 0; t < 50; t++) {
			unsigned char pal[256 * 3];
			for (int i = 0; i < 256 * 3; i++)
				pal[i] = t == 0 ? (unsigned char)((i / 3) * ((i % 3) + 1) / 3) : (unsigned char)(rand() & 255);
			RemasterColorMap m;
			m.reset(pal, 256);
			int bad = 0, oldBad = 0;
			for (int i = 0; i < 256; i++) {
				bad += !sameColor(m.color(m.map(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2])), pal + i * 3);
				oldBad += !sameColor(pal + remasterNearestColor4bit(pal, 256, pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]) * 3, pal + i * 3);
			}
			CHECK(bad == 0, "palette %d: %d entries not preserved", t, bad);
			if (t < 2)
				printf("palette %d: entries changed by the old table %d of 256, by the exact map %d\n", t, oldBad, bad);
		}
	}
	// 3. Bucket boundaries and gradients: the cached map equals a brute-force search everywhere.
	{
		srand(2);
		unsigned char pal[256 * 3];
		for (int i = 0; i < 256 * 3; i++)
			pal[i] = (unsigned char)(rand() & 255);
		RemasterColorMap m;
		m.reset(pal, 256);
		int bad = 0;
		for (int v = 0; v < 256; v++) {
			const int cases[][3] = {{v, v, v}, {v, 0, 255 - v}, {15, v, 16}, {v, 31, 32}, {255 - v, v, 128}};
			for (auto &c : cases)
				for (int rep = 0; rep < 2; rep++)   // second lookup comes from the cache
					bad += m.map(c[0], c[1], c[2]) != remasterNearestColor(pal, 256, c[0], c[1], c[2]);
		}
		CHECK(bad == 0, "%d boundary/gradient lookups differ from brute force", bad);
	}
	// 4. A palette change (fade) invalidates the cache.
	{
		unsigned char a[] = {0, 0, 0, 200, 200, 200}, b[] = {0, 0, 0, 100, 100, 100};
		RemasterColorMap m;
		m.reset(a, 2);
		CHECK(m.map(90, 90, 90) == 0, "before the fade");
		m.reset(b, 2);
		CHECK(m.map(90, 90, 90) == 1, "after the fade the cached answer must not be reused");
	}
	// 5. Real data (optional): palette and 2x2 averages of a captured frame.
	if (argc > 1) {
		FILE *f = fopen(argv[1], "r");
		std::vector<unsigned char> pal;
		std::vector<int> avg;
		char k;
		int r, g, b;
		while (f && fscanf(f, " %c %d %d %d", &k, &r, &g, &b) == 4) {
			if (k == 'P') { pal.push_back(r); pal.push_back(g); pal.push_back(b); }
			else { avg.push_back(r); avg.push_back(g); avg.push_back(b); }
		}
		if (f)
			fclose(f);
		const int n = (int)pal.size() / 3, blocks = (int)avg.size() / 3;
		RemasterColorMap m;
		m.reset(pal.data(), n);
		int differ = 0, exactInPal = 0, oldBrokeExact = 0;
		const auto t0 = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < blocks; i++)
			m.map(avg[i * 3], avg[i * 3 + 1], avg[i * 3 + 2]);
		const double first = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
		const auto t1 = std::chrono::high_resolution_clock::now();
		for (int i = 0; i < blocks; i++)
			m.map(avg[i * 3], avg[i * 3 + 1], avg[i * 3 + 2]);
		const double again = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t1).count();
		for (int i = 0; i < blocks; i++) {
			const int *c = &avg[i * 3];
			const unsigned char *e = m.color(m.map(c[0], c[1], c[2]));
			const unsigned char *o = &pal[remasterNearestColor4bit(pal.data(), n, c[0], c[1], c[2]) * 3];
			differ += !sameColor(e, o);
			const bool inPal = e[0] == c[0] && e[1] == c[1] && e[2] == c[2];
			exactInPal += inPal;
			oldBrokeExact += inPal && !sameColor(o, e);
		}
		printf("real frame: %d colours, %d blocks; old table differs from exact on %d blocks (%.1f%%); blocks whose colour is "
		       "in the palette: %d, of which the old table changed %d\n", n, blocks, differ, 100.0 * differ / blocks, exactInPal, oldBrokeExact);
		printf("timing: first pass %.2f ms (%u searches), cached pass %.2f ms\n", first, m.misses(), again);
		CHECK(blocks > 0, "no real-frame data");
	}
	printf(failures ? "%d FAILURES\n" : "all palette checks passed\n", failures);
	return failures ? 1 : 0;
}
