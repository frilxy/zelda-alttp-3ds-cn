// rando_settings.c — RandoSettings struct + canonical serialization
// (tasks.md §2.5).
//
// The canonical layout is pinned below. Reordering, widening, or renumbering
// any field is a kGeneratorVersion bump trigger (tasks.md §13.6).
//
// Layout (kSettingsCanonicalLen = 31 bytes):
//   offset 0   world_state                 WorldState enum
//   offset 1   goal                        Goal enum
//   offset 2   crystals_ganon              0..7 or 8=random (add-rando-random-crystals)
//   offset 3   crystals_tower              0..7 or 8=random
//   offset 4   tricks                      uint8 bitmask (Phase A: 0)
//   offset 5   item_pool_difficulty        ItemPoolDifficulty enum
//   offset 6   logic                       uint8 (Phase A: 0)
//   offset 7   mode_weapons                ModeWeapons enum
//   offset 8   accessibility               Accessibility enum
//   offset 9   pyramid_bow_upgrade         legacy PyramidBowUpgrade enum (0)
//   offset 10  region_boss_hearts_in_pool  legacy bool (canonicalized to 0)
//   offset 11  dungeon_small_keys_mode     DungeonItemMode enum
//   offset 12  dungeon_big_keys_mode       DungeonItemMode enum
//   offset 13  dungeon_maps_mode           DungeonItemMode enum
//   offset 14  dungeon_compasses_mode      DungeonItemMode enum
//   offset 15  prize_shuffle               bool
//   offset 16  medallion_shuffle           bool
//   offset 17  race_mode                   bool
//   offset 18  pieces_required (LE lo)
//   offset 19  pieces_required (LE hi)
//   offset 20  pieces_placed   (LE lo)
//   offset 21  pieces_placed   (LE hi)
//   offset 22  hints                       Off/Balanced  (§66, kGenVer 13→14)
//   offset 23  boss_shuffle                bool          (§66)
//   offset 24  drop_shuffle                bool          (§66)
//   offset 25  entrance_axes (bit-packed)  Phase C — bit0 shuffle_caves,
//                                          bit1 shuffle_dungeons, bit2 coupled,
//                                          bit3 cross_category, bit4 decoupled,
//                                          bit5 shuffle_gt, bit6 dungeon_chains.
//                                          bit7 hint_npc_reward_reveal.
//                                          0x00 for the default (no shuffle).
//   offset 26  misc axes (bit-packed)      bit0 enemy_shuffle, bit1
//                                          customizer_active, bits2-3 traps,
//                                          bit4 manual flute activation
//                                          (inverse of instant_flute).
//                                          0x00 for the default.
//   offset 27  door_shuffle                bits0-1
//   offset 28  enemy_drop_checks           bits0-1 (EnemyDropChecks enum)
//                                          bits2-3 souls_shuffle (SoulsShuffle
//                                          enum, add-enemy-souls)
//                                          bit4 npc_souls
//                                          bits5-6 hint tile coverage
//                                          bit7 hint mix low bit
//   offset 29  grass_shuffle               bits0-1 (TerrainShuffle enum)
//              rock_shuffle                bits2-3 (TerrainShuffle enum,
//                                          add-rando-grass-rock-shuffle)
//              shopsanity                  bit4 (bool, add-rando-shopsanity)
//              bonk_shuffle                bits5-6 (TerrainShuffle, add-rando-bonk-sanity)
//              hint mix                    bit7 (high bit)
//   offset 30  key_rings                   bits0-1 (KeyRingsMode enum)
//              skeleton_key                bit2 (bonus-only item toggle)
//              whirlpool/flute shuffle      bits3-5
//              hint paid depth wire code    bits6-7
//
// settings_version is NOT serialized — it's a runtime constant pinned to 1
// for Phase A. Bumping the layout requires kGeneratorVersion increment.

#include <string.h>  // memset (used below at Settings_CanonicalDeserialize)
#include "rando_settings.h"
#include "rando_hints.h"  // kHintsMode_Off / kHintsMode_Balanced
#include "../types.h"
#include "third_party/sha256/sha256.h"

void Settings_SetDefaults(RandoSettings *s) {
  s->settings_version = 1;
  s->world_state = kWorldState_Open;
  s->goal = kGoal_FastGanon;
  s->crystals_ganon = 7;
  s->crystals_tower = 7;
  s->tricks = 0;                            // Phase A pinned to none
  s->item_pool_difficulty = kItemPoolDifficulty_Normal;
  s->logic = 0;                             // Phase A pinned to NoGlitches
  s->mode_weapons = kModeWeapons_Randomized;
  s->accessibility = kAccessibility_Items;
  s->pyramid_bow_upgrade = kPyramidBowUpgrade_Silvers;  // legacy no-op
  s->region_boss_hearts_in_pool = 0;        // boss hearts shuffle by default
  s->dungeon_small_keys_mode = kDungeonItemMode_Vanilla;
  s->dungeon_big_keys_mode = kDungeonItemMode_Vanilla;
  s->dungeon_maps_mode = kDungeonItemMode_Vanilla;
  s->dungeon_compasses_mode = kDungeonItemMode_Vanilla;
  s->prize_shuffle = 1;
  s->medallion_shuffle = 1;
  s->race_mode = 0;
  s->pieces_required = 20;
  s->pieces_placed = 30;
  // Hints plus the Phase B shuffle axes are included in canonical serialization
  // starting at kGeneratorVersion 14. Hints default Balanced (the intended
  // out-of-the-box semantic plan and discovery journal);
  // the shuffle axes default off.
  s->hints = kHintsMode_Balanced;
  s->hint_tile_coverage = kHintTileCoverage_Full;
  s->hint_mix = kHintMix_Variety;
  s->hint_paid_depth = 3;
  s->hint_npc_reward_reveal = 0;
  s->boss_shuffle = 0;
  s->drop_shuffle = 0;
  // Phase C — entrance shuffle. All shuffle axes default OFF; `coupled`
  // defaults ON (the ALTTPR baseline, so enabling cave shuffle is coupled
  // unless the user opts into decoupled). With no shuffle active,
  // apply_derived_rules() normalizes coupled→0 for serialization, so the
  // default canonical byte [25] is 0x00 (corpus byte-identical).
  s->shuffle_cave_entrances = 0;
  s->shuffle_dungeon_entrances = 0;
  s->coupled = 1;
  s->cross_category = 0;
  s->decoupled = 0;
  s->shuffle_ganons_tower_entrance = 0;  // advanced opt-in (off by default)
  // dungeon-chains — boss-chain topology off by default, so canonical [25] bit6
  // stays 0 (corpus + default settings_hash byte-identical).
  s->dungeon_chains = 0;
  // add-rando-enemy-shuffle — enemy (sprite-type) substitution. Default OFF, so
  // the packed pad byte [26] is 0x00 (corpus byte-identical).
  s->enemy_shuffle = 0;
  // add-rando-door-shuffle — intra-dungeon door shuffle. Default vanilla, so
  // the packed pad byte [27] is 0x00 (corpus byte-identical).
  s->door_shuffle = kDoorShuffle_Vanilla;
  // add-rando-customizer-mode — manual placement off by default, so [26] bit1
  // stays 0 (corpus + default settings_hash byte-identical).
  s->customizer_active = 0;
  // add-rando-traps — trap frequency off by default, so [26] bits2-3 stay 0
  // (corpus + default settings_hash byte-identical).
  s->traps = kTrapFrequency_Off;
  // Randomizer QoL — default ON but serialized with an inverse bit so the
  // default canonical byte [26] stays 0. instant_flute=0 restores the old
  // "play it for the bird" activation route.
  s->instant_flute = 1;
  // add-rando-trap-catalog — zero mask = "all categories" when traps are on, so the
  // default canonical byte [27] stays 0 (corpus + default settings_hash byte-identical).
  s->trap_categories = 0;
  // add-rando-pot-sanity — pot shuffle off by default, so the packed bits in
  // canonical [26] (6-7) and [27] (bit 7) stay 0 (corpus + default settings_hash
  // byte-identical until the next append-only canonical growth).
  s->pot_shuffle = kPotShuffle_Off;
  // add-rando-enemy-drop-sanity — enemy drops are not locations by default.
  s->enemy_drop_checks = kEnemyDropChecks_Off;
  s->grass_shuffle = kTerrainShuffle_Off;  // add-rando-grass-rock-shuffle
  s->rock_shuffle = kTerrainShuffle_Off;
  // add-enemy-souls — souls off by default ([28] bits 2-3 stay 0; corpus +
  // default settings_hash byte-identical).
  s->souls_shuffle = kSoulsShuffle_Off;
  s->npc_souls = 0;  // add-npc-souls default off (byte-identical)
  s->key_rings = kKeyRings_Off;
  s->skeleton_key = 0;
  s->shopsanity = 0;  // add-rando-shopsanity default off ([29] bit 4 stays 0)
  s->bonk_shuffle = kTerrainShuffle_Off;  // add-rando-bonk-sanity ([29] bits 5-6 stay 0)
  // add-rando-ow-warp-shuffle default off ([30] bits 3-5 stay 0).
  s->flute_shuffle = kFluteShuffle_Off;
  s->whirlpool_shuffle = 0;
}

bool Settings_HintsEnabled(const RandoSettings *s) {
  return s != NULL && s->hints == kHintsMode_Balanced &&
         !(s->hint_tile_coverage == kHintTileCoverage_None &&
           s->hint_paid_depth == 0);
}

uint8 Settings_HintTileCount(const RandoSettings *s) {
  if (!Settings_HintsEnabled(s)) return 0;
  switch (s->hint_tile_coverage) {
    case kHintTileCoverage_Full: return 15;
    case kHintTileCoverage_None: return 0;
    case kHintTileCoverage_Sparse: return 5;
    case kHintTileCoverage_Standard: return 10;
    default: return 0;  // invalid/foreign struct: fail closed
  }
}

uint8 Settings_EffectiveHintPaidDepth(const RandoSettings *s) {
  if (!Settings_HintsEnabled(s) || s->hint_paid_depth > 3) return 0;
  return s->hint_paid_depth;
}

void Settings_NormalizeHintPolicy(RandoSettings *s) {
  if (s == NULL) return;
  if (s->hints == kHintsMode_Off ||
      (s->hints == kHintsMode_Balanced &&
       s->hint_tile_coverage == kHintTileCoverage_None &&
       s->hint_paid_depth == 0)) {
    s->hints = kHintsMode_Off;
    s->hint_tile_coverage = kHintTileCoverage_Full;
    s->hint_mix = kHintMix_Variety;
    s->hint_paid_depth = 3;
  }
}

HintProfile Settings_GetHintProfile(const RandoSettings *s) {
  if (!Settings_HintsEnabled(s)) return kHintProfile_Off;
  if (s->hint_tile_coverage == kHintTileCoverage_Sparse &&
      s->hint_mix == kHintMix_Variety && s->hint_paid_depth == 1)
    return kHintProfile_Sparse;
  if (s->hint_tile_coverage == kHintTileCoverage_Full &&
      s->hint_mix == kHintMix_Variety && s->hint_paid_depth == 3)
    return kHintProfile_Balanced;
  if (s->hint_tile_coverage == kHintTileCoverage_Full &&
      s->hint_mix == kHintMix_Important && s->hint_paid_depth == 3)
    return kHintProfile_Direct;
  return kHintProfile_Custom;
}

bool Settings_ApplyHintProfile(RandoSettings *s, HintProfile profile) {
  if (s == NULL) return false;
  switch (profile) {
    case kHintProfile_Off:
      s->hints = kHintsMode_Off;
      s->hint_tile_coverage = kHintTileCoverage_Full;
      s->hint_mix = kHintMix_Variety;
      s->hint_paid_depth = 3;
      break;
    case kHintProfile_Sparse:
      s->hints = kHintsMode_Balanced;
      s->hint_tile_coverage = kHintTileCoverage_Sparse;
      s->hint_mix = kHintMix_Variety;
      s->hint_paid_depth = 1;
      break;
    case kHintProfile_Balanced:
      s->hints = kHintsMode_Balanced;
      s->hint_tile_coverage = kHintTileCoverage_Full;
      s->hint_mix = kHintMix_Variety;
      s->hint_paid_depth = 3;
      break;
    case kHintProfile_Direct:
      s->hints = kHintsMode_Balanced;
      s->hint_tile_coverage = kHintTileCoverage_Full;
      s->hint_mix = kHintMix_Important;
      s->hint_paid_depth = 3;
      break;
    case kHintProfile_Custom:
    default:
      return false;
  }
  Settings_NormalizeHintPolicy(s);
  return true;
}

const char *Settings_HintProfileName(HintProfile profile) {
  static const char *const kNames[] = {
    "off", "sparse", "balanced", "direct", "custom",
  };
  if ((int)profile < 0 || profile > kHintProfile_Custom) return "(unknown)";
  return kNames[profile];
}

const char *Settings_HintProfileTitle(HintProfile profile) {
  static const char *const kTitles[] = {
    "Off", "Sparse", "Balanced", "Direct", "Custom",
  };
  if ((int)profile < 0 || profile > kHintProfile_Custom) return "(Unknown)";
  return kTitles[profile];
}

// Apply derived-from-other-fields normalization rules.
// `goal=completionist` forces `accessibility=locations` per spec —
// otherwise the canonical hash differs between
// (SetDefaults; s->goal=Completionist) callers and
// (Settings_ParseCsv("goal=completionist")) callers.
//
// Applied on a private copy in Settings_CanonicalSerialize so any external
// caller's struct is left untouched and the hash is consistent regardless of
// how the struct was populated.
uint8 Settings_EffectiveSmallKeysMode(const RandoSettings *s) {
  if (s->world_state == kWorldState_Retro)
    return kDungeonItemMode_Wild;  // Retro forces region.wildKeys
  // Door shuffle and dungeon chains both alter required dungeon traversal, so
  // vanilla/free small-key assumptions can certify a route that spends more
  // physical keys than the vanilla in-place model exposes. Force in-dungeon keys
  // for both axes so hash, placement, spoiler, and runtime agree.
  if (Settings_EffectiveDoorShuffle(s) != kDoorShuffle_Vanilla ||
      Settings_EffectiveDungeonChains(s))
    return kDungeonItemMode_Dungeon;
  return s->dungeon_small_keys_mode;
}

uint8 Settings_EffectiveKeyRings(const RandoSettings *s) {
  if (s == NULL || s->key_rings == kKeyRings_Off)
    return kKeyRings_Off;
  if (Settings_KeyRingsForcedOff(s))
    return kKeyRings_Off;
  return s->key_rings;
}

bool Settings_KeyRingsForcedOff(const RandoSettings *s) {
  return s == NULL || Settings_GenericKeysActive(s) ||
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Vanilla;
}

uint8 Settings_EffectiveBigKeysMode(const RandoSettings *s) {
  // Keep big-key handling paired with small keys under topology-changing axes;
  // otherwise vanilla-mode pregrants can skip the in-dungeon key pickup that the
  // runtime still requires before a boss seam.
  if (Settings_EffectiveDoorShuffle(s) != kDoorShuffle_Vanilla ||
      Settings_EffectiveDungeonChains(s))
    return kDungeonItemMode_Dungeon;
  return s->dungeon_big_keys_mode;
}

bool Settings_GenericKeysActive(const RandoSettings *s) {
  // Retro pins rom.genericKeys on (app/World/Retro.php). Computed from
  // world_state — never a serialized bit (canonical length unchanged).
  return s != NULL && s->world_state == kWorldState_Retro;
}

static void apply_derived_rules(RandoSettings *s) {
  Settings_NormalizeHintPolicy(s);
  if (s->goal == kGoal_Completionist) {
    s->accessibility = kAccessibility_Locations;
  }
  // These serialized slots are retained only so old share strings / CSV keys
  // decode cleanly. The Pyramid Fairy trade-in was retired, and boss-heart
  // drops are now always part of the shuffled location pool.
  s->pyramid_bow_upgrade = kPyramidBowUpgrade_Silvers;
  s->region_boss_hearts_in_pool = 0;
  // Retro forces wildKeys: normalize the small-keys mode to Wild so the
  // canonical settings hash matches the actual (Wild-placed) seed. The placer
  // reads the same override via Settings_EffectiveSmallKeysMode, so hash and
  // placement always agree. (genericKeys — one shared pool — is NOT applied;
  // keys keep dungeon identity. See the helper's doc comment.)
  s->dungeon_small_keys_mode = Settings_EffectiveSmallKeysMode(s);
  // Phase C — entrance-axis normalization.
  // entrance shuffle is only honored on Open/Standard (Inverted carries
  // a static region override the per-seed one would clobber; Retro re-uses cave
  // host-rooms for TakeAny — see Entrance_IsActive). So an entrance axis set under
  // Inverted/Retro produces NO shuffle at runtime; normalize the axes OFF here so
  // the settings_hash matches the actual (un-shuffled) seed and two
  // functionally-identical seeds don't hash differently.
  if (s->world_state != kWorldState_Open &&
      s->world_state != kWorldState_Standard) {
    s->shuffle_cave_entrances = 0;
    s->shuffle_dungeon_entrances = 0;
  }
  // add-rando-ow-warp-shuffle — v1 scope excludes Inverted ONLY (Inverted
  // rewrites the overworld and flute semantics; the spot data is LW-indexed).
  // Deliberately NOT the entrance axes' broader "not Open/Standard" guard
  // above: Retro warps are in scope and must survive normalization.
  if (s->world_state == kWorldState_Inverted) {
    s->flute_shuffle = kFluteShuffle_Off;
    s->whirlpool_shuffle = 0;
  }
  // The Ganon's Tower opt-in only means anything when dungeon entrances are
  // being shuffled (GT joins the dungeon pool); normalize it off otherwise so
  // the hash is stable and the default packs to 0.
  if (!s->shuffle_dungeon_entrances) {
    s->shuffle_ganons_tower_entrance = 0;
  }
  // Coupling/cross/decoupled are meaningless when no interior class is being
  // shuffled; force them off so the packed byte [25] is canonical (and 0x00 for
  // the default — the corpus byte-identical invariant). `decoupled` implies
  // `!coupled` (per-endpoint shuffle cannot also be coupled). This makes the
  // (struct → canonical) mapping many-to-one in a well-defined way so the hash
  // is stable regardless of stray flag values left in the struct.
  if (!s->shuffle_cave_entrances && !s->shuffle_dungeon_entrances) {
    s->coupled = 0;
    s->cross_category = 0;
    s->decoupled = 0;
  } else if (s->decoupled) {
    // Decoupled (one-way exits) is its OWN coupling mode: never coupled. It DOES
    // compose with cross-category — cross-decoupled = one-way exits over the mixed
    // cave+dungeon pool (Entrance_IsCrossDecoupledActive) — so cross is kept here.
    s->coupled = 0;
  } else {
    // A shuffle is on and not decoupled ⇒ coupled. `coupled` is fully DERIVED
    // (= shuffle-on && !decoupled), so the settings UI only toggles `decoupled`.
    s->coupled = 1;
  }
  // Cross-category mixes the two pools, so it only does anything when BOTH cave
  // and dungeon shuffle are on (matches Entrance_IsCrossActive). With only one
  // class shuffled, runtime produces a non-cross seed — normalize the bit off so
  // the settings_hash matches the actual seed. Must run AFTER the
  // both-off clear above so it sees the final cave/dungeon values.
  if (!s->shuffle_cave_entrances || !s->shuffle_dungeon_entrances) {
    s->cross_category = 0;
  }
  // Decoupled ("Insanity") composes on the cave AND/OR dungeon ENTRY shuffle:
  // cave-decoupled (Entrance_IsDecoupledActive) needs shuffle_cave_entrances,
  // dungeon-decoupled (Entrance_IsDungeonDecoupledActive) needs shuffle_dungeon_
  // entrances. So decoupled is meaningful whenever EITHER shuffle is on — and the
  // both-off clear above already strips it when neither is. No extra clear needed.

  // add-rando-door-shuffle — MVP compatibility pins (design plan P1-P5):
  //  * honored only on Open/Standard (Inverted has its own logic tree; Retro
  //    collapses per-key-door HAS_AMOUNT thresholds to GenericKey>=1) and only
  //    under NoGlitches logic (the door oracle models no glitch traversal);
  //  * mutually exclusive with entrance shuffle (both redirect dungeon
  //    topology; the per-seed lobby assumptions interact) — door shuffle
  //    yields to an explicit entrance-shuffle request;
  //  * forces in-dungeon small AND big keys: the key-door prover's
  //    containment assumption + the bk_restricted ban require both.
  // Normalizing here (not refusing) keeps the settings_hash equal to the
  // actually-generated seed, the same convention as the entrance-axis
  // normalization above.
  s->door_shuffle = Settings_EffectiveDoorShuffle(s);
  if (s->door_shuffle != kDoorShuffle_Vanilla) {
    s->dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
    s->dungeon_big_keys_mode = kDungeonItemMode_Dungeon;
  }

  // dungeon-chains — one-directional compatibility: the chain opt-in yields to
  // existing topology/runtime modes, it never forces those modes off. Once a
  // chain survives normalization, force dungeon keys in the serialized settings
  // so the hash matches the effective placer/runtime contract.
  s->dungeon_chains = Settings_EffectiveDungeonChains(s);
  if (s->dungeon_chains) {
    s->dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
    s->dungeon_big_keys_mode = kDungeonItemMode_Dungeon;
  }

  // add-rando-pot-sanity — pots do NOT compose with cave-entrance shuffle in v1.
  // Cave/house pot location IDs are above the per-location entrance
  // region-override range (Entrance_ApplyRegionOverrides only remaps the caves'
  // <512 chest IDs), so a cave/house pot would evaluate from its VANILLA overworld
  // region while the runtime reaches the interior through the shuffled entrance —
  // certifying progression in a pot the layout doesn't make reachable. Normalize
  // pot_shuffle off here so the settings_hash equals the actually-generated
  // (pot-less) seed; pot_active + the spoiler consult the SAME predicate
  // (Settings_PotShuffleForcedOff), so hash, placement, runtime, and spoiler agree.
  if (Settings_PotShuffleForcedOff(s)) {
    s->pot_shuffle = kPotShuffle_Off;
  }

  // add-rando-shopsanity — NO normalization here, deliberately. shopsanity was
  // once forced off under cave-entrance shuffle for the same reason pots still
  // are: the four shop-hosting interiors were single pool entries spanning
  // several doors — room 0x10F alone backs four shops through one entrance id —
  // so a shop slot would have been evaluated from its VANILLA region while the
  // player reached the interior through a different door, and no binding could
  // repair it while one entry had to serve four shops. Splitting the pool by
  // overworld DOOR ROW gave each shop its own entry, its own region and its own
  // three slot ids in location_ids, so Entrance_ApplyRegionOverrides rebinds
  // them and the axes compose. Settings_ShopsanityActive is now the whole
  // consumer contract. (Pots stay unlike shops: their ids sit ABOVE
  // kEntranceRegionOverrideMax, so the same fix is not available to them.)
  s->enemy_drop_checks = Settings_EffectiveEnemyDropChecks(s);
  // add-enemy-souls — the enemies tier degrades to Bosses under door shuffle
  // (species-blind oracle + vanilla-graph soul requirements; see
  // Settings_EffectiveSoulsShuffle). Normalize so the settings_hash matches the
  // actually-generated seed, same convention as enemy_drop_checks above. Must
  // run AFTER the door-shuffle normalization so it sees the final value.
  s->souls_shuffle = Settings_EffectiveSoulsShuffle(s);
  // add-rando-trap-catalog — trap_categories is meaningless with traps off,
  // and the full mask spells out what the 0 zero-sentinel already means ("all
  // categories while traps>0", see kTrapCategory_All). Collapse both to the
  // sentinel so the (struct -> canonical) mapping is many-to-one in a
  // well-defined way and two functionally-identical seeds can't hash
  // differently (the coupled/GT-bit convention above).
  if (s->traps == kTrapFrequency_Off || s->trap_categories == kTrapCategory_All) {
    s->trap_categories = 0;
  }
}

bool Settings_EffectiveShuffleCaveEntrances(const RandoSettings *s) {
  // Mirrors apply_derived_rules: the entrance axes are zeroed under any
  // world_state other than Open/Standard (Inverted carries a static region
  // override, Retro re-uses cave host-rooms — Entrance_IsActive), so a raw cave
  // bit set there is inert. Read this, never the raw flag, in generation logic.
  return s != NULL && s->shuffle_cave_entrances != 0 &&
         (s->world_state == kWorldState_Open ||
          s->world_state == kWorldState_Standard);
}

bool Settings_PotShuffleForcedOff(const RandoSettings *s) {
  return s != NULL && Settings_EffectiveShuffleCaveEntrances(s);
}

// add-rando-ow-warp-shuffle — v1 scope excludes Inverted (it rewrites the
// overworld and the flute semantics, and the spot data is LW-indexed).
// apply_derived_rules zeroes both axes there, but that only ever runs on the
// PRIVATE copy Settings_CanonicalSerialize normalizes -- generation and slot
// activation read the RAW struct. Without this re-derivation an Inverted seed
// really does shuffle warps while its share string encodes the axes as off, so
// the same share string reproduces a DIFFERENT world (measured: 6 of 12 sampled
// Inverted seeds moved placement_digest). Same consumer contract as
// Settings_PotShuffleForcedOff: read these, never the raw fields.
bool Settings_OwWarpForcedOff(const RandoSettings *s) {
  return s != NULL && s->world_state == kWorldState_Inverted;
}

uint8 Settings_EffectiveFluteShuffle(const RandoSettings *s) {
  if (s == NULL || Settings_OwWarpForcedOff(s))
    return kFluteShuffle_Off;
  return s->flute_shuffle;
}

bool Settings_EffectiveWhirlpoolShuffle(const RandoSettings *s) {
  return s != NULL && !Settings_OwWarpForcedOff(s) && s->whirlpool_shuffle != 0;
}

bool Settings_OwWarpActive(const RandoSettings *s) {
  return Settings_EffectiveFluteShuffle(s) != kFluteShuffle_Off ||
         Settings_EffectiveWhirlpoolShuffle(s);
}

// True when the 27 regular shop slots are live fill locations. Single source of
// truth for the placer, the spoiler and the runtime gate, so they can't drift
// apart. No derived-rule normalization: the axis composes with everything,
// including cave-entrance shuffle (each cave-resident shop is its own pool
// entry carrying its three slot ids, so Entrance_ApplyRegionOverrides rebinds
// them to whichever door reaches them — see shuffle_entrance.c's shop_loc_base).
bool Settings_ShopsanityActive(const RandoSettings *s) {
  return s != NULL && s->shopsanity != 0;
}

uint8 Settings_DoorPotTier(const RandoSettings *s) {
  if (s == NULL ||
      Settings_EffectiveDoorShuffle(s) == kDoorShuffle_Vanilla ||
      Settings_PotShuffleForcedOff(s))
    return kPotShuffle_Off;
  return s->pot_shuffle;
}

// goal == Completionist forces 100%-Locations (apply_derived_rules:155-157).
// Every placement/spoiler consumer reads this so it can't diverge from the
// canonical hash, which normalizes the same way on its private copy.
uint8 Settings_EffectiveAccessibility(const RandoSettings *s) {
  if (s == NULL) return kAccessibility_Items;
  if (s->goal == kGoal_Completionist) return kAccessibility_Locations;
  return s->accessibility;
}

// True when pot_shuffle itemizes small-key pots as live checks: pot_shuffle >=
// Keys AND pots are not forced off (cave-entrance shuffle). The placer
// consumes RAW (non-normalized) settings — Settings_CanonicalSerialize runs
// apply_derived_rules only on a private copy — so every pot-key predicate MUST
// re-derive "forced off" itself rather than trust a normalized pot_shuffle. This
// is the single source of truth shared by the logic VM (eval_pot_keys_*) and the
// placer (pot_keys_dungeon_active) so the two can't drift; the WILD/DUNGEON
// variants additionally test Settings_EffectiveSmallKeysMode.
bool Settings_PotKeysActive(const RandoSettings *s) {
  return s != NULL && s->pot_shuffle >= kPotShuffle_Keys &&
         !Settings_PotShuffleForcedOff(s);
}

static bool Settings_EffectiveAnyEntranceShuffle(const RandoSettings *s) {
  return s != NULL &&
         (s->world_state == kWorldState_Open ||
          s->world_state == kWorldState_Standard) &&
         (s->shuffle_cave_entrances || s->shuffle_dungeon_entrances);
}

uint8 Settings_EffectiveEnemyDropChecks(const RandoSettings *s) {
  if (s == NULL || s->enemy_drop_checks == kEnemyDropChecks_Off)
    return kEnemyDropChecks_Off;
  uint8 small_keys = Settings_EffectiveSmallKeysMode(s);
  if (small_keys != kDungeonItemMode_Wild && small_keys != kDungeonItemMode_Dungeon)
    return kEnemyDropChecks_Off;
  // Degrading to Keys under enemy_shuffle is sound only because every
  // Keys-tier (forced-drop) check room is in the species/soul pin set
  // (kRandoSoulPinRooms), so enemy shuffle never substitutes the drop source
  // whose vanilla soul/slot the check's logic and runtime key on. The codegen
  // that owns the pin table asserts that coverage against the emitted drops
  // registry (gen_soul_room_tables.py, assert_keys_tier_rooms_pinned).
  if (s->enemy_drop_checks >= kEnemyDropChecks_All) {
    if (s->enemy_shuffle)
      return kEnemyDropChecks_Keys;
    if (Settings_EffectiveAnyEntranceShuffle(s))
      return kEnemyDropChecks_Dungeon;
    return kEnemyDropChecks_All;
  }
  if (s->enemy_drop_checks == kEnemyDropChecks_Dungeon) {
    if (s->enemy_shuffle)
      return kEnemyDropChecks_Keys;
    return kEnemyDropChecks_Dungeon;
  }
  return kEnemyDropChecks_Keys;
}

bool Settings_EnemyDropKeysActive(const RandoSettings *s) {
  return Settings_EffectiveEnemyDropChecks(s) >= kEnemyDropChecks_Keys;
}

uint8 Settings_EffectiveSoulsShuffle(const RandoSettings *s) {
  if (s == NULL || s->souls_shuffle == kSoulsShuffle_Off)
    return kSoulsShuffle_Off;
  // Souls do not compose with door shuffle at ANY tier (see rando_settings.h):
  // the door-key oracle is species-blind, the enemies tier's kill-room soul
  // requirements are vanilla-door-graph-specific, and even the bosses tier's
  // soul-gated boss/prize wraps make the door-layout fill churn its whole
  // attempt budget per layout candidate (empirical: souls=all + door_basic
  // timed out across 7 layout passes x 256 attempts).
  if (Settings_EffectiveDoorShuffle(s) != kDoorShuffle_Vanilla)
    return kSoulsShuffle_Off;
  return s->souls_shuffle;
}

bool Settings_EnemyChecksDungeonActive(const RandoSettings *s) {
  return Settings_EffectiveEnemyDropChecks(s) >= kEnemyDropChecks_Dungeon;
}

bool Settings_EnemyChecksAllActive(const RandoSettings *s) {
  return Settings_EffectiveEnemyDropChecks(s) == kEnemyDropChecks_All;
}

int Settings_CanonicalSerialize(const RandoSettings *s_in,
                                uint8 out[kSettingsCanonicalLen]) {
  // Layout per `randomizer-core / Settings canonical serialization order`.
  // 21 single-byte fields + 2×u16 LE + packed/append-only axes = 31 bytes
  // (kGenVer 14 §66 grew 18→21 single-byte fields by absorbing hints +
  // boss_shuffle + drop_shuffle at offsets [22..24]).
  RandoSettings sn = *s_in;
  apply_derived_rules(&sn);
  const RandoSettings *s = &sn;
  out[0]  = s->world_state;
  out[1]  = s->goal;
  out[2]  = s->crystals_ganon;
  out[3]  = s->crystals_tower;
  out[4]  = s->tricks;
  out[5]  = s->item_pool_difficulty;
  out[6]  = s->logic;
  out[7]  = s->mode_weapons;
  out[8]  = s->accessibility;
  out[9]  = s->pyramid_bow_upgrade;
  out[10] = s->region_boss_hearts_in_pool;
  out[11] = s->dungeon_small_keys_mode;
  out[12] = s->dungeon_big_keys_mode;
  out[13] = s->dungeon_maps_mode;
  out[14] = s->dungeon_compasses_mode;
  out[15] = s->prize_shuffle;
  out[16] = s->medallion_shuffle;
  out[17] = s->race_mode;
  out[18] = (uint8)(s->pieces_required & 0xff);
  out[19] = (uint8)((s->pieces_required >> 8) & 0xff);
  out[20] = (uint8)(s->pieces_placed & 0xff);
  out[21] = (uint8)((s->pieces_placed >> 8) & 0xff);
  // §66: hints + shuffle axes joined the hash at kGeneratorVersion 14.
  out[22] = s->hints;
  out[23] = s->boss_shuffle;
  out[24] = s->drop_shuffle;
  // Phase C entrance-shuffle axes and independent NPC reward disclosure share
  // the historical pad byte [25]. apply_derived_rules() has normalized
  // coupled/cross/decoupled; every field defaults off, so this is 0x00 for the
  // default settings (corpus byte-identical).
  out[25] = (uint8)((s->shuffle_cave_entrances    ? kEntranceAxis_ShuffleCaves    : 0) |
                    (s->shuffle_dungeon_entrances ? kEntranceAxis_ShuffleDungeons : 0) |
                    (s->coupled                   ? kEntranceAxis_Coupled         : 0) |
                    (s->cross_category            ? kEntranceAxis_CrossCategory   : 0) |
                    (s->decoupled                 ? kEntranceAxis_Decoupled       : 0) |
                    (s->shuffle_ganons_tower_entrance ? kEntranceAxis_ShuffleGanonsTower : 0) |
                    (s->dungeon_chains            ? kEntranceAxis_DungeonChains   : 0) |
                    (s->hint_npc_reward_reveal    ? kHintNpcRewardRevealAxis_Enabled : 0));
  // add-rando-enemy-shuffle / customizer / traps / instant-flute — share the
  // formerly-zero pad byte [26]. Defaults keep every bit clear (instant-flute
  // uses an inverse manual-activation bit), preserving default settings_hash.
  out[26] = (uint8)((s->enemy_shuffle ? kEnemyShuffleAxis_Enabled : 0) |
                    (s->customizer_active ? kCustomizerAxis_Active : 0) |
                    ((s->traps << kTrapAxis_Shift) & kTrapAxis_Mask) |
                    ((s->traps & 4u) ? kTrapAxis_HighBit : 0) |  // Insanity(4) -> bit5
                    (s->instant_flute ? 0 : kInstantFluteAxis_ManualActivation) |
                    // add-rando-pot-sanity — pot_shuffle low 2 bits in [26] 6-7.
                    ((s->pot_shuffle << kPotShuffleAxis_LowShift) & kPotShuffleAxis_LowMask));
  // add-rando-door-shuffle — door_shuffle axis in the (formerly zero) pad
  // byte [27] bits 0-1. apply_derived_rules() normalized incompatible combos,
  // so the default packs to 0x00 (corpus byte-identical) and
  // kSettingsCanonicalLen stayed 28 until the append-only [28] axis.
  // add-rando-trap-catalog — door_shuffle (bits 0-1) shares [27] with the
  // per-category trap mask (bits 2-6). Default trap_categories=0 keeps [27] at the
  // door_shuffle value (0x00 for the default), so the corpus stays byte-identical.
  out[27] = (uint8)((s->door_shuffle & kDoorShuffleAxis_Mask) |
                    ((s->trap_categories << kTrapCategoriesAxis_Shift) & kTrapCategoriesAxis_Mask) |
                    // add-rando-pot-sanity — pot_shuffle high bit in [27] bit 7.
                    ((s->pot_shuffle & 4u) ? kPotShuffleAxis_HighBit : 0));
  // [28]: enemy/soul axes in bits 0-4, configurable-hints tile coverage in
  // bits 5-6, and the hint-mix low bit in bit 7. Full/Variety are wire zero,
  // so defaults keep the byte 0x00.
  out[28] = (uint8)((s->enemy_drop_checks & 3u) |
                    ((s->souls_shuffle << kSoulsShuffleAxis_Shift) & kSoulsShuffleAxis_Mask) |
                    (s->npc_souls ? kNpcSoulsAxis_Enabled : 0) |
                    ((s->hint_tile_coverage << kHintTileCoverageAxis_Shift) &
                     kHintTileCoverageAxis_Mask) |
                    ((s->hint_mix & 1u) ? kHintMixAxis_LowBit : 0));
  // add-rando-grass-rock-shuffle — the appended terrain byte [29]:
  // grass_shuffle bits 0-1, rock_shuffle bits 2-3 (TerrainShuffle enum).
  // Defaults keep the byte 0x00; the LENGTH growth 29->30 changes every
  // settings_hash once, under that change's kGeneratorVersion bump.
  out[29] = (uint8)(((s->grass_shuffle << kGrassShuffleAxis_Shift) & kGrassShuffleAxis_Mask) |
                    ((s->rock_shuffle << kRockShuffleAxis_Shift) & kRockShuffleAxis_Mask) |
                    (s->shopsanity ? kShopsanityAxis_Enabled : 0) |
                    ((s->bonk_shuffle << kBonkShuffleAxis_Shift) & kBonkShuffleAxis_Mask) |
                    ((s->hint_mix & 2u) ? kHintMixAxis_HighBit : 0));
  // add-rando-key-rings-skeleton-key — preserve the REQUESTED ring policy.
  // Effective-off semantics are computed by Settings_EffectiveKeyRings and are
  // deliberately not part of apply_derived_rules.
  // add-rando-ow-warp-shuffle — [30] bits 3-5 (defaults 0 = byte-stable).
  out[30] = (uint8)(((s->key_rings << kKeyRingsAxis_Shift) & kKeyRingsAxis_Mask) |
                    (s->skeleton_key ? kSkeletonKeyAxis_Enabled : 0) |
                    (s->whirlpool_shuffle ? kWhirlpoolAxis_Enabled : 0) |
                    ((s->flute_shuffle << kFluteShuffleAxis_Shift) &
                     kFluteShuffleAxis_Mask) |
                    // Actual depth 3 is wire zero; 0/1/2 map to 1/2/3.
                    ((((s->hint_paid_depth + 1u) & 3u) <<
                      kHintPaidDepthAxis_Shift) & kHintPaidDepthAxis_Mask));
  return kSettingsCanonicalLen;
}

// Phase B Slice 6 — inverse of Settings_CanonicalSerialize. Reads the
// kSettingsCanonicalLen (31)-byte canonical blob and populates `out`. Returns 0
// on success, -1 if the input is NULL. Body occupies [0..24] (through
// drop_shuffle); [25] = entrance axes, [26] = enemy_shuffle + customizer +
// traps + inverse instant-flute, [27] = door_shuffle, [28] = enemy_drop_checks.
//
// Byte [25] is the Phase C packed entrance-axis byte (0x00 = no shuffle); byte
// [26] packs bit0 = enemy_shuffle (add-rando-enemy-shuffle) and bit1 =
// customizer_active (add-rando-customizer-mode), bits2-3 = traps
// (add-rando-traps), bit4 = manual flute activation (inverse of instant_flute);
// byte [27] bits 0-1 are the add-rando-door-shuffle axis.
// Pre-extension files have those bits = 0, so older suppressed files still
// round-trip cleanly.
// Every bit in the packed flag bytes [25..27] is now defined. Pre-extension
// files have each added bit clear, so older suppressed files still round-trip
// cleanly. Enum values reassembled from these bytes remain range-checked below.
int Settings_CanonicalDeserialize(const uint8 in[kSettingsCanonicalLen],
                                  RandoSettings *out) {
  if (in == NULL || out == NULL) return -1;
  RandoSettings s;
  memset(&s, 0, sizeof(s));
  s.settings_version = 1;  // Phase A constant; not serialized
  s.world_state                = in[0];
  s.goal                       = in[1];
  s.crystals_ganon             = in[2];
  s.crystals_tower             = in[3];
  s.tricks                     = in[4];
  s.item_pool_difficulty       = in[5];
  s.logic                      = in[6];
  s.mode_weapons               = in[7];
  s.accessibility              = in[8];
  s.pyramid_bow_upgrade        = in[9];
  s.region_boss_hearts_in_pool = in[10];
  s.dungeon_small_keys_mode    = in[11];
  s.dungeon_big_keys_mode      = in[12];
  s.dungeon_maps_mode          = in[13];
  s.dungeon_compasses_mode     = in[14];
  s.prize_shuffle              = in[15];
  s.medallion_shuffle          = in[16];
  s.race_mode                  = in[17];
  s.pieces_required            = (uint16)(in[18] | ((uint16)in[19] << 8));
  s.pieces_placed              = (uint16)(in[20] | ((uint16)in[21] << 8));
  // §66: read hints + shuffle axes (offsets [22..24]). Byte [25] is the Phase C
  // entrance-axis byte (unpacked below); pad bytes [26..27] are not inspected.
  s.hints                      = in[22];
  s.boss_shuffle               = in[23];
  s.drop_shuffle               = in[24];
  // Phase C — unpack the entrance-axis byte [25]. A zero byte (the default /
  // any pre-Phase-C file) yields all-off, coupled-off — identical to a struct
  // with no entrance shuffle.
  s.shuffle_cave_entrances     = (in[25] & kEntranceAxis_ShuffleCaves)    ? 1 : 0;
  s.shuffle_dungeon_entrances  = (in[25] & kEntranceAxis_ShuffleDungeons) ? 1 : 0;
  s.coupled                    = (in[25] & kEntranceAxis_Coupled)         ? 1 : 0;
  s.cross_category             = (in[25] & kEntranceAxis_CrossCategory)   ? 1 : 0;
  s.decoupled                  = (in[25] & kEntranceAxis_Decoupled)       ? 1 : 0;
  s.shuffle_ganons_tower_entrance = (in[25] & kEntranceAxis_ShuffleGanonsTower) ? 1 : 0;
  s.dungeon_chains             = (in[25] & kEntranceAxis_DungeonChains)   ? 1 : 0;
  // Independent NPC transaction disclosure — [25] bit 7. Zero is the
  // historical/default behavior and remains independent of clue delivery.
  s.hint_npc_reward_reveal =
      (in[25] & kHintNpcRewardRevealAxis_Enabled) ? 1 : 0;
  // add-rando-enemy-shuffle — unpack the enemy-shuffle bit from pad byte [26].
  // A zero byte (the default / any pre-enemy-shuffle file) yields enemy_shuffle=0,
  // identical to a struct with no enemy shuffle.
  s.enemy_shuffle = (in[26] & kEnemyShuffleAxis_Enabled) ? 1 : 0;
  // add-rando-customizer-mode — unpack customizer_active from [26] bit1.
  // A zero bit (default / any pre-customizer file) yields customizer_active=0.
  s.customizer_active = (in[26] & kCustomizerAxis_Active) ? 1 : 0;
  // add-rando-traps — unpack the trap-frequency field from [26] bits 2-3.
  // A zero field (default / any pre-traps file) yields traps=off.
  // Reassemble the non-contiguous 3-bit traps value: low 2 bits from [26] bits 2-3,
  // high bit (Insanity) from [26] bit5.
  s.traps = (uint8)(((in[26] & kTrapAxis_Mask) >> kTrapAxis_Shift) |
                    ((in[26] & kTrapAxis_HighBit) ? 4u : 0u));
  // Randomizer QoL — inverse bit so old/default blobs activate flute instantly.
  s.instant_flute = (in[26] & kInstantFluteAxis_ManualActivation) ? 0 : 1;
  // add-rando-door-shuffle — unpack the door_shuffle axis from pad byte [27]
  // bits 0-1. A zero byte (the default / any pre-door-shuffle file) yields
  // vanilla. Trap-category and pot-shuffle fields consume bits 2-7 below.
  s.door_shuffle = in[27] & kDoorShuffleAxis_Mask;
  // add-rando-trap-catalog — unpack the per-category trap mask from [27] bits 2-6.
  // Zero (default / any pre-catalog file) is the "all categories" sentinel applied
  // at selection time when traps are enabled.
  s.trap_categories = (uint8)((in[27] & kTrapCategoriesAxis_Mask) >> kTrapCategoriesAxis_Shift);
  // add-rando-pot-sanity — reassemble the non-contiguous 3-bit pot_shuffle: low
  // 2 bits from [26] bits 6-7, high bit from [27] bit 7. Zero (default / any
  // pre-pot file) yields Off, identical to a struct with no pot shuffle.
  s.pot_shuffle = (uint8)(((in[26] & kPotShuffleAxis_LowMask) >> kPotShuffleAxis_LowShift) |
                          ((in[27] & kPotShuffleAxis_HighBit) ? 4u : 0u));
  s.enemy_drop_checks = (uint8)(in[28] & 3u);
  // add-enemy-souls — souls_shuffle from [28] bits 2-3. Zero (default / any
  // pre-souls file) yields Off, identical to a struct with no souls.
  s.souls_shuffle = (uint8)((in[28] & kSoulsShuffleAxis_Mask) >> kSoulsShuffleAxis_Shift);
  // add-npc-souls — [28] bit 4. Zero (default / pre-npc file) yields off.
  s.npc_souls = (in[28] & kNpcSoulsAxis_Enabled) ? 1 : 0;
  // Configurable hints — [28] bits 5-6 are tile coverage and bit 7 is the
  // low hint-mix bit. The high mix bit is decoded from [29] below.
  s.hint_tile_coverage =
      (uint8)((in[28] & kHintTileCoverageAxis_Mask) >>
              kHintTileCoverageAxis_Shift);
  s.hint_mix = (in[28] & kHintMixAxis_LowBit) ? 1 : 0;
  if (in[28] & ~(uint8)kCanon28_DefinedMask) return -2;
  // The appended byte [29] stores terrain axes in bits 0-3, shopsanity in
  // bit 4, bonk shuffle in bits 5-6, and the hint-mix high bit in bit 7.
  // Terrain value 3 remains corruption, not forward compatibility.
  s.grass_shuffle = (uint8)((in[29] & kGrassShuffleAxis_Mask) >> kGrassShuffleAxis_Shift);
  s.rock_shuffle = (uint8)((in[29] & kRockShuffleAxis_Mask) >> kRockShuffleAxis_Shift);
  s.shopsanity = (in[29] & kShopsanityAxis_Enabled) ? 1 : 0;
  s.bonk_shuffle = (uint8)((in[29] & kBonkShuffleAxis_Mask) >> kBonkShuffleAxis_Shift);
  if (in[29] & kHintMixAxis_HighBit) s.hint_mix |= 2u;
  if (in[29] & ~(uint8)(kGrassShuffleAxis_Mask | kRockShuffleAxis_Mask |
                        kShopsanityAxis_Enabled | kBonkShuffleAxis_Mask |
                        kHintMixAxis_HighBit)) return -2;
  if (s.bonk_shuffle > kTerrainShuffle_All) return -2;
  if (s.grass_shuffle > kTerrainShuffle_All || s.rock_shuffle > kTerrainShuffle_All) return -2;
  // add-rando-key-rings-skeleton-key — [30] is strict append-only data. Older
  // containers zero-extend a 30-byte blob before calling this function.
  if (in[30] & ~(uint8)kCanon30_DefinedMask) return -2;
  s.key_rings = (uint8)((in[30] & kKeyRingsAxis_Mask) >> kKeyRingsAxis_Shift);
  s.skeleton_key = (in[30] & kSkeletonKeyAxis_Enabled) ? 1 : 0;
  if (s.key_rings > kKeyRings_All) return -2;
  // add-rando-ow-warp-shuffle — [30] bits 3-5; flute enum value 3 is
  // refused-undefined (never aliased to a defined mode).
  s.whirlpool_shuffle = (in[30] & kWhirlpoolAxis_Enabled) ? 1 : 0;
  s.flute_shuffle = (uint8)((in[30] & kFluteShuffleAxis_Mask) >>
                            kFluteShuffleAxis_Shift);
  if (s.flute_shuffle > kFluteShuffle_Random) return -2;
  // Configurable hints — paid depth uses a zero-default wire rotation:
  // wire 0/1/2/3 decodes to actual depth 3/0/1/2.
  uint8 hint_paid_wire =
      (uint8)((in[30] & kHintPaidDepthAxis_Mask) >>
              kHintPaidDepthAxis_Shift);
  s.hint_paid_depth = (uint8)((hint_paid_wire + 3u) & 3u);
  // FIX #5 — refuse out-of-range enum bytes and decoded packed-field values.
  // The reserved mode_weapons==2 reject and the hunt-goal
  // pieces_required<=pieces_placed cross-field rule live inside
  // Settings_Validate, which this calls.
  if (!Settings_Validate(&s)) return -2;
  apply_derived_rules(&s);
  *out = s;
  return 0;
}

// FIX #5 — see rando_settings.h. One check per serialized enum/count field,
// in canonical-byte order. Every legitimate writer funnels through
// Settings_ParseCsv / the UI widgets (which constrain values) and then
// Settings_CanonicalSerialize, so all blobs ever written pass; only corrupt
// or foreign bytes are refused.
bool Settings_Validate(const RandoSettings *s) {
  if (s == NULL) return false;
  if (s->world_state > kWorldState_Retro) return false;                    // [0] 0..3
  if (s->goal > kGoal_Completionist) return false;                         // [1] 0..6
  // [2][3] 0..7 fixed or kCrystalsRandom (8) — add-rando-random-crystals.
  if (s->crystals_ganon > kCrystalsRandom || s->crystals_tower > kCrystalsRandom)
    return false;
  // [4] tricks: full 8-bit bitmask (all 8 trick bits defined) — any byte valid.
  if (s->item_pool_difficulty > kItemPoolDifficulty_Expert) return false;  // [5] 0..3
  if (s->logic > 4) return false;          // [6] tier ceiling = NoLogic (4), per the CSV parser
  // [7] 0/1/3 valid; 2 (kModeWeapons_Vanilla / "Absolute weapon mode") is
  // reserved-until-implemented (untested placement branch) — reject so a
  // future-version v2 share-string can't activate it.
  if (s->mode_weapons == 2 || s->mode_weapons > kModeWeapons_Swordless) return false;
  if (s->accessibility > kAccessibility_None) return false;                // [8] 0..2
  if (s->pyramid_bow_upgrade != kPyramidBowUpgrade_Silvers) return false;  // [9] only 0 defined
  if (s->region_boss_hearts_in_pool > 1) return false;                     // [10] bool
  if (s->dungeon_small_keys_mode > kDungeonItemMode_Wild) return false;    // [11] 0..2
  if (s->dungeon_big_keys_mode > kDungeonItemMode_Wild) return false;      // [12] 0..2
  if (s->dungeon_maps_mode > kDungeonItemMode_Wild) return false;          // [13] 0..2
  if (s->dungeon_compasses_mode > kDungeonItemMode_Wild) return false;     // [14] 0..2
  if (s->prize_shuffle > 1 || s->medallion_shuffle > 1) return false;      // [15][16] bool
  if (s->race_mode > 1) return false;                                      // [17] bool
  // [18..21] pieces_required/pieces_placed: full uint16 range is CLI-legal,
  // but for a hunt goal pieces_required must not exceed pieces_placed (the
  // pool is unbuildable otherwise — BuildItemPool refuses it; CSV + native
  // window reject it). Enforce here so a malformed v2 share-string can't
  // smuggle it past the share-paste path.
  if ((s->goal == kGoal_TriforceHunt || s->goal == kGoal_GanonHunt) &&
      s->pieces_required > s->pieces_placed)
    return false;
  if (s->hints > 1) return false;                                          // [22] Off/Balanced
  if (s->hint_tile_coverage > kHintTileCoverage_Standard) return false;     // [28] bits5-6
  if (s->hint_mix > kHintMix_WorldInfo) return false;                       // [28]/[29] bit7
  if (s->hint_paid_depth > 3) return false;                                 // [30] bits6-7
  if (s->hint_npc_reward_reveal > 1) return false;                          // [25] bit7
  if (s->boss_shuffle > 1 || s->drop_shuffle > 1) return false;            // [23][24] bool
  // [25] entrance/chains axes + NPC disclosure / [26] misc axes: bit-packed.
  if (s->shuffle_cave_entrances > 1 || s->shuffle_dungeon_entrances > 1 ||
      s->coupled > 1 || s->cross_category > 1 || s->decoupled > 1 ||
      s->shuffle_ganons_tower_entrance > 1 || s->dungeon_chains > 1)
    return false;
  if (s->instant_flute > 1) return false;                                  // [26] bit4 inverse bool
  if (s->door_shuffle > kDoorShuffle_Basic) return false;                  // [27] bits 0-1: only 0/1 defined
  if (s->trap_categories > kTrapCategory_All) return false;               // [27] bits 2-6: 5-bit category mask
  if (s->traps > kTrapFrequency_Insanity) return false;                   // [26] bits 2-3 + bit5: 0..4
  // [26] bits 6-7 + [27] bit 7: pot_shuffle 0..3 defined. 4 (Subset) is the
  // reserved-until-implemented value (Phase 7) — reject so a future-version
  // share-string can't activate the not-yet-built tier (the kModeWeapons==2
  // precedent). The 3-bit field is already wide enough for it; only the value
  // is gated.
  if (s->pot_shuffle > kPotShuffle_All) return false;
  // [28] enemy_drop_checks: 0=off, 1=forced key drops, 2=dungeon enemies,
  // 3=all-enemy tier (generation requires a complete all-enemy registry).
  if (s->enemy_drop_checks > kEnemyDropChecks_All) return false;
  // [28] bits 2-3: souls_shuffle 0..2 defined (add-enemy-souls); 3 is
  // reserved-until-implemented (the kModeWeapons==2 precedent).
  if (s->souls_shuffle > kSoulsShuffle_BossesEnemies) return false;
  // [28] bit 4: npc_souls is a strict boolean (add-npc-souls).
  if (s->npc_souls > 1) return false;
  // [29] grass_shuffle bits 0-1 / rock_shuffle bits 2-3: TerrainShuffle enum,
  // 0..2 defined (add-rando-grass-rock-shuffle); 3 is reserved.
  if (s->grass_shuffle > kTerrainShuffle_All) return false;
  if (s->rock_shuffle > kTerrainShuffle_All) return false;
  // [29] bit 4: shopsanity is a strict boolean (add-rando-shopsanity).
  if (s->shopsanity > 1) return false;
  // [29] bits 5-6: bonk_shuffle TerrainShuffle 0..2 (add-rando-bonk-sanity).
  if (s->bonk_shuffle > kTerrainShuffle_All) return false;
  // [30] bits 0-1: requested key-ring mode; bit2: Skeleton Key item toggle.
  if (s->key_rings > kKeyRings_All || s->skeleton_key > 1) return false;
  // [30] bit 3: whirlpool_shuffle strict bool; bits 4-5: flute_shuffle 0..2
  // (add-rando-ow-warp-shuffle); value 3 is refused-undefined.
  if (s->whirlpool_shuffle > 1) return false;
  if (s->flute_shuffle > kFluteShuffle_Random) return false;
  return true;
}

uint8 Settings_EffectiveDoorShuffle(const RandoSettings *s) {
  // The normalized door_shuffle value — a PURE computation of the same MVP
  // pins apply_derived_rules enforces (which defers to this helper, so the
  // canonical byte [27] equals this by construction; it must NOT serialize,
  // since Settings_EffectiveSmallKeysMode consults it from inside
  // apply_derived_rules). Honored only on Open/Standard + NoGlitches, yields
  // to an entrance-shuffle request. Reads the RAW entrance axes: every state
  // where derived rules would normalize those axes off (Inverted/Retro) is
  // already excluded by the world-state check.
  if (s == NULL || s->door_shuffle == kDoorShuffle_Vanilla)
    return kDoorShuffle_Vanilla;
  if ((s->world_state != kWorldState_Open && s->world_state != kWorldState_Standard) ||
      s->logic != 0 /* NoGlitches */ ||
      s->shuffle_cave_entrances || s->shuffle_dungeon_entrances)
    return kDoorShuffle_Vanilla;
  return (uint8)(s->door_shuffle & kDoorShuffleAxis_Mask);
}

bool Settings_EffectiveDungeonChains(const RandoSettings *s) {
  if (s == NULL || !s->dungeon_chains)
    return false;
  if (s->world_state != kWorldState_Open && s->world_state != kWorldState_Standard)
    return false;
  if (s->logic != 0 /* NoGlitches */ || s->boss_shuffle)
    return false;
  if (s->door_shuffle != kDoorShuffle_Vanilla)
    return false;
  if (s->shuffle_cave_entrances || s->shuffle_dungeon_entrances ||
      s->cross_category || s->decoupled || s->shuffle_ganons_tower_entrance)
    return false;
  return true;
}

void Settings_ComputeHash(const RandoSettings *s, uint8 out_hash[32]) {
  uint8 buf[kSettingsCanonicalLen];
  Settings_CanonicalSerialize(s, buf);
  sha256_buffer(buf, kSettingsCanonicalLen, out_hash);
}

void Settings_HashShort(const RandoSettings *s, uint8 out_hash[16]) {
  uint8 full[32];
  Settings_ComputeHash(s, full);
  for (int i = 0; i < 16; ++i) out_hash[i] = full[i];
}

// ===========================================================================
// Self-test. Defaults serialize to a known byte sequence; the SHA-256 of
// that sequence is the reference value below.
// ===========================================================================
#include <stdio.h>
#include <stdlib.h>

static int settings_byte_eq(const uint8 *a, const uint8 *b, int n) {
  for (int i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
  return 1;
}

// add-rando-major-glitch D6 — see rando_settings.h for the contract. A seed
// whose placement assumed a restored JP-1.0 glitch must run with the JP-glitch
// runtime flag forced on. The fork `logic` enum is NoGlitches=0,
// OverworldGlitches=1, MajorGlitches=2, HybridMG=3, NoLogic=4 (rando_settings.c
// CSV parser); fake-flippers is tricks bit 1 (op_registry.yaml). Raw integers
// match the house style (the spoiler/parser also use `logic < 4`, `bit 1`).
bool Rando_SettingsAssumeJpGlitches(const RandoSettings *s) {
  enum { kLogic_OverworldGlitches = 1, kTrickBit_FakeFlippers = 1 };
  return s->logic >= kLogic_OverworldGlitches ||
         (s->tricks & (1u << kTrickBit_FakeFlippers)) != 0;
}

void Settings_SelfCheck(void) {
  RandoSettings s;
  Settings_SetDefaults(&s);

  uint8 canonical[kSettingsCanonicalLen];
  int n = Settings_CanonicalSerialize(&s, canonical);
  if (n != kSettingsCanonicalLen) {
    fprintf(stderr, "Settings_SelfCheck: serialize length mismatch\n");
    exit(2);
  }

  // Default-settings canonical bytes, layout per Settings_CanonicalSerialize:
  //   ws=0 goal=1 cg=7 ct=7 tricks=0 pool=1 logic=0 weapons=0 access=0
  //   bow=0 bossH=0 sk=0 bk=0 mp=0 cmp=0 prize=1 med=1 race=0
  //   pieces_req=20 (0x0014 LE) pieces_pl=30 (0x001e LE)
  //   hints=1 boss_shuffle=0 drop_shuffle=0 pad pad pad enemy_drop_checks=0
  static const uint8 kExpectedCanonical[kSettingsCanonicalLen] = {
    0x00, 0x01, 0x07, 0x07, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x01, 0x00, 0x14, 0x00, 0x1e, 0x00, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  if (!settings_byte_eq(canonical, kExpectedCanonical, kSettingsCanonicalLen)) {
    fprintf(stderr,
            "Settings_SelfCheck: default canonical bytes mismatch.\n"
            "  Expected: ");
    for (int i = 0; i < kSettingsCanonicalLen; ++i)
      fprintf(stderr, "%02x", kExpectedCanonical[i]);
    fprintf(stderr, "\n  Got:      ");
    for (int i = 0; i < kSettingsCanonicalLen; ++i)
      fprintf(stderr, "%02x", canonical[i]);
    fprintf(stderr, "\n  Either the defaults changed (bump kGeneratorVersion!) or\n"
                    "  the serializer drifted from the pinned layout.\n");
    exit(2);
  }

  uint8 hash[32];
  Settings_ComputeHash(&s, hash);
  // SHA-256 of the kExpectedCanonical bytes (31 bytes; hints=1 default —
  // re-pinned when key rings/Skeleton Key appended byte [30]).
  static const uint8 kExpectedHash[32] = {
    0xab, 0x22, 0x19, 0xd8, 0x32, 0xf2, 0xad, 0x1e,
    0xd3, 0x1f, 0x42, 0xd9, 0xf7, 0x24, 0x7b, 0x05,
    0xb5, 0x36, 0x5f, 0x8a, 0x20, 0x3e, 0x65, 0xee,
    0x0b, 0x82, 0x15, 0x1e, 0x63, 0xac, 0xc4, 0x0f,
  };
  if (!settings_byte_eq(hash, kExpectedHash, 32)) {
    fprintf(stderr,
            "Settings_SelfCheck: default settings_hash mismatch (SHA-256 broken?).\n");
    exit(2);
  }

  // Hints v2 exposes Balanced by name while preserving all historical value-1
  // spellings and the exact canonical byte they produced.
  {
    static const char *const aliases[] = {
      "hints=balanced", "hints=on", "hints=full", "hints=sahasrahla",
      "hints=true", "hints=True", "hints=1",
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
      RandoSettings parsed;
      Settings_SetDefaults(&parsed);
      // A profile is a complete base tuple, not merely an enabled bit.
      parsed.hint_tile_coverage = kHintTileCoverage_Standard;
      parsed.hint_mix = kHintMix_Difficult;
      parsed.hint_paid_depth = 0;
      if (Settings_ParseCsv(aliases[i], &parsed) != 0 ||
          parsed.hints != kHintsMode_Balanced ||
          parsed.hint_tile_coverage != kHintTileCoverage_Full ||
          parsed.hint_mix != kHintMix_Variety ||
          parsed.hint_paid_depth != 3) {
        fprintf(stderr,
                "Settings_SelfCheck: Balanced hint alias parse failed: %s\n",
                aliases[i]);
        exit(2);
      }
    }
    RandoSettings parsed;
    Settings_SetDefaults(&parsed);
    static const char *const off_aliases[] = {
      "hints=off", "hints=none", "hints=false", "hints=False", "hints=0",
    };
    for (size_t i = 0;
         i < sizeof(off_aliases) / sizeof(off_aliases[0]); i++) {
      Settings_SetDefaults(&parsed);
      parsed.hint_tile_coverage = kHintTileCoverage_Standard;
      parsed.hint_mix = kHintMix_Difficult;
      parsed.hint_paid_depth = 0;
      if (Settings_ParseCsv(off_aliases[i], &parsed) != 0 ||
          parsed.hints != kHintsMode_Off ||
          parsed.hint_tile_coverage != kHintTileCoverage_Full ||
          parsed.hint_mix != kHintMix_Variety ||
          parsed.hint_paid_depth != 3) {
        fprintf(stderr, "Settings_SelfCheck: Off hint alias parse failed: %s\n",
                off_aliases[i]);
        exit(2);
      }
    }
  }

  // Configurable hint policy: all 64 semantic combinations have a pinned
  // zero-default wire mapping, round-trip without touching unrelated bytes,
  // and the single empty delivery tuple collapses to true Off.
  {
    static const uint8 kTileCounts[4] = { 15, 0, 5, 10 };
    for (uint8 coverage = kHintTileCoverage_Full;
         coverage <= kHintTileCoverage_Standard; coverage++) {
      for (uint8 mix = kHintMix_Variety; mix <= kHintMix_WorldInfo; mix++) {
        for (uint8 depth = 0; depth <= 3; depth++) {
          RandoSettings policy, round;
          uint8 blob[kSettingsCanonicalLen];
          Settings_SetDefaults(&policy);
          policy.hint_tile_coverage = coverage;
          policy.hint_mix = mix;
          policy.hint_paid_depth = depth;
          Settings_CanonicalSerialize(&policy, blob);

          bool empty = coverage == kHintTileCoverage_None && depth == 0;
          uint8 normalized_coverage =
              empty ? kHintTileCoverage_Full : coverage;
          uint8 normalized_mix = empty ? kHintMix_Variety : mix;
          uint8 normalized_depth = empty ? 3 : depth;
          uint8 expected28 =
              (uint8)((normalized_coverage << kHintTileCoverageAxis_Shift) |
                      ((normalized_mix & 1u) ? kHintMixAxis_LowBit : 0));
          uint8 expected29 =
              (normalized_mix & 2u) ? kHintMixAxis_HighBit : 0;
          uint8 expected30 =
              (uint8)(((normalized_depth + 1u) & 3u) <<
                      kHintPaidDepthAxis_Shift);
          if (blob[22] != (empty ? kHintsMode_Off : kHintsMode_Balanced) ||
              blob[28] != expected28 || blob[29] != expected29 ||
              blob[30] != expected30) {
            fprintf(stderr,
                    "Settings_SelfCheck: hint policy wire mismatch "
                    "(coverage=%u mix=%u depth=%u: %02x/%02x/%02x/%02x)\n",
                    coverage, mix, depth, blob[22], blob[28], blob[29],
                    blob[30]);
            exit(2);
          }
          for (int i = 0; i < kSettingsCanonicalLen; i++) {
            if (i == 22 || i == 28 || i == 29 || i == 30) continue;
            if (blob[i] != kExpectedCanonical[i]) {
              fprintf(stderr,
                      "Settings_SelfCheck: hint policy changed unrelated "
                      "canonical byte [%d]\n",
                      i);
              exit(2);
            }
          }
          if (Settings_CanonicalDeserialize(blob, &round) != 0) {
            fprintf(stderr,
                    "Settings_SelfCheck: hint policy deserialize failed\n");
            exit(2);
          }
          if (empty) {
            if (round.hints != kHintsMode_Off ||
                round.hint_tile_coverage != kHintTileCoverage_Full ||
                round.hint_mix != kHintMix_Variety ||
                round.hint_paid_depth != 3 ||
                Settings_HintsEnabled(&round) ||
                Settings_HintTileCount(&round) != 0 ||
                Settings_EffectiveHintPaidDepth(&round) != 0) {
              fprintf(stderr,
                      "Settings_SelfCheck: empty hint policy did not "
                      "normalize to true Off\n");
              exit(2);
            }
          } else if (!Settings_HintsEnabled(&round) ||
                     round.hint_tile_coverage != coverage ||
                     round.hint_mix != mix ||
                     round.hint_paid_depth != depth ||
                     Settings_HintTileCount(&round) != kTileCounts[coverage] ||
                     Settings_EffectiveHintPaidDepth(&round) != depth) {
            fprintf(stderr,
                    "Settings_SelfCheck: hint policy semantic round-trip "
                    "mismatch\n");
            exit(2);
          }
        }
      }
    }

    // Hint bits coexist with every previously-defined bit in shared bytes
    // [28..30]; changing only the hint policy must preserve those neighbors.
    RandoSettings neighbors, custom, round;
    uint8 before[kSettingsCanonicalLen], after[kSettingsCanonicalLen];
    Settings_SetDefaults(&neighbors);
    neighbors.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    neighbors.enemy_drop_checks = kEnemyDropChecks_Keys;
    neighbors.souls_shuffle = kSoulsShuffle_Bosses;
    neighbors.npc_souls = 1;
    neighbors.grass_shuffle = kTerrainShuffle_All;
    neighbors.rock_shuffle = kTerrainShuffle_Junk;
    neighbors.shopsanity = 1;
    neighbors.bonk_shuffle = kTerrainShuffle_All;
    neighbors.key_rings = kKeyRings_Random;
    neighbors.skeleton_key = 1;
    neighbors.whirlpool_shuffle = 1;
    neighbors.flute_shuffle = kFluteShuffle_Random;
    Settings_CanonicalSerialize(&neighbors, before);
    custom = neighbors;
    custom.hint_tile_coverage = kHintTileCoverage_Standard;
    custom.hint_mix = kHintMix_WorldInfo;
    custom.hint_paid_depth = 0;
    Settings_CanonicalSerialize(&custom, after);
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 28 || i == 29 || i == 30) continue;
      if (before[i] != after[i]) {
        fprintf(stderr,
                "Settings_SelfCheck: hint policy disturbed canonical byte "
                "[%d]\n",
                i);
        exit(2);
      }
    }
    if ((before[28] & ~(uint8)(kHintTileCoverageAxis_Mask |
                               kHintMixAxis_LowBit)) !=
            (after[28] & ~(uint8)(kHintTileCoverageAxis_Mask |
                                  kHintMixAxis_LowBit)) ||
        (before[29] & ~(uint8)kHintMixAxis_HighBit) !=
            (after[29] & ~(uint8)kHintMixAxis_HighBit) ||
        (before[30] & ~(uint8)kHintPaidDepthAxis_Mask) !=
            (after[30] & ~(uint8)kHintPaidDepthAxis_Mask) ||
        Settings_CanonicalDeserialize(after, &round) != 0 ||
        round.enemy_drop_checks != kEnemyDropChecks_Keys ||
        round.souls_shuffle != kSoulsShuffle_Bosses || !round.npc_souls ||
        round.grass_shuffle != kTerrainShuffle_All ||
        round.rock_shuffle != kTerrainShuffle_Junk || !round.shopsanity ||
        round.bonk_shuffle != kTerrainShuffle_All ||
        round.key_rings != kKeyRings_Random || !round.skeleton_key ||
        !round.whirlpool_shuffle ||
        round.flute_shuffle != kFluteShuffle_Random ||
        round.hint_tile_coverage != kHintTileCoverage_Standard ||
        round.hint_mix != kHintMix_WorldInfo ||
        round.hint_paid_depth != 0) {
      fprintf(stderr,
              "Settings_SelfCheck: shared hint-policy bits failed coexistence "
              "round-trip\n");
      exit(2);
    }

    // CSV normalization is deliberately deferred until the complete string is
    // parsed, making Off independent of key order. The empty tuple is the same
    // canonical Off representation.
    static const char *const kOffOrders[] = {
      "hints=off,hint_tiles=sparse,hint_mix=important,hint_paid=1",
      "hint_paid=1,hint_mix=important,hint_tiles=sparse,hints=off",
      "hint_tile_coverage=none,hint_mix=difficult,hint_paid_depth=0",
    };
    for (size_t i = 0; i < sizeof(kOffOrders) / sizeof(kOffOrders[0]); i++) {
      RandoSettings parsed;
      uint8 parsed_blob[kSettingsCanonicalLen];
      Settings_SetDefaults(&parsed);
      if (Settings_ParseCsv(kOffOrders[i], &parsed) != 0 ||
          parsed.hints != kHintsMode_Off ||
          parsed.hint_tile_coverage != kHintTileCoverage_Full ||
          parsed.hint_mix != kHintMix_Variety ||
          parsed.hint_paid_depth != 3) {
        fprintf(stderr,
                "Settings_SelfCheck: hint CSV Off normalization failed\n");
        exit(2);
      }
      Settings_CanonicalSerialize(&parsed, parsed_blob);
      RandoSettings expected_off;
      uint8 expected_off_blob[kSettingsCanonicalLen];
      Settings_SetDefaults(&expected_off);
      expected_off.hints = kHintsMode_Off;
      Settings_CanonicalSerialize(&expected_off, expected_off_blob);
      if (!settings_byte_eq(parsed_blob, expected_off_blob,
                            kSettingsCanonicalLen)) {
        fprintf(stderr,
                "Settings_SelfCheck: hint CSV Off canonical mismatch\n");
        exit(2);
      }
    }

    static const struct {
      const char *csv;
      uint8 hints;
      uint8 coverage;
      uint8 mix;
      uint8 depth;
    } kProfiles[] = {
      { "hints=off", kHintsMode_Off, kHintTileCoverage_Full,
        kHintMix_Variety, 3 },
      { "hints=sparse", kHintsMode_Balanced, kHintTileCoverage_Sparse,
        kHintMix_Variety, 1 },
      { "hints=balanced", kHintsMode_Balanced, kHintTileCoverage_Full,
        kHintMix_Variety, 3 },
      { "hints=direct", kHintsMode_Balanced, kHintTileCoverage_Full,
        kHintMix_Important, 3 },
    };
    for (size_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) {
      RandoSettings profile;
      Settings_SetDefaults(&profile);
      profile.hint_tile_coverage = kHintTileCoverage_None;
      profile.hint_mix = kHintMix_WorldInfo;
      profile.hint_paid_depth = 2;
      if (Settings_ParseCsv(kProfiles[i].csv, &profile) != 0 ||
          profile.hints != kProfiles[i].hints ||
          profile.hint_tile_coverage != kProfiles[i].coverage ||
          profile.hint_mix != kProfiles[i].mix ||
          profile.hint_paid_depth != kProfiles[i].depth) {
        fprintf(stderr,
                "Settings_SelfCheck: hint profile mapping failed: %s\n",
                kProfiles[i].csv);
        exit(2);
      }
    }

    static const char *const kProfileNames[] = {
      "off", "sparse", "balanced", "direct", "custom",
    };
    static const char *const kProfileTitles[] = {
      "Off", "Sparse", "Balanced", "Direct", "Custom",
    };
    for (int i = kHintProfile_Off; i <= kHintProfile_Direct; i++) {
      RandoSettings applied;
      Settings_SetDefaults(&applied);
      applied.hint_tile_coverage = kHintTileCoverage_None;
      applied.hint_mix = kHintMix_WorldInfo;
      applied.hint_paid_depth = 2;
      if (!Settings_ApplyHintProfile(&applied, (HintProfile)i) ||
          Settings_GetHintProfile(&applied) != (HintProfile)i ||
          strcmp(Settings_HintProfileName((HintProfile)i),
                 kProfileNames[i]) != 0 ||
          strcmp(Settings_HintProfileTitle((HintProfile)i),
                 kProfileTitles[i]) != 0) {
        fprintf(stderr,
                "Settings_SelfCheck: centralized hint profile API failed "
                "for %d\n",
                i);
        exit(2);
      }
    }
    RandoSettings custom_profile, custom_before;
    memset(&custom_profile, 0xa5, sizeof(custom_profile));
    Settings_SetDefaults(&custom_profile);
    custom_profile.hint_tile_coverage = kHintTileCoverage_Standard;
    custom_profile.hint_mix = kHintMix_Difficult;
    custom_profile.hint_paid_depth = 2;
    custom_before = custom_profile;
    if (Settings_GetHintProfile(&custom_profile) != kHintProfile_Custom ||
        Settings_ApplyHintProfile(&custom_profile, kHintProfile_Custom) ||
        memcmp(&custom_profile, &custom_before, sizeof(custom_profile)) != 0 ||
        Settings_ApplyHintProfile(&custom_profile, (HintProfile)99) ||
        memcmp(&custom_profile, &custom_before, sizeof(custom_profile)) != 0 ||
        Settings_ApplyHintProfile(NULL, kHintProfile_Balanced) ||
        Settings_GetHintProfile(NULL) != kHintProfile_Off ||
        strcmp(Settings_HintProfileName(kHintProfile_Custom), "custom") != 0 ||
        strcmp(Settings_HintProfileTitle(kHintProfile_Custom), "Custom") != 0 ||
        strcmp(Settings_HintProfileName((HintProfile)99), "(unknown)") != 0 ||
        strcmp(Settings_HintProfileTitle((HintProfile)99), "(Unknown)") != 0) {
      fprintf(stderr,
              "Settings_SelfCheck: Custom/invalid hint profile API contract "
              "failed\n");
      exit(2);
    }

    static const struct {
      const char *csv;
      uint8 coverage;
    } kTileAliases[] = {
      { "hint_tiles=few", kHintTileCoverage_Sparse },
      { "hint_tiles=many", kHintTileCoverage_Standard },
      { "hint_tiles=all", kHintTileCoverage_Full },
    };
    for (size_t i = 0;
         i < sizeof(kTileAliases) / sizeof(kTileAliases[0]); i++) {
      RandoSettings tiles;
      Settings_SetDefaults(&tiles);
      if (Settings_ParseCsv(kTileAliases[i].csv, &tiles) != 0 ||
          tiles.hint_tile_coverage != kTileAliases[i].coverage) {
        fprintf(stderr,
                "Settings_SelfCheck: hint tile alias failed: %s\n",
                kTileAliases[i].csv);
        exit(2);
      }
    }

    static const char *const kOverrideOrders[] = {
      "hints=direct,hint_tiles=standard,hint_paid=2,hint_mix=difficult",
      "hint_mix=difficult,hint_paid=2,hint_tiles=standard,hints=direct",
    };
    for (size_t i = 0;
         i < sizeof(kOverrideOrders) / sizeof(kOverrideOrders[0]); i++) {
      RandoSettings ordered;
      Settings_SetDefaults(&ordered);
      if (Settings_ParseCsv(kOverrideOrders[i], &ordered) != 0 ||
          ordered.hints != kHintsMode_Balanced ||
          ordered.hint_tile_coverage != kHintTileCoverage_Standard ||
          ordered.hint_mix != kHintMix_Difficult ||
          ordered.hint_paid_depth != 2) {
        fprintf(stderr,
                "Settings_SelfCheck: hint profile/component order drift: %s\n",
                kOverrideOrders[i]);
        exit(2);
      }
    }

    RandoSettings parsed;
    Settings_SetDefaults(&parsed);
    if (Settings_ParseCsv("hint_tiles=standard,hint_mix=world-info,"
                          "hint_paid=2",
                          &parsed) != 0 ||
        parsed.hint_tile_coverage != kHintTileCoverage_Standard ||
        parsed.hint_mix != kHintMix_WorldInfo ||
        parsed.hint_paid_depth != 2 ||
        Settings_HintTileCount(&parsed) != 10 ||
        Settings_EffectiveHintPaidDepth(&parsed) != 2) {
      fprintf(stderr,
              "Settings_SelfCheck: configurable hint CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&parsed);
    if (Settings_ParseCsv("hint_tile_coverage=standard,hint_mix=world_info,"
                          "hint_paid_depth=2",
                          &parsed) != 0 ||
        parsed.hint_tile_coverage != kHintTileCoverage_Standard ||
        parsed.hint_mix != kHintMix_WorldInfo ||
        parsed.hint_paid_depth != 2) {
      fprintf(stderr,
              "Settings_SelfCheck: long hint CSV aliases failed\n");
      exit(2);
    }

    static const char *const kDuplicatePolicyKeys[] = {
      "hint_tile_coverage=full,hint_tile_coverage=sparse",
      "hint_tiles=full,hint_tile_coverage=sparse",
      "hint_mix=variety,hint_mix=important",
      "hint_paid_depth=3,hint_paid_depth=2",
      "hint_paid=3,hint_paid_depth=2",
      "hints=sparse,hints=direct",
    };
    for (size_t i = 0;
         i < sizeof(kDuplicatePolicyKeys) / sizeof(kDuplicatePolicyKeys[0]);
         i++) {
      Settings_SetDefaults(&parsed);
      if (Settings_ParseCsv(kDuplicatePolicyKeys[i], &parsed) == 0) {
        fprintf(stderr,
                "Settings_SelfCheck: duplicate hint policy key accepted\n");
        exit(2);
      }
    }
    static const char *const kBadPolicyValues[] = {
      "hints=custom",
      "hint_tiles=some",
      "hint_tile_coverage=some",
      "hint_mix=cryptic",
      "hint_paid=4",
      "hint_paid_depth=4",
    };
    for (size_t i = 0;
         i < sizeof(kBadPolicyValues) / sizeof(kBadPolicyValues[0]); i++) {
      Settings_SetDefaults(&parsed);
      if (Settings_ParseCsv(kBadPolicyValues[i], &parsed) == 0) {
        fprintf(stderr,
                "Settings_SelfCheck: bad hint policy value accepted\n");
        exit(2);
      }
    }

    Settings_SetDefaults(&parsed);
    parsed.hint_tile_coverage = (uint8)(kHintTileCoverage_Standard + 1);
    if (Settings_Validate(&parsed)) {
      fprintf(stderr,
              "Settings_SelfCheck: invalid hint coverage passed validation\n");
      exit(2);
    }
    Settings_SetDefaults(&parsed);
    parsed.hint_mix = (uint8)(kHintMix_WorldInfo + 1);
    if (Settings_Validate(&parsed)) {
      fprintf(stderr,
              "Settings_SelfCheck: invalid hint mix passed validation\n");
      exit(2);
    }
    Settings_SetDefaults(&parsed);
    parsed.hint_paid_depth = 4;
    if (Settings_Validate(&parsed)) {
      fprintf(stderr,
              "Settings_SelfCheck: invalid hint paid depth passed validation\n");
      exit(2);
    }

    // NPC reward disclosure is an independent, zero-default bool. It occupies
    // only canonical [25] bit 7, round-trips in both states, survives every
    // clue profile/normalization operation, and has one duplicate identity
    // across all CLI spellings.
    RandoSettings npc_default, npc_policy, npc_round;
    uint8 npc_default_blob[kSettingsCanonicalLen];
    uint8 npc_policy_blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&npc_default);
    if (npc_default.hint_npc_reward_reveal != 0) {
      fprintf(stderr,
              "Settings_SelfCheck: NPC reward disclosure must default off\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&npc_default, npc_default_blob);
    for (uint8 reveal = 0; reveal <= 1; reveal++) {
      npc_policy = npc_default;
      npc_policy.hint_npc_reward_reveal = reveal;
      Settings_CanonicalSerialize(&npc_policy, npc_policy_blob);
      for (int i = 0; i < kSettingsCanonicalLen; i++) {
        uint8 expected = npc_default_blob[i];
        if (i == 25 && reveal)
          expected |= kHintNpcRewardRevealAxis_Enabled;
        if (npc_policy_blob[i] != expected) {
          fprintf(stderr,
                  "Settings_SelfCheck: NPC reward disclosure changed "
                  "canonical byte [%d] (%u: %02x != %02x)\n",
                  i, reveal, npc_policy_blob[i], expected);
          exit(2);
        }
      }
      if (Settings_CanonicalDeserialize(npc_policy_blob, &npc_round) != 0 ||
          npc_round.hint_npc_reward_reveal != reveal) {
        fprintf(stderr,
                "Settings_SelfCheck: NPC reward disclosure round-trip "
                "failed for state %u\n",
                reveal);
        exit(2);
      }
    }

    // Exercise coexistence with every neighboring [25] bit. Coupled and
    // decoupled are mutually exclusive after normalization, so cover both
    // legal packed forms.
    npc_policy = npc_default;
    npc_policy.shuffle_cave_entrances = 1;
    npc_policy.shuffle_dungeon_entrances = 1;
    npc_policy.coupled = 1;
    npc_policy.cross_category = 1;
    npc_policy.shuffle_ganons_tower_entrance = 1;
    npc_policy.hint_npc_reward_reveal = 1;
    Settings_CanonicalSerialize(&npc_policy, npc_policy_blob);
    if (npc_policy_blob[25] != 0xaf ||
        Settings_CanonicalDeserialize(npc_policy_blob, &npc_round) != 0 ||
        !npc_round.shuffle_cave_entrances ||
        !npc_round.shuffle_dungeon_entrances || !npc_round.coupled ||
        !npc_round.cross_category || npc_round.decoupled ||
        !npc_round.shuffle_ganons_tower_entrance ||
        npc_round.dungeon_chains || !npc_round.hint_npc_reward_reveal) {
      fprintf(stderr,
              "Settings_SelfCheck: NPC reward disclosure failed [25] "
              "coupled coexistence round-trip\n");
      exit(2);
    }
    npc_policy.decoupled = 1;
    Settings_CanonicalSerialize(&npc_policy, npc_policy_blob);
    if (npc_policy_blob[25] != 0xbb ||
        Settings_CanonicalDeserialize(npc_policy_blob, &npc_round) != 0 ||
        npc_round.coupled || !npc_round.decoupled ||
        !npc_round.hint_npc_reward_reveal) {
      fprintf(stderr,
              "Settings_SelfCheck: NPC reward disclosure failed [25] "
              "decoupled coexistence round-trip\n");
      exit(2);
    }
    npc_policy = npc_default;
    npc_policy.dungeon_chains = 1;
    npc_policy.hint_npc_reward_reveal = 1;
    Settings_CanonicalSerialize(&npc_policy, npc_policy_blob);
    if (npc_policy_blob[25] !=
            (kEntranceAxis_DungeonChains |
             kHintNpcRewardRevealAxis_Enabled) ||
        Settings_CanonicalDeserialize(npc_policy_blob, &npc_round) != 0 ||
        !npc_round.dungeon_chains || !npc_round.hint_npc_reward_reveal) {
      fprintf(stderr,
              "Settings_SelfCheck: NPC reward disclosure failed [25] "
              "dungeon-chain coexistence round-trip\n");
      exit(2);
    }

    for (int profile = kHintProfile_Off;
         profile <= kHintProfile_Direct; profile++) {
      Settings_SetDefaults(&npc_policy);
      npc_policy.hint_npc_reward_reveal = 1;
      if (!Settings_ApplyHintProfile(&npc_policy, (HintProfile)profile) ||
          Settings_GetHintProfile(&npc_policy) != (HintProfile)profile ||
          npc_policy.hint_npc_reward_reveal != 1) {
        fprintf(stderr,
                "Settings_SelfCheck: hint profile %d changed independent "
                "NPC reward disclosure\n",
                profile);
        exit(2);
      }
    }
    Settings_SetDefaults(&npc_policy);
    npc_policy.hint_npc_reward_reveal = 1;
    npc_policy.hints = kHintsMode_Off;
    Settings_NormalizeHintPolicy(&npc_policy);
    if (npc_policy.hint_npc_reward_reveal != 1 ||
        Settings_GetHintProfile(&npc_policy) != kHintProfile_Off) {
      fprintf(stderr,
              "Settings_SelfCheck: Off normalization changed independent "
              "NPC reward disclosure\n");
      exit(2);
    }

    static const struct {
      const char *csv;
      uint8 expected;
    } kNpcRewardAliases[] = {
      { "hint_npc_reward_reveal=true", 1 },
      { "hint_npc_rewards=true", 1 },
      { "hint_vendor_items=true", 1 },
      { "hint_npc_items=true", 1 },
      { "hint_npc_reward_reveal=false", 0 },
      { "hint_npc_rewards=false", 0 },
      { "hint_vendor_items=false", 0 },
      { "hint_npc_items=false", 0 },
    };
    for (size_t i = 0;
         i < sizeof(kNpcRewardAliases) / sizeof(kNpcRewardAliases[0]); i++) {
      Settings_SetDefaults(&npc_policy);
      if (Settings_ParseCsv(kNpcRewardAliases[i].csv, &npc_policy) != 0 ||
          npc_policy.hint_npc_reward_reveal !=
              kNpcRewardAliases[i].expected) {
        fprintf(stderr,
                "Settings_SelfCheck: NPC reward disclosure alias failed: "
                "%s\n",
                kNpcRewardAliases[i].csv);
        exit(2);
      }
    }
    static const char *const kNpcRewardDuplicates[] = {
      "hint_npc_reward_reveal=true,hint_npc_reward_reveal=false",
      "hint_npc_reward_reveal=true,hint_npc_rewards=false",
      "hint_npc_rewards=true,hint_npc_reward_reveal=false",
      "hint_npc_rewards=true,hint_npc_rewards=false",
      "hint_npc_rewards=true,hint_vendor_items=false",
      "hint_vendor_items=true,hint_npc_rewards=false",
      "hint_vendor_items=true,hint_npc_items=false",
      "hint_npc_items=true,hint_vendor_items=false",
      "hint_npc_items=true,hint_npc_rewards=false",
    };
    for (size_t i = 0;
         i < sizeof(kNpcRewardDuplicates) / sizeof(kNpcRewardDuplicates[0]);
         i++) {
      Settings_SetDefaults(&npc_policy);
      if (Settings_ParseCsv(kNpcRewardDuplicates[i], &npc_policy) == 0) {
        fprintf(stderr,
                "Settings_SelfCheck: duplicate NPC reward disclosure key "
                "accepted\n");
        exit(2);
      }
    }
    Settings_SetDefaults(&npc_policy);
    if (Settings_ParseCsv("hint_npc_reward_reveal=on", &npc_policy) == 0) {
      fprintf(stderr,
              "Settings_SelfCheck: invalid NPC reward disclosure bool "
              "accepted\n");
      exit(2);
    }
    Settings_SetDefaults(&npc_policy);
    npc_policy.hint_npc_reward_reveal = 2;
    if (Settings_Validate(&npc_policy)) {
      fprintf(stderr,
              "Settings_SelfCheck: invalid NPC reward disclosure bool "
              "passed validation\n");
      exit(2);
    }
  }

  // Key rings preserve the requested mode even while Vanilla/Retro semantics
  // make them ineffective. Skeleton Key is independent; configurable hints
  // now consume the former [30] tail bits without disturbing either axis.
  {
    RandoSettings kr, round;
    uint8 blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&kr);
    kr.key_rings = kKeyRings_All;
    kr.skeleton_key = 1;
    if (Settings_EffectiveKeyRings(&kr) != kKeyRings_Off) {
      fprintf(stderr, "Settings_SelfCheck: Vanilla small keys must disable key rings\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&kr, blob);
    if (blob[30] != (uint8)((uint8)kKeyRings_All |
                            (uint8)kSkeletonKeyAxis_Enabled) ||
        Settings_CanonicalDeserialize(blob, &round) != 0 ||
        round.key_rings != kKeyRings_All || !round.skeleton_key) {
      fprintf(stderr, "Settings_SelfCheck: requested key rings/Skeleton Key round-trip mismatch\n");
      exit(2);
    }
    kr.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    if (Settings_EffectiveKeyRings(&kr) != kKeyRings_All) {
      fprintf(stderr, "Settings_SelfCheck: Wild small keys must enable requested key rings\n");
      exit(2);
    }
    kr.world_state = kWorldState_Retro;
    if (Settings_EffectiveKeyRings(&kr) != kKeyRings_Off) {
      fprintf(stderr, "Settings_SelfCheck: Retro Generic Keys must disable key rings\n");
      exit(2);
    }
    // Flute enum value 3 ([30] bits 4-5 both set) must be refused, not
    // aliased.
    blob[30] |= (uint8)kFluteShuffleAxis_Mask;
    if (Settings_CanonicalDeserialize(blob, &round) == 0) {
      fprintf(stderr, "Settings_SelfCheck: flute_shuffle value 3 must be refused\n");
      exit(2);
    }
    blob[30] = (uint8)(blob[30] & ~(uint8)kFluteShuffleAxis_Mask);
    // Round-trip the defined warp bits.
    blob[30] |= (uint8)(kWhirlpoolAxis_Enabled |
                        (kFluteShuffle_Random << kFluteShuffleAxis_Shift));
    if (Settings_CanonicalDeserialize(blob, &round) != 0 ||
        round.whirlpool_shuffle != 1 ||
        round.flute_shuffle != kFluteShuffle_Random) {
      fprintf(stderr, "Settings_SelfCheck: warp-axis [30] round-trip failed\n");
      exit(2);
    }
    blob[30] = 0;  // restore the defaults blob for any later probe
    Settings_SetDefaults(&kr);
    if (Settings_ParseCsv("key_rings=random,skeleton_key=true", &kr) != 0 ||
        kr.key_rings != kKeyRings_Random || !kr.skeleton_key) {
      fprintf(stderr, "Settings_SelfCheck: key-rings CSV parse mismatch\n");
      exit(2);
    }
  }

  // Shopsanity occupies [29] bit 4 with no length change: a default blob is
  // bit-identical to the pre-shopsanity build, the enabled bit round-trips
  // alongside the terrain, bonk, and hint-mix axes.
  {
    RandoSettings sh, round;
    uint8 blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&sh);
    Settings_CanonicalSerialize(&sh, blob);
    if (blob[29] != 0) {
      fprintf(stderr, "Settings_SelfCheck: default [29] must stay 0x00 (shopsanity off)\n");
      exit(2);
    }
    sh.shopsanity = 1;
    sh.grass_shuffle = kTerrainShuffle_All;
    Settings_CanonicalSerialize(&sh, blob);
    if (blob[29] != (uint8)((kTerrainShuffle_All << kGrassShuffleAxis_Shift) |
                            kShopsanityAxis_Enabled) ||
        Settings_CanonicalDeserialize(blob, &round) != 0 ||
        round.shopsanity != 1 || round.grass_shuffle != kTerrainShuffle_All) {
      fprintf(stderr, "Settings_SelfCheck: shopsanity round-trip mismatch\n");
      exit(2);
    }
    Settings_SetDefaults(&sh);
    if (Settings_ParseCsv("shopsanity=true", &sh) != 0 || sh.shopsanity != 1) {
      fprintf(stderr, "Settings_SelfCheck: shopsanity CSV parse mismatch\n");
      exit(2);
    }
  }

  // add-rando-random-crystals — the requested sentinel (8) round-trips in
  // bytes [2]/[3], the CSV `random` keyword parses, and 9+ stays refused.
  {
    RandoSettings cr, round;
    uint8 blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&cr);
    cr.crystals_ganon = kCrystalsRandom;
    cr.crystals_tower = 5;
    Settings_CanonicalSerialize(&cr, blob);
    if (blob[2] != kCrystalsRandom || blob[3] != 5 ||
        Settings_CanonicalDeserialize(blob, &round) != 0 ||
        round.crystals_ganon != kCrystalsRandom || round.crystals_tower != 5) {
      fprintf(stderr, "Settings_SelfCheck: crystals sentinel round-trip mismatch\n");
      exit(2);
    }
    Settings_SetDefaults(&cr);
    if (Settings_ParseCsv("crystals.ganon=random,crystals.tower=random", &cr) != 0 ||
        cr.crystals_ganon != kCrystalsRandom ||
        cr.crystals_tower != kCrystalsRandom) {
      fprintf(stderr, "Settings_SelfCheck: crystals `random` CSV parse mismatch\n");
      exit(2);
    }
    Settings_SetDefaults(&cr);
    if (Settings_ParseCsv("crystals.ganon=9", &cr) == 0) {
      fprintf(stderr, "Settings_SelfCheck: crystals.ganon=9 must be refused\n");
      exit(2);
    }
    // Numeric 8 stays refused on the CSV surface (keyword-only sentinel; the
    // legacy vector above also pins this).
    Settings_SetDefaults(&cr);
    if (Settings_ParseCsv("crystals.tower=8", &cr) == 0) {
      fprintf(stderr, "Settings_SelfCheck: numeric crystals.tower=8 must be refused\n");
      exit(2);
    }
  }

  // add-rando-bonk-sanity — [29] bits 5-6 round-trip alongside the other
  // byte-29 axes, and the CSV tier keyword parses.
  {
    RandoSettings bk, round;
    uint8 blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&bk);
    bk.bonk_shuffle = kTerrainShuffle_All;
    bk.shopsanity = 1;
    bk.grass_shuffle = kTerrainShuffle_Junk;
    Settings_CanonicalSerialize(&bk, blob);
    if (blob[29] != (uint8)((kTerrainShuffle_Junk << kGrassShuffleAxis_Shift) |
                            kShopsanityAxis_Enabled |
                            (kTerrainShuffle_All << kBonkShuffleAxis_Shift)) ||
        Settings_CanonicalDeserialize(blob, &round) != 0 ||
        round.bonk_shuffle != kTerrainShuffle_All || round.shopsanity != 1 ||
        round.grass_shuffle != kTerrainShuffle_Junk) {
      fprintf(stderr, "Settings_SelfCheck: bonk_shuffle round-trip mismatch\n");
      exit(2);
    }
    Settings_SetDefaults(&bk);
    if (Settings_ParseCsv("bonk_shuffle=junk", &bk) != 0 ||
        bk.bonk_shuffle != kTerrainShuffle_Junk) {
      fprintf(stderr, "Settings_SelfCheck: bonk_shuffle CSV parse mismatch\n");
      exit(2);
    }
  }

  // A pre-dungeon-chains/default canonical blob has [25] bit6 clear and must
  // round-trip unchanged.
  {
    RandoSettings pre_axis;
    if (Settings_CanonicalDeserialize(kExpectedCanonical, &pre_axis) != 0 ||
        pre_axis.dungeon_chains != 0) {
      fprintf(stderr, "Settings_SelfCheck: pre-chain canonical blob should "
                      "deserialize with dungeon_chains off\n");
      exit(2);
    }
    uint8 pre_axis_round[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&pre_axis, pre_axis_round);
    if (!settings_byte_eq(pre_axis_round, kExpectedCanonical, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: pre-chain canonical blob round-trip drifted\n");
      exit(2);
    }
  }

  // CSV parser round-trip: defaults → CSV → defaults.
  {
    RandoSettings s2;
    Settings_SetDefaults(&s2);
    int rc = Settings_ParseCsv("mode.state=open,goal=fast_ganon,crystals.ganon=7,"
                               "crystals.tower=7,prize_shuffle=true", &s2);
    if (rc != 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser failed on valid input rc=%d\n", rc);
      exit(2);
    }
    uint8 c2[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&s2, c2);
    if (!settings_byte_eq(canonical, c2, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: CSV-parsed defaults serialize differently\n");
      exit(2);
    }
  }
  // Retired legacy axes: old CSV/share inputs may carry the former pinned
  // boss-heart value, but canonical settings must normalize it to shuffled.
  {
    RandoSettings legacy;
    Settings_SetDefaults(&legacy);
    legacy.region_boss_hearts_in_pool = 1;
    uint8 c_legacy[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&legacy, c_legacy);
    if (!settings_byte_eq(canonical, c_legacy, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: legacy boss-heart byte did not normalize\n");
      exit(2);
    }
  }
  // add-rando-trap-catalog — Insanity (traps=4) round-trips via the non-contiguous
  // 3-bit field (canonical [26] bits 2-3 + bit5), and `high` stays bits-2-3-only so
  // existing off/low/medium/high seeds are byte-stable.
  {
    RandoSettings si;
    Settings_SetDefaults(&si);
    si.traps = kTrapFrequency_Insanity;
    uint8 ci[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&si, ci);
    if (!(ci[26] & kTrapAxis_HighBit) || (ci[26] & kTrapAxis_Mask)) {
      fprintf(stderr, "Settings_SelfCheck: Insanity must set [26] bit5 only (got 0x%02x)\n", ci[26]);
      exit(2);
    }
    RandoSettings ri;
    if (Settings_CanonicalDeserialize(ci, &ri) != 0 || ri.traps != kTrapFrequency_Insanity) {
      fprintf(stderr, "Settings_SelfCheck: Insanity traps round-trip mismatch\n");
      exit(2);
    }
    RandoSettings sh;
    Settings_SetDefaults(&sh);
    sh.traps = kTrapFrequency_High;
    uint8 chh[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sh, chh);
    if ((chh[26] & kTrapAxis_HighBit) ||
        ((chh[26] & kTrapAxis_Mask) >> kTrapAxis_Shift) != kTrapFrequency_High) {
      fprintf(stderr, "Settings_SelfCheck: high traps must encode in bits 2-3 only (got 0x%02x)\n",
              chh[26]);
      exit(2);
    }
  }
  // add-rando-trap-catalog — trap_categories pack/unpack round-trip (canonical
  // [27] bits 2-6). The corpus only exercises the default mask 0, so assert here
  // that a partial mask round-trips losslessly and that the "all" forms collapse
  // to the 0 sentinel (one canonical encoding of "all categories").
  {
    RandoSettings st;
    Settings_SetDefaults(&st);
    st.traps = kTrapFrequency_High;
    st.trap_categories = (uint8)(kTrapCategory_Hazard | kTrapCategory_Drain);  // 0x05
    uint8 ct[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&st, ct);
    if ((uint8)((ct[27] & kTrapCategoriesAxis_Mask) >> kTrapCategoriesAxis_Shift) !=
        (uint8)(kTrapCategory_Hazard | kTrapCategory_Drain)) {
      fprintf(stderr, "Settings_SelfCheck: trap_categories pack mismatch ([27]=0x%02x)\n",
              ct[27]);
      exit(2);
    }
    RandoSettings rtc;
    if (Settings_CanonicalDeserialize(ct, &rtc) != 0 ||
        rtc.trap_categories != (uint8)(kTrapCategory_Hazard | kTrapCategory_Drain)) {
      fprintf(stderr, "Settings_SelfCheck: trap_categories deserialize round-trip mismatch\n");
      exit(2);
    }
    RandoSettings sa;
    Settings_SetDefaults(&sa);
    if (Settings_ParseCsv("traps=high,trap_categories=hazard+impair+drain+scare+displace", &sa) != 0 ||
        sa.trap_categories != 0) {
      fprintf(stderr, "Settings_SelfCheck: trap_categories all-checked must collapse to 0\n");
      exit(2);
    }
    RandoSettings sp;
    Settings_SetDefaults(&sp);
    if (Settings_ParseCsv("traps=high,trap_categories=hazard+drain", &sp) != 0 ||
        sp.trap_categories != (uint8)(kTrapCategory_Hazard | kTrapCategory_Drain)) {
      fprintf(stderr, "Settings_SelfCheck: trap_categories CSV parse mismatch\n");
      exit(2);
    }
  }
  // Phase C — entrance-axis pack/unpack round-trip + default byte-identity.
  {
    // Default settings MUST still pack canonical byte [25] = 0 (the corpus
    // byte-identical invariant — kExpectedCanonical[25]==0 above depends on it).
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if (cd[25] != 0) {
      fprintf(stderr, "Settings_SelfCheck: default entrance-axis byte [25]=0x%02x "
                      "!= 0 (corpus invariant broken)\n", cd[25]);
      exit(2);
    }
    // coupled set but NO shuffle axis active must normalize to 0 (so the
    // default and "coupled with nothing to couple" hash identically).
    RandoSettings sc;
    Settings_SetDefaults(&sc);
    sc.coupled = 1;
    uint8 cc[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sc, cc);
    if (cc[25] != 0) {
      fprintf(stderr, "Settings_SelfCheck: coupled-without-shuffle must "
                      "normalize byte [25] to 0 (got 0x%02x)\n", cc[25]);
      exit(2);
    }
    // A coupled cave-shuffle config packs to the expected bits and round-trips.
    RandoSettings se;
    Settings_SetDefaults(&se);
    se.shuffle_cave_entrances = 1;
    se.coupled = 1;
    uint8 ce[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&se, ce);
    if (ce[25] != (kEntranceAxis_ShuffleCaves | kEntranceAxis_Coupled)) {
      fprintf(stderr, "Settings_SelfCheck: entrance-axis pack mismatch "
                      "(got 0x%02x)\n", ce[25]);
      exit(2);
    }
    RandoSettings rt;
    if (Settings_CanonicalDeserialize(ce, &rt) != 0 ||
        rt.shuffle_cave_entrances != 1 || rt.coupled != 1 ||
        rt.shuffle_dungeon_entrances != 0 || rt.cross_category != 0 ||
        rt.decoupled != 0) {
      fprintf(stderr, "Settings_SelfCheck: entrance-axis deserialize round-trip "
                      "mismatch\n");
      exit(2);
    }
    // Ganon's Tower opt-in (bit 5) round-trips; it requires dungeon shuffle so it
    // normalizes off without it.
    RandoSettings sg;
    Settings_SetDefaults(&sg);
    sg.shuffle_dungeon_entrances = 1; sg.shuffle_ganons_tower_entrance = 1;
    uint8 cg[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sg, cg);
    RandoSettings rg;
    if (!(cg[25] & kEntranceAxis_ShuffleGanonsTower) ||
        Settings_CanonicalDeserialize(cg, &rg) != 0 ||
        rg.shuffle_ganons_tower_entrance != 1) {
      fprintf(stderr, "Settings_SelfCheck: GT-entrance axis round-trip mismatch\n");
      exit(2);
    }
    // GT opt-in without dungeon shuffle must normalize off (bit 5 clear).
    RandoSettings sgn;
    Settings_SetDefaults(&sgn);
    sgn.shuffle_ganons_tower_entrance = 1;  // but shuffle_dungeon_entrances = 0
    uint8 cgn[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sgn, cgn);
    if (cgn[25] != 0) {
      fprintf(stderr, "Settings_SelfCheck: GT opt-in without dungeon shuffle must "
                      "normalize byte [25] to 0 (got 0x%02x)\n", cgn[25]);
      exit(2);
    }
    // Dungeon chains own bit 6 and normalize one-directionally under conflicts.
    RandoSettings sch;
    Settings_SetDefaults(&sch);
    sch.dungeon_chains = 1;
    uint8 cch[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sch, cch);
    if (cch[25] != kEntranceAxis_DungeonChains) {
      fprintf(stderr, "Settings_SelfCheck: dungeon_chains pack mismatch "
                      "(got 0x%02x)\n", cch[25]);
      exit(2);
    }
    if (cch[11] != kDungeonItemMode_Dungeon ||
        cch[12] != kDungeonItemMode_Dungeon ||
        Settings_EffectiveSmallKeysMode(&sch) != kDungeonItemMode_Dungeon ||
        Settings_EffectiveBigKeysMode(&sch) != kDungeonItemMode_Dungeon) {
      fprintf(stderr, "Settings_SelfCheck: dungeon_chains must force dungeon "
                      "small/big keys\n");
      exit(2);
    }
    RandoSettings rch;
    if (Settings_CanonicalDeserialize(cch, &rch) != 0 || rch.dungeon_chains != 1) {
      fprintf(stderr, "Settings_SelfCheck: dungeon_chains deserialize round-trip "
                      "mismatch\n");
      exit(2);
    }
    RandoSettings svch;
    Settings_SetDefaults(&svch);
    if (Settings_ParseCsv("dungeon_chains=true", &svch) != 0 ||
        svch.dungeon_chains != 1) {
      fprintf(stderr, "Settings_SelfCheck: CSV parse of dungeon_chains failed\n");
      exit(2);
    }
    uint8 cvch[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&svch, cvch);
    if (!settings_byte_eq(cvch, cch, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: CSV-parsed dungeon_chains serializes "
                      "differently from the struct path\n");
      exit(2);
    }
    typedef struct {
      const char *name;
      uint8 world_state;
      uint8 logic;
      uint8 boss_shuffle;
      uint8 door_shuffle;
      uint8 shuffle_cave_entrances;
      uint8 shuffle_dungeon_entrances;
      uint8 expect_active;
    } ChainNormCase;
    static const ChainNormCase kChainNormCases[] = {
      { "open",             kWorldState_Open,     0, 0, kDoorShuffle_Vanilla, 0, 0, 1 },
      { "standard",         kWorldState_Standard, 0, 0, kDoorShuffle_Vanilla, 0, 0, 1 },
      { "inverted",         kWorldState_Inverted, 0, 0, kDoorShuffle_Vanilla, 0, 0, 0 },
      { "retro",            kWorldState_Retro,    0, 0, kDoorShuffle_Vanilla, 0, 0, 0 },
      { "glitched-logic",   kWorldState_Open,     1, 0, kDoorShuffle_Vanilla, 0, 0, 0 },
      { "boss-shuffle",     kWorldState_Open,     0, 1, kDoorShuffle_Vanilla, 0, 0, 0 },
      { "door-shuffle",     kWorldState_Open,     0, 0, kDoorShuffle_Basic,   0, 0, 0 },
      { "cave-entrance",    kWorldState_Open,     0, 0, kDoorShuffle_Vanilla, 1, 0, 0 },
      { "dungeon-entrance", kWorldState_Open,     0, 0, kDoorShuffle_Vanilla, 0, 1, 0 },
    };
    for (size_t i = 0; i < sizeof(kChainNormCases) / sizeof(kChainNormCases[0]); i++) {
      const ChainNormCase *tc = &kChainNormCases[i];
      RandoSettings sx;
      Settings_SetDefaults(&sx);
      sx.dungeon_chains = 1;
      sx.world_state = tc->world_state;
      sx.logic = tc->logic;
      sx.boss_shuffle = tc->boss_shuffle;
      sx.door_shuffle = tc->door_shuffle;
      sx.shuffle_cave_entrances = tc->shuffle_cave_entrances;
      sx.shuffle_dungeon_entrances = tc->shuffle_dungeon_entrances;
      uint8 cx[kSettingsCanonicalLen];
      Settings_CanonicalSerialize(&sx, cx);
      const uint8 active = (cx[25] & kEntranceAxis_DungeonChains) ? 1 : 0;
      if (active != tc->expect_active ||
          (uint8)Settings_EffectiveDungeonChains(&sx) != tc->expect_active) {
        fprintf(stderr, "Settings_SelfCheck: dungeon_chains normalization case "
                        "'%s' expected %u got canonical=%u effective=%u\n",
                tc->name, tc->expect_active, active,
                (uint8)Settings_EffectiveDungeonChains(&sx));
        exit(2);
      }
    }
    typedef struct {
      const char *name;
      uint8 cross_category;
      uint8 decoupled;
      uint8 shuffle_ganons_tower_entrance;
    } ChainRawEntranceFlagCase;
    static const ChainRawEntranceFlagCase kChainRawEntranceFlagCases[] = {
      { "cross-category", 1, 0, 0 },
      { "decoupled", 0, 1, 0 },
      { "gt-entrance", 0, 0, 1 },
    };
    for (size_t i = 0; i < sizeof(kChainRawEntranceFlagCases) /
                               sizeof(kChainRawEntranceFlagCases[0]); i++) {
      const ChainRawEntranceFlagCase *tc = &kChainRawEntranceFlagCases[i];
      RandoSettings sx;
      Settings_SetDefaults(&sx);
      sx.dungeon_chains = 1;
      sx.cross_category = tc->cross_category;
      sx.decoupled = tc->decoupled;
      sx.shuffle_ganons_tower_entrance = tc->shuffle_ganons_tower_entrance;
      if (Settings_EffectiveDungeonChains(&sx)) {
        fprintf(stderr, "Settings_SelfCheck: dungeon_chains raw entrance case "
                        "'%s' should normalize off\n", tc->name);
        exit(2);
      }
    }
    // decoupled implies !coupled in the serialized form.
    RandoSettings se2;
    Settings_SetDefaults(&se2);
    se2.shuffle_cave_entrances = 1;
    se2.coupled = 1;
    se2.decoupled = 1;
    uint8 ce2[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&se2, ce2);
    if (ce2[25] & kEntranceAxis_Coupled) {
      fprintf(stderr, "Settings_SelfCheck: decoupled must clear coupled in the "
                      "serialized byte (got 0x%02x)\n", ce2[25]);
      exit(2);
    }
    // CSV parse of the new keys round-trips through the canonical bytes.
    RandoSettings sv;
    Settings_SetDefaults(&sv);
    if (Settings_ParseCsv("shuffle_cave_entrances=true,coupled=true", &sv) != 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parse of entrance axes failed\n");
      exit(2);
    }
    uint8 cv[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sv, cv);
    if (!settings_byte_eq(cv, ce, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: CSV-parsed entrance axes serialize "
                      "differently from the struct path\n");
      exit(2);
    }
  }
  // add-rando-enemy-shuffle — enemy_shuffle pack/unpack round-trip + default
  // byte-identity. Default MUST keep pad byte [26]==0 (the corpus byte-identical
  // invariant; kExpectedCanonical[26]==0 above depends on it).
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if (cd[26] != 0) {
      fprintf(stderr, "Settings_SelfCheck: default enemy-shuffle byte [26]=0x%02x "
                      "!= 0 (corpus invariant broken)\n", cd[26]);
      exit(2);
    }
    // enemy_shuffle on packs bit0 and round-trips; placement-orthogonal so the
    // ONLY changed canonical byte is [26].
    RandoSettings se;
    Settings_SetDefaults(&se);
    se.enemy_shuffle = 1;
    uint8 ce[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&se, ce);
    if (ce[26] != kEnemyShuffleAxis_Enabled) {
      fprintf(stderr, "Settings_SelfCheck: enemy_shuffle pack mismatch "
                      "(got 0x%02x)\n", ce[26]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 26) continue;
      if (ce[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: enemy_shuffle changed canonical byte "
                        "[%d] (expected only [26] to move)\n", i);
        exit(2);
      }
    }
    RandoSettings rt;
    if (Settings_CanonicalDeserialize(ce, &rt) != 0 || rt.enemy_shuffle != 1) {
      fprintf(stderr, "Settings_SelfCheck: enemy_shuffle deserialize round-trip "
                      "mismatch\n");
      exit(2);
    }
    // CSV parse of the new key round-trips through the canonical bytes.
    RandoSettings sv;
    Settings_SetDefaults(&sv);
    if (Settings_ParseCsv("enemy_shuffle=true", &sv) != 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parse of enemy_shuffle failed\n");
      exit(2);
    }
    uint8 cv[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sv, cv);
    if (!settings_byte_eq(cv, ce, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: CSV-parsed enemy_shuffle serializes "
                      "differently from the struct path\n");
      exit(2);
    }
  }
  // add-rando-customizer-mode — customizer_active pack/unpack round-trip.
  // Shares pad byte [26] with enemy_shuffle (bit1 vs bit0); default off keeps
  // [26]==0 (already asserted above). No CSV key: main.c sets the flag from
  // --customizer=<path> presence, so only the struct path is exercised.
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    RandoSettings sc;
    Settings_SetDefaults(&sc);
    sc.customizer_active = 1;
    uint8 cc[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sc, cc);
    if (cc[26] != kCustomizerAxis_Active) {
      fprintf(stderr, "Settings_SelfCheck: customizer_active pack mismatch "
                      "(got 0x%02x)\n", cc[26]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 26) continue;
      if (cc[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: customizer_active changed canonical "
                        "byte [%d] (expected only [26] to move)\n", i);
        exit(2);
      }
    }
    RandoSettings rt;
    if (Settings_CanonicalDeserialize(cc, &rt) != 0 || rt.customizer_active != 1) {
      fprintf(stderr, "Settings_SelfCheck: customizer_active deserialize "
                      "round-trip mismatch\n");
      exit(2);
    }
    // Both [26] bits set coexist (enemy_shuffle bit0 | customizer bit1).
    sc.enemy_shuffle = 1;
    Settings_CanonicalSerialize(&sc, cc);
    if (cc[26] != (kEnemyShuffleAxis_Enabled | kCustomizerAxis_Active)) {
      fprintf(stderr, "Settings_SelfCheck: [26] bit coexistence mismatch "
                      "(got 0x%02x)\n", cc[26]);
      exit(2);
    }
  }
  // add-rando-traps — trap frequency pack/unpack round-trip. Shares pad byte
  // [26] bits2-3 with enemy_shuffle/customizer bits0-1; default off keeps
  // [26]==0 (already asserted above).
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    RandoSettings st;
    Settings_SetDefaults(&st);
    st.traps = kTrapFrequency_High;
    uint8 ct[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&st, ct);
    if (ct[26] != ((uint8)kTrapFrequency_High << kTrapAxis_Shift)) {
      fprintf(stderr, "Settings_SelfCheck: traps pack mismatch "
                      "(got 0x%02x)\n", ct[26]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 26) continue;
      if (ct[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: traps changed canonical byte [%d] "
                        "(expected only [26] to move)\n", i);
        exit(2);
      }
    }
    RandoSettings rt;
    if (Settings_CanonicalDeserialize(ct, &rt) != 0 ||
        rt.traps != kTrapFrequency_High) {
      fprintf(stderr, "Settings_SelfCheck: traps deserialize round-trip mismatch\n");
      exit(2);
    }
    if (Settings_ParseCsv("traps=medium", &st) != 0 ||
        st.traps != kTrapFrequency_Medium) {
      fprintf(stderr, "Settings_SelfCheck: traps=medium CSV parse failed\n");
      exit(2);
    }
    if (Settings_ParseCsv("traps=low,trap_frequency=high", &st) == 0) {
      fprintf(stderr, "Settings_SelfCheck: trap aliases should duplicate-check\n");
      exit(2);
    }
    st.enemy_shuffle = 1;
    st.customizer_active = 1;
    st.traps = kTrapFrequency_High;
    Settings_CanonicalSerialize(&st, ct);
    if (ct[26] != (kEnemyShuffleAxis_Enabled | kCustomizerAxis_Active |
                   ((uint8)kTrapFrequency_High << kTrapAxis_Shift))) {
      fprintf(stderr, "Settings_SelfCheck: [26] trap bit coexistence mismatch "
                      "(got 0x%02x)\n", ct[26]);
      exit(2);
    }
  }
  // Randomizer QoL — instant_flute defaults on with no canonical bit set;
  // disabling it packs the inverse manual-activation bit into [26] bit4.
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if (sd.instant_flute != 1 || cd[26] != 0) {
      fprintf(stderr, "Settings_SelfCheck: instant_flute default should be on "
                      "with byte [26]==0 (got default=%u byte=0x%02x)\n",
              sd.instant_flute, cd[26]);
      exit(2);
    }
    RandoSettings sf;
    Settings_SetDefaults(&sf);
    sf.instant_flute = 0;
    uint8 cf[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sf, cf);
    if (cf[26] != kInstantFluteAxis_ManualActivation) {
      fprintf(stderr, "Settings_SelfCheck: instant_flute=false pack mismatch "
                      "(got 0x%02x)\n", cf[26]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 26) continue;
      if (cf[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: instant_flute changed canonical "
                        "byte [%d] (expected only [26] to move)\n", i);
        exit(2);
      }
    }
    RandoSettings rt;
    if (Settings_CanonicalDeserialize(cf, &rt) != 0 || rt.instant_flute != 0) {
      fprintf(stderr, "Settings_SelfCheck: instant_flute deserialize round-trip mismatch\n");
      exit(2);
    }
    if (Settings_ParseCsv("instant_flute=false", &sf) != 0 ||
        sf.instant_flute != 0) {
      fprintf(stderr, "Settings_SelfCheck: instant_flute=false CSV parse failed\n");
      exit(2);
    }
    sf.enemy_shuffle = 1;
    sf.customizer_active = 1;
    sf.traps = kTrapFrequency_High;
    sf.instant_flute = 0;
    Settings_CanonicalSerialize(&sf, cf);
    if (cf[26] != (kEnemyShuffleAxis_Enabled | kCustomizerAxis_Active |
                   ((uint8)kTrapFrequency_High << kTrapAxis_Shift) |
                   kInstantFluteAxis_ManualActivation)) {
      fprintf(stderr, "Settings_SelfCheck: [26] instant-flute bit coexistence "
                      "mismatch (got 0x%02x)\n", cf[26]);
      exit(2);
    }
  }
  // add-rando-pot-sanity — pot_shuffle pack/unpack round-trip. Splits across
  // canonical [26] bits 6-7 (low 2) + [27] bit 7 (high). Default Off keeps both
  // bytes' pot bits clear (the corpus byte-identical invariant — the default
  // [26]==0 / [27]==0 selfchecks above depend on it).
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if ((cd[26] & kPotShuffleAxis_LowMask) || (cd[27] & kPotShuffleAxis_HighBit)) {
      fprintf(stderr, "Settings_SelfCheck: default pot_shuffle must leave [26] 6-7 "
                      "and [27] bit7 clear (got [26]=0x%02x [27]=0x%02x)\n", cd[26], cd[27]);
      exit(2);
    }
    // All=3 sets the low 2 bits (0xC0 in [26]); the high bit stays clear (only
    // the reserved Subset=4 uses it). Round-trips losslessly.
    RandoSettings sa;
    Settings_SetDefaults(&sa);
    sa.pot_shuffle = kPotShuffle_All;
    uint8 ca[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sa, ca);
    if ((ca[26] & kPotShuffleAxis_LowMask) != kPotShuffleAxis_LowMask ||
        (ca[27] & kPotShuffleAxis_HighBit) != 0) {
      fprintf(stderr, "Settings_SelfCheck: pot_shuffle=All pack mismatch "
                      "([26]=0x%02x [27]=0x%02x)\n", ca[26], ca[27]);
      exit(2);
    }
    RandoSettings ra;
    if (Settings_CanonicalDeserialize(ca, &ra) != 0 || ra.pot_shuffle != kPotShuffle_All) {
      fprintf(stderr, "Settings_SelfCheck: pot_shuffle=All deserialize round-trip mismatch\n");
      exit(2);
    }
    // Keys=1 round-trips (low bit only).
    RandoSettings sk;
    Settings_SetDefaults(&sk);
    sk.pot_shuffle = kPotShuffle_Keys;
    uint8 ck[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sk, ck);
    RandoSettings rk;
    if (Settings_CanonicalDeserialize(ck, &rk) != 0 || rk.pot_shuffle != kPotShuffle_Keys) {
      fprintf(stderr, "Settings_SelfCheck: pot_shuffle=Keys round-trip mismatch\n");
      exit(2);
    }
    // The reserved Subset value (4) sets the high bit ([27] bit 7) when
    // serialized — verifying the 3-bit field is wide enough for Phase 7 with no
    // re-pack. (Deserialize rejects it via Settings_Validate until then, so this
    // only exercises the encode side.)
    RandoSettings ssub;
    Settings_SetDefaults(&ssub);
    ssub.pot_shuffle = 4;
    uint8 csub[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&ssub, csub);
    if (!(csub[27] & kPotShuffleAxis_HighBit)) {
      fprintf(stderr, "Settings_SelfCheck: reserved pot_shuffle=4 must set [27] bit7 "
                      "(got [27]=0x%02x)\n", csub[27]);
      exit(2);
    }
    // Settings_Validate rejects the not-yet-implemented Subset value.
    if (Settings_Validate(&ssub)) {
      fprintf(stderr, "Settings_SelfCheck: reserved pot_shuffle=4 must fail Settings_Validate\n");
      exit(2);
    }
    // CSV parse round-trips through the canonical bytes.
    RandoSettings sv;
    Settings_SetDefaults(&sv);
    if (Settings_ParseCsv("pot_shuffle=all", &sv) != 0 || sv.pot_shuffle != kPotShuffle_All) {
      fprintf(stderr, "Settings_SelfCheck: pot_shuffle=all CSV parse failed\n");
      exit(2);
    }
    uint8 cv[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sv, cv);
    if (!settings_byte_eq(cv, ca, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: CSV-parsed pot_shuffle serializes differently "
                      "from the struct path\n");
      exit(2);
    }
    // Door shuffle now composes with pot_shuffle; only cave entrance shuffle
    // normalizes pots off.
    RandoSettings sdoor;
    Settings_SetDefaults(&sdoor);
    sdoor.door_shuffle = kDoorShuffle_Basic;
    sdoor.pot_shuffle = kPotShuffle_All;
    uint8 cdoor[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdoor, cdoor);
    if ((cdoor[26] & kPotShuffleAxis_LowMask) != kPotShuffleAxis_LowMask ||
        (cdoor[27] & kPotShuffleAxis_HighBit) != 0) {
      fprintf(stderr, "Settings_SelfCheck: door shuffle must preserve pot_shuffle=all "
                      "([26]=0x%02x [27]=0x%02x)\n", cdoor[26], cdoor[27]);
      exit(2);
    }
    RandoSettings scave;
    Settings_SetDefaults(&scave);
    scave.shuffle_cave_entrances = 1;
    scave.pot_shuffle = kPotShuffle_All;
    uint8 ccave[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&scave, ccave);
    if ((ccave[26] & kPotShuffleAxis_LowMask) || (ccave[27] & kPotShuffleAxis_HighBit)) {
      fprintf(stderr, "Settings_SelfCheck: cave entrance shuffle must normalize "
                      "pot_shuffle off ([26]=0x%02x [27]=0x%02x)\n",
              ccave[26], ccave[27]);
      exit(2);
    }
  }
  // add-rando-enemy-drop-sanity — enemy_drop_checks is append-only byte [28].
  // Keys activates with Wild/Dungeon effective small keys. Dungeon activates the
  // ordinary dungeon enemy checks when generated logic can prove them, including
  // door shuffle through the generated door bridge. Enemy shuffle still degrades
  // Dungeon/All to Keys so forced enemy key drops stay active without enabling
  // ordinary enemy rows whose shuffled type/HP cannot be proven.
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if (cd[28] != kEnemyDropChecks_Off) {
      fprintf(stderr, "Settings_SelfCheck: default enemy_drop_checks must be Off ([28]=0x%02x)\n",
              cd[28]);
      exit(2);
    }
    RandoSettings sk;
    Settings_SetDefaults(&sk);
    sk.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sk.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint8 ck[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sk, ck);
    if (ck[28] != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Keys pack mismatch ([28]=0x%02x)\n",
              ck[28]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 11 || i == 28) continue;  // small-key mode + enemy-drop axis
      if (ck[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks changed canonical byte [%d]\n", i);
        exit(2);
      }
    }
    RandoSettings rk;
    if (Settings_CanonicalDeserialize(ck, &rk) != 0 ||
        rk.enemy_drop_checks != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Keys round-trip mismatch\n");
      exit(2);
    }
    if (!Settings_EnemyDropKeysActive(&rk)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Keys should be effective with Wild keys\n");
      exit(2);
    }
    RandoSettings sdun;
    Settings_SetDefaults(&sdun);
    sdun.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
    sdun.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint8 cdun[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdun, cdun);
    if (cdun[28] != kEnemyDropChecks_Keys || !Settings_EnemyDropKeysActive(&sdun)) {
      fprintf(stderr, "Settings_SelfCheck: vanilla-door dungeon small keys should keep enemy_drop_checks active\n");
      exit(2);
    }
    RandoSettings sdoor = sdun;
    sdoor.door_shuffle = kDoorShuffle_Basic;
    uint8 cdoor[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdoor, cdoor);
    if (cdoor[28] != kEnemyDropChecks_Keys ||
        Settings_EffectiveSmallKeysMode(&sdoor) != kDungeonItemMode_Dungeon ||
        !Settings_EnemyDropKeysActive(&sdoor)) {
      fprintf(stderr, "Settings_SelfCheck: door shuffle must keep Dungeon enemy_drop_checks active\n");
      exit(2);
    }
    RandoSettings sv;
    Settings_SetDefaults(&sv);
    sv.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint8 cv[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sv, cv);
    if (cv[28] != kEnemyDropChecks_Off) {
      fprintf(stderr, "Settings_SelfCheck: vanilla small keys must normalize enemy_drop_checks off\n");
      exit(2);
    }
    RandoSettings se;
    Settings_SetDefaults(&se);
    se.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    se.enemy_shuffle = 1;
    se.enemy_drop_checks = kEnemyDropChecks_Keys;
    uint8 ce[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&se, ce);
    if (ce[28] != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: enemy_shuffle must compose with enemy_drop_checks=Keys\n");
      exit(2);
    }
    if (!Settings_EnemyDropKeysActive(&se)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Keys should be effective with enemy_shuffle\n");
      exit(2);
    }
    RandoSettings sdungeon;
    Settings_SetDefaults(&sdungeon);
    sdungeon.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    sdungeon.enemy_drop_checks = kEnemyDropChecks_Dungeon;
    uint8 call[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdungeon, call);
    if (call[28] != kEnemyDropChecks_Dungeon ||
        !Settings_EnemyDropKeysActive(&sdungeon) ||
        !Settings_EnemyChecksDungeonActive(&sdungeon)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Dungeon should be active with Wild keys\n");
      exit(2);
    }
    RandoSettings rdungeon;
    if (Settings_CanonicalDeserialize(call, &rdungeon) != 0 ||
        rdungeon.enemy_drop_checks != kEnemyDropChecks_Dungeon ||
        !Settings_EnemyChecksDungeonActive(&rdungeon)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=Dungeon round-trip mismatch\n");
      exit(2);
    }
    RandoSettings sall = sdungeon;
    sall.enemy_drop_checks = kEnemyDropChecks_All;
    uint8 callall[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sall, callall);
    if (callall[28] != kEnemyDropChecks_All ||
        !Settings_EnemyDropKeysActive(&sall) ||
        !Settings_EnemyChecksDungeonActive(&sall) ||
        !Settings_EnemyChecksAllActive(&sall)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=All should stay distinct with Wild keys\n");
      exit(2);
    }
    RandoSettings rall;
    if (Settings_CanonicalDeserialize(callall, &rall) != 0 ||
        rall.enemy_drop_checks != kEnemyDropChecks_All ||
        !Settings_EnemyChecksAllActive(&rall)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=All round-trip mismatch\n");
      exit(2);
    }
    RandoSettings sdungeondoor = sdungeon;
    sdungeondoor.door_shuffle = kDoorShuffle_Basic;
    uint8 calldoor[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdungeondoor, calldoor);
    if (calldoor[28] != kEnemyDropChecks_Dungeon ||
        !Settings_EnemyChecksDungeonActive(&sdungeondoor) ||
        Settings_EffectiveEnemyDropChecks(&sdungeondoor) != kEnemyDropChecks_Dungeon) {
      fprintf(stderr, "Settings_SelfCheck: door shuffle must preserve enemy_drop_checks=Dungeon\n");
      exit(2);
    }
    RandoSettings sdungeonenemy = sdungeon;
    sdungeonenemy.enemy_shuffle = 1;
    uint8 callenemy[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdungeonenemy, callenemy);
    if (callenemy[28] != kEnemyDropChecks_Keys ||
        !Settings_EnemyDropKeysActive(&sdungeonenemy) ||
        Settings_EffectiveEnemyDropChecks(&sdungeonenemy) != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: enemy shuffle must degrade enemy_drop_checks=Dungeon to Keys\n");
      exit(2);
    }
    RandoSettings salldoor = sall;
    salldoor.door_shuffle = kDoorShuffle_Basic;
    uint8 calldoor2[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&salldoor, calldoor2);
    if (calldoor2[28] != kEnemyDropChecks_All ||
        !Settings_EnemyChecksAllActive(&salldoor)) {
      fprintf(stderr, "Settings_SelfCheck: door shuffle must preserve enemy_drop_checks=All\n");
      exit(2);
    }
    RandoSettings sallenemy = sall;
    sallenemy.enemy_shuffle = 1;
    uint8 callenemy2[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sallenemy, callenemy2);
    if (callenemy2[28] != kEnemyDropChecks_Keys ||
        Settings_EnemyChecksAllActive(&sallenemy)) {
      fprintf(stderr, "Settings_SelfCheck: enemy shuffle must degrade enemy_drop_checks=All to Keys\n");
      exit(2);
    }
    RandoSettings sallboss = sall;
    sallboss.boss_shuffle = 1;
    uint8 callboss[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sallboss, callboss);
    if (callboss[28] != kEnemyDropChecks_All ||
        !Settings_EnemyChecksAllActive(&sallboss)) {
      fprintf(stderr, "Settings_SelfCheck: boss shuffle must preserve enemy_drop_checks=All\n");
      exit(2);
    }
    RandoSettings sallentrance = sall;
    sallentrance.shuffle_cave_entrances = 1;
    uint8 callentrance[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sallentrance, callentrance);
    if (callentrance[28] != kEnemyDropChecks_Dungeon ||
        Settings_EnemyChecksAllActive(&sallentrance) ||
        !Settings_EnemyChecksDungeonActive(&sallentrance)) {
      fprintf(stderr, "Settings_SelfCheck: entrance shuffle must degrade enemy_drop_checks=All to Dungeon\n");
      exit(2);
    }
    RandoSettings sr;
    Settings_SetDefaults(&sr);
    if (Settings_ParseCsv("dungeon_items.small_keys=wild,enemy_drop_checks=keys", &sr) != 0 ||
        sr.enemy_drop_checks != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=keys CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&sr);
    if (Settings_ParseCsv("dungeon_items.small_keys=wild,enemy_drop_checks=dungeon", &sr) != 0 ||
        sr.enemy_drop_checks != kEnemyDropChecks_Dungeon ||
        !Settings_EnemyChecksDungeonActive(&sr)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=dungeon CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&sr);
    if (Settings_ParseCsv("dungeon_items.small_keys=wild,enemy_drop_checks=all", &sr) != 0 ||
        sr.enemy_drop_checks != kEnemyDropChecks_All ||
        !Settings_EnemyChecksAllActive(&sr)) {
      fprintf(stderr, "Settings_SelfCheck: enemy_drop_checks=all CSV parse failed\n");
      exit(2);
    }
  }
  // add-enemy-souls — souls_shuffle pack/unpack in [28] bits 2-3, coexisting
  // with enemy_drop_checks in bits 0-1; default byte stays 0x00.
  {
    RandoSettings sd;
    Settings_SetDefaults(&sd);
    uint8 cd[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sd, cd);
    if (cd[28] & kSoulsShuffleAxis_Mask) {
      fprintf(stderr, "Settings_SelfCheck: default souls_shuffle must leave [28] bits 2-3 clear (0x%02x)\n",
              cd[28]);
      exit(2);
    }
    RandoSettings ss;
    Settings_SetDefaults(&ss);
    ss.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    ss.enemy_drop_checks = kEnemyDropChecks_Keys;
    ss.souls_shuffle = kSoulsShuffle_BossesEnemies;
    uint8 cs[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&ss, cs);
    if (cs[28] != (kEnemyDropChecks_Keys |
                   (kSoulsShuffle_BossesEnemies << kSoulsShuffleAxis_Shift))) {
      fprintf(stderr, "Settings_SelfCheck: souls_shuffle pack mismatch ([28]=0x%02x)\n", cs[28]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 11 || i == 28) continue;  // small-key mode + the shared [28] axes
      if (cs[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: souls_shuffle changed canonical byte [%d]\n", i);
        exit(2);
      }
    }
    RandoSettings rs;
    if (Settings_CanonicalDeserialize(cs, &rs) != 0 ||
        rs.souls_shuffle != kSoulsShuffle_BossesEnemies ||
        rs.enemy_drop_checks != kEnemyDropChecks_Keys) {
      fprintf(stderr, "Settings_SelfCheck: souls_shuffle round-trip mismatch\n");
      exit(2);
    }
    // add-npc-souls — [28] bit 4 pack/round-trip/CSV; independent of tiers.
    RandoSettings sn;
    Settings_SetDefaults(&sn);
    sn.npc_souls = 1;
    uint8 cn[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sn, cn);
    if (cn[28] != kNpcSoulsAxis_Enabled) {
      fprintf(stderr, "Settings_SelfCheck: npc_souls pack mismatch ([28]=0x%02x)\n", cn[28]);
      exit(2);
    }
    for (int i = 0; i < kSettingsCanonicalLen; i++) {
      if (i == 28) continue;
      if (cn[i] != cd[i]) {
        fprintf(stderr, "Settings_SelfCheck: npc_souls changed canonical byte [%d]\n", i);
        exit(2);
      }
    }
    RandoSettings rn;
    if (Settings_CanonicalDeserialize(cn, &rn) != 0 || rn.npc_souls != 1 ||
        rn.souls_shuffle != kSoulsShuffle_Off) {
      fprintf(stderr, "Settings_SelfCheck: npc_souls round-trip mismatch\n");
      exit(2);
    }
    Settings_SetDefaults(&sn);
    if (Settings_ParseCsv("npc_souls=true", &sn) != 0 || sn.npc_souls != 1) {
      fprintf(stderr, "Settings_SelfCheck: npc_souls=true CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&sn);
    if (Settings_ParseCsv("npc_souls=maybe", &sn) == 0) {
      fprintf(stderr, "Settings_SelfCheck: bad npc_souls value must be rejected\n");
      exit(2);
    }
    // NO door-shuffle degrade: npc_souls survives an active door layout
    // (no gated check is door-oracle-controlled — spec requirement).
    Settings_SetDefaults(&sn);
    sn.npc_souls = 1;
    sn.door_shuffle = kDoorShuffle_Basic;
    apply_derived_rules(&sn);
    if (sn.npc_souls != 1) {
      fprintf(stderr, "Settings_SelfCheck: npc_souls must NOT degrade under door shuffle\n");
      exit(2);
    }
    RandoSettings sp;
    Settings_SetDefaults(&sp);
    if (Settings_ParseCsv("souls_shuffle=bosses", &sp) != 0 ||
        sp.souls_shuffle != kSoulsShuffle_Bosses) {
      fprintf(stderr, "Settings_SelfCheck: souls_shuffle=bosses CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&sp);
    if (Settings_ParseCsv("souls_shuffle=all", &sp) != 0 ||
        sp.souls_shuffle != kSoulsShuffle_BossesEnemies) {
      fprintf(stderr, "Settings_SelfCheck: souls_shuffle=all CSV parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&sp);
    if (Settings_ParseCsv("souls_shuffle=maximum", &sp) == 0) {
      fprintf(stderr, "Settings_SelfCheck: bad souls_shuffle value must be rejected\n");
      exit(2);
    }
    // Souls degrade to OFF under door shuffle (species-blind door oracle;
    // any tier), and the canonical hash reflects the degraded value.
    RandoSettings sdoor;
    Settings_SetDefaults(&sdoor);
    sdoor.souls_shuffle = kSoulsShuffle_BossesEnemies;
    sdoor.door_shuffle = kDoorShuffle_Basic;
    if (Settings_EffectiveSoulsShuffle(&sdoor) != kSoulsShuffle_Off) {
      fprintf(stderr, "Settings_SelfCheck: souls must degrade to off under door shuffle\n");
      exit(2);
    }
    sdoor.souls_shuffle = kSoulsShuffle_Bosses;
    if (Settings_EffectiveSoulsShuffle(&sdoor) != kSoulsShuffle_Off) {
      fprintf(stderr, "Settings_SelfCheck: boss souls must degrade to off under door shuffle\n");
      exit(2);
    }
    uint8 cdoor[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&sdoor, cdoor);
    if ((cdoor[28] & kSoulsShuffleAxis_Mask) != 0) {
      fprintf(stderr, "Settings_SelfCheck: canonical souls tier must be the door-degraded value\n");
      exit(2);
    }
    if (Settings_EffectiveSoulsShuffle(&ss) != kSoulsShuffle_BossesEnemies) {
      fprintf(stderr, "Settings_SelfCheck: souls enemies tier must survive without door shuffle\n");
      exit(2);
    }
  }
  // Spec scenario "Truncation is first-16-bytes": Settings_HashShort writes
  // exactly the first 16 bytes of SHA-256(canonical), not a different hash.
  {
    RandoSettings sh;
    Settings_SetDefaults(&sh);
    uint8 full[32], short_hash[16];
    Settings_ComputeHash(&sh, full);
    Settings_HashShort(&sh, short_hash);
    for (int i = 0; i < 16; i++) {
      if (full[i] != short_hash[i]) {
        fprintf(stderr,
          "Settings_SelfCheck: HashShort byte %d differs from ComputeHash byte\n", i);
        exit(2);
      }
    }
  }
  // CSV parser rejects unknown key.
  {
    RandoSettings s3;
    Settings_SetDefaults(&s3);
    int rc = Settings_ParseCsv("not_a_key=42", &s3);
    if (rc == 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser should reject unknown key\n");
      exit(2);
    }
  }
  // CSV parser rejects bad enum value.
  {
    RandoSettings s4;
    Settings_SetDefaults(&s4);
    int rc = Settings_ParseCsv("goal=not_a_goal", &s4);
    if (rc == 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser should reject bad enum\n");
      exit(2);
    }
  }
  // CSV parser accepts Python YAML capitalized True/False.
  {
    RandoSettings sp;
    Settings_SetDefaults(&sp);
    if (Settings_ParseCsv("race_mode=True", &sp) != 0 || sp.race_mode != 1) {
      fprintf(stderr, "Settings_SelfCheck: parse_bool should accept Python-style True\n");
      exit(2);
    }
    if (Settings_ParseCsv("race_mode=False", &sp) != 0 || sp.race_mode != 0) {
      fprintf(stderr, "Settings_SelfCheck: parse_bool should accept Python-style False\n");
      exit(2);
    }
  }
  // CSV parser rejects duplicate keys.
  {
    RandoSettings s4d;
    Settings_SetDefaults(&s4d);
    int rc = Settings_ParseCsv("goal=fast_ganon,goal=dungeons", &s4d);
    if (rc == 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser should reject duplicate keys\n");
      exit(2);
    }
  }
  // Phase B Slice 4 — un-pinned axes. Round-trip the new parser surface.
  {
    RandoSettings sl;
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=OverworldGlitches", &sl) != 0 || sl.logic != 1) {
      fprintf(stderr, "Settings_SelfCheck: logic=OverworldGlitches should parse to 1\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=major_glitches", &sl) != 0 || sl.logic != 2) {
      fprintf(stderr, "Settings_SelfCheck: logic=major_glitches should parse to 2\n");
      exit(2);
    }
    // Phase D (add-rando-major-glitch) — HybridMG / NoLogic now parse.
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=HybridMG", &sl) != 0 || sl.logic != 3) {
      fprintf(stderr, "Settings_SelfCheck: logic=HybridMG should parse to 3\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=hybrid_major_glitches", &sl) != 0 || sl.logic != 3) {
      fprintf(stderr, "Settings_SelfCheck: logic=hybrid_major_glitches should parse to 3\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=no_logic", &sl) != 0 || sl.logic != 4) {
      fprintf(stderr, "Settings_SelfCheck: logic=no_logic should parse to 4\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("logic=NoLogic", &sl) != 0 || sl.logic != 4) {
      fprintf(stderr, "Settings_SelfCheck: logic=NoLogic should parse to 4\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("accessibility=none", &sl) != 0 || sl.accessibility != 2) {
      fprintf(stderr, "Settings_SelfCheck: accessibility=none should parse to 2 (kAccessibility_None)\n");
      exit(2);
    }
    Settings_SetDefaults(&sl);
    if (Settings_ParseCsv("accessibility=beatable", &sl) != 0 || sl.accessibility != 2) {
      fprintf(stderr, "Settings_SelfCheck: accessibility=beatable alias should parse to 2 (kAccessibility_None)\n");
      exit(2);
    }
  }
  // add-rando-major-glitch D6 — Rando_SettingsAssumeJpGlitches: a glitch seed
  // must force the JP-glitch runtime flag; a plain / non-glitch-trick seed must
  // NOT. Mirrors the coupling predicate exactly (logic>=OWG OR fake-flippers).
  {
    RandoSettings sj;
    Settings_SetDefaults(&sj);
    if (Rando_SettingsAssumeJpGlitches(&sj)) {
      fprintf(stderr, "Settings_SelfCheck: default seed must NOT assume JP glitches\n");
      exit(2);
    }
    // Every glitch logic tier (1..4) assumes glitches.
    static const char *const kGlitchLogics[] = {
      "logic=overworld_glitches", "logic=major_glitches",
      "logic=hybrid_mg", "logic=no_logic"};
    for (size_t i = 0; i < sizeof(kGlitchLogics) / sizeof(kGlitchLogics[0]); i++) {
      Settings_SetDefaults(&sj);
      if (Settings_ParseCsv(kGlitchLogics[i], &sj) != 0 ||
          !Rando_SettingsAssumeJpGlitches(&sj)) {
        fprintf(stderr, "Settings_SelfCheck: %s must assume JP glitches\n", kGlitchLogics[i]);
        exit(2);
      }
    }
    // The fake-flippers trick (logic=0) assumes glitches (it maps 1:1 to the
    // restored Fake Flippers glitch).
    Settings_SetDefaults(&sj);
    if (Settings_ParseCsv("tricks=fake-flippers", &sj) != 0 ||
        !Rando_SettingsAssumeJpGlitches(&sj)) {
      fprintf(stderr, "Settings_SelfCheck: tricks=fake-flippers must assume JP glitches\n");
      exit(2);
    }
    // boots-clip / dark-room-nav are NOT restored JP glitches — must NOT force.
    Settings_SetDefaults(&sj);
    if (Settings_ParseCsv("tricks=boots-clip+dark-room-nav", &sj) != 0 ||
        Rando_SettingsAssumeJpGlitches(&sj)) {
      fprintf(stderr, "Settings_SelfCheck: non-fake-flippers tricks must NOT force JP glitches\n");
      exit(2);
    }
  }
  // CSV parser rejects out-of-range numeric values.
  {
    RandoSettings s4r;
    Settings_SetDefaults(&s4r);
    int rc = Settings_ParseCsv("crystals.ganon=8", &s4r);  // valid range is 0..7
    if (rc == 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser should reject crystals.ganon=8\n");
      exit(2);
    }
  }
  // CSV parser rejects pieces_required > pieces_placed for Triforce-Hunt.
  {
    RandoSettings s4t;
    Settings_SetDefaults(&s4t);
    int rc = Settings_ParseCsv("goal=triforce-hunt,pieces_required=30,pieces_placed=20", &s4t);
    if (rc == 0) {
      fprintf(stderr, "Settings_SelfCheck: CSV parser should reject pieces_required > pieces_placed\n");
      exit(2);
    }
  }
  // Preset round-trip: each preset applies cleanly, and the Standard preset
  // changes world_state from the default.
  for (int p = 0; p < kPreset__Count; p++) {
    RandoSettings sp;
    int rc = Settings_ApplyPreset((SettingsPreset)p, &sp);
    if (rc != 0) {
      fprintf(stderr, "Settings_SelfCheck: preset %d failed\n", p);
      exit(2);
    }
  }
  {
    RandoSettings sp;
    Settings_ApplyPreset(kPreset_StandardGanon, &sp);
    if (sp.world_state != kWorldState_Standard) {
      fprintf(stderr, "Settings_SelfCheck: StandardGanon preset failed to set world_state\n");
      exit(2);
    }
  }
  {
    RandoSettings sp;
    Settings_ApplyPreset(kPreset_TriforceHuntDefault, &sp);
    if (sp.goal != kGoal_TriforceHunt || sp.pieces_required != 20 || sp.pieces_placed != 30) {
      fprintf(stderr, "Settings_SelfCheck: TriforceHuntDefault preset wrong\n");
      exit(2);
    }
  }
  // Phase B Slice 6 — Settings_CanonicalDeserialize round-trip. Verify
  // serialize → deserialize → serialize produces byte-identical output for
  // both defaults and a non-default preset. This is the reproducibility
  // guarantee Rando_RevealSpoiler depends on.
  {
    RandoSettings s_orig, s_round;
    Settings_SetDefaults(&s_orig);
    uint8 bytes_a[kSettingsCanonicalLen], bytes_b[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&s_orig, bytes_a);
    if (Settings_CanonicalDeserialize(bytes_a, &s_round) != 0) {
      fprintf(stderr, "Settings_SelfCheck: deserialize defaults failed\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&s_round, bytes_b);
    if (!settings_byte_eq(bytes_a, bytes_b, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: deserialize round-trip drift on defaults\n");
      exit(2);
    }
  }
  {
    // Apply a preset (Completionist triggers apply_derived_rules) and verify
    // round-trip still re-applies the derived rules so the bytes stabilize.
    RandoSettings s_orig, s_round;
    Settings_ApplyPreset(kPreset_TriforceHuntDefault, &s_orig);
    uint8 bytes_a[kSettingsCanonicalLen], bytes_b[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&s_orig, bytes_a);
    if (Settings_CanonicalDeserialize(bytes_a, &s_round) != 0) {
      fprintf(stderr, "Settings_SelfCheck: deserialize preset failed\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&s_round, bytes_b);
    if (!settings_byte_eq(bytes_a, bytes_b, kSettingsCanonicalLen)) {
      fprintf(stderr, "Settings_SelfCheck: deserialize round-trip drift on preset\n");
      exit(2);
    }
  }
  {
    // FIX #5 — Settings_CanonicalDeserialize refuses out-of-range enum bytes
    // (the byte paths previously copied them raw). Packed bytes [25]-[27] are
    // now fully allocated; their independent boolean bits decode normally.
    RandoSettings s_def, s_chk;
    uint8 blob[kSettingsCanonicalLen];
    Settings_SetDefaults(&s_def);
    Settings_CanonicalSerialize(&s_def, blob);
    blob[0] = 9;  // world_state: only 0..3 defined; 9 would UB `1u << ws` consumers
    if (Settings_CanonicalDeserialize(blob, &s_chk) == 0) {
      fprintf(stderr, "Settings_SelfCheck: out-of-range world_state must be refused\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&s_def, blob);
    blob[1] = 200;  // goal: only 0..6 defined
    if (Settings_CanonicalDeserialize(blob, &s_chk) == 0) {
      fprintf(stderr, "Settings_SelfCheck: out-of-range goal must be refused\n");
      exit(2);
    }
    Settings_CanonicalSerialize(&s_def, blob);
    blob[25] |= kHintNpcRewardRevealAxis_Enabled;
    if (Settings_CanonicalDeserialize(blob, &s_chk) != 0 ||
        !s_chk.hint_npc_reward_reveal) {
      fprintf(stderr,
              "Settings_SelfCheck: packed NPC reward disclosure bit did not "
              "decode\n");
      exit(2);
    }
  }
  {
    // FIX #17a — `tricks` grammar: hex mask, decimal mask, named list, and
    // garbage-hex rejection (the hex form was broken from day one: parse_uint
    // got the whole "0xNN" string and is decimal-only).
    RandoSettings st;
    Settings_SetDefaults(&st);
    if (Settings_ParseCsv("tricks=0x1F", &st) != 0 || st.tricks != 0x1F) {
      fprintf(stderr, "Settings_SelfCheck: tricks=0x1F hex parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&st);
    if (Settings_ParseCsv("tricks=31", &st) != 0 || st.tricks != 31) {
      fprintf(stderr, "Settings_SelfCheck: tricks=31 decimal parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&st);
    if (Settings_ParseCsv("tricks=boots-clip+fake-flippers", &st) != 0 || st.tricks != 0x03) {
      fprintf(stderr, "Settings_SelfCheck: tricks named-list parse failed\n");
      exit(2);
    }
    Settings_SetDefaults(&st);
    if (Settings_ParseCsv("tricks=0xZZ", &st) == 0) {
      fprintf(stderr, "Settings_SelfCheck: tricks=0xZZ should be rejected\n");
      exit(2);
    }
    Settings_SetDefaults(&st);
    if (Settings_ParseCsv("tricks=0x100", &st) == 0) {
      fprintf(stderr, "Settings_SelfCheck: tricks=0x100 (>0xFF) should be rejected\n");
      exit(2);
    }
  }
}

// ---------------------------------------------------------------------------
// CSV parser
// ---------------------------------------------------------------------------
#include <string.h>
#include <ctype.h>

static int csv_str_eq(const char *a, int alen, const char *b) {
  int blen = (int)strlen(b);
  if (alen != blen) return 0;
  for (int i = 0; i < alen; i++) if (a[i] != b[i]) return 0;
  return 1;
}

static int parse_bool(const char *v, int vlen, uint8 *out) {
  // Accept lowercase / capitalized / numeric variants — Python YAML writes
  // True/False (capitalized) and the corpus manifest goes through yaml.dump
  // before --generate-seed reads it back.
  if (csv_str_eq(v, vlen, "true") || csv_str_eq(v, vlen, "True") || csv_str_eq(v, vlen, "1")) { *out = 1; return 0; }
  if (csv_str_eq(v, vlen, "false") || csv_str_eq(v, vlen, "False") || csv_str_eq(v, vlen, "0")) { *out = 0; return 0; }
  return -1;
}

static int parse_uint(const char *v, int vlen, uint32 *out) {
  if (vlen <= 0) return -1;
  uint32 r = 0;
  for (int i = 0; i < vlen; i++) {
    if (v[i] < '0' || v[i] > '9') return -1;
    r = r * 10 + (uint32)(v[i] - '0');
  }
  *out = r;
  return 0;
}

// Hex digits only — the caller strips the "0x"/"0X" prefix first. parse_uint
// is decimal-only, so routing a prefixed string through it rejected every
// documented `tricks=0xNN` form (FIX #17a). Mirrors parse_uint's style.
static int parse_hex(const char *v, int vlen, uint32 *out) {
  if (vlen <= 0) return -1;
  uint32 r = 0;
  for (int i = 0; i < vlen; i++) {
    char c = v[i];
    uint32 d;
    if (c >= '0' && c <= '9') d = (uint32)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32)(c - 'A' + 10);
    else return -1;
    if (r > (0xFFFFFFFFu >> 4)) return -1;  // would overflow uint32
    r = (r << 4) | d;
  }
  *out = r;
  return 0;
}

static int parse_world_state(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "open"))      { *out = kWorldState_Open;     return 0; }
  if (csv_str_eq(v, vlen, "standard"))  { *out = kWorldState_Standard; return 0; }
  if (csv_str_eq(v, vlen, "inverted"))  { *out = kWorldState_Inverted; return 0; }
  if (csv_str_eq(v, vlen, "retro"))     { *out = kWorldState_Retro;    return 0; }
  return -1;
}

static int parse_goal(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "ganon"))         { *out = kGoal_Ganon;        return 0; }
  if (csv_str_eq(v, vlen, "fast_ganon"))    { *out = kGoal_FastGanon;    return 0; }
  if (csv_str_eq(v, vlen, "dungeons"))      { *out = kGoal_Dungeons;     return 0; }
  if (csv_str_eq(v, vlen, "pedestal"))      { *out = kGoal_Pedestal;     return 0; }
  if (csv_str_eq(v, vlen, "triforce-hunt")) { *out = kGoal_TriforceHunt; return 0; }
  if (csv_str_eq(v, vlen, "ganonhunt"))     { *out = kGoal_GanonHunt;    return 0; }
  if (csv_str_eq(v, vlen, "completionist")) { *out = kGoal_Completionist; return 0; }
  return -1;
}

static int parse_pool_diff(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "easy"))   { *out = kItemPoolDifficulty_Easy;   return 0; }
  if (csv_str_eq(v, vlen, "normal")) { *out = kItemPoolDifficulty_Normal; return 0; }
  if (csv_str_eq(v, vlen, "hard"))   { *out = kItemPoolDifficulty_Hard;   return 0; }
  if (csv_str_eq(v, vlen, "expert")) { *out = kItemPoolDifficulty_Expert; return 0; }
  return -1;
}

static int parse_dungeon_mode(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "vanilla")) { *out = kDungeonItemMode_Vanilla; return 0; }
  if (csv_str_eq(v, vlen, "dungeon")) { *out = kDungeonItemMode_Dungeon; return 0; }
  if (csv_str_eq(v, vlen, "wild"))    { *out = kDungeonItemMode_Wild;    return 0; }
  return -1;
}

static int parse_weapons(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "randomized")) { *out = kModeWeapons_Randomized; return 0; }
  if (csv_str_eq(v, vlen, "assured"))    { *out = kModeWeapons_Assured;    return 0; }
  // Phase B un-pin: swordless. `vanilla` (=2) stays reserved/out of scope.
  if (csv_str_eq(v, vlen, "swordless"))  { *out = kModeWeapons_Swordless;  return 0; }
  return -1;
}

static int parse_accessibility(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "items"))     { *out = kAccessibility_Items;     return 0; }
  if (csv_str_eq(v, vlen, "locations")) { *out = kAccessibility_Locations; return 0; }
  // `none` is ALTTPR's config value (UI label "beatable only"): the seed is
  // still guaranteed beatable, only full item/location accessibility is relaxed.
  // Accept `beatable` as a friendlier alias for the same value.
  if (csv_str_eq(v, vlen, "none"))      { *out = kAccessibility_None;      return 0; }
  if (csv_str_eq(v, vlen, "beatable")) { *out = kAccessibility_None;      return 0; }
  return -1;
}

static int parse_traps(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0")) {
    *out = kTrapFrequency_Off; return 0;
  }
  if (csv_str_eq(v, vlen, "low") || csv_str_eq(v, vlen, "1")) {
    *out = kTrapFrequency_Low; return 0;
  }
  if (csv_str_eq(v, vlen, "medium") || csv_str_eq(v, vlen, "med") ||
      csv_str_eq(v, vlen, "2")) {
    *out = kTrapFrequency_Medium; return 0;
  }
  if (csv_str_eq(v, vlen, "high") || csv_str_eq(v, vlen, "3")) {
    *out = kTrapFrequency_High; return 0;
  }
  if (csv_str_eq(v, vlen, "insanity") || csv_str_eq(v, vlen, "max") ||
      csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "4")) {
    *out = kTrapFrequency_Insanity; return 0;
  }
  return -1;
}

// add-rando-trap-catalog — parse the per-category enable mask. Accepts "all"/
// "none"/"0" (the all-categories sentinel) or a '+'-joined list of category names
// (hazard|impair|drain|scare|displace). "all checked" collapses to 0, matching the
// canonical zero-sentinel + the UI normalization (one stored encoding of "all").
static int parse_trap_categories(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "none") ||
      csv_str_eq(v, vlen, "0")) {
    *out = 0; return 0;
  }
  uint8 mask = 0;
  int i = 0;
  while (i < vlen) {
    int start = i;
    while (i < vlen && v[i] != '+') i++;
    int tlen = i - start;
    const char *t = v + start;
    if (csv_str_eq(t, tlen, "hazard"))        mask |= kTrapCategory_Hazard;
    else if (csv_str_eq(t, tlen, "impair"))   mask |= kTrapCategory_Impair;
    else if (csv_str_eq(t, tlen, "drain"))    mask |= kTrapCategory_Drain;
    else if (csv_str_eq(t, tlen, "scare"))    mask |= kTrapCategory_Scare;
    else if (csv_str_eq(t, tlen, "displace")) mask |= kTrapCategory_Displace;
    else return -1;
    if (i < vlen && v[i] == '+') i++;  // skip separator
  }
  if (mask == kTrapCategory_All) mask = 0;  // canonical "all" sentinel
  *out = mask;
  return 0;
}

// add-rando-pot-sanity — parse the pot_shuffle tier. The reserved Subset value
// (4) has no CSV spelling yet (Phase 7); off/keys/contents/all map 0..3.
static int parse_pot_shuffle(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0"))      { *out = kPotShuffle_Off;      return 0; }
  if (csv_str_eq(v, vlen, "keys") || csv_str_eq(v, vlen, "1"))     { *out = kPotShuffle_Keys;     return 0; }
  if (csv_str_eq(v, vlen, "contents") || csv_str_eq(v, vlen, "2")) { *out = kPotShuffle_Contents; return 0; }
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "3"))      { *out = kPotShuffle_All;       return 0; }
  return -1;
}

// add-rando-ow-warp-shuffle — parse the flute-spot shuffle mode.
static int parse_flute_shuffle(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0"))      { *out = kFluteShuffle_Off;      return 0; }
  if (csv_str_eq(v, vlen, "balanced") || csv_str_eq(v, vlen, "1")) { *out = kFluteShuffle_Balanced; return 0; }
  if (csv_str_eq(v, vlen, "random") || csv_str_eq(v, vlen, "2"))   { *out = kFluteShuffle_Random;   return 0; }
  return -1;
}

// add-rando-enemy-drop-sanity — parse the enemy_drop_checks tier.
static int parse_enemy_drop_checks(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0"))  { *out = kEnemyDropChecks_Off;      return 0; }
  if (csv_str_eq(v, vlen, "keys") || csv_str_eq(v, vlen, "1")) { *out = kEnemyDropChecks_Keys;     return 0; }
  if (csv_str_eq(v, vlen, "dungeon") || csv_str_eq(v, vlen, "2")) {
    *out = kEnemyDropChecks_Dungeon;
    return 0;
  }
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "3")) {
    *out = kEnemyDropChecks_All;
    return 0;
  }
  return -1;
}

// add-enemy-souls — parse the souls_shuffle tier.
static int parse_souls_shuffle(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0"))    { *out = kSoulsShuffle_Off;    return 0; }
  if (csv_str_eq(v, vlen, "bosses") || csv_str_eq(v, vlen, "1")) { *out = kSoulsShuffle_Bosses; return 0; }
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "2")) {
    *out = kSoulsShuffle_BossesEnemies;
    return 0;
  }
  return -1;
}

// add-rando-grass-rock-shuffle — shared tier grammar for both terrain axes.
static int parse_terrain_shuffle(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0"))  { *out = kTerrainShuffle_Off;  return 0; }
  if (csv_str_eq(v, vlen, "junk") || csv_str_eq(v, vlen, "1")) { *out = kTerrainShuffle_Junk; return 0; }
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "2"))  { *out = kTerrainShuffle_All;  return 0; }
  return -1;
}

static int parse_key_rings(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "off") || csv_str_eq(v, vlen, "0")) {
    *out = kKeyRings_Off;
    return 0;
  }
  if (csv_str_eq(v, vlen, "random") || csv_str_eq(v, vlen, "1")) {
    *out = kKeyRings_Random;
    return 0;
  }
  if (csv_str_eq(v, vlen, "all") || csv_str_eq(v, vlen, "2")) {
    *out = kKeyRings_All;
    return 0;
  }
  return -1;
}

static int parse_hint_tile_coverage(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "full") || csv_str_eq(v, vlen, "all") ||
      csv_str_eq(v, vlen, "15")) {
    *out = kHintTileCoverage_Full;
    return 0;
  }
  if (csv_str_eq(v, vlen, "none") || csv_str_eq(v, vlen, "0")) {
    *out = kHintTileCoverage_None;
    return 0;
  }
  if (csv_str_eq(v, vlen, "sparse") || csv_str_eq(v, vlen, "few") ||
      csv_str_eq(v, vlen, "5")) {
    *out = kHintTileCoverage_Sparse;
    return 0;
  }
  if (csv_str_eq(v, vlen, "standard") || csv_str_eq(v, vlen, "many") ||
      csv_str_eq(v, vlen, "10")) {
    *out = kHintTileCoverage_Standard;
    return 0;
  }
  return -1;
}

static int parse_hint_mix(const char *v, int vlen, uint8 *out) {
  if (csv_str_eq(v, vlen, "variety") || csv_str_eq(v, vlen, "0")) {
    *out = kHintMix_Variety;
    return 0;
  }
  if (csv_str_eq(v, vlen, "important") ||
      csv_str_eq(v, vlen, "important_items") ||
      csv_str_eq(v, vlen, "1")) {
    *out = kHintMix_Important;
    return 0;
  }
  if (csv_str_eq(v, vlen, "difficult") ||
      csv_str_eq(v, vlen, "difficult_checks") ||
      csv_str_eq(v, vlen, "2")) {
    *out = kHintMix_Difficult;
    return 0;
  }
  if (csv_str_eq(v, vlen, "world_info") ||
      csv_str_eq(v, vlen, "world-info") ||
      csv_str_eq(v, vlen, "3")) {
    *out = kHintMix_WorldInfo;
    return 0;
  }
  return -1;
}

static int parse_hint_paid_depth(const char *v, int vlen, uint8 *out) {
  uint32 depth;
  if (parse_uint(v, vlen, &depth) != 0 || depth > 3) return -1;
  *out = (uint8)depth;
  return 0;
}

// Bitmap of keys seen — used to reject duplicate keys per spec.
typedef struct {
  uint64 seen;
  uint8 hint_profile;        // HintProfile; applied after every pair parses
  uint8 hint_tile_override;  // HintTileCoverage
  uint8 hint_mix_override;   // HintMix
  uint8 hint_paid_override;  // actual depth 0..3
} SeenKeys;

#define KEY_BIT(name) (1ull << (name))
enum {
  KEY_world_state = 0,
  KEY_goal,
  KEY_crystals_ganon,
  KEY_crystals_tower,
  KEY_item_pool,
  KEY_dungeon_small_keys,
  KEY_dungeon_big_keys,
  KEY_dungeon_maps,
  KEY_dungeon_compasses,
  KEY_prize_shuffle,
  KEY_medallion_shuffle,
  KEY_mode_weapons,
  KEY_accessibility,
  KEY_pieces_required,
  KEY_pieces_placed,
  // New keys — were in canonical hash but not parseable.
  KEY_tricks,
  KEY_logic,
  KEY_pyramid_bow_upgrade,
  KEY_region_boss_hearts_in_pool,
  KEY_race_mode,
  // Hints axis (Off/Balanced; serialized values remain 0/1).
  KEY_hints,
  KEY_hint_tile_coverage,
  KEY_hint_mix,
  KEY_hint_paid_depth,
  KEY_hint_npc_rewards,
  // Phase B Slice 7 §63 / Slice 8 §64 — shuffle axes (binary on/off).
  KEY_boss_shuffle,
  KEY_drop_shuffle,
  // Phase C — entrance shuffle composable axes (binary on/off). Packed into
  // canonical byte [25]; see RandoSettings header.
  KEY_shuffle_cave_entrances,
  KEY_shuffle_dungeon_entrances,
  KEY_coupled,
  KEY_cross_category,
  KEY_decoupled,
  KEY_shuffle_ganons_tower_entrance,
  // dungeon-chains — boss-chain topology axis (binary). Packed into canonical
  // byte [25] bit6.
  KEY_dungeon_chains,
  // add-rando-enemy-shuffle — enemy (sprite-type) substitution axis (binary).
  KEY_enemy_shuffle,
  // add-rando-door-shuffle — door_shuffle axis (vanilla|basic). Packed into
  // canonical byte [27]; see RandoSettings header.
  KEY_door_shuffle,
  // add-rando-traps — trap frequency (off|low|medium|high). Packed into
  // canonical byte [26] bits 2-3.
  KEY_traps,
  // Randomizer QoL — instant flute activation. Packed inversely into
  // canonical byte [26] bit4.
  KEY_instant_flute,
  // add-rando-trap-catalog — per-category trap enable mask. Packed into canonical
  // byte [27] bits 2-6.
  KEY_trap_categories,
  // add-rando-pot-sanity — pot_shuffle tier (off|keys|contents|all). Packed into
  // canonical [26] bits 6-7 + [27] bit 7.
  KEY_pot_shuffle,
  // add-rando-enemy-drop-sanity — forced enemy small-key drops as checks.
  KEY_enemy_drop_checks,
  // add-enemy-souls — souls_shuffle tier (off|bosses|all).
  KEY_souls_shuffle,
  KEY_npc_souls,
  KEY_grass_shuffle,   // add-rando-grass-rock-shuffle
  KEY_rock_shuffle,
  KEY_key_rings,       // add-rando-key-rings-skeleton-key
  KEY_skeleton_key,
  KEY_flute_shuffle,   // add-rando-ow-warp-shuffle
  KEY_whirlpool_shuffle,
  KEY_shopsanity,      // add-rando-shopsanity
  KEY_bonk_shuffle,    // add-rando-bonk-sanity
};

static int handle_kv(const char *key, int klen, const char *val, int vlen,
                     RandoSettings *s, SeenKeys *seen) {
  #define MARK_SEEN(bit) do { \
    if (seen->seen & KEY_BIT(bit)) { \
      fprintf(stderr, "Settings_ParseCsv: duplicate key '%.*s'\n", klen, key); \
      return -1; \
    } \
    seen->seen |= KEY_BIT(bit); \
  } while (0)

  if (csv_str_eq(key, klen, "mode.state")) {
    MARK_SEEN(KEY_world_state);
    if (parse_world_state(val, vlen, &s->world_state) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "goal")) {
    MARK_SEEN(KEY_goal);
    if (parse_goal(val, vlen, &s->goal) != 0) goto bad_value;
    // Per spec: completionist auto-sets accessibility=locations
    if (s->goal == kGoal_Completionist) s->accessibility = kAccessibility_Locations;
  } else if (csv_str_eq(key, klen, "crystals.ganon")) {
    MARK_SEEN(KEY_crystals_ganon);
    // add-rando-random-crystals: the `random` KEYWORD maps to the requested
    // sentinel (effective count seed-resolved at generation/load). Numeric 8
    // stays rejected — the CSV surface keeps its historical 0..7 numeric
    // contract (Settings_SelfCheck pins it); the sentinel is keyword-only.
    if (csv_str_eq(val, vlen, "random")) {
      s->crystals_ganon = kCrystalsRandom;
    } else {
      uint32 v;
      if (parse_uint(val, vlen, &v) != 0 || v > 7) goto bad_value;
      s->crystals_ganon = (uint8)v;
    }
  } else if (csv_str_eq(key, klen, "crystals.tower")) {
    MARK_SEEN(KEY_crystals_tower);
    if (csv_str_eq(val, vlen, "random")) {
      s->crystals_tower = kCrystalsRandom;
    } else {
      uint32 v;
      if (parse_uint(val, vlen, &v) != 0 || v > 7) goto bad_value;
      s->crystals_tower = (uint8)v;
    }
  } else if (csv_str_eq(key, klen, "item_pool") || csv_str_eq(key, klen, "item.pool")) {
    MARK_SEEN(KEY_item_pool);
    if (parse_pool_diff(val, vlen, &s->item_pool_difficulty) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "dungeon_items.small_keys")) {
    MARK_SEEN(KEY_dungeon_small_keys);
    if (parse_dungeon_mode(val, vlen, &s->dungeon_small_keys_mode) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "dungeon_items.big_keys")) {
    MARK_SEEN(KEY_dungeon_big_keys);
    if (parse_dungeon_mode(val, vlen, &s->dungeon_big_keys_mode) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "dungeon_items.maps")) {
    MARK_SEEN(KEY_dungeon_maps);
    if (parse_dungeon_mode(val, vlen, &s->dungeon_maps_mode) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "dungeon_items.compasses")) {
    MARK_SEEN(KEY_dungeon_compasses);
    if (parse_dungeon_mode(val, vlen, &s->dungeon_compasses_mode) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "prize_shuffle")) {
    MARK_SEEN(KEY_prize_shuffle);
    if (parse_bool(val, vlen, &s->prize_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "medallion_shuffle")) {
    MARK_SEEN(KEY_medallion_shuffle);
    if (parse_bool(val, vlen, &s->medallion_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "mode.weapons")) {
    MARK_SEEN(KEY_mode_weapons);
    if (parse_weapons(val, vlen, &s->mode_weapons) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "accessibility")) {
    MARK_SEEN(KEY_accessibility);
    if (parse_accessibility(val, vlen, &s->accessibility) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "pieces_required")) {
    MARK_SEEN(KEY_pieces_required);
    uint32 v;
    if (parse_uint(val, vlen, &v) != 0 || v > 65535) goto bad_value;
    s->pieces_required = (uint16)v;
  } else if (csv_str_eq(key, klen, "pieces_placed")) {
    MARK_SEEN(KEY_pieces_placed);
    uint32 v;
    if (parse_uint(val, vlen, &v) != 0 || v > 65535) goto bad_value;
    s->pieces_placed = (uint16)v;
  } else if (csv_str_eq(key, klen, "tricks")) {
    // Phase B Slice 4 — extended grammar for the `tricks` setting. Accepts:
    //   tricks=none               → bitmask 0 (Phase A pinned default)
    //   tricks=0                  → bitmask 0 (alias)
    //   tricks=0xNN               → raw 8-bit bitmask
    //   tricks=<bit-name>         → single trick bit
    //   tricks=<n1>+<n2>+<n3>     → multiple trick bits (semicolons would
    //                              collide with the CSV separator, so we use
    //                              '+' as the intra-value list delimiter)
    //
    // Bit-name table MUST stay in sync with assets/rando/op_registry.yaml
    // `tricks:` section. When that section grows, mirror new entries here
    // (consider codegen if the list grows past a dozen).
    MARK_SEEN(KEY_tricks);
    if (csv_str_eq(val, vlen, "none")) {
      s->tricks = 0;
    } else if (vlen >= 2 && val[0] == '0' && (val[1] == 'x' || val[1] == 'X')) {
      // Hex bitmask form — digits AFTER the prefix (FIX #17a: parse_uint is
      // decimal-only, so the old prefixed pass-through rejected every 0xNN).
      uint32 v;
      if (parse_hex(val + 2, vlen - 2, &v) != 0 || v > 0xFF) goto bad_value;
      s->tricks = (uint8)v;
    } else if (val[0] >= '0' && val[0] <= '9') {
      // Plain decimal bitmask (covers the documented "0" alias; trick names
      // are kebab ids and never start with a digit, so no ambiguity).
      uint32 v;
      if (parse_uint(val, vlen, &v) != 0 || v > 0xFF) goto bad_value;
      s->tricks = (uint8)v;
    } else {
      static const struct { const char *name; uint8 bit; } kTrickNames[] = {
        { "boots-clip",     0 },
        { "fake-flippers",  1 },
        { "bunny-revival",  2 },
        { "dark-room-nav",  3 },
        { "bomb-jump",      4 },
        { "pearl-bypass",   5 },
        { "hookshot-clip",  6 },
        { "lobotomy",       7 },
      };
      uint8 mask = 0;
      const char *p = val;
      const char *end = val + vlen;
      while (p < end) {
        const char *tok_start = p;
        while (p < end && *p != '+') p++;
        size_t tok_len = (size_t)(p - tok_start);
        if (tok_len == 0) goto bad_value;
        int found = 0;
        for (size_t i = 0; i < sizeof(kTrickNames) / sizeof(kTrickNames[0]); i++) {
          size_t nlen = 0;
          while (kTrickNames[i].name[nlen] != '\0') nlen++;
          if (nlen == tok_len &&
              csv_str_eq(tok_start, (int)tok_len, kTrickNames[i].name)) {
            mask |= (uint8)(1u << kTrickNames[i].bit);
            found = 1;
            break;
          }
        }
        if (!found) goto bad_value;
        if (p < end) p++;  // skip '+'
      }
      s->tricks = mask;
    }
  } else if (csv_str_eq(key, klen, "logic")) {
    // Phase B Slice 4 un-pinned OWG / MajorGlitches; Phase D
    // (add-rando-major-glitch §2) lifts the HybridMG / NoLogic ceiling.
    //   logic=NoGlitches | none | 0                       → 0 (default)
    //   logic=OverworldGlitches | overworld_glitches | 1  → 1
    //   logic=MajorGlitches | major_glitches | 2          → 2
    //   logic=HybridMG | hybrid_mg | hybrid_major_glitches | 3 → 3
    //   logic=NoLogic | no_logic | 4                      → 4
    // NOTE: per the ALTTPR tier model (config/logic.php) MajorGlitches is the
    // MOST permissive tier; HybridMG (3) is numerically above it but a technique
    // SUBSET. NoLogic (4) short-circuits reachability entirely. See design.md.
    MARK_SEEN(KEY_logic);
    if (csv_str_eq(val, vlen, "NoGlitches") || csv_str_eq(val, vlen, "none") || csv_str_eq(val, vlen, "0")) {
      s->logic = 0;
    } else if (csv_str_eq(val, vlen, "OverworldGlitches") || csv_str_eq(val, vlen, "overworld_glitches") || csv_str_eq(val, vlen, "1")) {
      s->logic = 1;
    } else if (csv_str_eq(val, vlen, "MajorGlitches") || csv_str_eq(val, vlen, "major_glitches") || csv_str_eq(val, vlen, "2")) {
      s->logic = 2;
    } else if (csv_str_eq(val, vlen, "HybridMG") || csv_str_eq(val, vlen, "hybrid_mg") ||
               csv_str_eq(val, vlen, "hybrid_major_glitches") || csv_str_eq(val, vlen, "3")) {
      s->logic = 3;
    } else if (csv_str_eq(val, vlen, "NoLogic") || csv_str_eq(val, vlen, "no_logic") || csv_str_eq(val, vlen, "4")) {
      s->logic = 4;
    } else {
      goto bad_value;
    }
  } else if (csv_str_eq(key, klen, "pyramid_bow_upgrade") ||
             csv_str_eq(key, klen, "region.pyramidBowUpgrade")) {
    MARK_SEEN(KEY_pyramid_bow_upgrade);
    if (csv_str_eq(val, vlen, "silvers")) {
      s->pyramid_bow_upgrade = kPyramidBowUpgrade_Silvers;
    } else {
      // Phase A pins to silvers; arrows is Phase B reserved.
      goto bad_value;
    }
  } else if (csv_str_eq(key, klen, "region.bossHeartsInPool") ||
             csv_str_eq(key, klen, "region_boss_hearts_in_pool")) {
    MARK_SEEN(KEY_region_boss_hearts_in_pool);
    if (parse_bool(val, vlen, &s->region_boss_hearts_in_pool) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "race_mode") ||
             csv_str_eq(key, klen, "race")) {
    MARK_SEEN(KEY_race_mode);
    if (parse_bool(val, vlen, &s->race_mode) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "boss_shuffle")) {
    // Phase B Slice 7 §63 — boss-shuffle axis. Binary on/off. NOT YET
    // in canonical serialization (deferred — see RandoSettings header).
    MARK_SEEN(KEY_boss_shuffle);
    if (parse_bool(val, vlen, &s->boss_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "drop_shuffle")) {
    // Phase B Slice 8 §64 — drop-shuffle axis. Binary on/off.
    MARK_SEEN(KEY_drop_shuffle);
    if (parse_bool(val, vlen, &s->drop_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "door_shuffle")) {
    // add-rando-door-shuffle — intra-dungeon door shuffle (vanilla|basic).
    // Packed into canonical pad byte [27] bits 0-1 (see RandoSettings header).
    MARK_SEEN(KEY_door_shuffle);
    if (csv_str_eq(val, vlen, "vanilla")) s->door_shuffle = kDoorShuffle_Vanilla;
    else if (csv_str_eq(val, vlen, "basic")) s->door_shuffle = kDoorShuffle_Basic;
    else goto bad_value;
  } else if (csv_str_eq(key, klen, "enemy_shuffle")) {
    // add-rando-enemy-shuffle — enemy (sprite-type) substitution axis. Binary
    // on/off. Packed into canonical pad byte [26] bit0 (see RandoSettings header).
    MARK_SEEN(KEY_enemy_shuffle);
    if (parse_bool(val, vlen, &s->enemy_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "traps") ||
             csv_str_eq(key, klen, "trap_frequency")) {
    // add-rando-traps — frequency for masquerade trap junk replacements.
    // Both keys are aliases for the same canonical field and duplicate-check bit.
    MARK_SEEN(KEY_traps);
    if (parse_traps(val, vlen, &s->traps) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "trap_categories")) {
    // add-rando-trap-catalog — per-category enable mask (canonical [27] bits 2-6).
    // "all"/"none"/0 = the all-categories sentinel; or a '+'-joined name list.
    MARK_SEEN(KEY_trap_categories);
    if (parse_trap_categories(val, vlen, &s->trap_categories) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "pot_shuffle")) {
    // add-rando-pot-sanity — tiered pot shuffle (off|keys|contents|all). Packed
    // into canonical [26] bits 6-7 + [27] bit 7 (see RandoSettings header).
    MARK_SEEN(KEY_pot_shuffle);
    if (parse_pot_shuffle(val, vlen, &s->pot_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "enemy_drop_checks")) {
    // add-rando-enemy-drop-sanity — forced enemy small-key drops as checks.
    // Serialized as append-only canonical byte [28].
    MARK_SEEN(KEY_enemy_drop_checks);
    if (parse_enemy_drop_checks(val, vlen, &s->enemy_drop_checks) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "souls_shuffle")) {
    // add-enemy-souls — soul items gate enemy/boss spawns (off|bosses|all).
    // Serialized in canonical [28] bits 2-3.
    MARK_SEEN(KEY_souls_shuffle);
    if (parse_souls_shuffle(val, vlen, &s->souls_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "npc_souls")) {
    // add-npc-souls — NPC soul items gate check-giving people. Binary
    // true/false (parse_bool, the enemy_shuffle convention). Serialized in
    // canonical [28] bit 4; independent of souls_shuffle.
    MARK_SEEN(KEY_npc_souls);
    if (parse_bool(val, vlen, &s->npc_souls) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "grass_shuffle")) {
    // add-rando-grass-rock-shuffle — bushes + thick grass as checks
    // (off|junk|all). Serialized in the appended canonical [29] bits 0-1.
    MARK_SEEN(KEY_grass_shuffle);
    if (parse_terrain_shuffle(val, vlen, &s->grass_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "rock_shuffle")) {
    // add-rando-grass-rock-shuffle — light/heavy rocks + piles as checks
    // (off|junk|all). Serialized in canonical [29] bits 2-3.
    MARK_SEEN(KEY_rock_shuffle);
    if (parse_terrain_shuffle(val, vlen, &s->rock_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "flute_shuffle")) {
    // add-rando-ow-warp-shuffle — flute-spot shuffle (off|balanced|random).
    // Serialized in canonical [30] bits 4-5.
    MARK_SEEN(KEY_flute_shuffle);
    if (parse_flute_shuffle(val, vlen, &s->flute_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "whirlpool_shuffle")) {
    // add-rando-ow-warp-shuffle — whirlpool re-pairing. Canonical [30] bit 3.
    MARK_SEEN(KEY_whirlpool_shuffle);
    if (parse_bool(val, vlen, &s->whirlpool_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "key_rings")) {
    MARK_SEEN(KEY_key_rings);
    if (parse_key_rings(val, vlen, &s->key_rings) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "skeleton_key")) {
    MARK_SEEN(KEY_skeleton_key);
    if (parse_bool(val, vlen, &s->skeleton_key) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "shopsanity")) {
    // add-rando-shopsanity — shop slots as one-time purchase checks.
    MARK_SEEN(KEY_shopsanity);
    if (parse_bool(val, vlen, &s->shopsanity) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "bonk_shuffle")) {
    // add-rando-bonk-sanity — placed OW bonk sprites as checks (off/junk/all).
    MARK_SEEN(KEY_bonk_shuffle);
    if (parse_terrain_shuffle(val, vlen, &s->bonk_shuffle) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "instant_flute")) {
    // Randomizer QoL — seed-burned flute activation behavior. Default true.
    MARK_SEEN(KEY_instant_flute);
    if (parse_bool(val, vlen, &s->instant_flute) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "shuffle_cave_entrances")) {
    // Phase C — entrance shuffle: cave-door class.
    MARK_SEEN(KEY_shuffle_cave_entrances);
    if (parse_bool(val, vlen, &s->shuffle_cave_entrances) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "shuffle_dungeon_entrances")) {
    // Phase C — entrance shuffle: dungeon-door class (Stage 2).
    MARK_SEEN(KEY_shuffle_dungeon_entrances);
    if (parse_bool(val, vlen, &s->shuffle_dungeon_entrances) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "coupled")) {
    // Phase C — coupled return (enter A ⇒ exit A). Default on.
    MARK_SEEN(KEY_coupled);
    if (parse_bool(val, vlen, &s->coupled) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "cross_category")) {
    // Phase C — caves↔dungeons may mix (Stage 3).
    MARK_SEEN(KEY_cross_category);
    if (parse_bool(val, vlen, &s->cross_category) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "decoupled")) {
    // Phase C — per-endpoint independent shuffle (Stage 4); implies !coupled.
    MARK_SEEN(KEY_decoupled);
    if (parse_bool(val, vlen, &s->decoupled) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "shuffle_ganons_tower_entrance")) {
    // Phase C — advanced opt-in: include Ganon's Tower in the dungeon pool.
    MARK_SEEN(KEY_shuffle_ganons_tower_entrance);
    if (parse_bool(val, vlen, &s->shuffle_ganons_tower_entrance) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "dungeon_chains")) {
    // dungeon-chains — boss-chain topology axis.
    MARK_SEEN(KEY_dungeon_chains);
    if (parse_bool(val, vlen, &s->dungeon_chains) != 0) goto bad_value;
  } else if (csv_str_eq(key, klen, "hints")) {
    // Public profile base. Defer application until the entire CSV is parsed so
    // component keys override this tuple independent of textual order. Retain
    // all historical aliases for the exact Balanced/Off tuples.
    MARK_SEEN(KEY_hints);
    if (csv_str_eq(val, vlen, "off") || csv_str_eq(val, vlen, "0") ||
        csv_str_eq(val, vlen, "false") || csv_str_eq(val, vlen, "False") ||
        csv_str_eq(val, vlen, "none")) {
      seen->hint_profile = kHintProfile_Off;
    } else if (csv_str_eq(val, vlen, "sparse")) {
      seen->hint_profile = kHintProfile_Sparse;
    } else if (csv_str_eq(val, vlen, "balanced") ||
               csv_str_eq(val, vlen, "on") || csv_str_eq(val, vlen, "1") ||
               csv_str_eq(val, vlen, "true") || csv_str_eq(val, vlen, "True") ||
               csv_str_eq(val, vlen, "sahasrahla") || csv_str_eq(val, vlen, "full")) {
      seen->hint_profile = kHintProfile_Balanced;
    } else if (csv_str_eq(val, vlen, "direct")) {
      seen->hint_profile = kHintProfile_Direct;
    } else {
      // `custom` is derived by UIs from a non-profile tuple, never accepted as
      // an independently stored or parsed value.
      goto bad_value;
    }
  } else if (csv_str_eq(key, klen, "hint_tiles") ||
             csv_str_eq(key, klen, "hint_tile_coverage")) {
    MARK_SEEN(KEY_hint_tile_coverage);
    if (parse_hint_tile_coverage(val, vlen, &seen->hint_tile_override) != 0)
      goto bad_value;
  } else if (csv_str_eq(key, klen, "hint_mix")) {
    MARK_SEEN(KEY_hint_mix);
    if (parse_hint_mix(val, vlen, &seen->hint_mix_override) != 0)
      goto bad_value;
  } else if (csv_str_eq(key, klen, "hint_paid") ||
             csv_str_eq(key, klen, "hint_paid_depth")) {
    MARK_SEEN(KEY_hint_paid_depth);
    if (parse_hint_paid_depth(val, vlen, &seen->hint_paid_override) != 0)
      goto bad_value;
  } else if (csv_str_eq(key, klen, "hint_npc_reward_reveal") ||
             csv_str_eq(key, klen, "hint_npc_rewards") ||
             csv_str_eq(key, klen, "hint_vendor_items") ||
             csv_str_eq(key, klen, "hint_npc_items")) {
    // Independent pre-commit NPC reward disclosure. Every spelling is one
    // canonical setting and therefore shares duplicate detection.
    MARK_SEEN(KEY_hint_npc_rewards);
    if (parse_bool(val, vlen, &s->hint_npc_reward_reveal) != 0)
      goto bad_value;
  } else {
    fprintf(stderr, "Settings_ParseCsv: unknown key '%.*s'\n", klen, key);
    return -1;
  }
  return 0;
bad_value:
  fprintf(stderr, "Settings_ParseCsv: bad value '%.*s' for key '%.*s'\n",
          vlen, val, klen, key);
  return -1;
  #undef MARK_SEEN
}

// ---------------------------------------------------------------------------
// Settings presets (tasks.md §9.5)
// ---------------------------------------------------------------------------
static const char *kPresetNames[kPreset__Count] = {
  "Open Ganon",
  "Standard Ganon",
  "Inverted Ganon",
  "Retro",
  "Triforce Hunt Default",
};

const char *Settings_PresetName(SettingsPreset preset) {
  if ((int)preset < 0 || (int)preset >= kPreset__Count) return "(unknown)";
  return kPresetNames[preset];
}

int Settings_ApplyPreset(SettingsPreset preset, RandoSettings *out) {
  if (out == NULL) return -1;
  Settings_SetDefaults(out);  // shared baseline; each preset only overrides specifics
  switch (preset) {
    case kPreset_OpenGanon:
      // Defaults are already Open / FastGanon / 7-7. No further override.
      return 0;
    case kPreset_StandardGanon:
      out->world_state = kWorldState_Standard;
      out->goal = kGoal_FastGanon;
      return 0;
    case kPreset_InvertedGanon:
      out->world_state = kWorldState_Inverted;
      out->goal = kGoal_FastGanon;
      return 0;
    case kPreset_Retro:
      out->world_state = kWorldState_Retro;
      out->goal = kGoal_Ganon;  // Retro convention: defeat Ganon, no fast-ganon shortcut
      return 0;
    case kPreset_TriforceHuntDefault:
      out->goal = kGoal_TriforceHunt;
      out->pieces_required = 20;
      out->pieces_placed = 30;
      return 0;
    default:
      return -1;
  }
}

int Settings_ParseCsv(const char *csv, RandoSettings *out) {
  if (csv == NULL || out == NULL) return -1;
  SeenKeys seen = { 0 };
  const char *p = csv;
  while (*p) {
    // Skip leading whitespace.
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    // Parse key up to '='.
    const char *k_start = p;
    while (*p && *p != '=' && *p != ',') p++;
    if (*p != '=') {
      fprintf(stderr, "Settings_ParseCsv: expected '=' near '%.*s'\n",
              (int)(p - k_start), k_start);
      return -1;
    }
    int klen = (int)(p - k_start);
    p++;  // skip '='
    // Parse value up to ','.
    const char *v_start = p;
    while (*p && *p != ',') p++;
    int vlen = (int)(p - v_start);
    if (klen == 0 || vlen == 0) {
      fprintf(stderr, "Settings_ParseCsv: empty key or value\n");
      return -1;
    }
    if (handle_kv(k_start, klen, v_start, vlen, out, &seen) != 0) {
      return -1;
    }
    if (*p == ',') p++;
  }
  // Validate pieces_required <= pieces_placed for Triforce/Ganon-Hunt goals.
  if ((out->goal == kGoal_TriforceHunt || out->goal == kGoal_GanonHunt) &&
      out->pieces_required > out->pieces_placed) {
    fprintf(stderr, "Settings_ParseCsv: pieces_required (%u) > pieces_placed (%u)\n",
            out->pieces_required, out->pieces_placed);
    return -1;
  }

  // Apply the profile base only after parsing every pair, then layer component
  // overrides. This gives the same result for
  // `hints=sparse,hint_mix=difficult` and the reversed order. Off deliberately
  // wins over (and clears) component overrides.
  bool has_profile = (seen.seen & KEY_BIT(KEY_hints)) != 0;
  HintProfile profile = (HintProfile)seen.hint_profile;
  bool off_profile = has_profile && profile == kHintProfile_Off;
  if (has_profile && !Settings_ApplyHintProfile(out, profile))
    return -1;  // parser accepted only the four applicable named profiles
  if (!off_profile) {
    if (seen.seen & KEY_BIT(KEY_hint_tile_coverage))
      out->hint_tile_coverage = seen.hint_tile_override;
    if (seen.seen & KEY_BIT(KEY_hint_mix))
      out->hint_mix = seen.hint_mix_override;
    if (seen.seen & KEY_BIT(KEY_hint_paid_depth))
      out->hint_paid_depth = seen.hint_paid_override;
  }

  // Off and None+depth0 each have one canonical disabled representation.
  Settings_NormalizeHintPolicy(out);
  return 0;
}
