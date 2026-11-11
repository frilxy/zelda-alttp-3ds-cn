// Chinese text on the port's own screens (the bottom screen and the setup
// views).
//
// The Latin and Cyrillic letters are small hand-authored patterns; Chinese
// needs real glyphs, so tools/ui/generate_cjk_glyphs.py bakes exactly the
// characters the port's strings use, at the three sizes its screens draw
// (10 and 12 px Fusion Pixel, 16 px GNU Unifont). Everything here is inline
// over those tables: the drawers and the tests need no extra translation unit.
#ifndef ZELDA3_UI_FONT_CJK_H_
#define ZELDA3_UI_FONT_CJK_H_

#include <stdbool.h>
#include <stdint.h>

#include "ui_glyphs_cjk.h"

// Decodes one UTF-8 codepoint and advances *text past it. Invalid bytes give
// '?' and advance by one.
static inline uint32_t UiFont_DecodeUtf8(const char **text) {
  const unsigned char *p = (const unsigned char *)*text;
  if (p[0] < 0x80) {
    (*text)++;
    return p[0];
  }
  if ((p[0] & 0xe0) == 0xc0) {
    *text += 2;
    return (uint32_t)(((p[0] & 0x1f) << 6) | (p[1] & 0x3f));
  }
  if ((p[0] & 0xf0) == 0xe0) {
    *text += 3;
    return (uint32_t)(((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f));
  }
  (*text)++;
  return '?';
}

// Index of a character in the tables, or -1.
static inline int UiCjk_Index(uint32_t codepoint) {
  if (codepoint < kUiCjkFirstCodepoint)
    return -1;
  int lo = 0, hi = kUiCjkGlyphCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t value = kUiCjkCodepoints[mid];
    if (value == codepoint)
      return mid;
    if (value < codepoint)
      lo = mid + 1;
    else
      hi = mid - 1;
  }
  return -1;
}

static inline bool UiCjk_HasGlyph(uint32_t codepoint) {
  return UiCjk_Index(codepoint) >= 0;
}

// The bottom screen draws Latin at 11, 21 and 15 pixels for scales 1..3; the
// Chinese glyphs closest to those are 10, 16 and 12.
static inline int UiCjk_SizeForScale(int scale) {
  return scale == 2 ? 16 : scale == 3 ? 12 : 10;
}

// Rows of one glyph, `size` of them, most significant bit leftmost, or NULL
// when the index or size is unknown.
static inline const uint16_t *UiCjk_Rows(int index, int size) {
  if (index < 0 || index >= kUiCjkGlyphCount)
    return 0;
  switch (size) {
  case 16: return kUiCjk16[index];
  case 12: return kUiCjk12[index];
  case 10: return kUiCjk10[index];
  default: return 0;
  }
}

// The 16x16 rows of a character, or NULL: what the setup views draw.
static inline const uint16_t *UiCjk_RowsFor(uint32_t codepoint) {
  int index = UiCjk_Index(codepoint);
  return index < 0 ? 0 : kUiCjk16[index];
}

// Vertical offset from a text line's y to the top of a `size`-pixel Chinese
// glyph, centring the square in the Latin line box of that scale.
static inline int UiCjk_TopOffset(int scale, int size) {
  int latin = scale == 3 ? 15 : scale == 2 ? 21 : 11;
  return -2 * scale + (latin - size) / 2;
}

// Same question for a caller that knows where its Latin ink starts and how
// tall it is: the setup views start it at -3 (scale 3) rather than the bottom
// screen's -2*scale, so one shared formula cannot serve both.
static inline int UiCjk_TopOffsetFor(int latin_top, int latin_rows, int size) {
  return latin_top + (latin_rows - size) / 2;
}

// Pixels a character takes at `scale`, or 0 when it has no glyph.
static inline int UiCjk_WidthFor(uint32_t codepoint, int scale) {
  if (!UiCjk_HasGlyph(codepoint))
    return 0;
  return UiCjk_SizeForScale(scale);
}

#endif  // ZELDA3_UI_FONT_CJK_H_
