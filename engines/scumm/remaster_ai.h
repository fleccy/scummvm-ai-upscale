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
// The model is a Real-ESRGAN style ncnn model (e.g. realesr-animevideov3-x3) with blobs "data"/"output".
class RemasterAI;

// Loads <modelBase>.param/.bin and prepares the GPU. Returns nullptr (with err set) if no Vulkan GPU or the
// model cannot be loaded; the caller then falls back to plain scaling.
RemasterAI *remasterAICreate(const Common::String &modelBase, Common::String &err);
// Upscaler by name (remaster_ai_backend): "ncnn" (default, the model above), "dml" / "dml-ultra" (ONNX Runtime +
// DirectML), "trt" / "trt-ultra" (ONNX Runtime + TensorRT, optional download) or "fsr" (AMD FidelityFX Super Resolution 1,
// no AI). All upscalers take and return the same images, so the renderer can switch between them while running.
// fullW x fullH: the size of whole frames, prepared up front where that helps (DirectML, TensorRT).
RemasterAI *remasterAICreateBackend(const Common::String &backend, const Common::String &modelBase, Common::String &err, int fullW = 0, int fullH = 0);
const char *remasterAIBackendName(const RemasterAI *ai);
bool remasterAIBackendIsAI(const RemasterAI *ai);
int remasterAIPrepareState(const RemasterAI *ai);   // 0 still preparing (TensorRT engines), 1 ready, -1 failed
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
RemasterAIJob *remasterAIJobStart(RemasterAI *ai, const byte *in, int w, int h, int scale, int tile);
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

} // End of namespace Scumm

#endif
