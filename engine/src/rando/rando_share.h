// rando_share.h — share-string encoder/decoder.
//
// Two wire formats (randomizer-core / Share-string format), one decode entry
// point that dispatches on the decoded MAGIC bytes — never on string length:
//
// v1 (legacy; remains the internal seed-IDENTITY blob everywhere a share
// string is stored or compared — sidecar slot header, ZRSR field, spoiler
// filename + meta.share_string, 5-icon hash, reveal compares):
//   magic "ZRSS"[4] | generator_version[1] | settings_hash[16] |
//   seed_u64[8] LE | crc16[2] LE
//   = 31 bytes -> exactly 50 base32 chars.
//   settings_hash is one-way: a v1 string can restore only the seed.
//
// v2 (the EXCHANGE format — native-window copy/paste + CLI distribution;
// transport-only, design D1 of add-rando-share-string-v2):
//   magic "ZRS2"[4] | generator_version[1] | settings_len[1] |
//   settings_canonical[settings_len] | seed_u64[8] LE | crc16[2] LE
//   = 16+settings_len bytes -> ceil((16+settings_len)*8/5) base32 chars
//   (47 bytes -> exactly 76 chars at the current kSettingsCanonicalLen 31).
//   settings_hash is NOT embedded — decoders that need it recompute it from
//   the canonical bytes (Settings_CanonicalDeserialize + Settings_HashShort).
//   A v2 string restores settings AND seed.
//
// CRC (both formats): CRC-16-CCITT-FALSE over every byte before the CRC
// field, stored LE.
//
// Explicit rejects: alttpr.com format hashes, corrupted base32, wrong
// length, wrong magic, checksum mismatch, and (v2) a settings_len larger
// than this binary's kSettingsCanonicalLen ("made by a newer version" —
// honoring a prefix would silently drop unknown axes).

#ifndef ZELDA3_RANDO_SHARE_H_
#define ZELDA3_RANDO_SHARE_H_

#include "../types.h"
#include "rando_settings.h"  // kSettingsCanonicalLen — the v2 payload size

#define kShareStringBinaryLen 31  // v1 binary blob (the stored seed identity)
#define kShareStringV2BinaryLen (16 + kSettingsCanonicalLen)  // 47 today

// Text-buffer bound for encode/decode (string + NUL). Sized with headroom
// beyond the current 76-char v2 string so a future canonical growth doesn't
// immediately overflow every coupled buffer; rando_share.c carries the
// compile-time assert tying this to kSettingsCanonicalLen. (Asserts live in
// the .c, not here — this header is included from a C++ TU via the
// rando_window bridge, where C11 _Static_assert is unavailable.)
#define kShareStringBase32MaxLen 96

typedef enum {
  kShareDecodeOk = 0,
  kShareDecodeBadLength,
  kShareDecodeBadBase32,
  kShareDecodeBadMagic,
  kShareDecodeBadChecksum,
  kShareDecodeAlttprFormat,  // explicit reject of alttpr.com-style hashes
  kShareDecodeNewerSettings, // v2 settings_len > this binary's kSettingsCanonicalLen
} ShareDecodeStatus;

// ShareString.format values (set by Share_Decode; encoders ignore the field).
#define kShareFormatV1 1
#define kShareFormatV2 2

typedef struct ShareString {
  uint8 version;            // generator_version byte (both formats)
  uint8 settings_hash[16];  // v1: embedded hash. v2: ZEROED by decode — the
                            // hash is not on the wire; recompute from
                            // settings_canonical when needed.
  uint64 seed_u64;
  // --- v2 decode results (meaningful when format == kShareFormatV2) ---
  uint8 format;             // kShareFormatV1 / kShareFormatV2
  uint8 settings_len;       // embedded canonical length as transmitted
                            // (<= kSettingsCanonicalLen once accepted)
  uint8 settings_canonical[kSettingsCanonicalLen];  // zero-extended to the
                            // local canonical length (zero is the append-only
                            // default for later-added axes)
} ShareString;

// Encode `ss` into `out` as a v1 string (reads version, settings_hash,
// seed_u64). Returns the number of chars written (50, excluding the trailing
// NUL) or -1 on insufficient capacity; caller passes `out_capacity` >= 51.
// v1 encode remains the identity form used by slot/spoiler storage.
int Share_Encode(const ShareString *ss, char *out, int out_capacity);

// Encode `ss` into `out` as a v2 string (reads version, seed_u64, and the
// full settings_canonical[kSettingsCanonicalLen]; writes settings_len =
// kSettingsCanonicalLen on the wire — `ss->settings_len` is ignored).
// Returns the number of chars written (76 at the current canonical length,
// excluding the trailing NUL) or -1 on insufficient capacity; caller passes
// `out_capacity` >= the encoded length + 1 (kShareStringBase32MaxLen always
// suffices).
int Share_EncodeV2(const ShareString *ss, char *out, int out_capacity);

// UI helper: base32-encode an existing 31-byte raw share-string binary blob
// (as stored in `RandoSidecarSlot.header.share_string`). Writes exactly 50
// uppercase base32 characters into `out` followed by a NUL terminator;
// `out_capacity` must be >= 51. Returns the number of characters written
// (50) on success, -1 on insufficient capacity. Used by the file-select
// rando-banner renderer to display the first-N chars of the share string.
int Share_EncodeRaw(const uint8 raw_binary[kShareStringBinaryLen],
                    char *out, int out_capacity);

// produce the 31-byte raw binary form
// (magic[4] | version[1] | settings_hash[16] | seed_u64[8] | crc[2])
// from a ShareString. Callers that need to STORE the raw blob (e.g. the
// sidecar slot header) should use this instead of hand-rolling the
// pack — previously the file-select Generate path rebuilt the bytes
// inline and left the trailing CRC at zero, producing a banner
// share-string that disagreed with Share_Encode's output.
void Share_PackBinary(const ShareString *ss,
                      uint8 out[kShareStringBinaryLen]);

// Decode `in` (base32, NUL-terminated) into `out_ss`. Handles BOTH wire
// formats: dispatches on the decoded magic ("ZRSS" => require exactly 31
// bytes, v1 parse; "ZRS2" => require exactly 16+settings_len bytes, v2
// parse). Sets out_ss->format accordingly. Returns the decode-status enum;
// success is `kShareDecodeOk`.
ShareDecodeStatus Share_Decode(const char *in, ShareString *out_ss);

// Self-test. Round-trips (v1 + v2, including a pinned legacy v1 literal so
// v1 decoding can never silently rot) + explicit-reject tests. Exits the
// process with code 2 on any failure.
void Share_SelfCheck(void);

// ---------------------------------------------------------------------------
// Share-string paste path (tasks.md §9.6). UI helper that combines
// Share_Decode with a settings-population callback.
//
// `share_string_input` is the user-entered text (typically from RandoTextField).
// On success: decodes to ShareString, populates `out_seed_u64` and writes the
// settings_hash to `out_settings_hash_short[16]`. Returns kShareDecodeOk.
//
// On failure: returns the specific reject status (alttpr format, bad base32,
// wrong magic, bad checksum, bad length). The UI can surface a specific
// error message per status.
//
// This is the Switch / in-game textfield surface and is v1-only by design
// (D7). The TYPICAL v2 string (76 chars) cannot fit kRandoTextFieldMaxLen
// (64), but a SHORT v2 string (settings_len <= 24) physically could — so this
// helper REFUSES any non-v1 format (returns kShareDecodeBadMagic) rather than
// returning it seed-only with a zeroed hash. For a v1 string the settings_hash
// is the embedded one; settings are NOT restored (hash is one-way) — the UI
// warns when the user's settings don't match. (v2 entry on Switch rides with
// the parked add-rando-switch-swkbd change.)
// ---------------------------------------------------------------------------
ShareDecodeStatus Share_PastePath(const char *share_string_input,
                                  uint64 *out_seed_u64,
                                  uint8 out_settings_hash_short[16]);

#endif  // ZELDA3_RANDO_SHARE_H_
