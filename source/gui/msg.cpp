/*
	Kiosk-styled versions of Universal-Updater's message, prompt and wait dialogs.
	The engine calls these directly, so restyling them here restyles every popup.
*/
#include "common.hpp"
#include "kui.hpp"
#include "msg.hpp"
#include "sfx.hpp"
#include <algorithm>

using namespace KUI;

extern bool touching(touchPosition touch, Structs::ButtonPos button);

static int msgFrame = 0;

static void Backdrop(float w, float h) {
	Rect(0, 0, w, h, BG);
	Pattern(w, h, msgFrame * 0.25f, BG_DOT);
}

static void TopBar(const std::string &title) {
	Rect(0, 0, 400, 22, CARD);
	Rect(0, 22, 400, 1, LINE);
	Pill(6, 3, 64, 16, SKY);
	TextCentered(38, 3, 0.4f, WHITE, "Kiosk");
	if (!title.empty()) TextCentered(220, 3, 0.48f, TEXT, title, 280);
}

static void BeginFrame() {
	msgFrame++;
	Gui::clearTextBufs();
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(Top, TRANSPARENT);
	C2D_TargetClear(Bottom, TRANSPARENT);
}

/* The bag mascot hopping in place. */
static void Mascot(float cx, float groundY, float size) {
	const float phase = fmodf(msgFrame / 40.0f, 1.0f);
	const float hop = sinf(phase * M_PI);
	const float squash = phase < 0.12f ? (0.12f - phase) * 1.4f : 0.0f;
	Circle(cx, groundY + 2, size * 0.4f * (1.0f - hop * 0.3f), BG_DOT); // Shadow.
	Bag(cx, groundY - size * 0.5f - hop * size * 0.35f, size, ACCENT, BG, squash);
}

static void Dots(float cx, float y) {
	for (int i = 0; i < 3; i++) {
		const float a = 0.5f + 0.5f * sinf(msgFrame / 7.0f - i * 0.9f);
		Circle(cx - 14 + i * 14, y - a * 3, 4, Lerp(ACCENT_SOFT, ACCENT, a));
	}
}

void Msg::DisplayMsg(const std::string &Text, const std::string &BottomText, bool spinner) {
	BeginFrame();

	Gui::ScreenDraw(Top);
	Backdrop(400, 240);
	TopBar("");
	Mascot(200, 120, 56);
	const auto &lines = Wrap(Text, 0.55f, 340);
	const float cardH = 22 + std::min<int>(lines.size(), 3) * 18;
	Card(30, 146, 340, cardH, 14);
	Paragraph(42, 156, 0.55f, TEXT, Text, 316, 3, 18);
	(void)spinner;
	Dots(200, 146 + cardH + 20);

	Gui::ScreenDraw(Bottom);
	Backdrop(320, 240);
	if (!BottomText.empty()) {
		Card(16, 96, 288, 48, 14);
		TextCentered(160, 112, 0.5f, SUB, BottomText, 270);
	}

	C3D_FrameEnd(0);
}

void Msg::DisplayWarnMsg(const std::string &Text) {
	BeginFrame();
	Gui::ScreenDraw(Top);
	Backdrop(400, 240);
	TopBar("");
	Circle(200, 84, 30, BADGE);
	TextCentered(200, 64, 1.0f, WHITE, "!");
	Card(30, 130, 340, 70, 14);
	Paragraph(42, 140, 0.52f, TEXT, Text, 316, 3, 17);
	Gui::ScreenDraw(Bottom);
	Backdrop(320, 240);
	C3D_FrameEnd(0);

	Sfx::Play(Sfx::ERROR);
	for (int i = 0; i < 60 * 3; i++) gspWaitForVBlank();
}

bool Msg::promptMsg(const std::string &promptMsg, const std::string &saveKey, bool saveOnlyYes) {
	if (!saveKey.empty()) {
		switch (config->savedPrompt(saveKey)) {
			case PromptValue::Yes: return true;
			case PromptValue::No: return false;
			case PromptValue::Unset: break;
		}
	}

	const Structs::ButtonPos noBtn = { 20, 150, 132, 46 }, yesBtn = { 168, 150, 132, 46 };
	const Structs::ButtonPos saveBtn = { 60, 96, 200, 30 };
	const float size = 0.5f, lineH = 16.0f;
	const int visible = 10;
	const auto &lines = Wrap(promptMsg, size, 352);
	const int maxScroll = std::max(0, (int)lines.size() - visible);

	bool res = false, save = false;
	int scroll = 0, focus = 1; // Default to "Yes".
	float pressNo = 0, pressYes = 0;

	Sfx::Play(Sfx::OPEN);
	while (true) {
		BeginFrame();

		Gui::ScreenDraw(Top);
		Backdrop(400, 240);
		TopBar("Just checking");
		const float cardH = std::min<int>(lines.size(), visible) * lineH + 24;
		const float cardY = 30 + (200 - cardH) / 2.0f;
		Card(16, cardY, 368, cardH, 14);
		Paragraph(28, cardY + 12, size, TEXT, promptMsg, 344, visible, lineH, scroll);
		if (maxScroll > 0) {
			const float trackH = cardH - 24, thumb = std::max(16.0f, trackH * visible / lines.size());
			Pill(377, cardY + 12, 3, trackH, LINE);
			Pill(377, cardY + 12 + (trackH - thumb) * scroll / maxScroll, 3, thumb, ACCENT);
		}

		Gui::ScreenDraw(Bottom);
		Backdrop(320, 240);
		Text(20, 18, 0.5f, TEXT, "Do you want to continue?");
		if (maxScroll > 0) Text(20, 40, 0.4f, SUB, "Up / Down scrolls the message");

		if (!saveKey.empty()) {
			Pill(saveBtn.x, saveBtn.y, saveBtn.w, saveBtn.h, CARD);
			RoundRect(saveBtn.x + 10, saveBtn.y + 7, 16, 16, 4, save ? ACCENT : MUTED);
			if (save) Check(saveBtn.x + 18, saveBtn.y + 15, 10, WHITE);
			Text(saveBtn.x + 34, saveBtn.y + 7, 0.44f, TEXT, "Remember this (Y)");
		}

		auto button = [&](const Structs::ButtonPos &b, const std::string &label, bool primary, bool focused, float press) {
			const float s = 1.0f - 0.06f * press;
			const float w = b.w * s, h = b.h * s, x = b.x + (b.w - w) / 2, y = b.y + (b.h - h) / 2;
			if (focused) RoundRect(x - 3, y - 3, w + 6, h + 6, h / 2 + 3, ACCENT);
			Card(x, y, w, h, h / 2, primary ? ACCENT : CARD, primary ? Darken(ACCENT, 0.25f) : CARD_SHADOW);
			TextCentered(x + w / 2, y + h / 2 - 9, 0.58f, primary ? WHITE : TEXT, label);
		};
		button(noBtn, "No", false, focus == 0, pressNo);
		button(yesBtn, "Yes", true, focus == 1, pressYes);
		TextCentered(160, 212, 0.4f, SUB, "A  Yes      B  No");

		C3D_FrameEnd(0);

		hidScanInput();
		const u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
		touchPosition t;
		hidTouchRead(&t);
		pressNo = std::max(0.0f, pressNo - 0.2f);
		pressYes = std::max(0.0f, pressYes - 0.2f);

		if (rep & KEY_DOWN && scroll < maxScroll) scroll++;
		if (rep & KEY_UP && scroll > 0) scroll--;
		if (down & (KEY_LEFT | KEY_RIGHT)) {
			focus ^= 1;
			Sfx::Play(Sfx::MOVE);
		}

		if ((down & KEY_A) || (down & KEY_TOUCH && touching(t, yesBtn))) {
			res = (down & KEY_A) ? focus == 1 : true;
			break;
		}
		if ((down & KEY_B) || (down & KEY_TOUCH && touching(t, noBtn))) {
			res = false;
			break;
		}
		if (!saveKey.empty() && ((down & KEY_Y) || (down & KEY_TOUCH && touching(t, saveBtn)))) {
			save = !save;
			Sfx::Play(Sfx::MOVE);
		}
	}

	Sfx::Play(res ? Sfx::SELECT : Sfx::BACK);
	if (save && (!saveOnlyYes || res)) config->savePrompt(saveKey, res);
	return res;
}

void Msg::waitMsg(const std::string &msg) {
	/* Draw once, wait for buttons to be released, then for any press. */
	for (int i = 0; i < 4; i++) {
		BeginFrame();
		Gui::ScreenDraw(Top);
		Backdrop(400, 240);
		TopBar("");
		Card(30, 70, 340, 100, 14);
		Paragraph(42, 82, 0.52f, TEXT, msg, 316, 4, 18);
		Gui::ScreenDraw(Bottom);
		Backdrop(320, 240);
		Card(40, 100, 240, 40, 20);
		TextCentered(160, 111, 0.48f, SUB, "Press any button");
		C3D_FrameEnd(0);
	}

	hidScanInput();
	while (true) {
		gspWaitForVBlank();
		hidScanInput();
		if (hidKeysDown()) break;
	}
	Sfx::Play(Sfx::SELECT);
}
