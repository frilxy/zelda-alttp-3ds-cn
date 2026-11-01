// shuffle_chains.c - deterministic dungeon-chain construction.

#include "shuffle_chains.h"

#include "dungeon_ids.h"
#include "rando_logic.h"
#include "rando_rng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const uint8 kChainsPoolDungeons[kChainsPoolCount] = {
  kRandoDungeon_EasternPalace,
  kRandoDungeon_DesertPalace,
  kRandoDungeon_TowerOfHera,
  kRandoDungeon_PalaceOfDarkness,
  kRandoDungeon_SwampPalace,
  kRandoDungeon_ThievesTown,
  kRandoDungeon_IcePalace,
  kRandoDungeon_MiseryMire,
  kRandoDungeon_TurtleRock,
};

enum {
  kChainsIdx_ToH = 2,
  kChainsIdx_TT = 5,
};

#define kChainsRngSalt 0xC4A17D006E0A11Cull

static const uint8 kChainsFreeDungeons[7] = {
  kRandoDungeon_EasternPalace,
  kRandoDungeon_DesertPalace,
  kRandoDungeon_PalaceOfDarkness,
  kRandoDungeon_SwampPalace,
  kRandoDungeon_IcePalace,
  kRandoDungeon_MiseryMire,
  kRandoDungeon_TurtleRock,
};

static const uint8 kChainsFreeBosses[7] = {
  kRandoDungeon_EasternPalace,
  kRandoDungeon_DesertPalace,
  kRandoDungeon_PalaceOfDarkness,
  kRandoDungeon_SwampPalace,
  kRandoDungeon_IcePalace,
  kRandoDungeon_MiseryMire,
  kRandoDungeon_TurtleRock,
};

typedef struct ChainRegionNames {
  uint8 dungeon;
  const char *entry_region;
  const char *boss_region;
} ChainRegionNames;

static const ChainRegionNames kChainRegionNames[kChainsPoolCount] = {
  { kRandoDungeon_EasternPalace,     "EasternPalace_Lobby",    "EasternPalace_BossRoom" },
  { kRandoDungeon_DesertPalace,      "DesertPalace_Lobby",     "DesertPalace_BossRoom" },
  { kRandoDungeon_TowerOfHera,       "TowerOfHera_Lobby",      "TowerOfHera_BossRoom" },
  { kRandoDungeon_PalaceOfDarkness,  "PalaceOfDarkness",       "PalaceOfDarkness_BossRoom" },
  { kRandoDungeon_SwampPalace,       "SwampPalace",            "SwampPalace_BossRoom" },
  { kRandoDungeon_ThievesTown,       "ThievesTown",            "ThievesTown_BossRoom" },
  { kRandoDungeon_IcePalace,         "IcePalace_Lobby",        "IcePalace_BossRoom" },
  { kRandoDungeon_MiseryMire,        "MiseryMire_Lobby",       "MiseryMire_BossRoom" },
  { kRandoDungeon_TurtleRock,        "TurtleRock_Lobby",       "TurtleRock_BossRoom" },
};

int Chains_PoolIndexForDungeon(uint8 rando_dungeon) {
  for (int i = 0; i < kChainsPoolCount; i++)
    if (kChainsPoolDungeons[i] == rando_dungeon)
      return i;
  return -1;
}

uint8 Chains_PoolDungeonAt(uint8 pool_index) {
  return pool_index < kChainsPoolCount ? kChainsPoolDungeons[pool_index]
                                       : (uint8)kRandoDungeon_None;
}

static const ChainRegionNames *chains_region_row(uint8 rando_dungeon) {
  int idx = Chains_PoolIndexForDungeon(rando_dungeon);
  if (idx < 0)
    return NULL;
  return &kChainRegionNames[idx];
}

static uint16 chains_element_region(uint8 elem) {
  const ChainRegionNames *row = chains_region_row(Chains_ElementDungeon(elem));
  if (row == NULL)
    return 0xFFFF;
  return Rando_FindRegionByName(Chains_ElementIsBoss(elem) ? row->boss_region
                                                           : row->entry_region);
}

static void chains_shuffle_u8(RandoRng *rng, uint8 *arr, uint32 n) {
  for (uint32 i = n; i > 1; i--) {
    uint32 j = Rng_NextRange(rng, i);
    uint8 t = arr[i - 1]; arr[i - 1] = arr[j]; arr[j] = t;
  }
}

static void chains_sort_u8(uint8 *arr, uint32 n) {
  for (uint32 i = 1; i < n; i++) {
    uint8 v = arr[i];
    uint32 j = i;
    while (j > 0 && arr[j - 1] > v) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = v;
  }
}

static void chains_sample_segment_lengths(RandoRng *rng,
                                          uint8 out_len[kChainsPoolCount]) {
  uint8 positions[15];
  for (uint8 i = 0; i < (uint8)sizeof(positions); i++)
    positions[i] = i;
  chains_shuffle_u8(rng, positions, (uint32)sizeof(positions));
  chains_sort_u8(positions, kChainsPoolCount - 1);  // first 8 positions = bars

  int prev = -1;
  for (uint8 seg = 0; seg < kChainsPoolCount; seg++) {
    int bar = seg < kChainsPoolCount - 1 ? positions[seg] : 15;
    out_len[seg] = (uint8)(bar - prev - 1);
    prev = bar;
  }
}

static uint64 chains_seed(uint64 seed_u64, uint32 attempt) {
  uint64 x = seed_u64 ^ kChainsRngSalt;
  x ^= (uint64)attempt * 0x9E3779B97F4A7C15ull;
  return x;
}

static bool chains_append_dungeon(DungeonChainsLayout *out, uint8 chain_idx,
                                  uint8 dungeon, uint8 *first_elem,
                                  uint8 *prev_dungeon) {
  int pidx = Chains_PoolIndexForDungeon(dungeon);
  if (pidx < 0)
    return false;
  uint8 elem = Chains_DungeonElement(dungeon);
  if (*first_elem == kChainsElement_None)
    *first_elem = elem;
  if (*prev_dungeon != kRandoDungeon_None) {
    int prev_idx = Chains_PoolIndexForDungeon(*prev_dungeon);
    if (prev_idx < 0)
      return false;
    out->chain_successor[prev_idx] = elem;
  }
  *prev_dungeon = dungeon;
  (void)chain_idx;
  return true;
}

uint32 Chains_LayoutDigest(const DungeonChainsLayout *layout) {
  if (layout == NULL)
    return 0;
  uint64 h = 0xcbf29ce484222325ull;
#define DIG8(b) (h = (h ^ (uint8)(b)) * 0x100000001b3ull)
  for (uint8 i = 0; i < kChainsPoolCount; i++)
    DIG8(layout->chain_door_first[i]);
  for (uint8 i = 0; i < kChainsPoolCount; i++)
    DIG8(layout->chain_successor[i]);
  for (uint8 i = 0; i < kChainsPoolCount; i++)
    DIG8(layout->terminal_boss[i]);
#undef DIG8
  return (uint32)(h & 0xFFFFFFu);
}

bool Chains_Compute(uint64 seed_u64, uint32 attempt, DungeonChainsLayout *out) {
  if (out == NULL)
    return false;
  memset(out, 0, sizeof(*out));
  memset(out->chain_door_first, kChainsElement_None, sizeof(out->chain_door_first));
  memset(out->chain_successor, kChainsElement_None, sizeof(out->chain_successor));
  memset(out->terminal_boss, kRandoDungeon_None, sizeof(out->terminal_boss));

  RandoRng rng;
  Rng_SeedFromU64(&rng, chains_seed(seed_u64, attempt));

  uint8 free_dungeons[sizeof(kChainsFreeDungeons)];
  memcpy(free_dungeons, kChainsFreeDungeons, sizeof(free_dungeons));
  chains_shuffle_u8(&rng, free_dungeons,
                    (uint32)(sizeof(free_dungeons) / sizeof(free_dungeons[0])));

  uint8 segment_len[kChainsPoolCount];
  chains_sample_segment_lengths(&rng, segment_len);

  uint8 free_bosses[sizeof(kChainsFreeBosses)];
  memcpy(free_bosses, kChainsFreeBosses, sizeof(free_bosses));
  chains_shuffle_u8(&rng, free_bosses,
                    (uint32)(sizeof(free_bosses) / sizeof(free_bosses[0])));
  uint8 boss_pos = 0;
  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    if (c == kChainsIdx_TT) {
      out->terminal_boss[c] = kRandoDungeon_ThievesTown;
    } else if (c == kChainsIdx_ToH) {
      out->terminal_boss[c] = kRandoDungeon_TowerOfHera;
    } else {
      out->terminal_boss[c] = free_bosses[boss_pos++];
    }
  }

  uint8 free_pos = 0;
  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    uint8 first_elem = kChainsElement_None;
    uint8 prev_dungeon = kRandoDungeon_None;
    for (uint8 i = 0; i < segment_len[c]; i++) {
      if (!chains_append_dungeon(out, c, free_dungeons[free_pos++],
                                 &first_elem, &prev_dungeon))
        return false;
    }
    if (c == kChainsIdx_TT) {
      if (!chains_append_dungeon(out, c, kRandoDungeon_ThievesTown,
                                 &first_elem, &prev_dungeon))
        return false;
    } else if (c == kChainsIdx_ToH) {
      if (!chains_append_dungeon(out, c, kRandoDungeon_TowerOfHera,
                                 &first_elem, &prev_dungeon))
        return false;
    }

    uint8 terminal = Chains_BossElement(out->terminal_boss[c]);
    if (prev_dungeon == kRandoDungeon_None) {
      first_elem = terminal;
    } else {
      int prev_idx = Chains_PoolIndexForDungeon(prev_dungeon);
      if (prev_idx < 0)
        return false;
      out->chain_successor[prev_idx] = terminal;
    }
    out->chain_door_first[c] = first_elem;
  }

  if (free_pos != (uint8)(sizeof(kChainsFreeDungeons) / sizeof(kChainsFreeDungeons[0])) ||
      boss_pos != (uint8)(sizeof(kChainsFreeBosses) / sizeof(kChainsFreeBosses[0])))
    return false;
  out->digest24 = Chains_LayoutDigest(out);
  return true;
}

void Chains_ApplyEdgeOverrides(const DungeonChainsLayout *layout) {
  Rando_BeginEntranceEdgeOverrides();
  if (layout == NULL)
    return;

  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    const ChainRegionNames *row = &kChainRegionNames[c];
    uint16 from_key = Rando_FindRegionByName(row->entry_region);
    uint16 to_region = chains_element_region(layout->chain_door_first[c]);
    if (from_key != 0xFFFF && to_region != 0xFFFF)
      Rando_SetEntranceEdgeOverride(from_key, to_region);
  }

  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    const ChainRegionNames *row = &kChainRegionNames[i];
    uint16 from_key = Rando_FindRegionByName(row->boss_region);
    uint16 to_region = chains_element_region(layout->chain_successor[i]);
    if (from_key != 0xFFFF && to_region != 0xFFFF)
      Rando_SetEntranceEdgeOverride(from_key, to_region);
  }
}

static void chains_selfcheck_die(const char *msg) {
  fprintf(stderr, "[Chains_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

static bool chains_valid_element(uint8 elem) {
  if (elem == kChainsElement_None)
    return false;
  return Chains_PoolIndexForDungeon(Chains_ElementDungeon(elem)) >= 0;
}

static bool chains_chain_contains(const DungeonChainsLayout *layout,
                                  uint8 chain_idx, uint8 dungeon) {
  uint8 elem = layout->chain_door_first[chain_idx];
  for (uint8 step = 0; step <= kChainsPoolCount; step++) {
    if (!chains_valid_element(elem))
      return false;
    if (Chains_ElementIsBoss(elem))
      return false;
    uint8 d = Chains_ElementDungeon(elem);
    if (d == dungeon)
      return true;
    int pidx = Chains_PoolIndexForDungeon(d);
    if (pidx < 0)
      return false;
    elem = layout->chain_successor[pidx];
  }
  return false;
}

static void chains_assert_layout(const DungeonChainsLayout *layout) {
  uint8 seen_dungeons[kChainsPoolCount] = {0};
  uint8 seen_bosses[kChainsPoolCount] = {0};

  if (layout->digest24 != Chains_LayoutDigest(layout))
    chains_selfcheck_die("stored digest does not match resolved mapping");
  if (layout->terminal_boss[kChainsIdx_TT] != kRandoDungeon_ThievesTown ||
      layout->terminal_boss[kChainsIdx_ToH] != kRandoDungeon_TowerOfHera)
    chains_selfcheck_die("pinned terminal bosses moved");

  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    int bidx = Chains_PoolIndexForDungeon(layout->terminal_boss[c]);
    if (bidx < 0)
      chains_selfcheck_die("terminal boss outside chain pool");
    seen_bosses[bidx]++;

    uint8 elem = layout->chain_door_first[c];
    for (uint8 step = 0; step <= kChainsPoolCount; step++) {
      if (!chains_valid_element(elem))
        chains_selfcheck_die("invalid chain element");
      if (Chains_ElementIsBoss(elem)) {
        if (Chains_ElementDungeon(elem) != layout->terminal_boss[c])
          chains_selfcheck_die("chain terminal does not match terminal_boss table");
        break;
      }
      uint8 d = Chains_ElementDungeon(elem);
      int didx = Chains_PoolIndexForDungeon(d);
      if (didx < 0)
        chains_selfcheck_die("dungeon element outside chain pool");
      seen_dungeons[didx]++;
      elem = layout->chain_successor[didx];
      if (step == kChainsPoolCount)
        chains_selfcheck_die("chain traversal did not terminate");
    }
  }

  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    if (seen_dungeons[i] != 1)
      chains_selfcheck_die("pool dungeon does not appear exactly once");
    if (seen_bosses[i] != 1)
      chains_selfcheck_die("pool boss does not appear exactly once");
  }
  if (!chains_chain_contains(layout, kChainsIdx_TT, kRandoDungeon_ThievesTown) ||
      layout->chain_successor[Chains_PoolIndexForDungeon(kRandoDungeon_ThievesTown)] !=
          Chains_BossElement(kRandoDungeon_ThievesTown))
    chains_selfcheck_die("TT->Blind pinned adjacency not respected");
  if (!chains_chain_contains(layout, kChainsIdx_ToH, kRandoDungeon_TowerOfHera) ||
      layout->chain_successor[Chains_PoolIndexForDungeon(kRandoDungeon_TowerOfHera)] !=
          Chains_BossElement(kRandoDungeon_TowerOfHera))
    chains_selfcheck_die("ToH->Moldorm pinned adjacency not respected");
}

static void chains_assert_overrides(const DungeonChainsLayout *layout) {
  Chains_ApplyEdgeOverrides(layout);
  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    const ChainRegionNames *row = &kChainRegionNames[c];
    uint16 from_key = Rando_FindRegionByName(row->entry_region);
    uint16 to_region = chains_element_region(layout->chain_door_first[c]);
    if (from_key == 0xFFFF || to_region == 0xFFFF ||
        Rando_GetEntranceEdgeOverride(from_key) != to_region)
      chains_selfcheck_die("chain-start edge override missing");
  }
  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    const ChainRegionNames *row = &kChainRegionNames[i];
    uint16 from_key = Rando_FindRegionByName(row->boss_region);
    uint16 to_region = chains_element_region(layout->chain_successor[i]);
    if (from_key == 0xFFFF || to_region == 0xFFFF ||
        Rando_GetEntranceEdgeOverride(from_key) != to_region)
      chains_selfcheck_die("boss-seam edge override missing");
  }
  Rando_ClearEntranceEdgeOverrides();
}

void Chains_SelfCheck(void) {
  DungeonChainsLayout a, b;
  if (!Chains_Compute(0xDEADBEEFCAFEBABEull, 3, &a) ||
      !Chains_Compute(0xDEADBEEFCAFEBABEull, 3, &b))
    chains_selfcheck_die("Chains_Compute failed for a valid pointer");
  if (memcmp(&a, &b, sizeof(a)) != 0)
    chains_selfcheck_die("same seed/attempt produced different layouts");
  chains_assert_layout(&a);
  chains_assert_overrides(&a);

  // Cross-platform digest sentinel for the construction algorithm.
  if (a.digest24 != 0xB31390u)
    chains_selfcheck_die("fixed-vector digest changed");

  for (uint32 attempt = 0; attempt < 8; attempt++) {
    DungeonChainsLayout l;
    if (!Chains_Compute(0x0123456789ABCDEFull, attempt, &l))
      chains_selfcheck_die("Chains_Compute failed across attempts");
    chains_assert_layout(&l);
  }

  DungeonChainsLayout off;
  if (Chains_Compute(0, 0, NULL))
    chains_selfcheck_die("NULL output pointer should be rejected");
  (void)off;
  fprintf(stderr, "[Chains_SelfCheck] OK\n");
}
