// door_keylogic.h — INTERNAL header shared between shuffle_doors.c (explorer +
// stitcher) and door_keylogic.c (key-door candidates + softlock prover).
// Nothing here is a public contract; the public API is shuffle_doors.h.
#pragma once
#include "../types.h"
#include "door_tables.gen.h"
#include "rando_logic.h"
#include "shuffle_doors.h"
#include "rando_rng.h"

// ---------------------------------------------------------------------------
// Static topology index, built once from the generated tables (lazy).
// ---------------------------------------------------------------------------

// Location classification bits (from Rando_GetLocationName at init; mirrors
// DungeonStitcher.prize_or_event / find_big_chest_locations /
// find_big_key_locked_locations / blind_boss_unavail).
enum {
  kDoorLoc_Prize = 0x1,       // "... - Prize" (never a free/key location)
  kDoorLoc_BigChest = 0x2,    // "... - Big Chest"
  kDoorLoc_ThievesBoss = 0x4, // "Thieves' Town - Boss" (Blind availability gate)
  kDoorLoc_BkLockedName = 0x8,// Blind's Cell / Zelda's Cell (find_big_key_locked_locations)
};

#define kDoorGen_MaxPool 64       // pool stubs per dungeon (GT = 60)
#define kDoorGen_MaxOrigins 12    // portals + drops per dungeon (Skull = 8)
#define kDoorKey_MaxCandidates 96 // candidate key-door pairs per dungeon

typedef struct DoorIdx {
  bool ready;
  // Edge CSR grouped by from_region (edge order within a region preserved).
  uint16 edge_first[kDoorTbl_RegionCount + 1];
  uint16 edge_csr[kDoorTbl_EdgeCount];
  // Regions exempt from the all-regions connectivity requirement: the " Portal"
  // placeholder regions and regions with no satisfiable incoming edge (their
  // only entries carry a constant-false rule — DR's "considered out of logic"
  // ranged-crystal sub-regions). The reference explorer visits those because it
  // ignores access rules; ours evaluates rules, so they are exempted instead.
  uint8 region_exempt[(kDoorTbl_RegionCount + 7) / 8];
  uint8 loc_class[kDoorTbl_LocationCount];
  // Thieves Town special regions (resolved by name / event rows):
  uint16 maiden_region;     // 'Thieves Blind's Cell Interior' (Suspicious Maiden)
  uint16 attic_region;      // 'Thieves Attic Window' (Attic Cracked Floor)
  uint16 thieves_boss_region;
  uint16 blind_cell_region; // 'Thieves Blind's Cell' (tuple-path start)
  // get_special_big_key_doors (DungeonStitcher): doors gated on the big key
  // without carrying the BigKey kind.
  uint16 special_bk_door[2];
  int special_bk_count;
  // Destination portals (never origins; DoorShuffle.link_doors_prep hardcodes
  // these for vanilla entrance shuffle, open/standard non-inverted world).
  uint16 dest_lobby[4];
  int dest_lobby_count;
  // Staged origins (portal lobbies whose OUTSIDE region is in
  // find_inaccessible_regions; enabled once any enabler region is reached —
  // models find_enabled_origins/enable_new_entrances for whole-dungeon
  // stitching). enabler[1] may be 0xFFFF.
  struct { uint16 lobby; uint16 enabler[2]; } staged[3];
  int staged_count;
  // Pool doors grouped per dungeon, ascending door id.
  uint16 pool_first[kDoorTbl_DungeonCount + 1];
  uint16 pool_doors[512];
  uint8 drop_cnt[kDoorTbl_DungeonCount];   // kDoorTblDropKeys rows per dungeon
  uint16 external_events;                  // event rows with region 0xFFFF
  // Edges crossing a given door (a door appears in at most 2 edge rows).
  uint16 door_edge[kDoorTbl_DoorCount][2]; // 0xFFFF = none
} DoorIdx;

extern DoorIdx g_door_idx;
void DoorIdx_Init(void);

static inline const char *DoorIdx_Name(uint16 off) { return &kDoorTblNames[off]; }

// Current partner of a door: layout pairing for pool doors, vanilla otherwise.
static inline uint16 Door_PartnerOf(uint16 door, const DoorShuffleLayout *layout) {
  if (layout && (kDoorTblDoors[door].flags & kDoorTblFlag_InPool) &&
      layout->pairing[door] != 0xFFFF)
    return layout->pairing[door];
  return kDoorTblDoors[door].vanilla_partner;
}

// ---------------------------------------------------------------------------
// Explorer core. One reachability model, four big-key gate modes:
//  - Open:        big-key doors pass (public lenient default).
//  - LenientGate: pass once >= 1 free location is reachable (stitcher;
//                 DungeonStitcher.extend_reachable_state_lenient bk_relevant).
//  - State:       pass iff bk_open (prover state machine).
//  - Held:        pass iff big_key_held[dungeon] (oracle gates).
// ---------------------------------------------------------------------------
enum { kDoorBkMode_Open, kDoorBkMode_LenientGate, kDoorBkMode_State, kDoorBkMode_Held };

// A chosen key door: a = canonical door id, b = coupled partner (0xFFFF for
// spiral singles — vanilla spiral key stairs lock only their own direction).
typedef struct DoorKeyPair { uint16 a, b; } DoorKeyPair;

typedef struct DoorExploreSpec {
  const DoorShuffleLayout *layout;  // pairing redirects, NULL = vanilla
  uint8 dungeon;
  const uint16 *origins;
  int origin_count;
  bool (*vm_pred)(void *ud, uint16 vm_index);  // NULL = lenient (true)
  bool (*pot_pred)(void *ud, const RandoDoorPotLocation *pot);
  bool (*enemy_drop_pred)(void *ud, const RandoDoorEnemyDropLocation *drop);
  bool (*enemy_check_pred)(void *ud, const RandoDoorEnemyCheckLocation *check);
  void *ud;
  const DoorKeyPair *pairs;  // NULL = key doors open
  int pair_count;
  uint32 open_mask;          // bit i: pair i opened
  uint8 bk_mode;
  bool bk_open;              // State mode
  const uint8 *held_keys;       // Held mode: chest keys held per dungeon
  const uint8 *big_key_held;    // Held mode (NULL = held)
  const DoorShuffleLayout *thresholds;  // Held mode: key_worst_case source
  uint8 pot_tier;               // active pot tier for drop/key-source accounting
  uint8 enemy_drop_keys;         // active enemy key checks for drop accounting
  uint8 enemy_check_tier;        // active ordinary enemy checks for key-source accounting
} DoorExploreSpec;

void DoorExplore_Core(const DoorExploreSpec *spec, DoorExploreResult *out);

static inline bool DoorExplore_ColorAt(const DoorExploreResult *r, uint16 region, int blue) {
  const uint8 *v = blue ? r->visited_blue : r->visited_orange;
  return (v[region >> 3] >> (region & 7)) & 1;
}

// Free locations of `dungeon` reached in `res`: fork locations minus prizes,
// minus Thieves' Boss while Blind is unavailable (maiden/attic regions not
// reached), optionally minus big chests (count_locations_exclude_big_chest).
int Door_CountFreeLocs(uint8 dungeon, const DoorExploreResult *res, bool exclude_big_chest);
// Reached drop-key rows of `dungeon` (key_only_locations), excluding active
// key-source rows whose keys are first-class locations under the active tiers.
int Door_CountDropKeys(uint8 dungeon, const DoorExploreResult *res, uint8 pot_tier,
                       uint8 enemy_drop_keys);
bool Door_DropExcludedByActivePot(uint8 dungeon, uint16 drop_index, uint8 pot_tier);
bool Door_DropExcludedByActiveEnemyDrop(uint8 dungeon, uint16 drop_index,
                                        uint8 enemy_drop_keys);
int Door_CountActivePotKeySources(uint8 dungeon, const DoorExploreResult *res,
                                  uint8 pot_tier,
                                  bool (*pred)(void *ud, const RandoDoorPotLocation *pot),
                                  void *ud);
int Door_CountActiveKeyPots(uint8 dungeon, uint8 pot_tier);
int Door_CountActiveEnemyDropKeySources(uint8 dungeon, const DoorExploreResult *res,
                                        uint8 enemy_drop_keys,
                                        bool (*pred)(void *ud,
                                                     const RandoDoorEnemyDropLocation *drop),
                                        void *ud);
int Door_CountActiveEnemyDropKeys(uint8 dungeon, uint8 enemy_drop_keys);
int Door_CountActiveEnemyCheckKeySources(uint8 dungeon, const DoorExploreResult *res,
                                         uint8 enemy_check_tier,
                                         bool (*pred)(void *ud,
                                                      const RandoDoorEnemyCheckLocation *check),
                                         void *ud);
int Door_CountActiveEnemyChecks(uint8 dungeon, uint8 enemy_check_tier);
// True iff door `d` is gated on the big key (VanillaBigKey kind or one of
// get_special_big_key_doors).
bool Door_IsBkGated(uint16 door);
// True iff `edge` can be crossed from its from-region given `res` (region
// reached, crystal flags compatible, rule passes under lenient Vm). Ignores
// key/big-key gating — used to ask "is this closed door reachable-adjacent".
bool Door_EdgePassableFrom(const DoorExploreResult *res, uint16 edge_index);

// ---------------------------------------------------------------------------
// Key-door shuffle per dungeon (door_keylogic.c). `origins` = the stitcher's
// final staged origin set (== builder.path_entrances after
// filter_start_regions: destination portals excluded, drops included).
// Fills layout->key_door_count/key_doors/key_worst_case[dungeon] and appends
// to layout->bk_restricted. Returns false on honest cap exhaustion.
// ---------------------------------------------------------------------------
bool DoorKeys_ShuffleDungeon(uint8 dungeon, RandoRng *rng,
                             const uint16 *origins, int origin_count,
                             DoorShuffleLayout *layout);
