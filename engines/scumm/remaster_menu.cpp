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

#include "common/system.h"
#include "gui/gui-manager.h"
#include "gui/widget.h"
#include "gui/widgets/popup.h"

#include "scumm/remaster_ai.h"
#include "scumm/remaster_menu.h"

namespace Scumm {

namespace {

struct BackendEntry {
	const char *id, *label;
};

const BackendEntry kBackends[] = {
	{"auto", "Auto: the fastest on this PC"},
	{"ncnn", "Real-ESRGAN AI (Vulkan)"},
#ifdef WIN32
	{"dml", "Real-ESRGAN AI (DirectML)"},
	{"dml-ultra", "Real-ESRGAN Ultra (DirectML)"},
	{"trt", "Real-ESRGAN AI (TensorRT)"},
	{"trt-ultra", "Real-ESRGAN Ultra (TensorRT)"},
#endif
	{"fsr", "AMD FSR 1 (no AI)"},
};

const char *const kModes[] = {
	"AI HD", "Original pixels", "VGA (1992)", "EGA (1990)", "Amiga (32 colours)", "Modern pixel art", "Classic 320x200 (experimental)"
};

enum {
	kOkCmd = 'RMok',
	kCancelCmd = 'RMca',
	kReportCmd = 'RMrp',
	kStrengthCmd = 'RMst',
	kModeCmd = 'RMmo'
};

} // End of anonymous namespace

int remasterMenuBackendCount() { return ARRAYSIZE(kBackends); }
const char *remasterMenuBackendId(int i) { return kBackends[CLIP<int>(i, 0, ARRAYSIZE(kBackends) - 1)].id; }
const char *remasterMenuBackendLabel(int i) { return kBackends[CLIP<int>(i, 0, ARRAYSIZE(kBackends) - 1)].label; }

RemasterMenu::RemasterMenu(const RemasterMenuValues &values) : GUI::Dialog(0, 0, 16, 16), _in(values) {
	// Laid out in code from the font size, so it needs nothing from the theme files and fits any window.
	const int ow = g_system->getOverlayWidth(), oh = g_system->getOverlayHeight();
	const int fh = g_gui.getFontHeight();
	const int row = fh + MAX(8, fh * 3 / 4), ctl = fh + MAX(6, fh / 2), pad = fh;
	const int w = MIN(ow - 2 * pad, fh * 38);
	const int labelW = w * 34 / 100, ctlX = pad + labelW, ctlW = w - labelW - 2 * pad;
	const int half = (w - 2 * pad) / 2;
	int y = pad;
	using GUI::StaticTextWidget;
	auto heading = [&](const char *text) {
		new StaticTextWidget(this, pad, y, w - 2 * pad, ctl, Common::U32String(text), Graphics::kTextAlignLeft);
		y += row;
	};
	auto label = [&](const char *text) {
		new StaticTextWidget(this, pad + fh, y, labelW - fh, ctl, Common::U32String(text), Graphics::kTextAlignLeft, Common::U32String(),
		                     GUI::ThemeEngine::kFontStyleNormal);
	};

	// test aid (describe()): what is shown, as "Label=value" entries
	auto shown = [&](const Common::String &name, const Common::String &value) {
		if (!_shown.empty())
			_shown += "; ";
		_shown += name + "=" + value;
	};
	auto entry = [&](GUI::PopUpWidget *popup, Common::String &list, const Common::String &text, int tag, int selected) {
		popup->appendEntry(Common::U32String(text), tag);
		list += (list.empty() ? "" : "|") + text + (tag == selected ? "*" : "");
	};

	heading("Remaster settings");
	label("Look");
	_mode = new GUI::PopUpWidget(this, ctlX, y, ctlW, ctl, Common::U32String("F10 switches the look in game"), kModeCmd);
	{
		// The retro looks are made at half the game's resolution: they suit high-resolution games only. A look set
		// in the configuration file is listed anyway, so it stays selected.
		Common::String list;
		for (int m = 0; m < ARRAYSIZE(kModes); m++)
			if ((m != 0 || _in.aiAvailable) && (m <= 1 || _in.retroLooks || m == _in.mode))
				entry(_mode, list, kModes[m], m, _in.mode);
		shown("Look", list);
	}
	_mode->setSelectedTag(_in.mode);
	y += row;
	label("Upscaler");
	_backend = new GUI::PopUpWidget(this, ctlX, y, ctlW, ctl, Common::U32String("Ctrl+F9 switches it in game"));
	{
		Common::String list;
		for (int b = 0; b < remasterMenuBackendCount(); b++) {
			if (b == 0 && !_in.autoChoice.empty())
				entry(_backend, list, "Auto (" + _in.autoChoice + ")", b, _in.backend);
			else
				entry(_backend, list, remasterMenuBackendLabel(b), b, _in.backend);
		}
		shown("Upscaler", list);
	}
	_backend->setSelectedTag(_in.backend);
	y += row;
	label("AI model");
	_net = new GUI::PopUpWidget(this, ctlX, y, ctlW, ctl, Common::U32String("The network that upscales the live picture (other than the standard one: Vulkan only)"));
	{
		Common::String list;
		for (int k = 0; k < remasterAINetCount() && k < 32; k++)
			if ((_in.netsAvailable >> k) & 1)
				entry(_net, list, remasterAINetLabel(k), k, _in.net);
		shown("AI model", list);
	}
	_net->setSelectedTag(_in.net);
	y += row;
	if (_in.bgNetsAvailable & ~1u) {   // only when there is a choice (UltraSharpV2 needs DirectML / TensorRT)
		label("HD background model");
		_bgNet = new GUI::PopUpWidget(this, ctlX, y, ctlW, ctl, Common::U32String("The network that makes the HD backgrounds, once per room (cached on disk)"));
		Common::String list;
		for (int k = 0; k < remasterBgNetCount() && k < 32; k++)
			if ((_in.bgNetsAvailable >> k) & 1)
				entry(_bgNet, list, remasterBgNetLabel(k), k, _in.bgNet);
		shown("HD background model", list);
		_bgNet->setSelectedTag(_in.bgNet);
		y += row;
	}
	label("AI strength");
	shown("AI strength", Common::String::format("%d", _in.strength));
	_strength = new GUI::SliderWidget(this, ctlX, y, ctlW - fh * 4, ctl, Common::U32String("How much of the AI picture is used (Ctrl+wheel in game)"), kStrengthCmd);
	_strength->setMinValue(0);
	_strength->setMaxValue(100);
	_strength->setValue(_in.strength);
	_strengthText = new StaticTextWidget(this, ctlX + ctlW - fh * 4 + fh / 2, y, fh * 4 - fh / 2, ctl, Common::U32String(), Graphics::kTextAlignLeft,
	                                     Common::U32String(), GUI::ThemeEngine::kFontStyleNormal);
	updateStrengthText();
	y += row;
	_hdActors = new GUI::CheckboxWidget(this, pad + fh, y, half - fh, ctl, Common::U32String("HD characters"),
	                                    Common::U32String("Characters drawn in extra detail"));
	_hdBackgrounds = new GUI::CheckboxWidget(this, pad + half, y, half, ctl, Common::U32String("HD backgrounds"),
	                                         Common::U32String("Backgrounds upscaled by a larger AI model, cached on disk"));
	y += row;
	if (_in.sharpTextChoice)
		// games with a thin font without outlines: smooth letters lose its diagonal strokes, so the choice is between
		// rounded pixel edges and exact pixel letters
		_sharpText = new GUI::CheckboxWidget(this, pad + fh, y, half - fh, ctl, Common::U32String("Sharp text"),
		                                     Common::U32String("Letters drawn with rounded pixel edges (off: smooth letters in AI HD, exact pixels in the other looks)"));
	else
		_smoothText = new GUI::CheckboxWidget(this, pad + fh, y, half - fh, ctl, Common::U32String("Smooth text"),
		                                      Common::U32String("Anti-aliased subtitles in AI HD (off: exact pixel letters)"));
	_grain = new GUI::CheckboxWidget(this, pad + half, y, half, ctl, Common::U32String("Grain guard"),
	                                 Common::U32String("Keeps dithered areas from shimmering (Shift+F11)"));
	y += row;
	_crt = new GUI::CheckboxWidget(this, pad + fh, y, half - fh, ctl, Common::U32String("CRT scanlines"), Common::U32String("Ctrl+F10"));
	_performance = new GUI::CheckboxWidget(this, pad + half, y, half, ctl, Common::U32String("Performance mode *"),
	                                       Common::U32String("For older graphics cards: 2x output, no HD characters, no grain guard"));
	y += row;
	_subtitleBox = new GUI::CheckboxWidget(this, pad + fh, y, w - 2 * pad - fh, ctl, Common::U32String("Subtitle background"),
	                                       Common::U32String("A soft dark box behind subtitles and object names, for easier reading"));
	_subtitleBox->setState(_in.subtitleBox);
	y += row;
	_hdActors->setState(_in.hdActors);
	_hdBackgrounds->setState(_in.hdBackgrounds);
	if (_smoothText)
		_smoothText->setState(_in.smoothText);
	if (_sharpText)
		_sharpText->setState(_in.sharpText);
	_grain->setState(_in.grain);
	_crt->setState(_in.crt);
	_performance->setState(_in.performance);
	auto onOff = [](bool b) { return Common::String(b ? "on" : "off"); };
	shown("HD characters", onOff(_in.hdActors));
	shown("HD backgrounds", onOff(_in.hdBackgrounds));
	if (_sharpText)
		shown("Sharp text", onOff(_in.sharpText));
	else
		shown("Smooth text", onOff(_in.smoothText));
	shown("Grain guard", onOff(_in.grain));
	shown("CRT scanlines", onOff(_in.crt));
	shown("Performance mode", onOff(_in.performance));
	shown("Subtitle background", onOff(_in.subtitleBox));

	if (_in.sidePanels) {
		y += row / 3;
		heading("Widescreen");
		label("Narrow rooms");
		_sides = new GUI::PopUpWidget(this, ctlX, y, ctlW, ctl, Common::U32String("F12 switches it in game"));
		Common::String list;
		if (_in.sideArt)
			entry(_sides, list, "Side art", kRemasterSidesArt, _in.sides);
		entry(_sides, list, "Ambient glow", kRemasterSidesGlow, _in.sides);
		// zoom: where the game offers it, or when the configuration file asks for it anyway (so it stays selected)
		if (_in.zoom || _in.sides == kRemasterSidesZoom)
			entry(_sides, list, "Zoom to fill (experimental)", kRemasterSidesZoom, _in.sides);
		if (_in.zoom || _in.sides == kRemasterSidesZoomPart)
			entry(_sides, list, "Zoom part-way, 16:10 (experimental)", kRemasterSidesZoomPart, _in.sides);
		entry(_sides, list, "Black bars", kRemasterSidesBlack, _in.sides);
		shown("Narrow rooms", list);
		_sides->setSelectedTag(_in.sides);
		y += row;
		if (_in.smoothScrollChoice) {
			_smoothScroll = new GUI::CheckboxWidget(this, pad + fh, y, w - 2 * pad - fh, ctl, Common::U32String("Smooth scrolling (experimental)"),
			                                        Common::U32String("Ctrl+Shift+F9; AI HD with HD characters only"));
			_smoothScroll->setState(_in.smoothScroll);
			shown("Smooth scrolling", onOff(_in.smoothScroll));
			y += row;
		}
	}

	y += row / 3;
	new StaticTextWidget(this, pad, y, w - 2 * pad, ctl, Common::U32String("* takes effect after restarting the game"),
	                     Graphics::kTextAlignLeft, Common::U32String(), GUI::ThemeEngine::kFontStyleNormal);
	y += row;
	const char *keys = _in.menuAltF9 ? (_in.sidePanels ? "In game: Alt+F9 menu, F10 look, F11 AI, F12 narrow rooms, Shift+F9 photo"
	                                                   : "In game: Alt+F9 menu, F10 look, F11 AI, Shift+F9 photo") :
	                   _in.sidePanels ? "In game: F9 menu, F10 look, F11 AI, F12 narrow rooms, Shift+F9 photo"
	                                  : "In game: F9 menu, F10 look, F11 AI, Shift+F9 photo";
	new StaticTextWidget(this, pad, y, w - 2 * pad, ctl, Common::U32String(keys), Graphics::kTextAlignLeft, Common::U32String(),
	                     GUI::ThemeEngine::kFontStyleNormal);
	shown("Keys", keys);
	y += row + row / 3;
	const int bw = MAX(fh * 7, (w - 2 * pad) / 4), gap = fh / 2;
	new GUI::ButtonWidget(this, pad, y, bw + fh * 2, ctl, Common::U32String("Report a problem"),
	                      Common::U32String("Screenshots and a pre-filled GitHub issue (Ctrl+F12)"), kReportCmd);
	new GUI::ButtonWidget(this, w - pad - 2 * bw - gap, y, bw, ctl, Common::U32String("Cancel"), Common::U32String(), kCancelCmd, Common::ASCII_ESCAPE);
	new GUI::ButtonWidget(this, w - pad - bw, y, bw, ctl, Common::U32String("OK"), Common::U32String(), kOkCmd, Common::ASCII_RETURN);
	y += ctl + pad;

	_w = w;
	_h = MIN(y, oh);
	_x = (ow - _w) / 2;
	_y = MAX(0, (oh - _h) / 2);
	updateEnabled();
}

void RemasterMenu::updateStrengthText() {
	_strengthText->setLabel(Common::U32String(Common::String::format("%d%%", _strength->getValue())));
	_strengthText->markAsDirty();
}

void RemasterMenu::updateEnabled() {
	// the AI-only settings are greyed out unless the look is AI HD
	const bool ai = _mode->getSelectedTag() == 0;
	_backend->setEnabled(_in.aiAvailable);
	_net->setEnabled(_in.aiAvailable && (_in.netsAvailable & ~1u) != 0);
	_strength->setEnabled(ai);
	_hdActors->setEnabled(ai);
	_hdBackgrounds->setEnabled(ai);
	if (_bgNet)
		_bgNet->setEnabled(ai);
	if (_smoothText)
		_smoothText->setEnabled(ai);   // sharp text applies to every look
	_grain->setEnabled(ai);
	if (_smoothScroll)
		_smoothScroll->setEnabled(ai);
	markWidgetsAsDirty();
}

void RemasterMenu::handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) {
	switch (cmd) {
	case kStrengthCmd:
		updateStrengthText();
		break;
	case kModeCmd:
		updateEnabled();
		break;
	case kOkCmd:
		setResult(kResultOk);
		close();
		break;
	case kReportCmd:
		setResult(kResultReport);
		close();
		break;
	case kCancelCmd:
		setResult(0);
		close();
		break;
	default:
		GUI::Dialog::handleCommand(sender, cmd, data);
	}
}

Common::String RemasterMenu::describe() const {
	return _shown;
}

RemasterMenuValues RemasterMenu::values() const {
	RemasterMenuValues v = _in;
	v.mode = _mode->getSelectedTag();
	v.backend = _backend->getSelectedTag();
	v.net = _net->getSelectedTag();
	if (_bgNet)
		v.bgNet = _bgNet->getSelectedTag();
	v.strength = _strength->getValue();
	v.hdActors = _hdActors->getState();
	v.hdBackgrounds = _hdBackgrounds->getState();
	if (_smoothText)
		v.smoothText = _smoothText->getState();
	if (_sharpText)
		v.sharpText = _sharpText->getState();
	v.grain = _grain->getState();
	v.crt = _crt->getState();
	v.performance = _performance->getState();
	v.subtitleBox = _subtitleBox->getState();
	if (_sides)
		v.sides = _sides->getSelectedTag();
	if (_smoothScroll)
		v.smoothScroll = _smoothScroll->getState();
	return v;
}

} // End of namespace Scumm
