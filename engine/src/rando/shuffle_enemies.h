// shuffle_enemies.{c,h} — add-rando-enemy-shuffle (sprite-type substitution).
//
// Randomizes WHICH enemy sprite spawns in each dungeon room / overworld area,
// constrained so nothing crashes (GFX-sheet-safe) and nothing softlocks
// (killable in key/shutter/water rooms). A randomizer slot axis: deterministic
// from (settings, seed), reproducible for races. Mirrors the boss-shuffle
// install precedent (shuffle_boss.{c,h}) exactly: a one-shot Generate that
// installs a module-global, a Deactivate that tears it down on slot teardown /
// the fail-closed invalid-settings path, and a SelfCheck wired into
// Rando_RunAllSelfChecks.
//
// Built scope: sprite-type substitution, runtime sprite-group subgroup
// reshuffle across slots 0..3, and mild HP / contact-damage variation all ride
// the single enemy_shuffle axis. Still deferred: killable-thief, bush-enemy,
// absorbable, and randomize-on-hit axes.
//
// Reference (constraints, NOT code): Enemizer SpriteRequirement.cs (MIT). We
// mine the per-sprite flags + room exclusion lists; the mechanism is our own.
//
// BEATABILITY: logic models NO per-room kill-clear (every CanKill<X> macro is a
// player-firepower test over bosses/mini-bosses, all EXCLUDED here — see
// design.md D7), so a table bug does NOT move placement_digest and is invisible
// to the corpus / --rando-selftest. The constraint table is therefore the SOLE
// beatability enforcer; unknown-safety sprites are marked do_not_randomize.

#ifndef ZELDA3_RANDO_SHUFFLE_ENEMIES_H_
#define ZELDA3_RANDO_SHUFFLE_ENEMIES_H_

#include "../types.h"

struct RandoSettings;

// One-shot generation + INSTALL entry. Deterministic from (settings, seed_u64)
// via a dedicated xoshiro256** stream forked off the seed (so the fill RNG is
// unperturbed and the result is cross-platform self-consistent). Installs a
// module-global "active" flag + the per-seed RNG seed consumed at sprite load.
//
// When enemy_shuffle is off (or settings == NULL) this is a no-op install: the
// module reports inactive and the load paths take the vanilla branch (so an
// off slot is byte-identical to a non-shuffled slot). Returns true on success.
//
// NOTE: unlike boss shuffle there is no precomputed assignment table — the
// substitution is computed per-room/per-area at load time, because the
// candidate pool depends on which GFX sheets are resolved for that room/area
// (sprite_gfx_subset_0..3 after the sheet resolver snapshots them), which is
// load-history dependent and not known at generation time. Determinism comes
// from a per-room/per-area RNG derived from (seed, room/area key), not from a
// stored permutation.
bool EnemyShuffle_Generate(const struct RandoSettings *settings,
                           uint64 seed_u64);

// Clear the installed shuffle (slot teardown / fail-closed invalid-settings
// path). After this the load paths are pure passthroughs (vanilla enemies)
// until the next EnemyShuffle_Generate. Pairs with Rando_DeactivateSlot so a
// shuffle slot's substitution can't leak into a later (non-shuffle) slot or
// vanilla play — mirrors BossShuffle_Deactivate / DropShuffle_Deactivate.
void EnemyShuffle_Deactivate(void);

// True iff an enemy shuffle is installed AND enabled for the active slot.
// The sprite load paths gate on this before doing any work.
bool EnemyShuffle_IsActive(void);

// add-rando-trap-catalog — true iff every required GFX sheet of `type` is loaded
// in its canonical subgroup slot for the current room/area. Used by the Ambush
// trap to spawn only enemies that will render correctly (independent of whether
// enemy_shuffle is active).
bool EnemyShuffle_SheetsLoadedFor(uint8 type);

// === Sprite-load substitution hooks (consumed in src/sprite.c) ===
//
// DUNGEON: given the room-data type byte for a single sprite entry in
// `room` (dungeon_room_index2) at list position `slot`, return the substituted
// sprite type (or the input unchanged when no substitution applies: shuffle
// inactive, the type is excluded/marker, or no valid candidate exists). The
// caller writes the result into sprite_type[k] — a pure type-byte swap; the
// bit-packed y/x bytes are NOT touched. `slot` (the entry's index in the room's
// sprite list) is mixed into the per-room RNG so two identical types in one
// room can map to different replacements deterministically.
uint8 EnemyShuffle_PickDungeon(uint16 room, uint8 slot, uint8 vanilla_type);

// OVERWORLD: given the area-data type byte for an overworld sprite entry in
// `area` (overworld_area_index) at list position `slot`, return the substituted
// sprite type (or the input unchanged when no substitution applies). The caller
// stores `pick + 1` into sprite_where_in_overworld (preserving the +1 bias).
uint8 EnemyShuffle_PickOverworld(uint8 area, uint8 slot, uint8 vanilla_type);

// === Sprite-group SHEET reshuffle hook (design.md D4 — the variety unlock) ===
//
// Called from the GFX-sheet load (Gfx_LoadSpritesInner / InitializeTilesets in
// src/load_gfx.c) AFTER the four subgroup ids are resolved from `tileset_row`
// (= kSpriteTilesets[sprite_graphics_index]) but BEFORE any sheet is
// decompressed. When enemy_shuffle is active in a dungeon/overworld room load it
// may RE-ASSIGN which sheet loads into any owned, unpinned subgroup slot among
// 0..3, widening the pool the existing EnemyShuffle_Pick* draws from. It also
// snapshots the resolved sheet set with the room/area key, so the picker can
// reject stale transition state. A no-op when the shuffle is off, outside a
// room/area load (attract/menu/etc.), or when a subgroup slot is pinned by a
// non-substituted sprite (see shuffle_enemies.c for the eligibility +
// anti-garbage + inheritance model). `tileset_row` is the 4-byte kSpriteTilesets
// row for the load (slot N == 0 means "inherit the previously-loaded sheet").
void EnemyShuffle_ReshuffleCurrentRoomSheets(const uint8 *tileset_row);

// === Enemy STAT randomization (HP / contact damage) ===
//
// Called from SpritePrep_LoadProperties (src/sprite.c) on the per-type init
// values. Deterministic per (seed, sprite type) — every Octorok in a seed shares
// its scaled stats. Always-on with enemy_shuffle; a pure passthrough when the
// shuffle is off, so vanilla play is byte-identical. SOFTLOCK-SAFE: HP scaling
// never makes an enemy unkillable (Link can always keep attacking; bounded
// factor, clamp >= 1), and a 0-HP (non-killable) sprite is left untouched so
// NPCs/objects are unaffected.
//
// ScaleHealth: returns `base` HP scaled by a per-type factor in [0.5x .. 2.0x],
//   clamped to [1, 255]; `base == 0` returns 0.
// ScaleDamage: `sprite_bump_damage` is a flag+class byte (low nibble = damage
//   class into kPlayerDamages, high bits = immunity/special flags). We perturb
//   ONLY a plain class (value 1..8, no high bits) by a mild deterministic -1/0/+1,
//   clamped [1, 8]; flagged/boss-attack values are returned unchanged.
uint8 EnemyShuffle_ScaleHealth(uint8 type, uint8 base);
uint8 EnemyShuffle_ScaleDamage(uint8 type, uint8 base);

// Self-check (--rando-selftest): asserts determinism, the off→passthrough
// contract, that excluded types/markers are never substituted, that every
// candidate produced for a synthetic loaded-sheet set is in-sheet + killable /
// key-capable / water as required, and the structural integrity of the
// constraint table. exit(2) on failure. MUST be registered in
// Rando_RunAllSelfChecks or it won't run.
void EnemyShuffle_SelfCheck(void);

#endif  // ZELDA3_RANDO_SHUFFLE_ENEMIES_H_
