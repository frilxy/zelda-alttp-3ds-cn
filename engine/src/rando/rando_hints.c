// rando_hints.c — deterministic Hints v2 semantic plan, delivery, and journal.
//
// One caller-owned dispatcher preserves frozen algorithm/schema 1/1 and builds
// current 2/2 plans from a maximal semantic mix before pruning to 0/5/10/15
// tiles and paid depth 0..3. The immutable plan is reconstructed by exact
// version pair and SHA-256 identity; discovery and paid presentation are
// separate mutable runtime state. Murahdahla and confirmed vanilla-dialogue
// corrections remain compatibility surfaces outside the 24 fact-id ceiling.
//
// Every player-facing rich message is completely encoded before installation
// or payment. Exact facts preserve the full location qualifier (or a stable,
// exact coordinate abbreviation); underfilled/customizer pools use neutral
// text rather than duplicates, ambiguity, truncation, or fabrication.

#include "rando_hints.h"
#include "item_ids.h"     // ITEM_* symbols (codegen from item_registry.yaml)
#include "location_ids.h" // LOC_* symbols (codegen from location_registry.yaml)
#include "hint_metadata.h" // Generated Hints v2 classification and aliases.
#include "rando.h"        // Rando_GetActiveSettings
#include "rando_logic.h"  // Rando_GetLocationName, Rando_GetItemName
#include "rando_rng.h"
#include "rando_settings.h"  // Settings_SetDefaults for Hints_SelfCheck
#include "../config.h"       // g_config.language (neutral DE/FR fallbacks)
#include "../types.h"
#include "../variables.h"    // g_ram (backs the g_rando_slot_active macro)
#include "../features.h"     // g_rando_slot_active (g_ram macro)
#include "../sprite_main.h"  // paid NPC handler choreography self-check
#include "../zelda_rtl.h"    // g_zenv.dialogue_flags
#include "third_party/sha256/sha256.h"

#include <stdio.h>
#include <stdlib.h>  // abort
#include <string.h>

// String IDs.
//   Indices 0-14 mirror `app/Services/HintService.php:59-75` exactly
//   (15 telepathic tiles).
//   Index 15 = ALTTPR Murahdahla.
//   Indices 16-19 = zelda3-fork extensions (NOT in ALTTPR), prefixed
//   `fork_` so spoiler-JSON diffing against ALTTPR seeds cleanly
//   distinguishes the core 16 from the fork extras.
// Indexed by `RandoHintNpc - 1` (skipping `_None`).
static const char *kRandoHintStringIds[kRandoHintNpc__Count - 1] = {
  "telepathic_tile_eastern_palace",
  "telepathic_tile_tower_of_hera_floor_4",
  "telepathic_tile_spectacle_rock",
  "telepathic_tile_swamp_entrance",
  "telepathic_tile_thieves_town_upstairs",
  "telepathic_tile_misery_mire",
  "telepathic_tile_palace_of_darkness",
  "telepathic_tile_desert_bonk_torch_room",
  "telepathic_tile_castle_tower",
  "telepathic_tile_ice_large_room",
  "telepathic_tile_turtle_rock",
  "telepathic_tile_ice_entrace",  // [sic] ALTTPR typo
  "telepathic_tile_ice_stalfos_knights_room",
  "telepathic_tile_tower_of_hera_entrance",
  "telepathic_tile_south_east_darkworld_cave",
  "murahdahla",
  // Fork extensions.
  "fork_storyteller",
  "fork_fortune_teller_kakariko",
  "fork_fortune_teller_dark_world",
  "fork_fortune_teller_lake_hylia",
};

// Per-NPC hint storage. Sized exactly for the enum; index 0 (_None)
// is unused but kept so RandoHintNpc literals map directly.
#define kRandoHintTextMax 160
#define kHintMaxCharsPerLine 13  // VWF is variable-width; 13 is a safe cap that
                                 // avoids overrun on the 256-px message box for
                                 // worst-case wide glyphs.
#define kHintTileCount \
  (kRandoHintNpc_TeleSouthEastDarkworldCave - \
   kRandoHintNpc_TeleEasternPalace + 1)

typedef struct HintEntry {
  uint8 active;
  uint8 fact_id;
  uint16 placement_loc_id;
  uint16 placement_item_id;
  char text[kRandoHintTextMax];
  // Plan facts are capped at 96 bytes, but Murahdahla is a separate
  // compatibility summary with the full journal-text bound. Keep the runtime
  // cache large enough for either source so installation never truncates
  // before the complete-fit renderer decides whether to show or fail closed.
  char game_text[kRandoHintTextMax];
} HintEntry;

static HintEntry g_hint_table[kRandoHintNpc__Count];

typedef struct RandoHintPaidPending {
  uint8 armed;
  uint8 exhausted;
  uint8 fact_id;
  uint8 queue_index;
  uint8 encoded_len;
  uint8 dialogue_flags;
  uint16 reserved;
  uint32 plan_epoch;
  uint8 encoded[256];
} RandoHintPaidPending;

typedef struct RandoHintPaidPromptContext {
  uint8 active;
  uint8 source;
  uint16 price;
  uint32 plan_epoch;
} RandoHintPaidPromptContext;

static RandoHintPlan g_hint_plan;
static RandoHintPlanStatus g_hint_plan_status = kRandoHintPlanStatus_None;
static uint32 g_hint_plan_epoch = 1;
static uint8 g_hint_discovered[kRandoHintDiscoveryBytes];
static RandoHintPersistedState g_hint_expected_state;
static bool g_hint_expected_state_set;
static RandoHintPersistedState g_hint_last_computed_state;
static bool g_hint_last_computed_state_set;
static uint8 g_hint_last_computed_fact_count;
static RandoHintPaidPending g_hint_paid_pending[kRandoHintPaidSourceCount];
static RandoHintPaidPromptContext g_hint_paid_prompt_context;

static int encode_hint_text_locale(const char *text, uint8 *out,
                                   bool *out_complete, uint8 dialogue_flags);
static const char *hint_paid_neutral_text(void);
static bool hint_uses_eu_commands(uint8 dialogue_flags);
static bool hint_rich_locale_supported(uint8 dialogue_flags);

// Vanilla dialogue whose item/location claim becomes false under randomized
// placement. These are runtime dialogue_message_index values, NOT the one-based
// row numbers printed in assets/dialogue.txt (see the OpenSpec audit).
typedef enum RandoDialogueHintRedirectKind {
  // A named fixed item moved away from the vanilla location. Resolve item->loc.
  kDialogueHintRedirect_ItemLocation = 0,
  // A physical surface describes the reward at one fixed location. Resolve loc->item.
  kDialogueHintRedirect_LocationItem,
  // Item->loc, but the replacement must retain a live Yes/No command.
  kDialogueHintRedirect_ItemLocationChoice,
} RandoDialogueHintRedirectKind;

typedef struct RandoDialogueHintRedirect {
  uint16 message_id;
  uint16 item_id;
  uint16 location_id;
  const char *source_name;
  uint8 flags;
  uint8 kind;
} RandoDialogueHintRedirect;

enum {
  kDialogueHintRedirectFlag_None = 0,
  kDialogueHintRedirectFlag_BumperCaveSign = 1,
};

static const RandoDialogueHintRedirect kDialogueHintRedirects[] = {
  {0x033, ITEM_Prize_GreenPendant, 0xFFFFu, "Sahasrahla",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocation},
  {0x036, ITEM_MoonPearl, 0xFFFFu, "Post-Agahnim Sahasrahla",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocation},
  {0x09E, ITEM_MoonPearl, 0xFFFFu, "Old mountain man",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocation},
  {0x0A8, ITEM_PieceOfHeart, LOC_Bumper_Cave, "Bumper Cave sign",
   kDialogueHintRedirectFlag_BumperCaveSign, kDialogueHintRedirect_LocationItem},
  {0x0E5, ITEM_OcarinaInactive, 0xFFFFu, "Stumpy / Flute Boy",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocationChoice},
  {0x125, ITEM_BookOfMudora, 0xFFFFu, "Aginah",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocation},
  {0x15D, ITEM_MoonPearl, 0xFFFFu, "Dark-World bully",
   kDialogueHintRedirectFlag_None, kDialogueHintRedirect_ItemLocation},
};

typedef enum RandoDialogueHintRedirectStatus {
  kDialogueHintRedirect_NotRecognized = 0,
  kDialogueHintRedirect_SlotInactive,
  kDialogueHintRedirect_UnsupportedLocale,
  kDialogueHintRedirect_SettingsUnavailable,
  kDialogueHintRedirect_HintsOff,
  kDialogueHintRedirect_DiscriminatorMismatch,
  kDialogueHintRedirect_PlacementUnavailable,
  kDialogueHintRedirect_ItemAbsent,
  kDialogueHintRedirect_LocationAbsent,
  kDialogueHintRedirect_Active,
} RandoDialogueHintRedirectStatus;

typedef struct RandoDialogueHintResolution {
  const RandoDialogueHintRedirect *redirect;
  RandoDialogueHintRedirectStatus status;
  uint16 item_id;
  uint16 location_id;
} RandoDialogueHintResolution;

static const RandoDialogueHintRedirect *dialogue_hint_redirect_for_msg(uint16 msg_id) {
  for (size_t i = 0; i < sizeof(kDialogueHintRedirects) / sizeof(kDialogueHintRedirects[0]); i++) {
    if (kDialogueHintRedirects[i].message_id == msg_id) return &kDialogueHintRedirects[i];
  }
  return NULL;
}

static bool dialogue_hint_redirect_discriminator_matches(
    const RandoDialogueHintRedirect *redirect, bool is_indoors,
    uint16 overworld_screen) {
  if (redirect == NULL) return false;
  switch (redirect->flags) {
  case kDialogueHintRedirectFlag_None:
    return true;
  case kDialogueHintRedirectFlag_BumperCaveSign:
    return !is_indoors && (uint8)overworld_screen == 0x4A;
  default:
    // Fail closed: adding a shared ID requires an explicit context predicate.
    return false;
  }
}

static RandoDialogueHintResolution resolve_dialogue_hint_for_context(
    uint16 msg_id, bool slot_active, uint8 dialogue_flags,
    const RandoSettings *settings, const RandoPlacementTable *placements,
    bool is_indoors, uint16 overworld_screen) {
  RandoDialogueHintResolution out = {
    dialogue_hint_redirect_for_msg(msg_id), kDialogueHintRedirect_NotRecognized,
    0xFFFFu, 0xFFFFu,
  };
  if (out.redirect == NULL) return out;
  if (out.redirect->kind != kDialogueHintRedirect_LocationItem)
    out.item_id = out.redirect->item_id;
  if (!slot_active) {
    out.status = kDialogueHintRedirect_SlotInactive;
    return out;
  }
  // Shared dialogue ids belong to vanilla everywhere except their proven
  // physical surface. Test that discriminator before any randomizer-owned
  // neutral path so Off/missing metadata cannot claim an unrelated use.
  if (!dialogue_hint_redirect_discriminator_matches(out.redirect, is_indoors,
                                                     overworld_screen)) {
    out.status = kDialogueHintRedirect_DiscriminatorMismatch;
    return out;
  }
  if (settings == NULL) {
    out.status = kDialogueHintRedirect_SettingsUnavailable;
    return out;
  }
  if (settings->hints != kHintsMode_On) {
    out.status = kDialogueHintRedirect_HintsOff;
    return out;
  }
  if (!hint_rich_locale_supported(dialogue_flags)) {
    out.status = kDialogueHintRedirect_UnsupportedLocale;
    return out;
  }
  if (placements == NULL || placements->entries == NULL) {
    out.status = kDialogueHintRedirect_PlacementUnavailable;
    return out;
  }

  if (out.redirect->kind == kDialogueHintRedirect_LocationItem) {
    for (uint16 i = 0; i < placements->count; i++) {
      if (placements->entries[i].location_id == out.redirect->location_id) {
        out.location_id = placements->entries[i].location_id;
        out.item_id = placements->entries[i].item_id;
        break;
      }
    }
    out.status = out.location_id == 0xFFFFu ? kDialogueHintRedirect_LocationAbsent
                                            : kDialogueHintRedirect_Active;
    return out;
  }

  // Customizers may deliberately produce duplicate target items. Pick the
  // lowest location ID independently of placement-table iteration order.
  for (uint16 i = 0; i < placements->count; i++) {
    if (placements->entries[i].item_id == out.item_id &&
        (out.location_id == 0xFFFFu ||
         placements->entries[i].location_id < out.location_id))
      out.location_id = placements->entries[i].location_id;
  }
  out.status = out.location_id == 0xFFFFu ? kDialogueHintRedirect_ItemAbsent
                                          : kDialogueHintRedirect_Active;
  return out;
}

static RandoDialogueHintResolution resolve_active_dialogue_hint(uint16 msg_id) {
  if (!g_rando_slot_active) {
    return resolve_dialogue_hint_for_context(
        msg_id, false, g_zenv.dialogue_flags, NULL, NULL,
        player_is_indoors != 0, overworld_screen_index);
  }
  return resolve_dialogue_hint_for_context(
      msg_id, true, g_zenv.dialogue_flags, Rando_GetActiveSettings(),
      Placement_GetActive(), player_is_indoors != 0, overworld_screen_index);
}

static const GeneratedHintItemMetadata *hint_item_metadata(uint16 item_id) {
  if (item_id >= kGeneratedHintItemMetadataCount) return NULL;
  const GeneratedHintItemMetadata *metadata =
      &kGeneratedHintItemMetadata[item_id];
  if (metadata->short_name == NULL || metadata->qualified_name == NULL ||
      metadata->compact_qualified_name == NULL)
    return NULL;
  return metadata;
}

const char *Rando_GetHintItemDisplayName(uint16 item_id, bool compact) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  if (metadata == NULL) return NULL;
  return compact ? metadata->compact_qualified_name
                 : metadata->qualified_name;
}

// Unknown ids fail closed. The generated table covers ITEM__COUNT and codegen
// refuses an unclassified registry append, so this path is only corruption or
// a mismatched artifact.
static bool item_is_junk(uint16 item_id) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  return metadata == NULL ||
         (metadata->flags & kGeneratedHintItemFlag_Junk) != 0;
}

static bool hint_useful_item(uint16 item_id) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  return metadata != NULL &&
         (metadata->flags & kGeneratedHintItemFlag_Useful) != 0;
}

static bool hint_major_item(uint16 item_id) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  return metadata != NULL &&
         (metadata->flags & kGeneratedHintItemFlag_Major) != 0;
}

static bool hint_priority_item(uint16 item_id) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  return metadata != NULL &&
         (metadata->flags & kGeneratedHintItemFlag_Priority) != 0;
}

// Group ids 1..255 are stable declarative semantic families. Ungrouped items
// use a disjoint 0x100+item-id key so each remains its own diversity family.
static uint16 hint_item_diversity_key(uint16 item_id) {
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  if (metadata == NULL) return 0xFFFFu;
  return metadata->diversity_group != 0
             ? metadata->diversity_group
             : (uint16)(0x100u + item_id);
}

static bool loc_is_medallion_config(uint16 loc_id) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    if (kRandoLocations[i].id == loc_id)
      return kRandoLocations[i].type == LOCTYPE_Medallion;
  }
  return false;
}

// Seed the sub-RNG deterministically from the placement table. Same
// (settings, seed_u64) → same placement table → same digest → same
// hint sub-RNG → same hints.
static void seed_hint_rng(const RandoPlacementTable *placements,
                          uint64 *out_seed) {
  uint8 digest[32];
  PlacementTable_ComputeDigest(placements, digest);
  uint64 seed = 0;
  for (int i = 0; i < 8; i++) {
    seed = (seed << 8) | (uint64)digest[i];
  }
  // XOR a magic constant so the hint RNG is decoupled from any
  // future direct-use of the placement digest as a seed elsewhere.
  // Magic = "HINT" ASCII repeated (48 49 4e 54 48 49 4e 54).
  *out_seed = seed ^ 0x48494E5448494E54ULL;
}

// Fisher-Yates shuffle of `arr[0..n)` using the sub-RNG.
static void shuffle_u16(RandoRng *rng, uint16 *arr, uint16 n) {
  for (uint16 i = n - 1; i > 0; i--) {
    uint32 j = Rng_NextRange(rng, i + 1);
    uint16 tmp = arr[i];
    arr[i] = arr[(uint16)j];
    arr[(uint16)j] = tmp;
  }
}

// Friendly location: retain the complete registry identity, including every
// Left/Right/Middle/sub-spot qualifier. Generated compact metadata is used only
// if the complete registry identity does not fit.
static bool hint_friendly_loc(const char *in, char *out, int outsz) {
  if (in == NULL || out == NULL || outsz <= 0) return false;
  int n = snprintf(out, (size_t)outsz, "%s", in);
  return n >= 0 && n < outsz;
}

static const GeneratedHintLocationMetadata *hint_location_metadata(
    uint16 location_id) {
  size_t lo = 0, hi = kGeneratedHintLocationMetadataCount;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const GeneratedHintLocationMetadata *row =
        &kGeneratedHintLocationMetadata[mid];
    if (row->location_id < location_id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= kGeneratedHintLocationMetadataCount ||
      kGeneratedHintLocationMetadata[lo].location_id != location_id)
    return NULL;
  return &kGeneratedHintLocationMetadata[lo];
}

static bool hint_compact_loc(uint16 location_id, char *out, int outsz) {
  if (out == NULL || outsz <= 0) return false;
  const GeneratedHintLocationMetadata *metadata =
      hint_location_metadata(location_id);
  if (metadata == NULL || metadata->compact_name == NULL) {
    out[0] = '\0';
    return false;
  }
  int n = snprintf(out, (size_t)outsz, "%s", metadata->compact_name);
  return n >= 0 && n < outsz;
}

static const GeneratedHintRegionMetadata *hint_region_metadata(
    uint16 region_id) {
  if (region_id >= kGeneratedHintRegionMetadataCount) return NULL;
  const GeneratedHintRegionMetadata *metadata =
      &kGeneratedHintRegionMetadata[region_id];
  return metadata->region_id == region_id ? metadata : NULL;
}

static bool npc_hint_item_name(uint16 item_id, char *out, int outsz) {
  if (out == NULL || outsz <= 0) return false;
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  if (metadata == NULL) return false;
  int n = snprintf(out, (size_t)outsz, "%s", metadata->short_name);
  return n >= 0 && n < outsz;
}

static bool location_hint_item_name(uint16 item_id, char *out, int outsz) {
  if (out == NULL || outsz <= 0) return false;
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  if (metadata == NULL) return false;
  int n = snprintf(out, (size_t)outsz, "%s", metadata->qualified_name);
  return n >= 0 && n < outsz;
}

static bool compact_location_hint_item_name(uint16 item_id, char *out,
                                            int outsz) {
  if (out == NULL || outsz <= 0) return false;
  const GeneratedHintItemMetadata *metadata = hint_item_metadata(item_id);
  if (metadata == NULL) return false;
  int n = snprintf(out, (size_t)outsz, "%s",
                   metadata->compact_qualified_name);
  return n >= 0 && n < outsz;
}

typedef struct HintCandidate {
  uint16 location_id;
  uint16 item_id;
  uint16 region_id;
  uint8 selected;
} HintCandidate;

// File-static scratch keeps the candidate/index pools off the Switch/runtime
// stack. Plan construction is main-thread only.
static HintCandidate s_hint_candidates[kRandoLocationCapacity];
static uint16 s_hint_candidate_pool[kRandoLocationCapacity];

static const RandoLocationDef *hint_location_def(uint16 location_id) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (kRandoLocations[i].id == location_id) return &kRandoLocations[i];
  return NULL;
}

// Match the reachability engine's effective-location ordering exactly:
// world-state predicate metadata first, then the active per-seed entrance
// override. Region summaries and typed exact facts must describe the same world
// graph the placer/runtime actually use.
static uint16 hint_effective_region(const RandoLocationDef *loc,
                                    const RandoSettings *settings) {
  if (loc == NULL) return 0xFFFFu;
  uint16 region = loc->region_id;
  if (settings != NULL) {
    const RandoLocationPredOverride *ov =
        Rando_FindPredicateOverride(loc->id, settings->world_state);
    if (ov != NULL && ov->region_override != 0xFFFFu)
      region = ov->region_override;
  }
  uint16 entrance_region = Rando_GetEntranceRegionOverride(loc->id);
  if (entrance_region != 0xFFFFu) region = entrance_region;
  return region;
}

static int hint_candidate_compare(const void *a, const void *b) {
  const HintCandidate *x = (const HintCandidate *)a;
  const HintCandidate *y = (const HintCandidate *)b;
  if (x->location_id != y->location_id)
    return x->location_id < y->location_id ? -1 : 1;
  if (x->item_id != y->item_id)
    return x->item_id < y->item_id ? -1 : 1;
  return 0;
}

static bool hint_high_friction_location(uint16 location_id) {
  for (size_t i = 0; i < kGeneratedHintHighFrictionLocationCount; i++)
    if (kGeneratedHintHighFrictionLocations[i] == location_id) return true;
  return false;
}

static bool hint_text_fits_game(const char *text) {
  uint8 encoded[241];
  bool complete = false;
  (void)encode_hint_text_locale(text, encoded, &complete, 0);
  return complete;
}

static bool hint_build_exact_text(uint16 item_id, uint16 location_id,
                                  char *full, size_t full_cap,
                                  char *game, size_t game_cap) {
  char item[64], compact_item[64], compact[96], candidate[192];
  const char *raw_loc = Rando_GetLocationName(location_id);
  if (raw_loc == NULL ||
      !location_hint_item_name(item_id, item, (int)sizeof(item)))
    return false;
  int n = snprintf(full, full_cap, "%s is at %s", item, raw_loc);
  if (n < 0 || (size_t)n >= full_cap) return false;

  if (hint_text_fits_game(full)) {
    n = snprintf(game, game_cap, "%s", full);
    return n >= 0 && (size_t)n < game_cap;
  }
  if (!compact_location_hint_item_name(
          item_id, compact_item, (int)sizeof(compact_item)) ||
      !hint_compact_loc(location_id, compact, (int)sizeof(compact)))
    return false;
  n = snprintf(candidate, sizeof(candidate), "%s at %s",
               compact_item, compact);
  if (n >= 0 && (size_t)n < sizeof(candidate) && hint_text_fits_game(candidate)) {
    n = snprintf(game, game_cap, "%s", candidate);
    return n >= 0 && (size_t)n < game_cap;
  }
  // A future identity that defeats the lossless compact aliases is not
  // hintable until its metadata is authored. Never substitute an opaque
  // numeric label or a truncated claim.
  return false;
}

static uint8 hint_plan_add_exact(RandoHintPlan *plan, HintCandidate *candidate,
                                 RandoHintFactKind kind, bool primary) {
  if (plan->fact_count >= kRandoHintPlanMaxFacts || candidate->selected)
    return kRandoHintNoFact;
  RandoHintFact *fact = &plan->facts[plan->fact_count];
  memset(fact, 0, sizeof(*fact));
  fact->fact_id = plan->fact_count;
  fact->kind = (uint8)kind;
  fact->template_id = kRandoHintTemplate_ExactPlacement;
  fact->primary = primary ? 1 : 0;
  fact->location_id = candidate->location_id;
  fact->item_id = candidate->item_id;
  fact->region_id = candidate->region_id;
  if (!hint_build_exact_text(fact->item_id, fact->location_id,
                             fact->text, sizeof(fact->text),
                             fact->game_text, sizeof(fact->game_text)))
    return kRandoHintNoFact;
  candidate->selected = 1;
  plan->fact_count++;
  return fact->fact_id;
}

static uint8 hint_plan_add_text(RandoHintPlan *plan, RandoHintFactKind kind,
                                RandoHintTemplateId template_id,
                                uint16 region_id, uint16 template_parameter,
                                const char *full, const char *short_text) {
  if (plan->fact_count >= kRandoHintPlanMaxFacts || full == NULL ||
      short_text == NULL || !hint_text_fits_game(short_text))
    return kRandoHintNoFact;
  RandoHintFact *fact = &plan->facts[plan->fact_count];
  memset(fact, 0, sizeof(*fact));
  int n = snprintf(fact->text, sizeof(fact->text), "%s", full);
  int m = snprintf(fact->game_text, sizeof(fact->game_text), "%s", short_text);
  if (n < 0 || m < 0 || (size_t)n >= sizeof(fact->text) ||
      (size_t)m >= sizeof(fact->game_text))
    return kRandoHintNoFact;
  fact->fact_id = plan->fact_count;
  fact->kind = (uint8)kind;
  fact->template_id = (uint8)template_id;
  fact->primary = 1;
  fact->location_id = 0xFFFFu;
  fact->item_id = 0xFFFFu;
  fact->region_id = region_id;
  fact->template_parameter = template_parameter;
  plan->fact_count++;
  return fact->fact_id;
}

typedef enum HintCandidateClass {
  kHintCandidateClass_Priority,
  kHintCandidateClass_Major,
  kHintCandidateClass_HighFriction,
  kHintCandidateClass_Useful,
  // Algorithm-2 literal quota classes. The legacy classes above are
  // intentionally hierarchical and frozen for 1/1; these exclude stronger
  // classes so an extra priority item cannot be relabeled "other major", nor
  // an extra major item relabeled "other useful".
  kHintCandidateClass_OtherMajor,
  kHintCandidateClass_OtherUseful,
} HintCandidateClass;

static bool hint_candidate_matches(const HintCandidate *candidate,
                                   HintCandidateClass cls) {
  switch (cls) {
  case kHintCandidateClass_Priority:
    return hint_priority_item(candidate->item_id);
  case kHintCandidateClass_Major:
    return hint_major_item(candidate->item_id);
  case kHintCandidateClass_HighFriction:
    return hint_high_friction_location(candidate->location_id);
  case kHintCandidateClass_Useful:
    return hint_useful_item(candidate->item_id);
  case kHintCandidateClass_OtherMajor:
    return hint_major_item(candidate->item_id) &&
           !hint_priority_item(candidate->item_id);
  case kHintCandidateClass_OtherUseful:
    return hint_useful_item(candidate->item_id) &&
           !hint_major_item(candidate->item_id);
  default:
    return false;
  }
}

static bool hint_plan_has_exact_family(const RandoHintPlan *plan,
                                       uint16 item_id) {
  uint16 family = hint_item_diversity_key(item_id);
  for (uint8 i = 0; i < plan->fact_count; i++)
    if (plan->facts[i].location_id != 0xFFFFu &&
        hint_item_diversity_key(plan->facts[i].item_id) == family)
      return true;
  return false;
}

static uint8 hint_take_exact_class(RandoHintPlan *plan,
                                   HintCandidate *candidates,
                                   uint16 candidate_count, RandoRng *rng,
                                   HintCandidateClass cls,
                                   RandoHintFactKind kind, uint8 limit,
                                   uint8 primary_ids[kRandoHintPlanPrimaryTarget],
                                   uint8 primary_count) {
  uint16 pool_count = 0;
  for (uint16 i = 0; i < candidate_count; i++)
    if (!candidates[i].selected && hint_candidate_matches(&candidates[i], cls))
      s_hint_candidate_pool[pool_count++] = i;
  if (pool_count > 1) shuffle_u16(rng, s_hint_candidate_pool, pool_count);
  // Prefer distinct item identities across the whole primary deck. Duplicate
  // copies remain a deterministic second-pass fallback so a customizer with a
  // tiny item vocabulary can still underfill/fill honestly.
  for (uint8 pass = 0; pass < 2 && limit != 0 &&
                       primary_count < kRandoHintPlanPrimaryTarget; pass++) {
    for (uint16 i = 0; i < pool_count && limit != 0 &&
                         primary_count < kRandoHintPlanPrimaryTarget; i++) {
      HintCandidate *candidate =
          &candidates[s_hint_candidate_pool[i]];
      if (candidate->selected) continue;
      bool duplicate =
          hint_plan_has_exact_family(plan, candidate->item_id);
      if ((pass == 0 && duplicate) || (pass == 1 && !duplicate)) continue;
      uint8 fact_id = hint_plan_add_exact(plan, candidate, kind, true);
      if (fact_id == kRandoHintNoFact) continue;
      primary_ids[primary_count++] = fact_id;
      limit--;
    }
  }
  return primary_count;
}

static const char *hint_goal_name(uint8 goal) {
  switch (goal) {
  case kGoal_Ganon: return "Defeat Ganon";
  case kGoal_FastGanon: return "Fast Ganon";
  case kGoal_Dungeons: return "All Dungeons";
  case kGoal_Pedestal: return "Pedestal";
  case kGoal_TriforceHunt: return "Triforce Hunt";
  case kGoal_GanonHunt: return "Ganon Hunt";
  case kGoal_Completionist: return "Completionist";
  default: return "Unknown";
  }
}

static uint8 hint_add_goal_and_settings(
    RandoHintPlan *plan, const RandoSettings *settings,
    uint8 primary_ids[kRandoHintPlanPrimaryTarget], uint8 primary_count) {
  char full[160], short_text[96];
  const char *goal = hint_goal_name(settings->goal);
  snprintf(full, sizeof(full), "The seed goal is %s", goal);
  snprintf(short_text, sizeof(short_text), "Goal is %s", goal);
  uint8 id = hint_plan_add_text(plan, kRandoHintFact_Goal,
                                kRandoHintTemplate_Goal, 0xFFFFu,
                                settings->goal,
                                full, short_text);
  if (id != kRandoHintNoFact &&
      primary_count < kRandoHintPlanPrimaryTarget)
    primary_ids[primary_count++] = id;

  static const char *const kSettingTexts[] = {
    "Entrances are shuffled",
    "Dungeon doors are shuffled",
    "Dungeon chains are active",
    "Dungeon keys may be wild",
    "Enemy souls are shuffled",
    "NPC souls are shuffled",
    "Shops contain checks",
    "Terrain contains checks",
    "Bosses are shuffled",
    "Enemy drops are shuffled",
  };
  bool enabled[] = {
    settings->shuffle_cave_entrances || settings->shuffle_dungeon_entrances,
    Settings_EffectiveDoorShuffle(settings) != kDoorShuffle_Vanilla,
    settings->dungeon_chains != 0,
    settings->dungeon_small_keys_mode == kDungeonItemMode_Wild ||
        settings->dungeon_big_keys_mode == kDungeonItemMode_Wild,
    settings->souls_shuffle != 0,
    settings->npc_souls != 0,
    settings->shopsanity != 0,
    settings->grass_shuffle != 0 || settings->rock_shuffle != 0 ||
        settings->bonk_shuffle != 0,
    settings->boss_shuffle != 0,
    settings->enemy_drop_checks != 0,
  };
  uint8 added = 1;
  for (size_t i = 0; i < sizeof(enabled) / sizeof(enabled[0]) && added < 3 &&
                         primary_count < kRandoHintPlanPrimaryTarget; i++) {
    if (!enabled[i]) continue;
    id = hint_plan_add_text(plan, kRandoHintFact_ActiveSetting,
                            kRandoHintTemplate_ActiveSetting, 0xFFFFu,
                            (uint16)(i + 1u),
                            kSettingTexts[i], kSettingTexts[i]);
    if (id == kRandoHintNoFact) continue;
    primary_ids[primary_count++] = id;
    added++;
  }
  return primary_count;
}

static uint8 hint_add_additional_settings(
    RandoHintPlan *plan, const RandoSettings *settings, uint8 limit,
    uint8 primary_ids[kRandoHintPlanPrimaryTarget], uint8 primary_count) {
  static const char *const kSettingTexts[] = {
    "Entrances are shuffled",
    "Dungeon doors are shuffled",
    "Dungeon chains are active",
    "Dungeon keys may be wild",
    "Enemy souls are shuffled",
    "NPC souls are shuffled",
    "Shops contain checks",
    "Terrain contains checks",
    "Bosses are shuffled",
    "Enemy drops are shuffled",
  };
  bool enabled[] = {
    settings->shuffle_cave_entrances || settings->shuffle_dungeon_entrances,
    Settings_EffectiveDoorShuffle(settings) != kDoorShuffle_Vanilla,
    settings->dungeon_chains != 0,
    settings->dungeon_small_keys_mode == kDungeonItemMode_Wild ||
        settings->dungeon_big_keys_mode == kDungeonItemMode_Wild,
    settings->souls_shuffle != 0,
    settings->npc_souls != 0,
    settings->shopsanity != 0,
    settings->grass_shuffle != 0 || settings->rock_shuffle != 0 ||
        settings->bonk_shuffle != 0,
    settings->boss_shuffle != 0,
    settings->enemy_drop_checks != 0,
  };
  for (size_t setting = 0;
       setting < sizeof(enabled) / sizeof(enabled[0]) && limit != 0 &&
       primary_count < kRandoHintPlanPrimaryTarget;
       setting++) {
    if (!enabled[setting]) continue;
    uint16 parameter = (uint16)(setting + 1u);
    bool present = false;
    for (uint8 fact_id = 0; fact_id < plan->fact_count; fact_id++) {
      if (plan->facts[fact_id].kind == kRandoHintFact_ActiveSetting &&
          plan->facts[fact_id].template_parameter == parameter) {
        present = true;
        break;
      }
    }
    if (present) continue;
    uint8 id = hint_plan_add_text(
        plan, kRandoHintFact_ActiveSetting,
        kRandoHintTemplate_ActiveSetting, 0xFFFFu, parameter,
        kSettingTexts[setting], kSettingTexts[setting]);
    if (id == kRandoHintNoFact) continue;
    primary_ids[primary_count++] = id;
    limit--;
  }
  return primary_count;
}

static uint8 hint_add_region_value(
    RandoHintPlan *plan, const HintCandidate *candidates, uint16 candidate_count,
    uint8 limit,
    uint8 primary_ids[kRandoHintPlanPrimaryTarget], uint8 primary_count) {
  static uint16 region_counts[kReachabilityMaxRegions];
  memset(region_counts, 0, sizeof(region_counts));
  for (uint16 i = 0; i < candidate_count; i++) {
    uint16 region = candidates[i].region_id;
    if (region < kReachabilityMaxRegions &&
        hint_useful_item(candidates[i].item_id))
      region_counts[region]++;
  }
  // Repeated calls are the WorldInfo shortage-backfill path. Do not emit a
  // second summary for a region already represented in this plan.
  for (uint8 i = 0; i < plan->fact_count; i++) {
    if (plan->facts[i].kind == kRandoHintFact_RegionValue &&
        plan->facts[i].region_id < kReachabilityMaxRegions)
      region_counts[plan->facts[i].region_id] = 0;
  }
  uint8 added = 0;
  while (added < limit && primary_count < kRandoHintPlanPrimaryTarget) {
    uint16 best = 0xFFFFu;
    uint16 best_count = 1;  // Require at least two useful placements.
    for (uint16 region = 0; region < kReachabilityMaxRegions; region++) {
      if (region_counts[region] > best_count) {
        best = region;
        best_count = region_counts[region];
      }
    }
    if (best == 0xFFFFu) break;
    region_counts[best] = 0;
    const GeneratedHintRegionMetadata *region =
        hint_region_metadata(best);
    if (region == NULL || region->qualified_name == NULL ||
        region->compact_name == NULL)
      continue;
    char full[160], short_text[96];
    int n = snprintf(full, sizeof(full), "%s contains %u useful items",
                     region->qualified_name, (unsigned)best_count);
    int m = snprintf(short_text, sizeof(short_text), "%s has %u useful",
                     region->compact_name, (unsigned)best_count);
    if (n < 0 || m < 0 || (size_t)n >= sizeof(full) ||
        (size_t)m >= sizeof(short_text) || !hint_text_fits_game(short_text))
      continue;
    uint8 id = hint_plan_add_text(plan, kRandoHintFact_RegionValue,
                                  kRandoHintTemplate_RegionValue, best,
                                  best_count,
                                  full, short_text);
    if (id != kRandoHintNoFact) {
      primary_ids[primary_count++] = id;
      added++;
    }
  }
  return primary_count;
}

static void hint_hash_u8(SHA256_CTX *ctx, uint8 value) {
  sha256_update(ctx, &value, 1);
}

static void hint_hash_u16(SHA256_CTX *ctx, uint16 value) {
  uint8 le[2] = {(uint8)value, (uint8)(value >> 8)};
  sha256_update(ctx, le, sizeof(le));
}

static bool hint_version_pair_supported(uint16 algorithm_version,
                                        uint16 text_schema_version) {
  return (algorithm_version == kRandoHintPlanLegacyAlgorithmVersion &&
          text_schema_version == kRandoHintLegacyTextSchemaVersion) ||
         (algorithm_version == kRandoHintPlanAlgorithmVersion &&
          text_schema_version == kRandoHintTextSchemaVersion);
}

// Sidecar v14 and snapshot HintState type 11 first ship with the current 2/2
// pair. Algorithm 1 remains a direct, frozen builder for the authenticated
// generator-156 race-reveal path; it is not an ordinary persisted slot/replay
// format that current saves may import, bless, or perpetuate.
static bool hint_persisted_version_pair_supported(
    uint16 algorithm_version, uint16 text_schema_version) {
  return algorithm_version == kRandoHintPlanAlgorithmVersion &&
         text_schema_version == kRandoHintTextSchemaVersion;
}

static void hint_plan_compute_digest(RandoHintPlan *plan) {
  static const uint8 domain_v1[] = "Z3R-HINT-PLAN";
  static const uint8 domain_v2[] = "Z3R-HINT-PLAN-V2";
  SHA256_CTX ctx;
  sha256_init(&ctx);
  if (plan->algorithm_version == kRandoHintPlanLegacyAlgorithmVersion)
    sha256_update(&ctx, domain_v1, sizeof(domain_v1) - 1);
  else
    sha256_update(&ctx, domain_v2, sizeof(domain_v2) - 1);
  hint_hash_u16(&ctx, plan->algorithm_version);
  hint_hash_u16(&ctx, plan->text_schema_version);
  hint_hash_u8(&ctx, plan->mode);
  if (plan->algorithm_version != kRandoHintPlanLegacyAlgorithmVersion) {
    hint_hash_u8(&ctx, plan->tile_target);
    hint_hash_u8(&ctx, plan->mix);
    hint_hash_u8(&ctx, plan->paid_depth);
  }
  hint_hash_u8(&ctx, plan->fact_count);
  uint8 primary_count = 0;
  for (uint8 i = 0; i < plan->fact_count; i++)
    primary_count += plan->facts[i].primary != 0;
  hint_hash_u8(&ctx, primary_count);
  hint_hash_u8(&ctx, (uint8)(plan->fact_count - primary_count));
  for (uint8 i = 0; i < plan->fact_count; i++) {
    const RandoHintFact *fact = &plan->facts[i];
    hint_hash_u8(&ctx, fact->fact_id);
    hint_hash_u8(&ctx, fact->kind);
    hint_hash_u8(&ctx, fact->template_id);
    hint_hash_u8(&ctx, fact->primary);
    hint_hash_u16(&ctx, fact->location_id);
    hint_hash_u16(&ctx, fact->item_id);
    hint_hash_u16(&ctx, fact->region_id);
    hint_hash_u16(&ctx, fact->template_parameter);
  }
  for (uint8 npc = 0; npc < kRandoHintNpc__Count; npc++)
    hint_hash_u8(&ctx, plan->primary_fact_by_npc[npc]);
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    hint_hash_u8(&ctx, plan->paid_count[source]);
    for (uint8 q = 0; q < kRandoHintPaidQueueCapacity; q++)
      hint_hash_u8(&ctx, plan->paid_queue[source][q]);
  }
  sha256_final(&ctx, plan->digest);
}

// Self-check-only golden fingerprint. Unlike the persisted semantic digest,
// this deliberately includes complete human/game text and Murahdahla so one
// literal protects the full v1 builder contract from drift in transitive
// dependencies (settings canonicalization, placement digest/RNG, effective
// regions, or text encoding metadata). It hashes both the source game text and
// the actual original/US encoded glyph stream so encoder/wrapping drift is
// visible. Its byte encoding is explicit and platform-independent; no C
// padding enters the hash.
static bool hint_plan_compute_compat_fingerprint(
    const RandoHintPlan *plan, uint8 out[kRandoHintPlanDigestBytes]) {
  static const uint8 domain[] = "Z3R-HINT-COMPAT-GOLDEN";
  SHA256_CTX ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, domain, sizeof(domain) - 1);
  hint_hash_u16(&ctx, plan->algorithm_version);
  hint_hash_u16(&ctx, plan->text_schema_version);
  hint_hash_u8(&ctx, plan->mode);
  hint_hash_u8(&ctx, plan->fact_count);
  sha256_update(&ctx, plan->digest, sizeof(plan->digest));
  for (uint8 i = 0; i < plan->fact_count; i++) {
    const RandoHintFact *fact = &plan->facts[i];
    hint_hash_u8(&ctx, fact->fact_id);
    hint_hash_u8(&ctx, fact->kind);
    hint_hash_u8(&ctx, fact->template_id);
    hint_hash_u8(&ctx, fact->primary);
    hint_hash_u16(&ctx, fact->location_id);
    hint_hash_u16(&ctx, fact->item_id);
    hint_hash_u16(&ctx, fact->region_id);
    hint_hash_u16(&ctx, fact->template_parameter);
    size_t text_len = strlen(fact->text);
    size_t game_len = strlen(fact->game_text);
    uint8 encoded[241];
    bool encoded_complete = false;
    int encoded_len = encode_hint_text_locale(fact->game_text, encoded,
                                              &encoded_complete, 0);
    if (!encoded_complete || encoded_len < 0 ||
        encoded_len > (int)sizeof(encoded) - 1) {
      memset(out, 0, kRandoHintPlanDigestBytes);
      return false;
    }
    hint_hash_u16(&ctx, (uint16)text_len);
    sha256_update(&ctx, (const uint8 *)fact->text, text_len);
    hint_hash_u16(&ctx, (uint16)game_len);
    sha256_update(&ctx, (const uint8 *)fact->game_text, game_len);
    hint_hash_u8(&ctx, encoded_complete ? 1 : 0);
    hint_hash_u16(&ctx, (uint16)encoded_len);
    if (encoded_len > 0)
      sha256_update(&ctx, encoded, (size_t)encoded_len);
  }
  for (uint8 npc = 0; npc < kRandoHintNpc__Count; npc++)
    hint_hash_u8(&ctx, plan->primary_fact_by_npc[npc]);
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    hint_hash_u8(&ctx, plan->paid_count[source]);
    for (uint8 queue = 0; queue < kRandoHintPaidQueueCapacity; queue++)
      hint_hash_u8(&ctx, plan->paid_queue[source][queue]);
  }
  hint_hash_u8(&ctx, plan->murahdahla_active);
  size_t murah_len = strlen(plan->murahdahla_text);
  hint_hash_u16(&ctx, (uint16)murah_len);
  sha256_update(&ctx, (const uint8 *)plan->murahdahla_text, murah_len);
  sha256_final(&ctx, out);
  return true;
}

static void hint_build_murahdahla(RandoHintPlan *plan,
                                  const RandoSettings *settings,
                                  const RandoPlacementTable *placements) {
  if (settings->goal != kGoal_TriforceHunt &&
      settings->goal != kGoal_GanonHunt)
    return;
  static uint8 seen_regions[kReachabilityMaxRegions];
  memset(seen_regions, 0, sizeof(seen_regions));
  uint16 pieces = 0, regions = 0;
  uint16 count = placements->count;
  if (count > kRandoLocationCapacity) count = kRandoLocationCapacity;
  for (uint16 i = 0; i < count; i++) {
    if (placements->entries[i].item_id != ITEM_TriforcePiece) continue;
    pieces++;
    const RandoLocationDef *loc =
        hint_location_def(placements->entries[i].location_id);
    uint16 region = hint_effective_region(loc, settings);
    if (region >= kReachabilityMaxRegions) continue;
    if (!seen_regions[region]) {
      seen_regions[region] = 1;
      regions++;
    }
  }
  int n = snprintf(plan->murahdahla_text, sizeof(plan->murahdahla_text),
                   "Murahdahla: %u Triforce piece%s placed across %u region%s.",
                   (unsigned)pieces, pieces == 1 ? "" : "s",
                   (unsigned)regions, regions == 1 ? "" : "s");
  if (n >= 0 && (size_t)n < sizeof(plan->murahdahla_text))
    plan->murahdahla_active = 1;
}

static bool hint_build_variety_compat_plan(
    const RandoSettings *settings,
    const RandoPlacementTable *placements,
    const RandoSpheres *spheres,
    uint16 algorithm_version,
    uint16 text_schema_version,
    RandoHintPlan *out) {
  if (settings == NULL || placements == NULL || out == NULL) return false;
  (void)spheres;  // Reserved for a compatible future plan-algorithm version.
  // Generation callers may retain requested axes that canonical persistence
  // normalizes (Retro keys, incompatible entrance/chains/enemy tiers, and so
  // on). Build from the exact canonical settings image activation will recover,
  // otherwise a new slot can certify one deck and immediately digest-mismatch
  // on reload.
  uint8 settings_canonical[kSettingsCanonicalLen];
  RandoSettings normalized_settings;
  Settings_CanonicalSerialize(settings, settings_canonical);
  if (Settings_CanonicalDeserialize(settings_canonical,
                                    &normalized_settings) != 0)
    return false;
  settings = &normalized_settings;
  memset(out, 0, sizeof(*out));
  memset(out->primary_fact_by_npc, kRandoHintNoFact,
         sizeof(out->primary_fact_by_npc));
  memset(out->paid_queue, kRandoHintNoFact, sizeof(out->paid_queue));
  out->algorithm_version = algorithm_version;
  out->text_schema_version = text_schema_version;
  out->mode = settings->hints;
  out->tile_target = 15;
  out->mix = kHintMix_Variety;
  out->paid_depth = kRandoHintPaidQueueCapacity;
  if (!hint_version_pair_supported(algorithm_version, text_schema_version))
    return false;
  if (settings->hints == kHintsMode_Off) {
    out->tile_target = 0;
    out->mix = kHintMix_Variety;
    out->paid_depth = 0;
    hint_plan_compute_digest(out);
    return true;
  }
  if (settings->hints != kHintsMode_Balanced) return false;

  uint16 candidate_count = 0;
  uint16 placement_count = placements->count;
  if (placement_count > kRandoLocationCapacity)
    placement_count = kRandoLocationCapacity;
  for (uint16 i = 0; i < placement_count; i++) {
    uint16 loc_id = placements->entries[i].location_id;
    uint16 item_id = placements->entries[i].item_id;
    if (loc_is_medallion_config(loc_id) || item_is_junk(item_id)) continue;
    const RandoLocationDef *loc = hint_location_def(loc_id);
    uint16 region_id = hint_effective_region(loc, settings);
    if (loc == NULL || region_id == 0xFFFFu) continue;
    HintCandidate *candidate = &s_hint_candidates[candidate_count++];
    candidate->location_id = loc_id;
    candidate->item_id = item_id;
    candidate->region_id = region_id;
    candidate->selected = 0;
  }
  if (candidate_count > 1)
    qsort(s_hint_candidates, candidate_count, sizeof(s_hint_candidates[0]),
          hint_candidate_compare);

  uint64 sub_seed = 0;
  seed_hint_rng(placements, &sub_seed);
  RandoRng rng;
  Rng_SeedFromU64(&rng, sub_seed);
  uint8 primary_ids[kRandoHintPlanPrimaryTarget];
  uint8 primary_count = 0;
  memset(primary_ids, kRandoHintNoFact, sizeof(primary_ids));

  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_Priority, kRandoHintFact_PriorityItem,
      2,
      primary_ids, primary_count);
  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_Major, kRandoHintFact_MajorItem, 4,
      primary_ids, primary_count);
  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_HighFriction,
      kRandoHintFact_HighFrictionLocation, 3,
      primary_ids, primary_count);
  primary_count = hint_add_goal_and_settings(
      out, settings, primary_ids, primary_count);
  primary_count = hint_add_region_value(
      out, s_hint_candidates, candidate_count, 2,
      primary_ids, primary_count);
  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_Useful, kRandoHintFact_Useful, 3,
      primary_ids, primary_count);

  if (primary_count < kRandoHintPlanPrimaryTarget) {
    static const char *const flavor[] = {
      "Pots remember every insult",
      "Fortune favors exploration",
      "A good map starts with curiosity",
      "Even cuccos know more than they say",
    };
    uint16 flavor_id = (uint16)Rng_NextRange(
        &rng, (uint32)(sizeof(flavor) / sizeof(flavor[0])));
    const char *line = flavor[flavor_id];
    uint8 id = hint_plan_add_text(out, kRandoHintFact_Flavor,
                                  kRandoHintTemplate_Flavor, 0xFFFFu,
                                  flavor_id,
                                  line, line);
    if (id != kRandoHintNoFact) primary_ids[primary_count++] = id;
  }
  // Any unfilled category quota backfills from remaining progression first,
  // then from the general useful pool. This preserves the plan's value without
  // fabricating a missing category or duplicating a fact.
  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_Major, kRandoHintFact_MajorItem,
      (uint8)(kRandoHintPlanPrimaryTarget - primary_count),
      primary_ids, primary_count);
  primary_count = hint_take_exact_class(
      out, s_hint_candidates, candidate_count, &rng,
      kHintCandidateClass_Useful, kRandoHintFact_Useful,
      (uint8)(kRandoHintPlanPrimaryTarget - primary_count),
      primary_ids, primary_count);

  // Reserve three exact primary facts as the paid queue heads, then map the
  // remaining primaries to the 15 telepathic tiles in canonical selection order.
  bool assigned[kRandoHintPlanMaxFacts] = {false};
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    for (int i = (int)primary_count - 1; i >= 0; i--) {
      uint8 fact_id = primary_ids[i];
      if (fact_id == kRandoHintNoFact || assigned[fact_id] ||
          out->facts[fact_id].location_id == 0xFFFFu)
        continue;
      assigned[fact_id] = true;
      out->paid_queue[source][0] = fact_id;
      out->paid_count[source] = 1;
      out->primary_fact_by_npc[kRandoHintNpc_ForkStoryteller + source] =
          fact_id;
      break;
    }
  }
  uint8 tile = kRandoHintNpc_TeleEasternPalace;
  for (uint8 i = 0; i < primary_count &&
                      tile <= kRandoHintNpc_TeleSouthEastDarkworldCave; i++) {
    uint8 fact_id = primary_ids[i];
    if (fact_id == kRandoHintNoFact || assigned[fact_id]) continue;
    assigned[fact_id] = true;
    out->primary_fact_by_npc[tile++] = fact_id;
  }

  // Six unique exact reserves: reserve1 round-robin across the three services,
  // then reserve2. Underfilled/customizer pools stop safely without duplicates.
  uint16 reserve_pool_count = 0;
  for (uint16 i = 0; i < candidate_count; i++)
    if (!s_hint_candidates[i].selected)
      s_hint_candidate_pool[reserve_pool_count++] = i;
  if (reserve_pool_count > 1)
    shuffle_u16(&rng, s_hint_candidate_pool, reserve_pool_count);
  for (uint8 q = 1; q < kRandoHintPaidQueueCapacity; q++) {
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      bool filled = false;
      for (uint8 pass = 0; pass < 2 && !filled; pass++) {
        for (uint16 r = 0; r < reserve_pool_count; r++) {
          HintCandidate *candidate =
              &s_hint_candidates[s_hint_candidate_pool[r]];
          if (candidate->selected) continue;
          bool duplicate =
              hint_plan_has_exact_family(out, candidate->item_id);
          if ((pass == 0 && duplicate) || (pass == 1 && !duplicate)) continue;
          RandoHintFactKind kind =
              hint_priority_item(candidate->item_id)
                  ? kRandoHintFact_PriorityItem
                  : hint_major_item(candidate->item_id)
                        ? kRandoHintFact_MajorItem
                        : hint_high_friction_location(candidate->location_id)
                              ? kRandoHintFact_HighFrictionLocation
                              : kRandoHintFact_Useful;
          uint8 fact_id = hint_plan_add_exact(out, candidate, kind, false);
          if (fact_id == kRandoHintNoFact) continue;
          out->paid_queue[source][q] = fact_id;
          out->paid_count[source]++;
          filled = true;
          break;
        }
      }
    }
  }

  hint_build_murahdahla(out, settings, placements);
  hint_plan_compute_digest(out);
  return true;
}

static RandoHintFactKind hint_kind_for_candidate_class(
    HintCandidateClass cls) {
  switch (cls) {
  case kHintCandidateClass_Priority:
    return kRandoHintFact_PriorityItem;
  case kHintCandidateClass_Major:
  case kHintCandidateClass_OtherMajor:
    return kRandoHintFact_MajorItem;
  case kHintCandidateClass_HighFriction:
    return kRandoHintFact_HighFrictionLocation;
  case kHintCandidateClass_Useful:
  case kHintCandidateClass_OtherUseful:
  default:
    return kRandoHintFact_Useful;
  }
}

static uint8 hint_add_one_paid_exact(
    RandoHintPlan *plan, HintCandidate *candidates, uint16 candidate_count,
    RandoRng *rng, const HintCandidateClass *preferences,
    uint8 preference_count, bool primary) {
  for (uint8 preference = 0; preference < preference_count; preference++) {
    HintCandidateClass cls = preferences[preference];
    uint16 pool_count = 0;
    for (uint16 i = 0; i < candidate_count; i++) {
      if (!candidates[i].selected &&
          hint_candidate_matches(&candidates[i], cls))
        s_hint_candidate_pool[pool_count++] = i;
    }
    if (pool_count > 1)
      shuffle_u16(rng, s_hint_candidate_pool, pool_count);
    for (uint8 pass = 0; pass < 2; pass++) {
      for (uint16 i = 0; i < pool_count; i++) {
        HintCandidate *candidate =
            &candidates[s_hint_candidate_pool[i]];
        if (candidate->selected) continue;
        bool duplicate =
            hint_plan_has_exact_family(plan, candidate->item_id);
        if ((pass == 0 && duplicate) || (pass == 1 && !duplicate))
          continue;
        uint8 fact_id = hint_plan_add_exact(
            plan, candidate, hint_kind_for_candidate_class(cls), primary);
        if (fact_id != kRandoHintNoFact) return fact_id;
      }
    }
  }
  return kRandoHintNoFact;
}

// Non-Variety algorithm-2 mixes select an explicit 15-fact tile deck, then
// fill nine globally distinct exact paid facts in queue-position/service
// round-robin order. Variety deliberately uses the frozen maximal builder so
// its selected fact multiset, rendered text, and paid queues match 1/1.
static bool hint_build_custom_maximal_plan(
    const RandoSettings *settings, const RandoPlacementTable *placements,
    const RandoSpheres *spheres, RandoHintPlan *out) {
  if (settings == NULL || placements == NULL || out == NULL) return false;
  (void)spheres;

  uint8 canonical[kSettingsCanonicalLen];
  RandoSettings normalized;
  Settings_CanonicalSerialize(settings, canonical);
  if (Settings_CanonicalDeserialize(canonical, &normalized) != 0 ||
      normalized.hints != kHintsMode_Balanced ||
      normalized.hint_mix == kHintMix_Variety ||
      normalized.hint_mix > kHintMix_WorldInfo)
    return false;
  settings = &normalized;

  memset(out, 0, sizeof(*out));
  memset(out->primary_fact_by_npc, kRandoHintNoFact,
         sizeof(out->primary_fact_by_npc));
  memset(out->paid_queue, kRandoHintNoFact, sizeof(out->paid_queue));
  out->algorithm_version = kRandoHintPlanAlgorithmVersion;
  out->text_schema_version = kRandoHintTextSchemaVersion;
  out->mode = kHintsMode_Balanced;
  out->tile_target = kHintTileCount;
  out->mix = settings->hint_mix;
  out->paid_depth = kRandoHintPaidQueueCapacity;

  uint16 candidate_count = 0;
  uint16 placement_count = placements->count;
  if (placement_count > kRandoLocationCapacity)
    placement_count = kRandoLocationCapacity;
  for (uint16 i = 0; i < placement_count; i++) {
    uint16 loc_id = placements->entries[i].location_id;
    uint16 item_id = placements->entries[i].item_id;
    if (loc_is_medallion_config(loc_id) || item_is_junk(item_id)) continue;
    const RandoLocationDef *loc = hint_location_def(loc_id);
    uint16 region_id = hint_effective_region(loc, settings);
    if (loc == NULL || region_id == 0xFFFFu) continue;
    HintCandidate *candidate = &s_hint_candidates[candidate_count++];
    candidate->location_id = loc_id;
    candidate->item_id = item_id;
    candidate->region_id = region_id;
    candidate->selected = 0;
  }
  if (candidate_count > 1)
    qsort(s_hint_candidates, candidate_count, sizeof(s_hint_candidates[0]),
          hint_candidate_compare);

  uint64 sub_seed = 0;
  seed_hint_rng(placements, &sub_seed);
  static const uint64 kMixDomains[] = {
    0,
    0x494D504F5254414EULL,  // "IMPORTAN"
    0x444946464943554CULL,  // "DIFFICUL"
    0x574F524C44494E46ULL,  // "WORLDINF"
  };
  RandoRng rng;
  Rng_SeedFromU64(&rng, sub_seed ^ kMixDomains[settings->hint_mix]);

  uint8 tile_ids[kRandoHintPlanPrimaryTarget];
  uint8 tile_count = 0;
  memset(tile_ids, kRandoHintNoFact, sizeof(tile_ids));
  switch ((HintMix)settings->hint_mix) {
  case kHintMix_Important:
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_Priority, kRandoHintFact_PriorityItem, 2,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherMajor, kRandoHintFact_MajorItem, 8,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful, 5,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherMajor, kRandoHintFact_MajorItem,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    break;
  case kHintMix_Difficult:
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_HighFriction,
        kRandoHintFact_HighFrictionLocation, 9, tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_Priority, kRandoHintFact_PriorityItem, 2,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherMajor, kRandoHintFact_MajorItem, 2,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful, 2,
        tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_HighFriction,
        kRandoHintFact_HighFrictionLocation,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherMajor, kRandoHintFact_MajorItem,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    break;
  case kHintMix_WorldInfo:
    tile_count =
        hint_add_goal_and_settings(out, settings, tile_ids, tile_count);
    tile_count = hint_add_region_value(
        out, s_hint_candidates, candidate_count, 6, tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_HighFriction,
        kRandoHintFact_HighFrictionLocation, 3, tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful, 3,
        tile_ids, tile_count);
    tile_count = hint_add_additional_settings(
        out, settings, (uint8)(kHintTileCount - tile_count),
        tile_ids, tile_count);
    tile_count = hint_add_region_value(
        out, s_hint_candidates, candidate_count,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_HighFriction,
        kRandoHintFact_HighFrictionLocation,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherMajor, kRandoHintFact_MajorItem,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    tile_count = hint_take_exact_class(
        out, s_hint_candidates, candidate_count, &rng,
        kHintCandidateClass_OtherUseful, kRandoHintFact_Useful,
        (uint8)(kHintTileCount - tile_count), tile_ids, tile_count);
    break;
  default:
    return false;
  }

  for (uint8 i = 0; i < tile_count && i < kHintTileCount; i++)
    out->primary_fact_by_npc[kRandoHintNpc_TeleEasternPalace + i] =
        tile_ids[i];

  static const HintCandidateClass kImportantPaid[] = {
    kHintCandidateClass_Priority,
    kHintCandidateClass_OtherMajor,
    kHintCandidateClass_OtherUseful,
  };
  static const HintCandidateClass kDifficultPaid[] = {
    kHintCandidateClass_HighFriction,
    kHintCandidateClass_Priority,
    kHintCandidateClass_OtherMajor,
    kHintCandidateClass_OtherUseful,
  };
  const HintCandidateClass *paid_preferences =
      settings->hint_mix == kHintMix_Important
          ? kImportantPaid
          : kDifficultPaid;
  uint8 paid_preference_count =
      settings->hint_mix == kHintMix_Important
          ? (uint8)(sizeof(kImportantPaid) / sizeof(kImportantPaid[0]))
          : (uint8)(sizeof(kDifficultPaid) / sizeof(kDifficultPaid[0]));
  for (uint8 queue = 0; queue < kRandoHintPaidQueueCapacity; queue++) {
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      uint8 slot = out->paid_count[source];
      if (slot >= kRandoHintPaidQueueCapacity) continue;
      uint8 fact_id = hint_add_one_paid_exact(
          out, s_hint_candidates, candidate_count, &rng,
          paid_preferences, paid_preference_count, slot == 0);
      if (fact_id == kRandoHintNoFact) continue;
      out->paid_queue[source][slot] = fact_id;
      out->paid_count[source] = (uint8)(slot + 1);
      if (slot == 0)
        out->primary_fact_by_npc[kRandoHintNpc_ForkStoryteller + source] =
            fact_id;
    }
  }

  hint_build_murahdahla(out, settings, placements);
  hint_plan_compute_digest(out);
  return true;
}

typedef enum HintTileOrderToken {
  kHintTileOrder_Important = 0,
  kHintTileOrder_Difficult,
  kHintTileOrder_WorldInfo,
  kHintTileOrder_Useful,
  kHintTileOrder_Flavor,
} HintTileOrderToken;

// Algorithm-2 tile order is deliberately prefix-balanced. A lower coverage
// keeps a meaningful cross-section of the maximal deck instead of truncating
// the legacy category-block order after priority/major facts.
static const uint8 kHintTileBandSchedule[15] = {
  kHintTileOrder_Important,
  kHintTileOrder_Difficult,
  kHintTileOrder_WorldInfo,
  kHintTileOrder_Useful,
  kHintTileOrder_Flavor,
  kHintTileOrder_Important,
  kHintTileOrder_Difficult,
  kHintTileOrder_WorldInfo,
  kHintTileOrder_Useful,
  kHintTileOrder_Important,
  kHintTileOrder_Difficult,
  kHintTileOrder_WorldInfo,
  kHintTileOrder_Useful,
  kHintTileOrder_Important,
  kHintTileOrder_WorldInfo,
};

static bool hint_fact_matches_tile_token(const RandoHintFact *fact,
                                         uint8 token) {
  if (fact == NULL) return false;
  switch ((HintTileOrderToken)token) {
  case kHintTileOrder_Important:
    return fact->kind == kRandoHintFact_PriorityItem ||
           fact->kind == kRandoHintFact_MajorItem;
  case kHintTileOrder_Difficult:
    return fact->kind == kRandoHintFact_HighFrictionLocation;
  case kHintTileOrder_WorldInfo:
    return fact->kind == kRandoHintFact_Goal ||
           fact->kind == kRandoHintFact_ActiveSetting ||
           fact->kind == kRandoHintFact_RegionValue;
  case kHintTileOrder_Useful:
    return fact->kind == kRandoHintFact_Useful;
  case kHintTileOrder_Flavor:
    return fact->kind == kRandoHintFact_Flavor;
  default:
    return false;
  }
}

static uint8 hint_order_maximal_tile_facts(
    const RandoHintPlan *maximal,
    uint8 ordered[kHintTileCount]) {
  uint8 pool[kHintTileCount];
  uint8 pool_count = 0;
  bool used[kHintTileCount] = {false};
  memset(pool, kRandoHintNoFact, sizeof(pool));
  memset(ordered, kRandoHintNoFact, kHintTileCount);
  for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
       npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
    uint8 fact_id = maximal->primary_fact_by_npc[npc];
    if (fact_id < maximal->fact_count && pool_count < kHintTileCount)
      pool[pool_count++] = fact_id;
  }

  uint8 out_count = 0;
  // Visit the literal five-band schedule once. An empty/exhausted band is
  // skipped rather than immediately replaced, so Sparse takes one from each
  // populated band before a second fact from any band.
  for (uint8 slot = 0; slot < kHintTileCount; slot++) {
    int selected = -1;
    for (uint8 i = 0; i < pool_count; i++) {
      if (!used[i] &&
          hint_fact_matches_tile_token(&maximal->facts[pool[i]],
                                       kHintTileBandSchedule[slot])) {
        selected = i;
        break;
      }
    }
    if (selected < 0) continue;
    used[selected] = true;
    ordered[out_count++] = pool[selected];
  }
  // Quotas can contain more facts from a band than the schedule visits
  // (notably Important). Append those facts in their canonical maximal order.
  for (uint8 i = 0; i < pool_count; i++) {
    if (used[i]) continue;
    used[i] = true;
    ordered[out_count++] = pool[i];
  }
  return out_count;
}

static void hint_rank_tiles(const RandoSettings *settings,
                            const RandoPlacementTable *placements,
                            uint8 ranked[kHintTileCount]) {
  uint16 ids[kHintTileCount];
  for (uint8 i = 0; i < kHintTileCount; i++)
    ids[i] = (uint16)(kRandoHintNpc_TeleEasternPalace + i);

  // Coverage and paid depth are deliberately neutralized before ranking so
  // 5/10/15 and 1/2/3 remain strict prefixes. Race mode is orthogonal to hint
  // content and is normalized out of suppressed-spoiler stamps; neutralize it
  // here too so generation (race=1) and reveal regeneration (race=0) rebuild
  // the same physical tile assignments. NPC reward previews are likewise an
  // independent presentation axis and must not perturb clue delivery. Other
  // accepted settings, including mix, remain bound alongside the placement
  // identity.
  RandoSettings rank_settings = *settings;
  rank_settings.hints = kHintsMode_Balanced;
  rank_settings.hint_tile_coverage = kHintTileCoverage_Full;
  rank_settings.hint_paid_depth = kRandoHintPaidQueueCapacity;
  rank_settings.hint_npc_reward_reveal = 0;
  rank_settings.race_mode = 0;
  uint8 canonical[kSettingsCanonicalLen];
  uint8 placement_digest[32];
  uint8 rank_digest[32];
  Settings_CanonicalSerialize(&rank_settings, canonical);
  PlacementTable_ComputeDigest(placements, placement_digest);
  SHA256_CTX rank_ctx;
  static const uint8 rank_domain[] = "Z3R-HINT-TILE-RANK-V2";
  sha256_init(&rank_ctx);
  sha256_update(&rank_ctx, rank_domain, sizeof(rank_domain) - 1);
  sha256_update(&rank_ctx, placement_digest, sizeof(placement_digest));
  sha256_update(&rank_ctx, canonical, sizeof(canonical));
  sha256_final(&rank_ctx, rank_digest);
  uint64 coverage_seed = 0;
  for (uint8 i = 0; i < 8; i++)
    coverage_seed = (coverage_seed << 8) | rank_digest[i];
  RandoRng coverage_rng;
  Rng_SeedFromU64(&coverage_rng, coverage_seed);
  shuffle_u16(&coverage_rng, ids, kHintTileCount);
  for (uint8 i = 0; i < kHintTileCount; i++) ranked[i] = (uint8)ids[i];
}

static uint8 hint_copy_fact(RandoHintPlan *out,
                            const RandoHintPlan *maximal,
                            uint8 old_fact_id, bool primary) {
  if (old_fact_id >= maximal->fact_count ||
      out->fact_count >= kRandoHintPlanMaxFacts)
    return kRandoHintNoFact;
  uint8 new_fact_id = out->fact_count++;
  out->facts[new_fact_id] = maximal->facts[old_fact_id];
  out->facts[new_fact_id].fact_id = new_fact_id;
  out->facts[new_fact_id].primary = primary ? 1 : 0;
  return new_fact_id;
}

static bool hint_prune_plan_v2(const RandoSettings *settings,
                               const RandoPlacementTable *placements,
                               const RandoHintPlan *maximal,
                               RandoHintPlan *out) {
  if (settings == NULL || placements == NULL || maximal == NULL || out == NULL)
    return false;
  memset(out, 0, sizeof(*out));
  memset(out->primary_fact_by_npc, kRandoHintNoFact,
         sizeof(out->primary_fact_by_npc));
  memset(out->paid_queue, kRandoHintNoFact, sizeof(out->paid_queue));
  out->algorithm_version = kRandoHintPlanAlgorithmVersion;
  out->text_schema_version = kRandoHintTextSchemaVersion;
  out->mode = settings->hints;
  out->tile_target = Settings_HintTileCount(settings);
  out->mix = settings->hint_mix;
  out->paid_depth = Settings_EffectiveHintPaidDepth(settings);
  out->murahdahla_active = maximal->murahdahla_active;
  memcpy(out->murahdahla_text, maximal->murahdahla_text,
         sizeof(out->murahdahla_text));

  if (settings->hints == kHintsMode_Off) {
    out->tile_target = 0;
    out->mix = kHintMix_Variety;
    out->paid_depth = 0;
    hint_plan_compute_digest(out);
    return true;
  }

  uint8 ordered_facts[kHintTileCount];
  uint8 ranked_tiles[kHintTileCount];
  uint8 ordered_count =
      hint_order_maximal_tile_facts(maximal, ordered_facts);
  hint_rank_tiles(settings, placements, ranked_tiles);
  uint8 keep_tiles = out->tile_target;
  if (keep_tiles > ordered_count) keep_tiles = ordered_count;
  for (uint8 i = 0; i < keep_tiles; i++) {
    uint8 fact_id = hint_copy_fact(out, maximal, ordered_facts[i], true);
    if (fact_id != kRandoHintNoFact)
      out->primary_fact_by_npc[ranked_tiles[i]] = fact_id;
  }

  // Queue prefixes are copied after tiles so all retained primary facts remain
  // before reserve facts in the compact plan.
  if (out->paid_depth != 0) {
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      if (maximal->paid_count[source] == 0) continue;
      uint8 old_fact_id = maximal->paid_queue[source][0];
      uint8 fact_id = hint_copy_fact(out, maximal, old_fact_id, true);
      if (fact_id == kRandoHintNoFact) continue;
      out->paid_queue[source][0] = fact_id;
      out->paid_count[source] = 1;
      out->primary_fact_by_npc[kRandoHintNpc_ForkStoryteller + source] =
          fact_id;
    }
  }
  for (uint8 queue = 1; queue < out->paid_depth; queue++) {
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      // Keep each service a true prefix. If a custom placement pool could not
      // provide queue head 0 (or an earlier reserve), never expose a later
      // reserve as though the missing prefix existed.
      if (out->paid_count[source] != queue ||
          queue >= maximal->paid_count[source])
        continue;
      uint8 old_fact_id = maximal->paid_queue[source][queue];
      uint8 fact_id = hint_copy_fact(out, maximal, old_fact_id, false);
      if (fact_id == kRandoHintNoFact) continue;
      out->paid_queue[source][queue] = fact_id;
      out->paid_count[source]++;
    }
  }
  hint_plan_compute_digest(out);
  return true;
}

bool Rando_BuildHintPlan(const RandoSettings *settings,
                         const RandoPlacementTable *placements,
                         const RandoSpheres *spheres,
                         uint16 algorithm_version,
                         uint16 text_schema_version,
                         RandoHintPlan *out) {
  if (settings == NULL || placements == NULL || out == NULL ||
      !hint_version_pair_supported(algorithm_version, text_schema_version))
    return false;

  uint8 settings_canonical[kSettingsCanonicalLen];
  RandoSettings normalized;
  Settings_CanonicalSerialize(settings, settings_canonical);
  if (Settings_CanonicalDeserialize(settings_canonical, &normalized) != 0)
    return false;

  if (algorithm_version == kRandoHintPlanLegacyAlgorithmVersion) {
    // A 1/1 digest never represented configurable policy. Refuse rather than
    // blessing a custom setting under the legacy identity.
    if (normalized.hints != kHintsMode_Off &&
        (normalized.hint_tile_coverage != kHintTileCoverage_Full ||
         normalized.hint_mix != kHintMix_Variety ||
         Settings_EffectiveHintPaidDepth(&normalized) !=
             kRandoHintPaidQueueCapacity))
      return false;
    return hint_build_variety_compat_plan(
        &normalized, placements, spheres,
        kRandoHintPlanLegacyAlgorithmVersion,
        kRandoHintLegacyTextSchemaVersion, out);
  }

  static RandoHintPlan maximal;
  bool maximal_ok =
      normalized.hint_mix == kHintMix_Variety
          ? hint_build_variety_compat_plan(
                &normalized, placements, spheres,
                kRandoHintPlanAlgorithmVersion,
                kRandoHintTextSchemaVersion, &maximal)
          : hint_build_custom_maximal_plan(
                &normalized, placements, spheres, &maximal);
  if (!maximal_ok)
    return false;
  return hint_prune_plan_v2(&normalized, placements, &maximal, out);
}

static void hint_install_plan(const RandoHintPlan *plan,
                              const uint8 discovered[kRandoHintDiscoveryBytes],
                              RandoHintPlanStatus status) {
  if (++g_hint_plan_epoch == 0) g_hint_plan_epoch = 1;
  memset(&g_hint_plan, 0, sizeof(g_hint_plan));
  memset(g_hint_table, 0, sizeof(g_hint_table));
  memset(g_hint_discovered, 0, sizeof(g_hint_discovered));
  memset(g_hint_paid_pending, 0, sizeof(g_hint_paid_pending));
  memset(&g_hint_paid_prompt_context, 0,
         sizeof(g_hint_paid_prompt_context));
  if (plan != NULL) {
    g_hint_plan = *plan;
    if (discovered != NULL)
      memcpy(g_hint_discovered, discovered, sizeof(g_hint_discovered));
    for (uint8 bit = g_hint_plan.fact_count; bit < 32; bit++)
      g_hint_discovered[bit >> 3] &=
          (uint8)~(1u << (bit & 7));
    for (uint8 npc = 1; npc < kRandoHintNpc__Count; npc++) {
      uint8 fact_id = g_hint_plan.primary_fact_by_npc[npc];
      if (fact_id >= g_hint_plan.fact_count) continue;
      const RandoHintFact *fact = &g_hint_plan.facts[fact_id];
      HintEntry *entry = &g_hint_table[npc];
      entry->active = 1;
      entry->fact_id = fact_id;
      entry->placement_loc_id = fact->location_id;
      entry->placement_item_id = fact->item_id;
      snprintf(entry->text, sizeof(entry->text), "%s", fact->text);
      snprintf(entry->game_text, sizeof(entry->game_text), "%s",
               fact->game_text);
    }
    if (g_hint_plan.murahdahla_active) {
      HintEntry *entry = &g_hint_table[kRandoHintNpc_Murahdahla];
      entry->active = 1;
      entry->fact_id = kRandoHintNoFact;
      entry->placement_loc_id = 0xFFFFu;
      entry->placement_item_id = ITEM_TriforcePiece;
      snprintf(entry->text, sizeof(entry->text), "%s",
               g_hint_plan.murahdahla_text);
      snprintf(entry->game_text, sizeof(entry->game_text), "%s",
               g_hint_plan.murahdahla_text);
    }
  }
  g_hint_plan_status = status;
}

static void hint_capture_computed_plan(const RandoHintPlan *plan) {
  memset(&g_hint_last_computed_state, 0,
         sizeof(g_hint_last_computed_state));
  g_hint_last_computed_state_set =
      Rando_HintPlanExportPersistedState(plan, &g_hint_last_computed_state);
  g_hint_last_computed_fact_count =
      g_hint_last_computed_state_set ? plan->fact_count : 0;
}

bool Rando_GenerateHints(const RandoSettings *settings,
                         const RandoPlacementTable *placements,
                         const RandoSpheres *spheres) {
  static RandoHintPlan generated;
  if (!Rando_BuildHintPlan(settings, placements, spheres,
                           kRandoHintPlanAlgorithmVersion,
                           kRandoHintTextSchemaVersion, &generated)) {
    Rando_ClearHints();
    g_hint_plan_status = kRandoHintPlanStatus_BuildFailed;
    return false;
  }
  hint_install_plan(&generated, NULL,
                    generated.mode == kHintsMode_Off
                        ? kRandoHintPlanStatus_Off
                        : kRandoHintPlanStatus_Ready);
  hint_capture_computed_plan(&generated);
  g_hint_expected_state_set = false;
  memset(&g_hint_expected_state, 0, sizeof(g_hint_expected_state));
  return true;
}

void Rando_HintsImportPersistedState(const RandoHintPersistedState *state) {
  memset(&g_hint_expected_state, 0, sizeof(g_hint_expected_state));
  g_hint_expected_state_set =
      state != NULL && state->algorithm_version != 0 &&
      state->text_schema_version != 0;
  if (g_hint_expected_state_set) g_hint_expected_state = *state;
  memset(&g_hint_last_computed_state, 0,
         sizeof(g_hint_last_computed_state));
  g_hint_last_computed_state_set = false;
  g_hint_last_computed_fact_count = 0;
  memset(g_hint_paid_pending, 0, sizeof(g_hint_paid_pending));
  memset(&g_hint_paid_prompt_context, 0,
         sizeof(g_hint_paid_prompt_context));
}

bool Rando_RebuildHints(const RandoSettings *settings,
                        const RandoPlacementTable *placements,
                        const RandoSpheres *spheres) {
  static RandoHintPlan rebuilt;
  if (!g_hint_expected_state_set) {
    hint_install_plan(NULL, NULL, kRandoHintPlanStatus_MissingMetadata);
    return true;
  }
  if (g_hint_expected_state.algorithm_version !=
      kRandoHintPlanAlgorithmVersion) {
    hint_install_plan(NULL, NULL,
                      kRandoHintPlanStatus_UnsupportedAlgorithm);
    return true;
  }
  if (!hint_persisted_version_pair_supported(
          g_hint_expected_state.algorithm_version,
          g_hint_expected_state.text_schema_version)) {
    hint_install_plan(NULL, NULL,
                      kRandoHintPlanStatus_UnsupportedTextSchema);
    return true;
  }
  if (!Rando_BuildHintPlan(
          settings, placements, spheres,
          g_hint_expected_state.algorithm_version,
          g_hint_expected_state.text_schema_version, &rebuilt)) {
    hint_install_plan(NULL, NULL, kRandoHintPlanStatus_BuildFailed);
    return false;
  }
  hint_capture_computed_plan(&rebuilt);
  if (memcmp(rebuilt.digest, g_hint_expected_state.plan_digest,
             kRandoHintPlanDigestBytes) != 0) {
    hint_install_plan(NULL, NULL, kRandoHintPlanStatus_DigestMismatch);
    return true;
  }
  hint_install_plan(&rebuilt, g_hint_expected_state.discovered,
                    rebuilt.mode == kHintsMode_Off
                        ? kRandoHintPlanStatus_Off
                        : kRandoHintPlanStatus_Ready);
  return true;
}

static bool hint_plan_structure_valid(const RandoHintPlan *plan) {
  if (plan == NULL || plan->fact_count > kRandoHintPlanMaxFacts)
    return false;
  uint8 assignments[kRandoHintPlanMaxFacts] = {0};
  if (plan->primary_fact_by_npc[kRandoHintNpc_None] != kRandoHintNoFact ||
      plan->primary_fact_by_npc[kRandoHintNpc_Murahdahla] !=
          kRandoHintNoFact ||
      plan->primary_fact_by_npc[kRandoHintNpc_ForkFortuneTellerLakeHylia] !=
          kRandoHintNoFact)
    return false;
  for (uint8 fact_id = 0; fact_id < plan->fact_count; fact_id++) {
    if (plan->facts[fact_id].fact_id != fact_id)
      return false;
    for (uint8 earlier = 0; earlier < fact_id; earlier++) {
      const RandoHintFact *a = &plan->facts[earlier];
      const RandoHintFact *b = &plan->facts[fact_id];
      if ((a->location_id != 0xFFFFu &&
           a->location_id == b->location_id) ||
          (a->kind == b->kind &&
           a->template_id == b->template_id &&
           a->location_id == b->location_id &&
           a->item_id == b->item_id &&
           a->region_id == b->region_id &&
           a->template_parameter == b->template_parameter))
        return false;
    }
  }
  for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
       npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
    uint8 fact_id = plan->primary_fact_by_npc[npc];
    if (fact_id == kRandoHintNoFact) continue;
    if (fact_id >= plan->fact_count || !plan->facts[fact_id].primary)
      return false;
    assignments[fact_id]++;
  }
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    uint8 count = plan->paid_count[source];
    if (count > kRandoHintPaidQueueCapacity) return false;
    for (uint8 queue = 0; queue < kRandoHintPaidQueueCapacity; queue++) {
      uint8 fact_id = plan->paid_queue[source][queue];
      if (queue >= count) {
        if (fact_id != kRandoHintNoFact) return false;
        continue;
      }
      if (fact_id >= plan->fact_count ||
          plan->facts[fact_id].location_id == 0xFFFFu ||
          plan->facts[fact_id].primary != (queue == 0))
        return false;
      assignments[fact_id]++;
      if (queue == 0 &&
          plan->primary_fact_by_npc[
              kRandoHintNpc_ForkStoryteller + source] != fact_id)
        return false;
    }
    if (count == 0 &&
        plan->primary_fact_by_npc[
            kRandoHintNpc_ForkStoryteller + source] != kRandoHintNoFact)
      return false;
  }
  for (uint8 fact_id = 0; fact_id < plan->fact_count; fact_id++) {
    if (assignments[fact_id] != 1) return false;
  }
  return true;
}

bool Rando_HintPlanExportPersistedState(const RandoHintPlan *plan,
                                        RandoHintPersistedState *out) {
  static RandoHintPlan verified;
  if (plan == NULL || out == NULL ||
      !hint_persisted_version_pair_supported(plan->algorithm_version,
                                             plan->text_schema_version) ||
      plan->fact_count > kRandoHintPlanMaxFacts ||
      (plan->mode != kHintsMode_Off &&
       plan->mode != kHintsMode_Balanced) ||
      (plan->mode == kHintsMode_Off && plan->fact_count != 0) ||
      !hint_plan_structure_valid(plan))
    return false;
  if ((plan->tile_target != 0 && plan->tile_target != 5 &&
       plan->tile_target != 10 && plan->tile_target != kHintTileCount) ||
      plan->mix > kHintMix_WorldInfo ||
      plan->paid_depth > kRandoHintPaidQueueCapacity ||
      (plan->mode == kHintsMode_Off &&
       (plan->tile_target != 0 || plan->paid_depth != 0 ||
        plan->mix != kHintMix_Variety)) ||
      (plan->mode == kHintsMode_Balanced &&
       plan->tile_target == 0 && plan->paid_depth == 0))
    return false;
  verified = *plan;
  hint_plan_compute_digest(&verified);
  if (memcmp(verified.digest, plan->digest,
             kRandoHintPlanDigestBytes) != 0)
    return false;
  memset(out, 0, sizeof(*out));
  out->algorithm_version = plan->algorithm_version;
  out->text_schema_version = plan->text_schema_version;
  memcpy(out->plan_digest, plan->digest, sizeof(out->plan_digest));
  return true;
}

bool Rando_HintsExportPersistedState(RandoHintPersistedState *out) {
  if (out == NULL ||
      (g_hint_plan_status != kRandoHintPlanStatus_Ready &&
       g_hint_plan_status != kRandoHintPlanStatus_Off))
    return false;
  if (!Rando_HintPlanExportPersistedState(&g_hint_plan, out)) return false;
  memcpy(out->discovered, g_hint_discovered, sizeof(out->discovered));
  return true;
}

RandoHintPlanStatus Rando_GetHintPlanStatus(void) {
  return g_hint_plan_status;
}

const char *Rando_GetHintPlanStatusName(void) {
  switch (g_hint_plan_status) {
  case kRandoHintPlanStatus_None: return "none";
  case kRandoHintPlanStatus_Ready: return "ready";
  case kRandoHintPlanStatus_Off: return "off";
  case kRandoHintPlanStatus_MissingMetadata: return "missing metadata";
  case kRandoHintPlanStatus_UnsupportedAlgorithm: return "unsupported algorithm";
  case kRandoHintPlanStatus_UnsupportedTextSchema: return "unsupported text schema";
  case kRandoHintPlanStatus_DigestMismatch: return "digest mismatch";
  case kRandoHintPlanStatus_BuildFailed: return "build failed";
  default: return "unknown";
  }
}

uint8 Rando_GetHintFactCount(void) {
  return g_hint_plan.fact_count;
}

const RandoHintFact *Rando_GetHintFact(uint8 fact_id) {
  return fact_id < g_hint_plan.fact_count ? &g_hint_plan.facts[fact_id] : NULL;
}

bool Rando_GetHintFactAssignment(uint8 fact_id, RandoHintNpc *out_npc,
                                 uint8 *out_queue_index) {
  if (fact_id >= g_hint_plan.fact_count) return false;
  for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
       npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
    if (g_hint_plan.primary_fact_by_npc[npc] != fact_id) continue;
    if (out_npc != NULL) *out_npc = (RandoHintNpc)npc;
    if (out_queue_index != NULL) *out_queue_index = 0;
    return true;
  }
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    uint8 count = g_hint_plan.paid_count[source];
    if (count > kRandoHintPaidQueueCapacity)
      count = kRandoHintPaidQueueCapacity;
    for (uint8 queue = 0; queue < count; queue++) {
      if (g_hint_plan.paid_queue[source][queue] != fact_id) continue;
      if (out_npc != NULL)
        *out_npc =
            (RandoHintNpc)(kRandoHintNpc_ForkStoryteller + source);
      if (out_queue_index != NULL) *out_queue_index = queue;
      return true;
    }
  }
  return false;
}

bool Rando_IsHintFactDiscovered(uint8 fact_id) {
  return fact_id < g_hint_plan.fact_count &&
         (g_hint_discovered[fact_id >> 3] &
          (uint8)(1u << (fact_id & 7))) != 0;
}

bool Rando_IsHintFactResolved(uint8 fact_id) {
  const RandoHintFact *fact = Rando_GetHintFact(fact_id);
  return fact != NULL && fact->location_id != 0xFFFFu &&
         Rando_IsLocationChecked(fact->location_id);
}

uint8 Rando_GetHintDiscoveredCount(void) {
  uint8 count = 0;
  for (uint8 i = 0; i < g_hint_plan.fact_count; i++)
    if (Rando_IsHintFactDiscovered(i)) count++;
  return count;
}

const uint8 *Rando_GetHintPlanDigest(void) {
  return (g_hint_plan_status == kRandoHintPlanStatus_Ready ||
          g_hint_plan_status == kRandoHintPlanStatus_Off)
             ? g_hint_plan.digest
             : NULL;
}

uint16 Rando_GetHintPlanAlgorithmVersion(void) {
  return g_hint_plan.algorithm_version;
}

uint16 Rando_GetHintTextSchemaVersion(void) {
  return g_hint_plan.text_schema_version;
}

uint16 Rando_GetHintDialogueId(RandoHintNpc npc) {
  if (npc <= kRandoHintNpc_None || npc >= kRandoHintNpc__Count) return 0xFFFFu;
  if (!g_hint_table[npc].active) return 0xFFFFu;
  return (uint16)(kRandoHintDialogueBase + ((uint16)npc - 1));
}

const char *Rando_GetHintString(RandoHintNpc npc) {
  if (npc <= kRandoHintNpc_None || npc >= kRandoHintNpc__Count) return 0;
  if (!g_hint_table[npc].active) return 0;
  return g_hint_table[npc].text;
}

const char *Rando_GetHintNpcStringId(RandoHintNpc npc) {
  if (npc <= kRandoHintNpc_None || npc >= kRandoHintNpc__Count) return 0;
  return kRandoHintStringIds[npc - 1];
}

uint16 Rando_RemapTeleMsg(uint16 vanilla_id, uint16 room_or_area, bool is_overworld) {
  (void)room_or_area;
  (void)is_overworld;
  // Runtime interception operates directly on verified vanilla message IDs.
  // Keep this historical exported symbol as a no-op for source compatibility.
  return vanilla_id;
}

void Rando_ClearHints(void) {
  if (++g_hint_plan_epoch == 0) g_hint_plan_epoch = 1;
  memset(g_hint_table, 0, sizeof(g_hint_table));
  memset(&g_hint_plan, 0, sizeof(g_hint_plan));
  memset(g_hint_discovered, 0, sizeof(g_hint_discovered));
  memset(&g_hint_expected_state, 0, sizeof(g_hint_expected_state));
  memset(&g_hint_last_computed_state, 0,
         sizeof(g_hint_last_computed_state));
  memset(g_hint_paid_pending, 0, sizeof(g_hint_paid_pending));
  memset(&g_hint_paid_prompt_context, 0,
         sizeof(g_hint_paid_prompt_context));
  g_hint_expected_state_set = false;
  g_hint_last_computed_state_set = false;
  g_hint_last_computed_fact_count = 0;
  g_hint_plan_status = kRandoHintPlanStatus_None;
}

static void hint_mark_discovered(uint8 fact_id) {
  if (fact_id >= g_hint_plan.fact_count) return;
  g_hint_discovered[fact_id >> 3] |= (uint8)(1u << (fact_id & 7));
}

static bool hint_paid_source_valid(RandoHintPaidSource source) {
  return (uint8)source < kRandoHintPaidSourceCount;
}

static bool hint_find_paid_fact(RandoHintPaidSource source,
                                uint8 *out_fact_id,
                                uint8 *out_queue_index) {
  if (!hint_paid_source_valid(source) ||
      g_hint_plan_status != kRandoHintPlanStatus_Ready ||
      !hint_rich_locale_supported(g_zenv.dialogue_flags))
    return false;
  uint8 count = g_hint_plan.paid_count[source];
  if (count > kRandoHintPaidQueueCapacity)
    count = kRandoHintPaidQueueCapacity;
  for (uint8 i = 0; i < count; i++) {
    uint8 fact_id = g_hint_plan.paid_queue[source][i];
    const RandoHintFact *fact = Rando_GetHintFact(fact_id);
    if (fact == NULL || Rando_IsHintFactDiscovered(fact_id)) continue;
    if (fact->location_id != 0xFFFFu &&
        Rando_IsLocationChecked(fact->location_id))
      continue;
    if (out_fact_id != NULL) *out_fact_id = fact_id;
    if (out_queue_index != NULL) *out_queue_index = i;
    return true;
  }
  return false;
}

bool Rando_HintsPaidHasClue(RandoHintPaidSource source) {
  return hint_find_paid_fact(source, NULL, NULL);
}

void Rando_HintsSetPaidPromptContext(RandoHintPaidSource source,
                                     uint16 price) {
  if (!hint_paid_source_valid(source) || price == 0) {
    memset(&g_hint_paid_prompt_context, 0,
           sizeof(g_hint_paid_prompt_context));
    return;
  }
  g_hint_paid_prompt_context.active = 1;
  g_hint_paid_prompt_context.source = (uint8)source;
  g_hint_paid_prompt_context.price = price;
  g_hint_paid_prompt_context.plan_epoch = g_hint_plan_epoch;
}

bool Rando_HintsPreparePaid(RandoHintPaidSource source) {
  if (!hint_paid_source_valid(source)) return false;
  RandoHintPaidPending *pending = &g_hint_paid_pending[source];
  memset(pending, 0, sizeof(*pending));
  pending->exhausted = 1;
  pending->fact_id = kRandoHintNoFact;
  pending->plan_epoch = g_hint_plan_epoch;
  pending->dialogue_flags = g_zenv.dialogue_flags;

  const char *text = NULL;
  uint8 fact_id = kRandoHintNoFact;
  uint8 queue_index = 0;
  if (hint_find_paid_fact(source, &fact_id, &queue_index)) {
    const RandoHintFact *fact = Rando_GetHintFact(fact_id);
    if (fact != NULL) {
      pending->exhausted = 0;
      pending->fact_id = fact_id;
      pending->queue_index = queue_index;
      text = fact->game_text;
    }
  }

  if (text == NULL) text = hint_paid_neutral_text();
  bool complete = false;
  int w = encode_hint_text_locale(text, pending->encoded, &complete,
                                  pending->dialogue_flags);
  if (!complete || w < 0 || w >= 255) {
    memset(pending, 0, sizeof(*pending));
    return false;
  }
  pending->encoded[w++] = 0x7fu;
  pending->encoded_len = (uint8)w;
  pending->armed = 1;
  return true;
}

bool Rando_HintsCommitPaid(RandoHintPaidSource source) {
  if (!hint_paid_source_valid(source)) return false;
  RandoHintPaidPending *pending = &g_hint_paid_pending[source];
  if (!pending->armed) return false;
  if (pending->plan_epoch != g_hint_plan_epoch ||
      pending->encoded_len == 0) {
    memset(pending, 0, sizeof(*pending));
    return false;
  }
  if (!pending->exhausted) {
    if (pending->queue_index >=
            g_hint_plan.paid_count[source] ||
        pending->queue_index >= kRandoHintPaidQueueCapacity ||
        g_hint_plan.paid_queue[source][pending->queue_index] !=
            pending->fact_id ||
        Rando_GetHintFact(pending->fact_id) == NULL) {
      memset(pending, 0, sizeof(*pending));
      return false;
    }
    hint_mark_discovered(pending->fact_id);
  }
  // Keep the committed selection latched through message rendering and
  // healing. The service calls Cancel at the end of the interaction.
  return true;
}

void Rando_HintsCancelPaid(RandoHintPaidSource source) {
  if (!hint_paid_source_valid(source)) return;
  memset(&g_hint_paid_pending[source], 0,
         sizeof(g_hint_paid_pending[source]));
  if (g_hint_paid_prompt_context.active &&
      g_hint_paid_prompt_context.source == (uint8)source)
    memset(&g_hint_paid_prompt_context, 0,
           sizeof(g_hint_paid_prompt_context));
}

static bool hint_paid_copy_prepared(RandoHintPaidSource source,
                                    uint8 *out_buffer) {
  if (!hint_paid_source_valid(source) || out_buffer == NULL) return false;
  const RandoHintPaidPending *pending = &g_hint_paid_pending[source];
  if (!pending->armed || pending->plan_epoch != g_hint_plan_epoch ||
      pending->encoded_len == 0)
    return false;
  memcpy(out_buffer, pending->encoded, pending->encoded_len);
  return true;
}

// ---------------------------------------------------------------------------
// Telepathic-tile runtime dispatch.
//
// g_hint_table stores plain ASCII text (see HintEntry.text). The messaging
// engine consumes FONT CHARACTER CODES (the post-dictionary decode stream),
// so the dispatch path below encodes ASCII -> US font codes mirroring
// assets/text_compression.py::kTextAlphabet_US and emits the message-box line
// commands the renderer's Text_DecodeCmd understands.
// ---------------------------------------------------------------------------

// The 15 hint-bearing vanilla US telepathic-tile message ids, in ascending
// order. Index i maps to RandoHintNpc (kRandoHintNpc_TeleEasternPalace + i).
// NOTE: 0xB4 IS the Eastern Palace tile ("...the treasure hidden in this palace
// to defeat armored foes"), the FIRST ALTTPR tile — not generic filler. An
// earlier off-by-one (reading a 1-indexed dialogue dump as 0-indexed) dropped
// it and instead included 0xC7, which is actually the Chris Houlihan secret-room
// text and not a hint tile. 0xB4 is set as dialogue only by the tele table
// (Dungeon_GetTeleMsg), so intercepting it is safe; among the 15 tiles only
// Eastern Palace happens to use the value that doubles as the room default.
static const uint16 kHintTileMsgIds[kHintTileCount] = {
  0xB4, 0xB5, 0xB8, 0xB9, 0xBA, 0xBB, 0xBE, 0xBF,
  0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
};

bool Rando_IsHintTileMessage(uint16 msg_id) {
  for (int i = 0; i < kHintTileCount; i++) {
    if (kHintTileMsgIds[i] == msg_id) return true;
  }
  return false;
}

// ASCII -> US font character code. Mirrors kTextAlphabet_US (text_compression.py).
// Returns 0xFF for characters with no glyph in the US alphabet (caller maps
// those to a space). Note the US alphabet has NO ':' glyph.
static uint8 ascii_to_font(char ch) {
  if (ch >= 'A' && ch <= 'Z') return (uint8)(ch - 'A');            // 0..25
  if (ch >= 'a' && ch <= 'z') return (uint8)(26 + (ch - 'a'));     // 26..51
  if (ch >= '0' && ch <= '9') return (uint8)(52 + (ch - '0'));     // 52..61
  switch (ch) {
    case '!': return 62;
    case '?': return 63;
    case '-': return 64;
    case '.': return 65;
    case ',': return 66;
    case '>': return 68;
    case '\'': return 81;
    case ' ': return 89;  // 0x59
    default:  return 0xFF;
  }
}

// US message-engine control bytes (Text_DecodeCmd: code = 0x67 + cmd_index).
//   kTextCmd_1 = 13 -> 0x74 (jump to visual row 0 / top)
//   kTextCmd_2 = 14 -> 0x75 (jump to visual row 1 / middle)
//   kTextCmd_3 = 15 -> 0x76 (jump to visual row 2 / bottom)
//   kTextCmd_Waitkey = 23 -> 0x7e (pause for input between pages)
//   kTextCmd_EndMessage = 24 -> 0x7f (terminator; matches Text_LoadCharacterBuffer)
// NOTE: 0x74/0x75/0x76 map directly to visual rows 0/1/2 (the message engine's
// kVWF_RowPositions = {0,2,4}; see messaging.c). Row 0 is implicit (no command),
// so the encoder's 2nd row must emit 0x75 (row 1) and its 3rd row 0x76 (row 2).
// The previous values 0x74/0x75 sent the 2nd row to row 0 (overwriting the 1st)
// and the 3rd row to row 1 — the reported "overlapping/dropped words" glitch.
#define kHintFontCmdLine0   0x74u
#define kHintFontCmdLine1   0x75u
#define kHintFontCmdLine2   0x76u
#define kHintFontCmdChoose  0x68u
#define kHintFontCmdScroll  0x73u
#define kHintFontCmdWaitkey 0x7eu
#define kHintFontCmdEnd     0x7fu
#define kHintFontCmdEuScroll  0x80u
#define kHintFontCmdEuWaitkey 0x81u
#define kHintFontCmdEuLine0   0x82u
#define kHintFontCmdEuLine1   0x83u
#define kHintFontCmdEuLine2   0x84u
#define kHintFontCmdEuRest    0x87u
#define kHintFontCmdEuChoose  0x80u

static bool hint_uses_eu_commands(uint8 dialogue_flags) {
  return (dialogue_flags & 1u) != 0;
}

// dialogue_flags==0 is the original/US text contract. Bit 0 selects the EU
// command grammar and bit 1 selects the translated original-format alphabet;
// either nonzero mode must receive localized neutral text, never shared-English
// semantic facts.
static bool hint_rich_locale_supported(uint8 dialogue_flags) {
  return dialogue_flags == 0;
}

static uint8 hint_line_command(uint8 dialogue_flags, int row) {
  if (hint_uses_eu_commands(dialogue_flags))
    return row == 0 ? kHintFontCmdEuLine0
                    : row == 1 ? kHintFontCmdEuLine1
                               : kHintFontCmdEuLine2;
  return row == 0 ? kHintFontCmdLine0
                  : row == 1 ? kHintFontCmdLine1
                             : kHintFontCmdLine2;
}

// Render `text` (NUL-terminated ASCII) into `out` as US font codes, wrapping on
// word boundaries to fit the THREE-row dialogue box. The box holds rows 0/1/2;
// anything that would overflow row 2 is dropped rather than paged. The normal
// pre-decode hint path deliberately remains one box; the Stumpy choice surface
// uses a separate post-decode composer with one explicit page transition.
// Friendly generated item names and lossless location aliases are sized to fit.
// Returns bytes written (excluding the 0x7f terminator the caller appends).
static int encode_hint_text_locale(const char *text, uint8 *out,
                                   bool *out_complete,
                                   uint8 dialogue_flags) {
  int w = 0;            // write cursor into out
  int col = 0;          // glyph count on the current row
  int row = 0;          // 0..2 (the 3 box rows)
  const char *p = text;
  while (*p && w < 240) {
    // Measure the next word (run of non-space chars).
    const char *word_end = p;
    while (*word_end && *word_end != ' ') word_end++;
    int word_len = (int)(word_end - p);
    if (word_len == 0) { p++; continue; }  // skip the space separator itself

    // Wrap if the word won't fit on the current row (and the row isn't empty).
    if (col != 0 && col + 1 + word_len > kHintMaxCharsPerLine) {
      if (row >= 2) {
        if (out_complete != NULL) *out_complete = false;
        return w;  // box full — stop (no paging)
      }
      row++;
      col = 0;
      out[w++] = hint_line_command(dialogue_flags, row);
    } else if (col != 0) {
      out[w++] = ascii_to_font(' ');  // inter-word space
      col++;
    }

    // Emit the word's glyphs (hard-wrap if a single word exceeds the row).
    // Guard at 239 (not 240): one iteration can write TWO bytes — a row command
    // on a hard-wrap AND the glyph — so reserving a byte keeps the write cursor
    // <= 240, leaving room for the caller's terminator at out[w] within the soft
    // cap. (Real hints are short and never approach this; it only bit a
    // pathological >240-byte string, which could push w to 241.)
    for (const char *q = p; q < word_end && w < 239; q++) {
      if (col >= kHintMaxCharsPerLine) {
        if (row >= 2) {
          if (out_complete != NULL) *out_complete = false;
          return w;  // box full mid-word — stop
        }
        row++;
        col = 0;
        out[w++] = hint_line_command(dialogue_flags, row);
      }
      uint8 fc = ascii_to_font(*q);
      if (fc == 0xFF) fc = ascii_to_font(' ');  // glyphless char -> space
      out[w++] = fc;
      col++;
    }
    p = word_end;
  }
  if (out_complete != NULL) *out_complete = (*p == '\0');
  return w;
}

static int encode_hint_text(const char *text, uint8 *out,
                            bool *out_complete) {
  return encode_hint_text_locale(text, out, out_complete,
                                 g_zenv.dialogue_flags);
}

static bool encode_item_location_hint(
    const RandoDialogueHintResolution *resolution, uint8 *out_buffer,
    bool *out_used_id_fallback) {
  if (out_used_id_fallback != NULL) *out_used_id_fallback = false;
  char item[64];
  const char *raw_loc = Rando_GetLocationName(resolution->location_id);
  if (!npc_hint_item_name(resolution->item_id, item, sizeof(item)) ||
      raw_loc == NULL) return false;

  char loc[96], compact_loc[64], text[192];
  uint8 encoded[240];
  bool complete = false;
  bool full_loc_available =
      hint_friendly_loc(raw_loc, loc, (int)sizeof(loc));

  int w = 0;
  if (full_loc_available) {
    int n = snprintf(text, sizeof(text), "%s is in %s", item, loc);
    if (n >= 0 && (size_t)n < sizeof(text))
      w = encode_hint_text(text, encoded, &complete);
  }
  if (!complete && full_loc_available) {
    snprintf(text, sizeof(text), "%s at %s", item, loc);
    w = encode_hint_text(text, encoded, &complete);
  }
  if (!complete) {
    if (!hint_compact_loc(resolution->location_id, compact_loc,
                          sizeof(compact_loc)))
      return false;
    snprintf(text, sizeof(text), "%s at %s", item, compact_loc);
    w = encode_hint_text(text, encoded, &complete);
  }

  if (!complete) return false;
  memcpy(out_buffer, encoded, (size_t)w);
  out_buffer[w] = kHintFontCmdEnd;
  return true;
}

static bool encode_location_item_hint(
    const RandoDialogueHintResolution *resolution, uint8 *out_buffer) {
  char item[64], text[128];
  uint8 encoded[240];
  bool complete = false;
  int w = 0;
  if (!location_hint_item_name(resolution->item_id, item, sizeof(item)))
    return false;

  // Preserve the sign's Cape clue when the placed item name allows it. The
  // shorter forms keep the item intact rather than silently truncating it.
  snprintf(text, sizeof(text), "Cape prize is %s", item);
  w = encode_hint_text(text, encoded, &complete);
  if (!complete) {
    snprintf(text, sizeof(text), "Prize is %s", item);
    w = encode_hint_text(text, encoded, &complete);
  }
  if (!complete) {
    snprintf(text, sizeof(text), "%s", item);
    w = encode_hint_text(text, encoded, &complete);
  }
  if (!complete) return false;
  memcpy(out_buffer, encoded, (size_t)w);
  out_buffer[w] = kHintFontCmdEnd;
  return true;
}

static bool encode_dialogue_hint(const RandoDialogueHintResolution *resolution,
                                 uint8 *out_buffer,
                                 bool *out_used_id_fallback) {
  if (resolution == NULL || resolution->redirect == NULL ||
      resolution->status != kDialogueHintRedirect_Active)
    return false;
  if (resolution->redirect->kind == kDialogueHintRedirect_LocationItem) {
    if (out_used_id_fallback != NULL) *out_used_id_fallback = false;
    return encode_location_item_hint(resolution, out_buffer);
  }
  return encode_item_location_hint(resolution, out_buffer,
                                   out_used_id_fallback);
}

static int append_hint_ascii(uint8 *out, int pos, const char *text) {
  while (*text && pos < 239) {
    uint8 fc = ascii_to_font(*text++);
    out[pos++] = fc == 0xFF ? ascii_to_font(' ') : fc;
  }
  return pos;
}

// The low-two-row Choose handler reloads vanilla cursor messages 1/2 on every
// Up/Down press. Their fixed overlay draws the arrow after four spaces and
// clears only the first six spaces, so both option labels must begin at x=28.
static const char kHintChoiceSelectedPrefix[] = "    > ";
static const char kHintChoiceUnselectedPrefix[] = "       ";

static bool append_interactive_choice(uint8 *tmp, int hint_len,
                                      uint8 *out_buffer) {
  int w = hint_len;
  if (w >= 200) return false;  // ample room for the fixed choice page below.
  const bool eu = hint_uses_eu_commands(g_zenv.dialogue_flags);
  tmp[w++] = eu ? kHintFontCmdEuWaitkey : kHintFontCmdWaitkey;
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  // Roll each choice row in after its own Scroll. One Scroll plus a row-0 jump
  // leaves prior-page pixels behind and XOR-corrupts the new prompt.
  w = append_hint_ascii(tmp, w, "Can you help?");
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  w = append_hint_ascii(tmp, w, kHintChoiceSelectedPrefix);
  w = append_hint_ascii(tmp, w, "Yes");
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  w = append_hint_ascii(tmp, w, kHintChoiceUnselectedPrefix);
  w = append_hint_ascii(tmp, w, "No way");
  if (eu) {
    tmp[w++] = kHintFontCmdEuRest;
    tmp[w++] = kHintFontCmdEuChoose;
  } else {
    tmp[w++] = kHintFontCmdChoose;
  }
  tmp[w] = kHintFontCmdEnd;
  memcpy(out_buffer, tmp, (size_t)(w + 1));
  return true;
}

static bool encode_interactive_dialogue_hint(
    const RandoDialogueHintResolution *resolution, uint8 *out_buffer) {
  if (resolution == NULL || resolution->redirect == NULL ||
      resolution->redirect->kind != kDialogueHintRedirect_ItemLocationChoice ||
      resolution->status != kDialogueHintRedirect_Active)
    return false;

  // Encode into a local buffer and copy out only on FULL success. The header
  // contract is "returns false without modifying the buffer": out_buffer is
  // the live messaging text buffer holding the decoded vanilla prompt, and a
  // partial write followed by a false return would clobber it — losing the
  // Choose command, so the Yes/No branch would run on a stale selection.
  uint8 tmp[256];
  bool used_id_fallback = false;
  if (!encode_item_location_hint(resolution, tmp, &used_id_fallback))
    return false;
  int w = 0;
  while (w < 239 && tmp[w] != kHintFontCmdEnd) w++;
  return append_interactive_choice(tmp, w, out_buffer);
}

static bool encode_interactive_neutral(const char *text,
                                       uint8 *out_buffer) {
  uint8 tmp[256];
  bool complete = false;
  int w = encode_hint_text(text, tmp, &complete);
  if (!complete) return false;
  return append_interactive_choice(tmp, w, out_buffer);
}

// Fortune Teller reading-message id range (kFortuneTeller_Readings in
// sprite_main.c): 0xEA..0xF1 and 0xF6..0xFD. The 0xF2..0xF5 gap is the payment /
// solicit / decline flow, deliberately left to vanilla. These ids are
// FortuneTeller-exclusive (verified), so no room gate is needed.
static bool is_fortune_reading_msg(uint16 msg_id) {
  return (msg_id >= 0xEAu && msg_id <= 0xF1u) || (msg_id >= 0xF6u && msg_id <= 0xFDu);
}

// Map a dialogue message id to the hint NPC whose text should replace it: the 15
// telepathic tiles first, then the fork-extension NPCs. Returns
// kRandoHintNpc_None for non-hint ids (caller falls through to vanilla decode).
static RandoHintNpc hint_npc_for_msg(uint16 msg_id) {
  for (int i = 0; i < kHintTileCount; i++)
    if (kHintTileMsgIds[i] == msg_id)
      return (RandoHintNpc)(kRandoHintNpc_TeleEasternPalace + i);
  // Storyteller (Sprite_28_DarkWorldHintNPC): the paid-tip messages 0xFF/0x101/
  // 0x102/0x103 are storyteller-exclusive (verified — subtype2 cases 0/1/2/3 each
  // call Sprite_ShowMessageUnconditional with one of these ids after
  // DarkWorldHintNPC_HandlePayment succeeds), so no location gate is needed.
  if (msg_id == 0xFFu || msg_id == 0x101u || msg_id == 0x102u || msg_id == 0x103u)
    return kRandoHintNpc_ForkStoryteller;
  // Fortune Teller reading -> Kakariko (light world) or Dark-World instance by
  // the current world. The Lake-Hylia FT shares the Kakariko room with no
  // runtime discriminator, so it also surfaces the Kakariko hint (id 18).
  // Sprite_FortuneTeller selects its light/dark instance via
  // (savegame_is_darkworld >> 6) & 1, not the raw byte — match it so the hint
  // picks the same world the sprite renders for.
  if (is_fortune_reading_msg(msg_id))
    return ((savegame_is_darkworld >> 6) & 1) ? kRandoHintNpc_ForkFortuneTellerDark
                                              : kRandoHintNpc_ForkFortuneTellerKak;
  return kRandoHintNpc_None;
}

typedef enum HintNeutralKind {
  kHintNeutral_Unfilled = 0,
  kHintNeutral_Exhausted,
  kHintNeutral_Disabled,
  kHintNeutral_Unavailable,
} HintNeutralKind;

static bool hint_language_is(const char *prefix) {
  const char *language = g_config.language;
  if (language == NULL || prefix == NULL) return false;
  while (*prefix != '\0') {
    char a = *language++, b = *prefix++;
    if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
    if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
    if (a != b) return false;
  }
  return true;
}

static const char *hint_neutral_text(HintNeutralKind kind) {
  if (hint_language_is("de")) {
    switch (kind) {
    case kHintNeutral_Unfilled: return "Kein nuetzlicher Hinweis";
    case kHintNeutral_Exhausted: return "Keine weiteren Hinweise";
    case kHintNeutral_Disabled: return "Hinweise sind aus";
    default: return "Hinweise nicht verfuegbar";
    }
  }
  if (hint_language_is("fr")) {
    switch (kind) {
    case kHintNeutral_Unfilled: return "Aucun indice utile";
    case kHintNeutral_Exhausted: return "Plus aucun indice";
    case kHintNeutral_Disabled: return "Indices desactives";
    default: return "Indices indisponibles";
    }
  }
  switch (kind) {
  case kHintNeutral_Unfilled: return "No useful hint here";
  case kHintNeutral_Exhausted: return "No more clues";
  case kHintNeutral_Disabled: return "Hints disabled";
  default: return "Hints unavailable";
  }
}

static bool encode_paid_no_clue_prompt(uint16 price, uint8 *out_buffer) {
  if (price == 0 || out_buffer == NULL) return false;
  const char *notice = "No clue. Healing only";
  const char *currency = "rupees";
  const char *yes = "Yes";
  const char *no = "No thanks";
  if (hint_language_is("de")) {
    notice = "Kein Hinweis Nur Heilung";
    currency = "Rubine";
    yes = "Ja";
    no = "Nein";
  } else if (hint_language_is("fr")) {
    notice = "Aucun indice Soins seuls";
    currency = "rubis";
    yes = "Oui";
    no = "Non";
  }

  uint8 tmp[256];
  bool complete = false;
  int w = encode_hint_text_locale(notice, tmp, &complete,
                                  g_zenv.dialogue_flags);
  if (!complete || w < 0 || w >= 190) return false;

  const bool eu = hint_uses_eu_commands(g_zenv.dialogue_flags);
  tmp[w++] = eu ? kHintFontCmdEuWaitkey : kHintFontCmdWaitkey;
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  char price_line[32];
  int n = snprintf(price_line, sizeof(price_line), "%u %s?",
                   (unsigned)price, currency);
  if (n < 0 || (size_t)n >= sizeof(price_line) ||
      n > kHintMaxCharsPerLine)
    return false;
  w = append_hint_ascii(tmp, w, price_line);
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  w = append_hint_ascii(tmp, w, kHintChoiceSelectedPrefix);
  w = append_hint_ascii(tmp, w, yes);
  tmp[w++] = eu ? kHintFontCmdEuScroll : kHintFontCmdScroll;
  w = append_hint_ascii(tmp, w, kHintChoiceUnselectedPrefix);
  w = append_hint_ascii(tmp, w, no);
  if (eu) {
    tmp[w++] = kHintFontCmdEuRest;
    tmp[w++] = kHintFontCmdEuChoose;
  } else {
    tmp[w++] = kHintFontCmdChoose;
  }
  tmp[w++] = kHintFontCmdEnd;
  memcpy(out_buffer, tmp, (size_t)w);
  return true;
}

static bool hint_render_complete(const char *text, uint8 *out_buffer) {
  uint8 encoded[256];
  bool complete = false;
  int w = encode_hint_text_locale(text, encoded, &complete,
                                  g_zenv.dialogue_flags);
  if (!complete || w < 0 || w >= (int)sizeof(encoded)) return false;
  encoded[w++] = kHintFontCmdEnd;
  memcpy(out_buffer, encoded, (size_t)w);
  return true;
}

static bool hint_paid_source_for_npc(RandoHintNpc npc,
                                     RandoHintPaidSource *out) {
  switch (npc) {
  case kRandoHintNpc_ForkStoryteller:
    *out = kRandoHintPaidSource_Storyteller;
    return true;
  case kRandoHintNpc_ForkFortuneTellerKak:
  case kRandoHintNpc_ForkFortuneTellerLakeHylia:
    *out = kRandoHintPaidSource_FortuneLight;
    return true;
  case kRandoHintNpc_ForkFortuneTellerDark:
    *out = kRandoHintPaidSource_FortuneDark;
    return true;
  default:
    return false;
  }
}

static const char *hint_unavailable_text(void) {
  return hint_neutral_text(
      g_hint_plan_status == kRandoHintPlanStatus_Off
          ? kHintNeutral_Disabled
          : kHintNeutral_Unavailable);
}

static bool hint_render_rich_text_or_unavailable(
    const char *text, uint8 *out_buffer, bool *out_rendered_rich) {
  if (out_rendered_rich != NULL) *out_rendered_rich = false;
  if (hint_render_complete(text, out_buffer)) {
    if (out_rendered_rich != NULL) *out_rendered_rich = true;
    return true;
  }
  return hint_render_complete(hint_neutral_text(kHintNeutral_Unavailable),
                              out_buffer);
}

static bool hint_render_dialogue_or_unavailable(
    const RandoDialogueHintResolution *resolution, uint8 *out_buffer) {
  if (encode_dialogue_hint(resolution, out_buffer, NULL)) return true;
  return hint_render_complete(hint_neutral_text(kHintNeutral_Unavailable),
                              out_buffer);
}

static bool hint_render_interactive_or_unavailable(
    const RandoDialogueHintResolution *resolution, uint8 *out_buffer) {
  if (encode_interactive_dialogue_hint(resolution, out_buffer)) return true;
  return encode_interactive_neutral(
      hint_neutral_text(kHintNeutral_Unavailable), out_buffer);
}

static const char *hint_paid_neutral_text(void) {
  return g_hint_plan_status == kRandoHintPlanStatus_Ready &&
                 hint_rich_locale_supported(g_zenv.dialogue_flags)
             ? hint_neutral_text(kHintNeutral_Exhausted)
             : hint_unavailable_text();
}

bool Rando_RenderHintMessage(uint16 msg_id, uint8 *out_buffer) {
  if (out_buffer == NULL) return false;
  // Slot-active gate: g_rando_slot_active is a g_ram macro (features.h).
  if (!g_rando_slot_active) return false;

  RandoHintNpc npc = hint_npc_for_msg(msg_id);
  if (npc != kRandoHintNpc_None) {
    RandoHintPaidSource source;
    if (hint_paid_source_for_npc(npc, &source)) {
      if (hint_paid_copy_prepared(source, out_buffer)) return true;
      // A paid carrier without its epoch-bound presentation (for example after
      // replay/reload) cannot synthesize an old or new clue. Commit will fail
      // and refund; the carrier fails closed for this transient redraw.
      return hint_render_complete(hint_unavailable_text(), out_buffer);
    }

    if (g_hint_plan_status != kRandoHintPlanStatus_Ready)
      return hint_render_complete(hint_unavailable_text(), out_buffer);
    if (!hint_rich_locale_supported(g_zenv.dialogue_flags))
      return hint_render_complete(
          hint_neutral_text(kHintNeutral_Unavailable), out_buffer);
    const HintEntry *entry = &g_hint_table[npc];
    if (!entry->active || entry->fact_id >= g_hint_plan.fact_count)
      return hint_render_complete(
          hint_neutral_text(kHintNeutral_Unfilled), out_buffer);
    bool rendered_rich = false;
    bool rendered =
        hint_render_rich_text_or_unavailable(entry->game_text, out_buffer,
                                             &rendered_rich);
    if (rendered_rich) hint_mark_discovered(entry->fact_id);
    return rendered;
  }

  RandoDialogueHintResolution redirect = resolve_active_dialogue_hint(msg_id);
  if (redirect.redirect == NULL ||
      redirect.status == kDialogueHintRedirect_SlotInactive ||
      redirect.status == kDialogueHintRedirect_DiscriminatorMismatch ||
      redirect.redirect->kind == kDialogueHintRedirect_ItemLocationChoice)
    return false;
  if (redirect.status == kDialogueHintRedirect_Active &&
      g_hint_plan_status == kRandoHintPlanStatus_Ready)
    return hint_render_dialogue_or_unavailable(&redirect, out_buffer);
  return hint_render_complete(
      redirect.status == kDialogueHintRedirect_HintsOff
          ? hint_neutral_text(kHintNeutral_Disabled)
          : hint_neutral_text(kHintNeutral_Unavailable),
      out_buffer);
}

bool Rando_RewriteInteractiveHintMessage(uint16 msg_id, uint8 *out_buffer) {
  if (out_buffer == NULL) return false;
  if (g_rando_slot_active && g_hint_paid_prompt_context.active &&
      g_hint_paid_prompt_context.plan_epoch == g_hint_plan_epoch &&
      g_hint_plan.algorithm_version == kRandoHintPlanAlgorithmVersion &&
      g_hint_plan.text_schema_version == kRandoHintTextSchemaVersion) {
    RandoHintPaidSource source =
        (RandoHintPaidSource)g_hint_paid_prompt_context.source;
    bool prompt_matches =
        (source == kRandoHintPaidSource_Storyteller && msg_id == 0xFEu) ||
        ((source == kRandoHintPaidSource_FortuneLight ||
          source == kRandoHintPaidSource_FortuneDark) &&
         msg_id == 0xF3u);
    if (prompt_matches && !Rando_HintsPaidHasClue(source))
      return encode_paid_no_clue_prompt(g_hint_paid_prompt_context.price,
                                        out_buffer);
  }

  RandoDialogueHintResolution redirect = resolve_active_dialogue_hint(msg_id);
  if (redirect.redirect == NULL ||
      redirect.redirect->kind != kDialogueHintRedirect_ItemLocationChoice ||
      redirect.status == kDialogueHintRedirect_SlotInactive ||
      redirect.status == kDialogueHintRedirect_DiscriminatorMismatch)
    return false;
  if (redirect.status == kDialogueHintRedirect_Active &&
      g_hint_plan_status == kRandoHintPlanStatus_Ready)
    return hint_render_interactive_or_unavailable(&redirect, out_buffer);
  return encode_interactive_neutral(
      redirect.status == kDialogueHintRedirect_HintsOff
          ? hint_neutral_text(kHintNeutral_Disabled)
          : hint_neutral_text(kHintNeutral_Unavailable),
      out_buffer);
}

bool Rando_IsDynamicHintMessage(uint16 msg_id) {
  RandoDialogueHintResolution redirect = resolve_active_dialogue_hint(msg_id);
  return redirect.redirect != NULL &&
         redirect.status != kDialogueHintRedirect_SlotInactive &&
         redirect.status != kDialogueHintRedirect_DiscriminatorMismatch;
}

static const char *dialogue_hint_redirect_status_name(
    RandoDialogueHintRedirectStatus status) {
  switch (status) {
  case kDialogueHintRedirect_NotRecognized: return "not recognized";
  case kDialogueHintRedirect_SlotInactive: return "slot inactive";
  case kDialogueHintRedirect_UnsupportedLocale: return "unsupported locale";
  case kDialogueHintRedirect_SettingsUnavailable: return "settings unavailable";
  case kDialogueHintRedirect_HintsOff: return "hints off";
  case kDialogueHintRedirect_DiscriminatorMismatch: return "discriminator mismatch";
  case kDialogueHintRedirect_PlacementUnavailable: return "placement unavailable";
  case kDialogueHintRedirect_ItemAbsent: return "item absent";
  case kDialogueHintRedirect_LocationAbsent: return "location absent";
  case kDialogueHintRedirect_Active: return "active";
  default: return "unknown";
  }
}

static const char *dialogue_hint_redirect_kind_name(
    const RandoDialogueHintRedirect *redirect) {
  if (redirect == NULL) return "none";
  switch (redirect->kind) {
  case kDialogueHintRedirect_ItemLocation: return "item-location";
  case kDialogueHintRedirect_LocationItem: return "location-item sign";
  case kDialogueHintRedirect_ItemLocationChoice: return "interactive item-location";
  default: return "unknown";
  }
}

static void dump_dialogue_hint_resolution(
    FILE *f, const RandoDialogueHintResolution *redirect) {
  if (f == NULL || redirect == NULL || redirect->redirect == NULL) return;
  const char *item = redirect->item_id == 0xFFFFu
                         ? NULL : Rando_GetItemName(redirect->item_id);
  const char *loc = redirect->status == kDialogueHintRedirect_Active
                        ? Rando_GetLocationName(redirect->location_id)
                        : NULL;
  if (loc != NULL) {
    fprintf(f, "redirect source=%s  surface=%s  target_item=%s  status=%s  "
               "resolved_location=%s  location_id=%u\n",
            redirect->redirect->source_name,
            dialogue_hint_redirect_kind_name(redirect->redirect),
            item != NULL ? item : "(unresolved)",
            dialogue_hint_redirect_status_name(redirect->status), loc,
            (unsigned)redirect->location_id);
  } else {
    fprintf(f, "redirect source=%s  surface=%s  target_item=%s  status=%s  "
               "resolved_location=(none)  location_id=(none)\n",
            redirect->redirect->source_name,
            dialogue_hint_redirect_kind_name(redirect->redirect),
            item != NULL ? item : "(unresolved)",
            dialogue_hint_redirect_status_name(redirect->status));
  }
}

static void dump_hint_digest(FILE *f, const uint8 *digest) {
  if (digest == NULL) {
    fprintf(f, "(unavailable)");
    return;
  }
  for (uint8 i = 0; i < kRandoHintPlanDigestBytes; i++)
    fprintf(f, "%02x", (unsigned)digest[i]);
}

// Dev diagnostic (F12 / ZeldaDumpDebugState): write the live hint-table state to
// dump_hints.txt so a telepathic-tile "no hint" report can be diagnosed without
// a debugger. `cur_msg_id` is the current dialogue_message_index — pass the id
// of the tile just read to see whether it is a hint tile and which NPC/text it
// maps to.
void Rando_DumpHintDebug(uint16 cur_msg_id) {
  FILE *f = fopen("dump_hints.txt", "w");
  if (f == NULL) return;
  int active = 0;
  for (int i = 1; i < kRandoHintNpc__Count; i++)
    if (g_hint_table[i].active) active++;
  RandoHintNpc generated_npc = hint_npc_for_msg(cur_msg_id);
  bool generated_active = g_rando_slot_active &&
                          generated_npc != kRandoHintNpc_None;
  RandoDialogueHintResolution redirect = resolve_active_dialogue_hint(cur_msg_id);
  const char *kind = generated_npc != kRandoHintNpc_None ? "generated hint" :
                     redirect.redirect != NULL ? "vanilla-dialogue redirect" : "none";
  fprintf(f, "spoiler_diagnostic=1  redaction=none  race_mode=%d\n",
          (int)Rando_ActiveSlotHidesSpoiler());
  fprintf(f, "slot_active=%d  active_hints=%d/%d  cur_dialogue_msg=0x%02X  "
             "is_hint_tile=%d  hint_kind=%s  generated_active=%d\n",
          (int)g_rando_slot_active, active, (int)kRandoHintNpc__Count - 1,
          (unsigned)cur_msg_id, (int)Rando_IsHintTileMessage(cur_msg_id), kind,
          (int)generated_active);
  fprintf(f,
          "plan_status=%s  algorithm=%u  text_schema=%u  facts=%u  "
          "discovered=%u  digest=",
          Rando_GetHintPlanStatusName(),
          (unsigned)g_hint_plan.algorithm_version,
          (unsigned)g_hint_plan.text_schema_version,
          (unsigned)g_hint_plan.fact_count,
          (unsigned)Rando_GetHintDiscoveredCount());
  dump_hint_digest(f, Rando_GetHintPlanDigest());
  fprintf(f, "\n");
  fprintf(f, "current_discovery=%02x%02x%02x  expected_discovery=",
          (unsigned)g_hint_discovered[0],
          (unsigned)g_hint_discovered[1],
          (unsigned)g_hint_discovered[2]);
  if (g_hint_expected_state_set) {
    fprintf(f, "%02x%02x%02x",
            (unsigned)g_hint_expected_state.discovered[0],
            (unsigned)g_hint_expected_state.discovered[1],
            (unsigned)g_hint_expected_state.discovered[2]);
  } else {
    fprintf(f, "(unavailable)");
  }
  fprintf(f, "\n");
  fprintf(f,
          "expected_identity=%s  algorithm=%u  text_schema=%u  digest=",
          g_hint_expected_state_set ? "present" : "missing",
          (unsigned)(g_hint_expected_state_set
                         ? g_hint_expected_state.algorithm_version
                         : 0),
          (unsigned)(g_hint_expected_state_set
                         ? g_hint_expected_state.text_schema_version
                         : 0));
  dump_hint_digest(f, g_hint_expected_state_set
                          ? g_hint_expected_state.plan_digest
                          : NULL);
  fprintf(f, "\n");
  fprintf(f,
          "computed_identity=%s  algorithm=%u  text_schema=%u  facts=%u  "
          "digest=",
          g_hint_last_computed_state_set ? "present" : "unavailable",
          (unsigned)(g_hint_last_computed_state_set
                         ? g_hint_last_computed_state.algorithm_version
                         : 0),
          (unsigned)(g_hint_last_computed_state_set
                         ? g_hint_last_computed_state.text_schema_version
                         : 0),
          (unsigned)g_hint_last_computed_fact_count);
  dump_hint_digest(f, g_hint_last_computed_state_set
                          ? g_hint_last_computed_state.plan_digest
                          : NULL);
  fprintf(f, "\n");
  dump_dialogue_hint_resolution(f, &redirect);
  for (int i = 1; i < kRandoHintNpc__Count; i++) {
    fprintf(f, "  npc %2d id=%s active=%d fact=%u text=%s\n", i,
            Rando_GetHintNpcStringId((RandoHintNpc)i),
            (int)g_hint_table[i].active,
            (unsigned)g_hint_table[i].fact_id,
            g_hint_table[i].text[0] ? g_hint_table[i].text : "(empty)");
  }
  for (uint8 fact_id = 0; fact_id < g_hint_plan.fact_count; fact_id++) {
    const RandoHintFact *fact = &g_hint_plan.facts[fact_id];
    RandoHintNpc npc = kRandoHintNpc_None;
    uint8 queue = 0;
    bool assigned =
        Rando_GetHintFactAssignment(fact_id, &npc, &queue);
    fprintf(f,
            "  fact %2u kind=%u template=%u parameter=%u primary=%u source=%s "
            "queue=%u discovered=%u resolved=%u loc=%u item=%u region=%u\n"
            "    text=%s\n"
            "    game_text=%s\n",
            (unsigned)fact_id, (unsigned)fact->kind,
            (unsigned)fact->template_id,
            (unsigned)fact->template_parameter,
            (unsigned)fact->primary,
            assigned ? Rando_GetHintNpcStringId(npc) : "(unassigned)",
            (unsigned)queue,
            (unsigned)Rando_IsHintFactDiscovered(fact_id),
            (unsigned)Rando_IsHintFactResolved(fact_id),
            (unsigned)fact->location_id, (unsigned)fact->item_id,
            (unsigned)fact->region_id, fact->text, fact->game_text);
  }
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    const RandoHintPaidPending *pending = &g_hint_paid_pending[source];
    fprintf(f,
            "  paid source=%u count=%u queue=[%u,%u,%u] "
            "pending={armed:%u exhausted:%u fact:%u index:%u epoch:%u}\n",
            (unsigned)source, (unsigned)g_hint_plan.paid_count[source],
            (unsigned)g_hint_plan.paid_queue[source][0],
            (unsigned)g_hint_plan.paid_queue[source][1],
            (unsigned)g_hint_plan.paid_queue[source][2],
            (unsigned)pending->armed, (unsigned)pending->exhausted,
            (unsigned)pending->fact_id, (unsigned)pending->queue_index,
            (unsigned)pending->plan_epoch);
  }
  if (g_hint_plan.murahdahla_active)
    fprintf(f, "  murahdahla=%s\n", g_hint_plan.murahdahla_text);
  fclose(f);
}

// -----------------------------------------------------------------------------
// Hints_SelfCheck — deterministic plan, exact rendering, discovery,
// transactional paid delivery, persistence identity, locale-neutral fallback,
// and vanilla-dialogue redirect regression coverage.
// -----------------------------------------------------------------------------
static void hints_selfcheck_die(const char *what) {
  fprintf(stderr, "Hints_SelfCheck: %s\n", what);
  abort();
}

static char hint_font_to_ascii(uint8 c) {
  if (c < 26) return (char)('A' + c);
  if (c < 52) return (char)('a' + c - 26);
  if (c < 62) return (char)('0' + c - 52);
  if (c == 64) return '-';
  if (c == 81) return '\'';
  return ' ';
}

static bool encoded_hint_contains(const uint8 *encoded, const char *needle) {
  char ascii[256];
  int w = 0;
  for (int i = 0; i < 240 && encoded[i] != kHintFontCmdEnd && w < (int)sizeof(ascii) - 1; i++) {
    uint8 c = encoded[i];
    if (c == kHintFontCmdLine1 || c == kHintFontCmdLine2) {
      if (w != 0 && ascii[w - 1] != ' ') ascii[w++] = ' ';
    } else {
      ascii[w++] = hint_font_to_ascii(c);
    }
  }
  ascii[w] = '\0';
  return strstr(ascii, needle) != NULL;
}

static bool encoded_hint_has_ascii_run(const uint8 *encoded,
                                       const char *needle) {
  uint8 font_needle[64];
  size_t needle_len = strlen(needle);
  if (needle_len == 0 || needle_len > sizeof(font_needle)) return false;
  for (size_t i = 0; i < needle_len; i++) {
    font_needle[i] = ascii_to_font(needle[i]);
    if (font_needle[i] == 0xFF) return false;
  }

  int encoded_len = 0;
  while (encoded_len < 240 && encoded[encoded_len] != kHintFontCmdEnd)
    encoded_len++;
  for (int i = 0; i + (int)needle_len <= encoded_len; i++)
    if (memcmp(encoded + i, font_needle, needle_len) == 0)
      return true;
  return false;
}

static bool encoded_hint_has_cursor_safe_choice(
    const uint8 *encoded, const char *yes, const char *no) {
  char selected[64], unselected[64];
  int selected_len = snprintf(selected, sizeof(selected), "%s%s",
                              kHintChoiceSelectedPrefix, yes);
  int unselected_len = snprintf(unselected, sizeof(unselected), "%s%s",
                                kHintChoiceUnselectedPrefix, no);
  return selected_len > 0 && (size_t)selected_len < sizeof(selected) &&
         unselected_len > 0 && (size_t)unselected_len < sizeof(unselected) &&
         encoded_hint_has_ascii_run(encoded, selected) &&
         encoded_hint_has_ascii_run(encoded, unselected);
}

static bool hint_fact_compat_equal(const RandoHintFact *a,
                                   const RandoHintFact *b) {
  return a != NULL && b != NULL &&
         a->kind == b->kind &&
         a->template_id == b->template_id &&
         a->primary == b->primary &&
         a->location_id == b->location_id &&
         a->item_id == b->item_id &&
         a->region_id == b->region_id &&
         a->template_parameter == b->template_parameter &&
         strcmp(a->text, b->text) == 0 &&
         strcmp(a->game_text, b->game_text) == 0;
}

static bool hint_plan_same_fact_multiset(const RandoHintPlan *a,
                                         const RandoHintPlan *b) {
  if (a == NULL || b == NULL || a->fact_count != b->fact_count)
    return false;
  bool matched[kRandoHintPlanMaxFacts] = {false};
  for (uint8 i = 0; i < a->fact_count; i++) {
    bool found = false;
    for (uint8 j = 0; j < b->fact_count; j++) {
      if (matched[j] || !hint_fact_compat_equal(&a->facts[i], &b->facts[j]))
        continue;
      matched[j] = true;
      found = true;
      break;
    }
    if (!found) return false;
  }
  return true;
}

void Hints_SelfCheck(void) {
  // The generated artifact must match the registries linked into this binary.
  // Codegen proves the final exact-text cross-product injective; these runtime
  // checks catch a stale/mixed artifact and exercise the actual C renderer.
  if (kGeneratedHintMetadataFormatVersion != 1u ||
      kGeneratedHintMetadataAlgorithmVersion !=
          kRandoHintPlanAlgorithmVersion ||
      kGeneratedHintMetadataTextSchemaVersion !=
          kRandoHintTextSchemaVersion ||
      kGeneratedHintItemMetadataCount != ITEM__COUNT ||
      kGeneratedHintLocationMetadataCount != kRandoLocationsCount ||
      kGeneratedHintRegionMetadataCount != kRandoRegionsCount ||
      strlen(kGeneratedHintMetadataApprovedAlgorithmFingerprint) != 64 ||
      strlen(kGeneratedHintMetadataApprovedTextFingerprint) != 64)
    hints_selfcheck_die("generated hint metadata identity/count drift");
  for (uint16 item = 0; item < ITEM__COUNT; item++) {
    const GeneratedHintItemMetadata *metadata = hint_item_metadata(item);
    if (metadata == NULL || metadata->short_name[0] == '\0' ||
        metadata->qualified_name[0] == '\0' ||
        metadata->compact_qualified_name[0] == '\0')
      hints_selfcheck_die("generated hint item metadata missing");
    bool useful =
        (metadata->flags & kGeneratedHintItemFlag_Useful) != 0;
    bool junk = (metadata->flags & kGeneratedHintItemFlag_Junk) != 0;
    if (useful == junk ||
        ((metadata->flags & kGeneratedHintItemFlag_Priority) != 0 &&
         (metadata->flags & kGeneratedHintItemFlag_Major) == 0) ||
        ((metadata->flags & kGeneratedHintItemFlag_Major) != 0 && !useful))
      hints_selfcheck_die("generated hint item classification invariant");
    if (!useful) continue;
    for (uint16 other = (uint16)(item + 1); other < ITEM__COUNT; other++) {
      const GeneratedHintItemMetadata *other_metadata =
          hint_item_metadata(other);
      if (other_metadata != NULL &&
          (other_metadata->flags & kGeneratedHintItemFlag_Useful) != 0 &&
          strcmp(metadata->qualified_name,
                 other_metadata->qualified_name) == 0)
        hints_selfcheck_die("generated qualified item alias collision");
    }
  }
  if (hint_item_diversity_key(ITEM_ProgressiveSword) !=
          hint_item_diversity_key(ITEM_L1Sword) ||
      hint_item_diversity_key(ITEM_ProgressiveBow) !=
          hint_item_diversity_key(ITEM_Bow) ||
      hint_item_diversity_key(ITEM_BottleEmpty) !=
          hint_item_diversity_key(ITEM_BottleWithFairy) ||
      hint_item_diversity_key(ITEM_Hookshot) ==
          hint_item_diversity_key(ITEM_Hammer))
    hints_selfcheck_die("generated hint diversity groups");

  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    const GeneratedHintLocationMetadata *metadata =
        &kGeneratedHintLocationMetadata[i];
    if (metadata->location_id != kRandoLocations[i].id ||
        metadata->compact_name == NULL || metadata->compact_name[0] == '\0' ||
        strlen(metadata->compact_name) >= 64)
      hints_selfcheck_die("generated hint location metadata drift");
    for (uint32 j = i + 1; j < kRandoLocationsCount; j++)
      if (strcmp(metadata->compact_name,
                 kGeneratedHintLocationMetadata[j].compact_name) == 0)
        hints_selfcheck_die("generated compact location alias collision");
  }
  for (size_t i = 0; i < kGeneratedHintHighFrictionLocationCount; i++)
    if (hint_location_metadata(kGeneratedHintHighFrictionLocations[i]) == NULL)
      hints_selfcheck_die("generated high-friction location drift");
  for (uint32 i = 0; i < kRandoRegionsCount; i++) {
    const GeneratedHintRegionMetadata *metadata =
        &kGeneratedHintRegionMetadata[i];
    if (metadata->region_id != i || metadata->qualified_name == NULL ||
        metadata->qualified_name[0] == '\0' ||
        metadata->compact_name == NULL || metadata->compact_name[0] == '\0' ||
        strlen(metadata->qualified_name) >= 96 ||
        strlen(metadata->compact_name) >= 64)
      hints_selfcheck_die("generated hint region metadata drift");
  }

  // Regression oracle for the formerly colliding terrain identities. These
  // rows are present only when local terrain artifacts are available; if one
  // is present, both must render distinct final exact text retaining S52/S54.
  const GeneratedHintLocationMetadata *terrain_s52 =
      hint_location_metadata(5330u);
  const GeneratedHintLocationMetadata *terrain_s54 =
      hint_location_metadata(5444u);
  if ((terrain_s52 == NULL) != (terrain_s54 == NULL))
    hints_selfcheck_die("terrain S52/S54 metadata pair incomplete");
  if (terrain_s52 != NULL) {
    char full_s52[160], full_s54[160], game_s52[96], game_s54[96];
    if (!hint_build_exact_text(ITEM_Hookshot, 5330u,
                               full_s52, sizeof(full_s52),
                               game_s52, sizeof(game_s52)) ||
        !hint_build_exact_text(ITEM_Hookshot, 5444u,
                               full_s54, sizeof(full_s54),
                               game_s54, sizeof(game_s54)) ||
        strcmp(game_s52, game_s54) == 0 ||
        strstr(game_s52, "S52") == NULL ||
        strstr(game_s54, "S54") == NULL)
      hints_selfcheck_die("terrain S52/S54 final exact-text uniqueness");
  }

  // Synthetic placement table: 4 entries — 2 progression items (Hookshot,
  // Boots) + 2 junk (Rupee5, Bombs1). Item IDs sourced from item_ids.h so
  // future registry re-ids don't break the test. Prior version used raw
  // integers 6 and 8 with Hookshot / Boots comments, but those IDs are
  // L2Sword / L4Sword in the actual registry — the test still exercised
  // determinism but on the wrong items, which is exactly the drift this
  // file's item_is_junk symbol-reference fix was added to prevent.
  static RandoPlacement synth_entries[4];
  synth_entries[0].location_id = 1;  synth_entries[0].item_id = ITEM_Hookshot;
  synth_entries[1].location_id = 2;  synth_entries[1].item_id = ITEM_Boots;
  synth_entries[2].location_id = 3;  synth_entries[2].item_id = ITEM_Rupee5;
  synth_entries[3].location_id = 4;  synth_entries[3].item_id = ITEM_Bombs1;
  RandoPlacementTable table;
  table.entries = synth_entries;
  table.count = 4;

  RandoSettings settings;
  Settings_SetDefaults(&settings);
  settings.hints = kHintsMode_On;
  settings.goal = kGoal_FastGanon;  // Murahdahla path inactive on Fast Ganon.

  // Region metadata follows the same world-state-then-entrance precedence as
  // reachability. Ether Tablet is the pinned Inverted override canary.
  {
    const RandoLocationDef *ether = hint_location_def(LOC_Ether_Tablet);
    uint16 east =
        Rando_FindRegionByName("LightWorld_DeathMountain_East");
    uint16 moved =
        Rando_FindRegionByName("DarkWorld_NorthWest");
    RandoSettings inverted = settings;
    inverted.world_state = kWorldState_Inverted;
    Rando_ClearEntranceRegionOverrides();
    if (ether == NULL || east == 0xFFFFu || moved == 0xFFFFu ||
        hint_effective_region(ether, &inverted) != east)
      hints_selfcheck_die("Inverted effective hint region");
    Rando_BeginEntranceRegionOverrides();
    Rando_SetEntranceRegionOverride(LOC_Ether_Tablet, moved);
    if (hint_effective_region(ether, &inverted) != moved)
      hints_selfcheck_die("entrance hint-region precedence");
    Rando_ClearEntranceRegionOverrides();
    if (hint_effective_region(ether, &inverted) != east)
      hints_selfcheck_die("cleared entrance hint-region override");
  }

  // Region-value counts cover every generated Useful class, not only Major.
  // One map plus one major item in the same region must report two.
  {
    uint16 short_region = Rando_FindRegionByName("DarkWorld_Mire");
    HintCandidate region_candidates[2] = {
      {LOC_Sanctuary, ITEM_Hookshot, short_region, 0},
      {LOC_Sewers_Secret_Room_Left, ITEM_Map_EasternPalace, short_region, 0},
    };
    RandoHintPlan region_plan;
    uint8 primary_ids[kRandoHintPlanPrimaryTarget];
    memset(&region_plan, 0, sizeof(region_plan));
    memset(primary_ids, kRandoHintNoFact, sizeof(primary_ids));
    if (short_region == 0xFFFFu ||
        !hint_useful_item(ITEM_Map_EasternPalace) ||
        hint_major_item(ITEM_Map_EasternPalace) ||
        hint_add_region_value(&region_plan, region_candidates, 2,
                              2, primary_ids, 0) != 1 ||
        region_plan.fact_count != 1 ||
        region_plan.facts[0].template_parameter != 2)
      hints_selfcheck_die("region-value useful count");

    // An unavailable or unrenderable top-ranked region consumes a candidate,
    // not one of the two successful region-summary slots.
    uint16 second_region = Rando_FindRegionByName("EasternPalace");
    HintCandidate fallback_candidates[9] = {
      {LOC_Sanctuary, ITEM_Hookshot, kReachabilityMaxRegions - 1, 0},
      {LOC_Sanctuary, ITEM_Hookshot, kReachabilityMaxRegions - 1, 0},
      {LOC_Sanctuary, ITEM_Hookshot, kReachabilityMaxRegions - 1, 0},
      {LOC_Sanctuary, ITEM_Hookshot, kReachabilityMaxRegions - 1, 0},
      {LOC_Sanctuary, ITEM_Hookshot, short_region, 0},
      {LOC_Sanctuary, ITEM_Hookshot, short_region, 0},
      {LOC_Sanctuary, ITEM_Hookshot, short_region, 0},
      {LOC_Sanctuary, ITEM_Hookshot, second_region, 0},
      {LOC_Sanctuary, ITEM_Hookshot, second_region, 0},
    };
    memset(&region_plan, 0, sizeof(region_plan));
    memset(primary_ids, kRandoHintNoFact, sizeof(primary_ids));
    if (second_region == 0xFFFFu ||
        hint_region_metadata(kReachabilityMaxRegions - 1) != NULL ||
        hint_add_region_value(&region_plan, fallback_candidates, 9,
                              2, primary_ids, 0) != 2 ||
        region_plan.fact_count != 2)
      hints_selfcheck_die("region-value successful backfill");
  }

  // First run: capture the hint table.
  bool ok = Rando_GenerateHints(&settings, &table, NULL);
  if (!ok) {
    fprintf(stderr, "Hints_SelfCheck: first GenerateHints failed.\n");
    abort();
  }
  HintEntry snapshot[kRandoHintNpc__Count];
  memcpy(snapshot, g_hint_table, sizeof(snapshot));

  // Second run: must produce byte-identical output.
  Rando_ClearHints();
  ok = Rando_GenerateHints(&settings, &table, NULL);
  if (!ok) {
    fprintf(stderr, "Hints_SelfCheck: second GenerateHints failed.\n");
    abort();
  }
  if (memcmp(snapshot, g_hint_table, sizeof(snapshot)) != 0) {
    fprintf(stderr, "Hints_SelfCheck: non-deterministic hint output.\n");
    abort();
  }

  // Scarce/customizer pools underfill without duplicate assignments and every
  // unfilled tile remains a truthful neutral surface.
  {
    uint8 assignment_count[kRandoHintPlanMaxFacts] = {0};
    bool found_unfilled_tile = false;
    uint16 unfilled_msg = 0;
    if (g_hint_plan.fact_count >= kRandoHintPlanMaxFacts)
      hints_selfcheck_die("scarce fixture unexpectedly filled the deck");
    for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
         npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
      uint8 fact_id = g_hint_plan.primary_fact_by_npc[npc];
      if (fact_id < g_hint_plan.fact_count) {
        assignment_count[fact_id]++;
      } else if (!found_unfilled_tile) {
        found_unfilled_tile = true;
        unfilled_msg =
            kHintTileMsgIds[npc - kRandoHintNpc_TeleEasternPalace];
      }
    }
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      if (g_hint_plan.paid_count[source] > kRandoHintPaidQueueCapacity)
        hints_selfcheck_die("scarce paid count overflow");
      for (uint8 q = 0; q < g_hint_plan.paid_count[source]; q++) {
        uint8 fact_id = g_hint_plan.paid_queue[source][q];
        if (fact_id >= g_hint_plan.fact_count)
          hints_selfcheck_die("scarce paid assignment invalid");
        assignment_count[fact_id]++;
      }
    }
    for (uint8 i = 0; i < g_hint_plan.fact_count; i++)
      if (assignment_count[i] != 1)
        hints_selfcheck_die("scarce fact assignment duplicate/orphan");
    if (!found_unfilled_tile)
      hints_selfcheck_die("scarce fixture lacks an unfilled tile");

    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_dialogue_flags = g_zenv.dialogue_flags;
    const char *saved_language = g_config.language;
    uint8 encoded[256];
    uint8 discovered_before = Rando_GetHintDiscoveredCount();
    g_rando_slot_active = 1;
    g_zenv.dialogue_flags = 0;
    g_config.language = NULL;
    if (!Rando_RenderHintMessage(unfilled_msg, encoded) ||
        !encoded_hint_contains(encoded, "No useful hint here") ||
        Rando_GetHintDiscoveredCount() != discovered_before)
      hints_selfcheck_die("scarce unfilled tile neutral contract");
    g_config.language = saved_language;
    g_zenv.dialogue_flags = saved_dialogue_flags;
    g_rando_slot_active = saved_slot_active;
  }

  // Murahdahla determinism: the Fast-Ganon goal above never enters the
  // Triforce/Ganon-hunt branch — the only path with nested region-dedup
  // iteration, and thus the one most prone to nondeterministic drift — so its
  // determinism was previously unasserted. Round-trip a second time under
  // Triforce Hunt with a TriforcePiece in the pool so the branch actually runs.
  {
    static RandoPlacement th_entries[4];
    th_entries[0].location_id = 1;  th_entries[0].item_id = ITEM_TriforcePiece;
    th_entries[1].location_id = 2;  th_entries[1].item_id = ITEM_Hookshot;
    th_entries[2].location_id = 3;  th_entries[2].item_id = ITEM_Boots;
    th_entries[3].location_id = 4;  th_entries[3].item_id = ITEM_Rupee5;
    RandoPlacementTable th_table;
    th_table.entries = th_entries;
    th_table.count = 4;
    RandoSettings th = settings;       // still Balanced from above
    th.hints = kHintsMode_On;
    th.goal = kGoal_TriforceHunt;

    Rando_ClearHints();
    if (!Rando_GenerateHints(&th, &th_table, NULL)) {
      fprintf(stderr, "Hints_SelfCheck: Triforce-Hunt GenerateHints failed.\n");
      abort();
    }
    HintEntry th_snapshot[kRandoHintNpc__Count];
    memcpy(th_snapshot, g_hint_table, sizeof(th_snapshot));
    Rando_ClearHints();
    if (!Rando_GenerateHints(&th, &th_table, NULL)) {
      fprintf(stderr, "Hints_SelfCheck: second Triforce-Hunt GenerateHints failed.\n");
      abort();
    }
    if (memcmp(th_snapshot, g_hint_table, sizeof(th_snapshot)) != 0) {
      fprintf(stderr, "Hints_SelfCheck: non-deterministic Murahdahla hint output.\n");
      abort();
    }
  }

  // Medallion config slots are generation-time requirements, not item
  // locations. They should not be emitted as item hints even if their
  // pre-pinned item id is otherwise hintable.
  {
    static RandoPlacement med_entries[2];
    med_entries[0].location_id = LOC_Misery_Mire_Medallion;
    med_entries[0].item_id = ITEM_Hookshot;
    med_entries[1].location_id = LOC_Turtle_Rock_Medallion;
    med_entries[1].item_id = ITEM_Boots;
    RandoPlacementTable med_table;
    med_table.entries = med_entries;
    med_table.count = 2;
    Rando_ClearHints();
    settings.hints = kHintsMode_On;
    if (!Rando_GenerateHints(&settings, &med_table, NULL)) {
      fprintf(stderr, "Hints_SelfCheck: medallion-config GenerateHints failed.\n");
      abort();
    }
    for (uint8 i = 0; i < g_hint_plan.fact_count; i++) {
      if (g_hint_plan.facts[i].location_id == LOC_Misery_Mire_Medallion ||
          g_hint_plan.facts[i].location_id == LOC_Turtle_Rock_Medallion) {
        fprintf(stderr, "Hints_SelfCheck: medallion config slot was hinted.\n");
        abort();
      }
    }
  }

  // Hints v2 full-plan contract: deterministic caller-owned bytes regardless
  // of placement iteration order, exactly 18 assigned primaries + six unique
  // paid reserves when the pool is large enough, strict persisted identity,
  // render-after-success discovery, transactional paid advancement, checked
  // target skipping, exhaustion, and locale-aware neutral fallbacks.
  {
    static const uint16 kPlanItems[] = {
      ITEM_ProgressiveGlove, ITEM_ProgressiveBow, ITEM_FireRod,
      ITEM_IceRod, ITEM_Hammer, ITEM_Hookshot, ITEM_MagicMirror,
      ITEM_Boots, ITEM_Flippers, ITEM_MoonPearl,
      ITEM_ProgressiveSword, ITEM_ProgressiveShield,
      ITEM_ProgressiveArmor, ITEM_Lamp, ITEM_Mushroom, ITEM_Shovel,
      ITEM_OcarinaInactive, ITEM_BookOfMudora, ITEM_CaneOfSomaria,
      ITEM_CaneOfByrna, ITEM_Cape, ITEM_HalfMagic, ITEM_QuarterMagic,
      ITEM_BottleEmpty,
    };
    static RandoPlacement full_entries[64];
    static RandoPlacement reverse_entries[64];
    static RandoPlacement triforce_entries[64];
    static RandoSpheres full_spheres;
    static RandoHintPlan plan_a, plan_b, plan_c, plan_d, plan_e, plan_f,
                         legacy_a, legacy_b, legacy_c, legacy_e, legacy_f,
                         digest_probe, off_plan;
    uint16 full_count = 0;
    for (uint16 h = 0;
         h < kGeneratedHintHighFrictionLocationCount &&
         full_count < (uint16)(sizeof(full_entries) /
                               sizeof(full_entries[0]));
         h++) {
      const RandoLocationDef *loc =
          hint_location_def(kGeneratedHintHighFrictionLocations[h]);
      if (loc == NULL || loc->region_id == 0xFFFFu ||
          !Rando_LocationTypeCountsAsCheck(loc->type))
        continue;
      full_entries[full_count].location_id = loc->id;
      full_entries[full_count].item_id =
          kPlanItems[full_count %
                     (sizeof(kPlanItems) / sizeof(kPlanItems[0]))];
      full_count++;
    }
    for (uint32 i = 0; i < kRandoLocationsCount &&
                         full_count < (uint16)(sizeof(full_entries) /
                                               sizeof(full_entries[0])); i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      if (loc->region_id == 0xFFFFu ||
          !Rando_LocationTypeCountsAsCheck(loc->type))
        continue;
      bool already_present = false;
      for (uint16 j = 0; j < full_count; j++)
        if (full_entries[j].location_id == loc->id) {
          already_present = true;
          break;
        }
      if (already_present) continue;
      full_entries[full_count].location_id = loc->id;
      full_entries[full_count].item_id =
          kPlanItems[full_count %
                     (sizeof(kPlanItems) / sizeof(kPlanItems[0]))];
      full_count++;
    }
    if (full_count != sizeof(full_entries) / sizeof(full_entries[0]))
      hints_selfcheck_die("full-plan fixture lacks locations");
    for (uint16 i = 0; i < full_count; i++)
      reverse_entries[i] = full_entries[full_count - 1 - i];
    RandoPlacementTable full_table = {full_entries, full_count};
    RandoPlacementTable reverse_table = {reverse_entries, full_count};
    memset(&full_spheres, 0, sizeof(full_spheres));

    RandoSettings full_settings;
    Settings_SetDefaults(&full_settings);
    full_settings.hints = kHintsMode_Balanced;
    full_settings.goal = kGoal_FastGanon;
    if (!Rando_BuildHintPlan(
            &full_settings, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_a) ||
        !Rando_BuildHintPlan(
            &full_settings, &reverse_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_b))
      hints_selfcheck_die("full-plan build");
    if (memcmp(&plan_a, &plan_b, sizeof(plan_a)) != 0)
      hints_selfcheck_die("plan changed with placement iteration order");
    if (!Rando_BuildHintPlan(
            &full_settings, &full_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &legacy_a) ||
        !Rando_BuildHintPlan(
            &full_settings, &reverse_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &legacy_b) ||
        memcmp(&legacy_a, &legacy_b, sizeof(legacy_a)) != 0)
      hints_selfcheck_die("legacy plan changed with placement iteration order");
    if (plan_a.fact_count != kRandoHintPlanMaxFacts)
      hints_selfcheck_die("full plan must contain 24 facts");
    uint8 compat_fingerprint[kRandoHintPlanDigestBytes];
    if (!hint_plan_compute_compat_fingerprint(&legacy_a,
                                              compat_fingerprint))
      hints_selfcheck_die("default compatibility rendering incomplete");
    bool compat_fingerprint_nonzero = false;
    for (size_t i = 0; i < sizeof(compat_fingerprint); i++)
      compat_fingerprint_nonzero |= compat_fingerprint[i] != 0;
    if (!compat_fingerprint_nonzero)
      hints_selfcheck_die("v1 compatibility fingerprint unavailable");
    if (!hint_plan_same_fact_multiset(&legacy_a, &plan_a))
      hints_selfcheck_die("Variety changed the legacy fact/text multiset");
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      if (legacy_a.paid_count[source] != plan_a.paid_count[source])
        hints_selfcheck_die("Variety changed a legacy paid queue count");
      for (uint8 q = 0; q < legacy_a.paid_count[source]; q++) {
        uint8 legacy_id = legacy_a.paid_queue[source][q];
        uint8 current_id = plan_a.paid_queue[source][q];
        if (legacy_id >= legacy_a.fact_count ||
            current_id >= plan_a.fact_count ||
            !hint_fact_compat_equal(&legacy_a.facts[legacy_id],
                                    &plan_a.facts[current_id]))
          hints_selfcheck_die("Variety changed a legacy paid queue");
      }
    }

    uint8 assignment_count[kRandoHintPlanMaxFacts] = {0};
    uint8 primary_count = 0;
    uint8 major_primary_count = 0;
    uint8 priority_primary_count = 0;
    uint8 friction_primary_count = 0;
    uint8 goal_primary_count = 0;
    uint8 setting_primary_count = 0;
    uint8 region_primary_count = 0;
    uint8 useful_primary_count = 0;
    uint8 flavor_primary_count = 0;
    for (uint8 i = 0; i < plan_a.fact_count; i++) {
      const RandoHintFact *fact = &plan_a.facts[i];
      if (fact->fact_id != i || fact->text[0] == '\0' ||
          fact->game_text[0] == '\0' ||
          !hint_text_fits_game(fact->game_text))
        hints_selfcheck_die("fact identity/complete text");
      if (fact->primary) {
        primary_count++;
        switch (fact->kind) {
        case kRandoHintFact_PriorityItem: priority_primary_count++; break;
        case kRandoHintFact_MajorItem: major_primary_count++; break;
        case kRandoHintFact_HighFrictionLocation:
          friction_primary_count++;
          break;
        case kRandoHintFact_Goal: goal_primary_count++; break;
        case kRandoHintFact_ActiveSetting: setting_primary_count++; break;
        case kRandoHintFact_RegionValue: region_primary_count++; break;
        case kRandoHintFact_Useful: useful_primary_count++; break;
        case kRandoHintFact_Flavor: flavor_primary_count++; break;
        default: hints_selfcheck_die("unknown primary fact kind");
        }
      }
      if (fact->location_id == 0xFFFFu) continue;
      for (uint8 j = 0; j < i; j++) {
        if (plan_a.facts[j].location_id == fact->location_id)
          hints_selfcheck_die("duplicate exact target");
        if (plan_a.facts[j].location_id != 0xFFFFu &&
            hint_item_diversity_key(plan_a.facts[j].item_id) ==
                hint_item_diversity_key(fact->item_id))
          hints_selfcheck_die("duplicate item family chosen while diversity exists");
      }
    }
    if (primary_count != kRandoHintPlanPrimaryTarget)
      hints_selfcheck_die("full plan primary quota");
    if (priority_primary_count != 2 || major_primary_count != 6 ||
        friction_primary_count != 3 || goal_primary_count != 1 ||
        setting_primary_count != 0 || region_primary_count != 2 ||
        useful_primary_count != 3 || flavor_primary_count != 1)
      hints_selfcheck_die("full plan category/backfill quotas");
    for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
         npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
      uint8 fact_id = plan_a.primary_fact_by_npc[npc];
      if (fact_id >= plan_a.fact_count)
        hints_selfcheck_die("unfilled full-plan tile");
      assignment_count[fact_id]++;
    }
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
      if (plan_a.paid_count[source] != kRandoHintPaidQueueCapacity)
        hints_selfcheck_die("full-plan paid queue underfilled");
      uint8 paid_head = plan_a.paid_queue[source][0];
      if (paid_head >= plan_a.fact_count ||
          plan_a.facts[paid_head].location_id == 0xFFFFu ||
          !plan_a.facts[paid_head].primary ||
          plan_a.primary_fact_by_npc[kRandoHintNpc_ForkStoryteller + source] !=
              paid_head)
        hints_selfcheck_die("paid queue head must be an exact primary fact");
      for (uint8 queue = 0; queue < kRandoHintPaidQueueCapacity; queue++) {
        uint8 fact_id = plan_a.paid_queue[source][queue];
        if (fact_id >= plan_a.fact_count)
          hints_selfcheck_die("invalid paid assignment");
        assignment_count[fact_id]++;
      }
    }
    for (uint8 i = 0; i < plan_a.fact_count; i++)
      if (assignment_count[i] != 1)
        hints_selfcheck_die("fact must have one delivery assignment");

    // Algorithm-2 policy is maximal-then-pruned. Coverage retains exact
    // physical-source/fact prefixes, paid depth retains per-service queue
    // prefixes, and every rich topology compacts to the literal capacity.
    {
      static RandoHintPlan coverage_plans[3];
      static RandoHintPlan paid_plans[4];
      static RandoPlacement policy_entries[64];
      static RandoPlacement reverse_policy_entries[64];
      static const uint16 kPolicyUsefulItems[] = {
        ITEM_Map_EasternPalace,
        ITEM_Compass_EasternPalace,
        ITEM_Map_DesertPalace,
        ITEM_Compass_DesertPalace,
        ITEM_Map_TowerOfHera,
        ITEM_Compass_TowerOfHera,
        ITEM_Map_PalaceOfDarkness,
        ITEM_Compass_PalaceOfDarkness,
        ITEM_Map_SwampPalace,
        ITEM_Compass_SwampPalace,
      };
      uint16 policy_major_cursor = 0;
      for (uint16 i = 0; i < full_count; i++) {
        policy_entries[i].location_id = full_entries[i].location_id;
        if ((i & 3u) == 0) {
          policy_entries[i].item_id =
              kPolicyUsefulItems[(i >> 2) %
                  (sizeof(kPolicyUsefulItems) /
                   sizeof(kPolicyUsefulItems[0]))];
        } else {
          policy_entries[i].item_id =
              kPlanItems[policy_major_cursor++ %
                  (sizeof(kPlanItems) / sizeof(kPlanItems[0]))];
        }
      }
      RandoPlacementTable policy_table = {policy_entries, full_count};
      for (uint16 i = 0; i < full_count; i++)
        reverse_policy_entries[i] = policy_entries[full_count - 1 - i];
      RandoPlacementTable reverse_policy_table = {
        reverse_policy_entries, full_count};
      const uint8 coverages[] = {
        kHintTileCoverage_Sparse,
        kHintTileCoverage_Standard,
        kHintTileCoverage_Full,
      };
      const uint8 expected_tiles[] = {5, 10, 15};
      for (uint8 mix = kHintMix_Variety;
           mix <= kHintMix_WorldInfo; mix++) {
        RandoSettings policy = full_settings;
        policy.hint_mix = mix;
        policy.hint_paid_depth = 3;
        if (mix == kHintMix_WorldInfo) {
          // Goal plus two active settings make the literal three-row
          // goal/setting quota observable in this rich fixture.
          policy.shopsanity = 1;
          policy.boss_shuffle = 1;
        }
        for (uint8 c = 0; c < 3; c++) {
          policy.hint_tile_coverage = coverages[c];
          if (!Rando_BuildHintPlan(
                  &policy, &policy_table, &full_spheres,
                  kRandoHintPlanAlgorithmVersion,
                  kRandoHintTextSchemaVersion, &coverage_plans[c]) ||
              !hint_plan_structure_valid(&coverage_plans[c]))
            hints_selfcheck_die("coverage policy build/structure");
          uint8 assigned_tiles = 0;
          for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
               npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++)
            assigned_tiles +=
                coverage_plans[c].primary_fact_by_npc[npc] !=
                kRandoHintNoFact;
          if (coverage_plans[c].tile_target != expected_tiles[c] ||
              coverage_plans[c].mix != mix ||
              coverage_plans[c].paid_depth != 3 ||
              assigned_tiles != expected_tiles[c] ||
              coverage_plans[c].fact_count !=
                  (uint8)(expected_tiles[c] + 9))
            hints_selfcheck_die("rich coverage topology");
        }
        for (uint8 c = 0; c < 2; c++) {
          for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
               npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
            uint8 smaller =
                coverage_plans[c].primary_fact_by_npc[npc];
            if (smaller == kRandoHintNoFact) continue;
            uint8 larger =
                coverage_plans[c + 1].primary_fact_by_npc[npc];
            if (larger == kRandoHintNoFact ||
                !hint_fact_compat_equal(
                    &coverage_plans[c].facts[smaller],
                    &coverage_plans[c + 1].facts[larger]))
              hints_selfcheck_die("tile fact/source prefix moved");
          }
        }

        policy.hint_tile_coverage = kHintTileCoverage_Full;
        for (uint8 depth = 0; depth <= 3; depth++) {
          policy.hint_paid_depth = depth;
          if (!Rando_BuildHintPlan(
                  &policy, &policy_table, &full_spheres,
                  kRandoHintPlanAlgorithmVersion,
                  kRandoHintTextSchemaVersion, &paid_plans[depth]) ||
              !hint_plan_structure_valid(&paid_plans[depth]))
            hints_selfcheck_die("paid-depth policy build/structure");
          uint8 expected_fact_count =
              (uint8)(kHintTileCount +
                      (depth != 0 ? kRandoHintPaidSourceCount : 0) +
                      kRandoHintPaidSourceCount *
                          (depth > 0 ? depth - 1 : 0));
          if (paid_plans[depth].paid_depth != depth ||
              paid_plans[depth].fact_count != expected_fact_count)
            hints_selfcheck_die("rich paid-depth topology");
          for (uint8 source = 0; source < kRandoHintPaidSourceCount;
               source++)
            if (paid_plans[depth].paid_count[source] != depth)
              hints_selfcheck_die("rich paid queue depth");
        }
        for (uint8 depth = 1; depth < 3; depth++) {
          for (uint8 source = 0; source < kRandoHintPaidSourceCount;
               source++) {
            for (uint8 q = 0; q < depth; q++) {
              uint8 smaller = paid_plans[depth].paid_queue[source][q];
              uint8 larger = paid_plans[depth + 1].paid_queue[source][q];
              if (smaller >= paid_plans[depth].fact_count ||
                  larger >= paid_plans[depth + 1].fact_count ||
                  !hint_fact_compat_equal(
                      &paid_plans[depth].facts[smaller],
                      &paid_plans[depth + 1].facts[larger]))
                hints_selfcheck_die("paid queue prefix moved");
            }
          }
        }
        for (uint8 depth = 0; depth < 3; depth++) {
          for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
               npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
            uint8 smaller =
                paid_plans[depth].primary_fact_by_npc[npc];
            uint8 larger =
                paid_plans[depth + 1].primary_fact_by_npc[npc];
            if (smaller >= paid_plans[depth].fact_count ||
                larger >= paid_plans[depth + 1].fact_count ||
                !hint_fact_compat_equal(
                    &paid_plans[depth].facts[smaller],
                    &paid_plans[depth + 1].facts[larger]))
              hints_selfcheck_die("paid depth moved a tile source/fact");
          }
        }

        const RandoHintPlan *quota = &paid_plans[3];
        uint8 quota_counts[kRandoHintFact_Flavor + 1] = {0};
        for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
             npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
          uint8 fact_id = quota->primary_fact_by_npc[npc];
          if (fact_id >= quota->fact_count)
            hints_selfcheck_die("rich mix tile underfill");
          uint8 kind = quota->facts[fact_id].kind;
          if (kind > kRandoHintFact_Flavor)
            hints_selfcheck_die("rich mix unknown kind");
          quota_counts[kind]++;
        }
        if (mix == kHintMix_Important &&
            (quota_counts[kRandoHintFact_PriorityItem] != 2 ||
             quota_counts[kRandoHintFact_MajorItem] != 8 ||
             quota_counts[kRandoHintFact_Useful] != 5))
          hints_selfcheck_die("Important literal tile quotas");
        if (mix == kHintMix_Difficult &&
            (quota_counts[kRandoHintFact_HighFrictionLocation] != 9 ||
             quota_counts[kRandoHintFact_PriorityItem] != 2 ||
             quota_counts[kRandoHintFact_MajorItem] != 2 ||
             quota_counts[kRandoHintFact_Useful] != 2))
          hints_selfcheck_die("Difficult literal tile quotas");
        if (mix == kHintMix_WorldInfo &&
            (quota_counts[kRandoHintFact_Goal] +
                 quota_counts[kRandoHintFact_ActiveSetting] != 3 ||
             quota_counts[kRandoHintFact_RegionValue] != 6 ||
             quota_counts[kRandoHintFact_HighFrictionLocation] != 3 ||
             quota_counts[kRandoHintFact_Useful] != 3))
          hints_selfcheck_die("WorldInfo literal tile quotas");
        if (mix != kHintMix_Variety) {
          static RandoHintPlan reversed_policy_plan;
          if (!Rando_BuildHintPlan(
                  &policy, &reverse_policy_table, &full_spheres,
                  kRandoHintPlanAlgorithmVersion,
                  kRandoHintTextSchemaVersion, &reversed_policy_plan) ||
              memcmp(quota, &reversed_policy_plan,
                     sizeof(reversed_policy_plan)) != 0)
            hints_selfcheck_die(
                "custom mix changed with placement iteration order");
        }
        // Suppressed spoilers normalize race_mode out of their canonical
        // stamp before reveal regeneration. Hint policy is orthogonal to that
        // transport flag, so both sides must produce the exact same plan.
        {
          RandoSettings race_policy = policy;
          static RandoHintPlan race_policy_plan;
          race_policy.race_mode = 1;
          if (!Rando_BuildHintPlan(
                  &race_policy, &policy_table, &full_spheres,
                  kRandoHintPlanAlgorithmVersion,
                  kRandoHintTextSchemaVersion, &race_policy_plan) ||
              memcmp(quota, &race_policy_plan,
                     sizeof(race_policy_plan)) != 0)
            hints_selfcheck_die("race mode changed the hint plan");
        }
      }

      RandoSettings sparse_variety = full_settings;
      sparse_variety.hint_tile_coverage = kHintTileCoverage_Sparse;
      sparse_variety.hint_paid_depth = 3;
      sparse_variety.hint_mix = kHintMix_Variety;
      static RandoHintPlan sparse_plan;
      if (!Rando_BuildHintPlan(
              &sparse_variety, &policy_table, &full_spheres,
              kRandoHintPlanAlgorithmVersion,
              kRandoHintTextSchemaVersion, &sparse_plan))
        hints_selfcheck_die("Sparse Variety build");
      bool sparse_bands[5] = {false};
      for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
           npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
        uint8 fact_id = sparse_plan.primary_fact_by_npc[npc];
        if (fact_id >= sparse_plan.fact_count) continue;
        for (uint8 band = 0; band < 5; band++)
          sparse_bands[band] |= hint_fact_matches_tile_token(
              &sparse_plan.facts[fact_id], band);
      }
      for (uint8 band = 0; band < 5; band++)
        if (!sparse_bands[band])
          hints_selfcheck_die("Sparse Variety did not span five bands");

      // Pin the literal I,D,W,U,F,I,D,W,U,I,D,W,U,I,W scheduler directly,
      // independently of any registry fixture's category availability.
      static RandoHintPlan band_fixture;
      uint8 band_order[kHintTileCount];
      memset(&band_fixture, 0, sizeof(band_fixture));
      memset(band_fixture.primary_fact_by_npc, kRandoHintNoFact,
             sizeof(band_fixture.primary_fact_by_npc));
      band_fixture.fact_count = kHintTileCount;
      for (uint8 i = 0; i < kHintTileCount; i++) {
        band_fixture.facts[i].fact_id = i;
        switch ((HintTileOrderToken)kHintTileBandSchedule[i]) {
        case kHintTileOrder_Important:
          band_fixture.facts[i].kind = kRandoHintFact_MajorItem;
          break;
        case kHintTileOrder_Difficult:
          band_fixture.facts[i].kind =
              kRandoHintFact_HighFrictionLocation;
          break;
        case kHintTileOrder_WorldInfo:
          band_fixture.facts[i].kind = kRandoHintFact_RegionValue;
          break;
        case kHintTileOrder_Useful:
          band_fixture.facts[i].kind = kRandoHintFact_Useful;
          break;
        case kHintTileOrder_Flavor:
          band_fixture.facts[i].kind = kRandoHintFact_Flavor;
          break;
        }
        band_fixture.primary_fact_by_npc[
            kRandoHintNpc_TeleEasternPalace + i] = i;
      }
      if (hint_order_maximal_tile_facts(
              &band_fixture, band_order) != kHintTileCount)
        hints_selfcheck_die("literal band schedule underfilled");
      for (uint8 i = 0; i < kHintTileCount; i++)
        if (band_order[i] != i)
          hints_selfcheck_die("literal band schedule order drift");

      // A scarce non-Variety maximal deck may underfill a literal tile quota
      // and therefore leave an uneven number of exact facts for the paid
      // services. Increasing paid depth must retain every earlier per-service
      // prefix, grow only by the next available queue entry, and never
      // fabricate a missing reserve.
      RandoPlacementTable scarce_policy_table = {policy_entries, 18};
      static RandoHintPlan scarce_depth[3];
      RandoSettings scarce_policy = full_settings;
      scarce_policy.hint_tile_coverage = kHintTileCoverage_Sparse;
      scarce_policy.hint_mix = kHintMix_Important;
      for (uint8 depth = 1; depth <= 3; depth++) {
        scarce_policy.hint_paid_depth = depth;
        if (!Rando_BuildHintPlan(
                &scarce_policy, &scarce_policy_table, &full_spheres,
                kRandoHintPlanAlgorithmVersion,
                kRandoHintTextSchemaVersion, &scarce_depth[depth - 1]) ||
            !hint_plan_structure_valid(&scarce_depth[depth - 1]))
          hints_selfcheck_die("scarce custom policy structure");
      }
      bool saw_scarce_paid_queue = false;
      for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
        uint8 maximal_count = scarce_depth[2].paid_count[source];
        if (maximal_count < kRandoHintPaidQueueCapacity)
          saw_scarce_paid_queue = true;
        for (uint8 depth = 0; depth < 3; depth++) {
          uint8 count = scarce_depth[depth].paid_count[source];
          uint8 requested = (uint8)(depth + 1);
          uint8 expected =
              requested < maximal_count ? requested : maximal_count;
          if (count != expected)
            hints_selfcheck_die("scarce paid prefix count moved");
          if (depth == 0) continue;
          uint8 prior_count = scarce_depth[depth - 1].paid_count[source];
          for (uint8 q = 0; q < prior_count; q++) {
            uint8 first =
                scarce_depth[depth - 1].paid_queue[source][q];
            uint8 later = scarce_depth[depth].paid_queue[source][q];
            if (first >= scarce_depth[depth - 1].fact_count ||
                later >= scarce_depth[depth].fact_count ||
                !hint_fact_compat_equal(
                    &scarce_depth[depth - 1].facts[first],
                    &scarce_depth[depth].facts[later]))
              hints_selfcheck_die("scarce paid prefix fact moved");
          }
        }
      }
      if (!saw_scarce_paid_queue)
        hints_selfcheck_die("scarce paid fixture did not underfill");
    }

    // Murahdahla is a separate compatibility summary and cannot alter the
    // immutable delivery-plan digest.
    digest_probe = plan_a;
    digest_probe.murahdahla_active = 1;
    snprintf(digest_probe.murahdahla_text,
             sizeof(digest_probe.murahdahla_text), "digest exclusion probe");
    hint_plan_compute_digest(&digest_probe);
    if (memcmp(digest_probe.digest, plan_a.digest,
               kRandoHintPlanDigestBytes) != 0)
      hints_selfcheck_die("Murahdahla entered delivery digest");

    // Rendered strings are derived presentation, not semantic plan identity.
    // A stable template parameter is the corresponding identity-bearing input.
    digest_probe = plan_a;
    snprintf(digest_probe.facts[0].text,
             sizeof(digest_probe.facts[0].text), "presentation probe");
    snprintf(digest_probe.facts[0].game_text,
             sizeof(digest_probe.facts[0].game_text), "render probe");
    hint_plan_compute_digest(&digest_probe);
    if (memcmp(digest_probe.digest, plan_a.digest,
               kRandoHintPlanDigestBytes) != 0)
      hints_selfcheck_die("rendered text entered semantic digest");
    digest_probe = plan_a;
    digest_probe.facts[0].template_parameter++;
    hint_plan_compute_digest(&digest_probe);
    if (memcmp(digest_probe.digest, plan_a.digest,
               kRandoHintPlanDigestBytes) == 0)
      hints_selfcheck_die("template parameter missing from semantic digest");

    // Generation may hand the builder requested settings while activation
    // recovers the canonically normalized image. Both must produce one identity
    // (Retro's requested vanilla keys normalize to Wild).
    RandoSettings raw_retro, canonical_retro;
    uint8 retro_bytes[kSettingsCanonicalLen];
    if (Settings_ApplyPreset(kPreset_Retro, &raw_retro) != 0)
      hints_selfcheck_die("Retro preset fixture");
    raw_retro.hints = kHintsMode_Balanced;
    Settings_CanonicalSerialize(&raw_retro, retro_bytes);
    if (Settings_CanonicalDeserialize(retro_bytes, &canonical_retro) != 0 ||
        !Rando_BuildHintPlan(
            &raw_retro, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_c) ||
        !Rando_BuildHintPlan(
            &canonical_retro, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_d) ||
        memcmp(&plan_c, &plan_d, sizeof(plan_c)) != 0)
      hints_selfcheck_die("raw/canonical settings plan identity");
    if (!Rando_BuildHintPlan(
            &canonical_retro, &full_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &legacy_c))
      hints_selfcheck_die("legacy Retro compatibility plan");

    // Pin a setting-specific builder vector with both the static Inverted
    // region rules and the live entrance-region overlay in play.
    RandoSettings inverted_overlay = full_settings;
    inverted_overlay.world_state = kWorldState_Inverted;
    uint16 override_region = Rando_FindRegionByName("DarkWorld_Mire");
    uint16 override_count = 0;
    Rando_BeginEntranceRegionOverrides();
    for (uint16 i = 0; i < full_count; i++) {
      Rando_SetEntranceRegionOverride(full_entries[i].location_id,
                                      override_region);
      if (Rando_GetEntranceRegionOverride(full_entries[i].location_id) ==
          override_region)
        override_count++;
    }
    bool overlay_plan_ok =
        override_region != 0xFFFFu && override_count != 0 &&
        Rando_BuildHintPlan(
            &inverted_overlay, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_e) &&
        Rando_BuildHintPlan(
            &inverted_overlay, &full_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &legacy_e);
    Rando_ClearEntranceRegionOverrides();
    if (!overlay_plan_ok)
      hints_selfcheck_die("Inverted entrance-overlay plan fixture");

    // Exercise the separate Murahdahla compatibility surface in the same
    // literal. It is deliberately outside the 24-fact delivery digest, but its
    // deterministic builder output is still part of the text-schema contract.
    memcpy(triforce_entries, full_entries, sizeof(triforce_entries));
    triforce_entries[0].item_id = ITEM_TriforcePiece;
    RandoPlacementTable triforce_table = {triforce_entries, full_count};
    RandoSettings triforce_settings = full_settings;
    triforce_settings.goal = kGoal_TriforceHunt;
    if (!Rando_BuildHintPlan(
            &triforce_settings, &triforce_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &plan_f) ||
        !plan_f.murahdahla_active ||
        !Rando_BuildHintPlan(
            &triforce_settings, &triforce_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &legacy_f) ||
        !legacy_f.murahdahla_active)
      hints_selfcheck_die("Triforce compatibility plan fixture");

    // Collapse the default, canonically normalized Retro,
    // Inverted+entrance-overlay, and Triforce-Hunt golden plans into one
    // checked-in, versioned compatibility vector.
    uint8 retro_fingerprint[kRandoHintPlanDigestBytes];
    uint8 overlay_fingerprint[kRandoHintPlanDigestBytes];
    uint8 triforce_fingerprint[kRandoHintPlanDigestBytes];
    uint8 compat_vector[kRandoHintPlanDigestBytes];
    if (!hint_plan_compute_compat_fingerprint(&legacy_c,
                                              retro_fingerprint) ||
        !hint_plan_compute_compat_fingerprint(&legacy_e,
                                              overlay_fingerprint) ||
        !hint_plan_compute_compat_fingerprint(&legacy_f,
                                              triforce_fingerprint))
      hints_selfcheck_die("compatibility rendering incomplete");
    SHA256_CTX compat_ctx;
    static const uint8 compat_domain[] = "Z3R-HINT-COMPAT-V1-VECTOR";
    sha256_init(&compat_ctx);
    sha256_update(&compat_ctx, compat_domain, sizeof(compat_domain) - 1);
    sha256_update(&compat_ctx, compat_fingerprint,
                  sizeof(compat_fingerprint));
    sha256_update(&compat_ctx, retro_fingerprint,
                  sizeof(retro_fingerprint));
    sha256_update(&compat_ctx, overlay_fingerprint,
                  sizeof(overlay_fingerprint));
    sha256_update(&compat_ctx, triforce_fingerprint,
                  sizeof(triforce_fingerprint));
    sha256_final(&compat_ctx, compat_vector);
    static const uint8 kExpectedV1CompatibilityVector
        [kRandoHintPlanDigestBytes] = {
      0x1b, 0xe7, 0xf2, 0xec, 0xac, 0x3a, 0xf3, 0xad,
      0x34, 0x16, 0x5f, 0x2c, 0x29, 0x11, 0xea, 0x56,
      0x34, 0xce, 0x43, 0x77, 0x4f, 0x1a, 0x57, 0x0c,
      0xcd, 0xc0, 0x1f, 0xb2, 0x71, 0x68, 0xc8, 0x70,
    };
    if (memcmp(compat_vector, kExpectedV1CompatibilityVector,
               sizeof(compat_vector)) != 0) {
      fprintf(stderr, "Hints_SelfCheck: v1 compatibility vector drift actual=");
      for (size_t i = 0; i < sizeof(compat_vector); i++)
        fprintf(stderr, "%02x", compat_vector[i]);
      fprintf(stderr, "\n");
      hints_selfcheck_die("v1 compatibility vector drift");
    }

    RandoHintPersistedState legacy_certified = {0};
    legacy_certified.algorithm_version =
        kRandoHintPlanLegacyAlgorithmVersion;
    legacy_certified.text_schema_version =
        kRandoHintLegacyTextSchemaVersion;
    memcpy(legacy_certified.plan_digest, legacy_a.digest,
           sizeof(legacy_certified.plan_digest));
    if (Rando_HintPlanExportPersistedState(&legacy_a,
                                           &legacy_certified))
      hints_selfcheck_die("legacy plan identity exported as current persistence");
    Rando_HintsImportPersistedState(&legacy_certified);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() !=
            kRandoHintPlanStatus_UnsupportedAlgorithm ||
        Rando_GetHintFactCount() != 0)
      hints_selfcheck_die("legacy persisted identity did not fail closed");
    {
      uint8 saved_slot = g_rando_slot_active;
      uint8 saved_flags = g_zenv.dialogue_flags;
      uint8 untouched[256], legacy_prompt[256];
      memset(untouched, 0x5Au, sizeof(untouched));
      memcpy(legacy_prompt, untouched, sizeof(legacy_prompt));
      hint_install_plan(&legacy_a, NULL, kRandoHintPlanStatus_Ready);
      g_rando_slot_active = 1;
      g_zenv.dialogue_flags = 0;
      Rando_HintsSetPaidPromptContext(
          kRandoHintPaidSource_Storyteller, 20);
      if (Rando_RewriteInteractiveHintMessage(0xFEu, legacy_prompt) ||
          memcmp(legacy_prompt, untouched, sizeof(legacy_prompt)) != 0)
        hints_selfcheck_die("schema 1 paid prompt bytes changed");
      Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);
      g_zenv.dialogue_flags = saved_flags;
      g_rando_slot_active = saved_slot;
    }

    RandoHintPersistedState certified;
    if (!Rando_HintPlanExportPersistedState(&plan_a, &certified))
      hints_selfcheck_die("plan identity export");
    Rando_HintsImportPersistedState(&certified);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() != kRandoHintPlanStatus_Ready ||
        memcmp(Rando_GetHintPlanDigest(), plan_a.digest,
               kRandoHintPlanDigestBytes) != 0)
      hints_selfcheck_die("certified plan rebuild");

    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_dialogue_flags = g_zenv.dialogue_flags;
    uint8 saved_dark_world = savegame_is_darkworld;
    const char *saved_language = g_config.language;
    g_rando_slot_active = 1;
    g_zenv.dialogue_flags = 0;
    g_config.language = NULL;
    uint8 encoded[256];

    // Drive the real paid-handler seams, not only the plan API. Storyteller
    // preserves insufficient funds and charges exactly once on success.
    // Fortune state 6 commits an armed presentation and refunds an absent one
    // while retaining its normal heal/state transition.
    uint16 saved_rupees = link_rupees_goal;
    uint8 saved_sprite_state = sprite_ai_state[0];
    uint8 saved_sprite_a = sprite_A[0];
    uint8 saved_hearts_filler = link_hearts_filler;
    uint8 saved_immobilized = flag_is_link_immobilized;
    link_rupees_goal = 19;
    if (DarkWorldHintNPC_HandlePayment() ||
        link_rupees_goal != 19 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("Storyteller insufficient-funds choreography");
    link_rupees_goal = 100;
    if (!DarkWorldHintNPC_HandlePayment() ||
        link_rupees_goal != 80 ||
        Rando_GetHintDiscoveredCount() != 1)
      hints_selfcheck_die("Storyteller payment choreography");
    Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);

    Rando_HintsImportPersistedState(&certified);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        !Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneLight))
      hints_selfcheck_die("Fortune handler success setup");
    uint8 fortune_fact =
        g_hint_paid_pending[kRandoHintPaidSource_FortuneLight].fact_id;
    link_rupees_goal = 100;
    sprite_ai_state[0] = 6;
    sprite_A[0] = 0;  // 10-rupee reading
    flag_is_link_immobilized = 1;
    FortuneTeller_LightOrDarkWorld(0, false);
    if (link_rupees_goal != 90 || sprite_ai_state[0] != 7 ||
        link_hearts_filler != 160 || flag_is_link_immobilized != 0 ||
        !Rando_IsHintFactDiscovered(fortune_fact))
      hints_selfcheck_die("Fortune handler commit choreography");

    Rando_HintsImportPersistedState(&certified);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres))
      hints_selfcheck_die("Fortune handler refund setup");
    link_rupees_goal = 100;
    sprite_ai_state[0] = 6;
    sprite_A[0] = 0;
    flag_is_link_immobilized = 1;
    FortuneTeller_LightOrDarkWorld(0, false);
    if (link_rupees_goal != 100 || sprite_ai_state[0] != 7 ||
        link_hearts_filler != 160 || flag_is_link_immobilized != 0 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("Fortune handler lost-latch refund choreography");

    // Paid depth zero keeps the existing paid healing services, but schema 2
    // must disclose no clue + exact price + healing before the choice in every
    // supported locale. Acceptance charges/heals without creating discovery.
    RandoSettings zero_paid_settings = full_settings;
    zero_paid_settings.hint_tile_coverage = kHintTileCoverage_Full;
    zero_paid_settings.hint_paid_depth = 0;
    zero_paid_settings.hint_mix = kHintMix_Variety;
    static RandoHintPlan zero_paid_plan;
    RandoHintPersistedState zero_paid_identity;
    if (!Rando_BuildHintPlan(
            &zero_paid_settings, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &zero_paid_plan) ||
        zero_paid_plan.fact_count != kHintTileCount ||
        !Rando_HintPlanExportPersistedState(
            &zero_paid_plan, &zero_paid_identity))
      hints_selfcheck_die("paid-zero plan identity");
    Rando_HintsImportPersistedState(&zero_paid_identity);
    if (!Rando_RebuildHints(
            &zero_paid_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() != kRandoHintPlanStatus_Ready)
      hints_selfcheck_die("paid-zero plan rebuild");
    for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++)
      if (Rando_HintsPaidHasClue((RandoHintPaidSource)source))
        hints_selfcheck_die("paid-zero exposed a clue");

    const struct {
      const char *language;
      uint8 flags;
      const char *no_clue;
      const char *healing;
      const char *price;
      const char *yes;
      const char *no;
    } paid_zero_locales[] = {
      {NULL, 0, "No clue", "Healing only", "20 rupees", "Yes", "No thanks"},
      {"de", 3, "Kein Hinweis", "Nur Heilung", "20 Rubine", "Ja", "Nein"},
      {"fr", 3, "Aucun indice", "Soins seuls", "20 rubis", "Oui", "Non"},
    };
    for (size_t locale = 0;
         locale < sizeof(paid_zero_locales) /
                      sizeof(paid_zero_locales[0]);
         locale++) {
      g_config.language = paid_zero_locales[locale].language;
      g_zenv.dialogue_flags = paid_zero_locales[locale].flags;
      uint8 prompt_a[256], prompt_b[256];
      memset(prompt_a, 0x55, sizeof(prompt_a));
      memset(prompt_b, 0x55, sizeof(prompt_b));
      Rando_HintsSetPaidPromptContext(
          kRandoHintPaidSource_Storyteller, 20);
      if (!Rando_RewriteInteractiveHintMessage(0xFEu, prompt_a) ||
          !Rando_RewriteInteractiveHintMessage(0xFEu, prompt_b) ||
          memcmp(prompt_a, prompt_b, sizeof(prompt_a)) != 0 ||
          !encoded_hint_contains(
              prompt_a, paid_zero_locales[locale].no_clue) ||
          !encoded_hint_contains(
              prompt_a, paid_zero_locales[locale].healing) ||
          !encoded_hint_contains(
              prompt_a, paid_zero_locales[locale].price) ||
          !encoded_hint_has_cursor_safe_choice(
              prompt_a, paid_zero_locales[locale].yes,
              paid_zero_locales[locale].no))
        hints_selfcheck_die("localized paid-zero disclosure");
      bool choice = false;
      int page_waits = 0, page_scrolls = 0, unsafe_top_jumps = 0;
      for (int i = 0; i < 239 && prompt_a[i] != kHintFontCmdEnd; i++) {
        if (paid_zero_locales[locale].flags == 0) {
          choice |= prompt_a[i] == kHintFontCmdChoose;
          page_waits += prompt_a[i] == kHintFontCmdWaitkey;
          page_scrolls += prompt_a[i] == kHintFontCmdScroll;
          unsafe_top_jumps += prompt_a[i] == kHintFontCmdLine0;
        } else if (
            prompt_a[i] == kHintFontCmdEuRest &&
            prompt_a[i + 1] == kHintFontCmdEuChoose) {
          choice = true;
          break;
        } else {
          page_waits += prompt_a[i] == kHintFontCmdEuWaitkey;
          page_scrolls += prompt_a[i] == kHintFontCmdEuScroll;
          unsafe_top_jumps += prompt_a[i] == kHintFontCmdEuLine0;
        }
      }
      if (!choice || page_waits != 1 || page_scrolls != 3 ||
          unsafe_top_jumps != 0)
        hints_selfcheck_die("paid-zero choice grammar/page clear");
      Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);
    }
    g_config.language = NULL;
    g_zenv.dialogue_flags = 0;
    Rando_HintsSetPaidPromptContext(
        kRandoHintPaidSource_FortuneLight, 30);
    if (!Rando_RewriteInteractiveHintMessage(0xF3u, encoded) ||
        !encoded_hint_contains(encoded, "30 rupees"))
      hints_selfcheck_die("Fortune Light paid-zero exact price");
    Rando_HintsCancelPaid(kRandoHintPaidSource_FortuneLight);
    Rando_HintsSetPaidPromptContext(
        kRandoHintPaidSource_FortuneDark, 10);
    if (!Rando_RewriteInteractiveHintMessage(0xF3u, encoded) ||
        !encoded_hint_contains(encoded, "10 rupees"))
      hints_selfcheck_die("Fortune Dark paid-zero exact price");
    Rando_HintsCancelPaid(kRandoHintPaidSource_FortuneDark);

    link_rupees_goal = 19;
    if (DarkWorldHintNPC_HandlePayment() ||
        link_rupees_goal != 19 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("paid-zero insufficient funds");
    link_rupees_goal = 100;
    sprite_ai_state[0] = 2;
    link_hearts_filler = 0;
    if (!DarkWorldHintNPC_HandlePayment() ||
        link_rupees_goal != 80 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("paid-zero Storyteller charge");
    DarkWorldHintNPC_RestoreHealth(0);
    if (link_hearts_filler != 0xA0 || sprite_ai_state[0] != 0 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("paid-zero Storyteller healing");

    link_rupees_goal = 100;
    sprite_ai_state[0] = 6;
    sprite_A[0] = 0;
    link_hearts_filler = 0;
    flag_is_link_immobilized = 1;
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneLight))
      hints_selfcheck_die("paid-zero Fortune prepare");
    FortuneTeller_LightOrDarkWorld(0, false);
    if (link_rupees_goal != 90 || sprite_ai_state[0] != 7 ||
        link_hearts_filler != 160 || flag_is_link_immobilized != 0 ||
        Rando_GetHintDiscoveredCount() != 0)
      hints_selfcheck_die("paid-zero Fortune charge/healing");

    // Restore the rich current plan for the discovery/queue tests below.
    Rando_HintsImportPersistedState(&certified);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres))
      hints_selfcheck_die("restore plan after paid-zero tests");

    link_rupees_goal = saved_rupees;
    sprite_ai_state[0] = saved_sprite_state;
    sprite_A[0] = saved_sprite_a;
    link_hearts_filler = saved_hearts_filler;
    flag_is_link_immobilized = saved_immobilized;

    if (Rando_GetHintDiscoveredCount() != 0 ||
        !Rando_RenderHintMessage(kHintTileMsgIds[0], encoded) ||
        Rando_GetHintDiscoveredCount() != 1)
      hints_selfcheck_die("tile discovery after complete render");
    if (!Rando_RenderHintMessage(kHintTileMsgIds[0], encoded) ||
        Rando_GetHintDiscoveredCount() != 1)
      hints_selfcheck_die("tile discovery idempotence");

    RandoHintPersistedState one_discovered;
    if (!Rando_HintsExportPersistedState(&one_discovered))
      hints_selfcheck_die("live discovery export");
    Rando_HintsImportPersistedState(&one_discovered);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintDiscoveredCount() != 1)
      hints_selfcheck_die("discovery rebuild round-trip");

    uint8 before_paid = Rando_GetHintDiscoveredCount();
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller))
      hints_selfcheck_die("Storyteller abandoned prepare");
    uint8 abandoned_fact =
        g_hint_paid_pending[kRandoHintPaidSource_Storyteller].fact_id;
    // Replay/reload invalidates the epoch-bound presentation. A stale receipt
    // cannot commit a newly selected clue; the owning NPC handler refunds.
    Rando_HintsImportPersistedState(&one_discovered);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller) ||
        Rando_GetHintDiscoveredCount() != before_paid ||
        Rando_IsHintFactDiscovered(abandoned_fact))
      hints_selfcheck_die("lost paid latch must fail without discovery");

    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller))
      hints_selfcheck_die("Storyteller prepare");
    uint8 story_first =
        g_hint_paid_pending[kRandoHintPaidSource_Storyteller].fact_id;
    uint8 prepared_len =
        g_hint_paid_pending[kRandoHintPaidSource_Storyteller].encoded_len;
    uint8 prepared[256];
    memcpy(prepared,
           g_hint_paid_pending[kRandoHintPaidSource_Storyteller].encoded,
           prepared_len);
    if (!Rando_RenderHintMessage(0xFFu, encoded) ||
        memcmp(encoded, prepared, prepared_len) != 0 ||
        Rando_GetHintDiscoveredCount() != before_paid)
      hints_selfcheck_die("paid prepare/render must not discover");
    if (!Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller) ||
        !Rando_IsHintFactDiscovered(story_first) ||
        Rando_GetHintDiscoveredCount() != (uint8)(before_paid + 1) ||
        !Rando_RenderHintMessage(0xFFu, encoded) ||
        memcmp(encoded, prepared, prepared_len) != 0)
      hints_selfcheck_die("paid commit/presentation latch");
    Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);

    uint8 story_skip =
        g_hint_plan.paid_queue[kRandoHintPaidSource_Storyteller][1];
    uint8 story_after_skip =
        g_hint_plan.paid_queue[kRandoHintPaidSource_Storyteller][2];
    const RandoHintFact *skip_fact = Rando_GetHintFact(story_skip);
    if (skip_fact == NULL || skip_fact->location_id == 0xFFFFu)
      hints_selfcheck_die("paid reserve must be exact");
    uint8 *checked_byte =
        &g_rando_checked_bitmap[skip_fact->location_id >> 3];
    uint8 saved_checked_byte = *checked_byte;
    *checked_byte |= (uint8)(1u << (skip_fact->location_id & 7));
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller) ||
        g_hint_paid_pending[kRandoHintPaidSource_Storyteller].fact_id !=
            story_after_skip ||
        !Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller) ||
        Rando_IsHintFactDiscovered(story_skip))
      hints_selfcheck_die("paid checked-target skip");
    Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);
    uint8 before_exhausted = Rando_GetHintDiscoveredCount();
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller) ||
        !g_hint_paid_pending[kRandoHintPaidSource_Storyteller].exhausted ||
        !Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller) ||
        Rando_GetHintDiscoveredCount() != before_exhausted ||
        !Rando_RenderHintMessage(0xFFu, encoded) ||
        !encoded_hint_contains(encoded, "No more clues"))
      hints_selfcheck_die("paid exhausted service");
    Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);
    *checked_byte = saved_checked_byte;

    // Kakariko and Lake share the Light queue; Dark starts independently.
    savegame_is_darkworld = 0;
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneLight))
      hints_selfcheck_die("Fortune Light prepare");
    uint8 light_first =
        g_hint_paid_pending[kRandoHintPaidSource_FortuneLight].fact_id;
    if (light_first !=
            g_hint_plan.paid_queue[kRandoHintPaidSource_FortuneLight][0] ||
        !Rando_RenderHintMessage(0xEAu, encoded) ||
        !Rando_HintsCommitPaid(kRandoHintPaidSource_FortuneLight))
      hints_selfcheck_die("Fortune Light first transaction");
    Rando_HintsCancelPaid(kRandoHintPaidSource_FortuneLight);
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneLight) ||
        g_hint_paid_pending[kRandoHintPaidSource_FortuneLight].fact_id !=
            g_hint_plan.paid_queue[kRandoHintPaidSource_FortuneLight][1])
      hints_selfcheck_die("Fortune Light shared progression");
    Rando_HintsCancelPaid(kRandoHintPaidSource_FortuneLight);
    savegame_is_darkworld = 0x40;
    if (!Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneDark) ||
        g_hint_paid_pending[kRandoHintPaidSource_FortuneDark].fact_id !=
            g_hint_plan.paid_queue[kRandoHintPaidSource_FortuneDark][0] ||
        !Rando_RenderHintMessage(0xEAu, encoded))
      hints_selfcheck_die("Fortune Dark independent queue");
    Rando_HintsCancelPaid(kRandoHintPaidSource_FortuneDark);

    RandoHintPersistedState bad = certified;
    bad.plan_digest[0] ^= 0x80u;
    Rando_HintsImportPersistedState(&bad);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() != kRandoHintPlanStatus_DigestMismatch ||
        Rando_GetHintFactCount() != 0)
      hints_selfcheck_die("digest mismatch fail closed");
    Rando_HintsImportPersistedState(NULL);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() != kRandoHintPlanStatus_MissingMetadata)
      hints_selfcheck_die("missing metadata fail closed");
    bad = certified;
    bad.algorithm_version++;
    Rando_HintsImportPersistedState(&bad);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() !=
            kRandoHintPlanStatus_UnsupportedAlgorithm)
      hints_selfcheck_die("unsupported algorithm fail closed");
    bad = certified;
    bad.text_schema_version++;
    Rando_HintsImportPersistedState(&bad);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() !=
            kRandoHintPlanStatus_UnsupportedTextSchema)
      hints_selfcheck_die("unsupported text schema fail closed");
    bad = certified;
    bad.algorithm_version = kRandoHintPlanLegacyAlgorithmVersion;
    bad.text_schema_version = kRandoHintTextSchemaVersion;
    Rando_HintsImportPersistedState(&bad);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() !=
            kRandoHintPlanStatus_UnsupportedAlgorithm ||
        Rando_GetHintFactCount() != 0 ||
        Rando_BuildHintPlan(
            &full_settings, &full_table, &full_spheres,
            kRandoHintPlanLegacyAlgorithmVersion,
            kRandoHintTextSchemaVersion, &digest_probe))
      hints_selfcheck_die("crossed 1/2 pair did not fail closed");
    bad = certified;
    bad.algorithm_version = kRandoHintPlanAlgorithmVersion;
    bad.text_schema_version = kRandoHintLegacyTextSchemaVersion;
    Rando_HintsImportPersistedState(&bad);
    if (!Rando_RebuildHints(&full_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() !=
            kRandoHintPlanStatus_UnsupportedTextSchema ||
        Rando_GetHintFactCount() != 0 ||
        Rando_BuildHintPlan(
            &full_settings, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion,
            kRandoHintLegacyTextSchemaVersion, &digest_probe))
      hints_selfcheck_die("crossed 2/1 pair did not fail closed");

    RandoSettings off_settings = full_settings;
    off_settings.hints = kHintsMode_Off;
    if (!Rando_BuildHintPlan(
            &off_settings, &full_table, &full_spheres,
            kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
            &off_plan) ||
        off_plan.fact_count != 0 ||
        !Rando_HintPlanExportPersistedState(&off_plan, &certified))
      hints_selfcheck_die("certified empty Off plan");
    Rando_HintsImportPersistedState(&certified);
    if (!Rando_RebuildHints(&off_settings, &full_table, &full_spheres) ||
        Rando_GetHintPlanStatus() != kRandoHintPlanStatus_Off)
      hints_selfcheck_die("Off plan rebuild");
    const struct {
      const char *language;
      uint8 flags;
      const char *needle;
    } neutral_locales[] = {
      {NULL, 0, "Hints disabled"},
      {"de", 3, "Hinweise"},
      {"fr", 3, "Indices"},
    };
    for (size_t i = 0; i < sizeof(neutral_locales) /
                             sizeof(neutral_locales[0]); i++) {
      g_config.language = neutral_locales[i].language;
      g_zenv.dialogue_flags = neutral_locales[i].flags;
      if (!Rando_RenderHintMessage(kHintTileMsgIds[0], encoded) ||
          !encoded_hint_contains(encoded, neutral_locales[i].needle))
        hints_selfcheck_die("localized neutral fallback");
    }

    g_config.language = saved_language;
    savegame_is_darkworld = saved_dark_world;
    g_zenv.dialogue_flags = saved_dialogue_flags;
    g_rando_slot_active = saved_slot_active;
    Rando_ClearHints();
  }

  // Vanilla-dialogue redirects use a pure context resolver so every fallback
  // gate, direction, discriminator, and placement policy can be covered
  // without fabricating a live sidecar slot. Library's Bottle is deliberately
  // adjacent to a Book at Sick Kid: the lookup must reverse-search for Book,
  // never inspect Library.
  {
    static RandoPlacement redirect_entries[] = {
      {LOC_Library, ITEM_BottleEmpty},
      {LOC_Sick_Kid, ITEM_BookOfMudora},
      {LOC_Ice_Palace_Prize, ITEM_MoonPearl},
      {LOC_Magic_Bat, ITEM_Prize_GreenPendant},
      {LOC_Bumper_Cave, ITEM_Hookshot},
      {LOC_Desert_Ledge, ITEM_OcarinaInactive},
      {LOC_Maze_Race, ITEM_PieceOfHeart},
    };
    RandoPlacementTable redirect_table = {
      redirect_entries,
      (uint16)(sizeof(redirect_entries) / sizeof(redirect_entries[0])),
    };
    RandoSettings on;
    Settings_SetDefaults(&on);
    on.hints = kHintsMode_On;

    RandoDialogueHintResolution book = resolve_dialogue_hint_for_context(
        0x125, true, 0, &on, &redirect_table, false, 0);
    if (book.status != kDialogueHintRedirect_Active ||
        book.item_id != ITEM_BookOfMudora || book.location_id != LOC_Sick_Kid)
      hints_selfcheck_die("Aginah must resolve Book at Sick Kid, not Library contents");
    uint8 encoded[241];
    bool used_fallback = false;
    if (!encode_dialogue_hint(&book, encoded, &used_fallback) || used_fallback ||
        !encoded_hint_contains(encoded, "Book") ||
        !encoded_hint_contains(encoded, "Sick Kid"))
      hints_selfcheck_die("Aginah Book redirect text");

    const uint16 pearl_msgs[] = {0x15D, 0x09E, 0x036};
    for (size_t i = 0; i < sizeof(pearl_msgs) / sizeof(pearl_msgs[0]); i++) {
      RandoDialogueHintResolution pearl = resolve_dialogue_hint_for_context(
          pearl_msgs[i], true, 0, &on, &redirect_table, false, 0);
      if (pearl.status != kDialogueHintRedirect_Active ||
          pearl.location_id != LOC_Ice_Palace_Prize)
        hints_selfcheck_die("Moon Pearl redirect mapping");
      if (!encode_dialogue_hint(&pearl, encoded, &used_fallback) || used_fallback ||
          !encoded_hint_contains(encoded, "Moon Pearl") ||
          !encoded_hint_contains(encoded, "Ice Palace"))
        hints_selfcheck_die("Moon Pearl redirect text");
    }

    RandoDialogueHintResolution pendant = resolve_dialogue_hint_for_context(
        0x033, true, 0, &on, &redirect_table, false, 0);
    if (pendant.status != kDialogueHintRedirect_Active ||
        pendant.location_id != LOC_Magic_Bat)
      hints_selfcheck_die("Sahasrahla Green Pendant redirect mapping");

    // Bumper Cave is the inverse direction: the physical sign identifies the
    // location, so resolve that row's item. The unrelated Heart Piece at Maze
    // Race must not affect the answer.
    RandoDialogueHintResolution bumper = resolve_dialogue_hint_for_context(
        0x0A8, true, 0, &on, &redirect_table, false, 0x4A);
    if (bumper.status != kDialogueHintRedirect_Active ||
        bumper.location_id != LOC_Bumper_Cave || bumper.item_id != ITEM_Hookshot)
      hints_selfcheck_die("Bumper sign must resolve the item at Bumper Cave");
    if (!encode_dialogue_hint(&bumper, encoded, &used_fallback) || used_fallback ||
        !encoded_hint_contains(encoded, "Hookshot"))
      hints_selfcheck_die("Bumper sign redirect text");
    if (resolve_dialogue_hint_for_context(
            0x0A8, true, 0, &on, &redirect_table, false, 0x49).status !=
        kDialogueHintRedirect_DiscriminatorMismatch)
      hints_selfcheck_die("Bumper sign wrong-screen discriminator");
    if (resolve_dialogue_hint_for_context(
            0x0A8, true, 0, &on, &redirect_table, true, 0x4A).status !=
        kDialogueHintRedirect_DiscriminatorMismatch)
      hints_selfcheck_die("Bumper sign indoor discriminator");

    // Stumpy retains a live choice page after naming the Flute's actual
    // placement. The separate LOC_Stumpy reward remains owned by 0xE6.
    RandoDialogueHintResolution stumpy = resolve_dialogue_hint_for_context(
        0x0E5, true, 0, &on, &redirect_table, false, 0);
    if (stumpy.status != kDialogueHintRedirect_Active ||
        stumpy.item_id != ITEM_OcarinaInactive ||
        stumpy.location_id != LOC_Desert_Ledge)
      hints_selfcheck_die("Stumpy Flute redirect mapping");
    if (!encode_interactive_dialogue_hint(&stumpy, encoded) ||
        !encoded_hint_contains(encoded, "Flute") ||
        !encoded_hint_contains(encoded, "Desert Ledge") ||
        !encoded_hint_has_cursor_safe_choice(encoded, "Yes", "No way"))
      hints_selfcheck_die("Stumpy interactive hint text");
    bool has_choose = false;
    int page_waits = 0, page_scrolls = 0, unsafe_top_jumps = 0;
    for (int i = 0; i < 240 && encoded[i] != kHintFontCmdEnd; i++) {
      if (encoded[i] == kHintFontCmdChoose) has_choose = true;
      page_waits += encoded[i] == kHintFontCmdWaitkey;
      page_scrolls += encoded[i] == kHintFontCmdScroll;
      unsafe_top_jumps += encoded[i] == kHintFontCmdLine0;
    }
    if (!has_choose || page_waits != 1 || page_scrolls != 3 ||
        unsafe_top_jumps != 0) {
      fprintf(stderr,
              "Hints_SelfCheck: Stumpy page roll choose=%d waits=%d "
              "scrolls=%d top_jumps=%d flags=%u\n",
              has_choose, page_waits, page_scrolls, unsafe_top_jumps,
              (unsigned)g_zenv.dialogue_flags);
      hints_selfcheck_die("Stumpy interactive hint lost choice/page commands");
    }

    // A future metadata/render regression must fail to truthful neutral text,
    // never release the seed-invalid vanilla claim (and never lose Stumpy's
    // live choice grammar). Invalid typed ids force the rich encoders to fail
    // without depending on today's generated fit set.
    RandoDialogueHintResolution invalid_book = book;
    invalid_book.item_id = ITEM__COUNT;
    memset(encoded, 0x55, sizeof(encoded));
    if (!hint_render_dialogue_or_unavailable(&invalid_book, encoded) ||
        !encoded_hint_contains(encoded, "Hints unavailable"))
      hints_selfcheck_die("redirect rich-render neutral fallback");
    RandoDialogueHintResolution invalid_stumpy = stumpy;
    invalid_stumpy.item_id = ITEM__COUNT;
    memset(encoded, 0x55, sizeof(encoded));
    if (!hint_render_interactive_or_unavailable(&invalid_stumpy, encoded) ||
        !encoded_hint_contains(encoded, "Hints unavailable") ||
        !encoded_hint_has_cursor_safe_choice(encoded, "Yes", "No way"))
      hints_selfcheck_die("interactive rich-render neutral fallback");
    has_choose = false;
    for (int i = 0; i < 240 && encoded[i] != kHintFontCmdEnd; i++)
      if (encoded[i] == kHintFontCmdChoose) has_choose = true;
    if (!has_choose)
      hints_selfcheck_die("interactive neutral fallback lost choice");

    RandoSettings off = on;
    off.hints = kHintsMode_Off;
    if (resolve_dialogue_hint_for_context(
            0x125, true, 0, &off, &redirect_table, false, 0).status !=
        kDialogueHintRedirect_HintsOff)
      hints_selfcheck_die("redirect hints-off fallback");
    if (resolve_dialogue_hint_for_context(
            0x0A8, true, 0, &off, &redirect_table, false, 0x49).status !=
        kDialogueHintRedirect_DiscriminatorMismatch)
      hints_selfcheck_die("Bumper hints-off mismatch must remain vanilla");
    if (resolve_dialogue_hint_for_context(
            0x125, false, 0, &on, &redirect_table, false, 0).status !=
        kDialogueHintRedirect_SlotInactive)
      hints_selfcheck_die("redirect inactive-slot fallback");
    if (resolve_dialogue_hint_for_context(
            0x125, true, 0, NULL, &redirect_table, false, 0).status !=
        kDialogueHintRedirect_SettingsUnavailable)
      hints_selfcheck_die("redirect missing-settings fallback");
    if (resolve_dialogue_hint_for_context(
            0x0A8, true, 0, NULL, &redirect_table, true, 0x4A).status !=
        kDialogueHintRedirect_DiscriminatorMismatch)
      hints_selfcheck_die("Bumper unavailable mismatch must remain vanilla");
    for (uint8 flags = 1; flags <= 3; flags++) {
      if (resolve_dialogue_hint_for_context(
              0x125, true, flags, &on, &redirect_table, false, 0).status !=
          kDialogueHintRedirect_UnsupportedLocale)
        hints_selfcheck_die("redirect translated locale must be neutral");
    }
    if (resolve_dialogue_hint_for_context(
            0x125, true, 0, &on, NULL, false, 0).status !=
        kDialogueHintRedirect_PlacementUnavailable)
      hints_selfcheck_die("redirect missing-placement fallback");

    RandoPlacement no_target_entry = {LOC_Library, ITEM_BottleEmpty};
    RandoPlacementTable no_target = {&no_target_entry, 1};
    if (resolve_dialogue_hint_for_context(
            0x125, true, 0, &on, &no_target, false, 0).status !=
        kDialogueHintRedirect_ItemAbsent)
      hints_selfcheck_die("redirect missing-item fallback");
    if (resolve_dialogue_hint_for_context(
            0x0A8, true, 0, &on, &no_target, false, 0x4A).status !=
        kDialogueHintRedirect_LocationAbsent)
      hints_selfcheck_die("redirect missing-location fallback");

    const uint16 adjacent[] = {
      0x032, 0x034, 0x035, 0x037, 0x09D, 0x09F, 0x0A7, 0x0A9,
      0x0E4, 0x0E6, 0x124, 0x126, 0x15C, 0x15E,
    };
    for (size_t i = 0; i < sizeof(adjacent) / sizeof(adjacent[0]); i++) {
      if (resolve_dialogue_hint_for_context(
              adjacent[i], true, 0, &on, &redirect_table, false, 0).status !=
          kDialogueHintRedirect_NotRecognized)
        hints_selfcheck_die("adjacent dialogue ID was intercepted");
    }

    RandoDialogueHintRedirect flagged = {
      0x777, ITEM_MoonPearl, 0xFFFFu, "test", 0x80,
      kDialogueHintRedirect_ItemLocation,
    };
    if (dialogue_hint_redirect_discriminator_matches(&flagged, false, 0))
      hints_selfcheck_die("unknown redirect discriminator flags must fail closed");

    // Duplicate target policy is lowest location ID even when the rows arrive in
    // descending order.
    RandoPlacement duplicate_entries[] = {
      {LOC_Library, ITEM_MoonPearl},
      {LOC_Sick_Kid, ITEM_MoonPearl},
    };
    RandoPlacementTable duplicates = {duplicate_entries, 2};
    if (resolve_dialogue_hint_for_context(
            0x15D, true, 0, &on, &duplicates, false, 0).location_id !=
        LOC_Sick_Kid)
      hints_selfcheck_die("duplicate item policy must choose lowest location ID");

    // F12 uses this exact formatter. Pin the identifying source, target, status,
    // and resolved location rather than merely testing the resolver fields.
    FILE *diag = tmpfile();
    if (diag == NULL) hints_selfcheck_die("tmpfile for redirect diagnostic");
    dump_dialogue_hint_resolution(diag, &book);
    rewind(diag);
    char diag_text[512];
    size_t diag_len = fread(diag_text, 1, sizeof(diag_text) - 1, diag);
    fclose(diag);
    diag_text[diag_len] = '\0';
    if (strstr(diag_text, "source=Aginah") == NULL ||
        strstr(diag_text, "target_item=BookOfMudora") == NULL ||
        strstr(diag_text, "status=active") == NULL ||
        strstr(diag_text, "resolved_location=Sick Kid") == NULL)
      hints_selfcheck_die("redirect diagnostic content");

    diag = tmpfile();
    if (diag == NULL) hints_selfcheck_die("tmpfile for Bumper diagnostic");
    dump_dialogue_hint_resolution(diag, &bumper);
    rewind(diag);
    diag_len = fread(diag_text, 1, sizeof(diag_text) - 1, diag);
    fclose(diag);
    diag_text[diag_len] = '\0';
    if (strstr(diag_text, "source=Bumper Cave sign") == NULL ||
        strstr(diag_text, "surface=location-item sign") == NULL ||
        strstr(diag_text, "target_item=Hookshot") == NULL ||
        strstr(diag_text, "resolved_location=Bumper Cave") == NULL)
      hints_selfcheck_die("Bumper redirect diagnostic content");

    // The 15 tile IDs and existing fork mappings are independent of the new
    // redirect table. This catches an accidental overlap or range broadening.
    static const uint16 expected_tiles[kHintTileCount] = {
      0xB4, 0xB5, 0xB8, 0xB9, 0xBA, 0xBB, 0xBE, 0xBF,
      0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
    };
    if (memcmp(expected_tiles, kHintTileMsgIds, sizeof(expected_tiles)) != 0)
      hints_selfcheck_die("telepathic tile ID mapping drift");
    for (int i = 0; i < kHintTileCount; i++) {
      if (hint_npc_for_msg(expected_tiles[i]) !=
          (RandoHintNpc)(kRandoHintNpc_TeleEasternPalace + i))
        hints_selfcheck_die("telepathic tile NPC mapping drift");
    }
    if (hint_npc_for_msg(0xFF) != kRandoHintNpc_ForkStoryteller ||
        hint_npc_for_msg(0xEF) == kRandoHintNpc_None)
      hints_selfcheck_die("fork NPC mapping drift");

    // Rich facts are original/US only. Every nonzero dialogue format renders a
    // localized neutral message without discovering or advancing a paid fact.
    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_dialogue_flags = g_zenv.dialogue_flags;
    const char *saved_language = g_config.language;
    g_rando_slot_active = 1;
    if (!Rando_GenerateHints(&on, &table, NULL))
      hints_selfcheck_die("generate hints for locale fallback");

    // Force an assigned tile's rich text past the three-row envelope. The
    // public render path must install neutral text and leave discovery alone.
    RandoHintNpc failed_npc = kRandoHintNpc_None;
    for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
         npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
      if (!g_hint_table[npc].active ||
          g_hint_table[npc].fact_id >= g_hint_plan.fact_count)
        continue;
      failed_npc = (RandoHintNpc)npc;
      break;
    }
    if (failed_npc == kRandoHintNpc_None)
      hints_selfcheck_die("tile render-failure fixture missing assignment");
    HintEntry *failed_tile = &g_hint_table[failed_npc];
    uint16 failed_tile_msg =
        kHintTileMsgIds[failed_npc - kRandoHintNpc_TeleEasternPalace];
    char saved_game_text[sizeof(failed_tile->game_text)];
    uint8 saved_discovery[sizeof(g_hint_discovered)];
    uint8 expected_discovery[sizeof(g_hint_discovered)];
    memcpy(saved_game_text, failed_tile->game_text, sizeof(saved_game_text));
    memcpy(saved_discovery, g_hint_discovered, sizeof(saved_discovery));
    g_hint_discovered[failed_tile->fact_id >> 3] &=
        (uint8)~(1u << (failed_tile->fact_id & 7));
    memcpy(expected_discovery, g_hint_discovered, sizeof(expected_discovery));
    memset(failed_tile->game_text, 'X', sizeof(failed_tile->game_text) - 1);
    failed_tile->game_text[sizeof(failed_tile->game_text) - 1] = '\0';
    g_config.language = NULL;
    g_zenv.dialogue_flags = 0;
    memset(encoded, 0x55, sizeof(encoded));
    if (!Rando_RenderHintMessage(failed_tile_msg, encoded) ||
        !encoded_hint_contains(encoded, "Hints unavailable") ||
        memcmp(g_hint_discovered, expected_discovery,
               sizeof(expected_discovery)) != 0)
      hints_selfcheck_die("tile rich-render neutral/non-discovery fallback");
    memcpy(failed_tile->game_text, saved_game_text, sizeof(saved_game_text));
    memcpy(g_hint_discovered, saved_discovery, sizeof(saved_discovery));

    const struct {
      const char *language;
      uint8 flags;
      const char *needle;
    } translated_locales[] = {
      {"de", 1, "Hinweise"},
      {"fr", 2, "Indices"},
      {"fr", 3, "Indices"},
    };
    for (size_t lf = 0; lf < sizeof(translated_locales) /
                                  sizeof(translated_locales[0]); lf++) {
      g_config.language = translated_locales[lf].language;
      g_zenv.dialogue_flags = translated_locales[lf].flags;
      uint8 before = Rando_GetHintDiscoveredCount();
      memset(encoded, 0x55, sizeof(encoded));
      if (!Rando_RenderHintMessage(0xB4, encoded) ||
          !encoded_hint_contains(encoded, translated_locales[lf].needle) ||
          Rando_GetHintDiscoveredCount() != before ||
          !Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller) ||
          !g_hint_paid_pending[kRandoHintPaidSource_Storyteller].exhausted ||
          !Rando_RenderHintMessage(0xFFu, encoded) ||
          !encoded_hint_contains(encoded, translated_locales[lf].needle) ||
          !Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller) ||
          Rando_GetHintDiscoveredCount() != before)
        hints_selfcheck_die("translated locale neutral/discovery contract");
      Rando_HintsCancelPaid(kRandoHintPaidSource_Storyteller);
    }
    g_config.language = "de";
    g_zenv.dialogue_flags = 3;
    if (!encode_interactive_neutral(
            hint_neutral_text(kHintNeutral_Unavailable), encoded)) {
      hints_selfcheck_die("translated interactive neutral render");
    }
    bool has_eu_choice_tail = false;
    for (int i = 0; i < 239 && encoded[i] != kHintFontCmdEnd; i++) {
      if (encoded[i] == kHintFontCmdEuRest &&
          encoded[i + 1] == kHintFontCmdEuChoose) {
        has_eu_choice_tail = true;
        break;
      }
    }
    if (!has_eu_choice_tail)
      hints_selfcheck_die("translated interactive choice grammar");
    g_rando_slot_active = saved_slot_active;
    g_zenv.dialogue_flags = saved_dialogue_flags;
    g_config.language = saved_language;

    // Every current registry location must retain a readable name without the
    // numeric forward-compatibility fallback for each item->location surface.
    for (size_t r = 0; r < sizeof(kDialogueHintRedirects) /
                               sizeof(kDialogueHintRedirects[0]); r++) {
      if (kDialogueHintRedirects[r].kind == kDialogueHintRedirect_LocationItem)
        continue;
      RandoDialogueHintResolution format = {
        &kDialogueHintRedirects[r], kDialogueHintRedirect_Active,
        kDialogueHintRedirects[r].item_id, 0xFFFFu,
      };
      for (uint32 i = 0; i < kRandoLocationsCount; i++) {
        format.location_id = kRandoLocations[i].id;
        used_fallback = false;
        if (!encode_dialogue_hint(&format, encoded, &used_fallback) ||
            used_fallback) {
          fprintf(stderr,
                  "Hints_SelfCheck: dynamic hint format overflow item=%s loc=%u (%s)\n",
                  Rando_GetItemName(format.item_id),
                  (unsigned)format.location_id,
                  Rando_GetLocationName(format.location_id));
          abort();
        }
      }
    }

    // Bumper Cave's location->item direction can surface any registry item.
    // Assert that the useful item name always survives the three-row envelope.
    RandoDialogueHintResolution bumper_format = {
      dialogue_hint_redirect_for_msg(0x0A8), kDialogueHintRedirect_Active,
      0xFFFFu, LOC_Bumper_Cave,
    };
    for (uint16 item = 0; item < ITEM__COUNT; item++) {
      bumper_format.item_id = item;
      if (!encode_dialogue_hint(&bumper_format, encoded, NULL)) {
        fprintf(stderr,
                "Hints_SelfCheck: Bumper item format overflow item=%u (%s)\n",
                (unsigned)item, Rando_GetItemName(item));
        abort();
      }
    }

    // Every dungeon-item family must remain semantically distinguishable on
    // exact facts and location->item signs through generated qualified names.
    char eastern_key[64], pod_key[64], pod_big_key[64], eastern_map[64],
         pod_compass[64], pod_key_ring[64];
    if (!location_hint_item_name(ITEM_SmallKey_EasternPalace, eastern_key,
                                 sizeof(eastern_key)) ||
        !location_hint_item_name(ITEM_SmallKey_PalaceOfDarkness, pod_key,
                                 sizeof(pod_key)) ||
        !location_hint_item_name(ITEM_BigKey_PalaceOfDarkness, pod_big_key,
                                 sizeof(pod_big_key)) ||
        !location_hint_item_name(ITEM_Map_EasternPalace, eastern_map,
                                 sizeof(eastern_map)) ||
        !location_hint_item_name(ITEM_Compass_PalaceOfDarkness, pod_compass,
                                 sizeof(pod_compass)) ||
        !location_hint_item_name(ITEM_KeyRing_PalaceOfDarkness, pod_key_ring,
                                 sizeof(pod_key_ring)) ||
        strcmp(eastern_key, "Eastern Small Key") != 0 ||
        strcmp(pod_key, "PoD Small Key") != 0 ||
        strcmp(pod_big_key, "PoD Big Key") != 0 ||
        strcmp(eastern_map, "Eastern Map") != 0 ||
        strcmp(pod_compass, "PoD Compass") != 0 ||
        strcmp(pod_key_ring, "PoD Key Ring") != 0 ||
        strcmp(eastern_key, pod_key) == 0)
      hints_selfcheck_die(
          "generated dungeon-item names must retain dungeon identity");

    bumper_format.item_id = ITEM_SmallKey_PalaceOfDarkness;
    if (!encode_dialogue_hint(&bumper_format, encoded, NULL) ||
        !encoded_hint_contains(encoded, "PoD Small Key"))
      hints_selfcheck_die("Bumper qualified dungeon-item text");
  }

  // kHintsMode_Off: must populate nothing.
  Rando_ClearHints();
  settings.hints = kHintsMode_Off;
  ok = Rando_GenerateHints(&settings, &table, NULL);
  if (!ok) {
    fprintf(stderr, "Hints_SelfCheck: hints=Off branch failed.\n");
    abort();
  }
  for (int i = 0; i < kRandoHintNpc__Count; i++) {
    if (g_hint_table[i].active) {
      fprintf(stderr, "Hints_SelfCheck: hints=Off populated entry %d.\n", i);
      abort();
    }
  }

  // NULL args: defensive reject.
  if (Rando_GenerateHints(NULL, &table, NULL)) {
    fprintf(stderr, "Hints_SelfCheck: NULL settings should fail.\n");
    abort();
  }
  if (Rando_GenerateHints(&settings, NULL, NULL)) {
    fprintf(stderr, "Hints_SelfCheck: NULL placements should fail.\n");
    abort();
  }

  Rando_ClearHints();
  fprintf(stderr, "[Hints_SelfCheck] OK\n");
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_hints)
