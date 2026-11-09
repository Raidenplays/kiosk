/*
*   This file is part of Universal-Updater
*   Copyright (C) 2019-2021 Universal-Team
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#include "animation.hpp"
#include "kui.hpp"
#include <algorithm>
#include <cmath>
#include "common.hpp"
#include "queueSystem.hpp"
#include "stringutils.hpp"
#include <curl/curl.h>

extern int filesExtracted, extractFilesCount;
extern std::string extractingFile;
char progressBarMsg[128] = "";
bool showProgressBar = false;
ProgressBar progressbarType = ProgressBar::Downloading;
int Animation::DisplayY = 240, Animation::DisplayDelay = 3 * 60;
bool Animation::MoveUp = true, Animation::DoDelay = false;

extern u32 extractSize, writeOffset;
extern u32 installSize, installOffset;
extern u32 copyOffset, copySize;

extern curl_off_t downloadTotal;
extern curl_off_t downloadNow;

/*
	Draw the progressbar.

	u64 currentProgress: The current progress.
	u64 totalProgress: The total progress.
*/
void Animation::DrawProgressBar(u64 currentProgress, u64 totalProgress) {
	Gui::Draw_Rect(30, 120, 342, 30, UITheme.ProgressbarOut());
	Gui::Draw_Rect(31, 121, (int)(((float)currentProgress / (float)totalProgress) * 338.0f), 28, UITheme.ProgressbarIn());
}

/*
	Display the progressbar.
*/
void Animation::displayProgressBar() {
	using namespace KUI;
	char str[256];
	int frame = 0;
	while (showProgressBar) {
		u64 cur = 0, total = 1;
		const char *verb = "Working";
		str[0] = '\0';
		switch (progressbarType) {
			case ProgressBar::Downloading:
				if (downloadTotal < 1.0f) downloadTotal = 1.0f;
				if (downloadTotal < downloadNow) downloadTotal = downloadNow;
				cur = downloadNow; total = downloadTotal; verb = "Downloading";
				break;
			case ProgressBar::Extracting:
				cur = writeOffset; total = extractSize; verb = "Unpacking";
				break;
			case ProgressBar::Installing:
				cur = installOffset; total = installSize; verb = "Installing";
				break;
			case ProgressBar::Copying:
				cur = copyOffset; total = copySize; verb = "Copying";
				break;
		}
		if (total == 0) total = 1;
		const float p = std::clamp((float)cur / (float)total, 0.0f, 1.0f);
		snprintf(str, sizeof(str), "%s of %s", StringUtils::formatBytes(cur).c_str(), StringUtils::formatBytes(total).c_str());
		frame++;

		Gui::clearTextBufs();
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(Top, TRANSPARENT);
		C2D_TargetClear(Bottom, TRANSPARENT);

		Gui::ScreenDraw(Top);
		Rect(0, 0, 400, 240, BG);
		Pattern(400, 240, frame * 0.25f, BG_DOT);
		Rect(0, 0, 400, 22, CARD);
		Rect(0, 22, 400, 1, LINE);
		Pill(6, 3, 64, 16, SKY);
		TextCentered(38, 3, 0.4f, WHITE, "Kiosk");

		const float hop = fabsf(sinf(frame / 12.0f));
		Bag(200, 70 - hop * 10, 48, ACCENT, BG, hop < 0.15f ? 0.12f : 0.0f);
		Card(30, 110, 340, 104, 16);
		TextCentered(200, 120, 0.56f, TEXT, progressBarMsg, 316);
		Pill(50, 152, 300, 14, MUTED);
		if (p > 0.0f) Pill(50, 152, std::max(14.0f, 300 * p), 14, ACCENT);
		Text(50, 176, 0.44f, SUB, verb);
		TextRight(350, 176, 0.44f, SUB, str);
		if (progressbarType == ProgressBar::Extracting) TextCentered(200, 194, 0.4f, SUB, extractingFile, 300);

		Gui::ScreenDraw(Bottom);
		Rect(0, 0, 320, 240, BG);
		Pattern(320, 240, frame * 0.25f, BG_DOT);
		C3D_FrameEnd(0);
	}
}

static int rotation = 0;
extern bool QueueRuns;
extern std::deque<QueueEntry> queueEntries;

void Animation::DrawQueue(int x, int y, int w, int h) {
	GFX::DrawIconRotated(sprites_queue_idx, x + w/2, y + h/2, rotation);
	Gui::DrawStringCentered(x + 20 - 160, y + 11, 0.6f, UITheme.SideBarIconColor(), QueueSystem::Wait ? "!" : std::to_string(queueEntries.size()), 0, 0, font);
}
void Animation::QueueAnimHandle() {
	if (QueueRuns) {
		rotation = (rotation + 3) % 90;
	}
}


#define DISPLAYBOX_UP 206
#define DISPLAYBOX_DOWN 240

void Animation::QueueEntryDone() {
	if (QueueSystem::Popup) {
		Gui::Draw_Rect(0, DisplayY, 400, 34, UITheme.DownListPrev());

		if (QueueSystem::EndMsg != "") {
			Gui::DrawStringCentered(0, DisplayY + 8, 0.6f, UITheme.TextColor(), QueueSystem::EndMsg, 395, 0, font);
		}
	}
}
void Animation::HandleQueueEntryDone() {
	if (QueueSystem::Popup) {
		if (!Animation::DoDelay) {
			if (Animation::MoveUp) {
				if (Animation::DisplayY > DISPLAYBOX_UP) {
					Animation::DisplayY--;

					if (Animation::DisplayY <= DISPLAYBOX_UP) {
						Animation::DisplayDelay = 3 * 60;
						Animation::DoDelay = true;
					}
				}

			} else {
				if (Animation::DisplayY < DISPLAYBOX_DOWN) {
					Animation::DisplayY++;

					if (Animation::DisplayY >= DISPLAYBOX_DOWN) {
						QueueSystem::Popup = false;
						Animation::MoveUp = true;
					}
				}
			}

		} else {
			if (Animation::DisplayDelay > 0) {
				Animation::DisplayDelay--;

				if (Animation::DisplayDelay <= 0) {
					Animation::MoveUp = false;
					Animation::DoDelay = false;
				}
			}
		}
	}
}
