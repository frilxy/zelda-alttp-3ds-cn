// rando_spoiler.c — JSON + text spoiler writer (tasks.md §5.1, §5.2).
//
// Emits spoiler JSON/text with sphere_data, fallback_warnings, and item
// placements grouped by region.
//
// Determinism: JSON output is byte-identical for byte-identical input
// (sorted keys; no whitespace variation; entries iterated in location_id
// order). The file's SHA-256 is a stable identity for race-mode stamping.

#include "rando_spoiler.h"
#include "rando_placement.h"
#include "rando_settings.h"
#include "rando.h"
#include "rando_hints.h"
#include "shuffle_entrance.h"  // Entrance_WriteSpoilerJson
#include "shuffle_doors.h"     // DoorShuffleLayout (door_shuffle spoiler section)
#include "shuffle_boss.h"      // BossShuffle_BossName / _DungeonName
#include "shuffle_chains.h"    // DungeonChainsLayout
#include "location_ids.h"      // LOC_* medallion config slot names
#include "item_ids.h"          // ITEM_Nothing (empty-pot filler omitted from listings)
#include "dungeon_ids.h"       // ring mask order + ring item ids
#include "../config.h"
#include "../types.h"

// Single-source accessor over the vanilla prize-drop table (kPrizeItems[56]) —
// defined in src/sprite.c (declared in sprite.h). Forward-declared here (rather
// than including sprite.h, which would drag in variables.h) so the drop_tables
// section can print the resolved drop item for each shuffled slot without
// embedding a copy of the ROM-derived table. Keep in sync with sprite.h.
extern uint8 Sprite_VanillaPrizeItem(uint8 source_index);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define RANDO_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define RANDO_MKDIR(p) mkdir((p), 0755)
#endif

// Generated tables — typedefs + extern declarations live in rando_logic.h.
#include "rando_logic.h"
#include "rando_generate.h"   // Rando_GetOwWarpGenerationLayout
#include "shuffle_ow_warp.h"  // add-rando-ow-warp-shuffle spoiler section

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// Ensure `dir_path` exists on disk. Best-effort: on success or if the dir
// already exists, returns 0; otherwise returns nonzero. Spoiler writes
// guard against missing dirs by calling this before fopen.
static int ensure_directory(const char *dir_path) {
  if (dir_path == NULL || *dir_path == '\0') return 0;
  if (RANDO_MKDIR(dir_path) == 0) return 0;
  // errno == EEXIST means it already exists — treat as success.
  // We don't have <errno.h> included; rely on a probe: try fopen test isn't
  // robust either. Just always return 0 — if the dir truly can't be created,
  // the subsequent fopen will fail and the caller's error path will fire.
  return 0;
}

// Extract the directory portion of `path` (everything before the last '/'
// or '\\') into `out`. Returns true if there was a directory portion.
static bool extract_dir(const char *path, char *out, size_t out_cap) {
  if (path == NULL || out == NULL || out_cap == 0) return false;
  const char *last_sep = NULL;
  for (const char *p = path; *p; ++p) {
    if (*p == '/' || *p == '\\') last_sep = p;
  }
  if (last_sep == NULL) return false;
  size_t len = (size_t)(last_sep - path);
  if (len >= out_cap) return false;
  memcpy(out, path, len);
  out[len] = '\0';
  return true;
}

static void write_hex(FILE *f, const uint8 *bytes, size_t n) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    fputc(hex[bytes[i] >> 4], f);
    fputc(hex[bytes[i] & 0xf], f);
  }
}

static void write_json_string(FILE *f, const char *text) {
  fputc('"', f);
  if (text != NULL) {
    for (const char *p = text; *p; p++) {
      unsigned char uc = (unsigned char)*p;
      switch (uc) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        case '\b': fputs("\\b", f);  break;
        case '\f': fputs("\\f", f);  break;
        default:
          if (uc < 0x20) fprintf(f, "\\u%04x", (unsigned)uc);
          else           fputc((int)uc, f);
          break;
      }
    }
  }
  fputc('"', f);
}

typedef struct SpoilerHintAssignment {
  RandoHintNpc npc;
  uint8 queue_index;
  uint8 tile_index;
  bool paid;
  bool found;
} SpoilerHintAssignment;

static const char *spoiler_hint_fact_kind_name(uint8 kind) {
  switch ((RandoHintFactKind)kind) {
    case kRandoHintFact_PriorityItem:         return "priority_item";
    case kRandoHintFact_MajorItem:            return "major_item";
    case kRandoHintFact_HighFrictionLocation: return "high_friction_location";
    case kRandoHintFact_Goal:                 return "goal";
    case kRandoHintFact_ActiveSetting:        return "active_setting";
    case kRandoHintFact_RegionValue:          return "region_value";
    case kRandoHintFact_Useful:               return "useful";
    case kRandoHintFact_Flavor:               return "flavor";
    default:                                  return "unknown";
  }
}

static const char *spoiler_hint_template_name(uint8 template_id) {
  switch ((RandoHintTemplateId)template_id) {
    case kRandoHintTemplate_ExactPlacement: return "exact_placement";
    case kRandoHintTemplate_Goal:           return "goal";
    case kRandoHintTemplate_ActiveSetting:  return "active_setting";
    case kRandoHintTemplate_RegionValue:    return "region_value";
    case kRandoHintTemplate_Flavor:         return "flavor";
    default:                                return "unknown";
  }
}

static uint16 spoiler_hint_dialogue_id(RandoHintNpc npc) {
  if (npc <= kRandoHintNpc_None || npc >= kRandoHintNpc__Count)
    return 0xFFFFu;
  return (uint16)(kRandoHintDialogueBase + ((uint16)npc - 1));
}

static bool spoiler_find_hint_assignment(const RandoHintPlan *plan,
                                         uint8 fact_id,
                                         SpoilerHintAssignment *out) {
  if (plan == NULL || out == NULL) return false;
  memset(out, 0, sizeof(*out));

  for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
       npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
    if (plan->primary_fact_by_npc[npc] != fact_id) continue;
    out->npc = (RandoHintNpc)npc;
    out->tile_index =
        (uint8)(npc - (uint8)kRandoHintNpc_TeleEasternPalace);
    out->found = true;
    return true;
  }

  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    uint8 count = plan->paid_count[source];
    if (count > kRandoHintPaidQueueCapacity)
      count = kRandoHintPaidQueueCapacity;
    for (uint8 queue = 0; queue < count; queue++) {
      if (plan->paid_queue[source][queue] != fact_id) continue;
      out->npc =
          (RandoHintNpc)((uint8)kRandoHintNpc_ForkStoryteller + source);
      out->queue_index = queue;
      out->paid = true;
      out->found = true;
      return true;
    }
  }
  return false;
}

static uint8 spoiler_hint_primary_count(const RandoHintPlan *plan) {
  uint8 count = 0;
  if (plan == NULL) return 0;
  uint8 fact_count = plan->fact_count;
  if (fact_count > kRandoHintPlanMaxFacts)
    fact_count = kRandoHintPlanMaxFacts;
  for (uint8 i = 0; i < fact_count; i++)
    if (plan->facts[i].primary) count++;
  return count;
}

static uint16 spoiler_hint_row_count(const RandoHintPlan *plan) {
  if (plan == NULL) return 0;
  uint16 fact_count = plan->fact_count;
  if (fact_count > kRandoHintPlanMaxFacts)
    fact_count = kRandoHintPlanMaxFacts;
  return (uint16)(fact_count + (plan->murahdahla_active ? 1u : 0u));
}

// Generator 156's race stamp predates configurable policy metadata. The
// current binary deliberately reveals that one frozen predecessor, so its JSON
// writer must reproduce the exact 156 field set; current and future generators
// retain the additive configurable-hints schema. Generator 157 is the first
// schema-bearing version, not an alias for the current generator.
static bool spoiler_has_configurable_hint_schema(const RandoSpoiler *s) {
  return s != NULL &&
         s->generator_version >= kRandoHintConfigurableGeneratorVersion;
}

typedef struct SpoilerHintPolicy {
  const char *profile;
  const char *mix;
  uint8 tile_count;
  uint8 paid_depth;
} SpoilerHintPolicy;

typedef struct SpoilerHintTopology {
  uint8 tile_source_count;
  uint8 paid_source_count;
  uint8 paid_fact_count;
} SpoilerHintTopology;

static const char *spoiler_hint_mix_name(uint8 mix) {
  switch ((HintMix)mix) {
    case kHintMix_Variety:   return "variety";
    case kHintMix_Important: return "important";
    case kHintMix_Difficult: return "difficult";
    case kHintMix_WorldInfo: return "world-info";
    default:                 return "unknown";
  }
}

static HintProfile spoiler_hint_profile_from_policy(
    uint8 tile_count, uint8 paid_depth, uint8 mix) {
  RandoSettings settings = {0};
  settings.hints = kHintsMode_Balanced;
  settings.hint_mix = mix;
  settings.hint_paid_depth = paid_depth;
  switch (tile_count) {
    case 0:  settings.hint_tile_coverage = kHintTileCoverage_None; break;
    case 5:  settings.hint_tile_coverage = kHintTileCoverage_Sparse; break;
    case 10: settings.hint_tile_coverage = kHintTileCoverage_Standard; break;
    case 15: settings.hint_tile_coverage = kHintTileCoverage_Full; break;
    default: settings.hint_tile_coverage = 0xff; break;
  }
  return Settings_GetHintProfile(&settings);
}

// Report the normalized generation policy, not the number of rows that
// happened to survive candidate scarcity. Algorithm 1 predates configurable
// policy and is frozen to the legacy full-coverage Variety tuple.
static SpoilerHintPolicy spoiler_hint_policy(const RandoHintPlan *plan) {
  SpoilerHintPolicy policy = {
    Settings_HintProfileName(kHintProfile_Off), "not-applicable", 0, 0,
  };
  if (plan == NULL || plan->mode == kHintsMode_Off)
    return policy;

  uint8 mix = plan->mix;
  if (plan->algorithm_version == kRandoHintPlanLegacyAlgorithmVersion) {
    policy.tile_count = 15;
    policy.paid_depth = 3;
    mix = kHintMix_Variety;
  } else {
    policy.tile_count = plan->tile_target;
    policy.paid_depth = plan->paid_depth;
  }
  policy.mix = spoiler_hint_mix_name(mix);
  policy.profile = Settings_HintProfileName(
      spoiler_hint_profile_from_policy(
          policy.tile_count, policy.paid_depth, mix));
  return policy;
}

// Count actual retained delivery topology independently from requested
// coverage/depth. A scarce plan may legitimately report fewer assigned tile
// or paid facts, but it may never report a source that has no valid fact.
static SpoilerHintTopology spoiler_hint_topology(
    const RandoHintPlan *plan) {
  SpoilerHintTopology topology = {0, 0, 0};
  if (plan == NULL) return topology;
  uint8 fact_count = plan->fact_count;
  if (fact_count > kRandoHintPlanMaxFacts)
    fact_count = kRandoHintPlanMaxFacts;

  for (uint8 npc = kRandoHintNpc_TeleEasternPalace;
       npc <= kRandoHintNpc_TeleSouthEastDarkworldCave; npc++) {
    if (plan->primary_fact_by_npc[npc] < fact_count)
      topology.tile_source_count++;
  }
  for (uint8 source = 0; source < kRandoHintPaidSourceCount; source++) {
    uint8 count = plan->paid_count[source];
    if (count > kRandoHintPaidQueueCapacity)
      count = kRandoHintPaidQueueCapacity;
    uint8 valid_count = 0;
    for (uint8 queue = 0; queue < count; queue++) {
      if (plan->paid_queue[source][queue] < fact_count)
        valid_count++;
    }
    if (valid_count != 0)
      topology.paid_source_count++;
    topology.paid_fact_count =
        (uint8)(topology.paid_fact_count + valid_count);
  }
  return topology;
}

static void spoiler_write_key_ring_names_json(FILE *f, uint16 mask) {
  bool first = true;
  fprintf(f, "[");
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    if ((mask & (uint16)(1u << d)) == 0) continue;
    fprintf(f, "%s\"%s\"", first ? "" : ", ",
            BossShuffle_DungeonName(d));
    first = false;
  }
  fprintf(f, "]");
}

static void spoiler_write_key_ring_names_text(FILE *f, uint16 mask) {
  bool first = true;
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    if ((mask & (uint16)(1u << d)) == 0) continue;
    fprintf(f, "%s%s", first ? "" : ", ", BossShuffle_DungeonName(d));
    first = false;
  }
  if (first) fprintf(f, "(none)");
}

static int placement_cmp(const void *a, const void *b) {
  const RandoPlacement *pa = a;
  const RandoPlacement *pb = b;
  if (pa->location_id < pb->location_id) return -1;
  if (pa->location_id > pb->location_id) return 1;
  return 0;
}

static const char *spoiler_chain_dungeon_name(uint8 rando_dungeon) {
  return BossShuffle_DungeonName(rando_dungeon);
}

static const char *spoiler_chain_boss_name(uint8 rando_dungeon) {
  if (rando_dungeon >= kRandoDungeonCount)
    return "?";
  return BossShuffle_BossName(kRandoDungeonVanillaBoss[rando_dungeon]);
}

static void spoiler_write_chain_dungeon_list_json(FILE *f,
                                                  const DungeonChainsLayout *layout,
                                                  uint8 chain_idx) {
  fprintf(f, "[");
  uint8 elem = layout->chain_door_first[chain_idx];
  bool first = true;
  for (uint8 step = 0; step <= kChainsPoolCount; step++) {
    if (elem == kChainsElement_None || Chains_ElementIsBoss(elem))
      break;
    uint8 dungeon = Chains_ElementDungeon(elem);
    fprintf(f, "%s\"%s\"", first ? "" : ", ",
            spoiler_chain_dungeon_name(dungeon));
    first = false;
    int pidx = Chains_PoolIndexForDungeon(dungeon);
    if (pidx < 0)
      break;
    elem = layout->chain_successor[pidx];
  }
  fprintf(f, "]");
}

static void spoiler_write_chains_json(FILE *f, const RandoSpoiler *s) {
  const DungeonChainsLayout *layout = s->chains_layout;
  if (layout == NULL)
    return;

  fprintf(f, "  \"dungeon_chains\": {\n");
  fprintf(f, "    \"attempt\": %u,\n", (unsigned)s->chains_attempt);
  fprintf(f, "    \"digest24\": \"0x%06x\",\n", (unsigned)(layout->digest24 & 0xFFFFFFu));
  fprintf(f, "    \"chains\": [\n");
  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    uint8 door_dungeon = Chains_PoolDungeonAt(c);
    uint8 boss_dungeon = layout->terminal_boss[c];
    fprintf(f, "      {\"start_door\": \"%s\", \"dungeons\": ",
            spoiler_chain_dungeon_name(door_dungeon));
    spoiler_write_chain_dungeon_list_json(f, layout, c);
    fprintf(f, ", \"terminal_boss\": \"%s\", \"terminal_boss_dungeon\": \"%s\"}%s\n",
            spoiler_chain_boss_name(boss_dungeon),
            spoiler_chain_dungeon_name(boss_dungeon),
            (c + 1 < kChainsPoolCount) ? "," : "");
  }
  fprintf(f, "    ]\n");
  fprintf(f, "  },\n");
}

static void spoiler_write_chain_dungeon_list_text(FILE *f,
                                                  const DungeonChainsLayout *layout,
                                                  uint8 chain_idx) {
  uint8 elem = layout->chain_door_first[chain_idx];
  bool first = true;
  for (uint8 step = 0; step <= kChainsPoolCount; step++) {
    if (elem == kChainsElement_None || Chains_ElementIsBoss(elem))
      break;
    uint8 dungeon = Chains_ElementDungeon(elem);
    fprintf(f, "%s%s", first ? "" : " -> ",
            spoiler_chain_dungeon_name(dungeon));
    first = false;
    int pidx = Chains_PoolIndexForDungeon(dungeon);
    if (pidx < 0)
      break;
    elem = layout->chain_successor[pidx];
  }
  if (first)
    fprintf(f, "(none)");
}

static void spoiler_write_chains_text(FILE *f, const RandoSpoiler *s) {
  const DungeonChainsLayout *layout = s->chains_layout;
  if (layout == NULL)
    return;

  fprintf(f, "DUNGEON CHAINS\n");
  fprintf(f, "--------------\n");
  fprintf(f, "Attempt: %u, digest24: 0x%06x\n",
          (unsigned)s->chains_attempt,
          (unsigned)(layout->digest24 & 0xFFFFFFu));
  for (uint8 c = 0; c < kChainsPoolCount; c++) {
    uint8 door_dungeon = Chains_PoolDungeonAt(c);
    uint8 boss_dungeon = layout->terminal_boss[c];
    fprintf(f, "  %-20s door : ", spoiler_chain_dungeon_name(door_dungeon));
    spoiler_write_chain_dungeon_list_text(f, layout, c);
    fprintf(f, " -> %s (%s)\n",
            spoiler_chain_boss_name(boss_dungeon),
            spoiler_chain_dungeon_name(boss_dungeon));
  }
  fprintf(f, "\n");
}

// Look up a location def by id (replaces hand-inlined kRandoLocations scans).
// Returns NULL if the id is not in the registry.
static const RandoLocationDef *find_location(uint16 id) {
  for (uint32 j = 0; j < kRandoLocationsCount; j++)
    if (kRandoLocations[j].id == id) return &kRandoLocations[j];
  return NULL;
}

static bool spoiler_loc_is_medallion_config(uint16 id) {
  const RandoLocationDef *d = find_location(id);
  return d != NULL && d->type == LOCTYPE_Medallion;
}

// add-rando-pot-sanity — empty pots (pot_shuffle=All) are pinned to the
// ITEM_Nothing filler. They are real checks (counted in the digests), but an
// "= Nothing" row is pure noise in a human/JSON listing, so the listing
// sections omit them. Loot/key pots carry a real item and stay listed. The
// digest sections (placement_digest, sphere_digest) enumerate ALL placements
// and must NOT use this filter.
static bool spoiler_item_is_empty_pot(uint16 item_id) {
  return item_id == ITEM_Nothing;
}

static void write_medallion_requirements_text(FILE *f, const uint8 *assignment) {
  static const uint16 kLocByEntrance[kRandoMedallionEntranceCount] = {
    LOC_Misery_Mire_Medallion,
    LOC_Turtle_Rock_Medallion,
  };
  if (f == NULL || assignment == NULL) return;

  fprintf(f, "Medallion requirements:\n");
  fprintf(f, "-----------------------\n");
  for (uint8 i = 0; i < kRandoMedallionEntranceCount; i++) {
    fprintf(f, "  %-44s -> %s\n",
            Rando_GetLocationName(kLocByEntrance[i]),
            Rando_GetItemName(assignment[i]));
  }
  fprintf(f, "\n");
}

static void write_medallion_requirements_json(FILE *f, const uint8 *assignment) {
  static const uint16 kLocByEntrance[kRandoMedallionEntranceCount] = {
    LOC_Misery_Mire_Medallion,
    LOC_Turtle_Rock_Medallion,
  };

  fprintf(f, "  \"medallion_requirements\": [");
  if (assignment != NULL) {
    fprintf(f, "\n");
    for (uint8 i = 0; i < kRandoMedallionEntranceCount; i++) {
      fprintf(f, "    {\"location\": %u, \"item\": %u}%s\n",
              (unsigned)kLocByEntrance[i], (unsigned)assignment[i],
              (i + 1 < kRandoMedallionEntranceCount) ? "," : "");
    }
    fprintf(f, "  ");
  }
  fprintf(f, "],\n");
}

// One shop-class placement row, gathered once for both spoiler emitters (the
// JSON `shops[]` array and the text `Shops:` section).
typedef struct {
  uint16 loc; uint16 item; uint8 type; uint16 region_id;
  // add-rando-shopsanity — derived check price; valid iff has_price (set only
  // on LOCTYPE_Shop rows of a shopsanity seed; identity rows keep vanilla
  // purchase behavior and emit no price).
  uint16 price; bool has_price;
} SpoilerShopRow;

static int spoiler_shoprow_cmp(const void *a, const void *b) {
  uint16 la = ((const SpoilerShopRow *)a)->loc;
  uint16 lb = ((const SpoilerShopRow *)b)->loc;
  return (la > lb) - (la < lb);
}

// Collect the shop-class placements (LOCTYPE_Shop / ShopUpgrade / TakeAny — the
// Retro shop / capacity-upgrade / take-any slots) into `out`, sorted by location
// id so each shop's contiguous slots stay grouped. Returns the count (0 ⇒ no
// shop-class locations placed, i.e. a non-Retro seed). `cap` bounds `out`; 160
// exceeds the max possible shop-class count (27 Shop + 2 ShopUpgrade + 62 TakeAny
// = 91), so it never truncates — it is a hard backstop, not a real cap.
static uint16 collect_shop_rows(const RandoSpoiler *s, SpoilerShopRow *out, uint16 cap) {
  uint16 n = 0;
  if (s == NULL || s->placements == NULL) return 0;
  for (uint16 i = 0; i < s->placements->count && n < cap; i++) {
    uint16 loc = s->placements->entries[i].location_id;
    const RandoLocationDef *d = find_location(loc);
    if (d == NULL) continue;
    if (d->type != LOCTYPE_Shop && d->type != LOCTYPE_ShopUpgrade &&
        d->type != LOCTYPE_TakeAny) continue;
    out[n].loc = loc;
    out[n].item = s->placements->entries[i].item_id;
    out[n].type = d->type;
    out[n].region_id = d->region_id;
    out[n].has_price = d->type == LOCTYPE_Shop &&
                       Settings_ShopsanityActive(s->settings);
    out[n].price = out[n].has_price ? Rando_ShopPrice(s->seed_u64, loc) : 0;
    n++;
  }
  qsort(out, n, sizeof(out[0]), spoiler_shoprow_cmp);
  return n;
}

// ---------------------------------------------------------------------------
// JSON writer
// ---------------------------------------------------------------------------
static bool write_spoiler_json_stream(const RandoSpoiler *s, FILE *f);

bool Spoiler_WriteJson(const RandoSpoiler *s, const char *out_path) {
  if (s == NULL || out_path == NULL) return false;

  // Auto-create the spoiler directory if it doesn't exist. Avoids forcing
  // users to mkdir spoilers/ before the first run.
  char dir_buf[512];
  if (extract_dir(out_path, dir_buf, sizeof(dir_buf))) {
    ensure_directory(dir_buf);
  }

  FILE *f = fopen(out_path, "wb");
  if (f == NULL) return false;
  bool ok = write_spoiler_json_stream(s, f);
  fclose(f);
  return ok;
}

static bool write_spoiler_json_stream(const RandoSpoiler *s, FILE *f) {
  // Compute hashes/digests we'll cite in the meta block.
  uint8 settings_hash[32];
  Settings_ComputeHash(s->settings, settings_hash);
  uint8 placement_digest[32];
  PlacementTable_ComputeDigest(s->placements, placement_digest);

  // sphere_digest: SHA-256 over the canonical sphere assignment (per spec
  // "sphere_digest in meta block"). Lets corpus tooling detect
  // sphere-computation regressions independently of the placement table.
  // Canonical bytes: for each placement in location_id-sorted order,
  // emit (location_id u16 LE | sphere_index u8). Unreachable placements
  // contribute sphere_index 0xFF.
  uint8 sphere_digest[32];
  {
    extern void sha256_buffer(const uint8 *data, size_t len, uint8 out[32]);
    if (s->spheres != NULL && s->placements != NULL && s->placements->count > 0) {
      static struct { uint16 loc; uint8 sph; } rows[kRandoLocationCapacity];
      uint16 n = s->placements->count;
      if (n > kRandoLocationCapacity) n = kRandoLocationCapacity;
      for (uint16 i = 0; i < n; i++) {
        rows[i].loc = s->placements->entries[i].location_id;
        rows[i].sph = s->spheres->sphere_index_by_placement[i];
      }
      // Insertion sort by location_id (canonical).
      for (uint16 i = 1; i < n; i++) {
        uint16 j = i;
        while (j > 0 && rows[j - 1].loc > rows[j].loc) {
          uint16 tl = rows[j - 1].loc; uint8 ts = rows[j - 1].sph;
          rows[j - 1].loc = rows[j].loc; rows[j - 1].sph = rows[j].sph;
          rows[j].loc = tl; rows[j].sph = ts;
          j--;
        }
      }
      static uint8 buf[kRandoLocationCapacity * 3];
      for (uint16 i = 0; i < n; i++) {
        buf[i * 3 + 0] = (uint8)(rows[i].loc & 0xff);
        buf[i * 3 + 1] = (uint8)(rows[i].loc >> 8);
        buf[i * 3 + 2] = rows[i].sph;
      }
      sha256_buffer(buf, (size_t)n * 3, sphere_digest);
    } else {
      sha256_buffer((const uint8 *)"", 0, sphere_digest);
    }
  }

  // -----------------------------------------------------------------------
  // Meta block — names match ALTTPR's "meta" object where applicable (per
  // randomizer-core / "Spoiler JSON schema mirrors ALTTPR field names").
  // -----------------------------------------------------------------------
  fprintf(f, "{\n");
  fprintf(f, "  \"meta\": {\n");
  fprintf(f, "    \"spoiler_format_version\": 1,\n");
  fprintf(f, "    \"generator_version\": %u,\n", (unsigned)s->generator_version);
  fprintf(f, "    \"settings_hash_hex\": \"");
  write_hex(f, settings_hash, 16);  // first 16 bytes, matching share-string truncation
  fprintf(f, "\",\n");
  fprintf(f, "    \"settings_hash_full_hex\": \"");
  write_hex(f, settings_hash, 32);
  fprintf(f, "\",\n");
  fprintf(f, "    \"placement_digest_hex\": \"");
  write_hex(f, placement_digest, 32);
  fprintf(f, "\",\n");
  fprintf(f, "    \"sphere_digest\": \"");
  write_hex(f, sphere_digest, 32);
  fprintf(f, "\",\n");
  fprintf(f, "    \"share_string\": \"%s\",\n",
          s->share_string != NULL ? s->share_string : "");
  fprintf(f, "    \"seed_u64\": \"0x%016llx\",\n", (unsigned long long)s->seed_u64);
  fprintf(f, "    \"world_state\": %u,\n", (unsigned)s->settings->world_state);
  fprintf(f, "    \"goal\": %u,\n", (unsigned)s->settings->goal);
  fprintf(f, "    \"generation_wall_clock_ms\": %u,\n",
          (unsigned)s->generation_wall_clock_ms);
  fprintf(f, "    \"goal_completable\": %s,\n",
          s->goal_completable ? "true" : "false");
  // Number of explicit delivery rows plus the separate Murahdahla
  // compatibility row, exactly mirroring the top-level hints[] length.
  fprintf(f, "    \"hints_count\": %u,\n",
          (unsigned)spoiler_hint_row_count(s->hint_plan));
  // Emit a customizer_active marker ONLY when the seed was hand-placed (derived
  // from the canonical settings, so it round-trips on race-mode reveal).
  // Conditional emission keeps every non-customizer
  // spoiler — and thus the regression corpus + race stamps — byte-identical.
  if (s->settings != NULL && s->settings->customizer_active) {
    fprintf(f, "    \"customizer_active\": true,\n");
  }
  // fallback_warnings: each non-zero counter from the placer surfaces
  // here. Plus an "unreachable_placements" rollup when the placer could
  // not produce a fully-reachable seed.
  fprintf(f, "    \"fallback_warnings\": [");
  {
    bool first = true;
    if (s->forward_fill_fallback_count > 0) {
      fprintf(f, "%s\n      {\"kind\": \"forward_fill_fallback\", \"count\": %u}",
              first ? "" : ",",
              (unsigned)s->forward_fill_fallback_count);
      first = false;
    }
    if (s->retry_attempts > 1) {
      fprintf(f, "%s\n      {\"kind\": \"retry_attempts\", \"count\": %u}",
              first ? "" : ",",
              (unsigned)s->retry_attempts);
      first = false;
    }
    // Drop heart-floor exhausted its retry budget and fell back to the vanilla
    // identity drop table (essentially never; surfaced so the
    // reader knows the seed's drop shuffle is a no-op).
    if (s->drop_used_fallback) {
      fprintf(f, "%s\n      {\"kind\": \"drop_heart_floor_fallback\"}",
              first ? "" : ",");
      first = false;
    }
    if (s->spheres != NULL && s->spheres->unreachable_count > 0) {
      fprintf(f, "%s\n      {\"kind\": \"unreachable_placements\", \"count\": %u}",
              first ? "" : ",",
              (unsigned)s->spheres->unreachable_count);
      first = false;
    }
    // NoLogic seeds enforce NO reachability: the predicate VM short-circuits
    // the reachability eval (rando_logic.c), so placement ignores progression
    // gating and the seed may be un-completable.
    // Emitted by reading settings directly (the unverified_tricks_enabled
    // pattern below). The spec pins this entry's shape to {"code","detail"},
    // distinct from the {"kind"} shape of the placer-counter warnings above.
    if (s->settings->logic >= 4 /* NoLogic */) {
      fprintf(f, "%s\n      {\"code\": \"no_logic_seed\", \"detail\": \"Seed generated with logic=NoLogic; reachability is not enforced. The seed may be un-completable.\"}",
              first ? "" : ",");
      first = false;
    }
    // NOTE: there is no longer an `accessibility_none_seed` warning. Under the
    // ALTTPR three-way accessibility axis, `none` means "beatable only" — the
    // seed is still guaranteed completable, so the old "may be unwinnable"
    // warning no longer applies. When `items`/`beatable` intentionally leave
    // some locations unreachable, the `unreachable_placements` warning above
    // already surfaces that to the reader for every tier.
    //
    // §12.6.4 — surface tricks / glitch levels the user enabled that are NOT
    // yet confirmed to behave on the US 1.0 ROM (ALTTPR targets JP 1.0). This is
    // informational (the seed still generates); race admins / seed validators
    // decide whether to accept. cross-version + verified-us10 do NOT warn.
    {
      bool any_unverified = false;
      for (uint32 i = 0; i < kRandoTrickStatusCount; i++) {
        const RandoTrickStatus *ts = &kRandoTrickStatus[i];
        if ((s->settings->tricks & (uint8)(1u << ts->bit)) == 0) continue;
        if (ts->rom_version_status == kRomVerStatus_CrossVersion ||
            ts->rom_version_status == kRomVerStatus_VerifiedUs10) continue;
        if (!any_unverified) {
          fprintf(f, "%s\n      {\"kind\": \"unverified_tricks_enabled\", \"names\": [",
                  first ? "" : ",");
          any_unverified = true;
        } else {
          fprintf(f, ", ");
        }
        fprintf(f, "\"%s\"", ts->id);
      }
      // Glitch levels: logic >= level enables that tier's gates, so warn for
      // every non-zero level the seed reaches whose status is unverified.
      // Suppressed entirely under NoLogic (logic>=4): the reachability eval is
      // short-circuited (rando_logic.c), so NO glitch-tier gate actually
      // evaluates, and the dedicated no_logic_seed warning above already covers
      // the seed — listing every tier (including no_logic itself) here would be
      // misleading "in-use" noise.
      for (uint32 i = 0; i < kRandoGlitchLevelStatusCount && s->settings->logic < 4; i++) {
        const RandoGlitchLevelStatus *gs = &kRandoGlitchLevelStatus[i];
        if (gs->level == 0) continue;                 // no_glitches never warns
        if (s->settings->logic < gs->level) continue; // tier not reached
        if (gs->rom_version_status == kRomVerStatus_CrossVersion ||
            gs->rom_version_status == kRomVerStatus_VerifiedUs10) continue;
        if (!any_unverified) {
          fprintf(f, "%s\n      {\"kind\": \"unverified_tricks_enabled\", \"names\": [",
                  first ? "" : ",");
          any_unverified = true;
        } else {
          fprintf(f, ", ");
        }
        fprintf(f, "\"%s\"", gs->id);
      }
      if (any_unverified) {
        fprintf(f, "]}");
        first = false;
      }
    }
    if (!first) fprintf(f, "\n    ");
  }
  fprintf(f, "]\n");
  fprintf(f, "  },\n");

  // -----------------------------------------------------------------------
  // Settings — full struct as a flat object. Field order matches the C
  // struct (NOT the canonical-serialization order; that's a determinism
  // contract for the hash, not a human-readable surface).
  // -----------------------------------------------------------------------
  uint8 canon[kSettingsCanonicalLen];
  Settings_CanonicalSerialize(s->settings, canon);
  fprintf(f, "  \"settings\": {\n");
  fprintf(f, "    \"settings_version\": %u,\n", s->settings->settings_version);
  fprintf(f, "    \"world_state\": %u,\n", s->settings->world_state);
  fprintf(f, "    \"goal\": %u,\n", s->settings->goal);
  fprintf(f, "    \"crystals_ganon\": %u,\n", s->settings->crystals_ganon);
  fprintf(f, "    \"crystals_tower\": %u,\n", s->settings->crystals_tower);
  // add-rando-random-crystals — when either axis carries the requested
  // sentinel (8), also emit the seed-RESOLVED counts (keyrings
  // requested/effective precedent) + the resolve salt version.
  if (s->settings->crystals_ganon == kCrystalsRandom ||
      s->settings->crystals_tower == kCrystalsRandom) {
    uint8 rg, rt;
    Crystals_Resolve(s->settings, s->seed_u64, &rg, &rt);
    fprintf(f, "    \"crystals_ganon_resolved\": %u,\n", rg);
    fprintf(f, "    \"crystals_tower_resolved\": %u,\n", rt);
    fprintf(f, "    \"crystals_salt_version\": 1,\n");
  }
  fprintf(f, "    \"item_pool_difficulty\": %u,\n", s->settings->item_pool_difficulty);
  // Emit the EFFECTIVE key modes (Retro pins Wild; door shuffle pins Dungeon
  // for both) so the spoiler matches the canonical hash + actual placement,
  // not the user's raw (overridden) fields.
  fprintf(f, "    \"dungeon_small_keys_mode\": %u,\n", Settings_EffectiveSmallKeysMode(s->settings));
  fprintf(f, "    \"dungeon_big_keys_mode\": %u,\n", Settings_EffectiveBigKeysMode(s->settings));
  fprintf(f, "    \"dungeon_maps_mode\": %u,\n", s->settings->dungeon_maps_mode);
  fprintf(f, "    \"dungeon_compasses_mode\": %u,\n", s->settings->dungeon_compasses_mode);
  fprintf(f, "    \"prize_shuffle\": %s,\n", s->settings->prize_shuffle ? "true" : "false");
  fprintf(f, "    \"medallion_shuffle\": %s,\n", s->settings->medallion_shuffle ? "true" : "false");
  fprintf(f, "    \"mode_weapons\": %u,\n", s->settings->mode_weapons);
  // EFFECTIVE accessibility (Completionist forces Locations) so the spoiler can't
  // contradict the canonical hash / share string it is emitted alongside.
  fprintf(f, "    \"accessibility\": %u,\n", Settings_EffectiveAccessibility(s->settings));
  fprintf(f, "    \"pieces_required\": %u,\n", s->settings->pieces_required);
  fprintf(f, "    \"pieces_placed\": %u,\n", s->settings->pieces_placed);
  fprintf(f, "    \"hints\": %u,\n", (unsigned)canon[22]);
  if (spoiler_has_configurable_hint_schema(s)) {
    fprintf(f, "    \"hint_tile_coverage\": %u,\n",
            (unsigned)((canon[28] & kHintTileCoverageAxis_Mask) >>
                       kHintTileCoverageAxis_Shift));
    fprintf(f, "    \"hint_mix\": %u,\n",
            (unsigned)(((canon[28] & kHintMixAxis_LowBit) ? 1u : 0u) |
                       ((canon[29] & kHintMixAxis_HighBit) ? 2u : 0u)));
    {
      uint8 paid_wire =
          (uint8)((canon[30] & kHintPaidDepthAxis_Mask) >>
                  kHintPaidDepthAxis_Shift);
      fprintf(f, "    \"hint_paid_depth\": %u,\n",
              (unsigned)((paid_wire + 3u) & 3u));
    }
    // NPC reward previews are an independent disclosure axis, not part of the
    // clue profile or hint-plan digest. Decode the canonical bit so generation
    // and race reveal emit the same value even if the caller's raw settings
    // struct has not been normalized.
    fprintf(f, "    \"hint_npc_reward_reveal\": %s,\n",
            (canon[25] & kHintNpcRewardRevealAxis_Enabled)
                ? "true"
                : "false");
  }
  fprintf(f, "    \"boss_shuffle\": %u,\n", s->settings->boss_shuffle);
  fprintf(f, "    \"drop_shuffle\": %u,\n", s->settings->drop_shuffle);
  fprintf(f, "    \"enemy_drop_checks\": %u,\n",
          Settings_EffectiveEnemyDropChecks(s->settings));
  fprintf(f, "    \"traps\": %u,\n", s->settings->traps);
  // Report the EFFECTIVE pot_shuffle: door / cave-entrance shuffle force it off at
  // generation (Settings_PotShuffleForcedOff), so a raw value would claim a
  // pot-less seed shuffled pots. Matches the canonical/share normalization.
  fprintf(f, "    \"pot_shuffle\": %u,\n",
          Settings_PotShuffleForcedOff(s->settings) ? 0u : s->settings->pot_shuffle);
  fprintf(f, "    \"instant_flute\": %s,\n", s->settings->instant_flute ? "true" : "false");
  // The echo below completes the struct (it had silently drifted — several
  // newer axes were missing, so a player could not confirm from the spoiler
  // that e.g. npc_souls was on). KEEP IN SYNC with RandoSettings: every field
  // in the struct appears here, EFFECTIVE values where a derived rule can
  // override the raw one (souls under door shuffle, chains). The canonical
  // blob hex at the end is the machine-checkable completeness witness — any
  // settings axis change changes it even if a field line here is forgotten.
  // Serialize the canonical blob ONCE up front and DECODE the remaining
  // fields from it, rather than reading the raw struct. Two of the raw
  // entrance sub-fields (coupled defaults to 1; cross/decoupled) are
  // normalized only in the SERIALIZED form (apply_derived_rules), so a raw
  // echo reads 1 at generate but 0 after a canonical round-trip at reveal —
  // which false-fails the race-mode stamp. Decoding from `canon` makes every
  // emitted value canonical-consistent, so generate and reveal always agree.
  fprintf(f, "    \"tricks\": %u,\n", canon[4]);
  fprintf(f, "    \"logic\": %u,\n", canon[6]);
  fprintf(f, "    \"pyramid_bow_upgrade\": %u,\n", canon[9]);
  fprintf(f, "    \"region_boss_hearts_in_pool\": %u,\n", canon[10]);
  // race_mode is byte [17] but the stamp normalizes it to 0 (recorded in file
  // existence, not the stamped settings); emit the raw struct value for the
  // human-readable file, NOT canon[17] — the stamp path passes race_mode=0
  // settings, and the reveal path zeroes it too, so both stamp inputs read
  // false while the on-disk file shows the real value.
  fprintf(f, "    \"race_mode\": %s,\n", s->settings->race_mode ? "true" : "false");
  fprintf(f, "    \"shuffle_cave_entrances\": %s,\n", (canon[25] & kEntranceAxis_ShuffleCaves) ? "true" : "false");
  fprintf(f, "    \"shuffle_dungeon_entrances\": %s,\n", (canon[25] & kEntranceAxis_ShuffleDungeons) ? "true" : "false");
  fprintf(f, "    \"coupled\": %u,\n", (canon[25] & kEntranceAxis_Coupled) ? 1u : 0u);
  fprintf(f, "    \"cross_category\": %u,\n", (canon[25] & kEntranceAxis_CrossCategory) ? 1u : 0u);
  fprintf(f, "    \"decoupled\": %u,\n", (canon[25] & kEntranceAxis_Decoupled) ? 1u : 0u);
  fprintf(f, "    \"shuffle_ganons_tower_entrance\": %s,\n", (canon[25] & kEntranceAxis_ShuffleGanonsTower) ? "true" : "false");
  fprintf(f, "    \"dungeon_chains\": %s,\n", (canon[25] & kEntranceAxis_DungeonChains) ? "true" : "false");
  fprintf(f, "    \"enemy_shuffle\": %s,\n", (canon[26] & kEnemyShuffleAxis_Enabled) ? "true" : "false");
  fprintf(f, "    \"door_shuffle\": %u,\n", Settings_EffectiveDoorShuffle(s->settings));
  fprintf(f, "    \"customizer_active\": %s,\n", (canon[26] & kCustomizerAxis_Active) ? "true" : "false");
  fprintf(f, "    \"trap_categories\": %u,\n", s->settings->trap_categories);
  // EFFECTIVE souls tier (degrades to off under door shuffle) — matches the
  // pool, the logic VM, and the runtime suppression. souls_shuffle rides
  // canon[28] bits 2-3; npc_souls is canon[28] bit 4.
  fprintf(f, "    \"souls_shuffle\": %u,\n",
          (uint8)((canon[28] & kSoulsShuffleAxis_Mask) >> kSoulsShuffleAxis_Shift));
  fprintf(f, "    \"npc_souls\": %s,\n", (canon[28] & kNpcSoulsAxis_Enabled) ? "true" : "false");
  // add-rando-grass-rock-shuffle — the terrain axes ride canon[29] (the
  // pot-sanity 91a2aef lesson: every axis emits, or corpus scoping is blind).
  fprintf(f, "    \"grass_shuffle\": %u,\n",
          (uint8)((canon[29] & kGrassShuffleAxis_Mask) >> kGrassShuffleAxis_Shift));
  fprintf(f, "    \"rock_shuffle\": %u,\n",
          (uint8)((canon[29] & kRockShuffleAxis_Mask) >> kRockShuffleAxis_Shift));
  // add-rando-shopsanity — shares canon[29] with the terrain axes and was
  // missing from this echo. It matters now that cave-entrance shuffle forces
  // the axis off: without the field a player has only canonical_hex to tell
  // whether their shopsanity request survived. canon[] is post-normalization,
  // so this is already the EFFECTIVE value.
  //
  // The signed generator-156 Hints 1/1 race fixture predates this field. Its
  // explicit reveal exception must reproduce that exact frozen JSON schema;
  // every current/future schema-bearing generator keeps the shopsanity echo.
  if (s->generator_version != kRandoHintLegacyGeneratorVersion) {
    fprintf(f, "    \"shopsanity\": %s,\n",
            (canon[29] & kShopsanityAxis_Enabled) ? "true" : "false");
  }
  KeyRingSelection key_rings;
  bool key_rings_selection_valid =
      KeyRings_Resolve(s->settings, s->seed_u64, &key_rings);
  fprintf(f, "    \"key_rings_requested\": %u,\n",
          (uint8)((canon[30] & kKeyRingsAxis_Mask) >> kKeyRingsAxis_Shift));
  fprintf(f, "    \"key_rings_effective\": %u,\n",
          Settings_EffectiveKeyRings(s->settings));
  fprintf(f, "    \"key_rings_eligible_mask\": %u,\n", key_rings.eligible_mask);
  fprintf(f, "    \"key_rings_eligible_names\": ");
  spoiler_write_key_ring_names_json(f, key_rings.eligible_mask);
  fprintf(f, ",\n");
  fprintf(f, "    \"key_rings_selected_mask\": %u,\n", key_rings.selected_mask);
  fprintf(f, "    \"key_rings_selected_names\": ");
  spoiler_write_key_ring_names_json(f, key_rings.selected_mask);
  fprintf(f, ",\n");
  fprintf(f, "    \"key_rings_selection_salt_version\": 1,\n");
  fprintf(f, "    \"key_rings_selection_valid\": %s,\n",
          key_rings_selection_valid ? "true" : "false");
  fprintf(f, "    \"skeleton_key\": %s,\n",
          (canon[30] & kSkeletonKeyAxis_Enabled) ? "true" : "false");
  fprintf(f, "    \"canonical_hex\": \"");
  for (int ci = 0; ci < kSettingsCanonicalLen; ci++) fprintf(f, "%02x", canon[ci]);
  fprintf(f, "\"\n");
  fprintf(f, "  },\n");

  write_medallion_requirements_json(f, s->medallion_assignment);

  // -----------------------------------------------------------------------
  // Item placements — sorted by location_id (determinism). Medallion config
  // rows are reported separately as requirements, not collectable locations.
  // -----------------------------------------------------------------------
  fprintf(f, "  \"placements\": [\n");
  if (s->placements != NULL && s->placements->count > 0) {
    // Sort a copy by location_id.
    uint16 n = s->placements->count;
    if (n > kRandoLocationCapacity) n = kRandoLocationCapacity;
    RandoPlacement local[kRandoLocationCapacity];
    uint16 row_n = 0;
    for (uint16 i = 0; i < n; i++) {
      if (spoiler_loc_is_medallion_config(s->placements->entries[i].location_id)) continue;
      if (spoiler_item_is_empty_pot(s->placements->entries[i].item_id)) continue;
      local[row_n++] = s->placements->entries[i];
    }
    qsort(local, row_n, sizeof(RandoPlacement), placement_cmp);
    for (uint16 i = 0; i < row_n; i++) {
      fprintf(f, "    {\"location\": %u, \"item\": %u}%s\n",
              local[i].location_id, local[i].item_id,
              (i + 1 < row_n) ? "," : "");
    }
  }
  fprintf(f, "  ],\n");

  // -----------------------------------------------------------------------
  // entrance_mapping — door interior → loaded interior when an entrance shuffle
  // was applied. Omitted otherwise.
  // -----------------------------------------------------------------------
  Entrance_WriteSpoilerJson(f, s->entrance_assign, s->entrance_count);
  Entrance_WriteDungeonSpoilerJson(f, s->dungeon_assign, s->dungeon_count);
  Entrance_WriteCrossSpoilerJson(f, s->cross_assign, s->cross_count);
  Entrance_WriteDecoupledSpoilerJson(f, s->decoupled_assign, s->decoupled_count);
  Entrance_WriteDungeonDecoupledSpoilerJson(f, s->dun_decoupled_assign, s->dun_decoupled_count);
  Entrance_WriteCrossDecoupledSpoilerJson(f, s->cross_decoupled_assign, s->cross_decoupled_count);
  spoiler_write_chains_json(f, s);

  // -----------------------------------------------------------------------
  // door_shuffle — per-dungeon door pairings + relocated key doors with
  // worst-case thresholds. Reads the layout the generation pipeline leaves
  // installed through spoiler emission. Omitted when door shuffle is off.
  // -----------------------------------------------------------------------
  {
    uint16 door_mask = 0;
    const DoorShuffleLayout *dl = Rando_GetDoorLogicLayout(&door_mask);
    if (dl != NULL && door_mask != 0) {
      fprintf(f, "  \"door_shuffle\": {\n");
      bool first_dgn = true;
      for (int d = 0; d < kDoorTbl_DungeonCount; d++) {
        if (!((door_mask >> d) & 1)) continue;
        fprintf(f, "%s    \"%s\": {\n", first_dgn ? "" : ",\n",
                kDoorTblNames + kDoorTblDungeons[d].name_off);
        first_dgn = false;
        fprintf(f, "      \"doors\": [");
        bool first_pair = true;
        for (int i = 0; i < kDoorTbl_DoorCount; i++) {
          uint16 p = dl->pairing[i];
          if (p == 0xFFFF || p < (uint16)i) continue;  // each link once
          if (kDoorTblDoors[i].dungeon != d) continue;
          fprintf(f, "%s\n        [\"%s\", \"%s\"]", first_pair ? "" : ",",
                  kDoorTblNames + kDoorTblDoors[i].name_off,
                  kDoorTblNames + kDoorTblDoors[p].name_off);
          first_pair = false;
        }
        fprintf(f, "\n      ],\n      \"key_doors\": [");
        for (int k = 0; k < dl->key_door_count[d]; k++) {
          fprintf(f, "%s\n        { \"door\": \"%s\", \"keys_needed\": %d }",
                  k ? "," : "",
                  kDoorTblNames + kDoorTblDoors[dl->key_doors[d][k]].name_off,
                  dl->key_worst_case[d][k]);
        }
        fprintf(f, "\n      ]\n    }");
      }
      fprintf(f, "\n  },\n");
    }
  }

  // -----------------------------------------------------------------------
  // ow_warps — flute spot assignments + whirlpool pairs
  // (add-rando-ow-warp-shuffle). Emitted ONLY when a warp axis is active
  // (axes-off spoiler byte-identity); whirlpool pairs are in whirlpool-id
  // (source-screen) space.
  // -----------------------------------------------------------------------
  {
    const OwWarpLayout *wl = Rando_GetOwWarpGenerationLayout();
    if (wl != NULL && wl->active != 0) {
      fprintf(f, "  \"ow_warps\": {\n");
      if (wl->active & 1) {
        fprintf(f, "    \"flute\": [");
        for (int s = 0; s < kOwWarpFluteSlots; s++) {
          const RandoOwFluteCandidate *c =
              &kRandoOwFluteCandidates[wl->flute_cand[s]];
          fprintf(f, "%s\n      { \"slot\": %d, \"screen\": \"0x%02x\", "
                     "\"region\": \"%s\" }",
                  s ? "," : "", s, c->screen,
                  Rando_GetRegionName(c->region));
        }
        fprintf(f, "\n    ]%s\n", (wl->active & 2) ? "," : "");
      }
      if (wl->active & 2) {
        fprintf(f, "    \"whirlpools\": [");
        bool first_wp = true;
        for (uint32 i = 0; i < kRandoOwWhirlpoolsCount &&
                           i < kOwWarpWhirlpoolMax; i++) {
          uint8 p = wl->wp_partner[i];
          if (p <= (uint8)i) continue;  // each pair once
          fprintf(f, "%s\n      [\"0x%02x\", \"0x%02x\"]",
                  first_wp ? "" : ",", kRandoOwWhirlpools[i].screen,
                  kRandoOwWhirlpools[p].screen);
          first_wp = false;
        }
        fprintf(f, "\n    ]\n");
      }
      fprintf(f, "  },\n");
    }
  }

  // -----------------------------------------------------------------------
  // boss_assignments — dungeon → boss now in its boss room. Emitted only when
  // boss_shuffle is on (boss_assignment != NULL); the
  // dungeon→prize binding is unchanged (EP's prize stays at EP regardless).
  // -----------------------------------------------------------------------
  if (s->boss_assignment != NULL) {
    fprintf(f, "  \"boss_assignments\": [\n");
    bool first = true;
    for (uint8 d = 1; d <= 12; d++) {  // dungeons EP..GT; HCE (0) has no boss
      uint8 idx = s->boss_assignment[d];
      if (idx == 0xFF) continue;
      fprintf(f, "%s    {\"dungeon\": %u, \"dungeon_name\": \"%s\", "
                 "\"boss\": %u, \"boss_name\": \"%s\"}",
              first ? "" : ",\n", (unsigned)d, BossShuffle_DungeonName(d),
              (unsigned)idx, BossShuffle_BossName(idx));
      first = false;
    }
    fprintf(f, "%s  ],\n", first ? "" : "\n");
  }

  // -----------------------------------------------------------------------
  // drop_tables — the 7 enemy-drop packs after the shuffle,
  // each as 8 resolved drop item ids (kPrizeItems[drop_map[flat]]). Pack 0 is
  // the heart-floor pack (guaranteed >=1 heart, id 216 = 0xD8). Emitted only
  // when drop_shuffle is on (drop_map != NULL — §6.4).
  // -----------------------------------------------------------------------
  if (s->drop_map != NULL) {
    fprintf(f, "  \"drop_tables\": [\n");
    for (uint8 pack = 0; pack < 7; pack++) {
      fprintf(f, "    [");
      for (uint8 slot = 0; slot < 8; slot++) {
        uint8 flat = (uint8)(pack * 8 + slot);
        uint8 item = Sprite_VanillaPrizeItem(s->drop_map[flat]);
        fprintf(f, "%s%u", slot ? ", " : "", (unsigned)item);
      }
      fprintf(f, "]%s\n", (pack < 6) ? "," : "");
    }
    fprintf(f, "  ],\n");
  }

  // -----------------------------------------------------------------------
  // sphere_data — emitted per `randomizer-core / Sphere semantics` when a
  // sphere table is provided. Format: array of arrays; each inner array
  // holds {location, item} for that sphere.
  // -----------------------------------------------------------------------
  fprintf(f, "  \"sphere_data\": [");
  if (s->spheres != NULL && s->placements != NULL && s->placements->count > 0) {
    fprintf(f, "\n");
    uint8 max_sphere = s->spheres->max_sphere;
    for (uint8 sp = 0; sp <= max_sphere; sp++) {
      fprintf(f, "    [");
      bool first = true;
      for (uint16 i = 0; i < s->placements->count; i++) {
        if (s->spheres->sphere_index_by_placement[i] != sp) continue;
        if (spoiler_loc_is_medallion_config(s->placements->entries[i].location_id)) continue;
        if (spoiler_item_is_empty_pot(s->placements->entries[i].item_id)) continue;
        if (!first) fprintf(f, ", ");
        fprintf(f, "{\"location\": %u, \"item\": %u}",
                s->placements->entries[i].location_id,
                s->placements->entries[i].item_id);
        first = false;
      }
      fprintf(f, "]%s\n", (sp < max_sphere) ? "," : "");
    }
    fprintf(f, "  ");
  }
  fprintf(f, "],\n");
  // unreachable_placements: placements whose location never became reachable
  // — emitted alongside sphere_data so tooling can detect broken seeds.
  fprintf(f, "  \"unreachable_placements\": [");
  if (s->spheres != NULL && s->placements != NULL) {
    bool first = true;
    for (uint16 i = 0; i < s->placements->count; i++) {
      if (s->spheres->sphere_index_by_placement[i] != 0xFF) continue;
      if (spoiler_loc_is_medallion_config(s->placements->entries[i].location_id)) continue;
      if (spoiler_item_is_empty_pot(s->placements->entries[i].item_id)) continue;
      if (!first) fprintf(f, ", ");
      fprintf(f, "{\"location\": %u, \"item\": %u}",
              s->placements->entries[i].location_id,
              s->placements->entries[i].item_id);
      first = false;
    }
  }
  fprintf(f, "],\n");
  // -----------------------------------------------------------------------
  // Immutable hint-plan identity plus complete delivery rows. The plan is an
  // explicit caller-owned spoiler input; discovery, resolved state, paid
  // cursors, and presentation latches are intentionally absent.
  // -----------------------------------------------------------------------
  fprintf(f, "  \"hint_plan\": ");
  if (s->hint_plan == NULL) {
    fprintf(f, "null,\n");
  } else {
    const RandoHintPlan *plan = s->hint_plan;
    uint8 fact_count = plan->fact_count;
    if (fact_count > kRandoHintPlanMaxFacts)
      fact_count = kRandoHintPlanMaxFacts;
    uint8 primary_count = spoiler_hint_primary_count(plan);
    if (primary_count > fact_count) primary_count = fact_count;
    SpoilerHintPolicy policy = spoiler_hint_policy(plan);
    SpoilerHintTopology topology = spoiler_hint_topology(plan);
    fprintf(f, "{\n");
    fprintf(f, "    \"algorithm_version\": %u,\n",
            (unsigned)plan->algorithm_version);
    fprintf(f, "    \"text_schema_version\": %u,\n",
            (unsigned)plan->text_schema_version);
    fprintf(f, "    \"digest\": \"");
    write_hex(f, plan->digest, kRandoHintPlanDigestBytes);
    fprintf(f, "\",\n");
    fprintf(f, "    \"fact_count\": %u,\n", (unsigned)fact_count);
    fprintf(f, "    \"primary_count\": %u,\n", (unsigned)primary_count);
    if (spoiler_has_configurable_hint_schema(s)) {
      fprintf(f, "    \"reserve_count\": %u,\n",
              (unsigned)(fact_count - primary_count));
      fprintf(f, "    \"profile\": \"%s\",\n", policy.profile);
      fprintf(f, "    \"tile_count\": %u,\n",
              (unsigned)policy.tile_count);
      fprintf(f, "    \"paid_depth\": %u,\n",
              (unsigned)policy.paid_depth);
      fprintf(f, "    \"mix\": \"%s\",\n", policy.mix);
      fprintf(f, "    \"source_count\": %u,\n",
              (unsigned)(topology.tile_source_count +
                         topology.paid_source_count));
      fprintf(f, "    \"tile_source_count\": %u,\n",
              (unsigned)topology.tile_source_count);
      fprintf(f, "    \"paid_source_count\": %u,\n",
              (unsigned)topology.paid_source_count);
      fprintf(f, "    \"paid_fact_count\": %u\n",
              (unsigned)topology.paid_fact_count);
    } else {
      fprintf(f, "    \"reserve_count\": %u\n",
              (unsigned)(fact_count - primary_count));
    }
    fprintf(f, "  },\n");
  }

  fprintf(f, "  \"hints\": [");
  {
    bool first = true;
    const RandoHintPlan *plan = s->hint_plan;
    uint8 fact_count = plan != NULL ? plan->fact_count : 0;
    if (fact_count > kRandoHintPlanMaxFacts)
      fact_count = kRandoHintPlanMaxFacts;
    for (uint8 i = 0; i < fact_count; i++) {
      const RandoHintFact *fact = &plan->facts[i];
      SpoilerHintAssignment assignment;
      bool assigned =
          spoiler_find_hint_assignment(plan, fact->fact_id, &assignment);
      RandoHintNpc npc = assigned ? assignment.npc : kRandoHintNpc_None;
      const char *npc_str =
          assigned ? Rando_GetHintNpcStringId(npc) : "unassigned";
      uint16 dialogue_id =
          assigned ? spoiler_hint_dialogue_id(npc) : 0xFFFFu;

      fprintf(f, "%s    {\"npc\": ",
              first ? "\n" : ",\n");
      write_json_string(f, npc_str != NULL ? npc_str : "unassigned");
      fprintf(f, ", \"dialogue_id\": %u, \"text\": ",
              (unsigned)dialogue_id);
      write_json_string(f, fact->text);
      fprintf(f, ", \"fact_id\": %u, \"fact_kind\": ",
              (unsigned)fact->fact_id);
      write_json_string(f, spoiler_hint_fact_kind_name(fact->kind));
      fprintf(f, ", \"fact_kind_id\": %u, \"precision\": ",
              (unsigned)fact->kind);
      write_json_string(
          f, fact->location_id != 0xFFFFu && fact->item_id != 0xFFFFu
                 ? "exact"
                 : "positive_summary");
      fprintf(f, ", \"template\": ");
      write_json_string(f, spoiler_hint_template_name(fact->template_id));
      fprintf(f, ", \"template_id\": %u, \"template_parameter\": %u, "
                 "\"primary\": %s, \"source\": ",
              (unsigned)fact->template_id,
              (unsigned)fact->template_parameter,
              fact->primary ? "true" : "false");
      write_json_string(f, npc_str != NULL ? npc_str : "unassigned");
      fprintf(f, ", \"source_id\": %u, \"source_kind\": ",
              (unsigned)npc);
      write_json_string(
          f, !assigned ? "unassigned" : assignment.paid ? "paid" : "tile");
      if (assigned && assignment.paid) {
        fprintf(f, ", \"queue_index\": %u",
                (unsigned)assignment.queue_index);
      } else if (assigned) {
        fprintf(f, ", \"tile_index\": %u",
                (unsigned)assignment.tile_index);
      }
      if (fact->location_id != 0xFFFFu)
        fprintf(f, ", \"location_id\": %u", (unsigned)fact->location_id);
      if (fact->item_id != 0xFFFFu)
        fprintf(f, ", \"item_id\": %u", (unsigned)fact->item_id);
      if (fact->region_id != 0xFFFFu)
        fprintf(f, ", \"region_id\": %u", (unsigned)fact->region_id);
      fprintf(f, "}");
      first = false;
    }

    // Murahdahla is intentionally not a delivery fact: no fact id, source
    // assignment, queue position, or discovery bit, and it is excluded from
    // the delivery fact counts above.
    if (plan != NULL && plan->murahdahla_active) {
      RandoHintNpc npc = kRandoHintNpc_Murahdahla;
      fprintf(f, "%s    {\"npc\": ", first ? "\n" : ",\n");
      write_json_string(f, Rando_GetHintNpcStringId(npc));
      fprintf(f, ", \"dialogue_id\": %u, \"text\": ",
              (unsigned)spoiler_hint_dialogue_id(npc));
      write_json_string(f, plan->murahdahla_text);
      fprintf(f, ", \"compatibility\": \"murahdahla\"}");
      first = false;
    }
    if (!first) fprintf(f, "\n  ");
  }
  fprintf(f, "],\n");

  // shops[] (tasks §8.2-8.3) — the Retro shop / capacity-upgrade / take-any
  // placements, carrying their region (so consumers can group by region) and an
  // identity_placed flag for the Capacity Upgrade slots. Emitted ONLY when
  // shop-class locations are placed (Retro), so non-Retro JSON is byte-identical
  // (the key is omitted entirely, not emitted empty). This changes the JSON
  // bytes for Retro seeds, which feed the race-mode stamp — version-locked by
  // the kGeneratorVersion bump that ships with Retro wild-keys.
  {
    SpoilerShopRow shops[160];
    uint16 sn = collect_shop_rows(s, shops, 160);
    if (sn > 0) {
      fprintf(f, "  \"shops\": [\n");
      for (uint16 i = 0; i < sn; i++) {
        const char *type_str = (shops[i].type == LOCTYPE_ShopUpgrade) ? "ShopUpgrade"
                             : (shops[i].type == LOCTYPE_TakeAny)      ? "TakeAny"
                                                                      : "Shop";
        // The location NAME is the grouping key (e.g. "Light World Kakariko
        // Shop - 1"); a bound region is emitted only when one exists —
        // regular Shop slots carry one since add-rando-shopsanity's region
        // binding, ShopUpgrade/TakeAny remain region-optional (0xFFFF).
        const char *lname = Rando_GetLocationName(shops[i].loc);
        fprintf(f, "%s    {\"location\": %u, \"name\": \"%s\", \"item\": %u, "
                   "\"type\": \"%s\"",
                i ? ",\n" : "", shops[i].loc, lname, shops[i].item, type_str);
        if (shops[i].region_id != 0xFFFF)
          fprintf(f, ", \"region\": \"%s\"", Rando_GetRegionName(shops[i].region_id));
        if (shops[i].type == LOCTYPE_ShopUpgrade)
          fprintf(f, ", \"identity_placed\": true");
        if (shops[i].has_price)
          fprintf(f, ", \"price\": %u", shops[i].price);
        fprintf(f, "}");
      }
      fprintf(f, "\n  ],\n");
    }
  }

  fprintf(f, "  \"playthrough\": [],\n");
  fprintf(f, "  \"regions\": []\n");

  fprintf(f, "}\n");
  return true;
}

// ---------------------------------------------------------------------------
// Race-mode suppressed-spoiler writer + reader.
// ---------------------------------------------------------------------------

// CRC-32 IEEE 802.3, reflected, polynomial 0xEDB88320. Same algorithm as
// zlib's crc32. Small inline implementation — no LUT to keep .data
// footprint minimal; the suppressed spoiler is < 200 bytes so perf doesn't
// matter.
static uint32 crc32_ieee(const uint8 *data, size_t len) {
  uint32 c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= (uint32)data[i];
    for (int b = 0; b < 8; b++) {
      c = (c >> 1) ^ (0xEDB88320u & (uint32)(-(int32)(c & 1u)));
    }
  }
  return c ^ 0xFFFFFFFFu;
}

static void put_u16_le(uint8 *p, uint16 v) {
  p[0] = (uint8)(v & 0xff);
  p[1] = (uint8)((v >> 8) & 0xff);
}

static void put_u32_le(uint8 *p, uint32 v) {
  p[0] = (uint8)(v & 0xff);
  p[1] = (uint8)((v >> 8) & 0xff);
  p[2] = (uint8)((v >> 16) & 0xff);
  p[3] = (uint8)((v >> 24) & 0xff);
}

static uint16 read_u16_le(const uint8 *p) {
  return (uint16)((uint16)p[0] | ((uint16)p[1] << 8));
}

static uint32 read_u32_le(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

// Serialize the suppressed-spoiler header into the on-disk layout.
// Returns the number of bytes written (always kRandoSuppressedSpoilerSize).
static size_t serialize_suppressed(const RandoSuppressedSpoiler *h,
                                   uint8 out[kRandoSuppressedSpoilerSize]) {
  memcpy(out + 0, h->magic, 4);
  put_u16_le(out + 4, h->generator_version);
  memcpy(out + 6, h->spoiler_stamp, 32);
  put_u32_le(out + 38, h->share_string_len);
  memcpy(out + 42, h->share_string, kRandoSuppressedSpoilerShareStringMax);
  memcpy(out + 106, h->settings_canonical, kRandoSuppressedSpoilerSettingsLen);
  put_u32_le(out + kRandoSuppressedSpoilerCrcOffset, h->crc32);
  return kRandoSuppressedSpoilerSize;
}

// Build + write the suppressed-spoiler file at `out_path`. Caller supplies
// the SHA-256 stamp of the full canonical JSON (computed with race_mode
// cleared to 0 and generation_wall_clock_ms cleared to 0) AND the canonical
// settings bytes (also with race_mode cleared to 0).
static bool write_suppressed_file(const char *share_string,
                                  uint16 generator_version,
                                  const uint8 stamp[32],
                                  const uint8 settings_canonical[kRandoSuppressedSpoilerSettingsLen],
                                  const char *out_path) {
  RandoSuppressedSpoiler h;
  memset(&h, 0, sizeof(h));
  memcpy(h.magic, kRandoSuppressedSpoilerMagic, 4);
  h.generator_version = generator_version;
  memcpy(h.spoiler_stamp, stamp, 32);
  size_t len = share_string ? strlen(share_string) : 0;
  if (len > kRandoSuppressedSpoilerShareStringMax) len = kRandoSuppressedSpoilerShareStringMax;
  h.share_string_len = (uint32)len;
  if (len > 0) memcpy(h.share_string, share_string, len);
  memcpy(h.settings_canonical, settings_canonical, kRandoSuppressedSpoilerSettingsLen);

  uint8 buf[kRandoSuppressedSpoilerSize];
  serialize_suppressed(&h, buf);
  // CRC over everything except the crc32 trailer itself.
  h.crc32 = crc32_ieee(buf, kRandoSuppressedSpoilerCrcOffset);
  put_u32_le(buf + kRandoSuppressedSpoilerCrcOffset, h.crc32);

  // Auto-create the spoiler directory.
  char dir_buf[512];
  if (extract_dir(out_path, dir_buf, sizeof(dir_buf))) {
    ensure_directory(dir_buf);
  }

  FILE *f = fopen(out_path, "wb");
  if (f == NULL) return false;
  size_t wrote = fwrite(buf, 1, sizeof(buf), f);
  fclose(f);
  return wrote == sizeof(buf);
}

// Stamp computation: serialize the normalized spoiler JSON and SHA-256 the
// resulting bytes. Returns true and fills `out_stamp` on success.
//
// POSIX hashes an open_memstream() buffer. Windows lacks a portable in-memory
// FILE*, so it writes `<dest_path>.stamp-tmp`, reads it back, hashes those
// bytes, and removes it. Both paths feed SHA-256 with the bytes produced by
// write_spoiler_json_stream() against the same normalized spoiler view.
//
// **Stamp normalization** — fields normalized to stable values so the
// stamp is reproducible across runs and machines:
//   - settings.race_mode → 0       (race_mode is recorded in file existence,
//                                    not in the stamped settings)
//   - generation_wall_clock_ms → 0 (varies with machine speed)
//   - forward_fill_fallback_count → 0  (varies with placer's wall-clock
//   - retry_attempts → 1               budget cutoff; reveal at a different
//                                       budget would otherwise mismatch)
// The `Place_AssumedFill` retry-attempts count is also a function of the
// per-call `budget_seconds`. Reveal MUST pass `budget_seconds = 0` (no
// wall-clock cutoff) so the placer runs to its hard 8-attempt cap; that
// makes attempts_used deterministic and lets us normalize to 1 in the
// stamp without lying about what happened on a particular machine.
//
// This normalized field list (race_mode=0, generation_wall_clock_ms=0,
// forward_fill_fallback_count=0, retry_attempts=1) MUST stay byte-for-byte in
// sync with the independent reveal-side normalization in
// src/rando/rando.c (Rando_RevealActiveSlotSpoiler regenerates the spoiler and
// applies the identical four-field normalization before re-hashing). Any
// divergence makes the regenerated bytes differ from the minted stamp and
// false-fails EVERY race reveal as "tampered". Change both sites together.
static bool compute_stamp(const RandoSpoiler *s, const char *dest_path,
                          uint8 out_stamp[32]) {
  // Build the normalized spoiler view for stamping.
  RandoSettings norm_settings = *s->settings;
  norm_settings.race_mode = 0;
  RandoSpoiler norm = *s;
  norm.settings = &norm_settings;
  norm.generation_wall_clock_ms = 0;
  // these fields depend on placer wall-clock; clear so
  // the stamp is reproducible regardless of budget_seconds or machine speed.
  norm.forward_fill_fallback_count = 0;
  norm.retry_attempts = 1;

  extern void sha256_buffer(const uint8 *data, size_t len, uint8 out[32]);

#ifndef _WIN32
  // POSIX: hash a pure in-memory buffer — no filesystem touch at all.
  (void)dest_path;
  char *mem = NULL;
  size_t mem_size = 0;
  FILE *ms = open_memstream(&mem, &mem_size);
  if (ms == NULL) return false;
  bool wrote = write_spoiler_json_stream(&norm, ms);
  // Close to flush the writer's buffered bytes into `mem` / finalize mem_size.
  if (fclose(ms) != 0) { free(mem); return false; }
  if (!wrote || mem == NULL || mem_size == 0) { free(mem); return false; }
  sha256_buffer((const uint8 *)mem, mem_size, out_stamp);
  free(mem);
  return true;
#else
  // Windows: no portable in-memory FILE*; use a scratch file beside the
  // destination, then read it back and hash the same serialized bytes.
  if (dest_path == NULL) return false;
  char tmp_path[1280];
  if (snprintf(tmp_path, sizeof(tmp_path), "%s.stamp-tmp", dest_path) >= (int)sizeof(tmp_path))
    return false;

  // Ensure the destination directory exists (the suppressed file is about to
  // be written there too) so the scratch fopen can succeed.
  char dir_buf[512];
  if (extract_dir(tmp_path, dir_buf, sizeof(dir_buf))) {
    ensure_directory(dir_buf);
  }

  // Serialize the normalized JSON to the scratch file.
  FILE *tmp = fopen(tmp_path, "wb");
  if (tmp == NULL) return false;
  bool wrote = write_spoiler_json_stream(&norm, tmp);
  if (fclose(tmp) != 0 || !wrote) { remove(tmp_path); return false; }

  // Read it back into memory and hash. Always remove() the scratch
  // file on every exit so it never leaks.
  FILE *rb = fopen(tmp_path, "rb");
  if (rb == NULL) { remove(tmp_path); return false; }
  if (fseek(rb, 0, SEEK_END) != 0) { fclose(rb); remove(tmp_path); return false; }
  long len = ftell(rb);
  if (len <= 0) { fclose(rb); remove(tmp_path); return false; }
  rewind(rb);
  uint8 *bytes = (uint8 *)malloc((size_t)len);
  if (bytes == NULL) { fclose(rb); remove(tmp_path); return false; }
  size_t got = fread(bytes, 1, (size_t)len, rb);
  fclose(rb);
  remove(tmp_path);
  if (got != (size_t)len) { free(bytes); return false; }

  sha256_buffer(bytes, (size_t)len, out_stamp);
  free(bytes);
  return true;
#endif
}

bool Spoiler_Write(const RandoSpoiler *s,
                   const char *json_path,
                   const char *txt_path) {
  if (s == NULL || s->settings == NULL || json_path == NULL) return false;

  bool race_mode = s->settings->race_mode != 0;
  if (!race_mode) {
    if (!Spoiler_WriteJson(s, json_path)) return false;
    if (txt_path != NULL) (void)Spoiler_WriteText(s, txt_path);
    return true;
  }

  // Race mode: write the suppressed binary in lieu of the full JSON.
  // `json_path` also anchors the Windows stamp scratch file.
  uint8 stamp[32];
  if (!compute_stamp(s, json_path, stamp)) return false;

  // Canonicalize settings with race_mode = 0 (per spec — stamp is over
  // the placement, not the race-mode flag).
  RandoSettings norm_settings = *s->settings;
  norm_settings.race_mode = 0;
  uint8 settings_canon[kRandoSuppressedSpoilerSettingsLen];
  Settings_CanonicalSerialize(&norm_settings, settings_canon);

  return write_suppressed_file(s->share_string,
                               (uint16)s->generator_version,
                               stamp,
                               settings_canon,
                               json_path);
  // No .txt sibling in race mode.
}

int Spoiler_ReadSuppressed(const char *path, RandoSuppressedSpoiler *out) {
  if (path == NULL || out == NULL) return -2;
  FILE *f = fopen(path, "rb");
  if (f == NULL) return -1;
  uint8 buf[kRandoSuppressedSpoilerSize];
  size_t got = fread(buf, 1, sizeof(buf), f);
  int trailing = fgetc(f);
  fclose(f);
  // The only historical size changes are append-only settings bytes:
  // 138/139/140/141 bytes carry canonical lengths 28/29/30/31. Refuse both
  // unknown sizes and a future file whose known prefix fills our buffer.
  if (trailing != EOF || got < 138 || got > kRandoSuppressedSpoilerSize)
    return -2;
  uint32 settings_len = (uint32)got - 110u;  // 106 prefix + settings + 4 CRC
  if (settings_len < 28 || settings_len > kRandoSuppressedSpoilerSettingsLen)
    return -2;
  if (memcmp(buf, kRandoSuppressedSpoilerMagic, 4) != 0) return -2;

  uint32 crc_offset = (uint32)got - 4u;
  uint32 disk_crc = read_u32_le(buf + crc_offset);
  uint32 calc_crc = crc32_ieee(buf, crc_offset);
  if (disk_crc != calc_crc) return -3;

  memset(out, 0, sizeof(*out));
  memcpy(out->magic, buf + 0, 4);
  out->generator_version = read_u16_le(buf + 4);
  memcpy(out->spoiler_stamp, buf + 6, 32);
  out->share_string_len = read_u32_le(buf + 38);
  if (out->share_string_len > kRandoSuppressedSpoilerShareStringMax) return -2;
  memcpy(out->share_string, buf + 42, kRandoSuppressedSpoilerShareStringMax);
  memcpy(out->settings_canonical, buf + 106, settings_len);
  out->crc32 = disk_crc;
  return 0;
}

// ---------------------------------------------------------------------------
// Text writer — grouped by region (per randomizer-core / "Spoiler-log emission"
// scenario: Text spoiler is grouped by region).
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Spoiler_ResolvePath — derive `<spoiler_dir>/<share_string><ext>` at runtime.
//
// Source of truth: `[Randomizer] SpoilerDir` in zelda3.ini (parsed in
// config.c). When unset (NULL), defaults to "spoilers/" (relative to CWD).
// Per the spec: the path is NOT stored in the slot — it's recomputed on
// every write/read so config changes propagate cleanly.
// ---------------------------------------------------------------------------
int Spoiler_ResolvePath(const char *share_string,
                        const char *extension_with_dot,
                        char *out_path, int out_capacity) {
  if (share_string == NULL || extension_with_dot == NULL ||
      out_path == NULL || out_capacity <= 0) {
    return 0;
  }
  const char *dir = g_config.rando_spoiler_dir;
  if (dir == NULL || *dir == '\0') dir = "spoilers";

  // Build `<dir>/<share_string><ext>` — single-path-separator-aware. We use
  // '/' uniformly (Windows fopen accepts both).
  int written = snprintf(out_path, (size_t)out_capacity, "%s/%s%s",
                         dir, share_string, extension_with_dot);
  if (written < 0 || written >= out_capacity) return 0;
  return written;
}

bool Spoiler_WriteText(const RandoSpoiler *s, const char *out_path) {
  if (s == NULL || out_path == NULL) return false;

  // Auto-create the spoiler directory if it doesn't exist. Avoids forcing
  // users to mkdir spoilers/ before the first run.
  char dir_buf[512];
  if (extract_dir(out_path, dir_buf, sizeof(dir_buf))) {
    ensure_directory(dir_buf);
  }

  FILE *f = fopen(out_path, "wb");
  if (f == NULL) return false;

  uint8 settings_hash[32];
  Settings_ComputeHash(s->settings, settings_hash);

  fprintf(f, "Zelda3 Randomizer Spoiler\n");
  fprintf(f, "=========================\n\n");
  fprintf(f, "Share string: %s\n", s->share_string != NULL ? s->share_string : "");
  fprintf(f, "Generator version: %u\n", (unsigned)s->generator_version);
  fprintf(f, "Settings hash: ");
  write_hex(f, settings_hash, 16);
  fprintf(f, "\n");
  fprintf(f, "World state: %u, Goal: %u\n",
          s->settings->world_state, s->settings->goal);
  if (spoiler_has_configurable_hint_schema(s)) {
    uint8 canon[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(s->settings, canon);
    fprintf(f, "NPC reward previews: %s (Original/US rich dialogue only)\n",
            (canon[25] & kHintNpcRewardRevealAxis_Enabled) ? "yes" : "no");
  }
  {
    KeyRingSelection key_rings;
    bool selection_valid = KeyRings_Resolve(
        s->settings, s->seed_u64, &key_rings);
    fprintf(f, "Key rings: requested %u, effective %u (selection salt v1%s)\n",
            s->settings->key_rings, Settings_EffectiveKeyRings(s->settings),
            selection_valid ? "" : ", invalid eligible set");
    fprintf(f, "Eligible key rings: 0x%04x — ", key_rings.eligible_mask);
    spoiler_write_key_ring_names_text(f, key_rings.eligible_mask);
    fprintf(f, "\nSelected key rings: 0x%04x — ", key_rings.selected_mask);
    spoiler_write_key_ring_names_text(f, key_rings.selected_mask);
    fprintf(f, "\nSkeleton Key enabled: %s (bonus only; logic never requires it)\n",
            s->settings->skeleton_key ? "yes" : "no");
  }
  fprintf(f, "Generation wall-clock: %u ms\n", (unsigned)s->generation_wall_clock_ms);
  fprintf(f, "Goal completable: %s\n\n", s->goal_completable ? "yes" : "no");

  // Fallback warnings — surface forward-fill / retry / unreachable
  // counts prominently in the text spoiler.
  if (s->forward_fill_fallback_count > 0 || s->retry_attempts > 1 ||
      s->drop_used_fallback ||
      (s->spheres != NULL && s->spheres->unreachable_count > 0)) {
    fprintf(f, "WARNINGS\n--------\n");
    if (s->forward_fill_fallback_count > 0)
      fprintf(f, "  ! Forward-fill fallback hit %u time(s) during placement.\n",
              (unsigned)s->forward_fill_fallback_count);
    if (s->retry_attempts > 1)
      fprintf(f, "  ! Placer needed %u attempts before producing a seed.\n",
              (unsigned)s->retry_attempts);
    if (s->drop_used_fallback)
      fprintf(f, "  ! Drop-pool heart floor exhausted its retries; drop shuffle "
                 "fell back to the vanilla drop table.\n");
    if (s->spheres != NULL && s->spheres->unreachable_count > 0)
      fprintf(f, "  ! %u placement(s) are unreachable in this seed.\n",
              (unsigned)s->spheres->unreachable_count);
    fprintf(f, "\n");
  }

  // Entrance shuffle mappings (omitted when not shuffled). Mirrors the
  // JSON entrance_mapping / dungeon_entrance_mapping sections.
  if ((s->entrance_assign != NULL && s->entrance_count > 0) ||
      (s->dungeon_assign != NULL && s->dungeon_count > 0) ||
      (s->cross_assign != NULL && s->cross_count > 0)) {
    fprintf(f, "ENTRANCE SHUFFLE\n----------------\n");
    Entrance_WriteSpoilerText(f, s->entrance_assign, s->entrance_count);
    Entrance_WriteDungeonSpoilerText(f, s->dungeon_assign, s->dungeon_count);
    Entrance_WriteCrossSpoilerText(f, s->cross_assign, s->cross_count);
    Entrance_WriteDecoupledSpoilerText(f, s->decoupled_assign, s->decoupled_count);
    Entrance_WriteDungeonDecoupledSpoilerText(f, s->dun_decoupled_assign, s->dun_decoupled_count);
    Entrance_WriteCrossDecoupledSpoilerText(f, s->cross_decoupled_assign, s->cross_decoupled_count);
    fprintf(f, "\n");
  }
  spoiler_write_chains_text(f, s);

  // Boss assignments (omitted unless boss_shuffle is on).
  if (s->boss_assignment != NULL) {
    fprintf(f, "BOSS ASSIGNMENTS\n----------------\n");
    for (uint8 d = 1; d <= 12; d++) {
      uint8 idx = s->boss_assignment[d];
      if (idx == 0xFF) continue;
      fprintf(f, "  %-20s : %s\n", BossShuffle_DungeonName(d),
              BossShuffle_BossName(idx));
    }
    fprintf(f, "\n");
  }

  // Drop tables (omitted unless drop_shuffle is on). Each pack is the
  // 8 resolved drop item ids; pack 0 is the heart-floor pack.
  if (s->drop_map != NULL) {
    fprintf(f, "DROP TABLES (item ids, pack 0 = heart floor)\n");
    fprintf(f, "--------------------------------------------\n");
    for (uint8 pack = 0; pack < 7; pack++) {
      fprintf(f, "  pack %u:", (unsigned)pack);
      for (uint8 slot = 0; slot < 8; slot++) {
        uint8 flat = (uint8)(pack * 8 + slot);
        fprintf(f, " %u", (unsigned)Sprite_VanillaPrizeItem(s->drop_map[flat]));
      }
      fprintf(f, "\n");
    }
    fprintf(f, "\n");
  }

  write_medallion_requirements_text(f, s->medallion_assignment);

  fprintf(f, "Placements (grouped by region):\n");
  fprintf(f, "-------------------------------\n");
  if (s->placements != NULL && s->placements->count > 0) {
    uint16 n = s->placements->count;
    if (n > kRandoLocationCapacity) n = kRandoLocationCapacity;
    // Look up region_id for each placement via kRandoLocations.
    // Then sort by (region_id, location_id) and emit grouped sections.
    static struct {
      uint16 region_id;
      uint16 location_id;
      uint16 item_id;
    } rows[kRandoLocationCapacity];
    uint16 row_n = 0;
    for (uint16 i = 0; i < n; i++) {
      uint16 loc_id = s->placements->entries[i].location_id;
      if (spoiler_loc_is_medallion_config(loc_id)) continue;
      if (spoiler_item_is_empty_pot(s->placements->entries[i].item_id)) continue;
      rows[row_n].location_id = loc_id;
      rows[row_n].item_id = s->placements->entries[i].item_id;
      rows[row_n].region_id = 0xFFFF;
      const RandoLocationDef *d = find_location(rows[row_n].location_id);
      if (d != NULL) {
        rows[row_n].region_id = d->region_id;
        // under Inverted, a location may be assigned to a different
        // region via Rando_FindPredicateOverride. Honor that override here so the
        // grouped spoiler text matches the runtime's reachability view (e.g.,
        // Ether Tablet shows under LightWorld_DeathMountain_East, not the base
        // West region).
        if (s->settings != NULL) {
          const RandoLocationPredOverride *ov =
              Rando_FindPredicateOverride(rows[row_n].location_id,
                                          s->settings->world_state);
          if (ov != NULL && ov->region_override != 0xFFFF) {
            rows[row_n].region_id = ov->region_override;
          }
        }
      }
      row_n++;
    }
    // Insertion sort by (region_id, location_id) — n is small (~237).
    for (uint16 i = 1; i < row_n; i++) {
      uint16 j = i;
      while (j > 0 &&
             (rows[j - 1].region_id > rows[j].region_id ||
              (rows[j - 1].region_id == rows[j].region_id &&
               rows[j - 1].location_id > rows[j].location_id))) {
        // swap
        uint16 tr = rows[j - 1].region_id, tl = rows[j - 1].location_id, ti = rows[j - 1].item_id;
        rows[j - 1].region_id = rows[j].region_id;
        rows[j - 1].location_id = rows[j].location_id;
        rows[j - 1].item_id = rows[j].item_id;
        rows[j].region_id = tr; rows[j].location_id = tl; rows[j].item_id = ti;
        j--;
      }
    }
    uint16 cur_region = 0;
    bool first_section = true;
    for (uint16 i = 0; i < row_n; i++) {
      if (first_section || rows[i].region_id != cur_region) {
        if (!first_section) fprintf(f, "\n");
        first_section = false;
        cur_region = rows[i].region_id;
        const char *rname = Rando_GetRegionName(cur_region);
        fprintf(f, "[%s]\n", rname);
      }
      const char *lname = Rando_GetLocationName(rows[i].location_id);
      const char *iname = Rando_GetItemName(rows[i].item_id);
      fprintf(f, "  %-44s -> %s\n", lname, iname);
    }
  }
  fprintf(f, "\n");

  // Shops (tasks §8.1) — a dedicated grouping of the Retro shop / capacity-
  // upgrade / take-any slots. These also appear under their region headings
  // above; this section re-lists them grouped by shop for at-a-glance reading
  // and flags the identity-placed Capacity Upgrade slots. Emitted only when
  // shop-class locations are actually placed (i.e. Retro seeds), so non-Retro
  // spoilers are byte-unchanged. This is TEXT-spoiler only: the .txt is never
  // written in race mode and is not part of the race-mode stamp (computed over
  // the JSON), so this section cannot move any placement / sphere digest or stamp.
  {
    SpoilerShopRow shops[160];
    uint16 sn = collect_shop_rows(s, shops, 160);
    if (sn > 0) {
      fprintf(f, "Shops:\n");
      fprintf(f, "------\n");
      char cur_shop[64];
      cur_shop[0] = '\0';
      for (uint16 i = 0; i < sn; i++) {
        const char *lname = Rando_GetLocationName(shops[i].loc);
        // Group heading = the location name with the trailing " - <slot>" suffix
        // stripped (e.g. "Light World Kakariko Shop - 1" -> "Light World Kakariko
        // Shop"). Capacity-upgrade / take-any names follow the same shape.
        char shop_name[64];
        size_t k = 0;
        for (const char *p = lname; *p && k < sizeof(shop_name) - 1; p++) {
          if (p[0] == ' ' && p[1] == '-' && p[2] == ' ') break;  // slot separator
          shop_name[k++] = *p;
        }
        shop_name[k] = '\0';
        if (strcmp(shop_name, cur_shop) != 0) {
          if (cur_shop[0] != '\0') fprintf(f, "\n");
          fprintf(f, "[%s]\n", shop_name);
          strncpy(cur_shop, shop_name, sizeof(cur_shop) - 1);
          cur_shop[sizeof(cur_shop) - 1] = '\0';
        }
        const char *iname = Rando_GetItemName(shops[i].item);
        const char *tag =
            (shops[i].type == LOCTYPE_ShopUpgrade) ? "  [identity-placed]" : "";
        if (shops[i].has_price)
          fprintf(f, "  %-44s -> %s (%u rupees)%s\n", lname, iname,
                  shops[i].price, tag);
        else
          fprintf(f, "  %-44s -> %s%s\n", lname, iname, tag);
      }
      fprintf(f, "\n");
    }
  }

  // Hints — mirrors the caller-owned JSON plan and delivery rows. Mutable
  // discovery/resolution/paid-cursor state is deliberately not serialized.
  {
    const RandoHintPlan *plan = s->hint_plan;
    uint8 fact_count = plan != NULL ? plan->fact_count : 0;
    if (fact_count > kRandoHintPlanMaxFacts)
      fact_count = kRandoHintPlanMaxFacts;
    if (plan != NULL) {
      uint8 primary_count = spoiler_hint_primary_count(plan);
      if (primary_count > fact_count) primary_count = fact_count;
      SpoilerHintPolicy policy = spoiler_hint_policy(plan);
      SpoilerHintTopology topology = spoiler_hint_topology(plan);
      fprintf(f, "Hints:\n");
      fprintf(f, "------\n");
      fprintf(f, "  Plan: algorithm %u, text schema %u, digest ",
              (unsigned)plan->algorithm_version,
              (unsigned)plan->text_schema_version);
      write_hex(f, plan->digest, kRandoHintPlanDigestBytes);
      fprintf(f, "\n  Facts: %u primary, %u reserve\n",
              (unsigned)primary_count,
              (unsigned)(fact_count - primary_count));
      if (spoiler_has_configurable_hint_schema(s)) {
        fprintf(f, "  Policy: profile %s, tiles %u, paid depth %u, mix %s\n",
                policy.profile, (unsigned)policy.tile_count,
                (unsigned)policy.paid_depth, policy.mix);
        fprintf(f, "  Sources: %u total, %u tile, %u paid services, "
                   "%u paid facts\n",
                (unsigned)(topology.tile_source_count +
                           topology.paid_source_count),
                (unsigned)topology.tile_source_count,
                (unsigned)topology.paid_source_count,
                (unsigned)topology.paid_fact_count);
      }

      for (uint8 i = 0; i < fact_count; i++) {
        const RandoHintFact *fact = &plan->facts[i];
        SpoilerHintAssignment assignment;
        bool assigned =
            spoiler_find_hint_assignment(plan, fact->fact_id, &assignment);
        const char *npc_str =
            assigned ? Rando_GetHintNpcStringId(assignment.npc) : "unassigned";
        fprintf(f, "  [%02u] %-36s %s",
                (unsigned)fact->fact_id,
                npc_str != NULL ? npc_str : "unassigned",
                fact->primary ? "primary" : "reserve");
        if (assigned && assignment.paid)
          fprintf(f, " queue %u", (unsigned)assignment.queue_index);
        else if (assigned)
          fprintf(f, " tile %u", (unsigned)assignment.tile_index);
        fprintf(f, " | %s/%s/template:%s",
                spoiler_hint_fact_kind_name(fact->kind),
                fact->location_id != 0xFFFFu && fact->item_id != 0xFFFFu
                    ? "exact"
                    : "positive_summary",
                spoiler_hint_template_name(fact->template_id));
        fprintf(f, " parameter:%u",
                (unsigned)fact->template_parameter);
        if (fact->location_id != 0xFFFFu)
          fprintf(f, " location:%u", (unsigned)fact->location_id);
        if (fact->item_id != 0xFFFFu)
          fprintf(f, " item:%u", (unsigned)fact->item_id);
        if (fact->region_id != 0xFFFFu)
          fprintf(f, " region:%u", (unsigned)fact->region_id);
        fprintf(f, "\n       %s\n", fact->text);
      }
      if (plan->murahdahla_active) {
        const char *npc_str =
            Rando_GetHintNpcStringId(kRandoHintNpc_Murahdahla);
        fprintf(f, "  [compat] %-31s %s\n",
                npc_str != NULL ? npc_str : "murahdahla",
                plan->murahdahla_text);
      }
      fprintf(f, "\n");
    }
  }

  fclose(f);
  return true;
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_spoiler)
