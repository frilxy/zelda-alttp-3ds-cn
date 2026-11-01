#pragma once
// add-rando-ow-warp-shuffle — per-seed warp-layout derivation (design D6).
//
// The layout is a pure function of (seed, attempt, settings): flute spots
// via the ported upstream selection semantics (forced Desert/Mire entries,
// sector spread, balanced proximity-conflict rejection with the upstream
// 1-in-32 escape), whirlpools via a uniform perfect matching over the six
// Light World whirlpools (identity over the two Dark World indices, so the
// uniform landing-row formula degrades to vanilla for them). Regenerated
// identically at generation, slot activation, and snapshot cold replay;
// drift is caught by the 24-bit digest gate (hard-fail, door-gate class).

#include "../types.h"
#include "rando_settings.h"

enum { kOwWarpFluteSlots = 8, kOwWarpWhirlpoolMax = 8 };

typedef struct OwWarpLayout {
  uint8 active;                            // bit0 flute, bit1 whirlpool
  uint8 flute_cand[kOwWarpFluteSlots];     // kRandoOwFluteCandidates indices,
                                           // slot order (menu slot k)
  uint8 wp_partner[kOwWarpWhirlpoolMax];   // per kRandoOwWhirlpools index:
                                           // per-seed partner TABLE index
                                           // (identity when not shuffled)
} OwWarpLayout;

// Derive the layout. Returns false when a warp axis is requested but the
// compiled OW graph is absent/empty (fail-closed; caller refuses the seed /
// slot). With both axes off, returns true with active == 0.
bool OwWarp_Compute(const RandoSettings *settings, uint64 seed,
                    uint32 attempt, OwWarpLayout *out);

// 24-bit fold over the installed layout (spot candidate indices in slot
// order + whirlpool partner table), for the sidecar drift gate.
uint32 OwWarp_Digest24(const OwWarpLayout *l);

// Install the per-seed logic edges (8 hub->spot-component edges + the LW
// whirlpool component pair edges) through the added-edge overlay. The
// caller owns overlay lifecycle (Begin/Clear per attempt — the entrance
// retry-accumulation lesson). Asserts the overlay did not overflow.
void OwWarp_InstallLogicEdges(const OwWarpLayout *l);

// Runtime landing-row helper: entering whirlpool table index j returns the
// bird-travel row index to load ((vanilla partner of the per-seed partner)
// + 9 semantics live at the call site; this returns the TABLE index whose
// row to use). Identity mapping when whirlpool shuffle is inactive.
uint8 OwWarp_WhirlpoolLandingIndex(const OwWarpLayout *l, uint8 entered_idx);

// tracker-player-knowledge — table-index bits of whirlpools in a shuffled
// (non-identity) pair; the live knowledge mask hides them until ridden.
uint8 OwWarp_ShuffledWhirlpoolMask(const OwWarpLayout *l);

// --- runtime hooks (consume the ACTIVE slot's layout) -----------------------
// Flute: candidate row for bird-travel index k (< 8) when the flute axis is
// active on the current slot; NULL otherwise (vanilla path).
struct RandoOwFluteCandidate;
const struct RandoOwFluteCandidate *Rando_OwWarp_FluteSpotForBirdIndex(int k);
// Flute map blip for menu slot (< 8). Returns false when inactive (use the
// vanilla const tables). Vanilla-8 screens return the engine's hand-tuned
// positions verbatim; shuffled spots derive from the candidate link coords
// (cosmetic; playtest-tunable).
bool Rando_OwWarp_FluteBlip(uint8 slot, uint16 *x, uint16 *y);
// Whirlpool: landing bird-row base for the ENTERED kWhirlpoolAreas row.
// Returns the row whose (+9) tableau lands at the per-seed partner —
// identity when whirlpool shuffle is inactive. Screen-keyed against the
// asset table, so asset-vs-generated ordering cannot desync.
uint8 Rando_OwWarp_WhirlpoolBirdRow(uint8 entered_row);

// Selftest (wired into Rando_RunAllSelfChecks): determinism vector, forced-
// spot guarantee, sector spread, conflict honoring, whirlpool involution,
// digest stability, overlay budget (warp-alone AND the combined
// entrance+warp worst case, with zero dropped edges), and the vanilla-8
// tableau oracle (candidate tableaux byte-equal to the kBirdTravel_* asset
// rows; blips need no separate oracle — the vanilla arms of
// Rando_OwWarp_FluteBlip read the messaging.c const tables directly).
void OwWarp_SelfCheck(void);
