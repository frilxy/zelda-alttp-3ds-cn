// souls.h — enemy/boss souls runtime (add-enemy-souls).
//
// Soul items gate whether the matching enemy/boss SPAWNS. Without the soul the
// sprite is suppressed at all three spawn layers (static dungeon room load,
// overworld proximity load, the Sprite_SpawnDynamically funnel), and rooms
// whose kill-quota enemies were suppressed hold their kill-gates SHUT
// (Sprite_CheckIfScreenIsClear / RoomIsClear report not-clear) until the
// player returns with the soul and clears the room for real.
//
// The catalog (species -> soul, boss parts -> parent soul, boss-pool ->
// soul item) lives in generated soul_tables.h (gen_soul_tables.py). Ownership
// is a process-static bitfield captured by the sidecar v5 extension block and
// the souls snapshot-tail TLV; it is NOT g_ram state (no free reserved bytes —
// the g_rando_boomerang_owned precedent).

#ifndef ZELDA3_RANDO_SOULS_H_
#define ZELDA3_RANDO_SOULS_H_

#include "../types.h"

// 12 bytes = 96 bits: kSoulCount (46) enemy/boss souls + kNpcSoulCount (24)
// NPC souls (add-npc-souls widened 8->12; sidecar v7 + length-grown TLV carry
// the widened field, and pre-widening saves load with the tail zeroed).
// souls.c statically asserts the sum fits.
#define kSoulFlagsBytes 12

// Active souls tier (SoulsShuffle enum value). 0 (Off) when no slot is active
// or the active sidecar predates settings blobs (pre-souls seed by
// construction — suppressing on such a seed could make it unbeatable).
uint8 Souls_ActiveTier(void);

// Spawn gate. True = spawn normally; false = suppress. `final_type` is the
// POST-shuffle sprite type (after EnemyShuffle_Pick* / the boss-shuffle room
// redirect). Species without a soul in the active tier always spawn.
bool Souls_SpriteAllowed(uint8 final_type);

// True at the bosses+enemies tier (enemy-family souls bind). Gates the
// enemy-shuffle vanilla pin below and nothing else — boss souls need no pin
// (bosses are never substituted).
bool Souls_EnemiesTierActive(void);

// True when `room` must keep its VANILLA species under enemy shuffle at the
// enemies tier: kill-gated rooms (their logic soul terms name the vanilla
// residents) and forced key-drop rooms (the drop location predicates + the
// edc=off drop-source exemption name the vanilla holder). Data:
// kRandoSoulPinRooms baked into logic_data.c from soul_rooms.gen.yaml.
bool Souls_RoomPinnedVanilla(uint16 room);

// True when the registry item id is a soul item (enemy/boss OR NPC souls —
// the combined contiguous block starting at ITEM_Soul_ArmosKnights).
bool Souls_ItemIsSoul(uint16 item_id);
// Direct-grant: set the ownership bit for a soul item id (no-op otherwise).
void Souls_GrantItem(uint16 item_id);
// Ownership by soul index (kSoul_* from soul_tables.h).
bool Souls_OwnedIndex(uint8 soul_index);

// Kill-gate hold: Dungeon_LoadSprites resets this at the top of each room
// sprite load and every suppressed STATIC entry increments it; the room-clear
// checks treat a nonzero count as "not clear" (gate stays shut). Ganon's door
// gate uses the same count.
void Souls_ResetRoomSuppressed(void);
void Souls_NoteRoomSuppressed(void);
uint8 Souls_RoomSuppressedCount(void);

// Persistence surface (sidecar v5 ext block + snapshot-tail TLV): the raw
// kSoulFlagsBytes-byte ownership bitfield, bit index = soul index.
uint8 *Souls_Flags(void);
void Souls_ResetFlags(void);

// add-npc-souls — NPC spawn gate for the STATIC hooks only (room static
// load / overworld proxima). True = spawn normally. Site-scoped: (type, room)
// for interiors, (type, area) for overworld — kNpcSoulSites from
// npc_soul_tables.h. NEVER call from Sprite_SpawnDynamicallyEx: the dynamic
// 0xC0 reward deliveries (King Zora flippers / Catfish medallion) must always
// spawn. Off (or no active slot) => always true.
bool Souls_NpcSpriteAllowed(uint8 type, uint16 key, bool is_room);
// True when the active slot has npc_souls on.
bool Souls_NpcActive(void);
// Ownership by NPC soul index (kNpcSoul_* from npc_soul_tables.h).
bool Souls_NpcOwned(uint8 npc_soul);

void Souls_SelfCheck(void);

#endif  // ZELDA3_RANDO_SOULS_H_
