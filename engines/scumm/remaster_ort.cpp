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

// ONNX Runtime upscalers (Windows): DirectML (onnxruntime.dll next to scummvm.exe, any DirectX 12 GPU) and TensorRT
// (NVIDIA; a separate ONNX Runtime GPU build with the NVIDIA libraries in the "tensorrt" folder, an optional
// download of the installer). The DLLs are loaded at run time; without them these upscalers are not offered.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "scumm/remaster_ort.h"
#include "common/textconsole.h"

#ifdef USE_REMASTER_ORT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "onnxruntime/onnxruntime_c_api.h"

#ifndef CP_UTF8
#define CP_UTF8 65001
#endif
// winnls.h is left out by the build's Windows settings
extern "C" __declspec(dllimport) int WINAPI MultiByteToWideChar(UINT codePage, DWORD flags, LPCCH src, int srcLen, LPWSTR dst, int dstLen);
extern "C" __declspec(dllimport) int WINAPI WideCharToMultiByte(UINT codePage, DWORD flags, LPCWCH src, int srcLen, LPSTR dst, int dstLen, LPCCH defChar, LPBOOL usedDef);

// From dml_provider_factory.h (ONNX Runtime 1.24, MIT), which needs DirectML.h / d3d12.h annotations MinGW lacks.
// Only the function table layout matters; the D3D12 entries are never called here.
enum ScummOrtDmlPreference { kDmlDefault = 0, kDmlHighPerformance = 1, kDmlMinimumPower = 2 };
enum ScummOrtDmlFilter : uint32_t { kDmlFilterGpu = 1u << 0 };
struct ScummOrtDmlDeviceOptions {
	ScummOrtDmlPreference Preference;
	ScummOrtDmlFilter Filter;
};
struct ScummOrtDmlApi {
	OrtStatus *(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML)(OrtSessionOptions *options, int device_id);
	OrtStatus *(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML1)(OrtSessionOptions *, void *, void *);
	OrtStatus *(ORT_API_CALL *CreateGPUAllocationFromD3DResource)(void *, void **);
	OrtStatus *(ORT_API_CALL *FreeGPUAllocation)(void *);
	OrtStatus *(ORT_API_CALL *GetD3D12ResourceFromAllocation)(OrtAllocator *, void *, void **);
	OrtStatus *(ORT_API_CALL *SessionOptionsAppendExecutionProvider_DML2)(OrtSessionOptions *options, ScummOrtDmlDeviceOptions *device_opts);
};

namespace Scumm {

// One loaded ONNX Runtime per provider: the DirectML build and the GPU (TensorRT) build are different DLLs.
struct OrtLib {
	HMODULE dll = nullptr;
	const OrtApi *api = nullptr;
	OrtEnv *env = nullptr;
	std::wstring dir;
};
static OrtLib g_lib[2];
static std::mutex g_libMutex;

struct RemasterOrt {
	RemasterOrtProvider provider;
	OrtLib *lib = nullptr;
	std::wstring model, cacheDir;
	std::string prefix;              // TensorRT engine cache names: <model>_<w>x<h>
	struct Session {
		int w, h;
		OrtSession *s;
	};
	std::vector<Session> sessions;   // one per fixed input size (DirectML and TensorRT are fastest for a fixed size)
	OrtMemoryInfo *mem = nullptr;
	// Fixed tile sizes. A region that fits one tile is run as the smallest that fits (the renderer already sends
	// regions with context around them); larger ones are split into overlapping tiles of the biggest size.
	static const int kTiles = 4;
	int tiles[kTiles] = {96, 128, 192, 256};
	int pad = 20;
	int fullW = 0, fullH = 0;        // whole frames (made up front when known)
	std::vector<uint8_t> tin, tout;
	// TensorRT: engines are built (or loaded from the cache) on a worker thread
	std::atomic<int> state{0};       // 0 preparing, 1 ready, -1 failed
	std::thread builder;
	std::mutex m;
	bool building = false, abandoned = false;
	std::string failure;
};

static bool ortOk(const OrtApi *api, OrtStatus *st, std::string *err) {
	if (!st)
		return true;
	if (err)
		*err = api->GetErrorMessage(st);
	api->ReleaseStatus(st);
	return false;
}

static std::wstring exeDir() {
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe, n);
	return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

static std::string narrow(const std::wstring &w) {
	int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::vector<char> b(len > 0 ? len : 1);
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b.data(), len, nullptr, nullptr);
	return b.data();
}

static OrtLib *ortLoad(RemasterOrtProvider provider, std::string &err) {
	std::lock_guard<std::mutex> lk(g_libMutex);
	OrtLib &L = g_lib[provider];
	if (L.api)
		return &L;
	L.dir = exeDir() + (provider == kOrtTensorRT ? L"tensorrt\\" : L"");
	if (provider == kOrtTensorRT) {
		if (GetFileAttributesW((L.dir + L"nvinfer_10.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
			err = "TensorRT is not installed (optional download of the installer)";
			return nullptr;
		}
		// the NVIDIA libraries load further DLLs of their own by name, from this folder
		SetDllDirectoryW(L.dir.c_str());
	}
	// DirectML.dll (or the NVIDIA libraries) next to onnxruntime.dll are found through the altered search path
	L.dll = LoadLibraryExW((L.dir + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!L.dll) {
		err = provider == kOrtTensorRT ? "cannot load tensorrt\\onnxruntime.dll" : "onnxruntime.dll not found next to scummvm.exe";
		return nullptr;
	}
	typedef const OrtApiBase *(ORT_API_CALL *GetApiBase)();
	GetApiBase base = (GetApiBase)(void *)GetProcAddress(L.dll, "OrtGetApiBase");
	const OrtApi *api = base ? base()->GetApi(ORT_API_VERSION) : nullptr;
	if (!api) {
		err = "onnxruntime.dll is too old";
		return nullptr;
	}
	if (!ortOk(api, api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "scummvm-remaster", &L.env), &err))
		return nullptr;
	L.api = api;
	return &L;
}

static OrtSession *ortFind(RemasterOrt *o, int w, int h) {
	for (size_t k = 0; k < o->sessions.size(); k++)
		if (o->sessions[k].w == w && o->sessions[k].h == h)
			return o->sessions[k].s;
	return nullptr;
}

static OrtSession *ortSession(RemasterOrt *o, int w, int h, std::string *err) {
	if (OrtSession *s = ortFind(o, w, h))
		return s;
	const OrtApi *api = o->lib->api;
	OrtSessionOptions *so = nullptr;
	OrtSession *s = nullptr;
	bool ok = ortOk(api, api->CreateSessionOptions(&so), err) &&
		ortOk(api, api->DisableMemPattern(so), err) &&
		ortOk(api, api->SetSessionExecutionMode(so, ORT_SEQUENTIAL), err) &&
		ortOk(api, api->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), err) &&
		ortOk(api, api->AddFreeDimensionOverrideByName(so, "H", h), err) &&
		ortOk(api, api->AddFreeDimensionOverrideByName(so, "W", w), err);
	if (ok && o->provider == kOrtDirectML) {
		const ScummOrtDmlApi *dml = nullptr;
		ScummOrtDmlDeviceOptions dev = {kDmlHighPerformance, kDmlFilterGpu};   // the discrete GPU on laptops with two
		ok = ortOk(api, api->GetExecutionProviderApi("DML", ORT_API_VERSION, (const void **)&dml), err) &&
			ortOk(api, dml->SessionOptionsAppendExecutionProvider_DML2(so, &dev), err);
	} else if (ok) {
		OrtTensorRTProviderOptionsV2 *trt = nullptr;
		const std::string cache = narrow(o->cacheDir), pre = o->prefix + "_" + std::to_string(w) + "x" + std::to_string(h);
		const char *keys[] = {"device_id", "trt_fp16_enable", "trt_engine_cache_enable", "trt_engine_cache_path",
		                      "trt_engine_cache_prefix", "trt_timing_cache_enable", "trt_timing_cache_path"};
		const char *vals[] = {"0", "1", "1", cache.c_str(), pre.c_str(), "1", cache.c_str()};
		ok = ortOk(api, api->CreateTensorRTProviderOptions(&trt), err) &&
			ortOk(api, api->UpdateTensorRTProviderOptions(trt, keys, vals, 7), err) &&
			ortOk(api, api->SessionOptionsAppendExecutionProvider_TensorRT_V2(so, trt), err);
		if (trt)
			api->ReleaseTensorRTProviderOptions(trt);
	}
	ok = ok && ortOk(api, api->CreateSession(o->lib->env, o->model.c_str(), so, &s), err);
	if (so)
		api->ReleaseSessionOptions(so);
	if (!ok)
		return nullptr;
	o->sessions.push_back({w, h, s});
	return s;
}

static bool ortRunFixed(RemasterOrt *o, OrtSession *s, const uint8_t *in, int w, int h, int scale, uint8_t *out) {
	const OrtApi *api = o->lib->api;
	const int64_t ishape[4] = {1, h, w, 4}, oshape[4] = {1, (int64_t)h * scale, (int64_t)w * scale, 4};
	OrtValue *iv = nullptr, *ov = nullptr;
	const char *inName = "in", *outName = "out";
	bool ok = ortOk(api, api->CreateTensorWithDataAsOrtValue(o->mem, (void *)in, (size_t)w * h * 4, ishape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, &iv), nullptr) &&
		ortOk(api, api->CreateTensorWithDataAsOrtValue(o->mem, out, (size_t)w * h * scale * scale * 4, oshape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8, &ov), nullptr) &&
		ortOk(api, api->Run(s, nullptr, &inName, (const OrtValue *const *)&iv, 1, &outName, 1, &ov), nullptr);
	if (iv)
		api->ReleaseValue(iv);
	if (ov)
		api->ReleaseValue(ov);
	return ok;
}

static void ortFree(RemasterOrt *o) {
	for (size_t k = 0; k < o->sessions.size(); k++)
		o->lib->api->ReleaseSession(o->sessions[k].s);
	o->sessions.clear();
	if (o->mem)
		o->lib->api->ReleaseMemoryInfo(o->mem);
	o->mem = nullptr;
}

// Makes every fixed-size session (TensorRT: builds or loads its engines, which can take minutes the first time).
static bool ortPrepare(RemasterOrt *o, std::string &err) {
	for (int k = 0; k < RemasterOrt::kTiles; k++) {
		if (o->abandoned || !ortSession(o, o->tiles[k], o->tiles[k], &err))
			return false;
	}
	if (o->fullW > 0 && o->fullH > 0 && !ortSession(o, o->fullW, o->fullH, &err))
		return false;
	return true;
}

RemasterOrt *remasterOrtCreate(RemasterOrtProvider provider, const Common::String &modelPath, int fullW, int fullH, Common::String &err) {
	std::string e;
	OrtLib *lib = ortLoad(provider, e);
	if (!lib) {
		err = e.c_str();
		return nullptr;
	}
	RemasterOrt *o = new RemasterOrt();
	o->provider = provider;
	o->lib = lib;
	o->fullW = fullW;
	o->fullH = fullH;
	int len = MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, nullptr, 0);
	std::vector<wchar_t> wp(len > 0 ? len : 1);
	MultiByteToWideChar(CP_UTF8, 0, modelPath.c_str(), -1, wp.data(), len);
	o->model = wp.data();
	const size_t sl = o->model.find_last_of(L"\\/");
	std::wstring base = o->model.substr(sl == std::wstring::npos ? 0 : sl + 1);
	base = base.substr(0, base.find_last_of(L'.'));
	o->prefix = narrow(base);
	o->cacheDir = lib->dir + L"cache";
	if (!ortOk(lib->api, lib->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &o->mem), &e)) {
		err = e.c_str();
		delete o;
		return nullptr;
	}
	if (provider == kOrtDirectML) {
		if (!ortPrepare(o, e)) {
			err = e.c_str();
			remasterOrtDestroy(o);
			return nullptr;
		}
		o->state = 1;
		err = "DirectML";
		return o;
	}
	CreateDirectoryW(o->cacheDir.c_str(), nullptr);
	o->building = true;
	o->builder = std::thread([o] {
		std::string fe;
		const bool ok = ortPrepare(o, fe);
		if (ok)
			warning("remaster: TensorRT engines ready for %s", o->prefix.c_str());
		else
			warning("remaster: TensorRT could not prepare %s: %s", o->prefix.c_str(), fe.c_str());
		std::unique_lock<std::mutex> lk(o->m);
		o->building = false;
		o->failure = fe;
		o->state = ok ? 1 : -1;
		if (o->abandoned) {   // the renderer switched away meanwhile: tidy up here
			lk.unlock();
			ortFree(o);
			delete o;
		}
	});
	err = "TensorRT";
	return o;
}

int remasterOrtState(RemasterOrt *o, Common::String *failure) {
	if (!o)
		return -1;
	const int st = o->state;
	if (st < 0 && failure)
		*failure = o->failure.c_str();
	return st;
}

// Copies the w x h image into a T x T tile starting at (x0, y0) of the image, repeating its edges.
static void ortFillTile(std::vector<uint8_t> &tin, const uint8_t *in, int w, int h, int x0, int y0, int T) {
	tin.resize((size_t)T * T * 4);
	for (int y = 0; y < T; y++) {
		const int sy = y0 + y < 0 ? 0 : (y0 + y >= h ? h - 1 : y0 + y);
		uint32_t *d = (uint32_t *)&tin[(size_t)y * T * 4];
		const uint32_t *srow = (const uint32_t *)in + (size_t)sy * w;
		for (int x = 0; x < T; x++) {
			const int sx = x0 + x < 0 ? 0 : (x0 + x >= w ? w - 1 : x0 + x);
			d[x] = srow[sx];
		}
	}
}

bool remasterOrtRun(RemasterOrt *o, const uint8_t *in, int w, int h, int scale, uint8_t *out) {
	if (!o || o->state != 1)
		return false;
	const int S = scale;
	// Whole frames (always the same size) have their own fixed-size session.
	if ((long)w * h >= 200000) {
		OrtSession *s = o->provider == kOrtDirectML ? ortSession(o, w, h, nullptr) : ortFind(o, w, h);
		return s && ortRunFixed(o, s, in, w, h, scale, out);
	}
	// Fits one tile: the smallest that does.
	for (int k = 0; k < RemasterOrt::kTiles; k++) {
		const int T = o->tiles[k];
		if (w > T || h > T)
			continue;
		OrtSession *s = ortFind(o, T, T);
		if (!s)
			return false;
		const uint8_t *src = in;
		if (w != T || h != T) {
			ortFillTile(o->tin, in, w, h, 0, 0, T);
			src = o->tin.data();
		}
		o->tout.resize((size_t)T * S * T * S * 4);
		if (!ortRunFixed(o, s, src, T, T, S, o->tout.data()))
			return false;
		for (int y = 0; y < h * S; y++)
			memcpy(out + (size_t)y * w * S * 4, &o->tout[(size_t)y * T * S * 4], (size_t)w * S * 4);
		return true;
	}
	// Larger: overlapping tiles of the biggest size.
	const int T = o->tiles[RemasterOrt::kTiles - 1], P = o->pad, I = T - 2 * P;
	OrtSession *s = ortFind(o, T, T);
	if (!s)
		return false;
	o->tout.resize((size_t)T * S * T * S * 4);
	for (int ty = 0; ty < h; ty += I) {
		for (int tx = 0; tx < w; tx += I) {
			ortFillTile(o->tin, in, w, h, tx - P, ty - P, T);
			if (!ortRunFixed(o, s, o->tin.data(), T, T, S, o->tout.data()))
				return false;
			const int cw = (tx + I > w ? w - tx : I) * S, ch = (ty + I > h ? h - ty : I) * S;
			for (int y = 0; y < ch; y++)
				memcpy(out + ((size_t)(ty * S + y) * w * S + (size_t)tx * S) * 4,
				       &o->tout[((size_t)(P * S + y) * T * S + (size_t)P * S) * 4], (size_t)cw * 4);
		}
	}
	return true;
}

void remasterOrtDestroy(RemasterOrt *o) {
	if (!o)
		return;
	{
		std::lock_guard<std::mutex> lk(o->m);
		if (o->building) {
			// still building TensorRT engines: let the builder finish (they are cached) and free everything itself
			o->abandoned = true;
			o->builder.detach();
			return;
		}
	}
	if (o->builder.joinable())
		o->builder.join();
	ortFree(o);
	delete o;
}

} // End of namespace Scumm

#else

namespace Scumm {

RemasterOrt *remasterOrtCreate(RemasterOrtProvider provider, const Common::String &modelPath, int fullW, int fullH, Common::String &err) {
	err = "this build has no ONNX Runtime support";
	return nullptr;
}

int remasterOrtState(RemasterOrt *o, Common::String *failure) {
	return -1;
}

bool remasterOrtRun(RemasterOrt *o, const uint8 *in, int w, int h, int scale, uint8 *out) {
	return false;
}

void remasterOrtDestroy(RemasterOrt *o) {
}

} // End of namespace Scumm

#endif
