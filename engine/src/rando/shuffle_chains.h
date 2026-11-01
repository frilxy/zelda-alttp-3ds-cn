// shuffle_chains.h - dungeon-chain layout construction.
//
// Pure generation-side model for the dungeon_chains axis. Runtime hooks consume
// the resolved table later; this module owns only deterministic construction,
// digesting, and structural self-checks.

#ifndef ZELDA3_RANDO_SHUFFLE_CHAINS_H_
#define ZELDA3_RANDO_SHUFFLE_CHAINS_H_

#include "../types.h"

#define kChainsPoolCount 9
#define kChainsElement_BossFlag 0x80
#define kChainsElement_None 0xFF

typedef struct DungeonChainsLayout {
  // Per chain-start door, in kChainsPoolDungeons[] order. Each entry is an
  // encoded chain element: dungeon lobby (raw rando dungeon id) or boss terminal
  // (kChainsElement_BossFlag | rando dungeon id).
  uint8 chain_door_first[kChainsPoolCount];
  // Per pool dungeon, in kChainsPoolDungeons[] order: the element reached after
  // that dungeon's boss seam.
  uint8 chain_successor[kChainsPoolCount];
  // Per chain-start door, in kChainsPoolDungeons[] order: the terminal boss home
  // dungeon for this chain.
  uint8 terminal_boss[kChainsPoolCount];
  uint32 digest24;
} DungeonChainsLayout;

extern const uint8 kChainsPoolDungeons[kChainsPoolCount];

static inline uint8 Chains_DungeonElement(uint8 rando_dungeon) {
  return rando_dungeon;
}

static inline uint8 Chains_BossElement(uint8 rando_dungeon) {
  return (uint8)(kChainsElement_BossFlag | rando_dungeon);
}

static inline bool Chains_ElementIsBoss(uint8 elem) {
  return elem != kChainsElement_None && (elem & kChainsElement_BossFlag) != 0;
}

static inline uint8 Chains_ElementDungeon(uint8 elem) {
  return (uint8)(elem & ~kChainsElement_BossFlag);
}

int Chains_PoolIndexForDungeon(uint8 rando_dungeon);
uint8 Chains_PoolDungeonAt(uint8 pool_index);

// Deterministic in (seed_u64, attempt). Returns false only for bad pointers.
bool Chains_Compute(uint64 seed_u64, uint32 attempt, DungeonChainsLayout *out);

// Stable 24-bit digest over the resolved mapping. The same layout has the same
// digest across compilers and platforms.
uint32 Chains_LayoutDigest(const DungeonChainsLayout *layout);

// Install this layout's logic edge overrides. This uses the same override store
// as entrance shuffle; callers must clear it between attempts and after
// generation via Rando_ClearGenerationLogicOverlays.
void Chains_ApplyEdgeOverrides(const DungeonChainsLayout *layout);

// Invoked from --rando-selftest. Exits(2) on invariant failure.
void Chains_SelfCheck(void);

#endif  // ZELDA3_RANDO_SHUFFLE_CHAINS_H_
