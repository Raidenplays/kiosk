#include "kioskScreen.hpp"
#include "download.hpp"
#include "keyboard.hpp"
#include "kui.hpp"
#include "queueSystem.hpp"
#include "scriptUtils.hpp"
#include "sfx.hpp"
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
static constexpr float SHELF_H = 94.0f, TILE = 60.0f, TILE_STRIDE = 68.0f, SHELF_X = 12.0f;
static constexpr float ROW_H = 48.0f, ROW_STRIDE = 52.0f;
static constexpr float SCRIPT_ROW_H = 44.0f, SCRIPT_STRIDE = 48.0f, SCRIPT_Y0 = 50.0f, SCRIPT_Y1 = 196.0f;
static constexpr float SEARCH_Y0 = 50.0f, UPDATES_Y0 = 42.0f, DOWNLOAD_Y0 = 8.0f;

/* Download scene geometry (top screen). */
static constexpr float HORIZON = 150.0f, JAR_TOP = 122.0f, JAR_SIZE = 84.0f, RAIL_Y = 62.0f;

enum Glyph { GLYPH_CHECK, GLYPH_DOWNLOAD, GLYPH_SEARCH, GLYPH_LIBRARY, GLYPH_WARN };

static const u32 GUMMY_COLORS[] = {
	RGBA(120, 210, 90), RGBA(255, 150, 60), RGBA(255, 120, 170), RGBA(80, 190, 240), RGBA(170, 120, 240), RGBA(255, 210, 60)
};

static const std::shared_ptr<StoreEntry> &E(int i) { return StoreUtils::allEntries[i]; }
static int EntryCount() { return (int)StoreUtils::allEntries.size(); }

static bool hit(const touchPosition &t, float x, float y, float w, float h) {
	return t.px >= x && t.px < x + w && t.py >= y && t.py < y + h;
}

static float frand(float a, float b) { return a + (b - a) * (rand() / (float)RAND_MAX); }

static void Quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, u32 c) {
	C2D_DrawTriangle(x0, y0, c, x1, y1, c, x2, y2, c, 0.5f);
	C2D_DrawTriangle(x0, y0, c, x2, y2, c, x3, y3, c, 0.5f);
}

static void RotRect(float cx, float cy, float w, float h, float a, u32 c) {
	const float cs = cosf(a), sn = sinf(a), hw = w / 2, hh = h / 2;
	auto px = [&](float x, float y) { return cx + x * cs - y * sn; };
	auto py = [&](float x, float y) { return cy + x * sn + y * cs; };
	Quad(px(-hw, -hh), py(-hw, -hh), px(hw, -hh), py(hw, -hh), px(hw, hh), py(hw, hh), px(-hw, hh), py(-hw, hh), c);
}

static void Sparkle(float cx, float cy, float s, float a, u32 c) {
	RotRect(cx, cy, s * 0.22f, s, a, c);
	RotRect(cx, cy, s * 0.22f, s, a + M_PI / 2, c);
}

/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm). */
static int DaysFromCivil(int y, unsigned m, unsigned d) {
	y -= m <= 2;
	const int era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400);
	const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int)doe - 719468;
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

static bool RequestPending() { return QueueSystem::RequestNeeded != NO_REQUEST && QueueSystem::Wait; }

/*
	Construction / store loading.
*/
KioskScreen::KioskScreen() {
	this->heroColor = ACCENT;

	time_t now = time(nullptr);
	struct tm *tm = gmtime(&now);
	this->today = DaysFromCivil(tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

	this->LoadStore();
	this->BuildShelves();
	this->RunSearch();
	this->RefreshLists();
	this->tabPillX = this->tab * TAB_W;
	this->lastSerial = QueueSystem::FinishedSerial;
	this->lastSig = this->CursorSignature();
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

	Shelf fresh;
	fresh.title = "Recently updated";
	fresh.items = idx;
	std::stable_sort(fresh.items.begin(), fresh.items.end(), byDate);
	if (fresh.items.size() > 20) fresh.items.resize(20);
	this->shelves.push_back(fresh);

	Shelf popular;
	popular.title = "Popular";
	popular.items = idx;
	std::stable_sort(popular.items.begin(), popular.items.end(), byStars);
	if (popular.items.size() > 20) popular.items.resize(20);
	this->shelves.push_back(popular);

	for (const std::string &cat : StoreUtils::store->GetCategories()) {
		Shelf s;
		s.title = PrettyCategory(cat);
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

bool KioskScreen::IsNew(int idx) const {
	const std::string &d = E(idx)->GetLastUpdated();
	int y, m, day;
	if (d.size() < 10 || sscanf(d.c_str(), "%d-%d-%d", &y, &m, &day) != 3) return false;
	const int age = this->today - DaysFromCivil(y, m, day);
	return age >= 0 && age <= 14;
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

int KioskScreen::CursorSignature() const {
	int sig = this->tab * 7919;
	if (this->appOpen) return sig + 1000003 + this->appSel;
	switch (this->tab) {
		case TAB_HOME:
			sig += this->shelfSel * 131;
			if (!this->shelves.empty()) sig += this->shelves[this->shelfSel].sel;
			break;
		case TAB_SEARCH: sig += this->search.sel; break;
		case TAB_UPDATES: sig += this->updates.sel; break;
		case TAB_LIBRARY: sig += this->library.sel; break;
		case TAB_DOWNLOADS: sig += this->downloadSel; break;
	}
	return sig;
}

float KioskScreen::OverallProgress(const QueueEntry &q, float *stepP) const {
	float p = -1.0f;
	switch (q.status) {
		case QueueStatus::Downloading:
			if (downloadTotal > 1) p = (float)downloadNow / (float)std::max(downloadTotal, downloadNow);
			break;
		case QueueStatus::Extracting: if (extractSize > 0) p = (float)writeOffset / extractSize; break;
		case QueueStatus::Installing: if (installSize > 0) p = (float)installOffset / installSize; break;
		case QueueStatus::Copying: if (copySize > 0) p = (float)copyOffset / copySize; break;
		case QueueStatus::Done: p = 1.0f; break;
		default: break;
	}
	if (stepP) *stepP = p;

	const int steps = std::max(1, (int)q.storeEntry->GetScript(q.scriptIndex).GetActions().size());
	const int cur = std::clamp(q.currentStep, 1, steps);
	return std::clamp(((cur - 1) + (p < 0 ? 0.5f : std::clamp(p, 0.0f, 1.0f))) / steps, 0.0f, 1.0f);
}

void KioskScreen::ShowToast(const std::string &msg) {
	this->toast = msg;
	this->toastTimer = 150;
}

void KioskScreen::StartFly(int idx, float sx, float sy) {
	this->fly.icon = E(idx)->GetIcon();
	this->fly.sx = sx;
	this->fly.sy = sy;
	if (this->appOpen) {
		this->fly.ex = 296;
		this->fly.ey = 22;
	} else {
		this->fly.ex = TAB_DOWNLOADS * TAB_W + TAB_W / 2;
		this->fly.ey = TAB_Y + 14;
	}
	this->fly.t = 0.0f;
}

bool KioskScreen::QueueScript(int idx, int script) {
	if (idx < 0 || script < 0 || script >= (int)E(idx)->GetScripts().size()) return false;
	if (this->InQueue(idx, script)) {
		Sfx::Play(Sfx::ERROR);
		this->ShowToast("Already in your downloads");
		return false;
	}
	QueueSystem::AddToQueue(E(idx), script);
	Sfx::Play(Sfx::GET);
	this->pressAnim = 1.0f;
	return true;
}

bool KioskScreen::UpdateEntry(int idx, bool quiet) {
	if (idx < 0 || this->InQueue(idx)) {
		if (!quiet) Sfx::Play(Sfx::ERROR);
		return false;
	}
	auto &scripts = E(idx)->GetScripts();
	bool any = false;
	for (size_t i = 0; i < scripts.size(); i++) {
		if (scripts[i].IsInstalled()) {
			QueueSystem::AddToQueue(E(idx), i);
			any = true;
		}
	}
	if (!any && !scripts.empty()) QueueSystem::AddToQueue(E(idx), 0);
	if (!quiet) {
		Sfx::Play(Sfx::GET);
		this->pressAnim = 1.0f;
	}
	return true;
}

void KioskScreen::UpdateAll() {
	int n = 0;
	for (int i : this->updates.items) {
		if (this->InQueue(i)) continue;
		this->UpdateEntry(i, true);
		n++;
	}
	if (n > 0) {
		Sfx::Play(Sfx::GET);
		this->StartFly(this->updates.items[0], 262, 19);
		this->ShowToast("Updating " + std::to_string(n) + (n == 1 ? " app" : " apps"));
	} else {
		Sfx::Play(Sfx::ERROR);
		this->ShowToast("Updates are already queued");
	}
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
	Sfx::Play(Sfx::OPEN);
}

void KioskScreen::CloseApp() {
	this->appOpen = false;
	this->topMode = TopMode::Details;
	this->FreeShot();
	Sfx::Play(Sfx::BACK);
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
	Sfx::Update();

	/* The engine finished something: celebrate, or say what went wrong. */
	QueueSystem::Popup = false;
	if (QueueSystem::FinishedSerial != this->lastSerial) {
		this->lastSerial = QueueSystem::FinishedSerial;
		if (QueueSystem::LastOk && QueueSystem::LastEntry) {
			this->celebrating = true;
			this->celebT = 0;
			this->celebEntry = QueueSystem::LastEntry;
			this->confetti.clear();
			Sfx::Play(Sfx::COMPLETE);
			Sfx::Duck(150);
		} else {
			Sfx::Play(Sfx::ERROR);
			this->ShowToast(QueueSystem::EndMsg);
		}
	}

	const bool pending = RequestPending();
	if (pending && !this->requestToastShown && this->tab != TAB_DOWNLOADS) {
		this->ShowToast("A download needs your answer — open Downloads");
		Sfx::Play(Sfx::ERROR);
		this->requestToastShown = true;
	}
	if (!pending) this->requestToastShown = false;

	if (this->storeOk) this->RefreshLists();

#ifdef KIOSK_DEMO
	this->DemoStep();
	this->Animate();
	return;
#endif

	/* While celebrating, any button (after a beat) continues. */
	if (this->celebrating) {
		if (this->celebT > 45 && (hDown & (KEY_A | KEY_B | KEY_TOUCH | KEY_START))) {
			this->celebrating = false;
			Sfx::Play(Sfx::SELECT);
		}
		this->Animate();
		return;
	}

	if (hDown & KEY_START && !QueueRuns) {
		exiting = true;
		return;
	}

	if (hDown & KEY_SELECT) {
		Sfx::ToggleMusic();
		this->ShowToast(!Sfx::Available() ? "Sound needs dspfirm.cdc (run DSP1)" : (Sfx::MusicOn() ? "Music on" : "Music off"));
	}

	const bool wasOpen = this->appOpen;
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

	/* Cursor sounds come from one place: did the tab or the cursor move? */
	const int sig = this->CursorSignature();
	if (this->tab != this->lastTab) {
		Sfx::Play(Sfx::TAB);
		this->tabFade = 1.0f;
		this->lastTab = this->tab;
	} else if (sig != this->lastSig && wasOpen == this->appOpen) {
		Sfx::Play(Sfx::MOVE);
	}
	this->lastSig = sig;

	this->Animate();
}

void KioskScreen::HomeLogic(u32 down, u32 rep, touchPosition t) {
	if (this->shelves.empty()) return;
	Shelf &s = this->shelves[this->shelfSel];

	if (rep & KEY_RIGHT && s.sel < (int)s.items.size() - 1) s.sel++;
	if (rep & KEY_LEFT && s.sel > 0) s.sel--;
	if (rep & KEY_DOWN && this->shelfSel < (int)this->shelves.size() - 1) this->shelfSel++;
	if (rep & KEY_UP && this->shelfSel > 0) this->shelfSel--;
	if (down & KEY_A) {
		this->pressAnim = 1.0f;
		this->OpenApp(s.items[s.sel]);
	}
	if (down & KEY_Y) this->tab = TAB_SEARCH;

	if (down & KEY_TOUCH) {
		for (int si = 0; si < (int)this->shelves.size(); si++) {
			Shelf &sh = this->shelves[si];
			const float top = 6 + si * SHELF_H - this->homeScroll + 22;
			if (t.py < top || t.py >= top + TILE) continue;
			for (int i = 0; i < (int)sh.items.size(); i++) {
				const float x = SHELF_X + i * TILE_STRIDE - sh.scroll;
				if (!hit(t, x, top, TILE, TILE)) continue;
				if (si == this->shelfSel && i == sh.sel) {
					this->pressAnim = 1.0f;
					this->OpenApp(sh.items[i]);
				} else {
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
	const int minSel = kind == ListKind::Search ? -1 : 0;

	if (n > 0) {
		if (rep & KEY_DOWN && list.sel < n - 1) list.sel++;
		if (rep & KEY_UP && list.sel > minSel) list.sel--;
		if (rep & KEY_RIGHT) list.sel = std::min(n - 1, list.sel + 3);
		if (rep & KEY_LEFT) list.sel = std::max(minSel, list.sel - 3);
	}

	auto rowPill = [&](int i, float &x, float &y) {
		x = 267;
		y = y0 + i * ROW_STRIDE - list.scroll + 24;
	};

	if (down & KEY_A && list.sel >= 0 && list.sel < n) {
		if (kind == ListKind::Updates) {
			float x, y;
			rowPill(list.sel, x, y);
			if (this->UpdateEntry(list.items[list.sel])) this->StartFly(list.items[list.sel], x, y);
		} else {
			this->pressAnim = 1.0f;
			this->OpenApp(list.items[list.sel]);
		}
	}
	if (kind == ListKind::Updates && down & KEY_Y && list.sel >= 0 && list.sel < n) this->OpenApp(list.items[list.sel]);

	if (down & KEY_TOUCH && t.py >= y0 && t.py < TAB_Y) {
		const int i = (int)((t.py - y0 + list.scroll) / ROW_STRIDE);
		if (i >= 0 && i < n) {
			const bool onPill = t.px >= 226;
			if (kind == ListKind::Updates && onPill) {
				float x, y;
				rowPill(i, x, y);
				if (this->UpdateEntry(list.items[i])) this->StartFly(list.items[i], x, y);
			} else if (list.sel == i || onPill) {
				this->pressAnim = 1.0f;
				this->OpenApp(list.items[i]);
			}
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
		Sfx::Play(Sfx::BACK);
		return;
	}

	if (openKeyboard) {
		Sfx::Play(Sfx::SELECT);
		std::string q = this->query;
		if (Input::getTextKeyboard(q, 30, "Search Universal-DB", q, StoreUtils::allEntries)) {
			this->query = q;
			this->RunSearch();
		}
		this->lastSig = this->CursorSignature(); // Don't tick for the jump back to results.
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
	const bool pending = RequestPending();
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
	if (down & KEY_X && !pending && QueueRuns) {
		QueueSystem::CancelCallback = true;
		Sfx::Play(Sfx::BACK);
	}
	if (down & KEY_Y && this->downloadSel > 0 && this->downloadSel < n) {
		RemoveQueued(this->downloadSel);
		Sfx::Play(Sfx::BACK);
		this->ShowToast("Removed from downloads");
	}

	if (down & KEY_TOUCH) {
		for (int i = 0; i < n; i++) {
			const float y = i == 0 ? listY : listY + (ROW_H + 14 + 14) + (i - 1) * ROW_STRIDE;
			if (!hit(t, 8, y, 304, i == 0 ? ROW_H + 14 : ROW_H)) continue;
			if (hit(t, 240, y, 72, 30)) {
				if (i == 0 && !pending && QueueRuns) QueueSystem::CancelCallback = true;
				else if (i > 0) RemoveQueued(i);
				Sfx::Play(Sfx::BACK);
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
		if (this->topMode != TopMode::Details) {
			this->topMode = TopMode::Details;
			this->FreeShot();
			Sfx::Play(Sfx::BACK);
		} else {
			this->CloseApp();
		}
		return;
	}

	auto queueRow = [&](int i) {
		const float y = SCRIPT_Y0 + i * SCRIPT_STRIDE - this->appScroll + SCRIPT_ROW_H / 2;
		if (this->QueueScript(this->appIdx, i)) this->StartFly(this->appIdx, 260, y);
	};

	if (rep & KEY_DOWN && this->appSel < scripts - 1) this->appSel++;
	if (rep & KEY_UP && this->appSel > 0) this->appSel--;
	if (down & KEY_A) queueRow(this->appSel);

	auto toggleNotes = [&]() {
		if (!hasNotes) { Sfx::Play(Sfx::ERROR); this->ShowToast("No release notes for this one"); return; }
		this->topMode = this->topMode == TopMode::WhatsNew ? TopMode::Details : TopMode::WhatsNew;
		this->notesScroll = 0;
		this->FreeShot();
		Sfx::Play(Sfx::SELECT);
	};
	auto toggleShots = [&]() {
		if (!hasShots) { Sfx::Play(Sfx::ERROR); this->ShowToast("No screenshots for this one"); return; }
		Sfx::Play(Sfx::SELECT);
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
		if (down & (KEY_R | KEY_ZR) && count > 1) { this->shotIdx = (this->shotIdx + 1) % count; Sfx::Play(Sfx::TAB); this->LoadShot(); }
		if (down & (KEY_L | KEY_ZL) && count > 1) { this->shotIdx = (this->shotIdx + count - 1) % count; Sfx::Play(Sfx::TAB); this->LoadShot(); }
	} else if (this->topMode == TopMode::WhatsNew) {
		const int lines = Wrap(e->GetReleaseNotes(), 0.46f, 352).size();
		if (rep & (KEY_R | KEY_ZR)) this->notesScroll = std::min(this->notesScroll + 3, std::max(0, lines - 9));
		if (rep & (KEY_L | KEY_ZL)) this->notesScroll = std::max(0, this->notesScroll - 3);
	}

	if (down & KEY_TOUCH) {
		if (hit(t, 8, 204, 66, 30)) { this->CloseApp(); return; }
		if (hit(t, 80, 204, 114, 30)) { toggleShots(); return; }
		if (hit(t, 200, 204, 112, 30)) { toggleNotes(); return; }
		if (t.py >= SCRIPT_Y0 && t.py < SCRIPT_Y1) {
			const int i = (int)((t.py - SCRIPT_Y0 + this->appScroll) / SCRIPT_STRIDE);
			if (i >= 0 && i < scripts) {
				if (this->appSel == i || t.px >= 216) queueRow(i);
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
	this->pressAnim = std::max(0.0f, this->pressAnim - 0.12f);
	this->tabFade = std::max(0.0f, this->tabFade - 0.1f);
	this->tabBounce = std::max(0.0f, this->tabBounce - 0.05f);
	this->dockBounce = std::max(0.0f, this->dockBounce - 0.05f);

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
	if (this->celebrating && this->celebEntry) target = AccentFor(this->celebEntry->GetAccentColor(), this->celebEntry->GetTitle());
	else if (focus >= 0) target = AccentFor(E(focus)->GetAccentColor(), E(focus)->GetTitle());
	else if (this->tab == TAB_DOWNLOADS && !queueEntries.empty()) {
		const auto &q = queueEntries[0].storeEntry;
		target = AccentFor(q->GetAccentColor(), q->GetTitle());
	}
	this->heroColor = Lerp(this->heroColor, target, 0.18f);

	/* Flying icon. */
	if (this->fly.t < 1.0f) {
		this->fly.t = std::min(1.0f, this->fly.t + 0.045f);
		if (this->fly.t >= 1.0f) {
			if (this->appOpen) this->dockBounce = 1.0f;
			else this->tabBounce = 1.0f;
		}
	}

	/* Download scene. */
	static const StoreEntry *jarOwner = nullptr;
	const bool active = !queueEntries.empty() && QueueRuns && !RequestPending();
	this->floorPhase += active ? 0.008f : 0.002f;
	if (this->floorPhase >= 1.0f) { this->floorPhase -= 1.0f; this->floorShift++; }
	this->railOffset = fmodf(this->railOffset + (active ? 0.7f : 0.15f), 64.0f);

	if (!queueEntries.empty()) {
		const QueueEntry &q = queueEntries[0];
		if (q.storeEntry.get() != jarOwner) {
			jarOwner = q.storeEntry.get();
			this->jarLevel = 0.0f;
			this->gummies.clear();
		}
		this->jarLevel = Approach(this->jarLevel, this->OverallProgress(q), 0.06f);
	} else if (!this->celebrating) {
		jarOwner = nullptr;
		this->jarLevel = Approach(this->jarLevel, 0.0f, 0.1f);
	}

	if (active && ++this->dropTimer >= 30) {
		this->dropTimer = 0;
		this->bagDrop = 1.0f;
		this->gummies.push_back({ 200 + frand(-5, 5), RAIL_Y + 30, 0.5f, GUMMY_COLORS[rand() % 6] });
	}
	const float surface = JAR_TOP + JAR_SIZE - 3 - (JAR_SIZE - 6) * std::max(this->jarLevel, 0.06f);
	for (size_t i = 0; i < this->gummies.size();) {
		Gummy &g = this->gummies[i];
		g.vy += 0.32f;
		g.y += g.vy;
		if (g.y >= surface) {
			this->ripple = 1.0f;
			this->gummies.erase(this->gummies.begin() + i);
		} else {
			i++;
		}
	}
	this->ripple = std::max(0.0f, this->ripple - 0.035f);
	this->bagDrop = std::max(0.0f, this->bagDrop - 0.08f);

	/* Celebration timeline. */
	if (this->celebrating) {
		this->celebT++;
		if (this->celebT == 30) {
			Sfx::Play(Sfx::POP);
			for (int i = 0; i < 80; i++) {
				const float a = frand(0, 2 * M_PI), sp = frand(2.0f, 7.5f);
				this->confetti.push_back({ 200, JAR_TOP + JAR_SIZE / 2, cosf(a) * sp, sinf(a) * sp - 3.5f, frand(0, 6.28f), frand(-0.3f, 0.3f), frand(4, 8), frand(3, 6), GUMMY_COLORS[rand() % 6] });
			}
		}
		if (this->celebT > 280) this->celebrating = false;
	}
	for (size_t i = 0; i < this->confetti.size();) {
		Confetti &c = this->confetti[i];
		c.vy += 0.16f;
		c.vx *= 0.985f;
		c.x += c.vx;
		c.y += c.vy;
		c.rot += c.vrot;
		if (c.y > 260) this->confetti.erase(this->confetti.begin() + i);
		else i++;
	}

	if (this->fadeIn > 0) this->fadeIn = std::max(0.0f, this->fadeIn - 12.0f);

	if (this->toastTimer > 0) {
		this->toastTimer--;
		this->toastY = Approach(this->toastY, 28.0f, 0.25f);
	} else {
		this->toastY = Approach(this->toastY, -30.0f, 0.25f);
	}
}

/*
	Drawing — top screen.
*/
void KioskScreen::Draw(void) const {
	Gui::ScreenDraw(Top);
	if (this->celebrating) this->DrawCelebration();
	else if (this->appOpen && this->topMode == TopMode::Screenshot) this->DrawScreenshotTop(this->appIdx);
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
	Pattern(320, 240, this->frame * 0.15f, BG_DOT);
	if (this->appAnim < 0.99f) {
		if (!this->storeOk && this->tab != TAB_DOWNLOADS) {
			this->DrawEmptyBottom("Store unavailable", "Press START to exit.", 60);
		} else {
			switch (this->tab) {
				case TAB_HOME: this->DrawHome(); break;
				case TAB_SEARCH: this->DrawSearch(); break;
				case TAB_UPDATES: this->DrawUpdates(); break;
				case TAB_DOWNLOADS: this->DrawDownloads(); break;
				case TAB_LIBRARY:
					if (this->library.items.empty()) this->DrawEmptyBottom("Your library is empty", "Find something on the Home tab.", 50);
					else this->DrawList(this->library, ListKind::Library, 8, TAB_Y);
					break;
			}
		}
		if (this->tabFade > 0) Rect(0, 0, 320, TAB_Y, Alpha(BG, (u8)(230 * this->tabFade)));
		this->DrawTabBar();
	}
	if (this->appAnim > 0.01f && this->appIdx >= 0) this->DrawAppPage((1.0f - EaseOutCubic(this->appAnim)) * 320.0f);
	this->DrawFly();

	if (this->fadeIn > 0) Rect(0, 0, 320, 240, Alpha(BG, (u8)this->fadeIn));
	if (fadeAlpha > 0) Rect(0, 0, 320, 240, RGBA(0, 0, 0, fadeAlpha));
}

void KioskScreen::DrawStatusBar() const {
	Rect(0, 0, 400, 22, CARD);
	Pill(6, 3, 64, 16, SKY);
	TextCentered(38, 3, 0.4f, WHITE, "Kiosk");
	if (Sfx::Available() && Sfx::MusicOn()) {
		/* Little music note. */
		Circle(80, 15, 3, SKY);
		Rect(82, 5, 1.5f, 10, SKY);
		Rect(82, 5, 5, 2, SKY);
	}

	time_t now = time(nullptr);
	struct tm *tm = gmtime(&now);
	char clock[8];
	snprintf(clock, sizeof(clock), "%02d:%02d", tm->tm_hour, tm->tm_min);
	TextRight(358, 3, 0.48f, TEXT, clock);

	u8 level = 0, charging = 0;
	PTMU_GetBatteryLevel(&level);
	PTMU_GetBatteryChargeState(&charging);
	const float bx = 366, by = 5, bw = 22, bh = 12;
	RoundRect(bx, by, bw, bh, 3, SUB);
	RoundRect(bx + 1.5f, by + 1.5f, bw - 3, bh - 3, 2, CARD);
	Rect(bx + bw, by + 3.5f, 2, bh - 7, SUB);
	const float fill = std::clamp(level / 5.0f, 0.0f, 1.0f) * (bw - 5);
	Rect(bx + 2.5f, by + 2.5f, fill, bh - 5, (level <= 1 && !charging) ? BADGE : (charging ? GOOD : SKY));
}

void KioskScreen::DrawChrome(const std::string &title, const std::string &right) const {
	Rect(0, 0, 400, 240, BG);
	Pattern(400, 240, this->frame * 0.2f, BG_DOT);
	this->DrawStatusBar();
	Gradient(0, 22, 400, 20, RGBA(255, 252, 244), RGBA(244, 234, 208));
	Rect(0, 42, 400, 1, LINE);
	Text(10, 24, 0.5f, TEXT, title, 260);
	if (!right.empty()) TextRight(390, 25, 0.46f, SUB, right, 120);
}

void KioskScreen::DrawHero(int idx, bool detailed) const {
	const auto &e = E(idx);
	const u32 bg = this->heroColor;
	this->DrawChrome(detailed ? "Software details" : "Kiosk", e->GetConsoleString());

	const float t = EaseOutCubic(this->heroFade);
	const float off = (1.0f - t) * 8.0f;

	/* Colour header card. */
	RoundRect(10, 50, 380, 78, 14, Darken(bg, 0.35f));
	C2D_DrawRectangle(10 + 14, 48, 0.5f, 380 - 28, 78, Lighten(bg, 0.12f), Lighten(bg, 0.12f), Darken(bg, 0.12f), Darken(bg, 0.12f));
	RoundRect(10, 48, 28, 78, 14, Lighten(bg, 0.12f));
	RoundRect(362, 48, 28, 78, 14, Lighten(bg, 0.12f));
	C2D_DrawRectangle(10, 62, 0.5f, 380, 50, Lighten(bg, 0.05f), Lighten(bg, 0.05f), Darken(bg, 0.1f), Darken(bg, 0.1f));
	Circle(360, 58, 46, Alpha(WHITE, 18));

	Card(20, 55 + off, 64, 64, 14, WHITE, Darken(bg, 0.4f));
	Image(e->GetIcon(), 52, 87 + off, 1.2f);

	const u8 a = (u8)(255 * t);
	Text(96, 53 + off, 0.66f, Alpha(WHITE, a), e->GetTitle(), 284);
	Text(96, 76 + off, 0.44f, Alpha(Lighten(bg, 0.78f), a), "by " + e->GetAuthor(), 284);

	float cx = 96;
	auto chip = [&](const std::string &label, u32 fill, u32 ink) {
		const float w = TextWidth(0.38f, label) + 14;
		if (cx + w > 384) return;
		Pill(cx, 99 + off, w, 18, fill);
		Text(cx + 7, 101 + off, 0.38f, ink, label);
		cx += w + 5;
	};
	const State s = this->StateOf(idx);
	if (s == State::Update) chip("Update available", WHITE, ACCENT);
	else if (s == State::Installed) chip("Installed", WHITE, GOOD);
	else if (s == State::Queued) chip("In downloads", WHITE, SKY);
	if (this->IsNew(idx)) chip("New", SKY, WHITE);
	if (e->GetStars() > 0) chip("★ " + std::to_string(e->GetStars()), Darken(bg, 0.22f), WHITE);
	for (const std::string &cat : e->GetCategories()) chip(PrettyCategory(cat), Darken(bg, 0.22f), WHITE);

	/* Description card. */
	Card(10, 134, 380, 100, 14);
	const std::string desc = e->GetDescription().empty() ? "No description provided." : e->GetDescription();
	if (detailed) {
		Paragraph(22, 141, 0.45f, TEXT, desc, 356, 4, 15);
		Rect(22, 203, 356, 1, LINE);
		std::string meta;
		if (!e->GetVersion().empty()) meta += "Version " + e->GetVersion();
		if (!e->GetLastUpdated().empty()) meta += (meta.empty() ? "" : "   ·   ") + std::string("Updated ") + DateOnly(e->GetLastUpdated());
		if (!e->GetLicense().empty()) meta += (meta.empty() ? "" : "   ·   ") + e->GetLicense();
		Text(22, 210, 0.4f, SUB, meta, 356);
	} else {
		Paragraph(22, 142, 0.45f, TEXT, desc, 356, 6, 15);
	}
}

void KioskScreen::DrawEmptyTop(const std::string &title, const std::string &subtitle, int glyph) const {
	this->DrawChrome("Kiosk");

	const float phase = fmodf(this->frame / 45.0f, 1.0f);
	const float hop = sinf(phase * M_PI);
	if (glyph == GLYPH_WARN) {
		Circle(200, 104, 30, BADGE);
		TextCentered(200, 84, 1.0f, WHITE, "!");
	} else {
		C2D_DrawEllipseSolid(200 - 22 * (1 - hop * 0.3f), 131, 0.5f, 44 * (1 - hop * 0.3f), 8, BG_DOT);
		Bag(200, 104 - hop * 16, 54, ACCENT, BG, phase < 0.1f ? 0.1f : 0.0f);
		if (glyph == GLYPH_CHECK) {
			Circle(226, 86 - hop * 16, 11, GOOD);
			Check(226, 86 - hop * 16, 11, WHITE);
		}
	}
	Card(40, 150, 320, 66, 16);
	TextCentered(200, 159, 0.62f, TEXT, title, 300);
	TextCentered(200, 186, 0.45f, SUB, subtitle, 300);
}

void KioskScreen::DrawWhatsNewTop(int idx) const {
	const auto &e = E(idx);
	this->DrawChrome("What's new", e->GetVersion().empty() ? DateOnly(e->GetLastUpdated()) : e->GetVersion());

	Card(10, 50, 380, 184, 14);
	RoundRect(10, 50, 380, 26, 14, this->heroColor);
	Rect(10, 64, 380, 12, this->heroColor);
	Text(22, 54, 0.48f, WHITE, e->GetTitle(), 300);
	Paragraph(22, 84, 0.45f, TEXT, e->GetReleaseNotes(), 352, 9, 16, this->notesScroll);

	const int lines = Wrap(e->GetReleaseNotes(), 0.45f, 352).size();
	if (lines > 9) {
		const float trackH = 140, thumbH = std::max(20.0f, trackH * 9.0f / lines);
		const float y = 84 + (trackH - thumbH) * (this->notesScroll / (float)std::max(1, lines - 9));
		Pill(381, 84, 3, trackH, LINE);
		Pill(381, std::min(y, 84 + trackH - thumbH), 3, thumbH, ACCENT);
	}
}

void KioskScreen::DrawScreenshotTop(int idx) const {
	const auto &e = E(idx);
	Rect(0, 0, 400, 240, RGBA(30, 26, 22));

	if (this->shot.tex && this->shot.subtex) {
		const float w = this->shot.subtex->width, h = this->shot.subtex->height;
		const float scale = std::min(400.0f / w, 240.0f / h);
		Image(this->shot, 200, 120, scale);
	} else if (this->shotFailed) {
		TextCentered(200, 104, 0.55f, RGBA(220, 210, 195), "Couldn't load this screenshot");
	}

	Rect(0, 214, 400, 26, RGBA(0, 0, 0, 170));
	const auto &names = e->GetScreenshotNames();
	const std::string name = this->shotIdx < (int)names.size() ? names[this->shotIdx] : e->GetTitle();
	Text(10, 219, 0.45f, WHITE, name, 250);
	const int count = e->GetScreenshots().size();
	TextRight(390, 219, 0.45f, RGBA(220, 210, 195), (count > 1 ? "L / R   " : "") + std::to_string(this->shotIdx + 1) + " of " + std::to_string(count));
}

/* Warm room with a tiled floor sliding toward the viewer. */
void KioskScreen::DrawFloorScene(float horizon) const {
	Gradient(0, 43, 400, horizon - 43, RGBA(255, 242, 218), RGBA(255, 214, 160));

	const float vx = 200, vy = horizon - 70;
	const int rows = 7;
	const float tileW = 76;
	for (int k = -1; k < rows; k++) {
		float ta = (k + this->floorPhase) / rows, tb = (k + 1 + this->floorPhase) / rows;
		ta = std::clamp(ta, 0.0f, 1.0f);
		tb = std::clamp(tb, 0.0f, 1.0f);
		if (tb <= ta) continue;
		const float ya = horizon + (240 - horizon) * ta * ta, yb = horizon + (240 - horizon) * tb * tb;
		const float sa = (ya - vy) / (240 - vy), sb = (yb - vy) / (240 - vy);
		for (int c = -6; c < 6; c++) {
			const float X0 = vx + c * tileW, X1 = X0 + tileW;
			const u32 col = ((k - this->floorShift + c) & 1) ? RGBA(255, 206, 140) : RGBA(255, 222, 172);
			Quad(vx + (X0 - vx) * sa, ya, vx + (X1 - vx) * sa, ya, vx + (X1 - vx) * sb, yb, vx + (X0 - vx) * sb, yb, col);
		}
	}
	/* Haze where the floor meets the wall. */
	Gradient(0, horizon, 400, 16, RGBA(255, 226, 180), Alpha(RGBA(255, 226, 180), 0));
}

void KioskScreen::DrawJar(float cx, float top, float size, float level, const C2D_Image &icon, u32 frameColor, float scale, float alpha) const {
	const float s = size * scale;
	const float x = cx - s / 2, y = top + (size - s);
	const float d = s * 0.14f;
	const u8 a = (u8)(255 * std::clamp(alpha, 0.0f, 1.0f));
	const float bottom = y + s;

	/* Floor shadow. */
	C2D_DrawEllipseSolid(x - 6, bottom - 6, 0.5f, s + d + 12, 14, Alpha(RGBA(230, 150, 70), (u8)(90 * alpha)));

	/* Back faces. */
	Rect(x + d, y - d, s, s, Alpha(WHITE, a * 50 / 255));

	/* Liquid. */
	const float lvl = std::clamp(level, 0.0f, 1.0f);
	if (lvl > 0.01f) {
		const float h = (s - 6) * lvl;
		const float ly = bottom - 3 - h;
		const u32 hi = Alpha(Lighten(ACCENT, 0.3f), (u8)(235 * alpha)), lo = Alpha(Darken(ACCENT, 0.08f), (u8)(235 * alpha));
		const float amp = 1.2f + this->ripple * 3.5f;
		const int segs = 14;
		for (int i = 0; i < segs; i++) {
			const float xa = x + 3 + (s - 6) * i / segs, xb = x + 3 + (s - 6) * (i + 1) / segs;
			const float ya = ly + sinf(this->frame * 0.12f + i * 0.8f) * amp, yb = ly + sinf(this->frame * 0.12f + (i + 1) * 0.8f) * amp;
			C2D_DrawTriangle(xa, ya, hi, xb, yb, hi, xb, bottom - 3, lo, 0.5f);
			C2D_DrawTriangle(xa, ya, hi, xb, bottom - 3, lo, xa, bottom - 3, lo, 0.5f);
		}
		Quad(x + s - 3, ly, x + s - 3 + d, ly - d, x + s - 3 + d, bottom - 3 - d, x + s - 3, bottom - 3, Alpha(Darken(ACCENT, 0.2f), (u8)(200 * alpha)));
		Quad(x + 3, ly, x + s - 3, ly, x + s - 3 + d, ly - d, x + 3 + d, ly - d, Alpha(Lighten(ACCENT, 0.5f), (u8)(220 * alpha)));
	}

	/* Glass: top and side faces, front pane and bright edges. */
	Quad(x, y, x + s, y, x + s + d, y - d, x + d, y - d, Alpha(WHITE, a * 120 / 255));
	Quad(x + s, y, x + s + d, y - d, x + s + d, bottom - d, x + s, bottom, Alpha(WHITE, a * 70 / 255));
	Rect(x, y, s, s, Alpha(WHITE, a * 45 / 255));
	const u32 edge = Alpha(WHITE, a * 220 / 255);
	Rect(x, y, s, 2, edge);
	Rect(x, bottom - 2, s, 2, edge);
	Rect(x, y, 2, s, edge);
	Rect(x + s - 2, y, 2, s, edge);
	Rect(x + 6, y + 6, 4, s * 0.35f, Alpha(WHITE, a * 140 / 255));

	/* App icon framed on the front. */
	const float fs = s * 0.6f;
	const float fy = y + s * 0.52f - fs / 2;
	RoundRect(cx - fs / 2 - 3, fy - 3, fs + 6, fs + 6, 12 * scale, frameColor);
	RoundRect(cx - fs / 2, fy, fs, fs, 10 * scale, WHITE);
	Image(icon, cx, fy + fs / 2, fs / 54.0f);
}

void KioskScreen::DrawDownloadsTop() const {
	if (queueEntries.empty()) {
		this->DrawEmptyTop("No downloads", "Things you download or update show up here.", GLYPH_DOWNLOAD);
		return;
	}

	const QueueEntry &q = queueEntries[0];
	const auto &e = q.storeEntry;
	const bool pending = RequestPending();

	float stepP = -1.0f;
	const float overall = this->OverallProgress(q, &stepP);
	char pct[16];
	snprintf(pct, sizeof(pct), "%d%%", (int)(overall * 100));
	this->DrawChrome(pending ? "Waiting for you" : "Downloading software", pending ? "" : pct);
	this->DrawFloorScene(HORIZON);

	/* Rail with hanging beads, drifting while busy. */
	Rect(0, RAIL_Y, 400, 2, RGBA(240, 172, 96));
	for (float bx = -this->railOffset; bx < 420; bx += 64) {
		Rect(bx - 0.5f, RAIL_Y + 2, 1, 8, RGBA(200, 150, 100));
		Circle(bx, RAIL_Y + 12, 4, RGBA(255, 176, 70));
		Circle(bx - 1.2f, RAIL_Y + 10.8f, 1.3f, Alpha(WHITE, 200));
	}

	/* The bag riding the rail, squashing as it drops a gummy. */
	const float sway = sinf(this->frame * 0.06f) * 3;
	const float squash = this->bagDrop * 0.18f;
	Bag(200 + sway, RAIL_Y + 14 + this->bagDrop * 3, 30, ACCENT, RGBA(255, 236, 206), squash);
	if (pending) {
		Card(220, RAIL_Y - 16, 22, 22, 11);
		TextCentered(231, RAIL_Y - 14, 0.5f, ACCENT, "?");
	}

	for (const Gummy &g : this->gummies) {
		Circle(g.x, g.y, 6, g.color);
		Circle(g.x - 2, g.y - 2, 2, Alpha(WHITE, 190));
	}

	this->DrawJar(200, JAR_TOP, JAR_SIZE, this->jarLevel, e->GetIcon(), this->heroColor, 1.0f, 1.0f);

	/* Name plate. */
	const std::string title = Ellipsize(e->GetTitle(), 0.5f, 300);
	const float w = TextWidth(0.5f, title) + 28;
	Card(200 - w / 2, 214, w, 22, 11);
	TextCentered(200, 216, 0.5f, TEXT, title);

	if (queueEntries.size() > 1) {
		Pill(318, 214, 76, 22, Alpha(WHITE, 200));
		TextCentered(356, 217, 0.4f, SUB, "+" + std::to_string(queueEntries.size() - 1) + " waiting");
	}
}

void KioskScreen::DrawCelebration() const {
	const auto &e = this->celebEntry;
	const int t = this->celebT;
	this->DrawChrome("Download complete");
	this->DrawFloorScene(HORIZON);
	Rect(0, RAIL_Y, 400, 2, RGBA(240, 172, 96));

	/* 0-30: the full cube glows. 30-38: it pops. */
	if (t < 38) {
		const float scale = t < 30 ? 1.0f + 0.05f * sinf(t * 0.7f) : 1.0f - (t - 30) / 8.0f;
		if (scale > 0.02f) this->DrawJar(200, JAR_TOP, JAR_SIZE, 1.0f, e->GetIcon(), this->heroColor, scale, 1.0f);
		if (t < 30) Circle(200, JAR_TOP + JAR_SIZE / 2, 50 + t, Alpha(WHITE, (u8)(t * 3)));
	}
	if (t >= 30 && t < 44) Circle(200, JAR_TOP + JAR_SIZE / 2, (t - 30) * 12.0f, Alpha(WHITE, (u8)(220 * (1.0f - (t - 30) / 14.0f))));

	/* The icon springs out with sparkles circling it. */
	if (t >= 30) {
		const float k = EaseOutBack((t - 30) / 26.0f);
		const float cy = (JAR_TOP + JAR_SIZE * 0.52f) - 52 * k;
		const float size = 50 * (1.0f + 0.6f * k);
		for (int i = 0; i < 6; i++) {
			const float a = t * 0.05f + i * (M_PI / 3);
			const float r = size * 0.9f + 6 * sinf(t * 0.2f + i);
			Sparkle(200 + cosf(a) * r, cy + sinf(a) * r * 0.8f, 10 + 4 * sinf(t * 0.3f + i * 2), t * 0.08f, i % 2 ? WHITE : RGBA(255, 220, 90));
		}
		RoundRect(200 - size / 2 - 4, cy - size / 2 - 4, size + 8, size + 8, 14, this->heroColor);
		RoundRect(200 - size / 2, cy - size / 2, size, size, 12, WHITE);
		Image(e->GetIcon(), 200, cy, size / 54.0f);
	}

	for (const Confetti &c : this->confetti) RotRect(c.x, c.y, c.w, c.h, c.rot, c.color);

	/* Banner. */
	if (t >= 46) {
		const float k = EaseOutBack((t - 46) / 18.0f);
		const float w = 200 * std::max(0.05f, k);
		Card(200 - w / 2, 160, w, 28, 14, ACCENT, Darken(ACCENT, 0.3f));
		if (k > 0.8f) TextCentered(200, 164, 0.56f, WHITE, "Download complete!");
		const u8 a = (u8)(255 * std::clamp((t - 56) / 12.0f, 0.0f, 1.0f));
		const std::string title = Ellipsize(e->GetTitle(), 0.5f, 300);
		const float tw = TextWidth(0.5f, title) + 28;
		Card(200 - tw / 2, 196, tw, 22, 11, Alpha(WHITE, a), Alpha(CARD_SHADOW, a));
		TextCentered(200, 198, 0.5f, Alpha(TEXT, a), title);
	}
	if (t > 60) TextRight(392, 222, 0.4f, Alpha(SUB, (u8)std::min(255, (t - 60) * 8)), "A  Continue");
}

void KioskScreen::DrawToast() const {
	if (this->toastY <= -29.0f || this->toast.empty()) return;
	const std::string msg = Ellipsize(this->toast, 0.5f, 340);
	const float w = TextWidth(0.5f, msg) + 32;
	const float x = 200 - w / 2.0f, y = this->toastY;
	Pill(x, y + 2, w, 28, CARD_SHADOW);
	Pill(x, y, w, 28, WHITE);
	Pill(x + 2, y + 2, 4, 24, ACCENT);
	Text(x + 16, y + 6, 0.5f, TEXT, msg);
}

/*
	Drawing — bottom screen.
*/
void KioskScreen::DrawFocusRing(float x, float y, float w, float h, float r) const {
	const float pulse = 0.5f + 0.5f * sinf(this->frame * 0.11f);
	RoundRect(x - 4, y - 4, w + 8, h + 8, r + 4, Lerp(ACCENT, Lighten(ACCENT, 0.45f), pulse));
}

void KioskScreen::DrawTabBar() const {
	Rect(0, TAB_Y, 320, 240 - TAB_Y, WHITE);
	Rect(0, TAB_Y, 320, 1, LINE);
	Pill(this->tabPillX + 6, TAB_Y + 4, TAB_W - 12, 30, ACCENT_SOFT);

	static const char *labels[TAB_COUNT] = { "Home", "Search", "Updates", "Downloads", "Library" };
	for (int i = 0; i < TAB_COUNT; i++) {
		const float cx = i * TAB_W + TAB_W / 2.0f;
		const u32 c = i == this->tab ? ACCENT : SUB;
		const float bounce = i == TAB_DOWNLOADS ? sinf(this->tabBounce * M_PI) * 0.45f : 0.0f;
		const float iy = TAB_Y + 13 - bounce * 6;
		switch (i) {
			case TAB_HOME: House(cx, iy + 2, 13, c); break;
			case TAB_SEARCH: Sprite(sprites_search_idx, cx, iy, c, 0.75f); break;
			case TAB_UPDATES: Sprite(sprites_update_idx, cx, iy, c, 0.75f); break;
			case TAB_DOWNLOADS: Bag(cx, iy, 14 * (1.0f + bounce * 0.5f), c, i == this->tab ? ACCENT_SOFT : WHITE); break;
			case TAB_LIBRARY: Sprite(sprites_installed_app_idx, cx, iy, c, 0.75f); break;
		}
		TextCentered(cx, TAB_Y + 22, 0.34f, c, labels[i]);
	}

	if (!this->updates.items.empty()) Badge(TAB_UPDATES * TAB_W + 44, TAB_Y + 7, std::to_string(this->updates.items.size()));
	if (RequestPending()) Badge(TAB_DOWNLOADS * TAB_W + 44, TAB_Y + 7, "!");
	else if (!queueEntries.empty()) Badge(TAB_DOWNLOADS * TAB_W + 44, TAB_Y + 7, std::to_string(queueEntries.size()));
}

void KioskScreen::DrawHome() const {
	const float pop = EaseOutBack(this->focusPop);

	for (int si = 0; si < (int)this->shelves.size(); si++) {
		const Shelf &s = this->shelves[si];
		const float top = 6 + si * SHELF_H - this->homeScroll;
		if (top > TAB_Y || top + SHELF_H < 0) continue;

		const bool activeShelf = si == this->shelfSel;
		if (activeShelf) Pill(SHELF_X - 4, top - 2, TextWidth(0.5f, s.title) + 18, 18, ACCENT);
		Text(SHELF_X + 5, top, 0.5f, activeShelf ? WHITE : TEXT, s.title);
		TextRight(308, top + 2, 0.4f, SUB, std::to_string(s.items.size()));

		const float tileY = top + 22;
		int focusedIdx = -1;
		for (int i = 0; i < (int)s.items.size(); i++) {
			const float x = SHELF_X + i * TILE_STRIDE - s.scroll;
			if (x > 320 || x + TILE < 0) continue;
			if (activeShelf && i == s.sel) { focusedIdx = i; continue; } // Drawn last so it sits on top.

			const int ei = s.items[i];
			Card(x, tileY, TILE, TILE, 12);
			Image(E(ei)->GetIcon(), x + TILE / 2, tileY + TILE / 2, 1.0f);
			if (E(ei)->GetUpdateAvl()) Ribbon(x + 8, tileY - 7, 44, "Update", ACCENT);
			else if (this->IsNew(ei)) Ribbon(x + 12, tileY - 7, 36, "New", SKY);
		}

		if (focusedIdx >= 0) {
			const int ei = s.items[focusedIdx];
			const float scale = (1.0f + 0.1f * pop) * (1.0f - 0.08f * this->pressAnim);
			const float size = TILE * scale, grow = (size - TILE) / 2.0f;
			const float x = SHELF_X + focusedIdx * TILE_STRIDE - s.scroll - grow, y = tileY - grow;
			this->DrawFocusRing(x, y, size, size, 13);
			Card(x, y, size, size, 13);
			Image(E(ei)->GetIcon(), x + size / 2, y + size / 2, scale);
			if (E(ei)->GetUpdateAvl()) Ribbon(x + size / 2 - 22, y - 8, 44, "Update", ACCENT);
			else if (this->IsNew(ei)) Ribbon(x + size / 2 - 18, y - 8, 36, "New", SKY);
			if (this->IsInstalled(ei) && !E(ei)->GetUpdateAvl()) {
				Circle(x + size - 4, y + size - 4, 8, WHITE);
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
		case State::Queued: label = "Queued"; fill = SKY_SOFT; ink = SKY; break;
	}
	if (focused && s == State::Get) { fill = ACCENT; ink = WHITE; }
	if (focused) {
		const float sq = 1.0f - 0.1f * this->pressAnim;
		const float nw = w * sq, nh = h * sq;
		x += (w - nw) / 2;
		y += (h - nh) / 2;
		w = nw;
		h = nh;
	}
	Pill(x, y + 2, w, h, s == State::Get && !focused ? CARD_SHADOW : Darken(fill, 0.18f));
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

		if (sel) this->DrawFocusRing(8, y, 304, ROW_H, 12);
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

	/* Overflow is hidden behind a solid strip so lists don't run into the headers. */
	if (y0 > 8) {
		Rect(0, 0, 320, y0 - 4, BG);
		Rect(0, y0 - 5, 320, 1, LINE);
	}
}

void KioskScreen::DrawSearch() const {
	this->DrawList(this->search, ListKind::Search, SEARCH_Y0, TAB_Y);

	const bool fieldSel = this->search.sel < 0;
	if (fieldSel) this->DrawFocusRing(8, 6, 304, 34, 17);
	Pill(8, 8, 304, 34, CARD_SHADOW);
	Pill(8, 6, 304, 34, WHITE);
	Circle(290, 23, 13, ACCENT);
	Sprite(sprites_search_idx, 290, 23, WHITE, 0.6f);
	if (this->query.empty()) Text(22, 14, 0.5f, SUB, "Search titles, authors…");
	else {
		Text(22, 14, 0.5f, TEXT, this->query, 190);
		TextRight(270, 15, 0.42f, SUB, std::to_string(this->search.items.size()) + " found");
	}

	if (this->search.items.empty()) this->DrawEmptyBottom("No matches", "B clears the search.", 64);
}

void KioskScreen::DrawUpdates() const {
	if (this->updates.items.empty()) {
		this->DrawEmptyBottom("No updates right now", "Kiosk checks every time it opens.", 50);
		return;
	}

	this->DrawList(this->updates, ListKind::Updates, UPDATES_Y0, TAB_Y);
	const int n = this->updates.items.size();
	Text(12, 9, 0.56f, TEXT, std::to_string(n) + (n == 1 ? " update" : " updates"));
	Pill(212, 8, 100, 26, Darken(ACCENT, 0.2f));
	Pill(212, 6, 100, 26, ACCENT);
	TextCentered(262, 11, 0.44f, WHITE, "Update all");
}

void KioskScreen::DrawDownloads() const {
	const bool pending = RequestPending();
	float y = DOWNLOAD_Y0;

	if (pending) {
		const float pulse = 0.5f + 0.5f * sinf(this->frame * 0.12f);
		Card(8, y, 304, 54, 12, Lerp(ACCENT, Lighten(ACCENT, 0.2f), pulse), Darken(ACCENT, 0.3f));
		Text(20, y + 7, 0.5f, WHITE, "A download needs your answer");
		Text(20, y + 28, 0.42f, Lighten(ACCENT, 0.75f), "Press A or tap here to respond");
		y += 62;
	}

	if (queueEntries.empty()) {
		this->DrawEmptyBottom("Nothing downloading", "Updates and downloads appear here.", 50);
		return;
	}

	for (int i = 0; i < (int)queueEntries.size(); i++) {
		const QueueEntry &q = queueEntries[i];
		const float h = i == 0 ? ROW_H + 14 : ROW_H;
		if (y > TAB_Y) break;

		if (i == this->downloadSel) this->DrawFocusRing(8, y, 304, h, 12);
		Card(8, y, 304, h, 12);
		Image(q.storeEntry->GetIcon(), 34, y + 24, 0.75f);
		Text(60, y + 7, 0.5f, TEXT, q.storeEntry->GetTitle(), 170);
		Text(60, y + 26, 0.4f, SUB, q.storeEntry->GetScript(q.scriptIndex).GetName(), 170);

		if (i == 0) {
			const float p = this->OverallProgress(q);
			Pill(20, y + h - 13, 280, 6, MUTED);
			if (p > 0.0f) Pill(20, y + h - 13, std::max(6.0f, 280 * p), 6, ACCENT);
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
	const float phase = fmodf(this->frame / 45.0f, 1.0f);
	const float hop = sinf(phase * M_PI);
	C2D_DrawEllipseSolid(160 - 16, y + 50, 0.5f, 32, 6, BG_DOT);
	Bag(160, y + 30 - hop * 10, 38, ACCENT, BG, phase < 0.1f ? 0.1f : 0.0f);
	Card(30, y + 64, 260, 54, 14);
	TextCentered(160, y + 70, 0.54f, TEXT, title, 240);
	TextCentered(160, y + 94, 0.42f, SUB, subtitle, 240);
}

void KioskScreen::DrawDock(float cx, float cy) const {
	const float b = sinf(this->dockBounce * M_PI) * 0.35f;
	const float r = 14 * (1.0f + b);
	Circle(cx, cy + 2, r, CARD_SHADOW);
	Circle(cx, cy, r, WHITE);
	Bag(cx, cy, 16 * (1.0f + b), ACCENT, WHITE);
	if (!queueEntries.empty()) Badge(cx + 11, cy - 10, std::to_string(queueEntries.size()));
}

void KioskScreen::DrawAppPage(float ox) const {
	const auto &e = E(this->appIdx);
	Rect(ox, 0, 320, 240, BG);
	if (ox < 0.5f) Pattern(320, 240, this->frame * 0.15f, BG_DOT); // Pattern only once settled; it isn't offset-aware.
	else Rect(ox - 3, 0, 3, 240, RGBA(0, 0, 0, 30));

	/* Header. */
	Rect(ox, 0, 320, 42, CARD);
	Rect(ox, 42, 320, 1, LINE);
	RoundRect(ox + 8, 7, 28, 28, 7, MUTED);
	Image(e->GetIcon(), ox + 22, 21, 0.55f);
	Text(ox + 44, 6, 0.5f, TEXT, e->GetTitle(), 220);
	Text(ox + 44, 23, 0.38f, SUB, e->GetScripts().size() == 1 ? "Download" : "Choose a download", 220);

	/* Download options. */
	const auto &scripts = e->GetScripts();
	for (int i = 0; i < (int)scripts.size(); i++) {
		const float y = SCRIPT_Y0 + i * SCRIPT_STRIDE - this->appScroll;
		if (y + SCRIPT_ROW_H < SCRIPT_Y0 - 8 || y > SCRIPT_Y1) continue;
		const Script &sc = scripts[i];
		const bool sel = i == this->appSel;

		if (sel) this->DrawFocusRing(ox + 8, y, 304, SCRIPT_ROW_H, 12);
		Card(ox + 8, y, 304, SCRIPT_ROW_H, 12);
		Text(ox + 20, y + 5, 0.48f, TEXT, sc.GetName(), 186);

		std::string sub = sc.IsInstalled() ? "Installed" : (sc.IsPrerelease() ? "Pre-release" : "");
		if (!sc.GetSize().empty()) sub += (sub.empty() ? "" : " · ") + sc.GetSize();
		Text(ox + 20, y + 24, 0.4f, sc.IsInstalled() ? GOOD : SUB, sub, 186);

		State st = State::Get;
		if (this->InQueue(this->appIdx, i)) st = State::Queued;
		else if (sc.IsInstalled()) st = e->GetUpdateAvl() ? State::Update : State::Installed;

		if (st == State::Installed) {
			Pill(ox + 216, y + 11, 88, 26, sel ? Darken(ACCENT, 0.2f) : CARD_SHADOW);
			Pill(ox + 216, y + 9, 88, 26, sel ? ACCENT : MUTED);
			TextCentered(ox + 260, y + 14, 0.42f, sel ? WHITE : SUB, "Reinstall");
		} else {
			this->DrawPillButton(ox + 216, y + 9, 88, 26, st, sel);
		}
	}

	/* Re-cover anything scrolled under the header/footer. */
	if (this->appScroll > 1) {
		Rect(ox, 0, 320, 42, CARD);
		Rect(ox, 42, 320, 1, LINE);
		RoundRect(ox + 8, 7, 28, 28, 7, MUTED);
		Image(e->GetIcon(), ox + 22, 21, 0.55f);
		Text(ox + 44, 6, 0.5f, TEXT, e->GetTitle(), 220);
	}
	Rect(ox, SCRIPT_Y1 + 4, 320, 240 - SCRIPT_Y1 - 4, BG);
	Rect(ox, 198, 320, 42, WHITE);
	Rect(ox, 198, 320, 1, LINE);

	if (!queueEntries.empty() || this->fly.t < 1.0f) this->DrawDock(ox + 296, 22);

	/* Footer: Back bottom-left, then the two extras. */
	const bool hasShots = !e->GetScreenshots().empty(), hasNotes = !e->GetReleaseNotes().empty();
	Pill(ox + 8, 206, 66, 28, MUTED);
	Chevron(ox + 21, 220, 10, true, SUB);
	Text(ox + 30, 211, 0.44f, TEXT, "Back");
	auto footer = [&](float x, float w, const std::string &label, bool enabled, bool active) {
		Pill(ox + x, 208, w, 28, active ? Darken(ACCENT, 0.2f) : CARD_SHADOW);
		Pill(ox + x, 206, w, 28, active ? ACCENT : (enabled ? WHITE : MUTED));
		TextCentered(ox + x + w / 2, 211, 0.42f, active ? WHITE : (enabled ? TEXT : SUB), label);
	};
	footer(80, 114, "Y  Screenshots", hasShots, this->topMode == TopMode::Screenshot);
	footer(200, 112, "X  What's new", hasNotes, this->topMode == TopMode::WhatsNew);
}

void KioskScreen::DrawFly() const {
	if (this->fly.t >= 1.0f || !this->fly.icon.tex) return;
	const float k = EaseOutCubic(this->fly.t);
	const float x = this->fly.sx + (this->fly.ex - this->fly.sx) * k;
	const float y = this->fly.sy + (this->fly.ey - this->fly.sy) * k - sinf(this->fly.t * M_PI) * 70;
	const float s = 1.0f - 0.55f * this->fly.t;
	/* A few trailing sparkles. */
	for (int i = 1; i <= 3; i++) {
		const float tt = std::max(0.0f, this->fly.t - i * 0.06f);
		const float kk = EaseOutCubic(tt);
		const float tx = this->fly.sx + (this->fly.ex - this->fly.sx) * kk;
		const float ty = this->fly.sy + (this->fly.ey - this->fly.sy) * kk - sinf(tt * M_PI) * 70;
		Sparkle(tx, ty, 9 - i * 2, this->frame * 0.2f, i % 2 ? RGBA(255, 210, 80) : ACCENT);
	}
	RoundRect(x - 26 * s, y - 26 * s, 52 * s, 52 * s, 10 * s, WHITE);
	Image(this->fly.icon, x, y, s);
}

#ifdef KIOSK_DEMO
/*
	Screenshot tour (built only with -DKIOSK_DEMO): steps through every screen on a timer
	so an emulator can capture them without input. Fakes a download and some app states.
*/
void KioskScreen::DemoStep() {
	static constexpr int PHASE = 300;
	if (!this->storeOk || this->shelves.empty()) return;
	const int phase = (this->frame / PHASE) % 10, t = this->frame % PHASE;

	if (phase != this->demoPhase) {
		this->demoPhase = phase;
		/* Reset to a clean state each phase. */
		this->celebrating = false;
		if (this->appOpen) { this->appOpen = false; this->appAnim = 0; }
		queueEntries.clear();
		QueueRuns = false;
		this->tab = TAB_HOME;

		/* Pretend a few apps are installed, some with updates. */
		for (int i = 0; i < 5 && i < (int)this->allSorted.size(); i++) {
			const int ei = this->allSorted[(i * 7 + 3) % this->allSorted.size()];
			E(ei)->SetInstalled(true);
			E(ei)->SetUpdateAvl(i < 3);
		}

		const int pick = this->shelves[0].items[std::min<int>(2, this->shelves[0].items.size() - 1)];
		switch (phase) {
			case 0: this->shelfSel = 0; this->shelves[0].sel = 1; break;
			case 1: this->shelfSel = std::min<int>(2, this->shelves.size() - 1); this->shelves[this->shelfSel].sel = 3; break;
			case 2: this->OpenApp(pick); break;
			case 3: this->OpenApp(pick); this->topMode = TopMode::WhatsNew; break;
			case 4: this->tab = TAB_SEARCH; this->query = "emu"; this->RunSearch(); this->search.sel = 1; break;
			case 5: this->tab = TAB_UPDATES; break;
			case 6:
			case 7:
				this->tab = TAB_DOWNLOADS;
				queueEntries.emplace_back(E(pick), 0);
				if (this->allSorted.size() > 10) queueEntries.emplace_back(E(this->allSorted[10]), 0);
				queueEntries[0].status = QueueStatus::Downloading;
				queueEntries[0].currentStep = 1;
				QueueRuns = true;
				downloadTotal = 4 * 1024 * 1024;
				break;
			case 8:
				this->celebrating = true;
				this->celebT = 0;
				this->celebEntry = E(pick);
				this->confetti.clear();
				break;
			case 9: this->tab = TAB_LIBRARY; this->library.sel = 1; break;
		}
	}

	/* Live bits inside a phase. */
	if ((phase == 6 || phase == 7) && !queueEntries.empty()) {
		const float p = (phase == 6 ? 0.0f : 0.5f) + 0.5f * t / PHASE;
		const int steps = std::max(1, (int)queueEntries[0].storeEntry->GetScript(0).GetActions().size());
		queueEntries[0].currentStep = 1 + (int)(p * (steps - 1) + 0.5f);
		downloadNow = (curl_off_t)(downloadTotal * fmodf(p * steps, 1.0f));
	}
	if (phase == 0 && t == PHASE / 2) this->shelves[0].sel = 2;
}
#endif
