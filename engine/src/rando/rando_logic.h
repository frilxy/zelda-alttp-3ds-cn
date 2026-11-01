// rando_logic.h — predicate VM and reachability (tasks.md §3.7-§3.9).
//
// The predicate VM evaluates the bytecode emitted by assets/rando_logic_gen.py
// from logic.yaml. Two evaluation entry points:
//   - Predicate_Evaluate(bc, len, counts, settings) for can_reach
//   - Predicate_EvaluatePlacement(bc, len, counts, settings, candidate_item,
//     selected_key_rings_mask) for can_place (only context where OP_ITEM_IS may
//     appear)
//
// Logic_ComputeReachabilityFullKnowledge runs the fixed-point expansion over the static
// EdgeDef[] graph (consulting the RegionRemap overlay for Phase C entrance
// shuffle; identity in Phase A).
//
// Bytecode format (per assets/rando_logic_gen.py docstring):
//   HAS_ITEM       <op:u8> <item_id:u16_le>
//   HAS_AMOUNT     <op:u8> <item_id:u16_le> <n:u8>
//   HAS_ANY_OF     <op:u8> <count:u8> <id:u16_le>×count
//   HAS_ANY_COUNT  <op:u8> <count:u8> <id:u16_le>×count <n:u8>
//   WORLDSTATE_EQ  <op:u8> <state:u8>
//   GOAL_EQ        <op:u8> <goal:u8>
//   GOAL_REQUIRES_DUNGEON <op:u8> <dungeon:u8>
//   DUNGEON_CLEARED <op:u8> <dungeon:u8>
//   REGION_REACHABLE <op:u8> <region_id:u16_le>
//   HAS_PRIZE      <op:u8> <prize:u8>
//   MEDALLION_OPENS <op:u8> <entrance:u8>
//   ITEM_IS        <op:u8> <item_id:u16_le>
//   INSTANT_FLUTE  <op:u8>
//   NOT            <op:u8> <child>
//   AND            <op:u8> <count:u8> <child>×count
//   OR             <op:u8> <count:u8> <child>×count
// Vacuous AND (count=0) is TRUE; vacuous OR (count=0) is FALSE.

#ifndef ZELDA3_RANDO_LOGIC_H_
#define ZELDA3_RANDO_LOGIC_H_

#include "../types.h"
#include "rando_settings.h"

// ---------------------------------------------------------------------------
// Op IDs — mirror assets/rando/op_registry.yaml (0-based, append-only).
// ---------------------------------------------------------------------------
typedef enum {
  OP_HAS_ITEM = 0,
  OP_HAS_AMOUNT = 1,
  OP_HAS_ANY_OF = 2,
  OP_HAS_ANY_COUNT = 3,
  OP_WORLDSTATE_EQ = 4,
  OP_GOAL_EQ = 5,
  OP_GOAL_REQUIRES_DUNGEON = 6,
  OP_DUNGEON_CLEARED = 7,
  OP_REGION_REACHABLE = 8,
  OP_HAS_PRIZE = 9,
  OP_MEDALLION_OPENS = 10,
  OP_ITEM_IS = 11,
  OP_AND = 12,
  OP_OR = 13,
  OP_NOT = 14,
  // Phase B placeholders — predicates that reference these always evaluate
  // to their zero-glitch / zero-trick branch in Phase A.
  OP_TRICK = 15,
  OP_DIFFICULTY_AT_LEAST = 16,
  OP_GLITCH_LEVEL_AT_LEAST = 17,
  // Phase B swordless — settings.mode_weapons == operand. Used by the Ganon /
  // Agahnim / CanMeltThings predicates to add a swordless branch.
  OP_MODEWEAPONS_EQ = 18,
  // Boss-shuffle runtime — "can kill the boss assigned to dungeon_id". Resolves
  // the per-seed boss assignment (boss_assignment, NULL→vanilla) then evaluates
  // that boss's kill predicate from kRandoBossKillPred[]. boss_shuffle off ⇒
  // vanilla identity ⇒ byte-identical to the inline CanKill<Boss> it replaces.
  OP_CAN_KILL_BOSS = 19,
  // Door shuffle (add-rando-door-shuffle). OP_DOORS_ACTIVE(door_dungeon_idx):
  // a layout is installed AND that dungeon's shuffled_mask bit is set (pinned
  // dungeons stay clear -> vanilla branch of the codegen wrap).
  // OP_DOORS_LOC_REACHABLE(loc_u16): the door-shuffle oracle — the same
  // crystal-aware explorer the stitcher/prover use, seeded from the portal
  // lobbies whose kDoorPortalGates row holds under the current region bitset,
  // gated by inventory + per-key-door worst-case thresholds. False when no
  // layout is installed.
  OP_DOORS_ACTIVE = 20,
  OP_DOORS_LOC_REACHABLE = 21,
  // Randomizer QoL — true when the seed setting promotes OcarinaInactive
  // pickups to the active bird-woken flute immediately.
  OP_INSTANT_FLUTE = 22,
  // True iff dungeon pot keys are first-class shuffled checks
  // (pot_shuffle >= Keys AND not cave-entrance forced-off). A
  // pot-bearing dungeon's deep locations wrap their small-key term so pots-off
  // keeps the vanilla worst-case (byte-identical) and pots-on requires the
  // prover worst-case that counts the now-itemized pot keys.
  OP_POT_KEYS_ON = 23,
  // POT_KEYS_ON AND small keys are WILD (keysanity, incl. Retro). The
  // pot-bearing dungeons' deep locations gate their wild
  // worst-case key requirement on this: under wild keys you must HOLD the keys
  // before reaching (they live anywhere in the world), so the static worst-case
  // is both correct and achievable. Dungeon-keys (in-context collection) is a
  // separate follow-on and stays on the vanilla branch here.
  OP_POT_KEYS_WILD = 24,
  // POT_KEYS_ON AND small keys are DUNGEON (per-dungeon, the in-context case).
  // The pot-bearing dungeons' deep locations/pots gate their
  // SHORTEST-PATH (min-depth) small-key requirement on this: keys are collected en
  // route so the graduated min-depth is necessary+sufficient (a flat worst-case is
  // circular). Wild keys (worst-case) and pots-off both leave this false, so the
  // wrapped term collapses out and their placement is byte-identical.
  OP_POT_KEYS_DUNGEON = 25,
  // ENEMY_DROP_KEYS_DUNGEON is true when forced enemy-drop checks are active
  // under vanilla-door Dungeon small keys. Generated enemy-drop rows use this to
  // replace inherited same-key gates with exact DROP-region min-depth.
  OP_ENEMY_DROP_KEYS_DUNGEON = 26,
  // ENEMY_DROP_KEYS_WILD is true when forced enemy-drop checks are active under
  // Wild small keys, including Retro's generic-key mode. Generated wraps use it
  // for the held-before-entry worst-case key-depth.
  OP_ENEMY_DROP_KEYS_WILD = 27,
  // SOULS_TIER_AT_LEAST <tier:u8> — settings.souls_shuffle >= tier
  // (add-enemy-souls). Wraps soul-item requirements so they collapse to true
  // (inert) below the tier; default Off reproduces pre-souls reachability.
  OP_SOULS_TIER_AT_LEAST = 28,
  // NPC_SOULS_ACTIVE (no operands) — settings.npc_souls != 0 (add-npc-souls).
  // Wraps NPC-check soul requirements: (NOT NPC_SOULS_ACTIVE) OR HAS_ITEM(X)
  // collapses to true when the toggle is off (placement byte-identical).
  OP_NPC_SOULS_ACTIVE = 29,
  // TOWER_CRYSTALS_MET (no operands) — crystals held >= the seed's RESOLVED
  // crystals.tower requirement (add-rando-random-crystals; external-review
  // round-2: the GT edges hardcoded 7, so random/non-7 tower counts never
  // reached generator reachability). Resolution comes from the context's
  // tower_crystals_required_plus1 when a builder set it; otherwise falls
  // back to min(settings->crystals_tower, 7) — the sentinel (8) resolves
  // conservatively to 7 without a seed. Default tower=7 is byte-identical
  // to the old hardcoded term.
  OP_TOWER_CRYSTALS_MET = 30,
  // OW_FLUTE_VANILLA (no operands) — true iff settings->flute_shuffle is
  // off (add-rando-ow-warp-shuffle). The CanFly neutralization conjunct:
  // CanFly bakes the eight VANILLA flute destinations into every predicate
  // referencing it; under an active flute shuffle those routes don't exist,
  // so this op compiles the macro false and the OW_FluteNet hub becomes the
  // sole flute-reachability source. NULL-settings contexts evaluate TRUE
  // (vanilla behavior). Settings-constant → reachability-monotone.
  OP_OW_FLUTE_VANILLA = 31,
  OP__COUNT = 32,
} RandoOp;

// ---------------------------------------------------------------------------
// Inventory snapshot. by_item_id[id] holds the placer's count for the given
// item registry id. Absolute items are {0,1}; progressive items count up to
// their max_count; bottle-with-contents IDs are distinct counters.
// ---------------------------------------------------------------------------
typedef struct RandoCounts {
  uint16 by_item_id[256];
  // Set only by assumed-fill/prover inventories that include synthetic nonpot
  // door-rando drops for pot-key dungeon mode. Runtime live counts leave this 0.
  uint8 pot_nonpot_drops_seeded;
  // Per-dungeon (kDoorTblDungeons index) small keys the assumed-fill collect
  // model gathered FROM rows the door oracle's key budget already credits
  // internally (active key-source pots, enemy-drop / enemy-check key sources).
  // door_oracle_get subtracts these from its held-key input so every key
  // source funds the budget exactly once — as held inventory or as an
  // oracle-internal source credit, never both. Runtime live counts and the
  // conservative fill model leave this all-zero. Sized to cover
  // kDoorTbl_DungeonCount (asserted in rando_logic.c).
  uint8 door_source_keys_collected[16];
} RandoCounts;

// Ring-aware logical item count. For a per-dungeon SmallKey outside Retro, a
// held matching Key Ring saturates the count; other item ids use their raw
// count. Retro GenericKey remains the mutually exclusive wildcard model.
uint16 Logic_EffectiveItemCount(const RandoCounts *counts,
                                const RandoSettings *settings,
                                uint16 item_id);
// The registry must stay within this fixed inventory-snapshot capacity —
// souls.c carries a _Static_assert(ITEM__COUNT <= 256) (item_ids.h is not
// visible here).

// ---------------------------------------------------------------------------
// Phase A: per-seed shuffle tables exposed to the VM.
//
// - dungeon_prize_assignment[d] = prize_id placed at dungeon d's _Prize slot.
//   Indexed by kRandoDungeon_* order from dungeon_ids.h.
//   Phase A defaults: identity (each dungeon holds its vanilla prize).
// - medallion_entrance_assignment[e] = item_id of the medallion that opens
//   entrance e. e ∈ {MiseryMire_Entrance, TurtleRock_Entrance}.
// ---------------------------------------------------------------------------
#define kRandoDungeonCount  13
#define kRandoPrizeCount    10
#define kRandoMedallionEntranceCount 2

// ---------------------------------------------------------------------------
// PredicateContext — full evaluation environment. The simple entry points
// (Predicate_Evaluate / Predicate_EvaluatePlacement) build one of these and
// call Predicate_EvalCtx.
// ---------------------------------------------------------------------------
typedef struct PredicateContext {
  const RandoCounts *counts;
  const RandoSettings *settings;

  // can_place context:
  uint16 candidate_item;
  uint16 selected_key_rings_mask;  // family bits; OP_ITEM_IS key/ring aliases
  uint8 placement_context;  // 0 = can_reach, 1 = can_place (OP_ITEM_IS allowed)

  // Per-seed shuffle tables:
  const uint8 *dungeon_prize_assignment;       // [kRandoDungeonCount]; entries are prize ids 0..9
  const uint8 *medallion_entrance_assignment;  // [kRandoMedallionEntranceCount]; entries are item ids
  // Boss-shuffle assignment: [16]; entry = boss-pool index
  // (kBoss_* in shuffle_boss.c) currently in that dungeon's boss room. NULL when
  // the placer/slot hasn't installed it — OP_CAN_KILL_BOSS then falls back to
  // kRandoDungeonVanillaBoss (vanilla boss), so reachability is unchanged.
  const uint8 *boss_assignment;                // [16]; entries are boss-pool indices

  // Per-iteration reachability state (filled by Logic_ComputeReachabilityFullKnowledge;
  // unused by standalone Predicate_Evaluate calls — pass 0 / NULL).
  uint64 cleared_dungeons_bitmask;             // bit d = dungeon d cleared
  const uint8 *reachable_regions_bitset;       // bit array of region ids; NULL = none reachable
  uint16 reachable_regions_count;              // total region count (for bound check)

  // add-rando-random-crystals (OP_TOWER_CRYSTALS_MET): the seed's RESOLVED
  // crystals.tower requirement + 1, or 0 = "builder did not resolve" — the
  // op then falls back to min(settings->crystals_tower, 7), which keeps
  // zero-initialized contexts fail-CLOSED (the sentinel reads as 7, never
  // as an open door).
  uint8 tower_crystals_required_plus1;
} PredicateContext;

// ---------------------------------------------------------------------------
// Core evaluator.
//
// Returns true iff the predicate evaluates true under ctx. Caller passes the
// raw bytecode stream and its length in bytes; the VM consumes exactly len
// bytes (a debug-build assertion verifies). Returns false on malformed
// bytecode (and asserts in debug builds).
// ---------------------------------------------------------------------------
bool Predicate_EvalCtx(const uint8 *bytecode, uint16 length,
                       const PredicateContext *ctx);

// Convenience for can_reach predicates (no candidate_item).
bool Predicate_Evaluate(const uint8 *bytecode, uint16 length,
                        const RandoCounts *counts,
                        const RandoSettings *settings);

// Convenience for can_place predicates (candidate_item register populated).
// The selected Key Ring mask is required because OP_ITEM_IS aliases the chosen
// families' SmallKey and KeyRing identities during placement.
bool Predicate_EvaluatePlacement(const uint8 *bytecode, uint16 length,
                                 const RandoCounts *counts,
                                 const RandoSettings *settings,
                                 uint16 candidate_item,
                                 uint16 selected_key_rings_mask);

// Self-check (per Rng_SelfCheck / Share_SelfCheck / Settings_SelfCheck
// pattern). Builds a minimal in-memory bytecode stream exercising each op,
// asserts known results; exits with code 2 on failure.
void Logic_SelfCheck(void);

// ---------------------------------------------------------------------------
// Reachability (task 3.8). Phase A0 stub: returns NULL. Phase A1 implements
// the fixed-point expansion. Memoized per call; caller invalidates by calling
// Rando_BumpReachabilityCounter.
// ---------------------------------------------------------------------------
typedef struct RandoReachability RandoReachability;

// add-rando-random-crystals — install the per-seed RESOLVED crystals.tower
// requirement (+1; 0 clears) consumed by OP_TOWER_CRYSTALS_MET during the
// reachability flood. Same installed-store pattern as the prize/medallion
// assignments: generation entries and the runtime activation/tracker paths
// install; deactivation clears.
void Logic_SetResolvedTowerCrystals(uint8 count_plus1);

// tracker-player-knowledge — knowledge-exclusion input for the LIVE tracker
// view (openspec randomizer-player-knowledge). The masked flood presents only
// what the player could know: regions belonging to a hidden-identity dungeon
// never enter the fixpoint (blocking onward propagation — the chains aux-exit
// pass-through channel), knowledge-tagged added edges (whirlpool pairs,
// decoupled cave exits) are skipped while undiscovered, and suppressed
// locations (undiscovered shuffled cave contents) are never reported
// reachable. Passed EXPLICITLY per call — never process-global state — and
// only the live bridge (Rando_GetLiveReachability) passes it; every
// generation/placer/selftest path uses the FullKnowledge entry point.
typedef struct RandoKnowledgeMask {
  // bit d (kRandoDungeon_* order, matching RandoRegionDef.dungeon_id):
  // dungeon d's regions are excluded from the fixpoint.
  uint16 hidden_dungeon_mask;
  // bit i = kRandoOwWhirlpools table index i: added edges tagged
  // (kKnowledgeTag_Whirlpool | i) are skipped.
  uint8 hidden_whirlpool_mask;
  // bit j = cave interior j: added edges tagged
  // (kKnowledgeTag_DecoupledExit | j) are skipped.
  uint64 hidden_exit_mask;
  // Optional bitset over location ids ((kRandoLocationCapacity+7)>>3 bytes);
  // a set bit is never reported reachable. NULL = no suppression.
  const uint8 *suppressed_locations;
} RandoKnowledgeMask;

// Knowledge tags carried by per-seed ADDED edges (Rando_AddEntranceEdgeTagged).
// 0 = untagged (never suppressed). The low bits carry the whirlpool table
// index (0..7) / cave interior index (0..63).
enum {
  kKnowledgeTag_None = 0,
  kKnowledgeTag_Whirlpool = 0x40,      // | table index (0..7)
  kKnowledgeTag_DecoupledExit = 0x80,  // | cave interior (0..63)
  kKnowledgeTag_KindMask = 0xC0,
};

// FULL-KNOWLEDGE flood — generation/placer/selftest ONLY. Player-facing
// surfaces must consume the knowledge-limited live bridge instead
// (Rando_GetLiveReachability); check_knowledge_consumers.py enforces the
// split. (Renamed from Logic_ComputeReachability by tracker-player-knowledge
// so a new call site must consciously pick a side.)
const RandoReachability *Logic_ComputeReachabilityFullKnowledge(const RandoCounts *counts,
                                                   const RandoSettings *settings);
// Knowledge-limited flood: identical to FullKnowledge when mask is NULL
// (byte-for-byte — the empty-mask short-circuit), else applies the exclusions
// above. Only the live tracker bridge passes a non-NULL mask.
const RandoReachability *Logic_ComputeReachabilityMasked(const RandoCounts *counts,
                                                         const RandoSettings *settings,
                                                         const RandoKnowledgeMask *mask);
// Continue the previous reachability fixed-point after only increasing item
// counts under the same settings/shuffle state. Used by soul collection during
// assumed fill; Logic_SelfCheck enforces that generated reachability predicates
// stay monotone in inventory/reachability so preserved bits cannot become stale.
// Generation-only (full knowledge), like the FullKnowledge entry point.
const RandoReachability *Logic_ExpandReachability(const RandoCounts *counts,
                                                  const RandoSettings *settings);
// tracker-player-knowledge structural probe: masked ⊆ full, exclusion, and
// full-flood purity after a masked flood. Exits(2) on failure.
void Logic_KnowledgeMaskSelfCheck(void);

// Diagnostic-only generation work counter. The generator brackets one run
// with Begin/Freeze; the fixed-point loop counts completed expansion passes
// while active. This state is intentionally absent from every canonical seed,
// spoiler, share-string, and digest structure.
void Logic_WorkCounterBegin(void);
uint64 Logic_WorkCounterFreeze(void);

bool Reachability_HasLocation(const RandoReachability *r, uint16 location_id);
bool Reachability_HasRegion(const RandoReachability *r, uint16 region_id);

// Copy the last Logic_ComputeReachabilityFullKnowledge result into a private snapshot buffer
// (refresh=true) and return it, or return the existing snapshot unchanged
// (refresh=false). The snapshot survives later Logic_ComputeReachabilityFullKnowledge calls,
// which only touch the shared result buffer. Used by the runtime tracker bridge
// to hold a stable reachability across frames / multiple windows.
const RandoReachability *Reachability_Snapshot(bool refresh);

// ---------------------------------------------------------------------------
// Generated-data record types (emitted by assets/rando_logic_gen.py into
// src/rando/logic_data.c). Declared here so other compilation units can
// reference the table without redeclaring the type.
// ---------------------------------------------------------------------------

typedef struct RandoLocationDef {
  uint16 id;
  uint16 vanilla_item_id;
  uint16 region_id;            // index into kRandoRegions, or 0xFFFF when location lacks logic.yaml region binding
  uint16 _pad0;
  uint32 can_reach_offset;     // offset into kRandoPredicateStream
  uint16 can_reach_length;
  uint32 can_place_offset;
  uint16 can_place_length;
  uint32 always_allow_offset;
  uint16 always_allow_length;
  uint8 type;                  // location-type enum (Chest/BigChest/.../Medallion);
                               // shop-class ordinals shared below (LOCTYPE_*)
  uint8 world_state_filter;    // bitmask: 0=all worlds, else bit per world-state
} RandoLocationDef;

const RandoLocationDef *Rando_FindLocationDef(uint16 location_id);

// logic.schema.yaml location-type ordinals that are referenced across modules.
// Shared here so ordinals can't drift between the placer, spoiler emitters,
// trackers, and hint generator. Other type ordinals stay local to
// rando_placement.c while they are single-site.
enum {
  LOCTYPE_Prize_Event = 12,  // virtual event trigger, not an item check
  LOCTYPE_Medallion   = 13,  // MM/TR medallion config slot, not an item check
  LOCTYPE_Shop        = 14,  // Retro regular shop slot (identity-pinned inventory)
  LOCTYPE_ShopUpgrade = 15,  // Retro capacity-upgrade slot (identity-placed)
  LOCTYPE_TakeAny     = 16,  // Retro take-any cave slot
  LOCTYPE_Pot         = 17,  // Dungeon pot check (per-tier active subset)
  LOCTYPE_EnemyDrop   = 18,  // Forced enemy key-drop check (setting-active subset)
  LOCTYPE_Enemy       = 19,  // Ordinary dungeon enemy check.
  LOCTYPE_Grass       = 20,  // Overworld bush/thick-grass check (add-rando-grass-rock-shuffle)
  LOCTYPE_Rock        = 21,  // Overworld light/heavy rock check (add-rando-grass-rock-shuffle)
  LOCTYPE_Bonk        = 22,  // Placed OW bonk-item sprite check (add-rando-bonk-sanity)
};

static inline bool Rando_LocationTypeCountsAsCheck(uint8 type) {
  return type != LOCTYPE_Prize_Event && type != LOCTYPE_Medallion;
}


typedef struct RandoRegionDef {
  uint16 id;
  uint16 parent_id;            // 0xFFFF = no parent
  uint8 dungeon_id;            // 0xFF = not a dungeon
  uint8 world_state_filter;
} RandoRegionDef;

typedef struct RandoEdgeDef {
  uint16 from_region;
  uint16 to_region;
  uint32 predicate_offset;
  uint16 predicate_length;
  uint8 one_way;
  uint8 _pad;
} RandoEdgeDef;

// add-rando-shopsanity — does this location's world_state_filter accept the
// active world state? ONE definition for the junk-pad target, the
// open-location collection, Placement_SelfCheck's expected count, and the
// reachability expansion, so the four can't drift. Shopsanity overrides the
// Retro-only filter on the 27 regular shop slots: with the axis on they are
// live fill locations in EVERY world state. ShopUpgrade/TakeAny keep their
// plain Retro filter. The world_state >= 32 guard keeps the shift defined if
// a caller ever bypasses Settings_Validate (unknown state = filter never
// matches, matching the prior reachability-site behavior).
static inline bool Rando_LocationWorldStateActive(const RandoLocationDef *loc,
                                                  const RandoSettings *settings) {
  uint8 wsf = loc->world_state_filter;
  if (wsf == 0) return true;
  if (settings->world_state < 32 &&
      (wsf & (1u << settings->world_state)) != 0) return true;
  // Settings_ShopsanityActive, not the raw field: the placer consumes
  // un-normalized settings, and cave-entrance shuffle forces the axis off.
  return loc->type == LOCTYPE_Shop && Settings_ShopsanityActive(settings);
}

extern const RandoLocationDef kRandoLocations[];
extern const uint32 kRandoLocationsCount;
// add-rando-grass-rock-shuffle — index of the first terrain (Grass/Rock)
// location in the id-sorted kRandoLocations[]; terrain is a contiguous suffix
// (codegen-asserted), so a loop that does not need inactive terrain rows can
// bound by this instead of kRandoLocationsCount.
extern const uint32 kRandoNonTerrainLocationsCount;

// add-rando-ow-warp-shuffle raised 256 -> 512 (the ~211 OW screen-component
// regions push the total past 256; regions at/above the cap are SILENTLY
// never walked — the codegen budget assert + Logic_SelfCheck guard make that
// loud). The TOP id (kReachabilityMaxRegions - 1) is RESERVED as the
// entrance cross-void sink (kCrossVoidRegion, shuffle_entrance.c) and may
// never be a real region: codegen budgets regions to cap - 1. Keep in sync
// with rando_logic_gen.py's _REACHABILITY_MAX_REGIONS. Memory:
// g_inverted_pair_set grows 8KB -> 32KB.
#define kReachabilityMaxRegions 512

// --- add-rando-ow-warp-shuffle — OW screen-component substrate --------------
// The warp regions (components + the OW_FluteNet hub) are the contiguous
// TRAILING region-index suffix starting at kRandoOwRegionsFirst, and the
// static warp edges are the trailing kRandoEdges suffix: when no warp axis
// is active the reachability edge scan bounds at kRandoNonWarpEdgesCount and
// the substrate costs nothing (the terrain inert-suffix pattern, edge-side).
// kRandoOwGraphPresent/Digest gate generation + slot activation fail-closed
// (assetless builds refuse warp seeds; drifted graphs refuse certified
// slots — the chest_lookup fail-open class, closed explicitly).
extern const uint32 kRandoOwRegionsFirst;
extern const uint32 kRandoNonWarpEdgesCount;
extern const uint8 kRandoOwGraphPresent;
extern const uint32 kRandoOwGraphDigest;
extern const uint16 kRandoOwFluteHubRegion;  // 0xFFFF when graph absent

// One flute candidate per eligible LW screen (ported upstream flute_data):
// the arrival tableau columns feed the runtime resolver verbatim; icon_y/x
// are upstream-map-space (cross-check only — the fork's map blips are
// DERIVED, see shuffle_ow_warp).
typedef struct RandoOwFluteCandidate {
  uint8 screen;      // LW overworld screen id
  uint16 region;     // component region index (kRandoRegions)
  uint8 forced;      // upstream Desert/Mire-guarantee forced entry
  uint16 slot, vram, bg_y, bg_x, link_y, link_x, cam_y, cam_x;
  uint16 unk1, unk2; // engine arrival unknowns (kBirdTravel_Unk1/Unk3 class)
  uint16 icon_y, icon_x;
  uint16 sector;     // vanilla-connectivity sector (spot spread; flood
                     // reproduced per upstream build_sectors, portals excl.)
} RandoOwFluteCandidate;
extern const RandoOwFluteCandidate kRandoOwFluteCandidates[];
extern const uint32 kRandoOwFluteCandidatesCount;
// Balanced-mode pairwise conflict bitmask (upstream getIgnored port): row i
// bit j = candidates i,j too close to both host spots.
extern const uint32 kRandoOwFluteConflicts[][2];
extern const uint32 kRandoOwFluteConflictWords;
extern const uint8 kRandoOwVanillaFluteScreens[8];

typedef struct RandoOwWhirlpool {
  uint8 screen;          // source screen (kWhirlpoolAreas identity)
  uint8 partner_screen;  // vanilla partner's source screen
  uint16 region;         // water component region index
  uint16 partner_region;
} RandoOwWhirlpool;
extern const RandoOwWhirlpool kRandoOwWhirlpools[];
extern const uint32 kRandoOwWhirlpoolsCount;

// ---------------------------------------------------------------------------
// Single location-id ceiling for the whole randomizer module.
//
// EVERY array, bitmap, loop bound, or guard keyed by a location_id MUST size /
// bound by this constant — never a bare literal (512/1024). The static registry
// is append-only and grows over time (328 baseline + 835 pot-sanity pots +
// enemy rows to id 1999 + ~3943 grass/rock terrain rows at base 2048 —
// add-rando-grass-rock-shuffle); 8192 leaves headroom. Because every consumer
// routes through this one name, a registry that ever exceeds capacity is a
// SINGLE build break — the codegen `_Static_assert(LOC__COUNT <= 8192)` in
// location_ids.h plus the `_Static_assert(LOC__COUNT <= kRandoLocationCapacity)`
// name-ties in rando.c / rando_placement.c — not a silent overflow /
// truncation / drop (fail-open). Keep the 8192 in location_ids.h's codegen
// assert (rando_logic_gen.py) in lockstep with this value.
//
// CHANGING THIS VALUE: the Makefile has NO header-dependency tracking, so an
// incremental `make` after a bump ships TUs compiled against DIFFERENT
// capacities — mixed-ABI location arrays that historically passed selftest
// while producing a 16.8 GB runaway spoiler (enemy-drop review). ALWAYS
// `make clean`, and the per-TU capacity probes below let --rando-selftest
// catch a stale object loudly (Rando_SelfCheckCapacityABI in rando.c).
// ---------------------------------------------------------------------------
#define kRandoLocationCapacity 8192
extern const RandoRegionDef kRandoRegions[];
extern const uint32 kRandoRegionsCount;

// Cross-TU capacity ABI probes: every TU that sizes an array by
// kRandoLocationCapacity defines one (C++ TUs with extern "C") returning its
// compiled-in view of the constant; Rando_SelfCheckCapacityABI (rando.c,
// --rando-selftest) asserts they all agree. ADD a probe (definition +
// declaration here + checker row) when a new TU starts sizing arrays by the
// constant.
#define RANDO_DEFINE_CAPACITY_PROBE(tu) \
  uint32 RandoCapacityProbe_##tu(void) { return kRandoLocationCapacity; }
uint32 RandoCapacityProbe_rando(void);
uint32 RandoCapacityProbe_auto_tracker(void);
uint32 RandoCapacityProbe_rando_generate(void);
uint32 RandoCapacityProbe_rando_hints(void);
uint32 RandoCapacityProbe_rando_logic(void);
uint32 RandoCapacityProbe_rando_placement(void);
uint32 RandoCapacityProbe_rando_save(void);
uint32 RandoCapacityProbe_rando_snapshot_tail(void);
uint32 RandoCapacityProbe_rando_spoiler(void);
#ifdef Z3R_NATIVE_SETTINGS_WINDOW
uint32 RandoCapacityProbe_rando_reach_panel(void);
uint32 RandoCapacityProbe_tracker_windows(void);
#endif
extern const RandoEdgeDef kRandoEdges[];
extern const uint32 kRandoEdgesCount;

// Cave entrance-shuffle source predicates. Index-matched to
// shuffle_entrance.c::kCaveInteriors; len 0 means the source region alone is
// sufficient to enter the cave slot.
typedef struct RandoCaveSourcePred {
  uint32 off;
  uint16 len;
} RandoCaveSourcePred;
extern const RandoCaveSourcePred kRandoCaveSourcePreds[];
extern const uint32 kRandoCaveSourcePredsCount;
extern const uint8 kRandoPredicateStream[];
extern const uint32 kRandoPredicateStreamSize;

// Boss-shuffle runtime — OP_CAN_KILL_BOSS dispatch tables (emitted by
// assets/rando_logic_gen.py). Indexed by boss-pool index (kBoss_* in
// src/rando/shuffle_boss.c): 0=Armos, 1=Lanmolas, 2=Moldorm, 3=Agahnim,
// 4=Helmasaur, 5=Arrghus, 6=Mothula, 7=Blind, 8=Kholdstare, 9=Vitreous,
// 10=Trinexx, 11=Agahnim2. Each entry points at that boss's kill predicate in
// kRandoPredicateStream (the same compiled bytecode the inline CanKill<Boss>
// macro produces, so the off-shuffle resolution is byte-identical).
typedef struct RandoBossKillPred {
  uint32 offset;   // into kRandoPredicateStream
  uint16 length;   // bytes; 0 = no predicate (always false)
} RandoBossKillPred;
extern const RandoBossKillPred kRandoBossKillPred[];
extern const uint32 kRandoBossKillPredCount;
// dungeon-id (HCE=0..GT=12) → vanilla boss-pool index, or 0xFF for HCE/unused.
// Mirrors shuffle_boss.c kBossVanilla; used by OP_CAN_KILL_BOSS when no per-seed
// boss assignment is installed. BossShuffle_SelfCheck (shuffle_boss.c) element-
// wise cross-checks this against kBossVanilla — both run via
// Rando_RunAllSelfChecks (the check lives there, not in Logic_SelfCheck).
extern const uint8 kRandoDungeonVanillaBoss[kRandoDungeonCount];

// add-enemy-souls — kill-gated-room soul data baked into logic_data.c from the
// local soul_rooms.gen.yaml. kRandoSoulRoomsBaked == 0 means the build carries
// no kill-room soul requirements: BuildItemPool refuses souls_shuffle=all
// seeds (fail closed, mirroring the pot/enemy-drop registries).
// kRandoSoulPinRooms lists the kill-gated + forced key-drop rooms whose logic
// terms / runtime drop exemptions name VANILLA species; EnemyShuffle_
// PickDungeon returns the vanilla type for these rooms while the enemies tier
// is active (Souls_RoomPinnedVanilla, souls.c).
extern const uint8 kRandoSoulRoomsBaked;
extern const uint16 kRandoSoulPinRooms[];
extern const uint16 kRandoSoulPinRoomsCount;

// ---------------------------------------------------------------------------
// §12.6 — per-trick / per-glitch-level ROM-version verification status.
// ALTTPR targets the Japanese 1.0 ROM; this fork targets US 1.0. A trick that
// exists in ALTTPR's logic graph may have JP/US timing/mechanic differences or
// be absent on US 1.0. The generator consults these tables to warn (in the
// spoiler `fallback_warnings`) when a seed enables an unverified trick / level.
// The string→int encoding is produced by ROM_VER_STATUS in rando_logic_gen.py.
// ---------------------------------------------------------------------------
typedef enum {
  kRomVerStatus_UntestedUs10  = 0,  // in the upstream graph; not confirmed on US 1.0 (default)
  kRomVerStatus_VerifiedUs10  = 1,  // performed end-to-end on a real US 1.0 build
  kRomVerStatus_CrossVersion  = 2,  // pure player skill / verified identical JP↔US
  kRomVerStatus_Jp10Only      = 3,  // confirmed NOT to work on US 1.0 (codegen forbids in gates)
  kRomVerStatus_Us10Different = 4,  // exists on both but with different timing/mechanics
} RandoRomVerStatus;

typedef struct RandoTrickStatus {
  uint8 bit;                  // settings.tricks bit position
  uint8 rom_version_status;   // RandoRomVerStatus
  const char *id;             // kebab-case trick id (e.g. "pearl-bypass")
} RandoTrickStatus;
extern const RandoTrickStatus kRandoTrickStatus[];
extern const uint32 kRandoTrickStatusCount;

typedef struct RandoGlitchLevelStatus {
  uint8 level;                // settings.logic value (1=OverworldGlitches, 2=MajorGlitches, ...)
  uint8 rom_version_status;   // RandoRomVerStatus
  const char *id;             // e.g. "overworld_glitches"
} RandoGlitchLevelStatus;
extern const RandoGlitchLevelStatus kRandoGlitchLevelStatus[];
extern const uint32 kRandoGlitchLevelStatusCount;

// Phase B Slice 2 — per-world-state location-predicate override.
// When `settings.world_state == kWorldState_Inverted` (or Retro, future),
// the runtime VM looks up (location_id) in the per-world-state override
// table and uses its (cr/cp/aa) offsets instead of the base ones in
// `kRandoLocations`. Tables are sorted by location_id for binary search.
//
// `region_override`: 0xFFFF = "no region change" (use base region from
// `kRandoLocations[loc].region_id`). Otherwise, the runtime treats this
// location as belonging to `region_override` for reachability gating —
// e.g., Ether Tablet (id 194) moves from LW-DM-West (Standard) to
// LW-DM-East (Inverted) per PHP `Inverted/LightWorld/DeathMountain/East.php:25-26`.
typedef struct RandoLocationPredOverride {
  uint16 location_id;
  uint16 region_override;        // 0xFFFF = use base region_id
  uint32 can_reach_offset;
  uint16 can_reach_length;
  uint32 can_place_offset;
  uint16 can_place_length;
  uint32 always_allow_offset;
  uint16 always_allow_length;
} RandoLocationPredOverride;

extern const RandoLocationPredOverride kRandoLocationPredOverrides_Inverted[];
extern const uint32 kRandoLocationPredOverrides_InvertedCount;
extern const RandoLocationPredOverride kRandoLocationPredOverrides_Retro[];
extern const uint32 kRandoLocationPredOverrides_RetroCount;

// Per-world-state edges. Each non-Standard world-state with extra cross-
// region edges (Inverted's mirror-back, DarkWorld entry edges) emits its
// own table. The base `kRandoEdges` is the Standard graph.
extern const RandoEdgeDef kRandoEdges_Inverted[];
extern const uint32 kRandoEdges_InvertedCount;

// Lookup the predicate override for `loc_id` under `world_state`. Returns
// NULL when no override is installed (caller uses the base predicate from
// kRandoLocations[loc_id]). Binary search over the sorted override table.
const RandoLocationPredOverride *Rando_FindPredicateOverride(uint16 loc_id,
                                                             uint8 world_state);

// Human-readable name lookups (used by spoiler / debug output).
// Both return a pointer to a static string in the generated logic_data.c.
// Never returns NULL — falls back to "(unknown)" / "(unbound)" on miss.
const char *Rando_GetRegionName(uint16 region_id);
const char *Rando_GetLocationName(uint16 location_id);
const char *Rando_GetItemName(uint16 item_id);

// Semantic grant metadata generated from every `dispatch:` token in
// assets/rando/item_registry.yaml. `payload` is the receive code for Receive,
// the registry item id for identity-sensitive direct classes, or zero when the
// opcode fully describes the operation. Grant planning uses this table instead
// of parallel hand-maintained item-id ranges.
typedef enum RandoGrantOpcode {
  kRandoGrantOp_Invalid = 0,
  kRandoGrantOp_Receive,
  kRandoGrantOp_ProgressiveSword,
  kRandoGrantOp_ProgressiveShield,
  kRandoGrantOp_ProgressiveArmor,
  kRandoGrantOp_ProgressiveGlove,
  kRandoGrantOp_ProgressiveBow,
  kRandoGrantOp_DungeonSmallKey,
  kRandoGrantOp_DungeonBigKey,
  kRandoGrantOp_DungeonMap,
  kRandoGrantOp_DungeonCompass,
  kRandoGrantOp_DirectTriforcePiece,
  kRandoGrantOp_DirectMagicUpgrade,
  kRandoGrantOp_DirectPrize,
  kRandoGrantOp_DirectKeyRing,
  kRandoGrantOp_DirectSkeletonKey,
  kRandoGrantOp_DirectGenericKey,
  kRandoGrantOp_DirectRupoor,
  kRandoGrantOp_DirectTrap,
  kRandoGrantOp_DirectNothing,
  kRandoGrantOp_DirectSoul,
  kRandoGrantOp_DirectBombCapacity,
  kRandoGrantOp_DirectArrowCapacity,
  kRandoGrantOp_Virtual,
} RandoGrantOpcode;

typedef struct RandoItemGrantMetadata {
  uint8 opcode;
  uint16 payload;
} RandoItemGrantMetadata;

extern const RandoItemGrantMetadata kRandoItemGrantMetadata[];
extern const uint16 kRandoItemGrantMetadataCount;

// Compatibility accessor for semantic plans whose generated opcode is
// kRandoGrantOp_Receive. Returns 0xFF for progressive/direct/no-op/virtual
// plans and for out-of-range ids. Gameplay transaction code must never treat
// 0xFF as permission to grant the source location's vanilla item.
uint8 Rando_VanillaItemForRegistryId(uint16 registry_item_id);

// Start region per world_state. Indexed by WorldState enum (Open=0,
// Standard=1, Inverted=2, Retro=3). Value is a region id (index into
// kRandoRegions) — the reachability fixed-point seeds from this region.
// A value of 0xFFFF means "no start region declared for this world_state";
// the graph is treated as empty (no locations reachable).
extern const uint16 kRandoStartRegionByWorldState[4];

// Look up the numeric region id by string name. Returns 0xFFFF when not
// found. O(N) linear scan; fine for the few authoring-time uses.
uint16 Rando_FindRegionByName(const char *name);

// NB: the Phase A `RegionRemap` scaffold (RegionRemap_Lookup /
// Rando_SetRegionRemap / Rando_ResetRegionRemap) was RETIRED in Phase C
// entrance shuffle. It was dead code (0 install callers, identity in every
// shipped seed) AND the wrong abstraction — it remapped an OP_REGION_REACHABLE
// *region operand*, whereas entrance shuffle rewires which interior a
// door-*edge* terminates at. Caves now use a per-seed location-region override
// (see rando_logic.c `Rando_SetEntranceRegionOverrides`); dungeons (Stage 2)
// will use a per-seed edge overlay mirroring kRandoEdges_Inverted. See
// openspec/changes/add-rando-entrance-shuffle/design.md §1.

// Phase C entrance shuffle — per-seed cave location-region overrides. Begin
// resets to identity + activates; Set assigns one location's effective region;
// Clear deactivates (restoring byte-identical reachability). Driven by
// shuffle_entrance.c from the entrance permutation.
void Rando_BeginEntranceRegionOverrides(void);
void Rando_SetEntranceRegionOverride(uint16 loc_id, uint16 region_id);
void Rando_ClearEntranceRegionOverrides(void);
// Returns the current per-seed override region for a location, or 0xFFFF if
// none/inactive. For the self-check + tracker.
uint16 Rando_GetEntranceRegionOverride(uint16 loc_id);

// Phase C entrance shuffle (Stage 2) — per-seed DUNGEON edge overlay. Begin
// resets to identity + activates; Set remaps a door-edge whose destination is
// `old_to_region` to land at `new_to_region` instead (keyed by the dungeon entry
// region); Clear deactivates; Get resolves (returns the input when inactive/none).
void Rando_BeginEntranceEdgeOverrides(void);
void Rando_SetEntranceEdgeOverride(uint16 old_to_region, uint16 new_to_region);
void Rando_ClearEntranceEdgeOverrides(void);
uint16 Rando_GetEntranceEdgeOverride(uint16 to_region);
// True while edge overrides / added edges are active (between Begin and Clear).
// Lets the decoupled exit-edge pass add edges on top of an already-begun dungeon/
// cross edge set without re-Begin (which would wipe them).
bool Rando_EntranceEdgeOverridesActive(void);

// Phase C entrance shuffle (Stage 3 / cross-category) primitives.
// SetEntranceRegionOverridePred: like the region override, but ALSO AND a
// predicate (pred_off/pred_len into kRandoPredicateStream; len 0 = none) into the
// cave-location's reachability — for a cave behind a gated source door so it
// inherits the door's requirement. Reset by Rando_BeginEntranceRegionOverrides.
void Rando_SetEntranceRegionOverridePred(uint16 loc_id, uint16 region_id,
                                         uint32 pred_off, uint16 pred_len);
// AddEntranceEdge: add a per-seed edge from_region → to_region (pred_len 0 =
// unconditional) — for a dungeon behind a cave door. Reset by
// Rando_BeginEntranceEdgeOverrides; walked when edge overrides are active.
void Rando_AddEntranceEdge(uint16 from_region, uint16 to_region,
                           uint32 pred_off, uint16 pred_len);
// tracker-player-knowledge — tagged variant: `knowledge_tag` (kKnowledgeTag_*)
// lets the masked live flood skip the edge while its observable (whirlpool
// pair / decoupled exit) is undiscovered. Tag 0 == the plain form. Inert for
// generation (no mask ⇒ tags never consulted).
void Rando_AddEntranceEdgeTagged(uint16 from_region, uint16 to_region,
                                 uint32 pred_off, uint16 pred_len,
                                 uint8 knowledge_tag);
int Rando_GetEntranceAddedEdgeCount(void);
// Budget introspection for the combined-consumer selfcheck: edges dropped by
// the cap since the last Begin (must stay 0) and the store capacity.
int Rando_GetEntranceAddedEdgeDropped(void);
int Rando_EntranceAddedEdgeCapacity(void);

// ---------------------------------------------------------------------------
// Door shuffle (add-rando-door-shuffle) — logic-side install + generated data.
// ---------------------------------------------------------------------------

// Compiled fork-DSL predicates referenced by door-table rule-blob Vm leaves
// (emitted by rando_logic_gen.py from assets/rando/door_predicates.gen.json,
// index-matched to that manifest).
typedef struct RandoDoorVmPred {
  uint32 off;
  uint16 len;
} RandoDoorVmPred;
extern const RandoDoorVmPred kDoorVmPreds[];
extern const uint32 kDoorVmPredsCount;

// Portal seeding gates (door_portals.yaml): the oracle floods a dungeon from
// exactly the portal lobbies whose fork_region bit is set in the current
// reachability bitset AND whose optional predicate holds. fork_region 0xFFFF
// = never independently enterable (TR ledge doors).
typedef struct RandoDoorPortalGate {
  uint8 dungeon;       // kDoorTblDungeons index
  uint8 is_drop;
  uint16 door_region;  // door-table region id of the lobby
  uint16 fork_region;  // fork logic region id, 0xFFFF = never
  uint32 pred_off;     // extra gate predicate, len 0 = none
  uint16 pred_len;
} RandoDoorPortalGate;
extern const RandoDoorPortalGate kDoorPortalGates[];
extern const uint32 kDoorPortalGatesCount;

// Door x pot-shuffle bridge (generated from local pot artifacts). Pots are not
// in kDoorTblLocations, so the door oracle/prover need this table to bind an
// active pot check to one or more door-table regions and to the pot's base
// (pre-pot-key-depth) predicate.
enum {
  kDoorPot_KeySource = 0x01,  // non-empty pot; can host shuffled small keys
  kDoorPot_KeyPot    = 0x02,  // vanilla item is a dungeon small key
  kDoorPot_Empty     = 0x04,  // ITEM_Nothing filler; active only at All tier
};

typedef struct RandoDoorPotLocation {
  uint16 loc_id;
  uint16 region_first;
  uint32 pred_off;
  uint16 pred_len;
  uint16 drop_index;    // kDoorTblDropKeys index, or 0xFFFF for non-key pots
  uint8 dungeon;        // kDoorTblDungeons index
  uint8 min_tier;       // PotShuffle tier where this row first becomes active
  uint8 flags;          // kDoorPot_* bits
  uint8 region_count;
} RandoDoorPotLocation;
extern const RandoDoorPotLocation kRandoDoorPotLocations[];
extern const uint16 kRandoDoorPotRegions[];
extern const uint32 kRandoDoorPotLocationsCount;
extern const uint32 kRandoDoorPotBridgeDigest;

static inline bool Rando_DoorPotActive(const RandoDoorPotLocation *p, uint8 pot_tier) {
  return p != NULL && pot_tier >= p->min_tier && pot_tier <= kPotShuffle_All;
}

// Door x enemy-drop bridge. Enemy forced-key checks are also not
// kDoorTblLocations, so the door oracle/prover need the generated door region
// and original DROP index for each active small-key enemy-drop check.
typedef struct RandoDoorEnemyDropLocation {
  uint16 loc_id;
  uint16 region;
  uint32 pred_off;
  uint16 pred_len;
  uint16 drop_index;
  uint8 dungeon;
} RandoDoorEnemyDropLocation;
extern const RandoDoorEnemyDropLocation kRandoDoorEnemyDropLocations[];
extern const uint32 kRandoDoorEnemyDropLocationsCount;
extern const uint32 kRandoDoorEnemyDropBridgeDigest;

// Door x ordinary enemy-check bridge. Ordinary dungeon/all-tier enemy checks
// are separate locations, so the door oracle/prover need generated door
// regions and original base predicates for each active check.
typedef struct RandoDoorEnemyCheckLocation {
  uint16 loc_id;
  uint16 region_first;
  uint32 pred_off;
  uint16 pred_len;
  uint8 dungeon;
  uint8 min_tier;
  uint8 region_count;
} RandoDoorEnemyCheckLocation;
extern const RandoDoorEnemyCheckLocation kRandoDoorEnemyCheckLocations[];
extern const uint16 kRandoDoorEnemyCheckRegions[];
extern const uint32 kRandoDoorEnemyCheckLocationsCount;
extern const uint32 kRandoDoorEnemyCheckBridgeDigest;

static inline bool Rando_DoorEnemyCheckActive(const RandoDoorEnemyCheckLocation *e,
                                              uint8 enemy_check_tier) {
  return e != NULL && enemy_check_tier >= e->min_tier &&
         enemy_check_tier <= kEnemyDropChecks_All;
}

// Install/clear the per-seed door layout for logic evaluation (generation
// installs before Place_AssumedFill; slot activation installs the regenerated
// layout; teardown clears). `layout` must outlive the install. active_mask
// bit = kDoorTblDungeons index actually shuffled (pins excluded by caller).
struct DoorShuffleLayout;
void Rando_SetDoorLogicLayout(const struct DoorShuffleLayout *layout, uint16 active_mask);
const struct DoorShuffleLayout *Rando_GetDoorLogicLayout(uint16 *active_mask_out);

// Single-count support for the collect-model fill: returns the
// kDoorTblDungeons index whose door-oracle key budget internally credits
// `loc_id` as a small-key source under the INSTALLED layout (active
// key-source pot / enemy-drop / enemy-check bridge row of a shuffled dungeon)
// when `item_id` is that dungeon's small key; 0xFF otherwise (including when
// no layout is installed). Callers record collected keys from credited rows
// in RandoCounts.door_source_keys_collected so the oracle's held-key input
// stays disjoint from its internal source credits.
uint8 Rando_DoorKeySourceDungeon(uint16 loc_id, uint16 item_id);

#endif  // ZELDA3_RANDO_LOGIC_H_
