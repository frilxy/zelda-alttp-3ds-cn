// rando_generate.{h,c} — Rando_GenerateSlot(): the shared, UI-agnostic
// playable-slot generation path (Phase P3).
//
// This is a PURE RELOCATION of the body of SelectFile_Settings_HandleGenerate
// (src/select_file.c), parameterized so both the in-game settings screen and
// (later) the native settings window call the same code. There is NO behavior
// change vs. the in-game flow and NO change to seed determinism.
//
// This module is plain C and lives in src/rando/, so it is compiled on Switch
// too (the in-game screen calls Rando_GenerateSlot there). It uses ONLY
// rando-core + g_config + game-side slot helpers; it has NO window/ImGui deps.
#ifndef ZELDA3_RANDO_RANDO_GENERATE_H_
#define ZELDA3_RANDO_RANDO_GENERATE_H_

#include "../types.h"
#include "rando_settings.h"
#include "rando_share.h"     // kShareStringBase32MaxLen
#include "rando_placement.h"  // RandoPlacementTable
#include "rando_hints.h"      // RandoHintPlan
#include "seed_shape.h"       // SeedShapeFilter / metrics
#include "shuffle_chains.h"   // DungeonChainsLayout
#include "shuffle_entrance.h"  // kEntranceMaxInteriors

struct RandoSpoiler;  // fwd (full def in rando_spoiler.h)

// The accepted entrance/chain generation shape for one generation: entrance
// axis metadata plus every per-category assignment array, and the accepted
// dungeon-chain layout when that axis is active. Filled by
// Rando_PlaceWithEntrances.
typedef struct RandoEntranceRegen {
  uint8 entrance_axes;
  uint8 entrance_attempt;
  uint8 cave_assign[kEntranceMaxInteriors];            int cave_count;
  uint8 dun_assign[kEntranceMaxInteriors];             int dun_count;
  uint8 cross_assign[kEntranceMaxInteriors];           int cross_count;
  uint8 decoupled_assign[kEntranceMaxInteriors];       int decoupled_count;
  uint8 dun_decoupled_assign[kEntranceMaxInteriors];   int dun_decoupled_count;
  uint8 cross_decoupled_assign[kEntranceMaxInteriors]; int cross_decoupled_count;
  bool chains_active;
  uint8 chains_attempt;
  DungeonChainsLayout chains_layout;
} RandoEntranceRegen;

// Shared placement + entrance regeneration, used by BOTH Rando_GenerateSlot and
// the race-mode reveal path so they produce a byte-identical spoiler (incl. the
// entrance_mapping section that feeds the SHA-256 stamp — without this, revealing
// a race-mode + entrance-shuffle seed always false-fails as "tampered").
//
// Runs the entrance-attempt loop (or a single accessibility-gated
// Place_AssumedFill when no entrance axis is active), applying the accepted
// permutation's logic overrides. On success the overrides are LEFT ACTIVE so the
// caller's sphere/goal computation sees the shuffled reachability; the caller
// MUST then EITHER clear them (Entrance_Clear{Region,Edge}Overrides — what
// Rando_GenerateSlot does) OR ensure they end up matching whatever overlay should
// be active afterward. The race-mode reveal path (Rando_RevealSpoiler) relies on
// the latter: it does NOT clear, because for an active entrance slot the re-derived
// π is identical and thus restores the slot's own overrides — but that is only
// safe because reveal is either process-exit (CLI) or share-string-matched to the
// active slot. A new caller that reveals an UNMATCHED slot while a different
// entrance slot is active MUST clear + re-install the active slot's overlay.
// Fills `table` and `*reg`; returns true on success. Deterministic from
// (settings, seed_u64) for a given budget (race-mode generates with budget 0,
// matching the reveal path).
bool Rando_PlaceWithEntrances(const RandoSettings *settings, uint64 seed_u64,
                              int budget_seconds, RandoPlacementTable *table,
                              RandoEntranceRegen *reg);

// Point a RandoSpoiler's entrance_mapping/dungeon_chains fields at `reg`
// (NULL when a section is inactive, per the serializer's omit-empty contract).
// Shared so the generate and reveal call sites can never drift in which
// sections they emit.
void Rando_SpoilerSetEntranceFields(struct RandoSpoiler *spoiler,
                                    const RandoEntranceRegen *reg);

// add-rando-door-shuffle — the door_attempt + 24-bit layout digest of the
// ACCEPTED door layout from the last successful Rando_PlaceWithEntrances door
// phase (zero when door shuffle was off). Persisted at sidecar @76-79 so
// activation can regenerate + drift-check.
void Rando_GetDoorGeneration(uint8 *attempt_out, uint32 *digest24_out);
// add-rando-ow-warp-shuffle — accepted warp attempt + layout digest24 (0/0
// when no warp axis was active), consumed at slot assembly (door pattern).
void Rando_GetOwWarpGeneration(uint8 *attempt_out, uint32 *digest_out);
typedef struct OwWarpLayout OwWarpLayout;
const OwWarpLayout *Rando_GetOwWarpGenerationLayout(void);

// Clears every per-generation logic overlay Rando_PlaceWithEntrances may have
// left installed (entrance region/edge overrides + the door logic layout).
// Rando_GenerateSlot calls this at every exit after its sphere/goal/spoiler
// work; DoorShuffle_SelfTest exercises it in the cross-generation leak
// regression check. Does NOT touch slot-activation overlays — activation
// installs its own regenerated copies after this runs.
void Rando_ClearGenerationLogicOverlays(void);

typedef struct RandoGenerateShapeOptions {
  const SeedShapeFilter *filter;  // NULL or disabled => no shape search
  int search_limit;               // candidate seeds to try when filter is enabled
} RandoGenerateShapeOptions;

typedef struct RandoGenerateResult {
  bool ok;
  bool used_forward_fill;
  bool goal_completable;
  bool race_mode;
  uint64 seed_u64;  // accepted seed; may differ from input when shape search is used
  char share_string[kShareStringBase32MaxLen];
  uint8 settings_hash[32];
  bool has_medallion_assignment;
  uint8 medallion_assignment[kRandoMedallionEntranceCount];
  bool shape_filter_used;
  uint32 shape_attempts_used;
  int shape_search_limit;
  SeedShapeMetrics shape_metrics;
  bool has_spheres;
  RandoSpheres spheres;
  bool has_hint_plan;
  RandoHintPlan hint_plan;
  RandoPlacementTable placement;  // OWNED malloc'd copy when requested (caller frees); {0} otherwise
} RandoGenerateResult;

// Noncanonical deterministic work telemetry for one generator run. These
// counters are for performance diagnosis only and are never serialized into a
// slot, spoiler, share string, race stamp, or placement digest.
typedef struct RandoGenerationWorkCounters {
  uint32 placement_attempts;
  uint32 door_generation_attempts;
  uint64 reachability_expansion_passes;
} RandoGenerationWorkCounters;

void Rando_WorkCountersBeginGeneration(void);
void Rando_WorkCountersEndGeneration(void);
const RandoGenerationWorkCounters *Rando_GetLastGenerationWorkCounters(void);

// Game-thread only. Runs the full playable-slot generation: placement + share +
// spoiler files + sidecar slot write + SRAM commit + recommended-features apply.
// Does NOT call Placement_Install (install is slot-load-only). budget<0 => 0
// (no wall-clock cutoff: the placer runs to its deterministic attempt cap, so
// the placement a share-string receiver regenerates is machine-independent —
// see the Place_AssumedFill DETERMINISM WARNING; explicit positive budgets are
// honored for diagnostic use only). If out!=NULL, out->placement is an
// independently malloc'd copy the CALLER owns. Returns false + err on failure
// (no slot written).
bool Rando_GenerateSlot(const RandoSettings *settings, uint64 seed_u64, int budget,
                        int slot_index, uint32 recommended_features0,
                        RandoGenerateResult *out, char *err, size_t err_cap);

// Same slot generation path with an optional generator-side shape search. Shape
// filters are NOT settings and do not affect the settings hash/share format; on
// success the accepted candidate seed is stored in RandoGenerateResult.seed_u64
// and in the slot/share artifacts.
bool Rando_GenerateSlotWithShapeFilter(const RandoSettings *settings, uint64 seed_u64,
                                       int budget, int slot_index,
                                       uint32 recommended_features0,
                                       const RandoGenerateShapeOptions *shape,
                                       RandoGenerateResult *out,
                                       char *err, size_t err_cap);

// Initializes a freshly-generated rando slot's 0x500-byte SRAM image (vanilla
// new-file defaults + world-state post-escape start state). Writes only into
// target_sram[0..0x4ff]; no other side effects. Shared by Rando_GenerateSlot
// and RandoGenerate_SelfCheck.
void Rando_InitNewSlotSram(uint8 *target_sram, uint8 world_state);

// Self-test: asserts the world-state start-state SRAM init (the merge-dropped
// non-Standard-boot softlock class). Called by Rando_RunAllSelfChecks; exits(2)
// on failure. Side-effect-free (operates on a scratch buffer).
void RandoGenerate_SelfCheck(void);

extern void SelectFile_NotifySlotWritten(int slot_index);  // defined in select_file.c

#ifdef Z3R_NATIVE_SETTINGS_WINDOW
// Native-window "Load it now" seam (§13.7). Game-thread only; defined in
// select_file.c. Loads the just-generated playable slot exactly as pressing A
// on an occupied file-select slot would (sidecar reload + Rando_ActivateSidecarSlot
// + CopySaveToWRAM). Must run while Module01_FileSelect is active.
extern void SelectFile_LoadRandoSlot(int slot_index);
#endif

#endif  // ZELDA3_RANDO_RANDO_GENERATE_H_
