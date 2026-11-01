#include "ow_map_prizes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dungeon_ids.h"
#include "rando.h"
#include "rando_placement.h"
#include "rando_shuffles.h"
#include "../features.h"
#include "../messaging.h"  // vanilla marker tables (kOwMapCrystal_tab et al.)
#include "../variables.h"

// kDungeonCrystalPendantBit[game_dungeon] — the prize bit a dungeon sets in
// vanilla. Cross-referenced against kPendantBitMask/kCrystalBitMask (which say
// which bit each MARKER SLOT tests) this pins slot -> dungeon from a source the
// icon tables do not share; RandoOwMap_SelfCheck asserts exactly that, so a
// permutation applied consistently to slot->dungeon AND prize->icon (which
// would cancel out in a composition-only check) still fails.
extern const uint8 kDungeonCrystalPendantBit[13];

enum { kOwMapMarkerSlots = 7, kPrizeCount = 10 };

// Map-icon states whose markers point at prize dungeons. Everything else the
// pause map draws (Hyrule Castle, Sahasrahla, the Master Sword pedestal,
// Ganon's Tower) is routing, carries no prize claim, and stays vanilla.
enum {
  kOwMapState_Pendants = 3,   // the three light-world pendant dungeons
  kOwMapState_FirstDark = 6,  // "go to the first dark-world dungeon" (PoD)
  kOwMapState_Crystals = 7,   // the seven dark-world crystal dungeons
};

// Slot -> dungeon. Derived from kPendantBitMask / kCrystalBitMask (messaging.c)
// cross-referenced against kDungeonCrystalPendantBit (zelda_rtl.c), and
// independently confirmed against the upstream ALTTPR per-dungeon marker tables
// (z3randomizer tables.asm WorldMapIcon_pos*), whose coordinates are
// byte-identical to ours in this order. See the change's design.md.
static const uint8 kPendantSlotDungeon[3] = {
  kRandoDungeon_EasternPalace,   // slot 0, pendant bit 0x04
  kRandoDungeon_TowerOfHera,     // slot 1, pendant bit 0x01
  kRandoDungeon_DesertPalace,    // slot 2, pendant bit 0x02
};
static const uint8 kCrystalSlotDungeon[7] = {
  kRandoDungeon_PalaceOfDarkness,  // slot 0, crystal bit 0x02
  kRandoDungeon_SkullWoods,        // slot 1, crystal bit 0x40
  kRandoDungeon_TurtleRock,        // slot 2, crystal bit 0x08
  kRandoDungeon_ThievesTown,       // slot 3, crystal bit 0x20
  kRandoDungeon_MiseryMire,        // slot 4, crystal bit 0x01
  kRandoDungeon_IcePalace,         // slot 5, crystal bit 0x04
  kRandoDungeon_SwampPalace,       // slot 6, crystal bit 0x10
};

// Prize id -> marker icon word. Read off the vanilla tables under the identity
// assignment: Green is Eastern Palace (slot 0 @ state 3), the registry's "Red"
// is Desert (slot 2 @ state 3), the registry's "Blue" is Tower of Hera (slot 1
// @ state 3) — the registry names are swapped vs the drawn colour, so this is
// keyed by ID, never by colour word (see kPrize_* in rando_shuffles.h). Every
// crystal shares one icon and differs only in the number glyph below.
// RandoOwMap_SelfCheck re-proves all of this against the vanilla arrays.
static const uint16 kPrizeTab[kPrizeCount] = {
  0x6038,  // Green pendant
  0x6034,  // Red pendant (draws blue)
  0x6032,  // Blue pendant (draws red)
  0x6434, 0x6434, 0x6434, 0x6434, 0x6434, 0x6434, 0x6434,  // Crystal 1..7
};

// Crystal 1..7 -> number glyph. Vanilla blinks these out of kOverworldMapData
// indexed by OAM slot, which prize_shuffle invalidates. Cross-checked against
// kBirdTravel_tab1[0..6] (the flute map's independent 1..8 digit glyphs).
static const uint8 kCrystalNumChar[7] = {0x7f, 0x79, 0x6c, 0x6d, 0x6e, 0x6f, 0x7c};

// Which dungeon marker slot `slot` points at in map-icon state `k`, or
// kRandoDungeon_None when the slot carries no prize claim.
static uint8 PrizeDungeonForSlot(uint8 k, uint8 slot) {
  if (slot >= kOwMapMarkerSlots)
    return kRandoDungeon_None;
  switch (k) {
    // Slot 3 in this state is the Master Sword pedestal, not a prize.
    case kOwMapState_Pendants:
      return slot < 3 ? kPendantSlotDungeon[slot] : (uint8)kRandoDungeon_None;
    case kOwMapState_FirstDark:
      return slot == 0 ? (uint8)kRandoDungeon_PalaceOfDarkness
                       : (uint8)kRandoDungeon_None;
    case kOwMapState_Crystals:
      return kCrystalSlotDungeon[slot];
    default:
      return kRandoDungeon_None;
  }
}

// Pure decision core — reads NO process state, so RandoOwMap_SelfCheck can
// drive every branch with synthetic inputs. `shuffled` = the seed shuffles
// prizes (or its settings are unrecoverable and we fail closed); `assign` may
// be NULL; `observed` bit d = the player has collected dungeon d's prize.
// Keeping this separate is deliberate: the wrapper below is then a
// state-gathering shim thin enough to audit by eye, and the logic that decides
// what a marker CLAIMS is covered by the oracle.
static bool OwMapResolveMarker(uint8 k, uint8 slot, bool shuffled,
                               const uint8 *assign, uint16 observed,
                               uint16 *tab, uint8 *num_char) {
  if (!shuffled)
    return false;  // identity assignment: the vanilla art is already true
  uint8 dungeon = PrizeDungeonForSlot(k, slot);
  if (dungeon == kRandoDungeon_None)
    return false;

  // Unknown until observed. Note this holds even when the roll happens to
  // reproduce the vanilla assignment — the player cannot know that.
  *tab = 0;
  *num_char = 0;
  if (!(observed & (uint16)(1u << dungeon)))
    return true;
  if (assign == NULL)
    return true;
  uint8 prize = assign[dungeon];
  if (prize >= kPrizeCount)
    return true;  // unknown prize id: stay unknown rather than draw garbage
  *tab = kPrizeTab[prize];
  if (prize >= kPrize_Crystal1)
    *num_char = kCrystalNumChar[prize - kPrize_Crystal1];
  return true;
}

bool RandoOwMap_PrizeMarker(uint8 k, uint8 slot, uint16 *tab, uint8 *num_char) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive))
    return false;
  // Fail CLOSED when the active slot's settings can't be recovered (snapshot
  // replay / pre-settings slot): treat the seed as shuffled so the marker
  // degrades to the unknown form rather than asserting the vanilla prize.
  // `rs && rs->prize_shuffle` would render the vanilla claim when unknown.
  const RandoSettings *rs = Rando_GetActiveSettings();
  bool shuffled = (rs == NULL) || rs->prize_shuffle != 0;

  // Observation = the player collected that dungeon's prize, so naming it
  // reveals nothing they don't already hold. Allowlisted assignment read (this
  // file only) — see assets/scripts/check_knowledge_consumers.py.
  uint16 observed = 0;
  for (uint8 d = 0; d < kRandoDungeonCount; d++) {
    uint16 prize_loc = Rando_GetDungeonPrizeLocation(d);
    if (prize_loc != 0xFFFF && Rando_IsLocationChecked(prize_loc))
      observed |= (uint16)(1u << d);
  }
  return OwMapResolveMarker(k, slot, shuffled, Rando_GetDungeonPrizeAssignment(),
                            observed, tab, num_char);
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[RandoOwMap_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void RandoOwMap_SelfCheck(void) {
  // (1) ANCHOR slot -> dungeon to the prize BITS, a source the icon tables do
  // not share. A composition-only check (slot -> dungeon -> prize -> icon vs
  // the vanilla icon) is circular: permute slot->dungeon and prize->icon
  // together and the errors cancel. The bit tables cannot cancel with either.
  for (uint8 slot = 0; slot < 3; slot++) {
    uint8 game = Rando_GameDungeonFromRandoDungeon(kPendantSlotDungeon[slot]);
    if (game >= 13 || kDungeonCrystalPendantBit[game] != kPendantBitMask[slot])
      selfcheck_die("pendant slot -> dungeon disagrees with the vanilla prize bits");
  }
  for (uint8 slot = 0; slot < kOwMapMarkerSlots; slot++) {
    uint8 game = Rando_GameDungeonFromRandoDungeon(kCrystalSlotDungeon[slot]);
    if (game >= 13 || kDungeonCrystalPendantBit[game] != kCrystalBitMask[slot])
      selfcheck_die("crystal slot -> dungeon disagrees with the vanilla prize bits");
  }
  if (PrizeDungeonForSlot(kOwMapState_FirstDark, 0) !=
      kCrystalSlotDungeon[0])
    selfcheck_die("first-dark-world marker must point at the same dungeon as crystal slot 0");

  // (2) The identity assignment is what vanilla's baked marker art depicts, so
  // driving the REAL resolver with it must reproduce that art exactly — icon
  // word and crystal-number glyph, for every prize slot in every affected map
  // icon state. PrizeShuffle_Run(NULL, ...) takes the identity branch without
  // drawing RNG. `observed` is all-ones here: the revealed branch is the one
  // that makes a claim, so that is the branch this must pin.
  uint8 identity[kRandoDungeonCount];
  PrizeShuffle_Run(NULL, NULL, identity);
  const uint16 kAllObserved = 0xFFFF;

  static const uint8 kPrizeStates[3] = {
    kOwMapState_Pendants, kOwMapState_FirstDark, kOwMapState_Crystals,
  };
  int prize_slots = 0;
  for (int si = 0; si < 3; si++) {
    uint8 k = kPrizeStates[si];
    for (uint8 slot = 0; slot < kOwMapMarkerSlots; slot++) {
      uint8 dungeon = PrizeDungeonForSlot(k, slot);
      uint16 tab = 0xDEAD;
      uint8 num_char = 0xEE;
      bool owned = OwMapResolveMarker(k, slot, /*shuffled=*/true, identity,
                                      kAllObserved, &tab, &num_char);
      if (owned != (dungeon != kRandoDungeon_None))
        selfcheck_die("resolver disagrees with the slot -> dungeon table");
      if (!owned)
        continue;
      prize_slots++;
      uint8 prize = identity[dungeon];
      if (prize >= kPrizeCount)
        selfcheck_die("prize marker slot maps to a dungeon with no vanilla prize");
      if (tab != kOwMapCrystal_tab[slot][k])
        selfcheck_die("resolved icon word disagrees with the vanilla marker table");
      // Marker slot `slot` draws as sprite `14 - slot`; the vanilla number
      // glyph lives at kOverworldMapData[sprite - 8]. Pendants have none.
      uint8 want_num = (prize >= kPrize_Crystal1) ? kOverworldMapData[6 - slot] : 0;
      if (num_char != want_num)
        selfcheck_die("resolved crystal number glyph disagrees with the vanilla table");

      // (3) Every GATE of the real resolver, driven directly. Each of these
      // survived a mutation of the shipped code before this block existed.
      uint16 t2 = 0xDEAD; uint8 n2 = 0xEE;
      if (OwMapResolveMarker(k, slot, /*shuffled=*/false, identity, kAllObserved,
                             &t2, &n2))
        selfcheck_die("prize_shuffle off must fall through to the vanilla marker");
      if (t2 != 0xDEAD || n2 != 0xEE)
        selfcheck_die("vanilla fall-through must not touch the caller's outputs");
      // Unobserved -> the blinking unknown form, never the placed prize.
      if (!OwMapResolveMarker(k, slot, true, identity, /*observed=*/0, &t2, &n2) ||
          t2 != 0 || n2 != 0)
        selfcheck_die("an unobserved prize must resolve to the unknown marker");
      // Observing a DIFFERENT dungeon must not reveal this one.
      if (!OwMapResolveMarker(k, slot, true, identity,
                              (uint16)~(1u << dungeon), &t2, &n2) ||
          t2 != 0 || n2 != 0)
        selfcheck_die("another dungeon's prize must not reveal this marker");
      // A NULL assignment, or an out-of-range prize id, stays unknown.
      if (!OwMapResolveMarker(k, slot, true, NULL, kAllObserved, &t2, &n2) ||
          t2 != 0 || n2 != 0)
        selfcheck_die("a missing assignment must resolve to the unknown marker");
      uint8 bogus[kRandoDungeonCount];
      memset(bogus, 0xEE, sizeof(bogus));
      if (!OwMapResolveMarker(k, slot, true, bogus, kAllObserved, &t2, &n2) ||
          t2 != 0 || n2 != 0)
        selfcheck_die("an out-of-range prize id must resolve to the unknown marker");
    }
  }
  // 3 pendant dungeons + the first-dark-world marker + 7 crystal dungeons.
  if (prize_slots != 11)
    selfcheck_die("unexpected prize marker slot count");

  // (4) With the randomizer inactive the wrapper must decline without touching
  // the caller's outputs — this is what keeps the vanilla pause map (and the
  // side-by-side ROM RAM compare) byte-identical. The rest of the wrapper is a
  // deliberately thin state-gathering shim over the core tested above: it reads
  // the active settings (failing closed to "shuffled" when unrecoverable) and
  // builds the observed mask from Rando_IsLocationChecked. Driving those two
  // would need an installed slot; they are audited by eye instead.
  {
    uint32 saved = enhanced_features1;
    enhanced_features1 = saved & ~(uint32)kFeatures1_RandomizerActive;
    uint16 t = 0xDEAD;
    uint8 n = 0xEE;
    bool owned = RandoOwMap_PrizeMarker(kOwMapState_Crystals, 0, &t, &n);
    enhanced_features1 = saved;
    if (owned || t != 0xDEAD || n != 0xEE)
      selfcheck_die("an inactive randomizer must leave the vanilla marker untouched");
  }

  // (5) Every prize id must resolve to a DISTINCT (icon, number) pair, so no
  // two prizes can render identically — the failure mode a per-slot identity
  // comparison alone would miss for crystals (they share one icon word).
  for (uint8 a = 0; a < kPrizeCount; a++) {
    for (uint8 b = (uint8)(a + 1); b < kPrizeCount; b++) {
      uint8 na = (a >= kPrize_Crystal1) ? kCrystalNumChar[a - kPrize_Crystal1] : 0;
      uint8 nb = (b >= kPrize_Crystal1) ? kCrystalNumChar[b - kPrize_Crystal1] : 0;
      if (kPrizeTab[a] == kPrizeTab[b] && na == nb)
        selfcheck_die("two prizes render identically on the pause map");
    }
  }

  // Routing markers must stay vanilla: the pedestal shares state 3 with the
  // pendant dungeons, and states 0/1/2/4/5/8 carry no prize claim at all.
  if (PrizeDungeonForSlot(kOwMapState_Pendants, 3) != kRandoDungeon_None)
    selfcheck_die("Master Sword pedestal slot must not be a prize marker");
  static const uint8 kRoutingStates[6] = {0, 1, 2, 4, 5, 8};
  for (int si = 0; si < 6; si++) {
    for (uint8 slot = 0; slot < kOwMapMarkerSlots; slot++) {
      if (PrizeDungeonForSlot(kRoutingStates[si], slot) != kRandoDungeon_None)
        selfcheck_die("routing map-icon state must not carry a prize marker");
    }
  }

  // Every prize dungeon owns exactly one marker slot across the prize states,
  // so no dungeon can be silently dropped or double-drawn. PoD legitimately
  // appears twice (states 6 and 7) — one marker each, never both at once,
  // because the state IS the selector.
  for (uint8 d = 0; d < kRandoDungeonCount; d++) {
    uint16 loc = Rando_GetDungeonPrizeLocation(d);
    if (loc == 0xFFFF)
      continue;  // no prize location: HCE / HCT / GT
    int seen = 0;
    for (uint8 slot = 0; slot < kOwMapMarkerSlots; slot++) {
      if (PrizeDungeonForSlot(kOwMapState_Pendants, slot) == d) seen++;
      if (PrizeDungeonForSlot(kOwMapState_Crystals, slot) == d) seen++;
    }
    if (seen != 1)
      selfcheck_die("each prize dungeon needs exactly one pause-map marker slot");
  }

  fprintf(stderr, "[RandoOwMap_SelfCheck] OK\n");
}
