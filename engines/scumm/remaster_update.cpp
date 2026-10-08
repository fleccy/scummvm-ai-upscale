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

// COMI remaster: "a newer version is available" notice. Once a day, on a background thread, asks GitHub for the
// latest release's tag (one HTTPS GET of the public releases API, nothing is sent besides the request itself) and,
// if it is newer than this build, shows a message in game. Nothing is downloaded or installed.
// remaster_update_check=false switches it off.

// WinHTTP (part of Windows) is used directly.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "common/config-manager.h"
#include "common/system.h"

#include "scumm/scumm.h"
#include "scumm/remaster_version.h"

#ifdef WIN32
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <string>
#include <thread>
#endif

namespace Scumm {

#ifdef WIN32

namespace {

std::atomic<int> g_updState{0};   // 0 idle, 1 running, 2 done
std::string g_updTag;
std::thread g_updThread;

typedef HINTERNET(WINAPI *OpenFn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *ConnectFn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *OpenReqFn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *SendFn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *RecvFn)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *ReadFn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *CloseFn)(HINTERNET);
typedef BOOL(WINAPI *TimeoutsFn)(HINTERNET, int, int, int, int);

void fetchLatestTag() {
	std::string body;
	HMODULE dll = LoadLibraryW(L"winhttp.dll");
	if (dll) {
		OpenFn open = (OpenFn)GetProcAddress(dll, "WinHttpOpen");
		ConnectFn connect = (ConnectFn)GetProcAddress(dll, "WinHttpConnect");
		OpenReqFn openReq = (OpenReqFn)GetProcAddress(dll, "WinHttpOpenRequest");
		SendFn send = (SendFn)GetProcAddress(dll, "WinHttpSendRequest");
		RecvFn recv = (RecvFn)GetProcAddress(dll, "WinHttpReceiveResponse");
		ReadFn read = (ReadFn)GetProcAddress(dll, "WinHttpReadData");
		CloseFn close = (CloseFn)GetProcAddress(dll, "WinHttpCloseHandle");
		TimeoutsFn timeouts = (TimeoutsFn)GetProcAddress(dll, "WinHttpSetTimeouts");
		if (open && connect && openReq && send && recv && read && close) {
			HINTERNET s = open(L"scummvm-ai-upscale", 0 /* default proxy */, nullptr, nullptr, 0);
			if (s) {
				if (timeouts)
					timeouts(s, 5000, 5000, 5000, 5000);
				HINTERNET c = connect(s, L"api.github.com", 443, 0);
				if (c) {
					HINTERNET r = openReq(c, L"GET", L"/repos/" REMASTER_GITHUB_REPO_W L"/releases/latest", nullptr, nullptr, nullptr, 0x00800000 /* SECURE */);
					if (r) {
						if (send(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, nullptr, 0, 0, 0) && recv(r, nullptr)) {
							char buf[4096];
							DWORD got = 0;
							while (body.size() < 200000 && read(r, buf, sizeof(buf), &got) && got)
								body.append(buf, got);
						}
						close(r);
					}
					close(c);
				}
				close(s);
			}
		}
		FreeLibrary(dll);
	}
	const size_t k = body.find("\"tag_name\"");
	if (k != std::string::npos) {
		const size_t q1 = body.find('"', body.find(':', k) + 1);
		const size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
		if (q2 != std::string::npos && q2 - q1 < 40)
			g_updTag = body.substr(q1 + 1, q2 - q1 - 1);
	}
	g_updState = 2;
}

// v0.9.0-rc2 -> {0,9,0,2}; a final release sorts after its release candidates
bool parseVersion(const char *v, int out[4]) {
	while (*v && (*v < '0' || *v > '9'))
		v++;
	if (sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]) != 3)
		return false;
	const char *rc = strstr(v, "-rc");
	out[3] = rc ? atoi(rc + 3) : 9999;
	return true;
}

} // anonymous namespace

void ScummEngine::remasterUpdateCheckStart() {
	if (ConfMan.hasKey("remaster_update_check") && !ConfMan.getBool("remaster_update_check"))
		return;
	TimeDate td;
	_system->getTimeAndDate(td);
	const int day = (td.tm_year + 1900) * 400 + td.tm_mon * 32 + td.tm_mday;
	if (!ConfMan.hasKey("remaster_update_test_tag") && ConfMan.hasKey("remaster_update_last_day") && ConfMan.getInt("remaster_update_last_day") == day)
		return;
	ConfMan.setInt("remaster_update_last_day", day);
	ConfMan.flushToDisk();
	if (ConfMan.hasKey("remaster_update_test_tag")) { // testing: pretend this is the latest release
		g_updTag = ConfMan.get("remaster_update_test_tag").c_str();
		g_updState = 2;
		return;
	}
	g_updState = 1;
	g_updThread = std::thread(fetchLatestTag);
}

void ScummEngine::remasterUpdateCheckPoll() {
	if (g_updState != 2)
		return;
	g_updState = 0;
	if (g_updThread.joinable())
		g_updThread.join();
	int a[4], b[4];
	if (g_updTag.empty() || !parseVersion(g_updTag.c_str(), a) || !parseVersion(REMASTER_VERSION, b))
		return;
	bool newer = false;
	for (int i = 0; i < 4; i++)
		if (a[i] != b[i]) {
			newer = a[i] > b[i];
			break;
		}
	if (!newer)
		return;
	warning("remaster: version %s is available (this is %s)", g_updTag.c_str(), REMASTER_VERSION);
	_system->displayMessageOnOSD(Common::U32String(Common::String::format("scummvm-ai-upscale %s is available: github.com/%s/releases",
		g_updTag.c_str(), REMASTER_GITHUB_REPO)));
}

void ScummEngine::remasterUpdateCheckStop() {
	if (g_updThread.joinable())
		g_updThread.join();
}

#else

void ScummEngine::remasterUpdateCheckStart() {}
void ScummEngine::remasterUpdateCheckPoll() {}
void ScummEngine::remasterUpdateCheckStop() {}

#endif

} // End of namespace Scumm
