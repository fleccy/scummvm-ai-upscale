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

#ifndef SCUMM_REMASTER_VIDEOOV_H
#define SCUMM_REMASTER_VIDEOOV_H

#include "common/array.h"
#include "common/str.h"

namespace Scumm {

// Replacement cutscene video (remaster_video_override_dir): <dir>/<SAN base name>.mp4 (or .mkv, .webm, .mov; any
// codec the player's own ffmpeg reads) is decoded by an external ffmpeg process through a pipe, resampled to the
// SMUSH file's frame rate and scaled to the presented picture size. Replacement frame N is the picture at time
// N / rate, shown with SMUSH frame N. The SMUSH file keeps playing underneath for timing, audio and subtitles.
class RemasterVideoOverride {
public:
	RemasterVideoOverride();
	~RemasterVideoOverride();
	// The replacement file for the SAN file in dir (base name, case-insensitive, first matching extension); empty
	// when there is none.
	static Common::String find(const Common::String &dir, const Common::String &sanFile);
	// Starts ffmpeg on path: frames of width x height (RGBA), at `rate` frames per second. false: ffmpeg could not be
	// started (logged).
	bool open(const Common::String &path, const Common::String &ffmpeg, int width, int height, int rate);
	void close();
	// open and still delivering: a video that has ended or failed reads as closed
	bool isOpen() const { return _pipe != nullptr && !_ended; }
	// The picture for SMUSH frame `frame` (frames in between are read and dropped; a long jump restarts ffmpeg at that
	// time): RGBA bytes, width x height; nullptr when the video has ended or failed (logged once).
	const byte *frameFor(int frame);
	int width() const { return _w; }
	int height() const { return _h; }
	const Common::String &path() const { return _path; }

private:
	bool readOne();
	bool launch(int firstFrame);
	void stopProcess();
	void *_pipe;      // Windows: HANDLE of the read end; POSIX: FILE *
	void *_process;   // Windows: HANDLE of ffmpeg
	int _w, _h, _rate;
	int _decoded;     // number of the frame in _buf (-1: none yet)
	int _delivered;   // frames read in total (0 at the end: ffmpeg never delivered a picture)
	bool _ended;
	Common::String _path, _ffmpeg;
	Common::Array<byte> _buf;
};

} // End of namespace Scumm

#endif
