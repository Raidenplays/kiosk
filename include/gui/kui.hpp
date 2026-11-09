/*
	Kiosk UI kit — drawing helpers, palette and easing for the store-style interface.
	Everything here is drawn with citro2d primitives so no extra art is needed.
*/
#ifndef _KIOSK_KUI_HPP
#define _KIOSK_KUI_HPP

#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>

namespace KUI {
	constexpr u32 RGBA(u8 r, u8 g, u8 b, u8 a = 255) { return (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((u32)a << 24); }

	/* Palette: warm cream base, orange cursor/accent, sky-blue secondary. */
	constexpr u32 BG          = RGBA(248, 243, 229);
	constexpr u32 BG_DOT      = RGBA(240, 233, 214);
	constexpr u32 CARD        = RGBA(255, 255, 255);
	constexpr u32 CARD_SHADOW = RGBA(226, 216, 192);
	constexpr u32 TEXT        = RGBA(58, 52, 44);
	constexpr u32 SUB         = RGBA(140, 128, 110);
	constexpr u32 LINE        = RGBA(232, 222, 198);
	constexpr u32 ACCENT      = RGBA(247, 126, 20);
	constexpr u32 ACCENT_SOFT = RGBA(255, 230, 200);
	constexpr u32 SKY         = RGBA(74, 182, 236);
	constexpr u32 SKY_SOFT    = RGBA(214, 240, 252);
	constexpr u32 BADGE       = RGBA(232, 64, 60);
	constexpr u32 GOOD        = RGBA(64, 176, 84);
	constexpr u32 MUTED       = RGBA(238, 230, 212);
	#ifndef WHITE
	#define WHITE C2D_Color32(255, 255, 255, 255)
	#endif

	/* Colour maths. */
	u32 Lerp(u32 a, u32 b, float t);
	u32 Darken(u32 c, float amount);
	u32 Lighten(u32 c, float amount);
	u32 Alpha(u32 c, u8 a);
	u32 AccentFor(u32 storeColor, const std::string &seed); // Fallback accent when an entry has no colour.

	/* Easing. */
	float Approach(float cur, float target, float speed = 0.22f);
	float EaseOutBack(float t);
	float EaseOutCubic(float t);

	/* Shapes. Rounded shapes assume an opaque colour. */
	void Rect(float x, float y, float w, float h, u32 c);
	void Gradient(float x, float y, float w, float h, u32 top, u32 bottom);
	void RoundRect(float x, float y, float w, float h, float r, u32 c);
	void Card(float x, float y, float w, float h, float r, u32 c = CARD, u32 shadow = CARD_SHADOW);
	void Pill(float x, float y, float w, float h, u32 c);
	void Circle(float cx, float cy, float r, u32 c);
	void Ring(float cx, float cy, float r, float thick, float from, float to, u32 c); // from/to in 0..1 of a turn
	void Check(float cx, float cy, float s, u32 c);
	void Chevron(float cx, float cy, float s, bool left, u32 c);
	void House(float cx, float cy, float s, u32 c);
	void Star(float cx, float cy, float r, u32 c);
	void Tile(float cx, float cy, float s, float t, u32 c); // Small rounded square (pattern/confetti).
	void Pattern(float w, float h, float t, u32 c);      // Drifting rounded-square wallpaper.
	void Ribbon(float x, float y, float w, const std::string &label, u32 fill);
	void Bag(float cx, float cy, float s, u32 body, u32 cutout, float squash = 0.0f);

	/* Text. */
	float TextWidth(float size, const std::string &s);
	std::string Ellipsize(const std::string &s, float size, float maxW);
	void Text(float x, float y, float size, u32 c, const std::string &s, float maxW = 0);
	void TextCentered(float cx, float y, float size, u32 c, const std::string &s, float maxW = 0);
	void TextRight(float rx, float y, float size, u32 c, const std::string &s, float maxW = 0);
	const std::vector<std::string> &Wrap(const std::string &s, float size, float maxW);
	void Paragraph(float x, float y, float size, u32 c, const std::string &s, float maxW, int maxLines, float lineH, int skipLines = 0);

	/* Images. */
	void Image(const C2D_Image &img, float cx, float cy, float scale = 1.0f);
	void Sprite(int idx, float cx, float cy, u32 tint, float scale = 1.0f);
	void Badge(float cx, float cy, const std::string &label);
};

#endif
