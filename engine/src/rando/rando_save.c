// rando_save.c — sidecar save serialization + file IO (tasks.md §8.1, §8.2).
//
// Byte-layout discipline: every multi-byte field is written/read with explicit
// little-endian helpers. Compiler padding / struct layout is NOT relied upon
// for the on-disk format.
//
// Serialization, round-trip IO, and durable replacement are covered by
// RandoSave_SelfCheck. Normal paired game saves stage both SRAM copies, write
// and flush the sidecar first, then install the staged SRAM image so a sidecar
// failure cannot publish a mismatched game save.

// Windows headers must come BEFORE "../types.h" because types.h defines
// BYTE / WORD / DWORD as function-style macros that conflict with the same
// names as typedefs in winbase.h. Including windows.h first lets it lay
// down the typedefs; types.h's macros then shadow them — the macros are
// fine for project use, but the windows.h declarations of MoveFileExA etc.
// need the typedef to be the typedef, not a macro.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // MoveFileExA, BOOL
#include <io.h>       // _commit, _fileno
#endif

#include "rando_save.h"
#include "rando.h"
#include "alttpr_import.h"
#include "item_ids.h"
#include "../types.h"
#include "../zelda_rtl.h"
#include "../file_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The persisted settings blob must be exactly kSettingsCanonicalLen bytes (the
// canonical serializer's output size) — couples the slot field to the codec so
// a canonical-length change can never silently truncate/overrun it. Mirrors the
// ZRSR _Static_assert (rando_spoiler.h). See [[canonical-size-coupling]].
_Static_assert(sizeof(((RandoSidecarSlot *)0)->settings_canonical) == kSettingsCanonicalLen,
               "RandoSidecarSlot.settings_canonical size must equal kSettingsCanonicalLen");
_Static_assert(sizeof(((RandoSlotHeader *)0)->hint_state.plan_digest) ==
                   kRandoHintPlanDigestBytes,
               "sidecar hint digest width must match Hints v2");
_Static_assert(sizeof(((RandoSlotHeader *)0)->hint_state.discovered) ==
                   kRandoHintDiscoveryBytes,
               "sidecar hint discovery width must match Hints v2");

// Atomic-commit protocol (tasks.md §8.2 / design.md D12).
// POSIX: fsync the file descriptor, then rename, then fsync the containing dir.
// Windows: _commit on the file descriptor, then MoveFileEx with REPLACE_EXISTING.
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <libgen.h>
#endif

// ---------------------------------------------------------------------------
// LE byte helpers
// ---------------------------------------------------------------------------
static void put_u16le(uint8 *p, uint16 v) {
  p[0] = (uint8)v;
  p[1] = (uint8)(v >> 8);
}

static void put_u32le(uint8 *p, uint32 v) {
  p[0] = (uint8)v;
  p[1] = (uint8)(v >> 8);
  p[2] = (uint8)(v >> 16);
  p[3] = (uint8)(v >> 24);
}

static uint16 get_u16le(const uint8 *p) {
  return (uint16)p[0] | ((uint16)p[1] << 8);
}

static uint32 get_u32le(const uint8 *p) {
  return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

// ---------------------------------------------------------------------------
// CRC-32 for the file header's `file_crc` field (FIX #13 — the field was
// written as 0 and never verified). Standard reflected CRC-32 (poly
// 0xEDB88320, init/xorout 0xFFFFFFFF) — the SAME algorithm as src/util.c's
// static `crc32` (the BPS patcher); re-stated here because that one is
// file-local. Chosen over rando_share's CRC-16-CCITT because the on-disk
// field is u32. Known vector: "123456789" → 0xCBF43926 (selfcheck).
// ---------------------------------------------------------------------------
static uint32 rs_crc32(const uint8 *data, size_t length) {
  uint32 crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ ((crc & 1) * 0xEDB88320u);
  }
  return crc ^ 0xFFFFFFFF;
}

// ---------------------------------------------------------------------------
// On-disk sizing
//
// Per spec (randomizer-save § Slot header): `placement_table_size` is the
// byte size of the embedded placement table (= 2 × location_count, since
// each location takes a uint16 LE). The bitmap covers `location_count` bits.
// ---------------------------------------------------------------------------
// Base (version-1) on-disk size: header + flat placement table + bitmap, with
// no trailing settings blob. Internal — the version-aware paths add the blob.
static uint32 slot_on_disk_size_base(uint16 placement_table_size) {
  uint32 placements_bytes = (uint32)placement_table_size;
  uint32 location_count = (uint32)placement_table_size / 2;
  uint32 bitmap_bytes = (location_count + 7) >> 3;
  return kRandoSidecar_SlotHeaderSize + placements_bytes + bitmap_bytes;
}

enum { kRandoSidecar_LegacySettingsCanonicalLen28 = 28 };
enum { kRandoSidecar_LegacySettingsCanonicalLen29 = 29 };  // v4..v7 blob width
enum { kRandoSidecar_LegacySettingsCanonicalLen30 = 30 };  // v8 blob width

static uint32 settings_blob_len_versioned(uint16 format_version) {
  if (format_version < 2) return 0;
  if (format_version < 4) return kRandoSidecar_LegacySettingsCanonicalLen28;
  // v4..v7 carried 29 bytes; v8/v9 carried 30 bytes for terrain byte [29];
  // v10 widens to the current 31-byte layout for key rings/Skeleton Key [30].
  if (format_version < 8) return kRandoSidecar_LegacySettingsCanonicalLen29;
  if (format_version < 10) return kRandoSidecar_LegacySettingsCanonicalLen30;
  return kSettingsCanonicalLen;
}

static uint32 slot_ext_len_versioned(uint16 format_version) {
  if (format_version >= 14) return kRandoSidecar_SlotExtV14Size;
  if (format_version >= 13) return kRandoSidecar_SlotExtV13Size;
  if (format_version >= 12) return kRandoSidecar_SlotExtV12Size;
  if (format_version >= 11) return kRandoSidecar_SlotExtV11Size;
  if (format_version >= 9) return kRandoSidecar_SlotExtV9Size;
  if (format_version >= 8) return kRandoSidecar_SlotExtV8Size;
  if (format_version >= 7) return kRandoSidecar_SlotExtV7Size;
  if (format_version >= 6) return kRandoSidecar_SlotExtV6Size;
  if (format_version >= 5) return kRandoSidecar_SlotExtV5Size;
  if (format_version >= 4) return kRandoSidecar_SlotExtV4Size;
  if (format_version >= 3) return kRandoSidecar_SlotExtV3Size;
  return 0;
}

static uint32 legacy_slot_ext_len_versioned(uint16 format_version) {
  // Early v4 development builds used the current 29-byte settings blob but kept
  // the v3 8-byte extension. Accept them so slot generation can upgrade them.
  if (format_version == 4) return kRandoSidecar_SlotExtV3Size;
  return 0;
}

static uint32 slot_on_disk_size_versioned_ext(uint16 placement_table_size,
                                              uint16 format_version,
                                              uint32 ext_len) {
  uint32 total = slot_on_disk_size_base(placement_table_size);
  total += settings_blob_len_versioned(format_version);
  total += ext_len;
  return total;
}

// Format-version-aware slot size. settings_blob_len_versioned and
// slot_ext_len_versioned select every supported physical width; ReadFile
// refuses a version newer than kRandoSidecar_FileFormatVersion before it tries
// to walk slots.
static uint32 slot_on_disk_size_versioned(uint16 placement_table_size,
                                          uint16 format_version) {
  uint32 total = slot_on_disk_size_versioned_ext(
      placement_table_size, format_version, slot_ext_len_versioned(format_version));
  return total;
}

static bool all_zero_bytes(const uint8 *buf, uint32 size) {
  if (buf == NULL) return false;
  for (uint32 i = 0; i < size; i++) {
    if (buf[i] != 0)
      return false;
  }
  return true;
}

static uint32 try_deserialize_zero_empty_slot(const uint8 *buf, uint32 remaining,
                                              uint16 format_version,
                                              uint32 ext_len,
                                              uint8 slots_left,
                                              RandoSidecarSlot *out) {
  uint32 size = slot_on_disk_size_versioned_ext(0, format_version, ext_len);
  if (size == 0 || slots_left == 0)
    return 0;
  uint32 span = size * slots_left;
  if (remaining == span && all_zero_bytes(buf, span)) {
    memset(out, 0, sizeof(*out));
    return size;
  }
  if (remaining >= size && all_zero_bytes(buf, size)) {
    memset(out, 0, sizeof(*out));
    return size;
  }
  return 0;
}

// Public size is the CURRENT format: base + settings blob + extension.
uint32 RandoSave_SlotOnDiskSize(uint16 placement_table_size) {
  return slot_on_disk_size_versioned(placement_table_size,
                                     kRandoSidecar_FileFormatVersion);
}

// ---------------------------------------------------------------------------
// File header
// ---------------------------------------------------------------------------
uint32 RandoSave_SerializeFileHeader(const RandoSidecarFileHeader *h,
                                     uint8 *buf, uint32 buf_size) {
  if (h == NULL || buf == NULL || buf_size < kRandoSidecar_FileHeaderSize) return 0;
  put_u32le(buf + 0, kRandoSidecar_FileMagic);
  put_u16le(buf + 4, h->format_version);
  put_u16le(buf + 6, h->slot_count);
  put_u32le(buf + 8, h->file_crc);
  // reserved[4] @12 — zero on write.
  buf[12] = buf[13] = buf[14] = buf[15] = 0;
  return kRandoSidecar_FileHeaderSize;
}

uint32 RandoSave_DeserializeFileHeader(const uint8 *buf, uint32 buf_size,
                                       RandoSidecarFileHeader *out) {
  if (buf == NULL || out == NULL || buf_size < kRandoSidecar_FileHeaderSize) return 0;
  uint32 magic = get_u32le(buf + 0);
  if (magic != kRandoSidecar_FileMagic) return 0;
  out->format_version = get_u16le(buf + 4);
  out->slot_count = get_u16le(buf + 6);
  out->file_crc = get_u32le(buf + 8);
  // reserved bytes ignored
  return kRandoSidecar_FileHeaderSize;
}

// ---------------------------------------------------------------------------
// Slot header
// ---------------------------------------------------------------------------
static uint32 serialize_slot_header(const RandoSlotHeader *h, uint8 *buf) {
  // @0 magic[4]
  put_u32le(buf + 0, kRandoSidecar_SlotMagic);
  // @4 slot_kind
  buf[4] = h->slot_kind;
  // @5 generator_version (u16 LE)
  put_u16le(buf + 5, h->generator_version);
  // @7 settings_hash[16]
  memcpy(buf + 7, h->settings_hash, 16);
  // @23 share_string[32]
  memcpy(buf + 23, h->share_string, kRandoSidecar_ShareStringLength);
  // @55 last_vanilla_write_version (u16 LE)
  put_u16le(buf + 55, h->last_vanilla_write_version);
  // @57 sram_slot_checksum_at_last_write (u32 LE)
  put_u32le(buf + 57, h->sram_slot_checksum_at_last_write);
  // @61 placement_table_size (u16 LE)
  put_u16le(buf + 61, h->placement_table_size);
  // @63 flags
  buf[63] = h->flags;
  // @64 mushroom_held (rando Mushroom/Powder ownership bitfield).
  buf[64] = h->mushroom_held;
  // @65-67 legacy Phase B hints settings extension (additive; previously
  // zero). Retained for sidecar layout compatibility. Hints v2 reconstructs
  // exclusively from the full canonical settings blob plus certified v14 plan
  // identity; these bytes are not a substitute for either.
  buf[65] = h->settings_ext_present;
  buf[66] = h->hints_setting;
  buf[67] = h->goal;
  // @68 world_state (Phase B Inverted runtime). Additive; previously zero, so
  // old binaries reading a new file see @68 as "reserved" and ignore it, and
  // new binaries reading an old file see 0 == kWorldState_Open (safe no-op).
  buf[68] = h->world_state;
  // @69 flute_shovel_owned (rando flute/shovel decouple). Additive; previously
  // zero, so old binaries reading a new file ignore it and new binaries reading
  // an old file see 0 (own neither — the safe default).
  buf[69] = h->flute_shovel_owned;
  // @70 settings_present (format_version >= 2). Whether the slot body carries a
  // valid canonical RandoSettings blob. v1 readers see this as a reserved zero.
  buf[70] = h->settings_present;
  // @71-72 Phase C entrance shuffle (additive; previously zero). The packed
  // entrance-axis byte + accepted goal-retry attempt let slot-load regenerate
  // the cave permutation from the seed. 0/0 for non-shuffle slots. (Relocated
  // from @70/@71 on the main merge after main took @70 for settings_present.)
  buf[71] = h->entrance_axes;
  buf[72] = h->entrance_attempt;
  // @73-74 boomerang/bow ownership (additive; previously zero). Lets a
  // swapped-down byte keep the higher tier across save/reload. Pre-field readers
  // see these in the reserved tail and ignore them; new readers of an old file
  // see 0 (own neither — the safe default). See Rando_GrantBoomerang/Bow.
  buf[73] = h->boomerang_owned;
  buf[74] = h->bow_owned;
  // @75 prize_attempt (FIX #6; additive, previously zero). The accepted assumed-
  // fill attempt index used to re-derive the prize/medallion shuffle at slot
  // load. Pre-field readers see it in the reserved tail and ignore it; new
  // readers of an old file see 0 (== attempt 0 == legacy behavior).
  buf[75] = h->prize_attempt;
  // @76-79 door shuffle (claims the reserved tail): accepted door_attempt +
  // 24-bit layout digest for the activation drift hard-fail. Zero on
  // vanilla-door slots (== the old reserved zeros).
  buf[76] = h->door_attempt;
  buf[77] = (uint8)(h->door_digest24 & 0xff);
  buf[78] = (uint8)((h->door_digest24 >> 8) & 0xff);
  buf[79] = (uint8)((h->door_digest24 >> 16) & 0xff);
  return kRandoSidecar_SlotHeaderSize;
}

static uint32 deserialize_slot_header(const uint8 *buf, uint32 buf_size, RandoSlotHeader *out) {
  if (buf_size < kRandoSidecar_SlotHeaderSize) return 0;
  uint32 magic = get_u32le(buf + 0);
  if (magic != kRandoSidecar_SlotMagic) return 0;
  out->slot_kind = buf[4];
  out->generator_version = get_u16le(buf + 5);
  memcpy(out->settings_hash, buf + 7, 16);
  memcpy(out->share_string, buf + 23, kRandoSidecar_ShareStringLength);
  out->last_vanilla_write_version = get_u16le(buf + 55);
  out->sram_slot_checksum_at_last_write = get_u32le(buf + 57);
  out->placement_table_size = get_u16le(buf + 61);
  out->flags = buf[63];
  out->mushroom_held = buf[64];
  // Legacy Phase B hints settings extension (@65-67). A file written before
  // this field existed has zero here. Hints v2 treats every pre-v14 body as
  // missing certified plan metadata and fails only rich hints closed.
  out->settings_ext_present = buf[65];
  out->hints_setting = buf[66];
  out->goal = buf[67];
  // @68 world_state (Phase B Inverted runtime). Pre-field files read 0 here
  // (== kWorldState_Open), the safe no-op default.
  out->world_state = buf[68];
  // @69 flute_shovel_owned (rando flute/shovel decouple). Pre-field files read
  // 0 here (own neither), the safe default.
  out->flute_shovel_owned = buf[69];
  // @70 settings_present (format_version >= 2). v1 files read 0 (absent). The
  // version-aware body deserializer additionally forces this to 0 when the file
  // has no trailing blob, so a stray nonzero byte in a v1 file can't mislead.
  out->settings_present = buf[70];
  // @71-72 Phase C entrance shuffle. Pre-field files read 0/0 (no shuffle), the
  // safe no-op default.
  out->entrance_axes = buf[71];
  out->entrance_attempt = buf[72];
  // @73-74 boomerang/bow ownership (rando decouple). Pre-field files read 0/0
  // (own neither), the safe default.
  out->boomerang_owned = buf[73];
  out->bow_owned = buf[74];
  // @75 prize_attempt (FIX #6). Pre-field files read 0 here (== attempt 0 ==
  // legacy behavior), the safe no-op default.
  out->prize_attempt = buf[75];
  // @76-79 door shuffle. Pre-field files read zeros (vanilla doors, digest 0).
  out->door_attempt = buf[76];
  out->door_digest24 = (uint32)buf[77] | ((uint32)buf[78] << 8) | ((uint32)buf[79] << 16);
  // remaining reserved bytes ignored — forward-compat
  return kRandoSidecar_SlotHeaderSize;
}

// ---------------------------------------------------------------------------
// Slot body (placements + bitmap)
//
// Per spec: the on-disk embedded placement table is a FLAT uint16[] indexed
// by location_id. Each slot at index k holds the item_id placed at
// location_id k, or 0xFFFF as the "no placement" sentinel. The in-memory
// `placements[]` array is a sparse (location, item) list — we translate
// between the two representations here.
// ---------------------------------------------------------------------------
uint32 RandoSave_SerializeSlot(const RandoSidecarSlot *slot, uint8 *buf, uint32 buf_size) {
  if (slot == NULL || buf == NULL) return 0;
  // FIX #13 — an ODD placement_table_size is a caller bug (the table is a
  // uint16[] and the generator always writes (max_loc+1)*2). The sizing above
  // counts the raw byte size while the body below writes location_count*2
  // bytes, so an odd size would leave ONE uninitialized buffer byte between
  // the table and the bitmap. Reject explicitly rather than leak it.
  if (slot->header.placement_table_size & 1) return 0;
  uint32 size = RandoSave_SlotOnDiskSize(slot->header.placement_table_size);
  if (buf_size < size) return 0;
  uint32 location_count = (uint32)slot->header.placement_table_size / 2;
  serialize_slot_header(&slot->header, buf);
  uint8 *p = buf + kRandoSidecar_SlotHeaderSize;
  // Initialize the flat table to the no-placement sentinel.
  for (uint32 i = 0; i < location_count; i++) {
    put_u16le(p + i * 2, kRandoSidecar_NoPlacementSentinel);
  }
  // Scatter the sparse placements[] entries into the flat array. Entries with
  // location_id >= location_count are ignored (they would lose data on
  // round-trip — caller should size placement_table_size for the max id in
  // use, which is what we do at write time).
  uint16 sparse_count = slot->placement_count;
  if (sparse_count > (sizeof(slot->placements) / sizeof(slot->placements[0]))) {
    sparse_count = (uint16)(sizeof(slot->placements) / sizeof(slot->placements[0]));
  }
  for (uint16 i = 0; i < sparse_count; i++) {
    uint16 loc = slot->placements[i].location_id;
    if ((uint32)loc < location_count) {
      put_u16le(p + (uint32)loc * 2, slot->placements[i].item_id);
    }
  }
  p += location_count * 2;
  // Checked-bitmap span invariant: the on-disk bitmap is exactly
  // ((location_count + 7) >> 3) bytes and is NOT resized here (format/compat).
  // Every checkable location_id is < location_count (== placement_table_size/2),
  // so every legitimately-set checked bit falls within this span — same bound
  // the placement scatter above enforces (loc < location_count). As defense, if
  // location_count isn't byte-aligned, mask the out-of-span high bits of the
  // final partial byte so a stray bit beyond the last location can't ride along
  // (operates on the output buffer; `slot` is const and untouched).
  uint32 bitmap_bytes = (location_count + 7) >> 3;
  memcpy(p, slot->checked_bitmap, bitmap_bytes);
  if (bitmap_bytes > 0 && (location_count & 7) != 0) {
    p[bitmap_bytes - 1] &= (uint8)((1u << (location_count & 7)) - 1);
  }
  p += bitmap_bytes;
  // Current format: canonical settings blob trails the bitmap. Always written
  // (zeroed when settings_present == 0); RandoSave_SlotOnDiskSize accounts for it.
  memcpy(p, slot->settings_canonical, kSettingsCanonicalLen);
  p += kSettingsCanonicalLen;
  // format_version 5: slot extension block trails the blob (the 80-byte header
  // is full — see rando_save.h). v3/v4 bytes stay in place; v5 appends the
  // dungeon-chain layout identity.
  memset(p, 0, kRandoSidecar_SlotExtCurrentSize);
  p[0] = (uint8)(slot->header.entrance_digest24 & 0xff);
  p[1] = (uint8)((slot->header.entrance_digest24 >> 8) & 0xff);
  p[2] = (uint8)((slot->header.entrance_digest24 >> 16) & 0xff);
  if (slot->header.recommended_features0_present) {
    put_u32le(p + 3, slot->header.recommended_features0);
    p[7] = 1;
  }
  if (slot->header.pot_registry_present) {
    put_u32le(p + 8, slot->header.pot_registry_digest);
    put_u16le(p + 12, slot->header.pot_registry_count);
    p[14] = 1;
  }
  if (slot->header.chains_present) {
    p[16] = 1;
    p[17] = slot->header.chains_attempt;
    p[18] = (uint8)(slot->header.chains_digest24 & 0xff);
    p[19] = (uint8)((slot->header.chains_digest24 >> 8) & 0xff);
    p[20] = (uint8)((slot->header.chains_digest24 >> 16) & 0xff);
  }
  // format_version 6/7: soul-ownership bitfield @24-36 (v7 widened to 12
  // bytes — sizeof follows the header array).
  if (slot->header.souls_present) {
    p[24] = 1;
    memcpy(p + 25, slot->header.soul_flags, sizeof(slot->header.soul_flags));
  }
  // format_version 8: terrain registry identity @37-43
  // (add-rando-grass-rock-shuffle activation guard, mirroring the pot block).
  if (slot->header.terrain_registry_present) {
    put_u32le(p + 37, slot->header.terrain_registry_digest);
    put_u16le(p + 41, slot->header.terrain_registry_count);
    p[43] = 1;
  }
  // format_version 11: bonk registry identity @51-57 (same guard shape).
  if (slot->header.bonk_registry_present) {
    put_u32le(p + 51, slot->header.bonk_registry_digest);
    put_u16le(p + 55, slot->header.bonk_registry_count);
    p[57] = 1;
  }
  // format_version 9: enemy-check registry identity @44-50 (same guard shape).
  if (slot->header.enemy_check_registry_present) {
    put_u32le(p + 44, slot->header.enemy_check_registry_digest);
    put_u16le(p + 48, slot->header.enemy_check_registry_count);
    p[50] = 1;
  }
  // format_version 12: OW warp layout identity @58-61
  // (add-rando-ow-warp-shuffle): accepted attempt + 24-bit layout digest.
  // The layout itself is never serialized — activation regenerates it from
  // (seed, settings, ow_attempt) and HARD-FAILS on digest mismatch (a
  // drifted layout can strand a certified placement — door-gate class).
  p[58] = slot->header.ow_attempt;
  p[59] = (uint8)(slot->header.ow_digest24 & 0xff);
  p[60] = (uint8)((slot->header.ow_digest24 >> 8) & 0xff);
  p[61] = (uint8)((slot->header.ow_digest24 >> 16) & 0xff);
  // format_version 13: topology-discovery block @62-81
  // (tracker-player-knowledge). Always written (all-zero = nothing
  // discovered, a valid state). @82-231 is the RESERVED door-discovery
  // bitmap for the deferred door phase — kept zero by the leading memset.
  put_u16le(p + 62, slot->header.discovered_dungeons);
  p[64] = slot->header.discovered_whirlpools;
  memcpy(p + 66, slot->header.discovered_caves, sizeof(slot->header.discovered_caves));
  memcpy(p + 74, slot->header.discovered_exits, sizeof(slot->header.discovered_exits));
  // format_version 14: Hints v2 plan identity plus mutable discovery, appended
  // after the complete v13 topology-discovery block. Discovery is a 24-bit
  // fact mask; byte @277 is reserved and MUST remain
  // zero even if an in-memory caller accidentally sets it.
  put_u16le(p + 238, slot->header.hint_state.algorithm_version);
  put_u16le(p + 240, slot->header.hint_state.text_schema_version);
  memcpy(p + 242, slot->header.hint_state.plan_digest,
         kRandoHintPlanDigestBytes);
  memcpy(p + 274, slot->header.hint_state.discovered, 3);
  p[277] = 0;
  return size;
}

// Version-aware slot deserialize. `format_version` is the FILE's declared
// version; the helpers above select its exact settings and extension widths.
// RandoSave_ReadFile passes the file's validated value.
static uint32 deserialize_slot_versioned_ext(const uint8 *buf, uint32 buf_size,
                                             RandoSidecarSlot *out,
                                             uint16 format_version,
                                             uint32 ext_len) {
  if (buf == NULL || out == NULL) return 0;
  uint32 settings_blob_len = settings_blob_len_versioned(format_version);
  bool with_settings = (settings_blob_len != 0);
  bool with_ext = (ext_len != 0);
  memset(out, 0, sizeof(*out));
  uint32 hdr_used = deserialize_slot_header(buf, buf_size, &out->header);
  if (hdr_used == 0) return 0;
  // FIX #13 — the placement table is a uint16[]: every legitimate writer
  // produces an even byte size ((max_loc+1)*2). An odd size is corruption;
  // treat it like a bad magic rather than silently halving it (the writer/
  // loader would disagree on where the bitmap starts).
  if (out->header.placement_table_size & 1) return 0;
  uint32 location_count = (uint32)out->header.placement_table_size / 2;
  // Sanity-check against the in-memory bound — we cannot store more sparse
  // pairs than the static array can hold.
  if (location_count > (sizeof(out->placements) / sizeof(out->placements[0]))) {
    return 0;
  }
  uint32 total = slot_on_disk_size_versioned_ext(
      out->header.placement_table_size, format_version, ext_len);
  if (buf_size < total) return 0;
  const uint8 *p = buf + kRandoSidecar_SlotHeaderSize;
  // Gather the flat array back into the sparse in-memory list. Sentinel
  // entries (item_id == 0xFFFF) are skipped — the dispatcher's fall-back
  // path handles those.
  uint16 sparse_count = 0;
  for (uint32 i = 0; i < location_count; i++) {
    uint16 item_id = get_u16le(p + i * 2);
    if (item_id == kRandoSidecar_NoPlacementSentinel) continue;
    out->placements[sparse_count].location_id = (uint16)i;
    out->placements[sparse_count].item_id = item_id;
    sparse_count++;
  }
  out->placement_count = sparse_count;
  p += location_count * 2;
  uint32 bitmap_bytes = (location_count + 7) >> 3;
  // The location_count cap above (== placements[] length) is sized so the
  // checked-bitmap always covers it; assert that coupling so the two can't
  // drift apart silently. Fail CLOSED if a slot ever claims more bits than the
  // bitmap holds — better to reject the slot than load it with checks cleared.
  _Static_assert((sizeof(out->placements) / sizeof(out->placements[0]))
                     <= sizeof(out->checked_bitmap) * 8,
                 "checked_bitmap must cover every placement slot's location bit");
  if (bitmap_bytes > sizeof(out->checked_bitmap)) return 0;
  memcpy(out->checked_bitmap, p, bitmap_bytes);
  p += bitmap_bytes;
  if (with_settings) {
    if (settings_blob_len > kSettingsCanonicalLen) return 0;
    memcpy(out->settings_canonical, p, settings_blob_len);
    if (settings_blob_len < kSettingsCanonicalLen) {
      memset(out->settings_canonical + settings_blob_len, 0,
             kSettingsCanonicalLen - settings_blob_len);
    }
    p += settings_blob_len;
  } else {
    // A v1 file physically has no blob — force settings_present off so a stray
    // @70 byte can't be mistaken for a (nonexistent) valid blob.
    out->header.settings_present = 0;
  }
  if (with_ext) {
    if (ext_len < kRandoSidecar_SlotExtV3Size) return 0;
    // format_version 3+: extension block (@0-2 entrance_digest24 LE; @3-6
    // recommended_features0 LE; @7 presence). v4 adds pot registry identity;
    // v5 adds dungeon-chain identity.
    out->header.entrance_digest24 =
        (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16);
    out->header.recommended_features0 = get_u32le(p + 3);
    out->header.recommended_features0_present = p[7] != 0;
    if (format_version >= 4 && ext_len >= kRandoSidecar_SlotExtV4Size) {
      out->header.pot_registry_digest = get_u32le(p + 8);
      out->header.pot_registry_count = get_u16le(p + 12);
      out->header.pot_registry_present = p[14] != 0;
    } else {
      out->header.pot_registry_digest = 0;
      out->header.pot_registry_count = 0;
      out->header.pot_registry_present = 0;
    }
    if (format_version >= 5 && ext_len >= kRandoSidecar_SlotExtV5Size) {
      out->header.chains_present = p[16] != 0;
      out->header.chains_attempt = p[17];
      out->header.chains_digest24 =
          (uint32)p[18] | ((uint32)p[19] << 8) | ((uint32)p[20] << 16);
    } else {
      out->header.chains_present = 0;
      out->header.chains_attempt = 0;
      out->header.chains_digest24 = 0;
    }
    if (format_version >= 7 && ext_len >= kRandoSidecar_SlotExtV7Size) {
      out->header.souls_present = p[24] != 0;
      memcpy(out->header.soul_flags, p + 25, sizeof(out->header.soul_flags));
    } else if (format_version >= 6 && ext_len >= kRandoSidecar_SlotExtV6Size) {
      // v6 file: enemy/boss soul bytes only; the NPC tail reads as zero
      // (un-owned — the safe default; pre-npc seeds have npc_souls=off).
      out->header.souls_present = p[24] != 0;
      memset(out->header.soul_flags, 0, sizeof(out->header.soul_flags));
      memcpy(out->header.soul_flags, p + 25, 8);
    } else {
      out->header.souls_present = 0;
      memset(out->header.soul_flags, 0, sizeof(out->header.soul_flags));
    }
    if (format_version >= 8 && ext_len >= kRandoSidecar_SlotExtV8Size) {
      // add-rando-grass-rock-shuffle: terrain registry identity @37-43.
      out->header.terrain_registry_digest = get_u32le(p + 37);
      out->header.terrain_registry_count = get_u16le(p + 41);
      out->header.terrain_registry_present = p[43] != 0;
    } else {
      // Pre-v8 file: no terrain identity. Safe — pre-terrain slots carry both
      // axes off in the (zero-extended) settings blob, so the activation guard
      // is never consulted for them.
      out->header.terrain_registry_digest = 0;
      out->header.terrain_registry_count = 0;
      out->header.terrain_registry_present = 0;
    }
    if (format_version >= 11 && ext_len >= kRandoSidecar_SlotExtV11Size) {
      // bonk registry identity @51-57 (add-rando-bonk-sanity).
      out->header.bonk_registry_digest = get_u32le(p + 51);
      out->header.bonk_registry_count = get_u16le(p + 55);
      out->header.bonk_registry_present = p[57] != 0;
    } else {
      // Pre-v11 file: no bonk identity. Safe — pre-bonk slots carry the axis
      // off in the (zero-extended) settings blob, so the activation guard is
      // never consulted for them.
      out->header.bonk_registry_digest = 0;
      out->header.bonk_registry_count = 0;
      out->header.bonk_registry_present = 0;
    }
    if (format_version >= 9 && ext_len >= kRandoSidecar_SlotExtV9Size) {
      // enemy-check registry identity @44-50.
      out->header.enemy_check_registry_digest = get_u32le(p + 44);
      out->header.enemy_check_registry_count = get_u16le(p + 48);
      out->header.enemy_check_registry_present = p[50] != 0;
    } else {
      // Pre-v9 file: no enemy-check identity. present=0 makes activation
      // fail CLOSED for dungeon/all-tier enemy-check slots (unlike terrain,
      // such slots DO exist pre-v9 — regenerate the seed on this build).
      out->header.enemy_check_registry_digest = 0;
      out->header.enemy_check_registry_count = 0;
      out->header.enemy_check_registry_present = 0;
    }
    if (format_version >= 12 && ext_len >= kRandoSidecar_SlotExtV12Size) {
      // add-rando-ow-warp-shuffle: warp layout identity @58-61. Older files
      // read 0/0 = vanilla warps (no warp axis existed to set them).
      out->header.ow_attempt = p[58];
      out->header.ow_digest24 =
          (uint32)p[59] | ((uint32)p[60] << 8) | ((uint32)p[61] << 16);
    } else {
      out->header.ow_attempt = 0;
      out->header.ow_digest24 = 0;
    }
    if (format_version >= 13 && ext_len >= kRandoSidecar_SlotExtV13Size) {
      // tracker-player-knowledge: topology-discovery block @62-81. Pre-v13
      // files read zeros; activation then backfills dungeon/cave bits from
      // checked-location state (never from placement/settings).
      out->header.discovered_dungeons = get_u16le(p + 62);
      out->header.discovered_whirlpools = p[64];
      memcpy(out->header.discovered_caves, p + 66, sizeof(out->header.discovered_caves));
      memcpy(out->header.discovered_exits, p + 74, sizeof(out->header.discovered_exits));
    } else {
      out->header.discovered_dungeons = 0;
      out->header.discovered_whirlpools = 0;
      memset(out->header.discovered_caves, 0, sizeof(out->header.discovered_caves));
      memset(out->header.discovered_exits, 0, sizeof(out->header.discovered_exits));
    }
    if (format_version >= 14 && ext_len >= kRandoSidecar_SlotExtV14Size) {
      out->header.hint_state.algorithm_version = get_u16le(p + 238);
      out->header.hint_state.text_schema_version = get_u16le(p + 240);
      memcpy(out->header.hint_state.plan_digest, p + 242,
             kRandoHintPlanDigestBytes);
      memcpy(out->header.hint_state.discovered, p + 274, 3);
      // Bits 24..31 are reserved and never become gameplay discovery.
      out->header.hint_state.discovered[3] = 0;
    } else {
      // A pre-v14 slot has no certified plan identity. Zero versions mean
      // "metadata absent"; activation disables Hints v2 without guessing.
      memset(&out->header.hint_state, 0, sizeof(out->header.hint_state));
    }
  } else {
    // v1/v2 files physically lack the block — digest 0 = "absent", which keeps
    // the legacy warn-only entrance version-drift behavior for old slots.
    out->header.entrance_digest24 = 0;
    out->header.recommended_features0 = 0;
    out->header.recommended_features0_present = 0;
    out->header.pot_registry_digest = 0;
    out->header.pot_registry_count = 0;
    out->header.pot_registry_present = 0;
    out->header.chains_present = 0;
    out->header.chains_attempt = 0;
    out->header.chains_digest24 = 0;
    out->header.souls_present = 0;
    memset(out->header.soul_flags, 0, sizeof(out->header.soul_flags));
    out->header.terrain_registry_digest = 0;
    out->header.terrain_registry_count = 0;
    out->header.terrain_registry_present = 0;
    out->header.enemy_check_registry_digest = 0;
    out->header.enemy_check_registry_count = 0;
    out->header.enemy_check_registry_present = 0;
    out->header.bonk_registry_digest = 0;
    out->header.bonk_registry_count = 0;
    out->header.bonk_registry_present = 0;
    out->header.discovered_dungeons = 0;
    out->header.discovered_whirlpools = 0;
    memset(out->header.discovered_caves, 0, sizeof(out->header.discovered_caves));
    memset(out->header.discovered_exits, 0, sizeof(out->header.discovered_exits));
    memset(&out->header.hint_state, 0, sizeof(out->header.hint_state));
  }
  return total;
}

static uint32 deserialize_slot_versioned(const uint8 *buf, uint32 buf_size,
                                         RandoSidecarSlot *out, uint16 format_version) {
  return deserialize_slot_versioned_ext(
      buf, buf_size, out, format_version, slot_ext_len_versioned(format_version));
}

uint32 RandoSave_DeserializeSlot(const uint8 *buf, uint32 buf_size, RandoSidecarSlot *out) {
  // Public entry assumes the current (version-5) layout. RandoSave_ReadFile
  // uses the version-aware static directly for older files.
  return deserialize_slot_versioned(buf, buf_size, out, kRandoSidecar_FileFormatVersion);
}

// ---------------------------------------------------------------------------
// Atomic-commit file write (tasks.md §8.2 / design.md D12).
//
// Protocol:
//   1. fopen `<path>.tmp` for writing, fwrite the payload, fflush.
//   2. fsync (POSIX) / _commit (Windows) the file descriptor — ensures the
//      bytes hit the storage device before the rename.
//   3. fclose.
//   4. rename `<path>.tmp` → `<path>` atomically. On POSIX `rename(2)` is
//      atomic over an existing target. On Windows we use MoveFileExA with
//      MOVEFILE_REPLACE_EXISTING for the same atomicity property (best-effort
//      — Windows atomicity guarantees are weaker than POSIX, but the
//      MoveFileEx-replace semantics are the strongest tool available).
//   5. fsync the containing directory (POSIX) — ensures the directory entry's
//      new name is durable. Skipped on Windows (no equivalent; the
//      MoveFileEx already guarantees the rename is visible).
//
// On any I/O error in steps 1-3, the .tmp file is removed and we return false
// without touching the final path — the prior `<path>` (if any) is untouched.
// ---------------------------------------------------------------------------

static bool atomic_write_and_commit(const char *final_path,
                                    const uint8 *buf, uint32 total) {
  size_t plen = strlen(final_path);
  char *tmp_path = (char *)malloc(plen + 5);
  if (tmp_path == NULL) return false;
  memcpy(tmp_path, final_path, plen);
  strcpy(tmp_path + plen, ".tmp");

  FILE *f = fopen(tmp_path, "wb");
  if (f == NULL) { free(tmp_path); return false; }
  size_t wrote = fwrite(buf, 1, total, f);
  if (wrote != total) {
    fclose(f);
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
  if (fflush(f) != 0) {
    fclose(f);
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
#if defined(_WIN32)
  // _commit equivalent of fsync on Windows.
  if (_commit(_fileno(f)) != 0) {
    fclose(f);
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
#else
  if (fsync(fileno(f)) != 0) {
    fclose(f);
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
#endif
  if (fclose(f) != 0) {
    remove(tmp_path);
    free(tmp_path);
    return false;
  }

  // Atomic rename.
#if defined(_WIN32)
  // MoveFileExA with MOVEFILE_REPLACE_EXISTING is the closest Windows
  // analogue to POSIX rename(2) atomicity. Fails if the source doesn't exist
  // or the target is locked.
  BOOL ok = MoveFileExA(tmp_path, final_path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  if (!ok) {
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
#elif defined(__3DS__)
  // The SD filesystem cannot rename over an existing destination. Use the
  // same recoverable replacement contract as profile/settings writes.
  if (!FileStore_Commit(tmp_path, final_path)) {
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
#else
  if (rename(tmp_path, final_path) != 0) {
    remove(tmp_path);
    free(tmp_path);
    return false;
  }
  // fsync the containing directory so the rename's directory entry is
  // durable. POSIX-only; Windows MoveFileEx handles this internally.
  {
    char *path_copy = strdup(final_path);
    if (path_copy != NULL) {
      char *slash = strrchr(path_copy, '/');
      const char *dir = ".";
      if (slash == path_copy) dir = "/";
      else if (slash) { *slash = '\0'; dir = path_copy; }
      int dirfd = open(dir, O_RDONLY);
      if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
      }
      free(path_copy);
    }
  }
#endif
  free(tmp_path);
  return true;
}

static uint8 *serialize_file_buffer(
    const RandoSidecarSlot slots[kRandoSidecar_SlotCount], uint32 *size_out) {
  // FIX #13 — refuse odd placement_table_size up front. RandoSave_SerializeSlot
  // rejects it too, but failing HERE keeps the buffer arithmetic below sound
  // (a 0-return mid-loop would shift every later slot and write garbage).
  for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
    if (slots[i].header.placement_table_size & 1) return NULL;
  }
  // Compute total size.
  uint32 total = kRandoSidecar_FileHeaderSize;
  for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
    total += RandoSave_SlotOnDiskSize(slots[i].header.placement_table_size);
  }
  uint8 *buf = (uint8 *)malloc(total);
  if (buf == NULL) return NULL;

  RandoSidecarFileHeader fh = { kRandoSidecar_FileFormatVersion, kRandoSidecar_SlotCount, 0 };
  uint32 off = RandoSave_SerializeFileHeader(&fh, buf, total);
  for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
    uint32 used = RandoSave_SerializeSlot(&slots[i], buf + off, total - off);
    if (!used || used > total - off) { free(buf); return NULL; }
    off += used;
  }

  if (off != total) { free(buf); return NULL; }
  // FIX #13 — file_crc (header @8) now carries a real CRC-32 over the SLOT
  // region (every byte after the 16-byte file header; the crc field itself
  // lives inside the header, so it is excluded by construction). Patch it in
  // after the slots are serialized. A computed value of 0 is written as-is —
  // readers treat 0 as "legacy, no CRC" (every pre-FIX file has 0 here), so
  // that 1-in-2^32 file simply skips verification.
  put_u32le(buf + 8, rs_crc32(buf + kRandoSidecar_FileHeaderSize,
                              total - kRandoSidecar_FileHeaderSize));

  *size_out = total;
  return buf;
}

bool RandoSave_WriteFile(const char *path,
                         const RandoSidecarSlot slots[kRandoSidecar_SlotCount]) {
  if (!path || !slots) return false;
  uint32 size;
  uint8 *buffer = serialize_file_buffer(slots, &size);
  if (!buffer) return false;
  bool ok = atomic_write_and_commit(path, buffer, size);
  free(buffer);
  return ok;
}

static bool deserialize_file_buffer(const uint8 *buf, uint32 fsize,
                                    RandoSidecarSlot out_slots[kRandoSidecar_SlotCount]) {
  if (buf == NULL || out_slots == NULL) return false;
  if (fsize < kRandoSidecar_FileHeaderSize) return false;
  RandoSidecarFileHeader fh;
  uint32 hdr_used = RandoSave_DeserializeFileHeader(buf, fsize, &fh);
  if (hdr_used == 0 || fh.slot_count != kRandoSidecar_SlotCount) return false;
  // Sequential slot bodies have no per-slot length tag. Refuse a newer file
  // version explicitly rather than guessing its settings/extension widths.
  if (fh.format_version == 0 ||
      fh.format_version > kRandoSidecar_FileFormatVersion)
    return false;

  // FIX #13 — verify file_crc when present. 0 = legacy file (every writer
  // before the CRC landed wrote 0) → accept without verification. The CRC
  // covers the slot region only (bytes after the 16-byte header), matching
  // the writer.
  if (fh.file_crc != 0) {
    uint32 actual = rs_crc32(buf + kRandoSidecar_FileHeaderSize,
                             fsize - kRandoSidecar_FileHeaderSize);
    if (actual != fh.file_crc) {
      fprintf(stderr,
              "RandoSave_ReadFile: sidecar CRC mismatch (file %08x != computed %08x) "
              "— treating file as corrupt\n",
              (unsigned)fh.file_crc, (unsigned)actual);
      return false;
    }
  }

  // Key the body layout on the file's declared format_version so older
  // sidecars still load correctly: v1 slots lack the settings blob, v2 slots
  // lack an extension block, v2/v3 slots carry the legacy 28-byte settings
  // prefix, and each later supported version uses its declared extension
  // width. Early v4
  // development files declared format 4 but still used that 8-byte extension,
  // so try the canonical size first and then the legacy physical v4 size.
  uint32 ext_candidates[2] = {
    slot_ext_len_versioned(fh.format_version),
    legacy_slot_ext_len_versioned(fh.format_version),
  };
  for (uint8 pass = 0; pass < 2; pass++) {
    uint32 ext_len = ext_candidates[pass];
    if (pass != 0 && ext_len == 0)
      continue;
    if (pass != 0 && ext_len == ext_candidates[0])
      continue;
    RandoSidecarSlot tmp[kRandoSidecar_SlotCount];
    uint32 off = hdr_used;
    bool ok = true;
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      if (off > fsize) { ok = false; break; }
      uint32 used = deserialize_slot_versioned_ext(
          buf + off, fsize - off, &tmp[i], fh.format_version, ext_len);
      if (used == 0) {
        used = try_deserialize_zero_empty_slot(
            buf + off, fsize - off, fh.format_version, ext_len,
            (uint8)(kRandoSidecar_SlotCount - i), &tmp[i]);
      }
      if (used == 0) { ok = false; break; }
      off += used;
    }
    // Refuse trailing bytes: without this, an older reader can accidentally
    // accept a widened future slot body after walking only its known prefix.
    if (ok && off == fsize) {
      memcpy(out_slots, tmp, sizeof(tmp));
      return true;
    }
  }
  return false;
}

bool RandoSave_ReadFile(const char *path,
                        RandoSidecarSlot out_slots[kRandoSidecar_SlotCount]) {
  if (path == NULL || !FileStore_Recover(path)) return false;
  FILE *f = fopen(path, "rb");
  if (f == NULL) return false;
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fsize < (long)kRandoSidecar_FileHeaderSize) { fclose(f); return false; }
  uint8 *buf = (uint8 *)malloc((size_t)fsize);
  if (buf == NULL) { fclose(f); return false; }
  size_t read_bytes = fread(buf, 1, (size_t)fsize, f);
  fclose(f);
  if (read_bytes != (size_t)fsize) { free(buf); return false; }

  bool ok = deserialize_file_buffer(buf, (uint32)fsize, out_slots);
  free(buf);
  return ok;
}

// A seed namespace has one authoritative, atomically replaced bundle. The
// ordinary SRAM/sidecar files are compatibility mirrors; startup repairs them
// from this bundle. A crash between mirror replacements cannot split the pair.
// The initial bundle is immutable and supplies fresh files of the SAME seed.
static bool g_seed_storage_active;
static bool g_seed_storage_healthy = true;
#define kPairHeaderSize 20u
#define kPairSramSize 8192u
#define kPairMaxSize (1024u * 1024u)

void RandoSave_SetSeedStorageActive(bool active) {
  g_seed_storage_active = active;
  g_seed_storage_healthy = true;
  Alttpr_Clear();
}
bool RandoSave_SeedStorageActive(void) { return g_seed_storage_active; }
bool RandoSave_SeedStorageHealthy(void) { return g_seed_storage_healthy; }

static void pair_path(const char *name, char out[512]) {
  ZeldaFormatSavePath(name, out, 512);
}

static uint8 *read_pair(const char *name, uint32 *size_out,
                        RandoSidecarSlot slots[kRandoSidecar_SlotCount]) {
  char path[512]; pair_path(name, path);
  if (!FileStore_Recover(path)) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  bool ok = fseek(f, 0, SEEK_END) == 0;
  long length = ok ? ftell(f) : -1;
  ok = length >= (long)(kPairHeaderSize + kPairSramSize) &&
       length <= (long)kPairMaxSize && fseek(f, 0, SEEK_SET) == 0;
  uint8 *buf = ok ? malloc((size_t)length) : NULL;
  ok = buf && fread(buf, 1, (size_t)length, f) == (size_t)length;
  fclose(f);
  if (ok) {
    uint32 side_size = (uint32)length - kPairHeaderSize - kPairSramSize;
    ok = !memcmp(buf, "Z3PAIR\1\0", 8) && get_u32le(buf + 8) == (uint32)length &&
         get_u32le(buf + 12) == kPairSramSize &&
         get_u32le(buf + 16) == rs_crc32(buf + kPairHeaderSize,
                                       (uint32)length - kPairHeaderSize) &&
         deserialize_file_buffer(buf + kPairHeaderSize + kPairSramSize, side_size, slots);
    for (int i = 0; ok && i < 3; i++) {
      const uint8 *sram = buf + kPairHeaderSize + i * 0x500;
      bool occupied = (sram[0x3e5] | (unsigned)sram[0x3e6] << 8) == 0x55aa;
      ok = occupied ? slots[i].header.slot_kind == kSlotKind_Randomizer &&
            !Rando_DetectChecksumDrift(&slots[i].header, sram, 0x500) :
            slots[i].header.slot_kind != kSlotKind_Randomizer;
      if (ok && occupied)
        ok = RandoSave_ComputeSramSlotChecksum(sram,0x500) ==
             (uint32)(sram[0x4fe] | (unsigned)sram[0x4ff] << 8);
    }
  }
  if (!ok) { free(buf); return NULL; }
  *size_out = (uint32)length;
  return buf;
}

static void publish_pair_mirrors(const uint8 *bundle, uint32 length) {
  char path[512]; pair_path("saves/sram.dat", path);
  bool ok = atomic_write_and_commit(path, bundle + kPairHeaderSize, kPairSramSize);
  pair_path("saves/sram_rando.dat", path);
  ok = atomic_write_and_commit(path, bundle + kPairHeaderSize + kPairSramSize,
          length - kPairHeaderSize - kPairSramSize) && ok;
  if (!ok) fprintf(stderr, "Rando: save bundle committed; mirrors will recover on restart\n");
}

int RandoSave_RecoverPairedSave(void) {
  if (!g_seed_storage_active) return 0;
  char path[512]; pair_path("saves/seed_save.dat", path);
  bool present;
  if (!FileStore_Recover(path) || !FileStore_Exists(path, &present)) {
    g_seed_storage_healthy = false; return -1;
  }
  if (!present) return 0; // Legacy paired files are validated before resuming.
  RandoSidecarSlot *slots = malloc(sizeof(*slots) * kRandoSidecar_SlotCount);
  uint32 length;
  uint8 *buf = slots ? read_pair("saves/seed_save.dat", &length, slots) : NULL;
  free(slots);
  if (!buf) { g_seed_storage_healthy = false; return -1; }
  // Early local builds initialized only the primary SRAM copy before the
  // first file-menu copy. The bundle CRC and sidecar certify each primary;
  // rebuild all backups from those primaries, including erased/empty slots.
  for(int i=0;i<3;i++) memcpy(buf+kPairHeaderSize+i*0x500+0xf00,
                              buf+kPairHeaderSize+i*0x500,0x500);
  publish_pair_mirrors(buf, length);
  memcpy(g_zenv.sram, buf + kPairHeaderSize, kPairSramSize);
  free(buf);
  return 1;
}

bool RandoSave_CommitPairedSlot(int slot_index,
    const RandoSidecarSlot slots[kRandoSidecar_SlotCount],
    const uint8 *sram_slot, uint32 size) {
  if (slot_index < 0 || slot_index >= 3 || !sram_slot || size != 0x500) return false;
  if (!g_seed_storage_active) {
    char path[512]; pair_path("saves/sram_rando.dat", path);
    return RandoSave_WriteFile(path, slots);
  }
  uint32 side_size;
  uint8 *side = serialize_file_buffer(slots, &side_size);
  if (!side) return false;
  uint32 length = kPairHeaderSize + kPairSramSize + side_size;
  uint8 *buf = malloc(length);
  if (!buf) { free(side); return false; }
  memcpy(buf, "Z3PAIR\1\0", 8);
  put_u32le(buf + 8, length); put_u32le(buf + 12, kPairSramSize);
  memcpy(buf + kPairHeaderSize, g_zenv.sram, kPairSramSize);
  memcpy(buf + kPairHeaderSize + slot_index * 0x500, sram_slot, 0x500);
  memcpy(buf + kPairHeaderSize + slot_index * 0x500 + 0xf00, sram_slot, 0x500);
  for(int i=0;i<3;i++) memcpy(buf+kPairHeaderSize+i*0x500+0xf00,
                              buf+kPairHeaderSize+i*0x500,0x500);
  memcpy(buf + kPairHeaderSize + kPairSramSize, side, side_size);
  free(side);
  put_u32le(buf + 16, rs_crc32(buf + kPairHeaderSize, length - kPairHeaderSize));
  char path[512]; pair_path("saves/seed_initial.dat", path);
  bool present;
  bool ok = FileStore_Recover(path) && FileStore_Exists(path, &present);
  if (ok && !present) ok = atomic_write_and_commit(path, buf, length);
  pair_path("saves/seed_save.dat", path);
  if (ok) ok = atomic_write_and_commit(path, buf, length);
  if (ok) publish_pair_mirrors(buf, length);
  free(buf);
  return ok;
}

bool RandoSave_CopyFileSlot(int source, int target) {
  if (source < 0 || source >= 3 || target < 0 || target >= 3 || source == target)
    return false;
  RandoSidecarFileState state = Rando_SidecarFileState();
  if (state == kRandoSidecarFile_Absent) return !g_seed_storage_active;
  if (state != kRandoSidecarFile_Ok) return false;
  RandoSidecarSlot slot;
  if (!Rando_LoadSidecarSlot(source, &slot)) return false;
  if (slot.header.slot_kind != kSlotKind_Randomizer) return !g_seed_storage_active;
  const uint8 *bytes = g_zenv.sram + source * 0x500;
  if (Rando_DetectChecksumDrift(&slot.header, bytes, 0x500)) return false;
  return Rando_WriteSidecarSlot(target, &slot, bytes, 0x500);
}

bool RandoSave_EraseFileSlot(int target) {
  if (target < 0 || target >= 3) return false;
  RandoSidecarFileState state = Rando_SidecarFileState();
  if (state == kRandoSidecarFile_Absent) return !g_seed_storage_active;
  if (state != kRandoSidecarFile_Ok) return false;
  RandoSidecarSlot empty = {0};
  uint8 bytes[0x500] = {0};
  return Rando_WriteSidecarSlot(target, &empty, bytes, sizeof(bytes));
}

bool RandoSave_InitialSeedReady(void) {
  RandoSidecarSlot *slots = malloc(sizeof(*slots) * kRandoSidecar_SlotCount);
  uint32 length;
  uint8 *buf = slots ? read_pair("saves/seed_initial.dat", &length, slots) : NULL;
  bool ok = buf && slots[0].header.slot_kind == kSlotKind_Randomizer;
  free(buf); free(slots);
  return ok;
}

bool RandoSave_CreateNamedFileSlot(int target, const uint8 name[12]) {
  if (!g_seed_storage_active || target < 0 || target >= 3) return false;
  RandoSidecarSlot *slots = malloc(sizeof(*slots) * kRandoSidecar_SlotCount);
  uint32 length;
  uint8 *buf = slots ? read_pair("saves/seed_initial.dat", &length, slots) : NULL;
  uint8 fresh[0x500];
  bool ok = buf && slots[0].header.slot_kind == kSlotKind_Randomizer;
  if (ok) {
    memcpy(fresh, buf+kPairHeaderSize, sizeof(fresh));
    if (name) memcpy(fresh+0x3d9,name,12);
    uint16 checksum=(uint16)RandoSave_ComputeSramSlotChecksum(fresh,sizeof(fresh));
    fresh[0x4fe]=checksum; fresh[0x4ff]=checksum>>8;
    ok=Rando_WriteSidecarSlot(target,slots,fresh,sizeof(fresh));
  }
  if (ok) {
    memcpy(g_zenv.sram+target*0x500,fresh,sizeof(fresh));
    memcpy(g_zenv.sram+target*0x500+0xf00,fresh,sizeof(fresh));
  }
  free(buf); free(slots);
  return ok;
}

bool RandoSave_CreateFileSlot(int target) {
  return RandoSave_CreateNamedFileSlot(target,NULL);
}

bool RandoSave_OfferNamedFiles(void) {
  // Generation has already committed the immutable seed_initial template.
  // Present empty slots instead of publishing its internal placeholder name.
  if (!RandoSave_InitialSeedReady() || !RandoSave_EraseFileSlot(0)) return false;
  memset(g_zenv.sram,0,0x500);
  memset(g_zenv.sram+0xf00,0,0x500);
  Rando_DeactivateSlot();
  return true;
}

// ---------------------------------------------------------------------------
// Per-slot SRAM checksum (tasks.md §8.4).
//
// Mirrors `SaveGameFile`'s per-slot checksum:
//   t = 0x5a5a;
//   for (i = 0; i < 0x4fe; i += 2) t -= *(uint16 *)(slot_bytes + i);
//
// We re-implement byte-by-byte from `slot_bytes` (rather than #include
// messaging.c's table) because messaging.c reads a `save_dung_info`-typed
// pointer that's tied to game state. The arithmetic is identical: read u16
// LE pairs from offset 0..0x4fe in 2-byte stride, subtract from 0x5a5a.
// ---------------------------------------------------------------------------
uint32 RandoSave_ComputeSramSlotChecksum(const uint8 *slot_bytes, uint32 size) {
  if (slot_bytes == NULL || size < 0x4fe) return 0;
  uint16 t = 0x5a5a;
  for (uint32 i = 0; i < 0x4fe; i += 2) {
    // Vanilla game writes save_dung_info as a uint16-aligned buffer; we read
    // bytes in LE order to be host-endianness-independent.
    uint16 w = (uint16)slot_bytes[i] | ((uint16)slot_bytes[i + 1] << 8);
    t = (uint16)(t - w);
  }
  return (uint32)t;
}

// ---------------------------------------------------------------------------
// Rando_LoadSidecarSlot — pure read of saves/sram_rando.dat into `*out`.
// ---------------------------------------------------------------------------
static const char *rando_sidecar_path(void) {
  static char path[512];
  ZeldaFormatSavePath("saves/sram_rando.dat", path, sizeof(path));
  return path;
}

bool Rando_LoadSidecarSlot(int slot_index, RandoSidecarSlot *out) {
  if (out == NULL) return false;
  if (slot_index < 0 || slot_index >= (int)kRandoSidecar_SlotCount) return false;

  RandoSidecarSlot all_slots[kRandoSidecar_SlotCount];
  if (!RandoSave_ReadFile(rando_sidecar_path(), all_slots)) return false;

  // Sanity-check the requested slot's bounds before copying out — the
  // deserializer already validates buf sizes & magic, but defense-in-depth
  // catches a corrupted file that somehow parsed (deserialize returns
  // non-zero on header.placement_table_size that fits the array bound).
  const RandoSidecarSlot *src = &all_slots[slot_index];
  uint32 loc_count = (uint32)src->header.placement_table_size / 2u;
  if (loc_count > (sizeof(src->placements) / sizeof(src->placements[0]))) {
    return false;
  }
  *out = *src;
  return true;
}

RandoSidecarFileState Rando_SidecarFileState(void) {
  if (!FileStore_Recover(rando_sidecar_path())) return kRandoSidecarFile_Unreadable;
  FILE *probe = fopen(rando_sidecar_path(), "rb");
  if (probe == NULL) return kRandoSidecarFile_Absent;
  fclose(probe);
  RandoSidecarSlot all_slots[kRandoSidecar_SlotCount];
  if (!RandoSave_ReadFile(rando_sidecar_path(), all_slots))
    return kRandoSidecarFile_Unreadable;
  return kRandoSidecarFile_Ok;
}

// ---------------------------------------------------------------------------
// Rando_WriteSidecarSlot — atomic write of saves/sram_rando.dat.
// ---------------------------------------------------------------------------
bool Rando_WriteSidecarSlot(int slot_index, const RandoSidecarSlot *in,
                            const uint8 *paired_sram_slot,
                            uint32 paired_sram_slot_size) {
  if (in == NULL || paired_sram_slot == NULL) return false;
  if (!FileStore_Recover(rando_sidecar_path())) return false;
  if (slot_index < 0 || slot_index >= (int)kRandoSidecar_SlotCount) return false;

  // Read existing file (if any) so we don't clobber the other slots. If the
  // existing sidecar is unreadable, quarantine it before starting a fresh file:
  // other slots are not loadable from that file anyway, and requiring a second
  // Generate after the quarantine is just a first-write UX failure.
  RandoSidecarSlot all_slots[kRandoSidecar_SlotCount];
  bool file_exists = false;
  {
    FILE *probe = fopen(rando_sidecar_path(), "rb");
    if (probe != NULL) { file_exists = true; fclose(probe); }
  }
  if (file_exists) {
    if (!RandoSave_ReadFile(rando_sidecar_path(), all_slots)) {
      if (g_seed_storage_active) return false;
      // File exists but failed to parse. Back it up under a .bad suffix before
      // writing a fresh sidecar. If the quarantine fails, refuse the write so
      // we do not overwrite potentially recoverable bytes in place.
      char bak_path[520];
      snprintf(bak_path, sizeof(bak_path), "%s.bad", rando_sidecar_path());
      remove(bak_path);
      if (rename(rando_sidecar_path(), bak_path) != 0)
        return false;
      memset(all_slots, 0, sizeof(all_slots));
    }
  } else {
    // Genuine first-write — initialize all slots empty. The serializer
    // still writes a valid header per slot.
    memset(all_slots, 0, sizeof(all_slots));
  }

  // Compose the slot to write: caller-supplied data with two fields
  // overridden per spec.
  RandoSidecarSlot s = *in;
  s.header.last_vanilla_write_version = (uint16)kGeneratorVersion;
  s.header.sram_slot_checksum_at_last_write =
      RandoSave_ComputeSramSlotChecksum(paired_sram_slot, paired_sram_slot_size);

  // Defensive: ensure placement_count and placement_table_size are
  // consistent with the in-memory sparse list bounds. If
  // placement_table_size implies more locations than the static array can
  // hold, refuse — caller-side bug. An ODD byte size is likewise a caller
  // bug (the table is a uint16[]); RandoSave_WriteFile would refuse it too,
  // but failing here names the offending slot write (FIX #13).
  if (s.header.placement_table_size & 1) return false;
  uint32 loc_count = (uint32)s.header.placement_table_size / 2u;
  if (loc_count > (sizeof(s.placements) / sizeof(s.placements[0]))) {
    return false;
  }

  all_slots[slot_index] = s;

  return RandoSave_CommitPairedSlot(slot_index, all_slots, paired_sram_slot,
                                    paired_sram_slot_size);
}

// ---------------------------------------------------------------------------
// Drift-detection helpers (tasks.md §8.5, §8.6). Pure; no I/O.
//
// NULL-input policy: both helpers return false ("no drift signal") on NULL
// inputs. This is intentional — the load path that consults these only does
// so AFTER `Rando_LoadSidecarSlot` returned true (which guarantees the
// header is populated and a paired sram slot is available). A NULL on
// either input therefore means the caller already detected a load failure
// upstream and is calling these helpers in a defensive path that doesn't
// need a drift warning on top — the load failure itself is the signal.
// Callers MUST NOT rely on these helpers to detect missing data; they only
// compare two pieces of already-loaded state.
// ---------------------------------------------------------------------------
bool Rando_DetectVersionDrift(const RandoSlotHeader *hdr,
                              uint16 current_generator_version) {
  if (hdr == NULL) return false;
  return hdr->generator_version != current_generator_version;
}

bool Rando_DetectChecksumDrift(const RandoSlotHeader *hdr,
                               const uint8 *paired_sram_slot,
                               uint32 paired_sram_slot_size) {
  if (hdr == NULL || paired_sram_slot == NULL) return false;
  // Smaller-than-0x4fe buffers can't be checksummed; treat as "no signal".
  // (A truncated sram.dat is a load-path failure detected upstream; this
  // helper isn't the place to surface it as drift.)
  if (paired_sram_slot_size < 0x4fe) return false;
  uint32 current = RandoSave_ComputeSramSlotChecksum(paired_sram_slot,
                                                     paired_sram_slot_size);
  return current != hdr->sram_slot_checksum_at_last_write;
}

// ---------------------------------------------------------------------------
// Self-check
// ---------------------------------------------------------------------------
static void selfcheck_die(const char *msg) {
  fprintf(stderr, "[RandoSave_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

void RandoSave_SelfCheck(void) {
  // Round-trip one slot via serialize → deserialize.
  RandoSidecarSlot src, dst;
  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));

  src.header.slot_kind = kSlotKind_Randomizer;
  src.header.generator_version = 0x1234;
  for (int i = 0; i < 16; i++) src.header.settings_hash[i] = (uint8)(i * 17);
  for (int i = 0; i < 32; i++) src.header.share_string[i] = (uint8)('A' + (i % 26));
  src.header.last_vanilla_write_version = 0xABCD;
  src.header.sram_slot_checksum_at_last_write = 0xDEADBEEF;
  // placement_table_size is in BYTES per spec. We want 3 placements at
  // location_ids 5, 10, 20 — so we need a flat table covering loc_ids 0..20
  // (21 entries × 2 bytes = 42 bytes).
  src.header.placement_table_size = 42;
  src.header.flags = 0x42;
  src.header.mushroom_held = 0x01;
  // Phase B hints settings extension round-trip coverage.
  src.header.settings_ext_present = 1;
  src.header.hints_setting = 1;   // kHintsMode_Balanced
  src.header.goal = 4;            // kGoal_TriforceHunt
  src.header.world_state = 2;     // kWorldState_Inverted
  src.header.flute_shovel_owned = 0x05;  // shovel + flute-active (distinct from mushroom 0x01)
  // Current-format canonical settings blob round-trip coverage.
  src.header.settings_present = 1;
  for (int i = 0; i < kSettingsCanonicalLen; i++) src.settings_canonical[i] = (uint8)(0xC0 + i);
  // Phase C entrance shuffle round-trip coverage (@71/@72).
  src.header.entrance_axes = 0x05;       // cave + coupled bits (distinct value)
  src.header.entrance_attempt = 0x03;
  // Boomerang/bow ownership round-trip coverage (@73/@74).
  src.header.boomerang_owned = 0x03;     // blue + red
  src.header.bow_owned = 0x03;           // wood + silver
  // FIX #6 prize/medallion accepted-attempt round-trip coverage (@75).
  src.header.prize_attempt = 0x07;       // distinct non-zero attempt index
  // FIX #4 entrance-layout digest round-trip coverage (format_version 3
  // extension block, @0-2 after the settings blob).
  src.header.entrance_digest24 = 0xABCDEF;
  // Per-slot gameplay feature snapshot round-trip coverage (v3 extension
  // block, @3-7 after the settings blob).
  src.header.recommended_features0 = 0xA5A55A5Au;
  src.header.recommended_features0_present = 1;
  // add-rando-pot-sanity registry identity round-trip coverage (v4 extension
  // block, @8-14 after the settings blob).
  src.header.pot_registry_digest = 0x12345678u;
  src.header.pot_registry_count = 0x0456u;
  src.header.pot_registry_present = 1;
  // add-rando-bonk-sanity (review F5): non-zero v11 fields so the @51-57
  // round-trip assert below is not vacuous.
  src.header.bonk_registry_digest = 0xB0BAF00Du;
  src.header.bonk_registry_count = 10;
  src.header.bonk_registry_present = 1;
  // dungeon-chains layout identity round-trip coverage (v5 extension block).
  src.header.chains_present = 1;
  src.header.chains_attempt = 0x09;
  src.header.chains_digest24 = 0x654321u;
  // add-rando-ow-warp-shuffle layout identity round-trip (v12 ext @58-61).
  src.header.ow_attempt = 0x05;
  src.header.ow_digest24 = 0xABCDEFu;
  // Hints v2 plan identity/discovery round-trip (v14 ext @238-277). Set the
  // reserved discovery byte nonzero deliberately: the serializer/reader must
  // mask bits 24..31 rather than treating them as gameplay state.
  src.header.hint_state.algorithm_version = 0x1234u;
  src.header.hint_state.text_schema_version = 0x5678u;
  for (uint8 i = 0; i < kRandoHintPlanDigestBytes; i++)
    src.header.hint_state.plan_digest[i] = (uint8)(0x80u + i);
  src.header.hint_state.discovered[0] = 0xA5u;
  src.header.hint_state.discovered[1] = 0x5Au;
  src.header.hint_state.discovered[2] = 0xC3u;
  src.header.hint_state.discovered[3] = 0xFFu;  // reserved, must serialize as 0
  // add-enemy-souls soul-ownership round-trip coverage (v6 extension block).
  src.header.souls_present = 1;
  for (int i = 0; i < 12; i++) src.header.soul_flags[i] = (uint8)(0x81 + i);
  // tracker-player-knowledge discovery round-trip coverage (v13 ext @62-81).
  src.header.discovered_dungeons = 0x1235;      // distinct bit pattern
  src.header.discovered_whirlpools = 0x2C;
  for (int i = 0; i < 8; i++) src.header.discovered_caves[i] = (uint8)(0x11 * (i + 1));
  for (int i = 0; i < 8; i++) src.header.discovered_exits[i] = (uint8)(0xF0 - i);
  // Key Ring/Skeleton ownership is derived from these ordinary persisted rows
  // plus the checked bitmap, so keep both new item identities in the generic
  // sidecar round-trip fixture rather than inventing separate save fields.
  src.placements[0].location_id = 0;
  src.placements[0].item_id = ITEM_KeyRing_HyruleCastleEscape;
  src.placements[1].location_id = 2;
  src.placements[1].item_id = ITEM_SkeletonKey;
  src.placements[2].location_id = 5;  src.placements[2].item_id = 50;
  src.placements[3].location_id = 10; src.placements[3].item_id = 75;
  src.placements[4].location_id = 20; src.placements[4].item_id = 99;
  src.placement_count = 5;
  src.checked_bitmap[0] = 0x05;  // checked Key Ring (loc 0) + Skeleton Key (loc 2)

  uint8 buf[512];  // v14 ext (278B) puts the fixture slot at ~434 bytes
  uint32 wrote = RandoSave_SerializeSlot(&src, buf, sizeof(buf));
  if (wrote == 0) selfcheck_die("serialize returned 0");
  if (wrote != RandoSave_SlotOnDiskSize(src.header.placement_table_size)) {
    selfcheck_die("serialize size mismatch");
  }

  // Spot-check byte layout per spec offsets.
  if (get_u32le(buf + 0) != kRandoSidecar_SlotMagic) selfcheck_die("slot magic wrong");
  if (buf[4] != kSlotKind_Randomizer) selfcheck_die("slot_kind at @4 wrong");
  if (get_u16le(buf + 5) != 0x1234) selfcheck_die("generator_version at @5 wrong");
  if (get_u16le(buf + 55) != 0xABCD) selfcheck_die("last_vanilla_write_version at @55 wrong");
  if (get_u32le(buf + 57) != 0xDEADBEEF) selfcheck_die("sram_checksum at @57 wrong");
  if (get_u16le(buf + 61) != 42) selfcheck_die("placement_table_size at @61 wrong");
  if (buf[63] != 0x42) selfcheck_die("flags at @63 wrong");
  if (buf[64] != 0x01) selfcheck_die("mushroom_held at @64 wrong");
  // Phase B hints settings extension byte layout @65-67.
  if (buf[65] != 1) selfcheck_die("settings_ext_present at @65 wrong");
  if (buf[66] != 1) selfcheck_die("hints_setting at @66 wrong");
  if (buf[67] != 4) selfcheck_die("goal at @67 wrong");
  // Phase B Inverted runtime: world_state byte layout @68.
  if (buf[68] != 2) selfcheck_die("world_state at @68 wrong");
  // Rando flute/shovel decouple: flute_shovel_owned byte layout @69.
  if (buf[69] != 0x05) selfcheck_die("flute_shovel_owned at @69 wrong");
  // format_version 2: settings_present @70, and the canonical blob trails the
  // bitmap at offset (base size = header + placements + bitmap). format_version
  // 5: the extension block (entrance_digest24 LE @0-2, pot registry @8-14,
  // dungeon chains @16-20) trails the blob.
  if (buf[70] != 1) selfcheck_die("settings_present at @70 wrong");
  {
    uint32 blob = RandoSave_SlotOnDiskSize(src.header.placement_table_size)
                  - kSettingsCanonicalLen - kRandoSidecar_SlotExtCurrentSize;
    if (buf[blob] != 0xC0) selfcheck_die("settings_canonical blob not at expected offset");
    if (buf[blob + kSettingsCanonicalLen - 1] != (uint8)(0xC0 + kSettingsCanonicalLen - 1))
      selfcheck_die("settings_canonical blob tail wrong");
    uint32 ext = blob + kSettingsCanonicalLen;
    if (buf[ext] != 0xEF || buf[ext + 1] != 0xCD || buf[ext + 2] != 0xAB)
      selfcheck_die("entrance_digest24 not at expected v3 ext offset (LE)");
    if (buf[ext + 3] != 0x5A || buf[ext + 4] != 0x5A ||
        buf[ext + 5] != 0xA5 || buf[ext + 6] != 0xA5 ||
        buf[ext + 7] != 1)
      selfcheck_die("recommended_features0 not at expected v3 ext offset");
    if (get_u32le(buf + ext + 8) != 0x12345678u ||
        get_u16le(buf + ext + 12) != 0x0456u ||
        buf[ext + 14] != 1)
      selfcheck_die("pot registry identity not at expected v4 ext offset");
    if (buf[ext + 15] != 0)
      selfcheck_die("v4 ext reserved byte not zero");
    if (buf[ext + 16] != 1 || buf[ext + 17] != 0x09 ||
        buf[ext + 18] != 0x21 || buf[ext + 19] != 0x43 ||
        buf[ext + 20] != 0x65)
      selfcheck_die("dungeon chains identity not at expected v5 ext offset");
    if (buf[ext + 21] != 0 || buf[ext + 22] != 0 || buf[ext + 23] != 0)
      selfcheck_die("v5 ext reserved bytes not zero");
    // tracker-player-knowledge: discovery block byte layout (v13 ext @62-81).
    if (get_u16le(buf + ext + 62) != 0x1235)
      selfcheck_die("discovered_dungeons not at expected v13 ext offset (LE)");
    if (buf[ext + 64] != 0x2C)
      selfcheck_die("discovered_whirlpools not at expected v13 ext offset");
    if (buf[ext + 65] != 0) selfcheck_die("v13 ext reserved @65 not zero");
    if (buf[ext + 66] != 0x11 || buf[ext + 73] != 0x88)
      selfcheck_die("discovered_caves not at expected v13 ext offset");
    if (buf[ext + 74] != 0xF0 || buf[ext + 81] != 0xE9)
      selfcheck_die("discovered_exits not at expected v13 ext offset");
    // Reserved door-discovery bitmap @82-231 + tail @232-237 must be zero.
    for (int i = 82; i < (int)kRandoSidecar_SlotExtV13Size; i++) {
      if (buf[ext + i] != 0)
        selfcheck_die("v13 ext reserved door/tail bytes not zero");
    }
    if (get_u16le(buf + ext + 238) != 0x1234u ||
        get_u16le(buf + ext + 240) != 0x5678u)
      selfcheck_die("Hints v2 versions not at expected v14 ext offsets");
    for (uint8 i = 0; i < kRandoHintPlanDigestBytes; i++) {
      if (buf[ext + 242 + i] != (uint8)(0x80u + i))
        selfcheck_die("Hints v2 digest not at expected v14 ext offset");
    }
    if (buf[ext + 274] != 0xA5u || buf[ext + 275] != 0x5Au ||
        buf[ext + 276] != 0xC3u || buf[ext + 277] != 0)
      selfcheck_die("Hints v2 discovery mask/layout wrong at v14 ext offset");
  }
  // Phase C entrance shuffle: entrance_axes @71, entrance_attempt @72.
  if (buf[71] != 0x05) selfcheck_die("entrance_axes at @71 wrong");
  if (buf[72] != 0x03) selfcheck_die("entrance_attempt at @72 wrong");
  if (buf[75] != 0x07) selfcheck_die("prize_attempt at @75 wrong");
  // Flat table layout check: location 5 should hold item 50.
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 5 * 2) != 50)
    selfcheck_die("flat table: loc 5 item slot wrong");
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 10 * 2) != 75)
    selfcheck_die("flat table: loc 10 item slot wrong");
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 20 * 2) != 99)
    selfcheck_die("flat table: loc 20 item slot wrong");
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 0 * 2) !=
          ITEM_KeyRing_HyruleCastleEscape)
    selfcheck_die("flat table: checked Key Ring slot wrong");
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 2 * 2) != ITEM_SkeletonKey)
    selfcheck_die("flat table: checked Skeleton Key slot wrong");
  if (get_u16le(buf + kRandoSidecar_SlotHeaderSize + 1 * 2) !=
          kRandoSidecar_NoPlacementSentinel)
    selfcheck_die("flat table: loc 1 should be sentinel");

  uint32 used = RandoSave_DeserializeSlot(buf, wrote, &dst);
  if (used != wrote) selfcheck_die("deserialize used != serialize wrote");

  // Compare round-trip fields.
  if (dst.header.ow_attempt != src.header.ow_attempt ||
      dst.header.ow_digest24 != src.header.ow_digest24)
    selfcheck_die("ow warp identity round-trip (v12 ext)");
  if (dst.header.discovered_dungeons != src.header.discovered_dungeons ||
      dst.header.discovered_whirlpools != src.header.discovered_whirlpools ||
      memcmp(dst.header.discovered_caves, src.header.discovered_caves,
             sizeof(src.header.discovered_caves)) != 0 ||
      memcmp(dst.header.discovered_exits, src.header.discovered_exits,
             sizeof(src.header.discovered_exits)) != 0)
    selfcheck_die("discovery state round-trip (v13 ext)");
  if (dst.header.hint_state.algorithm_version !=
          src.header.hint_state.algorithm_version ||
      dst.header.hint_state.text_schema_version !=
          src.header.hint_state.text_schema_version ||
      memcmp(dst.header.hint_state.plan_digest,
             src.header.hint_state.plan_digest,
             kRandoHintPlanDigestBytes) != 0 ||
      memcmp(dst.header.hint_state.discovered,
             src.header.hint_state.discovered, 3) != 0 ||
      dst.header.hint_state.discovered[3] != 0)
    selfcheck_die("Hints v2 identity/discovery round-trip (v14 ext)");
  if (dst.header.slot_kind != src.header.slot_kind) selfcheck_die("slot_kind round-trip");
  if (dst.header.generator_version != src.header.generator_version) selfcheck_die("gen_version round-trip");
  if (memcmp(dst.header.settings_hash, src.header.settings_hash, 16) != 0) selfcheck_die("settings_hash round-trip");
  if (memcmp(dst.header.share_string, src.header.share_string, kRandoSidecar_ShareStringLength) != 0) selfcheck_die("share_string round-trip");
  if (dst.header.last_vanilla_write_version != src.header.last_vanilla_write_version) selfcheck_die("last_vanilla_write_version round-trip");
  if (dst.header.sram_slot_checksum_at_last_write != src.header.sram_slot_checksum_at_last_write) selfcheck_die("sram_checksum round-trip");
  if (dst.header.placement_table_size != src.header.placement_table_size) selfcheck_die("placement_table_size round-trip");
  if (dst.header.flags != src.header.flags) selfcheck_die("flags round-trip");
  if (dst.header.mushroom_held != src.header.mushroom_held) selfcheck_die("mushroom_held round-trip");
  if (dst.header.settings_ext_present != src.header.settings_ext_present) selfcheck_die("settings_ext_present round-trip");
  if (dst.header.hints_setting != src.header.hints_setting) selfcheck_die("hints_setting round-trip");
  if (dst.header.goal != src.header.goal) selfcheck_die("goal round-trip");
  if (dst.header.world_state != src.header.world_state) selfcheck_die("world_state round-trip");
  if (dst.header.flute_shovel_owned != src.header.flute_shovel_owned) selfcheck_die("flute_shovel_owned round-trip");
  if (dst.header.settings_present != src.header.settings_present) selfcheck_die("settings_present round-trip");
  if (memcmp(dst.settings_canonical, src.settings_canonical, kSettingsCanonicalLen) != 0) selfcheck_die("settings_canonical round-trip");
  if (dst.header.entrance_axes != src.header.entrance_axes) selfcheck_die("entrance_axes round-trip");
  if (dst.header.entrance_attempt != src.header.entrance_attempt) selfcheck_die("entrance_attempt round-trip");
  if (dst.header.boomerang_owned != src.header.boomerang_owned) selfcheck_die("boomerang_owned round-trip");
  if (dst.header.bow_owned != src.header.bow_owned) selfcheck_die("bow_owned round-trip");
  if (dst.header.prize_attempt != src.header.prize_attempt) selfcheck_die("prize_attempt round-trip");
  if (dst.header.entrance_digest24 != src.header.entrance_digest24) selfcheck_die("entrance_digest24 round-trip");
  if (dst.header.recommended_features0 != src.header.recommended_features0) selfcheck_die("recommended_features0 round-trip");
  if (dst.header.recommended_features0_present != src.header.recommended_features0_present) selfcheck_die("recommended_features0_present round-trip");
  if (dst.header.pot_registry_digest != src.header.pot_registry_digest) selfcheck_die("pot_registry_digest round-trip");
  if (dst.header.pot_registry_count != src.header.pot_registry_count) selfcheck_die("pot_registry_count round-trip");
  if (dst.header.pot_registry_present != src.header.pot_registry_present) selfcheck_die("pot_registry_present round-trip");
  if (dst.header.chains_present != src.header.chains_present) selfcheck_die("chains_present round-trip");
  if (dst.header.chains_attempt != src.header.chains_attempt) selfcheck_die("chains_attempt round-trip");
  if (dst.header.chains_digest24 != src.header.chains_digest24) selfcheck_die("chains_digest24 round-trip");
  if (dst.header.souls_present != src.header.souls_present) selfcheck_die("souls_present round-trip");
  if (memcmp(dst.header.soul_flags, src.header.soul_flags, sizeof(src.header.soul_flags)) != 0) selfcheck_die("soul_flags round-trip");
  if (dst.placement_count != src.placement_count) selfcheck_die("placement_count round-trip");
  // After deserialization the sparse list is sorted by location_id (because
  // we scatter+gather over the dense array).
  if (dst.placements[0].location_id != 0 ||
      dst.placements[0].item_id != ITEM_KeyRing_HyruleCastleEscape)
    selfcheck_die("checked Key Ring placement round-trip");
  if (dst.placements[1].location_id != 2 ||
      dst.placements[1].item_id != ITEM_SkeletonKey)
    selfcheck_die("checked Skeleton Key placement round-trip");
  if (dst.placements[2].location_id != 5  || dst.placements[2].item_id != 50) selfcheck_die("placements[2] round-trip");
  if (dst.placements[3].location_id != 10 || dst.placements[3].item_id != 75) selfcheck_die("placements[3] round-trip");
  if (dst.placements[4].location_id != 20 || dst.placements[4].item_id != 99) selfcheck_die("placements[4] round-trip");
  if (dst.checked_bitmap[0] != src.checked_bitmap[0]) selfcheck_die("checked_bitmap round-trip");

  // Bad-magic case → deserialize returns 0.
  buf[0] = 0;
  RandoSidecarSlot trash;
  if (RandoSave_DeserializeSlot(buf, wrote, &trash) != 0) {
    selfcheck_die("deserialize should reject bad magic");
  }

  // File header round-trip.
  RandoSidecarFileHeader fh = { kRandoSidecar_FileFormatVersion, kRandoSidecar_SlotCount, 0xCAFEBABEu };
  uint8 fhbuf[16];
  if (RandoSave_SerializeFileHeader(&fh, fhbuf, sizeof(fhbuf)) != 16) selfcheck_die("file header serialize");
  RandoSidecarFileHeader fh2;
  if (RandoSave_DeserializeFileHeader(fhbuf, sizeof(fhbuf), &fh2) != 16) selfcheck_die("file header deserialize");
  if (fh2.format_version != fh.format_version) selfcheck_die("file format_version round-trip");
  if (fh2.slot_count != fh.slot_count) selfcheck_die("file slot_count round-trip");
  if (fh2.file_crc != fh.file_crc) selfcheck_die("file_crc round-trip");

  // SRAM-slot checksum sanity: mirror the messaging.c routine on a known
  // pattern. For a zeroed slot, t = 0x5a5a - 0 - 0 - ... = 0x5a5a.
  {
    uint8 zeroed[0x500] = { 0 };
    if (RandoSave_ComputeSramSlotChecksum(zeroed, sizeof(zeroed)) != 0x5a5au) {
      selfcheck_die("checksum on zeroed slot != 0x5a5a");
    }
    // A slot with just byte 0 = 0x01 (so u16 LE at offset 0 = 0x0001) →
    // t = 0x5a5a - 0x0001 = 0x5a59.
    uint8 patterned[0x500] = { 0 };
    patterned[0] = 0x01;
    if (RandoSave_ComputeSramSlotChecksum(patterned, sizeof(patterned)) != 0x5a59u) {
      selfcheck_die("checksum on patterned slot wrong");
    }
    // Too-small buffer rejects with 0.
    if (RandoSave_ComputeSramSlotChecksum(zeroed, 100) != 0) {
      selfcheck_die("checksum on tiny buffer should be 0");
    }
  }

  // Rando_WriteSidecarSlot / Rando_LoadSidecarSlot round-trip via a temp
  // file path. Build a synthetic slot, write to a temp dir, load back,
  // compare byte-by-byte (against the spec's promise that round-trip is
  // exact). We bypass the production "saves/sram_rando.dat" path by
  // exercising RandoSave_WriteFile / RandoSave_ReadFile directly with a
  // tmpfile-derived path — keeps the self-check hermetic.
  //
  // (We can't reliably write to "saves/" in CI from --rando-selftest, so
  // exercise the serialize→deserialize→compare path which is the same data
  // flow as Rando_LoadSidecarSlot uses internally.)
  {
    RandoSidecarSlot slots[kRandoSidecar_SlotCount];
    memset(slots, 0, sizeof(slots));
    slots[0].header.slot_kind = kSlotKind_Empty;
    slots[1] = src;  // populated above
    slots[2].header.slot_kind = kSlotKind_Vanilla;

    // Compute total size.
    uint32 total = kRandoSidecar_FileHeaderSize;
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      total += RandoSave_SlotOnDiskSize(slots[i].header.placement_table_size);
    }
    uint8 *bigbuf = (uint8 *)malloc(total);
    if (bigbuf == NULL) selfcheck_die("malloc for round-trip buffer");
    RandoSidecarFileHeader hdr = { kRandoSidecar_FileFormatVersion,
                                   kRandoSidecar_SlotCount, 0 };
    uint32 off = RandoSave_SerializeFileHeader(&hdr, bigbuf, total);
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      uint32 used = RandoSave_SerializeSlot(&slots[i], bigbuf + off, total - off);
      if (used == 0) { free(bigbuf); selfcheck_die("serialize slot for round-trip"); }
      off += used;
    }
    if (off != total) { free(bigbuf); selfcheck_die("round-trip size mismatch"); }

    RandoSidecarSlot file_back[kRandoSidecar_SlotCount];
    if (!deserialize_file_buffer(bigbuf, total, file_back)) {
      free(bigbuf); selfcheck_die("current full-file buffer must deserialize");
    }
    // A sequential-body reader must reject both newer versions and any
    // otherwise-valid file with an unconsumed tail.
    uint8 *with_tail = (uint8 *)malloc(total + 1);
    if (with_tail == NULL) { free(bigbuf); selfcheck_die("malloc for trailing-byte test"); }
    memcpy(with_tail, bigbuf, total);
    with_tail[total] = 0;
    // Give the enlarged buffer a valid nonzero CRC so only the exact
    // full-consumption check can reject it (not the earlier CRC gate).
    put_u32le(with_tail + 8,
              rs_crc32(with_tail + kRandoSidecar_FileHeaderSize,
                       total + 1 - kRandoSidecar_FileHeaderSize));
    if (deserialize_file_buffer(with_tail, total + 1, file_back)) {
      free(with_tail); free(bigbuf);
      selfcheck_die("sidecar with trailing bytes must be refused");
    }
    free(with_tail);
    uint8 saved_vlo = bigbuf[4], saved_vhi = bigbuf[5];
    put_u16le(bigbuf + 4, (uint16)(kRandoSidecar_FileFormatVersion + 1));
    if (deserialize_file_buffer(bigbuf, total, file_back)) {
      free(bigbuf); selfcheck_die("future sidecar version must be refused");
    }
    bigbuf[4] = saved_vlo;
    bigbuf[5] = saved_vhi;

    // Read back via the same path the deserializer uses.
    RandoSidecarFileHeader hdr2;
    uint32 used = RandoSave_DeserializeFileHeader(bigbuf, total, &hdr2);
    if (used != kRandoSidecar_FileHeaderSize) {
      free(bigbuf); selfcheck_die("file header deserialize for round-trip");
    }
    uint32 cur = used;
    RandoSidecarSlot back[kRandoSidecar_SlotCount];
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      uint32 u = RandoSave_DeserializeSlot(bigbuf + cur, total - cur, &back[i]);
      if (u == 0) { free(bigbuf); selfcheck_die("slot deserialize for round-trip"); }
      cur += u;
    }
    free(bigbuf);

    // Byte-equality on the populated slot.
    if (back[1].header.slot_kind != slots[1].header.slot_kind ||
        back[1].header.generator_version != slots[1].header.generator_version ||
        memcmp(back[1].header.settings_hash, slots[1].header.settings_hash, 16) != 0 ||
        memcmp(back[1].header.share_string, slots[1].header.share_string, 32) != 0 ||
        back[1].header.last_vanilla_write_version != slots[1].header.last_vanilla_write_version ||
        back[1].header.sram_slot_checksum_at_last_write != slots[1].header.sram_slot_checksum_at_last_write ||
        back[1].header.placement_table_size != slots[1].header.placement_table_size ||
        back[1].header.flags != slots[1].header.flags ||
        back[1].placement_count != slots[1].placement_count) {
      selfcheck_die("full-file round-trip slot header mismatch");
    }
    for (uint16 i = 0; i < slots[1].placement_count; i++) {
      if (back[1].placements[i].location_id != slots[1].placements[i].location_id ||
          back[1].placements[i].item_id != slots[1].placements[i].item_id) {
        selfcheck_die("full-file round-trip placement mismatch");
      }
    }
    if (memcmp(back[1].checked_bitmap, slots[1].checked_bitmap,
               sizeof(slots[1].checked_bitmap)) != 0) {
      selfcheck_die("full-file round-trip bitmap mismatch");
    }
    if (back[1].header.settings_present != slots[1].header.settings_present ||
        memcmp(back[1].settings_canonical, slots[1].settings_canonical,
               kSettingsCanonicalLen) != 0) {
      selfcheck_die("full-file round-trip settings blob mismatch");
    }
  }

  // format_version 13 compatibility: retain main's complete topology-discovery
  // block while zeroing the not-yet-present Hints v2 identity/discovery.
  {
    uint8 current[512], v13buf[512];
    uint32 current_used = RandoSave_SerializeSlot(&src, current, sizeof(current));
    if (current_used == 0) selfcheck_die("v13 compat: serialize current slot");
    uint32 base = slot_on_disk_size_base(src.header.placement_table_size);
    uint32 v13_total = base + kSettingsCanonicalLen +
                       kRandoSidecar_SlotExtV13Size;
    memcpy(v13buf, current, v13_total);
    RandoSidecarSlot v13dst;
    uint32 v13_used =
        deserialize_slot_versioned(v13buf, v13_total, &v13dst, 13);
    if (v13_used != v13_total)
      selfcheck_die("v13 compat: used != v13 total");
    if (v13dst.header.discovered_dungeons !=
            src.header.discovered_dungeons ||
        v13dst.header.discovered_whirlpools !=
            src.header.discovered_whirlpools ||
        memcmp(v13dst.header.discovered_caves,
               src.header.discovered_caves,
               sizeof(src.header.discovered_caves)) != 0 ||
        memcmp(v13dst.header.discovered_exits,
               src.header.discovered_exits,
               sizeof(src.header.discovered_exits)) != 0)
      selfcheck_die("v13 compat: topology discovery lost");
    {
      const uint8 *hint_bytes = (const uint8 *)&v13dst.header.hint_state;
      for (size_t i = 0; i < sizeof(v13dst.header.hint_state); i++) {
        if (hint_bytes[i] != 0)
          selfcheck_die("v13 compat: Hints v2 metadata must be absent/zero");
      }
    }
  }

  // format_version 12 compatibility: the slot ends immediately after the OW
  // warp identity at extension byte 61. It must retain every v12 field while
  // zeroing both the v13 topology discovery and absent Hints v2 metadata.
  {
    uint8 current[512], v12buf[512];
    uint32 current_used = RandoSave_SerializeSlot(&src, current, sizeof(current));
    if (current_used == 0) selfcheck_die("v12 compat: serialize current slot");
    uint32 base = slot_on_disk_size_base(src.header.placement_table_size);
    uint32 v12_total = base + kSettingsCanonicalLen +
                       kRandoSidecar_SlotExtV12Size;
    memcpy(v12buf, current, v12_total);
    RandoSidecarSlot v12dst;
    uint32 v12_used =
        deserialize_slot_versioned(v12buf, v12_total, &v12dst, 12);
    if (v12_used != v12_total)
      selfcheck_die("v12 compat: used != v12 total");
    if (v12dst.header.ow_attempt != src.header.ow_attempt ||
        v12dst.header.ow_digest24 != src.header.ow_digest24)
      selfcheck_die("v12 compat: OW warp identity lost");
    if (v12dst.header.discovered_dungeons != 0 ||
        v12dst.header.discovered_whirlpools != 0 ||
        !all_zero_bytes(v12dst.header.discovered_caves,
                        sizeof(v12dst.header.discovered_caves)) ||
        !all_zero_bytes(v12dst.header.discovered_exits,
                        sizeof(v12dst.header.discovered_exits)))
      selfcheck_die("v12 compat: topology discovery must be absent/zero");
    {
      const uint8 *hint_bytes = (const uint8 *)&v12dst.header.hint_state;
      for (size_t i = 0; i < sizeof(v12dst.header.hint_state); i++) {
        if (hint_bytes[i] != 0)
          selfcheck_die("v12 compat: Hints v2 metadata must be absent/zero");
      }
    }
  }

  // format_version 9 compatibility: the audit-era slot has a 30-byte
  // canonical settings blob followed by the 51-byte enemy-registry extension.
  // The appended key-rings byte must zero-extend to Off while every v9
  // extension field remains intact.
  {
    uint8 current[512], v9buf[256];  // current holds a v14-size serialize
    uint32 current_used = RandoSave_SerializeSlot(&src, current, sizeof(current));
    if (current_used == 0) selfcheck_die("v9 compat: serialize current slot");
    uint32 base = slot_on_disk_size_base(src.header.placement_table_size);
    uint32 v9_total = base + kRandoSidecar_LegacySettingsCanonicalLen30 +
                      kRandoSidecar_SlotExtV9Size;
    memcpy(v9buf, current, base + kRandoSidecar_LegacySettingsCanonicalLen30);
    memcpy(v9buf + base + kRandoSidecar_LegacySettingsCanonicalLen30,
           current + base + kSettingsCanonicalLen,
           kRandoSidecar_SlotExtV9Size);
    RandoSidecarSlot v9dst;
    uint32 v9_used = deserialize_slot_versioned(v9buf, v9_total, &v9dst, 9);
    if (v9_used != v9_total) selfcheck_die("v9 compat: used != v9 total");
    if (memcmp(v9dst.settings_canonical, src.settings_canonical,
               kRandoSidecar_LegacySettingsCanonicalLen30) != 0 ||
        v9dst.settings_canonical[30] != 0)
      selfcheck_die("v9 compat: settings must zero-extend key-rings byte");
    if (v9dst.header.terrain_registry_present != src.header.terrain_registry_present ||
        v9dst.header.terrain_registry_digest != src.header.terrain_registry_digest ||
        v9dst.header.terrain_registry_count != src.header.terrain_registry_count ||
        v9dst.header.enemy_check_registry_present !=
            src.header.enemy_check_registry_present ||
        v9dst.header.enemy_check_registry_digest !=
            src.header.enemy_check_registry_digest ||
        v9dst.header.enemy_check_registry_count !=
            src.header.enemy_check_registry_count)
      selfcheck_die("v9 compat: registry extension round-trip");
    // add-rando-bonk-sanity (review F5): the v11 bonk fields @51-57 must
    // survive a CURRENT-version serialize/deserialize (the v9 slice above
    // deliberately truncates them, so nothing else asserted the offsets).
    {
      uint8 cur2[512];
      RandoSidecarSlot v11dst;
      uint32 used2 = RandoSave_SerializeSlot(&src, cur2, sizeof(cur2));
      if (used2 == 0 ||
          RandoSave_DeserializeSlot(cur2, used2, &v11dst) != used2 ||
          v11dst.header.bonk_registry_present != src.header.bonk_registry_present ||
          v11dst.header.bonk_registry_digest != src.header.bonk_registry_digest ||
          v11dst.header.bonk_registry_count != src.header.bonk_registry_count)
        selfcheck_die("v11 bonk registry extension round-trip");
    }
    // tracker-player-knowledge: a v12 file (62-byte ext, no discovery block)
    // must load with all discovery state zero (the loader then backfills from
    // checked state) while every v12 field stays intact.
    {
      uint8 v12buf[512];
      uint32 base12 = slot_on_disk_size_base(src.header.placement_table_size);
      uint32 v12_total = base12 + kSettingsCanonicalLen + kRandoSidecar_SlotExtV12Size;
      memcpy(v12buf, current, base12 + kSettingsCanonicalLen + kRandoSidecar_SlotExtV12Size);
      RandoSidecarSlot v12dst;
      uint32 v12_used = deserialize_slot_versioned(v12buf, v12_total, &v12dst, 12);
      if (v12_used != v12_total) selfcheck_die("v12 compat: used != v12 total");
      if (v12dst.header.ow_attempt != src.header.ow_attempt ||
          v12dst.header.ow_digest24 != src.header.ow_digest24 ||
          v12dst.header.bonk_registry_present != src.header.bonk_registry_present)
        selfcheck_die("v12 compat: v12 ext fields must survive");
      if (v12dst.header.discovered_dungeons != 0 ||
          v12dst.header.discovered_whirlpools != 0 ||
          v12dst.header.discovered_caves[0] != 0 || v12dst.header.discovered_caves[7] != 0 ||
          v12dst.header.discovered_exits[0] != 0 || v12dst.header.discovered_exits[7] != 0)
        selfcheck_die("v12 compat: discovery must read as all-zero");
    }
  }

  // -------------------------------------------------------------------------
  // format_version 1 backward-compat: a v1 slot has no trailing settings blob.
  // Serialize the body WITHOUT the blob (base size), then deserialize with
  // with_settings=false (the path RandoSave_ReadFile takes for a v1 file) and
  // confirm it loads with settings_present forced off and the placement/bitmap
  // intact. Guards the older-file load path the version gate protects.
  // -------------------------------------------------------------------------
  {
    // Build a v1-style buffer by hand: header (with @70 deliberately nonzero to
    // prove the loader forces it off), flat table, bitmap — and NO blob.
    uint8 v1buf[256];
    memset(v1buf, 0, sizeof(v1buf));
    serialize_slot_header(&src.header, v1buf);  // writes settings_present=1 @70
    uint32 loc_count = (uint32)src.header.placement_table_size / 2;
    uint8 *p = v1buf + kRandoSidecar_SlotHeaderSize;
    for (uint32 i = 0; i < loc_count; i++) put_u16le(p + i * 2, kRandoSidecar_NoPlacementSentinel);
    put_u16le(p + 5 * 2, 50);
    p += loc_count * 2;
    uint32 v1_bitmap = (loc_count + 7) >> 3;
    p[0] = 0x05;  // matches src.checked_bitmap[0]
    uint32 v1_total = kRandoSidecar_SlotHeaderSize + loc_count * 2 + v1_bitmap;  // NO blob

    RandoSidecarSlot v1dst;
    uint32 v1_used = deserialize_slot_versioned(v1buf, v1_total, &v1dst, 1);
    if (v1_used != v1_total) selfcheck_die("v1 compat: used != base total (blob must be absent)");
    if (v1dst.header.settings_present != 0) selfcheck_die("v1 compat: settings_present must be forced 0");
    if (v1dst.header.entrance_digest24 != 0) selfcheck_die("v1 compat: entrance_digest24 must be forced 0");
    if (v1dst.header.recommended_features0_present != 0)
      selfcheck_die("v1 compat: recommended_features0_present must be forced 0");
    if (v1dst.header.pot_registry_present != 0 || v1dst.header.pot_registry_digest != 0 ||
        v1dst.header.pot_registry_count != 0)
      selfcheck_die("v1 compat: pot registry fields must be forced 0");
    if (v1dst.header.chains_present != 0 || v1dst.header.chains_attempt != 0 ||
        v1dst.header.chains_digest24 != 0)
      selfcheck_die("v1 compat: dungeon-chain fields must be forced 0");
    if (v1dst.header.souls_present != 0 || v1dst.header.soul_flags[0] != 0 ||
        v1dst.header.soul_flags[11] != 0)
      selfcheck_die("v1 compat: soul fields must be forced 0");
    if (v1dst.placement_count != 1 || v1dst.placements[0].location_id != 5 ||
        v1dst.placements[0].item_id != 50) selfcheck_die("v1 compat: placement round-trip");
    if (v1dst.checked_bitmap[0] != 0x05) selfcheck_die("v1 compat: bitmap round-trip");
  }

  // -------------------------------------------------------------------------
  // format_version 2 backward-compat: a v2 slot has the legacy 28-byte settings
  // blob but NO v3 extension block. Deserialize with format_version=2 (the path
  // RandoSave_ReadFile takes for a v2 file) and confirm the blob prefix loads,
  // the new appended settings byte is zero-extended, and entrance_digest24 is
  // forced 0 (= legacy warn-only entrance behavior).
  // -------------------------------------------------------------------------
  {
    uint8 v2buf[256];
    memset(v2buf, 0, sizeof(v2buf));
    serialize_slot_header(&src.header, v2buf);
    uint32 loc_count = (uint32)src.header.placement_table_size / 2;
    uint8 *p = v2buf + kRandoSidecar_SlotHeaderSize;
    for (uint32 i = 0; i < loc_count; i++) put_u16le(p + i * 2, kRandoSidecar_NoPlacementSentinel);
    put_u16le(p + 5 * 2, 50);
    p += loc_count * 2;
    uint32 v2_bitmap = (loc_count + 7) >> 3;
    p[0] = 0x05;
    p += v2_bitmap;
    memcpy(p, src.settings_canonical, kRandoSidecar_LegacySettingsCanonicalLen28);
    uint32 v2_total = kRandoSidecar_SlotHeaderSize + loc_count * 2 + v2_bitmap +
                      kRandoSidecar_LegacySettingsCanonicalLen28;  // NO ext block

    RandoSidecarSlot v2dst;
    uint32 v2_used = deserialize_slot_versioned(v2buf, v2_total, &v2dst, 2);
    if (v2_used != v2_total) selfcheck_die("v2 compat: used != v2 total (ext must be absent)");
    if (v2dst.header.entrance_digest24 != 0) selfcheck_die("v2 compat: entrance_digest24 must be forced 0");
    if (v2dst.header.recommended_features0_present != 0)
      selfcheck_die("v2 compat: recommended_features0_present must be forced 0");
    if (v2dst.header.pot_registry_present != 0 || v2dst.header.pot_registry_digest != 0 ||
        v2dst.header.pot_registry_count != 0)
      selfcheck_die("v2 compat: pot registry fields must be forced 0");
    if (v2dst.header.chains_present != 0 || v2dst.header.chains_attempt != 0 ||
        v2dst.header.chains_digest24 != 0)
      selfcheck_die("v2 compat: dungeon-chain fields must be forced 0");
    if (v2dst.header.settings_present != 1) selfcheck_die("v2 compat: settings_present round-trip");
    if (memcmp(v2dst.settings_canonical, src.settings_canonical,
               kRandoSidecar_LegacySettingsCanonicalLen28) != 0)
      selfcheck_die("v2 compat: settings blob prefix round-trip");
    if (v2dst.settings_canonical[kRandoSidecar_LegacySettingsCanonicalLen28] != 0)
      selfcheck_die("v2 compat: appended settings byte must zero-extend");
    if (v2dst.placement_count != 1 || v2dst.placements[0].location_id != 5 ||
        v2dst.placements[0].item_id != 50) selfcheck_die("v2 compat: placement round-trip");
    if (v2dst.checked_bitmap[0] != 0x05) selfcheck_die("v2 compat: bitmap round-trip");
  }

  // -------------------------------------------------------------------------
  // format_version 3 backward-compat: v3 slots carry the legacy 28-byte
  // settings blob plus the extension block. Slots written before
  // recommended_features0 existed have zero reserved extension bytes. They must
  // zero-extend enemy_drop_checks and must NOT apply a zero feature snapshot on
  // activation.
  // -------------------------------------------------------------------------
  {
    uint8 v3buf[256];
    memset(v3buf, 0, sizeof(v3buf));
    serialize_slot_header(&src.header, v3buf);
    uint32 loc_count = (uint32)src.header.placement_table_size / 2;
    uint8 *p = v3buf + kRandoSidecar_SlotHeaderSize;
    for (uint32 i = 0; i < loc_count; i++) put_u16le(p + i * 2, kRandoSidecar_NoPlacementSentinel);
    put_u16le(p + 5 * 2, 50);
    p += loc_count * 2;
    uint32 v3_bitmap = (loc_count + 7) >> 3;
    p[0] = 0x05;
    p += v3_bitmap;
    memcpy(p, src.settings_canonical, kRandoSidecar_LegacySettingsCanonicalLen28);
    p += kRandoSidecar_LegacySettingsCanonicalLen28;
    p[0] = 0xEF; p[1] = 0xCD; p[2] = 0xAB;
    p[3] = p[4] = p[5] = p[6] = p[7] = 0;
    uint32 v3_total = kRandoSidecar_SlotHeaderSize + loc_count * 2 + v3_bitmap +
                      kRandoSidecar_LegacySettingsCanonicalLen28 +
                      kRandoSidecar_SlotExtV3Size;

    RandoSidecarSlot v3dst;
    uint32 v3_used = deserialize_slot_versioned(v3buf, v3_total, &v3dst, 3);
    if (v3_used != v3_total) selfcheck_die("v3 compat: used != v3 total");
    if (v3dst.header.entrance_digest24 != 0xABCDEF)
      selfcheck_die("v3 compat: entrance_digest24 round-trip");
    if (memcmp(v3dst.settings_canonical, src.settings_canonical,
               kRandoSidecar_LegacySettingsCanonicalLen28) != 0)
      selfcheck_die("v3 compat: settings blob prefix round-trip");
    if (v3dst.settings_canonical[kRandoSidecar_LegacySettingsCanonicalLen28] != 0)
      selfcheck_die("v3 compat: appended settings byte must zero-extend");
    if (v3dst.header.recommended_features0_present != 0)
      selfcheck_die("v3 compat: zero extension must not imply recommended_features0");
    if (v3dst.header.pot_registry_present != 0 || v3dst.header.pot_registry_digest != 0 ||
        v3dst.header.pot_registry_count != 0)
      selfcheck_die("v3 compat: pot registry fields must be forced 0");
    if (v3dst.header.chains_present != 0 || v3dst.header.chains_attempt != 0 ||
        v3dst.header.chains_digest24 != 0)
      selfcheck_die("v3 compat: dungeon-chain fields must be forced 0");
  }

  // -------------------------------------------------------------------------
  // format_version 4 backward-compat: slots written before dungeon chains
  // persisted their layout identity have the pot registry block but no v5 tail.
  // -------------------------------------------------------------------------
  {
    uint8 v4buf[256];
    memset(v4buf, 0, sizeof(v4buf));
    serialize_slot_header(&src.header, v4buf);
    uint32 loc_count = (uint32)src.header.placement_table_size / 2;
    uint8 *p = v4buf + kRandoSidecar_SlotHeaderSize;
    for (uint32 i = 0; i < loc_count; i++) put_u16le(p + i * 2, kRandoSidecar_NoPlacementSentinel);
    put_u16le(p + 5 * 2, 50);
    p += loc_count * 2;
    uint32 v4_bitmap = (loc_count + 7) >> 3;
    p[0] = 0x05;
    p += v4_bitmap;
    // A REAL v4 file carries the 29-byte legacy blob (the 30-byte width is
    // v8+ — add-rando-grass-rock-shuffle); the reader zero-extends byte [29].
    memcpy(p, src.settings_canonical, kRandoSidecar_LegacySettingsCanonicalLen29);
    p += kRandoSidecar_LegacySettingsCanonicalLen29;
    p[0] = 0xEF; p[1] = 0xCD; p[2] = 0xAB;
    put_u32le(p + 8, 0x12345678u);
    put_u16le(p + 12, 0x0456u);
    p[14] = 1;
    uint32 v4_total = kRandoSidecar_SlotHeaderSize + loc_count * 2 + v4_bitmap +
                      kRandoSidecar_LegacySettingsCanonicalLen29 +
                      kRandoSidecar_SlotExtV4Size;

    RandoSidecarSlot v4dst;
    uint32 v4_used = deserialize_slot_versioned(v4buf, v4_total, &v4dst, 4);
    if (v4_used != v4_total) selfcheck_die("v4 compat: used != v4 total");
    if (v4dst.header.pot_registry_digest != 0x12345678u ||
        v4dst.header.pot_registry_count != 0x0456u ||
        v4dst.header.pot_registry_present != 1)
      selfcheck_die("v4 compat: pot registry round-trip");
    if (v4dst.header.chains_present != 0 || v4dst.header.chains_attempt != 0 ||
        v4dst.header.chains_digest24 != 0)
      selfcheck_die("v4 compat: dungeon-chain fields must be forced 0");
  }

  // -------------------------------------------------------------------------
  // FIX #13 — file_crc algorithm vector + odd placement_table_size rejection.
  // -------------------------------------------------------------------------
  {
    // Standard CRC-32 check vector: CRC32("123456789") == 0xCBF43926.
    if (rs_crc32((const uint8 *)"123456789", 9) != 0xCBF43926u) {
      selfcheck_die("rs_crc32 known-vector mismatch (not standard CRC-32)");
    }
    // An odd placement_table_size must be refused on BOTH sides: the writer
    // would otherwise leak one uninitialized buffer byte (sizing counts the
    // raw odd size; the body writes location_count*2), and a loader given an
    // odd size would disagree with the writer on where the bitmap starts.
    RandoSidecarSlot odd = src;
    odd.header.placement_table_size = 43;  // odd — corrupt by construction
    uint8 odd_buf[512];  // ample capacity so the reject is for oddness, not size
    if (RandoSave_SerializeSlot(&odd, odd_buf, sizeof(odd_buf)) != 0) {
      selfcheck_die("serialize must reject odd placement_table_size");
    }
    // Loader side: serialize the valid slot afresh (`buf` was magic-corrupted
    // by an earlier reject test), then corrupt the size field (@61 u16 LE) to
    // an odd value — deserialize must reject it.
    uint8 corrupt[512];
    uint32 cw = RandoSave_SerializeSlot(&src, corrupt, sizeof(corrupt));
    if (cw == 0) selfcheck_die("re-serialize for odd-size loader test");
    put_u16le(corrupt + 61, 43);
    RandoSidecarSlot odd_dst;
    if (RandoSave_DeserializeSlot(corrupt, cw, &odd_dst) != 0) {
      selfcheck_die("deserialize must reject odd placement_table_size");
    }
  }

  // -------------------------------------------------------------------------
  // §8.5 cross-version load test.
  // Spec scenario "Version drift loads with warning": a slot whose
  // `generator_version` differs from the binary's current value still loads
  // (the embedded placement table is authoritative); the drift is signalled
  // by Rando_DetectVersionDrift so the UI can warn.
  // -------------------------------------------------------------------------
  {
    RandoSlotHeader hdr_old;
    memset(&hdr_old, 0, sizeof(hdr_old));
    hdr_old.slot_kind = kSlotKind_Randomizer;
    hdr_old.generator_version = (uint16)(kGeneratorVersion - 1);  // N
    hdr_old.placement_table_size = 0;

    // Newer binary (N+1) loading an older slot — drift detected.
    if (!Rando_DetectVersionDrift(&hdr_old, (uint16)kGeneratorVersion)) {
      selfcheck_die("§8.5: newer binary should detect version drift on older slot");
    }
    // Same-version load: no drift.
    RandoSlotHeader hdr_cur;
    memset(&hdr_cur, 0, sizeof(hdr_cur));
    hdr_cur.slot_kind = kSlotKind_Randomizer;
    hdr_cur.generator_version = (uint16)kGeneratorVersion;
    if (Rando_DetectVersionDrift(&hdr_cur, (uint16)kGeneratorVersion)) {
      selfcheck_die("§8.5: same-version load should report no drift");
    }
    // Older binary reading a newer slot — drift symmetric.
    RandoSlotHeader hdr_new = hdr_old;
    hdr_new.generator_version = (uint16)(kGeneratorVersion + 1);
    if (!Rando_DetectVersionDrift(&hdr_new, (uint16)kGeneratorVersion)) {
      selfcheck_die("§8.5: drift detection should be symmetric (older binary vs newer slot)");
    }
    // NULL header is defensively false.
    if (Rando_DetectVersionDrift(NULL, (uint16)kGeneratorVersion)) {
      selfcheck_die("§8.5: NULL header should report no drift");
    }
    // Spec invariant: even with drift, the on-disk slot's
    // `placement_table_size` is honored — we don't re-derive from the
    // binary's current registry. Build a serialized slot with size 10 (5
    // locations), drift the version, deserialize on the "newer binary",
    // and confirm `placement_table_size` round-trips exactly.
    //
    // Vacuous-by-construction note: the cross-registry
    // failure mode this is meant to guard against (a loader that re-derives
    // from current-binary registry count instead of reading the slot's own
    // size) is structurally impossible in `RandoSave_DeserializeSlot` —
    // there's no `current_registry_count` parameter or global it could
    // consult. The assertion below documents the invariant as a regression
    // guard against a future loader rewrite that adds such a parameter.
    RandoSidecarSlot drift_src;
    memset(&drift_src, 0, sizeof(drift_src));
    drift_src.header = hdr_old;
    drift_src.header.placement_table_size = 10;  // 5 locations
    drift_src.placements[0].location_id = 2;
    drift_src.placements[0].item_id = 0x0042;
    drift_src.placement_count = 1;
    uint8 drift_buf[512];
    uint32 drift_wrote = RandoSave_SerializeSlot(&drift_src, drift_buf, sizeof(drift_buf));
    if (drift_wrote == 0) selfcheck_die("§8.5: serialize for drift round-trip");
    RandoSidecarSlot drift_dst;
    if (RandoSave_DeserializeSlot(drift_buf, drift_wrote, &drift_dst) == 0) {
      selfcheck_die("§8.5: deserialize for drift round-trip");
    }
    if (drift_dst.header.generator_version != (uint16)(kGeneratorVersion - 1)) {
      selfcheck_die("§8.5: drifted generator_version should round-trip as-written");
    }
    if (drift_dst.header.placement_table_size != 10) {
      selfcheck_die("§8.5: placement_table_size should round-trip exactly (spec: read N bytes, not current-binary's count)");
    }
    if (drift_dst.placement_count != 1 ||
        drift_dst.placements[0].location_id != 2 ||
        drift_dst.placements[0].item_id != 0x0042) {
      selfcheck_die("§8.5: drifted slot placements should round-trip identically");
    }
  }

  // -------------------------------------------------------------------------
  // §8.6 downgrade-then-re-upgrade drift detection.
  // Spec scenario "Drift detected after downgrade-then-re-upgrade": after a
  // vanilla-only write to the paired sram.dat slot, the stored
  // sram_slot_checksum_at_last_write no longer matches the current paired
  // bytes — the helper returns true so the UI can fire the recovery prompt.
  // -------------------------------------------------------------------------
  {
    // Synthesize "v1.1 state" — populate sram_v11 with a deterministic
    // pattern and capture its checksum into the slot header.
    uint8 sram_v11[0x500];
    for (uint32 i = 0; i < sizeof(sram_v11); i++) {
      sram_v11[i] = (uint8)((i * 13u + 7u) & 0xffu);
    }
    RandoSlotHeader hdr_at_v11;
    memset(&hdr_at_v11, 0, sizeof(hdr_at_v11));
    hdr_at_v11.slot_kind = kSlotKind_Randomizer;
    hdr_at_v11.generator_version = (uint16)kGeneratorVersion;
    hdr_at_v11.sram_slot_checksum_at_last_write =
        RandoSave_ComputeSramSlotChecksum(sram_v11, sizeof(sram_v11));

    // Same paired slot bytes → no drift.
    if (Rando_DetectChecksumDrift(&hdr_at_v11, sram_v11, sizeof(sram_v11))) {
      selfcheck_die("§8.6: no-edit paired sram should report no drift");
    }

    // Simulate v1.0 writing the paired sram slot — different bytes.
    uint8 sram_v10[0x500];
    for (uint32 i = 0; i < sizeof(sram_v10); i++) {
      sram_v10[i] = (uint8)((i * 17u + 3u) & 0xffu);
    }
    if (!Rando_DetectChecksumDrift(&hdr_at_v11, sram_v10, sizeof(sram_v10))) {
      selfcheck_die("§8.6: edited paired sram should fire drift signal");
    }

    // Single-byte change is still drift (the checksum routine is u16
    // subtract-over-pairs; a single flipped bit shifts the result).
    uint8 sram_one_byte_off[0x500];
    memcpy(sram_one_byte_off, sram_v11, sizeof(sram_one_byte_off));
    sram_one_byte_off[0x100] ^= 0x01;
    if (!Rando_DetectChecksumDrift(&hdr_at_v11, sram_one_byte_off,
                                   sizeof(sram_one_byte_off))) {
      selfcheck_die("§8.6: single-byte sram change should fire drift signal");
    }

    // NULL/short-buffer cases are defensively false (no signal).
    if (Rando_DetectChecksumDrift(&hdr_at_v11, NULL, sizeof(sram_v11))) {
      selfcheck_die("§8.6: NULL paired slot should report no drift");
    }
    if (Rando_DetectChecksumDrift(NULL, sram_v11, sizeof(sram_v11))) {
      selfcheck_die("§8.6: NULL header should report no drift");
    }
    if (Rando_DetectChecksumDrift(&hdr_at_v11, sram_v11, 100)) {
      selfcheck_die("§8.6: too-small buffer should report no drift");
    }

    // Spec scenario "Crash between sidecar and sram.dat" produces
    // checksum drift WITHOUT version drift — single binary version, but
    // the sram.dat write was interrupted so its bytes don't match the
    // sidecar's recorded checksum. Verify checksum-drift fires alone on
    // this isolated scenario.
    {
      RandoSlotHeader hdr_crash = hdr_at_v11;  // same gen_version
      // sram_v10 represents the partially-written sram.dat after the crash.
      if (!Rando_DetectChecksumDrift(&hdr_crash, sram_v10, sizeof(sram_v10))) {
        selfcheck_die("§8.6: crash-mid-save should fire checksum drift");
      }
      if (Rando_DetectVersionDrift(&hdr_crash, (uint16)kGeneratorVersion)) {
        selfcheck_die("§8.6: crash-mid-save scenario should NOT have version drift (single binary)");
      }
    }

    // Cross-product: both drifts can co-occur — this is the
    // downgrade-then-re-upgrade pattern (player wrote on v1.1, ran an older
    // v1.0 binary that re-saved the paired sram.dat as vanilla without
    // touching the sidecar, then re-upgraded to a v1.2 binary that loads
    // the slot). The sidecar's gen_version still says v1.1; the binary is
    // v1.2 (version drift); and the paired sram.dat checksum no longer
    // matches the sidecar's recorded value (checksum drift). Both helpers
    // fire independently — the UI surfaces both conditions.
    RandoSlotHeader hdr_both = hdr_at_v11;
    hdr_both.generator_version = (uint16)(kGeneratorVersion - 1);
    if (!Rando_DetectVersionDrift(&hdr_both, (uint16)kGeneratorVersion)) {
      selfcheck_die("§8.6: downgrade-re-upgrade case should fire version drift");
    }
    if (!Rando_DetectChecksumDrift(&hdr_both, sram_v10, sizeof(sram_v10))) {
      selfcheck_die("§8.6: downgrade-re-upgrade case should fire checksum drift");
    }
  }

  // -------------------------------------------------------------------------
  // §8.7 coexistence test.
  // Spec scenarios "Slot-kind discriminator" and "sram.dat present but
  // sidecar absent": a sidecar with mixed slot_kinds (Randomizer / Vanilla /
  // Empty) loads correctly per-slot; a missing sidecar file produces no
  // rando metadata. We exercise the in-memory serialize → deserialize path
  // (the same data flow as RandoSave_ReadFile uses internally) so the test
  // stays hermetic — CI environments may not have a writable saves/ dir.
  // -------------------------------------------------------------------------
  {
    RandoSidecarSlot mixed[kRandoSidecar_SlotCount];
    memset(mixed, 0, sizeof(mixed));
    // Slot 0: Randomizer with a populated placement table.
    mixed[0].header.slot_kind = kSlotKind_Randomizer;
    mixed[0].header.generator_version = (uint16)kGeneratorVersion;
    mixed[0].header.placement_table_size = 8;  // 4 locations
    mixed[0].placements[0].location_id = 1;
    mixed[0].placements[0].item_id = 0x0080;
    mixed[0].placement_count = 1;
    // Slot 1: Vanilla — explicit opt-out. Build with a NON-zero placement
    // table body to exercise the spec scenario "Slot-kind discriminator"
    // properly: "the loader ignores any subsequent bytes of that slot and
    // treats the corresponding sram.dat slot as a plain vanilla save."
    // A Vanilla slot can in principle have leftover placement bytes from a
    // prior write (e.g., the slot was a Randomizer that the user converted
    // to Vanilla). The discriminator contract says the caller MUST gate on
    // slot_kind, not on placement contents. Test below asserts slot_kind
    // round-trips so the UI's slot-kind dispatch is reliable.
    mixed[1].header.slot_kind = kSlotKind_Vanilla;
    mixed[1].header.placement_table_size = 6;  // 3 location slots
    mixed[1].placements[0].location_id = 0;
    mixed[1].placements[0].item_id = 0xBEEF;   // junk that the UI must ignore
    mixed[1].placement_count = 1;
    // Slot 2: Empty — never touched.
    mixed[2].header.slot_kind = kSlotKind_Empty;
    mixed[2].header.placement_table_size = 0;

    // Serialize full file (file header + 3 slot regions).
    uint32 total = kRandoSidecar_FileHeaderSize;
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      total += RandoSave_SlotOnDiskSize(mixed[i].header.placement_table_size);
    }
    uint8 *bigbuf = (uint8 *)malloc(total);
    if (bigbuf == NULL) selfcheck_die("§8.7: malloc coexistence buffer");
    RandoSidecarFileHeader fh3 = { kRandoSidecar_FileFormatVersion,
                                   kRandoSidecar_SlotCount, 0 };
    uint32 off = RandoSave_SerializeFileHeader(&fh3, bigbuf, total);
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      uint32 u = RandoSave_SerializeSlot(&mixed[i], bigbuf + off, total - off);
      if (u == 0) { free(bigbuf); selfcheck_die("§8.7: serialize mixed slot"); }
      off += u;
    }
    if (off != total) { free(bigbuf); selfcheck_die("§8.7: coexistence size mismatch"); }

    // Read back and verify per-slot classification round-trips.
    RandoSidecarFileHeader fh4;
    uint32 fhu = RandoSave_DeserializeFileHeader(bigbuf, total, &fh4);
    if (fhu != kRandoSidecar_FileHeaderSize) {
      free(bigbuf); selfcheck_die("§8.7: file header deserialize");
    }
    uint32 cur = fhu;
    RandoSidecarSlot back[kRandoSidecar_SlotCount];
    for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
      uint32 u = RandoSave_DeserializeSlot(bigbuf + cur, total - cur, &back[i]);
      if (u == 0) { free(bigbuf); selfcheck_die("§8.7: slot deserialize"); }
      cur += u;
    }
    free(bigbuf);

    if (back[0].header.slot_kind != kSlotKind_Randomizer) {
      selfcheck_die("§8.7: slot 0 should round-trip as Randomizer");
    }
    if (back[0].placement_count != 1 ||
        back[0].placements[0].location_id != 1 ||
        back[0].placements[0].item_id != 0x0080) {
      selfcheck_die("§8.7: slot 0 placement should round-trip");
    }
    if (back[1].header.slot_kind != kSlotKind_Vanilla) {
      selfcheck_die("§8.7: slot 1 should round-trip as Vanilla (spec: discriminator preserved)");
    }
    // The Vanilla slot's body (junk placement bytes) is preserved through
    // serialize/deserialize — that's expected. The contract the spec pins
    // is that the UI MUST gate on slot_kind, not parse the body. Assert
    // the body bytes did make the round-trip so we know the discriminator
    // worked under realistic conditions (a Vanilla slot with non-zero
    // body), AND that the slot_kind discriminator is intact so the UI
    // can rely on it.
    if (back[1].placement_count != 1 ||
        back[1].placements[0].item_id != 0xBEEF) {
      selfcheck_die("§8.7: Vanilla slot body should round-trip (UI must gate on slot_kind, not contents)");
    }
    if (back[2].header.slot_kind != kSlotKind_Empty) {
      selfcheck_die("§8.7: slot 2 should round-trip as Empty");
    }

    // Compatibility: early v4 development builds declared format 4 and wrote
    // current-length settings blobs, but still used the older 8-byte extension
    // block. A later generate must read and upgrade that file instead of
    // quarantining it.
    {
      enum { kLegacyV4CompatFormatVersion = 4 };
      uint32 legacy_ext = legacy_slot_ext_len_versioned(kLegacyV4CompatFormatVersion);
      if (legacy_ext != kRandoSidecar_SlotExtV3Size)
        selfcheck_die("§8.7: expected legacy v4 extension size");
      uint32 compat_total = kRandoSidecar_FileHeaderSize;
      for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
        compat_total += slot_on_disk_size_versioned_ext(
            mixed[i].header.placement_table_size, kLegacyV4CompatFormatVersion,
            legacy_ext);
      }
      uint8 *compat = (uint8 *)calloc(1, compat_total);
      if (compat == NULL) selfcheck_die("§8.7: calloc legacy-v4 compat buffer");
      RandoSidecarFileHeader fh_compat = {
        kLegacyV4CompatFormatVersion, kRandoSidecar_SlotCount, 0
      };
      uint32 compat_off = RandoSave_SerializeFileHeader(&fh_compat, compat, compat_total);
      for (uint8 i = 0; i < kRandoSidecar_SlotCount; i++) {
        uint32 current_size = RandoSave_SlotOnDiskSize(mixed[i].header.placement_table_size);
        uint32 legacy_size = slot_on_disk_size_versioned_ext(
            mixed[i].header.placement_table_size, kLegacyV4CompatFormatVersion,
            legacy_ext);
        uint8 *slotbuf = (uint8 *)malloc(current_size);
        if (slotbuf == NULL) {
          free(compat);
          selfcheck_die("§8.7: malloc legacy-v4 slot buffer");
        }
        uint32 u = RandoSave_SerializeSlot(&mixed[i], slotbuf, current_size);
        if (u != current_size || legacy_size > current_size) {
          free(slotbuf);
          free(compat);
          selfcheck_die("§8.7: serialize compat slot");
        }
        memcpy(compat + compat_off, slotbuf, legacy_size);
        free(slotbuf);
        compat_off += legacy_size;
      }
      if (compat_off != compat_total) {
        free(compat);
        selfcheck_die("§8.7: compat size mismatch");
      }
      put_u32le(compat + 8, rs_crc32(compat + kRandoSidecar_FileHeaderSize,
                                     compat_total - kRandoSidecar_FileHeaderSize));
      RandoSidecarSlot compat_back[kRandoSidecar_SlotCount];
      if (!deserialize_file_buffer(compat, compat_total, compat_back)) {
        free(compat);
        selfcheck_die("§8.7: legacy v4 sidecar should load");
      }
      free(compat);
      if (compat_back[0].header.slot_kind != kSlotKind_Randomizer ||
          compat_back[0].placement_count != 1 ||
          compat_back[0].placements[0].item_id != 0x0080) {
        selfcheck_die("§8.7: legacy v4 sidecar lost populated slot");
      }
      if (compat_back[1].header.slot_kind != kSlotKind_Vanilla ||
          compat_back[1].placement_count != 1 ||
          compat_back[2].header.slot_kind != kSlotKind_Empty ||
          compat_back[2].placement_count != 0 ||
          compat_back[0].header.pot_registry_present ||
          compat_back[1].header.pot_registry_present ||
          compat_back[2].header.pot_registry_present) {
        selfcheck_die("§8.7: legacy v4 slots should load with pot registry absent");
      }
    }

    // Spec scenario "sram.dat present but sidecar absent": loading a path
    // that doesn't exist returns false; caller treats all slots as vanilla.
    // Use a path with embedded entropy (PID-equivalent + literal cookie) so
    // collision with a real CI file is implausible — protects against the
    // pathological cwd hygiene failure of a literal short filename.
    {
      RandoSidecarSlot dummy_slots[kRandoSidecar_SlotCount];
      char unique_path[128];
      // Compose: literal cookie + a deterministic-but-unlikely-to-collide
      // suffix derived from the function address (changes per build) and
      // the stack address (changes per invocation). Neither is a "secure"
      // random source, but the combination is essentially never the name
      // of a real file in any sane checkout.
      uintptr_t entropy = (uintptr_t)&dummy_slots ^ (uintptr_t)&RandoSave_ReadFile;
      snprintf(unique_path, sizeof(unique_path),
               "selfcheck_nonexistent_%016llx.dat",
               (unsigned long long)entropy);
      bool ok = RandoSave_ReadFile(unique_path, dummy_slots);
      if (ok) {
        selfcheck_die("§8.7: missing sidecar file should return false (vanilla appearance)");
      }
    }
  }

  fprintf(stderr, "[RandoSave_SelfCheck] OK\n");
}

// Cross-TU capacity ABI probe -- see rando_logic.h / Rando_SelfCheckCapacityABI.
RANDO_DEFINE_CAPACITY_PROBE(rando_save)
