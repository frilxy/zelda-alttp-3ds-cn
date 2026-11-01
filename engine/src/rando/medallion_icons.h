#ifndef ZELDA3_RANDO_MEDALLION_ICONS_H_
#define ZELDA3_RANDO_MEDALLION_ICONS_H_

#include "../types.h"

// Applies randomized Misery Mire / Turtle Rock medallion entrance glyphs to a
// decompressed 3bpp BG sheet. No-op outside the matching overworld screen,
// target sheet, and active rando medallion assignment.
void Rando_PatchMedallionEntranceBgGfx(uint8 *dst, int gfx_pack, int len);

// Redirects the medallion plaque's map8 words to the localized patched glyph
// tiles. `map8_words` is the four-entry map16->map8 expansion for `map16_id`.
void Rando_PatchMedallionEntranceMap8(uint16 map16_id, uint16 map8_words[4]);

// Structural guard for the sparse glyph patch tables.
void Rando_MedallionIcons_SelfCheck(void);

#endif  // ZELDA3_RANDO_MEDALLION_ICONS_H_
