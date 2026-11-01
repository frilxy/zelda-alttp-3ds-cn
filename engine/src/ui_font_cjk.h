// Chinese text on the port's own screens (the bottom screen, and later the
// setup views).
//
// The Latin and Cyrillic letters are 5x11/7x15/10x21 pixel patterns; Chinese
// needs real glyphs, so tools/ui/generate_cjk_glyphs.py bakes the characters
// the UI strings use at the three sizes those scales draw, and this module
// answers the questions the text routines ask: how many pixels a character
// takes, and where its rows are.
#ifndef ZELDA3_UI_FONT_CJK_H_
#define ZELDA3_UI_FONT_CJK_H_

#include <stdint.h>

// Decodes one UTF-8 codepoint and advances *text past it. Invalid bytes give
// '?' and advance by one.
uint32_t UiFont_DecodeUtf8(const char **text);

// How many characters the tables carry.
int UiCjk_GlyphCount(void);

// Index into the generated tables, or -1 when the codepoint has no glyph.
int UiCjk_GlyphIndex(uint32_t codepoint);

// Glyph size the port's own screens use for a text scale (1..3).
int UiCjk_SizeForScale(int scale);

// Rows of one glyph, `size` of them, most significant bit leftmost. NULL when
// the index or size is unknown.
const uint16_t *UiCjk_GlyphRows(int index, int size);

// Pixels a character takes at `scale`, or 0 when it is not Chinese.
int UiCjk_WidthFor(uint32_t codepoint, int scale);

#endif  // ZELDA3_UI_FONT_CJK_H_
