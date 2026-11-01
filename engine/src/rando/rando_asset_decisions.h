// rando_asset_decisions.{h,c} — shared asset-warn "always allow" decision store.
//
// Promoted out of select_file.c (Phase P3) so both UIs (the in-game settings
// screen and the native settings window) plus the INI loader/writer share one
// source of truth. The decision is keyed by hex(g_assets_hash); when the user
// picks "Always allow", we record their hash + decision in this session-static
// lookup. On next launch the config parser repopulates the lookup from the INI
// ([RandoAssetDecisions] section).
//
// This module is plain C and lives in src/rando/, so it is compiled on Switch
// too (the in-game settings screen consults it there). It has NO window/ImGui
// dependencies.
#ifndef ZELDA3_RANDO_RANDO_ASSET_DECISIONS_H_
#define ZELDA3_RANDO_RANDO_ASSET_DECISIONS_H_

#include "../types.h"

// Returns true if `hash` (32 bytes) has a recorded "always allow" decision.
bool AssetDecision_FindAllow(const uint8 hash[32]);

// Record an "always allow" decision for `hash`. Dedups: updates the existing
// entry in place if the hash is already recorded; otherwise inserts (capped at
// kAssetDecisions_Max).
void AssetDecision_Persist(const uint8 hash[32]);

// Called from config.c's [RandoAssetDecisions] parser to populate the session
// lookup from zelda3.ini (or the sidecar INI) at startup. Dedups against
// existing entries so reading the same hash from two INI sources does not
// double-insert.
void Rando_RegisterAssetDecisionFromIni(const uint8 hash[32]);

// Read accessors for enumerating persisted always-allow decisions (used by the
// sidecar writer in config.c). AssetDecision_Count returns how many decisions
// are recorded; AssetDecision_HashAt returns a pointer to the 32-byte hash at
// `index` (NULL if out of range). Only "allow" decisions are stored, so every
// enumerated entry is an always-allow.
int AssetDecision_Count(void);
const uint8 *AssetDecision_HashAt(int index);

#endif  // ZELDA3_RANDO_RANDO_ASSET_DECISIONS_H_
