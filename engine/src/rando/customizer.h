// customizer.h — customizer mode (add-rando-customizer-mode, Phase D).
//
// Customizer mode lets a user hand-place specific items at specific locations
// via a manifest file instead of (only) the random assumed-fill placer. The
// manifest PINS a subset of locations; the assumed-fill placer then completes
// the remaining locations exactly as in a normal seed. This is faithful to
// ALTTPR's customizer (partial manual placement + random fill of the rest).
//
// Pipeline integration (headless --generate-seed --customizer=<path>):
//   1. main.c loads + parses the manifest (Customizer_LoadFile), sets
//      settings.customizer_active = 1, and installs it (Customizer_Install).
//   2. place_assumed_fill_attempt() (rando_placement.c) reads the active
//      manifest via Customizer_GetActive() and, inside a single
//      customizer_active-guarded block, pins each location's slot and removes
//      the pinned items from the to-place pool BEFORE assumed fill runs. When
//      no manifest is installed the placer is byte-for-byte unchanged (the
//      regression corpus is the zero-regression proof).
//   3. The existing goal/accessibility refusal gate validates the result; an
//      un-completable hand-placement is refused exactly like a broken
//      assumed-fill seed (unless --allow-broken-seed).
//
// The runtime DISPATCHER is unchanged: a customizer-built placement table is
// indistinguishable from an assumed-fill one (same RandoPlacement pairs).
//
// MANIFEST FORMAT (a strict line-based YAML subset):
//
//   placements:
//     Eastern_Palace_Boss: BowAndArrows
//     Hyrule Castle - Boomerang Chest: Hookshot
//     Master_Sword_Pedestal: TitanMitt
//
// Location keys accept either the canonical "symbol" form (Eastern_Palace_Boss)
// or the human form printed in the spoiler (Eastern Palace - Boss); both
// resolve through a normalized (lowercase, alphanumeric-only) match against the
// codegen name tables. Item values use the item-registry names (Hookshot,
// TitanMitt, ProgressiveSword, ...).
//
// An optional pool_overrides section adjusts the item pool before assumed fill:
//
//   pool_overrides:
//     add: [ProgressiveSword, ProgressiveSword]
//     remove: [Rupoor]
//
// `remove` is best-effort (an item not in the settings-dependent pool is a
// no-op); `add` cannot add non-grantable prize/event/virtual items.
// add/remove apply remove-then-add, before pins.
//
// Per-item grant caps are enforced at generation time: the pool
// after overrides plus any out-of-pool pins may not exceed what the runtime
// can actually grant — 4 ProgressiveSword, 3 ProgressiveShield,
// 2 ProgressiveArmor, 2 ProgressiveGlove, 2 ProgressiveBow (the
// progressive_to_lttp tier ladders), and 4 bottles TOTAL across all bottle
// variants (link_bottle_info has 4 slots). An over-cap manifest is refused
// with a hard error naming the item and the cap (see
// customizer_validate_item_caps in rando_placement.c).

#ifndef ZELDA3_RANDO_CUSTOMIZER_H_
#define ZELDA3_RANDO_CUSTOMIZER_H_

#include "../types.h"

// Generous upper bound: a manifest may pin at most every location once.
#define kCustomizerMaxPins 384
// pool_overrides add/remove lists.
#define kCustomizerMaxPoolOps 128

typedef struct CustomizerPin {
  uint16 location_id;  // resolved LOC_* id (location_registry.yaml)
  uint16 item_id;      // resolved ITEM_* id (item_registry.yaml)
} CustomizerPin;

typedef struct CustomizerManifest {
  CustomizerPin pins[kCustomizerMaxPins];
  uint16 pin_count;
  // pool_overrides: item ids to add to / remove from the per-settings pool
  // BEFORE assumed fill (applied remove-then-add). Resolved item ids.
  uint16 pool_add[kCustomizerMaxPoolOps];
  uint16 pool_add_count;
  uint16 pool_remove[kCustomizerMaxPoolOps];
  uint16 pool_remove_count;
  uint8 seed8[8];  // sha256(manifest_bytes)[0..8] — the share-string customizer_seed
} CustomizerManifest;

// Resolve a location / item NAME to its numeric registry id via a normalized
// (lowercase, alphanumeric-only) match over the codegen name tables. Accepts
// the symbol form, the human form, or any punctuation variant that normalizes
// the same. Returns 0xFFFF when the name resolves to nothing.
uint16 Customizer_ResolveLocation(const char *name);
uint16 Customizer_ResolveItem(const char *name);
// True for registry ids that exist for logic/prize bookkeeping but cannot be
// granted by the runtime dispatcher and therefore cannot be hand-placed/added.
bool Customizer_IsNonGrantableItem(uint16 item_id);

// Parse a manifest from an in-memory text buffer (len bytes). Returns 0 on
// success; on failure returns non-zero and writes a one-line human reason
// (with a line number where applicable) into err[errlen]. Populates
// out->seed8 = sha256(text,len)[0..8]. Rejects: unknown section, malformed
// entry, unknown location/item name, duplicate location key, pin overflow.
int Customizer_Parse(const char *text, size_t len, CustomizerManifest *out,
                     char *err, size_t errlen);

// Read a manifest file from disk and parse it. Returns 0 on success; non-zero
// writes a reason into err. Files larger than 1 MiB are rejected.
int Customizer_LoadFile(const char *path, CustomizerManifest *out,
                        char *err, size_t errlen);

// Install / clear the manifest the placer consults. The pointer is borrowed
// (not copied); the caller must keep it alive for the duration of generation.
// Pass NULL to clear (revert to pure assumed-fill).
void Customizer_Install(const CustomizerManifest *m);
const CustomizerManifest *Customizer_GetActive(void);

// After a failed Place_AssumedFill in customizer mode, returns a one-line
// reason for a HARD customizer error (an illegal pin: unknown/closed location,
// non-pinnable location type, or a double-pinned slot; or an over-cap
// manifest exceeding a per-item grant cap), or "" if the failure was an
// ordinary completability refusal. Cleared at the start of every
// Place_AssumedFill call.
const char *Customizer_LastError(void);
// Internal: set by the placer's customizer block. Declared here so
// rando_placement.c can write it. Not for general use.
void Customizer__SetError(const char *msg);
void Customizer__ClearError(void);

// Self-check: name-table normalized-uniqueness + a parse round-trip. Exits 2 on
// failure (per the Settings_SelfCheck / Placement_SelfCheck pattern).
void Customizer_SelfCheck(void);

// Placement-path regression guard: installs a manifest, runs the real placer,
// and asserts every pin is honored + the digest is deterministic. Exits 2 on
// failure. Run from Rando_RunAllSelfChecks (CI via --rando-selftest).
void Customizer_PlacementSelfCheck(void);

#endif  // ZELDA3_RANDO_CUSTOMIZER_H_
