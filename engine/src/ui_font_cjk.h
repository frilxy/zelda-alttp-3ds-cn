// Chinese text on the port's own screens (the bottom screen and the setup
// views).
//
// The Latin and Cyrillic letters are small hand-authored patterns; Chinese
// needs real glyphs, so tools/ui/generate_cjk_glyphs.py bakes two sets:
// GB2312 level 1 at 16x16 for the screens that draw one size (and for text
// fetched at runtime, like the update notes), and the characters the UI
// strings use at the three sizes the bottom screen draws.
#ifndef ZELDA3_UI_FONT_CJK_H_
#define ZELDA3_UI_FONT_CJK_H_

#include <stdbool.h>
#include <stdint.h>

// Decodes one UTF-8 codepoint and advances *text past it. Invalid bytes give
// '?' and advance by one.
uint32_t UiFont_DecodeUtf8(const char **text);

// Characters the wide (16x16) set carries, and whether one is there.
int UiCjk_GlyphCount(void);
bool UiCjk_HasGlyph(uint32_t codepoint);

// Characters the bottom screen can draw, and one's index in its tables.
int UiCjk_SmallCount(void);
int UiCjk_SmallIndex(uint32_t codepoint);

// Glyph size the bottom screen uses for a text scale (1..3).
int UiCjk_SizeForScale(int scale);

// Rows of a bottom-screen glyph, `size` of them, most significant bit
// leftmost, or NULL when the index or size is unknown.
const uint16_t *UiCjk_SmallRows(int index, int size);

// Rows of any character at 16x16, or NULL when it has no glyph.
const uint16_t *UiCjk_WideRows(uint32_t codepoint);

// Pixels a character takes at `scale` on the bottom screen, or 0 when it is
// not one of the characters that screen draws.
int UiCjk_WidthFor(uint32_t codepoint, int scale);

// Vertical offset from a text line's y to the top of a `size`-pixel Chinese
// glyph, centring the square in the Latin line box of that scale. Both the
// bottom screen and the setup views use it, so a label sits the same way in
// either.
int UiCjk_TopOffset(int scale, int size);

#endif  // ZELDA3_UI_FONT_CJK_H_
