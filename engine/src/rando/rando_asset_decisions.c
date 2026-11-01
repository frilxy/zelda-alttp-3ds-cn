// rando_asset_decisions.c — see rando_asset_decisions.h.
//
// Promoted from select_file.c (Phase P3). Behavior of AssetDecision_FindAllow /
// AssetDecision_Persist is unchanged; Rando_RegisterAssetDecisionFromIni gains a
// dedup existence-check (mirroring AssetDecision_Persist) so union-reading the
// same hash from both zelda3.ini and the sidecar INI does not double-insert.
#include "rando_asset_decisions.h"

#include <string.h>
#include <stdio.h>

// Asset-warn persistence ([RandoAssetDecisions] in zelda3.ini).
//
// The decision is keyed by hex(g_assets_hash). When the user picks "Always
// allow", we record their hash + decision in a session-static lookup; on
// next launch the config parser populates the lookup from the INI. For
// Phase A the persistence is in-memory only (the INI write-back is deferred
// to a follow-up sprint — config.c is read-only today). The session-static
// lookup ensures the user is not re-prompted within a single session even
// without on-disk persistence.
// ---------------------------------------------------------------------------
typedef struct AssetDecision {
  uint8 hash[32];
  uint8 allow;  // 1 = always-allow recorded for this hash
} AssetDecision;
#define kAssetDecisions_Max 16
static AssetDecision g_asset_decisions[kAssetDecisions_Max];
static uint8 g_asset_decisions_count = 0;

bool AssetDecision_FindAllow(const uint8 hash[32]) {
  for (uint8 i = 0; i < g_asset_decisions_count; ++i) {
    if (memcmp(g_asset_decisions[i].hash, hash, 32) == 0) {
      return g_asset_decisions[i].allow != 0;
    }
  }
  return false;
}

void AssetDecision_Persist(const uint8 hash[32]) {
  // Update in-place if hash already recorded.
  for (uint8 i = 0; i < g_asset_decisions_count; ++i) {
    if (memcmp(g_asset_decisions[i].hash, hash, 32) == 0) {
      g_asset_decisions[i].allow = 1;
      return;
    }
  }
  if (g_asset_decisions_count >= kAssetDecisions_Max) return;
  AssetDecision *d = &g_asset_decisions[g_asset_decisions_count++];
  memcpy(d->hash, hash, 32);
  d->allow = 1;
  // Phase A: persistence is in-memory only. config.c's HandleIniConfig
  // already accepts the new [RandoAssetDecisions] section so future INI
  // edits by the user (or by a future INI-writer task) survive a launch.
  // Until the writer lands, the decision persists for the current session.
  fprintf(stderr, "[RandoAssetDecisions] recorded always-allow for hash ");
  for (int i = 0; i < 8; ++i) fprintf(stderr, "%02x", hash[i]);
  fprintf(stderr, "...\n");
}

// Called from config.c's [RandoAssetDecisions] parser to populate the
// session lookup from zelda3.ini at startup.
void Rando_RegisterAssetDecisionFromIni(const uint8 hash[32]) {
  // Dedup: union-reading the same hash from two INI sources (zelda3.ini +
  // the sidecar) must not insert a duplicate. Mirror AssetDecision_Persist:
  // update in place if already recorded.
  for (uint8 i = 0; i < g_asset_decisions_count; ++i) {
    if (memcmp(g_asset_decisions[i].hash, hash, 32) == 0) {
      g_asset_decisions[i].allow = 1;
      return;
    }
  }
  if (g_asset_decisions_count >= kAssetDecisions_Max) return;
  AssetDecision *d = &g_asset_decisions[g_asset_decisions_count++];
  memcpy(d->hash, hash, 32);
  d->allow = 1;
}

int AssetDecision_Count(void) {
  return (int)g_asset_decisions_count;
}

const uint8 *AssetDecision_HashAt(int index) {
  if (index < 0 || index >= (int)g_asset_decisions_count) return NULL;
  return g_asset_decisions[index].hash;
}
