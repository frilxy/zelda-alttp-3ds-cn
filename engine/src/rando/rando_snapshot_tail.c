#include <SDL_rwops.h>
// rando_snapshot_tail.c — TLV-chain encoder/decoder for snapshot tail
// (tasks.md §8.8, §8.8a; randomizer-save spec § "Snapshot interoperability";
// design.md §D11).
//
// On-disk byte layout (per spec, all multi-byte LE):
//
//   magic[8]                 = "ZRSNAP01"  raw ASCII, no NUL
//   type[4]   LE             = 1 (TAIL_RANDO_STATE)
//   length[4] LE             = payload size in bytes
//   payload:
//     generator_version[2]   LE
//     settings_hash[16]
//     share_string[32]
//     placement_table_size[2] LE       (bytes; = 2 × #locations)
//     placement_table[placement_table_size]  flat uint16 LE by location_id
//                              (0xFFFF = no placement sentinel)
//
// M4 can append a RandoSettings TLV after the RandoState one, so a COLD replay
// (Ctrl+F1 on a fresh launch with the slot not loaded) can rebuild the per-slot
// process state that g_ram (and thus LoadSnesState) does not carry:
//
//   type[4]   LE             = 2 (TAIL_RANDO_SETTINGS)
//   length[4] LE             = payload size in bytes
//   payload:
//     format_version[1]      = 1
//     prize_attempt[1]                 (accepted assumed-fill attempt; FIX #6)
//     mushroom_held[1]                 ┐ the 4 process-static ownership bytes
//     flute_shovel_owned[1]            │ (NOT in g_ram); read LIVE at save time
//     boomerang_owned[1]               │ so they reflect the snapshot instant
//     bow_owned[1]                     ┘
//     settings_len[1]                  (= kSettingsCanonicalLen on the wire)
//     settings_canonical[settings_len] (Settings_CanonicalSerialize output)
//
// Door-shuffle snapshots append a separate layout-identity TLV:
//
//   type[4]   LE             = 5 (TAIL_DOOR_LAYOUT)
//   length[4] LE             = 5
//   payload:
//     format_version[1]      = 1
//     door_attempt[1]
//     door_digest24[3] LE
//
// Dungeon-chain snapshots append a separate layout-identity TLV. Length 5 is
// the original layout-only form; newer saves extend the same format byte with
// in-flight chain session bytes that older readers skip.
//
//   type[4]   LE             = 7 (TAIL_CHAIN_LAYOUT)
//   length[4] LE             = 5 or 9
//   payload:
//     format_version[1]      = 1
//     chains_attempt[1]
//     chains_digest24[3] LE
//     session_flags[1]       bit0=origin_active, bit1=terminal_active (optional)
//     origin_exit_room[2] LE  optional
//     terminal_dungeon[1]     optional
//
// Entrance-shuffle snapshots append a layout-identity TLV (DoorLayout's
// pattern; the permutation regenerates from seed/axes/attempt/world_state and
// must match the digest before install — a cold replay without this TLV
// fails closed rather than playing with vanilla entrances):
//
//   type[4]   LE             = 9 (TAIL_ENTRANCE_LAYOUT)
//   length[4] LE             = 6
//   payload:
//     format_version[1]      = 1
//     entrance_axes[1]         (packed kEntranceAxis_* byte, slot header @71)
//     entrance_attempt[1]      (accepted goal-retry attempt, slot header @72)
//     entrance_digest24[3] LE  (Rando_EntranceLayoutDigest24; 0 = legacy)
//
// Another optional TLV carries per-slot Seed QoL features for snapshot replay:
//
//   type[4]   LE             = 4 (TAIL_RECOMMENDED_FEATURES)
//   length[4] LE             = 5
//   payload:
//     format_version[1]      = 1
//     recommended_features0[4] LE      (masked to kFeatures0_RandoSeedQolMask)
//
// Optional records are SEPARATE TLVs (not folded into RandoState) so an older
// binary — which requires RandoState's length to be exactly 52 +
// placement_table_size — skips them as unknown types and still reads the
// placement table. A v1/no-blob slot emits no type-2 TLV (settings absent), so
// the cold replay degrades to placement-only.
//
// Settings-bearing snapshots append a registry-identity TLV before settings:
//   type=6, payload format[1] + pot digest[4] LE + pot count[2] LE
//   (v2 appends terrain digest[4] + count[2] = 13 bytes; v3 appends
//   enemy-check digest[4] + count[2] = 19 bytes).
// Cold replay validates the identities a snapshot's settings make
// load-bearing before applying settings-derived state.
//
// Hints v2 snapshots append a certified plan identity/discovery TLV:
//   type=11, payload format[1] + algorithm[2] LE + text schema[2] LE +
//   plan digest[32] + discovery[4] LE = exactly 41 bytes. Bits 24..31 of
//   discovery are reserved and ignored.
//
// Multi-byte fields are emitted/consumed via explicit LE byte helpers (no
// host-endianness reliance) — per the same discipline as rando_save.c.

#include "rando_snapshot_tail.h"
#include "rando_hints.h"    // Hints v2 plan identity/discovery import/export
#include "item_ids.h"       // current-v14 hint replay self-check fixture
#include "location_ids.h"   // current-v14 hint replay self-check fixture
#include "rando_placement.h"
#include "rando.h"          // snapshot cold-replay helpers + ownership externs
#include "souls.h"          // add-enemy-souls (soul ownership TLV)
#include "rando_save.h"     // RandoSidecarSlot (self-check synthetic activation)
#include "rando_settings.h" // kSettingsCanonicalLen, Settings_CanonicalDeserialize
#include "rando_logic.h"    // kRandoOwGraphPresent (composed cold-replay probe gate)
#include "shuffle_ow_warp.h"  // OwWarp_Compute/Digest24 (composed probe identity)
#include "rando_share.h"    // Share_PackBinary (self-check valid raw share string)
#include "shuffle_doors.h"  // DoorShuffle_Generate/Digest (self-check)
#include "shuffle_chains.h" // Chains_Compute/Digest (self-check)
#include "door_runtime.h"   // DoorRt_Installed (self-check)
#include "chains_runtime.h" // Chains_RuntimeRecordDoorEntry (self-check)
#include "chain_boss_entrances.gen.h" // kChainBossEntranceChecks (self-check)
#include "../features.h"    // enhanced_features1 / kFeatures1_DoorShuffleActive
#include "../assets.h"      // kOverworld_Entrance_Id (chain self-check)
#include "../config.h"      // g_config (self-checks feature restore)
#include "../variables.h"   // g_ram (the features macro)
#include "../assets.h"      // g_asset_ptrs/g_asset_sizes (self-check synth entrance ids)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// LE byte helpers — local copies to keep this module self-contained
// (rando_save.c keeps its own as `static`, so we can't share without
// linkage churn).
// ---------------------------------------------------------------------------
static void put_u16le_bytes(uint8 out[2], uint16 v) {
  out[0] = (uint8)v;
  out[1] = (uint8)(v >> 8);
}
static void put_u32le_bytes(uint8 out[4], uint32 v) {
  out[0] = (uint8)v;
  out[1] = (uint8)(v >> 8);
  out[2] = (uint8)(v >> 16);
  out[3] = (uint8)(v >> 24);
}
static uint16 get_u16le_bytes(const uint8 in[2]) {
  return (uint16)in[0] | ((uint16)in[1] << 8);
}
static uint32 get_u32le_bytes(const uint8 in[4]) {
  return (uint32)in[0] | ((uint32)in[1] << 8)
       | ((uint32)in[2] << 16) | ((uint32)in[3] << 24);
}

_Static_assert(kRandoSnapshotTail_HintStatePayloadSize ==
                   1u + 2u + 2u + kRandoHintPlanDigestBytes +
                       kRandoHintDiscoveryBytes,
               "Hints v2 snapshot payload size drift");

static void encode_hint_state_payload(
    const RandoHintPersistedState *state,
    uint8 out[kRandoSnapshotTail_HintStatePayloadSize]) {
  memset(out, 0, kRandoSnapshotTail_HintStatePayloadSize);
  out[0] = kRandoSnapshotTail_HintStateFormat;
  put_u16le_bytes(out + 1, state->algorithm_version);
  put_u16le_bytes(out + 3, state->text_schema_version);
  memcpy(out + 5, state->plan_digest, kRandoHintPlanDigestBytes);
  // Discovery is a little-endian bitset. Current facts occupy bits 0..23;
  // byte 3 is reserved and must stay zero on every write.
  memcpy(out + 37, state->discovered, 3);
  out[40] = 0;
}

static bool decode_hint_state_payload(
    const uint8 payload[kRandoSnapshotTail_HintStatePayloadSize],
    RandoHintPersistedState *out) {
  if (payload == NULL || out == NULL ||
      payload[0] != kRandoSnapshotTail_HintStateFormat)
    return false;
  memset(out, 0, sizeof(*out));
  out->algorithm_version = get_u16le_bytes(payload + 1);
  out->text_schema_version = get_u16le_bytes(payload + 3);
  memcpy(out->plan_digest, payload + 5, kRandoHintPlanDigestBytes);
  memcpy(out->discovered, payload + 37, 3);
  // Mask reserved bits 24..31 from untrusted snapshots.
  out->discovered[3] = 0;
  return true;
}

typedef struct RandoSnapshotPendingHintState {
  bool seen;
  bool valid;
  bool invalid;
  RandoHintPersistedState state;
} RandoSnapshotPendingHintState;

typedef struct RandoSnapshotHintAssignmentView {
  uint8 fact_count;
  uint8 assigned[kRandoHintPlanMaxFacts];
  uint8 npc[kRandoHintPlanMaxFacts];
  uint8 queue_index[kRandoHintPlanMaxFacts];
} RandoSnapshotHintAssignmentView;

static bool hint_persisted_state_equal(const RandoHintPersistedState *a,
                                       const RandoHintPersistedState *b) {
  return a->algorithm_version == b->algorithm_version &&
         a->text_schema_version == b->text_schema_version &&
         memcmp(a->plan_digest, b->plan_digest,
                kRandoHintPlanDigestBytes) == 0 &&
         memcmp(a->discovered, b->discovered,
                kRandoHintDiscoveryBytes) == 0;
}

static bool capture_hint_assignment_view(
    RandoSnapshotHintAssignmentView *out) {
  if (out == NULL || Rando_GetHintPlanStatus() != kRandoHintPlanStatus_Ready)
    return false;
  memset(out, 0, sizeof(*out));
  out->fact_count = Rando_GetHintFactCount();
  if (out->fact_count > kRandoHintPlanMaxFacts) return false;
  for (uint8 fact_id = 0; fact_id < out->fact_count; fact_id++) {
    RandoHintNpc npc = kRandoHintNpc_None;
    uint8 queue_index = 0;
    if (!Rando_GetHintFactAssignment(fact_id, &npc, &queue_index)) continue;
    out->assigned[fact_id] = 1;
    out->npc[fact_id] = (uint8)npc;
    out->queue_index[fact_id] = queue_index;
  }
  return true;
}

static void pending_hint_state_reset(RandoSnapshotPendingHintState *pending) {
  memset(pending, 0, sizeof(*pending));
}

static void pending_hint_state_consume(
    RandoSnapshotPendingHintState *pending, bool accepted_rando_state,
    const uint8 payload[kRandoSnapshotTail_HintStatePayloadSize],
    uint32 length) {
  // Orphaned type-11 records never become candidates for a later type-1.
  if (!accepted_rando_state) return;
  // Exactly one record is allowed in an accepted identity scope. Once invalid,
  // no later duplicate can repair or replace it.
  if (pending->seen) {
    pending->valid = false;
    pending->invalid = true;
    return;
  }
  pending->seen = true;
  if (length != kRandoSnapshotTail_HintStatePayloadSize ||
      !decode_hint_state_payload(payload, &pending->state)) {
    pending->valid = false;
    pending->invalid = true;
    memset(&pending->state, 0, sizeof(pending->state));
    return;
  }
  pending->valid = true;
}

// ---------------------------------------------------------------------------
// Snapshot context — the metadata the TLV payload needs. Module-static.
// `g_has_ctx` discriminates "context installed" from "all-zeros happens to
// match" (since settings_hash could be all-zero in principle).
// ---------------------------------------------------------------------------
static struct {
  uint16 generator_version;
  uint8 settings_hash[16];
  uint8 share_string[32];
} g_ctx;
static bool g_has_ctx = false;

// settings sub-context for the type-2 RandoSettings TLV. Independent of the
// type-1 context above: a v1/no-blob slot sets the type-1 context but clears this
// one, so the type-2 TLV is suppressed.
static uint8 g_ctx_settings_canonical[kSettingsCanonicalLen];
static uint8 g_ctx_prize_attempt = 0;
static uint8 g_ctx_ow_attempt = 0;      // add-rando-ow-warp-shuffle
static uint32 g_ctx_ow_digest24 = 0;    // (cleared beside prize_attempt in
                                        // Rando_ClearSnapshotOptionalContexts)
static bool g_has_settings_ctx = false;
static uint8 g_ctx_door_attempt = 0;
static uint32 g_ctx_door_digest24 = 0;
static bool g_has_door_ctx = false;
static uint8 g_ctx_chains_attempt = 0;
static uint32 g_ctx_chains_digest24 = 0;
static bool g_has_chains_ctx = false;
static uint8 g_ctx_entrance_axes = 0;
static uint8 g_ctx_entrance_attempt = 0;
static uint32 g_ctx_entrance_digest24 = 0;
static bool g_has_entrance_ctx = false;
static uint32 g_ctx_recommended_features0 = 0;
static bool g_has_recommended_features_ctx = false;

static void Rando_ClearSnapshotOptionalContexts(void) {
  g_has_settings_ctx = false;
  memset(g_ctx_settings_canonical, 0, sizeof(g_ctx_settings_canonical));
  g_ctx_prize_attempt = 0;
  g_ctx_ow_attempt = 0;      // add-rando-ow-warp-shuffle (audit-2 LOW-3)
  g_ctx_ow_digest24 = 0;
  g_ctx_door_attempt = 0;
  g_ctx_door_digest24 = 0;
  g_has_door_ctx = false;
  g_ctx_chains_attempt = 0;
  g_ctx_chains_digest24 = 0;
  g_has_chains_ctx = false;
  g_ctx_entrance_axes = 0;
  g_ctx_entrance_attempt = 0;
  g_ctx_entrance_digest24 = 0;
  g_has_entrance_ctx = false;
  g_ctx_recommended_features0 = 0;
  g_has_recommended_features_ctx = false;
}

void Rando_SetSnapshotContext(uint16 generator_version,
                              const uint8 settings_hash[16],
                              const uint8 share_string[32]) {
  if (settings_hash == NULL || share_string == NULL) {
    g_has_ctx = false;
    memset(&g_ctx, 0, sizeof(g_ctx));
    return;
  }
  g_ctx.generator_version = generator_version;
  memcpy(g_ctx.settings_hash, settings_hash, 16);
  memcpy(g_ctx.share_string, share_string, 32);
  g_has_ctx = true;
}

void Rando_SetSnapshotSettingsContext(const uint8 *settings_canonical_or_null,
                                      uint8 prize_attempt, uint8 ow_attempt,
                                      uint32 ow_digest24) {
  if (settings_canonical_or_null == NULL) {
    g_has_settings_ctx = false;
    memset(g_ctx_settings_canonical, 0, sizeof(g_ctx_settings_canonical));
    g_ctx_prize_attempt = 0;
    g_ctx_ow_attempt = 0;
    g_ctx_ow_digest24 = 0;
    return;
  }
  memcpy(g_ctx_settings_canonical, settings_canonical_or_null, kSettingsCanonicalLen);
  g_ctx_prize_attempt = prize_attempt;
  // add-rando-ow-warp-shuffle — the warp layout identity rides the type-2
  // payload (format_version 2 appended tail) so a cold replay can
  // regenerate + digest-gate the layout exactly like slot activation.
  g_ctx_ow_attempt = ow_attempt;
  g_ctx_ow_digest24 = ow_digest24 & 0xFFFFFFu;
  g_has_settings_ctx = true;
}

void Rando_SetSnapshotDoorContext(uint8 door_attempt, uint32 door_digest24,
                                  bool present) {
  if (!present) {
    g_ctx_door_attempt = 0;
    g_ctx_door_digest24 = 0;
    g_has_door_ctx = false;
    return;
  }
  g_ctx_door_attempt = door_attempt;
  g_ctx_door_digest24 = door_digest24 & 0xFFFFFFu;
  g_has_door_ctx = true;
}

void Rando_SetSnapshotChainsContext(uint8 chains_attempt, uint32 chains_digest24,
                                    bool present) {
  if (!present) {
    g_ctx_chains_attempt = 0;
    g_ctx_chains_digest24 = 0;
    g_has_chains_ctx = false;
    return;
  }
  g_ctx_chains_attempt = chains_attempt;
  g_ctx_chains_digest24 = chains_digest24 & 0xFFFFFFu;
  g_has_chains_ctx = true;
}

void Rando_SetSnapshotEntranceContext(uint8 entrance_axes,
                                      uint8 entrance_attempt,
                                      uint32 entrance_digest24, bool present) {
  if (!present) {
    g_ctx_entrance_axes = 0;
    g_ctx_entrance_attempt = 0;
    g_ctx_entrance_digest24 = 0;
    g_has_entrance_ctx = false;
    return;
  }
  g_ctx_entrance_axes = entrance_axes;
  g_ctx_entrance_attempt = entrance_attempt;
  g_ctx_entrance_digest24 = entrance_digest24 & 0xFFFFFFu;
  g_has_entrance_ctx = true;
}

void Rando_SetSnapshotRecommendedFeaturesContext(uint32 features0, bool present) {
  if (!present) {
    g_ctx_recommended_features0 = 0;
    g_has_recommended_features_ctx = false;
    return;
  }
  g_ctx_recommended_features0 = features0 & kFeatures0_RandoSeedQolMask;
  g_has_recommended_features_ctx = true;
}

void Rando_ClearSnapshotContext(void) {
  g_has_ctx = false;
  memset(&g_ctx, 0, sizeof(g_ctx));
  Rando_ClearSnapshotOptionalContexts();
}

bool Rando_HasSnapshotContext(void) {
  return g_has_ctx;
}

uint16 Rando_GetSnapshotGeneratorVersion(void) {
  return g_ctx.generator_version;
}

const uint8 *Rando_GetSnapshotSettingsHash(void) {
  return g_ctx.settings_hash;
}

const uint8 *Rando_GetSnapshotShareString(void) {
  return g_ctx.share_string;
}

static bool Rando_SnapshotSettingsAllowedForReplay(const RandoSettings *s) {
  char placement_preflight_err[192];
  if (!Placement_PreflightSettings(
          s, placement_preflight_err, sizeof(placement_preflight_err))) {
    fprintf(stderr,
            "RandoSnapshotTail: snapshot settings are not supported by this "
            "build (%s) - deactivating randomizer state\n",
            placement_preflight_err[0] != '\0' ? placement_preflight_err
                                                : "preflight failed");
    return false;
  }
  if (Settings_EnemyDropKeysActive(s) &&
      g_has_ctx &&
      g_ctx.generator_version != (uint16)kGeneratorVersion) {
    fprintf(stderr,
            "RandoSnapshotTail: enemy-drop-check snapshot was generated by "
            "version %u but this build is version %u; enemy check location ids "
            "are table-derived, so this snapshot must be regenerated before "
            "loading here\n",
            (unsigned)g_ctx.generator_version,
            (unsigned)kGeneratorVersion);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Ordering-invariant tripwire (tasks.md §8.8a).
// ---------------------------------------------------------------------------
uint64 g_rando_oncheck_call_count = 0;

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
// The engine uses SDL_RWops for SD/profile files. Keep FILE wrappers for the
// upstream standalone checks, with one shared TLV encoder/decoder.
typedef struct SnapshotStream {
  void *context;
  size_t (*read)(void *, size_t, size_t, void *);
  size_t (*write)(const void *, size_t, size_t, void *);
  int (*seek)(void *, long, int);
} SnapshotStream;
static size_t stream_read(void *p, size_t s, size_t n, SnapshotStream *f) {
  return f->read(p, s, n, f->context);
}
static size_t stream_write(const void *p, size_t s, size_t n, SnapshotStream *f) {
  return f->write(p, s, n, f->context);
}
static int stream_seek(SnapshotStream *f, long offset, int origin) {
  return f->seek(f->context, offset, origin);
}
static bool snapshot_save(SnapshotStream *f) {
  if (f == NULL) return false;

  // No placement installed → nothing to emit. Older readers see a vanilla
  // tail; newer readers reading this file will see EOF at the TLV loop and
  // terminate cleanly. This is intentional graceful degradation.
  const RandoPlacementTable *t = Placement_GetActive();
  if (t == NULL || t->count == 0) return true;

  // No context → cannot emit the payload (we'd be lying about gen_version /
  // settings_hash / share_string). Suppress emission; the tail-load
  // graceful-degrade path takes over on the read side.
  if (!g_has_ctx) return true;

  // Translate sparse placements[] back into a flat uint16-by-location_id
  // array. Same convention as rando_save.c: max(location_id)+1 entries,
  // sentinel-fill any unset entry. Cap at kRandoLocationCapacity to match
  // RandoSidecarSlot's `placements[kRandoLocationCapacity]` size.
  uint16 max_loc = 0;
  for (uint16 i = 0; i < t->count; i++) {
    if (t->entries[i].location_id >= max_loc) {
      max_loc = (uint16)(t->entries[i].location_id + 1);
    }
  }
  if (max_loc > kRandoLocationCapacity) return false;  // refuse to emit truncated payload
  uint16 location_count = max_loc;
  uint32 placement_table_bytes = (uint32)location_count * 2u;

  // Build payload in a heap buffer so we can compute the total length up
  // front (TLV's length field must be exact).
  uint32 payload_len = 2u + 16u + 32u + 2u + placement_table_bytes;
  uint8 *payload = (uint8 *)malloc(payload_len);
  if (payload == NULL) return false;
  uint8 *p = payload;
  put_u16le_bytes(p, g_ctx.generator_version); p += 2;
  memcpy(p, g_ctx.settings_hash, 16); p += 16;
  memcpy(p, g_ctx.share_string, 32); p += 32;
  put_u16le_bytes(p, (uint16)placement_table_bytes); p += 2;
  // Sentinel-fill, then scatter sparse entries.
  for (uint16 i = 0; i < location_count; i++) {
    p[i * 2 + 0] = 0xFF;
    p[i * 2 + 1] = 0xFF;
  }
  for (uint16 i = 0; i < t->count; i++) {
    uint16 loc = t->entries[i].location_id;
    if (loc < location_count) {
      p[loc * 2 + 0] = (uint8)(t->entries[i].item_id & 0xff);
      p[loc * 2 + 1] = (uint8)(t->entries[i].item_id >> 8);
    }
  }

  // TLV header (magic + type + length).
  uint8 hdr[16];
  memcpy(hdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
  put_u32le_bytes(hdr + 8, kRandoSnapshotTail_Type_RandoState);
  put_u32le_bytes(hdr + 12, payload_len);

  size_t w1 = stream_write(hdr, 1, sizeof(hdr), f);
  size_t w2 = (w1 == sizeof(hdr)) ? stream_write(payload, 1, payload_len, f) : 0;
  free(payload);
  if (w1 != sizeof(hdr) || w2 != payload_len) return false;

  // Checked-location bitmap TLV (type 3) — g_rando_checked_bitmap lives outside
  // the SNES g_ram dump LoadSnesState restores, so without it a snapshot loses
  // which locations are collected. A flat kRandoCheckedBitmapBytes copy.
  {
    uint8 chdr[16];
    memcpy(chdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(chdr + 8, kRandoSnapshotTail_Type_CheckedBitmap);
    put_u32le_bytes(chdr + 12, (uint32)kRandoCheckedBitmapBytes);
    if (stream_write(chdr, 1, sizeof(chdr), f) != sizeof(chdr)) return false;
    if (stream_write(g_rando_checked_bitmap, 1, kRandoCheckedBitmapBytes, f) !=
        (size_t)kRandoCheckedBitmapBytes)
      return false;
  }

  // tracker-player-knowledge — topology-discovery TLV (type 10). Emitted
  // unconditionally alongside the checked bitmap (discovery is session state
  // of the same class), read LIVE so it reflects the snapshot instant.
  // Payload: format_version[1] + dungeons[2 LE] + whirlpools[1] + caves[8]
  // + exits[8]. Length-tolerant on read (the door phase appends later).
  {
    uint8 kp[20];
    uint16 kd; uint8 kw;
    kp[0] = 1u;  // format_version
    Rando_GetDiscoveryState(&kd, &kw, kp + 4, kp + 12);
    kp[1] = (uint8)(kd & 0xff);
    kp[2] = (uint8)(kd >> 8);
    kp[3] = kw;
    uint8 khdr[16];
    memcpy(khdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(khdr + 8, kRandoSnapshotTail_Type_Discovery);
    put_u32le_bytes(khdr + 12, (uint32)sizeof(kp));
    if (stream_write(khdr, 1, sizeof(khdr), f) != sizeof(khdr)) return false;
    if (stream_write(kp, 1, sizeof(kp), f) != sizeof(kp)) return false;
  }

  if (g_has_settings_ctx) {
    // PotRegistry TLV, payload format_version 4: pot identity (v1) followed by
    // the terrain registry identity (v2, add-rando-grass-rock-shuffle), the
    // enemy-check registry identity (v3), and the bonk registry identity
    // (v4, add-rando-bonk-sanity). Extending this TLV instead of adding
    // a new type keeps the snapshot's TLV COUNT unchanged, so the round-trip
    // selfchecks' recognized-count asserts stand.
    uint8 pp[25];
    pp[0] = 4u;  // format_version 4 = pot + terrain + enemy-check + bonk
    put_u32le_bytes(pp + 1, Rando_CurrentPotRegistryDigest());
    put_u16le_bytes(pp + 5, Rando_CurrentPotRegistryCount());
    put_u32le_bytes(pp + 7, Rando_CurrentTerrainRegistryDigest());
    put_u16le_bytes(pp + 11, Rando_CurrentTerrainRegistryCount());
    put_u32le_bytes(pp + 13, Rando_CurrentEnemyCheckRegistryDigest());
    put_u16le_bytes(pp + 17, Rando_CurrentEnemyCheckRegistryCount());
    put_u32le_bytes(pp + 19, Rando_CurrentBonkRegistryDigest());
    put_u16le_bytes(pp + 23, Rando_CurrentBonkRegistryCount());
    uint8 phdr[16];
    memcpy(phdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(phdr + 8, kRandoSnapshotTail_Type_PotRegistry);
    put_u32le_bytes(phdr + 12, (uint32)sizeof(pp));
    if (stream_write(phdr, 1, sizeof(phdr), f) != sizeof(phdr)) return false;
    if (stream_write(pp, 1, sizeof(pp), f) != sizeof(pp)) return false;
  }

  // append the type-2 RandoSettings TLV when the active slot installed a
  // settings sub-context (canonical blob present). It carries world_state +
  // the prize/medallion/boss/drop/enemy derivation inputs (canonical settings +
  // prize_attempt) plus the 4 process-static ownership bytes — read LIVE here so
  // they reflect the snapshot instant, not slot-activation time. A v1/no-blob
  // slot leaves g_has_settings_ctx false → no type-2 TLV, and a cold replay then
  // degrades to placement-only (type-1).
  if (g_has_settings_ctx) {
    // format_version 2 appends the warp-layout identity AFTER the settings
    // blob (add-rando-ow-warp-shuffle) — offsets of the v1 fields are
    // unchanged, so a v1 reader parses the prefix and a v2 reader checks
    // the payload length for the tail.
    uint8 sp[7 + kSettingsCanonicalLen + 4];
    sp[0] = 2u;                           // format_version
    sp[1] = g_ctx_prize_attempt;
    sp[2] = g_rando_mushroom_held;        // 4 process-static ownership bytes, LIVE
    sp[3] = g_rando_flute_shovel_owned;
    sp[4] = g_rando_boomerang_owned;
    sp[5] = g_rando_bow_owned;
    sp[6] = (uint8)kSettingsCanonicalLen;
    memcpy(sp + 7, g_ctx_settings_canonical, kSettingsCanonicalLen);
    sp[7 + kSettingsCanonicalLen + 0] = g_ctx_ow_attempt;
    sp[7 + kSettingsCanonicalLen + 1] = (uint8)(g_ctx_ow_digest24 & 0xff);
    sp[7 + kSettingsCanonicalLen + 2] = (uint8)((g_ctx_ow_digest24 >> 8) & 0xff);
    sp[7 + kSettingsCanonicalLen + 3] = (uint8)((g_ctx_ow_digest24 >> 16) & 0xff);
    uint32 sp_len = (uint32)sizeof(sp);
    uint8 shdr[16];
    memcpy(shdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(shdr + 8, kRandoSnapshotTail_Type_RandoSettings);
    put_u32le_bytes(shdr + 12, sp_len);
    if (stream_write(shdr, 1, sizeof(shdr), f) != sizeof(shdr)) return false;
    if (stream_write(sp, 1, sp_len, f) != sp_len) return false;
  }
  if (g_has_door_ctx) {
    uint8 dp[5];
    dp[0] = 1u;  // format_version
    dp[1] = g_ctx_door_attempt;
    dp[2] = (uint8)(g_ctx_door_digest24 & 0xff);
    dp[3] = (uint8)((g_ctx_door_digest24 >> 8) & 0xff);
    dp[4] = (uint8)((g_ctx_door_digest24 >> 16) & 0xff);
    uint8 dhdr[16];
    memcpy(dhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(dhdr + 8, kRandoSnapshotTail_Type_DoorLayout);
    put_u32le_bytes(dhdr + 12, (uint32)sizeof(dp));
    if (stream_write(dhdr, 1, sizeof(dhdr), f) != sizeof(dhdr)) return false;
    if (stream_write(dp, 1, sizeof(dp), f) != sizeof(dp)) return false;
  }
  if (g_has_chains_ctx) {
    ChainsRuntimeSession session;
    bool has_session = Chains_RuntimeGetSession(&session);
    uint8 cp[9];
    cp[0] = 1u;  // format_version
    cp[1] = g_ctx_chains_attempt;
    cp[2] = (uint8)(g_ctx_chains_digest24 & 0xff);
    cp[3] = (uint8)((g_ctx_chains_digest24 >> 8) & 0xff);
    cp[4] = (uint8)((g_ctx_chains_digest24 >> 16) & 0xff);
    cp[5] = has_session ? ((session.origin_active ? 1u : 0u) |
                           (session.terminal_active ? 2u : 0u)) : 0u;
    put_u16le_bytes(cp + 6, has_session ? session.origin_exit_room : 0);
    cp[8] = has_session ? session.terminal_dungeon : (uint8)kRandoDungeon_None;
    uint8 chdr[16];
    memcpy(chdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(chdr + 8, kRandoSnapshotTail_Type_ChainLayout);
    put_u32le_bytes(chdr + 12, (uint32)sizeof(cp));
    if (stream_write(chdr, 1, sizeof(chdr), f) != sizeof(chdr)) return false;
    if (stream_write(cp, 1, sizeof(cp), f) != sizeof(cp)) return false;
  }
  if (g_has_entrance_ctx) {
    uint8 ep[6];
    ep[0] = 1u;  // format_version
    ep[1] = g_ctx_entrance_axes;
    ep[2] = g_ctx_entrance_attempt;
    ep[3] = (uint8)(g_ctx_entrance_digest24 & 0xff);
    ep[4] = (uint8)((g_ctx_entrance_digest24 >> 8) & 0xff);
    ep[5] = (uint8)((g_ctx_entrance_digest24 >> 16) & 0xff);
    uint8 ehdr[16];
    memcpy(ehdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(ehdr + 8, kRandoSnapshotTail_Type_EntranceLayout);
    put_u32le_bytes(ehdr + 12, (uint32)sizeof(ep));
    if (stream_write(ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) return false;
    if (stream_write(ep, 1, sizeof(ep), f) != sizeof(ep)) return false;
  }
  if (g_has_recommended_features_ctx) {
    uint8 fp[5];
    fp[0] = 1u;  // format_version
    put_u32le_bytes(fp + 1, g_ctx_recommended_features0);
    uint8 fhdr[16];
    memcpy(fhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(fhdr + 8, kRandoSnapshotTail_Type_RecommendedFeatures);
    put_u32le_bytes(fhdr + 12, (uint32)sizeof(fp));
    if (stream_write(fhdr, 1, sizeof(fhdr), f) != sizeof(fhdr)) return false;
    if (stream_write(fp, 1, sizeof(fp), f) != sizeof(fp)) return false;
  }
  // add-enemy-souls — soul ownership TLV (type 8). Emitted whenever a settings
  // context is active (souls are only meaningful on a settings-bearing slot).
  // Payload: format_version(1) + soul_flags[8]. Read LIVE like the ownership
  // bytes in the type-2 TLV so it reflects the snapshot instant.
  if (g_has_settings_ctx) {
    // add-npc-souls: widened to kSoulFlagsBytes (12). The payload keeps
    // format_version 1 and only GROWS in length, so pre-widening builds
    // (which read 8 bytes and skip excess) still restore the enemy block.
    uint8 up[1 + 12];
    up[0] = 1u;  // format_version
    memcpy(up + 1, Souls_Flags(), 12);
    uint8 uhdr[16];
    memcpy(uhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(uhdr + 8, kRandoSnapshotTail_Type_Souls);
    put_u32le_bytes(uhdr + 12, (uint32)sizeof(up));
    if (stream_write(uhdr, 1, sizeof(uhdr), f) != sizeof(uhdr)) return false;
    if (stream_write(up, 1, sizeof(up), f) != sizeof(up)) return false;
  }
  // Hints v2 immutable plan identity plus mutable discovery (type 11).
  // Export succeeds only for a certified live plan (including the canonical
  // Hints-Off empty plan). Missing/mismatched/legacy hint state emits no TLV,
  // so a replay cannot mistake an unavailable deck for a current one.
  {
    RandoHintPersistedState hint_state;
    if (Rando_HintsExportPersistedState(&hint_state)) {
      uint8 hp[kRandoSnapshotTail_HintStatePayloadSize];
      uint8 hhdr[16];
      encode_hint_state_payload(&hint_state, hp);
      memcpy(hhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(hhdr + 8, kRandoSnapshotTail_Type_HintState);
      put_u32le_bytes(hhdr + 12,
                      kRandoSnapshotTail_HintStatePayloadSize);
      if (stream_write(hhdr, 1, sizeof(hhdr), f) != sizeof(hhdr)) return false;
      if (stream_write(hp, 1, sizeof(hp), f) != sizeof(hp)) return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Load
//
// Reads as many trailing TLV entries as the file holds. For each entry:
//   - Read 8-byte magic. EOF / short read / non-matching magic → terminate
//     loop cleanly (vanilla / older / unrelated trailing data).
//   - Read type[4] + length[4] (LE). On short read, terminate (malformed).
//   - If type is known, parse payload exactly `length` bytes.
//   - If type is unknown, fseek past `length` bytes; loop continues.
//
// Returns the number of recognized TLVs consumed.
// ---------------------------------------------------------------------------
static RandoPlacement g_tail_entries[kRandoLocationCapacity];
static RandoPlacementTable g_tail_table;

static int snapshot_load(SnapshotStream *f) {
  Rando_ClearDeferredPotConfirmation();
  if (f == NULL) return 0;
  // LoadSnesState restores g_ram but not the process-global hint plan,
  // discovery, or paid presentation state. Clear them before looking at the
  // tail so EOF/no-magic/unknown-only/legacy tails cannot inherit another
  // seed's deck. A valid type-11 record is rebuilt at FINISH_LOAD below.
  Rando_ClearHints();
  int recognized = 0;
  bool pending_door_layout = false;
  bool pending_chain_layout = false;
  // Armed only when the type-2 restore actually took the COLD path on an
  // entrance-shuffled seed: the cold restore tore down any stale entrance
  // overlay, so a valid type-9 EntranceLayout TLV must reinstall it — or the
  // load fails CLOSED (older entrance snapshots lack the TLV; silently
  // replaying with vanilla entrances desyncs the certified placement). Warm
  // same-slot replays keep the activation-installed layout and never arm this.
  bool pending_entrance_layout = false;
  bool has_pot_registry_ctx = false;
  uint32 pot_registry_digest = 0;
  uint16 pot_registry_count = 0;
  // add-rando-grass-rock-shuffle — terrain registry identity rides the same
  // PotRegistry TLV (payload format_version >= 2), so cold replay guards a
  // terrain-active snapshot the way the sidecar activation guard does.
  bool has_terrain_registry_ctx = false;
  uint32 terrain_registry_digest = 0;
  uint16 terrain_registry_count = 0;
  // Enemy-check registry identity rides the same TLV (payload format_version 3
  // appends it); pre-v3 snapshots leave it absent and an enemy-check-active
  // cold replay fails CLOSED below (same posture as the sidecar guard).
  bool has_enemy_registry_ctx = false;
  uint32 enemy_registry_digest = 0;
  uint16 enemy_registry_count = 0;
  // Bonk registry identity rides the same TLV (payload format_version 4
  // appends it); pre-v4 snapshots leave it absent and a bonk-active cold
  // replay fails CLOSED below (add-rando-bonk-sanity).
  bool has_bonk_registry_ctx = false;
  uint32 bonk_registry_digest = 0;
  uint16 bonk_registry_count = 0;
  bool pending_settings_header_clear = false;
  bool accepted_rando_state = false;
  RandoSnapshotPendingHintState pending_hint;
  pending_hint_state_reset(&pending_hint);

#define FINISH_LOAD() do { \
    if (pending_settings_header_clear) { \
      Rando_ClearSnapshotReplayHeader(); \
      pending_settings_header_clear = false; \
    } \
    if (pending_door_layout) { \
      fprintf(stderr, \
              "RandoSnapshotTail: door-shuffle snapshot missing a valid " \
              "DoorLayout TLV — deactivating randomizer state\n"); \
      Rando_DeactivateSlot(); \
      pending_door_layout = false; \
    } \
    if (pending_chain_layout) { \
      fprintf(stderr, \
              "RandoSnapshotTail: dungeon-chain snapshot missing a valid " \
              "ChainLayout TLV - deactivating randomizer state\n"); \
      Rando_DeactivateSlot(); \
      pending_chain_layout = false; \
    } \
    if (pending_entrance_layout) { \
      fprintf(stderr, \
              "RandoSnapshotTail: entrance-shuffle snapshot missing a valid " \
              "EntranceLayout TLV - refusing the cold replay (re-save the " \
              "snapshot on a current build) - deactivating randomizer state\n"); \
      Rando_DeactivateSlot(); \
      pending_entrance_layout = false; \
    } \
    /* Ownership depends on both the type-1 placement and the later type-3 */ \
    /* checked bitmap. Rebuild only after the complete TLV chain has run. */ \
    if (accepted_rando_state && Rando_IsActive()) { \
      Rando_RebuildKeyItemOwnership(); \
      /* tracker-player-knowledge: derive dungeon/cave discovery from the  */ \
      /* type-3 checked bitmap (idempotent OR) — covers snapshots without  */ \
      /* a type-10 TLV and self-heals newer checks on older discovery.     */ \
      Rando_BackfillDiscoveryFromChecked(); \
      /* The type-2 settings restore installs the shuffle logic overlays */ \
      /* (warp edges included), but the later door/chain/entrance layout */ \
      /* TLVs reset the shared logic-edge stores as part of their own    */ \
      /* installs. Rebuild the whole overlay set once, after every       */ \
      /* record has landed, so cold replay converges on the same         */ \
      /* activation-order state a fresh activation produces.             */ \
      Rando_ReinstallActiveSlotLogicOverlays(); \
      /* Hint selection may consume restored spheres/assignments/layouts. */ \
      /* Import only one well-formed type-11 identity and rebuild after   */ \
      /* the authoritative overlay reinstall. Missing/malformed/duplicate */ \
      /* metadata routes through the builder's non-destructive unavailable */ \
      /* state; it never falls back to a current deck.                    */ \
      if (pending_hint.seen && pending_hint.valid &&                    \
          !pending_hint.invalid)                                       \
        Rando_HintsImportPersistedState(&pending_hint.state);           \
      else                                                              \
        Rando_HintsImportPersistedState(NULL);                          \
      {                                                                 \
        const RandoSettings *hint_settings = Rando_GetActiveSettings(); \
        const RandoPlacementTable *hint_placements = Placement_GetActive(); \
        if (hint_settings != NULL && hint_placements != NULL)           \
          (void)Rando_RebuildHints(hint_settings, hint_placements, NULL); \
      }                                                                 \
    } \
    return recognized; \
  } while (0)

  for (;;) {
    uint8 magic[kRandoSnapshotTail_MagicLen];
    size_t mr = stream_read(magic, 1, kRandoSnapshotTail_MagicLen, f);
    if (mr != kRandoSnapshotTail_MagicLen) FINISH_LOAD();  // EOF / short read
    if (memcmp(magic, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen) != 0) {
      // Unknown / non-matching magic — terminate cleanly.
      FINISH_LOAD();
    }

    uint8 hdr[8];  // type[4] + length[4]
    if (stream_read(hdr, 1, 8, f) != 8) FINISH_LOAD();
    uint32 type = get_u32le_bytes(hdr + 0);
    uint32 length = get_u32le_bytes(hdr + 4);

    // `length` is read from an untrusted file. Cap it before any `(long)`-cast
    // fseek below: on LLP64 (Windows x64) `long` is 32-bit, so a length >=
    // 0x80000000 casts to a NEGATIVE seek and the loop could rewind and re-read
    // the same header forever (hang on a corrupt-but-parseable snapshot). The
    // largest legal payload is the RandoState body (52 + kRandoLocationCapacity
    // * 2); cap well above that and bail on anything larger — a real tail never
    // exceeds it.
    if (length > kRandoSnapshotTail_MaxPayloadBytes) FINISH_LOAD();

    if (type == kRandoSnapshotTail_Type_RandoState) {
      accepted_rando_state = false;
      // A new type-1 record starts a new identity scope even if it later
      // proves malformed. No type-11 record from an earlier state may bind to
      // it, and a later valid type-1 starts clean.
      pending_hint_state_reset(&pending_hint);
      // Payload schema: gen_version[2] + settings_hash[16] + share_string[32]
      //               + placement_table_size[2] + placement_table[...]
      // Minimum length = 2 + 16 + 32 + 2 = 52 (empty table is legal).
      if (length < 52u) {
        // Malformed — seek past whatever payload claims and continue.
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      // Read fixed-size prefix.
      uint8 head[52];
      if (stream_read(head, 1, 52, f) != 52) FINISH_LOAD();
      uint16 gen_version = get_u16le_bytes(head + 0);
      const uint8 *settings_hash = head + 2;     // 16 bytes
      const uint8 *share_string  = head + 18;    // 32 bytes
      uint16 placement_table_bytes = get_u16le_bytes(head + 50);

      uint32 expected_body = (uint32)placement_table_bytes;
      uint32 expected_total = 52u + expected_body;
      if (length != expected_total) {
        // Inner-size mismatch — skip the rest and continue.
        long remaining = (long)length - 52L;
        if (remaining > 0 && stream_seek(f, remaining, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      // Read the placement table.
      uint16 location_count = (uint16)(placement_table_bytes / 2u);
      if (location_count > kRandoLocationCapacity) {
        // Reject — exceeds our static buffer; skip body, continue.
        if (placement_table_bytes > 0 && stream_seek(f, (long)placement_table_bytes, SEEK_CUR) != 0) {
          FINISH_LOAD();
        }
        continue;
      }
      // Reject an ODD or oversized placement_table_bytes BEFORE the fread.
      // The `location_count > kRandoLocationCapacity` check above bounds
      // location_count (= placement_table_bytes/2, integer-truncated), but the
      // fread below uses the raw placement_table_bytes: an odd value (e.g.
      // capacity*2 + 1) truncates to location_count==capacity (passes the reject)
      // yet would fread one byte past `raw[]` — a 1-byte stack overflow. Require
      // an exact even round-trip and a hard ≤ capacity*2 cap. Skip the body +
      // continue, mirroring the location_count reject branch.
      if (placement_table_bytes != (uint16)(location_count * 2u) ||
          placement_table_bytes > (uint32)kRandoLocationCapacity * 2u) {
        if (placement_table_bytes > 0 && stream_seek(f, (long)placement_table_bytes, SEEK_CUR) != 0) {
          FINISH_LOAD();
        }
        continue;
      }
      if (placement_table_bytes > 0) {
        uint8 raw[kRandoLocationCapacity * 2];  // max kRandoLocationCapacity locations × 2 bytes
        if (stream_read(raw, 1, placement_table_bytes, f) != placement_table_bytes) {
          FINISH_LOAD();
        }
        uint16 sparse_count = 0;
        for (uint16 i = 0; i < location_count; i++) {
          uint16 item_id = get_u16le_bytes(raw + i * 2);
          if (item_id == 0xFFFFu) continue;  // sentinel — no placement
          g_tail_entries[sparse_count].location_id = i;
          g_tail_entries[sparse_count].item_id = item_id;
          sparse_count++;
        }
        g_tail_table.entries = g_tail_entries;
        g_tail_table.count = sparse_count;
        Placement_Install(&g_tail_table);
      } else {
        // Empty table — install an empty placement (effectively clears).
        g_tail_table.entries = g_tail_entries;
        g_tail_table.count = 0;
        Placement_Install(&g_tail_table);
      }

      // Older-snapshot clean-restore contract (randomizer-save spec scenario
      // "Older snapshot without the TLV restores cleanly"): clear the checked
      // bitmap when a valid type-1 RandoState is accepted. g_rando_checked_bitmap
      // is C-global state OUTSIDE the g_ram dump LoadSnesState restored, so
      // without this an older snapshot — written before the type-3 CheckedBitmap
      // TLV existed — would INHERIT whatever checked bits were live from the
      // current slot/session, suppressing or re-granting unrelated pots/checks.
      // A current-binary snapshot's type-3 TLV (emitted right after type-1)
      // re-memsets and restores the real bitmap below, so this clear is
      // load-bearing ONLY for the type-3-absent case.
      memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);

      // add-enemy-souls — same clean-restore contract as the checked bitmap:
      // soul ownership is C-global state outside the g_ram dump. Clear it on
      // every accepted RandoState so an older/type-8-absent snapshot cannot
      // inherit stale souls; a current snapshot's type-8 Souls TLV (emitted
      // after type-1) restores the real bitfield below.
      Souls_ResetFlags();

      // tracker-player-knowledge — same clean-restore contract again, and the
      // one that was missing: topology discovery is C-global state outside the
      // g_ram dump, so a snapshot with no type-10 TLV INHERITED whatever the
      // live session had discovered. Replaying a pre-kGen-157 snapshot of seed B
      // during a seed-A session therefore over-revealed B's tracker — and the
      // FINISH_LOAD backfill then persisted it to B's sidecar, which the save
      // spec's "never over-reveal" invariant forbids. The type-10 restore's own
      // comment already promised "Absent TLV → zeros + backfill"; this is the
      // zeros half. A current snapshot's type-10 TLV overwrites it below, and
      // the backfill re-derives whatever the checked bitmap legitimately implies.
      Rando_ResetDiscoveryState();

      // Optional TLV contexts are scoped to this accepted RandoState. A current
      // snapshot's later type-2/type-4 TLVs repopulate them; an older snapshot
      // that lacks those TLVs must not inherit stale context from a previous
      // replay and then re-save it under this new base identity.
      Rando_ClearSnapshotOptionalContexts();
      Rando_ClearSnapshotSettingsReplayRestore();
      has_pot_registry_ctx = false;
      pot_registry_digest = 0;
      pot_registry_count = 0;
      has_terrain_registry_ctx = false;
      terrain_registry_digest = 0;
      terrain_registry_count = 0;
      has_enemy_registry_ctx = false;
      enemy_registry_digest = 0;
      enemy_registry_count = 0;
      has_bonk_registry_ctx = false;
      bonk_registry_digest = 0;
      bonk_registry_count = 0;
      pending_settings_header_clear = true;
      pending_chain_layout = false;
      pending_entrance_layout = false;

      // Reinstall context so future re-saves of this snapshot carry the
      // same metadata.
      Rando_SetSnapshotContext(gen_version, settings_hash, share_string);
      accepted_rando_state = true;
      // Any restore drops an armed mid-staircase spiral redirect (process
      // state; it must not fire on a later unrelated room load).
      DoorRt_ClearSpiralPending();
      pending_door_layout = (enhanced_features1 & kFeatures1_DoorShuffleActive) != 0;
      // Door shuffle is process state, not part of the raw g_ram dump. Clear
      // any currently-installed door graph for every accepted rando snapshot;
      // a current snapshot's type-5 DoorLayout TLV will reinstall the exact
      // graph after digest validation. Older/no-type-5 snapshots fail closed
      // instead of inheriting a door layout from a different active slot.
      Rando_ClearSnapshotDoorReplayRestore();
      Rando_ClearSnapshotChainsReplayRestore();
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_HintState) {
      // Payload format 1 is EXACTLY 41 bytes. Read/skip the complete claimed
      // payload before interpreting it so malformed records cannot desync the
      // following TLV chain.
      uint8 hp[kRandoSnapshotTail_HintStatePayloadSize];
      memset(hp, 0, sizeof(hp));
      uint32 copy =
          length < kRandoSnapshotTail_HintStatePayloadSize
              ? length
              : kRandoSnapshotTail_HintStatePayloadSize;
      if (copy > 0 && stream_read(hp, 1, copy, f) != copy) FINISH_LOAD();
      if (length > copy &&
          stream_seek(f, (long)(length - copy), SEEK_CUR) != 0)
        FINISH_LOAD();
      recognized++;

      pending_hint_state_consume(&pending_hint, accepted_rando_state, hp,
                                 length);
      continue;
    }

    if (type == kRandoSnapshotTail_Type_PotRegistry) {
      // Payload v1: format_version[1] + pot_digest[4] + pot_count[2] (7 bytes).
      // Payload v2 (add-rando-grass-rock-shuffle): + terrain_digest[4] +
      // terrain_count[2] (13 bytes). Payload v3: + enemy_check_digest[4] +
      // enemy_check_count[2] (19 bytes). Payload v4 (add-rando-bonk-sanity):
      // + bonk_digest[4] + bonk_count[2] (25 bytes). Read up to 25; older
      // snapshots leave the newer contexts absent (so an axis-active older
      // snapshot is refused below — fail-closed, same posture as the sidecar
      // activation guard).
      if (length < 7u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 pp[25];
      size_t take = length < sizeof(pp) ? (size_t)length : sizeof(pp);
      if (stream_read(pp, 1, take, f) != take) FINISH_LOAD();
      if (length > take && stream_seek(f, (long)(length - take), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (pp[0] >= 1u && take >= 7u) {
        pot_registry_digest = get_u32le_bytes(pp + 1);
        pot_registry_count = get_u16le_bytes(pp + 5);
        has_pot_registry_ctx = true;
      }
      if (pp[0] >= 2u && take >= 13u) {
        terrain_registry_digest = get_u32le_bytes(pp + 7);
        terrain_registry_count = get_u16le_bytes(pp + 11);
        has_terrain_registry_ctx = true;
      }
      if (pp[0] >= 3u && take >= 19u) {
        enemy_registry_digest = get_u32le_bytes(pp + 13);
        enemy_registry_count = get_u16le_bytes(pp + 17);
        has_enemy_registry_ctx = true;
      }
      if (pp[0] >= 4u && take >= 25u) {
        bonk_registry_digest = get_u32le_bytes(pp + 19);
        bonk_registry_count = get_u16le_bytes(pp + 23);
        has_bonk_registry_ctx = true;
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_RandoSettings) {
      // Payload: format_version[1] + prize_attempt[1] + ownership[4]
      //           + settings_len[1] + settings_canonical[settings_len].
      // Minimum length = 1 + 1 + 4 + 1 = 7.
      if (length < 7u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 head2[7];
      if (stream_read(head2, 1, 7, f) != 7) FINISH_LOAD();
      uint8 fmt              = head2[0];
      uint8 prize_attempt    = head2[1];
      uint8 own_mushroom     = head2[2];
      uint8 own_flute_shovel = head2[3];
      uint8 own_boomerang    = head2[4];
      uint8 own_bow          = head2[5];
      uint8 settings_len     = head2[6];
      // v1 payload = 7 + settings_len; v2 (add-rando-ow-warp-shuffle)
      // appends ow_attempt[1] + ow_digest24[3] after the blob.
      bool v2_tail = (fmt >= 2u) &&
                     (uint32)length == 7u + (uint32)settings_len + 4u;
      if ((uint32)length != 7u + (uint32)settings_len && !v2_tail) {
        // Inner-size mismatch — skip the declared remainder and continue.
        long remaining = (long)length - 7L;
        if (remaining > 0 && stream_seek(f, remaining, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      // A LONGER blob comes from a newer binary carrying axes this build
      // doesn't know. Refuse it (mirrors Share_Decode's
      // kShareDecodeNewerSettings): honoring a prefix would silently drop
      // the unknown axes and restore the seed under wrong settings. SHORTER
      // (older) blobs still zero-extend below — zero is the append-only
      // default for later-added axes.
      if (settings_len > kSettingsCanonicalLen) {
        if (stream_seek(f, (long)settings_len, SEEK_CUR) != 0) FINISH_LOAD();
        if (accepted_rando_state && g_has_ctx) {
          fprintf(stderr,
                  "RandoSnapshotTail: snapshot settings blob carries %u bytes "
                  "but this build understands %u (snapshot from a newer "
                  "binary?) - deactivating randomizer state\n",
                  (unsigned)settings_len, (unsigned)kSettingsCanonicalLen);
          Rando_DeactivateSlot();
          pending_door_layout = false;
          pending_chain_layout = false;
        }
        recognized++;
        continue;
      }
      // Read the canonical settings blob (zero-extending an older/shorter one).
      uint8 blob[kSettingsCanonicalLen];
      memset(blob, 0, sizeof(blob));
      uint32 copy = settings_len;
      if (copy > 0 && stream_read(blob, 1, copy, f) != copy) FINISH_LOAD();
      // v2 tail: warp layout identity (zeros under v1 = vanilla warps).
      uint8 ow_attempt = 0;
      uint32 ow_digest24 = 0;
      if (v2_tail) {
        uint8 tail4[4];
        if (stream_read(tail4, 1, 4, f) != 4) FINISH_LOAD();
        ow_attempt = tail4[0];
        ow_digest24 = (uint32)tail4[1] | ((uint32)tail4[2] << 8) |
                      ((uint32)tail4[3] << 16);
      }
      // Everything below is interpreted per format_version 1/2 (v2 only adds
      // the tail above). An UNKNOWN fmt is a newer writer's payload layout we
      // can't parse — the bytes were already consumed above, so just count
      // the TLV recognized and move on WITHOUT touching ownership/settings
      // (head2[]/blob[] may not mean what we assume).
      if (fmt == 1u || fmt == 2u) {
        // Settings-derived reconstruction (world_state + prize/medallion/boss/drop/
        // enemy assignments + Inverted installs) — GATED inside the restore helper
        // to fire only on a true cold replay (no slot active). Needs the base seed,
        // which the type-1 share_string supplies (type-1 precedes type-2 in the
        // file, so g_ctx.share_string is already populated when g_has_ctx).
        if (accepted_rando_state && g_has_ctx) {
          RandoSettings s;
          if (Settings_CanonicalDeserialize(blob, &s) != 0) {
            fprintf(stderr,
                    "RandoSnapshotTail: snapshot settings blob failed range "
                    "validation - deactivating randomizer state\n");
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          if (!Rando_SnapshotSettingsAllowedForReplay(&s)) {
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          if (Rando_SettingsNeedPotRegistry(&s) &&
              (!has_pot_registry_ctx ||
               !Rando_PotRegistryMatches(pot_registry_digest, pot_registry_count))) {
            fprintf(stderr,
                    "RandoSnapshotTail: pot-shuffle registry drift or missing "
                    "registry identity — deactivating randomizer state\n");
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          // add-rando-grass-rock-shuffle — same guard for the terrain registry
          // (review MED-2): a terrain-active snapshot replayed on a build with
          // absent (v1 TLV / empty) or drifted terrain data would otherwise
          // reinstall stale terrain placement ids unguarded.
          if (Rando_SettingsNeedTerrainRegistry(&s) &&
              (!has_terrain_registry_ctx ||
               Rando_CurrentTerrainRegistryCount() == 0 ||
               !Rando_TerrainRegistryMatches(terrain_registry_digest,
                                             terrain_registry_count))) {
            fprintf(stderr,
                    "RandoSnapshotTail: grass/rock terrain registry drift or "
                    "missing registry identity — deactivating randomizer state\n");
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          // Enemy-check registry guard, same shape: an enemy-check-active
          // snapshot replayed against an absent (pre-v3 TLV / empty build
          // registry) or drifted enemy-check registry would reinstall stale
          // enemy-check placement ids unguarded — the sidecar activation
          // guard's cold-replay equivalent. Fails CLOSED for older snapshots
          // that predate the identity.
          if (Rando_SettingsNeedEnemyCheckRegistry(&s) &&
              (!has_enemy_registry_ctx ||
               Rando_CurrentEnemyCheckRegistryCount() == 0 ||
               !Rando_EnemyCheckRegistryMatches(enemy_registry_digest,
                                                enemy_registry_count))) {
            fprintf(stderr,
                    "RandoSnapshotTail: enemy-check registry drift or missing "
                    "registry identity — deactivating randomizer state\n");
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          // Bonk registry guard, same shape (add-rando-bonk-sanity): a
          // bonk-active snapshot replayed against an absent (pre-v4 TLV /
          // empty build registry) or drifted bonk registry fails CLOSED.
          if (Rando_SettingsNeedBonkRegistry(&s) &&
              (!has_bonk_registry_ctx ||
               Rando_CurrentBonkRegistryCount() == 0 ||
               !Rando_BonkRegistryMatches(bonk_registry_digest,
                                          bonk_registry_count))) {
            fprintf(stderr,
                    "RandoSnapshotTail: bonk registry drift or missing "
                    "registry identity - deactivating randomizer state\n");
            Rando_DeactivateSlot();
            pending_door_layout = false;
            pending_chain_layout = false;
            recognized++;
            FINISH_LOAD();
          }
          // Restore the 4 process-static ownership bytes — the snapshot-instant
          // runtime grant state (which tier of bow / boomerang / flute-shovel /
          // mushroom the player owns), NOT in g_ram, so neither LoadSnesState nor
          // the canonical settings reconstruct them. Restored on EVERY replay (cold
          // OR within-session) to match the g_ram the snapshot restored.
          g_rando_mushroom_held      = own_mushroom;
          g_rando_flute_shovel_owned = own_flute_shovel;
          g_rando_boomerang_owned    = own_boomerang;
          g_rando_bow_owned          = own_bow;
          // Reinstall the settings sub-context so a later re-save (Shift+Fn)
          // perpetuates the type-2 TLV — mirrors the type-1 branch's
          // Rando_SetSnapshotContext reinstall. Without this, a
          // cold-replayed-then-resaved snapshot would emit type-1 only and lose
          // world_state/Inverted/shuffle reconstruction on its next cold replay.
          // `blob` is already zero-extended to kSettingsCanonicalLen.
          Rando_SetSnapshotSettingsContext(blob, prize_attempt, ow_attempt,
                                           ow_digest24);
          bool cold_restored =
              Rando_SnapshotColdReplayRestore(&s, g_ctx.share_string,
                                              prize_attempt, ow_attempt,
                                              ow_digest24);
          pending_settings_header_clear = false;
          pending_chain_layout = Settings_EffectiveDungeonChains(&s);
          // Cold restore tore down any stale entrance overlay; an
          // entrance-shuffled seed must now get its verified layout back from
          // the type-9 TLV or fail closed. Warm same-slot replays keep the
          // activation-installed layout (cold_restored == false).
          pending_entrance_layout =
              cold_restored && Rando_SettingsHaveEntranceShuffle(&s);
        }
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_DoorLayout) {
      // Payload: format_version[1] + door_attempt[1] + door_digest24[3].
      if (length < 5u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 dp[5];
      if (stream_read(dp, 1, sizeof(dp), f) != sizeof(dp)) FINISH_LOAD();
      if (length > sizeof(dp) && stream_seek(f, (long)(length - sizeof(dp)), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (dp[0] == 1u && accepted_rando_state) {
        uint8 door_attempt = dp[1];
        uint32 door_digest24 =
            (uint32)dp[2] | ((uint32)dp[3] << 8) | ((uint32)dp[4] << 16);
        Rando_SetSnapshotDoorContext(door_attempt, door_digest24, true);
        if (g_has_ctx && g_has_settings_ctx) {
          RandoSettings s;
          if (Settings_CanonicalDeserialize(g_ctx_settings_canonical, &s) == 0) {
            if (Rando_SnapshotDoorReplayRestore(&s, g_ctx.share_string,
                                                door_attempt, door_digest24)) {
              pending_door_layout = false;
            }
          }
        }
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_EntranceLayout) {
      // Payload: format_version[1] + entrance_axes[1] + entrance_attempt[1] +
      // entrance_digest24[3]. Mirrors the DoorLayout flow: regenerate,
      // digest-verify, install; a successful install clears the fail-closed
      // pending flag armed by the type-2 cold restore.
      if (length < 6u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 ep[6];
      if (stream_read(ep, 1, sizeof(ep), f) != sizeof(ep)) FINISH_LOAD();
      if (length > sizeof(ep) && stream_seek(f, (long)(length - sizeof(ep)), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (ep[0] == 1u && accepted_rando_state) {
        uint8 entrance_axes = ep[1];
        uint8 entrance_attempt = ep[2];
        uint32 entrance_digest24 =
            (uint32)ep[3] | ((uint32)ep[4] << 8) | ((uint32)ep[5] << 16);
        Rando_SetSnapshotEntranceContext(entrance_axes, entrance_attempt,
                                         entrance_digest24, true);
        if (g_has_ctx && g_has_settings_ctx && entrance_axes != 0) {
          RandoSettings s;
          if (Settings_CanonicalDeserialize(g_ctx_settings_canonical, &s) == 0) {
            if (Rando_SnapshotEntranceReplayRestore(&s, g_ctx.share_string,
                                                    entrance_axes,
                                                    entrance_attempt,
                                                    entrance_digest24)) {
              pending_entrance_layout = false;
            }
          }
        }
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_ChainLayout) {
      // Payload: format_version[1] + chains_attempt[1] + chains_digest24[3],
      // optionally followed by session_flags + origin_exit_room +
      // terminal_dungeon.
      if (length < 5u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 cp[9];
      memset(cp, 0, sizeof(cp));
      uint32 copy = length <= (uint32)sizeof(cp) ? length : (uint32)sizeof(cp);
      if (stream_read(cp, 1, copy, f) != copy) FINISH_LOAD();
      if (length > copy && stream_seek(f, (long)(length - copy), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (cp[0] == 1u && accepted_rando_state) {
        uint8 chains_attempt = cp[1];
        uint32 chains_digest24 =
            (uint32)cp[2] | ((uint32)cp[3] << 8) | ((uint32)cp[4] << 16);
        ChainsRuntimeSession session;
        memset(&session, 0, sizeof(session));
        bool has_session_payload = length >= 9u;
        if (has_session_payload) {
          session.origin_active = (cp[5] & 1u) != 0;
          session.terminal_active = (cp[5] & 2u) != 0;
          session.origin_exit_room = get_u16le_bytes(cp + 6);
          session.terminal_dungeon = cp[8];
        }
        Rando_SetSnapshotChainsContext(chains_attempt, chains_digest24, true);
        if (g_has_ctx && g_has_settings_ctx) {
          RandoSettings s;
          if (Settings_CanonicalDeserialize(g_ctx_settings_canonical, &s) == 0) {
            if (Rando_SnapshotChainsReplayRestore(&s, g_ctx.share_string,
                                                  chains_attempt, chains_digest24)) {
              if (!has_session_payload || Chains_RuntimeRestoreSession(&session)) {
                pending_chain_layout = false;
              } else {
                fprintf(stderr,
                        "RandoSnapshotTail: invalid dungeon-chain session "
                        "state - deactivating randomizer state\n");
                Rando_DeactivateSlot();
                pending_chain_layout = false;
              }
            }
          }
        }
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_RecommendedFeatures) {
      // Payload: format_version[1] + recommended_features0[4].
      if (length < 5u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 fp[5];
      if (stream_read(fp, 1, sizeof(fp), f) != sizeof(fp)) FINISH_LOAD();
      if (length > sizeof(fp) && stream_seek(f, (long)(length - sizeof(fp)), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (fp[0] == 1u && accepted_rando_state) {
        uint32 features0 = get_u32le_bytes(fp + 1);
        Rando_SetSnapshotRecommendedFeaturesContext(features0, true);
        if (Rando_IsActive())
          Rando_ApplySeedQolFeatures0(features0);
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_CheckedBitmap) {
      // Restore the checked-location bitmap (a C global outside the g_ram dump).
      // Read min(length, kRandoCheckedBitmapBytes): a SMALLER payload (older /
      // lower-capacity snapshot) zero-extends, a LARGER one (newer binary) is
      // truncated to our static bound and the remainder skipped. memset first so
      // a short payload leaves the high locations un-checked (0), not stale.
      uint32 copy = (length <= (uint32)kRandoCheckedBitmapBytes)
                        ? length : (uint32)kRandoCheckedBitmapBytes;
      uint8 discard[kRandoCheckedBitmapBytes];
      uint8 *dst = accepted_rando_state ? g_rando_checked_bitmap : discard;
      memset(dst, 0, kRandoCheckedBitmapBytes);
      if (copy > 0 && stream_read(dst, 1, copy, f) != copy) FINISH_LOAD();
      if (length > copy && stream_seek(f, (long)(length - copy), SEEK_CUR) != 0) FINISH_LOAD();
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_Discovery) {
      // tracker-player-knowledge — restore topology-discovery state. Payload:
      // format_version[1] + dungeons[2 LE] + whirlpools[1] + caves[8] +
      // exits[8] (20 bytes); a LONGER payload (future door-phase append) has
      // its known prefix parsed and the excess skipped. Only applied to an
      // accepted rando state, mirroring the type-3/type-8 scoping. Absent TLV
      // → zeros + the FINISH_LOAD backfill from the checked bitmap.
      if (length < 20u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      uint8 kp[20];
      if (stream_read(kp, 1, sizeof(kp), f) != sizeof(kp)) FINISH_LOAD();
      if (length > (uint32)sizeof(kp) &&
          stream_seek(f, (long)(length - (uint32)sizeof(kp)), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (kp[0] == 1u && accepted_rando_state) {
        Rando_SetDiscoveryState((uint16)kp[1] | ((uint16)kp[2] << 8), kp[3],
                                kp + 4, kp + 12);
      }
      recognized++;
      continue;
    }

    if (type == kRandoSnapshotTail_Type_Souls) {
      // add-enemy-souls — restore soul ownership. Payload: format_version[1] +
      // soul_flags[8]. Absent TLV leaves souls zeroed (RandoState acceptance
      // resets them); a short payload zero-extends. Only apply to an accepted
      // rando state, mirroring the ownership handling in the type-2 TLV.
      if (length < 9u) {
        if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
        continue;
      }
      // Accept the legacy 8-byte (length 9) AND the widened 12-byte (length
      // 13) payloads; a short payload zero-extends the tail (NPC souls
      // un-owned — the safe default for pre-widening snapshots).
      uint8 up[13] = { 0 };
      uint32 body = length < (uint32)sizeof(up) ? length : (uint32)sizeof(up);
      if (stream_read(up, 1, body, f) != body) FINISH_LOAD();
      if (length > body && stream_seek(f, (long)(length - body), SEEK_CUR) != 0)
        FINISH_LOAD();
      if (up[0] == 1u && accepted_rando_state) {
        memset(Souls_Flags(), 0, 12);
        memcpy(Souls_Flags(), up + 1, body - 1 < 12u ? body - 1 : 12u);
      }
      recognized++;
      continue;
    }

    // Unknown type — seek past payload and continue.
    if (length > 0) {
      if (stream_seek(f, (long)length, SEEK_CUR) != 0) FINISH_LOAD();
    }
  }
#undef FINISH_LOAD
}

// ---------------------------------------------------------------------------
// Self-check — round-trip a synthetic TLV via tmpfile() and assert byte-
// equality + a known-good unknown-TLV-skip path.
// ---------------------------------------------------------------------------
static size_t file_read(void *p, size_t s, size_t n, void *f) { return fread(p,s,n,f); }
static size_t file_write(const void *p, size_t s, size_t n, void *f) { return fwrite(p,s,n,f); }
static int file_seek(void *f, long off, int origin) { return fseek(f,off,origin); }
static size_t rw_read(void *p, size_t s, size_t n, void *f) { return SDL_RWread(f,p,s,n); }
static size_t rw_write(const void *p, size_t s, size_t n, void *f) { return SDL_RWwrite(f,p,s,n); }
static int rw_seek(void *f, long off, int origin) { return SDL_RWseek(f,off,origin) < 0 ? -1 : 0; }

bool RandoSnapshotTail_Save(FILE *f) {
  if (!f) return false;
  SnapshotStream stream = {f, file_read, file_write, file_seek};
  return snapshot_save(&stream);
}
int RandoSnapshotTail_Load(FILE *f) {
  if (!f) return 0;
  SnapshotStream stream = {f, file_read, file_write, file_seek};
  return snapshot_load(&stream);
}
bool RandoSnapshotTail_SaveRW(SDL_RWops *f) {
  if (!f) return false;
  SnapshotStream stream = {f, rw_read, rw_write, rw_seek};
  return snapshot_save(&stream);
}
int RandoSnapshotTail_LoadRW(SDL_RWops *f) {
  if (!f) return 0;
  SnapshotStream stream = {f, rw_read, rw_write, rw_seek};
  return snapshot_load(&stream);
}

static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[RandoSnapshotTail_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

static uint64 snapshot_selfcheck_fold_entrance_logic_state(void) {
  uint64 h = 0xcbf29ce484222325ull;
  for (uint32 i = 0; i < 512; i++) {
    uint16 v = Rando_GetEntranceRegionOverride((uint16)i);
    h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
    h = (h ^ (v >> 8)) * 0x100000001b3ull;
  }
  for (uint32 i = 0; i < 64; i++) {
    uint16 v = Rando_GetEntranceEdgeOverride((uint16)i);
    h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
    h = (h ^ (v >> 8)) * 0x100000001b3ull;
  }
  // The added-edge store is the third overlay surface (decoupled/cross
  // entrance edges + the warp overlay). Fold its count so a wiped overlay
  // cannot alias the pre-wipe digest — the composed cold-replay probe
  // depends on this distinguishing power.
  uint32 n = (uint32)Rando_GetEntranceAddedEdgeCount();
  h = (h ^ (n & 0xFF)) * 0x100000001b3ull;
  h = (h ^ ((n >> 8) & 0xFF)) * 0x100000001b3ull;
  return h;
}

void RandoSnapshotTail_SelfCheck(void) {
  // Hints v2 type-11 payload codec: exact offsets, little-endian versions,
  // digest bytes, and reserved discovery-bit masking.
  {
    RandoHintPersistedState src, dst;
    uint8 payload[kRandoSnapshotTail_HintStatePayloadSize];
    memset(&src, 0, sizeof(src));
    src.algorithm_version = 0x1234u;
    src.text_schema_version = 0x5678u;
    for (uint8 i = 0; i < kRandoHintPlanDigestBytes; i++)
      src.plan_digest[i] = (uint8)(0x40u + i);
    src.discovered[0] = 0xA5u;
    src.discovered[1] = 0x5Au;
    src.discovered[2] = 0xC3u;
    src.discovered[3] = 0xFFu;  // reserved, writer/reader must mask it
    encode_hint_state_payload(&src, payload);
    if (payload[0] != kRandoSnapshotTail_HintStateFormat ||
        get_u16le_bytes(payload + 1) != 0x1234u ||
        get_u16le_bytes(payload + 3) != 0x5678u)
      selfcheck_die("type-11: format/version offsets");
    for (uint8 i = 0; i < kRandoHintPlanDigestBytes; i++) {
      if (payload[5 + i] != (uint8)(0x40u + i))
        selfcheck_die("type-11: digest offset");
    }
    if (payload[37] != 0xA5u || payload[38] != 0x5Au ||
        payload[39] != 0xC3u || payload[40] != 0)
      selfcheck_die("type-11: discovery offset/reserved mask");
    if (!decode_hint_state_payload(payload, &dst) ||
        dst.algorithm_version != src.algorithm_version ||
        dst.text_schema_version != src.text_schema_version ||
        memcmp(dst.plan_digest, src.plan_digest,
               kRandoHintPlanDigestBytes) != 0 ||
        memcmp(dst.discovered, src.discovered, 3) != 0 ||
        dst.discovered[3] != 0)
      selfcheck_die("type-11: payload round-trip");
    payload[0] = 2u;
    if (decode_hint_state_payload(payload, &dst))
      selfcheck_die("type-11: unknown payload format accepted");
    payload[0] = kRandoSnapshotTail_HintStateFormat;
    {
      RandoSnapshotPendingHintState pending;
      pending_hint_state_reset(&pending);
      pending_hint_state_consume(&pending, false, payload, sizeof(payload));
      if (pending.seen || pending.valid || pending.invalid)
        selfcheck_die("type-11: orphan record entered pending scope");
      pending_hint_state_consume(&pending, true, payload, sizeof(payload));
      if (!pending.seen || !pending.valid || pending.invalid)
        selfcheck_die("type-11: valid record not accepted");
      pending_hint_state_consume(&pending, true, payload, sizeof(payload));
      if (pending.valid || !pending.invalid)
        selfcheck_die("type-11: duplicate record did not fail hints closed");
      pending_hint_state_reset(&pending);
      pending_hint_state_consume(&pending, true, payload,
                                 sizeof(payload) - 1u);
      if (!pending.seen || pending.valid || !pending.invalid)
        selfcheck_die("type-11: wrong-length record did not fail closed");
      pending_hint_state_reset(&pending);
      payload[0] = 2u;
      pending_hint_state_consume(&pending, true, payload, sizeof(payload));
      if (!pending.seen || pending.valid || !pending.invalid)
        selfcheck_die("type-11: unknown format did not fail closed");
    }
  }

  // Build a synthetic placement table.
  static RandoPlacement entries[4];
  static RandoPlacementTable t;
  entries[0].location_id = 0;   entries[0].item_id = 0x0100;
  entries[1].location_id = 3;   entries[1].item_id = 0x0205;
  entries[2].location_id = 7;   entries[2].item_id = 0x0303;
  entries[3].location_id = 12;  entries[3].item_id = 0x0BEE;
  t.entries = entries;
  t.count = 4;

  // Capture any prior installed table so we can restore it after the test.
  const RandoPlacementTable *prior_table = Placement_GetActive();

  Placement_Install(&t);

  uint8 settings_hash[16];
  uint8 share_string[32];
  for (int i = 0; i < 16; i++) settings_hash[i] = (uint8)(0xA0 + i);
  for (int i = 0; i < 32; i++) share_string[i] = (uint8)(0x10 + i);
  Rando_SetSnapshotContext(0xCAFE, settings_hash, share_string);

  // Install the certified Hints-Off empty plan. Off is still a current,
  // versioned identity and must emit type 11 (not look like legacy/missing
  // metadata).
  RandoSettings hint_off_settings;
  Settings_SetDefaults(&hint_off_settings);
  hint_off_settings.hints = kHintsMode_Off;
  if (!Rando_GenerateHints(&hint_off_settings, &t, NULL))
    selfcheck_die("type-11: build/install canonical Off plan");
  RandoHintPersistedState saved_hint_state;
  if (!Rando_HintsExportPersistedState(&saved_hint_state))
    selfcheck_die("type-11: Off plan did not export certified identity");

  // Seed a couple of checked-bitmap bits so the type-3 round-trip is exercised
  // (the selfcheck runs at startup before any real pickup, so the bitmap is 0).
  g_rando_checked_bitmap[0] = 0x05;
  g_rando_checked_bitmap[10] = 0x80;

  // Round-trip via tmpfile.
  FILE *f = tmpfile();
  if (f == NULL) selfcheck_die("tmpfile() returned NULL");
  if (!RandoSnapshotTail_Save(f)) selfcheck_die("Save returned false");

  // Byte-inspect the actual emitted record, not only the codec helper.
  {
    uint8 expected[kRandoSnapshotTail_HintStatePayloadSize];
    encode_hint_state_payload(&saved_hint_state, expected);
    bool found = false;
    fseek(f, 0, SEEK_SET);
    for (;;) {
      uint8 hdr[16];
      if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) break;
      if (memcmp(hdr, kRandoSnapshotTail_Magic,
                 kRandoSnapshotTail_MagicLen) != 0)
        break;
      uint32 type = get_u32le_bytes(hdr + 8);
      uint32 length = get_u32le_bytes(hdr + 12);
      if (type == kRandoSnapshotTail_Type_HintState) {
        uint8 payload[kRandoSnapshotTail_HintStatePayloadSize];
        if (length != sizeof(payload) ||
            fread(payload, 1, sizeof(payload), f) != sizeof(payload) ||
            memcmp(payload, expected, sizeof(payload)) != 0)
          selfcheck_die("type-11: emitted payload bytes");
        found = true;
        break;
      }
      if (fseek(f, (long)length, SEEK_CUR) != 0) break;
    }
    if (!found) selfcheck_die("type-11: emitted record not found");
  }

  // Clear state so the reader is what reinstalls it (incl. the checked bitmap).
  Placement_Install(NULL);
  Rando_ClearSnapshotContext();
  Rando_ClearHints();
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);

  fseek(f, 0, SEEK_SET);
  int n = RandoSnapshotTail_Load(f);
  if (n != 4)
    selfcheck_die("Load consumed != 4 recognized TLVs (RandoState + CheckedBitmap + Discovery + HintState)");
  {
    RandoHintPersistedState should_be_unavailable;
    if (Rando_HintsExportPersistedState(&should_be_unavailable))
      selfcheck_die("type-11: inactive replay inherited pre-load plan");
  }
  if (g_rando_checked_bitmap[0] != 0x05 || g_rando_checked_bitmap[10] != 0x80)
    selfcheck_die("checked bitmap not restored from type-3 TLV");
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);  // restore startup state
  if (!Rando_HasSnapshotContext()) selfcheck_die("context not restored");
  if (Rando_GetSnapshotGeneratorVersion() != 0xCAFE) selfcheck_die("gen_version mismatch");
  if (memcmp(Rando_GetSnapshotSettingsHash(), settings_hash, 16) != 0) {
    selfcheck_die("settings_hash mismatch");
  }
  if (memcmp(Rando_GetSnapshotShareString(), share_string, 32) != 0) {
    selfcheck_die("share_string mismatch");
  }
  const RandoPlacementTable *r = Placement_GetActive();
  if (r == NULL) selfcheck_die("placement not installed after load");
  if (r->count != 4) selfcheck_die("placement count mismatch");
  for (uint16 i = 0; i < 4; i++) {
    if (r->entries[i].location_id != entries[i].location_id ||
        r->entries[i].item_id != entries[i].item_id) {
      selfcheck_die("placement entry round-trip mismatch");
    }
  }
  fclose(f);

  // Unknown-TLV-skip path: write magic + (type=0xFEEDFACE, length=8, junk[8])
  // then a known TLV; assert the unknown one is skipped and the known one
  // is still parsed.
  Placement_Install(NULL);
  Rando_ClearSnapshotContext();
  // Re-install the synthetic table so Save emits something.
  Placement_Install(&t);
  Rando_SetSnapshotContext(0x0001, settings_hash, share_string);

  FILE *f2 = tmpfile();
  if (f2 == NULL) selfcheck_die("tmpfile() for unknown-TLV test returned NULL");
  // Emit an unknown TLV first.
  uint8 unk_hdr[16];
  memcpy(unk_hdr, kRandoSnapshotTail_Magic, 8);
  put_u32le_bytes(unk_hdr + 8, 0xFEEDFACEu);
  put_u32le_bytes(unk_hdr + 12, 8u);
  fwrite(unk_hdr, 1, 16, f2);
  uint8 junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  fwrite(junk, 1, 8, f2);
  // Then the recognized TLV.
  if (!RandoSnapshotTail_Save(f2)) selfcheck_die("Save (unknown-TLV test) returned false");

  Placement_Install(NULL);
  Rando_ClearSnapshotContext();

  fseek(f2, 0, SEEK_SET);
  int n2 = RandoSnapshotTail_Load(f2);
  if (n2 != 3) selfcheck_die("Load (unknown-TLV test) didn't recognize 3 TLVs (state + bitmap + discovery)");
  if (!Rando_HasSnapshotContext()) selfcheck_die("context not restored (unknown-TLV test)");
  fclose(f2);

  // -------------------------------------------------------------------------
  // §8.9 snapshot replay test.
  // Spec scenario "Replay mode preserves rando": when StateRecorder_Load
  // restores g_ram from `base_snapshot` via LoadSnesState, the trailing TLV
  // reinstall must restore the placement table so every replayed frame's
  // dispatch fires correctly. We can't drive the full StateRecorder pipeline
  // from this self-check (it depends on game-runtime globals), but we CAN
  // exercise the equivalent TLV save → clear → load cycle and verify the
  // placement table comes back identically — which is exactly the state
  // StateRecorder_Load relies on after its LoadSnesState call.
  //
  // The §8.8 ClearKeyLog-on-rando-active forcing in StateRecorder_Save lives
  // in src/zelda_rtl.c; we sanity-check the contract here: Placement_GetActive
  // must be non-NULL when Save is asked to emit, AND HasSnapshotContext must
  // be true. The Save returns true (silently emits nothing) when either is
  // missing — graceful degradation. Verify both branches.
  // -------------------------------------------------------------------------
  {
    // Build a "mid-run" placement table and context — the state the player
    // would be in when they pressed Shift+F1 to snapshot.
    static RandoPlacement replay_entries[3];
    static RandoPlacementTable replay_table;
    replay_entries[0].location_id = 1;   replay_entries[0].item_id = 0xAAAA;
    replay_entries[1].location_id = 5;   replay_entries[1].item_id = 0xBBBB;
    replay_entries[2].location_id = 9;   replay_entries[2].item_id = 0xCCCC;
    replay_table.entries = replay_entries;
    replay_table.count = 3;

    uint8 r_settings[16];
    uint8 r_share[32];
    for (int i = 0; i < 16; i++) r_settings[i] = (uint8)(0x55 ^ i);
    for (int i = 0; i < 32; i++) r_share[i] = (uint8)(0x77 + i);

    Placement_Install(&replay_table);
    Rando_SetSnapshotContext(0xBEEF, r_settings, r_share);

    // "Snapshot save" — emit TLV to a tmpfile.
    FILE *fsnap = tmpfile();
    if (fsnap == NULL) selfcheck_die("§8.9: tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fsnap)) selfcheck_die("§8.9: Save returned false");

    // Simulate "exit + relaunch": clear all rando state, as if process died
    // and was restarted with no placement installed.
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    if (Placement_GetActive() != NULL) selfcheck_die("§8.9: clear placement should leave Active==NULL");
    if (Rando_HasSnapshotContext()) selfcheck_die("§8.9: clear context should leave HasContext==false");

    // "Replay-mode reload" — TLV consumer reinstalls placement + context.
    fseek(fsnap, 0, SEEK_SET);
    int recognized = RandoSnapshotTail_Load(fsnap);
    if (recognized != 3) selfcheck_die("§8.9: replay reload should recognize 3 TLVs (state + bitmap + discovery)");
    if (!Rando_HasSnapshotContext()) {
      selfcheck_die("§8.9: replay reload should restore snapshot context");
    }
    if (Rando_GetSnapshotGeneratorVersion() != 0xBEEF) {
      selfcheck_die("§8.9: replay reload should restore generator_version");
    }
    if (memcmp(Rando_GetSnapshotSettingsHash(), r_settings, 16) != 0) {
      selfcheck_die("§8.9: replay reload should restore settings_hash");
    }
    if (memcmp(Rando_GetSnapshotShareString(), r_share, 32) != 0) {
      selfcheck_die("§8.9: replay reload should restore share_string");
    }
    const RandoPlacementTable *post = Placement_GetActive();
    if (post == NULL) selfcheck_die("§8.9: replay reload should restore placement");
    if (post->count != 3) selfcheck_die("§8.9: placement count after replay reload");
    for (uint16 i = 0; i < 3; i++) {
      if (post->entries[i].location_id != replay_entries[i].location_id ||
          post->entries[i].item_id != replay_entries[i].item_id) {
        selfcheck_die("§8.9: placement entry mismatch after replay reload");
      }
    }
    fclose(fsnap);

    // §8.8 ClearKeyLog contract sanity-check: Save returns true (silent
    // emit) when no placement is installed — the snapshot then has a vanilla
    // tail and older readers see no rando state. This is the path
    // StateRecorder_Save in zelda_rtl.c takes for non-rando snapshots.
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    FILE *fsilent = tmpfile();
    if (fsilent == NULL) selfcheck_die("§8.9: tmpfile() silent-path");
    if (!RandoSnapshotTail_Save(fsilent)) selfcheck_die("§8.9: silent-emit path should return true");
    // File should be empty — no TLV header was written.
    fseek(fsilent, 0, SEEK_END);
    long silent_size = ftell(fsilent);
    if (silent_size != 0) selfcheck_die("§8.9: silent-emit path should write zero bytes");
    fclose(fsilent);

    // Same path for "placement installed but no context" — Save returns true
    // and emits nothing (cannot fabricate the payload metadata).
    Placement_Install(&replay_table);
    FILE *fno_ctx = tmpfile();
    if (fno_ctx == NULL) selfcheck_die("§8.9: tmpfile() no-ctx-path");
    if (!RandoSnapshotTail_Save(fno_ctx)) selfcheck_die("§8.9: no-context Save should return true");
    fseek(fno_ctx, 0, SEEK_END);
    long no_ctx_size = ftell(fno_ctx);
    if (no_ctx_size != 0) selfcheck_die("§8.9: no-context Save should write zero bytes");
    fclose(fno_ctx);
  }

  // -------------------------------------------------------------------------
  // §8.10 older-binary snapshot test.
  // Spec scenario "Older binary degrades gracefully": a TLV-aware writer's
  // output, when consumed by a reader that stops after the standard 4
  // chunks (older binary predates RandoSnapshotTail), leaves the older
  // binary in a clean vanilla state — the trailing tail bytes are simply
  // ignored.
  //
  // Two assertions to make:
  //
  //   1. Reading a snapshot that has NO trailing TLV (vanilla snapshot)
  //      cleanly returns 0 from RandoSnapshotTail_Load — no false positives.
  //      This is the equivalent test for the modern reader: it must not
  //      claim to recognize a TLV when none was emitted.
  //
  //   2. Reading a snapshot whose tail is a TLV with an UNKNOWN type code
  //      cleanly skips it and continues. This is the forward-compat
  //      property that lets older binaries cope with newer snapshots — the
  //      unknown-type-skip path is the same code path an older binary
  //      would use if it were extended to know about TLVs in general but
  //      not the specific new type. (An older binary that knows nothing
  //      about TLVs at all just closes the file after the 4 chunks; we
  //      can't simulate "close the file early" inside a self-check, but
  //      we CAN exercise the skip-then-continue path which is the same
  //      forward-compat machinery.)
  // -------------------------------------------------------------------------
  {
    // (1) Vanilla snapshot — no TLV bytes. Load should return 0 cleanly.
    FILE *fvanilla = tmpfile();
    if (fvanilla == NULL) selfcheck_die("§8.10: tmpfile() vanilla");
    // Write zero bytes — pure empty file, simulating a snapshot whose
    // standard chunks have already been consumed by the standard reader.
    fseek(fvanilla, 0, SEEK_SET);
    int n_vanilla = RandoSnapshotTail_Load(fvanilla);
    if (n_vanilla != 0) {
      selfcheck_die("§8.10: vanilla (empty) tail should yield zero recognized TLVs");
    }
    // Re-confirm prior state was untouched. We don't have a "snapshot prior"
    // saved here — just verify the loader was a pure no-op on its outputs.
    fclose(fvanilla);

    // (1b) "Garbage trailing bytes" (e.g., unrelated trailing payload from
    // a different format) — first 8 bytes don't match TLV magic, so loader
    // terminates cleanly. Mirrors the older-binary case where the trailing
    // bytes are simply foreign data the modern loader must reject without
    // crashing.
    FILE *fgarbage = tmpfile();
    if (fgarbage == NULL) selfcheck_die("§8.10: tmpfile() garbage");
    uint8 not_magic[8] = { 'X', 'Y', 'Z', 'Q', 'A', 'B', 'C', 'D' };
    fwrite(not_magic, 1, sizeof(not_magic), fgarbage);
    fseek(fgarbage, 0, SEEK_SET);
    int n_garbage = RandoSnapshotTail_Load(fgarbage);
    if (n_garbage != 0) {
      selfcheck_die("§8.10: non-matching magic should terminate loop cleanly (zero recognized)");
    }
    fclose(fgarbage);

    // (1c) Truncated TLV header (less than 8 bytes — magic incomplete).
    // Older-binary equivalent: snapshot file ends abruptly inside what would
    // have been a TLV. Loader returns 0 without crashing.
    FILE *ftrunc = tmpfile();
    if (ftrunc == NULL) selfcheck_die("§8.10: tmpfile() truncated");
    uint8 trunc[3] = { 'Z', 'R', 'S' };  // 3 of 8 magic bytes — short read
    fwrite(trunc, 1, sizeof(trunc), ftrunc);
    fseek(ftrunc, 0, SEEK_SET);
    int n_trunc = RandoSnapshotTail_Load(ftrunc);
    if (n_trunc != 0) {
      selfcheck_die("§8.10: short-read magic should terminate cleanly");
    }
    fclose(ftrunc);

    // (2) Unknown-type TLV — emit a valid magic + unknown type discriminator
    // + valid length, and verify the loader skips past it without claiming
    // recognition (returns 0 because the only TLV present was unknown).
    // This is the forward-compat property: future TLV types (e.g., Phase C
    // entrance-shuffle data) shipped to older binaries get cleanly skipped.
    FILE *funknown = tmpfile();
    if (funknown == NULL) selfcheck_die("§8.10: tmpfile() unknown-type");
    uint8 unk_hdr2[16];
    memcpy(unk_hdr2, kRandoSnapshotTail_Magic, 8);
    put_u32le_bytes(unk_hdr2 + 8, 0xDEADBEEFu);  // unknown type
    put_u32le_bytes(unk_hdr2 + 12, 12u);          // payload length
    fwrite(unk_hdr2, 1, 16, funknown);
    uint8 unk_payload[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
    fwrite(unk_payload, 1, sizeof(unk_payload), funknown);
    fseek(funknown, 0, SEEK_SET);
    int n_unknown = RandoSnapshotTail_Load(funknown);
    if (n_unknown != 0) {
      selfcheck_die("§8.10: unknown TLV type should be skipped (zero recognized)");
    }
    // Confirm we've consumed past the unknown TLV — the file cursor should
    // be at the unknown payload's end (16 + 12 = 28). If the loader had
    // truncated early or seek-overshot, the cursor would be elsewhere.
    long cursor_after_unknown = ftell(funknown);
    if (cursor_after_unknown != 28L) {
      selfcheck_die("§8.10: unknown-TLV skip should advance cursor exactly past payload");
    }
    fclose(funknown);

    // (3) Mixed-tail file: unknown TLV followed by recognized TAIL_RANDO_STATE.
    // Loader returns 1 (the recognized one), having skipped the unknown.
    // This is the same path exercised earlier in this self-check but reframed
    // as a §8.10 forward-compat assertion: the recognized TLV survives the
    // presence of unknown predecessors.
    static RandoPlacement mix_entries[2];
    static RandoPlacementTable mix_table;
    mix_entries[0].location_id = 0; mix_entries[0].item_id = 0x1111;
    mix_entries[1].location_id = 3; mix_entries[1].item_id = 0x2222;
    mix_table.entries = mix_entries; mix_table.count = 2;
    uint8 mix_settings[16];
    uint8 mix_share[32];
    for (int i = 0; i < 16; i++) mix_settings[i] = (uint8)i;
    for (int i = 0; i < 32; i++) mix_share[i] = (uint8)(0x80 + i);
    Placement_Install(&mix_table);
    Rando_SetSnapshotContext(0x0042, mix_settings, mix_share);

    FILE *fmix = tmpfile();
    if (fmix == NULL) selfcheck_die("§8.10: tmpfile() mixed");
    // Unknown TLV first.
    uint8 mix_hdr[16];
    memcpy(mix_hdr, kRandoSnapshotTail_Magic, 8);
    put_u32le_bytes(mix_hdr + 8, 0xBADC0DE5u);
    put_u32le_bytes(mix_hdr + 12, 4u);
    fwrite(mix_hdr, 1, 16, fmix);
    uint8 mix_pay[4] = { 9, 8, 7, 6 };
    fwrite(mix_pay, 1, 4, fmix);
    // Then the recognized TLV.
    if (!RandoSnapshotTail_Save(fmix)) selfcheck_die("§8.10: Save (mixed) returned false");
    // Clear and reload.
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fmix, 0, SEEK_SET);
    int n_mix = RandoSnapshotTail_Load(fmix);
    if (n_mix != 3) selfcheck_die("§8.10: mixed-tail should recognize 3 known TLVs (state + bitmap + discovery)");
    if (Rando_GetSnapshotGeneratorVersion() != 0x0042) {
      selfcheck_die("§8.10: mixed-tail should restore generator_version through the skip");
    }
    fclose(fmix);
  }

  // -------------------------------------------------------------------------
  // type-2 RandoSettings TLV round-trip.
  //
  // (A) With a settings sub-context installed, Save appends a type-2 TLV after
  //     the type-1 RandoState TLV. On Load the 4 process-static ownership bytes
  //     are restored UNCONDITIONALLY (cold OR within-session), and the type-2 is
  //     recognized (the settings-derived reconstruction inside
  //     Rando_SnapshotColdReplayRestore is gated on slot state + exercised by
  //     playtest, so it is not asserted here — but it runs asset-free with the
  //     Open all-zero blob below).
  // (B) With NO settings sub-context (a v1/no-blob slot), no type-2 is emitted,
  //     so the ownership bytes are left untouched.
  // -------------------------------------------------------------------------
  {
    uint8 open_canon[kSettingsCanonicalLen];
    memset(open_canon, 0, sizeof(open_canon));  // all-zero canonical == world_state Open + defaults

    static RandoPlacement m4_entries[2];
    static RandoPlacementTable m4_table;
    m4_entries[0].location_id = 0; m4_entries[0].item_id = 0x0123;
    m4_entries[1].location_id = 4; m4_entries[1].item_id = 0x0456;
    m4_table.entries = m4_entries; m4_table.count = 2;

    // (A) Round-trip with a settings sub-context.
    Placement_Install(&m4_table);
    Rando_SetSnapshotContext(0x0074, settings_hash, share_string);
    // audit-2 LOW-2: nonzero warp identity so the v2 payload tail is
    // exercised end-to-end (write offsets vs read offsets), not just with
    // zeros.
    Rando_SetSnapshotSettingsContext(open_canon, /*prize_attempt=*/3,
                                     /*ow_attempt=*/0x05,
                                     /*ow_digest24=*/0xABCDEFu);
    g_rando_mushroom_held      = 0x02;
    g_rando_flute_shovel_owned = 0x05;
    g_rando_boomerang_owned    = 0x03;
    g_rando_bow_owned          = 0x02;
    // add-enemy-souls — soul ownership rides the type-8 TLV.
    for (int i = 0; i < 12; i++) Souls_Flags()[i] = (uint8)(0x11 * (i + 1));
    // tracker-player-knowledge — discovery rides the type-10 TLV.
    {
      uint8 kc[8], ke[8];
      for (int i = 0; i < 8; i++) { kc[i] = (uint8)(0x21 + i); ke[i] = (uint8)(0x41 + i); }
      Rando_SetDiscoveryState(0x0A51, 0x1B, kc, ke);
    }

    FILE *fm = tmpfile();
    if (fm == NULL) selfcheck_die("tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fm)) selfcheck_die("Save returned false");

    // Wipe the process-static ownership bytes — only the type-2 load restores them.
    g_rando_mushroom_held = 0; g_rando_flute_shovel_owned = 0;
    g_rando_boomerang_owned = 0; g_rando_bow_owned = 0;
    Souls_ResetFlags();  // only the type-8 load restores souls
    Rando_SetDiscoveryState(0, 0, NULL, NULL);  // only type-10 restores discovery
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();

    fseek(fm, 0, SEEK_SET);
    int nm = RandoSnapshotTail_Load(fm);
    if (nm != 6)
      selfcheck_die("Load should recognize 6 TLVs (RandoState + PotRegistry + RandoSettings + CheckedBitmap + Discovery + Souls)");
    if (g_rando_mushroom_held != 0x02 || g_rando_flute_shovel_owned != 0x05 ||
        g_rando_boomerang_owned != 0x03 || g_rando_bow_owned != 0x02) {
      selfcheck_die("ownership bytes not restored from the type-2 TLV");
    }
    for (int i = 0; i < 12; i++)
      if (Souls_Flags()[i] != (uint8)(0x11 * (i + 1)))
        selfcheck_die("soul_flags not restored from the type-8 TLV");
    {
      uint16 kd; uint8 kw; uint8 kc[8], ke[8];
      Rando_GetDiscoveryState(&kd, &kw, kc, ke);
      if (kd != 0x0A51 || kw != 0x1B || kc[0] != 0x21 || kc[7] != 0x28 ||
          ke[0] != 0x41 || ke[7] != 0x48)
        selfcheck_die("discovery state not restored from the type-10 TLV");
      Rando_SetDiscoveryState(0, 0, NULL, NULL);  // restore startup state
    }
    // audit-2 LOW-2: byte-inspect the SAVED type-2 payload's v2 warp tail
    // (fmt==2; ow_attempt + ow_digest24 LE after the settings blob).
    {
      fseek(fm, 0, SEEK_END);
      long fm_len = ftell(fm);
      fseek(fm, 0, SEEK_SET);
      if (fm_len <= 0 || fm_len > 1 << 20)
        selfcheck_die("v2-tail probe: bad snapshot length");
      uint8 *fb = (uint8 *)malloc((size_t)fm_len);
      if (fb == NULL || fread(fb, 1, (size_t)fm_len, fm) != (size_t)fm_len)
        selfcheck_die("v2-tail probe: file read failed");
      bool tail_ok = false;
      for (long o = 0; o + 16 <= fm_len; o++) {
        if (memcmp(fb + o, kRandoSnapshotTail_Magic,
                   kRandoSnapshotTail_MagicLen) != 0)
          continue;
        uint32 ty = (uint32)fb[o + 8] | ((uint32)fb[o + 9] << 8) |
                    ((uint32)fb[o + 10] << 16) | ((uint32)fb[o + 11] << 24);
        if (ty != kRandoSnapshotTail_Type_RandoSettings) continue;
        uint32 ln = (uint32)fb[o + 12] | ((uint32)fb[o + 13] << 8) |
                    ((uint32)fb[o + 14] << 16) | ((uint32)fb[o + 15] << 24);
        const uint8 *pp = fb + o + 16;
        if (ln != 7u + kSettingsCanonicalLen + 4u || pp[0] != 2u)
          selfcheck_die("v2-tail probe: type-2 payload not v2-shaped");
        const uint8 *tl = pp + 7 + kSettingsCanonicalLen;
        if (tl[0] != 0x05 || tl[1] != 0xEF || tl[2] != 0xCD || tl[3] != 0xAB)
          selfcheck_die("v2-tail probe: warp identity bytes wrong in payload");
        tail_ok = true;
        break;
      }
      free(fb);
      if (!tail_ok) selfcheck_die("v2-tail probe: type-2 TLV not found");
      fseek(fm, 0, SEEK_SET);
    }
    fclose(fm);

    // the type-2 load reinstalled the settings sub-context, so a RE-SAVE
    // must AGAIN emit a type-2 TLV (a cold-replayed snapshot is self-perpetuating,
    // like type-1). Without that reinstall the re-save would be placement-only and
    // a later cold replay of it would lose world_state/Inverted. (Deterministic:
    // the reinstall is pre-gate, independent of whether the reconstruction fired.)
    g_rando_mushroom_held = 0x09;  // new live ownership to round-trip through re-save
    FILE *fr = tmpfile();
    if (fr == NULL) selfcheck_die("tmpfile() (re-save) returned NULL");
    if (!RandoSnapshotTail_Save(fr)) selfcheck_die("re-save returned false");
    g_rando_mushroom_held = 0;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fr, 0, SEEK_SET);
    int nr = RandoSnapshotTail_Load(fr);
    if (nr != 6)
      selfcheck_die("re-save of a cold-replayed snapshot must perpetuate type-6 + type-2 + type-3 + type-10 + type-8 TLVs");
    if (g_rando_mushroom_held != 0x09) selfcheck_die("re-saved ownership did not round-trip");
    fclose(fr);

    // (B) Suppression: no settings sub-context → no type-2 → ownership untouched.
    Placement_Install(&m4_table);
    Rando_SetSnapshotContext(0x0074, settings_hash, share_string);
    Rando_SetSnapshotSettingsContext(NULL, 0, 0, 0);  // v1/no-blob slot
    FILE *fb = tmpfile();
    if (fb == NULL) selfcheck_die("tmpfile() (B) returned NULL");
    if (!RandoSnapshotTail_Save(fb)) selfcheck_die("Save (B) returned false");
    g_rando_mushroom_held = 0x33;  // a (non-existent) type-2 would overwrite this
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fb, 0, SEEK_SET);
    int nb = RandoSnapshotTail_Load(fb);
    if (nb != 3) selfcheck_die("suppression — Load should recognize the type-1 + type-3 + type-10 TLVs (no type-2)");
    if (g_rando_mushroom_held != 0x33) {
      selfcheck_die("suppression — ownership must be untouched when no type-2 was emitted");
    }
    fclose(fb);

    // (C) A prior cold replay can install settings-derived process state and a
    // forced JP overlay. Replaying a no-type-2 snapshot must clear that cold
    // replay state instead of inheriting it.
    {
      uint32 saved_config_features0 = g_config.features0;
      uint32 saved_wanted_features0 = g_wanted_zelda_features;
      uint32 saved_enhanced_features0 = enhanced_features0;
      RandoSettings glitch_settings;
      Settings_SetDefaults(&glitch_settings);
      glitch_settings.logic = 1;  // OverworldGlitches forces JP glitches.
      g_config.features0 &= ~kFeatures0_RestoreJpGlitches;
      g_wanted_zelda_features &= ~kFeatures0_RestoreJpGlitches;
      enhanced_features0 &= ~kFeatures0_RestoreJpGlitches;
      Placement_Install(&m4_table);
      Rando_SnapshotColdReplayRestore(&glitch_settings, share_string, 0, 0, 0);
      if (!(g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
          !(enhanced_features0 & kFeatures0_RestoreJpGlitches))
        selfcheck_die("suppression — cold replay did not force JP glitches");

      Placement_Install(&m4_table);
      Rando_ClearSnapshotContext();
      Rando_SetSnapshotContext(0x0075, settings_hash, share_string);
      Rando_SetSnapshotSettingsContext(NULL, 0, 0, 0);
      FILE *fc = tmpfile();
      if (fc == NULL) selfcheck_die("tmpfile() (C) returned NULL");
      if (!RandoSnapshotTail_Save(fc)) selfcheck_die("Save (C) returned false");
      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      fseek(fc, 0, SEEK_SET);
      int nc = RandoSnapshotTail_Load(fc);
      if (nc != 3)
        selfcheck_die("suppression — cold replay clear should recognize type-1 + type-3 + type-10 only");
      if ((g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
          (enhanced_features0 & kFeatures0_RestoreJpGlitches))
        selfcheck_die("suppression — no-type-2 replay inherited forced JP overlay");
      fclose(fc);

      g_config.features0 = saved_config_features0;
      g_wanted_zelda_features = saved_wanted_features0;
      enhanced_features0 = saved_enhanced_features0;
      Rando_ClearSnapshotColdReplayRestore();
    }

    // (D) A v1/no-blob snapshot (type-1 + type-3 only) must also clear
    // settings from a genuine active slot, not only a prior cold replay. The
    // snapshot has no canonical settings TLV, so keeping another seed's active
    // shuffle/settings state would make the restored placement table evaluate
    // against stale process state.
    {
      uint8 saved_slot_active = g_rando_slot_active;
      uint8 saved_swordless = g_rando_swordless;
      uint32 saved_wanted_features1 = g_wanted_zelda_features1;
      uint32 saved_enhanced_features1 = enhanced_features1;
      static const uint8 zero_ids[0x100];
      bool synth_entrance_ids = false;
      if (g_asset_ptrs[126] == NULL) {
        g_asset_ptrs[126] = zero_ids;
        g_asset_sizes[126] = sizeof(zero_ids);
        synth_entrance_ids = true;
      }

      RandoSettings active_settings;
      Settings_SetDefaults(&active_settings);
      active_settings.shuffle_cave_entrances = 1;
      active_settings.shuffle_dungeon_entrances = 1;
      RandoSidecarSlot active_slot;
      memset(&active_slot, 0, sizeof(active_slot));
      active_slot.header.slot_kind = kSlotKind_Randomizer;
      active_slot.header.generator_version = (uint16)kGeneratorVersion;
      active_slot.header.settings_present = 1;
      active_slot.header.settings_ext_present = 1;
      active_slot.header.world_state = active_settings.world_state;
      active_slot.header.goal = active_settings.goal;
      active_slot.header.hints_setting = active_settings.hints;
      active_slot.header.entrance_axes =
          kEntranceAxis_ShuffleCaves | kEntranceAxis_ShuffleDungeons;
      static const RandoPlacement active_hint_fixture[] = {
        {LOC_Library, ITEM_Boots},
        {LOC_Sick_Kid, ITEM_Hookshot},
        {LOC_King_Zora, ITEM_Flippers},
        {LOC_Bumper_Cave, ITEM_MoonPearl},
        {LOC_Sahasrahla, ITEM_Bow},
        {LOC_Kakariko_Well_Top, ITEM_Hammer},
        {LOC_Mushroom, ITEM_Lamp},
        {LOC_Zora_s_Ledge, ITEM_BookOfMudora},
      };
      active_slot.placement_count =
          (uint16)(sizeof(active_hint_fixture) /
                   sizeof(active_hint_fixture[0]));
      memcpy(active_slot.placements, active_hint_fixture,
             sizeof(active_hint_fixture));
      active_slot.header.placement_table_size =
          (uint16)((LOC_Bumper_Cave + 1u) * 2u);
      Settings_CanonicalSerialize(&active_settings, active_slot.settings_canonical);
      uint8 active_settings_hash[32];
      Settings_ComputeHash(&active_settings, active_settings_hash);
      memcpy(active_slot.header.settings_hash, active_settings_hash,
             sizeof(active_slot.header.settings_hash));
      ShareString active_ss;
      memset(&active_ss, 0, sizeof(active_ss));
      active_ss.version = (uint8)kGeneratorVersion;
      memcpy(active_ss.settings_hash, active_settings_hash,
             sizeof(active_ss.settings_hash));
      active_ss.seed_u64 = 0x5EEDF00DCAFE1234ull;
      Share_PackBinary(&active_ss, active_slot.header.share_string);

      // First activate without v14 metadata solely to install the exact
      // generation inputs (placement + entrance/runtime overlays). Build one
      // caller-owned current plan from those accepted inputs, stamp its
      // identity into the synthetic sidecar, then reactivate from a clean slot
      // boundary. The second activation is the production Import+strict-
      // Rebuild path under test, not a direct Rando_GenerateHints shortcut.
      Rando_ActivateSidecarSlot(&active_slot);
      if (!Rando_HasActiveSettings())
        selfcheck_die("no-type-2: synthetic active slot did not recover settings");
      RandoHintPlan certified_hint_plan;
      if (!Rando_BuildHintPlan(
              &active_settings, Placement_GetActive(), NULL,
              kRandoHintPlanAlgorithmVersion, kRandoHintTextSchemaVersion,
              &certified_hint_plan))
        selfcheck_die("type-11 v14: generation-time plan build failed");
      RandoHintPersistedState certified_hint_identity;
      if (!Rando_HintPlanExportPersistedState(
              &certified_hint_plan, &certified_hint_identity))
        selfcheck_die("type-11 v14: plan identity export failed");
      active_slot.header.hint_state = certified_hint_identity;
      Rando_DeactivateSlot();
      Rando_ActivateSidecarSlot(&active_slot);
      if (!Rando_HasActiveSettings() ||
          Rando_GetHintPlanStatus() != kRandoHintPlanStatus_Ready)
        selfcheck_die("type-11 v14: certified sidecar activation failed");

      RandoHintPersistedState activated_hint_state;
      RandoSnapshotHintAssignmentView certified_assignments;
      if (!Rando_HintsExportPersistedState(&activated_hint_state) ||
          !hint_persisted_state_equal(&activated_hint_state,
                                      &certified_hint_identity) ||
          !capture_hint_assignment_view(&certified_assignments) ||
          certified_assignments.fact_count < 2)
        selfcheck_die("type-11 v14: activation identity/assignments mismatch");

      // Persist one committed paid discovery, then save. A second live
      // discovery made after the snapshot becomes the warm-replay poison:
      // replay must replace it with the snapshot mask rather than unioning
      // process-global state.
      if (!Rando_HintsPreparePaid(kRandoHintPaidSource_Storyteller) ||
          !Rando_HintsCommitPaid(kRandoHintPaidSource_Storyteller))
        selfcheck_die("type-11 v14: first paid discovery failed");
      RandoHintPersistedState snapshot_hint_state;
      if (!Rando_HintsExportPersistedState(&snapshot_hint_state) ||
          Rando_GetHintDiscoveredCount() == 0)
        selfcheck_die("type-11 v14: discovered state export failed");

      Rando_ReinstallActiveSlotLogicOverlays();
      uint64 active_entrance_digest = snapshot_selfcheck_fold_entrance_logic_state();
      Rando_ClearEntranceRegionOverrides();
      Rando_ClearEntranceEdgeOverrides();
      uint64 cleared_entrance_digest = snapshot_selfcheck_fold_entrance_logic_state();
      if (active_entrance_digest == cleared_entrance_digest)
        selfcheck_die("no-type-2: synthetic entrance slot did not install logic overrides");

      Rando_SetSnapshotContext((uint16)kGeneratorVersion,
                               active_slot.header.settings_hash,
                               active_slot.header.share_string);
      Rando_SetSnapshotSettingsContext(active_slot.settings_canonical, 0, 0, 0);
      FILE *fcur = tmpfile();
      if (fcur == NULL) selfcheck_die("type-2 active entrance: tmpfile() returned NULL");
      if (!RandoSnapshotTail_Save(fcur))
        selfcheck_die("type-2 active entrance: Save returned false");
      if (!Rando_HintsPreparePaid(kRandoHintPaidSource_FortuneLight) ||
          !Rando_HintsCommitPaid(kRandoHintPaidSource_FortuneLight))
        selfcheck_die("type-11 v14: warm-replay poison discovery failed");
      RandoHintPersistedState poisoned_hint_state;
      if (!Rando_HintsExportPersistedState(&poisoned_hint_state) ||
          hint_persisted_state_equal(&poisoned_hint_state,
                                     &snapshot_hint_state))
        selfcheck_die("type-11 v14: poison did not change live discovery");
      Rando_ClearEntranceRegionOverrides();
      Rando_ClearEntranceEdgeOverrides();
      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      g_rando_slot_active = 1;
      fseek(fcur, 0, SEEK_SET);
      int ncur = RandoSnapshotTail_Load(fcur);
      // 8 TLVs: activation installed the entrance context and a certified
      // current hint plan, so type 9, discovery type 10, and hint type 11
      // accompany the base records.
      if (ncur != 8)
        selfcheck_die("type-2 active entrance should parse state + bitmap + discovery + pot registry + settings + entrance layout + souls + hints");
      if (!Rando_HasActiveSettings())
        selfcheck_die("type-2 active entrance should restore settings");
      RandoHintPersistedState warm_hint_state;
      RandoSnapshotHintAssignmentView warm_assignments;
      if (!Rando_HintsExportPersistedState(&warm_hint_state) ||
          !hint_persisted_state_equal(&warm_hint_state,
                                      &snapshot_hint_state) ||
          !capture_hint_assignment_view(&warm_assignments) ||
          memcmp(&warm_assignments, &certified_assignments,
                 sizeof(warm_assignments)) != 0)
        selfcheck_die("type-11 v14: warm replay identity/assignment/discovery mismatch");
      Rando_ClearEntranceRegionOverrides();
      Rando_ClearEntranceEdgeOverrides();
      Rando_ReinstallActiveSlotLogicOverlays();
      if (snapshot_selfcheck_fold_entrance_logic_state() != active_entrance_digest)
        selfcheck_die("type-2 active entrance should preserve same-slot header");

      // Fresh-process equivalent: discard every process-global active-slot
      // input, retain only the raw snapshot's active bit, and replay the same
      // type-1/settings/layout/type-11 chain. The rebuilt plan must match the
      // generation-time identity and assignments byte-for-byte, including the
      // snapshot discovery mask.
      Rando_DeactivateSlot();
      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      g_rando_slot_active = 1;  // raw g_ram snapshot state
      g_wanted_zelda_features1 |= kFeatures1_RandomizerActive;
      enhanced_features1 |= kFeatures1_RandomizerActive;
      fseek(fcur, 0, SEEK_SET);
      int ncold_hint = RandoSnapshotTail_Load(fcur);
      if (ncold_hint != 8 || !Rando_IsActive() ||
          !Rando_HasActiveSettings())
        selfcheck_die("type-11 v14: cold replay did not restore active state");
      RandoHintPersistedState cold_hint_state;
      RandoSnapshotHintAssignmentView cold_assignments;
      if (!Rando_HintsExportPersistedState(&cold_hint_state) ||
          !hint_persisted_state_equal(&cold_hint_state,
                                      &snapshot_hint_state) ||
          !capture_hint_assignment_view(&cold_assignments) ||
          memcmp(&cold_assignments, &certified_assignments,
                 sizeof(cold_assignments)) != 0)
        selfcheck_die("type-11 v14: cold replay identity/assignment/discovery mismatch");
      fclose(fcur);

      // Composed entrance+warp COLD replay must land in activation-order
      // overlay state from the LOADER ALONE. The slot is deactivated before
      // the load so the type-2 handler takes the genuinely cold arm: settings
      // reconstruction + install_active_shuffles' digest-gated warp install,
      // then the later EntranceLayout TLV resets the shared edge stores, and
      // the loader's trailing reinstall reconverges them. Unlike the probe
      // above, NO manual reinstall runs between Load and the fold compare —
      // this asserts the state a player actually resumes into on a fresh
      // process. Skipped assetless (warp compute fails closed sans graph).
      if (kRandoOwGraphPresent) {
        RandoSettings warp_settings = active_settings;
        warp_settings.flute_shuffle = kFluteShuffle_Random;
        warp_settings.whirlpool_shuffle = 1;
        RandoSidecarSlot warp_slot = active_slot;
        Settings_CanonicalSerialize(&warp_settings, warp_slot.settings_canonical);
        OwWarpLayout wl;
        if (!OwWarp_Compute(&warp_settings, active_ss.seed_u64, 0, &wl))
          selfcheck_die("composed warp: OwWarp_Compute failed with graph present");
        warp_slot.header.ow_attempt = 0;
        warp_slot.header.ow_digest24 = OwWarp_Digest24(&wl) & 0xFFFFFFu;
        Rando_ActivateSidecarSlot(&warp_slot);
        if (!Rando_HasActiveSettings())
          selfcheck_die("composed warp: synthetic slot did not activate");
        uint64 composed_digest = snapshot_selfcheck_fold_entrance_logic_state();
        if (composed_digest == active_entrance_digest)
          selfcheck_die("composed warp: warp overlay did not move the edge fold");
        Rando_SetSnapshotContext((uint16)kGeneratorVersion,
                                 warp_slot.header.settings_hash,
                                 warp_slot.header.share_string);
        Rando_SetSnapshotSettingsContext(warp_slot.settings_canonical, 0,
                                         warp_slot.header.ow_attempt,
                                         warp_slot.header.ow_digest24);
        FILE *fwarp = tmpfile();
        if (fwarp == NULL) selfcheck_die("composed warp: tmpfile() returned NULL");
        if (!RandoSnapshotTail_Save(fwarp))
          selfcheck_die("composed warp: Save returned false");
        // Full teardown: an ACTIVE matching slot would route the type-2
        // handler down the warm arm and skip the cold reconstruction this
        // probe exists to exercise.
        Rando_DeactivateSlot();
        Placement_Install(NULL);
        Rando_ClearSnapshotContext();
        g_rando_slot_active = 1;
        fseek(fwarp, 0, SEEK_SET);
        (void)RandoSnapshotTail_Load(fwarp);
        if (!Rando_HasActiveSettings())
          selfcheck_die("composed warp: cold replay should restore settings");
        if (snapshot_selfcheck_fold_entrance_logic_state() != composed_digest)
          selfcheck_die("composed warp: loader post-state lost overlay edges "
                        "(cold replay stranded the warp overlay)");
        fclose(fwarp);
      }

      Rando_ActivateSidecarSlot(&active_slot);
      if (!Rando_HasActiveSettings())
        selfcheck_die("no-type-2: synthetic active slot did not recover settings after reactivation");

      Placement_Install(&m4_table);
      Rando_ClearSnapshotContext();
      Rando_SetSnapshotContext((uint16)kGeneratorVersion, settings_hash, share_string);
      Rando_SetSnapshotSettingsContext(NULL, 0, 0, 0);
      // This arm deliberately models a pre-type-2/pre-type-11 snapshot.
      Rando_ClearHints();
      FILE *fno = tmpfile();
      if (fno == NULL) selfcheck_die("no-type-2 active-settings: tmpfile() returned NULL");
      if (!RandoSnapshotTail_Save(fno))
        selfcheck_die("no-type-2 active-settings: Save returned false");
      // A later malformed type-1 in the same corrupt tail must not cancel the
      // stale-header cleanup pending from the accepted no-type-2 state.
      uint8 trailing_bad_state[16];
      memcpy(trailing_bad_state, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(trailing_bad_state + 8, kRandoSnapshotTail_Type_RandoState);
      put_u32le_bytes(trailing_bad_state + 12, 0);
      if (fwrite(trailing_bad_state, 1, sizeof(trailing_bad_state), fno) !=
          sizeof(trailing_bad_state))
        selfcheck_die("no-type-2 active-settings: trailing bad-state short write");

      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      g_rando_slot_active = 1;
      g_rando_swordless = 1;  // Simulate the RAM byte LoadSnesState restored.
      g_wanted_zelda_features1 |= kFeatures1_RandomizerActive;
      enhanced_features1 |= kFeatures1_RandomizerActive;
      fseek(fno, 0, SEEK_SET);
      int nno = RandoSnapshotTail_Load(fno);
      if (nno != 3)
        selfcheck_die("no-type-2 active-settings should parse type-1 + bitmap + discovery only");
      if (Rando_HasActiveSettings())
        selfcheck_die("no-type-2 active-settings should clear genuine active settings");
      if (Placement_GetActive() == NULL)
        selfcheck_die("no-type-2 active-settings should keep the restored placement table");
      if (!Rando_IsSwordlessActive())
        selfcheck_die("no-type-2 active-settings should preserve RAM swordless fallback");
      Rando_ClearEntranceRegionOverrides();
      Rando_ClearEntranceEdgeOverrides();
      Rando_ReinstallActiveSlotLogicOverlays();
      if (snapshot_selfcheck_fold_entrance_logic_state() != cleared_entrance_digest)
        selfcheck_die("no-type-2 active-settings should clear stale active header");
      fclose(fno);

      Rando_DeactivateSlot();
      g_rando_slot_active = saved_slot_active;
      g_rando_swordless = saved_swordless;
      g_wanted_zelda_features1 = saved_wanted_features1;
      enhanced_features1 = saved_enhanced_features1;
      Rando_ClearSnapshotContext();
      if (synth_entrance_ids) {
        g_asset_ptrs[126] = NULL;
        g_asset_sizes[126] = 0;
      }
    }

    // (E) Enemy-check snapshots depend on generated enemy location ids. Normal
    // slot activation refuses version drift for these seeds; snapshot replay
    // must fail closed too instead of accepting type-1 placement first and then
    // replaying stale settings-derived state.
    {
      uint8 saved_slot_active = g_rando_slot_active;
      uint32 saved_wanted_features1 = g_wanted_zelda_features1;
      uint32 saved_enhanced_features1 = enhanced_features1;

      RandoSettings enemy_settings;
      Settings_SetDefaults(&enemy_settings);
      enemy_settings.dungeon_small_keys_mode = kDungeonItemMode_Wild;
      enemy_settings.enemy_drop_checks = kEnemyDropChecks_Keys;
      uint8 enemy_canon[kSettingsCanonicalLen];
      Settings_CanonicalSerialize(&enemy_settings, enemy_canon);

      Placement_Install(&m4_table);
      Rando_ClearSnapshotContext();
      Rando_SetSnapshotContext((uint16)(kGeneratorVersion - 1u),
                               settings_hash, share_string);
      Rando_SetSnapshotSettingsContext(enemy_canon, /*prize_attempt=*/0, 0, 0);
      FILE *fd = tmpfile();
      if (fd == NULL) selfcheck_die("type-2 enemy drift: tmpfile() returned NULL");
      if (!RandoSnapshotTail_Save(fd)) selfcheck_die("type-2 enemy drift: Save returned false");

      g_rando_mushroom_held = 0;
      g_rando_flute_shovel_owned = 0;
      g_rando_boomerang_owned = 0;
      g_rando_bow_owned = 0;
      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      g_rando_slot_active = 1;
      g_wanted_zelda_features1 |= kFeatures1_RandomizerActive;
      enhanced_features1 |= kFeatures1_RandomizerActive;

      fseek(fd, 0, SEEK_SET);
      int nd = RandoSnapshotTail_Load(fd);
      if (nd != 5)
        selfcheck_die("type-2 enemy drift should parse state + bitmap + discovery + pot registry + settings");
      if (g_rando_slot_active || Placement_GetActive() != NULL || Rando_HasSnapshotContext())
        selfcheck_die("type-2 enemy drift should deactivate rando state");
      if (g_rando_mushroom_held != 0 || g_rando_flute_shovel_owned != 0 ||
          g_rando_boomerang_owned != 0 || g_rando_bow_owned != 0)
        selfcheck_die("type-2 enemy drift should not restore ownership bytes");
      fclose(fd);

      g_rando_slot_active = saved_slot_active;
      g_wanted_zelda_features1 = saved_wanted_features1;
      enhanced_features1 = saved_enhanced_features1;
      Rando_ClearSnapshotContext();
    }

    // (F) A type-2 settings TLV is meaningful only after THIS load accepted a
    // type-1 RandoState TLV. A malformed type-1 followed by a valid type-2 must
    // not replay settings or ownership against stale module-static context from
    // an earlier slot/load.
    {
      FILE *fstale = tmpfile();
      if (fstale == NULL) selfcheck_die("type-2 stale-context: tmpfile() returned NULL");
      uint8 bad_state[16];
      memcpy(bad_state, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(bad_state + 8, kRandoSnapshotTail_Type_RandoState);
      put_u32le_bytes(bad_state + 12, 0);  // malformed: shorter than 52-byte minimum
      if (fwrite(bad_state, 1, sizeof(bad_state), fstale) != sizeof(bad_state))
        selfcheck_die("type-2 stale-context: short bad-state write");

      uint8 sp[7 + kSettingsCanonicalLen];
      sp[0] = 1u;
      sp[1] = 0;
      sp[2] = 0x7E;
      sp[3] = 0x7D;
      sp[4] = 0x7C;
      sp[5] = 0x7B;
      sp[6] = (uint8)kSettingsCanonicalLen;
      memcpy(sp + 7, open_canon, kSettingsCanonicalLen);
      uint8 shdr[16];
      memcpy(shdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(shdr + 8, kRandoSnapshotTail_Type_RandoSettings);
      put_u32le_bytes(shdr + 12, (uint32)sizeof(sp));
      if (fwrite(shdr, 1, sizeof(shdr), fstale) != sizeof(shdr) ||
          fwrite(sp, 1, sizeof(sp), fstale) != sizeof(sp))
        selfcheck_die("type-2 stale-context: short settings write");

      Placement_Install(NULL);
      Rando_ClearSnapshotContext();
      Rando_SetSnapshotContext(0x5150, settings_hash, share_string);  // stale context
      g_rando_mushroom_held = 0;
      g_rando_flute_shovel_owned = 0;
      g_rando_boomerang_owned = 0;
      g_rando_bow_owned = 0;
      fseek(fstale, 0, SEEK_SET);
      int nstale = RandoSnapshotTail_Load(fstale);
      if (nstale != 1)
        selfcheck_die("type-2 stale-context should recognize only the settings TLV");
      if (g_rando_mushroom_held != 0 || g_rando_flute_shovel_owned != 0 ||
          g_rando_boomerang_owned != 0 || g_rando_bow_owned != 0)
        selfcheck_die("type-2 stale-context should not restore ownership bytes");
      if (Rando_HasActiveSettings())
        selfcheck_die("type-2 stale-context should not restore settings");
      fclose(fstale);
      Rando_ClearSnapshotContext();
    }

    // (G) A type-3 CheckedBitmap TLV is also scoped to an accepted type-1.
    // A malformed type-1 followed by a valid bitmap must consume the payload
    // without clearing or restoring the live checked-bitmap state.
    {
      FILE *fstalebitmap = tmpfile();
      if (fstalebitmap == NULL) selfcheck_die("type-3 stale-context: tmpfile() returned NULL");
      uint8 bad_state[16];
      memcpy(bad_state, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(bad_state + 8, kRandoSnapshotTail_Type_RandoState);
      put_u32le_bytes(bad_state + 12, 0);  // malformed: shorter than 52-byte minimum
      if (fwrite(bad_state, 1, sizeof(bad_state), fstalebitmap) != sizeof(bad_state))
        selfcheck_die("type-3 stale-context: short bad-state write");

      uint8 bitmap[kRandoCheckedBitmapBytes];
      memset(bitmap, 0xA5, sizeof(bitmap));
      uint8 chdr[16];
      memcpy(chdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
      put_u32le_bytes(chdr + 8, kRandoSnapshotTail_Type_CheckedBitmap);
      put_u32le_bytes(chdr + 12, (uint32)sizeof(bitmap));
      if (fwrite(chdr, 1, sizeof(chdr), fstalebitmap) != sizeof(chdr) ||
          fwrite(bitmap, 1, sizeof(bitmap), fstalebitmap) != sizeof(bitmap))
        selfcheck_die("type-3 stale-context: short bitmap write");

      memset(g_rando_checked_bitmap, 0x3C, kRandoCheckedBitmapBytes);
      fseek(fstalebitmap, 0, SEEK_SET);
      int nstalebitmap = RandoSnapshotTail_Load(fstalebitmap);
      if (nstalebitmap != 1)
        selfcheck_die("type-3 stale-context should recognize only the CheckedBitmap TLV");
      for (int i = 0; i < kRandoCheckedBitmapBytes; i++) {
        if (g_rando_checked_bitmap[i] != 0x3C)
          selfcheck_die("type-3 stale-context should not mutate the checked bitmap");
      }
      memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
      fclose(fstalebitmap);
    }

    g_rando_mushroom_held = 0; g_rando_flute_shovel_owned = 0;
    g_rando_boomerang_owned = 0; g_rando_bow_owned = 0;
  }

  // -------------------------------------------------------------------------
  // type-5 DoorLayout TLV round-trip.
  //
  // Door shuffle runtime redirects are process state. A snapshot must not
  // inherit whatever door graph happens to be active; it must clear stale doors
  // on type-1 and reinstall only the graph regenerated from its own
  // (settings, seed, door_attempt) after matching the saved digest.
  // -------------------------------------------------------------------------
  {
    uint8 saved_slot_active = g_rando_slot_active;
    uint32 saved_wanted_features1 = g_wanted_zelda_features1;
    uint32 saved_enhanced_features1 = enhanced_features1;

    RandoSettings door_settings;
    Settings_SetDefaults(&door_settings);
    door_settings.door_shuffle = kDoorShuffle_Basic;
    door_settings.pot_shuffle = kPotShuffle_Keys;
    uint8 door_canon[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&door_settings, door_canon);

    ShareString door_ss;
    memset(&door_ss, 0, sizeof(door_ss));
    door_ss.version = (uint8)kGeneratorVersion;
    for (int i = 0; i < 16; i++) door_ss.settings_hash[i] = (uint8)(0xD0 + i);
    door_ss.seed_u64 = 0xD00D5007C0FFEE11ull;
    uint8 door_share[32];
    memset(door_share, 0, sizeof(door_share));
    Share_PackBinary(&door_ss, door_share);

    static DoorShuffleLayout layout;
    uint8 door_attempt = 0;
    uint32 door_digest24 = 0;
    bool got_layout = false;
    for (uint32 a = 0; a < 32; a++) {
      if (DoorShuffle_Generate(door_ss.seed_u64, a, kDoorShuffle_MvpDungeonMask,
                               door_settings.pot_shuffle, 0,
                               kEnemyDropChecks_Off, &layout)) {
        door_attempt = (uint8)a;
        door_digest24 = DoorShuffle_LayoutDigest(&layout) & 0xFFFFFFu;
        got_layout = true;
        break;
      }
    }
    if (!got_layout) selfcheck_die("type-5: could not generate a door layout");

    static RandoPlacement door_entries[2];
    static RandoPlacementTable door_table;
    door_entries[0].location_id = 11; door_entries[0].item_id = 0x0D01;
    door_entries[1].location_id = 12; door_entries[1].item_id = 0x0D02;
    door_table.entries = door_entries; door_table.count = 2;

    Placement_Install(&door_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00D5, door_ss.settings_hash, door_share);
    Rando_SetSnapshotSettingsContext(door_canon, /*prize_attempt=*/0, 0, 0);
    Rando_SetSnapshotDoorContext(door_attempt, door_digest24, true);

    FILE *fd = tmpfile();
    if (fd == NULL) selfcheck_die("type-5: tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fd)) selfcheck_die("type-5: Save returned false");
    Rando_ClearSnapshotDoorReplayRestore();
    if (DoorRt_Installed())
      selfcheck_die("type-5: clear should remove installed door graph");
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    enhanced_features1 |= kFeatures1_DoorShuffleActive;  // restored g_ram claims door shuffle

    fseek(fd, 0, SEEK_SET);
    int nd = RandoSnapshotTail_Load(fd);
    if (nd != 7)
      selfcheck_die("type-5: expected RandoState + CheckedBitmap + Discovery + PotRegistry + RandoSettings + DoorLayout + Souls");
    if (!DoorRt_Installed())
      selfcheck_die("type-5: door graph was not restored");
    if (!(enhanced_features1 & kFeatures1_DoorShuffleActive))
      selfcheck_die("type-5: door feature bit was not restored");
    fclose(fd);

    FILE *frd = tmpfile();
    if (frd == NULL) selfcheck_die("type-5: re-save tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(frd)) selfcheck_die("type-5: re-save returned false");
    Rando_ClearSnapshotDoorReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    enhanced_features1 |= kFeatures1_DoorShuffleActive;  // restored g_ram claims door shuffle
    fseek(frd, 0, SEEK_SET);
    int nrd = RandoSnapshotTail_Load(frd);
    if (nrd != 7)
      selfcheck_die("type-5: replayed snapshot re-save must perpetuate DoorLayout");
    if (!DoorRt_Installed())
      selfcheck_die("type-5: re-saved door graph was not restored");
    fclose(frd);

    // Missing type-5: a door-shuffle RAM snapshot with placement/settings but
    // no DoorLayout TLV must deactivate instead of running under vanilla doors.
    Placement_Install(&door_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00D7, door_ss.settings_hash, door_share);
    Rando_SetSnapshotSettingsContext(door_canon, /*prize_attempt=*/0, 0, 0);
    FILE *fmissing = tmpfile();
    if (fmissing == NULL) selfcheck_die("type-5: missing tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fmissing)) selfcheck_die("type-5: missing Save returned false");
    Rando_ClearSnapshotDoorReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    g_rando_slot_active = 1;
    enhanced_features1 |= kFeatures1_DoorShuffleActive;
    fseek(fmissing, 0, SEEK_SET);
    int nmissing = RandoSnapshotTail_Load(fmissing);
    if (nmissing != 6)
      selfcheck_die("type-5: missing DoorLayout should parse RandoState + CheckedBitmap + Discovery + PotRegistry + RandoSettings + Souls");
    if (g_rando_slot_active || Placement_GetActive() != NULL || DoorRt_Installed())
      selfcheck_die("type-5: missing DoorLayout should deactivate rando state");
    fclose(fmissing);

    // Drift fail-closed: a corrupted saved digest must not leave the previously
    // installed graph active or keep the rando slot marked active.
    Placement_Install(&door_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00D6, door_ss.settings_hash, door_share);
    Rando_SetSnapshotSettingsContext(door_canon, /*prize_attempt=*/0, 0, 0);
    Rando_SetSnapshotDoorContext(door_attempt, door_digest24 ^ 1u, true);
    FILE *fbad = tmpfile();
    if (fbad == NULL) selfcheck_die("type-5: bad-digest tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fbad)) selfcheck_die("type-5: bad-digest Save returned false");
    fseek(fbad, 0, SEEK_SET);
    int nbad = RandoSnapshotTail_Load(fbad);
    if (nbad != 7)
      selfcheck_die("type-5: bad-digest snapshot should still parse all known TLVs");
    if (DoorRt_Installed())
      selfcheck_die("type-5: bad digest inherited or installed a door graph");
    if (g_rando_slot_active)
      selfcheck_die("type-5: bad digest should deactivate the rando slot");
    fclose(fbad);

    // Stale-context fail-closed: a malformed/skipped type-1 followed by a
    // valid type-5 must not use g_ctx/g_ctx_settings_canonical left over from
    // an earlier load to reinstall a door graph.
    Rando_ClearSnapshotDoorReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext((uint16)kGeneratorVersion, door_ss.settings_hash, door_share);
    Rando_SetSnapshotSettingsContext(door_canon, /*prize_attempt=*/0, 0, 0);
    FILE *fstaledoor = tmpfile();
    if (fstaledoor == NULL) selfcheck_die("type-5: stale-context tmpfile() returned NULL");
    uint8 bad_state[16];
    memcpy(bad_state, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(bad_state + 8, kRandoSnapshotTail_Type_RandoState);
    put_u32le_bytes(bad_state + 12, 0);  // malformed: shorter than 52-byte minimum
    if (fwrite(bad_state, 1, sizeof(bad_state), fstaledoor) != sizeof(bad_state))
      selfcheck_die("type-5: stale-context bad-state short write");
    uint8 dp[5];
    dp[0] = 1u;
    dp[1] = door_attempt;
    dp[2] = (uint8)(door_digest24 & 0xff);
    dp[3] = (uint8)((door_digest24 >> 8) & 0xff);
    dp[4] = (uint8)((door_digest24 >> 16) & 0xff);
    uint8 dhdr[16];
    memcpy(dhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(dhdr + 8, kRandoSnapshotTail_Type_DoorLayout);
    put_u32le_bytes(dhdr + 12, (uint32)sizeof(dp));
    if (fwrite(dhdr, 1, sizeof(dhdr), fstaledoor) != sizeof(dhdr) ||
        fwrite(dp, 1, sizeof(dp), fstaledoor) != sizeof(dp))
      selfcheck_die("type-5: stale-context door short write");
    fseek(fstaledoor, 0, SEEK_SET);
    int nstaledoor = RandoSnapshotTail_Load(fstaledoor);
    if (nstaledoor != 1)
      selfcheck_die("type-5: stale-context should recognize only DoorLayout");
    if (DoorRt_Installed())
      selfcheck_die("type-5: stale context should not install a door graph");
    fclose(fstaledoor);

    Rando_ClearSnapshotDoorReplayRestore();
    Rando_ClearSnapshotColdReplayRestore();
    Rando_ClearSnapshotContext();
    g_rando_slot_active = saved_slot_active;
    g_wanted_zelda_features1 = saved_wanted_features1;
    enhanced_features1 = saved_enhanced_features1;
  }

  // -------------------------------------------------------------------------
  // type-7 ChainLayout TLV round-trip.
  //
  // Dungeon-chain runtime redirects are process state, like door shuffle, but
  // they are gated by canonical settings rather than a dedicated RAM feature
  // bit. A chain snapshot must therefore fail closed if type-2 says chains are
  // active and type-7 is missing or fails the saved digest.
  // -------------------------------------------------------------------------
  {
    static uint8 synth_door_ids[256];
    static uint16 synth_u16[kChainBossEntranceLimit];
    static uint16 synth_rooms[kChainBossEntranceLimit];
    static uint8 synth_u8[kChainBossEntranceLimit];
    static uint8 synth_relative[kChainBossEntranceLimit * 8];
    static int8 synth_i8[kChainBossEntranceLimit];
    static int8 synth_palace[kChainBossEntranceLimit];
    static uint8 synth_music[kChainBossEntranceLimit];
    static const uint8 kSynthAssetIds[] = {
      11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 126,
    };
    const uint8 *saved_ptrs[sizeof(kSynthAssetIds)];
    uint32 saved_sizes[sizeof(kSynthAssetIds)];
    bool synth_assets = kOverworld_Entrance_Id == NULL ||
                        kOverworld_Entrance_Id_SIZE == 0 ||
                        !Chains_SyntheticEntrancesAvailable();
    uint8 saved_slot_active = g_rando_slot_active;
    uint32 saved_wanted_features1 = g_wanted_zelda_features1;
    uint32 saved_enhanced_features1 = enhanced_features1;
    if (synth_assets) {
      for (uint32 i = 0; i < (uint32)sizeof(kSynthAssetIds); i++) {
        uint8 id = kSynthAssetIds[i];
        saved_ptrs[i] = g_asset_ptrs[id];
        saved_sizes[i] = g_asset_sizes[id];
      }
      memset(synth_door_ids, 0, sizeof(synth_door_ids));
      memset(synth_u16, 0, sizeof(synth_u16));
      memset(synth_rooms, 0, sizeof(synth_rooms));
      memset(synth_u8, 0, sizeof(synth_u8));
      memset(synth_relative, 0, sizeof(synth_relative));
      memset(synth_i8, 0, sizeof(synth_i8));
      memset(synth_palace, 0, sizeof(synth_palace));
      memset(synth_music, 0, sizeof(synth_music));
      for (uint8 i = 0; i < kChainsPoolCount; i++) {
        const ChainBossEntranceCheck *row = &kChainBossEntranceChecks[i];
        synth_door_ids[16 + i] = row->main_entrance_id;
      }
      for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
        const ChainBossEntranceCheck *row = &kChainBossEntranceChecks[i];
        synth_rooms[row->entrance_id] = row->room;
        synth_palace[row->entrance_id] = row->palace;
        synth_music[row->entrance_id] = row->music;
      }
      g_asset_ptrs[11] = (const uint8 *)synth_rooms;
      g_asset_sizes[11] = sizeof(synth_rooms);
      g_asset_ptrs[12] = synth_relative;
      g_asset_sizes[12] = sizeof(synth_relative);
      g_asset_ptrs[13] = (const uint8 *)synth_u16;
      g_asset_sizes[13] = sizeof(synth_u16);
      g_asset_ptrs[14] = (const uint8 *)synth_u16;
      g_asset_sizes[14] = sizeof(synth_u16);
      g_asset_ptrs[15] = (const uint8 *)synth_u16;
      g_asset_sizes[15] = sizeof(synth_u16);
      g_asset_ptrs[16] = (const uint8 *)synth_u16;
      g_asset_sizes[16] = sizeof(synth_u16);
      g_asset_ptrs[17] = (const uint8 *)synth_u16;
      g_asset_sizes[17] = sizeof(synth_u16);
      g_asset_ptrs[18] = (const uint8 *)synth_u16;
      g_asset_sizes[18] = sizeof(synth_u16);
      g_asset_ptrs[19] = synth_u8;
      g_asset_sizes[19] = sizeof(synth_u8);
      g_asset_ptrs[20] = (const uint8 *)synth_i8;
      g_asset_sizes[20] = sizeof(synth_i8);
      g_asset_ptrs[21] = (const uint8 *)synth_palace;
      g_asset_sizes[21] = sizeof(synth_palace);
      g_asset_ptrs[22] = synth_u8;
      g_asset_sizes[22] = sizeof(synth_u8);
      g_asset_ptrs[23] = synth_u8;
      g_asset_sizes[23] = sizeof(synth_u8);
      g_asset_ptrs[24] = synth_u8;
      g_asset_sizes[24] = sizeof(synth_u8);
      g_asset_ptrs[25] = synth_u8;
      g_asset_sizes[25] = sizeof(synth_u8);
      g_asset_ptrs[26] = (const uint8 *)synth_u16;
      g_asset_sizes[26] = sizeof(synth_u16);
      g_asset_ptrs[27] = synth_music;
      g_asset_sizes[27] = sizeof(synth_music);
      g_asset_ptrs[126] = synth_door_ids;
      g_asset_sizes[126] = sizeof(synth_door_ids);
    }
    if (kOverworld_Entrance_Id == NULL || kOverworld_Entrance_Id_SIZE == 0 ||
        !Chains_SyntheticEntrancesAvailable())
      selfcheck_die("type-7: asset fixture unavailable");

    RandoSettings chain_settings;
    Settings_SetDefaults(&chain_settings);
    chain_settings.dungeon_chains = 1;
    uint8 chain_canon[kSettingsCanonicalLen];
    Settings_CanonicalSerialize(&chain_settings, chain_canon);

    ShareString chain_ss;
    memset(&chain_ss, 0, sizeof(chain_ss));
    chain_ss.version = (uint8)kGeneratorVersion;
    for (int i = 0; i < 16; i++) chain_ss.settings_hash[i] = (uint8)(0xC0 + i);
    chain_ss.seed_u64 = 0xC0A1C0A1D00D5007ull;
    uint8 chain_share[32];
    memset(chain_share, 0, sizeof(chain_share));
    Share_PackBinary(&chain_ss, chain_share);

    DungeonChainsLayout chain_layout;
    uint8 chains_attempt = 3;
    if (!Chains_Compute(chain_ss.seed_u64, chains_attempt, &chain_layout))
      selfcheck_die("type-7: could not generate a chain layout");
    uint32 chains_digest24 = Chains_LayoutDigest(&chain_layout) & 0xFFFFFFu;

    uint16 chain_lx = 0xFFFFu;
    uint8 source_entrance = kChainBossEntranceChecks[0].main_entrance_id;
    for (uint32 lx = 0; lx < kOverworld_Entrance_Id_SIZE; lx++) {
      if (((const uint8 *)kOverworld_Entrance_Id)[lx] == source_entrance) {
        chain_lx = (uint16)lx;
        break;
      }
    }
    if (chain_lx == 0xFFFFu)
      selfcheck_die("type-7: source chain door missing from vanilla table");

    static RandoPlacement chain_entries[2];
    static RandoPlacementTable chain_table;
    chain_entries[0].location_id = 21; chain_entries[0].item_id = 0x0C01;
    chain_entries[1].location_id = 22; chain_entries[1].item_id = 0x0C02;
    chain_table.entries = chain_entries; chain_table.count = 2;

    Placement_Install(&chain_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00C7, chain_ss.settings_hash, chain_share);
    Rando_SetSnapshotSettingsContext(chain_canon, /*prize_attempt=*/0, 0, 0);
    Rando_SetSnapshotChainsContext(chains_attempt, chains_digest24, true);
    if (!Chains_RuntimeInstallLayout(&chain_layout))
      selfcheck_die("type-7: could not install chain layout for session save");
    ChainsRuntimeSession saved_chain_session;
    memset(&saved_chain_session, 0, sizeof(saved_chain_session));
    saved_chain_session.origin_active = true;
    saved_chain_session.terminal_active = true;
    saved_chain_session.origin_exit_room = kChainBossEntranceChecks[0].main_exit_room;
    saved_chain_session.terminal_dungeon = kRandoDungeon_TowerOfHera;
    if (!Chains_RuntimeRestoreSession(&saved_chain_session))
      selfcheck_die("type-7: could not arm chain session for save");

    FILE *fc = tmpfile();
    if (fc == NULL) selfcheck_die("type-7: tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fc)) selfcheck_die("type-7: Save returned false");
    Rando_ClearSnapshotChainsReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    g_rando_slot_active = 1;
    enhanced_features1 |= kFeatures1_RandomizerActive;

    fseek(fc, 0, SEEK_SET);
    int nc = RandoSnapshotTail_Load(fc);
    if (nc != 7)
      selfcheck_die("type-7: expected RandoState + CheckedBitmap + Discovery + PotRegistry + RandoSettings + ChainLayout + Souls");
    ChainsRuntimeSession restored_chain_session;
    if (!Chains_RuntimeGetSession(&restored_chain_session))
      selfcheck_die("type-7: chain runtime session was not restored");
    if (!restored_chain_session.origin_active ||
        !restored_chain_session.terminal_active ||
        restored_chain_session.origin_exit_room != kChainBossEntranceChecks[0].main_exit_room ||
        restored_chain_session.terminal_dungeon != kRandoDungeon_TowerOfHera)
      selfcheck_die("type-7: restored chain session did not match");
    if (Chains_RuntimeConsumeMainExitOrigin(kChainBossEntranceChecks[0].main_exit_room) !=
        kChainBossEntranceChecks[0].main_exit_room)
      selfcheck_die("type-7: restored chain origin did not consume");
    if (!Chains_RuntimeRecordDoorEntry(chain_lx))
      selfcheck_die("type-7: chain runtime was not restored");
    (void)Chains_RuntimeConsumeMainExitOrigin(kChainBossEntranceChecks[0].main_exit_room);
    fclose(fc);

    FILE *frc = tmpfile();
    if (frc == NULL) selfcheck_die("type-7: re-save tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(frc)) selfcheck_die("type-7: re-save returned false");
    Rando_ClearSnapshotChainsReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    g_rando_slot_active = 1;
    enhanced_features1 |= kFeatures1_RandomizerActive;
    fseek(frc, 0, SEEK_SET);
    int nrc = RandoSnapshotTail_Load(frc);
    if (nrc != 7)
      selfcheck_die("type-7: replayed snapshot re-save must perpetuate ChainLayout");
    if (!Chains_RuntimeRecordDoorEntry(chain_lx))
      selfcheck_die("type-7: re-saved chain runtime was not restored");
    (void)Chains_RuntimeConsumeMainExitOrigin(kChainBossEntranceChecks[0].main_exit_room);
    fclose(frc);

    Placement_Install(&chain_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00C8, chain_ss.settings_hash, chain_share);
    Rando_SetSnapshotSettingsContext(chain_canon, /*prize_attempt=*/0, 0, 0);
    FILE *fmissing = tmpfile();
    if (fmissing == NULL) selfcheck_die("type-7: missing tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fmissing)) selfcheck_die("type-7: missing Save returned false");
    Rando_ClearSnapshotChainsReplayRestore();
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    g_rando_slot_active = 1;
    enhanced_features1 |= kFeatures1_RandomizerActive;
    fseek(fmissing, 0, SEEK_SET);
    int nmissing = RandoSnapshotTail_Load(fmissing);
    if (nmissing != 6)
      selfcheck_die("type-7: missing ChainLayout should parse RandoState + CheckedBitmap + Discovery + PotRegistry + RandoSettings + Souls");
    if (g_rando_slot_active || Placement_GetActive() != NULL ||
        Chains_RuntimeRecordDoorEntry(chain_lx))
      selfcheck_die("type-7: missing ChainLayout should deactivate rando state");
    fclose(fmissing);

    Placement_Install(&chain_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00C9, chain_ss.settings_hash, chain_share);
    Rando_SetSnapshotSettingsContext(chain_canon, /*prize_attempt=*/0, 0, 0);
    Rando_SetSnapshotChainsContext(chains_attempt, chains_digest24 ^ 1u, true);
    FILE *fbad = tmpfile();
    if (fbad == NULL) selfcheck_die("type-7: bad-digest tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fbad)) selfcheck_die("type-7: bad-digest Save returned false");
    fseek(fbad, 0, SEEK_SET);
    int nbad = RandoSnapshotTail_Load(fbad);
    if (nbad != 7)
      selfcheck_die("type-7: bad-digest snapshot should still parse all known TLVs");
    if (Chains_RuntimeRecordDoorEntry(chain_lx))
      selfcheck_die("type-7: bad digest inherited or installed a chain graph");
    if (g_rando_slot_active)
      selfcheck_die("type-7: bad digest should deactivate the rando slot");
    fclose(fbad);

    Rando_ClearSnapshotChainsReplayRestore();
    Rando_ClearSnapshotColdReplayRestore();
    Rando_ClearSnapshotContext();
    g_rando_slot_active = saved_slot_active;
    g_wanted_zelda_features1 = saved_wanted_features1;
    enhanced_features1 = saved_enhanced_features1;
    if (synth_assets) {
      for (uint32 i = 0; i < (uint32)sizeof(kSynthAssetIds); i++) {
        uint8 id = kSynthAssetIds[i];
        g_asset_ptrs[id] = saved_ptrs[i];
        g_asset_sizes[id] = saved_sizes[i];
      }
    }
  }

  // -------------------------------------------------------------------------
  // type-4 RecommendedFeatures TLV round-trip.
  //
  // This carries the per-slot Seed QoL features0 snapshot for replay. It
  // must mask out non-Seed-QoL bits, preserve live non-slot bits, and reinstall
  // its context so a replayed snapshot re-save perpetuates the TLV.
  // -------------------------------------------------------------------------
  {
    uint32 saved_config_features0 = g_config.features0;
    uint32 saved_wanted_features0 = g_wanted_zelda_features;
    uint32 saved_enhanced_features0 = enhanced_features0;
    uint32 saved_wanted_features1 = g_wanted_zelda_features1;
    uint32 saved_enhanced_features1 = enhanced_features1;
    uint8 saved_slot_active = g_rando_slot_active;

    static RandoPlacement f4_entries[2];
    static RandoPlacementTable f4_table;
    f4_entries[0].location_id = 2; f4_entries[0].item_id = 0x0A0A;
    f4_entries[1].location_id = 6; f4_entries[1].item_id = 0x0B0B;
    f4_table.entries = f4_entries; f4_table.count = 2;

    Placement_Install(&f4_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00F5, settings_hash, share_string);
    FILE *fold4 = tmpfile();
    if (fold4 == NULL) selfcheck_die("type-4: no-feature tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fold4))
      selfcheck_die("type-4: no-feature Save returned false");

    Placement_Install(&f4_table);
    Rando_ClearSnapshotContext();
    Rando_SetSnapshotContext(0x00F4, settings_hash, share_string);
    Rando_SetSnapshotRecommendedFeaturesContext(
        kFeatures0_RestoreJpGlitches | kFeatures0_WidescreenVisualFixes,
        true);

    FILE *ff = tmpfile();
    if (ff == NULL) selfcheck_die("type-4: tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(ff)) selfcheck_die("type-4: Save returned false");

    g_config.features0 = kFeatures0_ExtendScreen64;
    g_wanted_zelda_features = kFeatures0_ExtendScreen64;
    enhanced_features0 = kFeatures0_ExtendScreen64;
    g_rando_slot_active = 1;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();

    fseek(ff, 0, SEEK_SET);
    int nf = RandoSnapshotTail_Load(ff);
    if (nf != 4) selfcheck_die("type-4: expected RandoState + CheckedBitmap + Discovery + RecommendedFeatures");
    if (!(g_config.features0 & kFeatures0_RestoreJpGlitches) ||
        !(g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
        !(enhanced_features0 & kFeatures0_RestoreJpGlitches))
      selfcheck_die("type-4: RestoreJpGlitches not restored");
    if (!(g_config.features0 & kFeatures0_ExtendScreen64) ||
        !(g_wanted_zelda_features & kFeatures0_ExtendScreen64) ||
        !(enhanced_features0 & kFeatures0_ExtendScreen64))
      selfcheck_die("type-4: non-slot live feature not preserved");
    if ((g_config.features0 & kFeatures0_WidescreenVisualFixes) ||
        (g_wanted_zelda_features & kFeatures0_WidescreenVisualFixes) ||
        (enhanced_features0 & kFeatures0_WidescreenVisualFixes))
      selfcheck_die("type-4: non-Seed-QoL snapshot bit was applied");

    g_config.features0 = kFeatures0_ExtendScreen64;
    g_wanted_zelda_features = kFeatures0_ExtendScreen64;
    enhanced_features0 = kFeatures0_ExtendScreen64;
    g_rando_slot_active = 0;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(ff, 0, SEEK_SET);
    int ninactive = RandoSnapshotTail_Load(ff);
    if (ninactive != 4)
      selfcheck_die("type-4: inactive replay should still parse RecommendedFeatures");
    if ((g_config.features0 & kFeatures0_RandoSeedQolMask) ||
        (g_wanted_zelda_features & kFeatures0_RandoSeedQolMask) ||
        (enhanced_features0 & kFeatures0_RandoSeedQolMask))
      selfcheck_die("type-4: inactive replay applied RecommendedFeatures");
    fclose(ff);

    FILE *frf = tmpfile();
    if (frf == NULL) selfcheck_die("type-4: re-save tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(frf)) selfcheck_die("type-4: re-save returned false");
    g_config.features0 = kFeatures0_ExtendScreen64;
    g_wanted_zelda_features = kFeatures0_ExtendScreen64;
    enhanced_features0 = kFeatures0_ExtendScreen64;
    g_rando_slot_active = 1;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(frf, 0, SEEK_SET);
    int nfr = RandoSnapshotTail_Load(frf);
    if (nfr != 4) selfcheck_die("type-4: re-save must perpetuate RecommendedFeatures");
    if (!(g_config.features0 & kFeatures0_RestoreJpGlitches))
      selfcheck_die("type-4: re-saved features did not round-trip");
    fclose(frf);

    fseek(fold4, 0, SEEK_SET);
    int nof = RandoSnapshotTail_Load(fold4);
    if (nof != 3)
      selfcheck_die("type-4: no-feature snapshot should recognize type-1 + type-3 + type-10 only");
    FILE *fleak = tmpfile();
    if (fleak == NULL) selfcheck_die("type-4: stale-context tmpfile() returned NULL");
    if (!RandoSnapshotTail_Save(fleak))
      selfcheck_die("type-4: stale-context re-save returned false");
    g_config.features0 = kFeatures0_ExtendScreen64;
    g_wanted_zelda_features = kFeatures0_ExtendScreen64;
    enhanced_features0 = kFeatures0_ExtendScreen64;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fleak, 0, SEEK_SET);
    int nleak = RandoSnapshotTail_Load(fleak);
    if (nleak != 3)
      selfcheck_die("type-4: no-feature snapshot re-save inherited stale TLV");
    if ((g_config.features0 & kFeatures0_RestoreJpGlitches) ||
        (g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
        (enhanced_features0 & kFeatures0_RestoreJpGlitches))
      selfcheck_die("type-4: stale recommended features applied after no-feature replay");
    fclose(fleak);

    FILE *fstalefeatures = tmpfile();
    if (fstalefeatures == NULL) selfcheck_die("type-4: malformed-prefix tmpfile() returned NULL");
    uint8 bad_state[16];
    memcpy(bad_state, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(bad_state + 8, kRandoSnapshotTail_Type_RandoState);
    put_u32le_bytes(bad_state + 12, 0);  // malformed: shorter than 52-byte minimum
    if (fwrite(bad_state, 1, sizeof(bad_state), fstalefeatures) != sizeof(bad_state))
      selfcheck_die("type-4: malformed-prefix bad-state short write");
    uint8 fp[5];
    fp[0] = 1u;
    put_u32le_bytes(fp + 1, kFeatures0_RestoreJpGlitches);
    uint8 fhdr[16];
    memcpy(fhdr, kRandoSnapshotTail_Magic, kRandoSnapshotTail_MagicLen);
    put_u32le_bytes(fhdr + 8, kRandoSnapshotTail_Type_RecommendedFeatures);
    put_u32le_bytes(fhdr + 12, (uint32)sizeof(fp));
    if (fwrite(fhdr, 1, sizeof(fhdr), fstalefeatures) != sizeof(fhdr) ||
        fwrite(fp, 1, sizeof(fp), fstalefeatures) != sizeof(fp))
      selfcheck_die("type-4: malformed-prefix feature short write");
    g_config.features0 = kFeatures0_ExtendScreen64;
    g_wanted_zelda_features = kFeatures0_ExtendScreen64;
    enhanced_features0 = kFeatures0_ExtendScreen64;
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fstalefeatures, 0, SEEK_SET);
    int nstalefeatures = RandoSnapshotTail_Load(fstalefeatures);
    if (nstalefeatures != 1)
      selfcheck_die("type-4: malformed-prefix should recognize only RecommendedFeatures");
    if ((g_config.features0 & kFeatures0_RestoreJpGlitches) ||
        (g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
        (enhanced_features0 & kFeatures0_RestoreJpGlitches))
      selfcheck_die("type-4: malformed-prefix should not apply recommended features");
    fclose(fstalefeatures);

    fclose(fold4);

    Rando_DeactivateSlot();
    g_config.features0 = saved_config_features0;
    g_wanted_zelda_features = saved_wanted_features0;
    enhanced_features0 = saved_enhanced_features0;
    g_wanted_zelda_features1 = saved_wanted_features1;
    enhanced_features1 = saved_enhanced_features1;
    g_rando_slot_active = saved_slot_active;
    Rando_ClearSnapshotContext();
  }

  // -------------------------------------------------------------------------
  // Absent-type-3 (older-snapshot) bitmap clean-restore.
  // randomizer-save spec scenario "Older snapshot without the TLV restores
  // cleanly": a snapshot written before the type-3 CheckedBitmap TLV existed
  // (type-1 only, NO type-3) MUST restore an all-clear bitmap, NOT inherit
  // stale checked bits from the live session. Save() always emits type-3, so
  // we hand-write a lone type-1 RandoState TLV to exercise the absent path.
  // -------------------------------------------------------------------------
  {
    FILE *fold = tmpfile();
    if (fold == NULL) selfcheck_die("absent-type-3: tmpfile() returned NULL");
    // A real older snapshot carries a NON-empty placement table, so use one here
    // (ids 0 and 5 in a 6-location table) — this exercises the non-empty install
    // branch followed by the type-1 clear, not just the empty-table branch.
    const uint16 tbl_locs = 6;
    const uint16 tbl_bytes = (uint16)(tbl_locs * 2u);   // 12
    uint8 t1[16 + 52 + 12];
    memcpy(t1, kRandoSnapshotTail_Magic, 8);
    put_u32le_bytes(t1 + 8, kRandoSnapshotTail_Type_RandoState);
    put_u32le_bytes(t1 + 12, 52u + (uint32)tbl_bytes);  // payload = 52 + table
    uint8 *pp = t1 + 16;
    put_u16le_bytes(pp, 0x0011); pp += 2;               // gen_version
    memset(pp, 0xAB, 16); pp += 16;                     // settings_hash
    memset(pp, 0xCD, 32); pp += 32;                     // share_string
    put_u16le_bytes(pp, tbl_bytes); pp += 2;            // placement_table_size = 12
    for (uint16 i = 0; i < tbl_locs; i++) { pp[i * 2] = 0xFF; pp[i * 2 + 1] = 0xFF; }
    put_u16le_bytes(pp + 0 * 2, 0x0101);               // loc 0 -> item 0x0101
    put_u16le_bytes(pp + 5 * 2, 0x0202);               // loc 5 -> item 0x0202
    if (fwrite(t1, 1, sizeof(t1), fold) != sizeof(t1)) selfcheck_die("absent-type-3: short write");
    // Pre-seed the live bitmap with stale bits the load MUST clear.
    memset(g_rando_checked_bitmap, 0xFF, kRandoCheckedBitmapBytes);
    // tracker-player-knowledge: the SAME contract for topology discovery, which
    // is where it was actually broken. Seed live discovery from a different seed
    // (all bits set) — an absent type-10 TLV must clear it, not inherit it. The
    // positive type-10 test above cannot catch this: it wipes discovery by hand
    // before loading, so it never asks whether the accept path does the wipe.
    {
      uint8 kc[8], ke[8];
      memset(kc, 0xFF, sizeof(kc));
      memset(ke, 0xFF, sizeof(ke));
      Rando_SetDiscoveryState(0xFFFFu, 0xFFu, kc, ke);
    }
    Placement_Install(NULL);
    Rando_ClearSnapshotContext();
    fseek(fold, 0, SEEK_SET);
    int nold = RandoSnapshotTail_Load(fold);
    if (nold != 1) selfcheck_die("absent-type-3: should recognize the lone type-1 TLV");
    for (int i = 0; i < kRandoCheckedBitmapBytes; i++) {
      if (g_rando_checked_bitmap[i] != 0)
        selfcheck_die("absent-type-3: type-1 accept must clear the checked bitmap (no stale bits)");
    }
    {
      uint16 kd; uint8 kw; uint8 kc[8], ke[8];
      Rando_GetDiscoveryState(&kd, &kw, kc, ke);
      bool any = (kd != 0) || (kw != 0);
      for (int i = 0; i < 8; i++) any = any || kc[i] != 0 || ke[i] != 0;
      // All-zero is the right expectation even with the FINISH_LOAD backfill in
      // play: it derives discovery from the checked bitmap, which the accept
      // just cleared.
      if (any)
        selfcheck_die("absent-type-10: type-1 accept must clear topology discovery "
                      "(a snapshot with no discovery TLV must never inherit the "
                      "live session's — that over-reveals another seed's tracker)");
    }
    const RandoPlacementTable *ro = Placement_GetActive();
    if (ro == NULL || ro->count != 2)
      selfcheck_die("absent-type-3: non-empty type-1 table should install (2 entries)");
    fclose(fold);
    memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);  // leave clean
  }

  // Restore prior state to leave the world unchanged.
  Placement_Install(prior_table);
  if (prior_table == NULL) Rando_ClearSnapshotContext();

  fprintf(stderr, "[RandoSnapshotTail_SelfCheck] OK\n");
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_snapshot_tail)
