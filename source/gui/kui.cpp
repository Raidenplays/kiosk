#include "kui.hpp"
#include "common.hpp"
#include <algorithm>
#include <cmath>
#include <unordered_map>

static inline u8 R(u32 c) { return c & 0xFF; }
static inline u8 G(u32 c) { return (c >> 8) & 0xFF; }
static inline u8 B(u32 c) { return (c >> 16) & 0xFF; }
static inline u8 A(u32 c) { return (c >> 24) & 0xFF; }

static constexpr float DEPTH = 0.5f;

u32 KUI::Lerp(u32 a, u32 b, float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	auto mix = [t](u8 x, u8 y) { return (u8)(x + (y - x) * t + 0.5f); };
	return RGBA(mix(R(a), R(b)), mix(G(a), G(b)), mix(B(a), B(b)), mix(A(a), A(b)));
}

u32 KUI::Darken(u32 c, float amount) { return Lerp(c, RGBA(0, 0, 0, A(c)), amount); }
u32 KUI::Lighten(u32 c, float amount) { return Lerp(c, RGBA(255, 255, 255, A(c)), amount); }
u32 KUI::Alpha(u32 c, u8 a) { return (c & 0x00FFFFFF) | ((u32)a << 24); }

u32 KUI::AccentFor(u32 storeColor, const std::string &seed) {
	if (storeColor != 0) {
		/* Very light or very dark colours make white text unreadable, so pull them toward the middle. */
		const float lum = (0.299f * R(storeColor) + 0.587f * G(storeColor) + 0.114f * B(storeColor)) / 255.0f;
		if (lum > 0.72f) return Darken(storeColor, (lum - 0.6f));
		if (lum < 0.12f) return Lighten(storeColor, 0.18f);
		return storeColor;
	}

	static const u32 palette[] = {
		RGBA(255, 106, 19), RGBA(43, 124, 255), RGBA(124, 77, 255), RGBA(0, 166, 147),
		RGBA(232, 67, 147), RGBA(46, 160, 67), RGBA(240, 151, 0), RGBA(80, 96, 230)
	};
	u32 h = 2166136261u;
	for (char ch : seed) h = (h ^ (u8)ch) * 16777619u;
	return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

float KUI::Approach(float cur, float target, float speed) {
	const float d = target - cur;
	if (std::fabs(d) < 0.3f && std::fabs(target) > 1.0f) return target;
	if (std::fabs(d) < 0.001f) return target;
	return cur + d * speed;
}

float KUI::EaseOutBack(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	const float c1 = 1.70158f, c3 = c1 + 1.0f;
	return 1.0f + c3 * powf(t - 1.0f, 3) + c1 * powf(t - 1.0f, 2);
}

float KUI::EaseOutCubic(float t) {
	t = std::clamp(t, 0.0f, 1.0f);
	return 1.0f - powf(1.0f - t, 3);
}

void KUI::Rect(float x, float y, float w, float h, u32 c) {
	if (w <= 0 || h <= 0) return;
	C2D_DrawRectSolid(x, y, DEPTH, w, h, c);
}

void KUI::Gradient(float x, float y, float w, float h, u32 top, u32 bottom) {
	C2D_DrawRectangle(x, y, DEPTH, w, h, top, top, bottom, bottom);
}

void KUI::RoundRect(float x, float y, float w, float h, float r, u32 c) {
	r = std::min({ r, w / 2.0f, h / 2.0f });
	if (r < 1.0f) { Rect(x, y, w, h, c); return; }

	C2D_DrawRectSolid(x + r, y, DEPTH, w - 2 * r, h, c);
	C2D_DrawRectSolid(x, y + r, DEPTH, r, h - 2 * r, c);
	C2D_DrawRectSolid(x + w - r, y + r, DEPTH, r, h - 2 * r, c);
	C2D_DrawCircleSolid(x + r, y + r, DEPTH, r, c);
	C2D_DrawCircleSolid(x + w - r, y + r, DEPTH, r, c);
	C2D_DrawCircleSolid(x + r, y + h - r, DEPTH, r, c);
	C2D_DrawCircleSolid(x + w - r, y + h - r, DEPTH, r, c);
}

void KUI::Card(float x, float y, float w, float h, float r, u32 c, u32 shadow) {
	RoundRect(x, y + 2, w, h, r, shadow);
	RoundRect(x, y, w, h, r, c);
}

void KUI::Pill(float x, float y, float w, float h, u32 c) { RoundRect(x, y, w, h, h / 2.0f, c); }

void KUI::Circle(float cx, float cy, float r, u32 c) { C2D_DrawCircleSolid(cx, cy, DEPTH, r, c); }

void KUI::Ring(float cx, float cy, float r, float thick, float from, float to, u32 c) {
	if (to < from) std::swap(from, to);
	const float span = std::min(to - from, 1.0f);
	if (span <= 0.0f) return;

	const int segs = std::max(2, (int)(64 * span));
	const float start = from * 2.0f * M_PI - M_PI / 2.0f;
	const float step = span * 2.0f * M_PI / segs;
	const float ri = r - thick;

	for (int i = 0; i < segs; i++) {
		const float a0 = start + i * step, a1 = a0 + step;
		const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
		C2D_DrawTriangle(cx + c0 * r, cy + s0 * r, c, cx + c1 * r, cy + s1 * r, c, cx + c0 * ri, cy + s0 * ri, c, DEPTH);
		C2D_DrawTriangle(cx + c1 * r, cy + s1 * r, c, cx + c1 * ri, cy + s1 * ri, c, cx + c0 * ri, cy + s0 * ri, c, DEPTH);
	}

	/* Round caps. */
	const float mid = r - thick / 2.0f;
	Circle(cx + cosf(start) * mid, cy + sinf(start) * mid, thick / 2.0f, c);
	const float end = start + span * 2.0f * M_PI;
	Circle(cx + cosf(end) * mid, cy + sinf(end) * mid, thick / 2.0f, c);
}

/* A thick tick mark built from two quads. */
static void Thick(float x0, float y0, float x1, float y1, float t, u32 c) {
	const float dx = x1 - x0, dy = y1 - y0;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len <= 0) return;
	const float nx = -dy / len * t / 2.0f, ny = dx / len * t / 2.0f;
	C2D_DrawTriangle(x0 + nx, y0 + ny, c, x1 + nx, y1 + ny, c, x0 - nx, y0 - ny, c, DEPTH);
	C2D_DrawTriangle(x1 + nx, y1 + ny, c, x1 - nx, y1 - ny, c, x0 - nx, y0 - ny, c, DEPTH);
	C2D_DrawCircleSolid(x0, y0, DEPTH, t / 2.0f, c);
	C2D_DrawCircleSolid(x1, y1, DEPTH, t / 2.0f, c);
}

void KUI::Check(float cx, float cy, float s, u32 c) {
	const float t = std::max(1.5f, s * 0.22f);
	Thick(cx - s * 0.45f, cy + s * 0.02f, cx - s * 0.12f, cy + s * 0.34f, t, c);
	Thick(cx - s * 0.12f, cy + s * 0.34f, cx + s * 0.48f, cy - s * 0.3f, t, c);
}

void KUI::Chevron(float cx, float cy, float s, bool left, u32 c) {
	const float d = left ? 1.0f : -1.0f;
	const float t = std::max(1.5f, s * 0.25f);
	Thick(cx + d * s * 0.25f, cy - s * 0.5f, cx - d * s * 0.25f, cy, t, c);
	Thick(cx - d * s * 0.25f, cy, cx + d * s * 0.25f, cy + s * 0.5f, t, c);
}

void KUI::House(float cx, float cy, float s, u32 c) {
	const float h = s / 2.0f;
	C2D_DrawTriangle(cx - h - 1, cy - 1, c, cx + h + 1, cy - 1, c, cx, cy - h - 1, c, DEPTH);
	Rect(cx - h * 0.7f, cy - 1, h * 1.4f, h, c);
}

float KUI::TextWidth(float size, const std::string &s) {
	if (s.empty()) return 0.0f;
	return Gui::GetStringWidth(size, s, font);
}

std::string KUI::Ellipsize(const std::string &s, float size, float maxW) {
	if (maxW <= 0 || TextWidth(size, s) <= maxW) return s;

	const float dots = TextWidth(size, "…");
	size_t lo = 0, hi = s.size();
	/* Binary search the longest prefix that fits, then back off to a UTF-8 boundary. */
	while (lo < hi) {
		size_t mid = (lo + hi + 1) / 2;
		while (mid > 0 && mid < s.size() && ((u8)s[mid] & 0xC0) == 0x80) mid--;
		if (mid <= lo) break;
		if (TextWidth(size, s.substr(0, mid)) + dots <= maxW) lo = mid;
		else hi = mid - 1;
	}
	while (lo > 0 && lo < s.size() && ((u8)s[lo] & 0xC0) == 0x80) lo--;
	std::string out = s.substr(0, lo);
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out + "…";
}

void KUI::Text(float x, float y, float size, u32 c, const std::string &s, float maxW) {
	if (s.empty()) return;
	Gui::DrawString(x, y, size, c, maxW > 0 ? Ellipsize(s, size, maxW) : s, 0, 0, font);
}

void KUI::TextCentered(float cx, float y, float size, u32 c, const std::string &s, float maxW) {
	if (s.empty()) return;
	const std::string t = maxW > 0 ? Ellipsize(s, size, maxW) : s;
	Gui::DrawString(cx - TextWidth(size, t) / 2.0f, y, size, c, t, 0, 0, font);
}

void KUI::TextRight(float rx, float y, float size, u32 c, const std::string &s, float maxW) {
	if (s.empty()) return;
	const std::string t = maxW > 0 ? Ellipsize(s, size, maxW) : s;
	Gui::DrawString(rx - TextWidth(size, t), y, size, c, t, 0, 0, font);
}

const std::vector<std::string> &KUI::Wrap(const std::string &s, float size, float maxW) {
	static std::unordered_map<std::string, std::vector<std::string>> cache;
	const std::string key = s + '\x01' + std::to_string((int)(size * 1000)) + '\x01' + std::to_string((int)maxW);

	auto found = cache.find(key);
	if (found != cache.end()) return found->second;
	if (cache.size() > 48) cache.clear();

	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= s.size()) {
		size_t nl = s.find('\n', start);
		std::string para = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
		if (!para.empty() && para.back() == '\r') para.pop_back();

		std::string line;
		size_t p = 0;
		while (p <= para.size()) {
			size_t sp = para.find(' ', p);
			std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
			std::string trial = line.empty() ? word : line + " " + word;

			if (!line.empty() && TextWidth(size, trial) > maxW) {
				lines.push_back(line);
				line = word;
			} else {
				line = trial;
			}

			/* A single word wider than the box gets cut. */
			if (TextWidth(size, line) > maxW) {
				lines.push_back(Ellipsize(line, size, maxW));
				line.clear();
			}

			if (sp == std::string::npos) break;
			p = sp + 1;
		}
		lines.push_back(line);

		if (nl == std::string::npos) break;
		start = nl + 1;
	}

	/* Drop trailing blank lines. */
	while (!lines.empty() && lines.back().empty()) lines.pop_back();

	return cache.emplace(key, std::move(lines)).first->second;
}

void KUI::Paragraph(float x, float y, float size, u32 c, const std::string &s, float maxW, int maxLines, float lineH, int skipLines) {
	const std::vector<std::string> &lines = Wrap(s, size, maxW);
	int drawn = 0;
	for (int i = std::max(0, skipLines); i < (int)lines.size() && drawn < maxLines; i++, drawn++) {
		std::string line = lines[i];
		/* Last visible line with more after it gets an ellipsis. */
		if (drawn == maxLines - 1 && i < (int)lines.size() - 1) line = Ellipsize(line + "…", size, maxW);
		if (!line.empty()) Gui::DrawString(x, y + drawn * lineH, size, c, line, 0, 0, font);
	}
}

void KUI::Image(const C2D_Image &img, float cx, float cy, float scale) {
	if (!img.tex || !img.subtex) return;
	const float w = img.subtex->width * scale, h = img.subtex->height * scale;
	C2D_DrawImageAt(img, cx - w / 2.0f, cy - h / 2.0f, DEPTH, nullptr, scale, scale);
}

void KUI::Sprite(int idx, float cx, float cy, u32 tint, float scale) {
	if (!sprites) return;
	const C2D_Image img = C2D_SpriteSheetGetImage(sprites, idx);
	if (!img.subtex) return;
	C2D_ImageTint t;
	C2D_PlainImageTint(&t, tint, 1.0f);
	const float w = img.subtex->width * scale, h = img.subtex->height * scale;
	C2D_DrawImageAt(img, cx - w / 2.0f, cy - h / 2.0f, DEPTH, &t, scale, scale);
}

void KUI::Badge(float cx, float cy, const std::string &label) {
	const float size = 0.38f;
	const float w = std::max(14.0f, TextWidth(size, label) + 8.0f);
	Pill(cx - w / 2.0f - 1, cy - 8, w + 2, 16, WHITE);
	Pill(cx - w / 2.0f, cy - 7, w, 14, BADGE);
	TextCentered(cx, cy - 7, size, WHITE, label);
}
