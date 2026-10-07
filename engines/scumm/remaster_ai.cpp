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
#include "common/textconsole.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#ifdef USE_REMASTER_AI
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <vector>

#include "ncnn/command.h"
#include "ncnn/gpu.h"
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

void remasterParallelShutdown() {
	if (g_parPool) {
		g_parPool->stop();
		delete g_parPool;
		g_parPool = nullptr;
	}
}

#ifdef USE_REMASTER_AI

class RemasterAI {
public:
	enum Kind { kNcnn, kFsr, kOrt };
	Kind kind = kNcnn;
	ncnn::Net net;                      // kNcnn: the network
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
	~RemasterAI() {
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
layout(push_constant) uniform parameter { int w; int h; int cstep; } p;
void main()
{
	const int gx = int(gl_GlobalInvocationID.x);
	const int gy = int(gl_GlobalInvocationID.y);
	if (gx >= p.w || gy >= p.h)
		return;
	const int i = gy * p.w + gx;
	const uint r = uint(clamp(int(float(buffer_ld1(bottom_blob_data, i)) * 255.f), 0, 255));
	const uint g = uint(clamp(int(float(buffer_ld1(bottom_blob_data, p.cstep + i)) * 255.f), 0, 255));
	const uint b = uint(clamp(int(float(buffer_ld1(bottom_blob_data, 2 * p.cstep + i)) * 255.f), 0, 255));
	top_blob_data[i] = r | (g << 8) | (b << 16) | 0xff000000u;
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

RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW, int fullH) {
	if (backend == "fsr")
		return remasterFsrCreate(err);
	const bool trt = backend == "trt" || backend == "trt-ultra";
	if (trt || backend == "dml" || backend == "dml-ultra") {
		// ONNX models next to the ncnn model: the same light network, or the heavy one used for the HD backgrounds
		const char *sl = MAX(strrchr(modelBase.c_str(), '/'), strrchr(modelBase.c_str(), '\\'));
		const Common::String dir(modelBase.c_str(), sl ? sl + 1 : modelBase.c_str());
		const bool ultra = backend.hasSuffix("-ultra");
		const RemasterOrtProvider prov = trt ? kOrtTensorRT : kOrtDirectML;
		const Common::String light = dir + "realesr-animevideov3-x3-fp16.onnx", heavy = dir + "realesrgan-x4plus-anime-x3-fp16.onnx";
		// Ultra: the heavy network for regions, the light one for whole frames (and rooms too heavy for Ultra)
		RemasterOrt *o = remasterOrtCreate(prov, ultra ? heavy : light, ultra ? 0 : fullW, ultra ? 0 : fullH, err);
		if (!o)
			return nullptr;
		RemasterAI *ai = new RemasterAI();
		ai->kind = RemasterAI::kOrt;
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
			ai->standIn = remasterAICreate(modelBase, e3);
		}
		return ai;
	}
	return remasterAICreate(modelBase, err);
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
	return ai->kind == RemasterAI::kFsr ? "AMD FSR 1 (no AI)" : "Real-ESRGAN AI (ncnn)";
}

RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err) {
	if (g_remasterAIInstances++ == 0)
		ncnn::create_gpu_instance();
	if (ncnn::get_gpu_count() == 0) {
		err = "no Vulkan GPU found";
		remasterAIDestroy(nullptr);
		return nullptr;
	}
	RemasterAI *ai = new RemasterAI();
	ai->net.opt.use_vulkan_compute = true;
	ai->net.opt.use_fp16_packed = true;
	ai->net.opt.use_fp16_storage = true;
	ai->net.opt.use_fp16_arithmetic = false; // keeps colours stable (and measured no faster here)
	if (ai->net.load_param((modelBase + ".param").c_str()) || ai->net.load_model((modelBase + ".bin").c_str())) {
		err = "cannot load model " + modelBase;
		delete ai;
		remasterAIDestroy(nullptr);
		return nullptr;
	}
	err = Common::String("GPU: ") + ncnn::get_gpu_info(0).device_name();
	return ai;
}

// Resizes RGBA bytes (alpha left at 255): area average when shrinking (x3 -> x2), linear when enlarging (x3 -> x4),
// separably. Each output pixel only depends on nearby input, so tiles resized on their own match a whole frame
// away from their edges (the AI regions have context margins for that).
static void remasterResample(const byte *in, int iw, int ih, byte *out, int ow, int oh) {
	struct Tap { int i; float w; };
	auto taps = [](int n, int m, std::vector<std::vector<Tap> > &t) {
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

bool remasterAIRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out, double *ms) {
	if (!ai)
		return false;
	if (ai->kind != RemasterAI::kFsr && ai->nativeScale > 0 && ai->nativeScale != scale) {
		// display scale differs from the network's: run it at its own scale and resize
		const int ns = ai->nativeScale;
		std::vector<byte> tmp((size_t)w * ns * h * ns * 4);
		if (!remasterAIRun(ai, in, w, h, ns, tmp.data(), ms))
			return false;
		const std::chrono::high_resolution_clock::time_point r0 = std::chrono::high_resolution_clock::now();
		remasterResample(tmp.data(), w * ns, h * ns, out, w * scale, h * scale);
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
	ncnn::Mat src = ncnn::Mat::from_pixels(in, ncnn::Mat::PIXEL_RGBA2RGB, w, h);
	const float norm[3] = {1 / 255.f, 1 / 255.f, 1 / 255.f};
	src.substract_mean_normalize(nullptr, norm);
	const ncnn::VulkanDevice *vkdev = ai->net.vulkan_device();
	std::unique_lock<std::mutex> initLock(ai->toRgbaMutex);
	if (!ai->toRgba && !ai->toRgbaFailed) {
		std::vector<uint32_t> spirv;
		ai->toRgba = new ncnn::Pipeline(vkdev);
		ai->toRgba->set_optimal_local_size_xyz(16, 16, 1);
		std::vector<ncnn::vk_specialization_type> spec;
		if (ncnn::compile_spirv_module(kToRgbaShader, ai->net.opt, spirv) ||
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
	ncnn::Option opt = ai->net.opt;
	opt.blob_vkallocator = blobA;
	opt.workspace_vkallocator = blobA;
	opt.staging_vkallocator = stagA;
	const int OW = w * scale, OH = h * scale;
	bool ok = false;
	{
		ncnn::Extractor ex = ai->net.create_extractor();
		ex.set_blob_vkallocator(blobA);
		ex.set_workspace_vkallocator(blobA);
		ex.set_staging_vkallocator(stagA);
		ncnn::VkCompute cmd(vkdev);
		ncnn::VkMat inGpu, outGpu, rgba;
		// allocated before the network runs, so it never shares recycled memory with the network's scratch blobs
		if (ai->toRgba)
			rgba.create(OW, OH, (size_t)4u, 1, blobA);
		cmd.record_upload(src, inGpu, opt);
		const bool ran = !ex.input("data", inGpu) && !ex.extract("output", outGpu, cmd);
		if (ran && ai->nativeScale <= 0 && outGpu.w % w == 0 && outGpu.w / w >= 1 && outGpu.h == h * (outGpu.w / w)) {
			ai->nativeScale = outGpu.w / w;   // learnt once; other display scales are resized from it (see above)
			if (ai->nativeScale != scale)
				warning("remaster: the AI model is x%d; the x%d picture is resized from its output", ai->nativeScale, scale);
		}
		if (ran && outGpu.w == OW && outGpu.h == OH) {
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
				std::vector<ncnn::vk_constant_type> constants(3);
				constants[0].i = OW;
				constants[1].i = OH;
				constants[2].i = (int)planes.cstep;
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
					if (check && ref.w == OW && ref.h == OH) {
						ai->toRgbaChecked = true;
						std::vector<byte> cpu((size_t)OW * OH * 4);
						const float denorm[3] = {255.f, 255.f, 255.f};
						ref.substract_mean_normalize(nullptr, denorm);
						ref.to_pixels(cpu.data(), ncnn::Mat::PIXEL_RGB2RGBA);
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
				if (!cmd.submit_and_wait() && dst.w == OW && dst.h == OH) {
					const float denorm[3] = {255.f, 255.f, 255.f};
					dst.substract_mean_normalize(nullptr, denorm);
					dst.to_pixels(out, ncnn::Mat::PIXEL_RGB2RGBA);
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
	int w = 0, h = 0, scale = 1, tile = 128;
	std::vector<byte> in, out;
	std::atomic<bool> done{false};
	bool ok = false;
	std::thread worker;
};

static void remasterAIJobRun(RemasterAIJob *j) {
	const int W = j->w, H = j->h, S = j->scale, T = j->tile, PAD = 12;
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
			if (!remasterAIRun(j->ai, tin.data(), tw, th, S, tout.data(), nullptr)) {
				ok = false;
				break;
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

RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile) {
	if (!ai || w <= 0 || h <= 0)
		return nullptr;
	RemasterAIJob *j = new RemasterAIJob();
	j->ai = ai;
	j->w = w;
	j->h = h;
	j->scale = scale;
	j->tile = tile > 16 ? tile : 128;
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

RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err) {
	err = "this build has no AI support";
	return nullptr;
}

RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW, int fullH) {
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

RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile) {
	return nullptr;
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
