#ifndef ZELDA3_RANDO_OW_MAP_PRIZES_H_
#define ZELDA3_RANDO_OW_MAP_PRIZES_H_

#include "../types.h"

// Overworld pause-map pendant/crystal markers under prize_shuffle.
//
// Vanilla hard-wires each marker slot to a (dungeon position, prize icon,
// prize-TYPE ownership test) triple that is only coherent under the vanilla
// dungeon->prize assignment. This module re-keys the icon by DUNGEON and gates
// the reveal on player knowledge: the true prize appears only once that
// dungeon's prize location is checked; until then the marker draws vanilla's
// existing blinking "unknown" form (a red X — upstream ALTTPR names the same
// glyph table WorldMap_RedXChars; ours is kOwMap_tab2 in messaging.c).
//
// See openspec/specs/randomizer-player-knowledge — this is an in-world
// (diegetic) assignment surface, held to the tracker invariant.

// Resolve the marker in slot `slot` (0..6) for map-icon state `k`
// (savegame_map_icons_indicator). Returns false and leaves the outputs
// untouched when the slot is not a rando-owned prize marker, i.e. when the
// vanilla art is already the truth (no slot active, prize_shuffle off, or a
// non-prize routing marker) — the caller then draws vanilla.
//
// On true, *tab is the icon word to draw (0 = the vanilla blinking "unknown"
// marker: the prize has not been observed) and *num_char is the crystal-number
// glyph to blink (0 = none / vanilla).
bool RandoOwMap_PrizeMarker(uint8 k, uint8 slot, uint16 *tab, uint8 *num_char);

// Oracle: under the VANILLA prize assignment the re-keyed path must reproduce
// the vanilla marker data exactly, for every prize slot in every affected map
// icon state. Aborts the process on mismatch.
void RandoOwMap_SelfCheck(void);

#endif  // ZELDA3_RANDO_OW_MAP_PRIZES_H_
