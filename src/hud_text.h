#pragma once

#include "entity_frame.h"

#include <cstdint>
#include <string>

namespace haven {

// ---------------------------------------------------------------------------
// A 5x7 bitmap font baked into the client. The Ogre SDK ships no font files
// and the design allows no art assets, so every HUD string — XP bar label,
// item tray, level-up cards, scoreboard — is drawn as one quad per lit pixel
// through the same immediate-mode PrimitiveSink the combat batches use.
// ---------------------------------------------------------------------------

/// Glyph advance is 6 pixels (5 wide plus 1 space); a line of n characters
/// is 6n - 1 pixels wide.
constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kGlyphAdvance = 6;

/// Width of `text` in font pixels.
int text_pixel_width(const std::string& text);

/// Number of lit pixels in `text`: the exact vertex budget for one text line
/// (4 vertices and 6 indices per lit pixel quad).
int text_lit_pixels(const std::string& text);

/// True when the font has a real glyph for `c` (A-Z, 0-9 and the punctuation
/// the HUD uses); anything else draws as a space.
bool font_has_glyph(char c);

/// Appends one quad per lit pixel of each glyph into `sink`, left to right,
/// with the first glyph's left edge at `top_left.x` and the glyph row's top
/// at `top_left.y`. `pixel` is the edge length of one font pixel in world
/// units. Quads are counter-clockwise, like every other builder.
void append_text(PrimitiveSink& sink, const std::string& text, Vec2 top_left, float pixel);

/// The pixel size that fits `text` into `max_width` world units at the given
/// top_left, without shrinking below `min_pixel`. Used to auto-fit long card
/// titles.
float fit_text_pixel(const std::string& text, float max_width, float top_left_x, float min_pixel);

}  // namespace haven
