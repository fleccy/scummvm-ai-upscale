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

#ifndef SCUMM_REMASTER_AI_H
#define SCUMM_REMASTER_AI_H

#include "common/scummsys.h"
#include "common/str.h"

namespace Scumm {

// Real-time AI upscaling of whole frames on the GPU (ncnn + Vulkan), used by the COMI remaster renderer.
// The default model is Real-ESRGAN realesr-animevideov3-x3 (ncnn); other networks can be chosen (remaster_ai_net).
class RemasterAI;

// The real-time AI models (remaster_ai_net). 0 "default" is the model at remaster_ai_model itself
// (realesr-animevideov3-x3); the others are files next to it in the same folder.
int remasterAINetCount();
const char *remasterAINetId(int net);      // config value: "default", "cugan"
const char *remasterAINetLabel(int net);   // for menus
int remasterAINetIndex(const Common::String &id);   // -1 if unknown
// Whether the model files of this network are next to modelBase (the default network: modelBase itself).
bool remasterAINetAvailable(const Common::String &modelBase, int net);
// Whether the network has an ONNX version next to modelBase (DirectML / TensorRT can run it; otherwise ncnn only).
bool remasterAINetHasOnnx(const Common::String &modelBase, int net);
// Network of an upscaler (0 for FSR and the ONNX upscalers, which only have the default network).
int remasterAINetOf(const RemasterAI *ai);
// True when the network's result for a pixel depends on the whole picture (global pooling, Real-CUGAN): such a
// network must only be run on whole frames (no patches, no tile cache).
bool remasterAIWholeFrameOnly(const RemasterAI *ai);

// Loads <modelBase>.param/.bin and prepares the GPU. Returns nullptr (with err set) if no Vulkan GPU or the
// model cannot be loaded; the caller then falls back to plain scaling.
// net: one of the networks above (modelBase is then the default network's path); -1: a Real-ESRGAN style model at
// modelBase whose scale is learnt from its first output (the HD background model).
RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err, int net = -1);
// The HD background models (remaster_bg_net): "x4anime" Real-ESRGAN x4plus anime (ncnn, any Vulkan GPU) and
// "ultrasharpv2" 4x UltraSharpV2 (DAT2; ONNX Runtime only: DirectML or TensorRT, so Windows). Files next to the
// real-time model (remaster_ai_model).
int remasterBgNetCount();
const char *remasterBgNetId(int bgNet);
const char *remasterBgNetLabel(int bgNet);
int remasterBgNetIndex(const Common::String &id);   // -1 if unknown
// Whether this background model can run here: its file next to modelBase and, for ONNX models, ONNX Runtime built in.
bool remasterBgNetAvailable(const Common::String &modelBase, int bgNet);
// Makes the background upscaler for bgNet. ONNX models: TensorRT when preferTensorRT and its engines are ready to load
// or build, otherwise DirectML. When that is impossible (no DirectX 12 GPU, no ONNX Runtime, file missing) the
// x4anime model on ncnn is made instead; *usedNet says which model it is (it is part of the HD background cache's
// file names, so pictures made by one model are never taken for another's).
RemasterAI *remasterBgAICreate(const Common::String &modelBase, int bgNet, bool preferTensorRT, Common::String &err, int *usedNet);
// The background model of an upscaler made by remasterBgAICreate (-1 for others).
int remasterBgNetOf(const RemasterAI *ai);
// Tile size for remasterAIJobStart with this upscaler and the context around each tile: fixed-size models get
// tiles that fill their input exactly (0 if the upscaler takes any size; then the caller's own choice stands).
int remasterAIJobTile(const RemasterAI *ai, int *pad);

// Upscaler by name (remaster_ai_backend): "ncnn" (default, the model above), "dml" / "dml-ultra" (ONNX Runtime +
// DirectML), "trt" / "trt-ultra" (ONNX Runtime + TensorRT, optional download) or "fsr" (AMD FidelityFX Super Resolution 1,
// no AI). All upscalers take and return the same images, so the renderer can switch between them while running.
// fullW x fullH: the size of whole frames, prepared up front where that helps (DirectML, TensorRT).
// net: the AI network (see above). A network without an ONNX version asked for with DirectML or TensorRT runs on
// ncnn instead (logged); with another network than the default one, Ultra is the plain upscaler.
RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW = 0, int fullH = 0, int net = 0);
const char *remasterAIBackendName(const RemasterAI *ai);
bool remasterAIBackendIsAI(const RemasterAI *ai);
int remasterAIPrepareState(const RemasterAI *ai);   // 0 still preparing (TensorRT engines), 1 ready, -1 failed
// Whether another upscaler stands in while this one is not ready or has failed (TensorRT: ncnn or DirectML).
bool remasterAIHasStandIn(const RemasterAI *ai);
// Whether the optional TensorRT upscaler has its engines for whole frames of fullW x fullH built already.
bool remasterAITensorRTBuilt(const Common::String &modelBase, int fullW, int fullH, int net = 0);
// Ultra (heavy network): true -> use the standard network for everything (the governor's first step in heavy rooms).
// Returns false if this upscaler has no lighter mode.
bool remasterAISetLight(RemasterAI *ai, bool light);
// A cutscene is showing (Ultra on TensorRT then uses the heavy network for whole frames too).
// Returns true when the picture should be redone in full (the heavy network just became usable).
bool remasterAISetVideo(RemasterAI *ai, bool video);   // false for plain shader upscalers such as FSR
// in: w x h pixels, 4 bytes each in memory order R, G, B, A. out: (w*scale) x (h*scale), same layout,
// alpha 255. Returns false on failure. *ms receives the time taken.
bool remasterAIRun(RemasterAI *ai, const byte *in, int w, int h, int scale, byte *out, double *ms);
void remasterAIDestroy(RemasterAI *ai);

// Background jobs: run a model on a private copy of an image on a worker thread, in overlapping tiles (so large
// images need little GPU memory). Used for the HD room backgrounds, which use a heavier model than real time allows.
class RemasterAIJob;
// pad: context around each tile (cropped off again). pauseRatio > 0: after each tile the worker rests for that many
// times the tile's own run time, leaving the GPU to the real-time upscaler most of the time (heavy models).
RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile, int pad = 12, float pauseRatio = 0);
bool remasterAIJobDone(RemasterAIJob *job);          // finished, successfully or not
const byte *remasterAIJobResult(RemasterAIJob *job); // (w*scale) x (h*scale) R,G,B,A bytes, or nullptr on failure
void remasterAIJobDestroy(RemasterAIJob *job);       // waits for the worker if it is still running

// Runs fn(ctx, begin, end) over [0, n) split into chunks on a small pool of worker threads (and the calling thread);
// returns when all chunks are done. Chunks must not depend on each other.
void remasterParallelFor(int n, void (*fn)(void *ctx, int begin, int end), void *ctx);
template<class F> void remasterParallel(int n, const F &f) {
	remasterParallelFor(n, [](void *c, int a, int b) { (*(const F *)c)(a, b); }, (void *)&f);
}
void remasterParallelShutdown();
// Milliseconds from a steady high-resolution clock (for measuring).
double remasterNowMs();
// Height in pixels of the desktop on the main display (real pixels, not scaled by Windows' text size), 0 if unknown.
int remasterDesktopHeight();

} // End of namespace Scumm

#endif
