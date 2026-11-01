// door_keylogic.c — relocated small-key doors: candidate discovery, the
// combination search, the key-door softlock prover and the worst-case /
// big-key-restriction analysis.
//
// Ported from ALttPDoorRandomizer (MIT):
//   - DoorShuffle.find_small_key_door_candidates / find_key_door_candidates /
//     build_pair_list / valid_key_door_pair (candidate discovery)
//   - DoorShuffle.find_valid_combination + Utils.ncr / kth_combination +
//     build_sample_list (combination search; integer-only, u64 ncr)
//   - KeyDoorShuffle.validate_key_layout / validate_key_layout_sub_loop /
//     invalid_self_locking_key (the acceptance criteria)
//   - KeyDoorShuffle.create_key_counters / analyze_dungeon (worst-case rule:
//     find_best_counter.used_keys + 1) / find_bk_locked_sections
//
// door_type_mode=original: the per-dungeon key-door COUNT is vanilla
// (door_type_counts[d].smalls == chest_small_keys + drop-key rows, verified
// against the reference table for all 13 dungeons), capped at the reference's
// max_computation=11. Big-key doors stay at their vanilla kinds; the model
// passes them iff the big key is held.
//
// Deviations (reasons in the final report):
//   - the location-budget cap (calc_used_dungeon_items / max_keys) is not
//     applied: with vanilla counts it cannot bind in basic mode (the vanilla
//     dungeon holds those keys + items by construction).
//   - candidate discovery floods regions through non-blocked doors ignoring
//     crystal state, like the reference walk, but iterates doors in id order
//     instead of DFS discovery order (the determinism contract requires
//     id-ordered candidate lists before any draw).
#include "door_keylogic.h"

#include <stdio.h>
#include <string.h>

// Location-name accessor (rando_logic.h) — used only by the --dump-key-depth
// dev tool below; forward-declared to avoid pulling the logic header into the
// prover translation unit.
extern const char *Rando_GetLocationName(uint16 location_id);

// RoomData.DoorKind codes (ROM door-list kind bytes; values mirrored from the
// reference RoomData enum — structural ROM facts).
enum {
  kDoorKind_Normal = 0,
  kDoorKind_DungeonChanger = 20,
  kDoorKind_SmallKey = 28,
  kDoorKind_BigKey = 30,
  kDoorKind_StairKey = 32,
  kDoorKind_StairKey2 = 34,
  kDoorKind_StairKeyLow = 38,
  kDoorKind_Dashable = 40,
  kDoorKind_Bombable = 46,
};

static bool KindOkayNormal(uint8 kind) {  // okay_normals
  return kind == kDoorKind_Normal || kind == kDoorKind_SmallKey ||
         kind == kDoorKind_Bombable || kind == kDoorKind_Dashable ||
         kind == kDoorKind_DungeonChanger || kind == kDoorKind_BigKey;
}
static bool KindOkayInterior(uint8 kind) {  // okay_interiors
  return kind == kDoorKind_Normal || kind == kDoorKind_SmallKey ||
         kind == kDoorKind_Bombable || kind == kDoorKind_Dashable ||
         kind == kDoorKind_BigKey;
}
static bool KindStairKey(uint8 kind) {
  return kind == kDoorKind_StairKey || kind == kDoorKind_StairKey2 ||
         kind == kDoorKind_StairKeyLow;
}

static int PopCount16(uint32 v) {
  int n = 0;
  while (v) {
    v &= v - 1;
    n++;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Per-dungeon shuffle context + (mask, bk) state table
// ---------------------------------------------------------------------------

#define kKeyLocBytes ((kDoorTbl_LocationCount + 7) / 8)
#define kMaxKeyStates 2200

typedef struct KeyState {
  uint16 mask;
  uint8 bk;
  // counts (KeyCounter): free locations (blind-gated Thieves boss applied),
  // with/without big chests; reached drop keys; itemized free key sources.
  uint8 free_excl_bc, free_all, key_only, pot_free;
  uint16 child_mask;  // closed-but-reachable key pairs
  uint8 bk_child;     // some closed big-key door is reachable-adjacent
  // location sets for counter diffs / bk restrictions
  uint8 loc_free[kKeyLocBytes];   // free-class fork locations reached
  uint8 loc_other[kKeyLocBytes];  // prize-class fork locations reached
  uint32 drop_bits;               // kDoorTblDropKeys rows reached
  uint16 ev_locs;                 // event rows whose region is reached
} KeyState;

typedef struct KeyCtx {
  uint8 dungeon;
  DoorShuffleLayout *layout;
  const uint16 *origins;
  int norigins;
  int max_small_sources;  // chest_small_keys + active itemized key sources
  uint8 pot_tier;
  uint8 enemy_drop_keys;
  uint8 enemy_check_tier;
  DoorKeyPair pairs[kDoorShuffle_MaxKeyDoors];
  int np;
} KeyCtx;

static KeyState g_states[kMaxKeyStates];
static int g_nstates;
static int16 g_state_map[1 << (kDoorShuffle_MaxKeyDoors + 1)];
static int g_state_order[kMaxKeyStates];  // creation (BFS) order == indices

// Events whose granting location is "important" (imp_locations: 'Attic
// Cracked Floor', 'Suspicious Maiden'; Shining Light shares the attic
// location).
#define kImportantEvMask ((1u << kDoorEvent_ShiningLight) | \
                          (1u << kDoorEvent_MaidenRescued) | \
                          (1u << kDoorEvent_AtticCrackedFloor))

static void ResetStates(void) {
  g_nstates = 0;
  memset(g_state_map, 0xFF, sizeof(g_state_map));
}

static const KeyState *GetState(KeyCtx *kc, uint16 mask, bool bk) {
  uint32 key = mask | ((uint32)bk << kDoorShuffle_MaxKeyDoors);
  if (g_state_map[key] >= 0)
    return &g_states[g_state_map[key]];
  if (g_nstates >= kMaxKeyStates)
    return NULL;  // saturated; caller treats as failure
  DoorExploreSpec spec;
  DoorExploreResult res;
  memset(&spec, 0, sizeof(spec));
  spec.layout = kc->layout;
  spec.dungeon = kc->dungeon;
  spec.origins = kc->origins;
  spec.origin_count = kc->norigins;
  spec.pairs = kc->pairs;
  spec.pair_count = kc->np;
  spec.open_mask = mask;
  spec.bk_mode = kDoorBkMode_State;
  spec.bk_open = bk;
  spec.pot_tier = kc->pot_tier;
  spec.enemy_drop_keys = kc->enemy_drop_keys;
  spec.enemy_check_tier = kc->enemy_check_tier;
  DoorExplore_Core(&spec, &res);

  KeyState *st = &g_states[g_nstates];
  memset(st, 0, sizeof(*st));
  st->mask = mask;
  st->bk = bk;
  bool blind_ok = DoorExplore_Reached(&res, g_door_idx.maiden_region) &&
                  DoorExplore_Reached(&res, g_door_idx.attic_region);
  for (int i = 0; i < kDoorTbl_LocationCount; i++) {
    const DoorTblLocation *l = &kDoorTblLocations[i];
    if (kDoorTblRegions[l->region].dungeon != kc->dungeon)
      continue;
    if (!DoorExplore_Reached(&res, l->region))
      continue;
    uint8 cls = g_door_idx.loc_class[i];
    if (cls & kDoorLoc_Prize) {
      st->loc_other[i >> 3] |= 1 << (i & 7);
      continue;
    }
    st->loc_free[i >> 3] |= 1 << (i & 7);
    if ((cls & kDoorLoc_ThievesBoss) && !blind_ok)
      continue;  // Blind unavailable: not countable (still in the free set)
    st->free_all++;
    if (!(cls & kDoorLoc_BigChest))
      st->free_excl_bc++;
  }
  for (int i = 0; i < kDoorTbl_DropKeyCount; i++) {
    if (kDoorTblDropKeys[i].dungeon == kc->dungeon &&
        !Door_DropExcludedByActivePot(kc->dungeon, (uint16)i, kc->pot_tier) &&
        !Door_DropExcludedByActiveEnemyDrop(kc->dungeon, (uint16)i,
                                            kc->enemy_drop_keys) &&
        DoorExplore_Reached(&res, kDoorTblDropKeys[i].region)) {
      st->drop_bits |= 1u << i;
      st->key_only++;
    }
  }
  st->pot_free = (uint8)Door_CountActivePotKeySources(kc->dungeon, &res,
                                                       kc->pot_tier, NULL, NULL);
  st->pot_free += (uint8)Door_CountActiveEnemyDropKeySources(kc->dungeon, &res,
                                                             kc->enemy_drop_keys,
                                                             NULL, NULL);
  st->pot_free += (uint8)Door_CountActiveEnemyCheckKeySources(
      kc->dungeon, &res, kc->enemy_check_tier, NULL, NULL);
  for (int i = 0; i < kDoorTbl_EventCount; i++) {
    const DoorTblEvent *ev = &kDoorTblEvents[i];
    if (ev->region != 0xFFFF && kDoorTblRegions[ev->region].dungeon == kc->dungeon &&
        DoorExplore_Reached(&res, ev->region))
      st->ev_locs |= 1 << i;
  }
  // child doors: closed key pairs / big-key doors adjacent to a reached region.
  for (int i = 0; i < kc->np; i++) {
    if ((mask >> i) & 1)
      continue;
    uint16 dd[2] = { kc->pairs[i].a, kc->pairs[i].b };
    for (int k = 0; k < 2; k++) {
      if (dd[k] == 0xFFFF)
        continue;
      for (int j = 0; j < 2; j++) {
        uint16 e = g_door_idx.door_edge[dd[k]][j];
        if (e != 0xFFFF && Door_EdgePassableFrom(&res, e)) {
          st->child_mask |= 1 << i;
          break;
        }
      }
    }
  }
  if (!bk) {
    const DoorTblDungeon *dg = &kDoorTblDungeons[kc->dungeon];
    for (int door = 0; door < kDoorTbl_DoorCount && !st->bk_child; door++) {
      if (kDoorTblDoors[door].dungeon != kc->dungeon)
        continue;
      if (!Door_IsBkGated((uint16)door))
        continue;
      // skip special-bk doors of other dungeons via the dungeon filter above
      for (int j = 0; j < 2; j++) {
        uint16 e = g_door_idx.door_edge[door][j];
        if (e != 0xFFFF && Door_EdgePassableFrom(&res, e)) {
          st->bk_child = 1;
          break;
        }
      }
    }
    (void)dg;
  }
  g_state_map[key] = (int16)g_nstates;
  g_state_order[g_nstates] = g_nstates;
  g_nstates++;
  return st;
}

// cnt_avail_small_locations_by_ctr / cnt_avail_big_locations_by_ctr.
static int AvailSmall(const KeyCtx *kc, const KeyState *st) {
  int used = PopCount16(st->mask);
  int ttl = (st->bk ? st->free_all : st->free_excl_bc) + st->pot_free;
  int chest = ttl - (st->bk ? 1 : 0);
  if (chest > kc->max_small_sources)
    chest = kc->max_small_sources;
  int v = chest + st->key_only - used;
  return v > 0 ? v : 0;
}

static int AvailBigCtr(const KeyCtx *kc, const KeyState *st) {
  (void)kc;
  int used = PopCount16(st->mask) - st->key_only;
  if (used < 0)
    used = 0;
  used += st->bk ? 1 : 0;
  int ttl = st->bk ? st->free_all : st->free_excl_bc;
  int v = ttl - used;
  return v > 0 ? v : 0;
}

static int KeyWorstCaseCap(const KeyCtx *kc) {
  int nonpot_drops = 0;
  for (int i = 0; i < kDoorTbl_DropKeyCount; i++) {
    if (kDoorTblDropKeys[i].dungeon == kc->dungeon &&
        !Door_DropExcludedByActivePot(kc->dungeon, (uint16)i, kc->pot_tier)) {
      nonpot_drops++;
    }
  }
  return kc->max_small_sources + nonpot_drops;
}

// ---------------------------------------------------------------------------
// validate_key_layout (the proposal acceptance criterion)
// ---------------------------------------------------------------------------

#define kValMemoSize 8192
static uint32 g_val_memo_key[kValMemoSize];  // packed key + 1 (0 = empty)
static uint8 g_val_memo_val[kValMemoSize];

static void ValMemoReset(void) {
  memset(g_val_memo_key, 0, sizeof(g_val_memo_key));
}

static int ValMemoLookup(uint32 key, bool *val) {
  uint32 i = (key * 2654435761u) & (kValMemoSize - 1);
  while (g_val_memo_key[i]) {
    if (g_val_memo_key[i] == key + 1) {
      *val = g_val_memo_val[i] != 0;
      return 1;
    }
    i = (i + 1) & (kValMemoSize - 1);
  }
  return 0;
}

static void ValMemoStore(uint32 key, bool val) {
  uint32 i = (key * 2654435761u) & (kValMemoSize - 1);
  int guard = 0;
  while (g_val_memo_key[i]) {
    if (g_val_memo_key[i] == key + 1)
      return;
    if (++guard > kValMemoSize / 2)
      return;  // saturated: lose memoization, stay correct
    i = (i + 1) & (kValMemoSize - 1);
  }
  g_val_memo_key[i] = key + 1;
  g_val_memo_val[i] = val;
}

// invalid_self_locking_key: opening the last affordable key door must not
// strand the only progress behind an important-location dead end.
static bool InvalidSelfLock(const KeyState *st, const KeyState *prev, int prev_avail) {
  if (!prev || PopCount16(st->mask) == PopCount16(prev->mask))
    return false;
  // (big_key branch: with bk open the exploration already includes everything
  // behind big-key doors, so the reference's "open all new bk doors" copy is
  // the state itself.)
  bool new_important = false;
  for (int i = 0; i < kKeyLocBytes; i++)
    if (st->loc_other[i] & ~prev->loc_other[i])
      new_important = true;
  if ((st->ev_locs & ~prev->ev_locs) & kImportantEvMask)
    new_important = true;
  if (!new_important)
    return false;
  if (st->child_mask & ~prev->child_mask)
    return false;  // new small doors appeared
  return prev_avail - 1 == 0;
}

static bool ValidateSub(KeyCtx *kc, uint16 mask, bool bk, int used_locations,
                        const KeyState *prev, int prev_avail, int depth) {
  if (depth > kDoorShuffle_MaxKeyDoors + 4)
    return false;  // defensive; cannot trigger (one door opened per level)
  const KeyState *st = GetState(kc, mask, bk);
  if (!st)
    return false;
  bool smalls_avail = st->child_mask != 0;
  int num_bigs = st->bk_child ? 1 : 0;
  if (!smalls_avail && num_bigs == 0)
    return true;
  int ttl = bk ? st->free_all : st->free_excl_bc;
  int avail_small = AvailSmall(kc, st);
  int avail_big = bk ? 0 : (ttl - used_locations > 0 ? ttl - used_locations : 0);
  if (InvalidSelfLock(st, prev, prev_avail))
    return false;
  bool smalls_done = !smalls_avail || avail_small == 0;
  bool bk_done = bk || num_bigs == 0 || avail_big == 0;
  if (smalls_done && bk_done)
    return false;  // closed doors remain but nothing can be opened: softlock
  if (smalls_avail && avail_small > 0) {
    for (int i = 0; i < kc->np; i++) {
      if (!((st->child_mask >> i) & 1))
        continue;
      uint16 m2 = mask | (1 << i);
      int ul2 = used_locations + (PopCount16(m2) > st->key_only ? 1 : 0);
      uint32 key = m2 | ((uint32)bk << 12) | ((uint32)ul2 << 13);
      bool valid;
      if (!ValMemoLookup(key, &valid)) {
        valid = ValidateSub(kc, m2, bk, ul2, st, avail_small, depth + 1);
        ValMemoStore(key, valid);
      }
      if (!valid)
        return false;
    }
  }
  if (!bk && num_bigs > 0 && avail_big >= num_bigs) {
    int ul2 = used_locations + 1;
    uint32 key = mask | (1u << 12) | ((uint32)ul2 << 13);
    bool valid;
    if (!ValMemoLookup(key, &valid)) {
      valid = ValidateSub(kc, mask, true, ul2, st, avail_small, depth + 1);
      ValMemoStore(key, valid);
    }
    if (!valid)
      return false;
  }
  return true;
}

static bool ValidateLayout(KeyCtx *kc) {
  ResetStates();
  ValMemoReset();
  return ValidateSub(kc, 0, false, 0, NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// Candidate discovery (find_small_key_door_candidates)
// ---------------------------------------------------------------------------

// Region flood through non-blocked doors, crystal state and access rules
// ignored (find_key_door_candidates walks with `not d.blocked` only).
static void CandidateFlood(const KeyCtx *kc, uint8 *reach) {
  const DoorTblDungeon *dg = &kDoorTblDungeons[kc->dungeon];
  memset(reach, 0, (kDoorTbl_RegionCount + 7) / 8);
  uint16 stack[kDoorTbl_RegionCount];
  int sp = 0;
  for (int i = 0; i < kc->norigins; i++) {
    uint16 r = kc->origins[i];
    if (!((reach[r >> 3] >> (r & 7)) & 1)) {
      reach[r >> 3] |= 1 << (r & 7);
      stack[sp++] = r;
    }
  }
  while (sp > 0) {
    uint16 r = stack[--sp];
    for (uint32 ei = g_door_idx.edge_first[r]; ei < g_door_idx.edge_first[r + 1]; ei++) {
      const DoorTblEdge *e = &kDoorTblEdges[g_door_idx.edge_csr[ei]];
      uint16 dest = e->to_region;
      if (e->door != 0xFFFF) {
        const DoorTblDoor *d = &kDoorTblDoors[e->door];
        if (d->flags & kDoorTblFlag_Blocked)
          continue;
        if ((d->flags & kDoorTblFlag_InPool) && kc->layout) {
          uint16 p = kc->layout->pairing[e->door];
          if (p != 0xFFFF)
            dest = kDoorTblDoors[p].region;
        }
      }
      if (kDoorTblRegions[dest].dungeon != kc->dungeon)
        continue;
      if (!((reach[dest >> 3] >> (dest & 7)) & 1)) {
        reach[dest >> 3] |= 1 << (dest & 7);
        stack[sp++] = dest;
      }
    }
  }
  (void)dg;
}

static int RegionExitCount(uint16 r) {
  return g_door_idx.edge_first[r + 1] - g_door_idx.edge_first[r];
}

// valid_key_door_pair: same supertile needs one half in a <= 1-exit region.
static bool ValidKeyDoorPair(uint16 a, uint16 b) {
  if (kDoorTblDoors[a].room != kDoorTblDoors[b].room)
    return true;
  return RegionExitCount(kDoorTblDoors[a].region) <= 1 ||
         RegionExitCount(kDoorTblDoors[b].region) <= 1;
}

static int FindCandidates(const KeyCtx *kc, DoorKeyPair *out) {
  uint8 reach[(kDoorTbl_RegionCount + 7) / 8];
  CandidateFlood(kc, reach);
  uint8 emitted[kDoorTbl_DoorCount / 8 + 1];
  memset(emitted, 0, sizeof(emitted));
  int n = 0;
  for (int door = 0; door < kDoorTbl_DoorCount; door++) {
    const DoorTblDoor *d = &kDoorTblDoors[door];
    if (d->dungeon != kc->dungeon)
      continue;
    if ((emitted[door >> 3] >> (door & 7)) & 1)
      continue;
    uint16 r = d->region;
    if (!((reach[r >> 3] >> (r & 7)) & 1))
      continue;
    if (d->flags & kDoorTblFlag_Blocked)
      continue;
    if (d->pos == 0xFF || d->pos >= 4)
      continue;
    // okay_normals/okay_interiors admit DoorKind.BigKey because the reference
    // pipeline runs shuffle_big_key_doors FIRST and excludes its picks via the
    // `used` set passed into find_small_key_door_candidates. Under
    // door_type_mode=original the big-key doors are unmoved, so `used` == the
    // vanilla big-key doors (+ the special set get_special_big_key_doors:
    // Cellblock / Blind's Cell) — exclude them here or a vanilla BK door could
    // be re-keyed as a small-key door (the kind overlay would faithfully
    // un-big-key it at runtime).
    if (d->kind == kDoorKind_BigKey || (d->flags & kDoorTblFlag_VanillaBigKey))
      continue;
    uint16 partner;
    switch (d->type) {
    case kDoorTblType_Interior:
      if (!KindOkayInterior(d->kind))
        continue;
      partner = d->vanilla_partner;  // interior doors are static
      if (partner == 0xFFFF)
        continue;
      break;
    case kDoorTblType_SpiralStairs:
      if (!KindStairKey(d->kind))
        continue;  // only vanilla key stairs are spiral candidates
      partner = 0xFFFF;
      break;
    case kDoorTblType_Normal: {
      if (!KindOkayNormal(d->kind))
        continue;
      partner = Door_PartnerOf((uint16)door, kc->layout);
      if (partner == 0xFFFF)
        continue;
      const DoorTblDoor *p = &kDoorTblDoors[partner];
      if (!KindOkayNormal(p->kind) || p->pos == 0xFF || p->pos >= 4)
        continue;
      if (p->flags & kDoorTblFlag_Blocked)
        continue;
      if (p->kind == kDoorKind_BigKey || (p->flags & kDoorTblFlag_VanillaBigKey))
        continue;
      if (!ValidKeyDoorPair((uint16)door, partner))
        continue;
      break;
    }
    default:
      continue;
    }
    if (n >= kDoorKey_MaxCandidates)
      return -1;  // hard error, surfaced by the caller
    uint16 canon = (uint16)door;
    if (partner != 0xFFFF && partner < canon) {
      uint16 t = canon;
      canon = partner;
      partner = t;
    }
    out[n].a = canon;
    out[n].b = partner;
    n++;
    emitted[door >> 3] |= 1 << (door & 7);
    if (out[n - 1].b != 0xFFFF)
      emitted[out[n - 1].b >> 3] |= 1 << (out[n - 1].b & 7);
  }
  return n;
}

// ---------------------------------------------------------------------------
// Combination search (find_valid_combination + ncr / kth_combination /
// build_sample_list)
// ---------------------------------------------------------------------------

// Exact u64 multiplicative ncr; preserves the reference quirk
// (r == 0 or r >= n) -> 1. With pool <= 96 and r <= 11 this cannot overflow
// (C(96,11) ~ 1.1e13); on a hypothetical overflow clamp to UINT64_MAX (the
// sample path then over-draws indices, which kth_combination tolerates by
// returning the last combination — same as the reference's randint quirk).
static uint64 Ncr(uint32 n, uint32 r) {
  if (r == 0 || r >= n)
    return 1;
  uint64 result = 1;
  for (uint32 i = 1; i <= r; i++) {
    uint64 num = n - r + i;
    if (result > ~(uint64)0 / num)
      return ~(uint64)0;
    result = result * num / i;  // exact: product of i consecutive ints / i!
  }
  return result;
}

static void KthCombination(uint64 k, const DoorKeyPair *pool, int n, int r,
                           DoorKeyPair *out) {
  int i = 0, taken = 0;
  while (r - taken > 0) {
    if (n - i == r - taken) {
      while (i < n)
        out[taken++] = pool[i++];
      return;
    }
    uint64 c = Ncr((uint32)(n - i - 1), (uint32)(r - taken - 1));
    if (k < c) {
      out[taken++] = pool[i++];
    } else {
      k -= c;
      i++;
    }
  }
}

#define kMaxSamples 10000
static uint64 g_samples[kMaxSamples];
#define kSampleSetSize 16384
static uint64 g_sample_set[kSampleSetSize];

static uint64 RngRange64(RandoRng *rng, uint64 range) {  // [0, range)
  if (range <= 0xFFFFFFFFull)
    return Rng_NextRange(rng, (uint32)range);
  uint64 mask = range - 1;
  mask |= mask >> 1;
  mask |= mask >> 2;
  mask |= mask >> 4;
  mask |= mask >> 8;
  mask |= mask >> 16;
  mask |= mask >> 32;
  for (;;) {
    uint64 v = ((uint64)Rng_NextU32(rng) << 32 | Rng_NextU32(rng)) & mask;
    if (v < range)
      return v;
  }
}

static int CmpU64(const uint64 *a, const uint64 *b) {
  return *a < *b ? -1 : *a > *b ? 1 : 0;
}

static void SortU64(uint64 *v, int n) {  // insertion-free heapsort (no qsort)
  for (int gap = n / 2; gap > 0; gap /= 2)
    for (int i = gap; i < n; i++)
      for (int j = i - gap; j >= 0 && CmpU64(&v[j], &v[j + gap]) > 0; j -= gap) {
        uint64 t = v[j];
        v[j] = v[j + gap];
        v[j + gap] = t;
      }
}

// build_sample_list: enumerate when small, else 10000 DISTINCT draws from
// [0, combinations] (randint is inclusive — quirk preserved), sorted then
// Fisher-Yates shuffled.
static int BuildSampleList(uint64 combinations, RandoRng *rng) {
  int n = 0;
  if (combinations <= kMaxSamples) {
    n = (int)combinations;
    for (int i = 0; i < n; i++)
      g_samples[i] = (uint64)i;
  } else {
    memset(g_sample_set, 0, sizeof(g_sample_set));
    while (n < kMaxSamples) {
      uint64 v = RngRange64(rng, combinations + 1);
      uint64 stored = v + 1;  // 0 = empty sentinel
      uint32 idx = (uint32)(v ^ (v >> 32) ^ 0x9E37u) & (kSampleSetSize - 1);
      bool dup = false;
      while (g_sample_set[idx]) {
        if (g_sample_set[idx] == stored) {
          dup = true;
          break;
        }
        idx = (idx + 1) & (kSampleSetSize - 1);
      }
      if (dup)
        continue;
      g_sample_set[idx] = stored;
      g_samples[n++] = v;
    }
    SortU64(g_samples, n);
  }
  for (int i = n - 1; i > 0; i--) {  // random.shuffle
    uint32 j = Rng_NextRange(rng, (uint32)i + 1);
    uint64 t = g_samples[i];
    g_samples[i] = g_samples[j];
    g_samples[j] = t;
  }
  return n;
}

// ---------------------------------------------------------------------------
// analyze_dungeon: counters BFS, worst-case thresholds, bk restrictions
// ---------------------------------------------------------------------------

typedef struct OddDiff {
  uint8 free_d[kKeyLocBytes];
  uint8 other_d[kKeyLocBytes];
  uint32 drop_d;
  uint16 ev_d;
  uint16 child_d;  // pairs newly child in next
  uint8 bk_child_d;
  bool important;
} OddDiff;

static void ComputeOdd(OddDiff *o, const KeyState *next, const KeyState *cur) {
  memset(o, 0, sizeof(*o));
  for (int i = 0; i < kKeyLocBytes; i++) {
    o->free_d[i] = next->loc_free[i] & ~cur->loc_free[i];
    o->other_d[i] = next->loc_other[i] & ~cur->loc_other[i];
    if (o->other_d[i])
      o->important = true;
  }
  o->drop_d = next->drop_bits & ~cur->drop_bits;
  o->ev_d = next->ev_locs & ~cur->ev_locs;
  if (o->ev_d & kImportantEvMask)
    o->important = true;
  o->child_d = next->child_mask & ~(cur->child_mask | cur->mask);
  o->bk_child_d = next->bk_child && !cur->bk_child && !cur->bk;
}

static bool OddEmpty(const OddDiff *o) {  // empty_counter
  if (o->drop_d || o->child_d || o->bk_child_d)
    return false;
  for (int i = 0; i < kKeyLocBytes; i++)
    if (o->free_d[i])
      return false;
  return !o->important;
}

// relative_empty_counter: everything beyond the door already in `ctr`.
static bool RelativeEmpty(const OddDiff *o, const KeyState *ctr) {
  for (int i = 0; i < kKeyLocBytes; i++) {
    if (o->free_d[i] & ~ctr->loc_free[i])
      return false;
    if (o->other_d[i] & ~ctr->loc_other[i])
      return false;
  }
  if (o->drop_d & ~ctr->drop_bits)
    return false;
  if ((o->ev_d & kImportantEvMask) & ~(ctr->ev_locs & kImportantEvMask))
    return false;
  // unique child doors (not a child of ctr, not opened in ctr)
  if (o->child_d & ~(ctr->child_mask | ctr->mask))
    return false;
  if (o->bk_child_d && !ctr->bk_child && !ctr->bk)
    return false;
  return true;
}

static int BitsetPop(const uint8 *v, int bytes) {
  int n = 0;
  for (int i = 0; i < bytes; i++)
    n += PopCount16(v[i]);
  return n;
}

static int CountLocations(const KeyState *st) {
  // len(free) + len(key_only) + len(other) + len(important); important is a
  // subset of other in the reference, so prizes double-count (port as-is).
  int other = BitsetPop(st->loc_other, kKeyLocBytes) + PopCount16(st->ev_locs);
  int important = BitsetPop(st->loc_other, kKeyLocBytes) +
                  PopCount16(st->ev_locs & kImportantEvMask);
  return BitsetPop(st->loc_free, kKeyLocBytes) + st->key_only + st->pot_free +
         other + important;
}

// find_best_counter: scan counters in creation order; max used_keys, ties by
// CountLocations; skip relative-empty counters unless empty_flag.
static int FindBestCounter(const KeyCtx *kc, int pair, const OddDiff *odd, bool empty_flag) {
  int best = 0, best_locs = 0;
  bool have = false;
  for (int s = 0; s < g_nstates; s++) {
    const KeyState *ctr = &g_states[s];
    if ((ctr->mask >> pair) & 1)
      continue;
    int used = PopCount16(ctr->mask);
    int locs = CountLocations(ctr);
    if (!have || used > best || (used == best && locs > best_locs)) {
      if (empty_flag || !RelativeEmpty(odd, ctr)) {
        best = used;
        best_locs = locs;
        have = true;
      }
    }
  }
  return have ? best : 0;
}

// create_key_counters + analyze_dungeon worst-case slice +
// find_bk_locked_sections. Assumes ValidateLayout just succeeded (state table
// holds the proposal's states; rebuilt here from scratch for a clean BFS
// order).
static bool AnalyzeDungeon(KeyCtx *kc) {
  ResetStates();
  // --- counters BFS (create_key_counters): FIFO over openable children ----
  uint32 bfs[kMaxKeyStates];
  int head = 0, tail = 0;
  uint8 seen[1 << (kDoorShuffle_MaxKeyDoors + 1)];
  memset(seen, 0, sizeof(seen));
  const KeyState *st0 = GetState(kc, 0, false);
  if (!st0)
    return false;
  bfs[tail++] = 0;
  seen[0] = 1;
  while (head < tail) {
    uint32 key = bfs[head++];
    uint16 mask = key & ((1 << kDoorShuffle_MaxKeyDoors) - 1);
    bool bk = (key >> kDoorShuffle_MaxKeyDoors) & 1;
    const KeyState *st = GetState(kc, mask, bk);
    if (!st)
      return false;
    for (int i = 0; i < kc->np; i++) {
      if (((st->child_mask >> i) & 1) && AvailSmall(kc, st) > 0) {
        uint32 k2 = (mask | (1 << i)) | ((uint32)bk << kDoorShuffle_MaxKeyDoors);
        if (!seen[k2] && tail < kMaxKeyStates) {
          seen[k2] = 1;
          if (!GetState(kc, (uint16)(mask | (1 << i)), bk))
            return false;
          bfs[tail++] = k2;
        }
      }
    }
    if (st->bk_child && !bk && AvailBigCtr(kc, st) > 0) {
      uint32 k2 = mask | (1u << kDoorShuffle_MaxKeyDoors);
      if (!seen[k2] && tail < kMaxKeyStates) {
        seen[k2] = 1;
        if (!GetState(kc, mask, true))
          return false;
        bfs[tail++] = k2;
      }
    }
  }

  // --- worst-case rules (analyze_dungeon queue, small-entered before big) --
  uint8 d = kc->dungeon;
  int wc_cap = KeyWorstCaseCap(kc);
  for (int i = 0; i < kc->np; i++)  // conservative default if never reached
    kc->layout->key_worst_case[d][i] = (uint8)wc_cap;
  uint32 smallq[kMaxKeyStates], bigq[kMaxKeyStates];
  int sh = 0, stl = 0, bh = 0, btl = 0;
  uint8 visited[1 << (kDoorShuffle_MaxKeyDoors + 1)];
  memset(visited, 0, sizeof(visited));
  smallq[stl++] = 0;
  visited[0] = 1;
  uint16 completed = 0;
  while (sh < stl || bh < btl) {
    uint32 key = (sh < stl) ? smallq[sh++] : bigq[bh++];
    uint16 mask = key & ((1 << kDoorShuffle_MaxKeyDoors) - 1);
    bool bk = (key >> kDoorShuffle_MaxKeyDoors) & 1;
    const KeyState *st = GetState(kc, mask, bk);
    if (!st)
      return false;
    for (int i = 0; i < kc->np; i++) {
      if (!((st->child_mask >> i) & 1) || AvailSmall(kc, st) <= 0)
        continue;
      const KeyState *next = GetState(kc, (uint16)(mask | (1 << i)), bk);
      if (!next)
        return false;
      if (!((completed >> i) & 1)) {
        OddDiff odd;
        ComputeOdd(&odd, next, st);
        int best = FindBestCounter(kc, i, &odd, OddEmpty(&odd));
        int wc = best + 1;  // create_worst_case_rule: used_keys + 1
        kc->layout->key_worst_case[d][i] = (uint8)(wc > wc_cap ? wc_cap : wc);
        completed |= 1 << i;
      }
      uint32 k2 = (mask | (1 << i)) | ((uint32)bk << kDoorShuffle_MaxKeyDoors);
      if (!visited[k2]) {
        visited[k2] = 1;
        smallq[stl++] = k2;
      }
    }
    if (st->bk_child && !bk && AvailBigCtr(kc, st) > 0) {
      uint32 k2 = mask | (1u << kDoorShuffle_MaxKeyDoors);
      if (!visited[k2]) {
        visited[k2] = 1;
        bigq[btl++] = k2;
      }
    }
  }

  // --- find_bk_locked_sections -------------------------------------------
  if (kDoorTblDungeons[d].has_big_key) {
    uint8 all_chest[kKeyLocBytes], not_required[kKeyLocBytes];
    memset(all_chest, 0, sizeof(all_chest));
    memset(not_required, 0, sizeof(not_required));
    bool big_chest_allowed = true;  // accessibility != 'locations' (fork max
                                    // tier is 'items'); flipped if a bk-opened
                                    // counter holds an important location
    for (int s = 0; s < g_nstates; s++) {
      const KeyState *ctr = &g_states[s];
      bool imp = ctr->ev_locs & kImportantEvMask;
      for (int i = 0; i < kKeyLocBytes; i++) {
        all_chest[i] |= ctr->loc_free[i];
        if (!ctr->bk)
          not_required[i] |= ctr->loc_free[i] | ctr->loc_other[i];
        imp |= ctr->loc_other[i] != 0;
      }
      if (ctr->bk && imp)
        big_chest_allowed = false;
    }
    for (int i = 0; i < kDoorTbl_LocationCount; i++) {
      bool restricted = (all_chest[i >> 3] >> (i & 7)) & 1 &&
                        !((not_required[i >> 3] >> (i & 7)) & 1);
      if (!restricted && !big_chest_allowed &&
          kDoorTblRegions[kDoorTblLocations[i].region].dungeon == d &&
          (g_door_idx.loc_class[i] & (kDoorLoc_BigChest | kDoorLoc_BkLockedName)) &&
          ((all_chest[i >> 3] >> (i & 7)) & 1))
        restricted = true;
      if (restricted) {
        if (kc->layout->bk_restricted_count >= kDoorShuffle_MaxBkRestricted) {
          fprintf(stderr, "door-keylogic: bk_restricted overflow (dungeon %d)\n", d);
          return false;  // hard error, never truncate silently
        }
        kc->layout->bk_restricted[kc->layout->bk_restricted_count++] =
            kDoorTblLocations[i].fork_loc_id;
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

bool DoorKeys_ShuffleDungeon(uint8 dungeon, RandoRng *rng,
                             const uint16 *origins, int origin_count,
                             DoorShuffleLayout *layout) {
  DoorIdx_Init();
  KeyCtx kc;
  memset(&kc, 0, sizeof(kc));
  kc.dungeon = dungeon;
  kc.layout = layout;
  kc.origins = origins;
  kc.norigins = origin_count;
  kc.pot_tier = layout ? layout->pot_tier : kPotShuffle_Off;
  kc.enemy_drop_keys = layout ? layout->enemy_drop_keys : 0;
  kc.enemy_check_tier = layout ? layout->enemy_check_tier : kEnemyDropChecks_Off;
  kc.max_small_sources = kDoorTblDungeons[dungeon].chest_small_keys +
                         Door_CountActiveKeyPots(dungeon, kc.pot_tier) +
                         Door_CountActiveEnemyDropKeys(dungeon, kc.enemy_drop_keys) +
                         Door_CountActiveEnemyChecks(dungeon, kc.enemy_check_tier);

  static DoorKeyPair candidates[kDoorKey_MaxCandidates];
  int ncand = FindCandidates(&kc, candidates);
  if (ncand < 0) {
    fprintf(stderr, "door-keylogic: candidate overflow (dungeon %d)\n", dungeon);
    return false;
  }

  // door_type_counts smalls == chest keys + drop keys (verified identity);
  // max_computation = 11 (reference cap on the combination space).
  int needed = kDoorTblDungeons[dungeon].chest_small_keys + g_door_idx.drop_cnt[dungeon];
  if (needed > 11)
    needed = 11;
  if (needed > kDoorShuffle_MaxKeyDoors)
    needed = kDoorShuffle_MaxKeyDoors;
  if (needed > ncand)
    needed = ncand;  // "lowering key door count, not enough candidates"

  for (;;) {
    uint64 combos = Ncr((uint32)ncand, (uint32)needed);
    int ns = BuildSampleList(combos, rng);
    for (int si = 0; si < ns; si++) {
      kc.np = needed;
      KthCombination(g_samples[si], candidates, ncand, needed, kc.pairs);
      if (ValidateLayout(&kc))
        goto accepted;
    }
    if (needed == 0)
      return false;  // full exhaustion (reference raises "Bad dungeon")
    needed--;  // "lowering key door count, no valid layouts"
  }

accepted:
  layout->key_door_count[dungeon] = (uint8)kc.np;
  for (int i = 0; i < kc.np; i++)
    layout->key_doors[dungeon][i] = kc.pairs[i].a;
  return AnalyzeDungeon(&kc);
}

// ---------------------------------------------------------------------------
// Dev dump (--dump-key-depth): the AUTHORITATIVE per-region / per-location
// minimum small-key DEPTH under VANILLA doors, for pot-sanity task #25 (model
// pot keys in the logic).
//
// depth(region) = min over every key-door open-mask M that REACHES the region
// of popcount(M) — the fewest vanilla small-key doors that must be unlocked to
// stand there (items lenient, big-key doors treated open, every dungeon portal
// an origin). This is the true key-door depth counting ALL keys: the door
// table's chest_small_keys is the ALTTPR placed-key count and kDoorTblDropKeys
// carries the vanilla pot/drop keys, so the prover graph already prices both.
// It is the number a location must require as HAS_AMOUNT(SmallKey_X, depth)
// once pot keys become first-class shuffled items.
//
// A naive 0-1 BFS over kDoorTblEdges is WRONG (it ignores edge rules + crystal
// switch state, so free/logical edges bypass key doors and severely under-count
// — GT came out depth 2 vs ~6, MM/IP all-0). This enumerates through
// DoorExplore_Core, the same reachability engine the placer/prover use.
//
// Pots are not in kDoorTblLocations (door x pot is deferred), so we also emit a
// deduped room->region(+depth) map; the Python join resolves each pot's room.
// ---------------------------------------------------------------------------
int DoorKeys_DumpKeyDepth(const char *path) {
  DoorIdx_Init();
  FILE *out = fopen(path, "wb");
  if (!out) {
    fprintf(stderr, "--dump-key-depth: cannot open %s\n", path);
    return 1;
  }
  fprintf(out,
          "# zelda3 key-depth dump v2 - pot-sanity task #25 (depth -1 = unreachable)\n"
          "# depth=   WORST-CASE keys to guarantee reaching (wild requirement).\n"
          "# mindepth= SHORTEST-PATH keys to reach (in-context dungeon-keys req).\n"
          "# DUNGEON d chest=.. drop=.. pairs=.. name=..\n"
          "# REGION d region depth=.. mindepth=.. name=..\n"
          "# LOC d loc=forkid region=.. depth=.. mindepth=.. name=..\n"
          "# DROP d region=.. depth=.. mindepth=.. name=..   (vanilla pot/drop key spot)\n"
          "# ROOM room=0xNN region=.. depth=.. mindepth=.. dungeon=d\n");

  // Vanilla layout: pool stubs must redirect through their VANILLA partner, or
  // DoorExplore_Core (which only applies the pool redirect when spec->layout is
  // set — shuffle_doors.c:526) leaves pool edges pointing at their placeholder
  // stub region and the dungeon's real key-door chokepoints stop isolating
  // anything (PoD then read as all-depth-0). The real prover always passes a
  // layout; the vanilla depth needs the identity one.
  static DoorShuffleLayout vanilla;
  memset(&vanilla, 0, sizeof(vanilla));
  for (int i = 0; i < kDoorTbl_DoorCount; i++)
    vanilla.pairing[i] = (kDoorTblDoors[i].flags & kDoorTblFlag_InPool)
                             ? kDoorTblDoors[i].vanilla_partner
                             : 0xFFFF;

  static uint8 depth[kDoorTbl_RegionCount];
  // SHORTEST-PATH (min) key depth, alongside the worst-case `depth`. mindepth(R)
  // = the fewest key doors that must be open on SOME order to reach R = the min
  // popcount over every frontier-reachable open_mask that floods R. This is the
  // in-context DUNGEON-keys requirement (you collect keys en route, so the
  // shortest chain is necessary + sufficient for a known layout), as opposed to
  // the worst-case `depth` the WILD requirement uses.
  static uint8 mindepth[kDoorTbl_RegionCount];
  for (uint8 d = 0; d < kDoorTbl_DungeonCount; d++) {
    // Vanilla key-door pairs: each VanillaSmallKey-flagged door paired with its
    // vanilla partner (spiral stairs are single-direction). Dedup both sides.
    DoorKeyPair pairs[24];
    int np = 0;
    static uint8 emitted[kDoorTbl_DoorCount / 8 + 1];
    memset(emitted, 0, sizeof(emitted));
    for (int door = 0; door < kDoorTbl_DoorCount; door++) {
      const DoorTblDoor *dd = &kDoorTblDoors[door];
      if (dd->dungeon != d || !(dd->flags & kDoorTblFlag_VanillaSmallKey))
        continue;
      if ((emitted[door >> 3] >> (door & 7)) & 1)
        continue;
      if (np >= (int)(sizeof(pairs) / sizeof(pairs[0]))) {
        fprintf(out, "# ERROR dungeon %d: more key doors than pairs[]\n", d);
        break;
      }
      uint16 a = (uint16)door;
      uint16 b = (dd->type == kDoorTblType_SpiralStairs) ? 0xFFFF : dd->vanilla_partner;
      pairs[np].a = a;
      pairs[np].b = b;
      emitted[door >> 3] |= 1 << (door & 7);
      if (b != 0xFFFF && b < kDoorTbl_DoorCount)
        emitted[b >> 3] |= 1 << (b & 7);
      np++;
    }
    // Origins: walk-in lobby portals only (is_drop=0). DROP arrivals (hole/
    // pit landings) must NOT be free origins — you reach them by FALLING from a
    // floor that is itself behind key doors, so the prover should price them via
    // the in-dungeon falldown edge, not let them short-circuit deep regions to
    // depth ~0 (that bug made PoD's boss read depth 1 instead of 6).
    static uint16 origins[kDoorTbl_PortalCount];
    int norig = 0;
    for (int p = 0; p < kDoorTbl_PortalCount; p++)
      if (kDoorTblPortals[p].dungeon == d && !kDoorTblPortals[p].is_drop)
        origins[norig++] = kDoorTblPortals[p].region;

    // Per-region WORST-CASE small-key depth via the counter BFS — the validated
    // ALTTPR / door-rando model. depth(R) = the fewest keys that GUARANTEE
    // reaching R regardless of door-opening order = 1 + (the most keys an
    // adversary can spend on an openable counter-state that still does NOT reach
    // R), capped at the dungeon's real key count; 0 if reachable with no keys.
    //
    // MIN-depth is WRONG: PoD's boss has a 1-key lucky path, but you can waste 5
    // keys without reaching it, so it needs 6 — matching ALTTPR `has(KeyD1, 6)`.
    // Big-key doors are treated open (a location needing the big key carries a
    // separate HAS_ITEM(BigKey_*)); we only price the small-key worst case.
    uint8 total_keys = (uint8)(kDoorTblDungeons[d].chest_small_keys + g_door_idx.drop_cnt[d]);
    for (int r = 0; r < kDoorTbl_RegionCount; r++) {
      depth[r] = 0xFF;
      mindepth[r] = 0xFF;
    }
    if (np > 14) {
      fprintf(out, "# ERROR dungeon %d np=%d exceeds BFS cap\n", d, np);
    } else {
      static uint8 reached0[kDoorTbl_RegionCount];
      static uint8 ever[kDoorTbl_RegionCount];
      static uint8 worst[kDoorTbl_RegionCount];
      memset(reached0, 0, sizeof(reached0));
      memset(ever, 0, sizeof(ever));
      memset(worst, 0, sizeof(worst));
      static uint8 seen[1 << 14];
      static uint16 queue[1 << 14];
      memset(seen, 0, (size_t)1 << np);
      int qh = 0, qt = 0;
      queue[qt++] = 0;
      seen[0] = 1;
      while (qh < qt) {
        uint16 mask = queue[qh++];
        int pc = 0;
        for (uint16 m = mask; m; m &= m - 1)
          pc++;
        DoorExploreSpec spec;
        DoorExploreResult res;
        memset(&spec, 0, sizeof(spec));
        spec.layout = &vanilla;  // pool stubs -> vanilla partners (see above)
        spec.dungeon = d;
        spec.origins = origins;
        spec.origin_count = norig;
        spec.pairs = pairs;
        spec.pair_count = np;
        spec.open_mask = mask;
        spec.bk_mode = kDoorBkMode_Open;
        DoorExplore_Core(&spec, &res);
        for (int r = 0; r < kDoorTbl_RegionCount; r++) {
          if (kDoorTblRegions[r].dungeon != d)
            continue;
          bool reach = DoorExplore_Reached(&res, (uint16)r);
          if (reach) {
            ever[r] = 1;
            if (mask == 0)
              reached0[r] = 1;
            if ((uint8)pc < mindepth[r])
              mindepth[r] = (uint8)pc;  // shortest open-mask that floods R
          } else if ((uint8)pc > worst[r]) {
            worst[r] = (uint8)pc;
          }
        }
        // Expand: open any frontier-reachable closed key door. UNPRUNED — for
        // the WILD-keys requirement we must never under-gate (under-gate =
        // strand), so the worst case counts every openable order including
        // wasteful ones; the value over-gates harmlessly (a wild player holds
        // the keys externally before entering). Bounded by the real key count.
        if (pc < total_keys) {
          for (int i = 0; i < np; i++) {
            if ((mask >> i) & 1)
              continue;
            bool frontier = false;
            uint16 dd2[2] = { pairs[i].a, pairs[i].b };
            for (int k = 0; k < 2 && !frontier; k++) {
              if (dd2[k] == 0xFFFF)
                continue;
              for (int j = 0; j < 2; j++) {
                uint16 e = g_door_idx.door_edge[dd2[k]][j];
                if (e != 0xFFFF && Door_EdgePassableFrom(&res, e)) {
                  frontier = true;
                  break;
                }
              }
            }
            if (!frontier)
              continue;
            uint16 m2 = (uint16)(mask | (1u << i));
            if (!seen[m2]) {
              seen[m2] = 1;
              queue[qt++] = m2;
            }
          }
        }
      }
      for (int r = 0; r < kDoorTbl_RegionCount; r++) {
        if (kDoorTblRegions[r].dungeon != d)
          continue;
        if (!ever[r])
          depth[r] = 0xFF;  // unreachable even with every key
        else if (reached0[r])
          depth[r] = 0;
        else {
          uint8 v = (uint8)(worst[r] + 1);
          depth[r] = v > total_keys ? total_keys : v;
        }
      }
    }

    // Per-door WORST-CASE thresholds via AnalyzeDungeon — the calibrated
    // door-rando counter analysis (find_best_counter / RelativeEmpty), the SAME
    // values the door-shuffle oracle gates on. Dumped for the upcoming pot-key
    // oracle; drops are still free here (cur-equivalent) — the
    // pot-drops-as-inventory variant is the next step. Validates that even
    // sprawling GT yields GRADUATED per-door thresholds, not a flat over-count.
    if (np >= 1 && np <= kDoorShuffle_MaxKeyDoors) {
      KeyCtx kc;
      memset(&kc, 0, sizeof(kc));
      kc.dungeon = d;
      kc.layout = &vanilla;
      kc.origins = origins;
      kc.norigins = norig;
      kc.pot_tier = kPotShuffle_Off;
      kc.max_small_sources = kDoorTblDungeons[d].chest_small_keys;
      kc.np = np;
      for (int i = 0; i < np; i++)
        kc.pairs[i] = pairs[i];
      vanilla.bk_restricted_count = 0;  // don't accumulate across dungeons
      if (AnalyzeDungeon(&kc)) {
        for (int i = 0; i < np; i++)
          fprintf(out, "THRESH %d door=%d a=%d b=%d wc=%d\n", d, i, pairs[i].a,
                  pairs[i].b, vanilla.key_worst_case[d][i]);
      } else {
        fprintf(out, "# THRESH dungeon %d AnalyzeDungeon failed\n", d);
      }
    }

    fprintf(out, "DUNGEON %d chest=%d drop=%d pairs=%d name=\"%s\"\n", d,
            kDoorTblDungeons[d].chest_small_keys, g_door_idx.drop_cnt[d], np,
            DoorIdx_Name(kDoorTblDungeons[d].name_off));
    for (int r = 0; r < kDoorTbl_RegionCount; r++) {
      if (kDoorTblRegions[r].dungeon != d)
        continue;
      fprintf(out, "REGION %d %d depth=%d mindepth=%d name=\"%s\"\n", d, r,
              depth[r] == 0xFF ? -1 : depth[r],
              mindepth[r] == 0xFF ? -1 : mindepth[r],
              DoorIdx_Name(kDoorTblRegions[r].name_off));
    }
    for (int i = 0; i < kDoorTbl_LocationCount; i++) {
      const DoorTblLocation *l = &kDoorTblLocations[i];
      if (kDoorTblRegions[l->region].dungeon != d)
        continue;
      fprintf(out, "LOC %d loc=%d region=%d depth=%d mindepth=%d name=\"%s\"\n", d,
              l->fork_loc_id, l->region,
              depth[l->region] == 0xFF ? -1 : depth[l->region],
              mindepth[l->region] == 0xFF ? -1 : mindepth[l->region],
              Rando_GetLocationName(l->fork_loc_id));
    }
    for (int i = 0; i < kDoorTbl_DropKeyCount; i++) {
      const DoorTblDropKey *dk = &kDoorTblDropKeys[i];
      if (dk->dungeon != d)
        continue;
      fprintf(out, "DROP %d index=%d region=%d depth=%d mindepth=%d name=\"%s\"\n", d, i,
              dk->region, depth[dk->region] == 0xFF ? -1 : depth[dk->region],
              mindepth[dk->region] == 0xFF ? -1 : mindepth[dk->region],
              DoorIdx_Name(kDoorTblRegions[dk->region].name_off));
    }
    // Deduped room -> region(+depth) for joining pots by room.
    for (int door = 0; door < kDoorTbl_DoorCount; door++) {
      const DoorTblDoor *dd = &kDoorTblDoors[door];
      if (dd->dungeon != d)
        continue;
      bool dup = false;
      for (int e = 0; e < door; e++)
        if (kDoorTblDoors[e].dungeon == d && kDoorTblDoors[e].room == dd->room &&
            kDoorTblDoors[e].region == dd->region) {
          dup = true;
          break;
        }
      if (dup)
        continue;
      fprintf(out, "ROOM room=0x%02x region=%d depth=%d mindepth=%d dungeon=%d\n",
              dd->room, dd->region, depth[dd->region] == 0xFF ? -1 : depth[dd->region],
              mindepth[dd->region] == 0xFF ? -1 : mindepth[dd->region], d);
    }
  }
  fclose(out);
  fprintf(stderr, "--dump-key-depth: wrote %s\n", path);
  return 0;
}
