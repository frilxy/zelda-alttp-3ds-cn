// rando_spoiler.h — JSON + text spoiler writer (tasks.md §5.1-§5.2).
//
// Stable field names per the spec: share_string, generator_version, settings,
// placements[], sphere_data[], goal_completable, generation_wall_clock_ms,
// fallback_warnings[].

#ifndef ZELDA3_RANDO_SPOILER_H_
#define ZELDA3_RANDO_SPOILER_H_

#include "../types.h"
#include "rando_hints.h"
#include "rando_placement.h"
#include "rando_settings.h"  // kSettingsCanonicalLen for the _Static_assert below

typedef struct DungeonChainsLayout DungeonChainsLayout;

typedef struct RandoSpoiler {
  const char *share_string;
  uint64 seed_u64;                   // per spec — emitted in meta block
  uint32 generator_version;
  const RandoSettings *settings;
  const RandoPlacementTable *placements;
  const RandoSpheres *spheres;       // optional; NULL omits sphere_data
  // Immutable generation-time hint plan. Spoiler writers consume only this
  // caller-owned value and never consult or mutate the active gameplay plan.
  // NULL is accepted for non-randomizer/legacy callers and emits no hint rows.
  const RandoHintPlan *hint_plan;
  // Actual MM/TR medallion requirements for this seed. The placement table
  // carries legacy Medallion config rows, but those are not item checks and
  // may be vanilla-pinned; spoiler text should use this assignment instead.
  const uint8 *medallion_assignment;  // [kRandoMedallionEntranceCount], optional
  // Placer fallback counters — populated from Place_AssumedFill outputs.
  // Each non-zero counter produces one fallback_warnings[] entry in JSON
  // and a banner in the text spoiler.
  uint32 forward_fill_fallback_count;  // # of placements that fell back to forward-fill
  uint32 retry_attempts;               // # of assumed-fill attempts taken before success
  uint32 generation_wall_clock_ms;
  bool goal_completable;
  // Phase C entrance shuffle — the accepted cave permutation (interior index →
  // interior index now reached through its door). NULL/0 when no entrance
  // shuffle, which omits the "entrance_mapping" spoiler section.
  const uint8 *entrance_assign;
  int entrance_count;
  // Phase C Stage 2 — the accepted dungeon permutation (dungeon index → dungeon
  // index now reached through its overworld door). NULL/0 omits the section.
  const uint8 *dungeon_assign;
  int dungeon_count;
  // Phase C Stage 3 — the accepted CROSS-category combined permutation (caves +
  // cross-eligible dungeons in one pool). NULL/0 omits the section.
  const uint8 *cross_assign;
  int cross_count;
  // Phase C Stage 4 (D.1/D.2) — the accepted DECOUPLED exit permutation (hole →
  // emerge-hole). NULL/0 omits the section.
  const uint8 *decoupled_assign;
  int decoupled_count;
  // Phase C Stage 4 (dungeon decoupled) — the accepted one-way DUNGEON exit
  // permutation (loaded dungeon → exit door). NULL/0 omits the section.
  const uint8 *dun_decoupled_assign;
  int dun_decoupled_count;
  // Phase C Stage 4 (cross decoupled) — one-way exit permutation over the MIXED
  // cross pool (interior → emerge door). NULL/0 omits the section.
  const uint8 *cross_decoupled_assign;
  int cross_decoupled_count;
  // Dungeon chains — accepted per-seed chain layout. NULL omits the section.
  const DungeonChainsLayout *chains_layout;
  uint8 chains_attempt;
  // Phase B Slice 7 — boss shuffle. `boss_assignment[16]` is the dungeon-id →
  // boss-pool-index table (BossShuffle_ComputeAssignment). NULL omits the
  // `boss_assignments` section (set NULL when boss_shuffle is off — §6.4).
  const uint8 *boss_assignment;
  // Phase B Slice 8 — drop-pool shuffle. `drop_map[56]` is the flat-index →
  // source-index permutation (DropShuffle_ComputeAssignment). NULL omits the
  // `drop_tables` section (set NULL when drop_shuffle is off — §6.4).
  const uint8 *drop_map;
  // True when the drop heart-floor retry budget was exhausted and the table
  // fell back to the vanilla identity — surfaces as a fallback_warnings entry.
  bool drop_used_fallback;
} RandoSpoiler;

// Write JSON spoiler to `out_path`. Returns true on success, false on I/O
// failure. The JSON shape is byte-identical given the same `RandoSpoiler`
// input (sorted keys, no whitespace variation) so the file's SHA-256 is a
// stable identity for race-mode stamping.
bool Spoiler_WriteJson(const RandoSpoiler *s, const char *out_path);

// Write text spoiler grouped by region for human readability.
bool Spoiler_WriteText(const RandoSpoiler *s, const char *out_path);

// ---------------------------------------------------------------------------
// Phase B Slice 6 — race-mode spoiler suppression.
//
// Top-level entry that writes the JSON + .txt spoiler when `s->settings->race_mode == 0`,
// or the suppressed binary form alone (no .txt) when `race_mode == 1`. The
// suppressed form on disk is the file at `json_path` — the magic header
// (`ZRSR`) is what callers use to distinguish; the filename is unchanged.
//
// Returns true on success.
//
// `txt_path` may be NULL to skip the text companion in both branches; race
// mode always skips the text companion regardless of `txt_path`.
// ---------------------------------------------------------------------------
bool Spoiler_Write(const RandoSpoiler *s,
                   const char *json_path,
                   const char *txt_path);

// ---------------------------------------------------------------------------
// Suppressed-spoiler on-disk format (Phase B Slice 6 — randomizer-save spec).
//
// The file at `<spoiler_dir>/<share_string>.json` contains, in race mode, the
// bytes described by RandoSuppressedSpoiler (little-endian multibyte ints):
//
//   offset    size   field
//   ------    ----   --------------------------------
//   +0        4      magic[4] = "ZRSR"
//   +4        2      generator_version (u16 LE)
//   +6        32     spoiler_stamp[32] = SHA-256 of the full canonical
//                    JSON spoiler with race_mode cleared to 0 and
//                    generation_wall_clock_ms cleared to 0 (so the stamp
//                    is reproducible across runs)
//   +38       4      share_string_len (u32 LE) — actual length without NUL
//   +42       64     share_string[64] — base32 textual share string,
//                    zero-padded
//   +106      31     settings_canonical[31] = Settings_CanonicalSerialize
//                    output (= kSettingsCanonicalLen) with race_mode cleared to
//                    0 (race-mode flag itself is recorded in the file's
//                    existence, not in the serialized settings). Needed at
//                    reveal time to regenerate the placement deterministically.
//   +137      4      crc32 (u32 LE) — IEEE 802.3 over offsets 0..136
//
// Total: 141 bytes (see kRandoSuppressedSpoilerSize; grew 138->139 when
// enemy_drop_checks appended canonical byte [28], and 139->140 when
// add-rando-grass-rock-shuffle appended the terrain byte [29], then 140->141
// for key rings/Skeleton Key byte [30]). The struct
// below is for in-memory parsing; the on-disk
// layout is the byte sequence above (we serialize explicitly to avoid
// platform-dependent padding).
//
// Why settings are in the file: the per-slot sidecar (RandoSidecarSlot) does
// NOT preserve the original RandoSettings — only `settings_hash` (a truncated
// SHA-256, not invertible). To regenerate the placement and verify the
// stamp, the reveal pipeline needs the original settings. Including them
// here doesn't leak the placement (the secret); settings are already
// publicly visible on any tournament settings sheet.
// ---------------------------------------------------------------------------
#define kRandoSuppressedSpoilerMagic "ZRSR"
#define kRandoSuppressedSpoilerShareStringMax 64
#define kRandoSuppressedSpoilerSettingsLen 31  // = kSettingsCanonicalLen
#define kRandoSuppressedSpoilerSize 141  // on-disk byte length (140->141: key axes [30])
#define kRandoSuppressedSpoilerCrcOffset (kRandoSuppressedSpoilerSize - 4)

// Compile-time guard — when kSettingsCanonicalLen bumps, this assert
// forces a coupled update to kRandoSuppressedSpoilerSettingsLen +
// kRandoSuppressedSpoilerSize + the CRC32 offsets in rando_spoiler.c +
// the corpus runner constants. Caught at build time rather than at
// the run_rando_corpus race-mode regression (which is how the
// kGenVer 14 cycle first surfaced the gap).
_Static_assert(kRandoSuppressedSpoilerSettingsLen == kSettingsCanonicalLen,
               "ZRSR settings_canonical span must match kSettingsCanonicalLen; "
               "bump kRandoSuppressedSpoilerSettingsLen AND kRandoSuppressedSpoilerSize "
               "AND the "
               "ZRSR constants in assets/scripts/{bump,run}_rando_corpus.py.");

_Static_assert(kRandoSuppressedSpoilerCrcOffset == 137,
               "ZRSR CRC offset drifted; update the on-disk layout comments "
               "and corpus runner constants with kRandoSuppressedSpoilerSize.");

typedef struct RandoSuppressedSpoiler {
  uint8 magic[4];                // 'ZRSR'
  uint16 generator_version;      // LE on disk
  uint8 spoiler_stamp[32];       // SHA-256
  uint32 share_string_len;       // actual length (<= 64)
  uint8 share_string[kRandoSuppressedSpoilerShareStringMax]; // zero-padded
  uint8 settings_canonical[kRandoSuppressedSpoilerSettingsLen];
  uint32 crc32;                  // IEEE 802.3 over prior bytes (LE on disk)
} RandoSuppressedSpoiler;

// Read the suppressed spoiler at `path` into `out`, verifying magic + CRC.
// Returns 0 on success.
// Returns -1 on file-not-found, -2 on bad magic / parse error, -3 on CRC mismatch.
int Spoiler_ReadSuppressed(const char *path, RandoSuppressedSpoiler *out);

// Resolve the spoiler path for a slot's share string. The path is derived
// at runtime from `[Randomizer] SpoilerDir` (or `<exe-dir>/spoilers` when
// the INI key is unset) + the slot's share string + the extension.
//
// Per `randomizer-core / Spoiler-log emission`: the path is NOT stored in
// the slot — it's recomputed whenever the spoiler needs to be written or
// surfaced. This keeps slots invariant under config changes.
//
// Writes into `out_path` (max `out_capacity` bytes including NUL). Returns
// the length written (excluding NUL) or 0 on failure (buffer too small).
//
// Caller responsibility: the directory `<spoiler_dir>` must exist when the
// spoiler is actually written (Spoiler_WriteJson does not create it).
int Spoiler_ResolvePath(const char *share_string,
                        const char *extension_with_dot,  // ".json" or ".txt"
                        char *out_path, int out_capacity);

#endif  // ZELDA3_RANDO_SPOILER_H_
