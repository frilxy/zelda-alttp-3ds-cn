// inverted_entrances.{c,h} — static Inverted-mode entrance/exit override.
//
// When a rando slot's world_state is Inverted, the overworld door/exit asset
// tables must be repointed so a fixed set of entrance bindings swap. This
// mirrors ALTTPR's app/Rom.php setInvertedMode() (the kOverworld_Entrance_Id /
// kExitDataRooms / kExitData_ScreenIndex writes); it is a STATIC override set,
// not a per-seed permutation.
//
// This static override layer covers the extracted-data pieces of setInvertedMode:
//   - Link's House  <-> Bomb Shop
//   - Ganon's Tower <-> Agahnim's (Hyrule Castle) Tower
//   - Dark-World Death Mountain cave rewiring
//   - Dark-World flute destinations, including the Kakariko landing nudge
// The under-rock DW->LW warp records are installed by InvertedSecrets_Install
// below because they rebuild packed overworld-secret blobs instead of fixed
// parallel arrays.
//
// Idiom (copied from rando.c's Phase C entrance overlay): one static shadow
// buffer per touched asset array, copy vanilla -> shadow, apply that array's
// overrides, save the original g_asset_ptrs[idx], repoint to the shadow.
// Teardown restores every saved original pointer. Idempotent; a no-op unless
// world_state == kWorldState_Inverted.
//
// Non-inverted and non-rando paths are byte-identical: Install() returns
// immediately, the asset pointers are never touched, so a side-by-side
// RAM/VRAM compare against the original ROM is unaffected.
//
// ORDERING INVARIANT (same LoadAssets caveat as rando.c's entrance overlay):
// Teardown() MUST run before any LoadAssets() reload, because LoadAssets
// unconditionally rewrites every g_asset_ptrs[i] into a fresh buffer — that
// would both drop the override and leave the saved original pointers dangling.
// Today all LoadAssets call sites are startup/CLI-only, so this is latent; a
// future hot-reload must tear down first.
//
// MUTUAL EXCLUSION with entrance shuffle: the Phase C entrance shuffle bails for
// non-Open/Standard world states (shuffle_entrance.c, Entrance_IsActive), so on
// an Inverted slot Entrance_RuntimeInstall does NOT touch g_asset_ptrs[126].
// This subsystem therefore owns asset 126 alone while Inverted is active — no
// conflict. Install order in rando.c is Entrance_RuntimeInstall (no-op here)
// then InvertedEntrances_Install; teardown is the reverse.
#ifndef ZELDA3_RANDO_INVERTED_ENTRANCES_H_
#define ZELDA3_RANDO_INVERTED_ENTRANCES_H_

#include "../types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the static Inverted entrance/exit overrides. No-op unless
// world_state == kWorldState_Inverted (2). Tears down any prior install first,
// so a slot-switch without an intervening Teardown is safe. Idempotent.
void InvertedEntrances_Install(uint8 world_state);

// Restore every overridden asset pointer to its saved vanilla original.
// Idempotent; safe to call when nothing is installed.
void InvertedEntrances_Teardown(void);

// The saved PRISTINE g_asset_ptrs[126] (entrance-id table) while the Inverted
// override is installed; NULL when not installed. Entrance_PristineVanillaIds
// (rando.c) consults this so the entrance-shuffle digest/overlay always
// computes over the true vanilla table — e.g. when the native window generates
// an entrance-shuffle seed while an Inverted slot is still active, asset 126
// points at this subsystem's shadow, not the vanilla table.
const uint8 *InvertedEntrances_SavedEntranceIdOrig(void);

// Test support (--rando-selftest): the distinct asset indices this subsystem
// shadows. Writes up to `cap` indices into `out`; returns the total count.
// Lets the contamination selfcheck synthesize sources for absent assets
// (selftest runs without zelda3_assets.dat) without duplicating the list.
int InvertedEntrances_ShadowedAssets(uint8 *out, int cap);

// Headless regression guard for the static override table, including the
// Inverted Death Mountain exit-row coordinate mapping and flute landing nudge.
void InvertedEntrances_SelfCheck(void);

// Install the Inverted overworld DW->LW under-rock warps (overworld-secret
// type-0x82 records). No-op unless world_state == kWorldState_Inverted (2).
// Additive (vanilla has no warps); rebuilds the kOverworldSecrets blob + _Offs
// and repoints them. Tears down any prior install first. Idempotent.
void InvertedSecrets_Install(uint8 world_state);

// Restore the kOverworldSecrets / _Offs asset pointers to vanilla. Idempotent.
void InvertedSecrets_Teardown(void);

#ifdef __cplusplus
}
#endif

#endif  // ZELDA3_RANDO_INVERTED_ENTRANCES_H_
