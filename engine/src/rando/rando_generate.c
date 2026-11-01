// rando_generate.c — see rando_generate.h.
//
// Rando_GenerateSlot is a PURE RELOCATION of the body of
// SelectFile_Settings_HandleGenerate (src/select_file.c), lifted from the
// Settings_ComputeHash step through the SelectFile_ResetSidecarCache +
// selectfile_arr1[slot]=1 step. Parameterized:
//   g_settings_working        -> *settings
//   g_settings_target_slot    -> slot_index
//   g_rec_working_features0    -> recommended_features0
//   budget literal             -> (budget < 0 ? 0 : budget)  // deterministic default
//   SelectFile_ResetSidecarCache()/selectfile_arr1[slot]=1 -> SelectFile_NotifySlotWritten(slot)
// On any failure path it returns false with a message in `err` and frees the
// working `entries`. It does NOT call Placement_Install (install is slot-load-
// only). If out!=NULL, before freeing it malloc+memcpy's an owned placement
// copy into out->placement and fills the scalar result fields.
#include "rando_generate.h"

#include "../zelda_rtl.h"     // g_zenv, ZeldaWriteSram
#include "../config.h"        // g_config (.features0)
#include "../features.h"      // Seed QoL feature mask + JP-glitch coupling
#include "../load_gfx.h"      // kSrmOffs_Name, kSrmOffs_DiedCounter
#include "../select_file.h"   // Intro_FixCksum
#include "rando.h"            // kGeneratorVersion
#include "rando_save.h"       // RandoSidecarSlot, kSlotKind_Randomizer, Rando_WriteSidecarSlot, ...
#include "rando_spoiler.h"    // RandoSpoiler, Spoiler_ResolvePath, Spoiler_Write
#include "rando_hints.h"      // caller-owned RandoHintPlan build + persisted identity
#include "shuffle_doors.h"    // door-shuffle generation (add-rando-door-shuffle)
#include "shuffle_chains.h"   // dungeon-chain layout generation
#include "shuffle_ow_warp.h"  // add-rando-ow-warp-shuffle (per-seed warp layout)

// add-rando-door-shuffle — generation-side layout + accepted-attempt state.
// The layout must outlive Rando_PlaceWithEntrances: the installed logic
// pointer (Rando_SetDoorLogicLayout) references it through the caller's
// sphere/goal computation.
static DoorShuffleLayout g_door_gen_layout;
static uint8 g_door_gen_attempt;

// add-rando-ow-warp-shuffle — generation-side warp layout (door pattern:
// file-scope accepted state + accessor consumed at slot assembly).
static OwWarpLayout g_ow_gen_layout;
static uint8 g_ow_gen_attempt;
static uint32 g_ow_gen_digest24;

void Rando_GetOwWarpGeneration(uint8 *attempt_out, uint32 *digest_out) {
  if (attempt_out) *attempt_out = g_ow_gen_attempt;
  if (digest_out) *digest_out = g_ow_gen_digest24;
}

// Generation-time layout for the spoiler writer (the activation global is
// not populated on the headless path). NULL when no warp axis was active.
const OwWarpLayout *Rando_GetOwWarpGenerationLayout(void) {
  return g_ow_gen_layout.active != 0 ? &g_ow_gen_layout : NULL;
}

// Per-attempt warp application: compute + install (composes on the current
// overlay; Begins it when warp runs alone). False = fail-closed refusal
// (axis requested, graph tables absent) — no attempt can fix that.
static bool ow_warp_apply_attempt(const RandoSettings *settings,
                                  uint64 seed_u64, uint32 att) {
  // Settings_OwWarpActive, not the raw fields: the axes are forced off under
  // Inverted and only the accessor re-derives that (apply_derived_rules
  // normalizes a private copy for the hash, never this struct).
  bool warp_on = Settings_OwWarpActive(settings);
  g_ow_gen_attempt = 0;
  g_ow_gen_digest24 = 0;
  memset(&g_ow_gen_layout, 0, sizeof(g_ow_gen_layout));
  if (!warp_on) return true;
  if (!OwWarp_Compute(settings, seed_u64, att, &g_ow_gen_layout))
    return false;
  OwWarp_InstallLogicEdges(&g_ow_gen_layout);
  g_ow_gen_attempt = (uint8)att;
  g_ow_gen_digest24 = OwWarp_Digest24(&g_ow_gen_layout);
  return true;
}
static uint32 g_door_gen_digest24;
#include "shuffle_entrance.h" // Phase C entrance shuffle (cave permutation + region overrides)
#include "shuffle_boss.h"     // BossShuffle_ComputeAssignment (Slice 7 spoiler)
#include "shuffle_drops.h"    // DropShuffle_ComputeAssignment (Slice 8 spoiler)
#include "customizer.h"       // Customizer_GetActive/_LastError (slot-path pins)

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct GenerateProfileCounters {
  uint64 calls;
  uint64 plain_calls;
  uint64 entrance_calls;
  uint64 entrance_attempts;
  uint64 entrance_successes;
  uint64 door_calls;
  uint64 door_attempts;
  uint64 door_layout_failures;
  uint64 door_rejections;
  uint64 door_successes;
  uint64 chains_calls;
  uint64 chains_attempts;
  uint64 chains_layout_failures;
  uint64 chains_rejections;
  uint64 chains_successes;
  uint32 max_door_attempts;
  uint32 max_chains_attempts;
} GenerateProfileCounters;

static GenerateProfileCounters g_generate_profile;
static bool g_generate_profile_active;
static RandoGenerationWorkCounters g_generation_work_live;
static RandoGenerationWorkCounters g_generation_work_last;
static bool g_generation_work_active;

void Rando_WorkCountersBeginGeneration(void) {
  memset(&g_generation_work_live, 0, sizeof(g_generation_work_live));
  g_generation_work_active = true;
  Logic_WorkCounterBegin();
}

void Rando_WorkCountersEndGeneration(void) {
  if (!g_generation_work_active)
    return;
  g_generation_work_live.reachability_expansion_passes =
      Logic_WorkCounterFreeze();
  g_generation_work_last = g_generation_work_live;
  g_generation_work_active = false;
}

const RandoGenerationWorkCounters *Rando_GetLastGenerationWorkCounters(void) {
  return &g_generation_work_last;
}

static bool rando_place_assumed_fill_counted(const RandoSettings *settings,
                                             uint64 seed_u64,
                                             int budget_seconds,
                                             RandoPlacementTable *table) {
  bool ok = Place_AssumedFill(settings, seed_u64, budget_seconds, table);
  if (g_generation_work_active) {
    uint32 attempts = Placement_GetLastStats()->attempts_used;
    if (UINT32_MAX - g_generation_work_live.placement_attempts < attempts)
      g_generation_work_live.placement_attempts = UINT32_MAX;
    else
      g_generation_work_live.placement_attempts += attempts;
  }
  return ok;
}

static void generate_profile_dump(void) {
  if (!g_generate_profile_active) return;
  fprintf(stderr,
          "[RANDO_PROFILE generate] calls=%llu plain=%llu entrance=%llu "
          "entrance_attempts=%llu entrance_successes=%llu door=%llu "
          "door_attempts=%llu door_layout_failures=%llu door_rejections=%llu "
          "door_successes=%llu max_door_attempts=%u chains=%llu "
          "chains_attempts=%llu chains_layout_failures=%llu chains_rejections=%llu "
          "chains_successes=%llu max_chains_attempts=%u\n",
          (unsigned long long)g_generate_profile.calls,
          (unsigned long long)g_generate_profile.plain_calls,
          (unsigned long long)g_generate_profile.entrance_calls,
          (unsigned long long)g_generate_profile.entrance_attempts,
          (unsigned long long)g_generate_profile.entrance_successes,
          (unsigned long long)g_generate_profile.door_calls,
          (unsigned long long)g_generate_profile.door_attempts,
          (unsigned long long)g_generate_profile.door_layout_failures,
          (unsigned long long)g_generate_profile.door_rejections,
          (unsigned long long)g_generate_profile.door_successes,
          (unsigned)g_generate_profile.max_door_attempts,
          (unsigned long long)g_generate_profile.chains_calls,
          (unsigned long long)g_generate_profile.chains_attempts,
          (unsigned long long)g_generate_profile.chains_layout_failures,
          (unsigned long long)g_generate_profile.chains_rejections,
          (unsigned long long)g_generate_profile.chains_successes,
          (unsigned)g_generate_profile.max_chains_attempts);
}

static void generate_profile_maybe_init(void) {
  static bool initialized;
  if (initialized) return;
  initialized = true;
  const char *v = getenv("ZELDA3_RANDO_PROFILE");
  g_generate_profile_active = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
  if (g_generate_profile_active) atexit(generate_profile_dump);
}

static uint8 door_enemy_drop_keys_for_settings(const RandoSettings *settings) {
  if (settings == NULL ||
      Settings_EffectiveDoorShuffle(settings) == kDoorShuffle_Vanilla)
    return 0;
  return Settings_EnemyDropKeysActive(settings) ? 1 : 0;
}

static uint8 door_enemy_check_tier_for_settings(const RandoSettings *settings) {
  if (settings == NULL ||
      Settings_EffectiveDoorShuffle(settings) == kDoorShuffle_Vanilla)
    return kEnemyDropChecks_Off;
  uint8 tier = Settings_EffectiveEnemyDropChecks(settings);
  return tier >= kEnemyDropChecks_Dungeon ? tier : kEnemyDropChecks_Off;
}

static bool copy_active_medallion_assignment(uint8 out[kRandoMedallionEntranceCount]) {
  const uint8 *assignment = Rando_GetMedallionAssignment();
  if (assignment == NULL) return false;
  memcpy(out, assignment, kRandoMedallionEntranceCount);
  return true;
}

bool Rando_GenerateSlot(const RandoSettings *settings, uint64 seed_u64, int budget,
                        int slot_index, uint32 recommended_features0,
                        RandoGenerateResult *out, char *err, size_t err_cap) {
  return Rando_GenerateSlotWithShapeFilter(settings, seed_u64, budget, slot_index,
                                           recommended_features0, NULL,
                                           out, err, err_cap);
}

// Initializes a freshly-generated rando playable slot's 0x500-byte SRAM image:
// the vanilla "new file" defaults (RANDO name + health/magic baseline) plus the
// world-state-specific post-escape start state. Writes ONLY into
// target_sram[0..0x4ff]; no file/config/global side effects. Factored out of
// Rando_GenerateSlot so the start-state init is unit-testable
// (RandoGenerate_SelfCheck) against a scratch buffer — this exact init was
// silently dropped by a merge once, booting non-Standard slots into the vanilla
// intro and softlocking. Mirrors ALTTPR's initsramtable.asm.
void Rando_InitNewSlotSram(uint8 *target_sram, uint8 world_state) {
  memset(target_sram, 0, 0x500);
  // Pre-name the file "RANDO " to match the rando-banner convention.
  uint16 *name = (uint16 *)(target_sram + kSrmOffs_Name);
  name[0] = 0x21;  // R
  name[1] = 0x00;  // A
  name[2] = 0x0d;  // N
  name[3] = 0x03;  // D
  name[4] = 0x0e;  // O
  name[5] = 0xa9;  // blank
  WORD(target_sram[0x3e5]) = 0x55aa;
  WORD(target_sram[0x20c]) = 0xf000;
  WORD(target_sram[0x20e]) = 0xf000;
  WORD(target_sram[kSrmOffs_DiedCounter]) = 0xffff;
  // Replicate the new-file init from NameFile_DoTheNaming so the slot has the
  // canonical starting state — without this, health bytes stayed zero and the
  // rando slot loaded as instant-death (0/0 hearts). Offsets +44/+45 = starting
  // + max health (0x18 = 3 hearts in quarter-heart units); +57 = item baseline.
  static const uint8 kSramInit_Normal[60] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0,    0,    0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0,    0, 0, 0, 0x18, 0x18, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0xf8, 0, 0,
  };
  memcpy(target_sram + 0x340, kSramInit_Normal, 60);

  // Non-Standard world-state starting SRAM. The placer pre-grants RescuedZelda
  // and skips the sphere-0 weapon/lamp guarantee for every non-Standard
  // world_state (the logic graph assumes the HC escape is already done), so the
  // runtime MUST start post-escape or a fresh save boots into the vanilla
  // rain/uncle/escape intro and hard-softlocks. target_sram[X] maps to
  // g_ram[0xF000 + X] once the slot loads. Standard is intentionally untouched:
  // the vanilla intro IS the Standard start.
  switch (world_state) {
    case kWorldState_Open:
    case kWorldState_Retro:
      // Light-World post-escape free-roam. World flag stays 0 (Light World),
      // no Moon Pearl / Mirror grant.
      target_sram[0x3C5] = 0x02;  // sram_progress_indicator (skip intro)
      target_sram[0x3C6] = 0x14;  // sram_progress_flags
      break;
    case kWorldState_Inverted:
      target_sram[0x3CA] = 0x40;  // savegame_is_darkworld (DW)
      target_sram[0x3C5] = 0x02;  // sram_progress_indicator (skip intro)
      target_sram[0x3C6] = 0x14;  // sram_progress_flags
      target_sram[0x357] = 0x01;  // link_item_moon_pearl (held; no bunny in DW — Inverted starts in the Dark World)
      // link_item_mirror is intentionally NOT pre-granted
      // (add-rando-inverted-dark-chapel-spawn): ALTTPR places the mirror in the
      // world, and the spawn-select "Dark Mountain" option is gated on owning it
      // (link_item_mirror == 2). Leaving it to be found restores the vanilla
      // unlock. Logic-safe — Place_AssumedFill never pre-collects the mirror.
      // #82 Inverted spawn. which_starting_point indexes kStartingPoint_rooms[]
      // (dungeon.c): 1 = the Sanctuary, a *Light-World* building whose exit drops
      // the player in the LW — entirely wrong for an Inverted (DW-home) game, and
      // the cause of the "spawning in the LW" regression. 0 = Link's House, which
      // under the active Inverted slot exits to the DW Bomb-Shop position
      // (screen 0x6C) via the Link's-House<->Bomb-Shop entrance swap — i.e. the
      // Inverted home, in the DW, navigable to the DW-South early checks. (Open /
      // Retro leave this at the memset default 0 for the same Link's-House spawn,
      // there in the LW.) NOTE: baked at generation — an existing slot keeps its
      // stored value, so this only takes effect for a freshly generated seed.
      target_sram[0x3C8] = 0x00;  // which_starting_point (Link's House = Inverted home, DW)
      break;
    case kWorldState_Standard:
    default:
      // Standard: vanilla rain/uncle/escape intro — leave SRAM at fresh defaults.
      break;
  }
}

// Self-test for the world-state start-state SRAM init above (the merge-dropped
// softlock class). Runs against a scratch buffer — no side effects. Called by
// Rando_RunAllSelfChecks; exits(2) on mismatch.
void RandoGenerate_SelfCheck(void) {
  uint8 sram[0x500];

  // Common new-file defaults must always be present (a zeroed health block
  // booted the slot as instant-death; the RANDO name marks the banner).
  Rando_InitNewSlotSram(sram, kWorldState_Standard);
  if (sram[0x340 + 44] != 0x18 || sram[0x340 + 45] != 0x18 ||
      sram[0x340 + 57] != 0xf8 || sram[kSrmOffs_Name] != 0x21) {
    fprintf(stderr, "RandoGenerate_SelfCheck: new-file SRAM defaults missing\n");
    exit(2);
  }
  // Standard keeps the vanilla rain/uncle/escape intro: NO post-escape bytes.
  // If the whole switch is ever dropped again, non-Standard would match this
  // (all-zero) instead of its post-escape state — caught by the cases below.
  if (sram[0x3C5] != 0 || sram[0x3C6] != 0 || sram[0x3CA] != 0 ||
      sram[0x357] != 0 || sram[0x353] != 0 || sram[0x3C8] != 0) {
    fprintf(stderr, "RandoGenerate_SelfCheck: Standard slot must keep vanilla intro SRAM\n");
    exit(2);
  }
  // Open + Retro: Light-World post-escape free-roam (skip intro); no DW/MoonPearl.
  for (int i = 0; i < 2; ++i) {
    uint8 ws = (i == 0) ? (uint8)kWorldState_Open : (uint8)kWorldState_Retro;
    Rando_InitNewSlotSram(sram, ws);
    if (sram[0x3C5] != 0x02 || sram[0x3C6] != 0x14 ||
        sram[0x3CA] != 0 || sram[0x357] != 0 || sram[0x353] != 0) {
      fprintf(stderr, "RandoGenerate_SelfCheck: Open/Retro post-escape SRAM wrong (ws=%u)\n",
              (unsigned)ws);
      exit(2);
    }
  }
  // Inverted: Dark-World start with Moon Pearl (NOT the Magic Mirror — that is a
  // found item, see Rando_InitNewSlotSram) + Link's-House spawn
  // (which_starting_point 0 -> DW Bomb-Shop position via the entrance swap).
  Rando_InitNewSlotSram(sram, kWorldState_Inverted);
  if (sram[0x3CA] != 0x40 || sram[0x3C5] != 0x02 || sram[0x3C6] != 0x14 ||
      sram[0x357] != 0x01 || sram[0x353] != 0x00 || sram[0x3C8] != 0x00) {
    fprintf(stderr, "RandoGenerate_SelfCheck: Inverted DW start-state SRAM wrong\n");
    exit(2);
  }

  // Regression for the named `long` shape alias. It used to require
  // max_sphere >= 14, which is outside this generator's current sphere scale
  // and made the UI fail even at a 500-candidate search limit. Keep this
  // in-memory so the self-check remains free of slot/spoiler writes.
  {
    RandoSettings settings;
    Settings_SetDefaults(&settings);  // Open / Fast Ganon default
    SeedShapeFilter filter;
    char err[160];
    if (SeedShape_Parse("long", &filter, err, sizeof err) != 0) {
      fprintf(stderr, "RandoGenerate_SelfCheck: long shape parse failed: %s\n", err);
      exit(2);
    }
    RandoPlacement entries[kRandoLocationCapacity];
    RandoPlacementTable table = { entries, 0 };
    bool accepted = false;
    uint32 attempts = 0;
    SeedShapeMetrics metrics;
    memset(&metrics, 0, sizeof metrics);
    // Keep the probe below the UI's default 100-candidate search while leaving
    // enough room for legitimate logic changes to move sphere boundaries.
    for (int i = 0; i < 64; i++) {
      table.count = 0;
      Rando_ClearGenerationLogicOverlays();
      RandoEntranceRegen reg;
      memset(&reg, 0, sizeof reg);
      if (!Rando_PlaceWithEntrances(&settings, 0x1234ull + (uint64)i,
                                    /*budget_seconds=*/0, &table, &reg)) {
        Rando_ClearGenerationLogicOverlays();
        continue;
      }
      RandoSpheres spheres;
      memset(&spheres, 0, sizeof spheres);
      (void)Logic_ComputeSpheres(&settings, &table, &spheres);
      char reject[160];
      if (SeedShape_Evaluate(&filter, &settings, &table, &spheres,
                             Placement_GetLastStats(), &metrics,
                             reject, sizeof reject)) {
        accepted = true;
        attempts = (uint32)(i + 1);
        break;
      }
      Rando_ClearGenerationLogicOverlays();
    }
    Rando_ClearGenerationLogicOverlays();
    if (!accepted) {
      fprintf(stderr, "RandoGenerate_SelfCheck: long shape did not match within 64 candidates\n");
      exit(2);
    }
    if (attempts == 0 || metrics.max_sphere < kSeedShapePresetLongMinSphere) {
      fprintf(stderr, "RandoGenerate_SelfCheck: long shape accepted invalid metrics\n");
      exit(2);
    }
  }

  // Dungeon chains route through Rando_PlaceWithEntrances because they install
  // per-attempt edge overrides. Exercise that path under the strictest
  // accessibility tier so a regression cannot certify a chain layout with a
  // stranded location.
  {
    RandoSettings chains;
    Settings_SetDefaults(&chains);
    chains.dungeon_chains = 1;
    chains.accessibility = kAccessibility_Locations;
    RandoPlacement entries[kRandoLocationCapacity];
    RandoPlacementTable table = { entries, 0 };
    RandoEntranceRegen reg;
    memset(&reg, 0, sizeof reg);
    Rando_ClearGenerationLogicOverlays();
    if (!Rando_PlaceWithEntrances(&chains, 0xC4A1175EEDull,
                                  /*budget_seconds=*/0, &table, &reg)) {
      Rando_ClearGenerationLogicOverlays();
      fprintf(stderr, "RandoGenerate_SelfCheck: dungeon_chains placement failed\n");
      exit(2);
    }
    RandoSpheres spheres;
    memset(&spheres, 0, sizeof spheres);
    if (!Logic_ComputeSpheres(&chains, &table, &spheres) ||
        spheres.unreachable_count != 0) {
      Rando_ClearGenerationLogicOverlays();
      fprintf(stderr, "RandoGenerate_SelfCheck: dungeon_chains full-reachability gate failed\n");
      exit(2);
    }
    Rando_ClearGenerationLogicOverlays();
  }

  // Diagnostic work telemetry must be deterministic in-process and must not
  // perturb canonical placement output when disabled. Use a plain seed so the
  // probe stays fast while still exercising placement and reachability passes.
  {
    RandoSettings settings;
    Settings_SetDefaults(&settings);
    RandoPlacement entries[kRandoLocationCapacity];
    RandoPlacementTable table = { entries, 0 };
    uint8 digest[3][32];
    RandoGenerationWorkCounters observed[2];

    for (int run = 0; run < 2; run++) {
      table.count = 0;
      Rando_ClearGenerationLogicOverlays();
      RandoEntranceRegen reg;
      memset(&reg, 0, sizeof reg);
      Rando_WorkCountersBeginGeneration();
      bool placed = Rando_PlaceWithEntrances(&settings, 0x574F524B434E5452ull,
                                             /*budget_seconds=*/0, &table, &reg);
      if (placed) {
        RandoSpheres spheres;
        memset(&spheres, 0, sizeof spheres);
        (void)Logic_ComputeSpheres(&settings, &table, &spheres);
      }
      Rando_WorkCountersEndGeneration();
      if (!placed) {
        Rando_ClearGenerationLogicOverlays();
        fprintf(stderr, "RandoGenerate_SelfCheck: work-counter placement failed\n");
        exit(2);
      }
      observed[run] = *Rando_GetLastGenerationWorkCounters();
      PlacementTable_ComputeDigest(&table, digest[run]);
    }
    if (memcmp(&observed[0], &observed[1], sizeof observed[0]) != 0 ||
        memcmp(digest[0], digest[1], sizeof digest[0]) != 0 ||
        observed[0].placement_attempts == 0 ||
        observed[0].reachability_expansion_passes == 0) {
      Rando_ClearGenerationLogicOverlays();
      fprintf(stderr, "RandoGenerate_SelfCheck: work counters are not deterministic\n");
      exit(2);
    }

    // A third run with counters disabled must preserve the placement digest
    // and leave the frozen diagnostic record unchanged.
    table.count = 0;
    Rando_ClearGenerationLogicOverlays();
    RandoEntranceRegen reg;
    memset(&reg, 0, sizeof reg);
    if (!Rando_PlaceWithEntrances(&settings, 0x574F524B434E5452ull,
                                  /*budget_seconds=*/0, &table, &reg)) {
      Rando_ClearGenerationLogicOverlays();
      fprintf(stderr, "RandoGenerate_SelfCheck: counter-disabled placement failed\n");
      exit(2);
    }
    PlacementTable_ComputeDigest(&table, digest[2]);
    if (memcmp(digest[0], digest[2], sizeof digest[0]) != 0 ||
        memcmp(&observed[1], Rando_GetLastGenerationWorkCounters(),
               sizeof observed[1]) != 0) {
      Rando_ClearGenerationLogicOverlays();
      fprintf(stderr, "RandoGenerate_SelfCheck: counters changed canonical output\n");
      exit(2);
    }
    Rando_ClearGenerationLogicOverlays();
  }
  fprintf(stderr, "[RandoGenerate_SelfCheck] OK\n");
}

bool Rando_PlaceWithEntrances(const RandoSettings *settings, uint64 seed_u64,
                              int budget_seconds, RandoPlacementTable *table,
                              RandoEntranceRegen *reg) {
  generate_profile_maybe_init();
  if (g_generate_profile_active) g_generate_profile.calls++;
  memset(reg, 0, sizeof(*reg));
  bool cross_on = Entrance_IsCrossActive(settings);   // supersedes the separate paths
  bool cave_on = !cross_on && Entrance_IsActive(settings);
  bool dun_on = !cross_on && Entrance_IsDungeonActive(settings);
  bool decoupled_on = Entrance_IsDecoupledActive(settings);
  bool dun_decoupled_on = Entrance_IsDungeonDecoupledActive(settings);
  bool cross_decoupled_on = Entrance_IsCrossDecoupledActive(settings);  // one-way over mixed pool
  bool chains_on = Settings_EffectiveDungeonChains(settings);
  bool placed = false;
  Entrance_ClearRegionOverrides();  // ensure a clean logic graph
  Entrance_ClearEdgeOverrides();
  if (cross_on || cave_on || dun_on || decoupled_on || dun_decoupled_on || cross_decoupled_on) {
    if (g_generate_profile_active) g_generate_profile.entrance_calls++;
    uint8 canon[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(settings, canon);
    reg->entrance_axes = canon[25];  // == the packed entrance-axis byte
    const int kEntranceMaxRetry = 64;
    for (int att = 0; att < kEntranceMaxRetry; att++) {
      if (g_generate_profile_active) g_generate_profile.entrance_attempts++;
      // reset the edge-override set EVERY attempt so the
      // cave-only ApplyDecoupledExitEdges Begins fresh instead of appending onto
      // the prior attempt's leftover edges (phantom reachability + overflow).
      Entrance_ClearEdgeOverrides();
      if (cross_on) {
        reg->cross_count = Entrance_ComputeCrossPermutation(settings, seed_u64, (uint8)att, reg->cross_assign);
        Entrance_ApplyCrossOverrides(reg->cross_assign, reg->cross_count);
      } else {
        if (cave_on) {
          reg->cave_count = Entrance_ComputePermutation(settings, seed_u64, (uint8)att, reg->cave_assign);
          Entrance_ApplyRegionOverrides(reg->cave_assign, reg->cave_count);
        }
        if (dun_on) {
          reg->dun_count = Entrance_ComputeDungeonPermutation(settings, seed_u64, (uint8)att, reg->dun_assign);
          Entrance_ApplyEdgeOverrides(reg->dun_assign, reg->dun_count);
        }
      }
      // Decoupled exit warps compose on top of whichever entry pass ran (D.1).
      if (decoupled_on) {
        reg->decoupled_count = Entrance_ComputeDecoupledExit(settings, seed_u64, (uint8)att, reg->decoupled_assign);
        Entrance_ApplyDecoupledExitEdges(reg->decoupled_assign, reg->decoupled_count);
      }
      // Dungeon decoupled: NO logic edges (coupled-equivalent reachability is
      // correct + conservative); computed for the runtime redirect + spoiler only.
      if (dun_decoupled_on) {
        reg->dun_decoupled_count = Entrance_ComputeDungeonDecoupledExit(settings, seed_u64, (uint8)att, reg->dun_decoupled_assign);
      }
      // Cross + decoupled: also NO logic edges (entry logic is the cross overrides).
      if (cross_decoupled_on) {
        reg->cross_decoupled_count = Entrance_ComputeCrossDecoupledExit(settings, seed_u64, (uint8)att, reg->cross_decoupled_assign);
      }
      // add-rando-ow-warp-shuffle — warp layout shares the entrance retry
      // index; installs compose on this attempt's overlay.
      if (!ow_warp_apply_attempt(settings, seed_u64, (uint32)att))
        break;  // graph absent: deterministic fail-closed, no retry helps
      table->count = 0;
      if (rando_place_assumed_fill_counted(settings, seed_u64,
                                           budget_seconds, table)) {
        // Accept this permutation only if it meets the active accessibility tier
        // (always beatable, plus per-tier reachability). Rejects a π that strands
        // placements beyond the tier's bar (e.g. a gated door leading to the very
        // dungeon that grants the gating item) and tries another.
        if (Accessibility_SeedAcceptable(settings, seed_u64, table)) {
          placed = true;
          reg->entrance_attempt = (uint8)att;
          if (g_generate_profile_active) g_generate_profile.entrance_successes++;
          break;
        }
      } else if (Customizer_LastError()[0] != '\0') {
        // A customizer pin error is deterministic — no entrance permutation
        // will fix it, so stop retrying immediately (mirrors the CLI).
        break;
      }
    }
    // NB: leave the accepted π's overrides ACTIVE — the caller's sphere/goal
    // computation must see the shuffled reachability. Caller clears afterward.
  } else if (Settings_EffectiveDoorShuffle(settings) != kDoorShuffle_Vanilla) {
    if (g_generate_profile_active) g_generate_profile.door_calls++;
    // add-rando-door-shuffle — door phase. Mutually exclusive with entrance
    // shuffle (apply_derived_rules), so it owns this arm. Generate a layout
    // per door_attempt (stitch + key-prove every unpinned dungeon), install
    // it for the logic oracle, then run placement; a placement/accessibility
    // failure tries the next attempt. The accepted attempt + layout digest
    // are persisted (@76-79) so activation can regenerate + drift-check.
    for (uint32 datt = 0; datt < 16; datt++) {
      if (g_generate_profile_active) g_generate_profile.door_attempts++;
      if (g_generation_work_active)
        g_generation_work_live.door_generation_attempts++;
      if (!DoorShuffle_Generate(seed_u64, datt, kDoorShuffle_MvpDungeonMask,
                                Settings_DoorPotTier(settings),
                                door_enemy_drop_keys_for_settings(settings),
                                door_enemy_check_tier_for_settings(settings),
                                &g_door_gen_layout)) {
        if (g_generate_profile_active) g_generate_profile.door_layout_failures++;
        continue;
      }
      Rando_SetDoorLogicLayout(&g_door_gen_layout, g_door_gen_layout.shuffled_mask);
      // add-rando-ow-warp-shuffle — warp composes with door shuffle; the
      // door loop doesn't use the edge overlay, so clear + own it per
      // attempt (retry-accumulation lesson).
      Entrance_ClearEdgeOverrides();
      if (!ow_warp_apply_attempt(settings, seed_u64, datt))
        break;
      table->count = 0;
      if (rando_place_assumed_fill_counted(settings, seed_u64,
                                           budget_seconds, table) &&
          Accessibility_SeedAcceptable(settings, seed_u64, table)) {
        placed = true;
        g_door_gen_attempt = (uint8)datt;
        g_door_gen_digest24 = DoorShuffle_LayoutDigest(&g_door_gen_layout) & 0xFFFFFF;
        if (g_generate_profile_active) {
          uint32 used = datt + 1;
          g_generate_profile.door_successes++;
          if (used > g_generate_profile.max_door_attempts)
            g_generate_profile.max_door_attempts = used;
        }
        break;
      }
      if (g_generate_profile_active) g_generate_profile.door_rejections++;
      Rando_SetDoorLogicLayout(NULL, 0);
      if (Customizer_LastError()[0] != '\0')
        break;  // deterministic customizer pin error — no layout will fix it
    }
    // NB: the accepted layout stays INSTALLED — the caller's sphere/goal
    // computation must see the shuffled reachability (slot activation later
    // re-installs its own regenerated copy).
  } else if (chains_on) {
    if (g_generate_profile_active) g_generate_profile.chains_calls++;
    const int kChainsMaxRetry = 64;
    DungeonChainsLayout chains_layout;
    for (uint32 catt = 0; catt < kChainsMaxRetry; catt++) {
      if (g_generate_profile_active) g_generate_profile.chains_attempts++;
      Entrance_ClearEdgeOverrides();
      if (!Chains_Compute(seed_u64, catt, &chains_layout)) {
        if (g_generate_profile_active) g_generate_profile.chains_layout_failures++;
        continue;
      }
      Chains_ApplyEdgeOverrides(&chains_layout);
      // add-rando-ow-warp-shuffle — composes on the chains overlay.
      if (!ow_warp_apply_attempt(settings, seed_u64, catt))
        break;
      table->count = 0;
      if (rando_place_assumed_fill_counted(settings, seed_u64,
                                           budget_seconds, table) &&
          Accessibility_SeedAcceptable(settings, seed_u64, table)) {
        placed = true;
        reg->chains_active = true;
        reg->chains_attempt = (uint8)catt;
        reg->chains_layout = chains_layout;
        if (g_generate_profile_active) {
          uint32 used = catt + 1;
          g_generate_profile.chains_successes++;
          if (used > g_generate_profile.max_chains_attempts)
            g_generate_profile.max_chains_attempts = used;
        }
        break;
      }
      if (g_generate_profile_active) g_generate_profile.chains_rejections++;
      if (Customizer_LastError()[0] != '\0')
        break;  // deterministic customizer pin error — no layout will fix it
    }
    // NB: leave the accepted chain edge overrides ACTIVE — the caller's
    // sphere/goal computation must see the chain reachability.
    if (!placed)
      Entrance_ClearEdgeOverrides();
  } else if (Settings_OwWarpActive(settings)) {
    // add-rando-ow-warp-shuffle — warp-only retry loop (no other retrying
    // feature active): the layout varies per attempt so a warp arrangement
    // the accessibility tier rejects can be retried (mirrors the entrance
    // loop; the accepted attempt persists via Rando_GetOwWarpGeneration).
    const int kOwWarpMaxRetry = 64;
    for (int watt = 0; watt < kOwWarpMaxRetry; watt++) {
      Entrance_ClearEdgeOverrides();
      if (!ow_warp_apply_attempt(settings, seed_u64, (uint32)watt))
        break;  // graph absent: deterministic fail-closed
      table->count = 0;
      // Counted wrapper per the merged validation-hardening convention (its
      // invariant sweep audits per-generation placement-attempt totals).
      if (rando_place_assumed_fill_counted(settings, seed_u64, budget_seconds,
                                           table) &&
          Accessibility_SeedAcceptable(settings, seed_u64, table)) {
        placed = true;
        break;
      }
      if (Customizer_LastError()[0] != '\0')
        break;  // deterministic customizer pin error
    }
    if (!placed)
      Entrance_ClearEdgeOverrides();
    // NB: the accepted layout's overlay edges stay ACTIVE for the caller's
    // sphere/goal computation (entrance/chains precedent).
  } else {
    if (g_generate_profile_active) g_generate_profile.plain_calls++;
    // audit M3: reset the file-scope warp generation state (warp is off on
    // this arm, so this is a pure reset) — without it a non-warp seed
    // generated after a warp seed in the same process inherits the stale
    // spoiler ow_warps block and header identity.
    (void)ow_warp_apply_attempt(settings, seed_u64, 0);
    placed = rando_place_assumed_fill_counted(settings, seed_u64,
                                              budget_seconds, table);
    if (placed && !Accessibility_SeedAcceptable(settings, seed_u64, table)) {
      placed = false;
    }
  }
  return placed;
}

void Rando_GetDoorGeneration(uint8 *attempt_out, uint32 *digest24_out) {
  if (attempt_out) *attempt_out = g_door_gen_attempt;
  if (digest24_out) *digest24_out = g_door_gen_digest24;
}

void Rando_ClearGenerationLogicOverlays(void) {
  Entrance_ClearRegionOverrides();
  Entrance_ClearEdgeOverrides();
  // add-rando-door-shuffle — the accepted door layout is left installed by
  // Rando_PlaceWithEntrances for the caller's sphere/goal/spoiler work; it
  // MUST be cleared with the entrance overrides or the NEXT same-process
  // generation (doors off) places against stale door reachability
  // (eval_doors_active keys purely off the installed pointer) and its slot —
  // saved with door_digest 0 — is certified against the wrong logic graph.
  Rando_SetDoorLogicLayout(NULL, 0);
}

// (door-shuffle statics declared at the top of this file)

void Rando_SpoilerSetEntranceFields(struct RandoSpoiler *spoiler,
                                    const RandoEntranceRegen *reg) {
  spoiler->entrance_assign = (reg->cave_count > 0) ? reg->cave_assign : NULL;
  spoiler->entrance_count = reg->cave_count;
  spoiler->dungeon_assign = (reg->dun_count > 0) ? reg->dun_assign : NULL;
  spoiler->dungeon_count = reg->dun_count;
  spoiler->cross_assign = (reg->cross_count > 0) ? reg->cross_assign : NULL;
  spoiler->cross_count = reg->cross_count;
  spoiler->decoupled_assign = (reg->decoupled_count > 0) ? reg->decoupled_assign : NULL;
  spoiler->decoupled_count = reg->decoupled_count;
  spoiler->dun_decoupled_assign = (reg->dun_decoupled_count > 0) ? reg->dun_decoupled_assign : NULL;
  spoiler->dun_decoupled_count = reg->dun_decoupled_count;
  spoiler->cross_decoupled_assign = (reg->cross_decoupled_count > 0) ? reg->cross_decoupled_assign : NULL;
  spoiler->cross_decoupled_count = reg->cross_decoupled_count;
  spoiler->chains_layout = reg->chains_active ? &reg->chains_layout : NULL;
  spoiler->chains_attempt = reg->chains_active ? reg->chains_attempt : 0;
}

bool Rando_GenerateSlotWithShapeFilter(const RandoSettings *settings, uint64 seed_u64,
                                       int budget, int slot_index,
                                       uint32 recommended_features0,
                                       const RandoGenerateShapeOptions *shape,
                                       RandoGenerateResult *out,
                                       char *err, size_t err_cap) {
  uint32 user_recommended_features0 = recommended_features0;
  if (err != NULL && err_cap > 0) err[0] = '\0';

  // Refuse an out-of-range slot BEFORE any SRAM/sidecar write. The SRAM init
  // below indexes g_zenv.sram + slot_index*0x500, so a negative slot_index
  // (e.g. the window closed mid-request, clearing the kind-toggle target to
  // -1) would memset/write BEFORE the buffer.
  if (slot_index < 0 || slot_index >= kRandoSidecar_SlotCount) {
    if (err != NULL) snprintf(err, err_cap, "invalid slot index %d", slot_index);
    return false;
  }

  // add-rando-customizer-mode — customizer_active requires an installed
  // manifest (the placer hard-errors otherwise; refuse with a clearer message
  // here), and is mutually exclusive with race mode: the race reveal
  // regenerates placement from (seed, settings) alone, and without the
  // manifest the §3c pins cannot be reproduced — reveal would fail instead of
  // verifying the stamp. Fail closed at generation.
  if (settings->customizer_active) {
    if (Customizer_GetActive() == NULL) {
      if (err != NULL)
        snprintf(err, err_cap, "customizer mode is on but no manifest is loaded");
      return false;
    }
    if (settings->race_mode) {
      if (err != NULL)
        snprintf(err, err_cap,
                 "race mode cannot be combined with customizer mode (the race "
                 "reveal cannot regenerate manifest pins)");
      return false;
    }
  }

  if (!Placement_PreflightSettings(settings, err, err_cap)) {
    return false;
  }

  // Compute settings_hash (already cached as short).
  uint8 settings_hash_full[32];
  Settings_ComputeHash(settings, settings_hash_full);

  // Run placement.
  extern const uint32 kRandoLocationsCount;
  RandoPlacement *entries = (RandoPlacement *)calloc(kRandoLocationsCount,
                                                     sizeof(RandoPlacement));
  if (entries == NULL) {
    if (err != NULL) snprintf(err, err_cap, "OOM allocating placement table");
    return false;
  }
  RandoPlacementTable table = { entries, 0 };
  // Default budget is 0 (no wall-clock cutoff) so the placer runs to its
  // deterministic kAssumedFillMaxAttempts cap — see the DETERMINISM WARNING
  // at Place_AssumedFill. A positive budget selects a machine-speed-dependent
  // best-so-far, but the share string carries only (seed, settings_hash) and
  // the receiver REGENERATES placement, so a non-zero default diverges on
  // hard seeds. Race-mode reveal also passes 0; explicit positive budgets
  // from the caller are still honored (diagnostic use).
  int effective_budget = (budget < 0) ? 0 : budget;
  const SeedShapeFilter *shape_filter =
      (shape != NULL && shape->filter != NULL && shape->filter->enabled)
          ? shape->filter : NULL;
  int shape_search_limit = (shape_filter != NULL) ? shape->search_limit : 1;
  if (shape_search_limit < 1) shape_search_limit = 1;
  SeedShapeMetrics shape_metrics;
  memset(&shape_metrics, 0, sizeof shape_metrics);
  uint32 shape_attempts_used = 0;
  char last_shape_reject[160];
  last_shape_reject[0] = '\0';
  RandoSpheres spheres;
  bool spheres_computed = false;

  // Phase C — entrance shuffle: draw a cave permutation π, install its per-seed
  // region overrides so the placer/goal-check see the shuffled reachability, run
  // placement, accept the first π under which the active accessibility tier is
  // met, and record the accepted attempt index (stored in the slot header with
  // the packed axis byte so slot-load regenerates the same π). The same helper
  // runs at race-mode reveal so the regenerated spoiler is byte-identical.
  // Default-off ⇒ a single accessibility-gated Place_AssumedFill (byte-identical).
  RandoEntranceRegen reg;
  memset(&reg, 0, sizeof reg);
  bool placed = false;
  Rando_WorkCountersBeginGeneration();
  for (int shape_attempt = 0; shape_attempt < shape_search_limit; shape_attempt++) {
    uint64 candidate_seed = seed_u64 + (uint64)shape_attempt;
    table.count = 0;
    Rando_ClearGenerationLogicOverlays();
    RandoEntranceRegen candidate_reg;
    memset(&candidate_reg, 0, sizeof candidate_reg);
    bool candidate_placed = Rando_PlaceWithEntrances(settings, candidate_seed,
                                                     effective_budget, &table,
                                                     &candidate_reg);
    if (!candidate_placed) {
      const char *cerr = Customizer_LastError();
      snprintf(last_shape_reject, sizeof last_shape_reject, "%s",
               cerr[0] != '\0' ? cerr : "placement failed");
      Rando_ClearGenerationLogicOverlays();
      if (cerr[0] != '\0')
        break;
      continue;
    }

    if (shape_filter != NULL) {
      bool spheres_ok = Logic_ComputeSpheres(settings, &table, &spheres);
      (void)spheres_ok;
      spheres_computed = true;
      char shape_reject[160];
      if (!SeedShape_Evaluate(shape_filter, settings, &table, &spheres,
                              Placement_GetLastStats(), &shape_metrics,
                              shape_reject, sizeof shape_reject)) {
        snprintf(last_shape_reject, sizeof last_shape_reject, "%s", shape_reject);
        Rando_ClearGenerationLogicOverlays();
        spheres_computed = false;
        continue;
      }
    }

    placed = true;
    reg = candidate_reg;
    seed_u64 = candidate_seed;
    shape_attempts_used = (uint32)(shape_attempt + 1);
    break;
  }
  Rando_WorkCountersEndGeneration();
  if (!placed) {
    // clear the logic overlays on the failure path too (the
    // success path clears after the spoiler block; a failed generation would
    // otherwise leak the last attempt's shuffled reachability to the tracker).
    Rando_ClearGenerationLogicOverlays();
    if (err != NULL) {
      // A customizer pin error is the root cause when set — surface it instead
      // of the generic accessibility advice (mirrors the CLI failure path).
      if (Customizer_LastError()[0] != '\0')
        snprintf(err, err_cap, "customizer: %s", Customizer_LastError());
      else if (shape_filter != NULL) {
        char shape_desc[256];
        SeedShape_Describe(shape_filter, shape_desc, sizeof shape_desc);
        snprintf(err, err_cap,
                 "shape filter search failed after %d candidate seed(s): %s. "
                 "Try a higher search limit or loosen the filter.",
                 shape_search_limit,
                 last_shape_reject[0] != '\0' ? last_shape_reject : shape_desc);
      }
      else if (Settings_EffectiveAccessibility(settings) == kAccessibility_Locations)
        snprintf(err, err_cap,
                 "no 100%%-locations placement found; try 'items' or 'beatable only'");
      else if (Settings_EffectiveAccessibility(settings) == kAccessibility_Items)
        snprintf(err, err_cap,
                 "no 100%%-inventory placement found; try 'beatable only'");
      else
        snprintf(err, err_cap, "placement failed");
    }
    free(entries);
    return false;
  }

  // Build share string.
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  memcpy(ss.settings_hash, settings_hash_full, 16);
  ss.seed_u64 = seed_u64;
  char share_string[kShareStringBase32MaxLen];
  int share_len = Share_Encode(&ss, share_string, sizeof(share_string));
  if (share_len <= 0) {
    if (err != NULL) snprintf(err, err_cap, "share string encode failed");
    // This early return sits BEFORE the post-spoiler clear below — drop the
    // entrance/door overlays here too or they leak to the next generation.
    Rando_ClearGenerationLogicOverlays();
    free(entries);
    return false;
  }

  // Pack the 31-byte raw binary blob via the public Share_PackBinary
  // helper so the trailing CRC is correct. Previously the code rebuilt
  // the blob inline and left CRC = 0, which meant the slot's stored
  // share_string base32-re-encoded to a string
  // DIFFERENT from the one Share_Encode emitted to the user — friends
  // who tried Share_Decode on the banner-displayed string would get
  // BadChecksum. The slot header reserves 32 bytes for share_string; we
  // zero-pad bytes 31..32 here for cleanliness.
  uint8 raw_binary[32];
  memset(raw_binary, 0, sizeof(raw_binary));
  Share_PackBinary(&ss, raw_binary);

  // Hints are a post-placement product. Build exactly one immutable plan while
  // every accepted logic overlay is still installed, after the final placement
  // and sphere assignment are known. The same value feeds the spoiler, the
  // sidecar identity, and (when requested) the native-window snapshot.
  if (!spheres_computed) {
    bool spheres_ok = Logic_ComputeSpheres(settings, &table, &spheres);
    (void)spheres_ok;
    spheres_computed = true;
  }
  RandoHintPlan hint_plan;
  if (!Rando_BuildHintPlan(settings, &table, &spheres,
                           kRandoHintPlanAlgorithmVersion,
                           kRandoHintTextSchemaVersion, &hint_plan)) {
    if (err != NULL) snprintf(err, err_cap, "hint plan build failed");
    Rando_ClearGenerationLogicOverlays();
    free(entries);
    return false;
  }
  RandoHintPersistedState hint_state;
  if (!Rando_HintPlanExportPersistedState(&hint_plan, &hint_state)) {
    if (err != NULL) snprintf(err, err_cap, "hint plan identity export failed");
    Rando_ClearGenerationLogicOverlays();
    free(entries);
    return false;
  }

  bool goal_completable = Goal_IsCompletable(settings, seed_u64, &table);

  // Compute spoiler path + write spoiler files.
  char spoiler_json_path[512];
  char spoiler_txt_path[512];
  int n = Spoiler_ResolvePath(share_string, ".json", spoiler_json_path,
                              sizeof(spoiler_json_path));
  int m = Spoiler_ResolvePath(share_string, ".txt", spoiler_txt_path,
                              sizeof(spoiler_txt_path));
  if (n > 0 && m > 0) {
    RandoSpoiler spoiler;
    memset(&spoiler, 0, sizeof(spoiler));
    spoiler.share_string = share_string;
    spoiler.seed_u64 = seed_u64;
    spoiler.generator_version = kGeneratorVersion;
    spoiler.settings = settings;
    spoiler.placements = &table;
    spoiler.spheres = &spheres;
    spoiler.hint_plan = &hint_plan;
    uint8 medallion_assignment[kRandoMedallionEntranceCount];
    if (copy_active_medallion_assignment(medallion_assignment))
      spoiler.medallion_assignment = medallion_assignment;
    // Phase C — entrance_mapping sections (omitted when the respective count is 0).
    Rando_SpoilerSetEntranceFields(&spoiler, &reg);
    // Phase B Slice 7/8 — boss + drop shuffle spoiler sections. Computed with
    // the PURE forms (no runtime-global side effects: this is the in-game
    // generate path; the runtime install happens at slot load, not here).
    // Deterministic from (settings, seed) — identical to the runtime install
    // and the headless path. NULL pointers omit the section when off (§6.4).
    uint8 boss_assignment[16];
    uint8 drop_map[kDropTableEntryCount];
    bool drop_used_fallback = false;
    BossShuffle_ComputeAssignment(settings, seed_u64, boss_assignment);
    DropShuffle_ComputeAssignment(settings, seed_u64, drop_map, &drop_used_fallback);
    spoiler.boss_assignment = settings->boss_shuffle ? boss_assignment : NULL;
    spoiler.drop_map = settings->drop_shuffle ? drop_map : NULL;
    spoiler.drop_used_fallback = drop_used_fallback;
    spoiler.goal_completable = goal_completable;
    {
      const PlacementStats *st = Placement_GetLastStats();
      spoiler.forward_fill_fallback_count = st->forward_fill_fallback_count;
      spoiler.retry_attempts = st->attempts_used;
    }
    // Phase B Slice 6 — Spoiler_Write branches on race_mode (full vs suppressed).
    if (!Spoiler_Write(&spoiler, spoiler_json_path, spoiler_txt_path)) {
      fprintf(stderr, "[settings] spoiler write failed: %s\n", spoiler_json_path);
    }
  }
  // The accepted overlays (entrance π overrides AND the door logic layout)
  // have now fed placement, the spheres, the goal check, and the spoiler.
  // Clear them so any later reachability (e.g. the tracker, or the next
  // generation) starts from the identity graph — slot activation re-installs
  // its own regenerated copies. (The door attempt/digest read below comes
  // from Rando_GetDoorGeneration's statics, not the installed layout.)
  Rando_ClearGenerationLogicOverlays();

  // Build & write the sidecar slot. Slot kind = Randomizer.
  RandoSidecarSlot slot;
  memset(&slot, 0, sizeof(slot));
  slot.header.slot_kind = kSlotKind_Randomizer;
  slot.header.generator_version = (uint16)kGeneratorVersion;
  memcpy(slot.header.settings_hash, settings_hash_full, 16);
  memcpy(slot.header.share_string, raw_binary, kRandoSidecar_ShareStringLength);
  // Phase B hints: carry the `hints` and `goal` axes in the slot's reserved
  // tail (rando_save.h settings extension) so telepathic-tile hints can be
  // regenerated at slot load. The generator reads only these two axes.
  slot.header.settings_ext_present = 1;
  slot.header.hints_setting = settings->hints;
  slot.header.goal = settings->goal;
  slot.header.hint_state = hint_state;
  // Carry world_state too (@68). Without this the slot header keeps the memset
  // 0 = kWorldState_Open, so a Standard/Inverted/Retro seed loads as Open at
  // runtime: for Standard that makes Rando_SuppressHyruleCastleEscape() true and
  // despawns the opening (Link's-house) Uncle — room 0x104, whose low byte 0x04
  // collides with the sewers-passage branch in SpritePrep_UncleAndPriest_bounce —
  // so Link sleeps forever (player_sleep_in_bed_state never advances).
  slot.header.world_state = settings->world_state;
  // Phase C — carry the entrance-shuffle axes + accepted goal-retry attempt so
  // slot-load can regenerate the cave permutation π (and install the door
  // overlay) deterministically from the seed. 0/0 when no shuffle was active.
  // (Redundant with canon[25] in the settings blob below, but the dedicated
  // header bytes are what Entrance_RuntimeInstall reads.)
  slot.header.entrance_axes = reg.entrance_axes;
  slot.header.entrance_attempt = reg.entrance_attempt;
  // FIX #4 — 24-bit digest of the entrance layout these header fields
  // regenerate (mirrors door_digest24 below): Rando_ActivateSidecarSlot
  // recomputes it from the SAME header fields via the SAME layout computation
  // and refuses the slot on mismatch, so an entrance algorithm/pool change
  // can't silently install a layout the placement wasn't certified against.
  // 0 when no entrance axis is active. Reads share_string + world_state +
  // settings_ext_present from the header, all populated above.
  slot.header.entrance_digest24 = Rando_EntranceLayoutDigest24(&slot.header);
  // add-rando-ow-warp-shuffle — accepted warp attempt + layout digest
  // (ext v12 @58-61; 0/0 when no warp axis was active). Activation
  // regenerates the layout from these + the settings blob and hard-fails on
  // digest mismatch (door-gate class).
  Rando_GetOwWarpGeneration(&slot.header.ow_attempt, &slot.header.ow_digest24);
  // format_version 2: persist the FULL canonical settings blob so a reloaded
  // slot can reproduce the seed's settings + prize/medallion shuffle
  // assignments for the runtime reachability (tracker) engine. The reserved-tail
  // hints/goal/world_state above stay for backward-compat (hint regen, Inverted
  // detection on older readers); this blob is the authoritative settings source
  // for v2 readers. See Rando_RecoverActiveSettings / Rando_ActivateSidecarSlot.
  Settings_CanonicalSerialize(settings, slot.settings_canonical);
  slot.header.settings_present = 1;
  // Flags: set the forward-fill bit if the placer used the fallback.
  bool used_forward_fill = false;
  {
    const PlacementStats *st = Placement_GetLastStats();
    if (st->forward_fill_fallback_count > 0) {
      slot.header.flags |= kRandoSlotFlag_ForwardFillUsed;
      used_forward_fill = true;
    }
    // FIX #6 — persist the accepted assumed-fill attempt index so slot-load can
    // re-derive the prize/medallion shuffle with the SAME per-attempt seed the
    // placer baked into the table (Rando_ActivateSidecarSlot). The stats reflect
    // the accepted Place_AssumedFill call: Rando_PlaceWithEntrances breaks on the
    // accepted attempt and nothing re-runs placement before this point.
    slot.header.prize_attempt = st->prize_attempt;
  }
  // add-rando-door-shuffle — persist the accepted door_attempt + layout digest
  // (zero/zero when door shuffle was off; the digest is the activation-time
  // drift hard-fail).
  if (Settings_EffectiveDoorShuffle(settings) != kDoorShuffle_Vanilla) {
    Rando_GetDoorGeneration(&slot.header.door_attempt, &slot.header.door_digest24);
  }
  // dungeon-chains: persist the accepted layout identity so slot activation can
  // regenerate the same chain graph and hard-fail on drift.
  if (reg.chains_active) {
    slot.header.chains_present = 1;
    slot.header.chains_attempt = reg.chains_attempt;
    slot.header.chains_digest24 = reg.chains_layout.digest24 & 0xFFFFFFu;
  }
  // Stamp EVERY local-registry identity the activation guard checks (pot +
  // terrain + ...) through the single shared helper. This is the PLAYER-FACING
  // slot path and is SEPARATE code from the --generate-seed / selfcheck
  // slot-builders the corpus exercises; a fresh-eyes review found this path had
  // stamped pots but forgotten terrain, so every terrain slot self-refused
  // while the corpus stayed green. Routing all writers through one helper makes
  // that drift structurally impossible.
  Rando_StampSlotRegistries(&slot.header);
  // add-rando-major-glitch D6 — couple a glitch-logic seed to the JP-1.0
  // glitch runtime flag. The placer ASSUMED restored glitches are performable
  // for logic>=OverworldGlitches / fake-flippers seeds; the runtime MUST
  // provide them or this assumed-fill-certified seed is an unreachable-item
  // soft-softlock. Force the bit into the slot's recommended features before
  // serializing the sidecar so reloads preserve the guarantee.
  if (Rando_SettingsAssumeJpGlitches(settings)) {
    recommended_features0 |= kFeatures0_RestoreJpGlitches;
  }
  slot.header.recommended_features0 =
      recommended_features0 & kFeatures0_RandoSeedQolMask;
  slot.header.recommended_features0_present = 1;
  // Copy placements + compute placement_table_size (BYTES = 2 * max_loc_id + 2).
  if (table.count > (uint16)(sizeof(slot.placements) / sizeof(slot.placements[0]))) {
    if (err != NULL)
      snprintf(err, err_cap, "placement count %u exceeds sidecar slot capacity",
               (unsigned)table.count);
    free(entries);
    return false;
  }
  memcpy(slot.placements, entries, sizeof(RandoPlacement) * table.count);
  slot.placement_count = table.count;
  uint16 max_loc = 0;
  for (uint16 i = 0; i < table.count; ++i) {
    if (entries[i].location_id > max_loc) max_loc = entries[i].location_id;
  }
  slot.header.placement_table_size = (uint16)((max_loc + 1) * 2);

  // Initialize the target sram.dat slot image (new-file defaults + world-state
  // post-escape start state) via the shared, self-tested helper above. The
  // rando-specific runtime bookkeeping (starting-inventory injection, etc.)
  // happens at game-start time via Rando_TryGrantStartingInventory.
  // Keep the new vanilla slot image private until its paired sidecar has
  // durably accepted the same bytes. A sidecar failure must not leave a
  // half-generated slot in global SRAM that a later unrelated write can
  // accidentally publish.
  uint8 staged_target_sram[0x500];
  Rando_InitNewSlotSram(staged_target_sram, settings->world_state);

  // Swordless (ALTTPR InitialSram::setSwordlessCurtains, initsramtable.asm:21,23):
  // pre-open the sword-cut curtains that would otherwise wall off the Agahnim
  // Tower altar room and the Skull Woods back entry — a swordless player can
  // never cut them. ROOM_DATA is at slot-relative offset 0x000; bit 0x80 is the
  // curtain quadrant of room byte 0x61 (Aga-Tower altar) / 0x93 (Skull Woods).
  // Baked at generation, so only fresh swordless slots get it (same caveat as the
  // other start-state bytes). PLAYTEST-PENDING: confirm both curtains render open.
  if (settings->mode_weapons == kModeWeapons_Swordless) {
    staged_target_sram[0x61] |= 0x80;  // Agahnim Tower altar curtain pre-opened
    staged_target_sram[0x93] |= 0x80;  // Skull Woods back-entry curtain pre-opened
  }

  Intro_FixCksum(staged_target_sram);

  if (!Rando_WriteSidecarSlot(slot_index, &slot, staged_target_sram, 0x500)) {
    if (err != NULL)
      snprintf(err, err_cap, "sidecar write failed for slot %u", (unsigned)slot_index);
    free(entries);
    return false;
  }
  // Commit the vanilla SRAM image too (sidecar first by spec; then sram.dat).
  memcpy(g_zenv.sram + slot_index * 0x500, staged_target_sram,
         sizeof(staged_target_sram));
  memcpy(g_zenv.sram + slot_index * 0x500 + 0xf00, staged_target_sram,
         sizeof(staged_target_sram));
  ZeldaWriteSram();

  // Apply recommended-features panel choices (if user toggled). Per spec
  // the user must opt in explicitly; we honor whatever state the panel
  // reflected before logic-required runtime overlays were added to the slot
  // sidecar. The user changed bits — that's the explicit opt-in.
  if (user_recommended_features0 != g_config.features0) {
    g_config.features0 = user_recommended_features0;
  }

  // If the caller wants the placement, hand it an independently malloc'd copy
  // (POD RandoPlacement, trivially copyable) that the caller owns. Do this
  // BEFORE freeing the working `entries`.
  if (out != NULL) {
    memset(out, 0, sizeof(*out));
    out->ok = true;
    out->used_forward_fill = used_forward_fill;
    out->goal_completable = goal_completable;
    out->race_mode = (settings->race_mode != 0);
    out->seed_u64 = seed_u64;
    memcpy(out->share_string, share_string, sizeof(out->share_string));
    memcpy(out->settings_hash, settings_hash_full, sizeof(out->settings_hash));
    out->has_medallion_assignment =
        copy_active_medallion_assignment(out->medallion_assignment);
    out->shape_filter_used = (shape_filter != NULL);
    out->shape_attempts_used = shape_attempts_used;
    out->shape_search_limit = shape_search_limit;
    out->shape_metrics = shape_metrics;
    out->has_spheres = true;
    out->spheres = spheres;
    out->has_hint_plan = true;
    out->hint_plan = hint_plan;
    if (table.count > 0) {
      RandoPlacement *copy =
          (RandoPlacement *)malloc(sizeof(RandoPlacement) * table.count);
      if (copy != NULL) {
        memcpy(copy, entries, sizeof(RandoPlacement) * table.count);
        out->placement.entries = copy;
        out->placement.count = table.count;
      }
    }
  }

  free(entries);

  // Reset sidecar cache + flag the slot active so the next file-select
  // render picks up the rando banner.
  SelectFile_NotifySlotWritten(slot_index);

  return true;
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_generate)
