// shuffle_boss.h — Phase B Slice 7 (add-rando-shuffles-and-minigames) scaffold.
//
// Boss-room reassignment. Randomizes which boss sprite appears in each
// dungeon-boss room while preserving the dungeon's prize/reward binding
// (EP's pendant stays at EP regardless of which boss is there). Goal-
// required bosses (Agahnim 1, Agahnim 2, Ganon) are pinned to their
// canonical slots and never appear elsewhere in the pool.
//
// Status: LIVE (experimental). The assignment algorithm + the runtime RENDER
// redirect (BossShuffle_RenderHomeRoom — the Enemizer pointer-redirect model)
// are shipped; the shuffle pool is the 7 redirect-clean bosses (Blind /
// Kholdstare / Trinexx are pinned — they need home-room ENVIRONMENT a sprite +
// gfx + palette redirect can't carry). The earlier per-entry sprite-type swap
// (BossShuffle_RemapSpriteType / _ShouldSuppressSecondary) is SUPERSEDED and
// retained only for the selftest cross-check; the game never calls it.
//
// ALTTPR upstream: app/Boss.php (boss-pool definition + assignment rules).

#ifndef ZELDA3_RANDO_SHUFFLE_BOSS_H_
#define ZELDA3_RANDO_SHUFFLE_BOSS_H_

#include "../types.h"
#include "rando_placement.h"

// One-shot generation entry. Builds the per-dungeon boss assignment based on
// the active settings (boss_shuffle — a live CSV key / canonical settings byte).
// Deterministic from (settings, seed_u64).
//
// `out_assignment` is sized to one byte per kRandoDungeon_* slot; each entry is
// the boss-pool index assigned to that dungeon's boss room. The table is consumed at
// runtime by the render redirect (BossShuffle_RenderHomeRoom) and by the logic
// VM (OP_CAN_KILL_BOSS, via Rando_SetBossAssignment) so each dungeon's
// boss/prize gates on the SHUFFLED boss's kill predicate. When boss_shuffle is
// off, writes the identity (vanilla) assignment.
//
// Returns true on success. Also INSTALLS the assignment into module-global
// runtime state and marks the assignment active.
bool BossShuffle_Generate(const RandoSettings *settings,
                          uint64 seed_u64,
                          uint8 out_assignment[16]);

// Pure assignment computation — NO global side effects. Deterministic from
// (settings, seed_u64). Writes the dungeon-id → boss-pool-index table into
// `out_assignment[16]` (0xFF for HCE/unused slots; Agahnim 1/2 pinned at their
// kRandoDungeon_* slots). Used by the spoiler writer (which must not perturb the runtime
// install) and internally by BossShuffle_Generate. When boss_shuffle is off,
// writes the identity (vanilla) assignment.
void BossShuffle_ComputeAssignment(const RandoSettings *settings,
                                   uint64 seed_u64,
                                   uint8 out_assignment[16]);

// Clear the installed assignment (slot-teardown). After this,
// BossShuffle_RemapSpriteType/_ShouldSuppressSecondary are pure passthroughs
// (vanilla bosses) until the next BossShuffle_Generate. Pairs with
// Rando_DeactivateSlot so a shuffle slot's assignment can't leak into the
// next (non-shuffle) slot or into vanilla play.
void BossShuffle_Deactivate(void);

// Human-readable names for the spoiler (boss assignments). `pool_index` is a
// kBoss_* value (0..11); `rando_dungeon` is a kRandoDungeon_* slot. Returns "?"
// out of range.
const char *BossShuffle_BossName(uint8 pool_index);
const char *BossShuffle_DungeonName(uint8 rando_dungeon);

// Self-check (invoked from --rando-selftest): asserts determinism, the pinned-boss
// invariants (Agahnim 1/2 + Blind/Kholdstare/Trinexx stay at their slots), the
// off→identity contract, and that the shuffled assignment is a permutation of the
// 7-boss pool over the 7 shuffleable dungeons. exit(2) on failure.
void BossShuffle_SelfCheck(void);

// Returns the boss-pool index for `rando_dungeon` from the currently
// installed assignment. Returns 0xFF when no assignment is active
// (boss shuffle off, or no slot loaded).
uint8 BossShuffle_GetForDungeon(uint8 rando_dungeon);

// SUPERSEDED — do NOT call at runtime. The live model is the render redirect
// (BossShuffle_RenderHomeRoom, below). This per-entry sprite-type swap was the
// original architecture; a pure type swap loads the wrong GFX sheet and
// mis-spawns formation bosses, so it was replaced. Retained ONLY so
// BossShuffle_SelfCheck can cross-check the pool/mapping tables. Historical
// contract follows.
//
// Per-site instrumentation entry — Phase B §65.
//
// Given the vanilla boss sprite type the room data wants to spawn
// (e.g. 0x53 ArmosKnight for EP's boss room), returns the sprite
// type that should actually spawn under the active shuffle.
//
// Returns `vanilla_sprite_type` unchanged when:
//   - the sprite type is not a recognized boss
//   - boss-shuffle is off (assignment active but identity)
//
// Vanilla boss sprite IDs: 0x09 Moldorm, 0x53 ArmosKnights,
// 0x54 Lanmolas, 0x7A Agahnim 1/2 (both pinned), 0x88 Mothula,
// 0x8C Arrghus, 0x92 HelmasaurKing, 0xA2 Kholdstare, 0xBD Vitreous,
// 0xCB Trinexx, 0xCE Blind.
uint8 BossShuffle_RemapSpriteType(uint8 vanilla_sprite_type);

// SUPERSEDED — do NOT call at runtime (see BossShuffle_RemapSpriteType above).
// Retained only for the selftest cross-check.
//
// Per-site instrumentation.
//
// Returns true if `vanilla_sprite_type` is a room-data secondary segment
// of a boss whose primary has been remapped to something else (i.e., the
// segment would spawn as an orphan with no kill-logic context). The
// caller is expected to skip the sprite-spawn.
//
// Returns false when:
//   - boss-shuffle is inactive (g_boss_assignment_active = false)
//   - the sprite isn't a known secondary segment
//   - the parent dungeon still has its vanilla boss (identity remap —
//     segment is wanted)
//
// Known room-data secondaries (sprites that appear alongside their
// primary in dungeon room data, NOT spawned by runtime boss logic):
//   0xCC Trinexx left arm  → TR, parent kBoss_Trinexx
//   0xCD Trinexx right arm → TR, parent kBoss_Trinexx
//   0xA3 KholdstareShell   → IP, parent kBoss_Kholdstare
//
// Other bosses' segments (Mothula beams 0x89, Arrghi puffs 0x8D,
// VitreousEye 0xBE, BlindHead) are spawned at runtime by the primary
// boss's logic and so naturally come along with the remapped boss
// instead of the orphaned vanilla one — no suppression needed.
bool BossShuffle_ShouldSuppressSecondary(uint8 vanilla_sprite_type);

// Runtime RENDER redirect (the live model; supersedes RemapSpriteType). When
// `room` is a shuffled boss room (boss shuffle active AND its assigned boss !=
// its vanilla boss), returns the assigned boss's vanilla HOME boss-room index —
// the room whose sprite-data list AND sprite-graphics index the engine should
// load instead, so the substituted boss renders with the right tiles + the right
// formation/count (the Enemizer pointer-redirect model). Returns 0xFFFF when no
// redirect applies (boss shuffle off, `room` not a shuffleable boss room, or the
// assignment is the vanilla identity) — the caller then uses `room` unchanged, so
// vanilla / boss-off play is byte-identical. Consulted by Dungeon_LoadSprites
// (sprite list) and the dungeon room-header load (sprite-graphics index).
uint16 BossShuffle_RenderHomeRoom(uint16 room);

// Render redirect WITH anchor info for coordinate alignment. Like
// BossShuffle_RenderHomeRoom but also returns (via out params) the two boss
// SPRITE types the caller anchors on: `dest_vanilla_sprite` = this dungeon's
// vanilla boss (its position in THIS room is the target), `home_boss_sprite` =
// the assigned boss (its position in the home room is the source). The caller
// finds each anchor in the respective room's sprite data and shifts the
// redirected formation by (dest_anchor - home_anchor), so the substituted boss
// spawns where the dungeon's own boss did (reachable) instead of at the home
// room's coords (which can land behind a wall / one screen over). Returns true
// when a redirect applies (same condition as RenderHomeRoom != 0xFFFF).
bool BossShuffle_GetRenderRedirect(uint16 room, uint16 *home_room,
                                   uint8 *dest_vanilla_sprite,
                                   uint8 *home_boss_sprite);

#endif  // ZELDA3_RANDO_SHUFFLE_BOSS_H_
