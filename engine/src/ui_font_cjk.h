// Chinese text on the port's own screens (the bottom screen and the setup
// views).
//
// The Latin and Cyrillic letters are small hand-authored patterns; Chinese
// needs real glyphs, so tools/ui/generate_cjk_glyphs.py bakes exactly the
// characters the port's strings use, at the three sizes its screens draw
// (10 and 12 px Fusion Pixel, 16 px GNU Unifont).
#ifndef ZELDA3_UI_FONT_CJK_H_
#define ZELDA3_UI_FONT_CJK_H_

#include <stdbool.h>
#include <stdint.h>

// Decodes one UTF-8 codepoint and advances *text past it. Invalid bytes give
// '?' and advance by one.
uint32_t UiFont_DecodeUtf8(const char **text);

// Characters the tables carry.
int UiCjk_GlyphCount(void);
bool UiCjk_HasGlyph(uint32_t codepoint);

// Index of a character in the tables, or -1.
int UiCjk_Index(uint32_t codepoint);

// Glyph size the bottom screen uses for a text scale (1..3).
int UiCjk_SizeForScale(int scale);

// Rows of one glyph, `size` of them, most significant bit leftmost, or NULL
// when the index or size is unknown.
const uint16_t *UiCjk_Rows(int index, int size);

// The 16x16 rows of a character, or NULL: what the setup views draw.
const uint16_t *UiCjk_RowsFor(uint32_t codepoint);

// Vertical offset from a text line's y to the top of a `size`-pixel Chinese
// glyph, centring the square in the Latin line box of that scale.
int UiCjk_TopOffset(int scale, int size);

// Pixels a character takes at `scale`, or 0 when it has no glyph.
int UiCjk_WidthFor(uint32_t codepoint, int scale);

#endif  // ZELDA3_UI_FONT_CJK_H_
