// rando_placement.h — assumed fill + dispatcher (tasks.md §4, §6.1). Stub.
//
// BuildItemPool constructs the per-settings item pool (progressive vs absolute
// per mode.weapons, dungeon-item modes, Triforce-Hunt padding, Rupoor
// injection in hard/expert, ...).
//
// Place_AssumedFill runs the assumed-fill algorithm with bounded-retry
// rewind + forward-fill fallback (per randomizer-placement spec).
//
// Rando_OnLocationCheck — see rando.h. The dispatcher reads the
// placement_table to resolve location_id → placed_item_id.

#ifndef ZELDA3_RANDO_PLACEMENT_H_
#define ZELDA3_RANDO_PLACEMENT_H_

#include <stddef.h>

#include "../types.h"
#include "rando_logic.h"

typedef struct RandoPlacement {
  uint16 location_id;
  uint16 item_id;
} RandoPlacement;

typedef struct RandoPlacementTable {
  RandoPlacement *entries;
  uint16 count;
} RandoPlacementTable;

// Build the per-settings item pool. Returns the number of items written.
// |out_items| must be sized to hold the maximum possible pool (~216 entries
// at Phase A worst case).
// Return the small-key families that have at least one shuffled copy before
// Key Ring collapse (base chest keys + every active itemized key source).
uint16 KeyRings_EligibleMask(const RandoSettings *settings);

typedef struct KeyRingSelection {
  uint16 eligible_mask;
  uint16 selected_mask;
} KeyRingSelection;

// Resolve eligibility and the requested per-seed selection together without
// consuming the assumed-fill RNG stream. Effective Random with fewer than two
// eligible families is invalid; effective Off succeeds with selected_mask=0.
bool KeyRings_Resolve(const RandoSettings *settings, uint64 seed_u64,
                      KeyRingSelection *out);

// add-rando-random-crystals — resolve requested crystal counts (fixed 0..7
// pass through; kCrystalsRandom draws per-axis salted streams) to effective
// values. Callers MUST pass the BASE seed_u64 (never an attempt-perturbed
// seed). Shared by generation and slot activation.
void Crystals_Resolve(const RandoSettings *settings, uint64 seed_u64,
                      uint8 *out_ganon, uint8 *out_tower);

uint16 BuildItemPool(const RandoSettings *settings, uint64 seed_u64,
                     uint16 *out_items, uint16 capacity);

// Authoritative placement/reachability classifier. Hint selection uses this
// instead of maintaining a second progression-item list that could drift from
// assumed fill.
bool Rando_IsProgressionItem(uint16 item_id);

// Fail-fast validation for settings combinations that the placement pipeline
// cannot honestly build. BuildItemPool keeps equivalent guards for low-level
// callers and selftests; generation paths call this first to avoid retry-loop
// spam and to surface the real reason to users.
bool Placement_PreflightSettings(const RandoSettings *settings,
                                 char *err,
                                 size_t err_cap);

// Runtime/generator guard for settings that require generated enemy-drop or
// enemy-check registries. Kept beside BuildItemPool so slot activation and
// generation fail closed against the same compiled table availability.
bool Placement_EnemyRegistriesAvailableForSettings(const RandoSettings *settings,
                                                   char *err,
                                                   size_t err_cap);

// Assumed fill. Returns true on success; false if the budget was exhausted
// without producing a complete placement and the caller fell back to
// forward-fill. Either way, |out| is populated.
bool Place_AssumedFill(const RandoSettings *settings,
                       uint64 seed_u64,
                       int budget_seconds,
                       RandoPlacementTable *out);

// Seed the vanilla-mode dungeon items (small keys / big keys / maps / compasses
// whose class is in Vanilla mode) into a RandoCounts inventory — they are not
// shuffled into the world pool, so logic treats them as always available.
// Shared by the placer's assumed-fill seeding and the runtime reachability
// bridge (Rando_BuildRuntimeCounts) so both produce identical reachability.
void Rando_SeedVanillaDungeonItems(RandoCounts *counts, const RandoSettings *settings);

// Per-run placer stats from the most recent Place_AssumedFill call.
// Cleared at the start of every Place_AssumedFill call, populated before
// it returns. Read by the spoiler writer to populate fallback_warnings.
typedef struct PlacementStats {
  uint16 forward_fill_fallback_count;  // # of placements that fell to forward-fill
  uint8 attempts_used;                 // # of assumed-fill attempts taken
  uint16 best_unreachable_count;       // unreachable placements in the accepted attempt
  uint32 per_item_rewind_count;        // total per-item rewind events across all attempts (Bug #7)
  // FIX #6 — the assumed-fill attempt index whose per-attempt seed produced the
  // prize/medallion shuffle BAKED into the accepted placement table.
  // place_assumed_fill_attempt seeds PrizeShuffle_Run/MedallionShuffle_Run from
  // attempt_seed = base_seed ^ (attempt * 0x9E3779B97F4A7C15), so the runtime
  // MUST re-derive those assignments with the SAME perturbation at slot load,
  // not from the base seed (= attempt 0). The slot header persists this so
  // Rando_ActivateSidecarSlot can reproduce the baked assignment. 0 for the
  // common case where attempt 0 was accepted (matches the legacy behavior).
  uint8 prize_attempt;                 // accepted prize/medallion shuffle attempt index
} PlacementStats;
const PlacementStats *Placement_GetLastStats(void);

// SHA-256 of the canonical-serialized placement table, for regression-corpus
// diffing (tasks.md §4.4).
void PlacementTable_ComputeDigest(const RandoPlacementTable *t, uint8 out_digest[32]);

// Install the active placement table — subsequent Rando_OnLocationCheck calls
// consult this table. Pass NULL to clear (revert to pure pass-through, i.e.,
// rando mode inactive).
void Placement_Install(const RandoPlacementTable *t);
const RandoPlacementTable *Placement_GetActive(void);

// Presence-aware lookup for grant transactions. Returns true only when the
// active table contains location_id, and writes that entry's item id to
// out_item_id when non-NULL. Unlike Placement_Lookup, this distinguishes a
// present identity placement from an absent location that follows vanilla.
bool Placement_TryLookup(uint16 location_id, uint16 *out_item_id);

// Looks up location_id in the active table. Returns vanilla_item_id when no
// table is installed or the location_id is missing. Called from
// Rando_OnLocationCheck (rando.c). add-rando-pot-sanity D10: binary search over
// the location-id-sorted table (linear fallback if a producer ever installs an
// unsorted one — Placement_ActiveIsSorted reports which path is live).
uint16 Placement_Lookup(uint16 location_id, uint16 vanilla_item_id);

// add-rando-pot-sanity D10 — true iff the active placement table is sorted by
// location_id (so Placement_Lookup binary-searches). Exposed for the
// --rando-selftest sortedness assertion.
bool Placement_ActiveIsSorted(void);

// kRandoDungeon_* slot -> its Prize location id, or 0xFFFF if that dungeon
// awards no pendant/crystal (HCE/HCT/GT). Lets UI gate prize-completion on the
// per-dungeon prize location being checked (correct under prize_shuffle), not
// on global prize-type ownership.
uint16 Rando_GetDungeonPrizeLocation(int rando_dungeon);

// Self-check: digest stability under sort-order, BuildItemPool sanity. Exits 2
// on failure (per Settings_SelfCheck pattern).
void Placement_SelfCheck(void);

// ---------------------------------------------------------------------------
// Goal completability (tasks.md §3.9, randomizer-logic / Goal-completion).
//
// After Place_AssumedFill returns, the spoiler writer calls this to set the
// `goal_completable` field. The conservative semantics: with the full
// placement-table inventory in hand, the locations the active goal demands
// are reachable.
//
// Phase A1 implementation:
//   - Fast Ganon / Ganon: every dungeon required for the configured
//     crystals.ganon count is reachable AND the Ganon prize-event location
//     (and Agahnim 2 for goal=ganon) is reachable.
//   - All Dungeons: every dungeon-boss location is reachable.
//   - Pedestal: Master Sword Pedestal location is reachable AND inventory
//     contains the 3 pendants.
//   - Triforce Hunt: at least `pieces_required` TriforcePiece items are
//     placed at reachable locations, and the Pedestal location is reachable.
//   - Ganon Hunt: Triforce Hunt's piece requirement + the Ganon location.
//   - Completionist: every location in the placement table is reachable.
//
// Returns true when the goal is reachable, false otherwise. Logs a one-line
// reason to stderr on false.
// ---------------------------------------------------------------------------
// seed_u64 = the BASE generation seed (add-rando-random-crystals: the
// crystal arms certify against Crystals_Resolve of the BASE seed — never an
// attempt-perturbed seed).
bool Goal_IsCompletable(const RandoSettings *settings, uint64 seed_u64,
                        const RandoPlacementTable *placements);

// Accessibility-tier seed acceptance (ALTTPR three-way). Every tier requires
// the goal be completable (beatable); tiers add extra reachability:
//   kAccessibility_Locations — every location reachable (strictest)
//   kAccessibility_Items     — every PROGRESSION item's location reachable
//   kAccessibility_None       — "beatable only": no extra reachability demand
// See the definition in rando_placement.c for the full rationale.
bool Accessibility_SeedAcceptable(const RandoSettings *settings, uint64 seed_u64,
                                  const RandoPlacementTable *placements);

// Should the generator REFUSE this seed? The negation of
// Accessibility_SeedAcceptable: a seed is refused when it fails the active
// accessibility tier's bar (beatability for all tiers, plus per-tier
// reachability). The spoiler's `goal_completable` field is always the pure
// reachability predicate (Goal_IsCompletable); the refusal gate additionally
// honors the accessibility tier.
bool Goal_ShouldRefuse(const RandoSettings *settings, uint64 seed_u64,
                       const RandoPlacementTable *placements);

// ---------------------------------------------------------------------------
// Sphere computation (tasks.md §5 / randomizer-core / Sphere semantics).
//
// Sphere 0 = locations reachable with the starting inventory alone.
// Sphere N+1 = locations newly reachable after collecting every item in
// spheres 0..N. The fixed-point terminates when no new sphere can be added.
//
// Locations whose placed items never become reachable get sphere_index =
// kSphereIndexUnreachable. The presence of any such location is a strong
// signal that the placement is broken (or the logic graph is incomplete).
// ---------------------------------------------------------------------------
#define kSphereIndexUnreachable 0xFF
// Hard cap on sphere depth. Vanilla-graph logical depth is typically < 20,
// but deep pot-key ladders + dungeon chains approach 32, and hitting the cap
// silently converts still-reachable late-sphere placements into phantom
// unreachables (refusing locations-tier seeds). Raising it is free: no array
// is sized by it (sphere indices are one uint8 per placement either way); the
// only bound is staying below the kSphereIndexUnreachable sentinel.
#define kSphereMaxCount         64

typedef struct RandoSpheres {
  // For each entry index in the placement table: which sphere it belongs to.
  // Indexed by placement-table index (0..placements->count-1).
  uint8 sphere_index_by_placement[kRandoLocationCapacity];
  uint8 max_sphere;                  // highest sphere reached (inclusive); 0 if no progress
  uint16 unreachable_count;          // count of placements with sphere_index_by_placement == 0xFF
} RandoSpheres;

// Compute sphere assignment for every placement. Returns true if every
// placement is assigned a sphere (placement is fully reachable); false if
// any placement is unreachable.
bool Logic_ComputeSpheres(const RandoSettings *settings,
                          const RandoPlacementTable *placements,
                          RandoSpheres *out);

// ---------------------------------------------------------------------------
// Starting-inventory injection (tasks.md §4.2).
//
// Grants the per-world-state starting items exactly once per slot. The
// `kRam_RandoStartingInventoryGranted` gate ensures save-reload does NOT
// re-inject (per randomizer-placement / "Starting inventory injection").
//
// Per `randomizer-placement / Starting-inventory injection atomicity`: all
// per-item grants complete in the same ZeldaRunFrame call. Caller MUST NOT
// invoke ZeldaWriteSram between Try... and the flag being set; both happen
// in this function.
//
// Returns true if injection ran this call, false if it was already granted
// (no-op) or rando is inactive.
// ---------------------------------------------------------------------------
bool Rando_TryGrantStartingInventory(const RandoSettings *settings);

#endif  // ZELDA3_RANDO_PLACEMENT_H_
