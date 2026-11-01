// shuffle_boss.c — Phase B Slice 7 (add-rando-shuffles-and-minigames).
//
// The deterministic permutation algorithm + the runtime RENDER redirect
// (BossShuffle_RenderHomeRoom — Enemizer pointer-redirect model) are shipped and
// the assignment IS consumed by the game: the render redirect at room load plus
// the logic VM's OP_CAN_KILL_BOSS gate. The earlier per-entry sprite-type swap
// (BossShuffle_RemapSpriteType) is superseded, kept only for the selftest.
//
// ALTTPR upstream: app/Boss.php. Pinned (not shuffled): Agahnim 1 (HCT),
// Agahnim 2 (GT top), Blind (TT), Kholdstare (IP), Trinexx (TR) — the last three
// depend on home-room environment the redirect can't supply (see the
// kBossShuffleableDungeons comment). The remaining 7 bosses shuffle across 7
// dungeon-boss rooms (EP, DP, ToH, PoD, SP, SW, MM). (Matches
// kBossShuffleableDungeons[7] / kBossShufflePool[7] below.)
//
// Determinism: Fisher-Yates shuffle keyed by (seed_u64 XOR salt) via
// the project's xoshiro256** RNG (rando_rng.h).

#include "shuffle_boss.h"
#include "dungeon_ids.h"
#include "rando_settings.h"
#include "rando_rng.h"
#include "rando_logic.h"  // kRandoDungeonVanillaBoss / kRandoBossKillPredCount cross-check
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Boss-pool indices — match `Boss::all()` order in `app/Boss.php:68-129`.
// Agahnim (3) and Agahnim2 (11) are NOT in the shuffle pool; they're
// pinned per slot.
enum {
  kBoss_ArmosKnights  = 0,
  kBoss_Lanmolas      = 1,
  kBoss_Moldorm       = 2,
  kBoss_Agahnim       = 3,   // pinned at HCT
  kBoss_HelmasaurKing = 4,
  kBoss_Arrghus       = 5,
  kBoss_Mothula       = 6,
  kBoss_Blind         = 7,
  kBoss_Kholdstare    = 8,
  kBoss_Vitreous      = 9,
  kBoss_Trinexx       = 10,
  kBoss_Agahnim2      = 11,  // pinned at GT-top
};

// Vanilla per-dungeon boss assignment (kRandoDungeon_* -> boss-pool index).
// This is the rando/logic domain from dungeon_ids.h, not the engine's
// cur_palace_index_x2 >> 1 game domain.
static const uint8 kBossVanilla[16] = {
  [kRandoDungeon_HyruleCastleEscape] = 0xFF,
  [kRandoDungeon_EasternPalace]      = kBoss_ArmosKnights,
  [kRandoDungeon_DesertPalace]       = kBoss_Lanmolas,
  [kRandoDungeon_TowerOfHera]        = kBoss_Moldorm,
  [kRandoDungeon_HyruleCastleTower]  = kBoss_Agahnim,
  [kRandoDungeon_PalaceOfDarkness]   = kBoss_HelmasaurKing,
  [kRandoDungeon_SwampPalace]        = kBoss_Arrghus,
  [kRandoDungeon_SkullWoods]         = kBoss_Mothula,
  [kRandoDungeon_ThievesTown]        = kBoss_Blind,
  [kRandoDungeon_IcePalace]          = kBoss_Kholdstare,
  [kRandoDungeon_MiseryMire]         = kBoss_Vitreous,
  [kRandoDungeon_TurtleRock]         = kBoss_Trinexx,
  [kRandoDungeon_GanonsTower]        = kBoss_Agahnim2,
  [kRandoDungeon_Count]              = 0xFF,
  [14]                               = 0xFF,
  [15]                               = 0xFF,
};

// RandoDungeonIds whose boss is shuffleable. Excludes HCE, HCT (Agahnim 1),
// and GT (Agahnim 2) — the Agahnims share sprite_type 0x7A (the runtime
// discriminates by `is_in_dark_world` inside `Sprite_7A_Agahnim`), so pinning
// both is the only safe option without per-call-site world disambiguation.
//
// ALSO excludes three bosses that depend on their HOME room's ENVIRONMENT, which
// the sprite+gfx+palette redirect can't supply (each playtest- or Enemizer-
// confirmed); all are PINNED to their vanilla dungeon:
//   - TT Blind        — TT has no Blind sprite; it spawns via a maiden-follower
//     sequence and only materializes when `dung_savegame_state_bits & 0x2000`
//     (set by that TT-only sequence) is true. Shuffled elsewhere it never spawns
//     (playtest-confirmed strand).
//   - IP Kholdstare and TR Trinexx — their fights need the room's "effect"
//     ($00AD / header byte 4) + BG2-object setup (the ice block / lava floor).
//     The redirect carries the boss + shell SPRITES but not the room environment,
//     so the encase/melt (Kholdstare) and floor (Trinexx) sequences don't init —
//     Kholdstare spawned un-encased in Desert Palace (playtest-confirmed), and
//     Trinexx uses the identical mechanism (Enemizer special-cases both:
//     AddShellAndMoveObjectData + header bytes, BossRandomizer.cs:226-260).
// Making these shuffleable is enemizer-class room-object/header surgery and can't
// be validated headless — deferred. Net: 7 shuffleable bosses across 7 dungeon-
// boss rooms (the set that works with a pure sprite/gfx/palette redirect).
static const uint8 kBossShuffleableDungeons[7] = {
  kRandoDungeon_EasternPalace,
  kRandoDungeon_DesertPalace,
  kRandoDungeon_TowerOfHera,
  kRandoDungeon_PalaceOfDarkness,
  kRandoDungeon_SwampPalace,
  kRandoDungeon_SkullWoods,
  kRandoDungeon_MiseryMire,
};

// Pool of shuffleable bosses (7 entries: excludes both Agahnims, Blind,
// Kholdstare, Trinexx — see kBossShuffleableDungeons). The vanilla mapping
// (kBossVanilla) places these at the dungeons listed above.
static const uint8 kBossShufflePool[7] = {
  kBoss_ArmosKnights, kBoss_Lanmolas, kBoss_Moldorm,
  kBoss_HelmasaurKing, kBoss_Arrghus, kBoss_Mothula,
  kBoss_Vitreous,
};
#define kBossShufflePoolCount ((uint32)(sizeof(kBossShufflePool) / sizeof(kBossShufflePool[0])))

static uint8 g_boss_assignment[16];
static bool g_boss_assignment_active = false;

// Fisher-Yates shuffle of `arr[0..n-1]` using `rng` for randomness.
static void fy_shuffle_u8(RandoRng *rng, uint8 *arr, uint32 n) {
  for (uint32 i = n; i > 1; i--) {
    uint32 j = Rng_NextRange(rng, i);
    uint8 t = arr[i - 1]; arr[i - 1] = arr[j]; arr[j] = t;
  }
}

void BossShuffle_ComputeAssignment(const RandoSettings *settings,
                                   uint64 seed_u64,
                                   uint8 out_assignment[16]) {
  // Identity assignment first.
  memcpy(out_assignment, kBossVanilla, sizeof(kBossVanilla));

  if (settings == NULL || settings->boss_shuffle == 0) {
    // Off — identity (each dungeon keeps its vanilla boss).
    return;
  }

  // Deterministic permutation. Salt distinguishes the boss-shuffle RNG
  // stream from other slice's RNG so changing one shuffle setting
  // doesn't perturb the others.
  RandoRng rng;
  // Boss-shuffle salt: a recognizable "BOSSAILF" leetspeak constant
  // (0xB055A11F repeated) XORed into the seed so this stream is independent of
  // the drop-shuffle (0xD0DDA1FF...) and prize/medallion streams.
  Rng_SeedFromU64(&rng, seed_u64 ^ 0xB055A11FB055A11Full);

  uint8 pool[kBossShufflePoolCount];
  memcpy(pool, kBossShufflePool, sizeof(pool));
  fy_shuffle_u8(&rng, pool, kBossShufflePoolCount);

  // Assign shuffled pool to the shuffleable dungeon slots in order.
  for (uint32 i = 0; i < kBossShufflePoolCount; i++) {
    uint8 dungeon = kBossShuffleableDungeons[i];
    out_assignment[dungeon] = pool[i];
  }
  // Pin Agahnim 1 (HCT) and Agahnim 2 (GT-top). kBossVanilla already
  // assigns these, but write explicitly to defend against future pool
  // edits that could touch slots 4/12.
  out_assignment[kRandoDungeon_HyruleCastleTower] = kBoss_Agahnim;
  out_assignment[kRandoDungeon_GanonsTower] = kBoss_Agahnim2;
}

bool BossShuffle_Generate(const RandoSettings *settings,
                          uint64 seed_u64,
                          uint8 out_assignment[16]) {
  // Compute the pure assignment, then INSTALL it into the runtime globals.
  // Marking active even when boss_shuffle is off is intentional: the runtime
  // remap (BossShuffle_RemapSpriteType) is then an explicit identity for an
  // active rando slot, and only goes back to a hard passthrough on
  // BossShuffle_Deactivate (slot teardown / vanilla play).
  BossShuffle_ComputeAssignment(settings, seed_u64, out_assignment);
  memcpy(g_boss_assignment, out_assignment, sizeof(g_boss_assignment));
  g_boss_assignment_active = true;
  return true;
}

void BossShuffle_Deactivate(void) {
  g_boss_assignment_active = false;
  memset(g_boss_assignment, 0xFF, sizeof(g_boss_assignment));
}

uint8 BossShuffle_GetForDungeon(uint8 rando_dungeon) {
  if (!g_boss_assignment_active) return 0xFF;
  if (rando_dungeon >= 16) return 0xFF;
  return g_boss_assignment[rando_dungeon];
}

// Vanilla boss sprite IDs (cross-referenced with the `Sprite_*` symbol
// table in `src/sprite_main.h` and `other/names.txt`). Agahnim 1 and Agahnim 2
// share sprite_type 0x7A; the runtime discriminates by `is_in_dark_world`
// inside `Sprite_7A_Agahnim`. Both are PINNED at their vanilla rando dungeons
// (HCT/GT) so the remap never has to disambiguate.
//   0x09 Moldorm        (ToH)
//   0x53 ArmosKnights   (EP)
//   0x54 Lanmolas       (DP)
//   0x7A Agahnim 1/2    (HCT/GT — pinned, no remap entry)
//   0x88 Mothula        (SW)
//   0x8C Arrghus        (SP)
//   0x92 HelmasaurKing  (PoD)
//   0xA2 Kholdstare     (IP)  -- 0xA3 is KholdstareShell, NOT the boss
//   0xBD Vitreous       (MM)
//   0xCB Trinexx        (TR)
//   0xCE Blind          (TT)
//
// 0xB6 is `Sprite_B6_Kiki`, NOT Agahnim 2 — DO NOT
// add it to the table; remapping Kiki at GT-door would soft-lock GT entry.
//
// Translation: each boss sprite type in the table uniquely identifies
// which dungeon's boss is being spawned, because vanilla LttP places one
// boss per dungeon. We use the incoming sprite_type as the dungeon-key,
// look up the shuffle assignment, and translate the assigned pool index
// back to a sprite type via kBossPoolIdxToSprite.
//
// Limitation (M1 follow-up): multi-segment bosses (Trinexx CB/CC/CD,
// Lanmolas, Armos Knights x6, Mothula spikes) only remap the primary
// sprite; secondary segments pass through unchanged. The shuffled room
// will have orphan segments of the original boss alongside the new
// boss until a group-aware remap lands.
typedef struct BossSpriteMap {
  uint8 sprite_type;
  uint8 rando_dungeon;
  uint8 pool_idx;
} BossSpriteMap;

static const BossSpriteMap kBossSpriteMap[] = {
  { 0x09, kRandoDungeon_TowerOfHera,       kBoss_Moldorm },
  { 0x53, kRandoDungeon_EasternPalace,     kBoss_ArmosKnights },
  { 0x54, kRandoDungeon_DesertPalace,      kBoss_Lanmolas },
  { 0x88, kRandoDungeon_SkullWoods,        kBoss_Mothula },
  { 0x8C, kRandoDungeon_SwampPalace,       kBoss_Arrghus },
  { 0x92, kRandoDungeon_PalaceOfDarkness,  kBoss_HelmasaurKing },
  { 0xA2, kRandoDungeon_IcePalace,         kBoss_Kholdstare },
  { 0xBD, kRandoDungeon_MiseryMire,        kBoss_Vitreous },
  { 0xCB, kRandoDungeon_TurtleRock,        kBoss_Trinexx },
  { 0xCE, kRandoDungeon_ThievesTown,       kBoss_Blind },
};
#define kBossSpriteMapCount (sizeof(kBossSpriteMap) / sizeof(kBossSpriteMap[0]))

// Reverse: pool index → sprite type. 12 entries. Both Agahnim slots
// map to 0xFF (poisoned sentinel) rather than
// the shared 0x7A. The remap function falls back to vanilla on 0xFF,
// preserving correctness if a future edit puts Agahnim2 back in the
// pool: instead of silently spawning the wrong Agahnim variant in a
// random dungeon (the `is_in_dark_world` rendering discriminator inside
// `Sprite_7A_Agahnim` can't tell which Agahnim was intended), the
// remap returns the input vanilla type unchanged. The safer default
// future-proofs against the exact bug caught and removed.
static const uint8 kBossPoolIdxToSprite[12] = {
  [kBoss_ArmosKnights]  = 0x53,
  [kBoss_Lanmolas]      = 0x54,
  [kBoss_Moldorm]       = 0x09,
  [kBoss_Agahnim]       = 0xFF,  // pinned; never picked → poisoned
  [kBoss_HelmasaurKing] = 0x92,
  [kBoss_Arrghus]       = 0x8C,
  [kBoss_Mothula]       = 0x88,
  [kBoss_Blind]         = 0xCE,
  [kBoss_Kholdstare]    = 0xA2,
  [kBoss_Vitreous]      = 0xBD,
  [kBoss_Trinexx]       = 0xCB,
  [kBoss_Agahnim2]      = 0xFF,  // pinned; never picked → poisoned
};

uint8 BossShuffle_RemapSpriteType(uint8 vanilla_sprite_type) {
  if (!g_boss_assignment_active) return vanilla_sprite_type;

  for (uint32 i = 0; i < kBossSpriteMapCount; i++) {
    if (kBossSpriteMap[i].sprite_type != vanilla_sprite_type) continue;
    uint8 rando_dungeon = kBossSpriteMap[i].rando_dungeon;
    uint8 pool_idx = g_boss_assignment[rando_dungeon];
    if (pool_idx == 0xFF || pool_idx >= 12) return vanilla_sprite_type;
    uint8 mapped = kBossPoolIdxToSprite[pool_idx];
    // 0 = uninitialized entry; 0xFF = poisoned (pinned Agahnim variants
    // that must not be substituted because the sprite id cannot
    // disambiguate A1 vs A2 without world-state context). Fall back to
    // vanilla in both cases.
    if (mapped == 0 || mapped == 0xFF) return vanilla_sprite_type;
    return mapped;
  }
  return vanilla_sprite_type;
}

// Room-data secondary segments. Each entry pairs a secondary sprite_type
// with the dungeon + pool-index of its primary boss. Suppression fires
// when boss-shuffle is active AND the dungeon's assignment differs from
// the vanilla primary.
typedef struct BossSecondarySegment {
  uint8 sprite_type;
  uint8 parent_rando_dungeon;
  uint8 parent_pool_idx;
} BossSecondarySegment;

static const BossSecondarySegment kBossSecondaries[] = {
  { 0xCC, kRandoDungeon_TurtleRock, kBoss_Trinexx },    // Trinexx left arm  (TR)
  { 0xCD, kRandoDungeon_TurtleRock, kBoss_Trinexx },    // Trinexx right arm (TR)
  { 0xA3, kRandoDungeon_IcePalace,  kBoss_Kholdstare }, // KholdstareShell   (IP)
};
#define kBossSecondariesCount (sizeof(kBossSecondaries) / sizeof(kBossSecondaries[0]))

bool BossShuffle_ShouldSuppressSecondary(uint8 vanilla_sprite_type) {
  if (!g_boss_assignment_active) return false;
  for (uint32 i = 0; i < kBossSecondariesCount; i++) {
    if (kBossSecondaries[i].sprite_type != vanilla_sprite_type) continue;
    uint8 rando_dungeon = kBossSecondaries[i].parent_rando_dungeon;
    uint8 assigned = g_boss_assignment[rando_dungeon];
    return assigned != kBossSecondaries[i].parent_pool_idx;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Runtime RENDER — the Enemizer pointer-redirect model (add-rando-boss-shuffle
// render). The old per-entry BossShuffle_RemapSpriteType/_ShouldSuppressSecondary
// approach (a sprite-TYPE swap) is SUPERSEDED — it rendered garbage (the room
// kept the vanilla boss's GFX) and mis-spawned formation bosses (slot-hardcoded
// Armos/Lanmolas). Instead, when a dungeon's boss room loads under boss shuffle,
// the engine redirects BOTH the room's sprite-data list AND its sprite-graphics
// index to the ASSIGNED boss's vanilla HOME boss room — exactly what Enemizer
// does by repointing DungeonRoomSpritePointer→BossPointer + setting header byte 3
// →BossGraphics (../Enemizer/EnemizerLibrary/BossRandomizer/BossRandomizer.cs:
// 219-223). The home room already holds the correct boss formation (right count,
// right coords, its trigger overlords) drawn with the correct gfx, so the
// substituted boss renders + plays like its vanilla self, in the new room.
//
// Boss-room dungeon_room_index per shuffleable rando dungeon (standard ALTTP
// room ids, ROM-version-stable; cross-checked against Enemizer
// RoomIdConstants). Index = kRandoDungeon_*. 0xFFFF = no single shuffleable
// boss room (HCE / the pinned Agahnim slots / GT).
static const uint16 kBossRoom[16] = {
  [kRandoDungeon_HyruleCastleEscape] = 0xFFFF,
  [kRandoDungeon_EasternPalace]      = 200,
  [kRandoDungeon_DesertPalace]       = 51,
  [kRandoDungeon_TowerOfHera]        = 7,
  [kRandoDungeon_HyruleCastleTower]  = 0xFFFF,
  [kRandoDungeon_PalaceOfDarkness]   = 90,
  [kRandoDungeon_SwampPalace]        = 6,
  [kRandoDungeon_SkullWoods]         = 41,
  [kRandoDungeon_ThievesTown]        = 172,
  [kRandoDungeon_IcePalace]          = 222,
  [kRandoDungeon_MiseryMire]         = 144,
  [kRandoDungeon_TurtleRock]         = 164,
  [kRandoDungeon_GanonsTower]        = 0xFFFF,
  [kRandoDungeon_Count]              = 0xFFFF,
  [14]                               = 0xFFFF,
  [15]                               = 0xFFFF,
};

// boss-pool index → that boss's vanilla HOME boss room (the redirect target).
// = kBossRoom[dungeon where the boss is vanilla]. Pinned Agahnims → 0xFFFF (never
// assigned to a shuffle slot, so never a redirect target).
static const uint16 kBossHomeRoom[12] = {
  [kBoss_ArmosKnights]  = 200,
  [kBoss_Lanmolas]      = 51,
  [kBoss_Moldorm]       = 7,
  [kBoss_Agahnim]       = 0xFFFF,
  [kBoss_HelmasaurKing] = 90,
  [kBoss_Arrghus]       = 6,
  [kBoss_Mothula]       = 41,
  [kBoss_Blind]         = 172,
  [kBoss_Kholdstare]    = 222,
  [kBoss_Vitreous]      = 144,
  [kBoss_Trinexx]       = 164,
  [kBoss_Agahnim2]      = 0xFFFF,
};

uint16 BossShuffle_RenderHomeRoom(uint16 room) {
  if (!g_boss_assignment_active) return 0xFFFF;
  for (uint8 d = 0; d < 16; d++) {
    if (kBossRoom[d] != room) continue;
    uint8 assigned = g_boss_assignment[d];
    // Identity (vanilla boss stays) or invalid → no redirect → byte-identical to
    // vanilla. Note kBossVanilla[d] is the dungeon's own vanilla boss.
    if (assigned >= 12 || assigned == kBossVanilla[d]) return 0xFFFF;
    return kBossHomeRoom[assigned];  // 0xFFFF for a pinned boss (can't happen)
  }
  return 0xFFFF;  // not a shuffleable boss room
}

bool BossShuffle_GetRenderRedirect(uint16 room, uint16 *home_room,
                                   uint8 *dest_vanilla_sprite,
                                   uint8 *home_boss_sprite) {
  if (!g_boss_assignment_active) return false;
  for (uint8 d = 0; d < 16; d++) {
    if (kBossRoom[d] != room) continue;
    uint8 assigned = g_boss_assignment[d];
    uint8 vanilla = kBossVanilla[d];
    if (assigned >= 12 || assigned == vanilla) return false;  // identity / invalid
    uint16 hr = kBossHomeRoom[assigned];
    if (hr == 0xFFFF) return false;  // pinned boss — can't happen
    *home_room = hr;
    // The dungeon's vanilla boss is the anchor in THIS room; the assigned boss is
    // the anchor in its home room. The caller shifts the redirected formation by
    // (dest_anchor - home_anchor) so the substituted boss lands where the vanilla
    // boss spawned (reachable), not at the home room's coords. Agahnim entries are
    // poisoned to 0xFF here, but neither can be a vanilla shuffleable-dungeon boss
    // nor an assigned pool boss, so they never reach this point.
    *dest_vanilla_sprite = kBossPoolIdxToSprite[vanilla];
    *home_boss_sprite = kBossPoolIdxToSprite[assigned];
    return true;
  }
  return false;  // not a shuffleable boss room
}

// Human-readable names for the spoiler. Indices match the kBoss_* enum and the
// dungeon-id table at the top of this file.
const char *BossShuffle_BossName(uint8 pool_index) {
  static const char *kNames[12] = {
    "Armos Knights", "Lanmolas", "Moldorm", "Agahnim", "Helmasaur King",
    "Arrghus", "Mothula", "Blind", "Kholdstare", "Vitreous", "Trinexx",
    "Agahnim 2",
  };
  return (pool_index < 12) ? kNames[pool_index] : "?";
}

const char *BossShuffle_DungeonName(uint8 rando_dungeon) {
  static const char *kNames[13] = {
    "Hyrule Castle Escape", "Eastern Palace", "Desert Palace", "Tower of Hera",
    "Hyrule Castle Tower", "Palace of Darkness", "Swamp Palace", "Skull Woods",
    "Thieves' Town", "Ice Palace", "Misery Mire", "Turtle Rock",
    "Ganon's Tower",
  };
  return (rando_dungeon < kRandoDungeon_Count) ? kNames[rando_dungeon] : "?";
}

// ---------------------------------------------------------------------------
// Self-check (--rando-selftest). The regression corpus hashes only the
// placement + sphere digests, and boss shuffle is orthogonal to item placement
// (it never perturbs either), so boss determinism + the structural invariants
// can ONLY be pinned here. Exits(2) on any failure.
// ---------------------------------------------------------------------------
static void boss_selfcheck_die(const char *msg) {
  fprintf(stderr, "[BossShuffle_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void BossShuffle_SelfCheck(void) {
  RandoSettings s;
  Settings_SetDefaults(&s);

  // 1) Off → identity (each dungeon keeps its vanilla boss).
  {
    RandoSettings off = s;
    off.boss_shuffle = 0;
    uint8 a[16];
    BossShuffle_ComputeAssignment(&off, 0x0123456789ABCDEFull, a);
    if (memcmp(a, kBossVanilla, sizeof(kBossVanilla)) != 0)
      boss_selfcheck_die("boss_shuffle off must produce the vanilla assignment");
  }

  // 2) On → determinism + pinned Agahnim + permutation-of-pool invariant.
  {
    RandoSettings on = s;
    on.boss_shuffle = 1;
    uint8 a[16], b[16];
    BossShuffle_ComputeAssignment(&on, 0xDEADBEEFCAFEBABEull, a);
    BossShuffle_ComputeAssignment(&on, 0xDEADBEEFCAFEBABEull, b);
    if (memcmp(a, b, sizeof(a)) != 0)
      boss_selfcheck_die("same seed must produce identical boss assignment");
    if (a[kRandoDungeon_HyruleCastleTower] != kBoss_Agahnim)
      boss_selfcheck_die("Agahnim 1 must stay pinned at HCT");
    if (a[kRandoDungeon_GanonsTower] != kBoss_Agahnim2)
      boss_selfcheck_die("Agahnim 2 must stay pinned at GT");
    if (a[kRandoDungeon_HyruleCastleEscape] != 0xFF)
      boss_selfcheck_die("HCE must have no boss");
    if (a[kRandoDungeon_ThievesTown] != kBoss_Blind)
      boss_selfcheck_die("Blind must stay pinned at Thieves' Town");
    if (a[kRandoDungeon_IcePalace] != kBoss_Kholdstare)
      boss_selfcheck_die("Kholdstare must stay pinned at Ice Palace");
    if (a[kRandoDungeon_TurtleRock] != kBoss_Trinexx)
      boss_selfcheck_die("Trinexx must stay pinned at Turtle Rock");
    // The 7 shuffleable dungeons hold a permutation of the 7-boss pool: each pool
    // boss appears exactly once; no pinned boss (Agahnim/Blind/Kholdstare/Trinexx)
    // leaks in.
    uint8 seen[12] = {0};
    for (uint32 i = 0; i < kBossShufflePoolCount; i++) {
      uint8 idx = a[kBossShuffleableDungeons[i]];
      if (idx == kBoss_Agahnim || idx == kBoss_Agahnim2 || idx == kBoss_Blind ||
          idx == kBoss_Kholdstare || idx == kBoss_Trinexx)
        boss_selfcheck_die("a pinned boss (Agahnim/Blind/Kholdstare/Trinexx) leaked into a shuffleable dungeon");
      if (idx >= 12) boss_selfcheck_die("boss-pool index out of range");
      seen[idx]++;
    }
    for (uint32 p = 0; p < kBossShufflePoolCount; p++) {
      if (seen[kBossShufflePool[p]] != 1)
        boss_selfcheck_die("shuffleable assignment is not a permutation of the 7-boss pool");
    }
  }

  // 3) Runtime remap: hard passthrough with no assignment installed; on-slot
  // remaps each vanilla boss sprite to a known boss sprite and never touches
  // the pinned Agahnim sprite (0x7A).
  {
    BossShuffle_Deactivate();
    if (BossShuffle_RemapSpriteType(0x53) != 0x53)
      boss_selfcheck_die("remap must be a passthrough with no assignment active");

    RandoSettings on = s;
    on.boss_shuffle = 1;
    uint8 a[16];
    (void)BossShuffle_Generate(&on, 0x0Eull, a);
    if (BossShuffle_RemapSpriteType(0x7A) != 0x7A)
      boss_selfcheck_die("pinned Agahnim sprite (0x7A) must never be remapped");
    for (uint32 i = 0; i < kBossSpriteMapCount; i++) {
      uint8 r = BossShuffle_RemapSpriteType(kBossSpriteMap[i].sprite_type);
      bool ok = false;
      for (uint32 j = 0; j < kBossSpriteMapCount; j++)
        if (kBossSpriteMap[j].sprite_type == r) { ok = true; break; }
      if (!ok) boss_selfcheck_die("remapped boss sprite is not a known boss type");
    }
    // Off-slot remap is identity (active assignment, boss_shuffle off):
    // every boss sprite maps to itself.
    RandoSettings off = s;
    off.boss_shuffle = 0;
    (void)BossShuffle_Generate(&off, 0x0Eull, a);
    for (uint32 i = 0; i < kBossSpriteMapCount; i++) {
      uint8 v = kBossSpriteMap[i].sprite_type;
      if (BossShuffle_RemapSpriteType(v) != v)
        boss_selfcheck_die("identity (off) assignment must remap each boss to itself");
    }
    BossShuffle_Deactivate();
  }

  // 4) Codegen↔C contract: the OP_CAN_KILL_BOSS dispatch tables in logic_data.c
  // (kRandoDungeonVanillaBoss / kRandoBossKillPred, emitted by rando_logic_gen.py)
  // MUST agree with this module's kBossVanilla map + boss-pool size. A drift here
  // would silently gate a dungeon's `- Boss`/`- Prize` on the WRONG boss's kill
  // predicate (a strand-class bug invisible to the corpus). kRandoDungeonVanillaBoss
  // is sized kRandoDungeonCount (13); kBossVanilla covers slots 0..12.
  {
    for (uint8 d = 0; d < 13; d++) {
      if (kRandoDungeonVanillaBoss[d] != kBossVanilla[d])
        boss_selfcheck_die("kRandoDungeonVanillaBoss (codegen) != kBossVanilla (shuffle_boss.c)");
    }
    // 12-entry boss-kill predicate table (10 shuffleable + Agahnim 1/2).
    if (kRandoBossKillPredCount != 12)
      boss_selfcheck_die("kRandoBossKillPredCount must be 12 (boss pool incl. both Agahnims)");
  }

  fprintf(stderr, "[BossShuffle_SelfCheck] OK\n");
}
