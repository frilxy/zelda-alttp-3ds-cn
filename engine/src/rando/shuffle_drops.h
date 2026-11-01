// shuffle_drops.h — Phase B Slice 8 (add-rando-shuffles-and-minigames).
//
// Sprite-death drop-pool shuffle. Permutes the 56-entry vanilla prize-drop
// table (7 packs × 8 slots; see kPrizeItems[] in src/sprite.c) while keeping a
// heart-drop floor so weak early-game enemies still drop hearts and the player
// is not HP-starved.
//
// ALTTPR upstream: drop pool is app/Drops/PrizePack.php + PrizePackSlot.php +
// the 11-pack/63-slot roster at app/World.php:76-87 + the sprite table
// app/Sprite.php. (NOT app/EnemyDrop.php — that file does not exist; see
// the change's audit.md §"Drop-pool provenance".) This fork models the pool as
// the flat 56-entry kPrizeItems table; the shuffle is a permutation over it.

#ifndef ZELDA3_RANDO_SHUFFLE_DROPS_H_
#define ZELDA3_RANDO_SHUFFLE_DROPS_H_

#include "../types.h"
#include "rando_placement.h"

// Number of entries in the vanilla prize-drop table (7 packs × 8 slots).
#define kDropTableEntryCount 56

// One-shot generation + INSTALL entry. Deterministic from (settings, seed_u64).
// Computes the permutation (with the heart-drop floor, below) and installs it
// into the module-global runtime table consumed by DropShuffle_Lookup at the
// sprite-drop site. `placements`/`spheres` are accepted for API stability but
// are not consulted: the enemy→pack binding is static (per-sprite, not sphere-
// indexed), so the heart floor is enforced structurally on pack 0 rather than
// against sphere data — see DropShuffle_ComputeAssignment.
//
// Returns true on success.
bool DropShuffle_Generate(const RandoSettings *settings,
                          uint64 seed_u64,
                          const RandoPlacementTable *placements,
                          const RandoSpheres *spheres);

// Pure permutation computation — NO global side effects. Deterministic from
// (settings, seed_u64). Writes a permutation of 0..55 into `out_map`.
//
// Heart-drop floor (§3.3): pack 0 (flat indices 0..7 — the vanilla heart-heavy
// pack that weak overworld enemies draw from, hence reachable from sphere 0)
// must keep at least one heart entry (vanilla drop id 0xD8) after the shuffle.
// If a draw violates the floor it is re-rolled (bounded retry on the same RNG
// stream). If the retry budget is exhausted, the table falls back to the
// vanilla identity and `*out_used_fallback` is set (§3.5).
//
// When drop_shuffle is off (or settings == NULL), writes the identity table and
// returns 0. Returns the number of retries consumed (0 on the common path).
uint32 DropShuffle_ComputeAssignment(const RandoSettings *settings,
                                     uint64 seed_u64,
                                     uint8 out_map[kDropTableEntryCount],
                                     bool *out_used_fallback);

// Look up the (possibly shuffled) drop entry at index `drop_table_index`.
// Returns the vanilla drop when no shuffle is installed (passthrough).
uint8 DropShuffle_Lookup(uint8 drop_table_index);

// Clear the installed table (slot teardown). After this DropShuffle_Lookup is a
// pure passthrough until the next DropShuffle_Generate. Pairs with
// Rando_DeactivateSlot so a shuffle slot's table can't leak into a later
// (non-shuffle) slot or vanilla play.
void DropShuffle_Deactivate(void);

// Self-check (--rando-selftest): determinism, permutation property,
// off→identity, and the heart-drop floor. exit(2) on failure.
void DropShuffle_SelfCheck(void);

#endif  // ZELDA3_RANDO_SHUFFLE_DROPS_H_
