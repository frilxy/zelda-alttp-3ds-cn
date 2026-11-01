#pragma once
#include <stdint.h>
#include "generated_ui_sprites.h"
#include "ui_font_cjk.h"

// Deterministic Latin and Cyrillic UI glyphs. Decode once per character; accented letters
// use the same editable 5x7 base and an explicit accent, never byte fragments.
enum UiFontAccent { UI_ACUTE, UI_GRAVE, UI_CEDILLA, UI_CIRCUMFLEX, UI_TILDE, UI_DIAERESIS };
static inline char UiFontNext(const char **text, int *accent) {
  const unsigned char *p = (const unsigned char *)*text;
  *accent = -1;
  if (!*p) return 0;
  (*text)++;
  if (*p < 128) return *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
  if ((*p == 0xd0 || *p == 0xd1) && p[1] >= 0x80 && p[1] <= 0xbf) {
    unsigned cp = ((*p & 31) << 6) | (p[1] & 63);
    (*text)++;
    if (cp == 0x401 || cp == 0x451) return (char)160;
    if (cp >= 0x430 && cp <= 0x44f) cp -= 32;
    if (cp >= 0x410 && cp <= 0x42f) return (char)(128 + cp - 0x410);
    return '?';
  }
  if (*p == 0xc3 && p[1] >= 0x80 && p[1] <= 0xbf) {
    (*text)++;
    unsigned cp = p[1] + 0x40;
    if (cp >= 0xe0 && cp <= 0xfe && cp != 0xf7) cp -= 32;
    switch (cp) {
    case 0xc0: *accent=UI_GRAVE; return 'A';
    case 0xc1: *accent=UI_ACUTE; return 'A';
    case 0xc2: *accent=UI_CIRCUMFLEX; return 'A';
    case 0xc3: *accent=UI_TILDE; return 'A';
    case 0xc4: *accent=UI_DIAERESIS; return 'A';
    case 0xc7: *accent=UI_CEDILLA; return 'C';
    case 0xc8: *accent=UI_GRAVE; return 'E';
    case 0xc9: *accent=UI_ACUTE; return 'E';
    case 0xca: *accent=UI_CIRCUMFLEX; return 'E';
    case 0xcb: *accent=UI_DIAERESIS; return 'E';
    case 0xcc: *accent=UI_GRAVE; return 'I';
    case 0xcd: *accent=UI_ACUTE; return 'I';
    case 0xce: *accent=UI_CIRCUMFLEX; return 'I';
    case 0xcf: *accent=UI_DIAERESIS; return 'I';
    case 0xd1: *accent=UI_TILDE; return 'N';
    case 0xd2: *accent=UI_GRAVE; return 'O';
    case 0xd3: *accent=UI_ACUTE; return 'O';
    case 0xd4: *accent=UI_CIRCUMFLEX; return 'O';
    case 0xd5: *accent=UI_TILDE; return 'O';
    case 0xd6: *accent=UI_DIAERESIS; return 'O';
    case 0xd9: *accent=UI_GRAVE; return 'U';
    case 0xda: *accent=UI_ACUTE; return 'U';
    case 0xdb: *accent=UI_CIRCUMFLEX; return 'U';
    case 0xdc: *accent=UI_DIAERESIS; return 'U';
    default: return '?';
    }
  }
  unsigned remaining = *p >= 0xf0 && *p <= 0xf4 ? 3 : *p >= 0xe0 && *p <= 0xef ? 2 : *p >= 0xc2 && *p <= 0xdf ? 1 : 0;
  while (remaining-- && **text && ((unsigned char)**text & 0xc0) == 0x80) (*text)++;
  return '?';
}

static inline int UiFontWidth(const char *text, int scale) {
  int width=0, accent;
  while (*text) {
    const char *before=text;
    uint32_t codepoint=UiFont_DecodeUtf8(&text);
    int cjk=UiCjk_WidthFor(codepoint, scale);
    if (cjk>0) { width+=cjk; continue; }
    // A character only the 16x16 set carries (the setup views draw those).
    if (UiCjk_HasGlyph(codepoint)) { width+=16; continue; }
    text=before;
    char c=UiFontNext(&text,&accent);
    if (!c) break;
    width += scale==3 ? (c==' '?5:8) : (c==' ' ? 4 : 6)*scale;
  }
  return width;
}

static inline unsigned UiFontNextCode(const char **text) {
  int accent; char base=UiFontNext(text,&accent);
  if (accent>=0) for (unsigned i=0;i<sizeof(kNativeUiAccents)/sizeof(kNativeUiAccents[0]);i++)
    if(kNativeUiAccents[i].base==base && kNativeUiAccents[i].accent==accent) return kNativeUiAccents[i].code;
  return (unsigned char)base;
}

// Center the actual ink, including accents/descenders, rather than the font's
// padded texture rectangle. draw_tiny_text positions that rectangle at y-2*scale.
static inline int UiFontCenteredY(const char *text, int scale, int top, int height) {
  int first=10000,last=-10000;
  while (*text) {
    const char *before=text;
    uint32_t codepoint=UiFont_DecodeUtf8(&text);
    int index=UiCjk_SmallIndex(codepoint);
    int size=index>=0?UiCjk_SizeForScale(scale):UiCjk_HasGlyph(codepoint)?16:0;
    if (size) {
      int offset=UiCjk_TopOffset(scale,size);
      const uint16_t *rows=index>=0?UiCjk_SmallRows(index,size):UiCjk_WideRows(codepoint);
      for (int y=0;y<size;y++) if (rows[y]) {
        if(y+offset<first)first=y+offset;
        if(y+offset>last)last=y+offset;
      }
      continue;
    }
    text=before;
    unsigned code=UiFontNextCode(&text);
    int rows=scale==3?15:scale==2?21:11;
    for(int y=0;y<rows;y++) {
      unsigned bits=scale==3?kNativeUiMediumGlyphs[code][y]:scale==2?kNativeUiLargeGlyphs[code][y]:kNativeUiGlyphs[code][y];
      if(!bits) continue;
      int a=scale==3?y-3:(scale==2?y:y*scale)-2*scale;
      int b=a+(scale==2||scale==3?1:scale)-1;
      if(a<first)first=a;if(b>last)last=b;
    }
  }
  return first>last ? top : top+(height-(last-first+1))/2-first;
}
