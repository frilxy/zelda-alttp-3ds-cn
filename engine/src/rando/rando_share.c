// rando_share.c — share-string encoder/decoder (tasks.md §2.3-§2.4;
// add-rando-share-string-v2 design D2/D3).
//
// Two wire formats, one decode entry point that dispatches on the decoded
// MAGIC bytes — never on string length alone:
//
// v1 (legacy; remains the internal seed-IDENTITY blob everywhere a share
// string is stored or compared — sidecar slot header, ZRSR field, spoiler
// filename + meta.share_string, 5-icon hash, reveal compares):
//   offset  size  field
//   0       4     magic ("ZRSS" = 0x5A 0x52 0x53 0x53)
//   4       1     generator_version
//   5       16    settings_hash (SHA-256 of canonical settings, truncated to 16)
//   21      8     seed_u64 (little-endian)
//   29      2     checksum (CRC-16-CCITT-FALSE over bytes [0..28], LE)
//   = 31 bytes -> exactly 50 base32 chars.
//   settings_hash is one-way: a v1 string can restore only the seed.
//
// v2 (the EXCHANGE format — native-window copy/paste + CLI distribution;
// transport-only, design D1):
//   offset  size          field
//   0       4             magic ("ZRS2" = 0x5A 0x52 0x53 0x32)
//   4       1             generator_version
//   5       1             settings_len (= kSettingsCanonicalLen at encode)
//   6       settings_len  settings_canonical (verbatim Settings_CanonicalSerialize)
//   6+len   8             seed_u64 (little-endian)
//   14+len  2             checksum (CRC-16-CCITT-FALSE over all prior bytes, LE)
//   = 16+settings_len bytes -> ceil((16+settings_len)*8/5) base32 chars
//   (47 bytes -> exactly 76 chars at the current kSettingsCanonicalLen 31).
//   settings_hash is NOT embedded — decoders that need it recompute it from
//   the canonical bytes (Settings_CanonicalDeserialize + Settings_HashShort).
//   A v2 string restores settings AND seed.
//
// Base32 (both formats): RFC 4648 alphabet (uppercase, no padding); the final
// char's low bits are zero-padded.
//
// Explicit-reject signals (per task 2.4 + the v2 spec delta):
//   - alttpr.com hash format (lowercase, '/', '+', or base64-special chars)
//   - corrupted base32 character
//   - wrong-length input (per-format, against the decoded magic + settings_len)
//   - wrong magic prefix
//   - checksum mismatch
//   - (v2) settings_len > this binary's kSettingsCanonicalLen ("made by a
//     newer version" — honoring a prefix would silently drop unknown axes)
//
// All multi-byte fields are explicit little-endian via byte shifts (no
// htole*/be*toh macros — per randomizer-core / Byte-order pin).

#include "rando_share.h"
#include "../types.h"
#include <string.h>

#define kShareMagic0 0x5A  /* 'Z' */
#define kShareMagic1 0x52  /* 'R' */
#define kShareMagic2 0x53  /* 'S' */
#define kShareMagic3 0x53  /* 'S' — v1 magic is "ZRSS" */
#define kShareMagicV2_3 0x32  /* '2' — v2 magic is "ZRS2" */
#define kShareCurrentVersion 1

#define kShareBinaryLen 31     /* v1: magic+ver+hash+seed+ck */
#define kShareBase32Len 50     /* ceil(31*8 / 5) */
#define kShareV2Base32Len ((kShareStringV2BinaryLen * 8 + 4) / 5)  /* 76 today */

/* Largest binary blob a kShareStringBase32MaxLen-char string can carry —
 * floor(96*5/8) = 60 bytes, i.e. v2 settings_len up to 44. */
#define kShareMaxBinaryLen (kShareStringBase32MaxLen * 5 / 8)

/* Compile-time coupling (design D2). These live in the .c, NOT the header —
 * rando_share.h is included from a C++ TU (the rando_window bridge) where
 * C11 _Static_assert is unavailable. */
_Static_assert(kSettingsCanonicalLen <= 255,
               "v2 settings_len must fit a single wire byte");
_Static_assert(kShareStringBase32MaxLen >= kShareV2Base32Len + 1,
               "kShareStringBase32MaxLen must hold the v2 string + NUL; "
               "grow it alongside kSettingsCanonicalLen");

/* RFC 4648 Base32 alphabet — uppercase, no padding. */
static const char kBase32Alphabet[32] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

/* Reverse table: ASCII byte → 5-bit value, or -1 if not in alphabet. */
static int8 base32_decode_char(uint8 c) {
  if (c >= 'A' && c <= 'Z') return (int8)(c - 'A');
  if (c >= '2' && c <= '7') return (int8)(c - '2' + 26);
  return -1;
}

/* CRC-16-CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no XOR-out.
 * Byte-at-a-time bit-bang (~10 lines, no table needed). */
static uint16 crc16_ccitt(const uint8 *data, size_t len) {
  uint16 crc = 0xFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= (uint16)data[i] << 8;
    for (int b = 0; b < 8; ++b) {
      if (crc & 0x8000) {
        crc = (uint16)((crc << 1) ^ 0x1021);
      } else {
        crc = (uint16)(crc << 1);
      }
    }
  }
  return crc;
}

/* Build the 31-byte v1 binary blob from a ShareString. */
static void pack_binary(const ShareString *ss, uint8 out[kShareBinaryLen]) {
  out[0] = kShareMagic0;
  out[1] = kShareMagic1;
  out[2] = kShareMagic2;
  out[3] = kShareMagic3;
  out[4] = ss->version;
  for (int i = 0; i < 16; ++i) out[5 + i] = ss->settings_hash[i];
  /* seed_u64 little-endian */
  out[21] = (uint8)(ss->seed_u64 >> 0);
  out[22] = (uint8)(ss->seed_u64 >> 8);
  out[23] = (uint8)(ss->seed_u64 >> 16);
  out[24] = (uint8)(ss->seed_u64 >> 24);
  out[25] = (uint8)(ss->seed_u64 >> 32);
  out[26] = (uint8)(ss->seed_u64 >> 40);
  out[27] = (uint8)(ss->seed_u64 >> 48);
  out[28] = (uint8)(ss->seed_u64 >> 56);
  /* checksum over bytes [0..28], little-endian */
  uint16 ck = crc16_ccitt(out, 29);
  out[29] = (uint8)(ck & 0xFF);
  out[30] = (uint8)(ck >> 8);
}

/* Build the current v2 binary blob from a ShareString. The wire settings_len
 * is always this binary's kSettingsCanonicalLen — ss->settings_len is a
 * DECODE-side field and is ignored here (header contract). */
static void pack_binary_v2(const ShareString *ss,
                           uint8 out[kShareStringV2BinaryLen]) {
  out[0] = kShareMagic0;
  out[1] = kShareMagic1;
  out[2] = kShareMagic2;
  out[3] = kShareMagicV2_3;
  out[4] = ss->version;
  out[5] = kSettingsCanonicalLen;
  for (int i = 0; i < kSettingsCanonicalLen; ++i)
    out[6 + i] = ss->settings_canonical[i];
  /* seed_u64 little-endian at [6+len .. 13+len] */
  int p = 6 + kSettingsCanonicalLen;
  out[p + 0] = (uint8)(ss->seed_u64 >> 0);
  out[p + 1] = (uint8)(ss->seed_u64 >> 8);
  out[p + 2] = (uint8)(ss->seed_u64 >> 16);
  out[p + 3] = (uint8)(ss->seed_u64 >> 24);
  out[p + 4] = (uint8)(ss->seed_u64 >> 32);
  out[p + 5] = (uint8)(ss->seed_u64 >> 40);
  out[p + 6] = (uint8)(ss->seed_u64 >> 48);
  out[p + 7] = (uint8)(ss->seed_u64 >> 56);
  /* checksum over all prior bytes, little-endian */
  uint16 ck = crc16_ccitt(out, (size_t)(p + 8));
  out[p + 8] = (uint8)(ck & 0xFF);
  out[p + 9] = (uint8)(ck >> 8);
}

/* public entry point for callers that need the
 * 31-byte binary form (magic + version + settings_hash + seed_u64 + CRC).
 * Previously callers were recomputing the layout manually and leaving the
 * trailing CRC at zero, producing a banner share-string that differed
 * from the one Share_Encode emitted. */
void Share_PackBinary(const ShareString *ss, uint8 out[kShareBinaryLen]) {
  pack_binary(ss, out);
}

/* Base32-encode `in_len` bytes → ceil(in_len*8/5) chars (no padding, no NUL).
 * Returns the number of chars written. */
static int base32_encode(const uint8 *in, int in_len, char *out) {
  uint64 buf = 0;
  int bits = 0;
  int out_pos = 0;
  for (int i = 0; i < in_len; ++i) {
    buf = (buf << 8) | in[i];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      out[out_pos++] = kBase32Alphabet[(buf >> bits) & 0x1F];
    }
  }
  if (bits > 0) {
    /* Final char: lowest (5 - bits) bits are zero-padded. */
    out[out_pos++] = kBase32Alphabet[(buf << (5 - bits)) & 0x1F];
  }
  return out_pos;
}

/* Base32-decode `char_len` chars → floor(char_len*5/8) bytes. Returns the
 * number of bytes written, or -1 on any non-alphabet character or if the
 * output would exceed `out_cap`.
 * The trailing partial bits of the final char are zero padding; we don't
 * validate they're zero (some encoders may not zero them) — strict
 * validation would reject otherwise-valid round-tripped inputs from
 * lossy encoders. The CRC-16 catches any actual corruption. */
static int base32_decode(const char *in, int char_len, uint8 *out, int out_cap) {
  uint64 buf = 0;
  int bits = 0;
  int out_pos = 0;
  for (int i = 0; i < char_len; ++i) {
    int8 v = base32_decode_char((uint8)in[i]);
    if (v < 0) return -1;
    buf = (buf << 5) | (uint64)(uint8)v;
    bits += 5;
    if (bits >= 8) {
      bits -= 8;
      if (out_pos >= out_cap) return -1;
      out[out_pos++] = (uint8)((buf >> bits) & 0xFF);
    }
  }
  return out_pos;
}

/* ===========================================================================
 * Public API
 * =========================================================================== */

int Share_Encode(const ShareString *ss, char *out, int out_capacity) {
  if (out_capacity < kShareBase32Len + 1) return -1;
  uint8 bin[kShareBinaryLen];
  pack_binary(ss, bin);
  base32_encode(bin, kShareBinaryLen, out);
  out[kShareBase32Len] = '\0';
  return kShareBase32Len;
}

int Share_EncodeV2(const ShareString *ss, char *out, int out_capacity) {
  if (out_capacity < kShareV2Base32Len + 1) return -1;
  uint8 bin[kShareStringV2BinaryLen];
  pack_binary_v2(ss, bin);
  base32_encode(bin, kShareStringV2BinaryLen, out);
  out[kShareV2Base32Len] = '\0';
  return kShareV2Base32Len;
}

int Share_EncodeRaw(const uint8 raw_binary[kShareStringBinaryLen],
                    char *out, int out_capacity) {
  if (out == NULL || raw_binary == NULL) return -1;
  if (out_capacity < kShareBase32Len + 1) return -1;
  base32_encode(raw_binary, kShareBinaryLen, out);
  out[kShareBase32Len] = '\0';
  return kShareBase32Len;
}

ShareDecodeStatus Share_Decode(const char *in, ShareString *out_ss) {
  if (in == NULL || out_ss == NULL) return kShareDecodeBadLength;

  /* Length scan, capped at 2x the encode bound so huge pastes are rejected
   * without a full scan. Everything this binary can DECODE fits within
   * kShareStringBase32MaxLen (v1 = 50 chars, v2 <= kShareV2Base32Len); the
   * (kShareStringBase32MaxLen..2x] band exists so a longer v2 string from a
   * future binary with a grown canonical layout still classifies as "made
   * by a newer version" below instead of a bare length reject. */
  size_t len = 0;
  while (in[len] != '\0') {
    if (len >= (size_t)(2 * kShareStringBase32MaxLen)) return kShareDecodeBadLength;
    ++len;
  }
  /* 10 chars decode to 6 bytes — the minimum to read magic + version +
   * settings_len for the dispatch below. */
  if (len < 10) return kShareDecodeBadLength;

  /* Early-out: alttpr.com hashes contain lowercase letters or '/'+'-'.
   * Our alphabet is uppercase A-Z + 2-7 only (both formats). We surface a
   * specific status for recognized alttpr.com-style patterns. */
  bool has_lower = false, has_b64_special = false;
  for (size_t i = 0; i < len; ++i) {
    uint8 c = (uint8)in[i];
    if (c >= 'a' && c <= 'z') has_lower = true;
    if (c == '/' || c == '+' || c == '-' || c == '_' || c == '=') has_b64_special = true;
  }
  if (has_lower || has_b64_special) {
    return kShareDecodeAlttprFormat;
  }

  /* Deterministic output on every accept path: format/settings_len/
   * settings_canonical/settings_hash default to zero unless a parse below
   * fills them. */
  memset(out_ss, 0, sizeof *out_ss);

  if (len > (size_t)kShareStringBase32MaxLen) {
    /* Longer than any v2 token this binary can represent (settings_len <=
     * kSettingsCanonicalLen). Decode only the first 16 chars (10 bytes): a
     * well-formed v2 header whose settings_len exceeds ours classifies as
     * "newer version" (spec scenario "Newer-version v2 string is refused,
     * not truncated"); anything else is a plain length reject. */
    uint8 prefix[10];
    if (base32_decode(in, 16, prefix, (int)sizeof prefix) < 0)
      return kShareDecodeBadBase32;
    if (prefix[0] == kShareMagic0 && prefix[1] == kShareMagic1 &&
        prefix[2] == kShareMagic2 && prefix[3] == kShareMagicV2_3 &&
        prefix[5] > kSettingsCanonicalLen) {
      return kShareDecodeNewerSettings;
    }
    return kShareDecodeBadLength;
  }

  uint8 bin[kShareMaxBinaryLen];
  int bin_len = base32_decode(in, (int)len, bin, (int)sizeof bin);
  if (bin_len < 0) {
    return kShareDecodeBadBase32;
  }
  /* len >= 10 guarantees bin_len >= 6: magic + version + settings_len are
   * readable before any per-format length check. */

  if (bin[0] != kShareMagic0 || bin[1] != kShareMagic1 ||
      bin[2] != kShareMagic2) {
    return kShareDecodeBadMagic;
  }

  if (bin[3] == kShareMagic3) {
    /* ---- v1: "ZRSS" — exactly 31 bytes, i.e. exactly 50 chars. ---- */
    if (len != (size_t)kShareBase32Len) {
      return kShareDecodeBadLength;
    }
    uint16 stored_ck = (uint16)bin[29] | ((uint16)bin[30] << 8);
    uint16 expected_ck = crc16_ccitt(bin, 29);
    if (stored_ck != expected_ck) {
      return kShareDecodeBadChecksum;
    }
    out_ss->version = bin[4];
    for (int i = 0; i < 16; ++i) out_ss->settings_hash[i] = bin[5 + i];
    out_ss->seed_u64 =
        (uint64)bin[21] |
        ((uint64)bin[22] << 8) |
        ((uint64)bin[23] << 16) |
        ((uint64)bin[24] << 24) |
        ((uint64)bin[25] << 32) |
        ((uint64)bin[26] << 40) |
        ((uint64)bin[27] << 48) |
        ((uint64)bin[28] << 56);
    out_ss->format = kShareFormatV1;
    /* settings_len = 0 and settings_canonical all-zero (memset above): a v1
     * string carries no canonical settings. */
    return kShareDecodeOk;
  }

  if (bin[3] == kShareMagicV2_3) {
    /* ---- v2: "ZRS2" — exactly 16 + settings_len bytes. ---- */
    int settings_len = bin[5];
    int t = 16 + settings_len;
    if (len != (size_t)((t * 8 + 4) / 5)) {
      return kShareDecodeBadLength;
    }
    /* The exact-length requirement makes bin_len == t here, and
     * len <= kShareStringBase32MaxLen bounds t <= kShareMaxBinaryLen. */
    uint16 stored_ck = (uint16)bin[t - 2] | ((uint16)bin[t - 1] << 8);
    uint16 expected_ck = crc16_ccitt(bin, (size_t)(t - 2));
    if (stored_ck != expected_ck) {
      return kShareDecodeBadChecksum;
    }
    /* CRC first, newness second: corruption reports as a checksum failure,
     * a valid string from a newer binary as "newer version". */
    if (settings_len > kSettingsCanonicalLen) {
      return kShareDecodeNewerSettings;
    }
    out_ss->version = bin[4];
    out_ss->format = kShareFormatV2;
    out_ss->settings_len = (uint8)settings_len;  /* raw embedded length */
    for (int i = 0; i < settings_len; ++i)
      out_ss->settings_canonical[i] = bin[6 + i];
    /* Tail [settings_len..kSettingsCanonicalLen) stays zero (memset above) —
     * zero is the append-only default for later-added axes. settings_hash
     * also stays zero: it is not on the wire; recompute it from the
     * canonical bytes (Settings_CanonicalDeserialize + Settings_HashShort)
     * when needed. */
    int sp = 6 + settings_len;
    out_ss->seed_u64 =
        (uint64)bin[sp + 0] |
        ((uint64)bin[sp + 1] << 8) |
        ((uint64)bin[sp + 2] << 16) |
        ((uint64)bin[sp + 3] << 24) |
        ((uint64)bin[sp + 4] << 32) |
        ((uint64)bin[sp + 5] << 40) |
        ((uint64)bin[sp + 6] << 48) |
        ((uint64)bin[sp + 7] << 56);
    return kShareDecodeOk;
  }

  return kShareDecodeBadMagic;
}

/* ===========================================================================
 * Self-test (tasks.md §2.4 — round-trip + explicit rejects, v1 AND v2)
 * =========================================================================== */
#include <stdio.h>
#include <stdlib.h>

static void share_assert(bool cond, const char *msg) {
  if (!cond) {
    fprintf(stderr, "Share_SelfCheck: FAILED: %s\n", msg);
    exit(2);
  }
}

// ---------------------------------------------------------------------------
// Share_PastePath (tasks.md §9.6) — UI helper.
//
// Decodes a user-pasted share string and extracts the seed_u64 +
// settings_hash. Returns the decode status enum so the caller can surface a
// specific error message.
// ---------------------------------------------------------------------------
ShareDecodeStatus Share_PastePath(const char *share_string_input,
                                  uint64 *out_seed_u64,
                                  uint8 out_settings_hash_short[16]) {
  if (share_string_input == NULL || out_seed_u64 == NULL ||
      out_settings_hash_short == NULL) {
    return kShareDecodeBadLength;
  }
  ShareString ss;
  ShareDecodeStatus st = Share_Decode(share_string_input, &ss);
  if (st != kShareDecodeOk) {
    return st;
  }
  // This is the v1-only textfield surface (Switch / in-game alphabet picker,
  // design D7). The TYPICAL v2 string (settings_len 31 -> 76 chars) can't fit
  // kRandoTextFieldMaxLen (64), but a SHORT v2 string (settings_len <= 24 ->
  // <= 64 chars; settings_len 15 is exactly 50) physically fits and would
  // decode here with a ZEROED settings_hash (v2 carries no hash) and its
  // canonical settings silently dropped — exactly the seed-only silent-
  // divergence v2 exists to prevent. Refuse any non-v1 format explicitly
  // rather than relying on the length cap. (v2 entry on Switch rides with the
  // parked add-rando-switch-swkbd change, which restores full settings.)
  if (ss.format != kShareFormatV1) {
    return kShareDecodeBadMagic;
  }
  *out_seed_u64 = ss.seed_u64;
  for (int i = 0; i < 16; i++) out_settings_hash_short[i] = ss.settings_hash[i];
  return kShareDecodeOk;
}

/* SelfCheck-only: hand-pack a v2 blob with an ARBITRARY settings_len
 * (including lengths this binary cannot accept) and a valid CRC, so the
 * adversarial cases below exercise the decoder's settings_len handling.
 * Returns the blob length (16 + settings_len); `out` must hold that many. */
static int selfcheck_pack_v2(uint8 version, uint8 settings_len,
                             const uint8 *settings, uint64 seed, uint8 *out) {
  int t = 16 + settings_len;
  out[0] = kShareMagic0;
  out[1] = kShareMagic1;
  out[2] = kShareMagic2;
  out[3] = kShareMagicV2_3;
  out[4] = version;
  out[5] = settings_len;
  for (int i = 0; i < settings_len; ++i) out[6 + i] = settings[i];
  for (int i = 0; i < 8; ++i)
    out[6 + settings_len + i] = (uint8)(seed >> (8 * i));
  uint16 ck = crc16_ccitt(out, (size_t)(t - 2));
  out[t - 2] = (uint8)(ck & 0xFF);
  out[t - 1] = (uint8)(ck >> 8);
  return t;
}

void Share_SelfCheck(void) {
  /* Round-trip: encode + decode → identical struct. */
  ShareString original = {0};
  original.version = kShareCurrentVersion;
  for (int i = 0; i < 16; ++i) original.settings_hash[i] = (uint8)(i * 17 + 3);
  original.seed_u64 = 0xCAFEBABEDEADBEEFull;

  char encoded[kShareStringBase32MaxLen];
  int n = Share_Encode(&original, encoded, sizeof encoded);
  share_assert(n == kShareBase32Len, "encode length");

  ShareString decoded;
  ShareDecodeStatus st = Share_Decode(encoded, &decoded);
  share_assert(st == kShareDecodeOk, "round-trip decode");
  share_assert(decoded.format == kShareFormatV1, "v1 format tag");
  share_assert(decoded.settings_len == 0, "v1 settings_len is 0");
  share_assert(decoded.version == original.version, "version round-trip");
  share_assert(decoded.seed_u64 == original.seed_u64, "seed round-trip");
  for (int i = 0; i < 16; ++i) {
    share_assert(decoded.settings_hash[i] == original.settings_hash[i],
                 "settings_hash round-trip");
  }

  /* Pinned legacy v1 literal (minted before v2 existed: kGeneratorVersion 67,
   * seed 28, CRC 0x2895) so v1 decoding can never silently rot. */
  st = Share_Decode("LJJFGU2DPXU3RXZRMN25JP63H4N25AK4VMOAAAAAAAAAAAEVFA",
                    &decoded);
  share_assert(st == kShareDecodeOk, "pinned v1 literal decodes");
  share_assert(decoded.format == kShareFormatV1, "pinned v1 format");
  share_assert(decoded.version == 67, "pinned v1 version");
  share_assert(decoded.seed_u64 == 28, "pinned v1 seed");

  /* Reject: alttpr.com-style hash (lowercase + slashes — typical permalink). */
  st = Share_Decode("eyf+abc/def123EYF+ABCABCABCABCABCABCABCABCABCABCDEFG", &decoded);
  share_assert(st == kShareDecodeAlttprFormat ||
               st == kShareDecodeBadLength,
               "reject alttpr.com format (with slash/lower)");

  /* Reject: corrupted base32 char (`1` is not in the alphabet). */
  char corrupt_b32[kShareBase32Len + 1];
  memcpy(corrupt_b32, encoded, sizeof corrupt_b32);
  corrupt_b32[10] = '1';  /* invalid */
  st = Share_Decode(corrupt_b32, &decoded);
  share_assert(st == kShareDecodeBadBase32 ||
               st == kShareDecodeAlttprFormat,
               "reject corrupted base32 char");

  /* Reject: wrong-length input. */
  char shortstr[20];
  memcpy(shortstr, encoded, 19);
  shortstr[19] = '\0';
  st = Share_Decode(shortstr, &decoded);
  share_assert(st == kShareDecodeBadLength, "reject wrong-length input");

  /* Reject: wrong magic. Manually flip the first byte after a clean encode,
   * recompute the checksum so we don't trip the checksum check first, then
   * re-encode and decode. */
  uint8 bin[kShareBinaryLen];
  pack_binary(&original, bin);
  bin[0] = 'X';  /* break magic */
  /* recompute crc so the failure surfaces as BadMagic, not BadChecksum */
  uint16 ck = crc16_ccitt(bin, 29);
  bin[29] = (uint8)(ck & 0xFF);
  bin[30] = (uint8)(ck >> 8);
  char encoded_badmagic[kShareBase32Len + 1];
  base32_encode(bin, kShareBinaryLen, encoded_badmagic);
  encoded_badmagic[kShareBase32Len] = '\0';
  st = Share_Decode(encoded_badmagic, &decoded);
  share_assert(st == kShareDecodeBadMagic, "reject wrong magic");

  /* Reject: checksum corruption. Flip a byte mid-payload (after magic, before
   * the checksum), keeping the same checksum bytes → decoder recomputes,
   * sees mismatch, returns BadChecksum. */
  char encoded_badck[kShareBase32Len + 1];
  memcpy(encoded_badck, encoded, sizeof encoded_badck);
  /* Flip char at position 30 (well past the magic, before the trailing CRC
   * bytes which encode to positions ~46-50). Pick a valid alphabet char. */
  encoded_badck[30] = (encoded_badck[30] == 'A') ? 'B' : 'A';
  st = Share_Decode(encoded_badck, &decoded);
  /* It could fail as BadChecksum OR as BadMagic if the flip cascades through
   * base32 decoding. Either way it MUST NOT report Ok. */
  share_assert(st != kShareDecodeOk, "reject mutation in payload");

  /* ---- v2 cases (add-rando-share-string-v2) ---- */

  /* v2 round-trip with REAL settings (spec scenario "Round-trip encoding
   * (v2)"): defaults → canonical bytes → encode → decode → byte-identical
   * canonical + recomputable hash. */
  RandoSettings defaults;
  Settings_SetDefaults(&defaults);
  defaults.key_rings = kKeyRings_All;
  defaults.skeleton_key = 1;
  ShareString v2in = {0};
  v2in.version = 0x42;  /* arbitrary; real producers write kGeneratorVersion */
  v2in.seed_u64 = 0x0123456789ABCDEFull;
  Settings_CanonicalSerialize(&defaults, v2in.settings_canonical);
  char enc2[kShareStringBase32MaxLen];
  int n2 = Share_EncodeV2(&v2in, enc2, sizeof enc2);
  share_assert(n2 == kShareV2Base32Len, "v2 encode length (76 today)");
  ShareString v2out;
  st = Share_Decode(enc2, &v2out);
  share_assert(st == kShareDecodeOk, "v2 round-trip decode");
  share_assert(v2out.format == kShareFormatV2, "v2 format tag");
  share_assert(v2out.settings_len == kSettingsCanonicalLen, "v2 settings_len");
  share_assert(v2out.version == v2in.version, "v2 version round-trip");
  share_assert(v2out.seed_u64 == v2in.seed_u64, "v2 seed round-trip");
  share_assert(memcmp(v2out.settings_canonical, v2in.settings_canonical,
                      kSettingsCanonicalLen) == 0,
               "v2 canonical bytes round-trip");
  for (int i = 0; i < 16; ++i) {
    share_assert(v2out.settings_hash[i] == 0,
                 "v2 settings_hash zeroed (not on the wire)");
  }
  /* Hash recomputability: deserialize the decoded canonical bytes; their
   * Settings_HashShort must equal the original struct's. */
  RandoSettings roundtrip;
  share_assert(Settings_CanonicalDeserialize(v2out.settings_canonical,
                                             &roundtrip) == 0,
               "v2 canonical deserialize");
  uint8 hash_orig[16], hash_rt[16];
  Settings_HashShort(&defaults, hash_orig);
  Settings_HashShort(&roundtrip, hash_rt);
  share_assert(memcmp(hash_orig, hash_rt, 16) == 0,
               "v2 settings_hash recompute matches");

  /* Reject: truncated v2 (chop the last char). */
  char trunc2[kShareStringBase32MaxLen];
  memcpy(trunc2, enc2, (size_t)n2);
  trunc2[n2 - 1] = '\0';
  st = Share_Decode(trunc2, &v2out);
  share_assert(st == kShareDecodeBadLength, "v2 truncated -> BadLength");

  /* Reject: corrupted v2 mid-payload char (canonical-settings region). */
  char corrupt2[kShareStringBase32MaxLen];
  memcpy(corrupt2, enc2, (size_t)n2 + 1);
  corrupt2[35] = (corrupt2[35] == 'A') ? 'B' : 'A';
  st = Share_Decode(corrupt2, &v2out);
  share_assert(st != kShareDecodeOk, "v2 corrupted char must not decode Ok");

  /* Reject: settings_len = kSettingsCanonicalLen + 1 with a VALID CRC at its
   * correct length (48 bytes -> 77 chars) — "made by a newer version" (spec
   * scenario "Newer-version v2 string is refused, not truncated"). */
  uint8 fake_settings[64];
  for (int i = 0; i < (int)sizeof fake_settings; ++i)
    fake_settings[i] = (uint8)(i + 1);
  uint8 blob[96];
  char enc_big[2 * kShareStringBase32MaxLen];
  int t = selfcheck_pack_v2(0x42, kSettingsCanonicalLen + 1, fake_settings,
                            99, blob);
  int chars = base32_encode(blob, t, enc_big);
  enc_big[chars] = '\0';
  share_assert(chars == (t * 8 + 4) / 5, "settings_len+1 encode length");
  st = Share_Decode(enc_big, &v2out);
  share_assert(st == kShareDecodeNewerSettings,
               "v2 settings_len+1 -> NewerSettings");

  /* Accept every historical append-only canonical width (28/29/30) and
   * zero-extend all later axes. */
  for (uint8 older_len = 28; older_len <= 30; older_len++) {
    t = selfcheck_pack_v2(0x42, older_len, v2in.settings_canonical,
                          0x1122334455667788ull, blob);
    chars = base32_encode(blob, t, enc_big);
    enc_big[chars] = '\0';
    st = Share_Decode(enc_big, &v2out);
    share_assert(st == kShareDecodeOk, "legacy-width v2 decodes");
    share_assert(v2out.settings_len == older_len,
                 "legacy-width v2 embedded length kept");
    share_assert(memcmp(v2out.settings_canonical, v2in.settings_canonical,
                        older_len) == 0,
                 "legacy-width v2 canonical prefix matches");
    for (uint8 i = older_len; i < kSettingsCanonicalLen; i++)
      share_assert(v2out.settings_canonical[i] == 0,
                   "legacy-width v2 canonical tail zero-extended");
    share_assert(v2out.seed_u64 == 0x1122334455667788ull,
                 "legacy-width v2 seed");
  }

  /* Cross-format reject: a 31-byte v1-LAYOUT blob with the v2 magic — its
   * embedded settings_len (31) demands 76 chars; presented at 50 chars it
   * must reject on length (magic-based dispatch, never length-based). */
  pack_binary(&original, bin);
  bin[3] = kShareMagicV2_3;        /* ZRSS -> ZRS2 */
  bin[5] = kSettingsCanonicalLen;  /* settings_len byte in the v2 reading */
  ck = crc16_ccitt(bin, 29);
  bin[29] = (uint8)(ck & 0xFF);
  bin[30] = (uint8)(ck >> 8);
  char enc50[kShareBase32Len + 1];
  base32_encode(bin, kShareBinaryLen, enc50);
  enc50[kShareBase32Len] = '\0';
  st = Share_Decode(enc50, &decoded);
  share_assert(st == kShareDecodeBadLength,
               "ZRS2 magic at 50 chars -> BadLength");

  /* Cross-format reject: a current v2 blob with the v1 magic presented at 76
   * chars — the v1 arm requires exactly 50 chars. */
  uint8 blob_v2[kShareStringV2BinaryLen];
  pack_binary_v2(&v2in, blob_v2);
  blob_v2[3] = kShareMagic3;  /* ZRS2 -> ZRSS; length rejects before CRC */
  chars = base32_encode(blob_v2, kShareStringV2BinaryLen, enc_big);
  enc_big[chars] = '\0';
  st = Share_Decode(enc_big, &decoded);
  share_assert(st == kShareDecodeBadLength,
               "ZRSS magic at v2 length -> BadLength");

  /* Reject: settings_len = 45 (61 bytes -> 98 chars) — longer than any v2
   * token this binary can represent; classified as NewerSettings via the
   * 16-char prefix decode, not rejected as a bare bad length. */
  t = selfcheck_pack_v2(0x42, 45, fake_settings, 7, blob);
  chars = base32_encode(blob, t, enc_big);
  enc_big[chars] = '\0';
  share_assert(chars == 98, "settings_len 45 encode length (98)");
  st = Share_Decode(enc_big, &v2out);
  share_assert(st == kShareDecodeNewerSettings,
               "98-char v2 -> NewerSettings via prefix classification");

  /* Share_PastePath (the v1-only Switch/textfield surface, D7) MUST refuse a
   * v2 string rather than return it seed-only with a zeroed hash. A short v2
   * string (settings_len 15 -> exactly 50 chars) fits a 64-char textfield and
   * decodes as v2, so the length cap alone is not a sufficient guard. */
  t = selfcheck_pack_v2(0x42, 15, fake_settings, 0xABCD, blob);
  chars = base32_encode(blob, t, enc_big);
  enc_big[chars] = '\0';
  share_assert(chars == 50, "settings_len 15 v2 is 50 chars (fits textfield)");
  st = Share_Decode(enc_big, &v2out);
  share_assert(st == kShareDecodeOk && v2out.format == kShareFormatV2,
               "short v2 decodes as v2 via Share_Decode");
  uint64 pp_seed = 0;
  uint8 pp_hash[16];
  st = Share_PastePath(enc_big, &pp_seed, pp_hash);
  share_assert(st != kShareDecodeOk,
               "Share_PastePath refuses a short v2 string (no silent seed-only)");
  share_assert(pp_seed == 0,
               "Share_PastePath leaves the seed untouched on a v2 reject");
}
