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

#ifndef SCUMM_REMASTER_MENU_H
#define SCUMM_REMASTER_MENU_H

#include "common/str.h"
#include "gui/dialog.h"

namespace GUI {
class CheckboxWidget;
class PopUpWidget;
class SliderWidget;
class StaticTextWidget;
}

namespace Scumm {

// Remaster: everything the hotkeys switch, in one dialog (F9 / controller Back). Only what applies to the running game
// (its profile and setup) is shown; settings that are not shown keep their values.
struct RemasterMenuValues {
	int mode = 0;                 // ScummEngine::RemasterMode
	int backend = 0;              // index into remasterMenuBackends()
	int net = 0;                  // AI model: index into remasterAINetId()
	uint32 netsAvailable = 1;     // bit per AI model whose files are installed
	int bgNet = 0;                // HD background model: index into remasterBgNetId()
	uint32 bgNetsAvailable = 1;   // bit per HD background model that can run here
	int strength = 100;
	bool crt = false, hdActors = true, hdBackgrounds = true, smoothText = true, sharpText = false, grain = true, subtitleBox = false;
	bool performance = false, smoothScroll = false;
	int sides = 1;                // RemasterSides
	// what this setup offers
	bool aiAvailable = true, sidePanels = false, sideArt = false, zoom = false;
	bool retroLooks = true;       // the calculated retro looks (VGA, EGA, Amiga, modern pixel art): high-resolution games
	bool sharpTextChoice = false; // "Sharp text" instead of "Smooth text" (games whose thin font is drawn sharp)
	bool smoothScrollChoice = true;   // smooth scrolling (not where the verb panel is part of the picture)
	bool menuAltF9 = false;       // the menu is on Alt+F9 (the game uses F9 itself)
	Common::String autoChoice;    // what Auto picked on this PC, for the label
};

enum RemasterSides { kRemasterSidesArt, kRemasterSidesGlow, kRemasterSidesZoom, kRemasterSidesZoomPart, kRemasterSidesBlack };

// The upscalers the menu offers on this platform: config value and display name.
int remasterMenuBackendCount();
const char *remasterMenuBackendId(int i);
const char *remasterMenuBackendLabel(int i);

class RemasterMenu : public GUI::Dialog {
public:
	enum { kResultOk = 1, kResultReport = 2 };
	RemasterMenu(const RemasterMenuValues &values);
	void handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) override;
	RemasterMenuValues values() const;

private:
	RemasterMenuValues _in;
	GUI::PopUpWidget *_mode, *_backend, *_net, *_bgNet = nullptr, *_sides = nullptr;
	GUI::SliderWidget *_strength;
	GUI::StaticTextWidget *_strengthText;
	GUI::CheckboxWidget *_crt, *_hdActors, *_hdBackgrounds, *_smoothText = nullptr, *_sharpText = nullptr, *_grain, *_performance, *_subtitleBox,
		*_smoothScroll = nullptr;
	Common::String _shown;   // test aid: what the dialog shows, one entry per control (describe())
	void updateStrengthText();
	void updateEnabled();

public:
	// Test aid: the controls this dialog shows and their values ("Label=value; ..."), for checking each game's menu
	// without a screenshot.
	Common::String describe() const;
};

} // End of namespace Scumm

#endif
