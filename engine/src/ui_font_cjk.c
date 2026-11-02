#include "ui_font_cjk.h"

#include "ui_glyphs_cjk.h"

uint32_t UiFont_DecodeUtf8(const char **text) {
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

int UiCjk_GlyphCount(void) {
  return kUiCjkGlyphCount;
}

int UiCjk_Index(uint32_t codepoint) {
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

bool UiCjk_HasGlyph(uint32_t codepoint) {
  return UiCjk_Index(codepoint) >= 0;
}

int UiCjk_SizeForScale(int scale) {
  // The bottom screen draws Latin at 11, 21 and 15 pixels for scales 1..3; the
  // Chinese glyphs closest to those are 10, 16 and 12.
  return scale == 2 ? 16 : scale == 3 ? 12 : 10;
}

const uint16_t *UiCjk_Rows(int index, int size) {
  if (index < 0 || index >= kUiCjkGlyphCount)
    return 0;
  switch (size) {
  case 16: return kUiCjk16[index];
  case 12: return kUiCjk12[index];
  case 10: return kUiCjk10[index];
  default: return 0;
  }
}

const uint16_t *UiCjk_RowsFor(uint32_t codepoint) {
  int index = UiCjk_Index(codepoint);
  return index < 0 ? 0 : kUiCjk16[index];
}

int UiCjk_TopOffset(int scale, int size) {
  int latin = scale == 3 ? 15 : scale == 2 ? 21 : 11;
  return -2 * scale + (latin - size) / 2;
}

int UiCjk_TopOffsetFor(int latin_top, int latin_rows, int size) {
  return latin_top + (latin_rows - size) / 2;
}

int UiCjk_WidthFor(uint32_t codepoint, int scale) {
  if (!UiCjk_HasGlyph(codepoint))
    return 0;
  return UiCjk_SizeForScale(scale);
}
