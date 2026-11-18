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

// ncnn (BSD-3-Clause) uses the C/C++ standard library directly.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "scumm/remaster_ai.h"
#include "scumm/remaster_ort.h"
#include "common/config-manager.h"
#include "common/textconsole.h"

#include <cstdio>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#ifdef WIN32
#include <windows.h>
#elif defined(SDL_BACKEND)
#include <SDL.h>
#endif

#ifdef USE_REMASTER_AI
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

#include "ncnn/command.h"
#include "ncnn/gpu.h"
#include "ncnn/layer.h"
#include "ncnn/layer_type.h"
#include "ncnn/pipeline.h"
#include "ncnn/net.h"
#endif

namespace Scumm {

// ---- small thread pool for the renderer's per-row CPU work
namespace {
thread_local bool t_inParallel = false;   // inside a parallel chunk: nested calls run serially (no deadlock)
struct ParPool {
	// Every call is joined by every worker (each one checks in when it has finished), so no worker can still be
	// working on an old call when the next one starts.
	std::mutex m;
	std::condition_variable cv, doneCv;
	std::vector<std::thread> workers;
	void (*fn)(void *, int, int) = nullptr;
	void *ctx = nullptr;
	int n = 0, chunk = 1;
	std::atomic<int> next{0};
	size_t finished = 0;
	unsigned gen = 0;
	bool quit = false;

	static void runChunks(std::atomic<int> &nx, int n, int chunk, void (*fn)(void *, int, int), void *ctx) {
		for (;;) {
			const int a = nx.fetch_add(chunk);
			if (a >= n)
				break;
			fn(ctx, a, a + chunk < n ? a + chunk : n);
		}
	}
	void worker() {
		unsigned seen = 0;
		for (;;) {
			void (*f)(void *, int, int);
			void *c;
			int cn, ch;
			{
				std::unique_lock<std::mutex> lk(m);
				cv.wait(lk, [&] { return quit || gen != seen; });
				if (quit)
					return;
				seen = gen;
				f = fn;
				c = ctx;
				cn = n;
				ch = chunk;
			}
			t_inParallel = true;
			runChunks(next, cn, ch, f, c);
			t_inParallel = false;
			{
				std::lock_guard<std::mutex> lk(m);
				finished++;
			}
			doneCv.notify_all();
		}
	}
	void start() {
		unsigned hw = std::thread::hardware_concurrency();
		int count = hw > 1 ? (int)hw - 1 : 0;
		if (count > 11)
			count = 11;
		for (int i = 0; i < count; i++)
			workers.emplace_back([this] { worker(); });
	}
	void stop() {
		{
			std::lock_guard<std::mutex> lk(m);
			quit = true;
		}
		cv.notify_all();
		for (size_t i = 0; i < workers.size(); i++)
			workers[i].join();
		workers.clear();
		quit = false;
	}
};
ParPool *g_parPool = nullptr;
std::mutex g_parCall;
} // anonymous namespace

void remasterParallelFor(int n, void (*fn)(void *, int, int), void *ctx) {
	if (n <= 0)
		return;
	if (t_inParallel) {
		fn(ctx, 0, n);
		return;
	}
	std::lock_guard<std::mutex> call(g_parCall);
	if (!g_parPool) {
		g_parPool = new ParPool();
		g_parPool->start();
	}
	ParPool &p = *g_parPool;
	if (p.workers.empty() || n < 8) {
		fn(ctx, 0, n);
		return;
	}
	int chunk;
	{
		std::lock_guard<std::mutex> lk(p.m);
		p.fn = fn;
		p.ctx = ctx;
		p.n = n;
		const int parts = (int)(p.workers.size() + 1) * 4;
		p.chunk = chunk = n / parts > 0 ? n / parts : 1;
		p.next = 0;
		p.finished = 0;
		p.gen++;
	}
	p.cv.notify_all();
	t_inParallel = true;
	ParPool::runChunks(p.next, n, chunk, fn, ctx);
	t_inParallel = false;
	std::unique_lock<std::mutex> lk(p.m);
	p.doneCv.wait(lk, [&] { return p.finished == p.workers.size(); });
}

int remasterDesktopHeight() {
#ifdef WIN32
	DEVMODEW dm;
	memset(&dm, 0, sizeof(dm));
	dm.dmSize = sizeof(dm);
	if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm))
		return (int)dm.dmPelsHeight;
	return 0;
#elif defined(SDL_BACKEND)
	SDL_DisplayMode m;
	if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_GetCurrentDisplayMode(0, &m) == 0)
		return m.h;
	return 0;
#else
	return 0;
#endif
}

double remasterNowMs() {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void remasterParallelShutdown() {
	if (g_parPool) {
		g_parPool->stop();
		delete g_parPool;
		g_parPool = nullptr;
	}
}

// ---- the real-time AI networks (remaster_ai_net)
namespace {
struct RemasterAINetInfo {
	const char *id, *label, *name;   // config value, menu label, upscaler name shown in game
	const char *file;                // ncnn model next to the default one (nullptr: the default model itself)
	const char *inBlob, *outBlob;
	int scale;                       // the network's own scale
	int prepad, align;               // input edge padding the network crops off itself, input size alignment
	bool wholeFrame;                 // global pooling: the result depends on the whole picture
	const char *onnx;                // x3 ONNX version for DirectML / TensorRT (dists/remaster/ncnn2onnx.py), or nullptr
};
// Real-CUGAN (bilibili, MIT; ncnn conversion by nihui, MIT): a cascade of U-Nets with valid convolutions that crop
// 14 px of input from each side at x3, two stride-2 stages (input size a multiple of 4) and squeeze-and-excitation
// blocks (global average pooling).
const RemasterAINetInfo kAINets[] = {
	{"default", "Real-ESRGAN animevideo v3 (standard)", "Real-ESRGAN AI (ncnn)", nullptr, "data", "output", 3, 0, 1, false, "realesr-animevideov3-x3-fp16.onnx"},
	{"futsuu", "2x Futsuu-Anime (less dither speckle)", "Futsuu-Anime AI (ncnn)", "2x-Futsuu-Anime", "data", "output", 2, 0, 1, false, "2x-Futsuu-Anime-x3-fp16.onnx"},
	{"cugan", "Real-CUGAN 3x conservative (whole frames, slower)", "Real-CUGAN AI (ncnn)", "realcugan-up3x-conservative", "in0", "out0", 3, 14, 4, true, nullptr},
	{"starsample", "2x StarSample V2.0 Lite (clean art)", "StarSample AI (ncnn)", "2x-StarSample-V2.0-Lite", "data", "output", 2, 0, 1, false, nullptr},
	// ArtCNN works on luma only: its ncnn graph (dists/remaster/convert_models.py) converts RGB to Y for the network
	// and resizes Cb, Cr bicubically beside it
	{"artcnn", "ArtCNN C4F32 (lightest)", "ArtCNN AI (ncnn)", "ArtCNN_C4F32", "data", "output", 2, 0, 1, false, nullptr},
	// Sirosky's anime models (CC BY-NC 4.0), SRVGGNetCompact 2x like Futsuu-Anime: as fast as the standard network
	{"aniscale2s", "2x AniScale2S Compact (crisp lines)", "AniScale2S AI (ncnn)", "2x-AniScale2S-Compact", "data", "output", 2, 0, 1, false,
	 "2x-AniScale2S-Compact-x3-fp16.onnx"},
	{"anisd", "2x AniSD Compact (low-resolution art)", "AniSD AI (ncnn)", "2x-AniSD-G6i1b-Compact", "data", "output", 2, 0, 1, false,
	 "2x-AniSD-G6i1b-Compact-x3-fp16.onnx"},
};
const int kAINetCount = (int)ARRAYSIZE(kAINets);

// HD background models (remaster_bg_net)
struct RemasterBgNetInfo {
	const char *id, *label;
	const char *ncnn;                // ncnn model (base name), or nullptr
	const char *onnx;                // ONNX model of fixed input size tile x tile (ONNX Runtime only), or nullptr
	int tile;
};
// 4x UltraSharpV2 (Kim2091, CC BY-NC-SA 4.0): DAT2, whose windowed attention ncnn cannot run, so ONNX only, exported
// at a fixed tile size and computed in fp32 (fp16 overflows to NaN in its attention; the weights are stored as fp16 and
// cast, which halves the file and changes no output pixel by more than 1). Much heavier than x4plus anime (seconds per
// room), but it runs once per room and the result is cached.
const RemasterBgNetInfo kBgNets[] = {
	{"x4anime", "Real-ESRGAN x4plus anime (fast)", "realesrgan-x4plus-anime", nullptr, 0},
	{"ultrasharpv2", "4x UltraSharpV2 (sharper; DirectML / TensorRT)", nullptr, "4x-UltraSharpV2-t128.onnx", 128},
};
const int kBgNetCount = (int)ARRAYSIZE(kBgNets);
} // anonymous namespace

int remasterBgNetCount() { return kBgNetCount; }
const char *remasterBgNetId(int bgNet) { return kBgNets[CLIP<int>(bgNet, 0, kBgNetCount - 1)].id; }
const char *remasterBgNetLabel(int bgNet) { return kBgNets[CLIP<int>(bgNet, 0, kBgNetCount - 1)].label; }
int remasterBgNetIndex(const Common::String &id) {
	for (int i = 0; i < kBgNetCount; i++)
		if (id.equalsIgnoreCase(kBgNets[i].id))
			return i;
	return -1;
}

int remasterAINetCount() { return kAINetCount; }
const char *remasterAINetId(int net) { return kAINets[CLIP<int>(net, 0, kAINetCount - 1)].id; }
const char *remasterAINetLabel(int net) { return kAINets[CLIP<int>(net, 0, kAINetCount - 1)].label; }
int remasterAINetIndex(const Common::String &id) {
	for (int i = 0; i < kAINetCount; i++)
		if (id.equalsIgnoreCase(kAINets[i].id))
			return i;
	return -1;
}

// <folder of modelBase><file> for another network, modelBase itself for the default one
static Common::String remasterAINetBase(const Common::String &modelBase, int net) {
	if (net <= 0 || net >= kAINetCount || !kAINets[net].file)
		return modelBase;
	const char *sl = MAX(strrchr(modelBase.c_str(), '/'), strrchr(modelBase.c_str(), '\\'));
	return Common::String(modelBase.c_str(), sl ? sl + 1 : modelBase.c_str()) + kAINets[net].file;
}

static Common::String remasterAIModelDir(const Common::String &modelBase) {
	const char *sl = MAX(strrchr(modelBase.c_str(), '/'), strrchr(modelBase.c_str(), '\\'));
	return Common::String(modelBase.c_str(), sl ? sl + 1 : modelBase.c_str());
}

bool remasterAINetHasOnnx(const Common::String &modelBase, int net) {
	if (net < 0 || net >= kAINetCount || !kAINets[net].onnx)
		return false;
	FILE *f = fopen((remasterAIModelDir(modelBase) + kAINets[net].onnx).c_str(), "rb");
	if (f)
		fclose(f);
	return f != nullptr;
}

bool remasterAINetAvailable(const Common::String &modelBase, int net) {
	if (net < 0 || net >= kAINetCount)
		return false;
	const Common::String base = remasterAINetBase(modelBase, net);
	for (const char *ext : {".param", ".bin"}) {
		FILE *f = fopen((base + ext).c_str(), "rb");
		if (!f)
			return false;
		fclose(f);
	}
	return true;
}

static bool remasterFileExists(const Common::String &path) {
	FILE *f = fopen(path.c_str(), "rb");
	if (f)
		fclose(f);
	return f != nullptr;
}

// test aid: remaster_bg_tile picks another export of the ONNX model ("-t<size>.onnx")
static int remasterBgNetTile(int bgNet, Common::String *file) {
	const RemasterBgNetInfo &b = kBgNets[bgNet];
	int tile = b.tile;
	Common::String f = b.onnx ? b.onnx : "";
	if (b.onnx && ConfMan.hasKey("remaster_bg_tile")) {
		const int t = ConfMan.getInt("remaster_bg_tile");
		const char *at = strstr(b.onnx, "-t");
		if (t >= 64 && t <= 512 && at) {
			tile = t;
			f = Common::String(b.onnx, at) + Common::String::format("-t%d.onnx", t);
		}
	}
	if (file)
		*file = f;
	return tile;
}

bool remasterBgNetAvailable(const Common::String &modelBase, int bgNet) {
	if (bgNet < 0 || bgNet >= kBgNetCount)
		return false;
	const RemasterBgNetInfo &b = kBgNets[bgNet];
	const Common::String dir = remasterAIModelDir(modelBase);
	if (b.ncnn)
		return remasterFileExists(dir + b.ncnn + ".param") && remasterFileExists(dir + b.ncnn + ".bin");
#if defined(USE_REMASTER_AI) && defined(USE_REMASTER_ORT)
	Common::String file;
	remasterBgNetTile(bgNet, &file);
	return remasterFileExists(dir + file);
#else
	return false;
#endif
}

#ifdef USE_REMASTER_AI

class RemasterAI {
public:
	enum Kind { kNcnn, kFsr, kOrt };
	Kind kind = kNcnn;
	int net = -1;                       // kNcnn, kOrt: index into kAINets; -1 a Real-ESRGAN style model of learnt scale
	int bgNet = -1;                     // made by remasterBgAICreate: index into kBgNets
	int fixedTile = 0;                  // kOrt: the model takes only fixedTile x fixedTile images
	mutable Common::String nameBuf;     // remasterAIBackendName() of another network on DirectML / TensorRT
	ncnn::Net ncnnNet;                  // kNcnn: the network
	ncnn::Pipeline *toRgba = nullptr;   // kNcnn: network output -> RGBA bytes, on the GPU
	std::mutex toRgbaMutex;
	bool toRgbaFailed = false;
	bool toRgbaChecked = false;          // first use is compared with the CPU conversion (other GPUs / drivers)
	const ncnn::VulkanDevice *vkdev = nullptr;   // kFsr
	ncnn::Pipeline *easu = nullptr, *rcas = nullptr;
	float fsrSharpness = 0.25f;         // RCAS: 0 = sharpest, each +1 halves the sharpening
	RemasterOrt *ort = nullptr;         // kOrt: ONNX Runtime (DirectML or TensorRT)
	RemasterOrtProvider ortProvider = kOrtDirectML;
	bool ortUltra = false;
	RemasterAI *standIn = nullptr;      // TensorRT: the ncnn upscaler while the engines are being prepared
	RemasterOrt *ortWhole = nullptr;    // Ultra: light network for whole frames (the heavy one is too slow for those)
	bool ortLight = false;              // Ultra: light network for everything in this room
	// Ultra on TensorRT: cutscenes (whole frames, nothing else to draw) get the heavy network too, prepared on first use
	RemasterOrt *ortHeavyWhole = nullptr;
	Common::String heavyPath;
	int fullW = 0, fullH = 0;
	bool video = false, heavyShown = false;
	// The network's own scale (the models are x3; the HD background model is x4). Other display scales are made from
	// its output by remasterResample. 0 until known (ncnn: learnt from the first output; FSR scales to anything).
	int nativeScale = 0;
	// kNcnn, x2 networks shown at x3 / x4: bicubic resize of the network output on the GPU (made on first use)
	ncnn::Layer *resize = nullptr;
	ncnn::Option resizeOpt;
	int resizeScale = 0;
	bool resizeFailed = false;
	~RemasterAI() {
		if (resize) {
			resize->destroy_pipeline(resizeOpt);
			delete resize;
		}
		remasterOrtDestroy(ort);
		remasterOrtDestroy(ortWhole);
		remasterOrtDestroy(ortHeavyWhole);
		remasterAIDestroy(standIn);
		delete toRgba;
		delete easu;
		delete rcas;
	}
};

#include "scumm/remaster_fsr.inc"

static bool remasterFsrRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out);

// Converts the network output (fp16 or fp32 planes as stored by ncnn, 0..1) to packed RGBA bytes on the GPU, so only 4 bytes per pixel come
// back instead of 12, and the CPU has no conversion to do (same truncation as ncnn's to_pixels).
static const char kToRgbaShader[] = R"(
#version 450
layout(binding = 0) readonly buffer bottom_blob { sfp bottom_blob_data[]; };
layout(binding = 1) writeonly buffer top_blob { uint top_blob_data[]; };
layout(push_constant) uniform parameter { int w; int h; int cstep; int sw; } p;
void main()
{
	const int gx = int(gl_GlobalInvocationID.x);
	const int gy = int(gl_GlobalInvocationID.y);
	if (gx >= p.w || gy >= p.h)
		return;
	const int i = gy * p.sw + gx;   // the network's output may be wider (cropped to w x h here)
	const uint r = uint(clamp(int(float(buffer_ld1(bottom_blob_data, i)) * 255.f), 0, 255));
	const uint g = uint(clamp(int(float(buffer_ld1(bottom_blob_data, p.cstep + i)) * 255.f), 0, 255));
	const uint b = uint(clamp(int(float(buffer_ld1(bottom_blob_data, 2 * p.cstep + i)) * 255.f), 0, 255));
	top_blob_data[gy * p.w + gx] = r | (g << 8) | (b << 16) | 0xff000000u;
}
)";

static int g_remasterAIInstances = 0;

// AMD FidelityFX Super Resolution 1: edge-adaptive spatial upscaling (EASU) and sharpening (RCAS) as two GPU passes.
static RemasterAI *remasterFsrCreate(Common::String &err) {
	if (g_remasterAIInstances++ == 0)
		ncnn::create_gpu_instance();
	if (ncnn::get_gpu_count() == 0) {
		err = "no Vulkan GPU found";
		remasterAIDestroy(nullptr);
		return nullptr;
	}
	RemasterAI *ai = new RemasterAI();
	ai->kind = RemasterAI::kFsr;
	ai->vkdev = ncnn::get_gpu_device(0);
	ncnn::Option opt;
	opt.use_vulkan_compute = true;
	opt.use_fp16_packed = opt.use_fp16_storage = opt.use_fp16_arithmetic = false;
	opt.use_bf16_storage = opt.use_bf16_packed = false;
	const char *src[2] = {kFsrEasuShader, kFsrRcasShader};
	ncnn::Pipeline **pl[2] = {&ai->easu, &ai->rcas};
	for (int k = 0; k < 2; k++) {
		std::vector<uint32_t> spirv;
		std::vector<ncnn::vk_specialization_type> spec;
		*pl[k] = new ncnn::Pipeline(ai->vkdev);
		(*pl[k])->set_optimal_local_size_xyz(16, 16, 1);
		if (ncnn::compile_spirv_module(src[k], opt, spirv) || (*pl[k])->create(spirv.data(), spirv.size() * 4, spec)) {
			err = "cannot build the FSR shaders";
			remasterAIDestroy(ai);
			return nullptr;
		}
	}
	// Sanity test (the shaders run on whatever GPU this is): the centre of each output block must stay close to its
	// source pixel on a small test picture with gradients and hard edges.
	{
		const int TW = 40, TH = 24, S = 3;
		std::vector<uint32_t> tin(TW * TH), tout(TW * TH * S * S);
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++) {
				const uint32_t r = x * 255 / (TW - 1), g = y * 255 / (TH - 1), b = ((x / 5 + y / 4) & 1) ? 230 : 20;
				tin[y * TW + x] = r | (g << 8) | (b << 16) | 0xff000000u;
			}
		double sum = 0;
		if (remasterFsrRun(ai, (const byte *)tin.data(), TW, TH, S, (byte *)tout.data())) {
			for (int y = 0; y < TH; y++)
				for (int x = 0; x < TW; x++) {
					const uint32_t a = tin[y * TW + x], o = tout[(y * S + 1) * TW * S + x * S + 1];
					for (int k = 0; k < 24; k += 8)
						sum += abs((int)((a >> k) & 255) - (int)((o >> k) & 255));
				}
			sum /= TW * TH * 3;
		} else {
			sum = 255;
		}
		if (sum > 12) {
			err = Common::String::format("FSR self-test failed on this GPU (mean error %.1f)", sum);
			remasterAIDestroy(ai);
			return nullptr;
		}
	}
	err = Common::String("GPU: ") + ncnn::get_gpu_info(0).device_name();
	return ai;
}

bool remasterAITensorRTBuilt(const Common::String &modelBase, int fullW, int fullH, int net) {
	net = CLIP(net, 0, kAINetCount - 1);
	return kAINets[net].onnx && remasterOrtTensorRTCached(remasterAIModelDir(modelBase) + kAINets[net].onnx, fullW, fullH);
}

RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW, int fullH, int net) {
	if (backend == "fsr")
		return remasterFsrCreate(err);
	const bool trt = backend == "trt" || backend == "trt-ultra";
	bool ultra = backend.hasSuffix("-ultra");
	if ((trt || backend == "dml" || backend == "dml-ultra") && net > 0) {
		if (!remasterAINetHasOnnx(modelBase, net)) {
			// no ONNX version of this network (dists/remaster/ncnn2onnx.py converts only some)
			warning("remaster: the %s AI model has no %s version, using ncnn (Vulkan) for it", remasterAINetId(net), backend.c_str());
			return remasterAICreate(modelBase, err, net);
		}
		if (ultra) {
			// Ultra means the heavy standard network for moving parts: with another model chosen, that model throughout
			warning("remaster: %s with the %s AI model: the model itself is used throughout (no Ultra)", backend.c_str(), remasterAINetId(net));
			ultra = false;
		}
	}
	if (trt || backend == "dml" || backend == "dml-ultra") {
		// ONNX models next to the ncnn model: the same light network, or the heavy one used for the HD backgrounds
		const Common::String dir = remasterAIModelDir(modelBase);
		const RemasterOrtProvider prov = trt ? kOrtTensorRT : kOrtDirectML;
		const Common::String light = dir + kAINets[MAX(net, 0)].onnx, heavy = dir + "realesrgan-x4plus-anime-x3-fp16.onnx";
		// Ultra: the heavy network for regions, the light one for whole frames (and rooms too heavy for Ultra)
		RemasterOrt *o = remasterOrtCreate(prov, ultra ? heavy : light, ultra ? 0 : fullW, ultra ? 0 : fullH, err);
		if (!o)
			return nullptr;
		RemasterAI *ai = new RemasterAI();
		ai->kind = RemasterAI::kOrt;
		ai->net = MAX(net, 0);
		ai->nativeScale = 3;   // the ONNX models are x3
		ai->ort = o;
		ai->ortProvider = prov;
		ai->ortUltra = ultra;
		ai->heavyPath = heavy;
		ai->fullW = fullW;
		ai->fullH = fullH;
		if (ultra) {
			Common::String e2;
			ai->ortWhole = remasterOrtCreate(prov, light, fullW, fullH, e2);
			if (trt && fullW > 0)   // cutscenes: the heavy network for whole frames, prepared in the background now
				ai->ortHeavyWhole = remasterOrtCreate(prov, heavy, fullW, fullH, e2);
		}
		if (trt) {
			Common::String e3;
			ai->standIn = remasterAICreate(modelBase, e3, MAX(net, 0));
		}
		return ai;
	}
	return remasterAICreate(modelBase, err, MAX(net, 0));
}

RemasterAI *remasterBgAICreate(const Common::String &modelBase, int bgNet, bool preferTensorRT, Common::String &err, int *usedNet) {
	bgNet = CLIP(bgNet, 0, kBgNetCount - 1);
	const Common::String dir = remasterAIModelDir(modelBase);
	if (kBgNets[bgNet].onnx) {
		Common::String file, e;
		const int tile = remasterBgNetTile(bgNet, &file);
		if (!remasterFileExists(dir + file)) {
			e = "file missing: " + file;
		} else {
			// TensorRT (optional download) when asked for and it loads; its engines are built on a worker thread the first
			// time (minutes; remasterAIPrepareState says when they are ready). Otherwise DirectML.
			RemasterOrt *o = nullptr;
			RemasterOrtProvider prov = kOrtDirectML;
			if (preferTensorRT) {
				o = remasterOrtCreate(kOrtTensorRT, dir + file, 0, 0, e, tile, true);
				prov = kOrtTensorRT;
			}
			if (!o) {
				o = remasterOrtCreate(kOrtDirectML, dir + file, 0, 0, e, tile, true);
				prov = kOrtDirectML;
			}
			if (o) {
				RemasterAI *ai = new RemasterAI();
				ai->kind = RemasterAI::kOrt;
				ai->net = -1;
				ai->bgNet = bgNet;
				ai->fixedTile = tile;
				ai->nativeScale = 4;
				ai->ort = o;
				ai->ortProvider = prov;
				if (prov == kOrtTensorRT) {
					// DirectML stands in while TensorRT builds its engine (first time: minutes), and for good if that fails:
					// the same model, so its pictures are the same (within rounding) and share the cache
					Common::String e2;
					if (RemasterOrt *d = remasterOrtCreate(kOrtDirectML, dir + file, 0, 0, e2, tile, true)) {
						RemasterAI *s = new RemasterAI();
						s->kind = RemasterAI::kOrt;
						s->bgNet = bgNet;
						s->fixedTile = tile;
						s->nativeScale = 4;
						s->ort = d;
						s->ortProvider = kOrtDirectML;
						ai->standIn = s;
					}
				}
				err = Common::String::format("%s on %s%s", file.c_str(), prov == kOrtTensorRT ? "TensorRT" : "DirectML",
				                             ai->standIn ? " (DirectML until its engine is ready)" : "");
				if (usedNet)
					*usedNet = bgNet;
				return ai;
			}
		}
		warning("remaster: HD background model %s unavailable (%s), using %s", kBgNets[bgNet].id, e.c_str(), kBgNets[0].id);
		bgNet = 0;
	}
	RemasterAI *ai = remasterAICreate(dir + kBgNets[bgNet].ncnn, err);
	if (ai) {
		ai->bgNet = bgNet;
		err = Common::String(kBgNets[bgNet].ncnn) + " on ncnn (" + err + ")";
	}
	if (usedNet)
		*usedNet = ai ? bgNet : -1;
	return ai;
}

int remasterBgNetOf(const RemasterAI *ai) {
	return ai ? ai->bgNet : -1;
}

bool remasterAIHasStandIn(const RemasterAI *ai) {
	return ai && ai->standIn;
}

int remasterAIJobTile(const RemasterAI *ai, int *pad) {
	if (!ai || ai->kind != RemasterAI::kOrt || ai->fixedTile <= 0)
		return 0;
	// the context fills the rest of the model's input: one ONNX Runtime run per tile, nothing padded out
	const int p = 16;
	if (pad)
		*pad = p;
	return ai->fixedTile - 2 * p;
}

int remasterAINetOf(const RemasterAI *ai) {
	return ai && ai->kind != RemasterAI::kFsr ? MAX(ai->net, 0) : 0;
}

bool remasterAIWholeFrameOnly(const RemasterAI *ai) {
	return ai && ai->kind == RemasterAI::kNcnn && ai->net >= 0 && kAINets[ai->net].wholeFrame;
}

bool remasterAIBackendIsAI(const RemasterAI *ai) {
	return ai && ai->kind != RemasterAI::kFsr;
}

int remasterAIPrepareState(const RemasterAI *ai) {
	if (!ai || ai->kind != RemasterAI::kOrt)
		return 1;
	int st = remasterOrtState(ai->ort);
	if (ai->ortWhole)
		st = MIN(st, remasterOrtState(ai->ortWhole));
	if (ai->ortHeavyWhole)
		st = MIN(st, remasterOrtState(ai->ortHeavyWhole));
	return st;
}

bool remasterAISetVideo(RemasterAI *ai, bool video) {
	if (!ai || ai->kind != RemasterAI::kOrt || !ai->ortUltra || ai->ortProvider != kOrtTensorRT)
		return false;
	ai->video = video;
	// true once when the heavy network becomes usable for this cutscene: the picture is then redone in full
	const bool usable = video && ai->ortHeavyWhole && remasterOrtState(ai->ortHeavyWhole) == 1;
	const bool changed = usable && !ai->heavyShown;
	ai->heavyShown = usable;
	return changed;
}

bool remasterAISetLight(RemasterAI *ai, bool light) {
	if (!ai || !ai->ortWhole)
		return false;
	ai->ortLight = light;
	return true;
}

const char *remasterAIBackendName(const RemasterAI *ai) {
	if (!ai)
		return "none";
	if (ai->kind == RemasterAI::kOrt && ai->net > 0) {
		// another network on DirectML / TensorRT: "<its name> (DirectML)" etc.
		const char *nm = kAINets[ai->net].name;
		const char *par = strstr(nm, " (");
		const Common::String base(nm, par ? par : nm + strlen(nm));
		const int st = remasterOrtState(ai->ort);
		if (ai->ortProvider == kOrtDirectML)
			ai->nameBuf = base + " (DirectML)";
		else if (st == 0)
			ai->nameBuf = base + " (TensorRT) - preparing, first time takes a few minutes";
		else if (st < 0)
			ai->nameBuf = "TensorRT failed - using " + Common::String(nm);
		else
			ai->nameBuf = base + " (TensorRT)";
		return ai->nameBuf.c_str();
	}
	if (ai->kind == RemasterAI::kOrt) {
		if (ai->ortProvider == kOrtDirectML)
			return ai->ortUltra ? "Real-ESRGAN Ultra AI (DirectML)" : "Real-ESRGAN AI (DirectML)";
		const int st = MIN(remasterOrtState(ai->ort), ai->ortWhole ? remasterOrtState(ai->ortWhole) : 1);
		if (st == 0)
			return ai->ortUltra ? "Ultra AI (TensorRT) - preparing, first time takes a few minutes" : "Real-ESRGAN AI (TensorRT) - preparing, first time takes a few minutes";
		if (st < 0)
			return "TensorRT failed - using Real-ESRGAN AI (ncnn)";
		return ai->ortUltra ? "Real-ESRGAN Ultra AI (TensorRT)" : "Real-ESRGAN AI (TensorRT)";
	}
	return ai->kind == RemasterAI::kFsr ? "AMD FSR 1 (no AI)" : kAINets[MAX(ai->net, 0)].name;
}

RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err, int net) {
	if (net >= kAINetCount)
		net = 0;
	if (g_remasterAIInstances++ == 0)
		ncnn::create_gpu_instance();
	if (ncnn::get_gpu_count() == 0) {
		err = "no Vulkan GPU found";
		remasterAIDestroy(nullptr);
		return nullptr;
	}
	RemasterAI *ai = new RemasterAI();
	ai->net = net;
	if (net >= 0)
		ai->nativeScale = kAINets[net].scale;   // known up front (otherwise learnt from the first output)
	ai->ncnnNet.opt.use_vulkan_compute = true;
	ai->ncnnNet.opt.use_fp16_packed = true;
	ai->ncnnNet.opt.use_fp16_storage = true;
	ai->ncnnNet.opt.use_fp16_arithmetic = false; // keeps colours stable (and measured no faster here)
	const Common::String base = net >= 0 ? remasterAINetBase(modelBase, net) : modelBase;
	if (ai->ncnnNet.load_param((base + ".param").c_str()) || ai->ncnnNet.load_model((base + ".bin").c_str())) {
		err = "cannot load model " + base;
		delete ai;
		remasterAIDestroy(nullptr);
		return nullptr;
	}
	err = Common::String("GPU: ") + ncnn::get_gpu_info(0).device_name();
	return ai;
}

// Resizes RGBA bytes (alpha left at 255): area average when shrinking (x3 -> x2), linear when enlarging (x3 -> x4)
// or bicubic (cubic: the x2 networks enlarged to x3 / x4 when the GPU resize is unavailable), separably. Each output
// pixel only depends on nearby input, so tiles resized on their own match a whole frame away from their edges (the
// AI regions have context margins for that).
static void remasterResample(const byte *in, int iw, int ih, byte *out, int ow, int oh, bool cubic = false) {
	struct Tap { int i; float w; };
	auto taps = [cubic](int n, int m, std::vector<std::vector<Tap> > &t) {
		t.resize(m);
		const float r = (float)n / m;
		for (int o = 0; o < m; o++) {
			t[o].clear();
			if (r >= 1.0f) {   // area: input span [o*r, (o+1)*r)
				const float a = o * r, b = a + r;
				for (int i = (int)a; i < n && i < b; i++) {
					const float cov = MIN<float>(b, i + 1) - MAX<float>(a, i);
					if (cov > 0)
						t[o].push_back(Tap{i, cov / r});
				}
			} else if (cubic) {   // bicubic (a = -0.75, as ncnn's GPU resize), pixel centres aligned
				const float c = (o + 0.5f) * r - 0.5f;
				const int i0 = (int)floorf(c);
				const float f = c - i0, A = -0.75f;
				auto k1 = [A](float x) { return ((A + 2) * x - (A + 3)) * x * x + 1; };          // |x| <= 1
				auto k2 = [A](float x) { return ((A * x - 5 * A) * x + 8 * A) * x - 4 * A; };    // 1 < |x| < 2
				const float wt[4] = {k2(1 + f), k1(f), k1(1 - f), k2(2 - f)};
				for (int k = 0; k < 4; k++)
					t[o].push_back(Tap{CLIP(i0 - 1 + k, 0, n - 1), wt[k]});
			} else {           // linear, pixel centres aligned
				const float c = (o + 0.5f) * r - 0.5f;
				const int i0 = (int)floorf(c);
				const float f = c - i0;
				t[o].push_back(Tap{CLIP(i0, 0, n - 1), 1 - f});
				t[o].push_back(Tap{CLIP(i0 + 1, 0, n - 1), f});
			}
		}
	};
	std::vector<std::vector<Tap> > tx, ty;
	taps(iw, ow, tx);
	taps(ih, oh, ty);
	std::vector<float> mid;
	mid.resize((size_t)ow * ih * 3);
	remasterParallel(ih, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++)
			for (int x = 0; x < ow; x++) {
				float acc[3] = {0, 0, 0};
				for (const Tap &tp : tx[x])
					for (int c = 0; c < 3; c++)
						acc[c] += in[((size_t)y * iw + tp.i) * 4 + c] * tp.w;
				for (int c = 0; c < 3; c++)
					mid[((size_t)y * ow + x) * 3 + c] = acc[c];
			}
	});
	remasterParallel(oh, [&](int y0, int y1) {
		for (int y = y0; y < y1; y++)
			for (int x = 0; x < ow; x++) {
				float acc[3] = {0, 0, 0};
				for (const Tap &tp : ty[y])
					for (int c = 0; c < 3; c++)
						acc[c] += mid[((size_t)tp.i * ow + x) * 3 + c] * tp.w;
				byte *d = out + ((size_t)y * ow + x) * 4;
				for (int c = 0; c < 3; c++)
					d[c] = (byte)CLIP<int>((int)(acc[c] + 0.5f), 0, 255);
				d[3] = 255;
			}
	});
}

// x2 networks at a larger display scale: a bicubic Interp layer resizes the network output on the GPU, before the
// colour conversion (much cheaper than resizing on the CPU afterwards). False: use the CPU resize.
static bool remasterAIGpuResize(RemasterAI *ai, int scale) {
	if (ai->kind != RemasterAI::kNcnn || ai->net < 0 || ai->nativeScale != 2 || scale <= 2 || ai->resizeFailed)
		return false;
	std::lock_guard<std::mutex> lk(ai->toRgbaMutex);
	if (ai->resize && ai->resizeScale == scale)
		return true;
	if (ai->resize) {
		ai->resize->destroy_pipeline(ai->resizeOpt);
		delete ai->resize;
		ai->resize = nullptr;
	}
	ncnn::Layer *l = ncnn::create_layer_vulkan(ncnn::LayerType::Interp);
	if (l) {
		ncnn::ParamDict pd;
		pd.set(0, 3);   // bicubic
		pd.set(1, (float)scale / ai->nativeScale);
		pd.set(2, (float)scale / ai->nativeScale);
		l->vkdev = ai->ncnnNet.vulkan_device();
		ai->resizeOpt = ai->ncnnNet.opt;
		if (l->load_param(pd) || l->create_pipeline(ai->resizeOpt)) {
			delete l;
			l = nullptr;
		}
	}
	if (!l) {
		ai->resizeFailed = true;
		warning("remaster: GPU resize unavailable, resizing x%d -> x%d on the CPU", ai->nativeScale, scale);
		return false;
	}
	ai->resize = l;
	ai->resizeScale = scale;
	return true;
}

bool remasterAIRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out, double *ms) {
	if (!ai)
		return false;
	const bool gpuResize = remasterAIGpuResize(ai, scale);
	if (ai->kind != RemasterAI::kFsr && ai->nativeScale > 0 && ai->nativeScale != scale && !gpuResize) {
		// display scale differs from the network's: run it at its own scale and resize
		const int ns = ai->nativeScale;
		std::vector<byte> tmp((size_t)w * ns * h * ns * 4);
		if (!remasterAIRun(ai, in, w, h, ns, tmp.data(), ms))
			return false;
		const std::chrono::high_resolution_clock::time_point r0 = std::chrono::high_resolution_clock::now();
		remasterResample(tmp.data(), w * ns, h * ns, out, w * scale, h * scale, ns == 2 && scale > ns);
		static int logged = 0;
		if ((long)w * h >= 200000 && logged++ < 3)
			warning("remaster: x%d -> x%d resize of %dx%d: %.1f ms", ns, scale, w, h,
			        std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - r0).count());
		return true;
	}
	const std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
	if (ai->kind == RemasterAI::kOrt && ai->standIn &&
	    (remasterOrtState(ai->ort) != 1 || (ai->ortWhole && remasterOrtState(ai->ortWhole) != 1)))
		return remasterAIRun(ai->standIn, in, w, h, scale, out, ms);   // TensorRT engines not ready (or failed)
	if (ai->kind == RemasterAI::kFsr || ai->kind == RemasterAI::kOrt) {
		const bool ok = ai->kind == RemasterAI::kFsr ? remasterFsrRun(ai, in, w, h, scale, out) :
			remasterOrtRun(ai->ortHeavyWhole && ai->video && (long)w * h >= 200000 && remasterOrtState(ai->ortHeavyWhole) == 1 ? ai->ortHeavyWhole :
			               ai->ortWhole && (ai->ortLight || (long)w * h >= 200000) ? ai->ortWhole : ai->ort, in, w, h, scale, out);
		if (ok && ms)
			*ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
		return ok;
	}
	// Networks that crop their input edges themselves (Real-CUGAN) get them padded by repeating the edge pixels, and
	// the input size rounded up to what the network needs; the extra output on the right and bottom is cropped off.
	const RemasterAINetInfo *ni = ai->net >= 0 ? &kAINets[ai->net] : nullptr;
	const int P = ni ? ni->prepad : 0, A = ni ? ni->align : 1;
	const int exW = (A - (w + 2 * P) % A) % A, exH = (A - (h + 2 * P) % A) % A;
	const int iw = w + 2 * P + exW, ih = h + 2 * P + exH;
	std::vector<byte> padded;
	const byte *pin = in;
	if (iw != w || ih != h) {
		padded.resize((size_t)iw * ih * 4);
		for (int y = 0; y < ih; y++) {
			const byte *srow = in + (size_t)CLIP(y - P, 0, h - 1) * w * 4;
			byte *drow = &padded[(size_t)y * iw * 4];
			for (int x = 0; x < iw; x++)
				memcpy(drow + x * 4, srow + CLIP(x - P, 0, w - 1) * 4, 4);
		}
		pin = padded.data();
	}
	ncnn::Mat src = ncnn::Mat::from_pixels(pin, ncnn::Mat::PIXEL_RGBA2RGB, iw, ih);
	const float norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
	src.substract_mean_normalize(nullptr, norm);
	const ncnn::VulkanDevice *vkdev = ai->ncnnNet.vulkan_device();
	std::unique_lock<std::mutex> initLock(ai->toRgbaMutex);
	if (!ai->toRgba && !ai->toRgbaFailed) {
		std::vector<uint32_t> spirv;
		ai->toRgba = new ncnn::Pipeline(vkdev);
		ai->toRgba->set_optimal_local_size_xyz(16, 16, 1);
		std::vector<ncnn::vk_specialization_type> spec;
		if (ncnn::compile_spirv_module(kToRgbaShader, ai->ncnnNet.opt, spirv) ||
		    ai->toRgba->create(spirv.data(), spirv.size() * 4, spec)) {
			delete ai->toRgba;
			ai->toRgba = nullptr;
			ai->toRgbaFailed = true;
			warning("remaster: GPU colour conversion unavailable, converting on the CPU");
		}
	}
	initLock.unlock();
	ncnn::VkAllocator *blobA = vkdev->acquire_blob_allocator();
	ncnn::VkAllocator *stagA = vkdev->acquire_staging_allocator();
	ncnn::Option opt = ai->ncnnNet.opt;
	opt.blob_vkallocator = blobA;
	opt.workspace_vkallocator = blobA;
	opt.staging_vkallocator = stagA;
	const int OW = w * scale, OH = h * scale;
	const int NW = (w + exW) * scale, NH = (h + exH) * scale;   // the network's output (cropped to OW x OH)
	const int RW = (w + exW) * (gpuResize ? ai->nativeScale : scale), RH = (h + exH) * (gpuResize ? ai->nativeScale : scale);   // before the GPU resize
	bool ok = false;
	{
		ncnn::Extractor ex = ai->ncnnNet.create_extractor();
		ex.set_blob_vkallocator(blobA);
		ex.set_workspace_vkallocator(blobA);
		ex.set_staging_vkallocator(stagA);
		ncnn::VkCompute cmd(vkdev);
		ncnn::VkMat inGpu, outGpu, rgba;
		// allocated before the network runs, so it never shares recycled memory with the network's scratch blobs
		if (ai->toRgba)
			rgba.create(OW, OH, (size_t)4u, 1, blobA);
		cmd.record_upload(src, inGpu, opt);
		const bool ran = !ex.input(ni ? ni->inBlob : "data", inGpu) && !ex.extract(ni ? ni->outBlob : "output", outGpu, cmd);
		if (ran && ai->nativeScale <= 0 && outGpu.w % w == 0 && outGpu.w / w >= 1 && outGpu.h == h * (outGpu.w / w)) {
			ai->nativeScale = outGpu.w / w;   // learnt once; other display scales are resized from it (see above)
			if (ai->nativeScale != scale)
				warning("remaster: the AI model is x%d; the x%d picture is resized from its output", ai->nativeScale, scale);
		}
		if (ran && (outGpu.w != RW || outGpu.h != RH)) {
			static int badSize = 0;
			if (badSize++ < 3)
				warning("remaster: AI model output %dx%d for %dx%d input, expected %dx%d", outGpu.w, outGpu.h, iw, ih, RW, RH);
		} else if (ran && gpuResize) {
			ncnn::VkMat big;
			if (ai->resize->forward(outGpu, big, cmd, opt) == 0)
				outGpu = big;
		}
		if (ran && outGpu.w == NW && outGpu.h == NH) {
			if (ai->toRgba && !ai->toRgbaFailed) {
				// ncnn does not order our own shader after the network's last layer: finish the network first
				cmd.submit_and_wait();
				cmd.reset();
				ncnn::VkMat planes;
				if (outGpu.elempack != 1)
					vkdev->convert_packing(outGpu, planes, 1, cmd, opt);   // planes, storage type unchanged
				else
					planes = outGpu;
				std::vector<ncnn::VkMat> bindings(2);
				bindings[0] = planes;
				bindings[1] = rgba;
				std::vector<ncnn::vk_constant_type> constants(4);
				constants[0].i = OW;
				constants[1].i = OH;
				constants[2].i = (int)planes.cstep;
				constants[3].i = NW;
				cmd.record_pipeline(ai->toRgba, bindings, constants, rgba);
				ncnn::Mat host, ref;
				const bool check = !ai->toRgbaChecked;
				if (check)
					cmd.record_download(outGpu, ref, opt);
				ncnn::Option raw = opt;   // download the bytes as they are (no repacking or casting)
				raw.use_packing_layout = false;
				raw.use_fp16_storage = false;
				raw.use_fp16_packed = false;
				raw.use_fp16_arithmetic = false;
				raw.use_bf16_storage = false;
				raw.use_bf16_packed = false;
				cmd.record_download(rgba, host, raw);
				if (!cmd.submit_and_wait() && host.w == OW && host.h == OH) {
					memcpy(out, host.data, (size_t)OW * OH * 4);
					ok = true;
					if (check && ref.w == NW && ref.h == NH) {
						ai->toRgbaChecked = true;
						std::vector<byte> full((size_t)NW * NH * 4), cpu((size_t)OW * OH * 4);
						const float denorm[3] = {255.f, 255.f, 255.f};
						ref.substract_mean_normalize(nullptr, denorm);
						ref.to_pixels(full.data(), ncnn::Mat::PIXEL_RGB2RGBA);
						for (int y = 0; y < OH; y++)
							memcpy(&cpu[(size_t)y * OW * 4], &full[(size_t)y * NW * 4], (size_t)OW * 4);
						size_t diff = 0;
						for (size_t k = 0; k < cpu.size(); k++)
							diff += cpu[k] != out[k];
						if (diff) {
							// this GPU/driver disagrees: keep the slower but certain CPU conversion
							warning("remaster: GPU colour conversion differs on this GPU (%u bytes), converting on the CPU", (unsigned)diff);
							memcpy(out, cpu.data(), cpu.size());
							std::lock_guard<std::mutex> lk(ai->toRgbaMutex);
							ai->toRgbaFailed = true;
						}
					}
				}
			} else {
				ncnn::Mat dst;
				cmd.record_download(outGpu, dst, opt);
				if (!cmd.submit_and_wait() && dst.w == NW && dst.h == NH) {
					const float denorm[3] = {255.f, 255.f, 255.f};
					dst.substract_mean_normalize(nullptr, denorm);
					if (NW == OW && NH == OH) {
						dst.to_pixels(out, ncnn::Mat::PIXEL_RGB2RGBA);
					} else {
						std::vector<byte> full((size_t)NW * NH * 4);
						dst.to_pixels(full.data(), ncnn::Mat::PIXEL_RGB2RGBA);
						for (int y = 0; y < OH; y++)
							memcpy(out + (size_t)y * OW * 4, &full[(size_t)y * NW * 4], (size_t)OW * 4);
					}
					ok = true;
				}
			}
		}
	}
	vkdev->reclaim_blob_allocator(blobA);
	vkdev->reclaim_staging_allocator(stagA);
	if (ok && ms)
		*ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
	return ok;
}

static bool remasterFsrRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out) {
	const ncnn::VulkanDevice *vkdev = ai->vkdev;
	const int OW = w * scale, OH = h * scale;
	ncnn::VkAllocator *blobA = vkdev->acquire_blob_allocator();
	ncnn::VkAllocator *stagA = vkdev->acquire_staging_allocator();
	ncnn::Option opt;   // plain bytes: no repacking or casting on upload/download
	opt.use_vulkan_compute = true;
	opt.use_packing_layout = false;
	opt.use_fp16_packed = opt.use_fp16_storage = opt.use_fp16_arithmetic = false;
	opt.use_bf16_storage = opt.use_bf16_packed = false;
	opt.blob_vkallocator = blobA;
	opt.workspace_vkallocator = blobA;
	opt.staging_vkallocator = stagA;
	bool ok = false;
	{
		// one channel of w x h: ncnn regroups 2D images into blocks of 4 rows on upload, but never a single channel
		ncnn::Mat src(w, h, 1, (size_t)4u, 1);
		memcpy(src.data, in, (size_t)w * h * 4);
		ncnn::VkCompute cmd(vkdev);
		ncnn::VkMat inGpu, mid, dst;
		mid.create(OW, OH, (size_t)4u, 1, blobA);
		dst.create(OW, OH, (size_t)4u, 1, blobA);
		cmd.record_upload(src, inGpu, opt);
		cmd.submit_and_wait();   // our own shaders are not ordered by ncnn: finish each step
		cmd.reset();
		// FsrEasuCon with the whole image as viewport
		const float iw = (float)w, ih = (float)h, ow = (float)OW, oh = (float)OH;
		const float con[16] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f,
		                       1.0f / iw, 1.0f / ih, 1.0f / iw, -1.0f / ih,
		                       -1.0f / iw, 2.0f / ih, 1.0f / iw, 2.0f / ih,
		                       0.0f, 4.0f / ih, 0.0f, 0.0f};
		std::vector<ncnn::vk_constant_type> c1(20);
		for (int k = 0; k < 16; k++)
			c1[k].f = con[k];
		c1[14].u32 = 0;
		c1[15].u32 = 0;
		c1[16].i = w;
		c1[17].i = h;
		c1[18].i = OW;
		c1[19].i = OH;
		std::vector<ncnn::VkMat> b1(2);
		b1[0] = inGpu;
		b1[1] = mid;
		cmd.record_pipeline(ai->easu, b1, c1, mid);
		cmd.submit_and_wait();
		cmd.reset();
		std::vector<ncnn::vk_constant_type> c2(6);
		c2[0].f = exp2f(-ai->fsrSharpness);
		c2[1].u32 = c2[2].u32 = c2[3].u32 = 0;
		c2[4].i = OW;
		c2[5].i = OH;
		std::vector<ncnn::VkMat> b2(2);
		b2[0] = mid;
		b2[1] = dst;
		cmd.record_pipeline(ai->rcas, b2, c2, dst);
		cmd.submit_and_wait();
		cmd.reset();
		ncnn::Mat host;
		cmd.record_download(dst, host, opt);
		if (!cmd.submit_and_wait() && host.w == OW && host.h == OH) {
			memcpy(out, host.data, (size_t)OW * OH * 4);
			ok = true;
		}
	}
	vkdev->reclaim_blob_allocator(blobA);
	vkdev->reclaim_staging_allocator(stagA);
	return ok;
}

void remasterAIDestroy(RemasterAI *ai) {
	// The Vulkan instance is created once and never destroyed: other upscalers and worker threads (HD backgrounds,
	// HD characters) may still use it, and destroying it unloads vulkan-1.dll under them. Windows frees it on exit.
	delete ai;
	if (g_remasterAIInstances > 0)
		g_remasterAIInstances--;
}

class RemasterAIJob {
public:
	RemasterAI *ai = nullptr;
	int w = 0, h = 0, scale = 1, tile = 128, pad = 12;
	float pauseRatio = 0;
	std::vector<byte> in, out;
	std::atomic<bool> done{false};
	bool ok = false;
	std::thread worker;
};

static void remasterAIJobRun(RemasterAIJob *j) {
	const int W = j->w, H = j->h, S = j->scale, T = j->tile, PAD = j->pad;
	j->out.assign((size_t)W * S * H * S * 4, 0);
	std::vector<byte> tin, tout;
	bool ok = true;
	for (int ty = 0; ty < H && ok; ty += T) {
		for (int tx = 0; tx < W && ok; tx += T) {
			const int x0 = tx - PAD < 0 ? 0 : tx - PAD, y0 = ty - PAD < 0 ? 0 : ty - PAD;
			const int x1 = tx + T + PAD > W ? W : tx + T + PAD, y1 = ty + T + PAD > H ? H : ty + T + PAD;
			const int tw = x1 - x0, th = y1 - y0;
			tin.resize((size_t)tw * th * 4);
			for (int y = 0; y < th; y++)
				memcpy(&tin[(size_t)y * tw * 4], &j->in[((size_t)(y0 + y) * W + x0) * 4], (size_t)tw * 4);
			tout.resize((size_t)tw * S * th * S * 4);
			const double t0 = remasterNowMs();
			if (!remasterAIRun(j->ai, tin.data(), tw, th, S, tout.data(), nullptr)) {
				ok = false;
				break;
			}
			if (j->pauseRatio > 0) {
				// rest between tiles: the GPU is the real-time upscaler's most of the time
				const double rest = MIN((remasterNowMs() - t0) * j->pauseRatio, 2000.0);
				std::this_thread::sleep_for(std::chrono::microseconds((long long)(rest * 1000)));
			}
			const int cx1 = tx + T > W ? W : tx + T, cy1 = ty + T > H ? H : ty + T;
			for (int y = ty * S; y < cy1 * S; y++)
				memcpy(&j->out[((size_t)y * W * S + (size_t)tx * S) * 4],
				       &tout[((size_t)(y - y0 * S) * tw * S + (size_t)(tx - x0) * S) * 4], (size_t)(cx1 - tx) * S * 4);
		}
	}
	j->ok = ok;
	j->done = true;
}

RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile, int pad, float pauseRatio) {
	if (!ai || w <= 0 || h <= 0)
		return nullptr;
	RemasterAIJob *j = new RemasterAIJob();
	j->ai = ai;
	j->w = w;
	j->h = h;
	j->scale = scale;
	j->tile = tile > 16 ? tile : 128;
	j->pad = CLIP(pad, 0, 64);
	j->pauseRatio = pauseRatio;
	j->in.assign(in, in + (size_t)w * h * 4);
	j->worker = std::thread(remasterAIJobRun, j);
	return j;
}

bool remasterAIJobDone(RemasterAIJob *job) {
	return !job || job->done.load();
}

const byte *remasterAIJobResult(RemasterAIJob *job) {
	return (job && job->done.load() && job->ok) ? job->out.data() : nullptr;
}

void remasterAIJobDestroy(RemasterAIJob *job) {
	if (!job)
		return;
	if (job->worker.joinable())
		job->worker.join();
	delete job;
}

#else // !USE_REMASTER_AI

class RemasterAI {};

RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err, int net) {
	err = "this build has no AI support";
	return nullptr;
}

int remasterAINetOf(const RemasterAI *ai) {
	return 0;
}

bool remasterAIWholeFrameOnly(const RemasterAI *ai) {
	return false;
}

bool remasterAITensorRTBuilt(const Common::String &modelBase, int fullW, int fullH, int net) {
	return false;
}

RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW, int fullH, int net) {
	err = "this build has no AI support";
	return nullptr;
}

const char *remasterAIBackendName(const RemasterAI *ai) {
	return "none";
}

bool remasterAIBackendIsAI(const RemasterAI *ai) {
	return false;
}

int remasterAIPrepareState(const RemasterAI *ai) {
	return 1;
}

bool remasterAISetLight(RemasterAI *ai, bool light) {
	return false;
}

bool remasterAISetVideo(RemasterAI *ai, bool video) {
	return false;
}

bool remasterAIRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out, double *ms) {
	return false;
}

void remasterAIDestroy(RemasterAI *ai) {
}

class RemasterAIJob {};

RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile, int pad, float pauseRatio) {
	return nullptr;
}

RemasterAI *remasterBgAICreate(const Common::String &modelBase, int bgNet, bool preferTensorRT, Common::String &err, int *usedNet) {
	err = "this build has no AI support";
	if (usedNet)
		*usedNet = -1;
	return nullptr;
}

int remasterBgNetOf(const RemasterAI *ai) {
	return -1;
}

bool remasterAIHasStandIn(const RemasterAI *ai) {
	return false;
}

int remasterAIJobTile(const RemasterAI *ai, int *pad) {
	return 0;
}

bool remasterAIJobDone(RemasterAIJob *job) {
	return true;
}

const byte *remasterAIJobResult(RemasterAIJob *job) {
	return nullptr;
}

void remasterAIJobDestroy(RemasterAIJob *job) {
}

#endif

} // End of namespace Scumm
