// souls.c — enemy/boss souls runtime (add-enemy-souls). See souls.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "souls.h"
#define SOUL_TABLES_IMPL  // this TU owns the soul table definitions
#include "soul_tables.h"
#define NPC_SOUL_TABLES_IMPL  // and the NPC soul site table (add-npc-souls)
#include "npc_soul_tables.h"
#include "item_ids.h"
#include "rando.h"
#include "rando_settings.h"
#include "rando_logic.h"  // kRandoSoulPinRooms / kRandoSoulRoomsBaked

// The registry must stay within the fixed RandoCounts.by_item_id[256]
// inventory-snapshot capacity (rando_logic.h); souls brought it to 196.
_Static_assert(ITEM__COUNT <= 256, "by_item_id[256] capacity exceeded");

// The soul registry block must be contiguous and mirror kSoul_* order —
// gen_soul_tables.py emits both ends from one list; these pin the contract.
#define kSoulItemBase ITEM_Soul_ArmosKnights
_Static_assert(ITEM_Soul_Ganon == kSoulItemBase + kSoul_Ganon,
               "soul registry block must mirror kSoul_* order");
_Static_assert(ITEM_Soul_Tektite == kSoulItemBase + kSoulCount - 1,
               "soul registry block must be contiguous through the last soul");
_Static_assert(kSoulCount + kNpcSoulCount <= kSoulFlagsBytes * 8,
               "soul ownership bitfield too small");
// add-npc-souls: the NPC block is contiguous immediately after the enemy
// block — gen_npc_soul_tables.py emits both ends from one roster list.
_Static_assert(ITEM_Soul_Npc_Sahasrahla == kSoulItemBase + kSoulCount,
               "NPC soul registry block must start right after the enemy block");
_Static_assert(ITEM_Soul_Npc_Uncle ==
                   kSoulItemBase + kSoulCount + kNpcSoulCount - 1,
               "NPC soul registry block must be contiguous through the last soul");

// Process-static live ownership (the g_rando_boomerang_owned pattern):
// captured into the sidecar v5 extension block / souls snapshot-tail TLV at
// save time, restored on slot activation and cold replay, zeroed otherwise.
static uint8 g_soul_flags[kSoulFlagsBytes];

// Per-room-load suppressed-spawn counter for the kill-gate hold. Reset by
// Dungeon_LoadSprites before parsing the room's static sprite list.
static uint8 g_room_suppressed;

uint8 Souls_ActiveTier(void) {
  const RandoSettings *s = Rando_GetActiveSettings();
  if (s == NULL) return kSoulsShuffle_Off;
  // EFFECTIVE tier (enemies degrades to bosses under door shuffle) so runtime
  // suppression can never bind stricter than the placement was certified for.
  return Settings_EffectiveSoulsShuffle(s);
}

bool Souls_OwnedIndex(uint8 soul_index) {
  if (soul_index >= kSoulCount + kNpcSoulCount) return false;
  return (g_soul_flags[soul_index >> 3] >> (soul_index & 7)) & 1;
}

bool Souls_SpriteAllowed(uint8 final_type) {
  uint8 tier = Souls_ActiveTier();
  if (tier == kSoulsShuffle_Off) return true;
  uint8 soul = kSoulForSprite[final_type];
  if (soul == 0xFF) return true;  // species without a soul always spawns
  if (soul >= kSoulBossCount && tier < kSoulsShuffle_BossesEnemies)
    return true;  // enemy-family souls only bind at the bosses+enemies tier
  return Souls_OwnedIndex(soul);
}

bool Souls_EnemiesTierActive(void) {
  return Souls_ActiveTier() >= kSoulsShuffle_BossesEnemies;
}

bool Souls_RoomPinnedVanilla(uint16 room) {
  // kRandoSoulPinRooms is sorted ascending (rando_logic_gen.py emits sorted
  // unique rooms); binary search. Count 0 when soul_rooms.gen.yaml was absent
  // at codegen — an enemies-tier seed can't be GENERATED then (BuildItemPool
  // fails closed on !kRandoSoulRoomsBaked), but a foreign slot could still be
  // LOADED; pinning nothing matches that build's (absent) logic terms.
  int lo = 0, hi = (int)kRandoSoulPinRoomsCount;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    if (kRandoSoulPinRooms[mid] == room) return true;
    if (kRandoSoulPinRooms[mid] < room) lo = mid + 1; else hi = mid;
  }
  return false;
}

bool Souls_ItemIsSoul(uint16 item_id) {
  return item_id >= kSoulItemBase &&
         item_id < kSoulItemBase + kSoulCount + kNpcSoulCount;
}

bool Souls_NpcActive(void) {
  const RandoSettings *s = Rando_GetActiveSettings();
  return s != NULL && s->npc_souls != 0;
}

bool Souls_NpcOwned(uint8 npc_soul) {
  if (npc_soul >= kNpcSoulCount) return false;
  return Souls_OwnedIndex((uint8)(kSoulCount + npc_soul));
}

bool Souls_NpcSpriteAllowed(uint8 type, uint16 key, bool is_room) {
  if (!Souls_NpcActive()) return true;
  for (uint16 i = 0; i < kNpcSoulSiteCount; i++) {
    const NpcSoulSite *site = &kNpcSoulSites[i];
    if (site->type != type) continue;
    if ((site->is_room != 0) != is_room) continue;
    if (site->key != key) continue;
    // site->world is generator-validated documentation: area ids already
    // encode the world (0x40-0x7F = DW; interiors are world=any by assertion),
    // so the (type, kind, key) match is complete.
    return Souls_NpcOwned(site->npc_soul);
  }
  return true;  // un-sited (type,key) combinations always spawn
}

void Souls_GrantItem(uint16 item_id) {
  if (!Souls_ItemIsSoul(item_id)) return;
  uint16 idx = (uint16)(item_id - kSoulItemBase);
  g_soul_flags[idx >> 3] |= (uint8)(1u << (idx & 7));
}

void Souls_ResetRoomSuppressed(void) { g_room_suppressed = 0; }

void Souls_NoteRoomSuppressed(void) {
  if (g_room_suppressed != 0xFF) g_room_suppressed++;
}

uint8 Souls_RoomSuppressedCount(void) { return g_room_suppressed; }

uint8 *Souls_Flags(void) { return g_soul_flags; }

void Souls_ResetFlags(void) {
  memset(g_soul_flags, 0, sizeof(g_soul_flags));
  g_room_suppressed = 0;
}

static int Souls_SelfCheckBody(void) {
  // Catalog sanity: every boss-pool slot maps to a boss-tier soul, and the
  // sprite map only references valid soul indices.
  for (int i = 0; i < 12; i++) {
    if (kBossPoolSoul[i] >= kSoulBossCount) {
      fprintf(stderr, "Souls_SelfCheck: kBossPoolSoul[%d] out of boss range\n", i);
      return 1;
    }
  }
  for (int t = 0; t < 256; t++) {
    if (kSoulForSprite[t] != 0xFF && kSoulForSprite[t] >= kSoulCount) {
      fprintf(stderr, "Souls_SelfCheck: kSoulForSprite[0x%02x] out of range\n", t);
      return 1;
    }
  }
  // The soul map must not cover the dynamic-spawn item/effect types the grant
  // and ancilla paths create through Sprite_SpawnDynamically (over-suppression
  // guard): absorbable drops (0xD8-0xEC), the key-drop control ids handled by
  // PrepareEnemyDrop (0xE4/0xE5 as item forms), and the receipt sprite.
  for (int t = 0xD8; t <= 0xEC; t++) {
    if (kSoulForSprite[t] != 0xFF) {
      fprintf(stderr, "Souls_SelfCheck: soul map covers item-drop type 0x%02x\n", t);
      return 1;
    }
  }
  // Enemy-shuffle pin table (logic_data.c, from soul_rooms.gen.yaml): sorted
  // strictly ascending (Souls_RoomPinnedVanilla binary-searches it), in room
  // range, and non-empty exactly when the kill-room data is baked.
  if ((kRandoSoulRoomsBaked != 0) != (kRandoSoulPinRoomsCount != 0)) {
    fprintf(stderr, "Souls_SelfCheck: baked flag / pin-room count mismatch\n");
    return 1;
  }
  for (uint16 i = 0; i < kRandoSoulPinRoomsCount; i++) {
    if (kRandoSoulPinRooms[i] >= 0x128 ||
        (i > 0 && kRandoSoulPinRooms[i] <= kRandoSoulPinRooms[i - 1])) {
      fprintf(stderr, "Souls_SelfCheck: pin room table unsorted/out-of-range at %u\n", i);
      return 1;
    }
  }
  // Grant/ownership round-trip on a scratch copy.
  uint8 saved[kSoulFlagsBytes];
  memcpy(saved, g_soul_flags, sizeof(saved));
  memset(g_soul_flags, 0, sizeof(g_soul_flags));
  if (Souls_OwnedIndex(kSoul_Ganon)) { memcpy(g_soul_flags, saved, sizeof(saved)); return 1; }
  Souls_GrantItem((uint16)(kSoulItemBase + kSoul_Ganon));
  bool ok = Souls_OwnedIndex(kSoul_Ganon) && !Souls_OwnedIndex(kSoul_ArmosKnights) &&
            Souls_ItemIsSoul(kSoulItemBase) &&
            Souls_ItemIsSoul((uint16)(kSoulItemBase + kSoulCount - 1)) &&
            Souls_ItemIsSoul((uint16)(kSoulItemBase + kSoulCount)) &&  // 1st NPC soul
            Souls_ItemIsSoul((uint16)(kSoulItemBase + kSoulCount + kNpcSoulCount - 1)) &&
            !Souls_ItemIsSoul((uint16)(kSoulItemBase + kSoulCount + kNpcSoulCount)) &&
            !Souls_ItemIsSoul((uint16)(kSoulItemBase - 1));
  // add-npc-souls: grant/ownership round-trip through the NPC block.
  Souls_GrantItem(ITEM_Soul_Npc_Kiki);
  ok = ok && Souls_NpcOwned(kNpcSoul_Kiki) && !Souls_NpcOwned(kNpcSoul_Stumpy);
  // Site-table sanity: valid soul indices, sane keys, the 0xE9/0xC0-room
  // exclusions the generator promises (defense against a hand-edited header).
  for (uint16 i = 0; ok && i < kNpcSoulSiteCount; i++) {
    const NpcSoulSite *st = &kNpcSoulSites[i];
    if (st->npc_soul >= kNpcSoulCount || st->type == 0xE9 ||
        (st->type == 0xC0 && st->is_room) ||
        (st->is_room && st->key >= 0x128) ||
        (!st->is_room && st->key >= 0x90) ||
        (st->is_room && st->world != 0)) {
      fprintf(stderr, "Souls_SelfCheck: bad NPC site row %u\n", i);
      ok = false;
    }
  }
  memcpy(g_soul_flags, saved, sizeof(saved));
  return ok ? 0 : 1;
}

void Souls_SelfCheck(void) {
  if (Souls_SelfCheckBody() != 0) {
    fprintf(stderr, "[Souls_SelfCheck] FAILED\n");
    exit(2);
  }
  fprintf(stderr, "[Souls_SelfCheck] OK\n");
}
