// add-rando-ow-warp-shuffle — per-seed warp-layout derivation (design D6).
// See shuffle_ow_warp.h. Everything here is pure in (seed, attempt,
// settings) on the shared rando xoshiro stream; no wall clock, no platform
// variance (the placer-determinism contract).

#include "shuffle_ow_warp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../assets.h"
#include "rando.h"
#include "rando_logic.h"
#include "rando_rng.h"
#include "shuffle_entrance.h"  // Entrance_AddedEdgeWorstCase (budget selfcheck)

// Vanilla flute-map blip tables (messaging.c, exported for the rando path +
// the oracle — single source, no drift).
extern const uint8 kBirdTravel_x_lo[8], kBirdTravel_x_hi[8];
extern const uint8 kBirdTravel_y_lo[8], kBirdTravel_y_hi[8];

// Per-axis salts keep warp draws independent of the entrance/door/prize
// streams at the same (seed, attempt).
#define kOwWarpSaltFlute 0xF1073B1Full
#define kOwWarpSaltWhirlpool 0x3A9BC0DEull

static bool ow_conflicts(uint32 a, uint32 b) {
  return (kRandoOwFluteConflicts[a][b >> 5] >> (b & 31)) & 1u;
}

// Ported upstream flute selection (FluteShuffle.py shuffle_flute_spots):
// forced entries always join; remaining picks draw uniformly, preferring
// candidates in sectors that still lack a spot while empty sectors remain;
// `balanced` additionally rejects proximity-conflicting picks with the
// upstream 1-in-32 escape (randint(0,31) != 0 keeps the rejection).
static void ow_pick_flute(RandoRng *rng, uint8 mode,
                          uint8 out[kOwWarpFluteSlots]) {
  const uint32 n = kRandoOwFluteCandidatesCount;
  uint8 used[64];  // candidate taken
  uint8 sector_has[64];
  memset(used, 0, sizeof(used));
  memset(sector_has, 0, sizeof(sector_has));
  uint32 picked = 0;

  for (uint32 i = 0; i < n && picked < kOwWarpFluteSlots; i++) {
    if (kRandoOwFluteCandidates[i].forced) {
      used[i] = 1;
      sector_has[kRandoOwFluteCandidates[i].sector & 63] = 1;
      out[picked++] = (uint8)i;
    }
  }

  while (picked < kOwWarpFluteSlots) {
    // Candidate pool: unsectored preference first (sector lacking a spot),
    // falling back to any unused candidate.
    uint8 pool[64];
    uint32 pool_n = 0;
    for (int pass = 0; pass < 2 && pool_n == 0; pass++) {
      for (uint32 i = 0; i < n; i++) {
        if (used[i]) continue;
        if (pass == 0 &&
            sector_has[kRandoOwFluteCandidates[i].sector & 63])
          continue;
        pool[pool_n++] = (uint8)i;
      }
    }
    if (pool_n == 0) break;  // exhausted (asserted impossible by selfcheck)
    uint32 pick = pool[Rng_NextRange(rng, pool_n)];
    if (mode == kFluteShuffle_Balanced) {
      bool conflict = false;
      for (uint32 s = 0; s < picked; s++) {
        if (ow_conflicts(pick, out[s])) { conflict = true; break; }
      }
      // Upstream escape: randint(0,31) != 0 keeps the rejection — i.e. a
      // conflicting pick still lands 1 time in 32. Draw the escape roll
      // ONLY for conflicting picks (stream parity with the port).
      if (conflict && Rng_NextRange(rng, 32) != 0) continue;
    }
    used[pick] = 1;
    sector_has[kRandoOwFluteCandidates[pick].sector & 63] = 1;
    out[picked++] = (uint8)pick;
  }
  // Menu slot order is ascending screen id (stable, seed-independent
  // presentation; the SET is the shuffled entity).
  for (uint32 a = 0; a + 1 < picked; a++)
    for (uint32 b = a + 1; b < picked; b++)
      if (kRandoOwFluteCandidates[out[b]].screen <
          kRandoOwFluteCandidates[out[a]].screen) {
        uint8 t = out[a]; out[a] = out[b]; out[b] = t;
      }
}

static void ow_pick_whirlpools(RandoRng *rng,
                               uint8 out[kOwWarpWhirlpoolMax]) {
  const uint32 n = kRandoOwWhirlpoolsCount;
  uint8 lw[kOwWarpWhirlpoolMax];
  uint32 lw_n = 0;
  for (uint32 i = 0; i < n && i < kOwWarpWhirlpoolMax; i++) {
    out[i] = (uint8)i;  // identity default (DW pair + inactive)
    if (kRandoOwWhirlpools[i].screen < 0x40) lw[lw_n++] = (uint8)i;
  }
  // Uniform perfect matching over the LW indices: Fisher-Yates shuffle,
  // pair consecutive entries.
  for (uint32 i = lw_n - 1; i > 0; i--) {
    uint32 j = Rng_NextRange(rng, i + 1);
    uint8 t = lw[i]; lw[i] = lw[j]; lw[j] = t;
  }
  for (uint32 i = 0; i + 1 < lw_n; i += 2) {
    out[lw[i]] = lw[i + 1];
    out[lw[i + 1]] = lw[i];
  }
}

bool OwWarp_Compute(const RandoSettings *settings, uint64 seed,
                    uint32 attempt, OwWarpLayout *out) {
  memset(out, 0, sizeof(*out));
  for (uint32 i = 0; i < kOwWarpWhirlpoolMax; i++) out->wp_partner[i] = (uint8)i;
  if (settings == NULL) return true;
  // Effective, not raw: Inverted forces both axes off (v1 scope) and only these
  // accessors re-derive it on the un-normalized struct generation actually
  // carries. Gating here as well as at the two callers keeps this funnel safe
  // for any future caller — an Inverted request must return the identity layout
  // rather than reach the fail-closed refusal below.
  bool want_flute = Settings_EffectiveFluteShuffle(settings) != kFluteShuffle_Off;
  bool want_wp = Settings_EffectiveWhirlpoolShuffle(settings);
  if (!want_flute && !want_wp) return true;
  if (!kRandoOwGraphPresent || kRandoOwFluteCandidatesCount == 0 ||
      kRandoOwWhirlpoolsCount == 0) {
    fprintf(stderr, "OwWarp_Compute: warp axis requested but the OW graph "
                    "tables are absent/empty — refusing (fail-closed)\n");
    return false;
  }
  if (want_flute) {
    RandoRng rng;
    Rng_SeedFromU64(&rng, seed ^ ((uint64)attempt * 0x9E3779B97F4A7C15ull) ^
                              kOwWarpSaltFlute);
    ow_pick_flute(&rng, Settings_EffectiveFluteShuffle(settings),
                  out->flute_cand);
    out->active |= 1;
  }
  if (want_wp) {
    RandoRng rng;
    Rng_SeedFromU64(&rng, seed ^ ((uint64)attempt * 0x9E3779B97F4A7C15ull) ^
                              kOwWarpSaltWhirlpool);
    ow_pick_whirlpools(&rng, out->wp_partner);
    out->active |= 2;
  }
  return true;
}

uint32 OwWarp_Digest24(const OwWarpLayout *l) {
  uint32 h = 0x811C9DC5u;
  h = (h ^ l->active) * 0x01000193u;
  for (uint32 i = 0; i < kOwWarpFluteSlots; i++)
    h = (h ^ l->flute_cand[i]) * 0x01000193u;
  for (uint32 i = 0; i < kOwWarpWhirlpoolMax; i++)
    h = (h ^ l->wp_partner[i]) * 0x01000193u;
  return (h ^ (h >> 24)) & 0xFFFFFFu;
}

void OwWarp_InstallLogicEdges(const OwWarpLayout *l) {
  // Compose on whatever overlay pass ran this attempt (entrance/chains), or
  // own the overlay when warp runs alone — the ApplyDecoupledExitEdges
  // precedent (callers Clear per attempt; the entrance retry-accumulation
  // lesson).
  if (!Rando_EntranceEdgeOverridesActive()) Rando_BeginEntranceEdgeOverrides();
  if (l->active & 1) {
    for (uint32 s = 0; s < kOwWarpFluteSlots; s++) {
      const RandoOwFluteCandidate *c =
          &kRandoOwFluteCandidates[l->flute_cand[s]];
      // Unconditional: the hub feeders carry the flute possession +
      // activation predicate (CanUseFluteTravel).
      Rando_AddEntranceEdge(kRandoOwFluteHubRegion, c->region, 0, 0);
    }
  }
  if (l->active & 2) {
    for (uint32 i = 0; i < kRandoOwWhirlpoolsCount &&
                       i < kOwWarpWhirlpoolMax; i++) {
      uint8 p = l->wp_partner[i];
      if (p == i) continue;  // DW identity / self
      if (p < i) continue;   // emit each pair once, both directions below
      // Unconditional ride edges: the zone->water ENTRY edges are
      // Flippers-gated, so being at a whirlpool component implies Flippers.
      // tracker-player-knowledge — tagged with the FROM index so the masked
      // live view hides the pair until ridden (one ride marks both indices —
      // the matching is an involution). Inert for generation (no mask).
      Rando_AddEntranceEdgeTagged(kRandoOwWhirlpools[i].region,
                                  kRandoOwWhirlpools[p].region, 0, 0,
                                  (uint8)(kKnowledgeTag_Whirlpool | i));
      Rando_AddEntranceEdgeTagged(kRandoOwWhirlpools[p].region,
                                  kRandoOwWhirlpools[i].region, 0, 0,
                                  (uint8)(kKnowledgeTag_Whirlpool | p));
    }
  }
}

// tracker-player-knowledge — table-index bits of whirlpools participating in a
// SHUFFLED (non-identity) pair on this layout. Identity-rolled and DW-identity
// entries draw no pair edge at all (see the p==i skip above), so they need no
// hiding. 0 when the whirlpool axis is off.
uint8 OwWarp_ShuffledWhirlpoolMask(const OwWarpLayout *l) {
  if (l == NULL || !(l->active & 2)) return 0;
  uint8 mask = 0;
  for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax; i++) {
    if (l->wp_partner[i] != i) mask |= (uint8)(1u << i);
  }
  return mask;
}

uint8 OwWarp_WhirlpoolLandingIndex(const OwWarpLayout *l, uint8 entered_idx) {
  if (entered_idx >= kRandoOwWhirlpoolsCount ||
      entered_idx >= kOwWarpWhirlpoolMax)
    return entered_idx;
  uint8 mu = l ? l->wp_partner[entered_idx] : entered_idx;
  // Identity-mu = NOT SHUFFLED = vanilla behavior: load the ENTERED row,
  // whose +9 tableau arrives at the VANILLA PARTNER. (Audit H1: the old
  // uniform formula below self-looped identity entries — it loads
  // vanilla_partner(mu)+9 which arrives at mu, i.e. "arrive where you
  // entered" — killing both DW whirlpools on every whirlpool-shuffle seed,
  // and the old selfcheck asserted the self-loop as correct.)
  if (mu == entered_idx) return entered_idx;
  // Shuffled: the engine's bird-row m+9 encodes "arrive at
  // vanilla_partner(m)"; to land at mu we load vanilla_partner(mu)'s row
  // (involution symmetry gives the two-way property).
  uint8 partner_screen = kRandoOwWhirlpools[mu].partner_screen;
  for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax;
       i++) {
    if (kRandoOwWhirlpools[i].screen == partner_screen) return (uint8)i;
  }
  return entered_idx;  // unreachable with a coherent table (selfchecked)
}

// --- runtime hooks ----------------------------------------------------------

static const OwWarpLayout *active_flute_layout(void) {
  const OwWarpLayout *l = Rando_ActiveOwWarpLayout();
  return (l != NULL && (l->active & 1)) ? l : NULL;
}

const RandoOwFluteCandidate *Rando_OwWarp_FluteSpotForBirdIndex(int k) {
  const OwWarpLayout *l = active_flute_layout();
  if (l == NULL || k < 0 || k >= kOwWarpFluteSlots) return NULL;
  return &kRandoOwFluteCandidates[l->flute_cand[k]];
}

bool Rando_OwWarp_FluteBlip(uint8 slot, uint16 *x, uint16 *y) {
  const RandoOwFluteCandidate *c =
      Rando_OwWarp_FluteSpotForBirdIndex((int)slot);
  if (c == NULL) return false;
  // Vanilla-8 screens: the engine's hand-tuned blip verbatim (the positions
  // are cosmetic curation, not a formula — measured deltas vs link/icon
  // coords are irregular, so lookup is the only exact source).
  for (int v = 0; v < 8; v++) {
    if (kRandoOwVanillaFluteScreens[v] == c->screen) {
      *x = (uint16)((kBirdTravel_x_hi[v] << 8) | kBirdTravel_x_lo[v]);
      *y = (uint16)((kBirdTravel_y_hi[v] << 8) | kBirdTravel_y_lo[v]);
      return true;
    }
  }
  // Shuffled spot on a non-vanilla screen: center on the landing.
  *x = c->link_x;
  *y = (uint16)(c->link_y - 8);
  return true;
}

uint8 Rando_OwWarp_WhirlpoolBirdRow(uint8 entered_row) {
  const OwWarpLayout *l = Rando_ActiveOwWarpLayout();
  if (l == NULL || !(l->active & 2)) return entered_row;
  if ((const void *)kWhirlpoolAreas == NULL) return entered_row;
  uint32 rows = kWhirlpoolAreas_SIZE / 2;
  if (entered_row >= rows) return entered_row;
  uint16 entered_screen = kWhirlpoolAreas[entered_row];
  // asset row -> generated table index (screen-keyed).
  uint8 tbl = 0xFF;
  for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax;
       i++) {
    if (kRandoOwWhirlpools[i].screen == entered_screen) { tbl = (uint8)i; break; }
  }
  if (tbl == 0xFF) return entered_row;  // unknown whirlpool: vanilla
  uint8 land_tbl = OwWarp_WhirlpoolLandingIndex(l, tbl);
  // tracker-player-knowledge — the player is riding this pair now; the
  // matching is an involution, so one ride reveals both directions. This
  // hook's sole caller is the actual travel (no pre-travel display).
  //
  // Mark the PER-SEED PARTNER wp_partner[tbl] — NOT `land_tbl`. The edge tags
  // installed above are keyed (tag k <=> edge region[k] -> region[wp_partner[k]]),
  // whereas OwWarp_WhirlpoolLandingIndex deliberately returns the bird-ROW
  // SOURCE vanilla_partner(mu), a different index. Marking land_tbl cleared
  // the wrong bit: it un-hid an un-ridden pair's edge (a real leak) while
  // leaving the just-ridden return trip hidden.
  Rando_MarkWhirlpoolPairDiscovered(tbl, l->wp_partner[tbl]);
  uint16 land_screen = kRandoOwWhirlpools[land_tbl].screen;
  for (uint32 j = 0; j < rows; j++) {
    if (kWhirlpoolAreas[j] == land_screen) return (uint8)j;
  }
  return entered_row;  // coherence guaranteed by the selfcheck below
}

void OwWarp_SelfCheck(void) {
#define OWSC(cond, msg)                                              \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "OwWarp_SelfCheck: %s\n", msg);                \
      exit(2);                                                       \
    }                                                                \
  } while (0)

  // Inverted force-off (randomizer-ow-shuffle: "no warp layout is generated or
  // installed"). apply_derived_rules only normalizes the private copy
  // Settings_CanonicalSerialize takes, so this asserts the EFFECTIVE-accessor
  // path the raw struct actually travels.
  //
  // Deliberately ABOVE the graph-present gate: it needs no graph (that is the
  // point -- Inverted short-circuits before the graph check), and public CI
  // builds with no graph are exactly where the regression surfaces, as a red
  // corpus row. Below the gate this would be skipped on the only checkout that
  // runs it.
  //
  // Asserted here rather than left to the corpus: the manifest's inverted
  // twin-row pair is only a tripwire on seeds where the axes actually move
  // placement, and the pinned seed is not one -- it stayed byte-identical
  // through the whole period the axes leaked.
  {
    RandoSettings inv;
    Settings_SetDefaults(&inv);
    inv.world_state = kWorldState_Inverted;
    inv.flute_shuffle = kFluteShuffle_Random;
    inv.whirlpool_shuffle = 1;
    OWSC(Settings_OwWarpForcedOff(&inv), "Inverted must force the warp axes off");
    OWSC(!Settings_OwWarpActive(&inv),
         "Inverted+both-axes must not read as an active warp seed");
    OwWarpLayout iv;
    // Must SUCCEED (identity layout), not hit the missing-graph refusal --
    // that refusal firing here is exactly the assetless CI red this guards.
    OWSC(OwWarp_Compute(&inv, 0xC0FFEEu, 3, &iv),
         "Inverted warp compute must return the identity layout, not refuse");
    OWSC(iv.active == 0, "Inverted must not produce an active warp layout");
    for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax;
         i++)
      OWSC(iv.wp_partner[i] == (uint8)i,
           "Inverted whirlpools must stay vanilla (identity)");
  }

  if (!kRandoOwGraphPresent) {
    fprintf(stderr,
            "[OwWarp_SelfCheck] Inverted force-off OK; rest SKIPPED (graph "
            "absent)\n");
    return;
  }
  // audit-2 LOW-4: the selection buffers (used/sector_has/pool) are sized
  // 64; a future upstream flute_data expansion past that would overflow
  // silently without this loud bound.
  OWSC(kRandoOwFluteCandidatesCount <= 64,
       "flute candidate table exceeds the 64-entry selection buffers");
  RandoSettings s;
  Settings_SetDefaults(&s);
  s.flute_shuffle = kFluteShuffle_Random;
  s.whirlpool_shuffle = 1;
  OwWarpLayout a, b;
  OWSC(OwWarp_Compute(&s, 0xC0FFEEu, 3, &a), "compute failed");
  OWSC(OwWarp_Compute(&s, 0xC0FFEEu, 3, &b), "recompute failed");
  OWSC(memcmp(&a, &b, sizeof(a)) == 0, "layout not deterministic");
  OWSC(OwWarp_Digest24(&a) == OwWarp_Digest24(&b), "digest not stable");
  OwWarpLayout c;
  OWSC(OwWarp_Compute(&s, 0xC0FFEEu, 4, &c), "attempt-4 compute failed");
  // (Distinct attempts may legitimately collide on small pools; only
  // determinism is asserted.)
  // Forced guarantee: the forced candidate (Desert/Mire) is always chosen.
  int forced_idx = -1;
  for (uint32 i = 0; i < kRandoOwFluteCandidatesCount; i++)
    if (kRandoOwFluteCandidates[i].forced) forced_idx = (int)i;
  OWSC(forced_idx >= 0, "no forced candidate in the table");
  for (uint32 t = 0; t < 16; t++) {
    OwWarpLayout l;
    OWSC(OwWarp_Compute(&s, 0x1234u + t, 0, &l), "sweep compute failed");
    bool has_forced = false;
    for (uint32 k = 0; k < kOwWarpFluteSlots; k++) {
      if (l.flute_cand[k] == (uint8)forced_idx) has_forced = true;
      for (uint32 m = k + 1; m < kOwWarpFluteSlots; m++)
        OWSC(l.flute_cand[k] != l.flute_cand[m], "duplicate flute spot");
    }
    OWSC(has_forced, "forced Desert/Mire spot missing from a layout");
    // Whirlpool involution + DW identity.
    for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax;
         i++) {
      OWSC(l.wp_partner[l.wp_partner[i]] == i, "matching not an involution");
      if (kRandoOwWhirlpools[i].screen >= 0x40)
        OWSC(l.wp_partner[i] == i, "DW whirlpool not identity");
    }
    // Landing-row semantics (audit-H1-corrected): identity entries load the
    // ENTERED row (arrival = VANILLA partner — unshuffled behavior);
    // shuffled entries load the row whose +9 tableau arrives at mu.
    for (uint32 i = 0; i < kRandoOwWhirlpoolsCount && i < kOwWarpWhirlpoolMax;
         i++) {
      uint8 land = OwWarp_WhirlpoolLandingIndex(&l, (uint8)i);
      if (l.wp_partner[i] == (uint8)i) {
        OWSC(land == (uint8)i,
             "identity whirlpool must load its own (vanilla) row");
      } else {
        OWSC(kRandoOwWhirlpools[land].partner_screen ==
                 kRandoOwWhirlpools[l.wp_partner[i]].screen,
             "landing row does not arrive at the per-seed partner");
        // tracker-player-knowledge — the ride hook must mark the PER-SEED
        // PARTNER (matching the installed edge tags), never this landing
        // row. Pin the two apart on at least one shuffled entry so a future
        // "simplification" back to land_tbl fails here instead of silently
        // un-hiding an un-ridden pair. (They coincide only when
        // vanilla_partner(mu) == mu, which the table never has.)
        OWSC(land != l.wp_partner[i] ||
                 kRandoOwWhirlpools[l.wp_partner[i]].partner_screen ==
                     kRandoOwWhirlpools[l.wp_partner[i]].screen,
             "landing row must not be conflated with the per-seed partner");
      }
    }
  }
  // Vanilla-8 tableau oracle: every vanilla flute screen's candidate row
  // must byte-match the engine's kBirdTravel_* asset row for that screen —
  // pinning the upstream-column -> engine-field mapping to engine truth.
  // --rando-selftest runs BEFORE LoadAssets (and public CI is assetless),
  // so the asset-backed oracle skips gracefully when pointers are NULL;
  // any asset-present run (local dev, corpus, release CI) exercises it.
  if ((const void *)kBirdTravel_ScreenIndex == NULL) {
    fprintf(stderr, "[OwWarp_SelfCheck] tableau oracle SKIPPED (assets not "
                    "loaded)\n");
  } else
  for (int v = 0; v < 8; v++) {
    uint8 scr = kRandoOwVanillaFluteScreens[v];
    const RandoOwFluteCandidate *c = NULL;
    for (uint32 i = 0; i < kRandoOwFluteCandidatesCount; i++) {
      if (kRandoOwFluteCandidates[i].screen == scr) {
        c = &kRandoOwFluteCandidates[i];
        break;
      }
    }
    OWSC(c != NULL, "vanilla flute screen has no candidate row");
    int k = -1;
    for (int i = 0; i < 8; i++) {
      if (kBirdTravel_ScreenIndex[i] == scr) { k = i; break; }
    }
    OWSC(k >= 0, "vanilla flute screen not in kBirdTravel_ScreenIndex[0..7]");
    OWSC(c->vram == kBirdTravel_Map16LoadSrcOff[k] &&
             c->bg_y == kBirdTravel_ScrollY[k] &&
             c->bg_x == kBirdTravel_ScrollX[k] &&
             c->link_y == kBirdTravel_LinkYCoord[k] &&
             c->link_x == kBirdTravel_LinkXCoord[k] &&
             c->cam_y == kBirdTravel_CameraYScroll[k] &&
             c->cam_x == kBirdTravel_CameraXScroll[k] &&
             c->unk1 == (uint16)kBirdTravel_Unk1[k] &&
             c->unk2 == (uint16)kBirdTravel_Unk3[k],
         "vanilla flute tableau mismatch vs kBirdTravel assets — the "
         "upstream-column mapping drifted");
  }
  // Overlay budget: install a maximal layout on a fresh overlay and check
  // headroom (converts the historical silent-drop cliff into a loud check).
  Rando_BeginEntranceEdgeOverrides();
  OwWarpLayout ml;
  OWSC(OwWarp_Compute(&s, 0xBEEFu, 0, &ml), "budget compute failed");
  OwWarp_InstallLogicEdges(&ml);
  int added = Rando_GetEntranceAddedEdgeCount();
  OWSC(added > 0 && added <= 20, "warp overlay edge count out of range");
  // Combined-consumer budget (randomizer-logic spec): the worst case every
  // entrance mode can add plus this maximal warp overlay must fit the shared
  // store, and the store must not have dropped an edge while installing.
  OWSC(Entrance_AddedEdgeWorstCase() + added <=
           Rando_EntranceAddedEdgeCapacity(),
       "combined entrance+warp added-edge budget exceeded");
  OWSC(Rando_GetEntranceAddedEdgeDropped() == 0,
       "added-edge store dropped edges during warp install");
  Rando_ClearEntranceEdgeOverrides();
#undef OWSC
  fprintf(stderr, "[OwWarp_SelfCheck] OK\n");
}
