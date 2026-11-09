/*
	Kiosk — a store-style front end for Universal-DB.
	The download / install engine is Universal-Updater's; this screen replaces its whole UI.
*/
#ifndef _KIOSK_SCREEN_HPP
#define _KIOSK_SCREEN_HPP

#include "common.hpp"
#include "queueSystem.hpp"
#include "storeEntry.hpp"
#include <memory>
#include <string>
#include <vector>

class KioskScreen : public Screen {
public:
	KioskScreen();
	~KioskScreen();
	void Draw(void) const override;
	void Logic(u32 hDown, u32 hHeld, touchPosition touch) override;

private:
	enum Tab { TAB_HOME, TAB_SEARCH, TAB_UPDATES, TAB_DOWNLOADS, TAB_LIBRARY, TAB_COUNT };
	enum class TopMode { Details, WhatsNew, Screenshot };
	enum class ListKind { Search, Updates, Library };
	enum class State { Get, Installed, Update, Queued };

	struct Shelf {
		std::string title;
		std::vector<int> items;
		int sel = 0;
		float scroll = 0, scrollTarget = 0;
	};

	struct List {
		std::vector<int> items;
		int sel = 0;
		float scroll = 0, scrollTarget = 0;
	};

	/* Data. */
	std::vector<Shelf> shelves;
	std::vector<int> allSorted; // Every entry, A-Z.
	List search, updates, library;
	std::string query;
	bool storeOk = false;

	/* Navigation. */
	int tab = TAB_HOME;
	int shelfSel = 0;
	float homeScroll = 0, homeScrollTarget = 0;
	int downloadSel = 0;

	/* App page. */
	bool appOpen = false;
	int appIdx = -1, appSel = 0;
	float appAnim = 0, appScroll = 0, appScrollTarget = 0;
	TopMode topMode = TopMode::Details;
	int notesScroll = 0;
	C2D_Image shot = { nullptr, nullptr };
	int shotIdx = 0;
	bool shotFailed = false;

	/* Download scene + celebration. */
	struct Gummy { float x, y, vy; u32 color; };
	struct Confetti { float x, y, vx, vy, rot, vrot, w, h; u32 color; };
	struct Fly { C2D_Image icon = { nullptr, nullptr }; float sx = 0, sy = 0, ex = 0, ey = 0, t = 1.0f; };
	std::vector<Gummy> gummies;
	std::vector<Confetti> confetti;
	Fly fly;
	float jarLevel = 0.0f, railOffset = 0.0f, floorPhase = 0.0f, ripple = 0.0f, bagDrop = 0.0f;
	int floorShift = 0, dropTimer = 0;
	bool celebrating = false;
	int celebT = 0, lastSerial = 0;
	std::shared_ptr<StoreEntry> celebEntry;

	/* Feel. */
	float pressAnim = 0.0f, tabFade = 0.0f, tabBounce = 0.0f, dockBounce = 0.0f;
	int lastSig = 0, lastTab = 0;
	int today = 0; // Days since epoch, for "New" ribbons.

	/* Animation. */
	int frame = 0;
	float tabPillX = 0;
	float focusPop = 1.0f, heroFade = 1.0f;
	int lastFocus = -2;
	u32 heroColor;
	float fadeIn = 255.0f;
	std::string toast;
	int toastTimer = 0;
	float toastY = -30.0f;
	bool requestToastShown = false;

	/* Setup. */
	void LoadStore();
	void BuildShelves();
	void RunSearch();
	void RefreshLists();

	/* Helpers. */
	int FocusedEntry() const;
	State StateOf(int idx) const;
	bool IsInstalled(int idx) const;
	bool InQueue(int idx, int script = -1) const;
	bool QueueScript(int idx, int script);
	bool UpdateEntry(int idx, bool quiet = false);
	void UpdateAll();
	void OpenApp(int idx);
	void CloseApp();
	void FreeShot();
	void LoadShot();
	void ShowToast(const std::string &msg);
	void RespondToRequest();
	static void KeepVisible(List &list, float viewH, float rowStride);
	bool IsNew(int idx) const;
	void StartFly(int idx, float sx, float sy);
	float OverallProgress(const QueueEntry &q, float *stepP = nullptr) const;
	int CursorSignature() const;

	/* Logic. */
	void HomeLogic(u32 down, u32 rep, touchPosition t);
	void ListLogic(List &list, ListKind kind, u32 down, u32 rep, touchPosition t);
	void SearchLogic(u32 down, u32 rep, touchPosition t);
	void UpdatesLogic(u32 down, u32 rep, touchPosition t);
	void DownloadsLogic(u32 down, u32 rep, touchPosition t);
	void AppLogic(u32 down, u32 rep, touchPosition t);
	void Animate();

	/* Drawing. */
	void DrawChrome(const std::string &title, const std::string &right = "") const;
	void DrawStatusBar() const;
	void DrawFloorScene(float horizon) const;
	void DrawJar(float cx, float top, float size, float level, const C2D_Image &icon, u32 frameColor, float scale, float alpha) const;
	void DrawCelebration() const;
	void DrawFly() const;
	void DrawDock(float cx, float cy) const;
	void DrawFocusRing(float x, float y, float w, float h, float r) const;
	void DrawHero(int idx, bool detailed) const;
	void DrawEmptyTop(const std::string &title, const std::string &subtitle, int glyph) const;
	void DrawWhatsNewTop(int idx) const;
	void DrawScreenshotTop(int idx) const;
	void DrawDownloadsTop() const;
	void DrawToast() const;

	void DrawTabBar() const;
	void DrawHome() const;
	void DrawList(const List &list, ListKind kind, float y0, float y1) const;
	void DrawSearch() const;
	void DrawUpdates() const;
	void DrawDownloads() const;
	void DrawEmptyBottom(const std::string &title, const std::string &subtitle, float y) const;
	void DrawAppPage(float offsetX) const;
	void DrawPillButton(float x, float y, float w, float h, State s, bool focused) const;
};

#endif
