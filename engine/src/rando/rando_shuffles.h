// rando_shuffles.h — prize + medallion shuffle modules (tasks.md §4.1a, §4.1b).
//
// Both shuffles are independent of item placement and run before assumed
// fill. They produce per-seed assignment tables consumed by the predicate VM
// (OP_HAS_PRIZE, OP_MEDALLION_OPENS) and §6 dispatch (the boss-prize grant
// path consults the prize assignment).

#ifndef ZELDA3_RANDO_SHUFFLES_H_
#define ZELDA3_RANDO_SHUFFLES_H_

#include "../types.h"
#include "rando_rng.h"
#include "rando_settings.h"
#include "rando_logic.h"

// ---------------------------------------------------------------------------
// Prize ids (mirror item_registry.yaml prize IDs offset to a small palette).
// The op_registry.yaml's _resolve_prize_id mapping in rando_logic_gen.py
// declares prizes in this exact order — keep aligned.
//
// NOTE the Red/Blue naming is the REGISTRY's, and it is swapped relative to the
// in-game colour: kPrize_RedPendant is the Desert/Power pendant, which the game
// draws BLUE, and kPrize_BluePendant is the Hera/Wisdom pendant, drawn RED (see
// PrizeIcon in rando_window/tracker_windows.cpp). Key display code off the ID.
// ---------------------------------------------------------------------------
enum {
  kPrize_GreenPendant = 0,
  kPrize_RedPendant = 1,
  kPrize_BluePendant = 2,
  kPrize_Crystal1 = 3,
  kPrize_Crystal2 = 4,
  kPrize_Crystal3 = 5,
  kPrize_Crystal4 = 6,
  kPrize_Crystal5 = 7,
  kPrize_Crystal6 = 8,
  kPrize_Crystal7 = 9,
};

// ---------------------------------------------------------------------------
// Prize shuffle (task 4.1a).
//
// Assigns each of {Crystal 1..7, Green/Red/Blue Pendant} to a dungeon. The
// output is indexed by dungeon id (kRandoDungeonCount slots) and contains the
// prize id (0..kRandoPrizeCount-1) placed at that dungeon's _Prize slot.
//
// Phase A defaults: shuffle is ON. The 7-crystal-dungeon pool (PoD, SP, SW,
// TT, IP, MM, TR) maps to {Crystal 1..7} in some permutation. The 3-pendant-
// dungeon pool (EP, DP, TH) maps to {Green, Red, Blue} in some permutation.
// HCE, HCT, GT do not have a _Prize slot in the shuffle (HCT/GT terminate at
// Agahnim 1/2 prize events; HCE has the Zelda escort event).
//
// When `settings->prize_shuffle == 0`, the output is the identity assignment
// (each dungeon holds its vanilla prize).
// ---------------------------------------------------------------------------
void PrizeShuffle_Run(const RandoSettings *settings,
                      RandoRng *rng,
                      uint8 out_assignment[kRandoDungeonCount]);

// ---------------------------------------------------------------------------
// Medallion shuffle (task 4.1b).
//
// Assigns the medallion that opens each of {Misery Mire entrance,
// Turtle Rock entrance} to one of {Bombos=25, Ether=26, Quake=27} (item ids).
// The output is indexed by entrance id (0=MM, 1=TR).
//
// Independent of prize shuffle. Phase A defaults: shuffle is ON. When
// `settings->medallion_shuffle == 0`, MM gets Ether (26), TR gets Quake (27).
// ---------------------------------------------------------------------------
void MedallionShuffle_Run(const RandoSettings *settings,
                          RandoRng *rng,
                          uint8 out_assignment[kRandoMedallionEntranceCount]);

void Shuffles_SelfCheck(void);

#endif  // ZELDA3_RANDO_SHUFFLES_H_
