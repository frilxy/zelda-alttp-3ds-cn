// Chinese (and, in principle, any 16x16 CJK) text for the dialogue engine.
//
// The languages the port builds from the USA cartridge reuse the cartridge's
// own 8x16 letter alphabet: 256 tiles, one width byte per letter. A Chinese
// translation cannot: it needs thousands of 16x16 glyphs and a way to name
// them. Such a language carries two extra pieces in its asset language block,
// both appended after the standard ones:
//
//    font block:  4096 bytes of 256 8x16 letter tiles, then
//                 glyph_count * 64 bytes, one 16x16 1bpp glyph each
//                 (top-left, top-right, bottom-left, bottom-right, 16B each)
//    width block: 99 letter widths, then one total width byte per glyph
//
// Its messages then name a glyph with the two bytes 0x6f..0x73 + index (one
// prefix byte per 256 glyphs, index 111 in the text engine's alphabet), and
// dialogue_flags bit 2 says the block has that shape. The text engine renders
// the glyph as two 8x16 half tiles, which is the width of one letter column in
// the messaging buffer.
#ifndef ZELDA3_DIALOGUE_CJK_H_
#define ZELDA3_DIALOGUE_CJK_H_

#include <stdbool.h>
#include <stdint.h>

enum {
  kDialogueCjkFlag = 4,           // dialogue_flags bit 2: block has CJK glyphs
  kDialogueCjkFirstChar = 111,    // first glyph index the escapes name
  kDialogueCjkPrefixFirst = 0x6f, // first escape prefix byte
  kDialogueCjkPrefixLast = 0x73,  // last one: 5 * 256 glyphs reachable
  kDialogueCjkFontOffset = 4096,  // glyphs start behind the 256 letter tiles
  kDialogueCjkGlyphBytes = 64,    // one 16x16 glyph
  kDialogueCjkHalfBytes = 16,     // one 8x16 half tile
};

// Decodes one escape pair into the character the text engine carries in its
// command word. Returns false when `prefix` is not a CJK escape, in which case
// the caller keeps its normal handling.
bool DialogueCjk_DecodeEscape(uint8_t prefix, uint8_t index, uint32_t *out_char);

// True when text character `c` names a glyph rather than a letter.
bool DialogueCjk_IsGlyph(int c);

// Where the renderer draws: the messaging buffer, the pen state the caller
// owns, and the language's font block. `glyphs` points at the first 16x16
// glyph; `widths` at the block's width bytes.
typedef struct DialogueCjkTarget {
  uint8_t *buf;           // messaging buffer (16-bit words seen as bytes)
  uint8_t *arr;           // per-column pen positions
  uint16_t *pen;          // next free column slot
  int line;               // first row of the message line
  const uint8_t *glyphs;  // 16x16 glyphs
  const uint8_t *widths;  // width bytes, indexed by kDialogueCjkFirstChar + n
} DialogueCjkTarget;

// Draws one glyph (0-based, as named by the escapes) at the current pen.
void DialogueCjk_Render(const DialogueCjkTarget *target, int glyph);

#endif  // ZELDA3_DIALOGUE_CJK_H_
