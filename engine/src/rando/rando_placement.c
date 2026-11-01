// rando_placement.c — item pool, placement table, digest (tasks.md §4.1, §4.4, §6.1).
//
// Phase A0 scope:
//   - BuildItemPool returns a minimal "identity" pool: every location's
//     vanilla_item, in registry order. Phase A1 replaces with real per-settings
//     construction (progressive vs absolute, dungeon-item modes, etc.).
//   - Place_AssumedFill returns an identity placement (every location ←
//     its vanilla_item_id). Phase A1 replaces with the assumed-fill algorithm.
//   - PlacementTable_ComputeDigest emits SHA-256 over the canonical
//     serialization — this works at A0 and is what the regression corpus diffs.
//   - Rando_OnLocationCheck (in rando.c) consults the active placement table
//     once one is installed via Placement_Install.

#include "rando_placement.h"
#include "rando.h"
#include "alttpr_import.h"
#include "rando_logic.h"
#include "shuffle_doors.h"  // door-shuffle bk_restricted placement ban
#include "rando_rng.h"
#include "rando_shuffles.h"
#include "shuffle_boss.h"   // BossShuffle_ComputeAssignment (boss-shuffle reachability)
#include "customizer.h"     // Customizer_GetActive (customizer-mode pins)
#include "item_ids.h"
#include "location_ids.h"
#include "dungeon_ids.h"
#include "enemy_drop_lookup.h"
#include "bonk_lookup.h"    // add-rando-bonk-sanity selfcheck sortedness probe
#include "enemy_check_lookup.h"
#include "pot_nonpot_drop_counts.h"
#include "souls.h"        // add-enemy-souls (Souls_ItemIsSoul)
#include "soul_tables.h"  // kSoulCount / kSoulBossCount (pool construction)
#include "npc_soul_tables.h"  // kNpcSoulCount (add-npc-souls pool)
#include "../types.h"
#include "third_party/sha256/sha256.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Generated tables — RandoLocationDef typedef + kRandoLocations[] /
// kRandoLocationsCount extern come from rando_logic.h.

// ---------------------------------------------------------------------------
// Active placement table — set by Placement_Install. Phase A0: starts NULL,
// meaning Rando_OnLocationCheck returns vanilla_item_id for every call
// (effectively rando-inactive).
// ---------------------------------------------------------------------------
static const RandoPlacementTable *g_active_placement = NULL;
// Placement_Lookup binary-searches the active table by location_id, which
// requires the table sorted ascending. Every producer
// emits in that order (assumed-fill's step-7 loop walks open_loc_idx, a subset
// of the id-sorted kRandoLocations; the sidecar/snapshot install memcpys that
// order through). We still VERIFY sortedness once per install (O(N)) and fall
// back to a linear scan if a future producer breaks it — correctness never
// depends on the invariant, only speed does. The --rando-selftest sortedness
// assert guards the assumed-fill path so the fast path is the one exercised.
static bool g_active_placement_sorted = false;

void Placement_Install(const RandoPlacementTable *t) {
  g_active_placement = t;
  g_active_placement_sorted = true;
  if (t != NULL) {
    for (uint16 i = 1; i < t->count; i++) {
      if (t->entries[i].location_id < t->entries[i - 1].location_id) {
        g_active_placement_sorted = false;
        break;
      }
    }
  }
}

const RandoPlacementTable *Placement_GetActive(void) {
  return g_active_placement;
}

// Is the active placement table sorted by location_id (so Placement_Lookup
// binary-searches)? Exposed for the --rando-selftest sortedness assert.
bool Placement_ActiveIsSorted(void) {
  return g_active_placement_sorted;
}

// ---------------------------------------------------------------------------
// Presence-aware dispatch lookup. Grant transactions must distinguish a table
// entry whose item equals the source's vanilla item from a location that is not
// present at all. Placement_Lookup below retains the legacy fallback wrapper.
// ---------------------------------------------------------------------------
bool Placement_TryLookup(uint16 location_id, uint16 *out_item_id) {
  const RandoPlacementTable *t = g_active_placement;
  if (t == NULL) return false;
  // Binary search the sorted-by-location_id table.
  // At ~1163 active locations (pot/enemy location-expansion tiers) a
  // per-location-check linear scan would
  // be too slow for per-pot break dispatch; O(log N) keeps it cheap.
  if (g_active_placement_sorted) {
    uint16 lo = 0, hi = t->count;
    while (lo < hi) {
      uint16 mid = (uint16)(lo + (hi - lo) / 2u);
      uint16 mloc = t->entries[mid].location_id;
      if (mloc == location_id) {
        if (out_item_id != NULL) *out_item_id = t->entries[mid].item_id;
        return true;
      }
      if (mloc < location_id) lo = (uint16)(mid + 1u);
      else hi = mid;
    }
    return false;
  }
  // Fallback for an (unexpected) unsorted table: linear scan stays CORRECT.
  for (uint16 i = 0; i < t->count; i++) {
    if (t->entries[i].location_id == location_id) {
      if (out_item_id != NULL) *out_item_id = t->entries[i].item_id;
      return true;
    }
  }
  return false;
}

uint16 Placement_Lookup(uint16 location_id, uint16 vanilla_item_id) {
  uint16 placed;
  if (Placement_TryLookup(location_id, &placed))
    return placed;
  return vanilla_item_id;
}

// ---------------------------------------------------------------------------
// BuildItemPool — per-settings construction (tasks.md §4.1).
//
// Phase A1 implementation. The pool is built from:
//
//   1. Progression items per mode.weapons (progressive vs. absolute weapon set)
//   2. Common non-weapon equipment (boots, flippers, moon pearl, ...)
//   3. Dungeon items per dungeon_items.{small_keys,big_keys,maps,compasses} mode
//      - Vanilla : NOT added to pool (placed at vanilla locations)
//      - Dungeon : added to pool with per-location placement restrictions
//      - Wild    : added to pool, no per-location restrictions
//   4. Bottles (default 4; capped at link_bottle_info[4] slots total)
//   5. Magic upgrades (HalfMagic, QuarterMagic)
//   6. Heart items (PieceOfHeart, BossHeartContainer)
//   7. Multi-tier rupees
//   8. Junk pool (SmallMagic, Arrows, Bombs; Rupoor at hard/expert)
//   9. TriforcePiece × pieces_placed for Triforce Hunt / Ganon Hunt
//  10. Junk-pad to |locations|
//
// The exact junk counts mirror ALTTPR's config/alttp.php `item.junk` table
// (Phase A1 approximation — see ALTTPR `World.php::getItemPool` for the
// authoritative computation, line 838).
// ---------------------------------------------------------------------------

// Item registry id constants (from assets/rando/item_registry.yaml).
// Kept as named constants so the implementation reads cleanly.
enum {
  ID_ProgressiveSword = 0,
  ID_ProgressiveShield = 1,
  ID_ProgressiveArmor = 2,
  ID_ProgressiveGlove = 3,
  ID_ProgressiveBow = 4,
  ID_L1Sword = 5, ID_L2Sword = 6, ID_L3Sword = 7, ID_L4Sword = 8,
  ID_FighterShield = 9, ID_RedShield = 10, ID_MirrorShield = 11,
  ID_BlueMail = 12, ID_RedMail = 13,
  ID_PowerGlove = 14, ID_TitanMitt = 15,
  ID_FireRod = 16, ID_IceRod = 17, ID_Hammer = 18, ID_Hookshot = 19,
  ID_Bow = 20, ID_BlueBoomerang = 21, ID_RedBoomerang = 22,
  ID_MagicPowder = 23, ID_Mushroom = 24,
  ID_Bombos = 25, ID_Ether = 26, ID_Quake = 27, ID_Lamp = 28,
  ID_Shovel = 29, ID_OcarinaInactive = 30,
  ID_BugCatchingNet = 31, ID_BookOfMudora = 32,
  ID_CaneOfSomaria = 33, ID_CaneOfByrna = 34, ID_Cape = 35,
  ID_MagicMirror = 36, ID_Boots = 37, ID_Flippers = 38, ID_MoonPearl = 39,
  ID_SilverArrowUpgrade = 40,
  ID_HalfMagic = 41, ID_QuarterMagic = 42,
  ID_BottleEmpty = 43, ID_BottleWithFairy = 44, ID_BottleWithBee = 45,
  ID_BottleWithGoodBee = 46, ID_BottleWithRedPotion = 47,
  ID_BottleWithGreenPotion = 48, ID_BottleWithBluePotion = 49,
  ID_PieceOfHeart = 50, ID_BossHeartContainer = 51,
  ID_TriforcePiece = 52,
  ID_SmallKey_HCE = 53, ID_SmallKey_EP = 54, ID_SmallKey_DP = 55,
  ID_SmallKey_TH = 56, ID_SmallKey_HCT = 57, ID_SmallKey_PoD = 58,
  ID_SmallKey_SP = 59, ID_SmallKey_SW = 60, ID_SmallKey_TT = 61,
  ID_SmallKey_IP = 62, ID_SmallKey_MM = 63, ID_SmallKey_TR = 64,
  ID_SmallKey_GT = 65,
  ID_BigKey_EP = 66, ID_BigKey_DP = 67, ID_BigKey_TH = 68,
  ID_BigKey_PoD = 69, ID_BigKey_SP = 70, ID_BigKey_SW = 71,
  ID_BigKey_TT = 72, ID_BigKey_IP = 73, ID_BigKey_MM = 74,
  ID_BigKey_TR = 75, ID_BigKey_GT = 76,
  ID_Map_EP = 77, ID_Map_DP = 78, ID_Map_TH = 79, ID_Map_PoD = 80,
  ID_Map_SP = 81, ID_Map_SW = 82, ID_Map_TT = 83, ID_Map_IP = 84,
  ID_Map_MM = 85, ID_Map_TR = 86, ID_Map_GT = 87,
  ID_Compass_EP = 88, ID_Compass_DP = 89, ID_Compass_TH = 90,
  ID_Compass_PoD = 91, ID_Compass_SP = 92, ID_Compass_SW = 93,
  ID_Compass_TT = 94, ID_Compass_IP = 95, ID_Compass_MM = 96,
  ID_Compass_TR = 97, ID_Compass_GT = 98,
  ID_Rupee1 = 99, ID_Rupee5 = 100, ID_Rupee20 = 101,
  ID_Rupee100 = 102, ID_Rupee300 = 103,
  ID_SmallMagic = 104, ID_Arrow1 = 105, ID_Arrow10 = 106,
  ID_Bombs1 = 107, ID_Bombs3 = 108, ID_Bombs10 = 109, ID_Rupoor = 110,
  ID_Map_HCE = 124,
  ID_GenericKey = 125,  // Retro shared small-key (ROM 0xAF); substituted for every
                        // per-dungeon SmallKey under genericKeys (Settings_GenericKeysActive)
  ID_BluePotion = 126,  // unbottled BluePotion (ROM 0x30); Phase B Slice 3b TakeAny reward
  ID_TrapDamage = 132,
  ID_TrapFreeze = 133,
  // add-rando-trap-catalog — contiguous trap effect ids (mirror item_registry.yaml).
  ID_TrapBomb = 134, ID_TrapAmbush = 135, ID_TrapCucco = 136,
  ID_TrapReverse = 137, ID_TrapScramble = 138, ID_TrapDisarm = 139,
  ID_TrapRupeeDrain = 140, ID_TrapMagicDrain = 141, ID_TrapAmmoDrain = 142,
  ID_TrapShake = 143, ID_TrapDarkness = 144, ID_TrapFakeWarp = 145,
  ID_TrapFakeLowHp = 146, ID_TrapTeleport = 147,
};

// add-rando-trap-catalog — the hand-mirrored ID_* enum above MUST track the
// codegen'd ITEM_* ids (item_ids.h, included above). Assert the whole trap block
// so a registry renumber can't silently desync placement-side ids from the
// runtime-side dispatch/selector.
_Static_assert(ID_TrapDamage == ITEM_TrapDamage, "trap id drift: Damage");
_Static_assert(ID_TrapFreeze == ITEM_TrapFreeze, "trap id drift: Freeze");
_Static_assert(ID_TrapBomb == ITEM_TrapBomb, "trap id drift: Bomb");
_Static_assert(ID_TrapAmbush == ITEM_TrapAmbush, "trap id drift: Ambush");
_Static_assert(ID_TrapCucco == ITEM_TrapCucco, "trap id drift: Cucco");
_Static_assert(ID_TrapReverse == ITEM_TrapReverse, "trap id drift: Reverse");
_Static_assert(ID_TrapScramble == ITEM_TrapScramble, "trap id drift: Scramble");
_Static_assert(ID_TrapDisarm == ITEM_TrapDisarm, "trap id drift: Disarm");
_Static_assert(ID_TrapRupeeDrain == ITEM_TrapRupeeDrain, "trap id drift: RupeeDrain");
_Static_assert(ID_TrapMagicDrain == ITEM_TrapMagicDrain, "trap id drift: MagicDrain");
_Static_assert(ID_TrapAmmoDrain == ITEM_TrapAmmoDrain, "trap id drift: AmmoDrain");
_Static_assert(ID_TrapShake == ITEM_TrapShake, "trap id drift: Shake");
_Static_assert(ID_TrapDarkness == ITEM_TrapDarkness, "trap id drift: Darkness");
_Static_assert(ID_TrapFakeWarp == ITEM_TrapFakeWarp, "trap id drift: FakeWarp");
_Static_assert(ID_TrapFakeLowHp == ITEM_TrapFakeLowHp, "trap id drift: FakeLowHp");
_Static_assert(ID_TrapTeleport == ITEM_TrapTeleport, "trap id drift: Teleport");

// Phase B Slice 3b — Retro TakeAny constants (used by BuildItemPool's junk-pad
// target loop and the placer's selection/pin logic). Cave count + LOC id base
// must match assets/rando/location_registry.yaml (id = 266 + 2*cave + slot).
#define kTakeAnyCaveCount 31
#define kTakeAnyLocBase   266   // registry id of cave 0 slot 0
// LOCTYPE_Prize_Event / LOCTYPE_Medallion / LOCTYPE_Shop /
// LOCTYPE_ShopUpgrade / LOCTYPE_TakeAny now live in rando_logic.h (shared so
// the ordinals can't drift between files).

// Placer-local location-type ordinals (per logic.schema.yaml's types index).
// File-scope so BuildItemPool's junk-pad target and the pre-place pin pass in
// place_assumed_fill_attempt share one definition (see location_is_prepinned).
enum {
  // Overworld free-standing types (add-rando-trap-catalog: a Cucco trap may only
  // land on these, where player_is_indoors is always 0). Contiguous ordinals 3..6
  // per assets/rando_logic_gen.py's `types` index.
  LOCTYPE_Standing      = 3,
  LOCTYPE_Pedestal      = 4,
  LOCTYPE_Dash          = 5,
  LOCTYPE_Dig           = 6,
  LOCTYPE_Drop          = 7,
  LOCTYPE_Prize_Crystal = 10,
  LOCTYPE_Prize_Pendant = 11,
};

// kRandoDungeon_* -> Prize location id, for prize-shuffle placement.
// 0xFFFF = no Prize location for that dungeon.
// File-scope so Goal_IsCompletable can consult it for prize-reachability checks.
static const uint16 kDungeonPrizeLocations[13] = {
  0xFFFF,  // HCE
  17,      // EP Prize
  24,      // DP Prize
  31,      // TH Prize
  0xFFFF,  // HCT (Aga 1 is at 34, Prize_Event pinned separately)
  49,      // PoD Prize
  60,      // SP Prize
  69,      // SW Prize
  78,      // TT Prize
  87,      // IP Prize
  96,      // MM Prize
  109,     // TR Prize
  0xFFFF,  // GT (Aga 2 is at 137, Prize_Event pinned separately)
};

// Public accessor for the per-dungeon prize location (see rando_placement.h).
uint16 Rando_GetDungeonPrizeLocation(int rando_dungeon) {
  if (rando_dungeon < 0 || rando_dungeon >= kRandoDungeonCount) return 0xFFFF;
  return kDungeonPrizeLocations[rando_dungeon];
}

static const uint16 kBigKeys[] = {
  ID_BigKey_EP, ID_BigKey_DP, ID_BigKey_TH, ID_BigKey_PoD, ID_BigKey_SP,
  ID_BigKey_SW, ID_BigKey_TT, ID_BigKey_IP, ID_BigKey_MM, ID_BigKey_TR, ID_BigKey_GT,
};
static const uint16 kMaps[] = {
  ID_Map_HCE, ID_Map_EP, ID_Map_DP, ID_Map_TH, ID_Map_PoD, ID_Map_SP,
  ID_Map_SW, ID_Map_TT, ID_Map_IP, ID_Map_MM, ID_Map_TR, ID_Map_GT,
};
static const uint16 kCompasses[] = {
  ID_Compass_EP, ID_Compass_DP, ID_Compass_TH, ID_Compass_PoD, ID_Compass_SP,
  ID_Compass_SW, ID_Compass_TT, ID_Compass_IP, ID_Compass_MM, ID_Compass_TR, ID_Compass_GT,
};

static bool pot_active(const RandoLocationDef *loc, const RandoSettings *s);
static bool enemy_drop_active(const RandoLocationDef *loc, const RandoSettings *s);
static bool enemy_check_active(const RandoLocationDef *loc, const RandoSettings *s);
static bool terrain_active(const RandoLocationDef *loc, const RandoSettings *s);
static void seed_dungeon_free_key_drops(RandoCounts *counts, const RandoSettings *s);

// add-rando-grass-rock-shuffle — the terrain tier filter (Phase 3). A terrain
// location is ACTIVE when its axis (grass_shuffle for LOCTYPE_Grass,
// rock_shuffle for LOCTYPE_Rock) is non-off: BOTH the junk and all tiers
// activate the location (junk restricts WHAT may be placed there — see
// terrain_junk_only below — not whether it is a check). The skip-triple
// (open-location loop, junk-pad target, Placement_SelfCheck expected count)
// all key on this ONE predicate so the pool size and slot count cannot
// drift; with both axes off every seed is placement-byte-identical.
static bool terrain_active(const RandoLocationDef *loc, const RandoSettings *s) {
  if (loc == NULL || s == NULL) return false;
  if (loc->type == LOCTYPE_Grass) return s->grass_shuffle != kTerrainShuffle_Off;
  if (loc->type == LOCTYPE_Rock) return s->rock_shuffle != kTerrainShuffle_Off;
  // add-rando-bonk-sanity — same skip-triple discipline, third family.
  if (loc->type == LOCTYPE_Bonk) return s->bonk_shuffle != kTerrainShuffle_Off;
  return false;
}

// Junk-only class: a terrain location under tier `junk` participates in the
// junk-pad fill but is EXCLUDED from the assumed-fill progression candidate
// set (spec: "no pool/progression item on a junk-only location";
// Placement_SelfCheck asserts it). `all`-tier locations are ordinary open
// slots.
static bool terrain_junk_only(const RandoLocationDef *loc, const RandoSettings *s) {
  if (loc == NULL || s == NULL) return false;
  if (loc->type == LOCTYPE_Grass) return s->grass_shuffle == kTerrainShuffle_Junk;
  if (loc->type == LOCTYPE_Rock) return s->rock_shuffle == kTerrainShuffle_Junk;
  if (loc->type == LOCTYPE_Bonk) return s->bonk_shuffle == kTerrainShuffle_Junk;
  return false;
}

// NONPOT small-key drops per dungeon: the enemy / guard / under-block keys that
// pot_shuffle does NOT itemize (only POTS shuffle).
// Under DUNGEON keys + pots-on a dungeon's pot keys become pooled items, so its
// deep locations gate on the prover SHORTEST-PATH (min-depth) over ALL key doors.
// The nonpot drops are still collected in-context (exactly as in pots-off), so we
// pre-grant them into the assumed inventory to keep those gates satisfiable. Count
// = (door-rando small-key drop total) - (fork pot-key count); only dungeons
// whose drops exceed their pot keys are non-zero. Generated in
// pot_nonpot_drop_counts.h from the same pot_key_depth.gen.yaml artifact that
// drives the POT_KEYS_WILD/POT_KEYS_DUNGEON logic gates.

// True when pot_shuffle itemizes a dungeon's pot keys under DUNGEON keys — the
// exact condition OP_POT_KEYS_DUNGEON gates on (Settings_PotKeysActive: pot_shuffle
// >= Keys AND pots not forced off by CAVE-entrance shuffle) AND small keys
// == Dungeon. Routes through the SAME helper as the logic VM so the free-grant and
// the gates can't drift. Wild caps its own requirement at the pooled key count so
// it needs no nonpot free-grant; vanilla/pots-off/forced-off leave the keys free.
static bool pot_keys_dungeon_active(const RandoSettings *s) {
  return Settings_PotKeysActive(s) &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Dungeon;
}

// Pre-grant the nonpot small-key drops (kPotNonpotDropCounts) into `counts` when
// pot_keys_dungeon_active. Used only by the placer's assumed-fill seeding and the
// goal/sphere verifier. Runtime live counts must remain unmarked/unseeded because
// they already reflect the player's actual key counters.
static void seed_pot_nonpot_drops(RandoCounts *counts, const RandoSettings *s) {
  if (counts == NULL || !pot_keys_dungeon_active(s)) return;
  counts->pot_nonpot_drops_seeded = 1;
  for (uint8 i = 0; i < (uint8)kPotNonpotDropCounts_COUNT; i++)
    counts->by_item_id[kPotNonpotDropCounts[i].item_id] += kPotNonpotDropCounts[i].count;
}

// Seed the vanilla-mode dungeon items into a RandoCounts inventory. For each
// dungeon-item class in Vanilla mode the items are NOT shuffled into the world
// pool — the player collects them in-place — so logic treats them as always
// available during placement's assumed-fill pass. Runtime live counts must read
// the actual RAM counters/bitfields instead.
void Rando_SeedVanillaDungeonItems(RandoCounts *counts, const RandoSettings *settings) {
  if (counts == NULL || settings == NULL) return;
  if (Settings_EffectiveSmallKeysMode(settings) == kDungeonItemMode_Vanilla) {
    for (uint8 d = 0; d < kRandoDungeon_Count; d++)
      counts->by_item_id[Rando_SmallKeyItemForRandoDungeon(d)] =
          Rando_BaseSmallKeyCount(d);
  }
  if (Settings_EffectiveBigKeysMode(settings) == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kBigKeys) / sizeof(kBigKeys[0])); i++)
      counts->by_item_id[kBigKeys[i]] = 1;
  }
  if (settings->dungeon_maps_mode == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kMaps) / sizeof(kMaps[0])); i++)
      counts->by_item_id[kMaps[i]] = 1;
  }
  if (settings->dungeon_compasses_mode == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kCompasses) / sizeof(kCompasses[0])); i++)
      counts->by_item_id[kCompasses[i]] = 1;
  }
  seed_dungeon_free_key_drops(counts, settings);
}

// Add `n` copies of `item_id` to the pool, respecting capacity.
static uint16 pool_add(uint16 *pool, uint16 used, uint16 capacity, uint16 item_id, uint16 n) {
  for (uint16 i = 0; i < n && used < capacity; i++) pool[used++] = item_id;
  return used;
}

// Small-key item-id test. Contiguous per item_registry.yaml:
// SmallKey_HyruleCastleEscape(53) .. SmallKey_GanonsTower(65).
static bool is_small_key_item(uint16 item_id) {
  return item_id >= 53 && item_id <= 65;
}

// Is this pot an ACTIVE randomizer check under `s`? This ONE predicate gates all
// three placement sites that must
// agree — the open-location collection loop, BuildItemPool's junk-pad target,
// and Placement_SelfCheck's expected-count loop — so the pool size and the open
// slot count can never drift.
//
// A pot's KIND is derived from its vanilla_item_id (no extra table): empty pots
// carry ITEM_Nothing (rando_logic_gen.load_pots maps kind==empty → Nothing), key
// pots carry a SmallKey (53..65), everything else is loot. Tiers nest
// keys ⊆ contents ⊆ all.
//
// Under cave-entrance shuffle EVERY pot is inactive
// (Settings_PotShuffleForcedOff): cave/house pot IDs miss the entrance
// region-override so they'd evaluate from the vanilla overworld region.
// apply_derived_rules normalizes pot_shuffle off under the SAME predicate, so the
// settings_hash, placement, runtime, and spoiler all agree.
static bool pot_active(const RandoLocationDef *loc, const RandoSettings *s) {
  if (loc == NULL || s == NULL || loc->type != LOCTYPE_Pot) return false;
  if (Settings_PotShuffleForcedOff(s)) return false;  // cave-entrance shuffle
  bool is_empty = (loc->vanilla_item_id == ITEM_Nothing);
  switch (s->pot_shuffle) {
    case kPotShuffle_Off:      return false;
    case kPotShuffle_Keys:     return is_small_key_item(loc->vanilla_item_id);
    case kPotShuffle_Contents: return !is_empty;            // loot + keys
    case kPotShuffle_All:      return true;                 // + empty pots
    default:                   return false;                // reserved Subset(4)
  }
}

static bool enemy_drop_active(const RandoLocationDef *loc, const RandoSettings *s) {
  return loc != NULL && s != NULL && loc->type == LOCTYPE_EnemyDrop &&
         Settings_EnemyDropKeysActive(s);
}

// Count active itemized small-key sources once. Key Ring eligibility adds these
// to the base pool; dungeon-key reachability subtracts the same counts from the
// authored free-drop stock. Keeping both consumers on this scan prevents a new
// pot/enemy source from entering one model but not the other.
static void count_itemized_key_sources(
    const RandoSettings *settings,
    uint16 out_counts[kRandoDungeon_Count]) {
  memset(out_counts, 0, sizeof(uint16) * kRandoDungeon_Count);
  if (settings == NULL) return;

  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    const RandoLocationDef *loc = &kRandoLocations[i];
    if (!Rando_IsSmallKeyItem(loc->vanilla_item_id)) continue;
    if ((loc->type == LOCTYPE_Pot && pot_active(loc, settings)) ||
        (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, settings))) {
      uint8 d = Rando_RandoDungeonFromDungeonItem(loc->vanilla_item_id);
      if (d < kRandoDungeon_Count && out_counts[d] != 0xFFFFu)
        out_counts[d]++;
    }
  }
}

// Count the exact pre-collapse shuffled small-key multiset. This is the one
// source of truth for Key Ring eligibility: base chest keys plus every active
// itemized key pot and forced enemy key-drop check. Free/non-itemized dungeon
// drops deliberately stay out — they remain in the ROM after a ring collapse.
static void key_rings_count_sources(const RandoSettings *settings,
                                    uint16 out_counts[kRandoDungeon_Count]) {
  memset(out_counts, 0, sizeof(uint16) * kRandoDungeon_Count);
  if (settings == NULL ||
      Settings_EffectiveSmallKeysMode(settings) == kDungeonItemMode_Vanilla)
    return;

  uint16 itemized[kRandoDungeon_Count];
  count_itemized_key_sources(settings, itemized);
  for (uint8 d = 0; d < kRandoDungeon_Count; d++)
    out_counts[d] = (uint16)(Rando_BaseSmallKeyCount(d) + itemized[d]);
}

uint16 KeyRings_EligibleMask(const RandoSettings *settings) {
  uint16 counts[kRandoDungeon_Count];
  key_rings_count_sources(settings, counts);
  uint16 mask = 0;
  for (uint8 d = 0; d < kRandoDungeon_Count; d++)
    if (counts[d] != 0) mask |= (uint16)(1u << d);
  return mask;
}

static uint8 key_rings_mask_count(uint16 mask) {
  uint8 n = 0;
  while (mask != 0) {
    n += (uint8)(mask & 1u);
    mask >>= 1;
  }
  return n;
}

// add-rando-random-crystals — resolve the requested crystal counts to
// effective 0..7 values. Fixed axes pass through; a kCrystalsRandom axis
// draws uniformly from its OWN salted stream ("CrystlRq" ^ 'G'/'T' high
// byte), so the ganon draw is independent of whether tower is random and
// vice versa (the ShopPrice per-key-mix precedent). ONE function shared by
// generation (Goal_IsCompletable's crystal arms — BASE seed, never the
// attempt seed) and slot activation (the runtime cache) so the two cannot
// drift; the requested sentinel is never normalized into canonical bytes.
void Crystals_Resolve(const RandoSettings *settings, uint64 seed_u64,
                      uint8 *out_ganon, uint8 *out_tower) {
  uint8 ganon = settings ? settings->crystals_ganon : 7;
  uint8 tower = settings ? settings->crystals_tower : 7;
  if (ganon == kCrystalsRandom) {
    RandoRng rng;
    Rng_SeedFromU64(&rng, seed_u64 ^ 0x43727973746C5271ull ^ ((uint64)'G' << 56));
    ganon = (uint8)Rng_NextRange(&rng, 8);
  }
  if (tower == kCrystalsRandom) {
    RandoRng rng;
    Rng_SeedFromU64(&rng, seed_u64 ^ 0x43727973746C5271ull ^ ((uint64)'T' << 56));
    tower = (uint8)Rng_NextRange(&rng, 8);
  }
  if (out_ganon) *out_ganon = ganon;
  if (out_tower) *out_tower = tower;
}

bool KeyRings_Resolve(const RandoSettings *settings, uint64 seed_u64,
                      KeyRingSelection *out) {
  if (out == NULL || settings == NULL) return false;
  memset(out, 0, sizeof(*out));
  out->eligible_mask = KeyRings_EligibleMask(settings);
  uint8 mode = Settings_EffectiveKeyRings(settings);
  if (mode == kKeyRings_Off) return true;

  uint16 eligible = out->eligible_mask;
  if (mode == kKeyRings_All) {
    out->selected_mask = eligible;
    return true;
  }
  if (mode != kKeyRings_Random || key_rings_mask_count(eligible) < 2)
    return false;

  // FNV-1a("key-rings/v1") is a stable RNG-domain salt. Draw one bit per
  // eligible family; bounded retries exclude the two degenerate endpoints
  // without perturbing any other generator stream.
  RandoRng rng;
  Rng_SeedFromU64(&rng, seed_u64 ^ 0x8e7a299fd8176ce6ull);
  for (uint8 attempt = 0; attempt < 64; attempt++) {
    uint16 selected = 0;
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      uint16 bit = (uint16)(1u << d);
      if ((eligible & bit) && (Rng_NextU32(&rng) & 1u)) selected |= bit;
    }
    if (selected != 0 && selected != eligible) {
      out->selected_mask = selected;
      return true;
    }
  }

  // Deterministic proper-subset fallback. With >=2 eligible families this
  // selects at least one and leaves at least one unselected.
  uint16 selected = 0;
  uint8 ordinal = 0;
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    uint16 bit = (uint16)(1u << d);
    if (!(eligible & bit)) continue;
    if ((ordinal++ & 1u) == 0) selected |= bit;
  }
  out->selected_mask = selected;
  return true;
}

enum { kEnemyCheckNotOverworld = 0xFF };

static bool g_enemy_check_flags_ready;
static uint8 g_enemy_check_indoor_all_tier_only[kRandoLocationCapacity];
static uint8 g_enemy_check_all_tier_only[kRandoLocationCapacity];
static uint8 g_enemy_check_overworld_stage[kRandoLocationCapacity];

static void init_enemy_check_flags(void) {
  if (g_enemy_check_flags_ready) return;
  memset(g_enemy_check_overworld_stage, kEnemyCheckNotOverworld,
         sizeof(g_enemy_check_overworld_stage));
  for (uint32 i = 0; i < kRandoEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoEnemyCheckLookup[i].loc_id;
    if (loc_id >= kRandoLocationCapacity) continue;
    if (kRandoEnemyCheckLookup[i].all_tier_only) {
      g_enemy_check_indoor_all_tier_only[loc_id] = 1;
      g_enemy_check_all_tier_only[loc_id] = 1;
    }
  }
  for (uint32 i = 0; i < kRandoBossEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoBossEnemyCheckLookup[i].loc_id;
    if (loc_id < kRandoLocationCapacity) g_enemy_check_all_tier_only[loc_id] = 1;
  }
  for (uint32 i = 0; i < kRandoScriptedEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoScriptedEnemyCheckLookup[i].loc_id;
    if (loc_id < kRandoLocationCapacity) g_enemy_check_all_tier_only[loc_id] = 1;
  }
  for (uint32 i = 0; i < kRandoOverworldEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoOverworldEnemyCheckLookup[i].loc_id;
    if (loc_id >= kRandoLocationCapacity) continue;
    g_enemy_check_all_tier_only[loc_id] = 1;
    g_enemy_check_overworld_stage[loc_id] = kRandoOverworldEnemyCheckLookup[i].stage;
  }
  g_enemy_check_flags_ready = true;
}

static bool enemy_check_indoor_all_tier_only(uint16 loc_id) {
  init_enemy_check_flags();
  return loc_id < kRandoLocationCapacity &&
         g_enemy_check_indoor_all_tier_only[loc_id] != 0;
}

static bool enemy_check_all_tier_only(uint16 loc_id) {
  init_enemy_check_flags();
  return loc_id < kRandoLocationCapacity &&
         g_enemy_check_all_tier_only[loc_id] != 0;
}

static uint8 enemy_check_overworld_stage(uint16 loc_id) {
  init_enemy_check_flags();
  if (loc_id >= kRandoLocationCapacity) return kEnemyCheckNotOverworld;
  return g_enemy_check_overworld_stage[loc_id];
}

static bool enemy_check_active(const RandoLocationDef *loc, const RandoSettings *s) {
  if (loc == NULL || s == NULL || loc->type != LOCTYPE_Enemy) return false;
  if (Settings_EnemyChecksAllActive(s)) {
    uint8 ow_stage = enemy_check_overworld_stage(loc->id);
    if (ow_stage == 0 && s->world_state != kWorldState_Standard)
      return false;
    return true;
  }
  return Settings_EnemyChecksDungeonActive(s) &&
         !enemy_check_all_tier_only(loc->id);
}

static bool pot_registry_available(void) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (kRandoLocations[i].type == LOCTYPE_Pot) return true;
  return false;
}

static bool enemy_drop_registry_available(void) {
  return kRandoEnemyDropLookup_COUNT != 0;
}

static bool enemy_check_registry_available(void) {
  return kRandoEnemyCheckLookup_COUNT != 0;
}

static bool all_enemy_registry_available(void) {
  return enemy_check_registry_available() &&
         kRandoOverworldEnemyCheckLookup_COUNT != 0 &&
         kRandoBossEnemyCheckLookup_COUNT != 0 &&
         kRandoScriptedEnemyCheckLookup_COUNT != 0;
}

static bool settings_need_pot_registry(const RandoSettings *s) {
  return s != NULL && s->pot_shuffle != kPotShuffle_Off &&
         !Settings_PotShuffleForcedOff(s);
}

// add-rando-grass-rock-shuffle — terrain registry availability + need. Terrain
// adds NO pool items, so without this explicit fail-closed a grass/rock-active
// seed on an assetless/empty-registry build would generate a normal seed with
// no terrain checks and then ACTIVATE (the sidecar guard's (0,0)==(0,0) match
// is a fail-open) — silently dropping the feature the user asked for (the
// chest_lookup fail-open class; review HIGH-1).
static bool terrain_registry_available(void) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (kRandoLocations[i].type == LOCTYPE_Grass ||
        kRandoLocations[i].type == LOCTYPE_Rock) return true;
  return false;
}

static bool settings_need_terrain_registry(const RandoSettings *s) {
  return s != NULL && (s->grass_shuffle != kTerrainShuffle_Off ||
                       s->rock_shuffle != kTerrainShuffle_Off);
}

// add-rando-bonk-sanity — same fail-closed class as terrain: bonk adds no
// unique pool items, so an assetless build would otherwise silently generate
// a bonk-enabled seed with zero bonk checks (the grass-rock fail-open HIGH).
static bool bonk_registry_available(void) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (kRandoLocations[i].type == LOCTYPE_Bonk) return true;
  return false;
}

static bool settings_need_bonk_registry(const RandoSettings *s) {
  return s != NULL && s->bonk_shuffle != kTerrainShuffle_Off;
}

static bool settings_need_enemy_drop_registry(const RandoSettings *s) {
  return Settings_EnemyDropKeysActive(s);
}

static bool settings_need_enemy_check_registry(const RandoSettings *s) {
  return Settings_EnemyChecksDungeonActive(s);
}

static bool settings_need_all_enemy_registry(const RandoSettings *s) {
  return Settings_EnemyChecksAllActive(s);
}

static void placement_set_error(char *err, size_t err_cap, const char *msg) {
  if (err != NULL && err_cap > 0) {
    snprintf(err, err_cap, "%s", msg);
  }
}

bool Placement_PreflightSettings(const RandoSettings *settings,
                                 char *err,
                                 size_t err_cap) {
  if (err != NULL && err_cap > 0) err[0] = '\0';
  if (settings == NULL) {
    placement_set_error(err, err_cap, "settings are missing");
    return false;
  }
  if (!Placement_EnemyRegistriesAvailableForSettings(settings, err, err_cap))
    return false;
  if (Settings_EnemyChecksAllActive(settings) &&
      Settings_EffectiveAccessibility(settings) == kAccessibility_Locations) {
    placement_set_error(
        err, err_cap,
        "enemy_drop_checks=all is not supported with accessibility=locations yet "
        "because overworld enemy checks include missable pre/post-Aga windows");
    return false;
  }
  return true;
}

bool Placement_EnemyRegistriesAvailableForSettings(const RandoSettings *settings,
                                                   char *err,
                                                   size_t err_cap) {
  if (err != NULL && err_cap > 0) err[0] = '\0';
  if (settings == NULL) {
    placement_set_error(err, err_cap, "settings are missing");
    return false;
  }
  if (settings_need_enemy_drop_registry(settings) && !enemy_drop_registry_available()) {
    placement_set_error(
        err, err_cap,
        "enemy_drop_checks requires the generated forced enemy-drop registry; "
        "rebuild local enemy-drop assets or use enemy_drop_checks=off");
    return false;
  }
  if (settings_need_all_enemy_registry(settings) && !all_enemy_registry_available()) {
    placement_set_error(
        err, err_cap,
        "enemy_drop_checks=all requires the generated all-tier enemy registry; "
        "rebuild local enemy-check assets or use enemy_drop_checks=dungeon");
    return false;
  }
  if (settings_need_enemy_check_registry(settings) && !enemy_check_registry_available()) {
    placement_set_error(
        err, err_cap,
        "enemy_drop_checks=dungeon requires the generated dungeon enemy-check "
        "registry; rebuild local enemy-check assets or use enemy_drop_checks=keys");
    return false;
  }
  return true;
}

static bool dungeon_key_depth_active(const RandoSettings *s) {
  return s != NULL &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Dungeon &&
         (Settings_PotKeysActive(s) || Settings_EnemyDropKeysActive(s));
}

// Under DUNGEON keys, generated min-depth gates count every small-key door along
// the path. Drops that remain vanilla/free must therefore be present in the
// assumed inventory, while drops turned into checks must not be pre-granted.
// Count = door-rando DROP total - active key pots - active enemy small-key drops.
static void seed_dungeon_free_key_drops(RandoCounts *counts, const RandoSettings *s) {
  if (counts == NULL || !dungeon_key_depth_active(s)) return;
  uint16 itemized[kRandoDungeon_Count];
  count_itemized_key_sources(s, itemized);
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    uint16 key_id = Rando_SmallKeyItemForRandoDungeon(d);
    // itemized == 0 must still grant: the ENEMY_DROP_KEYS_DUNGEON /
    // POT_KEYS_DUNGEON min-depth gates are emitted per-row for EVERY dungeon
    // and the ops are GLOBAL (Settings_*Active), so a dungeon with no
    // itemized sources of its own (Swamp/Thieves'/Desert have no forced enemy
    // drops; cave-entrance shuffle forces their key pots off) still evaluates
    // its gates — while at runtime ALL of its drop keys fall vanilla-free.
    // The old `itemized == 0` skip left the model at pool-only (1 chest key)
    // against min-depth gates up to 5, permanently stranding 42 dungeon-tier
    // enemy checks under entrance shuffle x dungeon keys x enemy_drop_checks
    // (the DUNGEON-arm sibling of the wild-cap fix in rando_logic_gen.py's
    // _enemy_depth_terms).
    uint8 drop_total = Rando_DropSmallKeyCount(d);
    if (itemized[d] >= drop_total) continue;
    counts->by_item_id[key_id] += (uint16)(drop_total - itemized[d]);
  }
}

// shared pre-pin predicate. Returns true when the placer's
// pre-place pass (place_assumed_fill_attempt §3b) pins `loc` to a fixed item:
// vanilla identity (event / medallion slots, Retro shop + capacity-upgrade
// slots, vanilla-mode dungeon items) or the prize assignment (Prize_Pendant /
// Prize_Crystal — prize ids are never pool items). A pre-pinned slot never
// consumes an item from the shuffle pool, so BuildItemPool's junk-pad target
// must EXCLUDE it. Keeping the §3b pass and the junk-pad target on this ONE
// predicate prevents the two from drifting.
//
// TakeAny slots are intentionally NOT covered here: both callers handle them
// separately (per-seed active set; reward-pinned outside the pool).
static bool location_is_prepinned(const RandoLocationDef *loc,
                                  const RandoSettings *settings) {
  // Always pinned, independent of settings:
  //   - Prize_Event / Medallion: virtual event triggers + medallion config
  //     slots, pinned to their vanilla item.
  //   - Shop / ShopUpgrade: Retro shop inventory + capacity upgrades are
  //     identity-placed (per ALTTPR Randomizer.php Retro shops).
  //   - Prize_Pendant / Prize_Crystal: pinned per the prize-shuffle assignment.
  if (loc->type == LOCTYPE_Prize_Event || loc->type == LOCTYPE_Medallion ||
      loc->type == LOCTYPE_ShopUpgrade ||
      loc->type == LOCTYPE_Prize_Pendant || loc->type == LOCTYPE_Prize_Crystal)
    return true;
  // add-rando-shopsanity — regular shop slots stay identity-pinned (the Retro
  // economy) only while the axis is off; with it on they are ordinary open
  // fill targets in every world state (Rando_LocationWorldStateActive is the
  // matching filter override). ShopUpgrade stays unconditionally pinned above.
  if (loc->type == LOCTYPE_Shop)
    return !Settings_ShopsanityActive(settings);
  uint16 vi = loc->vanilla_item_id;
  // An ACTIVE empty pot (vanilla_item_id == ITEM_Nothing, present only under
  // tier All; both callers filter INACTIVE pots before reaching here) is pinned
  // to its ITEM_Nothing filler by the pre-pass, so
  // it consumes no pool item and the junk-pad target excludes it (no real item
  // can land on an empty pot, no Nothing on a real slot). Key-pots (vi 53..65)
  // fall through to the dungeon small-key rule below: pinned in vanilla key mode,
  // an open fillable slot when keys are shuffled.
  if (loc->type == LOCTYPE_Pot && vi == ITEM_Nothing)
    return true;
  // Vanilla-mode dungeon items are identity-placed (and never added to the
  // pool by BuildItemPool — the two are flip sides of the same mode check).
  if (vi >= 53 && vi <= 65) {
    uint8 km = Settings_EffectiveSmallKeysMode(settings);
    if (km == kDungeonItemMode_Vanilla) return true;
    // Under both DUNGEON and WILD keys the dungeon's small keys — chests AND
    // active key pots — shuffle and enter BuildItemPool, so
    // none are pinned. Under DUNGEON keys a key pot stays in its own dungeon (the
    // assumed-fill confines it via the per-pot min-depth gates: OP_POT_KEYS_DUNGEON
    // + the free-granted nonpot drops); under WILD it joins the world pool. An
    // INACTIVE pot (pot_shuffle off / cave forced-off) never reaches here — both
    // callers filter it — so it falls through to the vanilla pot drop. Only the
    // vanilla key mode pins.
    return false;
  }
  if (vi >= 66 && vi <= 76)
    return Settings_EffectiveBigKeysMode(settings) == kDungeonItemMode_Vanilla;
  if ((vi >= 77 && vi <= 87) || vi == 124)
    return settings->dungeon_maps_mode == kDungeonItemMode_Vanilla;
  if (vi >= 88 && vi <= 98)
    return settings->dungeon_compasses_mode == kDungeonItemMode_Vanilla;
  return false;
}

static bool location_grants_placed_item(const RandoLocationDef *loc) {
  return loc != NULL && loc->type != LOCTYPE_Medallion;
}

static bool placement_entry_grants_item(const RandoPlacementTable *t, uint16 entry_index) {
  if (t == NULL || entry_index >= t->count) return false;
  uint16 loc_id = t->entries[entry_index].location_id;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    if (kRandoLocations[i].id == loc_id)
      return location_grants_placed_item(&kRandoLocations[i]);
  }
  return true;
}

static uint16 trap_count_for_frequency(uint8 traps) {
  switch (traps) {
    case kTrapFrequency_Low:      return 4;
    case kTrapFrequency_Medium:   return 8;
    case kTrapFrequency_High:     return 16;
    case kTrapFrequency_Insanity: return 0xFFFF;  // all eligible junk (capped at eligible_n)
    default:                      return 0;
  }
}

static bool trap_replacement_candidate(uint16 item_id) {
  switch (item_id) {
    case ID_Rupee1:
    case ID_Rupee5:
    case ID_Rupee20:
    case ID_Rupee100:
    case ID_Rupee300:
    case ID_SmallMagic:
    case ID_Arrow1:
    case ID_Arrow10:
    case ID_Bombs1:
    case ID_Bombs3:
    case ID_Bombs10:
    case ID_Rupoor:
      return true;
    default:
      return false;
  }
}

// True iff loc_id is an OVERWORLD enemy check. LOCTYPE_Enemy spans four
// domains: overworld rows (outdoor) plus dungeon/boss/scripted rows (all
// interiors — including cave rooms, which bind to OW-named regions, so the
// region's dungeon_id cannot discriminate). Membership in the generated
// overworld lookup is the authoritative outdoor signal; both the table and
// the Enemy locations come from the same local registry, so a build without
// it has neither. Placement-time only (linear scan is fine).
static bool trap_loc_is_overworld_enemy_check(uint16 loc_id) {
  for (uint32 i = 0; i < kRandoOverworldEnemyCheckLookup_COUNT; i++)
    if (kRandoOverworldEnemyCheckLookup[i].loc_id == loc_id) return true;
  return false;
}

static uint16 inject_traps_into_junk_placements(uint16 *placement_at,
                                                const uint8 *junk_filled,
                                                const uint16 *open_loc_idx,
                                                uint16 n,
                                                const RandoSettings *settings,
                                                uint64 seed_u64,
                                                RandoRng *rng) {
  uint16 wanted = trap_count_for_frequency(settings ? settings->traps : 0);
  if (wanted == 0) return 0;

  uint16 eligible[kRandoLocationCapacity];
  uint16 eligible_n = 0;
  for (uint16 idx = 0; idx < n && eligible_n < (uint16)(sizeof eligible / sizeof eligible[0]); idx++) {
    if (!junk_filled[idx]) continue;
    // add-rando-shopsanity — shop slots are OUT of the trap-masquerade
    // eligible set in v1 (spec'd): a bought trap needs draw + refund
    // semantics the shop check flow doesn't have (design.md Q2 defers it).
    if (kRandoLocations[open_loc_idx[idx]].type == LOCTYPE_Shop) continue;
    if (!trap_replacement_candidate(placement_at[idx])) continue;
    eligible[eligible_n++] = idx;
  }
  for (int i = (int)eligible_n - 1; i > 0; i--) {
    uint32 j = rng ? Rng_NextRange(rng, (uint32)(i + 1)) : 0;
    uint16 tmp = eligible[i];
    eligible[i] = eligible[(uint16)j];
    eligible[(uint16)j] = tmp;
  }

  uint16 injected = 0;
  // Replace final junk-filled slots after fill, not BuildItemPool entries: the
  // placer may have more junk than open slots, and pool-level traps could be
  // shuffled into dropped junk.
  while (injected < wanted && injected < eligible_n) {
    uint16 idx = eligible[injected];
    // add-rando-trap-catalog — the per-slot effect TYPE is chosen deterministically
    // from (seed, location_id), filtered to the enabled category mask AND the
    // location's indoor/outdoor compatibility, so it is independent of the
    // eligible-slot shuffle order. The slot SELECTION above stays rng-driven; only
    // the type moved here off the old positional (injected & 1) alternation.
    const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[idx]];
    // Committed-data-only location flags so the placed effect always fires where the
    // spoiler shows it (Cucco never indoors, Darkness only in dungeons). Region
    // dungeon_id and the overworld free-standing types are deterministic + CI-safe.
    uint8 loc_flags = 0;
    if (loc->region_id != 0xFFFF && loc->region_id < kRandoRegionsCount &&
        kRandoRegions[loc->region_id].dungeon_id != 0xFF)
      loc_flags |= kTrapLoc_IsDungeon;
    if (loc->type >= LOCTYPE_Standing && loc->type <= LOCTYPE_Dig)
      loc_flags |= kTrapLoc_IsOutdoor;
    // Grass/rock checks are overworld-only by construction; enemy checks are
    // outdoor exactly when they row in the overworld enemy-check lookup (see
    // trap_loc_is_overworld_enemy_check — interiors keep flags 0, so
    // outdoor-locked effects still never roll there).
    if (loc->type == LOCTYPE_Grass || loc->type == LOCTYPE_Rock ||
        loc->type == LOCTYPE_Bonk ||
        (loc->type == LOCTYPE_Enemy &&
         trap_loc_is_overworld_enemy_check(loc->id)))
      loc_flags |= kTrapLoc_IsOutdoor;
    placement_at[idx] = Rando_PickTrapEffectId(
        seed_u64, loc->id, settings ? settings->trap_categories : 0, loc_flags);
    injected++;
  }
  return injected;
}

static bool pool_collapse_key_rings(uint16 *pool, uint16 *used,
                                    uint16 selected_mask) {
  uint16 write = 0;
  uint16 emitted_mask = 0;
  for (uint16 read = 0; read < *used; read++) {
    uint16 item = pool[read];
    if (!Rando_IsSmallKeyItem(item)) {
      pool[write++] = item;
      continue;
    }
    uint8 d = Rando_RandoDungeonFromDungeonItem(item);
    uint16 bit = d < kRandoDungeon_Count ? (uint16)(1u << d) : 0;
    if (!(selected_mask & bit)) {
      pool[write++] = item;
      continue;
    }
    if (!(emitted_mask & bit)) {
      pool[write++] = Rando_KeyRingItemForRandoDungeon(d);
      emitted_mask |= bit;
    }
    // Every later shuffled copy in this family is released to junk padding.
  }
  *used = write;
  return emitted_mask == selected_mask;
}

static bool pool_remove_deterministic_junk(uint16 *pool, uint16 *used) {
  // These are exactly the trap-replaceable filler identities. Scan backwards
  // so the removal is deterministic and minimally disturbs the established
  // pre-pad order.
  for (uint16 i = *used; i > 0; i--) {
    uint16 at = (uint16)(i - 1);
    if (!trap_replacement_candidate(pool[at])) continue;
    for (uint16 j = at; j + 1 < *used; j++) pool[j] = pool[j + 1];
    (*used)--;
    return true;
  }
  return false;
}

static uint16 build_item_pool_internal(const RandoSettings *settings,
                                       uint64 seed_u64,
                                       uint16 *out_items,
                                       uint16 capacity,
                                       KeyRingSelection *out_selection) {
  if (settings == NULL || out_items == NULL || capacity == 0) return 0;

  // Validate Triforce/Ganon-Hunt parameters before pool construction.
  // An un-buildable pool (pieces_required > pieces_placed) would
  // silently produce an un-completable seed.
  if ((settings->goal == kGoal_TriforceHunt || settings->goal == kGoal_GanonHunt) &&
      settings->pieces_required > settings->pieces_placed) {
    fprintf(stderr,
      "BuildItemPool: pieces_required (%u) > pieces_placed (%u) — refusing\n"
      "  to build a pool that can never satisfy the goal.\n",
      (unsigned)settings->pieces_required, (unsigned)settings->pieces_placed);
    return 0;
  }

  if (settings_need_pot_registry(settings) && !pot_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: pot_shuffle requested, but this binary was built without\n"
      "  assets/rando/pots.gen.yaml. Run the local pot codegen with ROM assets\n"
      "  and rebuild before generating pot-shuffle seeds.\n");
    return 0;
  }
  if (settings_need_terrain_registry(settings) && !terrain_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: grass/rock shuffle requested, but this binary was built\n"
      "  without assets/rando/terrain.gen.yaml. Run zelda3 --dump-terrain-table\n"
      "  + assets/scripts/gen_terrain_tables.py --emit with ROM assets and\n"
      "  rebuild before generating terrain-shuffle seeds.\n");
    return 0;
  }
  if (settings_need_bonk_registry(settings) && !bonk_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: bonk_shuffle requested, but this binary was built\n"
      "  without assets/rando/bonk.gen.yaml. Run\n"
      "  assets/scripts/gen_bonk_tables.py with ROM assets and rebuild\n"
      "  before generating bonk-shuffle seeds.\n");
    return 0;
  }
  if (settings_need_enemy_drop_registry(settings) && !enemy_drop_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: enemy_drop_checks=keys requested, but this binary was\n"
      "  built without assets/rando/enemy_drops.gen.yaml. Run\n"
      "  assets/scripts/gen_enemy_drop_tables.py with ROM assets and rebuild\n"
      "  before generating enemy-drop-check seeds.\n");
    return 0;
  }
  if (settings_need_all_enemy_registry(settings) && !all_enemy_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: enemy_drop_checks=all requested, but this binary was\n"
      "  built without all-tier overworld enemy-check rows. Run\n"
      "  assets/scripts/gen_enemy_check_tables.py with ROM assets and rebuild\n"
      "  before generating all enemy-check seeds.\n");
    return 0;
  }
  if (settings_need_enemy_check_registry(settings) && !enemy_check_registry_available()) {
    fprintf(stderr,
      "BuildItemPool: enemy_drop_checks=dungeon requested, but this binary was\n"
      "  built without assets/rando/enemy_checks.gen.yaml. Run\n"
      "  assets/scripts/gen_enemy_check_tables.py with ROM assets and rebuild\n"
      "  before generating dungeon enemy-check seeds.\n");
    return 0;
  }
  if (Settings_EnemyChecksAllActive(settings) &&
      Settings_EffectiveAccessibility(settings) == kAccessibility_Locations) {
    fprintf(stderr,
      "BuildItemPool: enemy_drop_checks=all with accessibility=locations is\n"
      "  unsupported until overworld enemy stage windows are modeled as temporal\n"
      "  locations. Use accessibility=items/beatable or a lower enemy tier.\n");
    return 0;
  }
  if (Settings_EffectiveSoulsShuffle(settings) >= kSoulsShuffle_BossesEnemies &&
      !kRandoSoulRoomsBaked) {
    fprintf(stderr,
      "BuildItemPool: souls_shuffle=all requested, but this binary was built\n"
      "  without assets/rando/soul_rooms.gen.yaml (kill-gated-room soul\n"
      "  requirements). Run assets/scripts/gen_soul_room_tables.py with ROM\n"
      "  assets and rebuild before generating enemies-tier souls seeds.\n");
    return 0;
  }

  KeyRingSelection key_rings;
  memset(&key_rings, 0, sizeof(key_rings));
  // Preserve the feature-off hot path: eligibility scans the complete
  // location registry, but an effective-Off pool needs neither eligibility
  // nor selection. Spoiler/UI callers that display eligibility use
  // KeyRings_Resolve directly.
  if (Settings_EffectiveKeyRings(settings) != kKeyRings_Off &&
      !KeyRings_Resolve(settings, seed_u64, &key_rings)) {
    fprintf(stderr,
      "BuildItemPool: key_rings=random requires at least two eligible "
      "small-key families\n");
    return 0;
  }
  if (out_selection != NULL) *out_selection = key_rings;
  uint16 selected_key_rings = key_rings.selected_mask;

  uint16 n = 0;

  // ----- Progression items: sword / shield / armor / glove / bow -----
  // Item-pool difficulty caps per spec scenario "Item-pool difficulty
  // downgrade" + ALTTPR's app/World.php:171-214 overflow.count.* table.
  // Hard / Expert cap the per-class counts; excess slots get filled with
  // junk during the junk-pad pass.
  uint8 sword_cap = 4, armor_cap = 2, shield_cap = 3, bow_cap = 2;
  uint8 bossheart_cap = 10, poh_cap = 24;
  switch (settings->item_pool_difficulty) {
    case kItemPoolDifficulty_Hard:
      sword_cap = 3; armor_cap = 0; shield_cap = 2; bow_cap = 1;
      bossheart_cap = 6; poh_cap = 16;
      break;
    case kItemPoolDifficulty_Expert:
      sword_cap = 2; armor_cap = 0; shield_cap = 1; bow_cap = 1;
      bossheart_cap = 2; poh_cap = 8;
      break;
    case kItemPoolDifficulty_Easy:
    case kItemPoolDifficulty_Normal:
    default:
      break;
  }

  if (settings->mode_weapons == kModeWeapons_Randomized ||
      settings->mode_weapons == kModeWeapons_Assured) {
    // ALTTPR convention for Randomized/Assured: 4 progressive swords, 3
    // progressive shields, 2 progressive armor, 2 progressive gloves, 2
    // progressive bows. (Per `Randomizer.php:183-198`; counts match the max
    // tier counts in item_registry.yaml.) item_pool_difficulty applies
    // overflow caps to sword / shield / armor / bow.
    n = pool_add(out_items, n, capacity, ID_ProgressiveSword, sword_cap);
    n = pool_add(out_items, n, capacity, ID_ProgressiveShield, shield_cap);
    n = pool_add(out_items, n, capacity, ID_ProgressiveArmor, armor_cap);
    n = pool_add(out_items, n, capacity, ID_ProgressiveGlove, 2);
    n = pool_add(out_items, n, capacity, ID_ProgressiveBow, bow_cap);
  } else if (settings->mode_weapons == kModeWeapons_Swordless) {
    // Swordless (ALTTPR Randomizer.php:224-239 + World.php:269-273): NO swords in
    // the pool. Mirror the Randomized equipment set MINUS ProgressiveSword. Silver
    // arrows are 100% required for Ganon under swordless, so guarantee a silver
    // source: ProgressiveBow (bow_cap) PLUS a standalone SilverArrowUpgrade (so
    // CanShootSilvers holds even when the difficulty overflow cap pins bow_cap=1).
    // The removed sword slots are absorbed by the junk pad. Runtime: the swordless
    // gates (Rando_IsSwordlessActive) let hammer/net stand in for the sword.
    n = pool_add(out_items, n, capacity, ID_ProgressiveShield, shield_cap);
    n = pool_add(out_items, n, capacity, ID_ProgressiveArmor, armor_cap);
    n = pool_add(out_items, n, capacity, ID_ProgressiveGlove, 2);
    n = pool_add(out_items, n, capacity, ID_ProgressiveBow, bow_cap);
    n = pool_add(out_items, n, capacity, ID_SilverArrowUpgrade, 1);
  } else {
    // Absolute weapon mode (Phase B 'vanilla' reserved):
    // emit one of each tier as a distinct item.
    n = pool_add(out_items, n, capacity, ID_L1Sword, 1);
    n = pool_add(out_items, n, capacity, ID_L2Sword, 1);
    n = pool_add(out_items, n, capacity, ID_L3Sword, 1);
    n = pool_add(out_items, n, capacity, ID_L4Sword, 1);
    n = pool_add(out_items, n, capacity, ID_FighterShield, 1);
    n = pool_add(out_items, n, capacity, ID_RedShield, 1);
    n = pool_add(out_items, n, capacity, ID_MirrorShield, 1);
    n = pool_add(out_items, n, capacity, ID_BlueMail, 1);
    n = pool_add(out_items, n, capacity, ID_RedMail, 1);
    n = pool_add(out_items, n, capacity, ID_PowerGlove, 1);
    n = pool_add(out_items, n, capacity, ID_TitanMitt, 1);
    n = pool_add(out_items, n, capacity, ID_Bow, 1);
    n = pool_add(out_items, n, capacity, ID_SilverArrowUpgrade, 1);
  }

  // ----- Common non-weapon equipment (always one copy each) -----
  n = pool_add(out_items, n, capacity, ID_FireRod, 1);
  n = pool_add(out_items, n, capacity, ID_IceRod, 1);
  n = pool_add(out_items, n, capacity, ID_Hammer, 1);
  n = pool_add(out_items, n, capacity, ID_Hookshot, 1);
  n = pool_add(out_items, n, capacity, ID_BlueBoomerang, 1);
  n = pool_add(out_items, n, capacity, ID_RedBoomerang, 1);
  n = pool_add(out_items, n, capacity, ID_MagicPowder, 1);
  n = pool_add(out_items, n, capacity, ID_Mushroom, 1);
  n = pool_add(out_items, n, capacity, ID_Bombos, 1);
  n = pool_add(out_items, n, capacity, ID_Ether, 1);
  n = pool_add(out_items, n, capacity, ID_Quake, 1);
  n = pool_add(out_items, n, capacity, ID_Lamp, 1);
  n = pool_add(out_items, n, capacity, ID_Shovel, 1);
  n = pool_add(out_items, n, capacity, ID_OcarinaInactive, 1);
  n = pool_add(out_items, n, capacity, ID_BugCatchingNet, 1);
  n = pool_add(out_items, n, capacity, ID_BookOfMudora, 1);
  n = pool_add(out_items, n, capacity, ID_CaneOfSomaria, 1);
  n = pool_add(out_items, n, capacity, ID_CaneOfByrna, 1);
  n = pool_add(out_items, n, capacity, ID_Cape, 1);
  n = pool_add(out_items, n, capacity, ID_MagicMirror, 1);
  n = pool_add(out_items, n, capacity, ID_Boots, 1);
  n = pool_add(out_items, n, capacity, ID_Flippers, 1);
  n = pool_add(out_items, n, capacity, ID_MoonPearl, 1);

  // ----- Magic upgrades -----
  n = pool_add(out_items, n, capacity, ID_HalfMagic, 1);
  // QuarterMagic is a Phase A item but ALTTPR places it only at specific
  // higher-pool-difficulty configs. Phase A includes it; if the player
  // collects both Half then Quarter, the dispatcher applies them in order
  // (per audit §0.4.1 notes).
  n = pool_add(out_items, n, capacity, ID_QuarterMagic, 1);

  // ----- Bottles: default 4 (the cap; link_bottle_info has 4 slots) -----
  // Per `randomizer-core / Item pool construction`: "Bottle count is capped
  // at 4 total". For now emit 4 BottleEmpty; future settings (Triforce Hunt
  // assured-bottle) may pre-place a starting bottle in which case the pool
  // contains 3.
  n = pool_add(out_items, n, capacity, ID_BottleEmpty, 4);

  // ----- Heart items: PoH and BossHeartContainer counts per ALTTPR vanilla -----
  // Vanilla ALTTPR: 24 Piece-of-Heart + 10 BossHeartContainer (one per dungeon
  // boss), capped lower under Hard/Expert (see bossheart_cap/poh_cap above).
  // Boss-heart drops are regular assumed-fill locations now, so the boss heart
  // containers always enter the pool.
  n = pool_add(out_items, n, capacity, ID_PieceOfHeart, poh_cap);
  n = pool_add(out_items, n, capacity, ID_BossHeartContainer, bossheart_cap);

  // ----- Dungeon items (per dungeon_items.* mode) -----
  // Vanilla: NOT in pool (placed at vanilla locations by the placement
  // algorithm's identity rule). Dungeon/Wild: add to pool.
  if (Settings_EffectiveSmallKeysMode(settings) != kDungeonItemMode_Vanilla) {
    // Under Retro genericKeys, every per-dungeon SmallKey is the fungible
    // GenericKey instead (ALTTPR Location::getItem swaps each Item\Key for KeyGK
    // — app/Location.php:201,268). Same per-dungeon COUNTS and same in-pool slots
    // (one substitution per entry, count 0 entries no-op), so the pool size is
    // unchanged; only the item identity changes. wildKeys already routes them
    // wild. Non-Retro keeps per-dungeon identity.
    bool generic = Settings_GenericKeysActive(settings);
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      uint16 key_id = generic ? (uint16)ID_GenericKey
                              : Rando_SmallKeyItemForRandoDungeon(d);
      n = pool_add(out_items, n, capacity, key_id, Rando_BaseSmallKeyCount(d));
    }
  }
  if (Settings_EffectiveBigKeysMode(settings) != kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kBigKeys) / sizeof(kBigKeys[0])); i++) {
      n = pool_add(out_items, n, capacity, kBigKeys[i], 1);
    }
  }
  if (settings->dungeon_maps_mode != kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kMaps) / sizeof(kMaps[0])); i++) {
      n = pool_add(out_items, n, capacity, kMaps[i], 1);
    }
  }
  if (settings->dungeon_compasses_mode != kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kCompasses) / sizeof(kCompasses[0])); i++) {
      n = pool_add(out_items, n, capacity, kCompasses[i], 1);
    }
  }

  // ----- Pot keys -----
  // Rando_BaseSmallKeyCount is the placed/chest key count; a dungeon's POT
  // keys are NOT in it. When pot_shuffle turns the key pots into live checks
  // their small key must ALSO enter the pool, or it vanishes. Under WILD keys the
  // keys live in the general world pool; under DUNGEON keys they shuffle within
  // their own dungeon (the assumed-fill confines them via the per-pot min-depth
  // gates, and the still-vanilla drops are free-granted by
  // seed_dungeon_free_key_drops). Either
  // way each active key pot contributes its vanilla SmallKey (or the shared
  // GenericKey under Retro, exactly as the chest keys above). Slot-balanced: every
  // active key pot is itself a fillable open slot counted by the junk-pad target,
  // so pool and slot grow together (a placed key that happens to sit under a pot
  // double-counts to a harmless surplus key). pots-off / vanilla leave this
  // untouched (the pool stays byte-identical).
  if (Settings_EffectiveSmallKeysMode(settings) != kDungeonItemMode_Vanilla) {
    bool generic = Settings_GenericKeysActive(settings);
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Pot && pot_active(loc, settings) &&
          is_small_key_item(loc->vanilla_item_id)) {
        uint16 key_id = generic ? (uint16)ID_GenericKey : loc->vanilla_item_id;
        n = pool_add(out_items, n, capacity, key_id, 1);
      }
    }
  }

  // ----- Enemy-drop checks -----
  // Forced enemy small-key drops are outside Rando_BaseSmallKeyCount, same as
  // key pots. When the tier is active, each generated enemy-drop location
  // becomes a fillable check and contributes its vanilla key to the pool (or the
  // shared GenericKey under Retro). Under Dungeon keys, exact min-depth gates and
  // seed_dungeon_free_key_drops keep formerly-free drops in sync. The castle
  // big-key marker is a one-shot check with no modeled registry item; it still
  // needs a pool filler so the spawned check never falls back to ITEM_Nothing.
  if (Settings_EnemyDropKeysActive(settings)) {
    bool generic = Settings_GenericKeysActive(settings);
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type != LOCTYPE_EnemyDrop || !enemy_drop_active(loc, settings))
        continue;
      if (is_small_key_item(loc->vanilla_item_id)) {
        uint16 key_id = generic ? (uint16)ID_GenericKey : loc->vanilla_item_id;
        n = pool_add(out_items, n, capacity, key_id, 1);
      } else if (loc->vanilla_item_id == ITEM_Nothing) {
        n = pool_add(out_items, n, capacity, ID_Rupee20, 1);
      }
    }
  }

  // ----- Key Rings -----
  // Collapse only after every active shuffled small-key source has contributed.
  // The selected mask is base-seed-derived and therefore invariant across the
  // assumed fill's retry attempts.
  if (selected_key_rings != 0 &&
      !pool_collapse_key_rings(out_items, &n, selected_key_rings)) {
    fprintf(stderr,
      "BuildItemPool: selected Key Ring family has no shuffled small-key copy\n");
    return 0;
  }

  // Skeleton Key is a real, unrestricted bonus item. It is deliberately added
  // before junk padding and is not progression/trap-replaceable.
  bool skeleton_added = settings->skeleton_key != 0;
  if (skeleton_added) {
    if (n >= capacity) {
      fprintf(stderr, "BuildItemPool: no capacity for Skeleton Key\n");
      return 0;
    }
    out_items[n++] = ITEM_SkeletonKey;
  }

  // ----- Rupees -----
  // ALTTPR vanilla pool: 4× Rupee300, 5× Rupee100, 28× Rupee20, 7× Rupee5,
  // 2× Rupee1. Numbers approximate per `config/alttp.php item.junk`.
  n = pool_add(out_items, n, capacity, ID_Rupee300, 4);
  n = pool_add(out_items, n, capacity, ID_Rupee100, 5);
  n = pool_add(out_items, n, capacity, ID_Rupee20,  28);
  n = pool_add(out_items, n, capacity, ID_Rupee5,   7);
  n = pool_add(out_items, n, capacity, ID_Rupee1,   2);

  // ----- Arrows, Bombs, Magic refills -----
  n = pool_add(out_items, n, capacity, ID_Arrow10, 5);
  n = pool_add(out_items, n, capacity, ID_Arrow1,  1);
  n = pool_add(out_items, n, capacity, ID_Bombs10, 1);
  n = pool_add(out_items, n, capacity, ID_Bombs3,  10);
  n = pool_add(out_items, n, capacity, ID_Bombs1,  0);
  n = pool_add(out_items, n, capacity, ID_SmallMagic, 1);

  // ----- Rupoor (hard/expert pools only) -----
  if (settings->item_pool_difficulty == kItemPoolDifficulty_Hard ||
      settings->item_pool_difficulty == kItemPoolDifficulty_Expert) {
    n = pool_add(out_items, n, capacity, ID_Rupoor,
                 settings->item_pool_difficulty == kItemPoolDifficulty_Expert ? 4 : 2);
  }

  // ----- Triforce pieces (Hunt goals) -----
  if (settings->goal == kGoal_TriforceHunt || settings->goal == kGoal_GanonHunt) {
    n = pool_add(out_items, n, capacity, ID_TriforcePiece, settings->pieces_placed);
  }

  // ----- Souls (add-enemy-souls) -----
  // Bosses tier places the kSoulBossCount boss souls; the bosses+enemies tier
  // adds one soul per enemy family. Souls are progression items (see
  // is_progression_item), one of each, displacing junk padding below.
  // EFFECTIVE tier: the enemies tier degrades to Bosses under door shuffle
  // (Settings_EffectiveSoulsShuffle) — the pool must match what the logic VM
  // and the runtime suppression actually bind.
  {
  uint8 souls_tier = Settings_EffectiveSoulsShuffle(settings);
  if (souls_tier >= kSoulsShuffle_Bosses) {
    uint16 soul_count = (souls_tier >= kSoulsShuffle_BossesEnemies)
                            ? kSoulCount : kSoulBossCount;
    for (uint16 i = 0; i < soul_count; i++)
      n = pool_add(out_items, n, capacity, (uint16)(ITEM_Soul_ArmosKnights + i), 1);
  }
  // add-npc-souls — independent of the tier: kNpcSoulCount NPC souls when the
  // toggle is on (no fail-closed arm needed: the site/gate tables are
  // COMMITTED, not asset-derived-gitignored like soul_rooms).
  if (settings->npc_souls) {
    for (uint16 i = 0; i < kNpcSoulCount; i++)
      n = pool_add(out_items, n, capacity, (uint16)(ITEM_Soul_Npc_Sahasrahla + i), 1);
  }
  }

  // ----- Junk-pad to match world-state-filtered location count -----
  // BuildItemPool used to pad to kRandoLocationsCount
  // unconditionally; that's wrong when locations carry a world_state_filter
  // (Phase A1 has none, but adding Inverted/Retro-specific locations later
  // would break). Count the locations whose filter accepts the active
  // world state and pad to that.
  //
  // Per spec scenario "Triforce Hunt junk-padding": pad with a rotation of
  // small rupee / single bomb / single arrow / small heart-equivalent so the
  // padded items match ALTTPR's mix. The rotation is deterministic (no RNG),
  // so the placement digest stays stable.
  static const uint16 kJunkRotation[] = {
    ID_Rupee20, ID_Bombs1, ID_Arrow1, ID_Rupee5,
  };
  const uint16 rotation_n = (uint16)(sizeof(kJunkRotation) / sizeof(kJunkRotation[0]));
  uint16 target = 0;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    const RandoLocationDef *loc = &kRandoLocations[i];
    if (Rando_LocationWorldStateActive(loc, settings)) {
      // Phase B Slice 3b — TakeAny LOCs are reward-pinned by role in the placer
      // (not pool-filled) and only a fixed 9-of-62 emit per seed. Excluding them
      // from the junk-pad target keeps the pool sized to the actual fillable
      // (non-pinned) slot count, so existing non-TakeAny Retro placements stay
      // decision-stable when TakeAny lands. See design.md §"Pool/pad".
      if (loc->type == LOCTYPE_TakeAny) continue;
      // An INACTIVE pot is out of the placement pool (its tier isn't selected,
      // or cave entrance shuffle is on). pot_active() is the SAME
      // predicate the open-location loop + Placement_SelfCheck use, so the pool
      // size and the open-slot count can't drift; with pot_shuffle off every pot
      // is inactive and the placement stays byte-identical. An ACTIVE
      // empty pot is prepinned (below) to ITEM_Nothing, so it too is excluded
      // here; only active loot/key pots count toward the junk-pad target.
      if (loc->type == LOCTYPE_Pot && !pot_active(loc, settings)) continue;
      if (loc->type == LOCTYPE_EnemyDrop && !enemy_drop_active(loc, settings)) continue;
      if (loc->type == LOCTYPE_Enemy && !enemy_check_active(loc, settings)) continue;
      if ((loc->type == LOCTYPE_Grass || loc->type == LOCTYPE_Rock ||
           loc->type == LOCTYPE_Bonk) &&
          !terrain_active(loc, settings)) continue;
      // pre-pinned slots (prizes, events, medallions, shops,
      // vanilla-mode dungeon items) are identity-placed by
      // the §3b pin pass and never consume a pool item. Counting them here
      // oversized the pool by the pin count, and the junk-fill surplus drop
      // then silently discarded a random subset of the pool every seed.
      if (location_is_prepinned(loc, settings)) continue;
      target++;
    }
  }
  if (skeleton_added && n > target) {
    if (!pool_remove_deterministic_junk(out_items, &n)) {
      fprintf(stderr,
        "BuildItemPool: Skeleton Key needs a replacement slot but no junk "
        "copy is available\n");
      return 0;
    }
  }
  uint16 rotation_i = 0;
  while (n < target && n < capacity) {
    out_items[n++] = kJunkRotation[rotation_i];
    rotation_i = (uint16)((rotation_i + 1u) % rotation_n);
  }

  return n;
}

uint16 BuildItemPool(const RandoSettings *settings, uint64 seed_u64,
                     uint16 *out_items, uint16 capacity) {
  return build_item_pool_internal(settings, seed_u64, out_items, capacity, NULL);
}

// ---------------------------------------------------------------------------
// Item-progression classifier (tasks.md §4.3).
//
// "Progression" items are those whose placement affects reachability —
// weapons, utility items, dungeon keys, bottles, prizes, triforce pieces,
// and the magic upgrades. Non-progression items (rupees, arrows, bombs,
// hearts, maps, compasses, junk) are placed last via simple shuffle.
//
// Item IDs match `assets/rando/item_registry.yaml`. Centralizing the
// classification here means the spec stays the source of truth.
// ---------------------------------------------------------------------------
static bool is_progression_item(uint16 item_id) {
  // Progressive items (0..4)
  if (item_id <= 4) return true;
  // Absolute weapons / utility (5..40 covers sword tiers, shields, armor,
  // gloves, rods, hammer, hookshot, bow, boomerangs, powder, mushroom,
  // medallions, lamp, shovel, ocarina, bug net, book, somaria, byrna, cape,
  // mirror, boots, flippers, moon pearl, silver arrows).
  if (item_id >= 5 && item_id <= 40) return true;
  // Magic upgrades (41, 42)
  if (item_id == 41 || item_id == 42) return true;
  // Bottles (43..49)
  if (item_id >= 43 && item_id <= 49) return true;
  // PieceOfHeart (50) / BossHeartContainer (51) — NOT progression for Phase A.
  // (Phase B's hero mode or low-health logic may revisit.)
  // TriforcePiece (52) — progression for Triforce Hunt / Ganon Hunt goals.
  if (item_id == 52) return true;
  // Small keys (53..65) and big keys (66..76) — progression for dungeon traversal.
  if (item_id >= 53 && item_id <= 76) return true;
  // Maps (77..87) and compasses (88..98) — NOT progression.
  // Multi-tier rupees (99..103) — NOT progression.
  // Junk (104..110) — NOT progression.
  // Prize pendants (111..113) and crystals (114..120) — progression
  // (gate Sahasrahla / Pedestal / Ganon's Tower / Ganon).
  if (item_id >= 111 && item_id <= 120) return true;
  // GenericKey (125) — the Retro shared small key. Progression: it gates the
  // small-key doors. Only ever in the pool under genericKeys (BuildItemPool
  // substitution), so classifying it unconditionally leaves non-Retro pools
  // unaffected. It must be in the assumed inventory so the door-reachability
  // collapse (rando_logic.c) sees the shared GenericKey count during fill.
  if (item_id == 125) return true;
  // Key Rings (220..232) collapse one dungeon family's shuffled keys into one
  // traversal item. SkeletonKey (233) is intentionally bonus-only.
  if (Rando_IsKeyRingItem(item_id)) return true;
  // Souls (add-enemy-souls) — progression: they gate boss kills, kill-gated
  // rooms, and enemy-check/forced-drop availability. Only in the pool under
  // souls_shuffle, so unconditional classification leaves other pools alone.
  if (Souls_ItemIsSoul(item_id)) return true;
  // Virtual items (121+, except 125/souls above) — NOT in pool; not progression.
  return false;
}

bool Rando_IsProgressionItem(uint16 item_id) {
  return is_progression_item(item_id);
}

static bool predicate_is_vacuous_true(const uint8 *bc, uint16 len) {
  return len == 2 && bc != NULL && bc[0] == OP_AND && bc[1] == 0;
}

static bool predicate_is_vacuous_false(const uint8 *bc, uint16 len) {
  return len == 2 && bc != NULL && bc[0] == OP_OR && bc[1] == 0;
}

typedef struct PlacementProfileCounters {
  uint64 place_calls;
  uint64 place_attempts;
  uint64 place_successes;
  uint64 place_exhausted;
  uint64 place_failures;
  uint64 open_locations_sum;
  uint64 progression_items_sum;
  uint64 candidate_scan_slots;
  uint64 accept_checks;
  uint64 accept_fast_true;
  uint64 accept_fast_false;
  uint64 accept_vm_can_place;
  uint64 accept_vm_always_allow;
  uint16 max_open_locations;
  uint16 max_progression_items;
} PlacementProfileCounters;

static PlacementProfileCounters g_place_profile;
static bool g_place_profile_active;

static void placement_profile_dump(void) {
  if (!g_place_profile_active) return;
  fprintf(stderr,
          "[RANDO_PROFILE placement] calls=%llu attempts=%llu successes=%llu "
          "exhausted=%llu failures=%llu open_sum=%llu prog_sum=%llu "
          "max_open=%u max_prog=%u candidate_scan_slots=%llu "
          "accept_checks=%llu fast_true=%llu fast_false=%llu "
          "vm_can_place=%llu vm_always_allow=%llu\n",
          (unsigned long long)g_place_profile.place_calls,
          (unsigned long long)g_place_profile.place_attempts,
          (unsigned long long)g_place_profile.place_successes,
          (unsigned long long)g_place_profile.place_exhausted,
          (unsigned long long)g_place_profile.place_failures,
          (unsigned long long)g_place_profile.open_locations_sum,
          (unsigned long long)g_place_profile.progression_items_sum,
          (unsigned)g_place_profile.max_open_locations,
          (unsigned)g_place_profile.max_progression_items,
          (unsigned long long)g_place_profile.candidate_scan_slots,
          (unsigned long long)g_place_profile.accept_checks,
          (unsigned long long)g_place_profile.accept_fast_true,
          (unsigned long long)g_place_profile.accept_fast_false,
          (unsigned long long)g_place_profile.accept_vm_can_place,
          (unsigned long long)g_place_profile.accept_vm_always_allow);
}

static void placement_profile_maybe_init(void) {
  static bool initialized;
  if (initialized) return;
  initialized = true;
  const char *v = getenv("ZELDA3_RANDO_PROFILE");
  g_place_profile_active = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
  if (g_place_profile_active) atexit(placement_profile_dump);
}

// Fisher-Yates over a uint16 array, using Rng_NextRange for unbiased picks.
static void shuffle_u16(uint16 *arr, uint16 n, RandoRng *rng) {
  for (int i = (int)n - 1; i > 0; i--) {
    uint32 j = Rng_NextRange(rng, (uint32)(i + 1));
    uint16 tmp = arr[i];
    arr[i] = arr[(uint16)j];
    arr[(uint16)j] = tmp;
  }
}

// Determine if `loc` is inside a dungeon. Returns 0..12 (dungeon id) or 0xFF.
// also consults Rando_FindPredicateOverride so a per-world-state
// override that moves a location across a dungeon boundary is honored.
// Today no Inverted override crosses dungeons (Ether/Spectacle Rock are
// overworld), but the guard prevents a latent regression when Slice 3+
// Inverted-only locations land.
static uint8 dungeon_id_for_location(const RandoLocationDef *loc,
                                     const RandoSettings *settings) {
  uint16 effective_region = loc->region_id;
  if (settings != NULL) {
    const RandoLocationPredOverride *ov =
        Rando_FindPredicateOverride(loc->id, settings->world_state);
    if (ov != NULL && ov->region_override != 0xFFFF) {
      effective_region = ov->region_override;
    }
  }
  if (effective_region == 0xFFFF) return 0xFF;
  for (uint32 i = 0; i < kRandoRegionsCount; i++) {
    if (kRandoRegions[i].id == effective_region) {
      uint8 dg = kRandoRegions[i].dungeon_id;
      if (dg == 0xFF) return 0xFF;
      return dg;
    }
  }
  return 0xFF;
}

// Per spec scenario "Dungeon-mode small key stays in its dungeon" — and the
// equivalent for big keys / maps / compasses. When the corresponding
// dungeon_items.* mode is Dungeon, the item is only acceptable at locations
// inside the matching dungeon. Returns true if the placement is allowed.
static bool dungeon_mode_accepts_item(const RandoLocationDef *loc,
                                      uint16 candidate_item,
                                      const RandoSettings *settings) {
  uint8 item_dungeon = Rando_RandoDungeonFromDungeonItem(candidate_item);
  if (item_dungeon == 0xFF) return true;  // not a dungeon item — always OK
  // Determine the active mode for this item class.
  uint8 mode;
  if (Rando_IsSmallKeyItem(candidate_item) || Rando_IsKeyRingItem(candidate_item)) {
    mode = Settings_EffectiveSmallKeysMode(settings);
  } else if (candidate_item >= 66 && candidate_item <= 76) {
    mode = Settings_EffectiveBigKeysMode(settings);
  } else if (candidate_item == 124 || (candidate_item >= 77 && candidate_item <= 87)) {
    mode = settings->dungeon_maps_mode;
  } else if (candidate_item >= 88 && candidate_item <= 98) {
    mode = settings->dungeon_compasses_mode;
  } else {
    return true;
  }
  if (mode != kDungeonItemMode_Dungeon) {
    // Vanilla mode pre-pins these locations; Wild mode allows them anywhere.
    // Only Dungeon mode requires per-dungeon containment.
    return true;
  }
  uint8 loc_dungeon = dungeon_id_for_location(loc, settings);
  return loc_dungeon == item_dungeon;
}

// Evaluate can_place OR always_allow for a given (location, candidate_item) pair.
static bool location_accepts_item(const RandoLocationDef *loc,
                                  uint16 candidate_item,
                                  const RandoCounts *counts,
                                  const RandoSettings *settings,
                                  uint16 selected_key_rings_mask) {
  if (g_place_profile_active) g_place_profile.accept_checks++;
  if (settings != NULL && loc->type == LOCTYPE_Enemy &&
      Settings_EnemyChecksAllActive(settings) &&
      enemy_check_overworld_stage(loc->id) != kEnemyCheckNotOverworld &&
      is_progression_item(candidate_item)) {
    return false;
  }

  // Dungeon-containment check (spec: "Dungeon-mode small key stays in its
  // dungeon" + same for big keys / maps / compasses). Runs before the
  // predicate VM so the can_place YAML doesn't need to enumerate every
  // dungeon item per location.
  if (!dungeon_mode_accepts_item(loc, candidate_item, settings)) return false;

  // Door shuffle — bk_restricted placement ban (add-rando-door-shuffle):
  // the key-door prover marks the locations reachable ONLY past a dungeon's
  // (un-relocated) big-key door; placing that big key there self-locks the
  // seed in a way the reachability gate alone cannot catch. Inert when no
  // door layout is installed.
  if (candidate_item >= 66 && candidate_item <= 76) {  // BigKey_* ids
    const struct DoorShuffleLayout *dl = Rando_GetDoorLogicLayout(NULL);
    if (dl != NULL && loc->type == LOCTYPE_Pot && pot_active(loc, settings))
      return false;
    if (dl != NULL && DoorShuffle_BkRestricted(dl, loc->id))
      return false;
  }

  // Phase B Slice 2 — per-world-state can_place/always_allow override.
  // Inverted seeds may have different can_place predicates per location.
  uint32 cp_offset = loc->can_place_offset;
  uint16 cp_length = loc->can_place_length;
  uint32 aa_offset = loc->always_allow_offset;
  uint16 aa_length = loc->always_allow_length;
  const RandoLocationPredOverride *ov =
      Rando_FindPredicateOverride(loc->id, settings->world_state);
  if (ov != NULL) {
    cp_offset = ov->can_place_offset;
    cp_length = ov->can_place_length;
    aa_offset = ov->always_allow_offset;
    aa_length = ov->always_allow_length;
  }
  // can_place defaults to TRUE() when not overridden — the bytecode for
  // the default is "AND with 0 children" (2 bytes 0x0c 0x00), which
  // Predicate_EvaluatePlacement evaluates as true.
  const uint8 *cp_bc = kRandoPredicateStream + cp_offset;
  if (predicate_is_vacuous_true(cp_bc, cp_length)) {
    if (g_place_profile_active) g_place_profile.accept_fast_true++;
    return true;
  }
  if (g_place_profile_active) g_place_profile.accept_vm_can_place++;
  if (Predicate_EvaluatePlacement(
          cp_bc, cp_length, counts, settings, candidate_item,
          selected_key_rings_mask)) {
    return true;
  }
  // always_allow defaults to FALSE() (OR with 0 children, 0x0d 0x00) — also
  // evaluatable safely. If it returns true, the placement is permitted even
  // when can_place rejected.
  const uint8 *aa_bc = kRandoPredicateStream + aa_offset;
  if (settings == NULL || settings->logic < 4 /* NoLogic */) {
    if (predicate_is_vacuous_false(aa_bc, aa_length)) {
      if (g_place_profile_active) g_place_profile.accept_fast_false++;
      return false;
    }
  }
  if (g_place_profile_active) g_place_profile.accept_vm_always_allow++;
  return Predicate_EvaluatePlacement(
      aa_bc, aa_length, counts, settings, candidate_item,
      selected_key_rings_mask);
}

// Single-attempt inner implementation of assumed-fill. Returns true on
// "every progression item placed at a reachable location" (success), false
// otherwise (forward-fill fallback fired at least once). The outer
// Place_AssumedFill wraps this with a bounded-retry loop.
static bool place_assumed_fill_attempt(const RandoSettings *settings,
                                       const uint16 *pool,
                                       uint16 pool_n,
                                       uint16 selected_key_rings_mask,
                                       uint64 seed_u64,
                                       RandoPlacementTable *out,
                                       uint16 *out_fallback_count);
static bool accessibility_reachability_ok(const RandoSettings *settings,
                                          const RandoPlacementTable *placements,
                                          const RandoSpheres *spheres);
// Goal_IsCompletable — declared in rando_placement.h (used for the none-tier
// attempt-accept bar below).

// ---------------------------------------------------------------------------
// Place_AssumedFill — Phase A1 implementation.
//
// Algorithm (per `randomizer-placement / Assumed-fill placement`):
//   1. Build the per-settings item pool via BuildItemPool.
//   2. Partition into progression (~70 items) vs junk (~165).
//   3. Shuffle progression order; "assumed inventory" = counts of all
//      progression items still unplaced.
//   4. For each progression item (in shuffled order):
//        a. Remove from assumed inventory.
//        b. Run Logic_ComputeReachabilityFullKnowledge with current counts.
//        c. Filter open locations to reachable + can_place(item).
//        d. If candidates: pick a random one (Rng_NextRange).
//        e. If no candidates: forward-fill fallback.
//   5. Shuffle junk; fill remaining open locations.
//   6. Run sphere computation against the produced placement; if any
//      progression item is unreachable, retry with a perturbed seed.
//   7. After kAssumedFillMaxAttempts retries, accept the last attempt and
//      surface a fallback warning. (Aligns with the spec's "forward-fill
//      fallback after timeout" — bounded-rewind is the in-attempt retry
//      strategy; this is the cross-attempt retry strategy.)
// ---------------------------------------------------------------------------
#define kAssumedFillMaxAttempts 256

// Per-item bounded rewind (Bug #7 / add-rando-trick-logic-and-axes design D1).
// When a progression item has no reachable+placeable location, the placer
// rewinds the last kPerItemRewindWindow placed progression items (reopening
// their slots + returning them to the assumed inventory), reshuffles that
// window, and retries — exploring a different local arrangement before falling
// back to the reachability-ignoring forward-fill. Bounded by
// kPerItemRewindBudget rewind events per attempt so it always terminates.
//
// Setting kPerItemRewindBudget to 0 disables the rewind entirely: the placer
// then reproduces the pre-Bug-#7 forward-fill behavior byte-for-byte (the
// happy path — an item with a candidate — is identical either way, drawing the
// same single Rng_NextRange). Any positive value only diverges on the
// no-candidate path, which default/easy seeds never hit, so their
// placement_digests are preserved regardless.
//
// SHIPPED GATED OFF (=0). Rationale: the while-loop refactor is verified
// byte-identical at budget=0 (corpus 79/79). At budget=10 (design D1's value),
// 11 hard corpus seeds change and all stay goal_completable, BUT the design-D1
// reshuffle-and-retry algorithm did NOT improve placement quality on the hard
// seeds measured — TH/expert/0xABC123 went 1→3 forward-fills, std ganon-hunt
// uses 10 — i.e. it churns the RNG without reducing the forward-fill safety-
// valve usage. Enabling it would change 11 corpus digests + force a kGen bump
// for no demonstrated benefit, and the quality outcome can only be judged by
// playtest. The mechanism is complete,
// terminating, and completability-preserving; flip to 10 and re-bench on a
// known-hard seed to evaluate/tune (design D1 explicitly calls N=10 "a guess").
#define kPerItemRewindBudget 0   // 0 = disabled (shipped); design D1 value is 10
#define kPerItemRewindWindow 10  // # of placements rewound per event (design D1 "last N")

static PlacementStats g_last_placement_stats;

const PlacementStats *Placement_GetLastStats(void) {
  return &g_last_placement_stats;
}

static uint64 placement_attempt_seed(uint64 base_seed, uint8 attempt) {
  return base_seed ^ ((uint64)attempt * 0x9E3779B97F4A7C15ull);
}

static void install_shuffle_assignments_for_attempt(const RandoSettings *settings,
                                                    uint64 base_seed,
                                                    uint8 attempt) {
  uint8 prize_assignment[kRandoDungeonCount];
  uint8 medallion_assignment[kRandoMedallionEntranceCount];
  RandoRng shuffle_rng;
  Rng_SeedFromU64(&shuffle_rng, placement_attempt_seed(base_seed, attempt));
  PrizeShuffle_Run(settings, &shuffle_rng, prize_assignment);
  MedallionShuffle_Run(settings, &shuffle_rng, medallion_assignment);
  Rando_SetDungeonPrizeAssignment(prize_assignment);
  Rando_SetMedallionAssignment(medallion_assignment);
}

bool Place_AssumedFill(const RandoSettings *settings,
                       uint64 seed_u64,
                       int budget_seconds,
                       RandoPlacementTable *out) {
  placement_profile_maybe_init();
  if (g_place_profile_active) g_place_profile.place_calls++;
  // Reset stats — caller reads via Placement_GetLastStats() after we return.
  memset(&g_last_placement_stats, 0, sizeof(g_last_placement_stats));
  if (settings == NULL || out == NULL || out->entries == NULL) return false;
  // add-rando-random-crystals — install the resolved tower requirement for
  // OP_TOWER_CRYSTALS_MET before any reachability flood. BASE seed, never an
  // attempt seed (same rule as every other per-seed resolve).
  {
    uint8 eff_tower;
    Crystals_Resolve(settings, seed_u64, NULL, &eff_tower);
    Logic_SetResolvedTowerCrystals((uint8)(eff_tower + 1));
  }
  // Customizer mode — clear any stale hard-error from a prior call. The
  // attempt function sets it on an illegal pin (see §3c).
  Customizer__ClearError();

  // Pool shape and Key Ring selection depend only on the base seed/settings,
  // never on the retry seed. Build and resolve them once so the collapsed
  // families and OP_ITEM_IS aliases are one inseparable plan across attempts.
  uint16 base_pool[kRandoLocationCapacity];
  KeyRingSelection key_rings;
  uint16 base_pool_n = build_item_pool_internal(
      settings, seed_u64, base_pool, kRandoLocationCapacity, &key_rings);
  if (base_pool_n == 0) return false;

  // Boss-shuffle runtime — install the per-seed boss assignment into the LOGIC
  // VM so OP_CAN_KILL_BOSS gates each `- Boss`/`- Prize` on the SHUFFLED boss's
  // kill predicate. Computed from the BASE seed (NOT the per-attempt seed) so it
  // matches the runtime install (Rando_ActivateSidecarSlot regenerates from the
  // base seed too) — the assignment is attempt-independent. boss_shuffle off ⇒
  // vanilla identity ⇒ OP_CAN_KILL_BOSS resolves to the vanilla boss-kill
  // predicate (placement byte-identical). The setter copies the bytes into
  // owned reachability state, so the local generation source need not outlive
  // this call.
  uint8 boss_assignment_for_reach[16];
  BossShuffle_ComputeAssignment(settings, seed_u64, boss_assignment_for_reach);
  Rando_SetBossAssignment(boss_assignment_for_reach);

  // Budget timer: if budget_seconds > 0, abort additional retry attempts once
  // the elapsed CPU time exceeds the budget. Each individual attempt still runs
  // to completion; the budget only gates the retry loop.
  //
  // DETERMINISM WARNING: a positive budget DOES affect output. When no attempt
  // fully completes, the loop ships the best-so-far; cutting the loop off early
  // (on a slow/loaded machine) selects a different best-so-far than a fast run,
  // so placement_digest + goal_completable become machine-dependent for any seed
  // that needs more retries than the budget allows. Pass budget_seconds=0 (the
  // headless --generate-seed default and the slot-generator default) for a fully
  // deterministic run to the fixed kAssumedFillMaxAttempts cap — the corpus's
  // cross-platform byte-identical contract REQUIRES budget=0. A positive budget
  // is only for batch/debug callers that knowingly accept non-determinism for a
  // time bound. (clock() is allowed by the determinism guard's wall-clock
  // blocklist, but that does NOT make a positive budget deterministic.)
  clock_t start_clock = clock();

  uint16 best_unreachable = 0xFFFF;
  uint16 best_fallback = 0xFFFF;
  uint16 best_score_cached = 0xFFFF;  // includes the no-core-weapon penalty
  static RandoPlacement best_entries[kRandoLocationCapacity];
  uint16 best_count = 0;
  bool best_complete = false;
  // FIX #6 — track which attempt produced the best-scored placement. The
  // prize/medallion shuffle baked into that attempt's table was seeded from
  // attempt_seed (base ^ best_attempt*0x9E37..), so the runtime must persist +
  // re-apply best_attempt, not re-derive from the base seed. (kAssumedFillMax-
  // Attempts is 256, so the index fits a uint8: range 0..255.)
  uint8 best_attempt = 0;

  for (int attempt = 0; attempt < kAssumedFillMaxAttempts; attempt++) {
    if (budget_seconds > 0 && attempt > 0) {
      // After the first attempt, check the budget before starting another.
      // (Always run attempt 0 so a tiny budget doesn't produce zero output.)
      // Integer comparison avoids the determinism-guard's float/double ban.
      clock_t now = clock();
      clock_t budget_ticks = (clock_t)budget_seconds * (clock_t)CLOCKS_PER_SEC;
      if ((clock_t)(now - start_clock) >= budget_ticks) {
        fprintf(stderr,
          "Place_AssumedFill: %d-second budget exhausted after %d attempt(s); "
          "accepting best-so-far.\n",
          budget_seconds, attempt);
        break;
      }
    }
    if (g_place_profile_active) g_place_profile.place_attempts++;
    g_last_placement_stats.attempts_used = (uint8)(attempt + 1);
    // Perturb the seed per attempt so each retry produces a different
    // progression order. The first attempt uses the unmodified seed so
    // single-seed determinism holds when the first attempt succeeds.
    uint64 attempt_seed = placement_attempt_seed(seed_u64, (uint8)attempt);
    uint16 fallback_count = 0;
    if (!place_assumed_fill_attempt(settings, base_pool, base_pool_n,
                                    key_rings.selected_mask, attempt_seed,
                                    out, &fallback_count)) {
      // A customizer pin error is deterministic (the pins don't depend on the
      // per-attempt seed), so retrying is futile — fail now and let the caller
      // surface Customizer_LastError().
      if (Customizer_LastError()[0] != '\0') return false;
      // Otherwise the inner attempt failed catastrophically (couldn't place an
      // item even with forward-fill). Continue retrying with a new seed.
      continue;
    }
    // Check completability via sphere computation.
    RandoSpheres spheres;
    bool full_reach = Logic_ComputeSpheres(settings, out, &spheres);
    // ALTTPR's Standard-mode lock-in-house ROM patch isn't shipped in this
    // port: the player can technically walk to Light World from game start
    // but combat-blocking guards make it impractical without a weapon. The
    // 5 always-reachable slots that need no weapon (Link's House chest,
    // Uncle, Sewers Dark Cross, HC Map Chest, Secret Passage) MUST contain
    // at least one canKillEscapeThings-satisfying item or the seed is a
    // soft-softlock. Reject attempts that fail this.
    bool has_core_weapon = false;
    bool has_escape_lamp = false;
    if (settings->world_state == kWorldState_Standard) {
      // The 4 sphere-0 slots — reachable from game start without combat.
      static const uint16 kCoreLocIds[] = {
        LOC_Link_s_House,
        LOC_Link_s_Uncle,
        LOC_Hyrule_Castle_Map_Chest,
        LOC_Secret_Passage,
      };
      // Lamp must be reachable BEFORE the dark sewers gate (Sanctuary etc.).
      // That means either at the 4 sphere-0 slots OR at one of the 2 HCE
      // chests that need only canKillEscapeThings (Boomerang Chest, Zelda's
      // Cell). All other locations require RescuedZelda, which itself
      // requires Lamp via the Sanctuary gate — placing Lamp anywhere else
      // creates a cycle and softlocks the escape.
      static const uint16 kEscapeLocIds[] = {
        LOC_Link_s_House,
        LOC_Link_s_Uncle,
        LOC_Hyrule_Castle_Map_Chest,
        LOC_Secret_Passage,
        LOC_Hyrule_Castle_Boomerang_Chest,
        LOC_Hyrule_Castle_Zelda_s_Cell,
      };
      // Items that satisfy canKillEscapeThings (matches macros.yaml
      // CanKillEscapeThings body: HasSword OR Somaria OR Byrna OR
      // CanBombThings OR CanShootArrowsL1 OR Hammer OR FireRod).
      static const uint16 kWeaponItemIds[] = {
        ITEM_ProgressiveSword, ITEM_L1Sword, ITEM_L2Sword, ITEM_L3Sword, ITEM_L4Sword,
        ITEM_CaneOfSomaria, ITEM_CaneOfByrna,
        ITEM_Bow, ITEM_ProgressiveBow,
        ITEM_Hammer, ITEM_FireRod,
        ITEM_Bombs1, ITEM_Bombs3, ITEM_Bombs10,
      };
      for (uint16 i = 0; i < out->count; i++) {
        uint16 loc = out->entries[i].location_id;
        uint16 item = out->entries[i].item_id;
        // add-npc-souls: with Uncle soul-gated, an item AT Link's Uncle is
        // uncollectable until his soul is found — it cannot satisfy the
        // sphere-0 weapon/lamp bars (the exact soft-softlock these bars
        // exist to prevent). Skip the Uncle slot when npc_souls is on.
        if (settings->npc_souls && loc == LOC_Link_s_Uncle) continue;
        if (!has_core_weapon) {
          for (size_t c = 0; c < sizeof(kCoreLocIds)/sizeof(kCoreLocIds[0]); c++) {
            if (loc != kCoreLocIds[c]) continue;
            for (size_t w = 0; w < sizeof(kWeaponItemIds)/sizeof(kWeaponItemIds[0]); w++) {
              if (item == kWeaponItemIds[w]) { has_core_weapon = true; break; }
            }
            break;
          }
        }
        if (!has_escape_lamp && item == ITEM_Lamp) {
          for (size_t e = 0; e < sizeof(kEscapeLocIds)/sizeof(kEscapeLocIds[0]); e++) {
            if (loc == kEscapeLocIds[e]) { has_escape_lamp = true; break; }
          }
        }
      }
    } else {
      // Open/Inverted/Retro: not gated on HC escape combat / sewer dark.
      has_core_weapon = true;
      has_escape_lamp = true;
    }
    // Attempt-accept bar = the ACCEPTANCE bar (Accessibility_SeedAcceptable),
    // per effective tier — NOT unconditional full reach. Full reach is
    // unattainable for combos with modeling-stranded junk (e.g. wild keys ×
    // dungeon enemy checks permanently strand ~63 junk-holding checks), and
    // demanding it burned all 256 attempts (~2m45s with the placed-item
    // collection fix-point) on seeds whose attempt 0 already satisfied the
    // real acceptance gate. locations: every placement reachable (== the old
    // bar). items (default): every PROGRESSION placement reachable. none
    // ("beatable only"): goal completability — the caller's refusal gate —
    // so hunt seeds still retry toward an attempt with enough reachable
    // pieces instead of shipping an attempt the caller must refuse.
    bool attempt_ok;
    if (Settings_EffectiveAccessibility(settings) == kAccessibility_None) {
      attempt_ok = Goal_IsCompletable(settings, seed_u64, out);  // BASE seed (crystal resolve)
    } else {
      // Goal completability is ALSO required (hardening, fresh-eyes review):
      // the reachability bar walks placement ENTRIES, while goal predicates
      // additionally test event locations that never hold pool items (Ganon,
      // Agahnim 2). All-progression-reachable implies those today (prizes are
      // progression placements, and full inventory opens the goal events), so
      // this is belt-and-suspenders that keeps the accept bar provably >= the
      // caller's refusal gate (Accessibility_SeedAcceptable) if a future goal
      // gains a requirement reachability alone does not imply.
      // ORDER MATTERS for wall-clock, not semantics: Goal_IsCompletable runs a
      // full reachability fix-point, so it must be the LAST conjunct — after
      // the cheap fallback/weapon/lamp gates — or it re-runs on every
      // bar-passing-but-doomed attempt (the door-layout search has hundreds;
      // evaluating it eagerly pushed the two 0xD004 door corpus entries past
      // the 60s runner budget). All conjuncts are also re-tested in the
      // accept `if` below, so short-circuit order cannot change the accepted
      // set (digest-inert).
      attempt_ok = accessibility_reachability_ok(settings, out, &spheres) &&
                   fallback_count == 0 && has_core_weapon && has_escape_lamp &&
                   Goal_IsCompletable(settings, seed_u64, out);  // BASE seed (crystal resolve)
    }
    (void)full_reach;
    if (attempt_ok && fallback_count == 0 && has_core_weapon && has_escape_lamp) {
      // Accepted — record the attempt's own stats (stranded junk is allowed
      // by the items tier and surfaces in the spoiler's unreachable list).
      g_last_placement_stats.forward_fill_fallback_count = 0;
      g_last_placement_stats.best_unreachable_count = spheres.unreachable_count;
      // FIX #6 — this attempt's per-attempt seed (attempt_seed above) is what
      // seeded the prize/medallion shuffle baked into `out`; persist it.
      g_last_placement_stats.prize_attempt = (uint8)attempt;
      if (g_place_profile_active) g_place_profile.place_successes++;
      return true;
    }
    // Track best-so-far (fewest unreachable + fewest fallbacks). Treat
    // "no core weapon" and "no escape-reachable Lamp" each as ~20 extra
    // unreachables for ranking purposes — they're effectively softlocks
    // even though the placer's local reachability thinks they're fine.
    uint16 effective_unreachable = spheres.unreachable_count
        + (has_core_weapon ? 0 : 20)
        + (has_escape_lamp ? 0 : 20);
    uint16 score = effective_unreachable * 100 + fallback_count;
    if (score < best_score_cached) {
      best_score_cached = score;
      best_unreachable = spheres.unreachable_count;
      best_fallback = fallback_count;
      best_count = out->count;
      for (uint16 i = 0; i < out->count; i++) best_entries[i] = out->entries[i];
      best_complete = full_reach;
      best_attempt = (uint8)attempt;  // FIX #6 — attempt that baked this table's prize/medallion
    }
  }
  // All attempts exhausted; restore the best-scored placement.
  if (best_unreachable == 0xFFFF) {
    fprintf(stderr, "Place_AssumedFill: no attempt produced a placement\n");
    if (g_place_profile_active) g_place_profile.place_failures++;
    return false;
  }
  for (uint16 i = 0; i < best_count; i++) out->entries[i] = best_entries[i];
  out->count = best_count;
  g_last_placement_stats.forward_fill_fallback_count = best_fallback;
  g_last_placement_stats.best_unreachable_count = best_unreachable;
  // FIX #6 — the restored table is `best_entries`, baked by attempt `best_attempt`.
  g_last_placement_stats.prize_attempt = best_attempt;
  install_shuffle_assignments_for_attempt(settings, seed_u64, best_attempt);
  fprintf(stderr,
          "Place_AssumedFill: best of %d attempts: %u unreachable placement(s), %u forward-fill fallback(s).\n",
          kAssumedFillMaxAttempts, (unsigned)best_unreachable, (unsigned)best_fallback);
  // Return true to signal "a placement was produced" — the caller checks
  // goal_completable / sphere unreachable_count to decide whether the seed
  // is actually playable. (best_complete may be false; we still keep the
  // best-scored output so the spoiler surfaces diagnostic info.)
  (void)best_complete;
  if (g_place_profile_active) g_place_profile.place_exhausted++;
  return true;
}

// ---------------------------------------------------------------------------
// Phase B Slice 3b — Retro TakeAny per-seed selection.
//
// ALTTPR declares 31 Shop\TakeAny (app/Region/Standard/**). Per
// app/Randomizer.php:716-735, in Retro the generator picks 4 "potion" caves
// (BluePotion@slot0 + BossHeartContainer@slot1) via randomCollection(4) and a
// 5th "weapon" cave (ProgressiveSword, or Rupee300 when mode.weapons is
// vanilla/swordless) via ->random(). The other 26 caves are inactive.
//
// Cave index ordering MUST match assets/rando/location_registry.yaml: registry
// id = 266 + 2*cave_index (slot 0) and +1 (slot 1). See design.md §3/§D4.
// Only the LOCs of active caves are emitted into the placement table (the slot-1
// LOC of the weapon cave is NOT emitted — it has a single inventory item).
//
// The runtime does NOT re-run this selection; it reads the placement table to
// learn which caves are active and what each slot grants. The runtime's own
// (door_id, host_entrance) cave table lives in src/rando/rando.c.
// (kTakeAnyCaveCount / kTakeAnyLocBase defined near the top; LOCTYPE_TakeAny in rando_logic.h.)
// ---------------------------------------------------------------------------
enum { kTakeAnyRole_Inactive = 0, kTakeAnyRole_Potion = 1, kTakeAnyRole_Weapon = 2 };

// Deterministic, pick-without-replacement (array_splice-style, matching ALTTPR
// randomCollection) selection over the 31-cave list, from a dedicated RNG
// salted off the seed so the main fill RNG stream is untouched. Mirrors the
// prize/medallion-shuffle pattern in place_assumed_fill_attempt. Writes a role
// per cave into roles_out[31]. No-op (all inactive) outside Retro.
static void takeany_select(const RandoSettings *settings, uint64 seed_u64,
                           uint8 roles_out[kTakeAnyCaveCount]) {
  for (uint8 i = 0; i < kTakeAnyCaveCount; i++) roles_out[i] = kTakeAnyRole_Inactive;
  if (settings == NULL || settings->world_state != kWorldState_Retro) return;

  RandoRng rng;
  Rng_SeedFromU64(&rng, seed_u64 ^ 0x54616B65416E79ull);  // "TakeAny" salt

  uint8 cand[kTakeAnyCaveCount];
  uint8 n = kTakeAnyCaveCount;
  for (uint8 i = 0; i < n; i++) cand[i] = i;

  // 4 potion caves (randomCollection(4)).
  for (uint8 p = 0; p < 4 && n > 0; p++) {
    uint32 pick = Rng_NextRange(&rng, n);
    roles_out[cand[pick]] = kTakeAnyRole_Potion;
    for (uint8 j = (uint8)pick; (uint8)(j + 1) < n; j++) cand[j] = cand[j + 1];  // splice out
    n--;
  }
  // 5th weapon cave (->random() over remaining inactive).
  if (n > 0) {
    uint32 pick = Rng_NextRange(&rng, n);
    roles_out[cand[pick]] = kTakeAnyRole_Weapon;
  }
}

// Resolve the pinned reward item id for a TakeAny location id under the seed's
// role assignment, or 0xFFFF if that LOC is NOT active this seed (inactive cave,
// or the weapon cave's unused slot-1). Used both to skip inactive LOCs in the
// open-location collection and to pin active LOCs in the placer.
static uint16 takeany_reward(const RandoSettings *settings,
                             const uint8 roles[kTakeAnyCaveCount], uint16 loc_id) {
  if (loc_id < kTakeAnyLocBase ||
      loc_id >= (uint16)(kTakeAnyLocBase + 2 * kTakeAnyCaveCount)) return 0xFFFF;
  uint16 rel = (uint16)(loc_id - kTakeAnyLocBase);
  uint8 cave = (uint8)(rel / 2);
  uint8 slot = (uint8)(rel % 2);
  switch (roles[cave]) {
    case kTakeAnyRole_Potion:
      return slot == 0 ? (uint16)ID_BluePotion : (uint16)ID_BossHeartContainer;
    case kTakeAnyRole_Weapon:
      if (slot != 0) return 0xFFFF;  // weapon cave has only slot 0
      // mode.weapons vanilla(2)/swordless(3) -> 300 rupees, else ProgressiveSword
      // (per app/Randomizer.php:733). 2/3 are reserved per rando_settings.h:56-57
      // and currently unreachable, so today this is always ProgressiveSword.
      return (settings->mode_weapons == 2 || settings->mode_weapons == 3)
                 ? (uint16)ID_Rupee300 : (uint16)ID_ProgressiveSword;
    default:
      return 0xFFFF;  // inactive
  }
}

// Customizer mode — remove ONE instance of `item` from the to-place pool
// (progression[] first, then junk[]). Preserves the dungeon-prefix invariant
// (the first *dungeon_prog_n entries of progression[] are dungeon items, placed
// first). Returns true if an instance was found+removed, false if `item` was
// not in the pool (an out-of-pool pin — harmless; see the call site).
static bool customizer_pool_remove_one(uint16 *progression, uint16 *prog_n,
                                       uint16 *dungeon_prog_n, uint16 *junk,
                                       uint16 *junk_n, uint16 item) {
  for (uint16 t = 0; t < *prog_n; t++) {
    if (progression[t] != item) continue;
    bool in_dungeon_prefix = (t < *dungeon_prog_n);
    for (uint16 u = t; u + 1 < *prog_n; u++) progression[u] = progression[u + 1];
    (*prog_n)--;
    if (in_dungeon_prefix && *dungeon_prog_n > 0) (*dungeon_prog_n)--;
    return true;
  }
  for (uint16 t = 0; t < *junk_n; t++) {
    if (junk[t] != item) continue;
    for (uint16 u = t; u + 1 < *junk_n; u++) junk[u] = junk[u + 1];
    (*junk_n)--;
    return true;
  }
  return false;
}

// Customizer pool_overrides `add`: insert `item` into the to-place pool,
// mirroring BuildItemPool's partition — a dungeon item (small/big key, ids
// 53..76) goes to the FRONT of the dungeon prefix, other progression appends to
// progression[], junk appends to junk[]. Capacity-guarded (a full array drops
// the add). is_progression_item classifies exactly as the pool builder does.
static void customizer_pool_add_one(uint16 *progression, uint16 *prog_n,
                                    uint16 *dungeon_prog_n, uint16 prog_cap,
                                    uint16 *junk, uint16 *junk_n, uint16 junk_cap,
                                    uint16 item) {
  if (is_progression_item(item)) {
    if (*prog_n >= prog_cap) return;
    bool is_dungeon_item = (item >= 53 && item <= 76) || Rando_IsKeyRingItem(item);
    if (is_dungeon_item) {
      for (uint16 j = *prog_n; j > *dungeon_prog_n; j--) progression[j] = progression[j - 1];
      progression[*dungeon_prog_n] = item;
      (*dungeon_prog_n)++;
      (*prog_n)++;
    } else {
      progression[(*prog_n)++] = item;
    }
  } else {
    if (*junk_n >= junk_cap) return;
    junk[(*junk_n)++] = item;
  }
}

// per-item legality caps for customizer manifests. The runtime can
// only honor a bounded number of copies of the progressive items and bottles:
//
//   - progressive_to_lttp (rando.c) returns 0xFF once an item's tier ladder is
//     exhausted (sword 4 tiers, shield 3, armor 2, glove 2, bow 2), and
//     Rando_DispatchVanillaGrant's 0xFF fallback then grants the slot's
//     VANILLA ROM item — so a 5th ProgressiveSword grants junk or, worse, a
//     duplicate of the slot's vanilla progression item.
//   - bottles: ItemReceipt_GiveBottledItem (misc.c) fills the first empty
//     link_bottle_info slot; there are exactly 4 slots and a 5th bottle of ANY
//     variant is a silent no-op. All 7 bottle variants count against one cap.
//
// Per item id, the count the placer will emit is max(pins, pool_after) where
// pool_after = in-pool copies − effective removes + adds (each pin consumes a
// matching pool copy when one exists; out-of-pool pins are pure overflow, per
// the pin loop below). Sum that over each cap class and reject the manifest
// when it exceeds the cap. Validated against the settings-dependent pool
// BEFORE any override/pin mutates it, as a HARD deterministic error through
// the customizer error channel — surfaced by both the CLI (--customizer) and
// the native settings window. Non-progressive duplicates (a 2nd Lamp, a 2nd
// SilverArrowUpgrade, ...) are harmless never-downgrade re-grants and stay
// uncapped. Placement legality (location/dungeon confinement) is enforced
// separately at pin time.
static bool customizer_validate_item_caps(const CustomizerManifest *cm,
                                          const uint16 *progression, uint16 prog_n,
                                          const uint16 *junk, uint16 junk_n) {
  static const struct { uint16 first, last; uint16 cap; const char *what; const char *why; } kCaps[] = {
    { ID_ProgressiveSword,  ID_ProgressiveSword,      4, "ProgressiveSword",       "the sword ladder has 4 tiers" },
    { ID_ProgressiveShield, ID_ProgressiveShield,     3, "ProgressiveShield",      "the shield ladder has 3 tiers" },
    { ID_ProgressiveArmor,  ID_ProgressiveArmor,      2, "ProgressiveArmor",       "the armor ladder has 2 tiers" },
    { ID_ProgressiveGlove,  ID_ProgressiveGlove,      2, "ProgressiveGlove",       "the glove ladder has 2 tiers" },
    { ID_ProgressiveBow,    ID_ProgressiveBow,        2, "ProgressiveBow",         "the bow ladder has 2 tiers" },
    { ID_BottleEmpty,       ID_BottleWithBluePotion,  4, "bottles (all variants)", "Link has 4 bottle slots" },
  };
  for (uint16 c = 0; c < (uint16)(sizeof kCaps / sizeof kCaps[0]); c++) {
    uint16 total = 0;
    for (uint16 id = kCaps[c].first; id <= kCaps[c].last; id++) {
      uint16 base = 0, removes = 0, adds = 0, pins = 0;
      for (uint16 i = 0; i < prog_n; i++) if (progression[i] == id) base++;
      for (uint16 i = 0; i < junk_n; i++) if (junk[i] == id) base++;
      for (uint16 r = 0; r < cm->pool_remove_count; r++) if (cm->pool_remove[r] == id) removes++;
      for (uint16 a = 0; a < cm->pool_add_count; a++) if (cm->pool_add[a] == id) adds++;
      for (uint16 p = 0; p < cm->pin_count; p++) if (cm->pins[p].item_id == id) pins++;
      // remove is best-effort: it cannot remove more copies than the pool has
      // (and removes apply BEFORE adds, so adds never feed removes).
      uint16 pool_after = (uint16)(base - (removes < base ? removes : base) + adds);
      total = (uint16)(total + (pins > pool_after ? pins : pool_after));
    }
    if (total > kCaps[c].cap) {
      char msg[160];
      snprintf(msg, sizeof msg,
               "manifest yields %u %s but at most %u can be granted (%s); "
               "remove pool copies or pins",
               (unsigned)total, kCaps[c].what, (unsigned)kCaps[c].cap, kCaps[c].why);
      Customizer__SetError(msg);
      return false;
    }
  }
  return true;
}

static bool customizer_validate_key_cardinality(const CustomizerManifest *cm,
                                                const uint16 *progression,
                                                uint16 prog_n,
                                                const uint16 *junk,
                                                uint16 junk_n) {
  // Parse-time validation handles ordinary manifests, but keep the generation
  // boundary fail-closed for programmatically constructed manifests/selftests.
  for (uint16 i = 0; i < cm->pool_remove_count; i++) {
    if (Rando_IsFixedKeyCardinalityItem(cm->pool_remove[i])) {
      Customizer__SetError(
          "pool_overrides cannot remove small keys, Key Rings, or Skeleton Key");
      return false;
    }
  }
  for (uint16 i = 0; i < cm->pool_add_count; i++) {
    if (Rando_IsFixedKeyCardinalityItem(cm->pool_add[i])) {
      Customizer__SetError(
          "pool_overrides cannot add small keys, Key Rings, or Skeleton Key");
      return false;
    }
  }

  uint16 pool_count[256] = {0};
  uint16 pin_count[256] = {0};
  for (uint16 i = 0; i < prog_n; i++)
    if (progression[i] < 256 && pool_count[progression[i]] != 0xFFFFu)
      pool_count[progression[i]]++;
  for (uint16 i = 0; i < junk_n; i++)
    if (junk[i] < 256 && pool_count[junk[i]] != 0xFFFFu)
      pool_count[junk[i]]++;
  for (uint16 i = 0; i < cm->pin_count; i++) {
    uint16 item = cm->pins[i].item_id;
    if (item < 256 && Rando_IsFixedKeyCardinalityItem(item) &&
        pin_count[item] != 0xFFFFu)
      pin_count[item]++;
  }

  for (uint16 item = 0; item < 256; item++) {
    if (!Rando_IsFixedKeyCardinalityItem(item) || pin_count[item] == 0) continue;
    if (pin_count[item] > pool_count[item]) {
      char msg[160];
      snprintf(msg, sizeof msg,
               "manifest pins %u copies of '%s' but this seed generated only %u; "
               "key items may be relocated but not minted",
               (unsigned)pin_count[item], Rando_GetItemName(item),
               (unsigned)pool_count[item]);
      Customizer__SetError(msg);
      return false;
    }
  }
  return true;
}

// Per-turn reachability with fix-point collection of already-placed pool items
// (the "assumed fill with fix-point reachability expansion" contract —
// ALTTPR app/Filler/RandomAssumed.php + World::collectItems). The assumed
// counts hold only UNPLACED pool items (+ the step-4 pre-pin grants); a pool
// item placed on an earlier turn must still count as obtainable when its
// location is reachable, or every location gated on it goes permanently dead
// for the rest of the fill. Pre-souls that conservatism was survivable (small
// gated clusters); the enemy-souls kill-room wraps gate whole dungeons on
// placed souls and collapsed the fill into forward-fill storms (a soul even
// landed on a location requiring itself, via the reachability-ignoring
// fallback). Deterministic: no RNG, order-independent (each pass adds every
// reachable uncollected placement, then recomputes until stable).
//
// Pins are collected too (prizes, events, TakeAny rewards, customizer pins) —
// they are gates' grants like anything else. The ONLY skipped pins are
// vanilla-MODE dungeon items: Rando_SeedVanillaDungeonItems already pre-granted
// those classes wholesale into the base counts (the ROM grants them in place),
// and collecting their pinned slots again would double-count keys into
// HAS_AMOUNT thresholds.
// KEEP IN SYNC with location_is_prepinned's per-class vanilla-mode ranges
// above (53..65 / 66..76 / 77..87+124 / 88..98): that function classifies the
// SLOTS the 3b pass pins, this one classifies the pinned ITEMS the collector
// must skip. A new dungeon-item class (or a mode carve-out like the key-pot
// rule in location_is_prepinned) must land in BOTH or the collector
// double-counts keys against HAS_AMOUNT thresholds.
static bool vanilla_mode_pregranted_item(uint16 item, const RandoSettings *s) {
  if (item >= 53 && item <= 65)
    return Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Vanilla;
  if (item >= 66 && item <= 76)
    return Settings_EffectiveBigKeysMode(s) == kDungeonItemMode_Vanilla;
  if ((item >= 77 && item <= 87) || item == 124)
    return s->dungeon_maps_mode == kDungeonItemMode_Vanilla;
  if (item >= 88 && item <= 98)
    return s->dungeon_compasses_mode == kDungeonItemMode_Vanilla;
  return false;
}

// Per-turn reachability for the EFFECTIVE souls_shuffle=all (bosses+enemies)
// tier: assumed(unplaced) + fix-point collection of committed items (placed on
// earlier turns + pins) whose locations are reachable — EXCEPT placed copies
// of the item id currently being placed.
//
// Scope: enemies-tier seeds ONLY (the caller falls back to the plain
// assumed-only model otherwise). The conservative pre-souls model — placed
// items vanish, pins assumed unconditionally — is a fill-guidance heuristic
// the worst-case key-threshold models (pot key depths, the door-key oracle)
// were calibrated against; the sphere walk validates every attempt either
// way, so neither model can ship an unsound seed. The enemies tier NEEDS this
// model: 46 souls gate whole-dungeon clusters, and under the conservative
// model (a) placed souls vanished, killing their clusters for the rest of the
// fill (forward-fill storms), and (b) phantom prize pins let GT-entry certify
// open through crystals whose Prize locations were soul-blocked, so every
// attempt failed validation. Conversely, applying this model to pot/door key
// seeds degraded THEIR calibrated fills (deep-stacked keys, oracle
// double-counts) — hence the tier scoping. The own-id exclusion keeps a key
// from sitting in a slot justified by its own placed siblings (the final walk
// could not order the copies).
static const RandoReachability *compute_reachability_collecting_placed(
    const RandoCounts *base_counts, const RandoSettings *settings,
    const uint16 *placement_at, const uint16 *open_loc_idx, uint16 open_n,
    uint16 placing_item) {
  static RandoCounts local;  // large; keep off the stack (single-threaded CLI)
  local = *base_counts;
  static uint8 collected[kRandoLocationCapacity];
  memset(collected, 0, open_n * sizeof(collected[0]));
  const RandoReachability *r = Logic_ComputeReachabilityFullKnowledge(&local, settings);
  for (;;) {
    bool added = false;
    for (uint16 k = 0; k < open_n; k++) {
      if (collected[k] || placement_at[k] == 0xFFFF)
        continue;
      uint16 item = placement_at[k];
      if (item >= 256 || item == placing_item || item == ITEM_Nothing ||
          vanilla_mode_pregranted_item(item, settings)) {
        if (item != placing_item)
          collected[k] = 1;  // never collected through this path
        continue;
      }
      const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
      if (!location_grants_placed_item(loc)) continue;
      if (r == NULL || !Reachability_HasLocation(r, loc->id)) continue;
      if (local.by_item_id[item] < 0xFFFF)
        local.by_item_id[item]++;
      // Door-oracle key single-count: a key collected from a bridge row the
      // oracle's key budget credits internally (active key-source pot /
      // enemy-drop / enemy-check row) must not ALSO raise the held-key input,
      // or the key funds a threshold gate twice. Record it per dungeon;
      // door_oracle_get subtracts. No-op when no door layout is installed.
      {
        uint8 src_d = Rando_DoorKeySourceDungeon(loc->id, item);
        if (src_d != 0xFF && local.door_source_keys_collected[src_d] != 0xFF)
          local.door_source_keys_collected[src_d]++;
      }
      collected[k] = 1;
      added = true;
    }
    if (!added) break;
    r = Logic_ExpandReachability(&local, settings);
  }
  return r;
}

static bool place_assumed_fill_attempt(const RandoSettings *settings,
                                       const uint16 *pool,
                                       uint16 pool_n,
                                       uint16 selected_key_rings_mask,
                                       uint64 seed_u64,
                                       RandoPlacementTable *out,
                                       uint16 *out_fallback_count) {
  if (settings == NULL || pool == NULL || pool_n == 0 || out == NULL ||
      out->entries == NULL)
    return false;
  if (out_fallback_count) *out_fallback_count = 0;

  // ----- 1. Install attempt-specific shuffle-assignment tables -----
  // Run prize + medallion shuffles. Their outputs are installed into owned
  // reachability state by Rando_SetDungeonPrizeAssignment /
  // Rando_SetMedallionAssignment. The placer needs these BEFORE running
  // reachability, otherwise OP_HAS_PRIZE / OP_MEDALLION_OPENS evaluate to
  // false everywhere and most of the graph is unreachable. Keep local copies
  // for the prize-pin pass below.
  //
  // Note: this state is process-global, which is fine here because the CLI
  // generates one seed at a time. Multi-seed batch generation (task 1.6a
  // batch form) would need to re-install per iteration.
  uint8 prize_assignment[kRandoDungeonCount];
  uint8 medallion_assignment[kRandoMedallionEntranceCount];
  {
    // Use a dedicated RNG seeded from seed_u64 so the shuffles are
    // deterministic per (seed, settings) — independent of the placer's RNG
    // consumption order.
    RandoRng shuffle_rng;
    Rng_SeedFromU64(&shuffle_rng, seed_u64);
    PrizeShuffle_Run(settings, &shuffle_rng, prize_assignment);
    MedallionShuffle_Run(settings, &shuffle_rng, medallion_assignment);
  }
  Rando_SetDungeonPrizeAssignment(prize_assignment);
  Rando_SetMedallionAssignment(medallion_assignment);

  // Phase B Slice 3b — pick the per-seed active TakeAny caves (Retro only).
  // Computed once here (like prize/medallion) so the collection + pin loops
  // below agree on which TakeAny LOCs are active. Salted RNG → main fill stream
  // untouched; non-Retro leaves all caves inactive (roles all 0).
  uint8 takeany_roles[kTakeAnyCaveCount];
  takeany_select(settings, seed_u64, takeany_roles);

  // ----- 2. Partition into progression / junk -----
  // Per ALTTPR Filler/RandomAssumed.php — split progression so DUNGEON items
  // (small keys, big keys, maps, compasses) are placed FIRST, then other
  // progression. Dungeon items have the most-restrictive can_place / dungeon-
  // mode constraints; placing them first means the assumed inventory still
  // contains all other progression (weapons, lamp, etc.), so they land at the
  // most-reachable in-dungeon slot. Placing them late lets other progression
  // fill in-dungeon slots first, often leaving keys with only circular slots
  // remaining and triggering forward-fill fallback.
  static uint16 progression[256];
  static uint16 junk[kRandoLocationCapacity];
  uint16 prog_n = 0, junk_n = 0;
  uint16 dungeon_prog_n = 0;  // first dungeon_prog_n entries of progression[] are dungeon items
  for (uint16 i = 0; i < pool_n; i++) {
    uint16 it = pool[i];
    if (is_progression_item(it)) {
      if (prog_n < sizeof(progression) / sizeof(progression[0])) {
        // Dungeon items: small key (53..65), big key (66..76).
        // (Maps 77..87 + 124 and compasses 88..98 are NOT in progression per
        // is_progression_item, so they fall through to junk and are handled
        // by the constraint-aware junk-fill below.)
        bool is_dungeon_item = (it >= 53 && it <= 76) || Rando_IsKeyRingItem(it);
        if (is_dungeon_item) {
          // Insert at the front of the dungeon-prog prefix.
          for (uint16 j = prog_n; j > dungeon_prog_n; j--) progression[j] = progression[j - 1];
          progression[dungeon_prog_n++] = it;
          prog_n++;
        } else {
          progression[prog_n++] = it;
        }
      }
    } else {
      if (junk_n < sizeof(junk) / sizeof(junk[0])) junk[junk_n++] = it;
    }
  }

  // ----- 3. Collect open locations (world-state-filtered, sorted by id) -----
  // open_loc_idx[k] = index into kRandoLocations of the k-th open location.
  static uint16 open_loc_idx[kRandoLocationCapacity];
  uint16 open_n = 0;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    const RandoLocationDef *loc = &kRandoLocations[i];
    if (!Rando_LocationWorldStateActive(loc, settings)) continue;
    // Phase B Slice 3b — only the per-seed ACTIVE TakeAny LOCs emit (Option B).
    // Inactive caves (and the weapon cave's unused slot 1) produce no placement
    // entry, per the spec "the placement table contains only the active slots".
    if (loc->type == LOCTYPE_TakeAny &&
        takeany_reward(settings, takeany_roles, loc->id) == 0xFFFF) continue;
    // Only pots ACTIVE under the selected tier enter the open-location set
    // (pot_active — the shared predicate; cave entrance shuffle forces
    // all pots inactive). Inactive pots draw no fill RNG, so pot-shuffle off is
    // placement-byte-identical. Active loot/key pots become open
    // slots; an active empty pot enters here too but is pinned to ITEM_Nothing
    // by the pre-pass (location_is_prepinned) before assumed-fill/junk run.
    if (loc->type == LOCTYPE_Pot && !pot_active(loc, settings)) continue;
    if (loc->type == LOCTYPE_EnemyDrop && !enemy_drop_active(loc, settings)) continue;
    if (loc->type == LOCTYPE_Enemy && !enemy_check_active(loc, settings)) continue;
    if ((loc->type == LOCTYPE_Grass || loc->type == LOCTYPE_Rock ||
         loc->type == LOCTYPE_Bonk) &&
        !terrain_active(loc, settings)) continue;
    open_loc_idx[open_n++] = (uint16)i;
  }
  if (g_place_profile_active) {
    g_place_profile.open_locations_sum += open_n;
    g_place_profile.progression_items_sum += prog_n;
    if (open_n > g_place_profile.max_open_locations)
      g_place_profile.max_open_locations = open_n;
    if (prog_n > g_place_profile.max_progression_items)
      g_place_profile.max_progression_items = prog_n;
  }

  // placement_at[k] = item placed at open_loc_idx[k], or 0xFFFF if empty.
  static uint16 placement_at[kRandoLocationCapacity];
  for (uint16 k = 0; k < open_n; k++) placement_at[k] = 0xFFFF;

  // ----- 3b. Pre-place vanilla-mode dungeon items + event-prize pins -----
  //
  // Several location types are NOT subject to assumed-fill randomization:
  //
  //   1. Prize_Event (Zelda, Agahnim, Agahnim 2, Ganon, Bomb Merchant
  //      in Inverted): these are virtual event triggers. Their vanilla
  //      item (RescuedZelda / DefeatAgahnim / etc.) MUST be pinned so
  //      sphere computation sees the event item enter inventory when the
  //      location is reached — otherwise downstream regions that gate
  //      on the event are forever unreachable.
  //
  //   2. Medallion config locations (Misery Mire Medallion / Turtle Rock
  //      Medallion): these are read by the medallion-shuffle module, not
  //      placed. For now pin to vanilla (matches medallion_shuffle=0 case).
  //
  //   3. In Vanilla dungeon-item mode (the default), small keys / big keys /
  //      maps / compasses belong at their vanilla locations. Pin them here
  //      before assumed fill runs, otherwise junk fills those slots and
  //      dungeon locks become unopenable.
  //
  // Per-class mode lookup: settings.dungeon_{small_keys,big_keys,maps,compasses}_mode.
  // Item id ranges (per item_registry.yaml):
  //   53..65 = small keys, 66..76 = big keys, 77..87 = maps (plus id 124 = Map_HCE),
  //   88..98 = compasses.
  //
  // The per-class pin conditions live in location_is_prepinned (file scope),
  // SHARED with BuildItemPool's junk-pad target: a pre-pinned slot never
  // consumes a pool item, so the pool is sized to the non-pinned slot count
  // and the pin set + pool size cannot drift apart. This pass
  // additionally handles the two pin classes whose pinned ITEM is not the
  // vanilla identity: TakeAny (per-seed role reward) and Prize_Pendant /
  // Prize_Crystal (prize-shuffle assignment).
  //
  // Rationale for the always-pinned types (details in location_is_prepinned):
  //   - ShopUpgrade: Capacity Upgrade slots (Bomb +5 / Arrow +5) are
  //     identity-placed per design.md §1a + proposal.md:41 — the slot exists
  //     in the registry so the shop dispatcher can route the grant through the
  //     uniform Rando_DispatchVanillaGrant call shape, but the player still
  //     buys the capacity upgrade for rupees as in vanilla.
  //   - Shop: Phase B Slice 3a #53 part 2 — per ALTTPR `Randomizer.php:737-750`,
  //     Retro shops retain their vanilla inventory (the randomization is that
  //     the player must find shops + pay rupees to survive, NOT that shop
  //     inventory is shuffled).
  //   - Prize_Event / Medallion: virtual event triggers + medallion config
  //     slots, always vanilla.
  for (uint16 k = 0; k < open_n; k++) {
    const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
    uint16 vi = loc->vanilla_item_id;
    if (loc->type == LOCTYPE_TakeAny) {
      // Phase B Slice 3b — active TakeAny caves are pinned to their per-seed
      // role reward (potion cave: BluePotion@0 / BossHeart@1; weapon cave:
      // ProgressiveSword|Rupee300 @0). Only active LOCs reach this point —
      // inactive ones were filtered out of open_loc_idx above. The reward is
      // NOT from the item pool (fixed inventory, matching ALTTPR addInventory),
      // so like the regular shops it consumes no pool slot.
      uint16 reward = takeany_reward(settings, takeany_roles, loc->id);
      placement_at[k] = reward;  // guaranteed != 0xFFFF for active LOCs
      continue;
    } else if (loc->type == LOCTYPE_Prize_Pendant || loc->type == LOCTYPE_Prize_Crystal) {
      // Pin per the prize-shuffle assignment. Find the dungeon whose Prize
      // location matches this slot, then look up the assigned prize id and
      // map it to the item-registry id (+111 offset).
      for (uint8 d = 0; d < kRandoDungeonCount; d++) {
        if (kDungeonPrizeLocations[d] == 0xFFFF) continue;
        if (kDungeonPrizeLocations[d] != loc->id) continue;
        uint8 prize_id = prize_assignment[d];
        if (prize_id < kRandoPrizeCount) {
          // prize_id 0..9 (Green/Red/Blue pendants + Crystal1..7) → item id 111..120
          placement_at[k] = 111 + prize_id;
          break;
        }
      }
      // If prize_shuffle is disabled, prize_assignment[d] is the vanilla
      // identity; the lookup above still resolves correctly. Locations not
      // in kDungeonPrizeLocations (shouldn't happen for Prize_* types) fall
      // through to the assumed-fill placer.
      continue;
    }
    // Every remaining pin class places the slot's vanilla identity:
    // Prize_Event / Medallion / Shop / ShopUpgrade, vanilla-mode dungeon
    // items
    // (per-class conditions + rationale in location_is_prepinned).
    if (location_is_prepinned(loc, settings)) {
      placement_at[k] = vi;
    }
  }

  // ----- 3c. Customizer pins (add-rando-customizer-mode) -----
  //
  // When a customizer manifest is installed, PIN each manifest location to its
  // chosen item and remove that item from the to-place pool, so assumed fill
  // (below) completes the remaining locations exactly as in a normal seed. The
  // whole block is gated on customizer_active: with no manifest it is never
  // entered, so this function stays byte-for-byte identical to the
  // non-customizer placer (the regression corpus is the zero-regression proof).
  //
  // A pin error (unknown/closed location, non-customizable type, or a slot the
  // current dungeon-item settings already vanilla-place) is a HARD,
  // deterministic failure: set the customizer error channel and return false.
  // Place_AssumedFill detects the error and stops retrying.
  if (settings->customizer_active) {
    const CustomizerManifest *cm = Customizer_GetActive();
    if (cm == NULL) {
      // customizer_active set but no manifest installed → internal bug.
      Customizer__SetError("customizer_active set but no manifest installed");
      return false;
    }
    if (!customizer_validate_key_cardinality(cm, progression, prog_n,
                                             junk, junk_n))
      return false;
    // reject a manifest whose pool overrides + pins exceed a
    // per-item grant cap (progressive ladders / bottle slots) before any of
    // them mutate the pool. Hard deterministic error; sets the error channel.
    if (!customizer_validate_item_caps(cm, progression, prog_n, junk, junk_n))
      return false;
    // pool_overrides: apply remove-then-add to the pool BEFORE pins (so a pin
    // can consume an added item). remove is best-effort (an item not in the
    // settings-dependent pool is a silent no-op); add inserts into the correct
    // tier. Determinism holds — both walk the manifest in order, before the
    // tier shuffles.
    for (uint16 r = 0; r < cm->pool_remove_count; r++) {
      customizer_pool_remove_one(progression, &prog_n, &dungeon_prog_n,
                                 junk, &junk_n, cm->pool_remove[r]);
    }
    for (uint16 a = 0; a < cm->pool_add_count; a++) {
      customizer_pool_add_one(progression, &prog_n, &dungeon_prog_n,
                              (uint16)(sizeof progression / sizeof progression[0]),
                              junk, &junk_n,
                              (uint16)(sizeof junk / sizeof junk[0]),
                              cm->pool_add[a]);
    }
    // pin_count == 0 is tolerated as a harmless no-op (the loop runs zero
    // times). The CLI rejects an empty manifest before installing, so this only
    // guards a future caller that installs a zero-pin manifest.
    for (uint16 pi = 0; pi < cm->pin_count; pi++) {
      uint16 loc_id = cm->pins[pi].location_id;
      uint16 item_id = cm->pins[pi].item_id;
      uint16 slot = 0xFFFF;
      for (uint16 k = 0; k < open_n; k++) {
        if (kRandoLocations[open_loc_idx[k]].id == loc_id) { slot = k; break; }
      }
      if (slot == 0xFFFF) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "pinned location '%s' is not an open item location for this world state",
                 Rando_GetLocationName(loc_id));
        Customizer__SetError(msg);
        return false;
      }
      const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[slot]];
      // Reject non-customizable location TYPES (computed-item slots: prize /
      // medallion / shop / capacity-upgrade / take-any).
      // add-rando-shopsanity: a regular Shop slot is an ordinary OPEN fill
      // location when the axis is on, so it becomes pinnable like a chest;
      // with the axis off it stays an identity-pinned economy slot and the
      // rejection below keeps its clear message.
      if (loc->type == LOCTYPE_Prize_Crystal || loc->type == LOCTYPE_Prize_Pendant ||
          loc->type == LOCTYPE_Prize_Event   || loc->type == LOCTYPE_Medallion ||
          (loc->type == LOCTYPE_Shop && !Settings_ShopsanityActive(settings)) ||
          loc->type == LOCTYPE_ShopUpgrade   || loc->type == LOCTYPE_TakeAny) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "pinned location '%s' has a non-customizable type "
                 "(prize/medallion/take-any, or a shop slot without shopsanity)",
                 Rando_GetLocationName(loc_id));
        Customizer__SetError(msg);
        return false;
      }
      // Empty pots are non-customizable by VALUE (not type): a NON-empty pot IS
      // customizable (like a chest), but an empty pot
      // carries the ITEM_Nothing filler and pinning a real item there defeats the
      // design (the pre-pass already pinned it to Nothing, so the generic
      // already-placed guard below would also catch it — this is the clear
      // message). ITEM_Nothing is likewise never a pinnable item; it exists only
      // as the empty-pot filler.
      if (loc->type == LOCTYPE_Pot && loc->vanilla_item_id == ITEM_Nothing) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "pinned location '%s' is an empty pot (non-customizable)",
                 Rando_GetLocationName(loc_id));
        Customizer__SetError(msg);
        return false;
      }
      if (item_id == ITEM_Nothing) {
        Customizer__SetError("ITEM_Nothing cannot be pinned (empty-pot filler only)");
        return false;
      }
      // Reject a slot the settings already vanilla-place (vanilla-mode dungeon
      // item). Overriding it would silently drop an item that is NOT in the
      // pool, which can make the seed unbeatable in a way the user did not
      // intend — surface it instead.
      if (placement_at[slot] != 0xFFFF) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "pinned location '%s' is vanilla-placed under the current "
                 "dungeon-item settings",
                 Rando_GetLocationName(loc_id));
        Customizer__SetError(msg);
        return false;
      }
      // Reject non-grantable ITEMS: prizes (placed by the prize shuffle, not
      // the pool) and virtual logic/event items. These have no pool entry and
      // no item grant path — pinning one would emit a placement the runtime
      // dispatcher can't honor.
      if (Customizer_IsNonGrantableItem(item_id)) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "pinned item '%s' is not grantable and cannot be hand-placed",
                 Rando_GetItemName(item_id));
        Customizer__SetError(msg);
        return false;
      }
      // Enforce the location's placement legality (dungeon-item confinement +
      // any can_place / always_allow predicate, e.g. a forced-key slot). The
      // can_place predicate tests the candidate ITEM, not player inventory, so a
      // zeroed RandoCounts is the correct "no inventory assumptions" context.
      // Without this a key could be pinned outside its dungeon, or a non-key
      // onto a forced-key slot — structurally invalid even when coincidentally
      // beatable.
      {
        RandoCounts cz;
        memset(&cz, 0, sizeof cz);
        if (!location_accepts_item(loc, item_id, &cz, settings,
                                   selected_key_rings_mask)) {
          char msg[160];
          snprintf(msg, sizeof msg,
                   "pinned item '%s' is not allowed at location '%s' "
                   "(dungeon-item confinement or a forced-item slot)",
                   Rando_GetItemName(item_id), Rando_GetLocationName(loc_id));
          Customizer__SetError(msg);
          return false;
        }
      }
      placement_at[slot] = item_id;
      // Remove the pinned item from the pool so assumed fill does not place a
      // second copy. An out-of-pool pin (nothing to remove) is allowed: the
      // pool then has one item too many for the remaining slots and a single
      // junk item is dropped at fill time, which is harmless (progression is
      // always placed first and always fits).
      customizer_pool_remove_one(progression, &prog_n, &dungeon_prog_n,
                                 junk, &junk_n, item_id);
    }
  }

  // ----- 4. Seed the assumed inventory with all progression items -----
  RandoRng rng;
  Rng_SeedFromU64(&rng, seed_u64);

  RandoCounts counts;
  memset(&counts, 0, sizeof(counts));
  counts.by_item_id[121] = 3;  // StartingHeart virtual item (id 121, count 3)
  // Per-world-state pre-collected virtual items — mirrors ALTTPR's
  // `World::pre_collected_items` mechanism. In Open / Inverted / Retro the
  // player begins with RescuedZelda already granted (the sanctuary escort
  // is skipped); in Standard the player earns it via HCE. (id 122 per
  // item_registry.yaml.)
  if (settings->world_state != kWorldState_Standard) {
    counts.by_item_id[122] = 1;
  }
  // Vanilla-mode dungeon items: when a dungeon-item class is in Vanilla mode,
  // the items aren't placed by the randomizer — the vanilla ROM grants them
  // when the player collects them in-place. Pre-grant them in the assumed
  // inventory so reachability treats them as always-available. (Otherwise
  // the placer treats SmallKey-gated dungeon locations as permanently
  // unreachable, breaking vanilla-mode seeds.) Shared with the runtime
  // reachability bridge so both agree.
  Rando_SeedVanillaDungeonItems(&counts, settings);
  seed_pot_nonpot_drops(&counts, settings);
  for (uint16 i = 0; i < prog_n; i++) {
    counts.by_item_id[progression[i]]++;
  }
  // Fill-model selection (see compute_reachability_collecting_placed): the
  // EFFECTIVE souls_shuffle=all tier collects committed items/pins through a
  // per-turn reachability fix-point; every other seed keeps the pre-souls
  // conservative model — placed items vanish from the assumed set and pins
  // are seeded unconditionally below. The conservative pin optimism is what
  // the enemies tier CANNOT use: with the kill-room soul wraps, GT-entry
  // certified open via assumed crystals whose Prize locations were themselves
  // soul-blocked, and every attempt failed the final sphere-walk validation.
  const bool souls_collect_model =
      Settings_EffectiveSoulsShuffle(settings) >= kSoulsShuffle_BossesEnemies ||
      settings->npc_souls != 0;  // add-npc-souls: 23 cross-gating scattered
                                 // items need the same phantom-pin-safe model
  if (!souls_collect_model) {
    // Pre-placed pin grants (prizes, events, TakeAny rewards, customizer
    // pins) join the assumed inventory so reachability treats them as
    // always-available (the sphere walk still validates every attempt).
    for (uint16 k = 0; k < open_n; k++) {
      if (placement_at[k] == 0xFFFF) continue;
      if (!location_grants_placed_item(&kRandoLocations[open_loc_idx[k]])) continue;
      uint16 vi = placement_at[k];
      if (vi < 256) counts.by_item_id[vi]++;
    }
  }

  // Shuffle within each tier independently so dungeon items stay first.
  shuffle_u16(progression, dungeon_prog_n, &rng);
  if (prog_n > dungeon_prog_n)
    shuffle_u16(progression + dungeon_prog_n, (uint16)(prog_n - dungeon_prog_n), &rng);

  // ----- 5. Place each progression item -----
  //
  // Per-item bounded rewind (Bug #7 / design D1): on a no-candidate item, rewind
  // the last N placements + reshuffle before forward-filling. `prog_slot[i]` is
  // the open-slot index where progression[i] landed (0xFFFF if unplaced/forward-
  // filled), so a rewind can reopen exactly the slots it reclaims. The happy
  // path (cand_n > 0) is byte-for-byte identical to the pre-rewind code — one
  // Rng_NextRange draw — so seeds that never hit the no-candidate branch keep
  // byte-identical digests regardless of kPerItemRewindBudget.
  uint16 fallback_count = 0;
  static uint16 prog_slot[256];
  for (uint16 t = 0; t < prog_n; t++) prog_slot[t] = 0xFFFF;
  uint16 rewind_budget = kPerItemRewindBudget;  // per-attempt event budget
  static uint16 candidates[kRandoLocationCapacity];
  uint16 i = 0;
  while (i < prog_n) {
    uint16 item = progression[i];
    // Remove from assumed inventory before computing reachability.
    if (counts.by_item_id[item] > 0) counts.by_item_id[item]--;

    // Enemies-tier souls: assumed(unplaced) + fix-point-collected committed
    // items (own id excluded). Everything else: the pre-souls assumed-only
    // model the key-threshold calibrations expect.
    const RandoReachability *r = souls_collect_model
        ? compute_reachability_collecting_placed(
              &counts, settings, placement_at, open_loc_idx, open_n, item)
        : Logic_ComputeReachabilityFullKnowledge(&counts, settings);
    if (g_place_profile_active) g_place_profile.candidate_scan_slots += open_n;

    // Find candidate locations: open + reachable + accepts item.
    uint16 cand_n = 0;
    for (uint16 k = 0; k < open_n; k++) {
      if (placement_at[k] != 0xFFFF) continue;
      const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
      // add-rando-grass-rock-shuffle: junk-tier terrain slots never take a
      // pool (progression) item — junk-pad only.
      if (terrain_junk_only(loc, settings)) continue;
      if (r != NULL && !Reachability_HasLocation(r, loc->id)) continue;
      if (!location_accepts_item(loc, item, &counts, settings,
                                 selected_key_rings_mask)) continue;
      candidates[cand_n++] = k;
    }

    if (cand_n > 0) {
      uint32 pick = Rng_NextRange(&rng, cand_n);
      uint16 slot = candidates[pick];
      placement_at[slot] = item;
      prog_slot[i] = slot;
      i++;
      continue;
    }

    // No reachable+placeable candidate. Try per-item bounded rewind before the
    // forward-fill fallback. Clamp the rewind window so it never crosses the
    // dungeon/non-dungeon tier boundary — dungeon items are placed first
    // because they have the most restrictive can_place; mixing tiers would
    // break that ordering invariant.
    if (rewind_budget > 0) {
      uint16 floor = (i >= dungeon_prog_n) ? dungeon_prog_n : 0;
      uint16 n = kPerItemRewindWindow;
      if (n > (uint16)(i - floor)) n = (uint16)(i - floor);
      if (n > 0) {
        // Return the current (unplaceable) item to the assumed inventory; it is
        // re-placed when the loop walks the reshuffled window forward again.
        counts.by_item_id[item]++;
        for (uint16 rr = 0; rr < n; rr++) {
          uint16 j = (uint16)(i - 1 - rr);
          uint16 sl = prog_slot[j];
          if (sl != 0xFFFF) placement_at[sl] = 0xFFFF;  // reopen slot
          prog_slot[j] = 0xFFFF;
          counts.by_item_id[progression[j]]++;          // back to unplaced
        }
        // Reshuffle the window [i-n, i] (n+1 items, incl. the stuck one) so the
        // deterministic retry explores a different order instead of reproducing
        // the identical failure.
        shuffle_u16(progression + (i - n), (uint16)(n + 1), &rng);
        rewind_budget--;
        g_last_placement_stats.per_item_rewind_count++;
        i = (uint16)(i - n);
        continue;
      }
      // n == 0 (nothing left to rewind in this tier): fall through to forward-fill.
    }

    // Forward-fill fallback: place at any open + can_place location (ignore
    // reachability). If still nothing, take the first open slot regardless of
    // can_place — last-resort recovery. Junk-only terrain slots stay excluded
    // in BOTH fallbacks: violating the junk-tier contract would be a silent
    // spec break, so an un-placeable pool item fails generation honestly
    // instead (Placement_SelfCheck also asserts the invariant).
    cand_n = 0;
    for (uint16 k = 0; k < open_n; k++) {
      if (placement_at[k] != 0xFFFF) continue;
      const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
      if (terrain_junk_only(loc, settings)) continue;
      if (!location_accepts_item(loc, item, &counts, settings,
                                 selected_key_rings_mask)) continue;
      candidates[cand_n++] = k;
    }
    if (cand_n == 0) {
      for (uint16 k = 0; k < open_n; k++) {
        if (placement_at[k] != 0xFFFF) continue;
        if (terrain_junk_only(&kRandoLocations[open_loc_idx[k]], settings)) continue;
        candidates[cand_n++] = k;
      }
    }
    if (cand_n == 0) {
      fprintf(stderr, "Place_AssumedFill: no open location for progression item %u\n",
              (unsigned)item);
      return false;
    }
    uint32 pick = Rng_NextRange(&rng, cand_n);
    uint16 slot = candidates[pick];
    placement_at[slot] = item;
    prog_slot[i] = slot;
    fallback_count++;
    i++;
  }

  // ----- 5b. Place the optional logic-neutral Skeleton Key -----
  //
  // Skeleton Key deliberately remains outside is_progression_item: it must not
  // influence assumed reachability or accessibility certification. It still
  // has fixed cardinality, though. Customizer out-of-pool pins can leave one
  // surplus junk-tier item, so letting the ordinary junk shuffle choose the
  // discarded copy could omit Skeleton Key despite the enabled setting.
  // Reserve it an unrestricted open slot first, without a reachability gate.
  // A customizer pin that relocates Skeleton Key already removed it from junk
  // and therefore needs no work here.
  uint16 skeleton_junk_index = 0xFFFF;
  for (uint16 j = 0; j < junk_n; j++) {
    if (junk[j] != ITEM_SkeletonKey) continue;
    if (skeleton_junk_index != 0xFFFF) {
      fprintf(stderr, "Place_AssumedFill: duplicate Skeleton Key in junk pool\n");
      return false;
    }
    skeleton_junk_index = j;
  }
  if (skeleton_junk_index != 0xFFFF) {
    uint16 cand_n = 0;
    for (uint16 k = 0; k < open_n; k++) {
      if (placement_at[k] != 0xFFFF) continue;
      const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
      if (!location_accepts_item(loc, ITEM_SkeletonKey, &counts, settings,
                                 selected_key_rings_mask)) continue;
      candidates[cand_n++] = k;
    }
    if (cand_n == 0) {
      fprintf(stderr,
              "Place_AssumedFill: no open location for enabled Skeleton Key\n");
      return false;
    }
    uint16 slot = candidates[Rng_NextRange(&rng, cand_n)];
    placement_at[slot] = ITEM_SkeletonKey;
    for (uint16 j = skeleton_junk_index; j + 1 < junk_n; j++)
      junk[j] = junk[j + 1];
    junk_n--;
  }

  // ----- 6. Fill remaining open locations with junk -----
  //
  // Junk fill must still honor `can_place` / `always_allow`. Restrictive
  // can_place predicates (e.g., SP Entrance's `OP_ITEM_IS(SmallKey_SwampPalace)`
  // forcing a specific item) get silently violated if junk lands at the slot
  // because the progression item was placed elsewhere first.
  shuffle_u16(junk, junk_n, &rng);
  uint8 junk_consumed[kRandoLocationCapacity] = {0};
  uint8 junk_filled[kRandoLocationCapacity] = {0};
  for (uint16 k = 0; k < open_n; k++) {
    if (placement_at[k] != 0xFFFF) continue;
    const RandoLocationDef *loc = &kRandoLocations[open_loc_idx[k]];
    bool placed = false;
    for (uint16 j = 0; j < junk_n; j++) {
      if (junk_consumed[j]) continue;
      if (!location_accepts_item(loc, junk[j], &counts, settings,
                                 selected_key_rings_mask)) continue;
      placement_at[k] = junk[j];
      junk_consumed[j] = 1;
      junk_filled[k] = 1;
      placed = true;
      break;
    }
    if (!placed) {
      // No junk item fits this slot's can_place — fall back to the slot's
      // vanilla item, which always satisfies (since vanilla is what shipped
      // there). This also covers the pool-cardinality-mismatch case.
      uint16 fb = loc->vanilla_item_id;
      // Under Retro genericKeys, a per-dungeon SmallKey fallback must become the
      // fungible GenericKey (mirrors ALTTPR Location::getItem swapping any
      // Item\Key to KeyGK). The only slot that hits this under Retro is the
      // Swamp Palace Entrance forced-key (can_place = OP_ITEM_IS(SmallKey_SP),
      // which no pool item satisfies once keys are generic). Without the swap it
      // would hold a dead per-dungeon key that the genericKeys runtime never
      // grants (the shared-counter grant only fires for GenericKey).
      if (Settings_GenericKeysActive(settings) &&
          fb >= ID_SmallKey_HCE && fb <= ID_SmallKey_GT)
        fb = ID_GenericKey;
      placement_at[k] = fb;
    }
  }
  (void)inject_traps_into_junk_placements(placement_at, junk_filled, open_loc_idx,
                                          open_n, settings, seed_u64, &rng);

  // ----- 7. Emit placement table -----
  for (uint16 k = 0; k < open_n; k++) {
    out->entries[k].location_id = kRandoLocations[open_loc_idx[k]].id;
    out->entries[k].item_id = placement_at[k];
  }
  out->count = open_n;

  if (out_fallback_count) *out_fallback_count = fallback_count;
  return true;
}

// ---------------------------------------------------------------------------
// PlacementTable_ComputeDigest — SHA-256 over a canonical-LE serialization.
//
// Canonical serialization: for each (location_id, item_id) pair in
// location_id-sorted order, emit 4 bytes: <location_id:u16_le> <item_id:u16_le>.
// The result is fed to SHA-256.
//
// Determinism contract: the input order is sorted by location_id, so the
// digest is invariant under any internal reordering during placement. This is
// the source of truth for the regression corpus (tasks.md §12.1).
// ---------------------------------------------------------------------------
static int placement_cmp(const void *a, const void *b) {
  const RandoPlacement *pa = a;
  const RandoPlacement *pb = b;
  if (pa->location_id < pb->location_id) return -1;
  if (pa->location_id > pb->location_id) return 1;
  return 0;
}

void PlacementTable_ComputeDigest(const RandoPlacementTable *t, uint8 out_digest[32]) {
  if (t == NULL || t->entries == NULL || t->count == 0) {
    // Hash of empty input is well-defined (SHA-256 of zero bytes).
    sha256_buffer((const uint8 *)"", 0, out_digest);
    return;
  }
  // Copy entries into a local buffer and sort by location_id.
  //
  // Sized by the module-wide location ceiling (rando_logic.h) so the digest
  // covers EVERY placed entry. Silent truncation here is the corpus's blind
  // spot: a too-small cap drops high-location_id entries (e.g. the 9 Retro
  // Light-World shop / capacity-upgrade slots once dropped at the old 256 cap)
  // out of placement_digest_hex when sorted by location_id, degrading corpus
  // coverage with no visible failure. The assert ties the cap to the registry
  // (LOC__COUNT) directly so a registry that outgrows it is a build break.
  enum { kDigestLocalCap = kRandoLocationCapacity };
  _Static_assert(kDigestLocalCap >= LOC__COUNT,
                 "kDigestLocalCap must cover the whole location registry — "
                 "a smaller cap silently truncates placement_digest");
  RandoPlacement local[kDigestLocalCap];
  uint16 n = t->count;
  if (n > kDigestLocalCap) n = kDigestLocalCap;
  memcpy(local, t->entries, n * sizeof(RandoPlacement));
  qsort(local, n, sizeof(RandoPlacement), placement_cmp);

  // Serialize: 4 bytes per entry, little-endian.
  uint8 buf[kDigestLocalCap * 4];
  for (uint16 i = 0; i < n; i++) {
    buf[i * 4 + 0] = local[i].location_id & 0xff;
    buf[i * 4 + 1] = local[i].location_id >> 8;
    buf[i * 4 + 2] = local[i].item_id & 0xff;
    buf[i * 4 + 3] = local[i].item_id >> 8;
  }
  sha256_buffer(buf, n * 4, out_digest);
}

// ---------------------------------------------------------------------------
// Goal completability (tasks.md §3.9). Run after Place_AssumedFill to verify
// the placement is winnable for the active goal.
// ---------------------------------------------------------------------------

// Apply vanilla-mode dungeon-item pre-grants to `counts`. Mirror of the
// pre-grant block in place_assumed_fill_attempt; called by both
// Goal_IsCompletable and Logic_ComputeSpheres so reachability stays
// consistent with what the placer assumed.
//
// maps/compasses are pre-granted here in Vanilla mode but in
// Dungeon/Wild mode they go through the pool's junk[] path (not the
// progression[] inventory-assumption). Today no predicate consults map or
// compass IDs, so the asymmetry is harmless. If a future predicate gates
// on a map/compass, classify those IDs in is_progression_item so the
// assumed-fill inventory model stays consistent across modes.
static void apply_vanilla_dungeon_item_grants(const RandoSettings *s, RandoCounts *out) {
  if (s == NULL || out == NULL) return;
  if (Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Vanilla) {
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      out->by_item_id[Rando_SmallKeyItemForRandoDungeon(d)] =
          Rando_BaseSmallKeyCount(d);
    }
  }
  if (Settings_EffectiveBigKeysMode(s) == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kBigKeys) / sizeof(kBigKeys[0])); i++) {
      out->by_item_id[kBigKeys[i]] = 1;
    }
  }
  if (s->dungeon_maps_mode == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kMaps) / sizeof(kMaps[0])); i++) {
      out->by_item_id[kMaps[i]] = 1;
    }
  }
  if (s->dungeon_compasses_mode == kDungeonItemMode_Vanilla) {
    for (uint8 i = 0; i < (uint8)(sizeof(kCompasses) / sizeof(kCompasses[0])); i++) {
      out->by_item_id[kCompasses[i]] = 1;
    }
  }
  seed_dungeon_free_key_drops(out, s);
}

// Lookup a location_id in the placement table; returns the placed item or
// 0xFFFF if not found.
static uint16 placement_at_location(const RandoPlacementTable *t, uint16 loc_id) {
  if (t == NULL) return 0xFFFF;
  for (uint16 i = 0; i < t->count; i++) {
    if (t->entries[i].location_id == loc_id) return t->entries[i].item_id;
  }
  return 0xFFFF;
}

// Count reachable placements that hold a specific item id.
static uint16 count_reachable_placements_of(const RandoPlacementTable *t,
                                            const RandoReachability *r,
                                            uint16 item_id) {
  if (t == NULL || r == NULL) return 0;
  uint16 n = 0;
  for (uint16 i = 0; i < t->count; i++) {
    if (!placement_entry_grants_item(t, i)) continue;
    if (t->entries[i].item_id != item_id) continue;
    if (Reachability_HasLocation(r, t->entries[i].location_id)) n++;
  }
  return n;
}

// Special location IDs frequently checked by goal predicates. These match
// location_registry.yaml ordering (kept in sync — codegen will eventually
// emit named LOC_* macros for this, but for Phase A1 the IDs are stable).
#define LOC_ID_MasterSwordPedestal 151
#define LOC_ID_Ganon               212
#define LOC_ID_Agahnim2            137

// Pendant item ids (matches item_registry.yaml).
#define ITEM_ID_GreenPendant 111
#define ITEM_ID_RedPendant   112
#define ITEM_ID_BluePendant  113
#define ITEM_ID_TriforcePiece 52

bool Goal_IsCompletable(const RandoSettings *settings, uint64 seed_u64,
                        const RandoPlacementTable *placements) {
  if (settings == NULL || placements == NULL) return false;
  // add-rando-random-crystals — certify against the RESOLVED ganon count.
  // seed_u64 MUST be the BASE seed (never placement_attempt_seed): the
  // retry loop, the outer acceptance gate, the spoiler, and the runtime all
  // resolve from the base seed, same rule as the key-ring / boss-assignment
  // resolves (see the Place_AssumedFill base-seed comments).
  uint8 eff_crystals_ganon, eff_crystals_tower;
  Crystals_Resolve(settings, seed_u64, &eff_crystals_ganon, &eff_crystals_tower);
  // Install the resolved tower requirement so the sphere walks below (and in
  // Accessibility_SeedAcceptable, which calls this first with the same
  // (settings, seed)) evaluate OP_TOWER_CRYSTALS_MET against the seed's
  // actual gate, not the conservative fallback.
  Logic_SetResolvedTowerCrystals((uint8)(eff_crystals_tower + 1));

  // Pure reachability predicate — does NOT consider accessibility=none.
  // For "should the generator refuse to ship this seed?", call
  // Goal_ShouldRefuse instead. (The earlier short-circuit-to-true here made
  // the spoiler report
  // `goal_completable: true` even for un-completable accessibility=none
  // seeds, which actively misled players who explicitly opted in to
  // an un-completable seed.)
  // Beatability must be judged against the inventory the player can ACTUALLY
  // collect (sphere-walked), not the full placed pool. Summing every placed
  // item over-approximates reachability: a goal-gating crystal can count as
  // "reachable" through a circular dependency — its location is only reachable
  // given an item that is itself stranded behind that same crystal. The
  // accessibility=none ("beatable only") tier skips the per-placement sphere
  // check in Accessibility_SeedAcceptable, so this completability predicate is
  // the ONLY backstop against shipping an unbeatable seed there (see the
  // GT crystal-gate circularity documented at tests/rando_corpus/manifest.yaml).
  //
  // Walk spheres first; keep only items from sphere-reachable placements. The
  // starting basis (StartingHeart + RescuedZelda pre-grant + vanilla dungeon
  // grants) mirrors Logic_ComputeSpheres exactly, so reachability over this
  // fixpoint inventory equals the true achievable set.
  RandoSpheres reachable_spheres;
  Logic_ComputeSpheres(settings, placements, &reachable_spheres);
  RandoCounts final_inv;
  memset(&final_inv, 0, sizeof(final_inv));
  final_inv.by_item_id[121] = 3;  // StartingHeart
  Alttpr_AddStartingCounts(&final_inv);
  if (settings->world_state != kWorldState_Standard) {
    final_inv.by_item_id[122] = 1;  // RescuedZelda pre-granted in non-Standard
  }
  apply_vanilla_dungeon_item_grants(settings, &final_inv);
  for (uint16 i = 0; i < placements->count; i++) {
    if (reachable_spheres.sphere_index_by_placement[i] == kSphereIndexUnreachable) continue;
    if (!placement_entry_grants_item(placements, i)) continue;
    uint16 item_id = placements->entries[i].item_id;
    if (item_id < 256 && final_inv.by_item_id[item_id] < 0xFFFF) {
      final_inv.by_item_id[item_id]++;
    }
  }
  const RandoReachability *r = Logic_ComputeReachabilityFullKnowledge(&final_inv, settings);
  if (r == NULL) {
    fprintf(stderr, "Goal_IsCompletable: reachability returned NULL (empty graph?)\n");
    return false;
  }

  switch (settings->goal) {
    case kGoal_Ganon: {
      // Need: Ganon vulnerable (≥ crystals.ganon crystals collected from
      // REACHABLE locations) AND Agahnim 2 cleared AND Ganon reachable.
      uint16 reachable_crystals = 0;
      for (uint16 cid = 114; cid <= 120; cid++) {
        reachable_crystals += count_reachable_placements_of(placements, r, cid);
      }
      if (reachable_crystals < eff_crystals_ganon) {
        fprintf(stderr, "Goal_IsCompletable(ganon): %u reachable crystals, need %u\n",
                (unsigned)reachable_crystals, (unsigned)eff_crystals_ganon);
        return false;
      }
      if (!Reachability_HasLocation(r, LOC_ID_Agahnim2)) {
        fprintf(stderr, "Goal_IsCompletable(ganon): Agahnim 2 unreachable\n");
        return false;
      }
      if (!Reachability_HasLocation(r, LOC_ID_Ganon)) {
        fprintf(stderr, "Goal_IsCompletable(ganon): Ganon unreachable\n");
        return false;
      }
      return true;
    }
    case kGoal_FastGanon: {
      // Per spec scenario "Fast Ganon gates on crystals and tower access":
      // require crystals.ganon crystals for vulnerability AND Ganon reachable
      // (GT entry edge implicitly requires crystals.tower).
      uint16 reachable_crystals = 0;
      for (uint16 cid = 114; cid <= 120; cid++) {
        reachable_crystals += count_reachable_placements_of(placements, r, cid);
      }
      if (reachable_crystals < eff_crystals_ganon) {
        fprintf(stderr, "Goal_IsCompletable(fast_ganon): %u reachable crystals, need %u\n",
                (unsigned)reachable_crystals, (unsigned)eff_crystals_ganon);
        return false;
      }
      if (!Reachability_HasLocation(r, LOC_ID_Ganon)) {
        fprintf(stderr, "Goal_IsCompletable(fast_ganon): Ganon unreachable\n");
        return false;
      }
      return true;
    }
    case kGoal_Dungeons:
      // Every dungeon's _Boss location reachable. Dungeon-boss IDs match
      // location_registry: EP=16, DP=23, TH=30, PoD=48, SP=59, SW=68, TT=77,
      // IP=86, MM=95, TR=108, plus HCT=34 (Agahnim) and GT=137 (Agahnim 2).
      {
        const uint16 boss_locs[] = {16, 23, 30, 34, 48, 59, 68, 77, 86, 95, 108, 137};
        for (size_t i = 0; i < sizeof(boss_locs) / sizeof(boss_locs[0]); i++) {
          if (!Reachability_HasLocation(r, boss_locs[i])) {
            fprintf(stderr, "Goal_IsCompletable(dungeons): boss location %u unreachable\n",
                    (unsigned)boss_locs[i]);
            return false;
          }
        }
      }
      return true;
    case kGoal_Pedestal:
      if (!Reachability_HasLocation(r, LOC_ID_MasterSwordPedestal)) {
        fprintf(stderr, "Goal_IsCompletable(pedestal): Master Sword Pedestal unreachable\n");
        return false;
      }
      // don't trust the placement count alone — verify that
      // each colored pendant's holding _Prize location is reachable. The
      // pendant placement could be at an unreachable Prize_* slot if the
      // dungeon holding that pendant cannot be cleared.
      {
        const uint8 *prize_assign = Rando_GetDungeonPrizeAssignment();
        uint8 pendant_prize_ids[3] = { 0, 1, 2 };  // Green=0, Red=1, Blue=2
        const char *pendant_names[3] = { "GreenPendant", "RedPendant", "BluePendant" };
        for (int p = 0; p < 3; p++) {
          // Find the dungeon assigned this pendant.
          int found_dungeon = -1;
          if (prize_assign != NULL) {
            for (uint8 d = 0; d < kRandoDungeonCount; d++) {
              if (prize_assign[d] == pendant_prize_ids[p]) {
                found_dungeon = d;
                break;
              }
            }
          }
          if (found_dungeon < 0) {
            fprintf(stderr, "Goal_IsCompletable(pedestal): %s not assigned to any dungeon\n",
                    pendant_names[p]);
            return false;
          }
          uint16 prize_loc = kDungeonPrizeLocations[found_dungeon];
          if (prize_loc == 0xFFFF || !Reachability_HasLocation(r, prize_loc)) {
            fprintf(stderr, "Goal_IsCompletable(pedestal): %s's dungeon (id %d) Prize location unreachable\n",
                    pendant_names[p], found_dungeon);
            return false;
          }
        }
      }
      return true;
    case kGoal_TriforceHunt: {
      uint16 reachable_pieces = count_reachable_placements_of(placements, r, ITEM_ID_TriforcePiece);
      if (reachable_pieces < settings->pieces_required) {
        fprintf(stderr, "Goal_IsCompletable(triforce-hunt): %u reachable pieces, need %u\n",
                (unsigned)reachable_pieces, (unsigned)settings->pieces_required);
        return false;
      }
      // Murahdahla is in the castle courtyard, not behind the three-pendant
      // pedestal. Match ALTTPR World.php's North East Light World win route.
      uint16 courtyard = Rando_FindRegionByName("LightWorld_NorthEast");
      if (courtyard == 0xffff || !Reachability_HasRegion(r, courtyard)) {
        fprintf(stderr, "Goal_IsCompletable(triforce-hunt): courtyard unreachable\n");
        return false;
      }
      return true;
    }
    case kGoal_GanonHunt: {
      uint16 reachable_pieces = count_reachable_placements_of(placements, r, ITEM_ID_TriforcePiece);
      if (reachable_pieces < settings->pieces_required) {
        fprintf(stderr, "Goal_IsCompletable(ganonhunt): %u reachable pieces, need %u\n",
                (unsigned)reachable_pieces, (unsigned)settings->pieces_required);
        return false;
      }
      if (!Reachability_HasLocation(r, LOC_ID_Ganon)) {
        fprintf(stderr, "Goal_IsCompletable(ganonhunt): Ganon unreachable\n");
        return false;
      }
      return true;
    }
    case kGoal_Completionist:
      for (uint16 i = 0; i < placements->count; i++) {
        const RandoLocationDef *loc=Rando_FindLocationDef(placements->entries[i].location_id);
        if(loc && !Rando_LocationTypeCountsAsCheck(loc->type) &&
           loc->id!=LOC_Agahnim && loc->id!=LOC_Agahnim_2) continue;
        if (!Reachability_HasLocation(r, placements->entries[i].location_id)) {
          fprintf(stderr,
                  "Goal_IsCompletable(completionist): location %u unreachable\n",
                  (unsigned)placements->entries[i].location_id);
          return false;
        }
      }
      return true;
    default:
      fprintf(stderr, "Goal_IsCompletable: unknown goal %u\n", (unsigned)settings->goal);
      return false;
  }
}

// Pure tier-reachability rule (no logic graph). Given precomputed spheres for a
// placement whose goal is ALREADY known completable, decide whether the extra
// per-tier reachability bar is met. Split out from Accessibility_SeedAcceptable
// so Placement_SelfCheck can exercise the tier discrimination with synthetic
// spheres (the graph-dependent pieces — Goal_IsCompletable / Logic_Compute-
// Spheres — are tested by the corpus + their own callers).
static bool accessibility_reachability_ok(const RandoSettings *settings,
                                          const RandoPlacementTable *placements,
                                          const RandoSpheres *spheres) {
  switch (Settings_EffectiveAccessibility(settings)) {
    case kAccessibility_None:
      return true;  // "beatable only" — no extra reachability demand.
    case kAccessibility_Locations:
      return spheres->unreachable_count == 0;  // every location reachable.
    case kAccessibility_Items:
    default:
      // Every PROGRESSION item's location must be reachable; junk / maps /
      // compasses / hearts may strand (see is_progression_item).
      for (uint16 i = 0; i < placements->count; i++) {
        if (!is_progression_item(placements->entries[i].item_id)) continue;
        if (!placement_entry_grants_item(placements, i)) continue;
        if (spheres->sphere_index_by_placement[i] == kSphereIndexUnreachable) {
          return false;
        }
      }
      return true;
  }
}

// Per-tier seed acceptance — the accessibility axis (ALTTPR three-way).
//
// EVERY tier requires the goal be completable (the seed is beatable). The three
// ALTTPR-faithful tiers then differ only in the EXTRA reachability they demand:
//
//   kAccessibility_Locations (1) — "100% Locations": every placed location must
//       be reachable (unreachable_count == 0). Strictest. (kGoal_Completionist
//       already implies this via its own goal predicate, which iterates every
//       placement; selecting `locations` with a looser goal makes that bar
//       explicit.)
//   kAccessibility_Items (0, default) — "100% Inventory": every PROGRESSION
//       item's location must be reachable. Junk/consumables (rupees, arrows,
//       bombs), maps, compasses, and heart pieces/containers may strand (see
//       is_progression_item). Matches ALTTPR "100% inventory."
//   kAccessibility_None (2) — UI label "beatable only": goal completability is
//       the whole bar; items/locations may strand.
//
// Strictness nests: locations ⊇ items ⊇ beatable.
//
// NOTE: kAccessibility_None keeps its serialized-value name "None" (the value
// is part of the determinism contract) but its MEANING is ALTTPR's "beatable
// only" — the seed is still guaranteed completable. The old "ship a literally
// unwinnable seed" behavior is no longer reachable from this axis; the CLI
// --allow-broken-seed flag remains for diagnostic seeds.
bool Accessibility_SeedAcceptable(const RandoSettings *settings, uint64 seed_u64,
                                  const RandoPlacementTable *placements) {
  if (settings == NULL || placements == NULL) return false;
  // Beatability is required for every tier.
  if (!Goal_IsCompletable(settings, seed_u64, placements)) return false;
  // "beatable only" needs nothing more — skip the sphere walk entirely.
  // EFFECTIVE tier: Completionist forces Locations, so a raw accessibility=none
  // can't sneak past the sphere walk the canonical hash already promises.
  if (Settings_EffectiveAccessibility(settings) == kAccessibility_None) return true;
  // locations / items inspect per-placement reachability. Logic_ComputeSpheres
  // fully populates the per-placement index even when it returns false (i.e.
  // when something is unreachable), so we always read the indices afterward.
  RandoSpheres spheres;
  (void)Logic_ComputeSpheres(settings, placements, &spheres);
  return accessibility_reachability_ok(settings, placements, &spheres);
}

// Should the generator refuse to ship this seed? A thin negation of the
// accessibility-tier acceptance predicate. The spoiler's `goal_completable`
// field is always the pure reachability predicate (Goal_IsCompletable); the
// refusal gate additionally honors the accessibility tier.
bool Goal_ShouldRefuse(const RandoSettings *settings, uint64 seed_u64,
                       const RandoPlacementTable *placements) {
  if (settings == NULL) return false;
  return !Accessibility_SeedAcceptable(settings, seed_u64, placements);
}

// ---------------------------------------------------------------------------
// Sphere computation (randomizer-core / Sphere semantics).
//
// Walks the placement table by sphere:
//   Sphere 0: starting inventory only.
//   Sphere N+1: locations newly reachable under inventory accumulated from
//               spheres 0..N (items from those placements).
//
// On success: every placement gets an assigned sphere. On failure: any
// unreachable placement keeps sphere_index = kSphereIndexUnreachable.
// ---------------------------------------------------------------------------
bool Logic_ComputeSpheres(const RandoSettings *settings,
                          const RandoPlacementTable *placements,
                          RandoSpheres *out) {
  if (settings == NULL || placements == NULL || out == NULL) return false;
  memset(out, 0, sizeof(*out));
  for (uint16 i = 0; i < (uint16)(sizeof(out->sphere_index_by_placement) / sizeof(out->sphere_index_by_placement[0])); i++) {
    out->sphere_index_by_placement[i] = kSphereIndexUnreachable;
  }

  // Build the running inventory: starts as defaults (StartingHeart + world-
  // state pre-grants + vanilla-mode dungeon-item pre-grants) and accumulates
  // items from each completed sphere.
  RandoCounts counts;
  memset(&counts, 0, sizeof(counts));
  counts.by_item_id[121] = 3;  // StartingHeart
  Alttpr_AddStartingCounts(&counts);
  if (settings->world_state != kWorldState_Standard) {
    counts.by_item_id[122] = 1;  // RescuedZelda pre-grant
  }
  apply_vanilla_dungeon_item_grants(settings, &counts);

  uint16 remaining = placements->count;
  uint8 sphere = 0;
  bool hit_sphere_cap = false;
  while (remaining > 0 && sphere < kSphereMaxCount) {
    const RandoReachability *r = Logic_ComputeReachabilityFullKnowledge(&counts, settings);
    uint16 added_this_sphere = 0;
    for (uint16 i = 0; i < placements->count; i++) {
      if (out->sphere_index_by_placement[i] != kSphereIndexUnreachable) continue;
      uint16 loc_id = placements->entries[i].location_id;
      if (r != NULL && Reachability_HasLocation(r, loc_id)) {
        out->sphere_index_by_placement[i] = sphere;
        added_this_sphere++;
      }
    }
    if (added_this_sphere == 0) {
      // Fixed point — remaining placements are unreachable.
      break;
    }
    // Accumulate items from this sphere into the running inventory.
    for (uint16 i = 0; i < placements->count; i++) {
      if (out->sphere_index_by_placement[i] != sphere) continue;
      if (!placement_entry_grants_item(placements, i)) continue;
      uint16 item = placements->entries[i].item_id;
      if (item < 256 && counts.by_item_id[item] < 0xFFFF) {
        counts.by_item_id[item]++;
      }
    }
    out->max_sphere = sphere;
    remaining -= added_this_sphere;
    sphere++;
  }
  // distinguish "fixed-point reached" from "kSphereMaxCount hit".
  // The latter is rare (logic depth > kSphereMaxCount) but silent failure
  // would mark late-sphere REACHABLE placements as unreachable — they flow
  // into unreachable_count below and can refuse an accessibility-gated seed
  // as if the placement were genuinely broken. Surface the cap explicitly so
  // a cap-induced refusal is distinguishable from real unreachability.
  if (sphere == kSphereMaxCount && remaining > 0) {
    hit_sphere_cap = true;
    fprintf(stderr,
      "Logic_ComputeSpheres: SPHERE-CAP OVERFLOW — hit kSphereMaxCount=%d "
      "with %u placements unprocessed. These are still-expanding (NOT proven "
      "unreachable) but will be counted as unreachable; any accessibility "
      "refusal of this seed may be a false negative. Raise kSphereMaxCount "
      "(src/rando/rando_placement.h) — the cap is a loop bound, not an array "
      "size.\n",
      (int)kSphereMaxCount, (unsigned)remaining);
  }
  (void)hit_sphere_cap;  // already logged; spheres struct doesn't carry it

  // Count unreachable placements.
  out->unreachable_count = 0;
  for (uint16 i = 0; i < placements->count; i++) {
    if (out->sphere_index_by_placement[i] == kSphereIndexUnreachable) {
      out->unreachable_count++;
    }
  }
  return out->unreachable_count == 0;
}

// ---------------------------------------------------------------------------
// Starting-inventory injection (tasks.md §4.2).
//
// Phase A1 implementation: world-state-aware starting inventory.
//
// Per world-state:
//   - Open / Retro: Link starts with no extra items (uncle's slot still places
//     the chosen item; collected on encounter — distinct from Standard mode).
//   - Standard: Link starts swordless (link_sword_type = 0); the uncle's slot
//     determines what becomes the starting item once collected. No injection
//     here — the dispatcher at the uncle's grant site grants the placed item.
//   - Inverted: Link starts with the Moon Pearl as a virtual starting item
//     (since the inverted dark-world traversal does not require it; ALTTPR
//     pre-collects to keep the placement pool's MoonPearl meaningful at the
//     pendant logic gate). MagicMirror is similarly pre-collected per
//     `app/Region/Inverted/`. (Phase B note: confirm exact pre-collected
//     set against ALTTPR's `pre_collected_items` config when finalizing.)
//
// Atomicity: the dispatch calls below all run in the same call frame; no
// I/O happens between them.
// ---------------------------------------------------------------------------

// link_item_* RAM cells (offsets per features.h / variables.h).
// We can't include variables.h cleanly from src/rando/ without pulling in
// game-state headers; use the same address-by-offset pattern as features.h
// and just call the dispatcher helpers exposed by Phase A1 receive paths.
//
// For Phase A1 starting inventory, we set the simplest items directly via
// the existing dispatcher (Link_ReceiveItem). That dispatcher is in
// src/misc.c and is the same one §6 grant sites use.

extern void Link_ReceiveItem(uint8 item, int chest_position);

// Returns the RAM address of kRam_RandoStartingInventoryGranted to allow the
// injection to read/write the gate cell without pulling in the macro defs.
#include "../features.h"
extern uint8 g_ram[];

bool Rando_TryGrantStartingInventory(const RandoSettings *settings) {
  // `settings` MAY be NULL — production callers (e.g., the Module05_LoadFile
  // wiring) don't have the full RandoSettings struct on slot-reload because
  // the sidecar slot persists only `settings_hash` + a few additive ext bytes,
  // not the canonical settings blob. When NULL, the world_state-dependent
  // Inverted branch reads the world_state captured at Rando_ActivateSidecarSlot
  // via Rando_GetActiveWorldState() (the sidecar header carries world_state
  // additively at @68); the placement-based escape-ammo grant doesn't need
  // world_state. CLI generation paths that DO have settings should pass them.
  if (g_rando_slot_active == 0) return false;
  // Official imports have Uncle-based, checkpoint-specific escape supplies.
  // Keep the native generator's starting policy separate from that contract.
  if (Alttpr_IsActive()) {
    bool granted=Alttpr_ApplyEscapeRefill();
    g_rando_starting_inventory_granted=1;
    return granted;
  }

  // Inverted: pre-grant Moon Pearl ONLY. The Magic Mirror is intentionally NOT
  // a starting item (add-rando-inverted-dark-chapel-spawn): ALTTPR places the
  // mirror in the world (it is not pre-collected), and the post-Agahnim
  // spawn-select gates its third option ("Dark Mountain") on having the Magic
  // Mirror (link_item_mirror == 2). Auto-granting the mirror at start made Dark
  // Mountain a free spawn option from the first frame; leaving it to be found
  // restores the vanilla unlock. This is logic-safe: Place_AssumedFill never
  // pre-collected the mirror (only RescuedZelda is pre-granted for non-Standard),
  // so placement/reachability is unchanged — the player simply must find the
  // mirror to do LW->DW mirror trips, exactly as the logic graph already assumes.
  // Moon Pearl stays a start grant (no-bunny in the Light World). This is ALSO
  // baked into the fresh-save SRAM image at slot creation (rando_generate.c), so
  // this is defense-in-depth — and idempotent. We grant ABOVE the once-per-boot
  // and cold-boot gates because:
  //   (a) `settings` is NULL on slot-reload (the sidecar persists only
  //       settings_hash, not the canonical settings blob), so we read the
  //       world_state captured at Rando_ActivateSidecarSlot instead; and
  //   (b) the Inverted starting state must hold on EVERY load (including
  //       cold-boot of an in-progress save), where the cold-boot gate below
  //       would otherwise short-circuit before reaching the old grant site.
  // rando-exempt: state-shuffle — bunny-state starting inventory (Inverted)
  {
    uint8 ws = (settings != NULL) ? settings->world_state
                                  : Rando_GetActiveWorldState();
    if (ws == kWorldState_Inverted) {
      // Direct byte-set, NOT Link_ReceiveItem: the receive path always triggers
      // the hold-up-item animation (kPlayerState_HoldUpItem = 0x15). Because
      // this grant runs on EVERY load (it sits above the cold-boot dedupe so
      // Inverted MP+Mirror persist across reloads), Link_ReceiveItem re-enters
      // that pose every frame -> permanent arm-raised softlock (confirmed by an
      // F12 dump: handler_state=0x15, pose_for_item=1). A direct write is
      // idempotent and animation-free.
      // rando-exempt: state-shuffle — bunny-state starting inventory (Inverted)
      g_ram[0xF357] = 1;  // link_item_moon_pearl
      // (Magic Mirror is deliberately NOT granted — see the comment above; it is
      // a found item so the spawn-select Dark Mountain option unlocks vanilla-style.)
    }
  }

  if (g_rando_starting_inventory_granted != 0) return false;

  // Cold-boot exploit guard. `g_rando_starting_inventory_granted` lives at
  // g_ram[0x65e] — OUTSIDE the SRAM-saved range (save_dung_info covers
  // 0xF000-0xF4FF), and ZeldaInitialize zeroes the cell on every cold boot.
  // Without an additional save-state-persisted check, every cold-boot reload
  // of an in-progress save would re-fire the escape-fill, queueing a fresh
  // 70-arrow / 0x80-magic / 50-bomb refill into the HUD filler registers
  // every launch. Gate on `sram_progress_indicator` (g_ram[0xF3C5]) which IS
  // SRAM-saved: 0 = brand-new save (Uncle's gift not yet received), 1+ =
  // past escape. Mid-escape reloads (progress still 0) intentionally re-fire
  // the refill, matching ALTTPR's setEscapeFills semantics (refill on each
  // Uncle/Zelda/Sanctuary respawn).
  //
  // The Inverted Moon-Pearl grant lives ABOVE this gate (see the top of this
  // function) precisely so it still fires when this cold-boot guard
  // short-circuits. That grant is idempotent (a direct already-owned byte-set),
  // so re-running it every boot is harmless; only the escape-ammo filler below
  // must be guarded.
  if (g_ram[0xF3C5] != 0) {
    g_rando_starting_inventory_granted = 1;  // dedupe within this boot
    return false;
  }

  // (Inverted Moon Pearl is granted ABOVE the cold-boot gate now — see the top
  // of this function — so the grant fires on reload where settings is NULL and
  // survives the cold-boot short-circuit. The Magic Mirror is no longer granted.)

  // Escape-ammo pre-grant. Prevents impossible-seed cases where the sphere-0
  // weapon needs ammo the player doesn't start with (bow/no-arrows,
  // FireRod/no-magic, bombs-only/no-bombs). This port diverges from ALTTPR by
  // treating four chests as sphere-0 (Link's House, Uncle, HC Map Chest,
  // Secret Passage — see `Place_AssumedFill`'s sphere-0 constraint), so the
  // inspection scans all four rather than only Uncle's slot.
  //
  // Applied in ALL world-states. Standard mode needs it for HC escape combat;
  // Open/Inverted/Retro don't have escape but the grant is harmless (slightly
  // generous starting ammo). Keying on world_state would require settings,
  // which isn't available on slot-reload (see the escape-fill TODO below).
  //
  // TODO(escape-fill v2): replace this one-shot pre-grant with a faithful
  // port of ALTTPR's `World::setEscapeFills` (in `app/World.php`). That
  // version hooks Uncle's death, Zelda's cell, and Sanctuary mantle to refill
  // ammo on each respawn during escape, and exposes a `rom.EscapeAssist`
  // toggle for infinite ammo during the escape state. This stopgap grants
  // once at game start, so dying mid-escape leaves the player without refill.
  // Refill counts (70/0x80/50) mirror ALTTPR's `Rom.php` EscapeRefills.Uncle.*
  // defaults — keep them in sync with the v2 implementation.
  if (g_active_placement != NULL) {
    static const uint16 kEscapeSlots[] = {
      LOC_Link_s_House,
      LOC_Link_s_Uncle,
      LOC_Hyrule_Castle_Map_Chest,
      LOC_Secret_Passage,
    };
    bool grant_arrows = false, grant_magic = false, grant_bombs = false;
    for (uint16 i = 0; i < g_active_placement->count; i++) {
      uint16 loc = g_active_placement->entries[i].location_id;
      uint16 item = g_active_placement->entries[i].item_id;
      bool in_escape_slot = false;
      for (size_t s = 0; s < sizeof(kEscapeSlots) / sizeof(kEscapeSlots[0]); s++) {
        if (loc == kEscapeSlots[s]) { in_escape_slot = true; break; }
      }
      if (!in_escape_slot) continue;
      switch (item) {
        case ITEM_Bow: case ITEM_ProgressiveBow:
          grant_arrows = true; break;
        case ITEM_FireRod: case ITEM_CaneOfSomaria: case ITEM_CaneOfByrna:
          grant_magic = true; break;
        case ITEM_Bombs1: case ITEM_Bombs3: case ITEM_Bombs10:
          grant_bombs = true; break;
      }
    }
    // Direct cell writes (offset-pattern per top-of-block note re: variables.h).
    // 0xF373 = link_magic_filler, 0xF375 = link_bomb_filler, 0xF376 = link_arrow_filler.
    // The HUD tick (src/hud.c) drains each filler into its live count over frames.
    if (grant_arrows) g_ram[0xF376] = 70;
    if (grant_magic)  g_ram[0xF373] = 0x80;
    if (grant_bombs)  g_ram[0xF375] = 50;
  }

  // Open / Retro: no pre-grant by default; future hero-mode (Phase B) may
  // pre-grant a starting bottle, hearts, etc.

  // Mark gate so save-reload does not re-inject. Per §4.2 atomicity, this
  // SHALL happen in the same frame as the grants above.
  g_rando_starting_inventory_granted = 1;
  return true;
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[Placement_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void Placement_SelfCheck(void) {
  // Central Key Ring source/grant metadata must stay aligned with the pool's
  // established base table, and active itemized drop sources may never exceed
  // the authored drop stock that backs runtime ring grants.
  {
    RandoSettings sources;
    Settings_SetDefaults(&sources);
    sources.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sources.pot_shuffle = kPotShuffle_Keys;
    sources.enemy_drop_checks = kEnemyDropChecks_Keys;
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      if (Rando_KeyRingGrantCount(d) !=
              (uint8)(Rando_BaseSmallKeyCount(d) +
                      Rando_DropSmallKeyCount(d)))
        selfcheck_die("Key Ring base/drop/grant metadata drift");
      uint16 active_drop_sources = 0;
      for (uint32 i = 0; i < kRandoLocationsCount; i++) {
        const RandoLocationDef *loc = &kRandoLocations[i];
        if (loc->vanilla_item_id != Rando_SmallKeyItemForRandoDungeon(d))
          continue;
        if ((loc->type == LOCTYPE_Pot && pot_active(loc, &sources)) ||
            (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sources)))
          active_drop_sources++;
      }
      if (active_drop_sources > Rando_DropSmallKeyCount(d))
        selfcheck_die("active itemized key sources exceed authored drop stock");
    }
  }

  // Build identity placement, compute digest, assert digest stable across
  // sort-order perturbations of input.
  RandoPlacement entries[3] = {
    { 100, 5 },
    { 50,  10 },
    { 200, 15 },
  };
  RandoPlacementTable t = { entries, 3 };
  uint8 digest1[32];
  PlacementTable_ComputeDigest(&t, digest1);

  // Same entries in different order → same digest (sort invariance).
  RandoPlacement entries2[3] = {
    { 200, 15 },
    { 50,  10 },
    { 100, 5 },
  };
  RandoPlacementTable t2 = { entries2, 3 };
  uint8 digest2[32];
  PlacementTable_ComputeDigest(&t2, digest2);

  if (memcmp(digest1, digest2, 32) != 0) {
    selfcheck_die("placement digest not sort-invariant");
  }

  // Presence-aware lookup must distinguish an identity placement from a miss
  // on both the sorted fast path and the defensive unsorted path. The legacy
  // wrapper intentionally collapses those outcomes to the same item value.
  {
    uint16 found = 0xBEEF;
    Placement_Install(&t);  // deliberately unsorted: linear fallback
    if (Placement_ActiveIsSorted() ||
        !Placement_TryLookup(100, &found) || found != 5 ||
        Placement_TryLookup(101, &found) || found != 5)
      selfcheck_die("presence-aware lookup failed on unsorted table");

    RandoPlacement sorted_entries[3] = {
      {50, 10}, {100, 5}, {200, 15},
    };
    RandoPlacementTable sorted = {sorted_entries, 3};
    Placement_Install(&sorted);
    found = 0xBEEF;
    if (!Placement_ActiveIsSorted() ||
        !Placement_TryLookup(100, &found) || found != 5 ||
        Placement_TryLookup(101, &found) || found != 5 ||
        Placement_Lookup(100, 5) != 5 ||
        Placement_Lookup(101, 5) != 5)
      selfcheck_die("identity-vs-absent lookup contract failed on sorted table");

    Placement_Install(NULL);
    found = 0xBEEF;
    if (Placement_TryLookup(100, &found) || found != 0xBEEF)
      selfcheck_die("presence-aware lookup must fail cleanly without a table");
  }

  // Changing one item changes the digest.
  entries[0].item_id = 99;
  uint8 digest3[32];
  PlacementTable_ComputeDigest(&t, digest3);
  if (memcmp(digest1, digest3, 32) == 0) {
    selfcheck_die("placement digest collision on different items");
  }

  // The Desert bonk torch is the sole placed Desert small key in vanilla-key
  // mode. If its registry metadata drifts to the Boots reach requirement (the
  // historical bug), the vanilla pre-pin misses it and a random item can strand
  // the player behind Desert's key door.
  {
    const RandoLocationDef *desert_torch = NULL;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      if (kRandoLocations[i].id == LOC_Desert_Palace_Torch) {
        desert_torch = &kRandoLocations[i];
        break;
      }
    }
    if (desert_torch == NULL ||
        desert_torch->vanilla_item_id != ID_SmallKey_DP)
      selfcheck_die("Desert Palace torch must carry its vanilla small key");
    RandoSettings vanilla_keys;
    Settings_SetDefaults(&vanilla_keys);
    if (!location_is_prepinned(desert_torch, &vanilla_keys))
      selfcheck_die("Desert Palace torch must be pinned in vanilla-key mode");
  }

  // BuildItemPool: with default settings (Open), pool size equals the count
  // of FILLABLE locations active in the Open world-state — i.e. excluding
  // TakeAny slots and the slots the §3b pre-place pass pins (prizes, events,
  // medallions, vanilla-mode dungeon items), which never consume a pool item.
  // With NULL settings, the function safely
  // returns 0.
  {
    RandoSettings defaults;
    Settings_SetDefaults(&defaults);
    uint16 pool[kRandoLocationCapacity];
    uint16 n = BuildItemPool(&defaults, 0, pool, kRandoLocationCapacity);
    // Expected = count of Open-active locations that are neither TakeAny nor
    // pre-pinned under default settings (mirrors the junk-pad target loop).
    uint16 expected = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (!Rando_LocationWorldStateActive(loc, &defaults)) continue;
      if (loc->type == LOCTYPE_TakeAny) continue;
      // Same tier-aware predicate as the open-loc + junk-pad loops. Under default
      // settings pot_shuffle is Off, so pot_active is false and every pot is
      // excluded here (the expected count is unchanged from pre-pot-sanity).
      if (loc->type == LOCTYPE_Pot && !pot_active(loc, &defaults)) continue;
      if (loc->type == LOCTYPE_EnemyDrop && !enemy_drop_active(loc, &defaults)) continue;
      if (loc->type == LOCTYPE_Enemy && !enemy_check_active(loc, &defaults)) continue;
      if ((loc->type == LOCTYPE_Grass || loc->type == LOCTYPE_Rock ||
           loc->type == LOCTYPE_Bonk) &&
          !terrain_active(loc, &defaults)) continue;
      if (location_is_prepinned(loc, &defaults)) continue;
      expected++;
    }
    if (n != expected) {
      fprintf(stderr, "[Placement_SelfCheck] BuildItemPool returned %u, expected %u\n",
              (unsigned)n, (unsigned)expected);
      selfcheck_die("BuildItemPool count mismatch");
    }
    uint16 default_traps = 0;
    for (uint16 i = 0; i < n; i++)
      if (pool[i] == ID_TrapDamage || pool[i] == ID_TrapFreeze) default_traps++;
    if (default_traps != 0) selfcheck_die("default BuildItemPool must not contain traps");
    // NULL settings → 0 (safe rejection, not crash).
    uint16 n_null = BuildItemPool(NULL, 0, pool, kRandoLocationCapacity);
    if (n_null != 0) selfcheck_die("BuildItemPool(NULL) should return 0");

    // add-rando-shopsanity — with the axis on, the 27 regular shop slots are
    // ordinary open fill targets in EVERY world state: the pool grows by
    // exactly the shop-slot count vs the same-world-state baseline. Open
    // exercises the world_state_filter bypass (slots are Retro-filtered),
    // Retro exercises the formerly-prepinned arm.
    {
      uint16 shop_slot_count = 0;
      for (uint32 i = 0; i < kRandoLocationsCount; i++)
        if (kRandoLocations[i].type == LOCTYPE_Shop) shop_slot_count++;
      if (shop_slot_count != 27)
        selfcheck_die("registry must carry exactly 27 regular shop slots");
      RandoSettings shopson = defaults;
      shopson.shopsanity = 1;
      uint16 n_on = BuildItemPool(&shopson, 0, pool, kRandoLocationCapacity);
      if (n_on != n + shop_slot_count)
        selfcheck_die("shopsanity(Open) pool must grow by the shop-slot count");
      RandoSettings retro_off = defaults, retro_on;
      retro_off.world_state = kWorldState_Retro;
      retro_on = retro_off;
      retro_on.shopsanity = 1;
      uint16 n_roff = BuildItemPool(&retro_off, 0, pool, kRandoLocationCapacity);
      uint16 n_ron = BuildItemPool(&retro_on, 0, pool, kRandoLocationCapacity);
      if (n_ron != n_roff + shop_slot_count)
        selfcheck_die("shopsanity(Retro) pool must grow by the shop-slot count");
    }
  }

  // add-rando-bonk-sanity — pool accounting per tier (junk and all both add
  // every registered bonk location to the fillable set; off adds none), the
  // junk-only proof (a junk-tier fill never puts progression on a bonk
  // location), and lookup sortedness. Skipped entirely on a registry-less
  // build (generation there fails closed instead — the terrain pattern).
  {
    uint16 bonk_locs = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++)
      if (kRandoLocations[i].type == LOCTYPE_Bonk) bonk_locs++;
    if (bonk_locs > 0) {
      RandoSettings boff, bjunk, ball;
      Settings_SetDefaults(&boff);
      bjunk = boff; bjunk.bonk_shuffle = kTerrainShuffle_Junk;
      ball = boff;  ball.bonk_shuffle = kTerrainShuffle_All;
      uint16 pool_b[kRandoLocationCapacity];
      uint16 n_off = BuildItemPool(&boff, 0, pool_b, kRandoLocationCapacity);
      uint16 n_junk = BuildItemPool(&bjunk, 0, pool_b, kRandoLocationCapacity);
      uint16 n_all = BuildItemPool(&ball, 0, pool_b, kRandoLocationCapacity);
      if (n_junk != n_off + bonk_locs || n_all != n_off + bonk_locs)
        selfcheck_die("bonk_shuffle pool must grow by the bonk-location count");
      static RandoPlacement bonk_entries[kRandoLocationCapacity];
      RandoPlacementTable bt = { bonk_entries, 0 };
      if (!Place_AssumedFill(&bjunk, 0x424F4E4Bull, 0, &bt))
        selfcheck_die("bonk junk-tier fill failed");
      for (uint16 e = 0; e < bt.count; e++) {
        uint16 lid = bt.entries[e].location_id;
        const RandoLocationDef *ld = NULL;
        for (uint32 i = 0; i < kRandoLocationsCount; i++)
          if (kRandoLocations[i].id == lid) { ld = &kRandoLocations[i]; break; }
        if (ld != NULL && ld->type == LOCTYPE_Bonk &&
            is_progression_item(bt.entries[e].item_id))
          selfcheck_die("bonk junk tier placed progression on a bonk location");
      }
      for (uint32 i = 1; i < (uint32)kRandoBonkLookup_COUNT; i++) {
        if (kRandoBonkLookup[i - 1].area > kRandoBonkLookup[i].area ||
            (kRandoBonkLookup[i - 1].area == kRandoBonkLookup[i].area &&
             kRandoBonkLookup[i - 1].block >= kRandoBonkLookup[i].block))
          selfcheck_die("bonk lookup must be strictly (area, block)-sorted");
      }
    }
  }

  // Key Ring selector/pool invariants: All collapses every eligible family to
  // one ring, Random is deterministic and a proper subset, Off preserves pool
  // cardinality, and Skeleton Key replaces one junk slot.
  {
    RandoSettings off, all, random, skeleton;
    Settings_SetDefaults(&off);
    off.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    all = off;
    all.key_rings = kKeyRings_All;
    random = off;
    random.key_rings = kKeyRings_Random;
    skeleton = off;
    skeleton.skeleton_key = 1;
    KeyRingSelection all_selection, random_a, random_b;
    if (!KeyRings_Resolve(&all, 0x4B455952494E4753ull, &all_selection) ||
        all_selection.selected_mask != all_selection.eligible_mask)
      selfcheck_die("Key Rings All selector must return every eligible family");
    if (!KeyRings_Resolve(&random, 0x4B455952494E4753ull, &random_a) ||
        !KeyRings_Resolve(&random, 0x4B455952494E4753ull, &random_b) ||
        random_a.selected_mask != random_b.selected_mask ||
        random_a.selected_mask == 0 ||
        random_a.selected_mask == random_a.eligible_mask)
      selfcheck_die("Key Rings Random selector must be deterministic proper subset");
    uint16 eligible = all_selection.eligible_mask;
    uint16 random_mask = random_a.selected_mask;
    if (eligible != 0x1FFDu || random_mask != 0x1229u)
      selfcheck_die("Key Rings fixed-seed selector mask drifted");

    uint16 pool_off[kRandoLocationCapacity];
    uint16 pool_all[kRandoLocationCapacity];
    uint16 pool_random[kRandoLocationCapacity];
    uint16 pool_skeleton[kRandoLocationCapacity];
    uint16 n_off = BuildItemPool(&off, 0x4B455952494E4753ull,
                                 pool_off, kRandoLocationCapacity);
    uint16 n_all = BuildItemPool(&all, 0x4B455952494E4753ull,
                                 pool_all, kRandoLocationCapacity);
    uint16 n_random = BuildItemPool(&random, 0x4B455952494E4753ull,
                                    pool_random, kRandoLocationCapacity);
    uint16 n_skeleton = BuildItemPool(&skeleton, 0x4B455952494E4753ull,
                                      pool_skeleton, kRandoLocationCapacity);
    if (n_off == 0 || n_all != n_off || n_random != n_off || n_skeleton != n_off)
      selfcheck_die("Key Rings/Skeleton must preserve item-pool cardinality");
    uint16 skeleton_count = 0;
    for (uint16 i = 0; i < n_skeleton; i++)
      if (pool_skeleton[i] == ITEM_SkeletonKey) skeleton_count++;
    if (skeleton_count != 1)
      selfcheck_die("Skeleton-enabled pool must contain exactly one Skeleton Key");

    CustomizerManifest cm;
    memset(&cm, 0, sizeof cm);
    cm.pool_add[cm.pool_add_count++] = ITEM_SmallKey_PalaceOfDarkness;
    if (customizer_validate_key_cardinality(&cm, pool_all, n_all, NULL, 0))
      selfcheck_die("customizer pool override must not alter small-key cardinality");
    Customizer__ClearError();
    memset(&cm, 0, sizeof cm);
    cm.pins[0].item_id = ITEM_SmallKey_PalaceOfDarkness;
    cm.pin_count = 1;
    if (customizer_validate_key_cardinality(&cm, pool_all, n_all, NULL, 0))
      selfcheck_die("customizer must not reintroduce a selected-family key");
    Customizer__ClearError();
    memset(&cm, 0, sizeof cm);
    cm.pins[0].item_id = ITEM_KeyRing_PalaceOfDarkness;
    cm.pins[1].item_id = ITEM_KeyRing_PalaceOfDarkness;
    cm.pin_count = 2;
    if (customizer_validate_key_cardinality(&cm, pool_all, n_all, NULL, 0))
      selfcheck_die("customizer must reject duplicate Key Ring pins");
    Customizer__ClearError();
    memset(&cm, 0, sizeof cm);
    cm.pins[0].item_id = ITEM_SkeletonKey;
    cm.pin_count = 1;
    if (customizer_validate_key_cardinality(&cm, pool_off, n_off, NULL, 0))
      selfcheck_die("customizer must reject disabled Skeleton Key pin");
    Customizer__ClearError();
    if (!customizer_validate_key_cardinality(&cm, pool_skeleton, n_skeleton,
                                             NULL, 0))
      selfcheck_die("customizer must allow relocating enabled Skeleton Key");

    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      uint16 key = Rando_SmallKeyItemForRandoDungeon(d);
      uint16 ring = Rando_KeyRingItemForRandoDungeon(d);
      uint16 all_keys = 0, all_rings = 0, random_keys = 0, random_rings = 0;
      for (uint16 i = 0; i < n_all; i++) {
        if (pool_all[i] == key) all_keys++;
        if (pool_all[i] == ring) all_rings++;
      }
      for (uint16 i = 0; i < n_random; i++) {
        if (pool_random[i] == key) random_keys++;
        if (pool_random[i] == ring) random_rings++;
      }
      uint16 bit = (uint16)(1u << d);
      if (eligible & bit) {
        if (all_keys != 0 || all_rings != 1)
          selfcheck_die("All-ring pool must hold one ring and zero family keys");
      } else if (all_rings != 0) {
        selfcheck_die("ineligible family must not gain a Key Ring");
      }
      if (random_mask & bit) {
        if (random_keys != 0 || random_rings != 1)
          selfcheck_die("selected Random family collapse is not one-check exact");
      } else if (random_rings != 0) {
        selfcheck_die("unselected Random family gained a Key Ring");
      }
    }

    // Requested modes that are ineffective under Vanilla/Retro resolve Off and
    // do not trigger Random's >=2-family refusal.
    RandoSettings normalized;
    Settings_SetDefaults(&normalized);
    normalized.key_rings = kKeyRings_Random;
    KeyRingSelection normalized_selection;
    if (!KeyRings_Resolve(&normalized, 1, &normalized_selection) ||
        normalized_selection.selected_mask != 0)
      selfcheck_die("Vanilla requested Random Key Rings must resolve Off");
    normalized.world_state = kWorldState_Retro;
    normalized.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    if (!KeyRings_Resolve(&normalized, 1, &normalized_selection) ||
        normalized_selection.selected_mask != 0)
      selfcheck_die("Retro requested Random Key Rings must resolve Off");

    // Source matrix: independently and jointly enable itemized key pots and
    // forced enemy key drops. Every eligible family must still collapse to the
    // exact one-ring/zero-key invariant.
    for (uint8 source_case = 1; source_case < 4; source_case++) {
      if ((source_case & 1u) && !pot_registry_available()) continue;
      if ((source_case & 2u) && !enemy_drop_registry_available()) continue;
      RandoSettings sc = all;
      if (source_case & 1u) sc.pot_shuffle = kPotShuffle_Keys;
      if (source_case & 2u) sc.enemy_drop_checks = kEnemyDropChecks_Keys;
      uint16 source_mask = KeyRings_EligibleMask(&sc);
      uint16 source_pool[kRandoLocationCapacity];
      uint16 source_n = BuildItemPool(&sc, 0x5352434D41545258ull,
                                      source_pool, kRandoLocationCapacity);
      if (source_n == 0) selfcheck_die("Key Ring source-matrix pool failed");
      for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
        uint16 keys = 0, rings = 0;
        for (uint16 i = 0; i < source_n; i++) {
          if (source_pool[i] == Rando_SmallKeyItemForRandoDungeon(d)) keys++;
          if (source_pool[i] == Rando_KeyRingItemForRandoDungeon(d)) rings++;
        }
        if (source_mask & (uint16)(1u << d)) {
          if (keys != 0 || rings != 1)
            selfcheck_die("pot/enemy Key Ring source collapse drift");
        } else if (rings != 0) {
          selfcheck_die("pot/enemy source matrix emitted ineligible ring");
        }
      }
    }
  }

  // add-rando-traps — frequency replaces final junk-filled placements (not
  // pre-fill pool entries, which can be dropped) and alternates damage/freeze.
  // Default-off was asserted above.
  {
    RandoSettings s;
    static const struct { uint8 freq; uint16 count; } kTrapCases[] = {
      { kTrapFrequency_Low, 4 },
      { kTrapFrequency_Medium, 8 },
      { kTrapFrequency_High, 16 },
    };
    for (uint8 c = 0; c < (uint8)(sizeof(kTrapCases) / sizeof(kTrapCases[0])); c++) {
      Settings_SetDefaults(&s);
      s.traps = kTrapCases[c].freq;
      static RandoPlacement trap_entries[kRandoLocationCapacity];
      RandoPlacementTable tt = { trap_entries, 0 };
      if (!Place_AssumedFill(&s, (uint64)(0x54524150u + c), 0, &tt))
        selfcheck_die("trap placement selfcheck could not generate a placement");
      // add-rando-trap-catalog — type is now (seed,location)-selected across the
      // whole catalog (no Damage/Freeze even split). Assert the trap COUNT honors
      // the frequency and every placed trap id is in the contiguous block.
      uint16 traps = 0;
      for (uint16 i = 0; i < tt.count; i++) {
        uint16 id = tt.entries[i].item_id;
        if (id >= ID_TrapDamage && id <= ID_TrapTeleport) traps++;
      }
      if (traps != kTrapCases[c].count)
        selfcheck_die("trap frequency placed wrong number of traps");
    }
  }

  // Placement-side selfchecks for pot activation and lookup invariants.
  {
    // (a) Pot type round-trips through codegen (a silent type→0 mapping would
    //     leave zero LOCTYPE_Pot locations), empty pots carry ITEM_Nothing, and
    //     ITEM_Nothing is a logic no-op (never progression).
    uint32 pot_locs = 0, empty_pots = 0, key_pots = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      if (kRandoLocations[i].type != LOCTYPE_Pot) continue;
      pot_locs++;
      if (kRandoLocations[i].vanilla_item_id == ITEM_Nothing) empty_pots++;
      else if (is_small_key_item(kRandoLocations[i].vanilla_item_id)) key_pots++;
    }
    bool has_pot_registry = pot_locs != 0;
    if (has_pot_registry && empty_pots == 0)
      selfcheck_die("no empty pots found (ITEM_Nothing mapping broken)");
    if (is_progression_item(ITEM_Nothing))
      selfcheck_die("ITEM_Nothing must be a logic no-op (non-progression)");

    // (b) Tier nesting keys ⊆ contents ⊆ all, computed from the SAME pot_active
    //     predicate the placer uses; Off activates zero pots (the D9 invariant).
    RandoSettings so, sk, sc, sa;
    Settings_SetDefaults(&so); so.pot_shuffle = kPotShuffle_Off;
    Settings_SetDefaults(&sk); sk.pot_shuffle = kPotShuffle_Keys;
    Settings_SetDefaults(&sc); sc.pot_shuffle = kPotShuffle_Contents;
    Settings_SetDefaults(&sa); sa.pot_shuffle = kPotShuffle_All;
    uint32 n_off = 0, n_keys = 0, n_cont = 0, n_all = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type != LOCTYPE_Pot) continue;
      n_off  += pot_active(loc, &so) ? 1 : 0;
      n_keys += pot_active(loc, &sk) ? 1 : 0;
      n_cont += pot_active(loc, &sc) ? 1 : 0;
      n_all  += pot_active(loc, &sa) ? 1 : 0;
    }
    if (n_off != 0) selfcheck_die("pot_shuffle Off must activate zero pots (D9 byte-identical)");
    if (!(n_keys <= n_cont && n_cont <= n_all)) selfcheck_die("pot tiers must nest keys<=contents<=all");
    if (n_keys != key_pots) selfcheck_die("Keys tier must activate exactly the key pots");
    if (n_all != pot_locs) selfcheck_die("All tier must activate every pot");

    // (b') Pot-only DUNGEON free grants must equal door-rando DROP total minus
    //      active key pots for every dungeon with key pots. This pins the dynamic
    //      seed_dungeon_free_key_drops path to the same invariant the old static
    //      kPotNonpotDropCounts table represented.
    {
      RandoSettings spd;
      Settings_SetDefaults(&spd);
      spd.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
      spd.pot_shuffle = kPotShuffle_Keys;
      RandoCounts seeded;
      memset(&seeded, 0, sizeof(seeded));
      seed_dungeon_free_key_drops(&seeded, &spd);
      for (uint8 d = 0; d < 13; d++) {
        uint16 key = Rando_SmallKeyItemForRandoDungeon(d);
        uint8 npots = 0;
        for (uint32 i = 0; i < kRandoLocationsCount; i++)
          if (kRandoLocations[i].type == LOCTYPE_Pot &&
              kRandoLocations[i].vanilla_item_id == key) npots++;
        if (npots > Rando_DropSmallKeyCount(d))
          selfcheck_die("pot keys exceed door-rando drop total (pots.gen.yaml drift)");
        // npots == 0 dungeons still free-grant their whole drop total: the
        // key-depth ops are GLOBAL, so their gates evaluate everywhere while
        // every non-itemized drop falls vanilla-free at runtime (the 42-check
        // entrance x dungeon-keys strand fix).
        uint16 want = (uint16)(Rando_DropSmallKeyCount(d) - npots);
        if (seeded.by_item_id[key] != want)
          selfcheck_die("Dungeon free-drop seeding drifted for pot-only keys");
      }
    }

    // (c) door shuffle composes with pot shuffle; cave entrance shuffle still
    //     forces every pot inactive because cave/house pot region overrides are
    //     not modeled.
    RandoSettings sd;
    Settings_SetDefaults(&sd); sd.pot_shuffle = kPotShuffle_All; sd.door_shuffle = kDoorShuffle_Basic;
    uint32 n_door = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++)
      if (kRandoLocations[i].type == LOCTYPE_Pot && pot_active(&kRandoLocations[i], &sd)) n_door++;
    if (n_door != n_all) selfcheck_die("door shuffle must preserve active pots");
    RandoSettings sce;
    Settings_SetDefaults(&sce); sce.pot_shuffle = kPotShuffle_All; sce.shuffle_cave_entrances = 1;
    uint32 n_cave = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++)
      if (kRandoLocations[i].type == LOCTYPE_Pot && pot_active(&kRandoLocations[i], &sce)) n_cave++;
    if (n_cave != 0) selfcheck_die("cave entrance shuffle must force every pot inactive");

    if (!has_pot_registry) {
      uint16 missing_pool[kRandoLocationCapacity];
      uint16 n_missing = BuildItemPool(&sa, 0, missing_pool, kRandoLocationCapacity);
      if (n_missing != 0)
        selfcheck_die("pot_shuffle must fail closed when the pot registry is absent");
    } else {
      // (d) Placement_Lookup binary search: the All-tier assumed-fill output is
      //     sorted by location_id, and binary search agrees with the table on every
      //     entry plus a miss.
      static RandoPlacement pot_entries[kRandoLocationCapacity];
      RandoPlacementTable pt = { pot_entries, 0 };
      if (!Place_AssumedFill(&sa, 0x504F5453ull /*"POTS"*/, 0, &pt))
        selfcheck_die("All-tier pot placement could not be generated");
      Placement_Install(&pt);
      if (!Placement_ActiveIsSorted())
        selfcheck_die("assumed-fill placement table is not location-id sorted (binary search invalid)");
      for (uint16 i = 0; i < pt.count; i++) {
        if (Placement_Lookup(pt.entries[i].location_id, 0xFFFF) != pt.entries[i].item_id)
          selfcheck_die("Placement_Lookup binary search disagrees with the table entry");
      }
    }
    if (Placement_Lookup(0xFFFE, 0x1234) != 0x1234)
      selfcheck_die("Placement_Lookup must return the vanilla fallback for a missing location");
    Placement_Install(NULL);
  }

  // add-rando-grass-rock-shuffle — terrain tier filter + junk-only invariant.
  {
    uint32 grass_locs = 0, rock_locs = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      if (kRandoLocations[i].type == LOCTYPE_Grass) grass_locs++;
      if (kRandoLocations[i].type == LOCTYPE_Rock) rock_locs++;
    }
    RandoSettings st;
    Settings_SetDefaults(&st);
    uint32 n_def = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++)
      if (terrain_active(&kRandoLocations[i], &st)) n_def++;
    if (n_def != 0) selfcheck_die("terrain must be inactive at default settings");
    st.grass_shuffle = kTerrainShuffle_All;
    st.rock_shuffle = kTerrainShuffle_Junk;
    uint32 n_grass = 0, n_rock = 0, n_junk_only = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Grass && terrain_active(loc, &st)) n_grass++;
      if (loc->type == LOCTYPE_Rock && terrain_active(loc, &st)) n_rock++;
      if (terrain_junk_only(loc, &st)) n_junk_only++;
    }
    if (n_grass != grass_locs) selfcheck_die("grass all-tier must activate every grass location");
    if (n_rock != rock_locs) selfcheck_die("rock junk-tier must activate every rock location");
    if (n_junk_only != rock_locs) selfcheck_die("junk-only class must cover exactly the junk-tier axis");
    // Empty-registry fail-closed (review HIGH-1): an assetless build has no
    // terrain locations, so a terrain-active pool MUST come back empty (0) —
    // BuildItemPool refuses rather than silently producing a normal seed with
    // no terrain checks (which would then fail-open through activation). This
    // is the terrain analogue of the pot fail-closed proof above.
    if (grass_locs == 0 && rock_locs == 0) {
      RandoSettings se;
      Settings_SetDefaults(&se);
      se.grass_shuffle = kTerrainShuffle_All;
      se.rock_shuffle = kTerrainShuffle_All;
      uint16 empty_pool[kRandoLocationCapacity];
      if (BuildItemPool(&se, 0, empty_pool, kRandoLocationCapacity) != 0)
        selfcheck_die("grass/rock shuffle must fail closed when the terrain registry is absent");
    }
    // Junk-only placement invariant: fill a junk-tier seed and assert no
    // progression item landed on a junk-only location (junk-pad items are
    // fine — that is the tier's purpose). Present only when the registry is.
    if (grass_locs != 0 && rock_locs != 0) {
      RandoSettings sj;
      Settings_SetDefaults(&sj);
      sj.grass_shuffle = kTerrainShuffle_Junk;
      sj.rock_shuffle = kTerrainShuffle_Junk;
      static RandoPlacement terr_entries[kRandoLocationCapacity];
      RandoPlacementTable tt = { terr_entries, 0 };
      if (!Place_AssumedFill(&sj, 0x4752415353ull, 0, &tt))
        selfcheck_die("junk-tier terrain placement could not be generated");
      // The spec's junk-tier contract: no POOL PROGRESSION item on a
      // junk-only location (non-progression pool bulk — rupees, refills,
      // heart pieces — flows through the junk fill and IS allowed there).
      // is_progression_item is the placer's own classifier, so this cannot
      // drift from the fill's progression/junk split.
      for (uint16 i = 0; i < tt.count; i++) {
        const RandoLocationDef *loc = NULL;
        for (uint32 j = 0; j < kRandoLocationsCount; j++)
          if (kRandoLocations[j].id == tt.entries[i].location_id) { loc = &kRandoLocations[j]; break; }
        if (loc == NULL || !terrain_junk_only(loc, &sj)) continue;
        uint16 it = tt.entries[i].item_id;
        if (it == 0xFFFF) continue;  // sentinel = no placement
        if (is_progression_item(it))
          selfcheck_die("junk-only terrain location holds a progression item");
      }
    }
    fprintf(stderr, "[Placement_SelfCheck] terrain tiers OK (%u grass, %u rock)\n",
            (unsigned)grass_locs, (unsigned)rock_locs);
  }

  // Placement-side selfchecks for enemy-drop key activation and dungeon enemy rows.
  {
    uint32 enemy_locs = 0, enemy_key_locs = 0, enemy_bigkey_one_shots = 0;
    uint32 enemy_check_locs = 0, enemy_check_dungeon_locs = 0, enemy_check_all_only_locs = 0;
    uint32 enemy_check_indoor_all_only_locs = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      if (kRandoLocations[i].type == LOCTYPE_EnemyDrop) {
        enemy_locs++;
        if (is_small_key_item(kRandoLocations[i].vanilla_item_id)) enemy_key_locs++;
        else if (kRandoLocations[i].vanilla_item_id == ITEM_Nothing) enemy_bigkey_one_shots++;
      } else if (kRandoLocations[i].type == LOCTYPE_Enemy) {
        enemy_check_locs++;
        if (enemy_check_all_tier_only(kRandoLocations[i].id)) {
          enemy_check_all_only_locs++;
          if (enemy_check_indoor_all_tier_only(kRandoLocations[i].id))
            enemy_check_indoor_all_only_locs++;
        } else {
          enemy_check_dungeon_locs++;
        }
        if (kRandoLocations[i].vanilla_item_id != ITEM_Nothing)
          selfcheck_die("ordinary enemy checks must use ITEM_Nothing as vanilla filler");
      }
    }
    uint32 enemy_check_lookup_dungeon_locs = 0, enemy_check_lookup_all_only_locs = 0;
    for (uint32 i = 0; i < kRandoEnemyCheckLookup_COUNT; i++) {
      if (kRandoEnemyCheckLookup[i].all_tier_only)
        enemy_check_lookup_all_only_locs++;
      else
        enemy_check_lookup_dungeon_locs++;
    }
    bool has_enemy_drop_registry = kRandoEnemyDropLookup_COUNT != 0;
    bool has_enemy_check_registry = kRandoEnemyCheckLookup_COUNT != 0;
    uint32 enemy_check_lookup_boss_locs = kRandoBossEnemyCheckLookup_COUNT;
    uint32 enemy_check_lookup_scripted_locs = kRandoScriptedEnemyCheckLookup_COUNT;
    uint32 enemy_check_lookup_all_total =
        enemy_check_lookup_all_only_locs + kRandoOverworldEnemyCheckLookup_COUNT +
        enemy_check_lookup_boss_locs + enemy_check_lookup_scripted_locs;
    bool has_all_enemy_registry =
        enemy_check_lookup_all_total != 0;
    if (has_enemy_drop_registry != (enemy_locs != 0))
      selfcheck_die("enemy_drop_lookup/count drifted from LOCTYPE_EnemyDrop locations");
    if (has_enemy_check_registry !=
            (enemy_check_dungeon_locs + enemy_check_indoor_all_only_locs != 0) ||
        enemy_check_lookup_dungeon_locs != enemy_check_dungeon_locs ||
        enemy_check_lookup_all_only_locs != enemy_check_indoor_all_only_locs)
      selfcheck_die("enemy_check_lookup/count drifted from indoor LOCTYPE_Enemy locations");
    if (has_all_enemy_registry != (enemy_check_all_only_locs != 0) ||
        enemy_check_lookup_all_total != enemy_check_all_only_locs ||
        enemy_check_locs != enemy_check_dungeon_locs + enemy_check_all_only_locs)
      selfcheck_die("all-tier enemy_check_lookup/count drifted from Enemy locations");
    if (enemy_locs != enemy_key_locs + enemy_bigkey_one_shots)
      selfcheck_die("enemy-drop rows must be small keys or the one-shot big-key check");
    if (has_enemy_drop_registry && enemy_bigkey_one_shots != 1)
      selfcheck_die("expected exactly one one-shot enemy big-key check");

    RandoSettings so, sk;
    Settings_SetDefaults(&so);
    Settings_SetDefaults(&sk);
    so.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sk.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sk.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint32 n_off = 0, n_keys = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type != LOCTYPE_EnemyDrop) continue;
      n_off += enemy_drop_active(loc, &so) ? 1 : 0;
      n_keys += enemy_drop_active(loc, &sk) ? 1 : 0;
    }
    if (n_off != 0)
      selfcheck_die("enemy_drop_checks Off must activate zero enemy-drop rows");
    if (n_keys != enemy_locs)
      selfcheck_die("enemy_drop_checks Keys must activate exactly generated rows");
    RandoSettings seshuf = sk;
    seshuf.enemy_shuffle = 1;
    uint32 n_enemy_shuffle = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &seshuf))
        n_enemy_shuffle++;
    }
    if (n_enemy_shuffle != enemy_locs)
      selfcheck_die("enemy_drop_checks=Keys must compose with enemy_shuffle");
    RandoSettings sdun;
    Settings_SetDefaults(&sdun);
    sdun.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
    sdun.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint32 n_dun = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sdun))
        n_dun++;
    }
    if (n_dun != enemy_locs)
      selfcheck_die("enemy_drop_checks must activate for vanilla-door Dungeon small keys");
    RandoSettings sdoor = sdun;
    sdoor.door_shuffle = kDoorShuffle_Basic;
    uint32 n_door = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sdoor))
        n_door++;
    }
    if (n_door != enemy_locs)
      selfcheck_die("enemy_drop_checks must compose with door shuffle Dungeon keys");

    RandoSettings sdungeon = sk;
    sdungeon.enemy_drop_checks = kEnemyDropChecks_Dungeon;
    uint32 n_dungeon_forced = 0, n_dungeon_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sdungeon))
        n_dungeon_forced++;
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sdungeon))
        n_dungeon_ordinary++;
    }
    if (n_dungeon_forced != enemy_locs)
      selfcheck_die("enemy_drop_checks Dungeon must keep forced key-drop rows active");
    if (n_dungeon_ordinary != enemy_check_dungeon_locs)
      selfcheck_die("enemy_drop_checks Dungeon must activate exactly dungeon ordinary enemy rows");
    RandoSettings sall = sdungeon;
    sall.enemy_drop_checks = kEnemyDropChecks_All;
    uint32 n_all_forced = 0, n_all_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sall))
        n_all_forced++;
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sall))
        n_all_ordinary++;
    }
    uint32 enemy_check_locs_all_effective = n_all_ordinary;
    RandoSettings sall_standard = sall;
    sall_standard.world_state = kWorldState_Standard;
    uint32 n_all_standard_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sall_standard))
        n_all_standard_ordinary++;
    }
    if (n_all_forced != enemy_locs ||
        n_all_ordinary < enemy_check_dungeon_locs ||
        n_all_ordinary > enemy_check_locs ||
        n_all_standard_ordinary != enemy_check_locs)
      selfcheck_die("enemy_drop_checks All must include lower-tier enemy rows");
    if (!settings_need_all_enemy_registry(&sall))
      selfcheck_die("enemy_drop_checks All must require the all-enemy registry");
    if (has_all_enemy_registry != all_enemy_registry_available())
      selfcheck_die("all enemy registry availability drifted from generated rows");
    RandoSettings sdungeondoor = sdungeon;
    sdungeondoor.door_shuffle = kDoorShuffle_Basic;
    uint32 n_dungeon_door_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sdungeondoor))
        n_dungeon_door_ordinary++;
    }
    if (n_dungeon_door_ordinary != enemy_check_dungeon_locs ||
        Settings_EffectiveEnemyDropChecks(&sdungeondoor) != kEnemyDropChecks_Dungeon)
      selfcheck_die("door shuffle must preserve Dungeon ordinary enemy checks");
    RandoSettings salldoor = sall;
    salldoor.door_shuffle = kDoorShuffle_Basic;
    uint32 n_all_door_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &salldoor))
        n_all_door_ordinary++;
    }
    if (n_all_door_ordinary != enemy_check_locs_all_effective ||
        Settings_EffectiveEnemyDropChecks(&salldoor) != kEnemyDropChecks_All)
      selfcheck_die("door shuffle must preserve All ordinary enemy checks");
    RandoSettings sallboss = sall;
    sallboss.boss_shuffle = 1;
    uint32 n_all_boss_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sallboss))
        n_all_boss_ordinary++;
    }
    if (n_all_boss_ordinary != enemy_check_locs_all_effective ||
        Settings_EffectiveEnemyDropChecks(&sallboss) != kEnemyDropChecks_All)
      selfcheck_die("boss shuffle must preserve All ordinary enemy checks");
    RandoSettings sdungeonenemy = sdungeon;
    sdungeonenemy.enemy_shuffle = 1;
    uint32 n_dungeon_enemy_shuffle_forced = 0, n_dungeon_enemy_shuffle_ordinary = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->type == LOCTYPE_EnemyDrop && enemy_drop_active(loc, &sdungeonenemy))
        n_dungeon_enemy_shuffle_forced++;
      if (loc->type == LOCTYPE_Enemy && enemy_check_active(loc, &sdungeonenemy))
        n_dungeon_enemy_shuffle_ordinary++;
    }
    if (n_dungeon_enemy_shuffle_forced != enemy_locs ||
        n_dungeon_enemy_shuffle_ordinary != 0 ||
        Settings_EffectiveEnemyDropChecks(&sdungeonenemy) != kEnemyDropChecks_Keys)
      selfcheck_die("enemy shuffle must degrade Dungeon ordinary enemy checks to Keys");

    if (has_enemy_drop_registry) {
      RandoCounts seeded;
      memset(&seeded, 0, sizeof(seeded));
      seed_dungeon_free_key_drops(&seeded, &sdun);
      for (uint8 d = 0; d < 13; d++) {
        uint16 key = Rando_SmallKeyItemForRandoDungeon(d);
        uint16 enemies = 0;
        for (uint32 i = 0; i < kRandoLocationsCount; i++)
          if (kRandoLocations[i].type == LOCTYPE_EnemyDrop &&
              kRandoLocations[i].vanilla_item_id == key) enemies++;
        if (enemies > Rando_DropSmallKeyCount(d))
          selfcheck_die("enemy-drop key sources exceed door-rando drop total");
        // enemies == 0 dungeons (Swamp/Thieves'/Desert) still free-grant their
        // full drop total — their pots are inactive here and all drops fall
        // vanilla-free at runtime while the global ENEMY_DROP_KEYS_DUNGEON
        // min-depth gates stay live (the 42-check entrance strand fix).
        uint16 want = (uint16)(Rando_DropSmallKeyCount(d) - enemies);
        if (seeded.by_item_id[key] != want)
          selfcheck_die("Dungeon free-drop seeding drifted for enemy-only keys");
      }
      RandoSettings sboth = sdun;
      sboth.pot_shuffle = kPotShuffle_Keys;
      memset(&seeded, 0, sizeof(seeded));
      seed_dungeon_free_key_drops(&seeded, &sboth);
      for (uint8 d = 0; d < 13; d++) {
        uint16 key = Rando_SmallKeyItemForRandoDungeon(d);
        uint16 itemized = 0;
        for (uint32 i = 0; i < kRandoLocationsCount; i++) {
          const RandoLocationDef *loc = &kRandoLocations[i];
          if ((loc->type == LOCTYPE_Pot || loc->type == LOCTYPE_EnemyDrop) &&
              loc->vanilla_item_id == key)
            itemized++;
        }
        if (itemized > Rando_DropSmallKeyCount(d))
          selfcheck_die("combined key sources exceed door-rando drop total");
        // itemized == 0 asserts want == total (see the enemy-only note above).
        uint16 want = (uint16)(Rando_DropSmallKeyCount(d) - itemized);
        if (seeded.by_item_id[key] != want)
          selfcheck_die("Dungeon free-drop seeding drifted for pot+enemy keys");
      }
    }

    uint16 off_pool[kRandoLocationCapacity];
    uint16 n_pool_off = BuildItemPool(&so, 0, off_pool, kRandoLocationCapacity);
    if (n_pool_off == 0)
      selfcheck_die("enemy-drop Off pool unexpectedly failed");
    uint16 key_pool[kRandoLocationCapacity];
    uint16 n_pool_keys = BuildItemPool(&sk, 0, key_pool, kRandoLocationCapacity);
    if (!has_enemy_drop_registry) {
      if (n_pool_keys != 0)
        selfcheck_die("enemy_drop_checks must fail closed when the registry is absent");
    } else if (n_pool_keys != (uint16)(n_pool_off + enemy_locs)) {
      fprintf(stderr,
              "[Placement_SelfCheck] enemy-drop pool off=%u keys=%u locs=%u "
              "small=%u one_shot=%u\n",
              (unsigned)n_pool_off, (unsigned)n_pool_keys,
              (unsigned)enemy_locs, (unsigned)enemy_key_locs,
              (unsigned)enemy_bigkey_one_shots);
      selfcheck_die("enemy_drop_checks pool/slot count drift");
    }
    RandoSettings sdun_off = sdun;
    sdun_off.enemy_drop_checks = kEnemyDropChecks_Off;
    uint16 dun_off_pool[kRandoLocationCapacity];
    uint16 n_pool_dun_off = BuildItemPool(&sdun_off, 0, dun_off_pool, kRandoLocationCapacity);
    uint16 dun_key_pool[kRandoLocationCapacity];
    uint16 n_pool_dun_keys = BuildItemPool(&sdun, 0, dun_key_pool, kRandoLocationCapacity);
    if (has_enemy_drop_registry && n_pool_dun_keys != (uint16)(n_pool_dun_off + enemy_locs))
      selfcheck_die("enemy_drop_checks Dungeon pool/slot count drift");
    uint16 dungeon_pool[kRandoLocationCapacity];
    uint16 n_pool_dungeon = BuildItemPool(&sdungeon, 0, dungeon_pool, kRandoLocationCapacity);
    if (!has_enemy_check_registry) {
      if (n_pool_dungeon != 0)
        selfcheck_die("enemy_drop_checks Dungeon must fail closed when the ordinary enemy registry is absent");
    } else if (n_pool_dungeon != (uint16)(n_pool_keys + enemy_check_dungeon_locs)) {
      fprintf(stderr,
              "[Placement_SelfCheck] enemy dungeon pool keys=%u dungeon=%u ordinary=%u\n",
              (unsigned)n_pool_keys, (unsigned)n_pool_dungeon,
              (unsigned)enemy_check_dungeon_locs);
      selfcheck_die("enemy_drop_checks Dungeon pool/slot count drift");
    }
    uint16 all_pool[kRandoLocationCapacity];
    uint16 n_pool_all = BuildItemPool(&sall, 0, all_pool, kRandoLocationCapacity);
    if (!has_all_enemy_registry) {
      if (n_pool_all != 0)
        selfcheck_die("enemy_drop_checks All must fail closed when the all-tier registry is absent");
    } else if (n_pool_all != (uint16)(n_pool_keys + enemy_check_locs_all_effective)) {
      fprintf(stderr,
              "[Placement_SelfCheck] enemy all pool keys=%u all=%u ordinary=%u "
              "effective=%u dungeon=%u all_only=%u\n",
              (unsigned)n_pool_keys, (unsigned)n_pool_all,
              (unsigned)enemy_check_locs, (unsigned)enemy_check_locs_all_effective,
              (unsigned)enemy_check_dungeon_locs,
              (unsigned)enemy_check_all_only_locs);
      selfcheck_die("enemy_drop_checks All pool/slot count drift");
    }
    uint16 dungeon_door_pool[kRandoLocationCapacity];
    uint16 n_pool_dungeon_door = BuildItemPool(&sdungeondoor, 0, dungeon_door_pool, kRandoLocationCapacity);
    RandoSettings sdoor_keys = sdungeondoor;
    sdoor_keys.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint16 door_key_pool[kRandoLocationCapacity];
    uint16 n_pool_door_keys = BuildItemPool(&sdoor_keys, 0, door_key_pool, kRandoLocationCapacity);
    if (has_enemy_drop_registry &&
        n_pool_dungeon_door != (uint16)(n_pool_door_keys + enemy_check_dungeon_locs))
      selfcheck_die("door-shuffle Dungeon pool must include ordinary enemy checks");
    uint16 all_door_pool[kRandoLocationCapacity];
    uint16 n_pool_all_door = BuildItemPool(&salldoor, 0, all_door_pool, kRandoLocationCapacity);
    if (has_all_enemy_registry &&
        n_pool_all_door != (uint16)(n_pool_door_keys + enemy_check_locs_all_effective))
      selfcheck_die("door-shuffle All pool must include ordinary enemy checks");
    uint16 all_boss_pool[kRandoLocationCapacity];
    uint16 n_pool_all_boss = BuildItemPool(&sallboss, 0, all_boss_pool, kRandoLocationCapacity);
    if (has_all_enemy_registry && n_pool_all_boss != n_pool_all)
      selfcheck_die("boss-shuffle All pool must preserve ordinary enemy checks");
  }

  // add-enemy-souls — souls pool tiers, the door-shuffle degrade, and the
  // enemies-tier fail-closed gate (kRandoSoulRoomsBaked). Counts souls by id
  // range so a pool-construction regression can't slip past the junk-pad
  // (souls displace junk, keeping the total constant).
  {
    uint16 pool[kRandoLocationCapacity];
    RandoSettings sb;
    Settings_SetDefaults(&sb);
    sb.souls_shuffle = kSoulsShuffle_Bosses;
    uint16 nb = BuildItemPool(&sb, 0, pool, kRandoLocationCapacity);
    uint16 souls_in_pool = 0;
    for (uint16 i = 0; i < nb; i++)
      if (Souls_ItemIsSoul(pool[i])) souls_in_pool++;
    if (souls_in_pool != kSoulBossCount)
      selfcheck_die("souls bosses tier pool must hold exactly the boss souls");
    RandoSettings sa = sb;
    sa.souls_shuffle = kSoulsShuffle_BossesEnemies;
    uint16 na = BuildItemPool(&sa, 0, pool, kRandoLocationCapacity);
    souls_in_pool = 0;
    for (uint16 i = 0; i < na; i++)
      if (Souls_ItemIsSoul(pool[i])) souls_in_pool++;
    if (kRandoSoulRoomsBaked) {
      if (souls_in_pool != kSoulCount)
        selfcheck_die("souls enemies tier pool must hold every soul");
    } else if (na != 0) {
      selfcheck_die("souls_shuffle=all must fail closed without soul_rooms.gen.yaml");
    }
    // Door shuffle degrades souls to OFF at any tier (species-blind door
    // oracle) — the pool must follow Settings_EffectiveSoulsShuffle.
    RandoSettings sd = sa;
    sd.door_shuffle = kDoorShuffle_Basic;
    uint16 nd = BuildItemPool(&sd, 0, pool, kRandoLocationCapacity);
    souls_in_pool = 0;
    for (uint16 i = 0; i < nd; i++)
      if (Souls_ItemIsSoul(pool[i])) souls_in_pool++;
    if (souls_in_pool != 0)
      selfcheck_die("souls must degrade to off under door shuffle");
    // add-npc-souls — kNpcSoulCount NPC souls when on (0 when off is covered
    // by every other arm above: none of them set npc_souls). Independent of
    // the tier AND of door shuffle (no degrade). Also: worst-case progression
    // count must fit the placer's fixed arrays (progression[256]/prog_slot[256]).
    RandoSettings snp;
    Settings_SetDefaults(&snp);
    snp.npc_souls = 1;
    uint16 nn = BuildItemPool(&snp, 0, pool, kRandoLocationCapacity);
    uint16 npc_in_pool = 0;
    for (uint16 i = 0; i < nn; i++)
      if (pool[i] >= ITEM_Soul_Npc_Sahasrahla &&
          pool[i] <= ITEM_Soul_Npc_Uncle) npc_in_pool++;
    if (npc_in_pool != kNpcSoulCount)
      selfcheck_die("npc_souls pool must hold exactly kNpcSoulCount souls");
    snp.door_shuffle = kDoorShuffle_Basic;
    nn = BuildItemPool(&snp, 0, pool, kRandoLocationCapacity);
    npc_in_pool = 0;
    for (uint16 i = 0; i < nn; i++)
      if (pool[i] >= ITEM_Soul_Npc_Sahasrahla &&
          pool[i] <= ITEM_Soul_Npc_Uncle) npc_in_pool++;
    if (npc_in_pool != kNpcSoulCount)
      selfcheck_die("npc_souls must NOT degrade under door shuffle");
    // Worst-case progression head-count: wild keys + hunt pieces + souls-all
    // + npc souls must fit progression[256]/prog_slot[256].
    RandoSettings sw;
    Settings_SetDefaults(&sw);
    sw.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sw.dungeon_big_keys_mode = kDungeonItemMode_Wild;
    sw.goal = kGoal_TriforceHunt;
    sw.accessibility = kAccessibility_None;
    sw.souls_shuffle = kSoulsShuffle_BossesEnemies;
    sw.npc_souls = 1;
    uint16 nw = BuildItemPool(&sw, 0, pool, kRandoLocationCapacity);
    uint16 prog_count = 0;
    for (uint16 i = 0; i < nw; i++)
      if (is_progression_item(pool[i])) prog_count++;
    if (prog_count > 256)
      selfcheck_die("worst-case progression pool exceeds progression[256]");
  }

  // BuildItemPool refuses pieces_required > pieces_placed for
  // Triforce/Ganon-Hunt goals.
  {
    RandoSettings s;
    Settings_SetDefaults(&s);
    s.goal = kGoal_TriforceHunt;
    s.pieces_required = 30;
    s.pieces_placed = 20;
    uint16 pool[kRandoLocationCapacity];
    uint16 n = BuildItemPool(&s, 0, pool, kRandoLocationCapacity);
    if (n != 0) selfcheck_die("BuildItemPool should reject pieces_required > pieces_placed");
    // GanonHunt should validate the same way.
    s.goal = kGoal_GanonHunt;
    n = BuildItemPool(&s, 0, pool, kRandoLocationCapacity);
    if (n != 0) selfcheck_die("BuildItemPool should reject GanonHunt pieces_required > pieces_placed");
    // Equal counts is allowed.
    s.pieces_required = 20;
    s.pieces_placed = 20;
    n = BuildItemPool(&s, 0, pool, kRandoLocationCapacity);
    if (n == 0) selfcheck_die("BuildItemPool should allow pieces_required == pieces_placed");
  }

  // Settings_CanonicalSerialize normalizes completionist→locations on a
  // private copy, so any direct-API user gets the spec-compliant hash.
  {
    RandoSettings a, b;
    Settings_SetDefaults(&a);
    a.goal = kGoal_Completionist;
    a.accessibility = kAccessibility_Items;     // "wrong" — should normalize
    Settings_SetDefaults(&b);
    b.goal = kGoal_Completionist;
    b.accessibility = kAccessibility_Locations; // "right"
    uint8 hash_a[32], hash_b[32];
    Settings_ComputeHash(&a, hash_a);
    Settings_ComputeHash(&b, hash_b);
    if (memcmp(hash_a, hash_b, 32) != 0) {
      selfcheck_die("Settings_ComputeHash should normalize completionist→accessibility=locations");
    }
  }

  // All-tier enemy checks include overworld stage windows, so Locations
  // accessibility is rejected. Completionist forces Locations even if the raw
  // accessibility byte says something else; guard the effective value.
  {
    RandoSettings s;
    Settings_SetDefaults(&s);
    s.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    s.enemy_drop_checks = kEnemyDropChecks_All;
    s.accessibility = kAccessibility_Items;
    s.goal = kGoal_Completionist;
    char err[192];
    if (Placement_PreflightSettings(&s, err, sizeof(err)))
      selfcheck_die("enemy_drop_checks=All + Completionist must fail preflight");
    uint16 pool[kRandoLocationCapacity];
    if (BuildItemPool(&s, 0, pool, kRandoLocationCapacity) != 0)
      selfcheck_die("enemy_drop_checks=All + Completionist must fail BuildItemPool");
  }

  // Rando_RandoDungeonFromDungeonItem mapping for the keys-skip-HCT enums.
  // Pins the lookup table so a future formula-based regression breaks the
  // selftest before a corpus run.
  {
    // Small keys (53..65) are contiguous HCE..GT (HCT included).
    if (Rando_RandoDungeonFromDungeonItem(53) != 0)  selfcheck_die("SmallKey_HCE → 0");
    if (Rando_RandoDungeonFromDungeonItem(57) != 4)  selfcheck_die("SmallKey_HCT → 4");
    if (Rando_RandoDungeonFromDungeonItem(58) != 5)  selfcheck_die("SmallKey_PoD → 5");
    if (Rando_RandoDungeonFromDungeonItem(65) != 12) selfcheck_die("SmallKey_GT → 12");
    // BigKey ids 66..76 skip HCE AND HCT.
    if (Rando_RandoDungeonFromDungeonItem(66) != 1)  selfcheck_die("BigKey_EP → 1");
    if (Rando_RandoDungeonFromDungeonItem(68) != 3)  selfcheck_die("BigKey_TH → 3");
    if (Rando_RandoDungeonFromDungeonItem(69) != 5)  selfcheck_die("BigKey_PoD → 5 (HCT skip)");
    if (Rando_RandoDungeonFromDungeonItem(76) != 12) selfcheck_die("BigKey_GT → 12");
    // Map_HCE = 124 → 0; Map ids 77..87 skip HCT.
    if (Rando_RandoDungeonFromDungeonItem(124) != 0) selfcheck_die("Map_HCE → 0");
    if (Rando_RandoDungeonFromDungeonItem(77) != 1)  selfcheck_die("Map_EP → 1");
    if (Rando_RandoDungeonFromDungeonItem(80) != 5)  selfcheck_die("Map_PoD → 5 (HCT skip)");
    if (Rando_RandoDungeonFromDungeonItem(87) != 12) selfcheck_die("Map_GT → 12");
    // Compass ids 88..98 skip HCE AND HCT.
    if (Rando_RandoDungeonFromDungeonItem(88) != 1)  selfcheck_die("Compass_EP → 1");
    if (Rando_RandoDungeonFromDungeonItem(91) != 5)  selfcheck_die("Compass_PoD → 5 (HCT skip)");
    if (Rando_RandoDungeonFromDungeonItem(98) != 12) selfcheck_die("Compass_GT → 12");
    // Non-dungeon items return 0xFF.
    if (Rando_RandoDungeonFromDungeonItem(0) != 0xFF)   selfcheck_die("ProgressiveSword → 0xFF");
    if (Rando_RandoDungeonFromDungeonItem(100) != 0xFF) selfcheck_die("Rupee20 → 0xFF");
  }

  // Phase B Slice 3b — Retro TakeAny selection invariants. Pins the per-seed
  // activation model (exactly 4 potion caves + 1 weapon cave = 9 active slots;
  // deterministic per seed; inactive outside Retro; role→reward mapping) so a
  // future placer/RNG change trips the selftest before a corpus regen.
  {
    RandoSettings s;
    Settings_SetDefaults(&s);
    s.world_state = kWorldState_Retro;  // mode_weapons defaults to Randomized → ProgressiveSword
    uint8 roles[kTakeAnyCaveCount], roles2[kTakeAnyCaveCount];
    takeany_select(&s, 0x1234ull, roles);
    takeany_select(&s, 0x1234ull, roles2);
    uint8 potion = 0, weapon = 0, active_slots = 0;
    for (uint8 cave = 0; cave < kTakeAnyCaveCount; cave++) {
      if (roles[cave] != roles2[cave])
        selfcheck_die("TakeAny selection not deterministic for a fixed seed");
      uint16 r0 = takeany_reward(&s, roles, (uint16)(kTakeAnyLocBase + 2 * cave));
      uint16 r1 = takeany_reward(&s, roles, (uint16)(kTakeAnyLocBase + 2 * cave + 1));
      if (roles[cave] == kTakeAnyRole_Potion) {
        potion++;
        active_slots += 2;
        if (r0 != ID_BluePotion || r1 != ID_BossHeartContainer)
          selfcheck_die("TakeAny potion cave reward mapping wrong (expect BluePotion@0 + BossHeart@1)");
      } else if (roles[cave] == kTakeAnyRole_Weapon) {
        weapon++;
        active_slots += 1;
        if (r0 != ID_ProgressiveSword || r1 != 0xFFFF)
          selfcheck_die("TakeAny weapon cave reward mapping wrong (expect ProgressiveSword@0, no slot 1)");
      } else if (r0 != 0xFFFF || r1 != 0xFFFF) {
        selfcheck_die("TakeAny inactive cave must yield no reward");
      }
    }
    if (potion != 4)       selfcheck_die("TakeAny must activate exactly 4 potion caves");
    if (weapon != 1)       selfcheck_die("TakeAny must activate exactly 1 weapon cave");
    if (active_slots != 9) selfcheck_die("TakeAny must emit exactly 9 active slots per seed");

    // Outside Retro, no cave activates (Open default world-state).
    RandoSettings open;
    Settings_SetDefaults(&open);
    uint8 oroles[kTakeAnyCaveCount];
    takeany_select(&open, 0x1234ull, oroles);
    for (uint8 cave = 0; cave < kTakeAnyCaveCount; cave++)
      if (oroles[cave] != kTakeAnyRole_Inactive)
        selfcheck_die("TakeAny must be inactive outside Retro world-state");
  }

  // Accessibility tier discrimination (ALTTPR three-way). Exercises the pure
  // tier rule with synthetic spheres so the nesting locations ⊇ items ⊇
  // beatable is pinned independent of the logic graph. Placement holds one
  // progression item (id 10, a weapon ≤ 40) and one junk item (id 105).
  {
    RandoPlacement acc_entries[2] = {
      { 100, 10 },   // progression item
      { 200, 105 },  // junk item (104..110 → non-progression)
    };
    RandoPlacementTable acc_t = { acc_entries, 2 };
    RandoSettings acc_s;
    Settings_SetDefaults(&acc_s);

    // Case 1 — only JUNK stranded.
    RandoSpheres sp;
    memset(&sp, 0, sizeof(sp));  // 0 == sphere 0 (reachable)
    sp.sphere_index_by_placement[1] = kSphereIndexUnreachable;
    sp.unreachable_count = 1;
    acc_s.accessibility = kAccessibility_Locations;
    if (accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("locations must reject a stranded (junk) location");
    acc_s.accessibility = kAccessibility_Items;
    if (!accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("items must accept a stranded JUNK item");
    acc_s.accessibility = kAccessibility_None;
    if (!accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("beatable must accept a stranded junk item");

    // Case 2 — PROGRESSION stranded.
    memset(&sp, 0, sizeof(sp));
    sp.sphere_index_by_placement[0] = kSphereIndexUnreachable;  // progression
    sp.unreachable_count = 1;
    acc_s.accessibility = kAccessibility_Locations;
    if (accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("locations must reject a stranded progression item");
    acc_s.accessibility = kAccessibility_Items;
    if (accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("items must REJECT a stranded PROGRESSION item");
    acc_s.accessibility = kAccessibility_None;
    if (!accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("beatable must accept a stranded progression item");

    // Case 3 — fully reachable: every tier accepts.
    memset(&sp, 0, sizeof(sp));
    sp.unreachable_count = 0;
    for (uint8 a = kAccessibility_Items; a <= kAccessibility_None; a++) {
      acc_s.accessibility = a;
      if (!accessibility_reachability_ok(&acc_s, &acc_t, &sp))
        selfcheck_die("every tier must accept a fully-reachable placement");
    }

    // Case 4 — audit sibling (accept-bad-seed): goal==Completionist forces
    // EFFECTIVE Locations even when the raw accessibility is none, so a stranded
    // location is REJECTED. Guards the bug where the placer read raw accessibility
    // and `goal=completionist,accessibility=none` skipped the sphere walk the
    // canonical hash promises. The corpus is structurally blind to this (the
    // placement digest is identical for any ACCEPTING seed), so it lives here.
    acc_s.goal = kGoal_Completionist;
    acc_s.accessibility = kAccessibility_None;  // raw "beatable only" — must be overridden
    memset(&sp, 0, sizeof(sp));
    sp.sphere_index_by_placement[1] = kSphereIndexUnreachable;  // strand a (junk) location
    sp.unreachable_count = 1;
    if (accessibility_reachability_ok(&acc_s, &acc_t, &sp))
      selfcheck_die("Completionist must enforce Locations even with raw accessibility=none");
  }

  // Medallion config slots choose the MM/TR entrance requirements; they are not
  // collectible item checks and must not contribute Ether/Quake/Bombos to
  // sphere-walked inventory or the accessibility=items tier.
  {
    RandoPlacement med_entries[2] = {
      { LOC_Misery_Mire_Medallion, ID_Ether },
      { LOC_Sewers_Secret_Room_Left, ID_Hookshot },
    };
    RandoPlacementTable med_t = { med_entries, 2 };
    if (placement_entry_grants_item(&med_t, 0))
      selfcheck_die("Medallion config slot must not grant an item");
    if (!placement_entry_grants_item(&med_t, 1))
      selfcheck_die("normal item location must grant its item");

    RandoSettings med_s;
    Settings_SetDefaults(&med_s);
    RandoSpheres med_sp;
    memset(&med_sp, 0, sizeof(med_sp));
    med_sp.sphere_index_by_placement[0] = kSphereIndexUnreachable;
    med_sp.unreachable_count = 1;
    med_s.accessibility = kAccessibility_Items;
    if (!accessibility_reachability_ok(&med_s, &med_t, &med_sp))
      selfcheck_die("items accessibility must ignore stranded medallion config slots");
  }

  // Skeleton Key end-to-end logic neutrality. Generate a real enabled seed,
  // replace its one Skeleton Key with the no-op item at the same location, and
  // prove sphere assignment plus goal certification are byte-identical.
  {
    static RandoPlacement sk_entries[kRandoLocationCapacity];
    RandoSettings sks;
    Settings_SetDefaults(&sks);
    sks.skeleton_key = 1;
    sks.goal = kGoal_Ganon;
    RandoPlacementTable skt = { sk_entries, 0 };
    if (!Place_AssumedFill(&sks, 0x4B4559520000000Dull, 0, &skt))
      selfcheck_die("Skeleton Key neutrality: Place_AssumedFill failed");
    if (!Goal_IsCompletable(&sks, 0x4B4559520000000Dull, &skt))
      selfcheck_die("Skeleton Key neutrality: enabled seed is not completable");
    RandoSpheres with_sk, without_sk;
    Logic_ComputeSpheres(&sks, &skt, &with_sk);
    uint16 skeleton_count = 0;
    for (uint16 i = 0; i < skt.count; i++) {
      if (skt.entries[i].item_id == ITEM_SkeletonKey) {
        skt.entries[i].item_id = ITEM_Nothing;
        skeleton_count++;
      }
    }
    if (skeleton_count != 1)
      selfcheck_die("Skeleton Key neutrality: expected exactly one placement");
    if (!Goal_IsCompletable(&sks, 0x4B4559520000000Dull, &skt))
      selfcheck_die("Skeleton Key neutrality: removal changed goal completion");
    Logic_ComputeSpheres(&sks, &skt, &without_sk);
    if (with_sk.max_sphere != without_sk.max_sphere ||
        with_sk.unreachable_count != without_sk.unreachable_count ||
        memcmp(with_sk.sphere_index_by_placement,
               without_sk.sphere_index_by_placement, skt.count) != 0)
      selfcheck_die("Skeleton Key neutrality: removal changed sphere assignment");
  }

  // Retro genericKeys end-to-end: generate full Retro seeds and assert the
  // shared-pool key routing never strands the goal. For each seed: the goal is
  // completable, there are zero unreachable placements, the pool holds the
  // fungible GenericKey, and NO per-dungeon SmallKey (53-65) leaked into the
  // placement (every key — including the Swamp Palace Entrance forced key — is
  // generic). This is the headless guard for task 3.3; the in-game playtest
  // remains the real acceptance gate for key-strand beatability.
  {
    static RandoPlacement gk_entries[kRandoLocationCapacity];
    static const uint64 kGkSeeds[3] = {
      0x00000000000000AFull,  // the hard-pool corpus seed's mnemonic
      0x0000000000003039ull,  // 12345
      0xC0FFEE0000000044ull,
    };
    for (uint8 si = 0; si < 3; si++) {
      RandoSettings gks;
      Settings_SetDefaults(&gks);
      gks.world_state = kWorldState_Retro;
      gks.goal = kGoal_Ganon;  // needs the 7 crystal dungeons + GT (key-heavy)
      RandoPlacementTable gkt = { gk_entries, 0 };
      if (!Place_AssumedFill(&gks, kGkSeeds[si], 0, &gkt))
        selfcheck_die("Retro genericKeys: Place_AssumedFill produced no placement");
      if (!Goal_IsCompletable(&gks, kGkSeeds[si], &gkt))
        selfcheck_die("Retro genericKeys: goal not completable (key strand?)");
      RandoSpheres gsp;
      Logic_ComputeSpheres(&gks, &gkt, &gsp);
      if (gsp.unreachable_count != 0)
        selfcheck_die("Retro genericKeys: unreachable placement(s) (key strand?)");
      uint16 generic = 0, leaked = 0;
      for (uint16 e = 0; e < gkt.count; e++) {
        uint16 it = gkt.entries[e].item_id;
        if (it == ID_GenericKey) generic++;
        else if (it >= ID_SmallKey_HCE && it <= ID_SmallKey_GT) leaked++;
      }
      if (generic == 0)
        selfcheck_die("Retro genericKeys: no GenericKey in the placement");
      if (leaked != 0)
        selfcheck_die("Retro genericKeys: a per-dungeon SmallKey leaked into a Retro seed");
    }
  }

  // add-rando-shopsanity — full generation net for the open shop slots.
  // One seed per world state (the axis composes with all four): assumed fill
  // completes, the goal is completable, spheres leave 0 unreachable at the
  // default items tier (the 27 slots carry real region gates from
  // logic_parts/50_shops.yaml — a broken predicate would strand them), and
  // every one of the 27 shop slots emits exactly one placement entry.
  {
    static RandoPlacement shop_entries[kRandoLocationCapacity];
    static const uint8 kShopWorldStates[4] = {
      kWorldState_Open, kWorldState_Standard, kWorldState_Inverted,
      kWorldState_Retro,
    };
    for (uint8 wi = 0; wi < 4; wi++) {
      RandoSettings shs;
      Settings_SetDefaults(&shs);
      shs.world_state = kShopWorldStates[wi];
      shs.shopsanity = 1;
      RandoPlacementTable sht = { shop_entries, 0 };
      if (!Place_AssumedFill(&shs, 0x53686F7053414Eull, 0, &sht))
        selfcheck_die("shopsanity: Place_AssumedFill produced no placement");
      if (!Goal_IsCompletable(&shs, 0x53686F7053414Eull, &sht))
        selfcheck_die("shopsanity: goal not completable");
      RandoSpheres ssp;
      Logic_ComputeSpheres(&shs, &sht, &ssp);
      if (ssp.unreachable_count != 0)
        selfcheck_die("shopsanity: unreachable placement(s)");
      uint16 shop_rows = 0;
      for (uint32 i = 0; i < kRandoLocationsCount; i++) {
        if (kRandoLocations[i].type != LOCTYPE_Shop) continue;
        for (uint16 e = 0; e < sht.count; e++) {
          if (sht.entries[e].location_id == kRandoLocations[i].id) { shop_rows++; break; }
        }
      }
      if (shop_rows != 27)
        selfcheck_die("shopsanity: expected all 27 shop slots in the placement");
    }
  }

  // Boss-shuffle reachability (add-rando-shuffles-and-minigames boss-runtime,
  // workstream 2). The ONLY automated net for the boss-shuffle strand bug: with
  // boss_shuffle on, each dungeon's `- Boss`/`- Prize` gates on OP_CAN_KILL_BOSS
  // (the SHUFFLED boss's kill predicate). Generate boss_shuffle=1 seeds across
  // goals/world-states that REQUIRE the dungeon prizes (dungeons / ganon force
  // every crystal+pendant dungeon reachable) and assert the generator honors the
  // shuffled boss-kill requirements without stranding: goal completable + zero
  // unreachable placements. A non-identity assignment must actually be installed
  // (else the test trivially passes against vanilla). Sprite substitution stays
  // runtime-off; this validates the LOGIC half (the half that can strand).
  {
    static RandoPlacement bs_entries[kRandoLocationCapacity];
    struct { uint8 ws; uint8 goal; uint64 seed; } kBsCases[] = {
      { kWorldState_Open,     kGoal_Dungeons,  0x00000000B0550001ull },
      { kWorldState_Standard, kGoal_Ganon,     0x00000000B0550002ull },
      { kWorldState_Open,     kGoal_FastGanon, 0x00000000B0550003ull },
      { kWorldState_Inverted, kGoal_Ganon,     0x00000000B0550004ull },
      { kWorldState_Standard, kGoal_Dungeons,  0x00000000B0550005ull },
    };
    const uint8 nbs = (uint8)(sizeof(kBsCases) / sizeof(kBsCases[0]));
    for (uint8 ci = 0; ci < nbs; ci++) {
      RandoSettings bss;
      Settings_SetDefaults(&bss);
      bss.world_state = kBsCases[ci].ws;
      bss.goal = kBsCases[ci].goal;
      bss.boss_shuffle = 1;
      RandoPlacementTable bst = { bs_entries, 0 };
      if (!Place_AssumedFill(&bss, kBsCases[ci].seed, 0, &bst))
        selfcheck_die("boss shuffle: Place_AssumedFill produced no placement");
      // The boss assignment the placer installed must be a real (non-identity)
      // shuffle, so the boss-kill gate is genuinely exercised. Check only the 7
      // SHUFFLEABLE dungeons — HCE (0) is always 0xFF, and the pinned slots
      // (Agahnim 4/12, Blind 8, Kholdstare 9, Trinexx 11) always equal vanilla, so
      // including them would add dead entries that can never satisfy non_identity.
      const uint8 *ba = Rando_GetBossAssignment();
      if (ba == NULL) selfcheck_die("boss shuffle: assignment not installed by placer");
      static const uint8 kShufDng[7] = {1, 2, 3, 5, 6, 7, 10};
      static const uint8 kVanBoss[13] = {0xFF, 0,1,2,3,4,5,6,7,8,9,10,11};
      bool non_identity = false;
      for (uint8 i = 0; i < 7; i++)
        if (ba[kShufDng[i]] != kVanBoss[kShufDng[i]]) non_identity = true;
      if (!non_identity)
        selfcheck_die("boss shuffle: installed assignment is the identity (test not exercising the shuffle)");
      if (!Goal_IsCompletable(&bss, kBsCases[ci].seed, &bst))
        selfcheck_die("boss shuffle: goal not completable (boss-kill strand?)");
      RandoSpheres bsp;
      Logic_ComputeSpheres(&bss, &bst, &bsp);
      if (bsp.unreachable_count != 0)
        selfcheck_die("boss shuffle: unreachable placement(s) (boss-kill strand?)");
    }
    // Restore the vanilla-identity assignment so later self-checks / callers
    // don't observe a stale boss-on assignment (Place_AssumedFill installs per
    // call, but be explicit — the next non-boss Place_AssumedFill overwrites it
    // anyway with the vanilla identity for boss_shuffle=0).
    Rando_SetBossAssignment(NULL);
  }

  fprintf(stderr, "[Placement_SelfCheck] OK\n");
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_placement)
