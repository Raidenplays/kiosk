#include "kioskScreen.hpp"
#include "download.hpp"
#include "keyboard.hpp"
#include "kui.hpp"
#include "queueSystem.hpp"
#include "scriptUtils.hpp"
#include "storeUtils.hpp"
#include <algorithm>
#include <cmath>
#include <curl/curl.h>
#include <deque>
#include <unistd.h>

using namespace KUI;

/* Universal-Updater engine state we read for progress. */
extern std::deque<QueueEntry> queueEntries;
extern bool exiting, QueueRuns;
extern int fadeAlpha;
extern u32 extractSize, writeOffset, installSize, installOffset, copyOffset, copySize;
extern curl_off_t downloadTotal, downloadNow, downloadSpeed;
extern CURL *CurlHandle;

/* Layout. */
static constexpr float TAB_Y = 204.0f, TAB_W = 64.0f;
static constexpr float SHELF_H = 92.0f, TILE = 60.0f, TILE_STRIDE = 68.0f, SHELF_X = 12.0f;
static constexpr float ROW_H = 48.0f, ROW_STRIDE = 52.0f;
static constexpr float SCRIPT_ROW_H = 44.0f, SCRIPT_STRIDE = 48.0f, SCRIPT_Y0 = 64.0f, SCRIPT_Y1 = 196.0f;
static constexpr float SEARCH_Y0 = 48.0f, UPDATES_Y0 = 40.0f, DOWNLOAD_Y0 = 8.0f;

enum Glyph { GLYPH_CHECK, GLYPH_DOWNLOAD, GLYPH_SEARCH, GLYPH_LIBRARY, GLYPH_WARN };

static const std::shared_ptr<StoreEntry> &E(int i) { return StoreUtils::allEntries[i]; }
static int EntryCount() { return (int)StoreUtils::allEntries.size(); }

static bool hit(const touchPosition &t, float x, float y, float w, float h) {
	return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

/*
	Remove a waiting (not running) queue item without moving element 0 in memory:
	the engine thread holds a reference to the running entry, and a plain deque::erase
	in the middle can relocate it.
*/
static void RemoveQueued(int i) {
	if (i <= 0 || i >= (int)queueEntries.size()) return;
	std::move(queueEntries.begin() + i + 1, queueEntries.end(), queueEntries.begin() + i);
	queueEntries.pop_back();
}

static std::string Upper(std::string s) {
	for (char &c : s) c = toupper((unsigned char)c);
	return s;
}

static std::string DateOnly(const std::string &s) { return s.size() > 10 ? s.substr(0, 10) : s; }

static std::string PrettyCategory(const std::string &c) {
	if (c == "game") return "Games";
	if (c == "utility") return "Utilities";
	if (c == "emulator") return "Emulators";
	if (c == "app") return "Apps";
	if (c == "firm") return "Firmware & Payloads";
	if (c == "save-tool") return "Save Tools";
	if (c == "luma-plugin") return "Luma Plugins";
	if (c == "theme") return "Themes";
	if (c.empty()) return "Other";
	std::string out = c;
	for (char &ch : out) if (ch == '-' || ch == '_') ch = ' ';
	out[0] = toupper((unsigned char)out[0]);
	return out;
}

/*
	Construction / store loading.
*/
KioskScreen::KioskScreen() {
	this->heroColor = ACCENT;
	this->LoadStore();
	this->BuildShelves();
	this->RunSearch();
	this->RefreshLists();
	this->tabPillX = this->tab * TAB_W;
}

KioskScreen::~KioskScreen() { this->FreeShot(); }

void KioskScreen::LoadStore() {
	static const std::string storeFile = "universal-db.unistore";
	static const std::string storeUrl = "https://db.universal-team.net/unistore/universal-db.unistore";
	const std::string storePath = _STORE_PATH + storeFile;

	Msg::DisplayMsg("Opening the store…");
	config->lastStore(storeFile);

	Store::UpdateMode mode = Store::UpdateMode::automatic;
	if (access(storePath.c_str(), F_OK) != 0) {
		if (checkWifiStatus()) {
			DownloadUniStore(storeUrl, -1, "Downloading Universal-DB…");
			mode = Store::UpdateMode::spritesheet;
		} else {
			notConnectedMsg();
		}
	}

	StoreUtils::store = std::make_unique<Store>(storePath, storeFile, mode, true);
	if (!StoreUtils::store->GetValid() && checkWifiStatus()) {
		DownloadUniStore(storeUrl, -1, "Downloading Universal-DB…");
		StoreUtils::store = std::make_unique<Store>(storePath, storeFile, Store::UpdateMode::spritesheet, true);
	}

	this->storeOk = StoreUtils::store && StoreUtils::store->GetValid() && EntryCount() > 0;
	if (this->storeOk) StoreUtils::RefreshInstalledApps("");
}

void KioskScreen::BuildShelves() {
	this->shelves.clear();
	this->allSorted.clear();
	if (!this->storeOk) return;

	const int n = EntryCount();
	std::vector<int> idx;
	for (int i = 0; i < n; i++) if (E(i)) idx.push_back(i);

	this->allSorted = idx;
	std::sort(this->allSorted.begin(), this->allSorted.end(), [](int a, int b) {
		return strcasecmp(E(a)->GetTitle().c_str(), E(b)->GetTitle().c_str()) < 0;
	});

	auto byStars = [](int a, int b) { return E(a)->GetStars() > E(b)->GetStars(); };
	auto byDate = [](int a, int b) { return E(a)->GetLastUpdated() > E(b)->GetLastUpdated(); };

	Shelf fresh { "Recently updated" };
	fresh.items = idx;
	std::stable_sort(fresh.items.begin(), fresh.items.end(), byDate);
	if (fresh.items.size() > 20) fresh.items.resize(20);
	this->shelves.push_back(fresh);

	Shelf popular { "Popular" };
	popular.items = idx;
	std::stable_sort(popular.items.begin(), popular.items.end(), byStars);
	if (popular.items.size() > 20) popular.items.resize(20);
	this->shelves.push_back(popular);

	for (const std::string &cat : StoreUtils::store->GetCategories()) {
		Shelf s { PrettyCategory(cat) };
		for (int i : idx) {
			const auto &cats = E(i)->GetCategories();
			if (std::find(cats.begin(), cats.end(), cat) != cats.end()) s.items.push_back(i);
		}
		if (s.items.size() < 3) continue;
		std::stable_sort(s.items.begin(), s.items.end(), byStars);
		if (s.items.size() > 40) s.items.resize(40);
		this->shelves.push_back(s);
	}
}

void KioskScreen::RunSearch() {
	this->search.items.clear();
	if (this->query.empty()) {
		this->search.items = this->allSorted;
	} else {
		const std::string q = Upper(this->query);
		std::vector<int> titleHits, otherHits;
		for (int i : this->allSorted) {
			if (Upper(E(i)->GetTitle()).find(q) != std::string::npos) titleHits.push_back(i);
			else if (Upper(E(i)->GetAuthor()).find(q) != std::string::npos || Upper(E(i)->GetDescription()).find(q) != std::string::npos) otherHits.push_back(i);
		}
		this->search.items = titleHits;
		this->search.items.insert(this->search.items.end(), otherHits.begin(), otherHits.end());
	}
	this->search.sel = this->search.items.empty() ? -1 : 0;
	this->search.scroll = this->search.scrollTarget = 0;
}

/* Updates and Library are live views over the engine's state, so rebuild them when anything changes. */
void KioskScreen::RefreshLists() {
	std::vector<int> u, l;
	for (int i : this->allSorted) {
		if (E(i)->GetUpdateAvl()) u.push_back(i);
		if (this->IsInstalled(i)) l.push_back(i);
	}

	auto apply = [](List &list, std::vector<int> &items) {
		if (items == list.items) return;
		list.items.swap(items);
		list.sel = std::clamp(list.sel, 0, std::max(0, (int)list.items.size() - 1));
	};
	apply(this->updates, u);
	apply(this->library, l);
}

/*
	Helpers.
*/
bool KioskScreen::IsInstalled(int idx) const {
	const auto &e = E(idx);
	if (e->GetInstalled()) return true;
	for (const Script &s : e->GetScripts()) if (s.IsInstalled()) return true;
	return false;
}

bool KioskScreen::InQueue(int idx, int script) const {
	const StoreEntry *ptr = E(idx).get();
	for (const QueueEntry &q : queueEntries) {
		if (q.storeEntry.get() == ptr && (script < 0 || q.scriptIndex == script)) return true;
	}
	return false;
}

KioskScreen::State KioskScreen::StateOf(int idx) const {
	if (this->InQueue(idx)) return State::Queued;
	if (E(idx)->GetUpdateAvl()) return State::Update;
	if (this->IsInstalled(idx)) return State::Installed;
	return State::Get;
}

int KioskScreen::FocusedEntry() const {
	if (!this->storeOk) return -1;
	if (this->appOpen) return this->appIdx;

	switch (this->tab) {
		case TAB_HOME:
			if (this->shelves.empty()) return -1;
			return this->shelves[this->shelfSel].items[this->shelves[this->shelfSel].sel];
		case TAB_SEARCH:
			if (this->search.sel < 0 || this->search.items.empty()) return -1;
			return this->search.items[this->search.sel];
		case TAB_UPDATES:
			return this->updates.items.empty() ? -1 : this->updates.items[this->updates.sel];
		case TAB_LIBRARY:
			return this->library.items.empty() ? -1 : this->library.items[this->library.sel];
		default:
			return -1;
	}
}

void KioskScreen::ShowToast(const std::string &msg) {
	this->toast = msg;
	this->toastTimer = 150;
}

void KioskScreen::QueueScript(int idx, int script) {
	if (idx < 0 || script < 0 || script >= (int)E(idx)->GetScripts().size()) return;
	if (this->InQueue(idx, script)) {
		this->ShowToast("Already in your downloads");
		return;
	}
	QueueSystem::AddToQueue(E(idx), script);
	this->ShowToast("Added to downloads: " + E(idx)->GetTitle());
}

void KioskScreen::UpdateEntry(int idx, bool quiet) {
	if (idx < 0 || this->InQueue(idx)) return;
	auto &scripts = E(idx)->GetScripts();
	bool any = false;
	for (size_t i = 0; i < scripts.size(); i++) {
		if (scripts[i].IsInstalled()) {
			QueueSystem::AddToQueue(E(idx), i);
			any = true;
		}
	}
	if (!any && !scripts.empty()) QueueSystem::AddToQueue(E(idx), 0);
	if (!quiet) this->ShowToast("Updating " + E(idx)->GetTitle());
}

void KioskScreen::UpdateAll() {
	int n = 0;
	for (int i : this->updates.items) {
		if (this->InQueue(i)) continue;
		this->UpdateEntry(i, true);
		n++;
	}
	this->ShowToast(n > 0 ? "Updating " + std::to_string(n) + (n == 1 ? " app" : " apps") : "Updates are already queued");
}

void KioskScreen::OpenApp(int idx) {
	if (idx < 0) return;
	this->appIdx = idx;
	this->appOpen = true;
	this->appSel = 0;
	this->appScroll = this->appScrollTarget = 0;
	this->topMode = TopMode::Details;
	this->notesScroll = 0;
	this->shotIdx = 0;
	this->FreeShot();
}

void KioskScreen::CloseApp() {
	this->appOpen = false;
	this->topMode = TopMode::Details;
	this->FreeShot();
}

void KioskScreen::FreeShot() {
	if (this->shot.tex) {
		C3D_TexDelete(this->shot.tex);
		delete this->shot.tex;
	}
	if (this->shot.subtex) delete const_cast<Tex3DS_SubTexture *>(this->shot.subtex);
	this->shot = { nullptr, nullptr };
	this->shotFailed = false;
}

void KioskScreen::LoadShot() {
	this->FreeShot();
	const auto &urls = E(this->appIdx)->GetScreenshots();
	if (this->shotIdx < 0 || this->shotIdx >= (int)urls.size()) return;
	if (!checkWifiStatus()) {
		this->shotFailed = true;
		return;
	}
	this->shot = FetchScreenshot(urls[this->shotIdx]);
	this->shotFailed = !this->shot.tex;
}

void KioskScreen::RespondToRequest() {
	switch (QueueSystem::RequestNeeded) {
		case RMDIR_REQUEST:
			QueueSystem::RequestAnswer = Msg::promptMsg(QueueSystem::RequestMsg);
			QueueSystem::Wait = false;
			QueueSystem::Resume();
			break;
		case PROMPT_REQUEST:
			QueueSystem::RequestAnswer = ScriptUtils::prompt(QueueSystem::RequestMsg, QueueSystem::RequestMsgName);
			QueueSystem::Wait = false;
			QueueSystem::Resume();
			break;
	}
}

void KioskScreen::KeepVisible(List &list, float viewH, float stride) {
	if (list.sel < 0) {
		list.scrollTarget = 0;
		return;
	}
	const float top = list.sel * stride, bottom = top + stride;
	if (top < list.scrollTarget) list.scrollTarget = top;
	if (bottom > list.scrollTarget + viewH) list.scrollTarget = bottom - viewH;
	const float maxScroll = std::max(0.0f, list.items.size() * stride - viewH + 4);
	list.scrollTarget = std::clamp(list.scrollTarget, 0.0f, maxScroll);
}

/*
	Logic.
*/
void KioskScreen::Logic(u32 hDown, u32 hHeld, touchPosition touch) {
	this->frame++;
	const u32 rep = hRepeat;

	/* Messages coming from the download engine. */
	if (QueueSystem::Popup) {
		this->ShowToast(QueueSystem::EndMsg);
		QueueSystem::Popup = false;
	}

	const bool pending = QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait;
	if (pending && !this->requestToastShown && this->tab != TAB_DOWNLOADS) {
		this->ShowToast("A download needs your answer — open Downloads");
		this->requestToastShown = true;
	}
	if (!pending) this->requestToastShown = false;

	if (this->storeOk) this->RefreshLists();

	if (hDown & KEY_START && !QueueRuns) {
		exiting = true;
		return;
	}

	if (this->appOpen) {
		this->AppLogic(hDown, rep, touch);
	} else {
		/* Tab switching: L/R or tapping the bar. */
		if (hDown & KEY_L) this->tab = (this->tab + TAB_COUNT - 1) % TAB_COUNT;
		if (hDown & KEY_R) this->tab = (this->tab + 1) % TAB_COUNT;
		if ((hDown & KEY_TOUCH) && touch.py >= TAB_Y) {
			this->tab = std::clamp((int)(touch.px / TAB_W), 0, TAB_COUNT - 1);
		} else if (this->storeOk) {
			switch (this->tab) {
				case TAB_HOME: this->HomeLogic(hDown, rep, touch); break;
				case TAB_SEARCH: this->SearchLogic(hDown, rep, touch); break;
				case TAB_UPDATES: this->UpdatesLogic(hDown, rep, touch); break;
				case TAB_DOWNLOADS: this->DownloadsLogic(hDown, rep, touch); break;
				case TAB_LIBRARY: this->ListLogic(this->library, ListKind::Library, hDown, rep, touch); break;
			}
		} else if (this->tab == TAB_DOWNLOADS) {
			this->DownloadsLogic(hDown, rep, touch);
		}
	}

	this->Animate();
}

void KioskScreen::HomeLogic(u32 down, u32 rep, touchPosition t) {
	if (this->shelves.empty()) return;
	Shelf &s = this->shelves[this->shelfSel];

	if (rep & KEY_RIGHT && s.sel < (int)s.items.size() - 1) s.sel++;
	if (rep & KEY_LEFT && s.sel > 0) s.sel--;
	if (rep & KEY_DOWN && this->shelfSel < (int)this->shelves.size() - 1) this->shelfSel++;
	if (rep & KEY_UP && this->shelfSel > 0) this->shelfSel--;
	if (down & KEY_A) this->OpenApp(s.items[s.sel]);
	if (down & KEY_Y) this->tab = TAB_SEARCH;

	if (down & KEY_TOUCH) {
		for (int si = 0; si < (int)this->shelves.size(); si++) {
			Shelf &sh = this->shelves[si];
			const float top = 6 + si * SHELF_H - this->homeScroll + 20;
			if (t.py < top || t.py >= top + TILE) continue;
			for (int i = 0; i < (int)sh.items.size(); i++) {
				const float x = SHELF_X + i * TILE_STRIDE - sh.scroll;
				if (!hit(t, x, top, TILE, TILE)) continue;
				if (si == this->shelfSel && i == sh.sel) this->OpenApp(sh.items[i]);
				else {
					this->shelfSel = si;
					sh.sel = i;
				}
				return;
			}
		}
	}
}

void KioskScreen::ListLogic(List &list, ListKind kind, u32 down, u32 rep, touchPosition t) {
	const float y0 = kind == ListKind::Search ? SEARCH_Y0 : (kind == ListKind::Updates ? UPDATES_Y0 : 8.0f);
	const int n = list.items.size();

	if (n > 0) {
		if (rep & KEY_DOWN && list.sel < n - 1) list.sel++;
		if (rep & KEY_UP && list.sel > (kind == ListKind::Search ? -1 : 0)) list.sel--;
		if (rep & KEY_RIGHT) list.sel = std::min(n - 1, list.sel + 3);
		if (rep & KEY_LEFT) list.sel = std::max(kind == ListKind::Search ? -1 : 0, list.sel - 3);
	}

	if (down & KEY_A && list.sel >= 0 && list.sel < n) {
		if (kind == ListKind::Updates) this->UpdateEntry(list.items[list.sel]);
		else this->OpenApp(list.items[list.sel]);
	}
	if (kind == ListKind::Updates && down & KEY_Y && list.sel >= 0 && list.sel < n) this->OpenApp(list.items[list.sel]);

	if (down & KEY_TOUCH && t.py >= y0 && t.py < TAB_Y) {
		const int i = (int)((t.py - y0 + list.scroll) / ROW_STRIDE);
		if (i >= 0 && i < n) {
			const bool onPill = t.px >= 226;
			if (kind == ListKind::Updates && onPill) this->UpdateEntry(list.items[i]);
			else if (list.sel == i || onPill) this->OpenApp(list.items[i]);
			list.sel = i;
		}
	}

	KeepVisible(list, TAB_Y - y0, ROW_STRIDE);
}

void KioskScreen::SearchLogic(u32 down, u32 rep, touchPosition t) {
	bool openKeyboard = (down & KEY_Y) || ((down & KEY_A) && this->search.sel < 0) || ((down & KEY_TOUCH) && hit(t, 8, 6, 304, 34));

	if (down & KEY_B && !this->query.empty()) {
		this->query.clear();
		this->RunSearch();
		return;
	}

	if (openKeyboard) {
		std::string q = this->query;
		if (Input::getTextKeyboard(q, 30, "Search Universal-DB", q, StoreUtils::allEntries)) {
			this->query = q;
			this->RunSearch();
		}
		return;
	}

	this->ListLogic(this->search, ListKind::Search, down, rep, t);
}

void KioskScreen::UpdatesLogic(u32 down, u32 rep, touchPosition t) {
	if (!this->updates.items.empty()) {
		if (down & KEY_X) this->UpdateAll();
		if (down & KEY_TOUCH && hit(t, 212, 6, 100, 26)) {
			this->UpdateAll();
			return;
		}
	}
	this->ListLogic(this->updates, ListKind::Updates, down, rep, t);
}

void KioskScreen::DownloadsLogic(u32 down, u32 rep, touchPosition t) {
	const bool pending = QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait;
	const int n = queueEntries.size();
	const float listY = DOWNLOAD_Y0 + (pending ? 62 : 0);

	if (pending && ((down & KEY_A) || ((down & KEY_TOUCH) && hit(t, 8, DOWNLOAD_Y0, 304, 54)))) {
		this->RespondToRequest();
		return;
	}

	if (n == 0) {
		this->downloadSel = 0;
		return;
	}

	if (rep & KEY_DOWN && this->downloadSel < n - 1) this->downloadSel++;
	if (rep & KEY_UP && this->downloadSel > 0) this->downloadSel--;
	this->downloadSel = std::clamp(this->downloadSel, 0, n - 1);

	/* X cancels the running download, Y removes a waiting one. */
	if (down & KEY_X && !pending && QueueRuns) QueueSystem::CancelCallback = true;
	if (down & KEY_Y && this->downloadSel > 0 && this->downloadSel < n) {
		RemoveQueued(this->downloadSel);
		this->ShowToast("Removed from downloads");
	}

	if (down & KEY_TOUCH) {
		for (int i = 0; i < n; i++) {
			const float y = i == 0 ? listY : listY + (ROW_H + 14 + 14) + (i - 1) * ROW_STRIDE;
			if (!hit(t, 8, y, 304, i == 0 ? ROW_H + 14 : ROW_H)) continue;
			if (hit(t, 240, y, 72, 30)) {
				if (i == 0 && !pending && QueueRuns) QueueSystem::CancelCallback = true;
				else if (i > 0) RemoveQueued(i);
			}
			this->downloadSel = i;
			break;
		}
	}
}

void KioskScreen::AppLogic(u32 down, u32 rep, touchPosition t) {
	auto &e = E(this->appIdx);
	const int scripts = e->GetScripts().size();
	const bool hasShots = !e->GetScreenshots().empty();
	const bool hasNotes = !e->GetReleaseNotes().empty();

	if (down & KEY_B) {
		if (this->topMode != TopMode::Details) this->topMode = TopMode::Details;
		else this->CloseApp();
		return;
	}

	if (rep & KEY_DOWN && this->appSel < scripts - 1) this->appSel++;
	if (rep & KEY_UP && this->appSel > 0) this->appSel--;
	if (down & KEY_A) this->QueueScript(this->appIdx, this->appSel);

	auto toggleNotes = [&]() {
		if (!hasNotes) { this->ShowToast("No release notes for this one"); return; }
		this->topMode = this->topMode == TopMode::WhatsNew ? TopMode::Details : TopMode::WhatsNew;
		this->notesScroll = 0;
	};
	auto toggleShots = [&]() {
		if (!hasShots) { this->ShowToast("No screenshots for this one"); return; }
		if (this->topMode == TopMode::Screenshot) {
			this->topMode = TopMode::Details;
			this->FreeShot();
		} else {
			this->topMode = TopMode::Screenshot;
			this->LoadShot();
		}
	};

	if (down & KEY_X) toggleNotes();
	if (down & KEY_Y) toggleShots();

	if (this->topMode == TopMode::Screenshot) {
		const int count = e->GetScreenshots().size();
		if (down & (KEY_R | KEY_ZR) && count > 1) { this->shotIdx = (this->shotIdx + 1) % count; this->LoadShot(); }
		if (down & (KEY_L | KEY_ZL) && count > 1) { this->shotIdx = (this->shotIdx + count - 1) % count; this->LoadShot(); }
	} else if (this->topMode == TopMode::WhatsNew) {
		const int lines = Wrap(e->GetReleaseNotes(), 0.46f, 352).size();
		if (rep & (KEY_R | KEY_ZR)) this->notesScroll = std::min(this->notesScroll + 3, std::max(0, lines - 9));
		if (rep & (KEY_L | KEY_ZL)) this->notesScroll = std::max(0, this->notesScroll - 3);
	}

	if (down & KEY_TOUCH) {
		if (hit(t, 6, 6, 70, 30)) { this->CloseApp(); return; }
		if (hit(t, 8, 204, 148, 30)) { toggleShots(); return; }
		if (hit(t, 164, 204, 148, 30)) { toggleNotes(); return; }
		if (t.py >= SCRIPT_Y0 && t.py < SCRIPT_Y1) {
			const int i = (int)((t.py - SCRIPT_Y0 + this->appScroll) / SCRIPT_STRIDE);
			if (i >= 0 && i < scripts) {
				if (this->appSel == i || t.px >= 216) this->QueueScript(this->appIdx, i);
				this->appSel = i;
			}
		}
	}

	/* Keep the selected download row in view. */
	const float viewH = SCRIPT_Y1 - SCRIPT_Y0;
	const float top = this->appSel * SCRIPT_STRIDE, bottom = top + SCRIPT_STRIDE;
	if (top < this->appScrollTarget) this->appScrollTarget = top;
	if (bottom > this->appScrollTarget + viewH) this->appScrollTarget = bottom - viewH;
}

void KioskScreen::Animate() {
	/* Home scroll: keep the focused shelf near the top. */
	if (!this->shelves.empty()) {
		const float total = this->shelves.size() * SHELF_H + 6;
		this->homeScrollTarget = std::clamp(this->shelfSel * SHELF_H - 20.0f, 0.0f, std::max(0.0f, total - TAB_Y));
		this->homeScroll = Approach(this->homeScroll, this->homeScrollTarget);

		for (Shelf &s : this->shelves) {
			const float maxScroll = std::max(0.0f, SHELF_X * 2 + s.items.size() * TILE_STRIDE - 8 - 320);
			s.scrollTarget = std::clamp(s.sel * TILE_STRIDE - (320 - TILE) / 2.0f + SHELF_X, 0.0f, maxScroll);
			s.scroll = Approach(s.scroll, s.scrollTarget);
		}
	}

	for (List *l : { &this->search, &this->updates, &this->library }) l->scroll = Approach(l->scroll, l->scrollTarget);
	this->appScroll = Approach(this->appScroll, this->appScrollTarget);

	this->tabPillX = Approach(this->tabPillX, this->tab * TAB_W, 0.28f);
	this->appAnim = Approach(this->appAnim, this->appOpen ? 1.0f : 0.0f, 0.2f);

	/* Focus change: pop the tile and fade the hero in with the new accent colour. */
	const int focus = this->FocusedEntry();
	if (focus != this->lastFocus) {
		this->lastFocus = focus;
		this->focusPop = 0.0f;
		this->heroFade = 0.0f;
	}
	this->focusPop = std::min(1.0f, this->focusPop + 0.09f);
	this->heroFade = std::min(1.0f, this->heroFade + 0.1f);

	u32 target = ACCENT;
	if (focus >= 0) target = AccentFor(E(focus)->GetAccentColor(), E(focus)->GetTitle());
	else if (this->tab == TAB_DOWNLOADS && !queueEntries.empty()) {
		const auto &q = queueEntries[0].storeEntry;
		target = AccentFor(q->GetAccentColor(), q->GetTitle());
	}
	this->heroColor = Lerp(this->heroColor, target, 0.18f);

	if (this->fadeIn > 0) this->fadeIn = std::max(0.0f, this->fadeIn - 12.0f);

	if (this->toastTimer > 0) {
		this->toastTimer--;
		this->toastY = Approach(this->toastY, 26.0f, 0.25f);
	} else {
		this->toastY = Approach(this->toastY, -30.0f, 0.25f);
	}
}

/*
	Drawing — top screen.
*/
void KioskScreen::Draw(void) const {
	Gui::ScreenDraw(Top);
	if (this->appOpen && this->topMode == TopMode::Screenshot) this->DrawScreenshotTop(this->appIdx);
	else if (this->appOpen && this->topMode == TopMode::WhatsNew) this->DrawWhatsNewTop(this->appIdx);
	else if (!this->storeOk && this->tab != TAB_DOWNLOADS) this->DrawEmptyTop("Couldn't open Universal-DB", "Connect to Wi-Fi and restart Kiosk.", GLYPH_WARN);
	else if (this->appOpen) this->DrawHero(this->appIdx, true);
	else {
		const int focus = this->FocusedEntry();
		switch (this->tab) {
			case TAB_DOWNLOADS:
				this->DrawDownloadsTop();
				break;
			case TAB_UPDATES:
				if (focus >= 0) this->DrawHero(focus, false);
				else this->DrawEmptyTop("You're all up to date", "Updates for apps you've installed show up here.", GLYPH_CHECK);
				break;
			case TAB_SEARCH:
				if (focus >= 0) this->DrawHero(focus, false);
				else if (this->search.items.empty()) this->DrawEmptyTop("No results", "Try a different search.", GLYPH_SEARCH);
				else this->DrawEmptyTop("Search", std::to_string(this->allSorted.size()) + " apps in Universal-DB", GLYPH_SEARCH);
				break;
			case TAB_LIBRARY:
				if (focus >= 0) this->DrawHero(focus, false);
				else this->DrawEmptyTop("Nothing installed yet", "Apps you download show up here.", GLYPH_LIBRARY);
				break;
			default:
				if (focus >= 0) this->DrawHero(focus, false);
				break;
		}
	}
	this->DrawToast();
	if (this->fadeIn > 0) Rect(0, 0, 400, 240, Alpha(BG, (u8)this->fadeIn));
	if (fadeAlpha > 0) Rect(0, 0, 400, 240, RGBA(0, 0, 0, fadeAlpha));

	/* Bottom screen. */
	Gui::ScreenDraw(Bottom);
	Rect(0, 0, 320, 240, BG);
	if (this->appAnim < 0.99f) {
		if (!this->storeOk && this->tab != TAB_DOWNLOADS) {
			this->DrawEmptyBottom("Store unavailable", "Press START to exit.", 70);
		} else {
			switch (this->tab) {
				case TAB_HOME: this->DrawHome(); break;
				case TAB_SEARCH: this->DrawSearch(); break;
				case TAB_UPDATES: this->DrawUpdates(); break;
				case TAB_DOWNLOADS: this->DrawDownloads(); break;
				case TAB_LIBRARY:
					if (this->library.items.empty()) this->DrawEmptyBottom("Your library is empty", "Find something on the Home tab.", 60);
					else this->DrawList(this->library, ListKind::Library, 8, TAB_Y);
					break;
			}
		}
		this->DrawTabBar();
	}
	if (this->appAnim > 0.01f && this->appIdx >= 0) this->DrawAppPage((1.0f - EaseOutCubic(this->appAnim)) * 320.0f);

	if (this->fadeIn > 0) Rect(0, 0, 320, 240, Alpha(BG, (u8)this->fadeIn));
	if (fadeAlpha > 0) Rect(0, 0, 320, 240, RGBA(0, 0, 0, fadeAlpha));
}

void KioskScreen::DrawStatusBar(u32 color) const {
	Text(10, 3, 0.5f, color, "Kiosk");

	time_t now = time(nullptr);
	struct tm *tm = gmtime(&now);
	char clock[8];
	snprintf(clock, sizeof(clock), "%02d:%02d", tm->tm_hour, tm->tm_min);
	TextRight(358, 3, 0.5f, color, clock);

	/* Battery. */
	u8 level = 0, charging = 0;
	PTMU_GetBatteryLevel(&level);
	PTMU_GetBatteryChargeState(&charging);
	const float bx = 366, by = 6, bw = 22, bh = 11;
	Rect(bx, by, bw, bh, color);
	Rect(bx + 1.5f, by + 1.5f, bw - 3, bh - 3, Darken(this->heroColor, 0.3f));
	Rect(bx + bw, by + 3, 2, bh - 6, color);
	const float fill = std::clamp(level / 5.0f, 0.0f, 1.0f) * (bw - 5);
	Rect(bx + 2.5f, by + 2.5f, fill, bh - 5, (level <= 1 && !charging) ? BADGE : (charging ? RGBA(120, 230, 150) : color));
}

void KioskScreen::DrawHero(int idx, bool detailed) const {
	const auto &e = E(idx);
	const u32 bg = this->heroColor;
	Gradient(0, 0, 400, 240, Lighten(bg, 0.1f), Darken(bg, 0.38f));
	Circle(372, 18, 84, Alpha(WHITE, 16));
	Circle(300, 132, 40, Alpha(WHITE, 10));
	this->DrawStatusBar(WHITE);

	const float t = EaseOutCubic(this->heroFade);
	const float off = (1.0f - t) * 10.0f;
	const u8 textA = (u8)(255 * t);

	/* Icon tile. */
	Card(20, 32 + off, 84, 84, 18, WHITE, Darken(bg, 0.45f));
	Image(e->GetIcon(), 62, 74 + off, 1.5f);

	/* Title block. */
	Text(118, 34 + off, 0.72f, Alpha(WHITE, textA), e->GetTitle(), 266);
	Text(118, 60 + off, 0.48f, Alpha(Lighten(bg, 0.78f), textA), "by " + e->GetAuthor(), 266);

	/* Chips. */
	float cx = 118;
	auto chip = [&](const std::string &label, u32 fill, u32 ink) {
		const float w = TextWidth(0.4f, label) + 16;
		if (cx + w > 392) return;
		Pill(cx, 86 + off, w, 20, fill);
		Text(cx + 8, 89 + off, 0.4f, ink, label);
		cx += w + 6;
	};

	const State s = this->StateOf(idx);
	if (s == State::Update) chip("Update available", WHITE, Darken(bg, 0.15f));
	else if (s == State::Installed) chip("Installed", WHITE, GOOD);
	else if (s == State::Queued) chip("In downloads", WHITE, Darken(bg, 0.15f));
	if (e->GetStars() > 0) chip("★ " + std::to_string(e->GetStars()), Darken(bg, 0.22f), WHITE);
	for (const std::string &cat : e->GetCategories()) chip(PrettyCategory(cat), Darken(bg, 0.22f), WHITE);

	/* Description card. */
	Card(14, 128, 372, 104, 14, WHITE, Darken(bg, 0.45f));
	const std::string desc = e->GetDescription().empty() ? "No description provided." : e->GetDescription();
	if (detailed) {
		Paragraph(28, 136, 0.46f, TEXT, desc, 344, 4, 15);
		Rect(28, 199, 344, 1, LINE);
		std::string meta;
		if (!e->GetVersion().empty()) meta += "Version " + e->GetVersion();
		if (!e->GetLastUpdated().empty()) meta += (meta.empty() ? "" : "   ·   ") + std::string("Updated ") + DateOnly(e->GetLastUpdated());
		if (!e->GetLicense().empty()) meta += (meta.empty() ? "" : "   ·   ") + e->GetLicense();
		Text(28, 206, 0.42f, SUB, meta, 344);
	} else {
		Paragraph(28, 138, 0.46f, TEXT, desc, 344, 6, 15);
	}
}

void KioskScreen::DrawEmptyTop(const std::string &title, const std::string &subtitle, int glyph) const {
	Gradient(0, 0, 400, 240, Lighten(this->heroColor, 0.1f), Darken(this->heroColor, 0.38f));
	Circle(372, 18, 84, Alpha(WHITE, 16));
	this->DrawStatusBar(WHITE);

	const u32 ink = Darken(this->heroColor, 0.1f);
	Circle(200, 96, 36, WHITE);
	switch (glyph) {
		case GLYPH_CHECK: Check(200, 96, 34, ink); break;
		case GLYPH_DOWNLOAD: Sprite(sprites_download_idx, 200, 96, ink, 1.4f); break;
		case GLYPH_SEARCH: Sprite(sprites_search_idx, 200, 96, ink, 1.4f); break;
		case GLYPH_LIBRARY: Sprite(sprites_installed_app_idx, 200, 96, ink, 1.4f); break;
		default: TextCentered(200, 78, 1.0f, ink, "!"); break;
	}
	TextCentered(200, 148, 0.72f, WHITE, title, 380);
	TextCentered(200, 176, 0.48f, Lighten(this->heroColor, 0.78f), subtitle, 380);
}

void KioskScreen::DrawWhatsNewTop(int idx) const {
	const auto &e = E(idx);
	Gradient(0, 0, 400, 240, Lighten(this->heroColor, 0.1f), Darken(this->heroColor, 0.38f));
	this->DrawStatusBar(WHITE);

	Text(20, 28, 0.66f, WHITE, "What's new", 220);
	std::string sub = e->GetTitle();
	if (!e->GetVersion().empty()) sub += "  ·  " + e->GetVersion();
	TextRight(380, 33, 0.44f, Lighten(this->heroColor, 0.78f), sub, 170);

	Card(14, 56, 372, 176, 14, WHITE, Darken(this->heroColor, 0.45f));
	Paragraph(24, 64, 0.46f, TEXT, e->GetReleaseNotes(), 352, 10, 16, this->notesScroll);

	/* Scroll indicator. */
	const int lines = Wrap(e->GetReleaseNotes(), 0.46f, 352).size();
	if (lines > 10) {
		const float trackH = 160, thumbH = std::max(20.0f, trackH * 10.0f / lines);
		const float y = 64 + (trackH - thumbH) * (this->notesScroll / (float)std::max(1, lines - 9));
		Pill(379, 64, 3, trackH, LINE);
		Pill(379, std::min(y, 64 + trackH - thumbH), 3, thumbH, this->heroColor);
	}
}

void KioskScreen::DrawScreenshotTop(int idx) const {
	const auto &e = E(idx);
	Rect(0, 0, 400, 240, RGBA(14, 15, 18));

	if (this->shot.tex && this->shot.subtex) {
		const float w = this->shot.subtex->width, h = this->shot.subtex->height;
		const float scale = std::min(400.0f / w, 240.0f / h);
		Image(this->shot, 200, 120, scale);
	} else if (this->shotFailed) {
		TextCentered(200, 104, 0.55f, RGBA(200, 200, 205), "Couldn't load this screenshot");
	}

	/* Caption strip. */
	Rect(0, 214, 400, 26, RGBA(0, 0, 0, 170));
	const auto &names = e->GetScreenshotNames();
	const std::string name = this->shotIdx < (int)names.size() ? names[this->shotIdx] : e->GetTitle();
	Text(10, 219, 0.45f, WHITE, name, 250);
	const int count = e->GetScreenshots().size();
	TextRight(390, 219, 0.45f, RGBA(200, 200, 205), (count > 1 ? "L / R   " : "") + std::to_string(this->shotIdx + 1) + " of " + std::to_string(count));
}

void KioskScreen::DrawDownloadsTop() const {
	if (queueEntries.empty()) {
		this->DrawEmptyTop("No downloads", "Things you download or update show up here.", GLYPH_DOWNLOAD);
		return;
	}

	const QueueEntry &q = queueEntries[0];
	const auto &e = q.storeEntry;
	const u32 bg = this->heroColor;
	Gradient(0, 0, 400, 240, Lighten(bg, 0.1f), Darken(bg, 0.38f));
	Circle(372, 18, 84, Alpha(WHITE, 16));
	this->DrawStatusBar(WHITE);

	const bool pending = QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait;

	/* Work out what the engine is doing right now. */
	float p = -1.0f;
	std::string status = "Preparing", detail;
	char buf[96];
	switch (q.status) {
		case QueueStatus::Downloading: {
			status = "Downloading";
			const curl_off_t total = std::max<curl_off_t>(1, std::max(downloadTotal, downloadNow));
			if (downloadTotal > 1) p = (float)downloadNow / total;
			curl_off_t speed = 0;
			if (CurlHandle) curl_easy_getinfo(CurlHandle, CURLINFO_SPEED_DOWNLOAD_T, &speed);
			snprintf(buf, sizeof(buf), "%s of %s  ·  %lld KB/s", StringUtils::formatBytes(downloadNow).c_str(), StringUtils::formatBytes(total).c_str(), (long long)(speed / 1024));
			detail = buf;
			break;
		}
		case QueueStatus::Extracting:
			status = "Unpacking";
			if (extractSize > 0) p = (float)writeOffset / extractSize;
			snprintf(buf, sizeof(buf), "%s of %s", StringUtils::formatBytes(writeOffset).c_str(), StringUtils::formatBytes(extractSize).c_str());
			detail = buf;
			break;
		case QueueStatus::Installing:
			status = "Installing";
			if (installSize > 0) p = (float)installOffset / installSize;
			snprintf(buf, sizeof(buf), "%s of %s", StringUtils::formatBytes(installOffset).c_str(), StringUtils::formatBytes(installSize).c_str());
			detail = buf;
			break;
		case QueueStatus::Copying:
			status = "Copying files";
			if (copySize > 0) p = (float)copyOffset / copySize;
			break;
		case QueueStatus::Moving: status = "Moving files"; break;
		case QueueStatus::Deleting: status = "Cleaning up"; break;
		case QueueStatus::Request: status = "Waiting for you"; break;
		case QueueStatus::Done: status = "Done"; p = 1.0f; break;
		case QueueStatus::Failed: status = "Failed"; break;
		default: break;
	}
	if (pending) {
		status = "Waiting for you";
		detail = "Answer on the bottom screen";
		p = -1.0f;
	}

	/* Progress ring. */
	const float cx = 104, cy = 130, r = 62;
	Ring(cx, cy, r, 11, 0.0f, 1.0f, Darken(bg, 0.28f));
	if (p >= 0.0f) {
		Ring(cx, cy, r, 11, 0.0f, std::clamp(p, 0.001f, 1.0f), WHITE);
		TextCentered(cx, cy - 18, 0.95f, WHITE, std::to_string((int)(std::clamp(p, 0.0f, 1.0f) * 100)) + "%");
	} else if (!pending) {
		const float spin = fmodf(this->frame / 70.0f, 1.0f);
		Ring(cx, cy, r, 11, spin, spin + 0.22f, WHITE);
	}
	if (p < 0.0f) Image(e->GetIcon(), cx, cy, 1.0f);

	/* Details. */
	Card(196, 40, 56, 56, 12, WHITE, Darken(bg, 0.45f));
	Image(e->GetIcon(), 224, 68, 1.0f);
	Text(196, 106, 0.62f, WHITE, e->GetTitle(), 194);
	Text(196, 130, 0.44f, Lighten(bg, 0.78f), e->GetScript(q.scriptIndex).GetName(), 194);
	Text(196, 158, 0.52f, WHITE, status, 194);
	Text(196, 178, 0.42f, Lighten(bg, 0.78f), detail, 194);

	const int steps = e->GetScript(q.scriptIndex).GetActions().size();
	snprintf(buf, sizeof(buf), "Step %d of %d", std::max(1, std::min(q.currentStep, steps)), std::max(1, steps));
	Text(196, 200, 0.42f, Lighten(bg, 0.78f), buf);
	if (queueEntries.size() > 1) TextRight(390, 200, 0.42f, Lighten(bg, 0.78f), std::to_string(queueEntries.size() - 1) + " more waiting");
}

void KioskScreen::DrawToast() const {
	if (this->toastY <= -29.0f || this->toast.empty()) return;
	const std::string msg = Ellipsize(this->toast, 0.5f, 340);
	const float w = TextWidth(0.5f, msg) + 32;
	const float x = 200 - w / 2.0f, y = this->toastY;
	Pill(x, y + 2, w, 28, Darken(this->heroColor, 0.5f));
	Pill(x, y, w, 28, WHITE);
	Text(x + 16, y + 6, 0.5f, TEXT, msg);
}

/*
	Drawing — bottom screen.
*/
void KioskScreen::DrawTabBar() const {
	Rect(0, TAB_Y, 320, 240 - TAB_Y, WHITE);
	Rect(0, TAB_Y, 320, 1, LINE);

	/* Sliding highlight. */
	Pill(this->tabPillX + 6, TAB_Y + 4, TAB_W - 12, 30, ACCENT_SOFT);

	static const char *labels[TAB_COUNT] = { "Home", "Search", "Updates", "Downloads", "Library" };
	for (int i = 0; i < TAB_COUNT; i++) {
		const float cx = i * TAB_W + TAB_W / 2.0f;
		const u32 c = i == this->tab ? ACCENT : SUB;
		switch (i) {
			case TAB_HOME: House(cx, TAB_Y + 15, 13, c); break;
			case TAB_SEARCH: Sprite(sprites_search_idx, cx, TAB_Y + 13, c, 0.75f); break;
			case TAB_UPDATES: Sprite(sprites_update_idx, cx, TAB_Y + 13, c, 0.75f); break;
			case TAB_DOWNLOADS: Sprite(sprites_download_idx, cx, TAB_Y + 13, c, 0.75f); break;
			case TAB_LIBRARY: Sprite(sprites_installed_app_idx, cx, TAB_Y + 13, c, 0.75f); break;
		}
		TextCentered(cx, TAB_Y + 22, 0.34f, c, labels[i]);
	}

	if (!this->updates.items.empty()) Badge(TAB_UPDATES * TAB_W + 44, TAB_Y + 7, std::to_string(this->updates.items.size()));
	const bool pending = QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait;
	if (pending) Badge(TAB_DOWNLOADS * TAB_W + 44, TAB_Y + 7, "!");
	else if (!queueEntries.empty()) Badge(TAB_DOWNLOADS * TAB_W + 44, TAB_Y + 7, std::to_string(queueEntries.size()));
}

void KioskScreen::DrawHome() const {
	const float pop = EaseOutBack(this->focusPop);

	for (int si = 0; si < (int)this->shelves.size(); si++) {
		const Shelf &s = this->shelves[si];
		const float top = 6 + si * SHELF_H - this->homeScroll;
		if (top > TAB_Y || top + SHELF_H < 0) continue;

		const bool activeShelf = si == this->shelfSel;
		Text(SHELF_X, top, 0.52f, activeShelf ? TEXT : SUB, s.title);
		TextRight(308, top + 2, 0.4f, SUB, std::to_string(s.items.size()));

		int focusedIdx = -1;
		for (int i = 0; i < (int)s.items.size(); i++) {
			const float x = SHELF_X + i * TILE_STRIDE - s.scroll;
			if (x > 320 || x + TILE < 0) continue;
			if (activeShelf && i == s.sel) { focusedIdx = i; continue; } // Drawn last so it sits on top.

			Card(x, top + 20, TILE, TILE, 12);
			Image(E(s.items[i])->GetIcon(), x + TILE / 2, top + 20 + TILE / 2, 1.0f);
			if (E(s.items[i])->GetUpdateAvl()) Circle(x + TILE - 6, top + 26, 4, ACCENT);
		}

		if (focusedIdx >= 0) {
			const int ei = s.items[focusedIdx];
			const float scale = 1.0f + 0.1f * pop;
			const float size = TILE * scale, grow = (size - TILE) / 2.0f;
			const float x = SHELF_X + focusedIdx * TILE_STRIDE - s.scroll - grow, y = top + 20 - grow;
			RoundRect(x - 3, y - 3, size + 6, size + 6, 15, this->heroColor);
			RoundRect(x, y, size, size, 12, WHITE);
			Image(E(ei)->GetIcon(), x + size / 2, y + size / 2, scale);
			if (E(ei)->GetUpdateAvl()) Circle(x + size - 6, y + 6, 4.5f, ACCENT);
			else if (this->IsInstalled(ei)) {
				Circle(x + size - 4, y + size - 4, 7, GOOD);
				Check(x + size - 4, y + size - 4, 8, WHITE);
			}
		}
	}
}

void KioskScreen::DrawPillButton(float x, float y, float w, float h, State s, bool focused) const {
	const char *label = "Get";
	u32 fill = ACCENT_SOFT, ink = ACCENT;
	switch (s) {
		case State::Get: label = "Get"; break;
		case State::Update: label = "Update"; fill = ACCENT; ink = WHITE; break;
		case State::Installed: label = "Installed"; fill = MUTED; ink = SUB; break;
		case State::Queued: label = "Queued"; fill = MUTED; ink = SUB; break;
	}
	if (focused && s == State::Get) { fill = ACCENT; ink = WHITE; }
	Pill(x, y, w, h, fill);
	TextCentered(x + w / 2, y + (h - 14) / 2.0f, 0.44f, ink, label);
}

void KioskScreen::DrawList(const List &list, ListKind kind, float y0, float y1) const {
	for (int i = 0; i < (int)list.items.size(); i++) {
		const float y = y0 + i * ROW_STRIDE - list.scroll;
		if (y > y1 || y + ROW_H < y0 - ROW_STRIDE) continue;
		const int ei = list.items[i];
		const auto &e = E(ei);
		const bool sel = i == list.sel;

		if (sel) RoundRect(5, y - 3, 310, ROW_H + 6, 14, this->heroColor);
		Card(8, y, 304, ROW_H, 12);
		Image(e->GetIcon(), 34, y + ROW_H / 2, 0.75f);

		std::string sub;
		switch (kind) {
			case ListKind::Search: sub = e->GetAuthor(); break;
			case ListKind::Updates: sub = "New: " + (e->GetVersion().empty() ? DateOnly(e->GetLastUpdated()) : e->GetVersion()); break;
			case ListKind::Library: sub = e->GetUpdateAvl() ? "Update available" : (e->GetVersion().empty() ? "Installed" : "Installed · " + e->GetVersion()); break;
		}

		Text(60, y + 7, 0.52f, TEXT, e->GetTitle(), 160);
		Text(60, y + 26, 0.4f, (kind == ListKind::Library && e->GetUpdateAvl()) ? ACCENT : SUB, sub, 160);

		State st = this->StateOf(ei);
		if (kind == ListKind::Updates && st != State::Queued) st = State::Update;
		this->DrawPillButton(230, y + 11, 74, 26, st, sel);
	}

	/* Overflow is hidden behind solid strips so lists don't run into the headers. */
	if (y0 > 0) Rect(0, 0, 320, y0 - 4, BG);
}

void KioskScreen::DrawSearch() const {
	this->DrawList(this->search, ListKind::Search, SEARCH_Y0, TAB_Y);

	const bool fieldSel = this->search.sel < 0;
	Rect(0, 0, 320, SEARCH_Y0 - 4, BG);
	if (fieldSel) Pill(5, 3, 310, 40, this->heroColor);
	Pill(8, 6, 304, 34, WHITE);
	Sprite(sprites_search_idx, 28, 23, SUB, 0.7f);
	if (this->query.empty()) Text(44, 14, 0.5f, SUB, "Search titles, authors…");
	else {
		Text(44, 14, 0.5f, TEXT, this->query, 190);
		TextRight(302, 15, 0.42f, SUB, std::to_string(this->search.items.size()) + " found");
	}

	if (this->search.items.empty()) this->DrawEmptyBottom("No matches", "B clears the search.", 80);
}

void KioskScreen::DrawUpdates() const {
	if (this->updates.items.empty()) {
		this->DrawEmptyBottom("No updates right now", "Kiosk checks every time it opens.", 60);
		return;
	}

	this->DrawList(this->updates, ListKind::Updates, UPDATES_Y0, TAB_Y);
	Rect(0, 0, 320, UPDATES_Y0 - 4, BG);
	const int n = this->updates.items.size();
	Text(12, 9, 0.56f, TEXT, std::to_string(n) + (n == 1 ? " update" : " updates"));
	Pill(212, 6, 100, 26, ACCENT);
	TextCentered(262, 11, 0.44f, WHITE, "Update all");
}

void KioskScreen::DrawDownloads() const {
	const bool pending = QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait;
	float y = DOWNLOAD_Y0;

	if (pending) {
		Card(8, y, 304, 54, 12, ACCENT, Darken(ACCENT, 0.3f));
		Text(20, y + 7, 0.5f, WHITE, "A download needs your answer");
		Text(20, y + 28, 0.42f, Lighten(ACCENT, 0.75f), "Press A or tap here to respond");
		y += 62;
	}

	if (queueEntries.empty()) {
		this->DrawEmptyBottom("Nothing downloading", "Updates and downloads appear here.", 60);
		return;
	}

	for (int i = 0; i < (int)queueEntries.size(); i++) {
		const QueueEntry &q = queueEntries[i];
		const float h = i == 0 ? ROW_H + 14 : ROW_H;
		if (y > TAB_Y) break;

		if (i == this->downloadSel) RoundRect(5, y - 3, 310, h + 6, 14, this->heroColor);
		Card(8, y, 304, h, 12);
		Image(q.storeEntry->GetIcon(), 34, y + 24, 0.75f);
		Text(60, y + 7, 0.5f, TEXT, q.storeEntry->GetTitle(), 170);
		Text(60, y + 26, 0.4f, SUB, q.storeEntry->GetScript(q.scriptIndex).GetName(), 170);

		if (i == 0) {
			const int steps = std::max(1, (int)q.storeEntry->GetScript(q.scriptIndex).GetActions().size());
			const float p = std::clamp(q.currentStep / (float)steps, 0.0f, 1.0f);
			Pill(20, y + h - 12, 280, 5, MUTED);
			if (p > 0.0f) Pill(20, y + h - 12, std::max(5.0f, 280 * p), 5, ACCENT);
			if (!pending && QueueRuns) {
				Pill(240, y + 10, 64, 24, MUTED);
				TextCentered(272, y + 14, 0.42f, SUB, "Cancel");
			}
		} else {
			Pill(240, y + 11, 64, 24, MUTED);
			TextCentered(272, y + 15, 0.42f, SUB, "Remove");
		}
		y += h + 4 + (i == 0 ? 10 : 0);
	}
}

void KioskScreen::DrawEmptyBottom(const std::string &title, const std::string &subtitle, float y) const {
	Circle(160, y + 26, 26, ACCENT_SOFT);
	Check(160, y + 26, 24, ACCENT);
	TextCentered(160, y + 64, 0.58f, TEXT, title, 300);
	TextCentered(160, y + 86, 0.44f, SUB, subtitle, 300);
}

void KioskScreen::DrawAppPage(float ox) const {
	const auto &e = E(this->appIdx);
	Rect(ox, 0, 320, 240, BG);
	if (ox > 0) Rect(ox - 3, 0, 3, 240, RGBA(0, 0, 0, 30)); // Edge shadow while sliding in.

	/* Header. */
	Pill(ox + 8, 8, 66, 26, WHITE);
	Chevron(ox + 22, 21, 10, true, this->heroColor);
	Text(ox + 32, 13, 0.46f, TEXT, "Back");
	Text(ox + 84, 12, 0.52f, TEXT, e->GetTitle(), 226);
	Text(ox + 12, 44, 0.42f, SUB, e->GetScripts().size() == 1 ? "Download" : "Choose a download");

	/* Download options. */
	const auto &scripts = e->GetScripts();
	for (int i = 0; i < (int)scripts.size(); i++) {
		const float y = SCRIPT_Y0 + i * SCRIPT_STRIDE - this->appScroll;
		if (y + SCRIPT_ROW_H < SCRIPT_Y0 - 8 || y > SCRIPT_Y1) continue;
		const Script &sc = scripts[i];
		const bool sel = i == this->appSel;

		if (sel) RoundRect(ox + 5, y - 3, 310, SCRIPT_ROW_H + 6, 14, this->heroColor);
		Card(ox + 8, y, 304, SCRIPT_ROW_H, 12);
		Text(ox + 20, y + 5, 0.48f, TEXT, sc.GetName(), 186);

		std::string sub = sc.IsInstalled() ? "Installed" : (sc.IsPrerelease() ? "Pre-release" : "");
		if (!sc.GetSize().empty()) sub += (sub.empty() ? "" : " · ") + sc.GetSize();
		Text(ox + 20, y + 24, 0.4f, sc.IsInstalled() ? GOOD : SUB, sub, 186);

		State st = State::Get;
		if (this->InQueue(this->appIdx, i)) st = State::Queued;
		else if (sc.IsInstalled()) st = e->GetUpdateAvl() ? State::Update : State::Installed;

		if (st == State::Installed) {
			/* Installed scripts can still be re-run. */
			Pill(ox + 216, y + 9, 88, 26, sel ? ACCENT : MUTED);
			TextCentered(ox + 260, y + 14, 0.42f, sel ? WHITE : SUB, "Reinstall");
		} else {
			this->DrawPillButton(ox + 216, y + 9, 88, 26, st, sel);
		}
	}

	/* Hide rows scrolled under the header/footer. */
	Rect(ox, SCRIPT_Y1 + 2, 320, 240 - SCRIPT_Y1 - 2, BG);
	if (this->appScroll > 1) {
		Rect(ox, 0, 320, 40, BG);
		Pill(ox + 8, 8, 66, 26, WHITE);
		Chevron(ox + 22, 21, 10, true, this->heroColor);
		Text(ox + 32, 13, 0.46f, TEXT, "Back");
		Text(ox + 84, 12, 0.52f, TEXT, e->GetTitle(), 226);
	}

	/* Footer buttons. */
	const bool hasShots = !e->GetScreenshots().empty(), hasNotes = !e->GetReleaseNotes().empty();
	auto footer = [&](float x, const std::string &label, bool enabled, bool active) {
		Pill(ox + x, 204, 148, 30, active ? this->heroColor : (enabled ? WHITE : MUTED));
		TextCentered(ox + x + 74, 211, 0.45f, active ? WHITE : (enabled ? TEXT : SUB), label);
	};
	footer(8, "Y  Screenshots", hasShots, this->topMode == TopMode::Screenshot);
	footer(164, "X  What's new", hasNotes, this->topMode == TopMode::WhatsNew);
}
