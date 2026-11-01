// rando_logic.c — predicate VM (tasks.md §3.7).
//
// Decodes the bytecode stream produced by assets/rando_logic_gen.py.
// Format reference lives in rando_logic.h and the codegen docstring.
//
// Determinism: no rand, no time, no float. Iteration over operand lists is
// linear scan. The recursive evaluator's depth is bounded by the codegen's
// inline-complexity check (~6 ops per macro after expansion).

#include "rando_logic.h"
#include "alttpr_import.h"
#include "enemy_drop_lookup.h"
#include "enemy_check_lookup.h"
#include "rando.h"
#include "rando_placement.h"
#include "dungeon_ids.h"
#include "item_ids.h"  // ITEM_GenericKey / ITEM_SmallKey_* (genericKeys collapse)
#include "pot_nonpot_drop_counts.h"
#include "location_ids.h"
#include "shuffle_doors.h"  // door-shuffle oracle (OP_DOORS_LOC_REACHABLE)
#include "soul_tables.h"    // kBossPoolSoul (add-enemy-souls boss-soul gate)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Read helpers — every multi-byte read is explicit LE per byte-order pin.
// ---------------------------------------------------------------------------
static inline uint16 read_u16le(const uint8 *p) {
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

// ---------------------------------------------------------------------------
// Internal evaluator. Walks the bytecode tree recursively.
//
// Caller passes a cursor that the evaluator advances. Each top-level call
// reads exactly one predicate (i.e., one opcode + its operands + any nested
// children).
//
// Returns the boolean result. On malformed bytecode (unknown op, ran off the
// end), returns false in release and asserts in debug.
// ---------------------------------------------------------------------------

typedef struct Cursor {
  const uint8 *p;
  const uint8 *end;
  bool error;
} Cursor;

static bool cursor_ok(const Cursor *c, size_t need) {
  return !c->error && (size_t)(c->end - c->p) >= need;
}

static uint8 cursor_u8(Cursor *c) {
  if (!cursor_ok(c, 1)) { c->error = true; return 0; }
  return *c->p++;
}

static uint16 cursor_u16le(Cursor *c) {
  if (!cursor_ok(c, 2)) { c->error = true; return 0; }
  uint16 v = read_u16le(c->p);
  c->p += 2;
  return v;
}

static bool eval(Cursor *c, const PredicateContext *ctx);

// genericKeys (Retro) small-key collapse — a direct port of ALTTPR
// ItemCollection::has() (app/Support/ItemCollection.php:271-273): when generic
// keys are in effect, ANY per-dungeon small-key requirement (regardless of the
// requested amount) is satisfied by holding >=1 GenericKey. Big keys, maps and
// compasses are unaffected (they keep per-dungeon identity under Retro). The
// per-dungeon SmallKey ids are 53..65 (item_registry.yaml, contiguous
// ITEM_SmallKey_HyruleCastleEscape..ITEM_SmallKey_GanonsTower). When generic
// keys are NOT active the caller falls through to the normal per-dungeon count,
// so non-Retro evaluation is byte-identical.
uint16 Logic_EffectiveItemCount(const RandoCounts *counts,
                                const RandoSettings *settings,
                                uint16 item_id) {
  if (counts == NULL || item_id >= 256) return 0;
  if (item_id == ITEM_SkeletonKey) return 0;  // bonus-only; never logic
  if (!Rando_IsSmallKeyItem(item_id)) return counts->by_item_id[item_id];
  if (Settings_GenericKeysActive(settings))
    return counts->by_item_id[ITEM_GenericKey] != 0 ? 0xFFFFu : 0;
  uint8 d = Rando_RandoDungeonFromDungeonItem(item_id);
  uint16 ring = Rando_KeyRingItemForRandoDungeon(d);
  if (ring < 256 && counts->by_item_id[ring] != 0) return 0xFFFFu;
  return counts->by_item_id[item_id];
}

static bool eval_has_item(Cursor *c, const PredicateContext *ctx) {
  uint16 item_id = cursor_u16le(c);
  if (c->error || item_id >= 256) return false;
  return Logic_EffectiveItemCount(ctx->counts, ctx->settings, item_id) >= 1;
}

static bool eval_has_amount(Cursor *c, const PredicateContext *ctx) {
  uint16 item_id = cursor_u16le(c);
  uint8 n = cursor_u8(c);
  if (c->error || item_id >= 256) return false;
  return Logic_EffectiveItemCount(ctx->counts, ctx->settings, item_id) >= n;
}

static bool eval_has_any_of(Cursor *c, const PredicateContext *ctx) {
  uint8 count = cursor_u8(c);
  bool result = false;
  for (uint8 i = 0; i < count; i++) {
    uint16 item_id = cursor_u16le(c);
    if (c->error || item_id >= 256) { result = false; continue; }
    // genericKeys (Retro) small-key collapse — mirror eval_has_item /
    // eval_has_amount: any per-dungeon SmallKey requirement is satisfied by
    // holding >=1 GenericKey. Inert under default settings (no author uses
    // HAS_ANY for a SmallKey today) but kept consistent so a future logic_parts
    // file that lists a SmallKey in HAS_ANY can't desync from the runtime pool.
    uint16 count_value = Logic_EffectiveItemCount(
        ctx->counts, ctx->settings, item_id);
    if (count_value >= 1) result = true;
  }
  return result;
}

static bool eval_has_any_count(Cursor *c, const PredicateContext *ctx) {
  uint8 count = cursor_u8(c);
  // Sum the counts across all ids, then compare to threshold.
  uint32 sum = 0;
  bool generic = Settings_GenericKeysActive(ctx->settings);
  bool added_generic = false;
  for (uint8 i = 0; i < count; i++) {
    uint16 item_id = cursor_u16le(c);
    if (c->error || item_id >= 256) continue;
    // genericKeys (Retro) small-key collapse — fold every per-dungeon SmallKey
    // id in the list into the single shared GenericKey count, added at most
    // once so multiple SmallKey ids don't multiply-count the shared pool.
    // Inert under default settings (no author uses HAS_ANY for a SmallKey).
    if (generic && Rando_IsSmallKeyItem(item_id)) {
      if (!added_generic) {
        sum += ctx->counts->by_item_id[ITEM_GenericKey];
        added_generic = true;
      }
      continue;
    }
    sum += Logic_EffectiveItemCount(ctx->counts, ctx->settings, item_id);
  }
  uint8 n = cursor_u8(c);
  if (c->error) return false;
  return sum >= n;
}

static bool eval_worldstate_eq(Cursor *c, const PredicateContext *ctx) {
  uint8 ws = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return ctx->settings->world_state == ws;
}

static bool eval_goal_eq(Cursor *c, const PredicateContext *ctx) {
  uint8 g = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return ctx->settings->goal == g;
}

static bool eval_goal_requires_dungeon(Cursor *c, const PredicateContext *ctx) {
  uint8 d = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  if (d >= kRandoDungeonCount) return false;
  // Phase A goal semantics: All Dungeons requires every dungeon. Completionist
  // requires every dungeon whose locations are non-empty. Fast Ganon / Ganon
  // do NOT require dungeon clears (only crystals.ganon). Triforce-Hunt /
  // Ganon-Hunt require no specific dungeons.
  switch (ctx->settings->goal) {
    case kGoal_Dungeons:
    case kGoal_Completionist:
      return true;
    case kGoal_Ganon:
    case kGoal_FastGanon:
    case kGoal_GanonHunt:
    case kGoal_Pedestal:
    case kGoal_TriforceHunt:
    default:
      return false;
  }
}

static bool eval_dungeon_cleared(Cursor *c, const PredicateContext *ctx) {
  uint8 d = cursor_u8(c);
  if (c->error) return false;
  if (d >= 64) return false;  // bitmask is 64 bits
  return (ctx->cleared_dungeons_bitmask >> d) & 1;
}

static bool eval_region_reachable(Cursor *c, const PredicateContext *ctx) {
  uint16 region_id = cursor_u16le(c);
  if (c->error) return false;
  // OP_REGION_REACHABLE's operand IS a region id (the Phase A `RegionRemap`
  // indirection was retired in Phase C — it was identity dead code that would
  // have corrupted this hot predicate if ever populated; see design.md §1).
  // Phase A0: if no reachability bitset has been supplied (e.g., a standalone
  // Predicate_Evaluate call outside of Logic_ComputeReachabilityFullKnowledge), conservatively
  // return false. The placer / tracker pass a populated bitset.
  if (ctx->reachable_regions_bitset == NULL) return false;
  if (region_id >= ctx->reachable_regions_count) return false;
  uint16 byte_idx = region_id >> 3;
  uint8 bit_idx = region_id & 7;
  return (ctx->reachable_regions_bitset[byte_idx] >> bit_idx) & 1;
}

static bool eval_has_prize(Cursor *c, const PredicateContext *ctx) {
  uint8 prize_id = cursor_u8(c);
  if (c->error || ctx->dungeon_prize_assignment == NULL) return false;
  // OP_HAS_PRIZE p evaluates true iff the dungeon currently holding prize p
  // (per the shuffle assignment) is in cleared_dungeons.
  for (uint8 d = 0; d < kRandoDungeonCount; d++) {
    if (ctx->dungeon_prize_assignment[d] == prize_id) {
      return (ctx->cleared_dungeons_bitmask >> d) & 1;
    }
  }
  return false;
}

static bool eval_medallion_opens(Cursor *c, const PredicateContext *ctx) {
  uint8 entrance_id = cursor_u8(c);
  if (c->error) return false;
  if (entrance_id >= kRandoMedallionEntranceCount) return false;
  if (ctx->medallion_entrance_assignment == NULL) return false;
  uint16 medallion_item_id = ctx->medallion_entrance_assignment[entrance_id];
  if (medallion_item_id >= 256) return false;
  return ctx->counts->by_item_id[medallion_item_id] >= 1;
}

static bool eval_item_is(Cursor *c, const PredicateContext *ctx) {
  uint16 item_id = cursor_u16le(c);
  if (c->error) return false;
  // Well-formedness pass in codegen rejects OP_ITEM_IS outside can_place; in
  // release we still guard against bad bytecode reaching here.
  assert(ctx->placement_context && "OP_ITEM_IS only valid in placement context");
  if (!ctx->placement_context) return false;
  if (ctx->candidate_item == item_id) return true;
  bool target_key = Rando_IsSmallKeyItem(item_id);
  bool target_ring = Rando_IsKeyRingItem(item_id);
  bool candidate_key = Rando_IsSmallKeyItem(ctx->candidate_item);
  bool candidate_ring = Rando_IsKeyRingItem(ctx->candidate_item);
  if (!((target_key && candidate_ring) || (target_ring && candidate_key)))
    return false;
  uint8 target_d = Rando_RandoDungeonFromDungeonItem(item_id);
  uint8 candidate_d = Rando_RandoDungeonFromDungeonItem(ctx->candidate_item);
  if (target_d >= kRandoDungeon_Count || target_d != candidate_d) return false;
  return (ctx->selected_key_rings_mask & (uint16)(1u << target_d)) != 0;
}

static bool eval_not(Cursor *c, const PredicateContext *ctx) {
  return !eval(c, ctx);
}

// ---------------------------------------------------------------------------
// skip_pred — structurally advance the cursor past ONE predicate node without
// evaluating it. This is what lets AND/OR short-circuit: a decided composite
// still has to move the cursor past its remaining children (the bytecode has
// no length prefixes), but skipping only DECODES operand layout — it never
// re-enters boss-kill predicates or the door-shuffle oracle, which is where
// the evaluation cost lives (short-circuiting cut the slowest door corpus
// seed from ~86s to well under the 60s runner budget).
//
// MUST mirror the operand reads of the eval_* handler for every op — a
// mismatch desyncs the cursor and corrupts the enclosing predicate.
// Logic_SelfCheck walks EVERY predicate blob in the generated tables with
// skip_pred and asserts exact-length consumption, so a divergence (e.g. a new
// op added to eval() but not here) fails the selfcheck before it can ship.
// ---------------------------------------------------------------------------
static void skip_pred(Cursor *c) {
  uint8 op = cursor_u8(c);
  if (c->error) return;
  switch (op) {
    // no operands
    case OP_INSTANT_FLUTE:
    case OP_OW_FLUTE_VANILLA:
    case OP_NPC_SOULS_ACTIVE:
    case OP_TOWER_CRYSTALS_MET:
    case OP_POT_KEYS_ON:
    case OP_POT_KEYS_WILD:
    case OP_POT_KEYS_DUNGEON:
    case OP_ENEMY_DROP_KEYS_DUNGEON:
    case OP_ENEMY_DROP_KEYS_WILD:
      return;
    // one u8 operand
    case OP_WORLDSTATE_EQ:
    case OP_GOAL_EQ:
    case OP_GOAL_REQUIRES_DUNGEON:
    case OP_DUNGEON_CLEARED:
    case OP_HAS_PRIZE:
    case OP_MEDALLION_OPENS:
    case OP_TRICK:
    case OP_DIFFICULTY_AT_LEAST:
    case OP_GLITCH_LEVEL_AT_LEAST:
    case OP_MODEWEAPONS_EQ:
    case OP_CAN_KILL_BOSS:
    case OP_DOORS_ACTIVE:
    case OP_SOULS_TIER_AT_LEAST:
      (void)cursor_u8(c);
      return;
    // one u16 operand
    case OP_HAS_ITEM:
    case OP_REGION_REACHABLE:
    case OP_ITEM_IS:
    case OP_DOORS_LOC_REACHABLE:
      (void)cursor_u16le(c);
      return;
    case OP_HAS_AMOUNT:  // u16 item + u8 amount
      (void)cursor_u16le(c);
      (void)cursor_u8(c);
      return;
    case OP_HAS_ANY_OF: {  // u8 count + count*u16
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) (void)cursor_u16le(c);
      return;
    }
    case OP_HAS_ANY_COUNT: {  // u8 count + count*u16 + u8 threshold
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) (void)cursor_u16le(c);
      (void)cursor_u8(c);
      return;
    }
    case OP_NOT:
      skip_pred(c);
      return;
    case OP_AND:
    case OP_OR: {
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) skip_pred(c);
      return;
    }
    default:
      assert(0 && "unknown predicate op (skip_pred)");
      c->error = true;
      return;
  }
}

// Structural audit companion for Key Ring stock metadata. Record every
// authored small-key threshold while walking the same bytecode grammar as
// skip_pred; Logic_SelfCheck compares the maxima with the runtime grant stock.
// This makes a future YAML threshold increase fail loudly instead of leaving a
// ring logically saturated but unable to pay the corresponding real doors.
static void audit_key_thresholds(Cursor *c,
                                 uint8 out_max[kRandoDungeon_Count]) {
  uint8 op = cursor_u8(c);
  if (c->error) return;
  switch (op) {
    case OP_HAS_ITEM: {
      uint16 item = cursor_u16le(c);
      uint8 d = Rando_IsSmallKeyItem(item)
                    ? Rando_RandoDungeonFromDungeonItem(item)
                    : (uint8)kRandoDungeon_None;
      if (d < kRandoDungeon_Count && out_max[d] < 1) out_max[d] = 1;
      return;
    }
    case OP_HAS_AMOUNT: {
      uint16 item = cursor_u16le(c);
      uint8 amount = cursor_u8(c);
      uint8 d = Rando_IsSmallKeyItem(item)
                    ? Rando_RandoDungeonFromDungeonItem(item)
                    : (uint8)kRandoDungeon_None;
      if (d < kRandoDungeon_Count && out_max[d] < amount)
        out_max[d] = amount;
      return;
    }
    case OP_HAS_ANY_OF: {
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) {
        uint16 item = cursor_u16le(c);
        uint8 d = Rando_IsSmallKeyItem(item)
                      ? Rando_RandoDungeonFromDungeonItem(item)
                      : (uint8)kRandoDungeon_None;
        if (d < kRandoDungeon_Count && out_max[d] < 1) out_max[d] = 1;
      }
      return;
    }
    case OP_HAS_ANY_COUNT: {
      uint8 n = cursor_u8(c);
      uint16 families = 0;
      for (uint8 i = 0; i < n && !c->error; i++) {
        uint16 item = cursor_u16le(c);
        uint8 d = Rando_IsSmallKeyItem(item)
                      ? Rando_RandoDungeonFromDungeonItem(item)
                      : (uint8)kRandoDungeon_None;
        if (d < kRandoDungeon_Count) families |= (uint16)(1u << d);
      }
      uint8 amount = cursor_u8(c);
      for (uint8 d = 0; d < kRandoDungeon_Count; d++)
        if ((families & (uint16)(1u << d)) && out_max[d] < amount)
          out_max[d] = amount;
      return;
    }
    case OP_NOT:
      audit_key_thresholds(c, out_max);
      return;
    case OP_AND:
    case OP_OR: {
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++)
        audit_key_thresholds(c, out_max);
      return;
    }
    default:
      // Rewind the opcode and use the already selfcheck-covered structural
      // decoder for every leaf that cannot carry a key threshold.
      c->p--;
      skip_pred(c);
      return;
  }
}

static bool predicate_reachability_monotone(Cursor *c, bool negated) {
  uint8 op = cursor_u8(c);
  if (c->error) return false;
  switch (op) {
    case OP_HAS_ITEM:
    case OP_REGION_REACHABLE:
    case OP_DOORS_LOC_REACHABLE:
      (void)cursor_u16le(c);
      return !c->error && !negated;
    case OP_HAS_AMOUNT:
      (void)cursor_u16le(c);
      (void)cursor_u8(c);
      return !c->error && !negated;
    case OP_HAS_ANY_OF: {
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) (void)cursor_u16le(c);
      return !c->error && !negated;
    }
    case OP_HAS_ANY_COUNT: {
      uint8 n = cursor_u8(c);
      for (uint8 i = 0; i < n && !c->error; i++) (void)cursor_u16le(c);
      (void)cursor_u8(c);
      return !c->error && !negated;
    }
    case OP_DUNGEON_CLEARED:
    case OP_HAS_PRIZE:
    case OP_MEDALLION_OPENS:
    case OP_CAN_KILL_BOSS:
      (void)cursor_u8(c);
      return !c->error && !negated;
    case OP_NOT:
      return predicate_reachability_monotone(c, !negated);
    case OP_AND:
    case OP_OR: {
      uint8 n = cursor_u8(c);
      bool ok = true;
      for (uint8 i = 0; i < n && !c->error; i++) {
        if (!predicate_reachability_monotone(c, negated))
          ok = false;
      }
      return ok && !c->error;
    }
    case OP_TOWER_CRYSTALS_MET:
      // Counts crystals HELD (inventory-dependent): monotone un-negated but
      // anti-monotone under OP_NOT — it does NOT belong in the
      // settings-constant negation-immune group below.
      return !c->error && !negated;
    case OP_INSTANT_FLUTE:
    case OP_OW_FLUTE_VANILLA:
    case OP_NPC_SOULS_ACTIVE:
    case OP_POT_KEYS_ON:
    case OP_POT_KEYS_WILD:
    case OP_POT_KEYS_DUNGEON:
    case OP_ENEMY_DROP_KEYS_DUNGEON:
    case OP_ENEMY_DROP_KEYS_WILD:
      return true;
    case OP_WORLDSTATE_EQ:
    case OP_GOAL_EQ:
    case OP_GOAL_REQUIRES_DUNGEON:
    case OP_TRICK:
    case OP_DIFFICULTY_AT_LEAST:
    case OP_GLITCH_LEVEL_AT_LEAST:
    case OP_MODEWEAPONS_EQ:
    case OP_DOORS_ACTIVE:
    case OP_SOULS_TIER_AT_LEAST:
      (void)cursor_u8(c);
      return !c->error;
    case OP_ITEM_IS:
      (void)cursor_u16le(c);
      return !c->error;
    default:
      assert(0 && "unknown predicate op (monotone scan)");
      c->error = true;
      return false;
  }
}

static bool predicate_blob_reachability_monotone(uint32 off, uint16 len) {
  if (len == 0) return true;
  Cursor c = { kRandoPredicateStream + off, kRandoPredicateStream + off + len, false };
  bool ok = predicate_reachability_monotone(&c, false);
  return ok && !c.error && c.p == c.end;
}

static bool eval_and(Cursor *c, const PredicateContext *ctx) {
  uint8 count = cursor_u8(c);
  bool result = true;
  for (uint8 i = 0; i < count; i++) {
    // Short-circuit: once false, the remaining children can't change the
    // result — skip their bytecode structurally instead of evaluating it.
    // (Handlers are pure reads of ctx, so skipping is result-identical;
    // skip_pred's layout table is selfcheck-validated against every blob.)
    if (result) {
      if (!eval(c, ctx)) result = false;
    } else {
      skip_pred(c);
    }
  }
  return result;
}

static bool eval_or(Cursor *c, const PredicateContext *ctx) {
  uint8 count = cursor_u8(c);
  bool result = false;
  for (uint8 i = 0; i < count; i++) {
    if (!result) {
      if (eval(c, ctx)) result = true;
    } else {
      skip_pred(c);
    }
  }
  return result;
}

// Phase B trick gate. Phase A pinned settings.tricks=0, so the bit test
// always failed. Slice 4 wires the bit test against the 8-bit
// settings.tricks bitfield (op_registry.yaml `tricks:` section enumerates
// the bit positions). Predicates referencing trick_id >= 8 are rejected
// by the codegen well-formedness pass; the runtime guard returns false
// defensively.
static bool eval_trick(Cursor *c, const PredicateContext *ctx) {
  uint8 trick_id = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  if (trick_id >= 8) return false;
  return (ctx->settings->tricks & (uint8)(1u << trick_id)) != 0;
}
static bool eval_difficulty(Cursor *c, const PredicateContext *ctx) {
  uint8 level = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return ctx->settings->item_pool_difficulty >= level;
}
static bool eval_glitch(Cursor *c, const PredicateContext *ctx) {
  uint8 level = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return ctx->settings->logic >= level;
}
// Phase B swordless — settings.mode_weapons == operand. The Ganon/Agahnim/
// CanMeltThings predicates branch on OP_MODEWEAPONS_EQ(swordless); under the
// default (randomized) it is false so those swordless disjuncts are inert.
static bool eval_modeweapons_eq(Cursor *c, const PredicateContext *ctx) {
  uint8 mw = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return ctx->settings->mode_weapons == mw;
}
// add-enemy-souls — settings souls tier >= operand tier. Soul-item
// requirements wrap in (NOT SOULS_TIER_AT_LEAST(t)) OR HAS_ITEM(Soul_X), so
// they collapse to true below the tier (default Off = pre-souls reachability).
// EFFECTIVE tier (enemies degrades to Bosses under door shuffle) — must match
// the pool, the placer fail-closed gate, and the runtime suppression.
static bool eval_souls_tier(Cursor *c, const PredicateContext *ctx) {
  uint8 tier = cursor_u8(c);
  if (c->error || ctx->settings == NULL) return false;
  return Settings_EffectiveSoulsShuffle(ctx->settings) >= tier;
}

static bool eval_instant_flute(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  return ctx->settings != NULL && ctx->settings->instant_flute != 0;
}

// add-rando-ow-warp-shuffle — the CanFly neutralization conjunct: true iff
// flute spots are VANILLA (flute_shuffle off). NULL-settings contexts are
// vanilla by definition. Must read the EFFECTIVE axis: the VM is handed the raw
// settings struct, so the Inverted force-off apply_derived_rules applies to the
// canonical copy is not visible here. Reading the raw field made Inverted's
// CanFly uses evaluate as shuffled — the opposite of the intent.
static bool eval_ow_flute_vanilla(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  return ctx->settings == NULL ||
         Settings_EffectiveFluteShuffle(ctx->settings) == kFluteShuffle_Off;
}

// add-npc-souls — the npc_souls toggle (no operands; no derived rules).
static bool eval_npc_souls_active(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  return ctx->settings != NULL && ctx->settings->npc_souls != 0;
}

// add-rando-random-crystals — the installed per-seed resolved tower count
// (+1; 0 = none installed). Same installed-store pattern as the prize /
// medallion / boss assignments: generation entries (Place_AssumedFill,
// Goal_IsCompletable, the spoiler) and the runtime (activation, cold
// replay, the tracker bridge) install it; deactivation clears it.
static uint8 g_logic_resolved_tower_plus1;

void Logic_SetResolvedTowerCrystals(uint8 count_plus1) {
  g_logic_resolved_tower_plus1 = count_plus1;
}

// add-rando-random-crystals — crystals held >= the RESOLVED crystals.tower
// requirement (no operands). Builders that know the seed set
// tower_crystals_required_plus1; otherwise fall back to the settings value
// clamped to 7 so the random sentinel (8) reads as the conservative 7 —
// zero-initialized/standalone contexts therefore stay fail-CLOSED and the
// default (tower=7) is byte-identical to the old hardcoded term.
static bool eval_tower_crystals_met(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  uint8 need;
  if (ctx->tower_crystals_required_plus1 != 0) {
    need = (uint8)(ctx->tower_crystals_required_plus1 - 1);
  } else {
    need = ctx->settings != NULL ? ctx->settings->crystals_tower : 7;
    if (need > 7) need = 7;
  }
  if (need == 0) return true;
  uint16 have = 0;
  for (uint16 id = ITEM_Prize_Crystal1; id <= ITEM_Prize_Crystal7; id++) {
    if (ctx->counts != NULL && ctx->counts->by_item_id[id] != 0) have++;
  }
  return have >= need;
}

static bool eval_pot_keys_on(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  // Mirrors pot_active() (rando_placement.c): dungeon pot keys are live checks
  // only when pot_shuffle itemizes them — Settings_PotKeysActive: pot_shuffle >=
  // Keys AND pots are not forced off (CAVE-ENTRANCE shuffle, the same
  // predicate pot_active uses). When false, the wrapped small-key term collapses
  // to its vanilla worst-case, so default + pots-off placement is byte-identical.
  return Settings_PotKeysActive(ctx->settings);
}

static bool eval_pot_keys_wild(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  // POT_KEYS_ON AND small keys are WILD (keysanity / Retro). The wild
  // worst-case key gate on a pot-bearing dungeon's deep locations applies only
  // here; dungeon/vanilla keys keep the vanilla branch (the in-context dungeon
  // case is a follow-on). false whenever pots are off OR forced off (cave
  // shuffle), so default + pots-off + dungeon-keys placement is byte-identical.
  const RandoSettings *s = ctx->settings;
  return Settings_PotKeysActive(s) &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Wild;
}

static bool eval_pot_keys_dungeon(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  // POT_KEYS_ON AND small keys are DUNGEON (per-dungeon, in-context). The pot-
  // bearing dungeons' deep locations/pots gate their SHORTEST-PATH key-door count
  // on this; the keys are collected en route so the graduated min-depth is the
  // exact requirement (a flat worst-case would be circular). The nonpot drops are
  // free-granted into the assumed inventory by rando_placement.c so the gates stay
  // satisfiable. Wild keys and pots-off/forced-off all leave this false (byte-id).
  const RandoSettings *s = ctx->settings;
  return Settings_PotKeysActive(s) &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Dungeon;
}

static bool eval_enemy_drop_keys_dungeon(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  const RandoSettings *s = ctx->settings;
  return Settings_EnemyDropKeysActive(s) &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Dungeon;
}

static bool eval_enemy_drop_keys_wild(Cursor *c, const PredicateContext *ctx) {
  (void)c;
  const RandoSettings *s = ctx->settings;
  return Settings_EnemyDropKeysActive(s) &&
         Settings_EffectiveSmallKeysMode(s) == kDungeonItemMode_Wild;
}

// Boss-shuffle runtime — "can kill the boss assigned to a kRandoDungeon_* slot". Resolves
// the per-seed boss assignment (ctx->boss_assignment; NULL ⇒ the vanilla boss
// via kRandoDungeonVanillaBoss), then RE-ENTERS the evaluator on that boss's
// kill predicate (kRandoBossKillPred[boss]). The outer cursor `c` advances past
// exactly the 1-byte dungeon operand; the sub-predicate runs on its own cursor
// so the length-consumed invariant of the caller is preserved.
//
// With boss_shuffle off the installed assignment is the vanilla identity (and a
// NULL assignment falls back to the same vanilla map), so this resolves to the
// dungeon's vanilla boss-kill predicate — byte-identical reachability to the
// inline CanKill<Boss> macro it replaced. No boss-kill predicate references
// OP_CAN_KILL_BOSS, so the re-entry can't recurse unboundedly.
// Door-shuffle ops — bodies live after the per-seed install section below
// (they consult the installed layout + the explorer-backed oracle).
static bool eval_doors_active(Cursor *c, const PredicateContext *ctx);
static bool eval_doors_loc_reachable(Cursor *c, const PredicateContext *ctx);

static bool eval_can_kill_boss(Cursor *c, const PredicateContext *ctx) {
  uint8 dungeon = cursor_u8(c);
  if (c->error || dungeon >= kRandoDungeonCount) return false;
  int imported_combat=Alttpr_CanKillBoss(dungeon,ctx->counts);
  if(imported_combat>=0) return imported_combat!=0;
  uint8 boss = (ctx->boss_assignment != NULL)
                   ? ctx->boss_assignment[dungeon]
                   : kRandoDungeonVanillaBoss[dungeon];
  if (boss >= kRandoBossKillPredCount) return false;  // 0xFF (HCE/unused) → false
  // add-enemy-souls note: the boss-soul requirement is NOT enforced here — it
  // lives inside each CanKill<Boss> macro body (NeedsBossSoul term), which
  // compiles into kRandoBossKillPred[boss] below. Resolving through `boss`
  // therefore requires the ASSIGNED boss's soul (follows boss shuffle), and the
  // same macro bodies cover the GT-refight / override sites that call
  // CanKill<Boss> inline without going through this op.
  const RandoBossKillPred *p = &kRandoBossKillPred[boss];
  if (p->length == 0) return false;
  Cursor sub = { kRandoPredicateStream + p->offset,
                 kRandoPredicateStream + p->offset + p->length, false };
  bool r = eval(&sub, ctx);
  if (sub.error) c->error = true;  // propagate malformed-bytecode signal
  return r;
}

static bool eval(Cursor *c, const PredicateContext *ctx) {
  uint8 op = cursor_u8(c);
  if (c->error) return false;
  switch (op) {
    case OP_HAS_ITEM:               return eval_has_item(c, ctx);
    case OP_HAS_AMOUNT:             return eval_has_amount(c, ctx);
    case OP_HAS_ANY_OF:             return eval_has_any_of(c, ctx);
    case OP_HAS_ANY_COUNT:          return eval_has_any_count(c, ctx);
    case OP_WORLDSTATE_EQ:          return eval_worldstate_eq(c, ctx);
    case OP_GOAL_EQ:                return eval_goal_eq(c, ctx);
    case OP_GOAL_REQUIRES_DUNGEON:  return eval_goal_requires_dungeon(c, ctx);
    case OP_DUNGEON_CLEARED:        return eval_dungeon_cleared(c, ctx);
    case OP_REGION_REACHABLE:       return eval_region_reachable(c, ctx);
    case OP_HAS_PRIZE:              return eval_has_prize(c, ctx);
    case OP_MEDALLION_OPENS:        return eval_medallion_opens(c, ctx);
    case OP_ITEM_IS:                return eval_item_is(c, ctx);
    case OP_NOT:                    return eval_not(c, ctx);
    case OP_AND:                    return eval_and(c, ctx);
    case OP_OR:                     return eval_or(c, ctx);
    case OP_TRICK:                  return eval_trick(c, ctx);
    case OP_DIFFICULTY_AT_LEAST:    return eval_difficulty(c, ctx);
    case OP_GLITCH_LEVEL_AT_LEAST:  return eval_glitch(c, ctx);
    case OP_MODEWEAPONS_EQ:         return eval_modeweapons_eq(c, ctx);
    case OP_CAN_KILL_BOSS:          return eval_can_kill_boss(c, ctx);
    case OP_DOORS_ACTIVE:           return eval_doors_active(c, ctx);
    case OP_DOORS_LOC_REACHABLE:    return eval_doors_loc_reachable(c, ctx);
    case OP_INSTANT_FLUTE:          return eval_instant_flute(c, ctx);
    case OP_OW_FLUTE_VANILLA:       return eval_ow_flute_vanilla(c, ctx);
    case OP_POT_KEYS_ON:            return eval_pot_keys_on(c, ctx);
    case OP_POT_KEYS_WILD:          return eval_pot_keys_wild(c, ctx);
    case OP_POT_KEYS_DUNGEON:       return eval_pot_keys_dungeon(c, ctx);
    case OP_ENEMY_DROP_KEYS_DUNGEON:return eval_enemy_drop_keys_dungeon(c, ctx);
    case OP_ENEMY_DROP_KEYS_WILD:   return eval_enemy_drop_keys_wild(c, ctx);
    case OP_SOULS_TIER_AT_LEAST:    return eval_souls_tier(c, ctx);
    case OP_NPC_SOULS_ACTIVE:       return eval_npc_souls_active(c, ctx);
    case OP_TOWER_CRYSTALS_MET:     return eval_tower_crystals_met(c, ctx);
    default:
      assert(0 && "unknown predicate op");
      c->error = true;
      return false;
  }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

bool Predicate_EvalCtx(const uint8 *bytecode, uint16 length,
                       const PredicateContext *ctx) {
  if (bytecode == NULL || length == 0 || ctx == NULL || ctx->counts == NULL) {
    return false;
  }
  // Phase D (add-rando-major-glitch) NoLogic short-circuit. At logic==NoLogic
  // EVERY predicate — reachability (placement_context==0) AND the LOGIC-level
  // placement can_place (placement_context==1) — returns true.
  //
  // Reachability-true (D1) → goal-completability and all accessibility tiers pass
  // vacuously, so the --generate-seed strict refusal (main.c) does not fire and
  // the seed has no reachability guarantee (mirrors ALTTPR World.php:93 — regions
  // not initialized under NoLogic).
  //
  // can_place-true (F5; was D1-deferred, now built per owner request) bypasses the
  // LOGIC-level can_place PREDICATES — the NotADungeonItem fill ban and item-locked
  // slots (e.g. Swamp Palace Entrance's `OP_ITEM_IS(SmallKey_SwampPalace)`) — so
  // placement is no longer LOGIC-constrained. SCOPE NOTE: the structural dungeon-
  // item MODE containment ("Dungeon-mode small key stays in its dungeon") is the
  // C-level `dungeon_mode_accepts_item` check in rando_placement.c, OUTSIDE this
  // predicate VM, and is deliberately STILL respected — it is an explicit MODE
  // choice (not a logic rule) and keeps Dungeon-mode keys runtime-grantable. So
  // F5 = "NoLogic ignores placement LOGIC", NOT "dungeon keys anywhere even in
  // Dungeon mode" (that would override a deliberate setting + risk ungrantable
  // keys — see design.md D1). Verified: a NoLogic + wild-keys seed's placement
  // DIFFERS from the pre-F5 confined placement (c0f5e87c -> 8be3e395 at seed
  // 0xF5F5), while the default Vanilla-keys NoLogic seed is BYTE-IDENTICAL (its
  // dungeon items are pre-pinned, never subject to can_place).
  //
  // Fires only at logic==NoLogic(4): no logic<4 digest moves; the default-keys
  // NoLogic corpus entry is unchanged and a NoLogic+wild-keys entry pins F5's
  // effect. See design.md D1/F5.
  if (ctx->settings != NULL && ctx->settings->logic >= 4 /* NoLogic */) {
    return true;
  }
  Cursor c = { bytecode, bytecode + length, false };
  bool result = eval(&c, ctx);
  // Either we consumed the entire stream, or hit an error. Malformed bytecode
  // asserts in debug builds; in release we just return the partial result.
  assert(!c.error && "predicate bytecode error");
  // Trailing bytes is also an error — the codegen always emits exactly the
  // declared length.
  assert(c.p == c.end && "predicate length mismatch");
  return result;
}

bool Predicate_Evaluate(const uint8 *bytecode, uint16 length,
                        const RandoCounts *counts,
                        const RandoSettings *settings) {
  PredicateContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.counts = counts;
  ctx.settings = settings;
  ctx.placement_context = 0;
  return Predicate_EvalCtx(bytecode, length, &ctx);
}

bool Predicate_EvaluatePlacement(const uint8 *bytecode, uint16 length,
                                 const RandoCounts *counts,
                                 const RandoSettings *settings,
                                 uint16 candidate_item,
                                 uint16 selected_key_rings_mask) {
  PredicateContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.counts = counts;
  ctx.settings = settings;
  ctx.candidate_item = candidate_item;
  ctx.selected_key_rings_mask = selected_key_rings_mask;
  ctx.placement_context = 1;
  return Predicate_EvalCtx(bytecode, length, &ctx);
}

// ---------------------------------------------------------------------------
// Phase C entrance shuffle — per-seed cave location-region overrides.
//
// When a cave entrance is shuffled, the cave-location's effective region becomes
// the vanilla region of whichever overworld door now leads to it (the closed
// form in shuffle_entrance.c). This is the SAME `effective_region` seam the
// static Inverted/Retro override uses (below), but driven per-seed by the
// entrance permutation instead of by world_state. It is consulted AFTER the
// static override; Stage 1 only supports Open/Standard, which carry no static
// location override, so there is no clobber. Inactive by default ⇒ the location
// loop is byte-identical to non-entrance-shuffle reachability.
#define kEntranceRegionOverrideMax 512
static uint16 g_entrance_region_override[kEntranceRegionOverrideMax];
static bool g_entrance_override_active = false;
// Stage 3 (cross-category): optional extra predicate AND-ed into an overridden
// cave-location's reachability — used when a cave lands behind a (gated) dungeon
// door so it inherits the door's requirement. pred_len 0 = no extra predicate.
static uint32 g_entrance_override_pred_off[kEntranceRegionOverrideMax];
static uint16 g_entrance_override_pred_len[kEntranceRegionOverrideMax];

void Rando_BeginEntranceRegionOverrides(void) {
  for (int i = 0; i < kEntranceRegionOverrideMax; i++) {
    g_entrance_region_override[i] = 0xFFFF;
    g_entrance_override_pred_len[i] = 0;
  }
  g_entrance_override_active = true;
}

void Rando_SetEntranceRegionOverride(uint16 loc_id, uint16 region_id) {
  if (loc_id < kEntranceRegionOverrideMax)
    g_entrance_region_override[loc_id] = region_id;
}

void Rando_ClearEntranceRegionOverrides(void) {
  g_entrance_override_active = false;
}

uint16 Rando_GetEntranceRegionOverride(uint16 loc_id) {
  if (!g_entrance_override_active || loc_id >= kEntranceRegionOverrideMax)
    return 0xFFFF;
  return g_entrance_region_override[loc_id];
}

// ---------------------------------------------------------------------------
// Phase C entrance shuffle (Stage 2) — per-seed DUNGEON edge overlay.
//
// Dungeons ARE first-class regions with inbound overworld door-edges, so the
// caves' location-region-override is the wrong tool. Instead we remap the
// *destination* of dungeon door-edges per π, keyed by the dungeon ENTRY region
// (each single-entrance dungeon's entry region is the `to_region` of exactly one
// door-edge). The edge's PREDICATE (the door-access requirement) stays with the
// door; only where the door leads changes — correct entrance-shuffle semantics.
// Internal dungeon edges + event gates are untouched (they have a dungeon entry
// region as `from`, not `to`). Inactive by default ⇒ byte-identical reachability.
#define kEntranceEdgeOverrideMax 64
static uint16 g_entrance_edge_override[kEntranceEdgeOverrideMax];
static bool g_entrance_edge_active = false;
// Stage 3 (cross-category): per-seed ADDED edges (overworld region → dungeon
// entry) for dungeons that land behind cave doors. Walked alongside kRandoEdges
// when g_entrance_edge_active. pred_len 0 = unconditional (cave-door access).
// add-rando-ow-warp-shuffle raised 64 -> 128. Worst-case consumers: decoupled
// entrance mode adds up to kEntranceCaveInteriorCount (40) exit edges,
// cross-category adds one per dungeon behind a cave door
// (<= kEntranceDungeonCount = 11), and the warp overlay adds <= 20 (asserted
// per install) — 71 total, 57 slots of headroom. Overflow drops the edge but
// counts it (one-time stderr below); OwWarp_SelfCheck asserts the combined
// worst case fits the cap and that no drop occurred.
#define kEntranceAddedEdgeMax 128
static struct {
  uint16 from_region, to_region;
  uint32 pred_off;
  uint16 pred_len;
  // tracker-player-knowledge — kKnowledgeTag_* | index; consulted only by the
  // masked live flood (generation passes no mask, so tags are inert there).
  uint8 knowledge_tag;
} g_entrance_added_edges[kEntranceAddedEdgeMax];
static int g_entrance_added_edge_count = 0;
static int g_entrance_added_edge_dropped = 0;

void Rando_BeginEntranceEdgeOverrides(void) {
  for (int i = 0; i < kEntranceEdgeOverrideMax; i++)
    g_entrance_edge_override[i] = 0xFFFF;
  g_entrance_added_edge_count = 0;
  g_entrance_added_edge_dropped = 0;
  g_entrance_edge_active = true;
}

void Rando_SetEntranceEdgeOverride(uint16 old_to_region, uint16 new_to_region) {
  if (old_to_region < kEntranceEdgeOverrideMax)
    g_entrance_edge_override[old_to_region] = new_to_region;
}

void Rando_ClearEntranceEdgeOverrides(void) {
  g_entrance_edge_active = false;
}

bool Rando_EntranceEdgeOverridesActive(void) {
  return g_entrance_edge_active;
}

uint16 Rando_GetEntranceEdgeOverride(uint16 to_region) {
  if (!g_entrance_edge_active || to_region >= kEntranceEdgeOverrideMax)
    return to_region;
  uint16 ov = g_entrance_edge_override[to_region];
  return (ov == 0xFFFF) ? to_region : ov;
}

// ---------------------------------------------------------------------------
// Phase C entrance shuffle (Stage 3 / cross-category) — two extra primitives for
// caves↔dungeons mixing. ALL dungeon doors are item-gated (Moon Pearl, Flippers,
// Book, crystals, …), so a clean cross-shuffle needs both halves below.
//
// 3a. PREDICATE-CARRYING cave override: when a CAVE lands behind a gated source
//     door (dungeon or cave), the cave's locations must inherit that door's
//     predicate (else the placer could strand the gating item inside — a softlock
//     the reachability gate can't see). Parallels the plain region override with
//     an optional extra predicate AND-ed into the cave's can_reach.
// 3b. ADDED edges: when a DUNGEON lands behind a CAVE door, there is no existing
//     edge to remap (caves aren't regions), so we ADD an edge
//     overworld-region → dungeon-entry. Gated cave source slots pass their
//     predicate through this added edge; pred_len 0 means no extra source gate.
//
// Both share the g_entrance_override_active flag (set by Begin) and are inert by
// default ⇒ byte-identical reachability. (State declared above with the region /
// edge override blocks so the Begin functions can reset it.)
void Rando_SetEntranceRegionOverridePred(uint16 loc_id, uint16 region_id,
                                         uint32 pred_off, uint16 pred_len) {
  if (loc_id >= kEntranceRegionOverrideMax) return;
  g_entrance_region_override[loc_id] = region_id;
  g_entrance_override_pred_off[loc_id] = pred_off;
  g_entrance_override_pred_len[loc_id] = pred_len;
}

// Count of currently-active added edges (0 when the edge set is inactive). Used
// by the self-check to guard against cross-attempt accumulation (decoupled HIGH).
int Rando_GetEntranceAddedEdgeCount(void) {
  return g_entrance_edge_active ? g_entrance_added_edge_count : 0;
}

// Edges dropped by the cap since the last Begin. Any nonzero value means a
// consumer's reachability silently shrank — the budget selfcheck asserts 0.
int Rando_GetEntranceAddedEdgeDropped(void) {
  return g_entrance_added_edge_dropped;
}

int Rando_EntranceAddedEdgeCapacity(void) {
  return kEntranceAddedEdgeMax;
}

void Rando_AddEntranceEdgeTagged(uint16 from_region, uint16 to_region,
                                 uint32 pred_off, uint16 pred_len,
                                 uint8 knowledge_tag) {
  if (g_entrance_added_edge_count >= kEntranceAddedEdgeMax) {
    if (g_entrance_added_edge_dropped++ == 0)
      fprintf(stderr,
              "Rando: added-edge store full (%d) — edge %u->%u dropped; "
              "logic reachability is understated\n",
              kEntranceAddedEdgeMax, from_region, to_region);
    return;
  }
  int i = g_entrance_added_edge_count++;
  g_entrance_added_edges[i].from_region = from_region;
  g_entrance_added_edges[i].to_region = to_region;
  g_entrance_added_edges[i].pred_off = pred_off;
  g_entrance_added_edges[i].pred_len = pred_len;
  g_entrance_added_edges[i].knowledge_tag = knowledge_tag;
}

void Rando_AddEntranceEdge(uint16 from_region, uint16 to_region,
                           uint32 pred_off, uint16 pred_len) {
  Rando_AddEntranceEdgeTagged(from_region, to_region, pred_off, pred_len,
                              kKnowledgeTag_None);
}

// ---------------------------------------------------------------------------
// Door shuffle — per-seed layout install + the OP_DOORS_* oracle.
//
// The oracle runs the SAME crystal-aware explorer the stitcher and key
// prover use (DoorExplore_Run), seeded from the portal lobbies whose
// kDoorPortalGates row holds under the current region bitset, with Vm rule
// leaves bound to the compiled fork predicate stream (kDoorVmPreds) and
// key-door edges gated by the prover's worst-case thresholds. Results are
// memoized per (dungeon, fixed-point pass): within one pass counts are fixed
// and the region bitset only grows between passes, so a stale-by-one-pass
// cache can only underestimate — the loop exits only after a zero-change
// pass, whose cache inputs are exact. Monotone, converges.
// ---------------------------------------------------------------------------

static const DoorShuffleLayout *g_door_logic_layout;
static uint16 g_door_logic_mask;
static uint32 g_door_oracle_gen = 1;
static uint32 g_door_oracle_cache_gen[kDoorTbl_DungeonCount];
static DoorExploreResult g_door_oracle_cache[kDoorTbl_DungeonCount];
// Input fingerprint per cached flood: the flood's outcome is a pure function
// of (inventory counts, portal set, held keys, big key) for a fixed layout.
// Assumed fill calls Logic_ComputeReachabilityFullKnowledge hundreds of times with mostly
// IDENTICAL inventory; re-flooding per fixed-point pass made a basic seed
// take minutes. When the fingerprint matches, the pass-stale cache entry is
// still exact — reuse it across generations.
static uint64 g_door_oracle_cache_fp[kDoorTbl_DungeonCount];
static uint64 g_door_counts_fp;       // FNV over ctx->counts, per oracle gen
static uint32 g_door_counts_fp_gen;
static bool g_in_door_oracle;
// Per-counts memo for the vm-pred leaves (see door_vm_pred_cb).
#define kDoorVmMemoMax 128
// The flood-skip fingerprint masks (CollectVmAt in shuffle_doors.c) and this
// memo both cap at 128 vm indices; the memo fails safe past the cap but the
// MASK would fail UNSOUND (a dropped index = a missed reflood trigger) —
// refuse to compile instead.
_Static_assert(kDoorTbl_VmPredCount <= kDoorVmMemoMax,
               "door vm preds exceed the fingerprint/memo width");
static uint8 g_door_vm_memo[kDoorVmMemoMax];  // 0 unknown / 1 false / 2 true
static uint64 g_door_vm_memo_fp;

static uint64 door_fnv64(uint64 h, const void *data, size_t n) {
  const uint8 *p = (const uint8 *)data;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

void Rando_SetDoorLogicLayout(const struct DoorShuffleLayout *layout, uint16 active_mask) {
  g_door_logic_layout = layout;
  g_door_logic_mask = layout ? active_mask : 0;
  g_door_oracle_gen++;
  // Hard-invalidate the flood cache + vm-pred memo: the input fingerprints
  // deliberately exclude the layout pointer and the settings/boss-assignment
  // context (constant within a run), so a NEW install must drop everything.
  memset(g_door_oracle_cache_gen, 0, sizeof(g_door_oracle_cache_gen));
  memset(g_door_oracle_cache_fp, 0, sizeof(g_door_oracle_cache_fp));
  memset(g_door_vm_memo, 0, sizeof(g_door_vm_memo));
  g_door_vm_memo_fp = ~0ull;
  g_door_counts_fp_gen = 0;
}

const struct DoorShuffleLayout *Rando_GetDoorLogicLayout(uint16 *active_mask_out) {
  if (active_mask_out)
    *active_mask_out = g_door_logic_mask;
  return g_door_logic_layout;
}

// Per-counts memo for the vm-pred leaves: they are pure functions of the
// inventory counts (verified: no door vm pred expands to OP_REGION_REACHABLE
// or OP_DOORS_*), and some expand large (CanKillMostThings). A flood
// evaluates them per edge; without the memo a basic seed's fill spent most
// of its time re-running the same 69 predicates thousands of times.
static bool door_vm_pred_cb(void *ud, uint16 vm_index) {
  const PredicateContext *ctx = (const PredicateContext *)ud;
  if (vm_index >= kDoorVmPredsCount)
    return false;
  if (g_door_vm_memo_fp != g_door_counts_fp) {
    memset(g_door_vm_memo, 0, sizeof(g_door_vm_memo));
    g_door_vm_memo_fp = g_door_counts_fp;
  }
  if (vm_index < kDoorVmMemoMax && g_door_vm_memo[vm_index])
    return g_door_vm_memo[vm_index] == 2;
  // Door vm-pred leaves are pure item/macro terms (the door-table translator
  // never emits OP_DOORS_* into them), so this cannot recurse into the
  // oracle; g_in_door_oracle guards the invariant.
  bool v = Predicate_EvalCtx(kRandoPredicateStream + kDoorVmPreds[vm_index].off,
                             kDoorVmPreds[vm_index].len, ctx);
  if (vm_index < kDoorVmMemoMax)
    g_door_vm_memo[vm_index] = v ? 2 : 1;
  return v;
}

static bool door_pot_pred_cb(void *ud, const RandoDoorPotLocation *pot) {
  const PredicateContext *ctx = (const PredicateContext *)ud;
  if (pot == NULL || pot->pred_len == 0)
    return true;
  return Predicate_EvalCtx(kRandoPredicateStream + pot->pred_off,
                           pot->pred_len, ctx);
}

static uint8 door_checked_active_key_pots(uint8 dungeon, uint16 small_key_item,
                                          uint8 pot_tier) {
  if (pot_tier == kPotShuffle_Off || small_key_item == 0xFFFFu)
    return 0;
  uint8 n = 0;
  for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[i];
    if (p->dungeon != dungeon || !(p->flags & kDoorPot_KeyPot) ||
        !Rando_DoorPotActive(p, pot_tier) || !Rando_IsLocationChecked(p->loc_id)) {
      continue;
    }
    if (Placement_Lookup(p->loc_id, 0xFFFFu) == small_key_item && n != 0xFFu)
      n++;
  }
  return n;
}

static bool door_enemy_drop_pred_cb(void *ud, const RandoDoorEnemyDropLocation *drop) {
  const PredicateContext *ctx = (const PredicateContext *)ud;
  if (drop == NULL || drop->pred_len == 0)
    return true;
  return Predicate_EvalCtx(kRandoPredicateStream + drop->pred_off,
                           drop->pred_len, ctx);
}

static bool door_enemy_check_pred_cb(void *ud, const RandoDoorEnemyCheckLocation *check) {
  const PredicateContext *ctx = (const PredicateContext *)ud;
  if (check == NULL || check->pred_len == 0)
    return true;
  return Predicate_EvalCtx(kRandoPredicateStream + check->pred_off,
                           check->pred_len, ctx);
}

static const DoorExploreResult *door_oracle_get(uint8 dungeon, const PredicateContext *ctx,
                                                DoorExploreGates *gates_out) {
  // The gates are rebuilt every call (cheap); the flood itself is cached.
  static uint8 held_keys[kDoorTbl_DungeonCount];
  static uint8 big_key_held[kDoorTbl_DungeonCount];
  uint8 pot_tier = g_door_logic_layout ? g_door_logic_layout->pot_tier : kPotShuffle_Off;
  for (int i = 0; i < kDoorTbl_DungeonCount; i++) {
    uint16 sk = kDoorTblDungeons[i].small_key_item;
    uint16 bk = kDoorTblDungeons[i].big_key_item;
    if (sk != 0xFFFF && ctx->counts) {
      uint16 effective = Logic_EffectiveItemCount(ctx->counts, ctx->settings, sk);
      held_keys[i] = effective > 0xFFu ? 0xFFu : (uint8)effective;
    } else {
      held_keys[i] = 0;
    }
    big_key_held[i] = (bk == 0xFFFF) ? 1
                      : ((ctx->counts && ctx->counts->by_item_id[bk]) ? 1 : 0);
  }
  if (ctx->counts != NULL &&
      ctx->counts->pot_nonpot_drops_seeded &&
      g_door_logic_layout != NULL &&
      g_door_logic_layout->pot_tier != kPotShuffle_Off) {
    for (uint32 i = 0; i < kPotNonpotDropCounts_COUNT; i++) {
      const RandoPotNonpotDropCount *row = &kPotNonpotDropCounts[i];
      uint8 d = Rando_RandoDungeonFromDungeonItem(row->item_id);
      if (d >= kDoorTbl_DungeonCount)
        continue;
      held_keys[d] = (held_keys[d] > row->count) ? (uint8)(held_keys[d] - row->count) : 0;
    }
  }
  if (Rando_IsActive() && ctx->counts != NULL && pot_tier != kPotShuffle_Off) {
    // DoorExplore_Core adds reachable drop-key rows internally. Active key pots
    // are itemized checks, so remove checked key-pot grants from the held-key
    // input here; otherwise a checked pot key can also satisfy the pot/drop side
    // of the door+pot key economy.
    for (int i = 0; i < kDoorTbl_DungeonCount; i++) {
      uint16 sk = kDoorTblDungeons[i].small_key_item;
      uint8 pot_keys = door_checked_active_key_pots((uint8)i, sk, pot_tier);
      held_keys[i] = (held_keys[i] > pot_keys) ? (uint8)(held_keys[i] - pot_keys) : 0;
    }
  }
  // Single-count of key sources between the inventory and the oracle's
  // internal credits: DoorExplore_Core's key budget already counts every
  // reachable active key-source row (key pots, enemy drops, enemy checks).
  // Keys the assumed-fill collect model gathered FROM those rows re-entered
  // by_item_id, so remove them from the held-key input — each source key
  // funds the budget exactly once, on one side or the other. Runtime live
  // counts and the conservative fill model carry all-zero fields (no-op).
  if (ctx->counts != NULL) {
    for (int i = 0; i < kDoorTbl_DungeonCount; i++) {
      uint8 src = ctx->counts->door_source_keys_collected[i];
      held_keys[i] = (held_keys[i] > src) ? (uint8)(held_keys[i] - src) : 0;
    }
  }
  gates_out->vm_pred = door_vm_pred_cb;
  gates_out->pot_pred = door_pot_pred_cb;
  gates_out->enemy_drop_pred = door_enemy_drop_pred_cb;
  gates_out->enemy_check_pred = door_enemy_check_pred_cb;
  gates_out->ud = (void *)ctx;
  gates_out->held_keys = held_keys;
  gates_out->big_key_held = big_key_held;
  gates_out->key_thresholds = g_door_logic_layout;

  if (g_door_oracle_cache_gen[dungeon] == g_door_oracle_gen)
    return &g_door_oracle_cache[dungeon];

  uint16 portals[12];
  int n = 0;
  for (uint32 i = 0; i < kDoorPortalGatesCount; i++) {
    const RandoDoorPortalGate *g = &kDoorPortalGates[i];
    if (g->dungeon != dungeon || g->fork_region == 0xFFFF)
      continue;
    if (ctx->reachable_regions_bitset == NULL)
      continue;
    if (!((ctx->reachable_regions_bitset[g->fork_region >> 3] >> (g->fork_region & 7)) & 1))
      continue;
    if (g->pred_len &&
        !Predicate_EvalCtx(kRandoPredicateStream + g->pred_off, g->pred_len, ctx))
      continue;
    if (n < 12)
      portals[n++] = g->door_region;
  }

  // Flood-skip: a flood's outcome is a pure function of (the vm-pred results
  // the dungeon's rules reference, held keys, big key, portal set) for a
  // fixed layout. Fingerprint exactly those — assumed fill changes one item
  // at a time, and most changes flip no predicate relevant to most dungeons.
  if (g_door_counts_fp_gen != g_door_oracle_gen) {
    uint64 fp = 0;
    if (ctx->counts) {
      // Skeleton Key is bonus-only and must not even perturb logic/oracle cache
      // identity. Hash the full item vector with that one cell canonicalized to
      // zero; Key Ring cells remain included and therefore invalidate correctly.
      uint16 zero = 0;
      fp = door_fnv64(0xcbf29ce484222325ull, ctx->counts->by_item_id,
                      ITEM_SkeletonKey * sizeof(ctx->counts->by_item_id[0]));
      fp = door_fnv64(fp, &zero, sizeof(zero));
      fp = door_fnv64(fp, ctx->counts->by_item_id + ITEM_SkeletonKey + 1,
                      (256u - ITEM_SkeletonKey - 1u) *
                          sizeof(ctx->counts->by_item_id[0]));
    }
    if (ctx->counts) {
      fp = door_fnv64(fp, &ctx->counts->pot_nonpot_drops_seeded,
                      sizeof(ctx->counts->pot_nonpot_drops_seeded));
    }
    // A door VM predicate can read settings fields via ops such as
    // OP_MODEWEAPONS_EQ and OP_INSTANT_FLUTE, so the per-counts memo/flood
    // fingerprint is NOT a pure function of the inventory counts. Fold those
    // fields in too so a future caller that evaluates the same door layout under
    // two settings differing only there can't get a stale memoized result.
    // Memo-key only — never affects the computed value.
    if (ctx->settings) {
      fp = door_fnv64(fp, &ctx->settings->mode_weapons,
                      sizeof(ctx->settings->mode_weapons));
      fp = door_fnv64(fp, &ctx->settings->instant_flute,
                      sizeof(ctx->settings->instant_flute));
    }
    g_door_counts_fp = fp;
    g_door_counts_fp_gen = g_door_oracle_gen;
  }
  uint64 vm_mask[2];
  DoorExplore_RelevantVmMask(dungeon, vm_mask);
  uint64 vm_bits[2] = { 0, 0 };
  for (int w = 0; w < 2; w++) {
    for (int b = 0; b < 64; b++) {
      if (!((vm_mask[w] >> b) & 1))
        continue;
      if (door_vm_pred_cb((void *)ctx, (uint16)(w * 64 + b)))
        vm_bits[w] |= 1ull << b;
    }
  }
  uint64 fp = door_fnv64(0xD00Eull + dungeon, vm_bits, sizeof(vm_bits));
  fp = door_fnv64(fp, portals, (size_t)n * sizeof(portals[0]));
  fp = door_fnv64(fp, &held_keys[dungeon], 1);
  fp = door_fnv64(fp, &big_key_held[dungeon], 1);
  for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[i];
    if (p->dungeon != dungeon || !(p->flags & kDoorPot_KeySource) ||
        !Rando_DoorPotActive(p, g_door_logic_layout->pot_tier))
      continue;
    uint8 v = door_pot_pred_cb((void *)ctx, p) ? 1 : 0;
    fp = door_fnv64(fp, &p->loc_id, sizeof(p->loc_id));
    fp = door_fnv64(fp, &v, sizeof(v));
  }
  for (uint32 i = 0; i < kRandoDoorEnemyDropLocationsCount; i++) {
    const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[i];
    if (e->dungeon != dungeon || !g_door_logic_layout->enemy_drop_keys)
      continue;
    uint8 v = door_enemy_drop_pred_cb((void *)ctx, e) ? 1 : 0;
    fp = door_fnv64(fp, &e->loc_id, sizeof(e->loc_id));
    fp = door_fnv64(fp, &v, sizeof(v));
  }
  for (uint32 i = 0; i < kRandoDoorEnemyCheckLocationsCount; i++) {
    const RandoDoorEnemyCheckLocation *e = &kRandoDoorEnemyCheckLocations[i];
    if (e->dungeon != dungeon ||
        !Rando_DoorEnemyCheckActive(e, g_door_logic_layout->enemy_check_tier))
      continue;
    uint8 v = door_enemy_check_pred_cb((void *)ctx, e) ? 1 : 0;
    fp = door_fnv64(fp, &e->loc_id, sizeof(e->loc_id));
    fp = door_fnv64(fp, &v, sizeof(v));
  }
  if (fp == g_door_oracle_cache_fp[dungeon] && g_door_oracle_cache_gen[dungeon] != 0) {
    g_door_oracle_cache_gen[dungeon] = g_door_oracle_gen;
    return &g_door_oracle_cache[dungeon];
  }

  g_in_door_oracle = true;
  DoorExplore_Run(g_door_logic_layout, dungeon, portals, n, gates_out,
                  &g_door_oracle_cache[dungeon]);
  g_in_door_oracle = false;
  g_door_oracle_cache_gen[dungeon] = g_door_oracle_gen;
  g_door_oracle_cache_fp[dungeon] = fp;
  return &g_door_oracle_cache[dungeon];
}

static bool eval_doors_active(Cursor *c, const PredicateContext *ctx) {
  uint8 d = cursor_u8(c);
  (void)ctx;
  return g_door_logic_layout != NULL && d < kDoorTbl_DungeonCount &&
         ((g_door_logic_mask >> d) & 1) != 0;
}

static bool eval_doors_loc_reachable(Cursor *c, const PredicateContext *ctx) {
  uint16 loc_id = cursor_u16le(c);
  if (g_door_logic_layout == NULL || g_in_door_oracle)
    return false;
  // kDoorTblLocations is sorted by fork_loc_id — binary search.
  int lo = 0, hi = kDoorTbl_LocationCount - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kDoorTblLocations[mid].fork_loc_id == loc_id) { found = mid; break; }
    if (kDoorTblLocations[mid].fork_loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  if (found >= 0) {
    const DoorTblLocation *dl = &kDoorTblLocations[found];
    uint8 dungeon = kDoorTblRegions[dl->region].dungeon;
    DoorExploreGates gates;
    const DoorExploreResult *r = door_oracle_get(dungeon, ctx, &gates);
    if (!DoorExplore_Reached(r, dl->region))
      return false;
    return DoorExplore_EvalRule(dl->rule, r, &gates);
  }

  // Generated pot bridge rows are sorted by loc_id.
  lo = 0;
  hi = (int)kRandoDoorPotLocationsCount - 1;
  found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorPotLocations[mid].loc_id == loc_id) { found = mid; break; }
    if (kRandoDoorPotLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  if (found >= 0) {
    const RandoDoorPotLocation *p = &kRandoDoorPotLocations[found];
    if (!Rando_DoorPotActive(p, g_door_logic_layout->pot_tier))
      return false;
    if (!((g_door_logic_mask >> p->dungeon) & 1))
      return false;
    DoorExploreGates gates;
    const DoorExploreResult *r = door_oracle_get(p->dungeon, ctx, &gates);
    for (uint8 i = 0; i < p->region_count; i++) {
      uint16 region = kRandoDoorPotRegions[p->region_first + i];
      if (!DoorExplore_Reached(r, region))
        return false;
    }
    return door_pot_pred_cb((void *)ctx, p);
  }

  // Generated enemy-drop bridge rows are sorted by loc_id.
  lo = 0;
  hi = (int)kRandoDoorEnemyDropLocationsCount - 1;
  found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorEnemyDropLocations[mid].loc_id == loc_id) { found = mid; break; }
    if (kRandoDoorEnemyDropLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  if (found >= 0) {
    const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[found];
    if (!g_door_logic_layout->enemy_drop_keys)
      return false;
    if (!((g_door_logic_mask >> e->dungeon) & 1))
      return false;
    DoorExploreGates gates;
    const DoorExploreResult *r = door_oracle_get(e->dungeon, ctx, &gates);
    if (!DoorExplore_Reached(r, e->region))
      return false;
    return door_enemy_drop_pred_cb((void *)ctx, e);
  }

  // Generated ordinary enemy-check bridge rows are sorted by loc_id.
  lo = 0;
  hi = (int)kRandoDoorEnemyCheckLocationsCount - 1;
  found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorEnemyCheckLocations[mid].loc_id == loc_id) { found = mid; break; }
    if (kRandoDoorEnemyCheckLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  if (found < 0)
    return false;
  const RandoDoorEnemyCheckLocation *ec = &kRandoDoorEnemyCheckLocations[found];
  if (!Rando_DoorEnemyCheckActive(ec, g_door_logic_layout->enemy_check_tier))
    return false;
  if (!((g_door_logic_mask >> ec->dungeon) & 1))
    return false;
  DoorExploreGates gates;
  const DoorExploreResult *r = door_oracle_get(ec->dungeon, ctx, &gates);
  for (uint8 i = 0; i < ec->region_count; i++) {
    uint16 region = kRandoDoorEnemyCheckRegions[ec->region_first + i];
    if (!DoorExplore_Reached(r, region))
      return false;
  }
  return door_enemy_check_pred_cb((void *)ctx, ec);
}

_Static_assert(kDoorTbl_DungeonCount <=
                   sizeof(((RandoCounts *)0)->door_source_keys_collected),
               "RandoCounts.door_source_keys_collected must cover every "
               "door-table dungeon");

// Single-count support for the collect-model fill (see
// RandoCounts.door_source_keys_collected): classify `loc_id` against the three
// bridge-row classes DoorExplore_Core credits internally as key sources. The
// filters mirror the credit exactly — Door_CountActivePotKeySources
// (kDoorPot_KeySource + active tier), Door_CountActiveEnemyDropKeySources
// (enemy_drop_keys), Door_CountActiveEnemyCheckKeySources (active tier) — plus
// the shuffled-mask gate the location bridge applies (rows of unshuffled
// dungeons resolve through vanilla logic, so the oracle takes no credit for
// them). Region reachability and the row predicate need no re-test here: a
// COLLECTED row already passed both via its bridge, and both are monotone in
// the inventory. Returns the kDoorTblDungeons index when `item_id` is that
// dungeon's small key and the row is credited; 0xFF otherwise (including when
// no door layout is installed).
uint8 Rando_DoorKeySourceDungeon(uint16 loc_id, uint16 item_id) {
  if (g_door_logic_layout == NULL)
    return 0xFF;
  int lo = 0, hi = (int)kRandoDoorPotLocationsCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorPotLocations[mid].loc_id == loc_id) {
      const RandoDoorPotLocation *p = &kRandoDoorPotLocations[mid];
      if ((p->flags & kDoorPot_KeySource) &&
          Rando_DoorPotActive(p, g_door_logic_layout->pot_tier) &&
          ((g_door_logic_mask >> p->dungeon) & 1) &&
          p->dungeon < kDoorTbl_DungeonCount &&
          item_id == kDoorTblDungeons[p->dungeon].small_key_item)
        return p->dungeon;
      return 0xFF;
    }
    if (kRandoDoorPotLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  lo = 0;
  hi = (int)kRandoDoorEnemyDropLocationsCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorEnemyDropLocations[mid].loc_id == loc_id) {
      const RandoDoorEnemyDropLocation *e = &kRandoDoorEnemyDropLocations[mid];
      if (g_door_logic_layout->enemy_drop_keys &&
          ((g_door_logic_mask >> e->dungeon) & 1) &&
          e->dungeon < kDoorTbl_DungeonCount &&
          item_id == kDoorTblDungeons[e->dungeon].small_key_item)
        return e->dungeon;
      return 0xFF;
    }
    if (kRandoDoorEnemyDropLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  lo = 0;
  hi = (int)kRandoDoorEnemyCheckLocationsCount - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (kRandoDoorEnemyCheckLocations[mid].loc_id == loc_id) {
      const RandoDoorEnemyCheckLocation *ec = &kRandoDoorEnemyCheckLocations[mid];
      if (Rando_DoorEnemyCheckActive(ec, g_door_logic_layout->enemy_check_tier) &&
          ((g_door_logic_mask >> ec->dungeon) & 1) &&
          ec->dungeon < kDoorTbl_DungeonCount &&
          item_id == kDoorTblDungeons[ec->dungeon].small_key_item)
        return ec->dungeon;
      return 0xFF;
    }
    if (kRandoDoorEnemyCheckLocations[mid].loc_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  return 0xFF;
}

// ---------------------------------------------------------------------------
// Logic_ComputeReachabilityFullKnowledge (task 3.8) — fixed-point expansion.
//
// Algorithm:
//   1. Initialize reachable_regions = {start_region}, reachable_locations = {}.
//      The start region depends on world_state (Open/Standard/Retro start in
//      LinksHouse; Inverted starts in DarkWorld_South). Phase A maps these
//      via region-id-by-name lookups when logic.yaml is populated; until
//      then, region 0 is treated as the start.
//   2. Iterate: for each edge from a reachable region, evaluate its predicate;
//      if true, mark target region reachable. For each location in a reachable
//      region, evaluate its can_reach; if true, mark reachable.
//   3. Stop when no new region / location was added in an iteration.
//   4. Memoize the result keyed by (counts, settings) — Phase A0 keeps the
//      result in a single static buffer (single-context per process).
//
// Performance budget (task 3.11): under 5 ms on reference desktop, under 20 ms
// on Switch, both for the full ~216-location graph. Phase A0's empty graph
// completes in microseconds.
// ---------------------------------------------------------------------------

// kReachabilityMaxRegions moved to rando_logic.h (add-rando-ow-warp-shuffle):
// the region budget is now a cross-file contract — shuffle_entrance.c derives
// its cross-void sink from it, and the codegen budget assert mirrors it.
// Sized by the module-wide location ceiling (rando_logic.h) so the location
// bitset always spans the full registry. A registry past capacity is caught by
// the LOC__COUNT <= kRandoLocationCapacity name-tie in rando.c.
#define kReachabilityMaxLocations kRandoLocationCapacity

struct RandoReachability {
  uint8 region_bitset[(kReachabilityMaxRegions + 7) >> 3];
  uint8 location_bitset[(kReachabilityMaxLocations + 7) >> 3];
  uint64 cleared_dungeons_bitmask;
  uint16 reachable_regions_count;
};

static struct RandoReachability g_reachability;

typedef struct LogicProfileCounters {
  uint64 reach_calls;
  uint64 reach_iterations;
  uint64 edge_predicates;
  uint64 edge_fast_true;
  uint64 edge_fast_false;
  uint64 location_predicates;
  uint64 location_fast_true;
  uint64 location_fast_false;
} LogicProfileCounters;

static LogicProfileCounters g_logic_profile;
static bool g_logic_profile_active;
static uint64 g_logic_generation_passes;
static bool g_logic_generation_counter_active;

void Logic_WorkCounterBegin(void) {
  g_logic_generation_passes = 0;
  g_logic_generation_counter_active = true;
}

uint64 Logic_WorkCounterFreeze(void) {
  g_logic_generation_counter_active = false;
  return g_logic_generation_passes;
}

static void logic_profile_dump(void) {
  if (!g_logic_profile_active) return;
  fprintf(stderr,
          "[RANDO_PROFILE logic] reach_calls=%llu iterations=%llu "
          "edge_predicates=%llu edge_fast_true=%llu edge_fast_false=%llu "
          "location_predicates=%llu location_fast_true=%llu location_fast_false=%llu\n",
          (unsigned long long)g_logic_profile.reach_calls,
          (unsigned long long)g_logic_profile.reach_iterations,
          (unsigned long long)g_logic_profile.edge_predicates,
          (unsigned long long)g_logic_profile.edge_fast_true,
          (unsigned long long)g_logic_profile.edge_fast_false,
          (unsigned long long)g_logic_profile.location_predicates,
          (unsigned long long)g_logic_profile.location_fast_true,
          (unsigned long long)g_logic_profile.location_fast_false);
}

static void logic_profile_maybe_init(void) {
  static bool initialized;
  if (initialized) return;
  initialized = true;
  const char *v = getenv("ZELDA3_RANDO_PROFILE");
  g_logic_profile_active = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
  if (g_logic_profile_active) atexit(logic_profile_dump);
}

static inline void bitset_set(uint8 *bs, uint16 idx) {
  bs[idx >> 3] |= (uint8)(1u << (idx & 7));
}

static inline bool bitset_has(const uint8 *bs, uint16 idx) {
  return (bs[idx >> 3] >> (idx & 7)) & 1;
}

static bool predicate_is_vacuous_true(const uint8 *bc, uint16 len) {
  return len == 2 && bc != NULL && bc[0] == OP_AND && bc[1] == 0;
}

static bool predicate_is_vacuous_false(const uint8 *bc, uint16 len) {
  return len == 2 && bc != NULL && bc[0] == OP_OR && bc[1] == 0;
}

static bool eval_reachability_predicate_fast(const uint8 *bc, uint16 len,
                                             PredicateContext *ctx,
                                             bool location_predicate) {
  if (predicate_is_vacuous_true(bc, len)) {
    if (g_logic_profile_active) {
      if (location_predicate) g_logic_profile.location_fast_true++;
      else g_logic_profile.edge_fast_true++;
    }
    return true;
  }
  if (ctx == NULL || ctx->settings == NULL || ctx->settings->logic < 4 /* NoLogic */) {
    if (predicate_is_vacuous_false(bc, len)) {
      if (g_logic_profile_active) {
        if (location_predicate) g_logic_profile.location_fast_false++;
        else g_logic_profile.edge_fast_false++;
      }
      return false;
    }
  }
  if (g_logic_profile_active) {
    if (location_predicate) g_logic_profile.location_predicates++;
    else g_logic_profile.edge_predicates++;
  }
  return Predicate_EvalCtx(bc, len, ctx);
}

static bool logic_pot_active(const RandoLocationDef *loc, const RandoSettings *s) {
  if (loc == NULL || s == NULL || loc->type != LOCTYPE_Pot) return false;
  if (Settings_PotShuffleForcedOff(s)) return false;
  bool is_empty = (loc->vanilla_item_id == ITEM_Nothing);
  switch (s->pot_shuffle) {
    case kPotShuffle_Off:      return false;
    case kPotShuffle_Keys:     return Rando_IsSmallKeyItem(loc->vanilla_item_id);
    case kPotShuffle_Contents: return !is_empty;
    case kPotShuffle_All:      return true;
    default:                   return false;
  }
}

static bool logic_enemy_drop_active(const RandoLocationDef *loc,
                                    const RandoSettings *s) {
  return loc != NULL && s != NULL && loc->type == LOCTYPE_EnemyDrop &&
         Settings_EnemyDropKeysActive(s);
}

static bool g_logic_enemy_flags_ready;
static uint8 g_logic_enemy_all_tier_only[kReachabilityMaxLocations];
static uint8 g_logic_enemy_overworld_stage[kReachabilityMaxLocations];

enum { kLogicEnemyCheckNotOverworld = 0xFF };

static void logic_init_enemy_flags(void) {
  if (g_logic_enemy_flags_ready) return;
  memset(g_logic_enemy_overworld_stage, kLogicEnemyCheckNotOverworld,
         sizeof(g_logic_enemy_overworld_stage));
  for (uint32 i = 0; i < kRandoEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoEnemyCheckLookup[i].loc_id;
    if (loc_id < kReachabilityMaxLocations &&
        kRandoEnemyCheckLookup[i].all_tier_only) {
      g_logic_enemy_all_tier_only[loc_id] = 1;
    }
  }
  for (uint32 i = 0; i < kRandoBossEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoBossEnemyCheckLookup[i].loc_id;
    if (loc_id < kReachabilityMaxLocations) g_logic_enemy_all_tier_only[loc_id] = 1;
  }
  for (uint32 i = 0; i < kRandoScriptedEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoScriptedEnemyCheckLookup[i].loc_id;
    if (loc_id < kReachabilityMaxLocations) g_logic_enemy_all_tier_only[loc_id] = 1;
  }
  for (uint32 i = 0; i < kRandoOverworldEnemyCheckLookup_COUNT; i++) {
    uint16 loc_id = kRandoOverworldEnemyCheckLookup[i].loc_id;
    if (loc_id < kReachabilityMaxLocations) {
      g_logic_enemy_all_tier_only[loc_id] = 1;
      g_logic_enemy_overworld_stage[loc_id] = kRandoOverworldEnemyCheckLookup[i].stage;
    }
  }
  g_logic_enemy_flags_ready = true;
}

static bool logic_enemy_check_all_tier_only(uint16 loc_id) {
  logic_init_enemy_flags();
  return loc_id < kReachabilityMaxLocations && g_logic_enemy_all_tier_only[loc_id] != 0;
}

static uint8 logic_enemy_check_overworld_stage(uint16 loc_id) {
  logic_init_enemy_flags();
  if (loc_id >= kReachabilityMaxLocations) return kLogicEnemyCheckNotOverworld;
  return g_logic_enemy_overworld_stage[loc_id];
}

static bool logic_enemy_check_active(const RandoLocationDef *loc,
                                     const RandoSettings *s) {
  if (loc == NULL || s == NULL || loc->type != LOCTYPE_Enemy) return false;
  if (Settings_EnemyChecksAllActive(s)) {
    uint8 ow_stage = logic_enemy_check_overworld_stage(loc->id);
    if (ow_stage == 0 && s->world_state != kWorldState_Standard)
      return false;
    return true;
  }
  return Settings_EnemyChecksDungeonActive(s) &&
         !logic_enemy_check_all_tier_only(loc->id);
}

static bool logic_location_active_for_settings(const RandoLocationDef *loc,
                                               const RandoSettings *settings) {
  if (loc == NULL) return false;
  if (loc->type == LOCTYPE_Pot) return logic_pot_active(loc, settings);
  if (loc->type == LOCTYPE_EnemyDrop) return logic_enemy_drop_active(loc, settings);
  if (loc->type == LOCTYPE_Enemy) return logic_enemy_check_active(loc, settings);
  // add-rando-bonk-sanity — bonk rows sit BELOW the terrain suffix (always
  // scanned); the axis gate keeps them out of reachability when off.
  if (loc->type == LOCTYPE_Bonk)
    return settings != NULL && settings->bonk_shuffle != kTerrainShuffle_Off;
  return true;
}

static bool derive_hce_big_key_from_reachability(RandoCounts *counts,
                                                 const RandoSettings *settings) {
  if (counts == NULL) return false;
  if (!Settings_EnemyDropKeysActive(settings)) return false;
  if (kRandoEnemyDropHceBigKeyLocId == 0xFFFF) return false;
  if (kRandoEnemyDropHceBigKeyLocId >= kReachabilityMaxLocations) return false;
  if (counts->by_item_id[ITEM_HyruleCastleBigKey] != 0) return false;
  if (!bitset_has(g_reachability.location_bitset, kRandoEnemyDropHceBigKeyLocId))
    return false;
  counts->by_item_id[ITEM_HyruleCastleBigKey] = 1;
  return true;
}

// under Inverted the placer should evaluate the INVERTED graph, not
// base ∪ inverted. A base edge whose (from,to) pair ALSO has an Inverted edge is
// SHADOWED: the Inverted edge (walked separately below) is authoritative for that
// pair, and walking the looser base edge too leaks base reachability — e.g. the
// base Desert/EP entrance edges gate on RescuedZelda-only, while their Inverted
// counterparts require MoonPearl, so the unshadowed base edge let the Desert
// interior be reached in the Inverted model without the pearl. Precompute the
// shadowed-base-edge set once from the constant edge tables; the base loop skips
// these when world_state==Inverted. (Beatability is unchanged — the runtime
// pre-grants MoonPearl in Inverted — but this restores ALTTPR-faithful Inverted
// placement, so Inverted placement_digests move; the rest stay byte-identical.)
// Bitmap of (from,to) region pairs that an Inverted edge covers (compile-time
// sized on kReachabilityMaxRegions so it isn't a file-scope VLA — kRandoEdgesCount
// is a runtime const). A base edge whose pair is set is shadowed under Inverted.
static uint8 g_inverted_pair_set[kReachabilityMaxRegions][(kReachabilityMaxRegions + 7) >> 3];
static bool g_inverted_pair_ready = false;
static void compute_inverted_shadow(void) {
  if (g_inverted_pair_ready) return;
  memset(g_inverted_pair_set, 0, sizeof(g_inverted_pair_set));
  for (uint32 j = 0; j < kRandoEdges_InvertedCount; j++) {
    uint16 f = kRandoEdges_Inverted[j].from_region, t = kRandoEdges_Inverted[j].to_region;
    if (f < kReachabilityMaxRegions && t < kReachabilityMaxRegions)
      g_inverted_pair_set[f][t >> 3] |= (uint8)(1u << (t & 7));
  }
  g_inverted_pair_ready = true;
}
static inline bool base_edge_inverted_shadowed(uint16 from, uint16 to) {
  if (from >= kReachabilityMaxRegions || to >= kReachabilityMaxRegions) return false;
  return (g_inverted_pair_set[from][to >> 3] >> (to & 7)) & 1;
}

// tracker-player-knowledge — region id → RandoRegionDef.dungeon_id lookup for
// the masked flood's region-admission gate. Lazily built from the generated
// region table (kRandoDungeon_* convention; 0xFF = not a dungeon).
static uint8 g_region_dungeon_id[kReachabilityMaxRegions];
static bool g_region_dungeon_ready = false;
static void build_region_dungeon_index(void) {
  if (g_region_dungeon_ready) return;
  memset(g_region_dungeon_id, 0xFF, sizeof(g_region_dungeon_id));
  for (uint32 i = 0; i < kRandoRegionsCount; i++) {
    if (kRandoRegions[i].id < kReachabilityMaxRegions)
      g_region_dungeon_id[kRandoRegions[i].id] = kRandoRegions[i].dungeon_id;
  }
  g_region_dungeon_ready = true;
}

// True when the mask excludes `region` from the fixpoint (its dungeon's
// identity is hidden). NULL mask / zero mask short-circuits to false.
static inline bool knowledge_mask_blocks_region(const RandoKnowledgeMask *mask,
                                                uint16 region) {
  if (mask == NULL || mask->hidden_dungeon_mask == 0) return false;
  if (region >= kReachabilityMaxRegions) return false;
  uint8 d = g_region_dungeon_id[region];
  return d < 16 && ((mask->hidden_dungeon_mask >> d) & 1u) != 0;
}

// True when the mask suppresses a knowledge-tagged added edge.
static inline bool knowledge_mask_blocks_edge(const RandoKnowledgeMask *mask,
                                              uint8 tag) {
  if (mask == NULL || tag == kKnowledgeTag_None) return false;
  uint8 kind = tag & (uint8)kKnowledgeTag_KindMask;
  if (kind == kKnowledgeTag_Whirlpool)
    return ((mask->hidden_whirlpool_mask >> (tag & 7u)) & 1u) != 0;
  if (kind == kKnowledgeTag_DecoupledExit)
    return ((mask->hidden_exit_mask >> (tag & 0x3Fu)) & 1u) != 0;
  return false;
}

static const RandoReachability *logic_compute_reachability_internal(
    const RandoCounts *counts, const RandoSettings *settings, bool preserve_existing,
    const RandoKnowledgeMask *mask) {
  logic_profile_maybe_init();
  if (g_logic_profile_active) g_logic_profile.reach_calls++;
  if (counts == NULL || settings == NULL) return NULL;
  if (mask != NULL) build_region_dungeon_index();
  if (!preserve_existing)
    memset(&g_reachability, 0, sizeof(g_reachability));
  g_reachability.reachable_regions_count = kReachabilityMaxRegions;

  // Seed the fixed-point with the world-state-appropriate start region. The
  // codegen-emitted `kRandoStartRegionByWorldState[]` maps WorldState → region
  // id; 0xFFFF means "no start region for this world-state" (e.g., Inverted
  // before LinksHouse_Inverted is declared). In that case the graph has no
  // reachable region and Logic_ComputeReachabilityFullKnowledge returns an all-zero bitset
  // — the placer falls back to "every location stays at vanilla" for that
  // world-state.
  uint16 start_region = 0xFFFF;
  if (settings->world_state < 4) {
    start_region = kRandoStartRegionByWorldState[settings->world_state];
  }
  if (start_region != 0xFFFF && start_region < kReachabilityMaxRegions) {
    bitset_set(g_reachability.region_bitset, start_region);
  }

  PredicateContext ctx;
  RandoCounts effective_counts = *counts;
  memset(&ctx, 0, sizeof(ctx));
  ctx.counts = &effective_counts;
  ctx.settings = settings;
  ctx.cleared_dungeons_bitmask = g_reachability.cleared_dungeons_bitmask;
  ctx.reachable_regions_bitset = g_reachability.region_bitset;
  ctx.reachable_regions_count = kReachabilityMaxRegions;
  ctx.tower_crystals_required_plus1 = g_logic_resolved_tower_plus1;
  // Per-seed shuffle assignments (NULL when the placer hasn't installed
  // them yet — OP_HAS_PRIZE and OP_MEDALLION_OPENS evaluate to false in
  // that case, which makes prize-gated / medallion-gated areas unreachable).
  ctx.dungeon_prize_assignment = Rando_GetDungeonPrizeAssignment();
  ctx.medallion_entrance_assignment = Rando_GetMedallionAssignment();
  // Boss-shuffle assignment for OP_CAN_KILL_BOSS. NULL ⇒ vanilla boss fallback,
  // so a graph with no boss assignment installed gates each `- Boss` on its
  // vanilla boss-kill predicate (byte-identical to the pre-op logic).
  ctx.boss_assignment = Rando_GetBossAssignment();
  // cleared_dungeons_bitmask gets recomputed at the end of each fixed-point
  // iteration based on which boss locations have been reached. See below.

  // L7 — precompute (once) which base edges an inverted edge shadows, so the
  // base-edge walk can skip them under Inverted (the inverted edge is then the
  // sole authority for that (from,to) pair).
  const bool inverted = (settings->world_state == 2 /* kWorldState_Inverted */);
  if (inverted) compute_inverted_shadow();

  // Fixed-point iteration. Cap at 64 iterations to bound runtime; the graph
  // depth is well under 32 in practice (per ALTTPR's region nesting).
  for (int iter = 0; iter < 64; iter++) {
    if (g_logic_profile_active) g_logic_profile.reach_iterations++;
    if (g_logic_generation_counter_active) g_logic_generation_passes++;
    bool changed = false;
    // Door shuffle: invalidate the oracle memo each pass — counts are fixed
    // for this whole call, but the region bitset (portal availability) grows
    // between passes.
    g_door_oracle_gen++;

    // Expand reachable regions via edges.
    // Phase B Slice 2 — also walk per-world-state edges. The base kRandoEdges
    // array carries the Standard/Open/Retro graph; Inverted adds its own edges
    // (DarkWorld_South → IcePalace, LightWorld mirror back to DarkWorld, etc.).
    // Under Inverted both tables are walked, BUT a base edge shadowed by an
    // inverted edge for the same (from,to) is skipped (L7) so the Inverted graph
    // REPLACES — not unions with — the base graph for those pairs.
    // add-rando-ow-warp-shuffle — the static warp edges are the trailing
    // kRandoEdges suffix; when no warp axis is active the scan bounds at
    // kRandoNonWarpEdgesCount so the substrate is free for non-warp seeds
    // (the terrain inert-suffix pattern, edge-side). NULL settings = vanilla.
    uint32 edge_scan_n = Settings_OwWarpActive(ctx.settings)
                             ? kRandoEdgesCount
                             : kRandoNonWarpEdgesCount;
    for (uint32 e = 0; e < edge_scan_n; e++) {
      const RandoEdgeDef *edge = &kRandoEdges[e];
      if (edge->from_region == 0xFFFF || edge->to_region == 0xFFFF) continue;
      if (inverted && base_edge_inverted_shadowed(edge->from_region, edge->to_region))
        continue;  // L7: the inverted edge for this (from,to) pair is authoritative
      // Phase C Stage 2 — dungeon entrance shuffle remaps a door-edge's
      // destination per π (keeping its door-access predicate). Identity when
      // inactive ⇒ byte-identical.
      uint16 to_region = Rando_GetEntranceEdgeOverride(edge->to_region);
      if (!bitset_has(g_reachability.region_bitset, edge->from_region)) continue;
      if (bitset_has(g_reachability.region_bitset, to_region)) continue;
      // tracker-player-knowledge — a hidden-identity dungeon's regions never
      // enter the fixpoint (mask NULL for every generation path).
      if (knowledge_mask_blocks_region(mask, to_region)) continue;
      const uint8 *bc = kRandoPredicateStream + edge->predicate_offset;
      if (Alttpr_AdditionalRegionRoute(edge->from_region,to_region,ctx.counts) ||
          eval_reachability_predicate_fast(bc, edge->predicate_length, &ctx, false)) {
        bitset_set(g_reachability.region_bitset, to_region);
        changed = true;
      }
    }
    if (settings->world_state == 2 /* kWorldState_Inverted */) {
      for (uint32 e = 0; e < kRandoEdges_InvertedCount; e++) {
        const RandoEdgeDef *edge = &kRandoEdges_Inverted[e];
        if (edge->from_region == 0xFFFF || edge->to_region == 0xFFFF) continue;
        uint16 to_region = Rando_GetEntranceEdgeOverride(edge->to_region);
        if (!bitset_has(g_reachability.region_bitset, edge->from_region)) continue;
        if (bitset_has(g_reachability.region_bitset, to_region)) continue;
        if (knowledge_mask_blocks_region(mask, to_region)) continue;
        const uint8 *bc = kRandoPredicateStream + edge->predicate_offset;
        if (eval_reachability_predicate_fast(bc, edge->predicate_length, &ctx, false)) {
          bitset_set(g_reachability.region_bitset, to_region);
          changed = true;
        }
      }
    }
    // Phase C Stage 3 (cross-category) — per-seed ADDED edges (overworld region →
    // dungeon entry, for dungeons behind cave doors). Unconditional when
    // pred_len == 0 (a cave door has no access gate beyond being in the region).
    if (g_entrance_edge_active) {
      for (int e = 0; e < g_entrance_added_edge_count; e++) {
        uint16 fr = g_entrance_added_edges[e].from_region;
        uint16 tr = g_entrance_added_edges[e].to_region;
        if (fr == 0xFFFF || tr == 0xFFFF) continue;
        if (!bitset_has(g_reachability.region_bitset, fr)) continue;
        if (bitset_has(g_reachability.region_bitset, tr)) continue;
        // tracker-player-knowledge — hidden-dungeon targets and undiscovered
        // tagged edges (whirlpool pairs / decoupled exits) are skipped.
        if (knowledge_mask_blocks_region(mask, tr)) continue;
        if (knowledge_mask_blocks_edge(mask, g_entrance_added_edges[e].knowledge_tag))
          continue;
        uint16 pl = g_entrance_added_edges[e].pred_len;
        if (pl == 0 ||
            eval_reachability_predicate_fast(
                kRandoPredicateStream + g_entrance_added_edges[e].pred_off,
                pl, &ctx, false)) {
          bitset_set(g_reachability.region_bitset, tr);
          changed = true;
        }
      }
    }

    // Expand reachable locations: a location is reachable iff
    //   (a) its world_state_filter permits the active world_state,
    //   (b) its region is in the reachable_region set (when the location
    //       has a region_id binding; a location with region_id == 0xFFFF
    //       would be treated as "always-reachable region" — a defensive
    //       fallback; every currently generated location has a binding,
    //       since the unbound Fountain slots 140/141 were retired), AND
    //   (c) its can_reach predicate evaluates true under the current
    //       inventory snapshot.
    // add-rando-grass-rock-shuffle: terrain locations (Grass/Rock) are a
    // contiguous suffix of the id-sorted registry. When NEITHER axis is
    // active they are placement-inert and nothing consumes their reachable
    // bits, so bound the expansion at the non-terrain count — skipping 3943
    // rows per pass entirely (a door-shuffle corpus seed's generation
    // otherwise crosses the 120 s budget). When at least one axis is active
    // we walk the full array and skip only the inactive-axis rows inline.
    bool any_terrain = settings->grass_shuffle != kTerrainShuffle_Off ||
                       settings->rock_shuffle != kTerrainShuffle_Off;
    uint32 loc_scan_n = any_terrain ? kRandoLocationsCount
                                    : kRandoNonTerrainLocationsCount;
    for (uint32 i = 0; i < loc_scan_n; i++) {
      const RandoLocationDef *loc = &kRandoLocations[i];
      // Bound loc->id before indexing the [kReachabilityMaxLocations]-sized
      // bitset. Every shipping id is < kRandoLocationCapacity (a codegen
      // _Static_assert enforces it), so this never fires today; it fails CLOSED
      // (the location is simply treated unreachable) if a future registry
      // append overflows,
      // mirroring the bounded cleared-dungeons boss loop instead of corrupting
      // adjacent memory.
      if (loc->id >= kReachabilityMaxLocations) continue;
      // tracker-player-knowledge — a suppressed location (undiscovered
      // shuffled cave contents) is never reported reachable in the live view.
      if (mask != NULL && mask->suppressed_locations != NULL &&
          bitset_has(mask->suppressed_locations, loc->id)) continue;
      // With a terrain axis active, still skip the OTHER axis's inactive rows.
      if (loc->type == LOCTYPE_Grass &&
          settings->grass_shuffle == kTerrainShuffle_Off) continue;
      if (loc->type == LOCTYPE_Rock &&
          settings->rock_shuffle == kTerrainShuffle_Off) continue;
      if (bitset_has(g_reachability.location_bitset, loc->id)) continue;
      // Shared filter (shift-guarded; shopsanity overrides the Retro-only
      // filter on regular shop slots) — see Rando_LocationWorldStateActive.
      if (!Rando_LocationWorldStateActive(loc, settings)) continue;
      if (!logic_location_active_for_settings(loc, settings)) continue;
      // Phase B Slice 2 — consult the per-world-state override table.
      // Inverted seeds get a different can_reach predicate per location;
      // when no override exists, fall back to the base predicate. The
      // override may ALSO change the region the location belongs to —
      // e.g., Ether Tablet (id 194) moves East↔West in Inverted per
      // `Inverted/LightWorld/DeathMountain/East.php:25-26`.
      uint32 cr_offset = loc->can_reach_offset;
      uint16 cr_length = loc->can_reach_length;
      uint16 effective_region = loc->region_id;
      const RandoLocationPredOverride *ov =
          Rando_FindPredicateOverride(loc->id, settings->world_state);
      if (ov != NULL) {
        cr_offset = ov->can_reach_offset;
        cr_length = ov->can_reach_length;
        if (ov->region_override != 0xFFFF) {
          effective_region = ov->region_override;
        }
      }
      // Phase C — per-seed entrance-shuffle cave region override (takes
      // precedence; Open/Standard carry no static `ov`, so no conflict).
      uint32 ov_pred_off = 0; uint16 ov_pred_len = 0;  // Stage 3 cross-cat gate
      if (g_entrance_override_active && loc->id < kEntranceRegionOverrideMax &&
          g_entrance_region_override[loc->id] != 0xFFFF) {
        effective_region = g_entrance_region_override[loc->id];
        // Stage 3 — a cave behind a (gated) dungeon door inherits that door's
        // predicate so the placer can't strand the gating item inside.
        ov_pred_off = g_entrance_override_pred_off[loc->id];
        ov_pred_len = g_entrance_override_pred_len[loc->id];
      }
      if (effective_region != 0xFFFF) {
        if (effective_region >= kReachabilityMaxRegions) continue;
        if (!bitset_has(g_reachability.region_bitset, effective_region)) continue;
      }
      // Cross-category override predicate (the destination dungeon door's gate).
      if (ov_pred_len != 0 &&
          !Predicate_EvalCtx(kRandoPredicateStream + ov_pred_off, ov_pred_len, &ctx)) {
        continue;
      }
      const uint8 *bc = kRandoPredicateStream + cr_offset;
      int imported_reach = Alttpr_LocationReachability(loc->id, ctx.counts);
      if (imported_reach >= 0 ? imported_reach != 0 :
          eval_reachability_predicate_fast(bc, cr_length, &ctx, true)) {
        bitset_set(g_reachability.location_bitset, loc->id);
        changed = true;
      }
    }

    if (derive_hce_big_key_from_reachability(&effective_counts, settings)) {
      changed = true;
    }

    // Update cleared_dungeons_bitmask based on which boss locations are
    // now reachable. Indexing is kRandoDungeon_* order from dungeon_ids.h.
    // The boss location is the canonical "dungeon cleared"
    // signal — when its Prize location is reachable the boss is necessarily
    // defeated to get there.
    static const uint16 kDungeonBossLocations[13] = {
      0xFFFF,  // HCE: no boss (escape sequence; Zelda event handles it)
      16,      // EP Boss
      23,      // DP Boss
      30,      // TH Boss
      34,      // HCT Agahnim (Prize_Event)
      48,      // PoD Boss
      59,      // SP Boss
      68,      // SW Boss
      77,      // TT Boss
      86,      // IP Boss
      95,      // MM Boss
      108,     // TR Boss
      137,     // GT Agahnim 2 (Prize_Event)
    };
    uint64 new_cleared = 0;
    for (uint8 d = 0; d < 13; d++) {
      uint16 loc_id = kDungeonBossLocations[d];
      if (loc_id == 0xFFFF) continue;
      if (loc_id < kReachabilityMaxLocations &&
          bitset_has(g_reachability.location_bitset, loc_id)) {
        new_cleared |= (uint64)1 << d;
      }
    }
    if (new_cleared != g_reachability.cleared_dungeons_bitmask) {
      g_reachability.cleared_dungeons_bitmask = new_cleared;
      ctx.cleared_dungeons_bitmask = new_cleared;
      changed = true;
    }

    if (!changed) break;
  }

  return &g_reachability;
}

const RandoReachability *Logic_ComputeReachabilityFullKnowledge(const RandoCounts *counts,
                                                   const RandoSettings *settings) {
  return logic_compute_reachability_internal(counts, settings, false, NULL);
}

const RandoReachability *Logic_ComputeReachabilityMasked(const RandoCounts *counts,
                                                         const RandoSettings *settings,
                                                         const RandoKnowledgeMask *mask) {
  return logic_compute_reachability_internal(counts, settings, false, mask);
}

const RandoReachability *Logic_ExpandReachability(const RandoCounts *counts,
                                                  const RandoSettings *settings) {
  return logic_compute_reachability_internal(counts, settings, true, NULL);
}

bool Reachability_HasLocation(const RandoReachability *r, uint16 location_id) {
  if (r == NULL) return false;
  if (location_id >= kReachabilityMaxLocations) return false;
  return bitset_has(r->location_bitset, location_id);
}

bool Reachability_HasRegion(const RandoReachability *r, uint16 region_id) {
  if (r == NULL) return false;
  if (region_id >= kReachabilityMaxRegions) return false;
  return bitset_has(r->region_bitset, region_id);
}

// Private snapshot buffer. Logic_ComputeReachabilityFullKnowledge returns a pointer into the
// single shared g_reachability, which any later call overwrites. A caller that
// must hold a result across other reachability computations (the runtime
// tracker bridge) copies it here once and reads the stable snapshot.
static struct RandoReachability g_reachability_snapshot;

const RandoReachability *Reachability_Snapshot(bool refresh) {
  if (refresh) g_reachability_snapshot = g_reachability;
  return &g_reachability_snapshot;
}

// ---------------------------------------------------------------------------
// Phase B Slice 2 — per-world-state predicate override lookup.
//
// Override tables are sorted by location_id at codegen time; binary search
// is the standard lookup. Returns NULL when no override is installed for
// (location_id, world_state).
// ---------------------------------------------------------------------------
static const RandoLocationPredOverride *
binary_search_overrides(const RandoLocationPredOverride *arr, uint32 count, uint16 loc_id) {
  if (count == 0) return NULL;
  int lo = 0, hi = (int)count - 1;
  while (lo <= hi) {
    int mid = lo + ((hi - lo) >> 1);
    uint16 mid_id = arr[mid].location_id;
    if (mid_id == loc_id) return &arr[mid];
    if (mid_id < loc_id) lo = mid + 1;
    else hi = mid - 1;
  }
  return NULL;
}

const RandoLocationPredOverride *
Rando_FindPredicateOverride(uint16 loc_id, uint8 world_state) {
  switch (world_state) {
    case 2:  // kWorldState_Inverted
      return binary_search_overrides(kRandoLocationPredOverrides_Inverted,
                                     kRandoLocationPredOverrides_InvertedCount,
                                     loc_id);
    case 3:  // kWorldState_Retro
      return binary_search_overrides(kRandoLocationPredOverrides_Retro,
                                     kRandoLocationPredOverrides_RetroCount,
                                     loc_id);
    default:
      return NULL;
  }
}

// ---------------------------------------------------------------------------
// Self-check — synthetic bytecode exercising each op.
//
// Pattern mirrors Rng_SelfCheck / Share_SelfCheck / Settings_SelfCheck:
// builds known inputs, asserts known outputs, exits 2 on failure.
// ---------------------------------------------------------------------------

static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[Logic_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

#define LSC_ASSERT(cond, msg) do { if (!(cond)) selfcheck_die(msg); } while (0)

static const RandoLocationDef *logic_selfcheck_find_location(uint16 loc_id) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    if (kRandoLocations[i].id == loc_id) return &kRandoLocations[i];
  }
  return NULL;
}

static const RandoRegionDef *logic_selfcheck_find_region(uint16 region_id) {
  for (uint32 i = 0; i < kRandoRegionsCount; i++) {
    if (kRandoRegions[i].id == region_id) return &kRandoRegions[i];
  }
  return NULL;
}

void Logic_SelfCheck(void) {
  RandoCounts counts;
  memset(&counts, 0, sizeof(counts));
  RandoSettings settings;
  Settings_SetDefaults(&settings);

#if defined(LOC_Eastern_Palace_Dark_Eyegore_Key_Drop) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_02_Green_Eyegore) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_04_Popo) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_05_Popo) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_06_Popo2) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_07_Popo2) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_08_Popo2) && \
    defined(LOC_Enemy_Check_Room_0x099_Slot_09_Popo2) && \
    defined(LOC_EasternPalace_Lobby_Pot_R099_P1428) && \
    defined(LOC_EasternPalace_Lobby_Pot_R099_P1454)
  // Eastern room 0x099 is entered through the vanilla big-key door from
  // room 0x0A9. Pin the generated predicates for every check domain that can
  // occupy that room: the forced Eyegore key drop, seven ordinary enemies,
  // and both pots. This is the exact self-lock class caught by the all-enemy
  // playtest; key-depth data intentionally treats big-key doors as open, so
  // these generated room predicates must carry the separate item gate.
  {
    static const uint16 kEasternDarknessLocations[] = {
      LOC_Eastern_Palace_Dark_Eyegore_Key_Drop,
      LOC_Enemy_Check_Room_0x099_Slot_02_Green_Eyegore,
      LOC_Enemy_Check_Room_0x099_Slot_04_Popo,
      LOC_Enemy_Check_Room_0x099_Slot_05_Popo,
      LOC_Enemy_Check_Room_0x099_Slot_06_Popo2,
      LOC_Enemy_Check_Room_0x099_Slot_07_Popo2,
      LOC_Enemy_Check_Room_0x099_Slot_08_Popo2,
      LOC_Enemy_Check_Room_0x099_Slot_09_Popo2,
      LOC_EasternPalace_Lobby_Pot_R099_P1428,
      LOC_EasternPalace_Lobby_Pot_R099_P1454,
    };
    RandoCounts dark_counts;
    memset(&dark_counts, 0, sizeof(dark_counts));
    for (uint32 i = 0; i < ITEM__COUNT; i++)
      dark_counts.by_item_id[i] = 4;
    // This fixture isolates the Big Key requirement. Newly appended Key Rings
    // are alternate small-key inventory, not part of the legacy "everything
    // else held" basis: an Eastern ring saturates the door-oracle key budget
    // and can open a different route into the tested room.
    for (uint16 item = ITEM_KeyRing_HyruleCastleEscape;
         item <= ITEM_KeyRing_GanonsTower; item++)
      dark_counts.by_item_id[item] = 0;
    dark_counts.by_item_id[ITEM_SkeletonKey] = 0;
    dark_counts.by_item_id[ITEM_BigKey_EasternPalace] = 0;
    for (uint32 i = 0; i < sizeof(kEasternDarknessLocations) / sizeof(kEasternDarknessLocations[0]); i++) {
      const RandoLocationDef *loc = logic_selfcheck_find_location(kEasternDarknessLocations[i]);
      LSC_ASSERT(loc != NULL, "Eastern Darkness location missing from generated registry");
      LSC_ASSERT(!Predicate_Evaluate(kRandoPredicateStream + loc->can_reach_offset,
                                    loc->can_reach_length, &dark_counts, &settings),
                 "Eastern Darkness location reachable without Eastern big key");
    }
    dark_counts.by_item_id[ITEM_BigKey_EasternPalace] = 1;
    for (uint32 i = 0; i < sizeof(kEasternDarknessLocations) / sizeof(kEasternDarknessLocations[0]); i++) {
      const RandoLocationDef *loc = logic_selfcheck_find_location(kEasternDarknessLocations[i]);
      LSC_ASSERT(Predicate_Evaluate(kRandoPredicateStream + loc->can_reach_offset,
                                   loc->can_reach_length, &dark_counts, &settings),
                 "Eastern Darkness location stayed closed with Eastern big key");
    }
  }
#endif

  // HAS_ITEM(5) when count[5] == 0 -> false
  {
    uint8 bc[] = { OP_HAS_ITEM, 5, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "HAS_ITEM(5) without item should be false");
  }
  // HAS_ITEM(5) when count[5] == 1 -> true
  counts.by_item_id[5] = 1;
  {
    uint8 bc[] = { OP_HAS_ITEM, 5, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "HAS_ITEM(5) with item should be true");
  }

  // HAS_AMOUNT(5, 2) when count[5] == 1 -> false
  {
    uint8 bc[] = { OP_HAS_AMOUNT, 5, 0, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "HAS_AMOUNT(5,2) with count=1 should be false");
  }
  // HAS_AMOUNT(5, 2) when count[5] == 2 -> true
  counts.by_item_id[5] = 2;
  {
    uint8 bc[] = { OP_HAS_AMOUNT, 5, 0, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "HAS_AMOUNT(5,2) with count=2 should be true");
  }
  counts.by_item_id[5] = 0;

  // HAS_ANY_OF([7, 8]) when neither set -> false
  {
    uint8 bc[] = { OP_HAS_ANY_OF, 2, 7, 0, 8, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "HAS_ANY_OF empty inventory should be false");
  }
  counts.by_item_id[8] = 1;
  {
    uint8 bc[] = { OP_HAS_ANY_OF, 2, 7, 0, 8, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "HAS_ANY_OF with one item set should be true");
  }
  counts.by_item_id[8] = 0;

  // HAS_ANY_COUNT([7,8,9], 2): 1+1+0 = 2 -> true; 1+0+0 = 1 -> false
  counts.by_item_id[7] = 1;
  counts.by_item_id[8] = 1;
  {
    uint8 bc[] = { OP_HAS_ANY_COUNT, 3, 7, 0, 8, 0, 9, 0, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "HAS_ANY_COUNT sum=2 threshold 2 should be true");
  }
  counts.by_item_id[8] = 0;
  {
    uint8 bc[] = { OP_HAS_ANY_COUNT, 3, 7, 0, 8, 0, 9, 0, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "HAS_ANY_COUNT sum=1 threshold 2 should be false");
  }
  // Vacuous HAS_ANY_COUNT edge cases:
  //   - count=0, n=0: sum 0 >= 0 → true (vacuous)
  //   - count=0, n=1: sum 0 >= 1 → false
  {
    uint8 bc[] = { OP_HAS_ANY_COUNT, 0, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "HAS_ANY_COUNT vacuous (count=0, n=0) should be true");
  }
  {
    uint8 bc[] = { OP_HAS_ANY_COUNT, 0, 1 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "HAS_ANY_COUNT vacuous (count=0, n=1) should be false");
  }
  counts.by_item_id[7] = 0;

  // Every HAS form observes a matching Key Ring as a saturated family-key
  // count, while Skeleton Key is absent and Retro keeps GenericKey exclusive.
  {
    RandoCounts ring_counts;
    memset(&ring_counts, 0, sizeof(ring_counts));
    ring_counts.by_item_id[ITEM_KeyRing_PalaceOfDarkness] = 1;
    ring_counts.by_item_id[ITEM_SkeletonKey] = 1;
    #define LSC_ITEM16(id) (uint8)((id) & 0xff), (uint8)((id) >> 8)
    uint8 has_item[] = {
      OP_HAS_ITEM, LSC_ITEM16(ITEM_SmallKey_PalaceOfDarkness),
    };
    uint8 has_amount[] = {
      OP_HAS_AMOUNT, LSC_ITEM16(ITEM_SmallKey_PalaceOfDarkness), 200,
    };
    uint8 has_any[] = {
      OP_HAS_ANY_OF, 2,
      LSC_ITEM16(ITEM_SmallKey_PalaceOfDarkness),
      LSC_ITEM16(ITEM_IceRod),
    };
    uint8 has_any_count[] = {
      OP_HAS_ANY_COUNT, 2,
      LSC_ITEM16(ITEM_SmallKey_PalaceOfDarkness),
      LSC_ITEM16(ITEM_IceRod), 200,
    };
    uint8 has_skeleton[] = {
      OP_HAS_ITEM, LSC_ITEM16(ITEM_SkeletonKey),
    };
    LSC_ASSERT(Predicate_Evaluate(has_item, sizeof(has_item), &ring_counts, &settings),
               "Key Ring must satisfy HAS_ITEM for its small-key family");
    LSC_ASSERT(Predicate_Evaluate(has_amount, sizeof(has_amount), &ring_counts, &settings),
               "Key Ring must saturate HAS_AMOUNT for its small-key family");
    LSC_ASSERT(Predicate_Evaluate(has_any, sizeof(has_any), &ring_counts, &settings),
               "Key Ring must satisfy HAS_ANY_OF for its small-key family");
    LSC_ASSERT(Predicate_Evaluate(has_any_count, sizeof(has_any_count),
                                 &ring_counts, &settings),
               "Key Ring must saturate HAS_ANY_COUNT for its small-key family");
    LSC_ASSERT(!Predicate_Evaluate(has_skeleton, sizeof(has_skeleton),
                                  &ring_counts, &settings),
               "Skeleton Key must be invisible to the predicate VM");
    ring_counts.by_item_id[ITEM_KeyRing_PalaceOfDarkness] = 0;
    LSC_ASSERT(!Predicate_Evaluate(has_item, sizeof(has_item), &ring_counts, &settings),
               "Skeleton Key must not satisfy small-key predicates");
    ring_counts.by_item_id[ITEM_KeyRing_PalaceOfDarkness] = 1;
    RandoSettings retro = settings;
    retro.world_state = kWorldState_Retro;
    LSC_ASSERT(!Predicate_Evaluate(has_item, sizeof(has_item), &ring_counts, &retro),
               "Retro must ignore per-dungeon Key Rings in favor of GenericKey");
    ring_counts.by_item_id[ITEM_GenericKey] = 1;
    LSC_ASSERT(Predicate_Evaluate(has_amount, sizeof(has_amount), &ring_counts, &retro),
               "Retro GenericKey wildcard semantics must remain unchanged");
    #undef LSC_ITEM16
  }

  // Skeleton Key is a runtime-only bonus: even a full reachability fixed point
  // must be bit-identical when its inventory cell is toggled. Door-oracle cache
  // identity separately canonicalizes the same cell above.
  {
    RandoCounts without_skeleton;
    memset(&without_skeleton, 0, sizeof without_skeleton);
    struct RandoReachability before =
        *Logic_ComputeReachabilityFullKnowledge(&without_skeleton, &settings);
    RandoCounts with_skeleton = without_skeleton;
    with_skeleton.by_item_id[ITEM_SkeletonKey] = 1;
    struct RandoReachability after =
        *Logic_ComputeReachabilityFullKnowledge(&with_skeleton, &settings);
    LSC_ASSERT(memcmp(&before, &after, sizeof before) == 0,
               "Skeleton Key changed logical reachability");
  }

  // WORLDSTATE_EQ(open) — defaults set world_state = Open (0)
  {
    uint8 bc[] = { OP_WORLDSTATE_EQ, kWorldState_Open };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "WORLDSTATE_EQ(open) against defaults should be true");
  }
  {
    uint8 bc[] = { OP_WORLDSTATE_EQ, kWorldState_Inverted };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "WORLDSTATE_EQ(inverted) against defaults should be false");
  }

  // GOAL_EQ(fast_ganon) — defaults set goal = FastGanon (1)
  {
    uint8 bc[] = { OP_GOAL_EQ, kGoal_FastGanon };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "GOAL_EQ(fast_ganon) against defaults should be true");
  }

  // AND with all true children
  {
    uint8 bc[] = {
      OP_AND, 2,
        OP_WORLDSTATE_EQ, kWorldState_Open,
        OP_GOAL_EQ, kGoal_FastGanon,
    };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "AND of two truths should be true");
  }
  // AND with one false child
  {
    uint8 bc[] = {
      OP_AND, 2,
        OP_WORLDSTATE_EQ, kWorldState_Inverted,
        OP_GOAL_EQ, kGoal_FastGanon,
    };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "AND with one false should be false");
  }
  // Vacuous AND -> TRUE
  {
    uint8 bc[] = { OP_AND, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "vacuous AND should be true");
  }
  // Vacuous OR -> FALSE
  {
    uint8 bc[] = { OP_OR, 0 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "vacuous OR should be false");
  }

  // OR with one truth
  {
    uint8 bc[] = {
      OP_OR, 2,
        OP_WORLDSTATE_EQ, kWorldState_Inverted,
        OP_GOAL_EQ, kGoal_FastGanon,
    };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "OR with one truth should be true");
  }

  // NOT
  {
    uint8 bc[] = { OP_NOT, OP_WORLDSTATE_EQ, kWorldState_Inverted };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "NOT (false) should be true");
  }

  // ITEM_IS in placement context — true iff candidate matches
  {
    uint8 bc[] = { OP_ITEM_IS, 42, 0 };
    LSC_ASSERT(Predicate_EvaluatePlacement(bc, sizeof(bc), &counts, &settings, 42, 0) == true,
               "ITEM_IS(42) with candidate 42 should be true");
    LSC_ASSERT(Predicate_EvaluatePlacement(bc, sizeof(bc), &counts, &settings, 41, 0) == false,
               "ITEM_IS(42) with candidate 41 should be false");
  }
  // Selected-family SmallKey/KeyRing aliases are symmetric in placement
  // context; an unselected or unrelated family remains distinct.
  {
    uint8 key_bc[] = {
      OP_ITEM_IS,
      (uint8)(ITEM_SmallKey_SwampPalace & 0xff),
      (uint8)(ITEM_SmallKey_SwampPalace >> 8),
    };
    uint8 ring_bc[] = {
      OP_ITEM_IS,
      (uint8)(ITEM_KeyRing_SwampPalace & 0xff),
      (uint8)(ITEM_KeyRing_SwampPalace >> 8),
    };
    uint16 sp_bit = (uint16)(1u << kRandoDungeon_SwampPalace);
    LSC_ASSERT(Predicate_EvaluatePlacement(
                   key_bc, sizeof(key_bc), &counts, &settings,
                   ITEM_KeyRing_SwampPalace, sp_bit),
               "selected Key Ring must satisfy OP_ITEM_IS(SmallKey)");
    LSC_ASSERT(Predicate_EvaluatePlacement(
                   ring_bc, sizeof(ring_bc), &counts, &settings,
                   ITEM_SmallKey_SwampPalace, sp_bit),
               "selected SmallKey must satisfy OP_ITEM_IS(KeyRing)");
    LSC_ASSERT(!Predicate_EvaluatePlacement(
                   key_bc, sizeof(key_bc), &counts, &settings,
                   ITEM_KeyRing_SwampPalace, 0),
               "unselected Key Ring must not alias OP_ITEM_IS(SmallKey)");
    LSC_ASSERT(!Predicate_EvaluatePlacement(
                   key_bc, sizeof(key_bc), &counts, &settings,
                   ITEM_KeyRing_TurtleRock,
                   (uint16)(sp_bit | (1u << kRandoDungeon_TurtleRock))),
               "different Key Ring family must not alias OP_ITEM_IS(SmallKey)");
  }

  // DUNGEON_CLEARED — driven by cleared_dungeons_bitmask in ctx
  {
    uint8 bc[] = { OP_DUNGEON_CLEARED, 5 };
    PredicateContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.counts = &counts;
    ctx.settings = &settings;
    ctx.cleared_dungeons_bitmask = (uint64)1 << 5;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == true,
               "DUNGEON_CLEARED with bit set should be true");
    ctx.cleared_dungeons_bitmask = 0;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == false,
               "DUNGEON_CLEARED with bit clear should be false");
  }

  // HAS_PRIZE — true iff the dungeon currently holding the prize is cleared
  {
    uint8 bc[] = { OP_HAS_PRIZE, 0 };  // prize id 0 = Prize_GreenPendant
    uint8 prize_assignment[kRandoDungeonCount];
    memset(prize_assignment, 0xff, sizeof(prize_assignment));
    prize_assignment[1] = 0;  // EasternPalace (id 1) holds the green pendant
    PredicateContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.counts = &counts;
    ctx.settings = &settings;
    ctx.dungeon_prize_assignment = prize_assignment;
    ctx.cleared_dungeons_bitmask = (uint64)1 << 1;  // EP cleared
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == true,
               "HAS_PRIZE when holding-dungeon cleared should be true");
    ctx.cleared_dungeons_bitmask = 0;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == false,
               "HAS_PRIZE when holding-dungeon not cleared should be false");
  }

  // MEDALLION_OPENS — true iff inventory has the medallion that opens the entrance
  {
    uint8 bc[] = { OP_MEDALLION_OPENS, 0 };  // entrance 0 = MiseryMire
    uint8 medallion_assignment[kRandoMedallionEntranceCount];
    // Misery Mire opens with item id 26 (Ether per item_registry)
    medallion_assignment[0] = 26;
    medallion_assignment[1] = 25;
    PredicateContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.counts = &counts;
    ctx.settings = &settings;
    ctx.medallion_entrance_assignment = medallion_assignment;
    counts.by_item_id[26] = 1;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == true,
               "MEDALLION_OPENS with required medallion should be true");
    counts.by_item_id[26] = 0;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == false,
               "MEDALLION_OPENS without required medallion should be false");
  }

  // CAN_KILL_BOSS — resolves the dungeon's ASSIGNED boss, then evaluates THAT
  // boss's kill predicate. The same op + dungeon yields different results under
  // different assignments — the core of boss-shuffle beatability. Self-contained
  // (local counts + default settings) so it doesn't disturb the shared fixtures.
  {
    uint8 bc[] = { OP_CAN_KILL_BOSS, 1 };  // dungeon 1 = EasternPalace
    RandoCounts c2;
    RandoSettings s2;
    Settings_SetDefaults(&s2);  // randomized weapons (not swordless)
    PredicateContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.counts = &c2;
    ctx.settings = &s2;

    // (a) NULL assignment -> vanilla boss: EP = Armos Knights (a single sword
    // suffices). No weapon -> false; a sword -> true.
    memset(&c2, 0, sizeof(c2));
    ctx.boss_assignment = NULL;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == false,
               "CAN_KILL_BOSS(EP) vanilla(Armos) with no weapon should be false");
    c2.by_item_id[ITEM_L1Sword] = 1;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == true,
               "CAN_KILL_BOSS(EP) vanilla(Armos) with a sword should be true");

    // (b) Shuffle Trinexx (boss-pool index 10) into EP. Trinexx needs FireRod
    // AND IceRod AND a real weapon tier — so the single L1 sword that killed
    // Armos is NOT enough. Proves the op tracks the SHUFFLED boss.
    uint8 boss_assignment[kRandoDungeonCount];
    memset(boss_assignment, 0xff, sizeof(boss_assignment));
    boss_assignment[1] = 10;  // EasternPalace <- Trinexx
    ctx.boss_assignment = boss_assignment;
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == false,
               "CAN_KILL_BOSS(EP) shuffled(Trinexx) with only an L1 sword should be false");
    memset(&c2, 0, sizeof(c2));
    c2.by_item_id[ITEM_FireRod] = 1;
    c2.by_item_id[ITEM_IceRod] = 1;
    c2.by_item_id[ITEM_ProgressiveSword] = 3;  // HasSword3 satisfies both sword conjuncts
    LSC_ASSERT(Predicate_EvalCtx(bc, sizeof(bc), &ctx) == true,
               "CAN_KILL_BOSS(EP) shuffled(Trinexx) with fire+ice+sword3 should be true");

    // (c) No-boss dungeon (HCE=0 -> vanilla map 0xFF) resolves to false, not OOB.
    uint8 bc0[] = { OP_CAN_KILL_BOSS, 0 };
    ctx.boss_assignment = NULL;
    LSC_ASSERT(Predicate_EvalCtx(bc0, sizeof(bc0), &ctx) == false,
               "CAN_KILL_BOSS(HCE=no boss) should be false");
  }

  // Dungeon-chains: generated BossRoom factoring invariants. These regions are
  // intentionally inert while chains are off: one edge carries the approach
  // predicate, and the Boss/Prize locations are homed in the derived room.
  {
    typedef struct ChainBossRoomCheck {
      const char *region_name;
      uint16 boss_loc;
      uint16 prize_loc;
    } ChainBossRoomCheck;
    static const ChainBossRoomCheck kChainBossRooms[] = {
      { "EasternPalace_BossRoom", LOC_Eastern_Palace_Boss, LOC_Eastern_Palace_Prize },
      { "DesertPalace_BossRoom", LOC_Desert_Palace_Boss, LOC_Desert_Palace_Prize },
      { "TowerOfHera_BossRoom", LOC_Tower_of_Hera_Boss, LOC_Tower_of_Hera_Prize },
      { "PalaceOfDarkness_BossRoom", LOC_Palace_of_Darkness_Boss, LOC_Palace_of_Darkness_Prize },
      { "SwampPalace_BossRoom", LOC_Swamp_Palace_Boss, LOC_Swamp_Palace_Prize },
      { "ThievesTown_BossRoom", LOC_Thieves_Town_Boss, LOC_Thieves_Town_Prize },
      { "IcePalace_BossRoom", LOC_Ice_Palace_Boss, LOC_Ice_Palace_Prize },
      { "MiseryMire_BossRoom", LOC_Misery_Mire_Boss, LOC_Misery_Mire_Prize },
      { "TurtleRock_BossRoom", LOC_Turtle_Rock_Boss, LOC_Turtle_Rock_Prize },
    };

    for (uint32 i = 0; i < sizeof(kChainBossRooms) / sizeof(kChainBossRooms[0]); i++) {
      const ChainBossRoomCheck *row = &kChainBossRooms[i];
      uint16 boss_region = Rando_FindRegionByName(row->region_name);
      LSC_ASSERT(boss_region != 0xFFFF,
                 "dungeon-chain BossRoom region missing");
      const RandoRegionDef *region = logic_selfcheck_find_region(boss_region);
      LSC_ASSERT(region != NULL,
                 "dungeon-chain BossRoom region id not present in kRandoRegions");

      uint32 inbound = 0;
      uint16 inbound_from = 0xFFFF;
      for (uint32 e = 0; e < kRandoEdgesCount; e++) {
        if (kRandoEdges[e].to_region == boss_region) {
          inbound++;
          inbound_from = kRandoEdges[e].from_region;
        }
      }
      LSC_ASSERT(inbound == 1,
                 "dungeon-chain BossRoom must have exactly one inbound edge");
      LSC_ASSERT(region->parent_id == inbound_from,
                 "dungeon-chain BossRoom inbound edge must come from parent region");

      const RandoLocationDef *boss = logic_selfcheck_find_location(row->boss_loc);
      const RandoLocationDef *prize = logic_selfcheck_find_location(row->prize_loc);
      LSC_ASSERT(boss != NULL && prize != NULL,
                 "dungeon-chain Boss/Prize location missing");
      LSC_ASSERT(boss->region_id == boss_region && prize->region_id == boss_region,
                 "dungeon-chain Boss/Prize locations must be homed in BossRoom");
    }
  }

  // OP_TRICK / OP_GLITCH_LEVEL_AT_LEAST are wired (Slice 4) but resolve to their
  // OFF branch under default settings (tricks=0, logic=NoGlitches).
  {
    uint8 bc[] = { OP_TRICK, 5 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "OP_TRICK(5) should be false under default settings (tricks=0)");
  }
  {
    uint8 bc[] = { OP_GLITCH_LEVEL_AT_LEAST, 1 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "OP_GLITCH_LEVEL_AT_LEAST(1) should be false in Phase A (logic=NoGlitches)");
  }

  // ON-state coverage: assert the bit-shift / threshold math actually fires when
  // the axis is enabled. Without this, a regression in `1u << trick_id` or the
  // `>= level` compare passes both selftest and the (default-settings) corpus
  // silently and only surfaces in playtest — the exact gap this project keeps
  // hitting. Uses a scratch copy so the shared `settings` stays at defaults.
  {
    RandoSettings on = settings;
    uint8 bc[] = { OP_TRICK, 5 };
    on.tricks = (uint8)(1u << 5);
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &on) == true,
               "OP_TRICK(5) should be true when settings.tricks bit 5 is set");
    on.tricks = (uint8)(1u << 4);  // adjacent bit must NOT satisfy trick 5
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &on) == false,
               "OP_TRICK(5) must not be satisfied by an adjacent trick bit");
  }
  {
    RandoSettings on = settings;
    on.logic = 1;  // one glitch tier above NoGlitches
    uint8 bc1[] = { OP_GLITCH_LEVEL_AT_LEAST, 1 };
    LSC_ASSERT(Predicate_Evaluate(bc1, sizeof(bc1), &counts, &on) == true,
               "OP_GLITCH_LEVEL_AT_LEAST(1) should be true when settings.logic>=1");
    uint8 bc2[] = { OP_GLITCH_LEVEL_AT_LEAST, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc2, sizeof(bc2), &counts, &on) == false,
               "OP_GLITCH_LEVEL_AT_LEAST(2) should be false when settings.logic==1");
  }
  // OP_MODEWEAPONS_EQ (swordless) — off under default (randomized=0), on at 3.
  {
    uint8 bc[] = { OP_MODEWEAPONS_EQ, 3 };  // 3 = swordless
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "OP_MODEWEAPONS_EQ(swordless) should be false under default (randomized)");
    RandoSettings on = settings;
    on.mode_weapons = 3;
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &on) == true,
               "OP_MODEWEAPONS_EQ(swordless) should be true when mode_weapons==3");
    uint8 bc0[] = { OP_MODEWEAPONS_EQ, 0 };  // randomized
    LSC_ASSERT(Predicate_Evaluate(bc0, sizeof(bc0), &counts, &on) == false,
               "OP_MODEWEAPONS_EQ(randomized) should be false when mode_weapons==3");
  }
  // OP_INSTANT_FLUTE — default on; explicit off restores manual activation.
  {
    uint8 bc[] = { OP_INSTANT_FLUTE };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "OP_INSTANT_FLUTE should be true under default settings");
    RandoSettings off = settings;
    off.instant_flute = 0;
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &off) == false,
               "OP_INSTANT_FLUTE should be false when instant_flute=0");
  }
  // add-rando-ow-warp-shuffle — OP_OW_FLUTE_VANILLA: the CanFly
  // neutralization conjunct. True at defaults (flute vanilla), false under
  // an active flute shuffle.
  {
    uint8 bc[] = { OP_OW_FLUTE_VANILLA };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "OP_OW_FLUTE_VANILLA should be true under default settings");
    RandoSettings fs = settings;
    fs.flute_shuffle = kFluteShuffle_Random;
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &fs) == false,
               "OP_OW_FLUTE_VANILLA should be false under flute_shuffle");
  }
  // add-rando-ow-warp-shuffle — substrate invariants: the region budget
  // (regions at/above kReachabilityMaxRegions are silently never walked),
  // the warp suffix bounds, and present/absent coherence.
  LSC_ASSERT(kRandoRegionsCount < kReachabilityMaxRegions,
             "kRandoRegionsCount must stay BELOW kReachabilityMaxRegions "
             "(the top id is the reserved entrance cross-void sink)");
  LSC_ASSERT(kRandoOwRegionsFirst <= kRandoRegionsCount,
             "kRandoOwRegionsFirst must be within the region table");
  LSC_ASSERT(kRandoNonWarpEdgesCount <= kRandoEdgesCount,
             "kRandoNonWarpEdgesCount must be within the edge table");
  LSC_ASSERT(kRandoOwGraphPresent
                 ? (kRandoOwRegionsFirst < kRandoRegionsCount &&
                    kRandoNonWarpEdgesCount < kRandoEdgesCount &&
                    kRandoOwFluteCandidatesCount > 0 &&
                    kRandoOwWhirlpoolsCount > 0 &&
                    kRandoOwFluteHubRegion < kRandoRegionsCount)
                 : (kRandoOwRegionsFirst == kRandoRegionsCount &&
                    kRandoNonWarpEdgesCount == kRandoEdgesCount &&
                    kRandoOwFluteCandidatesCount == 0),
             "OW graph present/absent tables must be coherent");

  // Enemy-drop key-depth mode ops: Wild and Dungeon are distinct because Wild
  // keys must be held before entry, while Dungeon keys can be collected en route.
  {
    uint8 wild_bc[] = { OP_ENEMY_DROP_KEYS_WILD };
    uint8 dungeon_bc[] = { OP_ENEMY_DROP_KEYS_DUNGEON };
    RandoSettings ed = settings;
    ed.enemy_drop_checks = kEnemyDropChecks_Keys;

    ed.dungeon_small_keys_mode = kDungeonItemMode_Wild;
    LSC_ASSERT(Predicate_Evaluate(wild_bc, sizeof(wild_bc), &counts, &ed) == true,
               "OP_ENEMY_DROP_KEYS_WILD should be true under enemy-drop Wild keys");
    LSC_ASSERT(Predicate_Evaluate(dungeon_bc, sizeof(dungeon_bc), &counts, &ed) == false,
               "OP_ENEMY_DROP_KEYS_DUNGEON should be false under enemy-drop Wild keys");

    ed.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;
    LSC_ASSERT(Predicate_Evaluate(wild_bc, sizeof(wild_bc), &counts, &ed) == false,
               "OP_ENEMY_DROP_KEYS_WILD should be false under enemy-drop Dungeon keys");
    LSC_ASSERT(Predicate_Evaluate(dungeon_bc, sizeof(dungeon_bc), &counts, &ed) == true,
               "OP_ENEMY_DROP_KEYS_DUNGEON should be true under enemy-drop Dungeon keys");

    ed.dungeon_small_keys_mode = kDungeonItemMode_Vanilla;
    LSC_ASSERT(Predicate_Evaluate(wild_bc, sizeof(wild_bc), &counts, &ed) == false,
               "OP_ENEMY_DROP_KEYS_WILD should be false under Vanilla small keys");
    LSC_ASSERT(Predicate_Evaluate(dungeon_bc, sizeof(dungeon_bc), &counts, &ed) == false,
               "OP_ENEMY_DROP_KEYS_DUNGEON should be false under Vanilla small keys");
  }

  // OP_DIFFICULTY_AT_LEAST against defaults (normal=1)
  {
    uint8 bc[] = { OP_DIFFICULTY_AT_LEAST, 1 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == true,
               "OP_DIFFICULTY_AT_LEAST(1) against defaults (normal=1) should be true");
  }
  {
    uint8 bc[] = { OP_DIFFICULTY_AT_LEAST, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &settings) == false,
               "OP_DIFFICULTY_AT_LEAST(2) against defaults (normal=1) should be false");
  }
  {
    // ON-state: difficulty axis above default must satisfy the higher threshold.
    RandoSettings on = settings;
    on.item_pool_difficulty = 2;
    uint8 bc[] = { OP_DIFFICULTY_AT_LEAST, 2 };
    LSC_ASSERT(Predicate_Evaluate(bc, sizeof(bc), &counts, &on) == true,
               "OP_DIFFICULTY_AT_LEAST(2) should be true when item_pool_difficulty>=2");
  }

  // Inverted reachability self-check. Verify the world-state
  // override path actually fires by computing reachability under Inverted
  // and asserting that `LinksHouse_Inverted` (the Inverted start region)
  // is reachable while `LinksHouse` (the Standard start) is NOT.
  // Exercises:
  //   - `Logic_ComputeReachabilityFullKnowledge` running under `world_state = 2`.
  //   - `kRandoStartRegionByWorldState[Inverted]` resolving to the right id.
  //   - `kRandoEdges_Inverted` being walked (the trivial
  //     LinksHouse_Inverted → DarkWorld_South edge means DW_South should
  //     also be reachable).
  {
    RandoSettings inv_settings;
    Settings_SetDefaults(&inv_settings);
    inv_settings.world_state = 2;  // kWorldState_Inverted
    RandoCounts inv_counts;
    memset(&inv_counts, 0, sizeof(inv_counts));
    // Grant the full pool (every item id has count >= 1) so we're
    // checking GRAPH reachability, not item gating.
    for (int i = 0; i < 256; i++) inv_counts.by_item_id[i] = 99;
    const RandoReachability *inv_reach =
        Logic_ComputeReachabilityFullKnowledge(&inv_counts, &inv_settings);
    LSC_ASSERT(inv_reach != NULL,
               "Logic_ComputeReachabilityFullKnowledge returned NULL under Inverted");

    uint16 lhi = Rando_FindRegionByName("LinksHouse_Inverted");
    uint16 lhs = Rando_FindRegionByName("LinksHouse");
    LSC_ASSERT(lhi != 0xFFFF,
               "LinksHouse_Inverted region must exist for Inverted seeds");
    if (lhi != 0xFFFF && inv_reach != NULL) {
      LSC_ASSERT(Reachability_HasRegion(inv_reach, lhi),
                 "LinksHouse_Inverted should be reachable under world_state=Inverted");
    }
    // The Standard start region (LinksHouse) should NOT be in the reachable set
    // under Inverted. NOTE: this exclusion is by GRAPH TOPOLOGY — it is
    // not the Inverted start region and no edge points TO it — NOT by its
    // world_state_filter. Region world_state_filter is metadata that
    // reachability never consults (only LOCATIONS are ws-filtered); per-world
    // region scoping must therefore be done via edges, not the filter.
    if (lhs != 0xFFFF && lhs != lhi && inv_reach != NULL) {
      LSC_ASSERT(!Reachability_HasRegion(inv_reach, lhs),
                 "LinksHouse should NOT be reachable under world_state=Inverted "
                 "(gated by world_state_filter)");
    }
  }

  // genericKeys (Retro) small-key collapse — the cross-dungeon key dependency.
  // Under Retro, any per-dungeon small-key predicate is satisfied by holding
  // >=1 GenericKey, so a single generic key opens BOTH a PoD 5-key door AND a
  // TR 4-key door (the shared-pool semantics). Big keys are NOT collapsed. When
  // genericKeys is off the per-dungeon count rules unchanged.
  {
    RandoCounts gk;
    memset(&gk, 0, sizeof(gk));
    RandoSettings retro;
    Settings_SetDefaults(&retro);
    retro.world_state = kWorldState_Retro;

    // Operand bytes (item ids are u16 little-endian in the bytecode).
    #define LSC_U16(id) (uint8)((id) & 0xff), (uint8)((id) >> 8)
    uint8 pod5[] = { OP_HAS_AMOUNT, LSC_U16(ITEM_SmallKey_PalaceOfDarkness), 5 };
    uint8 tr4[]  = { OP_HAS_AMOUNT, LSC_U16(ITEM_SmallKey_TurtleRock), 4 };
    uint8 sp1[]  = { OP_HAS_ITEM,   LSC_U16(ITEM_SmallKey_SwampPalace) };
    uint8 bkpod[]= { OP_HAS_ITEM,   LSC_U16(ITEM_BigKey_PalaceOfDarkness) };

    // 0 generic keys → every small-key door closed (no per-dungeon keys exist).
    LSC_ASSERT(Predicate_Evaluate(pod5, sizeof(pod5), &gk, &retro) == false,
               "Retro: PoD 5-key door must be closed with 0 generic keys");
    LSC_ASSERT(Predicate_Evaluate(tr4, sizeof(tr4), &gk, &retro) == false,
               "Retro: TR 4-key door must be closed with 0 generic keys");

    // 1 generic key → BOTH the PoD-5 and TR-4 doors open (cross-dungeon share),
    // and the Swamp Palace single-key door opens — all from one shared key.
    gk.by_item_id[ITEM_GenericKey] = 1;
    LSC_ASSERT(Predicate_Evaluate(pod5, sizeof(pod5), &gk, &retro) == true,
               "Retro: 1 generic key must open the PoD 5-key door (collapse)");
    LSC_ASSERT(Predicate_Evaluate(tr4, sizeof(tr4), &gk, &retro) == true,
               "Retro: the SAME generic key must open the TR 4-key door (cross-dungeon)");
    LSC_ASSERT(Predicate_Evaluate(sp1, sizeof(sp1), &gk, &retro) == true,
               "Retro: 1 generic key must open the Swamp Palace small-key door");
    // Big keys keep per-dungeon identity — NOT satisfied by a generic key.
    LSC_ASSERT(Predicate_Evaluate(bkpod, sizeof(bkpod), &gk, &retro) == false,
               "Retro: a generic key must NOT satisfy a BigKey requirement");

    // Non-Retro: the collapse is OFF — a generic key does not open a small-key
    // door (the per-dungeon count rules, and no per-dungeon key is held).
    RandoSettings open;
    Settings_SetDefaults(&open);  // world_state defaults to Open
    LSC_ASSERT(Predicate_Evaluate(pod5, sizeof(pod5), &gk, &open) == false,
               "non-Retro: a generic key must NOT collapse small-key doors");
    #undef LSC_U16
  }

  // Enemy-drop HCE chain regression: Boomerang side requires the Map Guard key,
  // Ball-n-chain requires a second HCE key, Zelda's Cell requires that big-key
  // side effect, and the Zelda rescue event requires the sewer key as well.
  if (kRandoEnemyDropHceBigKeyLocId != 0xFFFF) {
    RandoSettings hce;
    Settings_SetDefaults(&hce);
    hce.enemy_drop_checks = kEnemyDropChecks_Keys;
    hce.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;

    RandoCounts hc;
    memset(&hc, 0, sizeof(hc));
    hc.by_item_id[ITEM_StartingHeart] = 3;
    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 2;

    // Generated forced-drop predicates must follow the same Open parent as the
    // static chests. Ordinary renewable bombs are enough to collect the guard
    // keys and defeat Ball-and-chain in Open, which then derives HCE's virtual
    // big key for Zelda's Cell.
    const RandoReachability *hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
    LSC_ASSERT(hr != NULL, "HCE enemy-drop chain reachability returned NULL");
#ifdef LOC_Hyrule_Castle_Map_Guard_Key_Drop
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Map_Guard_Key_Drop),
               "Open HCE enemy-drop: renewable bombs should reach Map Guard");
#endif
    LSC_ASSERT(Reachability_HasLocation(hr, kRandoEnemyDropHceBigKeyLocId),
               "Open HCE enemy-drop: two keys should reach Ball-n-chain weaponless");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Open HCE enemy-drop: virtual big key should open Zelda's Cell");

    // Standard deliberately remains stricter: general renewable-bomb access
    // does not satisfy the opening escape's concrete weapon/refill contract.
    hce.world_state = kWorldState_Standard;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
#ifdef LOC_Hyrule_Castle_Map_Guard_Key_Drop
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Map_Guard_Key_Drop),
               "Standard HCE enemy-drop: Map Guard must require an escape weapon");
#endif
    LSC_ASSERT(!Reachability_HasLocation(hr, kRandoEnemyDropHceBigKeyLocId),
               "Standard HCE enemy-drop: Ball-n-chain must require an escape weapon");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Standard HCE enemy-drop: Zelda's Cell must require an escape weapon");

    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 0;
    hc.by_item_id[ITEM_ProgressiveSword] = 1;
    hc.by_item_id[ITEM_Lamp] = 1;

    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Boomerang_Chest),
               "HCE enemy-drop: Boomerang Chest must require the first HCE key");
    LSC_ASSERT(!Reachability_HasLocation(hr, kRandoEnemyDropHceBigKeyLocId),
               "HCE enemy-drop: Ball-n-chain must require the second HCE key");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "HCE enemy-drop: Zelda's Cell must not be reachable with zero HCE keys");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Zelda),
               "HCE enemy-drop: Zelda rescue must not be reachable with zero HCE keys");

    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 1;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Boomerang_Chest),
               "HCE enemy-drop: first HCE key should open Boomerang Chest");
    LSC_ASSERT(!Reachability_HasLocation(hr, kRandoEnemyDropHceBigKeyLocId),
               "HCE enemy-drop: one HCE key must not reach Ball-n-chain");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "HCE enemy-drop: one HCE key must not reach Zelda's Cell");

    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 2;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
    LSC_ASSERT(Reachability_HasLocation(hr, kRandoEnemyDropHceBigKeyLocId),
               "HCE enemy-drop: two HCE keys should reach Ball-n-chain");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "HCE enemy-drop: Ball-n-chain side effect should open Zelda's Cell");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Zelda),
               "HCE enemy-drop: Zelda rescue must require the sewer key");

    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 3;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &hce);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Zelda),
               "HCE enemy-drop: third HCE key should allow Zelda rescue");
  }

  // General bomb access is renewable, but the Standard escape's bombs-as-weapon
  // branch still requires a concrete refill so the runtime escape assist has
  // something to supply.
  {
    RandoSettings sescape;
    Settings_SetDefaults(&sescape);
    sescape.world_state = kWorldState_Standard;
    RandoCounts ec;
    memset(&ec, 0, sizeof(ec));
    ec.by_item_id[ITEM_StartingHeart] = 3;
    const RandoReachability *er = Logic_ComputeReachabilityFullKnowledge(&ec, &sescape);
    LSC_ASSERT(!Reachability_HasLocation(er, LOC_Hyrule_Castle_Boomerang_Chest),
               "Standard escape: renewable bomb access must not waive the weapon gate");
    ec.by_item_id[ITEM_Bombs10] = 1;
    er = Logic_ComputeReachabilityFullKnowledge(&ec, &sescape);
    LSC_ASSERT(Reachability_HasLocation(er, LOC_Hyrule_Castle_Boomerang_Chest),
               "Standard escape: a bomb refill should satisfy the weapon gate");
  }

  // Open/Retro inherit HCE overrides from ALTTPR's Open parent. Dark Cross only
  // needs the Lamp, while Boomerang Chest and Zelda's Cell use the ordinary
  // renewable-bomb combat predicate instead of Standard's stricter escape
  // predicate. Generated key-depth, HCE-big-key, and soul terms compose after
  // these base predicates when their settings are active.
  {
    RandoSettings hce_parent;
    Settings_SetDefaults(&hce_parent);  // Open, enemy drops/souls off
    RandoCounts hce_counts;
    memset(&hce_counts, 0, sizeof(hce_counts));
    hce_counts.by_item_id[ITEM_StartingHeart] = 3;

    const RandoReachability *hce_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Open HCE: Dark Cross must still require the Lamp");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Open HCE: renewable bombs should satisfy Boomerang Chest combat");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Open HCE: renewable bombs should satisfy Zelda's Cell combat");

    hce_counts.by_item_id[ITEM_Lamp] = 1;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Open HCE: Lamp alone should open Dark Cross");

    hce_parent.world_state = kWorldState_Retro;
    hce_counts.by_item_id[ITEM_Lamp] = 0;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Retro HCE: Dark Cross must still require the Lamp");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Retro HCE: renewable bombs should satisfy Boomerang Chest combat");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Retro HCE: renewable bombs should satisfy Zelda's Cell combat");
    hce_counts.by_item_id[ITEM_Lamp] = 1;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Retro HCE: Lamp alone should open Dark Cross");

    hce_parent.world_state = kWorldState_Standard;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Standard HCE: Lamp must not waive the escape-weapon gate");
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Standard HCE: renewable bombs must not waive Boomerang combat");
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Standard HCE: renewable bombs must not waive Zelda's Cell combat");

    hce_counts.by_item_id[ITEM_Bombs1] = 1;
    hce_counts.by_item_id[ITEM_Lamp] = 0;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Standard HCE: a bomb refill must not waive Dark Cross's Lamp");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Standard HCE: a bomb refill should satisfy Boomerang combat");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Standard HCE: a bomb refill should satisfy Zelda's Cell combat");
    hce_counts.by_item_id[ITEM_Lamp] = 1;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Standard HCE: Lamp plus a bomb refill should open Dark Cross");

    hce_parent.world_state = kWorldState_Inverted;
    hce_counts.by_item_id[ITEM_Bombs1] = 0;
    hce_counts.by_item_id[ITEM_MoonPearl] = 1;
    hce_counts.by_item_id[ITEM_Flippers] = 1;
    hce_counts.by_item_id[ITEM_DefeatAgahnim] = 1;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Sewers_Dark_Cross),
               "Inverted HCE: inherited Open Dark Cross should need only Lamp");
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Inverted HCE: Boomerang Chest must retain its key override");
    LSC_ASSERT(!Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Inverted HCE: Zelda's Cell must retain its key override");
    hce_counts.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 1;
    hce_reach = Logic_ComputeReachabilityFullKnowledge(&hce_counts, &hce_parent);
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Boomerang_Chest),
               "Inverted HCE: one HCE key should open Boomerang Chest");
    LSC_ASSERT(Reachability_HasLocation(hce_reach, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Inverted HCE: one HCE key should open Zelda's Cell");
  }

#if defined(LOC_Enemy_Check_Room_0x071_Slot_00_Green_sword_soldier) && \
    defined(LOC_HyruleCastleEscape_Pot_R011_P0F90) && \
    defined(LOC_HyruleCastleEscape_Pot_R021_P14A0) && \
    defined(LOC_HyruleCastleEscape_Pot_R032_P0D1C)
  // Generated ordinary-enemy and pot checks must not regress independently of
  // the static/forced-drop predicates above. Exercise one reviewed HCE guard
  // (no throwable-pot escape hatch) and one explicit HCE pot-room override in
  // every non-Standard world state, then prove Standard remains strict.
  {
    RandoSettings hce_generated;
    Settings_SetDefaults(&hce_generated);
    hce_generated.enemy_drop_checks = kEnemyDropChecks_All;
    hce_generated.pot_shuffle = kPotShuffle_All;
    hce_generated.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;

    RandoCounts hce_generated_counts;
    memset(&hce_generated_counts, 0, sizeof(hce_generated_counts));
    hce_generated_counts.by_item_id[ITEM_StartingHeart] = 3;
    hce_generated_counts.by_item_id[ITEM_Lamp] = 1;
    hce_generated_counts.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 4;
    hce_generated_counts.by_item_id[ITEM_GenericKey] = 4;

    const RandoReachability *hce_generated_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_generated_counts, &hce_generated);
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach,
                   LOC_Enemy_Check_Room_0x071_Slot_00_Green_sword_soldier),
               "Open HCE enemy check: renewable bombs should satisfy combat");
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R021_P14A0),
               "Open HCE pot check: renewable bombs should satisfy room combat");

    hce_generated.world_state = kWorldState_Retro;
    hce_generated_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_generated_counts, &hce_generated);
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach,
                   LOC_Enemy_Check_Room_0x071_Slot_00_Green_sword_soldier),
               "Retro HCE enemy check: renewable bombs should satisfy combat");
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R021_P14A0),
               "Retro HCE pot check: renewable bombs should satisfy room combat");

    hce_generated.world_state = kWorldState_Inverted;
    hce_generated_counts.by_item_id[ITEM_MoonPearl] = 1;
    hce_generated_counts.by_item_id[ITEM_Flippers] = 1;
    hce_generated_counts.by_item_id[ITEM_DefeatAgahnim] = 1;
    hce_generated_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_generated_counts, &hce_generated);
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach,
                   LOC_Enemy_Check_Room_0x071_Slot_00_Green_sword_soldier),
               "Inverted HCE enemy check: renewable bombs should satisfy combat");
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R021_P14A0),
               "Inverted HCE pot check: renewable bombs should satisfy room combat");
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R032_P0D1C),
               "Inverted HCE Dark Cross pot: Lamp should satisfy the parent override");

    // The Inverted Secret Room override also inherits into its room's generated
    // pots: the graveyard approach needs lifting strength, not the forward-route
    // key and Lamp. This guards the other world-state-specific chest anchor.
    hce_generated_counts.by_item_id[ITEM_Lamp] = 0;
    hce_generated_counts.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 0;
    hce_generated_counts.by_item_id[ITEM_PowerGlove] = 1;
    hce_generated_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_generated_counts, &hce_generated);
    LSC_ASSERT(Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R011_P0F90),
               "Inverted HCE Secret Room pot: lifting strength should open back route");

    hce_generated.world_state = kWorldState_Standard;
    hce_generated_counts.by_item_id[ITEM_MoonPearl] = 0;
    hce_generated_counts.by_item_id[ITEM_Flippers] = 0;
    hce_generated_counts.by_item_id[ITEM_DefeatAgahnim] = 0;
    hce_generated_counts.by_item_id[ITEM_PowerGlove] = 0;
    hce_generated_counts.by_item_id[ITEM_Lamp] = 1;
    hce_generated_counts.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 4;
    hce_generated_reach =
        Logic_ComputeReachabilityFullKnowledge(&hce_generated_counts, &hce_generated);
    LSC_ASSERT(!Reachability_HasLocation(
                   hce_generated_reach,
                   LOC_Enemy_Check_Room_0x071_Slot_00_Green_sword_soldier),
               "Standard HCE enemy check: combat must require an escape weapon");
    LSC_ASSERT(!Reachability_HasLocation(
                   hce_generated_reach, LOC_HyruleCastleEscape_Pot_R021_P14A0),
               "Standard HCE pot check: room combat must require an escape weapon");
  }
#endif

  // Open/Retro sewer back entrance: lift the graveyard rock, drop through the
  // grave, then bomb/dash the cracked wall. Ordinary bomb access is renewable,
  // so Power Glove alone is the inventory gate. Standard must not inherit this
  // overworld shortcut and still needs its key + Lamp + escape weapon route.
  {
    static const uint16 kSewerSecretRoomLocations[] = {
      LOC_Sewers_Secret_Room_Left,
      LOC_Sewers_Secret_Room_Middle,
      LOC_Sewers_Secret_Room_Right,
    };
    RandoSettings sewer;
    Settings_SetDefaults(&sewer);  // Open
    RandoCounts sewer_counts;
    memset(&sewer_counts, 0, sizeof(sewer_counts));
    sewer_counts.by_item_id[ITEM_StartingHeart] = 3;

    const RandoReachability *sewer_reach =
        Logic_ComputeReachabilityFullKnowledge(&sewer_counts, &sewer);
    for (uint32 i = 0; i < sizeof(kSewerSecretRoomLocations) /
                                sizeof(kSewerSecretRoomLocations[0]); i++) {
      LSC_ASSERT(!Reachability_HasLocation(sewer_reach, kSewerSecretRoomLocations[i]),
                 "Open sewers: back entrance must require lifting the graveyard rock");
    }

    sewer_counts.by_item_id[ITEM_ProgressiveGlove] = 1;
    sewer_reach = Logic_ComputeReachabilityFullKnowledge(&sewer_counts, &sewer);
    for (uint32 i = 0; i < sizeof(kSewerSecretRoomLocations) /
                                sizeof(kSewerSecretRoomLocations[0]); i++) {
      LSC_ASSERT(Reachability_HasLocation(sewer_reach, kSewerSecretRoomLocations[i]),
                 "Open sewers: Power Glove should open the graveyard back route");
    }

    sewer.world_state = kWorldState_Retro;
    sewer_reach = Logic_ComputeReachabilityFullKnowledge(&sewer_counts, &sewer);
    for (uint32 i = 0; i < sizeof(kSewerSecretRoomLocations) /
                                sizeof(kSewerSecretRoomLocations[0]); i++) {
      LSC_ASSERT(Reachability_HasLocation(sewer_reach, kSewerSecretRoomLocations[i]),
                 "Retro sewers: Power Glove should open the graveyard back route");
    }

    sewer.world_state = kWorldState_Standard;
    sewer_reach = Logic_ComputeReachabilityFullKnowledge(&sewer_counts, &sewer);
    for (uint32 i = 0; i < sizeof(kSewerSecretRoomLocations) /
                                sizeof(kSewerSecretRoomLocations[0]); i++) {
      LSC_ASSERT(!Reachability_HasLocation(sewer_reach, kSewerSecretRoomLocations[i]),
                 "Standard sewers: Power Glove must not waive the forward escape route");
    }
  }

  // add-enemy-souls — kill-gated-room soul wraps (soul_rooms.gen.yaml ->
  // NeedsEnemySoul terms). Mini Moldorm Cave is the isolated probe: entry
  // is generally accessible, and its chests sit behind the shutter the Mini
  // Moldorms hold shut while suppressed. souls_shuffle=all must gate the chests
  // on the soul; off must stay pre-souls-identical (term inert below the tier).
  if (kRandoSoulRoomsBaked) {
    RandoSettings ssoul;
    Settings_SetDefaults(&ssoul);
    ssoul.souls_shuffle = kSoulsShuffle_BossesEnemies;

    RandoCounts sc;
    memset(&sc, 0, sizeof(sc));
    sc.by_item_id[ITEM_StartingHeart] = 3;
    sc.by_item_id[ITEM_RescuedZelda] = 1;  // Open pre-grant (mirrors the sphere walker)

    const RandoReachability *sr = Logic_ComputeReachabilityFullKnowledge(&sc, &ssoul);
    LSC_ASSERT(sr != NULL, "souls kill-room reachability returned NULL");
    LSC_ASSERT(!Reachability_HasLocation(sr, LOC_Mini_Moldorm_Cave_Far_Left),
               "souls=all: Mini Moldorm Cave chest must require the Mini Moldorm soul");

    sc.by_item_id[ITEM_Soul_MiniMoldorm] = 1;
    sr = Logic_ComputeReachabilityFullKnowledge(&sc, &ssoul);
    LSC_ASSERT(Reachability_HasLocation(sr, LOC_Mini_Moldorm_Cave_Far_Left),
               "souls=all: the Mini Moldorm soul should open the cave's chests");

    RandoSettings soff;
    Settings_SetDefaults(&soff);
    RandoCounts sc0;
    memset(&sc0, 0, sizeof(sc0));
    sc0.by_item_id[ITEM_StartingHeart] = 3;
    sc0.by_item_id[ITEM_RescuedZelda] = 1;
    sr = Logic_ComputeReachabilityFullKnowledge(&sc0, &soff);
    LSC_ASSERT(Reachability_HasLocation(sr, LOC_Mini_Moldorm_Cave_Far_Left),
               "souls=off: the soul term must be inert (pre-souls reachability)");
  }

  // Key Rings only replace small keys; they must not waive the generated static
  // soul requirement for Hyrule Castle's room-0x071 guard. Both generation and
  // the live F12 tracker consume this same reachability result, so this prevents
  // a tracker-only/dynamic-kill-state workaround. Run both Open/base and
  // Inverted/override predicates: the soul-room wrapper must cover both.
  if (kRandoSoulRoomsBaked) {
    RandoSettings shce;
    Settings_SetDefaults(&shce);
    shce.souls_shuffle = kSoulsShuffle_BossesEnemies;
    shce.dungeon_small_keys_mode = kDungeonItemMode_Dungeon;

    RandoCounts hc;
    memset(&hc, 0, sizeof(hc));
    for (int item = 0; item < ITEM__COUNT; item++)
      hc.by_item_id[item] = 4;
    hc.by_item_id[ITEM_SmallKey_HyruleCastleEscape] = 0;
    hc.by_item_id[ITEM_KeyRing_HyruleCastleEscape] = 1;
    hc.by_item_id[ITEM_Soul_Soldier] = 0;

    const RandoReachability *hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(hr != NULL, "HCE soul/key-ring reachability returned NULL");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "HCE key ring must not waive Zelda's Cell Soldier soul gate");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Zelda),
               "HCE key ring must not waive Zelda rescue's Soldier soul gate");

    hc.by_item_id[ITEM_Soul_Soldier] = 1;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Soldier soul should open Zelda's Cell with the HCE key ring");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Zelda),
               "Soldier soul should open Zelda rescue with the HCE key ring");

    shce.enemy_drop_checks = kEnemyDropChecks_Keys;
    hc.by_item_id[ITEM_Soul_Soldier] = 0;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "enemy keys: HCE ring must not waive Zelda's Cell Soldier soul gate");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Zelda),
               "enemy keys: HCE ring must not waive Zelda rescue's Soldier soul gate");
    hc.by_item_id[ITEM_Soul_Soldier] = 1;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "enemy keys: Soldier soul should open Zelda's Cell");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Zelda),
               "enemy keys: Soldier soul should open Zelda rescue");

    shce.enemy_drop_checks = kEnemyDropChecks_Off;
    hc.by_item_id[ITEM_Soul_Soldier] = 0;
    shce.world_state = kWorldState_Inverted;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Inverted HCE override must retain Zelda's Cell Soldier soul gate");
    LSC_ASSERT(!Reachability_HasLocation(hr, LOC_Zelda),
               "Inverted HCE override must retain Zelda rescue's Soldier soul gate");
    hc.by_item_id[ITEM_Soul_Soldier] = 1;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "Inverted: Soldier soul should open Zelda's Cell");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Zelda),
               "Inverted: Soldier soul should open Zelda rescue");

    RandoSettings shce_off;
    Settings_SetDefaults(&shce_off);
    hc.by_item_id[ITEM_Soul_Soldier] = 0;
    hr = Logic_ComputeReachabilityFullKnowledge(&hc, &shce_off);
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Hyrule_Castle_Zelda_s_Cell),
               "souls=off: Zelda's Cell soul term must be inert");
    LSC_ASSERT(Reachability_HasLocation(hr, LOC_Zelda),
               "souls=off: Zelda rescue soul term must be inert");
  }

  // add-npc-souls end-to-end gate probe: with npc_souls on and a full
  // inventory (minus NPC souls), Stumpy is unreachable until his soul is
  // granted, the Kiki edge holds Palace of Darkness shut until Kiki's, and
  // the Maze Race needs BOTH race souls. With npc_souls off all are
  // reachable soul-less (terms collapse — the digest-inert contract).
  {
    RandoSettings snpc;
    Settings_SetDefaults(&snpc);
    snpc.npc_souls = 1;
    RandoCounts nc;
    memset(&nc, 0, sizeof(nc));
    for (int i = 0; i < ITEM_Soul_Npc_Sahasrahla; i++) nc.by_item_id[i] = 4;  // full non-NPC pool
    const RandoReachability *nr = Logic_ComputeReachabilityFullKnowledge(&nc, &snpc);
    LSC_ASSERT(!Reachability_HasLocation(nr, LOC_Stumpy),
               "npc on: Stumpy must be gated without his soul");
    LSC_ASSERT(!Reachability_HasLocation(nr, LOC_Palace_of_Darkness_Shooter_Room),
               "npc on: the Kiki edge must hold PoD shut without his soul");
    LSC_ASSERT(!Reachability_HasLocation(nr, LOC_Maze_Race),
               "npc on: the Maze Race needs both race souls");
    nc.by_item_id[ITEM_Soul_Npc_Stumpy] = 1;
    nc.by_item_id[ITEM_Soul_Npc_Kiki] = 1;
    nc.by_item_id[ITEM_Soul_Npc_MazeGameLady] = 1;
    nr = Logic_ComputeReachabilityFullKnowledge(&nc, &snpc);
    LSC_ASSERT(Reachability_HasLocation(nr, LOC_Stumpy),
               "npc on: the Stumpy soul should open his check");
    LSC_ASSERT(Reachability_HasLocation(nr, LOC_Palace_of_Darkness_Shooter_Room),
               "npc on: the Kiki soul should open PoD");
    LSC_ASSERT(!Reachability_HasLocation(nr, LOC_Maze_Race),
               "npc on: ONE race soul must not open the Maze Race (needs both)");
    nc.by_item_id[ITEM_Soul_Npc_MazeGameGuy] = 1;
    nr = Logic_ComputeReachabilityFullKnowledge(&nc, &snpc);
    LSC_ASSERT(Reachability_HasLocation(nr, LOC_Maze_Race),
               "npc on: both race souls should open the Maze Race");
    RandoSettings snoff;
    Settings_SetDefaults(&snoff);
    RandoCounts nc0;
    memset(&nc0, 0, sizeof(nc0));
    for (int i = 0; i < ITEM_Soul_Npc_Sahasrahla; i++) nc0.by_item_id[i] = 4;
    nr = Logic_ComputeReachabilityFullKnowledge(&nc0, &snoff);
    LSC_ASSERT(Reachability_HasLocation(nr, LOC_Stumpy) &&
               Reachability_HasLocation(nr, LOC_Palace_of_Darkness_Shooter_Room) &&
               Reachability_HasLocation(nr, LOC_Maze_Race),
               "npc off: every NPC gate must be inert (pre-npc reachability)");
  }

  // skip_pred structural validation (short-circuit support). skip_pred must
  // consume EXACTLY the bytes eval() would for every op, or short-circuiting
  // desyncs the cursor inside composite predicates. Walk every predicate blob
  // in the generated tables and assert exact-length, error-free consumption.
  // This catches a new op wired into eval() but missing from skip_pred's
  // layout table the moment the codegen first emits it into any table.
  {
    struct { uint32 off; uint16 len; } blob;
    uint32 walked = 0;
    uint8 key_threshold_max[kRandoDungeon_Count] = {0};
    #define LSC_SKIP_WALK(what) do { \
      if (blob.len != 0) { \
        Cursor sk = { kRandoPredicateStream + blob.off, \
                      kRandoPredicateStream + blob.off + blob.len, false }; \
        skip_pred(&sk); \
        LSC_ASSERT(!sk.error && sk.p == sk.end, \
                   "skip_pred layout mismatch in " what); \
        Cursor ka = { kRandoPredicateStream + blob.off, \
                      kRandoPredicateStream + blob.off + blob.len, false }; \
        audit_key_thresholds(&ka, key_threshold_max); \
        LSC_ASSERT(!ka.error && ka.p == ka.end, \
                   "key-threshold audit layout mismatch in " what); \
        walked++; \
      } \
    } while (0)
    #define LSC_REACH_WALK(what) do { \
      LSC_SKIP_WALK(what); \
      LSC_ASSERT(predicate_blob_reachability_monotone(blob.off, blob.len), \
                 "non-monotone reachability predicate in " what); \
    } while (0)
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      const RandoLocationDef *L = &kRandoLocations[i];
      blob.off = L->can_reach_offset;    blob.len = L->can_reach_length;    LSC_REACH_WALK("location can_reach");
      blob.off = L->can_place_offset;    blob.len = L->can_place_length;    LSC_SKIP_WALK("location can_place");
      blob.off = L->always_allow_offset; blob.len = L->always_allow_length; LSC_SKIP_WALK("location always_allow");
    }
    for (uint32 i = 0; i < kRandoEdgesCount; i++) {
      blob.off = kRandoEdges[i].predicate_offset;
      blob.len = kRandoEdges[i].predicate_length;
      LSC_REACH_WALK("edge predicate");
    }
    for (uint32 i = 0; i < kRandoEdges_InvertedCount; i++) {
      blob.off = kRandoEdges_Inverted[i].predicate_offset;
      blob.len = kRandoEdges_Inverted[i].predicate_length;
      LSC_REACH_WALK("inverted edge predicate");
    }
    for (uint32 i = 0; i < kRandoLocationPredOverrides_InvertedCount; i++) {
      const RandoLocationPredOverride *O = &kRandoLocationPredOverrides_Inverted[i];
      blob.off = O->can_reach_offset;    blob.len = O->can_reach_length;    LSC_REACH_WALK("inverted override can_reach");
      blob.off = O->can_place_offset;    blob.len = O->can_place_length;    LSC_SKIP_WALK("inverted override can_place");
      blob.off = O->always_allow_offset; blob.len = O->always_allow_length; LSC_SKIP_WALK("inverted override always_allow");
    }
    for (uint32 i = 0; i < kRandoLocationPredOverrides_RetroCount; i++) {
      const RandoLocationPredOverride *O = &kRandoLocationPredOverrides_Retro[i];
      blob.off = O->can_reach_offset;    blob.len = O->can_reach_length;    LSC_REACH_WALK("retro override can_reach");
      blob.off = O->can_place_offset;    blob.len = O->can_place_length;    LSC_SKIP_WALK("retro override can_place");
      blob.off = O->always_allow_offset; blob.len = O->always_allow_length; LSC_SKIP_WALK("retro override always_allow");
    }
    for (uint32 i = 0; i < kRandoBossKillPredCount; i++) {
      blob.off = kRandoBossKillPred[i].offset;
      blob.len = kRandoBossKillPred[i].length;
      LSC_REACH_WALK("boss kill predicate");
    }
    for (uint32 i = 0; i < kRandoCaveSourcePredsCount; i++) {
      blob.off = kRandoCaveSourcePreds[i].off;
      blob.len = kRandoCaveSourcePreds[i].len;
      LSC_REACH_WALK("cave-source predicate");
    }
    for (uint32 i = 0; i < kDoorVmPredsCount; i++) {
      blob.off = kDoorVmPreds[i].off;
      blob.len = kDoorVmPreds[i].len;
      LSC_REACH_WALK("door vm predicate");
    }
    for (uint32 i = 0; i < kRandoDoorPotLocationsCount; i++) {
      blob.off = kRandoDoorPotLocations[i].pred_off;
      blob.len = kRandoDoorPotLocations[i].pred_len;
      LSC_REACH_WALK("door pot predicate");
    }
    for (uint32 i = 0; i < kRandoDoorEnemyDropLocationsCount; i++) {
      blob.off = kRandoDoorEnemyDropLocations[i].pred_off;
      blob.len = kRandoDoorEnemyDropLocations[i].pred_len;
      LSC_REACH_WALK("door enemy-drop predicate");
    }
    for (uint32 i = 0; i < kRandoDoorEnemyCheckLocationsCount; i++) {
      blob.off = kRandoDoorEnemyCheckLocations[i].pred_off;
      blob.len = kRandoDoorEnemyCheckLocations[i].pred_len;
      LSC_REACH_WALK("door enemy-check predicate");
    }
    for (uint32 i = 0; i < kDoorPortalGatesCount; i++) {
      blob.off = kDoorPortalGates[i].pred_off;
      blob.len = kDoorPortalGates[i].pred_len;
      LSC_REACH_WALK("door portal-gate predicate");
    }
    #undef LSC_REACH_WALK
    #undef LSC_SKIP_WALK
    LSC_ASSERT(walked > 1000, "skip_pred walk covered suspiciously few blobs");
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      LSC_ASSERT(key_threshold_max[d] <= Rando_KeyRingGrantCount(d),
                 "authored small-key threshold exceeds Key Ring grant stock");
    }
  }

  fprintf(stderr, "[Logic_SelfCheck] OK\n");
}

// ---------------------------------------------------------------------------
// tracker-player-knowledge — structural probes for the knowledge-limited
// flood (openspec randomizer-player-knowledge / "Reachability compute accepts
// a knowledge-exclusion input"). Runs on the base graph (no shuffle installs
// needed): masked ⊆ full, hidden-dungeon exclusion, suppressed-location
// exclusion, empty-mask byte-identity, and full-flood purity after a masked
// flood (the placer-contamination guard).
// ---------------------------------------------------------------------------
void Logic_KnowledgeMaskSelfCheck(void) {
#define LKM_ASSERT(cond, msg)                                    \
  do {                                                           \
    if (!(cond)) {                                               \
      fprintf(stderr, "Logic_KnowledgeMaskSelfCheck: %s\n", msg);\
      exit(2);                                                   \
    }                                                            \
  } while (0)
  RandoSettings s;
  Settings_SetDefaults(&s);
  RandoCounts c;
  memset(&c, 0, sizeof c);
  // A kit broad enough to reach into Eastern Palace (and well beyond) on the
  // Open base graph, so the hidden-dungeon assertions are not vacuous.
  c.by_item_id[ITEM_ProgressiveSword] = 2;
  c.by_item_id[ITEM_Lamp] = 1;
  c.by_item_id[ITEM_Bow] = 1;
  c.by_item_id[ITEM_PowerGlove] = 2;
  c.by_item_id[ITEM_Hammer] = 1;
  c.by_item_id[ITEM_Hookshot] = 1;
  c.by_item_id[ITEM_Flippers] = 1;
  c.by_item_id[ITEM_MoonPearl] = 1;

  struct RandoReachability full1 = *Logic_ComputeReachabilityFullKnowledge(&c, &s);

  // Hidden-dungeon mask: Eastern Palace excluded from the fixpoint.
  RandoKnowledgeMask m;
  memset(&m, 0, sizeof m);
  m.hidden_dungeon_mask = (uint16)(1u << kRandoDungeon_EasternPalace);
  struct RandoReachability masked = *Logic_ComputeReachabilityMasked(&c, &s, &m);

  // (1) masked ⊆ full, both bitsets.
  for (size_t i = 0; i < sizeof(masked.region_bitset); i++)
    LKM_ASSERT((masked.region_bitset[i] & ~full1.region_bitset[i]) == 0,
               "masked region set must be a subset of the full set");
  for (size_t i = 0; i < sizeof(masked.location_bitset); i++)
    LKM_ASSERT((masked.location_bitset[i] & ~full1.location_bitset[i]) == 0,
               "masked location set must be a subset of the full set");

  // (2) every EP location is absent from the masked view — and at least one
  // was reachable in the full view (non-vacuous fixture).
  {
    int ep_full = 0;
    for (uint32 i = 0; i < kRandoLocationsCount; i++) {
      uint16 region = kRandoLocations[i].region_id;
      uint8 d = 0xFF;
      for (uint32 r = 0; r < kRandoRegionsCount; r++) {
        if (kRandoRegions[r].id == region) { d = kRandoRegions[r].dungeon_id; break; }
      }
      if (d != kRandoDungeon_EasternPalace) continue;
      if (Reachability_HasLocation(&full1, kRandoLocations[i].id)) ep_full++;
      LKM_ASSERT(!Reachability_HasLocation(&masked, kRandoLocations[i].id),
                 "hidden-dungeon location leaked into the masked view");
    }
    LKM_ASSERT(ep_full > 0, "fixture must reach Eastern Palace in the full view");
  }

  // (3) suppressed-location mask: a location reachable in full is absent when
  // its bit is set, with no other location gained.
  {
    uint16 probe = 0xFFFF;
    for (uint32 i = 0; i < kRandoLocationsCount && probe == 0xFFFF; i++) {
      if (Reachability_HasLocation(&full1, kRandoLocations[i].id))
        probe = kRandoLocations[i].id;
    }
    LKM_ASSERT(probe != 0xFFFF, "fixture reached no location at all");
    static uint8 supp[(kRandoLocationCapacity + 7) >> 3];
    memset(supp, 0, sizeof supp);
    supp[probe >> 3] |= (uint8)(1u << (probe & 7));
    RandoKnowledgeMask ms;
    memset(&ms, 0, sizeof ms);
    ms.suppressed_locations = supp;
    struct RandoReachability msup = *Logic_ComputeReachabilityMasked(&c, &s, &ms);
    LKM_ASSERT(!Reachability_HasLocation(&msup, probe),
               "suppressed location leaked into the masked view");
    for (size_t i = 0; i < sizeof(msup.location_bitset); i++)
      LKM_ASSERT((msup.location_bitset[i] & ~full1.location_bitset[i]) == 0,
                 "suppression must not add reachability");
  }

  // (4) empty mask (all-zero struct) is byte-identical to the full flood.
  {
    RandoKnowledgeMask m0;
    memset(&m0, 0, sizeof m0);
    struct RandoReachability same = *Logic_ComputeReachabilityMasked(&c, &s, &m0);
    LKM_ASSERT(memcmp(&same, &full1, sizeof full1) == 0,
               "empty mask must be byte-identical to the full flood");
  }

  // (5) purity: a full flood AFTER masked floods is byte-identical to the
  // first — the mask leaves no residue a generation call could observe.
  {
    struct RandoReachability full2 = *Logic_ComputeReachabilityFullKnowledge(&c, &s);
    LKM_ASSERT(memcmp(&full2, &full1, sizeof full1) == 0,
               "full flood after a masked flood must be unaffected");
  }
#undef LKM_ASSERT
  fprintf(stderr, "[Logic_KnowledgeMaskSelfCheck] OK\n");
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_logic)
