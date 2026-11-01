// shuffle_cosmetic.h — cosmetic shuffles (palette / sprite / music).
//
// See openspec/changes/add-rando-cosmetic-shuffles. These are client-local
// presentation transforms driven by a `cosmetic_seed` (a [Graphics] INI key),
// entirely outside the generation path: they NEVER touch the placement table,
// share_string, settings_hash, or any canonical serialization. Each axis
// defaults OFF, so an unopted run (rando or vanilla) is byte-identical to the
// original game (RAM/PPU compare clean).
//
// Determinism guarantee is cross-platform self-consistency (identical effective
// seed -> identical look on every platform via the project xoshiro256** RNG),
// NOT byte-parity with ALTTPR's JS-patcher transforms.

#ifndef ZELDA3_RANDO_SHUFFLE_COSMETIC_H_
#define ZELDA3_RANDO_SHUFFLE_COSMETIC_H_

#include "../types.h"

enum {
  kCosmeticPalette_Vanilla   = 0,  // off
  kCosmeticPalette_Shuffled  = 1,  // per-group channel permutation
  kCosmeticPalette_Grayscale = 2,  // luma collapse
  kCosmeticPalette_Negative  = 3,  // per-channel 1's complement
};

// Parse a palette-mode config string ("vanilla"/"shuffled"/"grayscale"/
// "negative"); unknown -> vanilla. Case-insensitive.
uint8 Cosmetic_ParsePaletteMode(const char *s);

// (Re)seed the cosmetic RNG and recompute the derived tables (palette group
// transforms + song permutation). `config_cosmetic_seed` is g_config.cosmetic_-
// seed; when it is 0 the effective seed is `slot_seed_u64` (the active rando
// slot's seed, or 0 for a vanilla run). Idempotent for a given pair; call at
// startup and again whenever a rando slot is (de)activated.
void Cosmetic_SetSeed(uint64 config_cosmetic_seed, uint64 slot_seed_u64);

// Transform the 256-entry BGR555 PPU CGRAM buffer in place. Called from the NMI
// CGRAM flush AFTER the copy from main_palette_buffer, so game RAM stays vanilla
// and there is no frame-to-frame accumulation. No-op when palette mode is
// vanilla. `count` is the number of 16-bit entries (256 for full CGRAM).
void Cosmetic_ApplyPaletteCgram(uint16 *cgram, int count);
void Cosmetic_ApplyPaletteCgramRange(uint16 *cgram, int start_index, int count);
uint16 Cosmetic_TransformPaletteColor(uint16 c, int cgram_index);

// Deterministically pick one .zspr file from g_config.cosmetic_sprite_dir.
// Returns an interned path (lives to process exit) or NULL when sprite shuffle
// is off, the folder is missing/empty, or enumeration fails (caller falls back
// to the configured single sprite). Resolved at launch.
const char *Cosmetic_PickSpriteFile(void);

// Remap an APU music command for the music-shuffle axis. Returns `music_ctrl`
// unchanged for control codes (high nibble 0xf), value 0, songs outside the
// shuffled band, or when music shuffle is off. The remap is a bijection within
// the band so no song is silenced or duplicated. Game-facing music state
// (last_music_control, music_unk1) stays vanilla — only the audible SPC/MSU
// output changes. One query-side exception: ZeldaIsPlayingMusicTrack's
// MSU-Deluxe arm compares against resume_info.actual_track, which derives
// from the REMAPPED id, so that arm applies this remap to its vanilla-id
// argument symmetrically (see audio.c).
uint8 Cosmetic_RemapSong(uint8 music_ctrl);

// Determinism + structural self-check (called from Rando_RunAllSelfChecks; aborts
// on failure). Asserts: a fixed seed yields identical derived tables across calls;
// config_cosmetic_seed==0 resolves to the slot seed; the seed actually drives the
// output; the song map is a band bijection (identity outside the band); and every
// group permutation index is in range. The cosmetic-determinism CI net (task 7.1).
void Cosmetic_SelfCheck(void);

#endif  // ZELDA3_RANDO_SHUFFLE_COSMETIC_H_
