// shuffle_drops.c — Phase B Slice 8 (add-rando-shuffles-and-minigames).
//
// Deterministic permutation of the 56-entry vanilla prize-drop table
// (kPrizeItems[] in src/sprite.c: 7 packs × 8 slots) with a heart-drop floor.
//
// Runtime model: ForcePrizeDrop (src/sprite.c) computes a flat index
// `prize*8 | slot`, calls DropShuffle_Lookup to remap it, then reads
// kPrizeItems[remapped]. So this shuffle remaps which source entry each flat
// index reads — a permutation over 0..55.
//
// Heart-drop floor: pack 0 (flat indices 0..7) is the vanilla heart-heavy pack
// that weak overworld enemies draw from, so it is reachable from sphere 0. We
// require pack 0 to keep >=1 heart entry after the shuffle (re-roll on the same
// RNG stream; fall back to identity if exhausted). The enemy→pack binding is
// static (sprite_flags5 per sprite type, not sphere-indexed), so this
// structural floor is the faithful realization of the spec's "at least one
// tier reachable in spheres 0-2 contains a heart" in this fork's drop model.
//
// Determinism: identical (settings, seed_u64) → identical table on every host.

#include "shuffle_drops.h"
#include "rando_settings.h"
#include "rando_rng.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Vanilla LttP "small heart" drop sprite id — the heart the floor guarantees.
#define kDropItem_Heart 0xD8

// Canonical accessor over the vanilla prize-drop table (kPrizeItems[56]) lives
// in src/sprite.c (Sprite_VanillaPrizeItem, declared in sprite.h). Forward-
// declared here so the rando layer does NOT embed a copy of the ROM-derived
// table (the no-embedded-data guard scans src/rando/) and does NOT have to pull
// in variables.h via sprite.h. Keep this prototype in sync with sprite.h.
extern uint8 Sprite_VanillaPrizeItem(uint8 source_index);

// Per-shuffle RNG salt — distinguishes the drop-shuffle stream from boss/etc.
#define kDropShuffleSalt 0xD0DDA1FFD0DDA1FFull

// Pack 0 occupies flat indices [0,8). The heart-heavy starter pack.
#define kDropPack0Begin 0
#define kDropPack0End   8

// Bounded re-roll budget for the heart-drop floor. P(pack 0 gets no heart) on a
// uniform permutation is ~ C(43,8)/C(56,8) ~ 0.10 (13 heart entries of 56), so
// 16 retries leaves a vanishing (~1e-16) chance of falling back.
#define kDropHeartFloorMaxRetry 16

static uint8 g_drop_assignment[kDropTableEntryCount];
static bool g_drop_assignment_active = false;

// Fisher-Yates shuffle of `arr[0..n-1]` using `rng`.
static void fy_shuffle_u8(RandoRng *rng, uint8 *arr, uint32 n) {
  for (uint32 i = n; i > 1; i--) {
    uint32 j = Rng_NextRange(rng, i);
    uint8 t = arr[i - 1]; arr[i - 1] = arr[j]; arr[j] = t;
  }
}

// True iff pack 0's flat indices read at least one heart after the remap, i.e.
// some source index assigned to [0,8) has vanilla drop id == heart.
static bool drop_pack0_has_heart(const uint8 *map) {
  for (uint8 i = kDropPack0Begin; i < kDropPack0End; i++) {
    if (Sprite_VanillaPrizeItem(map[i]) == kDropItem_Heart) return true;
  }
  return false;
}

static void drop_identity(uint8 *map) {
  for (uint8 i = 0; i < kDropTableEntryCount; i++) map[i] = i;
}

uint32 DropShuffle_ComputeAssignment(const RandoSettings *settings,
                                     uint64 seed_u64,
                                     uint8 out_map[kDropTableEntryCount],
                                     bool *out_used_fallback) {
  if (out_used_fallback != NULL) *out_used_fallback = false;
  drop_identity(out_map);

  if (settings == NULL || settings->drop_shuffle == 0) {
    return 0;  // off — identity (vanilla drop table).
  }

  RandoRng rng;
  Rng_SeedFromU64(&rng, seed_u64 ^ kDropShuffleSalt);

  uint32 retries = 0;
  for (;;) {
    drop_identity(out_map);
    fy_shuffle_u8(&rng, out_map, kDropTableEntryCount);
    if (drop_pack0_has_heart(out_map)) break;  // heart floor satisfied
    retries++;
    if (retries >= kDropHeartFloorMaxRetry) {
      // Floor unreachable within budget — fall back to the vanilla identity
      // table so the player is never HP-starved. (Essentially never hit; the
      // spoiler surfaces it as a fallback_warning when it does.)
      drop_identity(out_map);
      if (out_used_fallback != NULL) *out_used_fallback = true;
      break;
    }
  }
  return retries;
}

bool DropShuffle_Generate(const RandoSettings *settings,
                          uint64 seed_u64,
                          const RandoPlacementTable *placements,
                          const RandoSpheres *spheres) {
  (void)placements;
  (void)spheres;
  bool used_fallback = false;
  DropShuffle_ComputeAssignment(settings, seed_u64, g_drop_assignment,
                                &used_fallback);
  g_drop_assignment_active = true;
  return true;
}

uint8 DropShuffle_Lookup(uint8 drop_table_index) {
  if (!g_drop_assignment_active) return drop_table_index;
  if (drop_table_index >= kDropTableEntryCount) return drop_table_index;
  return g_drop_assignment[drop_table_index];
}

void DropShuffle_Deactivate(void) {
  g_drop_assignment_active = false;
  drop_identity(g_drop_assignment);
}

// ---------------------------------------------------------------------------
// Self-check (--rando-selftest). Boss/drop shuffle is orthogonal to item
// placement, so the regression corpus cannot cover it; determinism + the
// permutation/heart-floor invariants are pinned here. Exits(2) on failure.
// ---------------------------------------------------------------------------
static void drop_selfcheck_die(const char *msg) {
  fprintf(stderr, "[DropShuffle_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

static void drop_assert_permutation(const uint8 *map, const char *ctx) {
  uint8 seen[kDropTableEntryCount] = {0};
  for (uint8 i = 0; i < kDropTableEntryCount; i++) {
    if (map[i] >= kDropTableEntryCount) drop_selfcheck_die(ctx);
    seen[map[i]]++;
  }
  for (uint8 i = 0; i < kDropTableEntryCount; i++)
    if (seen[i] != 1) drop_selfcheck_die(ctx);
}

void DropShuffle_SelfCheck(void) {
  RandoSettings s;
  Settings_SetDefaults(&s);

  // 0) The canonical prize table must actually contain a heart in pack 0 (else
  // the floor can never be satisfied — guards against a kPrizeItems edit that
  // silently breaks this module). Also confirms the accessor is wired.
  {
    bool any = false;
    for (uint8 i = kDropPack0Begin; i < kDropPack0End; i++)
      if (Sprite_VanillaPrizeItem(i) == kDropItem_Heart) { any = true; break; }
    if (!any) drop_selfcheck_die("vanilla pack 0 has no heart — heart floor unsatisfiable");
  }

  // 1) Off → identity table.
  {
    RandoSettings off = s;
    off.drop_shuffle = 0;
    uint8 a[kDropTableEntryCount];
    bool fb = true;
    DropShuffle_ComputeAssignment(&off, 0x0123456789ABCDEFull, a, &fb);
    for (uint8 i = 0; i < kDropTableEntryCount; i++)
      if (a[i] != i) drop_selfcheck_die("drop_shuffle off must be the identity table");
    if (fb) drop_selfcheck_die("off path must not report a fallback");
  }

  // 2) On → determinism + permutation + heart floor (pack 0 has a heart).
  {
    RandoSettings on = s;
    on.drop_shuffle = 1;
    uint8 a[kDropTableEntryCount], b[kDropTableEntryCount];
    bool fa = false, fb = false;
    DropShuffle_ComputeAssignment(&on, 0xDEADBEEFCAFEBABEull, a, &fa);
    DropShuffle_ComputeAssignment(&on, 0xDEADBEEFCAFEBABEull, b, &fb);
    if (memcmp(a, b, sizeof(a)) != 0)
      drop_selfcheck_die("same seed must produce identical drop table");
    if (fa != fb) drop_selfcheck_die("fallback flag must be deterministic");
    drop_assert_permutation(a, "shuffled drop table is not a permutation of 0..55");
    if (!drop_pack0_has_heart(a))
      drop_selfcheck_die("heart-drop floor violated: pack 0 has no heart");
  }

  // 3) Heart floor holds across many seeds (the retry loop must always satisfy
  // it on the common path; any fallback is itself a valid identity table whose
  // pack 0 keeps its vanilla hearts).
  {
    RandoSettings on = s;
    on.drop_shuffle = 1;
    for (uint64 seed = 1; seed <= 256; seed++) {
      uint8 a[kDropTableEntryCount];
      bool fb = false;
      DropShuffle_ComputeAssignment(&on, seed * 0x9E3779B97F4A7C15ull, a, &fb);
      drop_assert_permutation(a, "drop table not a permutation across seeds");
      if (!drop_pack0_has_heart(a))
        drop_selfcheck_die("heart floor violated for some seed");
    }
  }

  // 4) Runtime install/teardown: Generate installs (lookup remaps), Deactivate
  // restores passthrough.
  {
    DropShuffle_Deactivate();
    if (DropShuffle_Lookup(5) != 5)
      drop_selfcheck_die("lookup must be a passthrough with no table installed");
    RandoSettings on = s;
    on.drop_shuffle = 1;
    (void)DropShuffle_Generate(&on, 0x42ull, NULL, NULL);
    for (uint8 i = 0; i < kDropTableEntryCount; i++)
      if (DropShuffle_Lookup(i) >= kDropTableEntryCount)
        drop_selfcheck_die("installed lookup out of range");
    DropShuffle_Deactivate();
    if (DropShuffle_Lookup(5) != 5)
      drop_selfcheck_die("Deactivate must restore passthrough");
  }

  fprintf(stderr, "[DropShuffle_SelfCheck] OK\n");
}
