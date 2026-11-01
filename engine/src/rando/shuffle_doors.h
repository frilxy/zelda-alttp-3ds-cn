// shuffle_doors.h — door-shuffle generation: the per-dungeon stitcher,
// crystal-aware explorer, and key-door softlock prover (randomizer-shuffles).
//
// Ported from ALttPDoorRandomizer's live pipeline (DoorShuffle.py
// main_dungeon_pool/main_dungeon_generation -> source/dungeon/
// DungeonStitcher.py generate_dungeon; KeyDoorShuffle.py analyze_dungeon /
// validate_key_layout), basic mode, intensity 1 (Normal + SpiralStairs),
// door_type_mode=original (vanilla per-dungeon SMALL-key-door counts,
// positions re-chosen + prover-validated; big-key doors stay put).
//
// Determinism contract: every random draw comes from the RandoRng passed in
// (seeded by the caller from (base_seed, door_attempt)); every candidate list
// is door-id-ordered before a draw; iteration-bounded, never wall-clock.
#pragma once
#include "../types.h"
#include "door_tables.gen.h"
#include "rando_logic.h"

// Per-dungeon caps (worst case GT: 8 chest keys + drops; pool stubs ~60).
#define kDoorShuffle_MaxKeyDoors 10
#define kDoorShuffle_MaxBkRestricted 96

// Door-table dungeon indices come from generated door_tables.gen.*. They are a
// third domain: not kRandoDungeon_* and not cur_palace_index_x2>>1.
enum {
  kDoorShuffleTblDungeon_HyruleCastle = 0,
  kDoorShuffleTblDungeon_SwampPalace = 6,
  kDoorShuffleTblDungeon_ThievesTown = 8,
};

// MVP shuffled-dungeon mask: all 13 kDoorTblDungeons EXCEPT the pins —
// Hyrule Castle (forced escape start + Zelda escort + std key special-casing)
// and Swamp Palace (the unique non-monotone water-level rule cluster +
// drain/flood runtime risk). See the
// add-rando-door-shuffle design pins P4'/P5.
#define kDoorShuffle_MvpDungeonMask \
  ((uint16)(0x1FFF & ~(1u << kDoorShuffleTblDungeon_HyruleCastle) & \
            ~(1u << kDoorShuffleTblDungeon_SwampPalace)))

typedef struct DoorShuffleLayout {
  // pairing[door_id] = partner door id, 0xFFFF = unshuffled (vanilla).
  // Both directed halves are present for coupled pairs. Doors outside the
  // shuffle pool are always 0xFFFF.
  uint16 pairing[kDoorTbl_DoorCount];
  // Relocated small-key doors per dungeon (door_type_mode=original): the
  // canonical (lower) door id of each chosen key-door pair + the prover's
  // worst-case threshold N ("safe to open holding >= N keys of this
  // dungeon, counting reachable drop keys").
  uint8 key_door_count[kDoorTbl_DungeonCount];
  uint16 key_doors[kDoorTbl_DungeonCount][kDoorShuffle_MaxKeyDoors];
  uint8 key_worst_case[kDoorTbl_DungeonCount][kDoorShuffle_MaxKeyDoors];
  // Big-key placement ban (fork location ids): locations reachable only past
  // the (un-relocated) big-key door under this layout.
  uint16 bk_restricted[kDoorShuffle_MaxBkRestricted];
  uint8 bk_restricted_count;
  // Which dungeons were actually shuffled (bit = kDoorTblDungeons index).
  uint16 shuffled_mask;
  // Effective pot shuffle tier this door layout was generated/proven against.
  // 0 means pot shuffle inactive for door logic.
  uint8 pot_tier;
  uint32 pot_bridge_digest;
  // Effective enemy-drop key checks this door layout was generated/proven
  // against. 0 means forced enemy drops remain vanilla/free for door logic.
  uint8 enemy_drop_keys;
  uint32 enemy_drop_bridge_digest;
  // Effective ordinary enemy-check tier this door layout was generated/proven
  // against. 0 means ordinary enemy checks are inactive for door logic.
  uint8 enemy_check_tier;
  uint32 enemy_check_bridge_digest;
} DoorShuffleLayout;

// Generate the full per-seed layout: for each dungeon whose bit is set in
// `dungeon_mask`, stitch a proposal (connectivity + required paths) and prove
// the relocated key doors softlock-free. Deterministic in (seed, attempt).
// Returns false when any dungeon exhausts its iteration caps (caller bumps
// door_attempt and retries).
bool DoorShuffle_Generate(uint64 seed, uint32 attempt, uint16 dungeon_mask,
                          uint8 pot_tier, uint8 enemy_drop_keys,
                          uint8 enemy_check_tier, DoorShuffleLayout *out);

// Stable digest over the layout (pairings + key doors + thresholds +
// bk_restricted), used for the sidecar drift hard-fail. Door ids are frozen
// by door_registry.yaml, so equal layouts digest equally across builds.
uint32 DoorShuffle_LayoutDigest(const DoorShuffleLayout *l);

// True iff the prover banned the big key from this fork location (consulted
// by the placer next to dungeon-mode containment).
static inline bool DoorShuffle_BkRestricted(const DoorShuffleLayout *l, uint16 loc_id) {
  for (int i = 0; i < l->bk_restricted_count; i++)
    if (l->bk_restricted[i] == loc_id)
      return true;
  return false;
}

// ---------------------------------------------------------------------------
// Explorer query API — the single reachability model shared by the stitcher
// (lenient), the prover (key-door state machine), and the logic oracle
// (Stage 3 binds `vm_pred` to the compiled fork predicate stream).
// ---------------------------------------------------------------------------

typedef struct DoorExploreGates {
  // Evaluate door-table rule-blob Vm leaves (kDoorRuleOp_Vm index). NULL =
  // lenient (every Vm leaf true).
  bool (*vm_pred)(void *ud, uint16 vm_index);
  bool (*pot_pred)(void *ud, const RandoDoorPotLocation *pot);
  bool (*enemy_drop_pred)(void *ud, const RandoDoorEnemyDropLocation *drop);
  bool (*enemy_check_pred)(void *ud, const RandoDoorEnemyCheckLocation *check);
  void *ud;
  // Small keys HELD (chest keys) per dungeon; the explorer adds reachable
  // drop-key rooms internally. Ignored when key_thresholds is NULL.
  const uint8 *held_keys;        // [kDoorTbl_DungeonCount] or NULL
  const uint8 *big_key_held;     // [kDoorTbl_DungeonCount] or NULL = held
  // Key-door gating: when non-NULL, a key-door edge of dungeon d passes iff
  // held_keys[d] + reachable_drop_keys(d) >= its worst-case threshold from
  // `layout`. NULL = key doors treated open (lenient / stitcher mode).
  const DoorShuffleLayout *key_thresholds;
} DoorExploreGates;

typedef struct DoorExploreResult {
  // Region reachability per crystal state (bit = region id - the queried
  // dungeon's region_first).
  uint8 visited_blue[(kDoorTbl_RegionCount + 7) / 8];
  uint8 visited_orange[(kDoorTbl_RegionCount + 7) / 8];
  // Events granted during exploration (bit = kDoorEvent_*).
  uint16 events;
} DoorExploreResult;

// Explore one dungeon under `layout` (NULL = vanilla pairing) from the given
// portal lobby regions. `portal_regions`/`portal_count` come from the door
// portal table filtered by the caller (generation: all portals; oracle:
// currently-reachable portals only).
void DoorExplore_Run(const DoorShuffleLayout *layout, uint8 dungeon,
                     const uint16 *portal_regions, int portal_count,
                     const DoorExploreGates *gates, DoorExploreResult *out);

// True iff `region` is visited in either crystal state.
bool DoorExplore_Reached(const DoorExploreResult *r, uint16 region);

// Evaluate a door-table rule blob (kDoorTblRuleBlob offset; 0xFFFF = always
// true) against an exploration result (Event/CReach/Reach leaves) and the
// gates' vm_pred (Vm leaves; NULL gates/vm_pred = lenient true). Used by the
// explorer internally for edge rules and by the logic oracle for at-location
// rules after the flood.
bool DoorExplore_EvalRule(uint16 rule_off, const DoorExploreResult *r,
                          const DoorExploreGates *gates);

// Bitmask (over kDoorVmPreds indices, 128-bit) of the vm predicates whose
// value can influence `dungeon`'s flood or at-location rules. The logic
// oracle fingerprints exactly these results to skip refloods when an
// inventory change is irrelevant to the dungeon.
void DoorExplore_RelevantVmMask(uint8 dungeon, uint64 out_mask[2]);

// Self-test (wired to --door-selftest): for every shuffleable dungeon x N
// seeds, generate + assert connectivity, prover acceptance, determinism
// (regenerate -> identical digest), and full-inventory oracle==stitcher
// agreement. Returns 0 on success, nonzero on failure (messages to stderr).
int DoorShuffle_SelfTest(void);

// Dev dump (wired to --dump-key-depth): the AUTHORITATIVE per-region /
// per-location minimum small-key DEPTH under VANILLA doors — the number a
// location must require as HAS_AMOUNT(SmallKey_X, depth) once pot keys become
// first-class shuffled items (pot-sanity task #25). Computed by enumerating
// every key-door open-mask through DoorExplore_Core (the real reachability
// engine), NOT a naive edge BFS. Writes a parseable text table to `path`;
// returns 0 on success, 1 on file error.
int DoorKeys_DumpKeyDepth(const char *path);
