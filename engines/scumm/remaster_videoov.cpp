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

// The pipe to an external ffmpeg process needs the platform APIs.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#ifdef WIN32
#include <windows.h>
#else
#include <cstdio>
#endif

#include "scumm/remaster_videoov.h"
#include "common/fs.h"
#include "common/textconsole.h"

namespace Scumm {

// Frames that are read and dropped to catch up (3 s at 15 fps); a longer jump starts ffmpeg again at the wanted time.
static const int kMaxDrain = 45;

RemasterVideoOverride::RemasterVideoOverride() : _pipe(nullptr), _process(nullptr), _w(0), _h(0), _rate(0), _decoded(-1), _delivered(0), _ended(false) {
}

RemasterVideoOverride::~RemasterVideoOverride() {
	close();
}

Common::String RemasterVideoOverride::find(const Common::String &dir, const Common::String &sanFile) {
	if (dir.empty())
		return Common::String();
	Common::String base = sanFile;
	for (uint i = base.size(); i-- > 0;)
		if (base[i] == '/' || base[i] == '\\') {
			base = base.substr(i + 1);
			break;
		}
	const int dot = base.findLastOf('.');
	if (dot > 0)
		base.erase(dot);
	if (base.empty())
		return Common::String();
	const Common::FSNode d(Common::Path(dir, Common::Path::kNativeSeparator));
	Common::FSList files;
	if (!d.isDirectory() || !d.getChildren(files, Common::FSNode::kListFilesOnly))
		return Common::String();
	static const char *const kExt[] = {".mp4", ".mkv", ".webm", ".mov"};
	for (int i = 0; i < ARRAYSIZE(kExt); i++)
		for (const Common::FSNode &f : files)
			if (f.getName().equalsIgnoreCase(base + kExt[i]))
				return f.getPath().toString(Common::Path::kNativeSeparator);
	return Common::String();
}

bool RemasterVideoOverride::open(const Common::String &path, const Common::String &ffmpeg, int width, int height, int rate) {
	close();
	if (path.empty() || width <= 0 || height <= 0 || rate <= 0)
		return false;
	_path = path;
	_ffmpeg = ffmpeg.empty() ? Common::String("ffmpeg") : ffmpeg;
	_w = width;
	_h = height;
	_rate = rate;
	_delivered = 0;
	_buf.resize((size_t)width * height * 4);
	return launch(0);
}

#ifndef WIN32
// A string as one word for /bin/sh
static Common::String remasterShellQuote(const Common::String &s) {
	Common::String q("'");
	for (uint i = 0; i < s.size(); i++) {
		if (s[i] == '\'')
			q += "'\\''";
		else
			q += s[i];
	}
	return q + "'";
}
#endif

// Starts ffmpeg so that the next frame read is frame `firstFrame` (time firstFrame / rate). The fps filter resamples the
// file to the SMUSH frame rate before scaling, so a replacement made at another frame rate stays in step with the
// audio; scaling fills the picture and crops what is left over (a wider file loses its sides instead of being squeezed).
bool RemasterVideoOverride::launch(int firstFrame) {
	_decoded = firstFrame - 1;
	_ended = false;
	const Common::String seek = firstFrame > 0 ? Common::String::format("-ss %.4f ", (double)firstFrame / _rate) : Common::String();
	const Common::String filter = Common::String::format("fps=%d,scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,crop=%d:%d,setsar=1",
		_rate, _w, _h, _w, _h);
#ifdef WIN32
	const Common::String cmd = Common::String::format("\"%s\" -hide_banner -loglevel error -nostdin %s-i \"%s\" -an -sn -vf %s -f rawvideo -pix_fmt rgba -",
		_ffmpeg.c_str(), seek.c_str(), _path.c_str(), filter.c_str());
	// UTF-8 (ScummVM's paths) to the wide command line
	const int n = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, nullptr, 0);
	if (n <= 0)
		return false;
	Common::Array<wchar_t> line(n);
	MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, line.data(), n);
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	sa.lpSecurityDescriptor = nullptr;
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 1 << 20))
		return false;
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // only the write end goes to ffmpeg
	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
	STARTUPINFOW si;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	si.hStdOutput = wr;
	si.hStdError = nul;   // messages must not end up in the picture data
	si.hStdInput = nul;
	si.dwFlags = STARTF_USESTDHANDLES;
	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof(pi));
	// no console window; ffmpeg ends with a broken pipe when the game stops reading
	const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(wr);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);
	if (!ok) {
		CloseHandle(rd);
		_ended = true;
		warning("remaster: replacement video: could not start ffmpeg (%s); the game's own video is shown. Install ffmpeg or set remaster_ffmpeg_path",
			_ffmpeg.c_str());
		return false;
	}
	CloseHandle(pi.hThread);
	_process = pi.hProcess;
	_pipe = rd;
#else
	// (popen succeeds even when ffmpeg is missing: that shows as no picture on the first read)
	const Common::String cmd = Common::String::format("%s -hide_banner -loglevel error -nostdin %s-i %s -an -sn -vf %s -f rawvideo -pix_fmt rgba - 2>/dev/null </dev/null",
		remasterShellQuote(_ffmpeg).c_str(), seek.c_str(), remasterShellQuote(_path).c_str(), filter.c_str());
	FILE *p = popen(cmd.c_str(), "r");
	if (!p) {
		_ended = true;
		warning("remaster: replacement video: could not start ffmpeg (%s); the game's own video is shown. Install ffmpeg or set remaster_ffmpeg_path",
			_ffmpeg.c_str());
		return false;
	}
	_pipe = p;
#endif
	return true;
}

void RemasterVideoOverride::stopProcess() {
#ifdef WIN32
	if (_pipe) {
		CloseHandle((HANDLE)_pipe);   // ffmpeg's next write fails and it ends
		_pipe = nullptr;
	}
	if (_process) {
		if (WaitForSingleObject((HANDLE)_process, 2000) == WAIT_TIMEOUT)
			TerminateProcess((HANDLE)_process, 0);
		CloseHandle((HANDLE)_process);
		_process = nullptr;
	}
#else
	if (_pipe) {
		pclose((FILE *)_pipe);   // ffmpeg gets SIGPIPE on its next write; pclose waits for it
		_pipe = nullptr;
	}
#endif
}

bool RemasterVideoOverride::readOne() {
	if (!_pipe || _ended)
		return false;
	size_t total = 0;
	const size_t need = _buf.size();
	while (total < need) {
#ifdef WIN32
		DWORD got = 0;
		const DWORD ask = (DWORD)MIN<size_t>(need - total, 1u << 30);
		if (!ReadFile((HANDLE)_pipe, _buf.data() + total, ask, &got, nullptr) || got == 0)
			break;
		total += got;
#else
		const size_t got = fread(_buf.data() + total, 1, need - total, (FILE *)_pipe);
		if (got == 0)
			break;
		total += got;
#endif
	}
	if (total < need) {
		_ended = true;
		if (_delivered == 0)
			warning("remaster: replacement video: no picture from ffmpeg (%s) for %s; the game's own video is shown. Is ffmpeg installed (or "
				"remaster_ffmpeg_path set), and does it read this file?", _ffmpeg.c_str(), _path.c_str());
		else
			warning("remaster: replacement video %s ended at frame %d; the rest of the cutscene shows the game's own video", _path.c_str(), _decoded + 1);
		stopProcess();
		return false;
	}
	_decoded++;
	_delivered++;
	return true;
}

const byte *RemasterVideoOverride::frameFor(int frame) {
	if (!_pipe || _ended || frame < 0)
		return nullptr;
	if (frame <= _decoded && _decoded >= 0) {
		if (frame == _decoded)
			return _buf.data();   // the same SMUSH frame again
		// an earlier frame (the cutscene went back): start again there
		stopProcess();
		if (!launch(frame))
			return nullptr;
	} else if (frame - _decoded > kMaxDrain) {
		// A long jump (the replacement was not shown for a while, e.g. another look during the cutscene): reading and
		// dropping every frame in between would hold the game for as long as ffmpeg takes to make them, so ffmpeg starts
		// again at the wanted time (with the fps filter, frame n of the new run is frame first + n).
		stopProcess();
		if (!launch(frame))
			return nullptr;
	}
	while (_decoded < frame)
		if (!readOne())
			return nullptr;
	return _buf.data();
}

void RemasterVideoOverride::close() {
	stopProcess();
	_decoded = -1;
	_ended = false;
}

} // End of namespace Scumm
