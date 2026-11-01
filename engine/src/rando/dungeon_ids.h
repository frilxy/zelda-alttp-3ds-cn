#ifndef ZELDA3_RANDO_DUNGEON_IDS_H_
#define ZELDA3_RANDO_DUNGEON_IDS_H_

#include "../types.h"

#ifndef __cplusplus
#include "item_ids.h"
#include "location_ids.h"
#endif

// Dungeon ID domains used by the randomizer boundary code.
//
// RandoDungeonId mirrors ALTTPR/op_registry order and is used by logic,
// placement, prize assignment, boss shuffle, spoilers, and generated data.
// GameDungeonId mirrors the live engine's `cur_palace_index_x2 >> 1` order and
// is used by engine RAM cells: link_bigkey/link_dungeon_map/link_compass bits,
// link_keys_earned_per_dungeon[], boss drop dispatch, and HUD/debug/tracker
// snapshots. Keep conversions here; do not hand-roll one-off tables at call
// sites.
enum {
  kRandoDungeon_None = 0xff,
  kRandoDungeon_HyruleCastleEscape = 0,
  kRandoDungeon_EasternPalace = 1,
  kRandoDungeon_DesertPalace = 2,
  kRandoDungeon_TowerOfHera = 3,
  kRandoDungeon_HyruleCastleTower = 4,
  kRandoDungeon_PalaceOfDarkness = 5,
  kRandoDungeon_SwampPalace = 6,
  kRandoDungeon_SkullWoods = 7,
  kRandoDungeon_ThievesTown = 8,
  kRandoDungeon_IcePalace = 9,
  kRandoDungeon_MiseryMire = 10,
  kRandoDungeon_TurtleRock = 11,
  kRandoDungeon_GanonsTower = 12,
  kRandoDungeon_Count = 13,
};

enum {
  kGameDungeon_None = 0xff,
  kGameDungeon_HyruleCastleEscape = 0,
  kGameDungeon_HyruleCastle = 1,
  kGameDungeon_EasternPalace = 2,
  kGameDungeon_DesertPalace = 3,
  kGameDungeon_HyruleCastleTower = 4,
  kGameDungeon_SwampPalace = 5,
  kGameDungeon_PalaceOfDarkness = 6,
  kGameDungeon_MiseryMire = 7,
  kGameDungeon_SkullWoods = 8,
  kGameDungeon_IcePalace = 9,
  kGameDungeon_TowerOfHera = 10,
  kGameDungeon_ThievesTown = 11,
  kGameDungeon_TurtleRock = 12,
  kGameDungeon_GanonsTower = 13,
  kGameDungeon_Count = 14,
};

static inline uint8 Rando_GameDungeonFromRawPalace(uint8 raw_palace_x2) {
  return raw_palace_x2 == 0xff ? (uint8)kGameDungeon_None
                               : (uint8)(raw_palace_x2 >> 1);
}

static inline uint8 Rando_KeySlotFromGameDungeon(uint8 game_dungeon) {
  if (game_dungeon == kGameDungeon_None || game_dungeon >= 16)
    return (uint8)kGameDungeon_None;
  // Hyrule Castle proper (game slot 1) saves small keys into the escape/sewers
  // slot 0. Other game dungeon ids use their own slot.
  return game_dungeon == kGameDungeon_HyruleCastle
             ? (uint8)kGameDungeon_HyruleCastleEscape
             : game_dungeon;
}

static inline uint8 Rando_KeySlotFromRawPalace(uint8 raw_palace_x2) {
  uint8 game_dungeon = Rando_GameDungeonFromRawPalace(raw_palace_x2);
  return Rando_KeySlotFromGameDungeon(game_dungeon);
}

static inline uint16 Rando_DungeonBitForGameDungeon(uint8 game_dungeon) {
  if (game_dungeon >= 16) return 0;
  return (uint16)(0x8000u >> game_dungeon);
}

static inline uint8 Rando_GameDungeonFromRandoDungeon(uint8 rando_dungeon) {
  static const uint8 kGameFromRando[kRandoDungeon_Count] = {
    kGameDungeon_HyruleCastleEscape, // HCE
    kGameDungeon_EasternPalace,      // EP
    kGameDungeon_DesertPalace,       // DP
    kGameDungeon_TowerOfHera,        // TH
    kGameDungeon_HyruleCastleTower,  // HCT
    kGameDungeon_PalaceOfDarkness,   // PoD
    kGameDungeon_SwampPalace,        // SP
    kGameDungeon_SkullWoods,         // SW
    kGameDungeon_ThievesTown,        // TT
    kGameDungeon_IcePalace,          // IP
    kGameDungeon_MiseryMire,         // MM
    kGameDungeon_TurtleRock,         // TR
    kGameDungeon_GanonsTower,        // GT
  };
  return rando_dungeon < kRandoDungeon_Count ? kGameFromRando[rando_dungeon]
                                             : (uint8)kGameDungeon_None;
}

static inline uint8 Rando_RandoDungeonFromGameDungeon(uint8 game_dungeon) {
  static const uint8 kRandoFromGame[16] = {
    kRandoDungeon_HyruleCastleEscape, //  0 HCE
    kRandoDungeon_None,               //  1 HC proper has no ALTTPR dungeon id
    kRandoDungeon_EasternPalace,       //  2 EP
    kRandoDungeon_DesertPalace,        //  3 DP
    kRandoDungeon_HyruleCastleTower,   //  4 HCT
    kRandoDungeon_SwampPalace,         //  5 SP
    kRandoDungeon_PalaceOfDarkness,    //  6 PoD
    kRandoDungeon_MiseryMire,          //  7 MM
    kRandoDungeon_SkullWoods,          //  8 SW
    kRandoDungeon_IcePalace,           //  9 IP
    kRandoDungeon_TowerOfHera,         // 10 TH
    kRandoDungeon_ThievesTown,         // 11 TT
    kRandoDungeon_TurtleRock,          // 12 TR
    kRandoDungeon_GanonsTower,         // 13 GT
    kRandoDungeon_None,
    kRandoDungeon_None,
  };
  return game_dungeon < 16 ? kRandoFromGame[game_dungeon]
                           : (uint8)kRandoDungeon_None;
}

// Hyrule Castle proper uses its own live engine dungeon slot, but its map and
// randomizer checks belong to the shared Hyrule Castle Escape / Sewers bucket.
// Keep the general game->rando conversion strict and fold only map displays.
static inline uint8 Rando_MapDisplayDungeonFromGameDungeon(uint8 game_dungeon) {
  return game_dungeon == kGameDungeon_HyruleCastle
             ? (uint8)kRandoDungeon_HyruleCastleEscape
             : Rando_RandoDungeonFromGameDungeon(game_dungeon);
}

static inline uint8 Rando_PrizeRandoDungeonFromGameDungeon(uint8 game_dungeon) {
  uint8 rando_dungeon = Rando_RandoDungeonFromGameDungeon(game_dungeon);
  switch (rando_dungeon) {
    case kRandoDungeon_EasternPalace:
    case kRandoDungeon_DesertPalace:
    case kRandoDungeon_TowerOfHera:
    case kRandoDungeon_PalaceOfDarkness:
    case kRandoDungeon_SwampPalace:
    case kRandoDungeon_SkullWoods:
    case kRandoDungeon_ThievesTown:
    case kRandoDungeon_IcePalace:
    case kRandoDungeon_MiseryMire:
    case kRandoDungeon_TurtleRock:
      return rando_dungeon;
    default:
      return (uint8)kRandoDungeon_None;
  }
}

#ifndef __cplusplus

// Key Rings are append-only and deliberately mirror the 13 contiguous
// SmallKey_* families. Keep all conversions at this boundary so placement,
// logic, runtime grants, spoilers, and trackers cannot drift into hand-written
// range arithmetic.
static inline bool Rando_IsSmallKeyItem(uint16 item_id) {
  return item_id >= ITEM_SmallKey_HyruleCastleEscape &&
         item_id <= ITEM_SmallKey_GanonsTower;
}

static inline bool Rando_IsKeyRingItem(uint16 item_id) {
  return item_id >= ITEM_KeyRing_HyruleCastleEscape &&
         item_id <= ITEM_KeyRing_GanonsTower;
}

// Customizer pool edits may relocate these items but must never mint or remove
// copies: their counts are derived from the selected key model.
static inline bool Rando_IsFixedKeyCardinalityItem(uint16 item_id) {
  return Rando_IsSmallKeyItem(item_id) || Rando_IsKeyRingItem(item_id) ||
         item_id == ITEM_SkeletonKey;
}

static inline uint16 Rando_SmallKeyItemForRandoDungeon(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count
             ? (uint16)(ITEM_SmallKey_HyruleCastleEscape + rando_dungeon)
             : 0xFFFFu;
}

static inline uint16 Rando_KeyRingItemForRandoDungeon(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count
             ? (uint16)(ITEM_KeyRing_HyruleCastleEscape + rando_dungeon)
             : 0xFFFFu;
}

// Complete authored small-key stock: base shuffled chest/placed copies plus all
// authored drop sources (pot/enemy/free). The two component rows mirror the
// authoritative placement and generated door-drop metadata; placement
// selfchecks cross-check them so future source changes cannot silently leave a
// stale ring grant. All totals stay below the engine's 0xff sentinel.
static const uint8 kRandoBaseSmallKeyCount[kRandoDungeon_Count] = {
  1, 0, 1, 1, 2, 6, 1, 3, 1, 2, 3, 4, 4,
};
static const uint8 kRandoDropSmallKeyCount[kRandoDungeon_Count] = {
  3, 2, 3, 0, 2, 0, 5, 2, 2, 4, 3, 2, 4,
};

static inline uint8 Rando_BaseSmallKeyCount(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count
             ? kRandoBaseSmallKeyCount[rando_dungeon]
             : 0;
}

static inline uint8 Rando_DropSmallKeyCount(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count
             ? kRandoDropSmallKeyCount[rando_dungeon]
             : 0;
}

static inline uint8 Rando_KeyRingGrantCount(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count
             ? (uint8)(kRandoBaseSmallKeyCount[rando_dungeon] +
                       kRandoDropSmallKeyCount[rando_dungeon])
             : 0;
}

_Static_assert(ITEM_KeyRing_GanonsTower - ITEM_KeyRing_HyruleCastleEscape + 1 ==
                   kRandoDungeon_Count,
               "Key Ring item range must cover all randomizer dungeons");
_Static_assert(ITEM_KeyRing_HyruleCastleEscape - ITEM_SmallKey_HyruleCastleEscape ==
                   ITEM_KeyRing_GanonsTower - ITEM_SmallKey_GanonsTower,
               "Small-key and Key Ring ranges must be parallel");
_Static_assert(ITEM_SkeletonKey == ITEM_KeyRing_GanonsTower + 1,
               "Skeleton Key must remain directly after the Key Ring block");

static inline uint8 Rando_RandoDungeonFromDungeonItem(uint16 item_id) {
  // SmallKey ids 53..65 are contiguous in ALTTPR dungeon order.
  if (Rando_IsSmallKeyItem(item_id))
    return (uint8)(item_id - ITEM_SmallKey_HyruleCastleEscape);
  // KeyRing ids 220..232 use the exact same family order.
  if (Rando_IsKeyRingItem(item_id))
    return (uint8)(item_id - ITEM_KeyRing_HyruleCastleEscape);
  // BigKey ids 66..76 skip HCE and HCT; their order is EP, DP, TH, PoD, SP,
  // SW, TT, IP, MM, TR, GT.
  static const uint8 kBigMapCompassRandoDungeon[11] = {
    kRandoDungeon_EasternPalace,
    kRandoDungeon_DesertPalace,
    kRandoDungeon_TowerOfHera,
    kRandoDungeon_PalaceOfDarkness,
    kRandoDungeon_SwampPalace,
    kRandoDungeon_SkullWoods,
    kRandoDungeon_ThievesTown,
    kRandoDungeon_IcePalace,
    kRandoDungeon_MiseryMire,
    kRandoDungeon_TurtleRock,
    kRandoDungeon_GanonsTower,
  };
  if (item_id >= ITEM_BigKey_EasternPalace && item_id <= ITEM_BigKey_GanonsTower)
    return kBigMapCompassRandoDungeon[item_id - ITEM_BigKey_EasternPalace];
  if (item_id == ITEM_Map_HyruleCastleEscape)
    return (uint8)kRandoDungeon_HyruleCastleEscape;
  if (item_id >= ITEM_Map_EasternPalace && item_id <= ITEM_Map_GanonsTower)
    return kBigMapCompassRandoDungeon[item_id - ITEM_Map_EasternPalace];
  if (item_id >= ITEM_Compass_EasternPalace && item_id <= ITEM_Compass_GanonsTower)
    return kBigMapCompassRandoDungeon[item_id - ITEM_Compass_EasternPalace];
  return (uint8)kRandoDungeon_None;
}

static inline uint8 Rando_DungeonItemGameDungeon(uint16 item_id) {
  uint8 rando_dungeon = Rando_RandoDungeonFromDungeonItem(item_id);
  return rando_dungeon == kRandoDungeon_None
             ? (uint8)kGameDungeon_None
             : Rando_GameDungeonFromRandoDungeon(rando_dungeon);
}

static inline uint16 Rando_SmallKeyItemForGameDungeon(uint8 game_dungeon) {
  uint8 rando_dungeon = Rando_RandoDungeonFromGameDungeon(game_dungeon);
  return Rando_SmallKeyItemForRandoDungeon(rando_dungeon);
}

static inline uint16 Rando_BigKeyItemForGameDungeon(uint8 game_dungeon) {
  switch (Rando_RandoDungeonFromGameDungeon(game_dungeon)) {
    case kRandoDungeon_EasternPalace:     return ITEM_BigKey_EasternPalace;
    case kRandoDungeon_DesertPalace:      return ITEM_BigKey_DesertPalace;
    case kRandoDungeon_TowerOfHera:       return ITEM_BigKey_TowerOfHera;
    case kRandoDungeon_PalaceOfDarkness:  return ITEM_BigKey_PalaceOfDarkness;
    case kRandoDungeon_SwampPalace:       return ITEM_BigKey_SwampPalace;
    case kRandoDungeon_SkullWoods:        return ITEM_BigKey_SkullWoods;
    case kRandoDungeon_ThievesTown:       return ITEM_BigKey_ThievesTown;
    case kRandoDungeon_IcePalace:         return ITEM_BigKey_IcePalace;
    case kRandoDungeon_MiseryMire:        return ITEM_BigKey_MiseryMire;
    case kRandoDungeon_TurtleRock:        return ITEM_BigKey_TurtleRock;
    case kRandoDungeon_GanonsTower:       return ITEM_BigKey_GanonsTower;
    default: return 0xFFFFu;
  }
}

static inline uint16 Rando_MapItemForGameDungeon(uint8 game_dungeon) {
  switch (Rando_RandoDungeonFromGameDungeon(game_dungeon)) {
    case kRandoDungeon_HyruleCastleEscape:return ITEM_Map_HyruleCastleEscape;
    case kRandoDungeon_EasternPalace:     return ITEM_Map_EasternPalace;
    case kRandoDungeon_DesertPalace:      return ITEM_Map_DesertPalace;
    case kRandoDungeon_TowerOfHera:       return ITEM_Map_TowerOfHera;
    case kRandoDungeon_PalaceOfDarkness:  return ITEM_Map_PalaceOfDarkness;
    case kRandoDungeon_SwampPalace:       return ITEM_Map_SwampPalace;
    case kRandoDungeon_SkullWoods:        return ITEM_Map_SkullWoods;
    case kRandoDungeon_ThievesTown:       return ITEM_Map_ThievesTown;
    case kRandoDungeon_IcePalace:         return ITEM_Map_IcePalace;
    case kRandoDungeon_MiseryMire:        return ITEM_Map_MiseryMire;
    case kRandoDungeon_TurtleRock:        return ITEM_Map_TurtleRock;
    case kRandoDungeon_GanonsTower:       return ITEM_Map_GanonsTower;
    default: return 0xFFFFu;
  }
}

static inline uint16 Rando_CompassItemForGameDungeon(uint8 game_dungeon) {
  switch (Rando_RandoDungeonFromGameDungeon(game_dungeon)) {
    case kRandoDungeon_EasternPalace:     return ITEM_Compass_EasternPalace;
    case kRandoDungeon_DesertPalace:      return ITEM_Compass_DesertPalace;
    case kRandoDungeon_TowerOfHera:       return ITEM_Compass_TowerOfHera;
    case kRandoDungeon_PalaceOfDarkness:  return ITEM_Compass_PalaceOfDarkness;
    case kRandoDungeon_SwampPalace:       return ITEM_Compass_SwampPalace;
    case kRandoDungeon_SkullWoods:        return ITEM_Compass_SkullWoods;
    case kRandoDungeon_ThievesTown:       return ITEM_Compass_ThievesTown;
    case kRandoDungeon_IcePalace:         return ITEM_Compass_IcePalace;
    case kRandoDungeon_MiseryMire:        return ITEM_Compass_MiseryMire;
    case kRandoDungeon_TurtleRock:        return ITEM_Compass_TurtleRock;
    case kRandoDungeon_GanonsTower:       return ITEM_Compass_GanonsTower;
    default: return 0xFFFFu;
  }
}

static inline uint16 Rando_BossHeartLocationForGameDungeon(uint8 game_dungeon) {
  switch (Rando_RandoDungeonFromGameDungeon(game_dungeon)) {
    case kRandoDungeon_EasternPalace:     return LOC_Eastern_Palace_Boss;
    case kRandoDungeon_DesertPalace:      return LOC_Desert_Palace_Boss;
    case kRandoDungeon_TowerOfHera:       return LOC_Tower_of_Hera_Boss;
    case kRandoDungeon_PalaceOfDarkness:  return LOC_Palace_of_Darkness_Boss;
    case kRandoDungeon_SwampPalace:       return LOC_Swamp_Palace_Boss;
    case kRandoDungeon_SkullWoods:        return LOC_Skull_Woods_Boss;
    case kRandoDungeon_ThievesTown:       return LOC_Thieves_Town_Boss;
    case kRandoDungeon_IcePalace:         return LOC_Ice_Palace_Boss;
    case kRandoDungeon_MiseryMire:        return LOC_Misery_Mire_Boss;
    case kRandoDungeon_TurtleRock:        return LOC_Turtle_Rock_Boss;
    default: return 0xFFFFu;
  }
}

static inline uint16 Rando_BossPrizeLocationForGameDungeon(uint8 game_dungeon) {
  switch (Rando_RandoDungeonFromGameDungeon(game_dungeon)) {
    case kRandoDungeon_EasternPalace:     return LOC_Eastern_Palace_Prize;
    case kRandoDungeon_DesertPalace:      return LOC_Desert_Palace_Prize;
    case kRandoDungeon_TowerOfHera:       return LOC_Tower_of_Hera_Prize;
    case kRandoDungeon_PalaceOfDarkness:  return LOC_Palace_of_Darkness_Prize;
    case kRandoDungeon_SwampPalace:       return LOC_Swamp_Palace_Prize;
    case kRandoDungeon_SkullWoods:        return LOC_Skull_Woods_Prize;
    case kRandoDungeon_ThievesTown:       return LOC_Thieves_Town_Prize;
    case kRandoDungeon_IcePalace:         return LOC_Ice_Palace_Prize;
    case kRandoDungeon_MiseryMire:        return LOC_Misery_Mire_Prize;
    case kRandoDungeon_TurtleRock:        return LOC_Turtle_Rock_Prize;
    default: return 0xFFFFu;
  }
}

#endif  // __cplusplus

typedef struct RandoDungeonRuntimeRow {
  // Small-key counters and item bitfields are separate game RAM axes. Hyrule
  // Castle proper folds keys into HCE/Sewers slot 0, while the rando Map_HCE
  // item uses map bit 0. Do not collapse these into one "dungeon id" field.
  uint8 key_slot;
  uint8 bigkey_game_dungeon;
  uint8 map_game_dungeon;
  uint8 compass_game_dungeon;
  uint8 rando_dungeon;
  uint8 has_prize;
  const char *name;
  const char *short_name;
} RandoDungeonRuntimeRow;

#define kRandoDungeonRuntimeRowCount 13
#define RANDO_RUNTIME_ROW(key, big, map, compass, rando, prize, name, short_name) \
  { key, big, map, compass, rando, prize, name, short_name }
#define RANDO_RUNTIME_ROW_ALL(game, rando, prize, name, short_name) \
  RANDO_RUNTIME_ROW(game, game, game, game, rando, prize, name, short_name)
static const RandoDungeonRuntimeRow kRandoDungeonRuntimeRows[kRandoDungeonRuntimeRowCount] = {
  RANDO_RUNTIME_ROW(kGameDungeon_HyruleCastleEscape, kGameDungeon_None,
                    kGameDungeon_HyruleCastleEscape, kGameDungeon_None,
                    kRandoDungeon_HyruleCastleEscape, 0,
                    "Hyrule Castle / Sewers", "Hyrule Castle"),
  RANDO_RUNTIME_ROW(kGameDungeon_HyruleCastleTower, kGameDungeon_None,
                    kGameDungeon_None, kGameDungeon_None,
                    kRandoDungeon_HyruleCastleTower, 0,
                    "Castle Tower", "Castle Tower"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_EasternPalace, kRandoDungeon_EasternPalace,
                        1, "Eastern Palace", "Eastern"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_DesertPalace, kRandoDungeon_DesertPalace,
                        1, "Desert Palace", "Desert"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_TowerOfHera, kRandoDungeon_TowerOfHera,
                        1, "Tower of Hera", "Tower of Hera"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_PalaceOfDarkness, kRandoDungeon_PalaceOfDarkness,
                        1, "Palace of Darkness", "Pal. of Darkness"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_SwampPalace, kRandoDungeon_SwampPalace,
                        1, "Swamp Palace", "Swamp"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_SkullWoods, kRandoDungeon_SkullWoods,
                        1, "Skull Woods", "Skull Woods"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_ThievesTown, kRandoDungeon_ThievesTown,
                        1, "Thieves Town", "Thieves'"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_IcePalace, kRandoDungeon_IcePalace,
                        1, "Ice Palace", "Ice"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_MiseryMire, kRandoDungeon_MiseryMire,
                        1, "Misery Mire", "Misery Mire"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_TurtleRock, kRandoDungeon_TurtleRock,
                        1, "Turtle Rock", "Turtle Rock"),
  RANDO_RUNTIME_ROW_ALL(kGameDungeon_GanonsTower, kRandoDungeon_GanonsTower,
                        0, "Ganons Tower", "Ganon's Tower"),
};
#undef RANDO_RUNTIME_ROW_ALL
#undef RANDO_RUNTIME_ROW

typedef struct RandoDungeonDebugRow {
  uint8 game_dungeon;
  uint8 key_slot;
  const char *name;
} RandoDungeonDebugRow;

#define kRandoDungeonDebugRowCount 13
static const RandoDungeonDebugRow kRandoDungeonDebugRows[kRandoDungeonDebugRowCount] = {
  { kGameDungeon_HyruleCastleEscape, kGameDungeon_HyruleCastleEscape, "Hyrule Castle / Sewers" },
  { kGameDungeon_EasternPalace,      kGameDungeon_EasternPalace,      "Eastern Palace" },
  { kGameDungeon_DesertPalace,       kGameDungeon_DesertPalace,       "Desert Palace" },
  { kGameDungeon_TowerOfHera,        kGameDungeon_TowerOfHera,        "Tower of Hera" },
  { kGameDungeon_HyruleCastleTower,  kGameDungeon_HyruleCastleTower,  "Hyrule Castle Tower" },
  { kGameDungeon_PalaceOfDarkness,   kGameDungeon_PalaceOfDarkness,   "Palace of Darkness" },
  { kGameDungeon_SwampPalace,        kGameDungeon_SwampPalace,        "Swamp Palace" },
  { kGameDungeon_SkullWoods,         kGameDungeon_SkullWoods,         "Skull Woods" },
  { kGameDungeon_ThievesTown,        kGameDungeon_ThievesTown,        "Thieves' Town" },
  { kGameDungeon_IcePalace,          kGameDungeon_IcePalace,          "Ice Palace" },
  { kGameDungeon_MiseryMire,         kGameDungeon_MiseryMire,         "Misery Mire" },
  { kGameDungeon_TurtleRock,         kGameDungeon_TurtleRock,         "Turtle Rock" },
  { kGameDungeon_GanonsTower,        kGameDungeon_GanonsTower,        "Ganon's Tower" },
};

#endif  // ZELDA3_RANDO_DUNGEON_IDS_H_
