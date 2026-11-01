// rando_shuffles.c — prize + medallion shuffle (tasks.md §4.1a, §4.1b).
//
// Both shuffles use Fisher-Yates over arrays of small fixed size. The RNG is
// `Rng_NextRange` from rando_rng.c (xoshiro256**, deterministic per seed).
//
// Determinism contract: identical (settings_hash, seed_u64) produces identical
// assignment tables regardless of host platform.

#include "rando_shuffles.h"
#include "dungeon_ids.h"
#include "rando_rng.h"
#include "rando_logic.h"
#include "../types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Prize ids (kPrize_*) live in rando_shuffles.h — the pause-map marker module
// consumes the same vocabulary.

// ---------------------------------------------------------------------------
// Pendant pool — dungeons that hold a pendant in vanilla: EP, DP, TH.
// Crystal pool — dungeons that hold a crystal in vanilla: PoD, SP, SW, TT, IP, MM, TR.
// HCE, HCT, GT do not participate in prize shuffle (they have event prizes
// that are not items in the placement table).
// ---------------------------------------------------------------------------
static const uint8 kPendantDungeons[3] = {
  kRandoDungeon_EasternPalace,
  kRandoDungeon_DesertPalace,
  kRandoDungeon_TowerOfHera,
};

static const uint8 kCrystalDungeons[7] = {
  kRandoDungeon_PalaceOfDarkness,
  kRandoDungeon_SwampPalace,
  kRandoDungeon_SkullWoods,
  kRandoDungeon_ThievesTown,
  kRandoDungeon_IcePalace,
  kRandoDungeon_MiseryMire,
  kRandoDungeon_TurtleRock,
};

static const uint8 kPendantPrizes[3] = {
  kPrize_GreenPendant,
  kPrize_RedPendant,
  kPrize_BluePendant,
};

static const uint8 kCrystalPrizes[7] = {
  kPrize_Crystal1,
  kPrize_Crystal2,
  kPrize_Crystal3,
  kPrize_Crystal4,
  kPrize_Crystal5,
  kPrize_Crystal6,
  kPrize_Crystal7,
};

// Vanilla assignments per ALTTPR app/Region/Standard/*.php (default crystals
// 1..7 placed at PoD/SP/SW/TT/IP/MM/TR; pendants at EP/DP/TH).
static const uint8 kVanillaPrize_EP = kPrize_GreenPendant;
static const uint8 kVanillaPrize_DP = kPrize_RedPendant;
static const uint8 kVanillaPrize_TH = kPrize_BluePendant;
static const uint8 kVanillaPrize_PoD = kPrize_Crystal1;
static const uint8 kVanillaPrize_SP = kPrize_Crystal2;
static const uint8 kVanillaPrize_SW = kPrize_Crystal3;
static const uint8 kVanillaPrize_TT = kPrize_Crystal4;
static const uint8 kVanillaPrize_IP = kPrize_Crystal5;
static const uint8 kVanillaPrize_MM = kPrize_Crystal6;
static const uint8 kVanillaPrize_TR = kPrize_Crystal7;

// ---------------------------------------------------------------------------
// Fisher-Yates shuffle for small arrays.
//
// The standard Durstenfeld variant: for i from n-1 down to 1, pick j in [0, i]
// uniformly and swap items[i] and items[j]. Rng_NextRange's rejection sampling
// keeps the distribution unbiased.
// ---------------------------------------------------------------------------
static void shuffle_array(uint8 *items, uint8 n, RandoRng *rng) {
  for (int i = n - 1; i > 0; i--) {
    uint32 j = Rng_NextRange(rng, (uint32)(i + 1));
    uint8 tmp = items[i];
    items[i] = items[(uint8)j];
    items[(uint8)j] = tmp;
  }
}

// ---------------------------------------------------------------------------
// PrizeShuffle_Run
// ---------------------------------------------------------------------------
void PrizeShuffle_Run(const RandoSettings *settings,
                      RandoRng *rng,
                      uint8 out_assignment[kRandoDungeonCount]) {
  // Initialize with sentinel 0xFF (no prize at this dungeon).
  memset(out_assignment, 0xFF, kRandoDungeonCount);

  if (settings == NULL || settings->prize_shuffle == 0) {
    // Identity assignment — vanilla placements per ALTTPR.
    out_assignment[kRandoDungeon_EasternPalace] = kVanillaPrize_EP;
    out_assignment[kRandoDungeon_DesertPalace] = kVanillaPrize_DP;
    out_assignment[kRandoDungeon_TowerOfHera] = kVanillaPrize_TH;
    out_assignment[kRandoDungeon_PalaceOfDarkness] = kVanillaPrize_PoD;
    out_assignment[kRandoDungeon_SwampPalace] = kVanillaPrize_SP;
    out_assignment[kRandoDungeon_SkullWoods] = kVanillaPrize_SW;
    out_assignment[kRandoDungeon_ThievesTown] = kVanillaPrize_TT;
    out_assignment[kRandoDungeon_IcePalace] = kVanillaPrize_IP;
    out_assignment[kRandoDungeon_MiseryMire] = kVanillaPrize_MM;
    out_assignment[kRandoDungeon_TurtleRock] = kVanillaPrize_TR;
    return;
  }

  // Shuffle the pendant pool among the 3 pendant dungeons.
  uint8 pendants[3];
  memcpy(pendants, kPendantPrizes, sizeof(pendants));
  shuffle_array(pendants, 3, rng);
  for (uint8 i = 0; i < 3; i++) {
    out_assignment[kPendantDungeons[i]] = pendants[i];
  }

  // Shuffle the crystal pool among the 7 crystal dungeons.
  uint8 crystals[7];
  memcpy(crystals, kCrystalPrizes, sizeof(crystals));
  shuffle_array(crystals, 7, rng);
  for (uint8 i = 0; i < 7; i++) {
    out_assignment[kCrystalDungeons[i]] = crystals[i];
  }
}

// ---------------------------------------------------------------------------
// MedallionShuffle_Run
// ---------------------------------------------------------------------------
// Item registry IDs for the three medallions (per assets/rando/item_registry.yaml).
#define kItem_Bombos 25
#define kItem_Ether  26
#define kItem_Quake  27

void MedallionShuffle_Run(const RandoSettings *settings,
                          RandoRng *rng,
                          uint8 out_assignment[kRandoMedallionEntranceCount]) {
  if (settings == NULL || settings->medallion_shuffle == 0) {
    // Vanilla: MM=Ether, TR=Quake.
    out_assignment[0] = kItem_Ether;
    out_assignment[1] = kItem_Quake;
    return;
  }

  // Each entrance independently picks from the 3-medallion pool. Note: per
  // ALTTPR, medallion shuffle does NOT enforce uniqueness across entrances —
  // both MM and TR can require the same medallion. So this is independent
  // sampling, not a permutation.
  static const uint8 medallions[3] = { kItem_Bombos, kItem_Ether, kItem_Quake };
  for (uint8 e = 0; e < kRandoMedallionEntranceCount; e++) {
    uint32 idx = Rng_NextRange(rng, 3);
    out_assignment[e] = medallions[idx];
  }
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[Shuffles_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void Shuffles_SelfCheck(void) {
  RandoSettings settings;
  Settings_SetDefaults(&settings);

  // Prize shuffle disabled → identity assignment.
  {
    RandoSettings s = settings;
    s.prize_shuffle = 0;
    RandoRng rng;
    Rng_SeedFromU64(&rng, 0xDEADBEEFCAFEBABEull);
    uint8 assn[kRandoDungeonCount];
    PrizeShuffle_Run(&s, &rng, assn);
    if (assn[kRandoDungeon_EasternPalace] != kVanillaPrize_EP) selfcheck_die("EP vanilla expected GreenPendant");
    if (assn[kRandoDungeon_PalaceOfDarkness] != kVanillaPrize_PoD) selfcheck_die("PoD vanilla expected Crystal1");
    if (assn[kRandoDungeon_TurtleRock] != kVanillaPrize_TR) selfcheck_die("TR vanilla expected Crystal7");
    // Non-prize dungeons retain 0xFF sentinel.
    if (assn[kRandoDungeon_HyruleCastleEscape] != 0xFF) selfcheck_die("HCE should have no prize slot");
    if (assn[kRandoDungeon_GanonsTower] != 0xFF) selfcheck_die("GT should have no prize slot");
  }

  // Prize shuffle enabled with seed 0 → deterministic permutation.
  {
    RandoSettings s = settings;
    s.prize_shuffle = 1;
    RandoRng rng_a, rng_b;
    Rng_SeedFromU64(&rng_a, 0x12345678ull);
    Rng_SeedFromU64(&rng_b, 0x12345678ull);
    uint8 a[kRandoDungeonCount], b[kRandoDungeonCount];
    PrizeShuffle_Run(&s, &rng_a, a);
    PrizeShuffle_Run(&s, &rng_b, b);
    if (memcmp(a, b, sizeof(a)) != 0) {
      selfcheck_die("same seed should produce identical prize assignment");
    }
    // Every pendant dungeon has a pendant prize; every crystal dungeon has a crystal.
    for (uint8 i = 0; i < 3; i++) {
      uint8 p = a[kPendantDungeons[i]];
      if (p > kPrize_BluePendant) selfcheck_die("pendant dungeon got non-pendant prize");
    }
    for (uint8 i = 0; i < 7; i++) {
      uint8 p = a[kCrystalDungeons[i]];
      if (p < kPrize_Crystal1 || p > kPrize_Crystal7) selfcheck_die("crystal dungeon got non-crystal prize");
    }
    // Set of pendants used is {Green, Red, Blue} — no duplicates.
    uint8 pendant_count[3] = {0, 0, 0};
    for (uint8 i = 0; i < 3; i++) pendant_count[a[kPendantDungeons[i]]]++;
    if (pendant_count[0] != 1 || pendant_count[1] != 1 || pendant_count[2] != 1) {
      selfcheck_die("pendant assignment should be a permutation");
    }
    // Set of crystals used is {1..7} — no duplicates.
    uint8 crystal_count[7] = {0};
    for (uint8 i = 0; i < 7; i++) crystal_count[a[kCrystalDungeons[i]] - kPrize_Crystal1]++;
    for (uint8 i = 0; i < 7; i++) {
      if (crystal_count[i] != 1) selfcheck_die("crystal assignment should be a permutation");
    }
  }

  // Medallion shuffle disabled → vanilla MM=Ether, TR=Quake.
  {
    RandoSettings s = settings;
    s.medallion_shuffle = 0;
    RandoRng rng;
    Rng_SeedFromU64(&rng, 0);
    uint8 assn[kRandoMedallionEntranceCount];
    MedallionShuffle_Run(&s, &rng, assn);
    if (assn[0] != kItem_Ether) selfcheck_die("vanilla MM should be Ether");
    if (assn[1] != kItem_Quake) selfcheck_die("vanilla TR should be Quake");
  }

  // Medallion shuffle enabled — output is one of {Bombos, Ether, Quake} for each.
  {
    RandoSettings s = settings;
    s.medallion_shuffle = 1;
    RandoRng rng;
    Rng_SeedFromU64(&rng, 42);
    uint8 assn[kRandoMedallionEntranceCount];
    MedallionShuffle_Run(&s, &rng, assn);
    for (uint8 e = 0; e < kRandoMedallionEntranceCount; e++) {
      if (assn[e] != kItem_Bombos && assn[e] != kItem_Ether && assn[e] != kItem_Quake) {
        selfcheck_die("medallion assignment must be Bombos/Ether/Quake");
      }
    }
    // Determinism: same seed → same result.
    RandoRng rng2;
    Rng_SeedFromU64(&rng2, 42);
    uint8 assn2[kRandoMedallionEntranceCount];
    MedallionShuffle_Run(&s, &rng2, assn2);
    if (memcmp(assn, assn2, sizeof(assn)) != 0) {
      selfcheck_die("same seed should produce identical medallion assignment");
    }
  }

  fprintf(stderr, "[Shuffles_SelfCheck] OK\n");
}
