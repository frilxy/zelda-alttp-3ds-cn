// shuffle_doors.c — door-shuffle generation: static-topology index, the
// crystal-aware explorer, the per-dungeon door stitcher, layout digest and
// the --door-selftest harness.
//
// Ported from ALttPDoorRandomizer's live pipeline (MIT):
//   - explorer: DungeonStitcher.ExplorationState dual blue/orange exploration,
//     reformulated as a bitwise fixed point over (region x crystal-color).
//     Equivalence: the reference explores (region, crystal) pairs with
//     monotonically growing visited_blue/visited_orange lists, so the fixed
//     point is order-independent and per-color bit propagation matches it.
//   - stitcher: DungeonStitcher.generate_dungeon_find_proposal /
//     create_random_proposal / modify_proposal / check_valid /
//     determine_paths_for_dungeon.
//   - key shuffle: DoorShuffle.shuffle_small_key_doors flow (door_keylogic.c).
//
// Deviations from the reference (documented in detail at the relevant code):
//   - whole-dungeon stitching with STAGED ORIGINS replaces
//     split_dungeon_builder + treat_split_as_whole_dungeon for Desert/Skull
//     (player-sound: origins on inaccessible ledges only activate once a
//     region exiting onto that ledge is reachable, mirroring
//     find_enabled_origins/enable_new_entrances).
//   - edge access rules are evaluated (leniently: every Vm leaf true) where
//     the reference stitcher ignores them entirely; regions whose every
//     incoming edge is constant-false are exempted from the connectivity
//     requirement instead (see region_exempt).
//   - Skull Pinball WS stays blocked in the model (vanilla trap_door_mode
//     semantics — check_proposed_3/can_traverse); check_for_pinball_fix's
//     runtime door mutation is NOT modeled, so no proposal may rely on it.
#include "shuffle_doors.h"
#include "door_keylogic.h"
#include "door_runtime.h"  // DoorRt_KindOverlaySelfCheck (Stage-1b kind overlay)
#include "rando_rng.h"
#include "rando_generate.h"  // Rando_PlaceWithEntrances + overlay clear (leak selftest)
#include "dungeon_ids.h"  // Key Ring full-stock threshold guard

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(kDoorTbl_DungeonCount == kRandoDungeon_Count,
               "door-table and Key Ring dungeon orders must stay aligned");

// From the generated logic tables (rando_logic.h pulls in much more; the
// names are only used for one-time location classification at init).
const char *Rando_GetLocationName(uint16 location_id);

DoorIdx g_door_idx;

// ---------------------------------------------------------------------------
// Static index construction
// ---------------------------------------------------------------------------

static bool StrSuffix(const char *s, const char *suffix) {
  size_t ls = strlen(s), lf = strlen(suffix);
  return ls >= lf && memcmp(s + ls - lf, suffix, lf) == 0;
}

static uint16 RegionByName(const char *name) {
  for (int i = 0; i < kDoorTbl_RegionCount; i++)
    if (!strcmp(DoorIdx_Name(kDoorTblRegions[i].name_off), name))
      return (uint16)i;
  return 0xFFFF;
}

static uint16 DoorByName(const char *name) {
  for (int i = 0; i < kDoorTbl_DoorCount; i++)
    if (!strcmp(DoorIdx_Name(kDoorTblDoors[i].name_off), name))
      return (uint16)i;
  return 0xFFFF;
}

void DoorIdx_Init(void) {
  DoorIdx *x = &g_door_idx;
  if (x->ready)
    return;
  memset(x, 0, sizeof(*x));

  // Edge CSR by from_region (preserving table order within a region).
  uint16 count[kDoorTbl_RegionCount] = { 0 };
  for (int e = 0; e < kDoorTbl_EdgeCount; e++)
    count[kDoorTblEdges[e].from_region]++;
  uint32 acc = 0;
  for (int r = 0; r < kDoorTbl_RegionCount; r++) {
    x->edge_first[r] = (uint16)acc;
    acc += count[r];
    count[r] = 0;
  }
  x->edge_first[kDoorTbl_RegionCount] = (uint16)acc;
  for (int e = 0; e < kDoorTbl_EdgeCount; e++) {
    uint16 r = kDoorTblEdges[e].from_region;
    x->edge_csr[x->edge_first[r] + count[r]++] = (uint16)e;
  }

  // Exempt regions: " Portal" placeholders + no satisfiable incoming edge
  // (every incoming rule is the constant-false op).
  {
    uint8 incoming_ok[kDoorTbl_RegionCount] = { 0 };
    for (int e = 0; e < kDoorTbl_EdgeCount; e++) {
      uint16 rule = kDoorTblEdges[e].rule;
      if (rule == 0xFFFF || kDoorTblRuleBlob[rule] != kDoorRuleOp_False)
        incoming_ok[kDoorTblEdges[e].to_region] = 1;
    }
    for (int r = 0; r < kDoorTbl_RegionCount; r++) {
      if (!incoming_ok[r] || StrSuffix(DoorIdx_Name(kDoorTblRegions[r].name_off), " Portal"))
        x->region_exempt[r >> 3] |= 1 << (r & 7);
    }
  }

  // Location classification by fork name.
  for (int i = 0; i < kDoorTbl_LocationCount; i++) {
    const char *n = Rando_GetLocationName(kDoorTblLocations[i].fork_loc_id);
    uint8 cls = 0;
    if (strstr(n, "- Prize")) cls |= kDoorLoc_Prize;
    if (strstr(n, "- Big Chest")) cls |= kDoorLoc_BigChest;
    if (strstr(n, "Blind's Cell") || strstr(n, "Zelda's Cell")) cls |= kDoorLoc_BkLockedName;
    if (kDoorTblRegions[kDoorTblLocations[i].region].dungeon ==
            kDoorShuffleTblDungeon_ThievesTown &&
        StrSuffix(n, "- Boss"))
      cls |= kDoorLoc_ThievesBoss;
    x->loc_class[i] = cls;
  }

  // Thieves Town fixtures from the event rows (Maiden Rescued = 7 grants at
  // Blind's Cell Interior; Attic Cracked Floor = 9 at the Attic Window;
  // Maiden Unmasked = 8 at Thieves Boss).
  x->maiden_region = x->attic_region = x->thieves_boss_region = 0xFFFF;
  for (int i = 0; i < kDoorTbl_EventCount; i++) {
    const DoorTblEvent *ev = &kDoorTblEvents[i];
    if (ev->event_id == kDoorEvent_MaidenRescued) x->maiden_region = ev->region;
    if (ev->event_id == kDoorEvent_AtticCrackedFloor) x->attic_region = ev->region;
    if (ev->event_id == kDoorEvent_MaidenUnmasked) x->thieves_boss_region = ev->region;
    if (ev->region == 0xFFFF) x->external_events |= 1 << ev->event_id;
  }
  x->blind_cell_region = RegionByName("Thieves Blind's Cell");

  // get_special_big_key_doors (non-standard mode list).
  {
    static const char *const kSpecialBk[] = {
      "Hyrule Dungeon Cellblock Door", "Thieves Blind's Cell Door",
    };
    for (int i = 0; i < 2; i++) {
      uint16 d = DoorByName(kSpecialBk[i]);
      if (d != 0xFFFF)
        x->special_bk_door[x->special_bk_count++] = d;
    }
  }

  // Destination portals (DoorShuffle.link_doors_prep, shuffle == 'vanilla',
  // non-inverted: Desert East, Skull 2 West, TR Lazy Eyes, TR Eye Bridge).
  {
    static const char *const kDestLobbies[] = {
      "Desert East Lobby", "Skull 2 West Lobby", "TR Lazy Eyes", "TR Eye Bridge",
    };
    for (int i = 0; i < 4; i++) {
      uint16 r = RegionByName(kDestLobbies[i]);
      if (r != 0xFFFF)
        x->dest_lobby[x->dest_lobby_count++] = r;
    }
  }

  // Staged origins: portals on walk-inaccessible outside areas (open mode,
  // vanilla entrance shuffle; find_inaccessible_regions): the Desert ledge
  // (West/Back, mutually connected outside) and the Dark Death Mountain
  // two-door ledge (TR Chest, shared with the destination Lazy Eyes door).
  {
    uint16 dw = RegionByName("Desert West Lobby");
    uint16 db = RegionByName("Desert Back Lobby");
    uint16 tc = RegionByName("TR Big Chest Entrance");
    uint16 tl = RegionByName("TR Lazy Eyes");
    if (dw != 0xFFFF && db != 0xFFFF) {
      x->staged[x->staged_count].lobby = dw;
      x->staged[x->staged_count].enabler[0] = dw;
      x->staged[x->staged_count].enabler[1] = db;
      x->staged_count++;
      x->staged[x->staged_count].lobby = db;
      x->staged[x->staged_count].enabler[0] = dw;
      x->staged[x->staged_count].enabler[1] = db;
      x->staged_count++;
    }
    if (tc != 0xFFFF) {
      x->staged[x->staged_count].lobby = tc;
      x->staged[x->staged_count].enabler[0] = tc;
      x->staged[x->staged_count].enabler[1] = tl;
      x->staged_count++;
    }
  }

  // Pool doors per dungeon, ascending id (door ids are name-sorted/frozen).
  {
    uint32 p = 0;
    for (int d = 0; d < kDoorTbl_DungeonCount; d++) {
      x->pool_first[d] = (uint16)p;
      for (int i = 0; i < kDoorTbl_DoorCount; i++) {
        if (kDoorTblDoors[i].dungeon == d && (kDoorTblDoors[i].flags & kDoorTblFlag_InPool))
          x->pool_doors[p++] = (uint16)i;
      }
    }
    x->pool_first[kDoorTbl_DungeonCount] = (uint16)p;
  }

  for (int i = 0; i < kDoorTbl_DropKeyCount; i++)
    x->drop_cnt[kDoorTblDropKeys[i].dungeon]++;

  memset(x->door_edge, 0xFF, sizeof(x->door_edge));
  for (int e = 0; e < kDoorTbl_EdgeCount; e++) {
    uint16 d = kDoorTblEdges[e].door;
    if (d == 0xFFFF)
      continue;
    if (x->door_edge[d][0] == 0xFFFF)
      x->door_edge[d][0] = (uint16)e;
    else if (x->door_edge[d][1] == 0xFFFF)
      x->door_edge[d][1] = (uint16)e;
  }

  x->ready = true;
}

bool Door_IsBkGated(uint16 door) {
  if (kDoorTblDoors[door].flags & kDoorTblFlag_VanillaBigKey)
    return true;
  for (int i = 0; i < g_door_idx.special_bk_count; i++)
    if (g_door_idx.special_bk_door[i] == door)
      return true;
  return false;
}

// ---------------------------------------------------------------------------
// Rule evaluation (kDoorTblRuleBlob bytecode) against an exploration result.
// Vm leaves: spec->vm_pred or lenient-true. Event leaves: granted bit OR
// external (granted outside the dungeons, e.g. Open Floodgate — lenient).
// ---------------------------------------------------------------------------

typedef struct RuleEnv {
  const DoorExploreResult *res;
  bool (*vm_pred)(void *ud, uint16 vm_index);
  void *ud;
} RuleEnv;

// Collect the kDoorRuleOp_Vm indices a rule references (mirror of
// EvalRuleAt's traversal — the blob is preorder, so operands must be skipped
// structurally, never scanned linearly). Feeds the per-dungeon relevant-vm
// masks used by the logic oracle's flood-skip fingerprint.
static void CollectVmAt(int *pos, uint64 mask[2]) {
  uint8 op = kDoorTblRuleBlob[(*pos)++];
  switch (op) {
  case kDoorRuleOp_True:
  case kDoorRuleOp_False:
    return;
  case kDoorRuleOp_And:
  case kDoorRuleOp_Or: {
    int n = kDoorTblRuleBlob[(*pos)++];
    for (int i = 0; i < n; i++)
      CollectVmAt(pos, mask);
    return;
  }
  case kDoorRuleOp_Not:
    CollectVmAt(pos, mask);
    return;
  case kDoorRuleOp_Vm: {
    uint16 idx = kDoorTblRuleBlob[*pos] | (kDoorTblRuleBlob[*pos + 1] << 8);
    *pos += 2;
    if (idx < 128)
      mask[idx >> 6] |= 1ull << (idx & 63);
    return;
  }
  case kDoorRuleOp_Event:
    (*pos)++;
    return;
  case kDoorRuleOp_CReach:
    *pos += 3;
    return;
  case kDoorRuleOp_Reach:
    *pos += 2;
    return;
  default:
    return;  // corrupt blob: nothing to collect
  }
}

static void CollectVm(uint16 rule_off, uint64 mask[2]) {
  if (rule_off == 0xFFFF)
    return;
  int pos = rule_off;
  CollectVmAt(&pos, mask);
}

// Per-dungeon mask of vm-pred indices referenced by ANY rule the dungeon's
// flood or at-location evaluation can touch (edges from its regions, its
// locations, its drop keys, its events; external events conservatively taint
// every dungeon). Lazily built; const tables make it process-stable.
static uint64 g_door_vm_masks[kDoorTbl_DungeonCount][2];
static bool g_door_vm_masks_ready;

void DoorExplore_RelevantVmMask(uint8 dungeon, uint64 out_mask[2]) {
  if (!g_door_vm_masks_ready) {
    memset(g_door_vm_masks, 0, sizeof(g_door_vm_masks));
    uint64 external[2] = { 0, 0 };
    for (int e = 0; e < kDoorTbl_EdgeCount; e++) {
      uint8 d = kDoorTblRegions[kDoorTblEdges[e].from_region].dungeon;
      CollectVm(kDoorTblEdges[e].rule, g_door_vm_masks[d]);
    }
    for (int i = 0; i < kDoorTbl_LocationCount; i++) {
      uint8 d = kDoorTblRegions[kDoorTblLocations[i].region].dungeon;
      CollectVm(kDoorTblLocations[i].rule, g_door_vm_masks[d]);
    }
    for (int i = 0; i < kDoorTbl_DropKeyCount; i++)
      CollectVm(kDoorTblDropKeys[i].rule, g_door_vm_masks[kDoorTblDropKeys[i].dungeon]);
    for (int i = 0; i < kDoorTbl_EventCount; i++) {
      if (kDoorTblEvents[i].region == 0xFFFF)
        CollectVm(kDoorTblEvents[i].rule, external);
      else
        CollectVm(kDoorTblEvents[i].rule,
                  g_door_vm_masks[kDoorTblRegions[kDoorTblEvents[i].region].dungeon]);
    }
    for (int d = 0; d < kDoorTbl_DungeonCount; d++) {
      g_door_vm_masks[d][0] |= external[0];
      g_door_vm_masks[d][1] |= external[1];
    }
    g_door_vm_masks_ready = true;
  }
  out_mask[0] = g_door_vm_masks[dungeon][0];
  out_mask[1] = g_door_vm_masks[dungeon][1];
}

static bool EvalRuleAt(const RuleEnv *env, int *pos) {
  uint8 op = kDoorTblRuleBlob[(*pos)++];
  switch (op) {
  case kDoorRuleOp_True: return true;
  case kDoorRuleOp_False: return false;
  case kDoorRuleOp_And: {
    int n = kDoorTblRuleBlob[(*pos)++];
    bool v = true;
    for (int i = 0; i < n; i++)
      v = EvalRuleAt(env, pos) && v;  // evaluate all children (fixed encoding)
    return v;
  }
  case kDoorRuleOp_Or: {
    int n = kDoorTblRuleBlob[(*pos)++];
    bool v = false;
    for (int i = 0; i < n; i++)
      v = EvalRuleAt(env, pos) || v;
    return v;
  }
  case kDoorRuleOp_Not:
    return !EvalRuleAt(env, pos);
  case kDoorRuleOp_Vm: {
    uint16 idx = kDoorTblRuleBlob[*pos] | (kDoorTblRuleBlob[*pos + 1] << 8);
    *pos += 2;
    return env->vm_pred ? env->vm_pred(env->ud, idx) : true;
  }
  case kDoorRuleOp_Event: {
    uint8 ev = kDoorTblRuleBlob[(*pos)++];
    return ((env->res->events | g_door_idx.external_events) >> ev) & 1;
  }
  case kDoorRuleOp_CReach: {
    uint16 r = kDoorTblRuleBlob[*pos] | (kDoorTblRuleBlob[*pos + 1] << 8);
    uint8 color = kDoorTblRuleBlob[*pos + 2];
    *pos += 3;
    return DoorExplore_ColorAt(env->res, r, color == 0);
  }
  case kDoorRuleOp_Reach: {
    uint16 r = kDoorTblRuleBlob[*pos] | (kDoorTblRuleBlob[*pos + 1] << 8);
    *pos += 2;
    return DoorExplore_Reached(env->res, r);
  }
  default:
    return false;  // corrupt blob: fail closed
  }
}

static bool EvalRule(uint16 off, const DoorExploreResult *res, const DoorExploreSpec *spec) {
  if (off == 0xFFFF)
    return true;
  RuleEnv env = { res, spec->vm_pred, spec->ud };
  int pos = off;
  return EvalRuleAt(&env, &pos);
}

// Public wrapper (logic oracle: at-location rules after the flood).
bool DoorExplore_EvalRule(uint16 rule_off, const DoorExploreResult *r,
                          const DoorExploreGates *gates) {
  if (rule_off == 0xFFFF)
    return true;
  RuleEnv env = { r, gates ? gates->vm_pred : NULL, gates ? gates->ud : NULL };
  int pos = rule_off;
  return EvalRuleAt(&env, &pos);
}

// ---------------------------------------------------------------------------
// Explorer core
// ---------------------------------------------------------------------------

enum { kColor_Blue = 1, kColor_Orange = 2 };

bool Door_EdgePassableFrom(const DoorExploreResult *res, uint16 edge_index) {
  const DoorTblEdge *e = &kDoorTblEdges[edge_index];
  uint16 r = e->from_region;
  uint8 src = (DoorExplore_ColorAt(res, r, 1) ? kColor_Blue : 0) |
              (DoorExplore_ColorAt(res, r, 0) ? kColor_Orange : 0);
  if (!src)
    return false;
  if (e->door != 0xFFFF) {
    const DoorTblDoor *d = &kDoorTblDoors[e->door];
    if (d->flags & kDoorTblFlag_Blocked)
      return false;
    uint16 cf = d->flags & (kDoorTblFlag_CrystalBlue | kDoorTblFlag_CrystalOrange);
    if (cf == kDoorTblFlag_CrystalBlue && !(src & kColor_Blue))
      return false;
    if (cf == kDoorTblFlag_CrystalOrange && !(src & kColor_Orange))
      return false;
  }
  if (e->rule != 0xFFFF) {
    RuleEnv env = { res, NULL, NULL };
    int pos = e->rule;
    if (!EvalRuleAt(&env, &pos))
      return false;
  }
  return true;
}

static int PairIndexOfDoor(const DoorExploreSpec *spec, uint16 door) {
  for (int i = 0; i < spec->pair_count; i++)
    if (spec->pairs[i].a == door || spec->pairs[i].b == door)
      return i;
  return -1;
}

int Door_CountFreeLocs(uint8 dungeon, const DoorExploreResult *res, bool exclude_big_chest) {
  // Reference semantics: a location is "found" when its REGION is visited
  // (location access rules are world logic, not stitcher state).
  bool blind_ok = DoorExplore_Reached(res, g_door_idx.maiden_region) &&
                  DoorExplore_Reached(res, g_door_idx.attic_region);
  int n = 0;
  for (int i = 0; i < kDoorTbl_LocationCount; i++) {
    const DoorTblLocation *l = &kDoorTblLocations[i];
    if (kDoorTblRegions[l->region].dungeon != dungeon)
      continue;
    uint8 cls = g_door_idx.loc_class[i];
    if (cls & kDoorLoc_Prize)
      continue;
    if (exclude_big_chest && (cls & kDoorLoc_BigChest))
      continue;
    if ((cls & kDoorLoc_ThievesBoss) && !blind_ok)
      continue;
    if (DoorExplore_Reached(res, l->region))
      n++;
  }
  return n;
}

bool Door_DropExcludedByActivePot(uint8 dungeon, uint16 drop_index, uint8 pot_tier) {
  if (pot_tier == kPotShuffle_Off || drop_index == 0xFFFF)
    return false;
  for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[i];
    if (p->dungeon == dungeon && p->drop_index == drop_index &&
        (p->flags & kDoorPot_KeyPot) && Rando_DoorPotActive(p, pot_tier))
      return true;
  }
  return false;
}

bool Door_DropExcludedByActiveEnemyDrop(uint8 dungeon, uint16 drop_index,
                                        uint8 enemy_drop_keys) {
  if (!enemy_drop_keys || drop_index == 0xFFFF)
    return false;
  for (uint32 i = 0; i < kRandoDoorEnemyDropLocationsCount; i++) {
    const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[i];
    if (e->dungeon == dungeon && e->drop_index == drop_index)
      return true;
  }
  return false;
}

static bool DoorPotRegionsReached(const RandoDoorPotLocation *p, const DoorExploreResult *res) {
  for (uint8 i = 0; i < p->region_count; i++) {
    uint16 region = kRandoDoorPotRegions[p->region_first + i];
    if (!DoorExplore_Reached(res, region))
      return false;
  }
  return p->region_count != 0;
}

int Door_CountActivePotKeySources(uint8 dungeon, const DoorExploreResult *res,
                                  uint8 pot_tier,
                                  bool (*pred)(void *ud, const RandoDoorPotLocation *pot),
                                  void *ud) {
  if (pot_tier == kPotShuffle_Off)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[i];
    if (p->dungeon != dungeon || !(p->flags & kDoorPot_KeySource) ||
        !Rando_DoorPotActive(p, pot_tier))
      continue;
    if (!DoorPotRegionsReached(p, res))
      continue;
    if (pred != NULL && !pred(ud, p))
      continue;
    n++;
  }
  return n;
}

int Door_CountActiveEnemyDropKeySources(uint8 dungeon, const DoorExploreResult *res,
                                        uint8 enemy_drop_keys,
                                        bool (*pred)(void *ud,
                                                     const RandoDoorEnemyDropLocation *drop),
                                        void *ud) {
  if (!enemy_drop_keys)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorEnemyDropLocationsCount; i++) {
    const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[i];
    if (e->dungeon == dungeon && DoorExplore_Reached(res, e->region) &&
        (pred == NULL || pred(ud, e)))
      n++;
  }
  return n;
}

int Door_CountActiveEnemyDropKeys(uint8 dungeon, uint8 enemy_drop_keys) {
  if (!enemy_drop_keys)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorEnemyDropLocationsCount; i++) {
    const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[i];
    if (e->dungeon == dungeon)
      n++;
  }
  return n;
}

static bool DoorEnemyCheckRegionsReached(const RandoDoorEnemyCheckLocation *e,
                                         const DoorExploreResult *res) {
  for (uint8 i = 0; i < e->region_count; i++) {
    uint16 region = kRandoDoorEnemyCheckRegions[e->region_first + i];
    if (!DoorExplore_Reached(res, region))
      return false;
  }
  return e->region_count != 0;
}

int Door_CountActiveEnemyCheckKeySources(uint8 dungeon, const DoorExploreResult *res,
                                         uint8 enemy_check_tier,
                                         bool (*pred)(void *ud,
                                                      const RandoDoorEnemyCheckLocation *check),
                                         void *ud) {
  if (enemy_check_tier < kEnemyDropChecks_Dungeon)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorEnemyCheckLocationsCount; i++) {
    const RandoDoorEnemyCheckLocation *e = &kRandoDoorEnemyCheckLocations[i];
    if (e->dungeon != dungeon || !Rando_DoorEnemyCheckActive(e, enemy_check_tier))
      continue;
    if (!DoorEnemyCheckRegionsReached(e, res))
      continue;
    if (pred != NULL && !pred(ud, e))
      continue;
    n++;
  }
  return n;
}

int Door_CountActiveEnemyChecks(uint8 dungeon, uint8 enemy_check_tier) {
  if (enemy_check_tier < kEnemyDropChecks_Dungeon)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorEnemyCheckLocationsCount; i++) {
    const RandoDoorEnemyCheckLocation *e = &kRandoDoorEnemyCheckLocations[i];
    if (e->dungeon == dungeon && Rando_DoorEnemyCheckActive(e, enemy_check_tier))
      n++;
  }
  return n;
}

int Door_CountActiveKeyPots(uint8 dungeon, uint8 pot_tier) {
  if (pot_tier == kPotShuffle_Off)
    return 0;
  int n = 0;
  for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[i];
    if (p->dungeon == dungeon && (p->flags & kDoorPot_KeyPot) &&
        Rando_DoorPotActive(p, pot_tier))
      n++;
  }
  return n;
}

int Door_CountDropKeys(uint8 dungeon, const DoorExploreResult *res, uint8 pot_tier,
                       uint8 enemy_drop_keys) {
  int n = 0;
  for (int i = 0; i < kDoorTbl_DropKeyCount; i++)
    if (kDoorTblDropKeys[i].dungeon == dungeon &&
        !Door_DropExcludedByActivePot(dungeon, (uint16)i, pot_tier) &&
        !Door_DropExcludedByActiveEnemyDrop(dungeon, (uint16)i, enemy_drop_keys) &&
        DoorExplore_Reached(res, kDoorTblDropKeys[i].region))
      n++;
  return n;
}

void DoorExplore_Core(const DoorExploreSpec *spec, DoorExploreResult *out) {
  DoorIdx_Init();
  memset(out, 0, sizeof(*out));
  const DoorTblDungeon *dg = &kDoorTblDungeons[spec->dungeon];

  uint8 colors[kDoorTbl_RegionCount];
  memset(colors, 0, sizeof(colors));
  for (int i = 0; i < spec->origin_count; i++)
    colors[spec->origins[i]] = kColor_Orange;  // ExplorationState init = Orange

  bool bk_pass;
  switch (spec->bk_mode) {
  case kDoorBkMode_State: bk_pass = spec->bk_open; break;
  case kDoorBkMode_Held:
    bk_pass = spec->big_key_held ? spec->big_key_held[spec->dungeon] != 0 : true;
    break;
  case kDoorBkMode_LenientGate: bk_pass = false; break;  // re-derived below
  default: bk_pass = true; break;
  }
  out->events = g_door_idx.external_events;

  bool changed = true;
  while (changed) {
    changed = false;
    // Sync the visited bitsets so rule Reach/CReach leaves and the location
    // counters see the colors discovered so far.
    for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
      if (colors[r] & kColor_Blue) out->visited_blue[r >> 3] |= 1 << (r & 7);
      if (colors[r] & kColor_Orange) out->visited_orange[r >> 3] |= 1 << (r & 7);
    }
    // Threshold-mode key budget: chest keys held + reachable drop keys.
    int key_budget = 0;
    if (spec->bk_mode == kDoorBkMode_Held && spec->held_keys)
      key_budget = spec->held_keys[spec->dungeon] +
                   Door_CountDropKeys(spec->dungeon, out, spec->pot_tier,
                                      spec->enemy_drop_keys) +
                   Door_CountActivePotKeySources(spec->dungeon, out,
                                                 spec->pot_tier,
                                                 spec->pot_pred, spec->ud) +
                   Door_CountActiveEnemyDropKeySources(spec->dungeon, out,
                                                       spec->enemy_drop_keys,
                                                       spec->enemy_drop_pred,
                                                       spec->ud) +
                   Door_CountActiveEnemyCheckKeySources(spec->dungeon, out,
                                                        spec->enemy_check_tier,
                                                        spec->enemy_check_pred,
                                                        spec->ud);

    for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
      uint8 src = colors[r];
      if (!src)
        continue;
      for (uint32 ei = g_door_idx.edge_first[r]; ei < g_door_idx.edge_first[r + 1]; ei++) {
        const DoorTblEdge *e = &kDoorTblEdges[g_door_idx.edge_csr[ei]];
        uint16 dest = e->to_region;
        uint8 outc = src;
        if (e->door != 0xFFFF) {
          const DoorTblDoor *d = &kDoorTblDoors[e->door];
          if (d->flags & kDoorTblFlag_Blocked)
            continue;  // can_traverse: blocked doors cannot be exited
          if (Door_IsBkGated(e->door) && !bk_pass)
            continue;
          if (spec->pairs) {
            int pi = PairIndexOfDoor(spec, e->door);
            if (pi >= 0 && !((spec->open_mask >> pi) & 1)) {
              if (spec->bk_mode != kDoorBkMode_Held)
                continue;  // closed key door
              // Threshold gate: passable when holding >= worst-case keys.
              if (!spec->thresholds ||
                  key_budget < spec->thresholds->key_worst_case[spec->dungeon][pi])
                continue;
            }
          }
          uint16 cf = d->flags & (kDoorTblFlag_CrystalBlue | kDoorTblFlag_CrystalOrange);
          if (cf == kDoorTblFlag_CrystalBlue)
            outc = (src & kColor_Blue) ? kColor_Blue : 0;
          else if (cf == kDoorTblFlag_CrystalOrange)
            outc = (src & kColor_Orange) ? kColor_Orange : 0;
          else if (cf == (kDoorTblFlag_CrystalBlue | kDoorTblFlag_CrystalOrange))
            outc = src ? (kColor_Blue | kColor_Orange) : 0;  // crystal switch: Either
          if (!outc)
            continue;
          // Pool stubs: redirect the vanilla connection through the layout.
          if ((d->flags & kDoorTblFlag_InPool) && spec->layout) {
            uint16 p = spec->layout->pairing[e->door];
            if (p != 0xFFFF)
              dest = kDoorTblDoors[p].region;
          }
        }
        if (e->rule != 0xFFFF && !EvalRule(e->rule, out, spec))
          continue;
        if ((colors[dest] | outc) != colors[dest]) {
          colors[dest] |= outc;
          changed = true;
        }
      }
    }

    // Events: granted once their region is reached and their rule holds
    // (perform_event + the flooded-key Reach coupling baked into the rule).
    for (int i = 0; i < kDoorTbl_EventCount; i++) {
      const DoorTblEvent *ev = &kDoorTblEvents[i];
      if (ev->region == 0xFFFF || ((out->events >> ev->event_id) & 1))
        continue;
      if (colors[ev->region] && EvalRule(ev->rule, out, spec)) {
        out->events |= 1 << ev->event_id;
        changed = true;
      }
    }

    // Lenient big-key gate: openable once some free location is in reach
    // (extend_reachable_state_lenient's count_locations_exclude_specials > 0).
    if (spec->bk_mode == kDoorBkMode_LenientGate && !bk_pass) {
      for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
        if (colors[r] & kColor_Blue) out->visited_blue[r >> 3] |= 1 << (r & 7);
        if (colors[r] & kColor_Orange) out->visited_orange[r >> 3] |= 1 << (r & 7);
      }
      if (Door_CountFreeLocs(spec->dungeon, out, true) > 0) {
        bk_pass = true;
        changed = true;
      }
    }
  }

  for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
    if (colors[r] & kColor_Blue) out->visited_blue[r >> 3] |= 1 << (r & 7);
    if (colors[r] & kColor_Orange) out->visited_orange[r >> 3] |= 1 << (r & 7);
  }
}

// ---------------------------------------------------------------------------
// Public explorer API
// ---------------------------------------------------------------------------

bool DoorExplore_Reached(const DoorExploreResult *r, uint16 region) {
  return ((r->visited_blue[region >> 3] | r->visited_orange[region >> 3]) >> (region & 7)) & 1;
}

void DoorExplore_Run(const DoorShuffleLayout *layout, uint8 dungeon,
                     const uint16 *portal_regions, int portal_count,
                     const DoorExploreGates *gates, DoorExploreResult *out) {
  DoorIdx_Init();
  DoorExploreSpec spec;
  memset(&spec, 0, sizeof(spec));
  spec.layout = layout;
  spec.dungeon = dungeon;
  spec.origins = portal_regions;
  spec.origin_count = portal_count;
  spec.bk_mode = kDoorBkMode_Open;
  DoorKeyPair pairs[kDoorShuffle_MaxKeyDoors];
  if (gates) {
    spec.vm_pred = gates->vm_pred;
    spec.pot_pred = gates->pot_pred;
    spec.enemy_drop_pred = gates->enemy_drop_pred;
    spec.enemy_check_pred = gates->enemy_check_pred;
    spec.ud = gates->ud;
    if (gates->key_thresholds) {
      const DoorShuffleLayout *t = gates->key_thresholds;
      spec.pair_count = t->key_door_count[dungeon];
      for (int i = 0; i < spec.pair_count; i++) {
        uint16 a = t->key_doors[dungeon][i];
        pairs[i].a = a;
        pairs[i].b = (kDoorTblDoors[a].type == kDoorTblType_SpiralStairs)
                         ? 0xFFFF : Door_PartnerOf(a, layout);
      }
      spec.pairs = pairs;
      spec.open_mask = 0;
      spec.thresholds = t;
      spec.held_keys = gates->held_keys;
      spec.big_key_held = gates->big_key_held;
      spec.pot_tier = t->pot_tier;
      spec.enemy_drop_keys = t->enemy_drop_keys;
      spec.enemy_check_tier = t->enemy_check_tier;
      spec.bk_mode = kDoorBkMode_Held;
    } else if (gates->big_key_held) {
      spec.big_key_held = gates->big_key_held;
      spec.bk_mode = kDoorBkMode_Held;
    }
  }
  DoorExplore_Core(&spec, out);
}

// ---------------------------------------------------------------------------
// Origins (whole-dungeon stitching with staged enablement)
// ---------------------------------------------------------------------------

static bool RegionInList(uint16 r, const uint16 *list, int n) {
  for (int i = 0; i < n; i++)
    if (list[i] == r)
      return true;
  return false;
}

// All entrance-origin portal rows (the is_drop==0 section includes drop
// arrival regions — they are origins; the is_drop==1 rows only mark which).
static int Door_AllPortalOrigins(uint8 dungeon, uint16 *out) {
  int n = 0;
  for (int i = 0; i < kDoorTbl_PortalCount; i++) {
    if (kDoorTblPortals[i].dungeon == dungeon && !kDoorTblPortals[i].is_drop) {
      if (n >= kDoorGen_MaxOrigins)
        break;  // table outgrew kDoorGen_MaxOrigins: fail closed, don't overflow
      out[n++] = kDoorTblPortals[i].region;
    }
  }
  return n;
}

// Initial origins: all portals minus destination lobbies minus staged
// (inaccessible-outside) lobbies. Mirrors generate_dungeon_find_proposal's
// `excluded` computation for open mode + vanilla entrance shuffle.
static int Door_InitialOrigins(uint8 dungeon, uint16 *out) {
  uint16 all[kDoorGen_MaxOrigins];
  int n = Door_AllPortalOrigins(dungeon, all), m = 0;
  for (int i = 0; i < n; i++) {
    uint16 r = all[i];
    if (RegionInList(r, g_door_idx.dest_lobby, g_door_idx.dest_lobby_count))
      continue;
    bool staged = false;
    for (int s = 0; s < g_door_idx.staged_count; s++)
      staged |= g_door_idx.staged[s].lobby == r;
    if (staged)
      continue;
    out[m++] = r;
  }
  return m;
}

// Lenient staged exploration: explore, enable staged lobbies whose enabler
// region is reached (the player can exit onto the shared ledge and re-enter),
// re-explore until stable. Returns the final origin set.
static void Door_ExploreStaged(const DoorShuffleLayout *layout, uint8 dungeon,
                               uint16 *origins, int *origin_count,
                               DoorExploreResult *out) {
  for (;;) {
    DoorExploreSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.layout = layout;
    spec.dungeon = dungeon;
    spec.origins = origins;
    spec.origin_count = *origin_count;
    spec.bk_mode = kDoorBkMode_LenientGate;
    DoorExplore_Core(&spec, out);
    bool grew = false;
    for (int s = 0; s < g_door_idx.staged_count; s++) {
      uint16 lobby = g_door_idx.staged[s].lobby;
      if (kDoorTblRegions[lobby].dungeon != dungeon)
        continue;
      if (RegionInList(lobby, origins, *origin_count))
        continue;
      for (int k = 0; k < 2; k++) {
        uint16 en = g_door_idx.staged[s].enabler[k];
        if (en != 0xFFFF && DoorExplore_Reached(out, en)) {
          if (*origin_count >= kDoorGen_MaxOrigins)
            break;  // origin array full: fail closed (skip), don't overflow
          origins[(*origin_count)++] = lobby;
          grew = true;
          break;
        }
      }
    }
    if (!grew)
      return;
  }
}

// ---------------------------------------------------------------------------
// Stitcher (generate_dungeon_find_proposal port)
// ---------------------------------------------------------------------------

// Hook buckets in reference order; hook_from_door: spiral -> Stairs, normal ->
// its direction. type_map pairs North<->South, West<->East.
//
// DEVIATION from the reference's single Stairs<->Stairs bucket: spirals pair
// strictly Up<->Down. The reference allows Up<->Up, but the engine's spiral
// walk-out machinery (Dungeon_SyncBackgroundsFromSpiralStairs offsets, the
// dung_cur_floor +-1, SpiralStairs_MakeNearbyWallsLowPriority — all keyed on
// the SOURCE which_staircase_index bit 2) assumes the arrival tile is the
// opposite-direction stair, which is true for every vanilla pair. Enforcing
// Up<->Down keeps that geometry contract; per-dungeon pools are balanced
// (the pool is built from vanilla Up<->Down pairs), so a perfect matching
// always exists.
enum { kHook_North, kHook_South, kHook_West, kHook_East,
       kHook_StairsUp, kHook_StairsDown, kHook_COUNT };
static const uint8 kOppositeHook[kHook_COUNT] = {
  kHook_South, kHook_North, kHook_East, kHook_West,
  kHook_StairsDown, kHook_StairsUp,
};

static uint8 HookOfDoor(uint16 door) {
  const DoorTblDoor *d = &kDoorTblDoors[door];
  if (d->type == kDoorTblType_SpiralStairs)
    return d->direction == kDoorTblDir_Up ? kHook_StairsUp : kHook_StairsDown;
  switch (d->direction) {
  case kDoorTblDir_North: return kHook_North;
  case kDoorTblDir_South: return kHook_South;
  case kDoorTblDir_West: return kHook_West;
  default: return kHook_East;
  }
}

typedef struct Bucket {
  uint16 doors[kDoorGen_MaxPool];
  int n;
} Bucket;

static void BucketRemove(Bucket *b, uint16 door) {
  for (int i = 0; i < b->n; i++) {
    if (b->doors[i] == door) {
      memmove(&b->doors[i], &b->doors[i + 1], (b->n - i - 1) * sizeof(uint16));
      b->n--;
      return;
    }
  }
}

// create_random_proposal: draw hanger/hook pairs until the buckets drain.
// Coupled mode, no self-loops (DR defaults decoupledoors=False,
// door_self_loops=False). Buckets are built in door-id order.
static bool Door_CreateRandomProposal(uint8 dungeon, RandoRng *rng,
                                      DoorShuffleLayout *layout) {
  Bucket primary[kHook_COUNT], secondary[kHook_COUNT];
  memset(primary, 0, sizeof(primary));
  memset(secondary, 0, sizeof(secondary));
  for (uint32 i = g_door_idx.pool_first[dungeon]; i < g_door_idx.pool_first[dungeon + 1]; i++) {
    uint16 door = g_door_idx.pool_doors[i];
    uint8 h = HookOfDoor(door);
    primary[h].doors[primary[h].n++] = door;
    secondary[h].doors[secondary[h].n++] = door;
  }
  for (;;) {
    uint8 hooks_left[kHook_COUNT];
    int nh = 0, left = 0;
    for (int h = 0; h < kHook_COUNT; h++) {
      if (primary[h].n > 0) {
        hooks_left[nh++] = (uint8)h;
        left += primary[h].n;
      }
    }
    if (left == 0)
      return true;
    uint8 hook = hooks_left[Rng_NextRange(rng, nh)];
    uint16 a = primary[hook].doors[Rng_NextRange(rng, primary[hook].n)];
    uint8 opp = kOppositeHook[hook];
    uint16 b;
    int guard = 0;
    do {
      if (secondary[opp].n == 0 || (secondary[opp].n == 1 && secondary[opp].doors[0] == a))
        return false;  // odd parity: cannot happen with table-balanced hooks
      b = secondary[opp].doors[Rng_NextRange(rng, secondary[opp].n)];
      if (++guard > 4096)
        return false;
    } while (b == a);
    layout->pairing[a] = b;
    layout->pairing[b] = a;
    BucketRemove(&primary[hook], a);
    BucketRemove(&secondary[opp], b);
    BucketRemove(&primary[opp], b);
    BucketRemove(&secondary[hook], a);
  }
}

// proposal_hash: FNV-1a 64 over partner ids in pool order.
static uint64 Door_ProposalHash(uint8 dungeon, const DoorShuffleLayout *layout) {
  uint64 h = 0xcbf29ce484222325ull;
  for (uint32 i = g_door_idx.pool_first[dungeon]; i < g_door_idx.pool_first[dungeon + 1]; i++) {
    uint16 v = layout->pairing[g_door_idx.pool_doors[i]];
    h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
    h = (h ^ (v >> 8)) * 0x100000001b3ull;
  }
  return h;
}

#define kHashSetSize 4096  // >= 2x the 1001 max distinct proposals per stitch
typedef struct HashSet {
  uint64 v[kHashSetSize];
  int n;
} HashSet;

static bool HashSetContains(const HashSet *s, uint64 h) {
  uint32 i = (uint32)(h ^ (h >> 32)) & (kHashSetSize - 1);
  while (s->v[i]) {
    if (s->v[i] == h)
      return true;
    i = (i + 1) & (kHashSetSize - 1);
  }
  return false;
}

static void HashSetAdd(HashSet *s, uint64 h) {
  if (h == 0)
    h = 1;  // 0 is the empty sentinel; remap (negligible collision)
  if (s->n >= kHashSetSize / 2)
    return;  // saturated: degrade to permissive (iteration caps end the loop)
  uint32 i = (uint32)(h ^ (h >> 32)) & (kHashSetSize - 1);
  while (s->v[i]) {
    if (s->v[i] == h)
      return;
    i = (i + 1) & (kHashSetSize - 1);
  }
  s->v[i] = h;
  s->n++;
}

// modify_proposal: up to 10 local swap repairs, then a full reshuffle.
// Candidate lists are door-id ordered (the reference defensively sorts the
// unvisited bucket by name; door ids are name-sorted so id order == name
// order).
static bool Door_ModifyProposal(uint8 dungeon, RandoRng *rng, DoorShuffleLayout *layout,
                                const DoorExploreResult *explored, const HashSet *seen,
                                uint64 *hash_out) {
  for (int itr = 0;; itr++) {
    if (itr > 10) {
      if (!Door_CreateRandomProposal(dungeon, rng, layout))
        return false;
      *hash_out = Door_ProposalHash(dungeon, layout);
      return true;
    }
    // visited = the door's region was visited (any color); the reference adds
    // every pool door of a visited region to visited_doors.
    uint16 visited[kDoorGen_MaxPool];
    Bucket unvisited[kHook_COUNT];
    memset(unvisited, 0, sizeof(unvisited));
    int nv = 0, nunv = 0;
    for (uint32 i = g_door_idx.pool_first[dungeon]; i < g_door_idx.pool_first[dungeon + 1]; i++) {
      uint16 door = g_door_idx.pool_doors[i];
      if (DoorExplore_Reached(explored, kDoorTblDoors[door].region)) {
        visited[nv++] = door;
      } else {
        uint8 h = HookOfDoor(door);
        unvisited[h].doors[unvisited[h].n++] = door;
        nunv++;
      }
    }
    if (nunv == 0) {
      // connectivity is fine but something else failed: full reshuffle
      if (!Door_CreateRandomProposal(dungeon, rng, layout))
        return false;
      *hash_out = Door_ProposalHash(dungeon, layout);
      return true;
    }
    uint16 attempt = 0xFFFF;
    uint8 opp = 0;
    int swaps_n = nv;
    uint16 swaps[kDoorGen_MaxPool];
    memcpy(swaps, visited, nv * sizeof(uint16));
    while (swaps_n > 0) {
      int k = Rng_NextRange(rng, swaps_n);
      uint16 cand = swaps[k];
      memmove(&swaps[k], &swaps[k + 1], (swaps_n - k - 1) * sizeof(uint16));
      swaps_n--;
      opp = kOppositeHook[HookOfDoor(cand)];
      if (unvisited[opp].n > 0) {
        attempt = cand;
        break;
      }
    }
    if (attempt == 0xFFFF)
      continue;
    uint16 new_door = unvisited[opp].doors[Rng_NextRange(rng, unvisited[opp].n)];
    uint16 old_target = layout->pairing[attempt];
    uint16 old_attempt = layout->pairing[new_door];
    // Re-link algebra from the reference (coupled mode).
    if (attempt == old_target && old_attempt == new_door) {
      old_attempt = new_door;
      old_target = attempt;
    } else if (attempt == old_target) {
      old_target = old_attempt;
    } else if (old_attempt == new_door) {
      old_attempt = old_target;
    }
    layout->pairing[attempt] = new_door;
    layout->pairing[old_attempt] = old_target;
    layout->pairing[old_target] = old_attempt;
    layout->pairing[new_door] = attempt;
    uint64 h = Door_ProposalHash(dungeon, layout);
    if (!HashSetContains(seen, h)) {
      *hash_out = h;
      return true;
    }
  }
}

// Required paths (determine_paths_for_dungeon + valid_paths). The kind-0
// boss/attic rows assert reachability from the origins (subsumed by the
// all-regions check but kept for fidelity); kind-1 rows assert each drop
// region can reach SOME non-hole portal; the Thieves Blind path asserts the
// maiden can be walked from her cell to the boss.
static bool Door_PathFromReaches(const DoorShuffleLayout *layout, uint8 dungeon,
                                 uint16 start, const uint16 *targets, int target_count) {
  DoorExploreSpec spec;
  DoorExploreResult res;
  memset(&spec, 0, sizeof(spec));
  spec.layout = layout;
  spec.dungeon = dungeon;
  spec.origins = &start;
  spec.origin_count = 1;
  spec.bk_mode = kDoorBkMode_LenientGate;
  DoorExplore_Core(&spec, &res);
  for (int i = 0; i < target_count; i++)
    if (DoorExplore_Reached(&res, targets[i]))
      return true;
  return false;
}

static bool Door_CheckValid(const DoorShuffleLayout *layout, uint8 dungeon,
                            const DoorExploreResult *staged) {
  const DoorTblDungeon *dg = &kDoorTblDungeons[dungeon];
  // 1. all non-exempt regions visited in either crystal state (check_valid).
  for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
    if ((g_door_idx.region_exempt[r >> 3] >> (r & 7)) & 1)
      continue;
    if (!DoorExplore_Reached(staged, (uint16)r))
      return false;
  }
  // 2. kind-0 paths from origins.
  for (int i = 0; i < kDoorTbl_PathCount; i++) {
    if (kDoorTblPaths[i].dungeon != dungeon || kDoorTblPaths[i].kind != 0)
      continue;
    if (!DoorExplore_Reached(staged, kDoorTblPaths[i].region))
      return false;
  }
  // 2b. 'Thieves Boss' (appended separately from boss_path_checks in
  // determine_paths_for_dungeon; the harvested path table only carries the
  // boss_path_checks list).
  if (dungeon == kDoorShuffleTblDungeon_ThievesTown &&
      g_door_idx.thieves_boss_region != 0xFFFF &&
      !DoorExplore_Reached(staged, g_door_idx.thieves_boss_region))
    return false;
  // 2c. Blind's maiden escort: ('Thieves Blind's Cell', 'Thieves Boss') —
  // the fork pins Blind in Thieves Town (boss shuffle), so always required.
  // Guard the boss region too (mirrors 2b): an unresolved 0xFFFF target would
  // index visited_blue[0xFFFF>>3] OOB inside DoorExplore_Reached.
  if (dungeon == kDoorShuffleTblDungeon_ThievesTown &&
      g_door_idx.blind_cell_region != 0xFFFF &&
      g_door_idx.thieves_boss_region != 0xFFFF) {
    if (!Door_PathFromReaches(layout, dungeon, g_door_idx.blind_cell_region,
                              &g_door_idx.thieves_boss_region, 1))
      return false;
  }
  // 3. kind-1 drop-exit paths: drop region -> any non-hole portal.
  uint16 nonhole[kDoorGen_MaxOrigins];
  int nnh = 0;
  for (int i = 0; i < kDoorTbl_PortalCount; i++) {
    if (kDoorTblPortals[i].dungeon != dungeon || kDoorTblPortals[i].is_drop)
      continue;
    uint16 r = kDoorTblPortals[i].region;
    bool is_drop_region = false;
    for (int j = 0; j < kDoorTbl_PortalCount; j++)
      if (kDoorTblPortals[j].dungeon == dungeon && kDoorTblPortals[j].is_drop &&
          kDoorTblPortals[j].region == r)
        is_drop_region = true;
    if (!is_drop_region)
      nonhole[nnh++] = r;
  }
  for (int i = 0; i < kDoorTbl_PathCount; i++) {
    if (kDoorTblPaths[i].dungeon != dungeon || kDoorTblPaths[i].kind != 1)
      continue;
    if (!Door_PathFromReaches(layout, dungeon, kDoorTblPaths[i].region, nonhole, nnh))
      return false;
  }
  return true;
}

static HashSet g_stitch_seen;  // single-threaded generation scratch

// generate_dungeon_find_proposal: 1000-iteration cap, proposal-hash dedup,
// modify-or-reshuffle repair loop. Returns the final staged origins for the
// key shuffle.
static bool Door_StitchDungeon(uint8 dungeon, RandoRng *rng, DoorShuffleLayout *layout,
                               uint16 *origins, int *origin_count) {
  // Clear any previous pairing for this dungeon's pool.
  for (uint32 i = g_door_idx.pool_first[dungeon]; i < g_door_idx.pool_first[dungeon + 1]; i++)
    layout->pairing[g_door_idx.pool_doors[i]] = 0xFFFF;
  if (g_door_idx.pool_first[dungeon] == g_door_idx.pool_first[dungeon + 1]) {
    *origin_count = Door_InitialOrigins(dungeon, origins);
    return true;  // nothing to stitch
  }

  HashSet *seen = &g_stitch_seen;
  memset(seen, 0, sizeof(*seen));
  if (!Door_CreateRandomProposal(dungeon, rng, layout))
    return false;
  uint64 h = Door_ProposalHash(dungeon, layout);
  for (int itr = 0;; itr++) {
    if (itr > 1000)
      return false;  // honest cap exhaustion ("Generation taking too long")
    if (HashSetContains(seen, h)) {
      if (!Door_CreateRandomProposal(dungeon, rng, layout))
        return false;
      h = Door_ProposalHash(dungeon, layout);
    }
    if (!HashSetContains(seen, h)) {
      HashSetAdd(seen, h);
      *origin_count = Door_InitialOrigins(dungeon, origins);
      DoorExploreResult staged;
      Door_ExploreStaged(layout, dungeon, origins, origin_count, &staged);
      if (Door_CheckValid(layout, dungeon, &staged))
        return true;
      if (!Door_ModifyProposal(dungeon, rng, layout, &staged, seen, &h))
        return false;
    }
  }
}

// ---------------------------------------------------------------------------
// Generation entry + digest
// ---------------------------------------------------------------------------

// One RNG stream per (seed, attempt, dungeon): SplitMix64-expanded
// xoshiro256** seeded with
//   seed ^ 0xD0025EEDD0025EED                      (door-shuffle domain salt)
//        ^ attempt * 0x9E3779B97F4A7C15            (golden-ratio odd constant)
//        ^ (dungeon+1) * 0xBF58476D1CE4E5B9        (SplitMix64 mix constant)
// The stitcher consumes the stream first, the key shuffle continues it.
static uint64 DoorSeedSalt(uint64 seed, uint32 attempt, uint8 dungeon) {
  uint64 x = seed ^ 0xD0025EEDD0025EEDull;
  x ^= (uint64)attempt * 0x9E3779B97F4A7C15ull;
  x ^= (uint64)(dungeon + 1) * 0xBF58476D1CE4E5B9ull;
  return x;
}

static uint8 g_door_fail_stage;  // selftest diagnostics: 0 ok, 1 stitch, 2 keys, 3 ring stock

static bool Door_KeyThresholdsFitRingStock(const DoorShuffleLayout *layout,
                                           uint8 dungeon) {
  if (layout == NULL || dungeon >= kRandoDungeon_Count) return false;
  uint8 stock = Rando_KeyRingGrantCount(dungeon);
  for (uint8 i = 0; i < layout->key_door_count[dungeon]; i++) {
    if (layout->key_worst_case[dungeon][i] > stock) return false;
  }
  return true;
}

bool DoorShuffle_Generate(uint64 seed, uint32 attempt, uint16 dungeon_mask,
                          uint8 pot_tier, uint8 enemy_drop_keys,
                          uint8 enemy_check_tier, DoorShuffleLayout *out) {
  DoorIdx_Init();
  memset(out->pairing, 0xFF, sizeof(out->pairing));
  memset(out->key_door_count, 0, sizeof(out->key_door_count));
  memset(out->key_doors, 0xFF, sizeof(out->key_doors));
  memset(out->key_worst_case, 0, sizeof(out->key_worst_case));
  out->bk_restricted_count = 0;
  out->shuffled_mask = 0;
  out->pot_tier = pot_tier;
  out->pot_bridge_digest = pot_tier ? kRandoDoorPotBridgeDigest : 0;
  out->enemy_drop_keys = enemy_drop_keys ? 1 : 0;
  out->enemy_drop_bridge_digest = out->enemy_drop_keys ? kRandoDoorEnemyDropBridgeDigest : 0;
  out->enemy_check_tier = enemy_check_tier;
  out->enemy_check_bridge_digest =
      enemy_check_tier >= kEnemyDropChecks_Dungeon ? kRandoDoorEnemyCheckBridgeDigest : 0;
  g_door_fail_stage = 0;
  for (uint8 d = 0; d < kDoorTbl_DungeonCount; d++) {
    if (!((dungeon_mask >> d) & 1))
      continue;
    RandoRng rng;
    Rng_SeedFromU64(&rng, DoorSeedSalt(seed, attempt, d));
    uint16 origins[kDoorGen_MaxOrigins];
    int norigins = 0;
    if (!Door_StitchDungeon(d, &rng, out, origins, &norigins)) {
      g_door_fail_stage = 1;
      return false;
    }
    if (!DoorKeys_ShuffleDungeon(d, &rng, origins, norigins, out)) {
      g_door_fail_stage = 2;
      return false;
    }
    // A ring's runtime max-write must be sufficient for every accepted layout
    // threshold. Fail this attempt closed if future door authoring exceeds the
    // centralized full stock; --door-selftest exercises the same guard broadly.
    if (!Door_KeyThresholdsFitRingStock(out, d)) {
      g_door_fail_stage = 3;
      return false;
    }
    out->shuffled_mask |= 1 << d;
  }
  return true;
}

uint32 DoorShuffle_LayoutDigest(const DoorShuffleLayout *l) {
  uint64 h = 0xcbf29ce484222325ull;
#define DIG8(b) (h = (h ^ (uint8)(b)) * 0x100000001b3ull)
#define DIG16(w) (DIG8((w) & 0xFF), DIG8((w) >> 8))
#define DIG32(w) (DIG16((w) & 0xFFFF), DIG16((w) >> 16))
  DIG16(l->shuffled_mask);
  DIG8(l->pot_tier);
  DIG32(l->pot_bridge_digest);
  DIG8(l->enemy_drop_keys);
  DIG32(l->enemy_drop_bridge_digest);
  DIG8(l->enemy_check_tier);
  DIG32(l->enemy_check_bridge_digest);
  for (int i = 0; i < kDoorTbl_DoorCount; i++) {
    if (l->pairing[i] != 0xFFFF) {
      DIG16((uint16)i);
      DIG16(l->pairing[i]);
    }
  }
  for (int d = 0; d < kDoorTbl_DungeonCount; d++) {
    DIG8(l->key_door_count[d]);
    for (int i = 0; i < l->key_door_count[d]; i++) {
      DIG16(l->key_doors[d][i]);
      DIG8(l->key_worst_case[d][i]);
    }
  }
  DIG8(l->bk_restricted_count);
  for (int i = 0; i < l->bk_restricted_count; i++)
    DIG16(l->bk_restricted[i]);
#undef DIG8
#undef DIG16
#undef DIG32
  return (uint32)(h ^ (h >> 32));
}

// ---------------------------------------------------------------------------
// Self-test (--door-selftest)
// ---------------------------------------------------------------------------

// Greedy completability sim mirroring the prover's state machine: keys-only
// constrained (lenient items), one openable door per round (lowest index;
// the big-key door only via an available big location). The prover certifies
// EVERY order is softlock-free, so this single greedy order must reach the
// terminal state (no reachable closed door) with the boss paths reached.
static bool SelfTest_GreedyComplete(const DoorShuffleLayout *l, uint8 dungeon,
                                    const uint16 *origins, int norigins) {
  DoorKeyPair pairs[kDoorShuffle_MaxKeyDoors];
  int np = l->key_door_count[dungeon];
  for (int i = 0; i < np; i++) {
    uint16 a = l->key_doors[dungeon][i];
    pairs[i].a = a;
    pairs[i].b = (kDoorTblDoors[a].type == kDoorTblType_SpiralStairs)
                     ? 0xFFFF : Door_PartnerOf(a, l);
  }
  int max_small_sources = kDoorTblDungeons[dungeon].chest_small_keys +
                          Door_CountActiveKeyPots(dungeon, l->pot_tier) +
                          Door_CountActiveEnemyDropKeys(dungeon, l->enemy_drop_keys) +
                          Door_CountActiveEnemyChecks(dungeon, l->enemy_check_tier);
  uint32 mask = 0;
  bool bk = false;
  int used_locations = 0;
  for (int step = 0; step <= np + 3; step++) {
    DoorExploreSpec spec;
    DoorExploreResult res;
    memset(&spec, 0, sizeof(spec));
    spec.layout = l;
    spec.dungeon = dungeon;
    spec.origins = origins;
    spec.origin_count = norigins;
    spec.pairs = pairs;
    spec.pair_count = np;
    spec.open_mask = mask;
    spec.bk_mode = kDoorBkMode_State;
    spec.bk_open = bk;
    spec.pot_tier = l->pot_tier;
    spec.enemy_drop_keys = l->enemy_drop_keys;
    spec.enemy_check_tier = l->enemy_check_tier;
    DoorExplore_Core(&spec, &res);
    // closed-but-reachable children
    uint32 child = 0;
    for (int i = 0; i < np; i++) {
      if ((mask >> i) & 1)
        continue;
      uint16 dd[2] = { pairs[i].a, pairs[i].b };
      for (int k = 0; k < 2 && !((child >> i) & 1); k++) {
        if (dd[k] == 0xFFFF)
          continue;
        for (int j = 0; j < 2; j++) {
          uint16 e = g_door_idx.door_edge[dd[k]][j];
          if (e != 0xFFFF && Door_EdgePassableFrom(&res, e)) {
            child |= 1u << i;
            break;
          }
        }
      }
    }
    bool bk_child = false;
    if (!bk) {
      for (int door = 0; door < kDoorTbl_DoorCount && !bk_child; door++) {
        if (kDoorTblDoors[door].dungeon != dungeon || !Door_IsBkGated((uint16)door))
          continue;
        for (int j = 0; j < 2; j++) {
          uint16 e = g_door_idx.door_edge[door][j];
          if (e != 0xFFFF && Door_EdgePassableFrom(&res, e)) {
            bk_child = true;
            break;
          }
        }
      }
    }
    if (!child && !bk_child) {
      // terminal: nothing left to open — boss paths must be reached
      for (int i = 0; i < kDoorTbl_PathCount; i++) {
        if (kDoorTblPaths[i].dungeon == dungeon && kDoorTblPaths[i].kind == 0 &&
            !DoorExplore_Reached(&res, kDoorTblPaths[i].region))
          return false;
      }
      return true;
    }
    int used = 0;
    for (int i = 0; i < np; i++)
      used += (mask >> i) & 1;
    int ttl = Door_CountFreeLocs(dungeon, &res, !bk);
    int key_only = Door_CountDropKeys(dungeon, &res, l->pot_tier, l->enemy_drop_keys);
    int pot_free = Door_CountActivePotKeySources(dungeon, &res, l->pot_tier,
                                                 NULL, NULL);
    int enemy_drop_free = Door_CountActiveEnemyDropKeySources(dungeon, &res,
                                                              l->enemy_drop_keys,
                                                              NULL, NULL);
    int enemy_check_free = Door_CountActiveEnemyCheckKeySources(
        dungeon, &res, l->enemy_check_tier, NULL, NULL);
    int chest = ttl - (bk ? 1 : 0);
    chest += pot_free + enemy_drop_free + enemy_check_free;
    if (chest > max_small_sources)
      chest = max_small_sources;
    int avail_small = chest + key_only - used;
    int avail_big = bk ? 0 : ttl - used_locations;
    if (child && avail_small > 0) {
      for (int i = 0; i < np; i++) {
        if ((child >> i) & 1) {
          mask |= 1u << i;
          if (used + 1 > key_only)
            used_locations++;
          break;
        }
      }
    } else if (bk_child && avail_big > 0) {
      bk = true;
      used_locations++;
    } else {
      return false;  // stuck: closed doors remain but nothing affordable
    }
  }
  return false;
}

int DoorShuffle_SelfTest(void) {
  DoorIdx_Init();
  enum { kSeeds = 20, kMaxAttempts = 16 };
  int total_fail = 0, total_pairs = 0;
  bool hard_fail = false;

  if (DoorRt_GeometrySelfCheck() != 0) {
    hard_fail = true;
  } else {
    printf("door-selftest: runtime geometry decode ok\n");
  }

  for (uint8 d = 0; d < kDoorTbl_DungeonCount; d++) {
    const char *name = DoorIdx_Name(kDoorTblDungeons[d].name_off);
    // (f) vanilla sanity: layout=NULL, all portals, lenient, full coverage.
    {
      uint16 origins[kDoorGen_MaxOrigins];
      int n = Door_AllPortalOrigins(d, origins);
      DoorExploreResult res;
      DoorExplore_Run(NULL, d, origins, n, NULL, &res);
      const DoorTblDungeon *dg = &kDoorTblDungeons[d];
      for (int r = dg->region_first; r < dg->region_first + dg->region_count; r++) {
        if ((g_door_idx.region_exempt[r >> 3] >> (r & 7)) & 1)
          continue;
        if (!DoorExplore_Reached(&res, (uint16)r)) {
          fprintf(stderr, "door-selftest: %s vanilla unreachable region '%s'\n",
                  name, DoorIdx_Name(kDoorTblRegions[r].name_off));
          hard_fail = true;
        }
      }
    }

    int fail = 0, retries = 0, key_doors_sum = 0, bk_restricted_sum = 0;
    static DoorShuffleLayout layout, layout2;
    for (int seed = 0; seed < kSeeds; seed++) {
      uint64 s = 0xD00D5EED00000000ull + (uint64)seed * 7919;
      bool got = false;
      uint32 att = 0;
      for (; att < kMaxAttempts; att++) {
        if (DoorShuffle_Generate(s, att, (uint16)(1u << d), kPotShuffle_Off, 0,
                                 kEnemyDropChecks_Off,
                                 &layout)) {
          got = true;
          break;
        }
      }
      total_pairs++;
      if (!got) {
        fail++;
        total_fail++;
        fprintf(stderr, "door-selftest: %s seed %d: cap-exhausted after %d attempts (stage %d)\n",
                name, seed, (int)kMaxAttempts, g_door_fail_stage);
        continue;
      }
      retries += (int)att;
      key_doors_sum += layout.key_door_count[d];
      bk_restricted_sum += layout.bk_restricted_count;

      // (b)+(c) connectivity + paths under the final layout.
      uint16 origins[kDoorGen_MaxOrigins];
      int norigins = Door_InitialOrigins(d, origins);
      DoorExploreResult staged;
      Door_ExploreStaged(&layout, d, origins, &norigins, &staged);
      if (!Door_CheckValid(&layout, d, &staged)) {
        fprintf(stderr, "door-selftest: %s seed %d: accepted layout fails recheck\n", name, seed);
        hard_fail = true;
      }
      // (d) determinism: regenerate at the same (seed, attempt).
      if (!DoorShuffle_Generate(s, att, (uint16)(1u << d), kPotShuffle_Off, 0,
                                kEnemyDropChecks_Off,
                                &layout2) ||
          DoorShuffle_LayoutDigest(&layout) != DoorShuffle_LayoutDigest(&layout2)) {
        fprintf(stderr, "door-selftest: %s seed %d: regeneration digest mismatch\n", name, seed);
        hard_fail = true;
      }
      // (e) greedy key-collection completability.
      if (!SelfTest_GreedyComplete(&layout, d, origins, norigins)) {
        fprintf(stderr, "door-selftest: %s seed %d: greedy key sim cannot complete\n", name, seed);
        hard_fail = true;
      }
      // (g) Stage-1b kind overlay: every chosen key half keyed at slot<4,
      // pos_byte multisets preserved, abandoned vanilla key doors un-keyed,
      // blocked/pinned doors untouched.
      if (DoorRt_KindOverlaySelfCheck(&layout) != 0) {
        fprintf(stderr, "door-selftest: %s seed %d: kind-overlay selfcheck failed\n", name, seed);
        hard_fail = true;
      }
    }
    int ok = kSeeds - fail;
    printf("door-selftest: %-18s ok %2d/%2d  retries %2d  keydoors avg %.1f  bk_restr avg %.1f\n",
           name, ok, (int)kSeeds, retries,
           ok ? (double)key_doors_sum / ok : 0.0,
           ok ? (double)bk_restricted_sum / ok : 0.0);
  }

  // Whole-seed generation across the MVP dungeon set: cross-dungeon
  // accumulation (bk_restricted append, shuffled_mask) + digest determinism.
  {
    static DoorShuffleLayout full, full2;
    for (int seed = 0; seed < 5; seed++) {
      uint64 s = 0xF0F0D00Dull + (uint64)seed * 104729;
      uint32 att = 0;
      bool got = false;
      for (; att < kMaxAttempts; att++) {
        if (DoorShuffle_Generate(s, att, kDoorShuffle_MvpDungeonMask, kPotShuffle_Off,
                                 0, kEnemyDropChecks_Off, &full)) {
          got = true;
          break;
        }
      }
      if (!got) {
        fprintf(stderr, "door-selftest: full-mask seed %d cap-exhausted\n", seed);
        hard_fail = true;
        continue;
      }
      if (!DoorShuffle_Generate(s, att, kDoorShuffle_MvpDungeonMask, kPotShuffle_Off,
                                0, kEnemyDropChecks_Off, &full2) ||
          DoorShuffle_LayoutDigest(&full) != DoorShuffle_LayoutDigest(&full2)) {
        fprintf(stderr, "door-selftest: full-mask seed %d digest mismatch\n", seed);
        hard_fail = true;
      }
      if (full.shuffled_mask != kDoorShuffle_MvpDungeonMask) {
        fprintf(stderr, "door-selftest: full-mask seed %d shuffled_mask %x\n",
                seed, full.shuffled_mask);
        hard_fail = true;
      }
      if (DoorRt_KindOverlaySelfCheck(&full) != 0) {
        fprintf(stderr, "door-selftest: full-mask seed %d kind-overlay selfcheck failed\n", seed);
        hard_fail = true;
      }
    }
    // Digest printed so CI logs cross-check byte-identity across platforms.
    printf("door-selftest: full-mask (0x%x) x5 ok, last bk_restricted %d, digest %08x\n",
           kDoorShuffle_MvpDungeonMask, full.bk_restricted_count,
           DoorShuffle_LayoutDigest(&full));
  }

  // Door x pot-shuffle composition: same full-mask path, but with active pot
  // tiers so the prover sees pot key sources, excluded pot drops, empty-pot
  // tiering, and the bridge digest. This is intentionally compact; the
  // per-dungeon kPotShuffle_Off loop above already carries the broad seed sweep.
  {
    static const uint8 kPotSelftestTiers[] = {
      kPotShuffle_Keys, kPotShuffle_Contents, kPotShuffle_All,
    };
    static DoorShuffleLayout full, full2;
    for (uint32 ti = 0; ti < sizeof(kPotSelftestTiers); ti++) {
      uint8 tier = kPotSelftestTiers[ti];
      int ok_count = 0;
      for (int seed = 0; seed < 3; seed++) {
        uint64 s = 0x504F54444F4F5200ull + ((uint64)tier << 8) + (uint64)seed * 8191;
        uint32 att = 0;
        bool got = false;
        for (; att < kMaxAttempts; att++) {
          if (DoorShuffle_Generate(s, att, kDoorShuffle_MvpDungeonMask, tier,
                                   0, kEnemyDropChecks_Off, &full)) {
            got = true;
            break;
          }
        }
        if (!got) {
          fprintf(stderr, "door-selftest: door+pot tier %u seed %d cap-exhausted\n",
                  (unsigned)tier, seed);
          hard_fail = true;
          continue;
        }
        if (full.pot_tier != tier ||
            full.pot_bridge_digest != kRandoDoorPotBridgeDigest) {
          fprintf(stderr, "door-selftest: door+pot tier %u bridge identity mismatch\n",
                  (unsigned)tier);
          hard_fail = true;
        }
        if (!DoorShuffle_Generate(s, att, kDoorShuffle_MvpDungeonMask, tier,
                                  0, kEnemyDropChecks_Off, &full2) ||
            DoorShuffle_LayoutDigest(&full) != DoorShuffle_LayoutDigest(&full2)) {
          fprintf(stderr, "door-selftest: door+pot tier %u seed %d digest mismatch\n",
                  (unsigned)tier, seed);
          hard_fail = true;
        }
        if (DoorRt_KindOverlaySelfCheck(&full) != 0) {
          fprintf(stderr, "door-selftest: door+pot tier %u seed %d kind-overlay failed\n",
                  (unsigned)tier, seed);
          hard_fail = true;
        }
        for (uint8 d = 0; d < kDoorTbl_DungeonCount; d++) {
          if (!((full.shuffled_mask >> d) & 1)) continue;
          uint16 origins[kDoorGen_MaxOrigins];
          int norigins = Door_InitialOrigins(d, origins);
          DoorExploreResult staged;
          Door_ExploreStaged(&full, d, origins, &norigins, &staged);
          if (!Door_CheckValid(&full, d, &staged)) {
            fprintf(stderr, "door-selftest: door+pot tier %u seed %d dungeon %s "
                    "accepted layout fails recheck\n",
                    (unsigned)tier, seed, DoorIdx_Name(kDoorTblDungeons[d].name_off));
            hard_fail = true;
          }
        }
        ok_count++;
      }
      printf("door-selftest: door+pot tier %u full-mask ok %d/3, digest %08x bridge %08x\n",
             (unsigned)tier, ok_count, DoorShuffle_LayoutDigest(&full),
             full.pot_bridge_digest);
    }
  }

  // Cross-generation door-layout leak regression:
  // Rando_PlaceWithEntrances leaves the accepted door layout installed for the
  // caller's sphere/goal/spoiler work; Rando_GenerateSlot's overlay clear MUST
  // drop it, or the next vanilla-doors generation in the same process places
  // against stale door reachability (and saves a slot with door_digest 0
  // certified against the wrong logic graph). Sequence: vanilla-doors digest →
  // doors-ON generation (layout installed by contract) → the same
  // Rando_ClearGenerationLogicOverlays the slot path runs → vanilla-doors
  // digest again; first and third must be byte-identical.
  {
    bool leak_fail = false;
    RandoSettings st;
    Settings_SetDefaults(&st);
    const uint64 s = 0xD00DCAFE0000BEEFull;
    RandoPlacement *entries =
        (RandoPlacement *)calloc(kRandoLocationsCount, sizeof(RandoPlacement));
    if (entries == NULL) {
      fprintf(stderr, "door-selftest: leak check: OOM\n");
      return 1;
    }
    RandoPlacementTable table = { entries, 0 };
    uint8 digest_before[32], digest_after[32];
    if (!Place_AssumedFill(&st, s, 0, &table)) {
      fprintf(stderr, "door-selftest: leak check: vanilla placement failed\n");
      leak_fail = true;
    } else {
      PlacementTable_ComputeDigest(&table, digest_before);
      RandoSettings st_doors = st;
      st_doors.door_shuffle = kDoorShuffle_Basic;
      RandoEntranceRegen reg;
      table.count = 0;
      if (!Rando_PlaceWithEntrances(&st_doors, s, /*budget_seconds=*/0, &table, &reg)) {
        fprintf(stderr, "door-selftest: leak check: doors-ON generation failed\n");
        leak_fail = true;
      } else if (Rando_GetDoorLogicLayout(NULL) == NULL) {
        // The doors-ON arm must leave the layout installed (the contract the
        // clear exists for) — a NULL here would make this check vacuous.
        fprintf(stderr, "door-selftest: leak check: doors-ON left no layout installed\n");
        leak_fail = true;
      } else {
        Rando_ClearGenerationLogicOverlays();
        if (Rando_GetDoorLogicLayout(NULL) != NULL) {
          fprintf(stderr, "door-selftest: leak check: clear left the door layout installed\n");
          leak_fail = true;
        }
        table.count = 0;
        if (!Place_AssumedFill(&st, s, 0, &table)) {
          fprintf(stderr, "door-selftest: leak check: post-clear vanilla placement failed\n");
          leak_fail = true;
        } else {
          PlacementTable_ComputeDigest(&table, digest_after);
          if (memcmp(digest_before, digest_after, sizeof(digest_before)) != 0) {
            fprintf(stderr,
                    "door-selftest: leak check: vanilla digest changed after a doors-ON generation\n");
            leak_fail = true;
          }
        }
      }
    }
    free(entries);
    printf("door-selftest: cross-generation leak check %s\n", leak_fail ? "FAILED" : "ok");
    hard_fail |= leak_fail;
  }

  if (hard_fail) {
    fprintf(stderr, "door-selftest: FAIL (hard check)\n");
    return 1;
  }
  if (total_fail * 20 > total_pairs) {  // > 5% generation failure
    fprintf(stderr, "door-selftest: FAIL (%d/%d generation failures)\n", total_fail, total_pairs);
    return 1;
  }
  printf("door-selftest: OK (%d/%d generated)\n", total_pairs - total_fail, total_pairs);
  return 0;
}
