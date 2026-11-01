// chains_runtime.c - dungeon-chain runtime hooks.

#include "chains_runtime.h"

#include "chain_boss_entrances.gen.h"
#include "chain_seams.gen.h"
#include "door_tables.gen.h"
#include "rando.h"
#include "souls.h"

#include "../assets.h"
#include "../dungeon.h"
#include "../features.h"
#include "../hud.h"
#include "../variables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define kChainsEntranceOverlayMax 4096

static bool g_chains_hop_pending;
static bool g_chains_runtime_active;
static bool g_chains_origin_active;
static bool g_chains_terminal_active;
static bool g_chains_cleared_terminal_exit_pending;
static uint16 g_chains_origin_exit_room;
static uint8 g_chains_terminal_dungeon = kRandoDungeon_None;
static DungeonChainsLayout g_chains_runtime_layout;
static uint8 g_chains_entrance_overlay[kChainsEntranceOverlayMax];
static const uint8 *g_chains_entrance_overlay_orig;

_Static_assert(kChainBossEntranceBase == 133,
               "synthetic chain boss entrances must append after vanilla rows");
_Static_assert(kChainBossEntranceCount == 9,
               "dungeon-chain boss entrance count drift");

#define kChainsDesertEastEntranceId 0x0A
#define kChainsDesertWestEntranceId 0x0B
#define kChainsDesertBackEntranceId 0x0C
#define kChainsDesertEastExitRoom 0x085
#define kChainsDesertWestExitRoom 0x083
#define kChainsDesertBackExitRoom 0x063

// TR's Dark Death Mountain ledge doors (unshuffled aux portals, like DP's).
// Each exit room is the interior room its entrance loads into; all three are
// exit-table rooms, so a chain exit can resolve an overworld door from them.
#define kChainsTurtleLazyEyesEntranceId 0x15
#define kChainsTurtleEyeBridgeEntranceId 0x18
#define kChainsTurtleBigChestEntranceId 0x19
#define kChainsTurtleLazyEyesExitRoom 0x023
#define kChainsTurtleEyeBridgeExitRoom 0x0D5
#define kChainsTurtleBigChestExitRoom 0x024

uint8 Chains_BossEntranceForRandoDungeon(uint8 rando_dungeon) {
  for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
    if (kChainBossEntranceChecks[i].rando_dungeon == rando_dungeon)
      return kChainBossEntranceChecks[i].entrance_id;
  }
  return 0xFF;
}

static const ChainBossEntranceCheck *Chains_CheckForRandoDungeon(uint8 rando_dungeon) {
  for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
    if (kChainBossEntranceChecks[i].rando_dungeon == rando_dungeon)
      return &kChainBossEntranceChecks[i];
  }
  return NULL;
}

static uint8 Chains_MainEntranceForRandoDungeon(uint8 rando_dungeon) {
  const ChainBossEntranceCheck *row = Chains_CheckForRandoDungeon(rando_dungeon);
  if (row != NULL)
    return row->main_entrance_id;
  return 0xFF;
}

static uint16 Chains_MainExitRoomForRandoDungeon(uint8 rando_dungeon) {
  const ChainBossEntranceCheck *row = Chains_CheckForRandoDungeon(rando_dungeon);
  if (row != NULL)
    return row->main_exit_room;
  return 0;
}

static uint8 Chains_EntranceForElement(uint8 elem) {
  uint8 rando_dungeon = Chains_ElementDungeon(elem);
  if (elem == kChainsElement_None || Chains_PoolIndexForDungeon(rando_dungeon) < 0)
    return 0xFF;
  return Chains_ElementIsBoss(elem)
             ? Chains_BossEntranceForRandoDungeon(rando_dungeon)
             : Chains_MainEntranceForRandoDungeon(rando_dungeon);
}

static bool Chains_EntranceAssetCountOk(uint32 size, uint32 bytes_per_entry) {
  return size >= (uint32)kChainBossEntranceLimit * bytes_per_entry;
}

bool Chains_SyntheticEntrancesAvailable(void) {
  if (kEntranceData_rooms == NULL || kEntranceData_palace == NULL ||
      kEntranceData_musicTrack == NULL) {
    return false;
  }
  if (!Chains_EntranceAssetCountOk(kEntranceData_rooms_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_relativeCoords_SIZE, 8) ||
      !Chains_EntranceAssetCountOk(kEntranceData_scrollX_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_scrollY_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_playerX_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_playerY_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_cameraX_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_cameraY_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_blockset_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_floor_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_palace_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_doorwayOrientation_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_startingBg_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_quadrant1_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_quadrant2_SIZE, 1) ||
      !Chains_EntranceAssetCountOk(kEntranceData_doorSettings_SIZE, 2) ||
      !Chains_EntranceAssetCountOk(kEntranceData_musicTrack_SIZE, 1)) {
    return false;
  }
  for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
    const ChainBossEntranceCheck *row = &kChainBossEntranceChecks[i];
    uint8 entrance = row->entrance_id;
    if (kEntranceData_rooms[entrance] != row->room ||
        kEntranceData_palace[entrance] != row->palace ||
        kEntranceData_musicTrack[entrance] != row->music) {
      return false;
    }
  }
  return true;
}

static bool Chains_InstallEntranceOverlay(const DungeonChainsLayout *layout) {
  if (layout == NULL || kOverworld_Entrance_Id == NULL)
    return false;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (len == 0 || len > kChainsEntranceOverlayMax)
    return false;

  const uint8 *vanilla = (const uint8 *)kOverworld_Entrance_Id;
  memcpy(g_chains_entrance_overlay, vanilla, len);
  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    uint8 source_dungeon = Chains_PoolDungeonAt(i);
    uint8 source_entrance = Chains_MainEntranceForRandoDungeon(source_dungeon);
    uint8 target_entrance = Chains_EntranceForElement(layout->chain_door_first[i]);
    if (source_entrance == 0xFF || target_entrance == 0xFF)
      return false;

    bool found = false;
    for (uint32 lx = 0; lx < len; lx++) {
      if (vanilla[lx] == source_entrance) {
        g_chains_entrance_overlay[lx] = target_entrance;
        found = true;
      }
    }
    if (!found)
      return false;
  }

  g_chains_entrance_overlay_orig = vanilla;
  g_asset_ptrs[126] = g_chains_entrance_overlay;
  return true;
}

void Chains_RuntimeTeardown(void) {
  if (g_chains_entrance_overlay_orig != NULL) {
    g_asset_ptrs[126] = (void *)g_chains_entrance_overlay_orig;
    g_chains_entrance_overlay_orig = NULL;
  }
  g_chains_runtime_active = false;
  g_chains_origin_active = false;
  g_chains_terminal_active = false;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_origin_exit_room = 0;
  g_chains_terminal_dungeon = kRandoDungeon_None;
  g_chains_hop_pending = false;
  memset(&g_chains_runtime_layout, 0xFF, sizeof(g_chains_runtime_layout));
}

bool Chains_RuntimeInstallLayout(const DungeonChainsLayout *layout) {
  if (layout == NULL) {
    Chains_RuntimeTeardown();
    return false;
  }
  DungeonChainsLayout copy = *layout;
  Chains_RuntimeTeardown();
  if (!Chains_SyntheticEntrancesAvailable())
    return false;
  if (!Chains_InstallEntranceOverlay(&copy))
    return false;
  g_chains_runtime_layout = copy;
  g_chains_runtime_active = true;
  return true;
}

void Chains_RuntimeArmOrigin(uint16 origin_exit_room) {
  g_chains_origin_active = origin_exit_room != 0;
  g_chains_origin_exit_room = origin_exit_room;
  g_chains_terminal_active = false;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_terminal_dungeon = kRandoDungeon_None;
}

void Chains_RuntimeClearOrigin(void) {
  g_chains_origin_active = false;
  g_chains_terminal_active = false;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_origin_exit_room = 0;
  g_chains_terminal_dungeon = kRandoDungeon_None;
}

static bool Chains_IsMainExitRoom(uint16 room) {
  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    if (Chains_MainExitRoomForRandoDungeon(Chains_PoolDungeonAt(i)) == room)
      return true;
  }
  return false;
}

static bool Chains_IsDesertAuxExitRoom(uint16 room) {
  return room == kChainsDesertEastExitRoom ||
         room == kChainsDesertWestExitRoom ||
         room == kChainsDesertBackExitRoom;
}

static bool Chains_IsTurtleAuxExitRoom(uint16 room) {
  return room == kChainsTurtleLazyEyesExitRoom ||
         room == kChainsTurtleEyeBridgeExitRoom ||
         room == kChainsTurtleBigChestExitRoom;
}

static bool Chains_IsValidOriginExitRoom(uint16 room) {
  return Chains_IsMainExitRoom(room) || Chains_IsDesertAuxExitRoom(room) ||
         Chains_IsTurtleAuxExitRoom(room);
}

bool Chains_RuntimeGetSession(ChainsRuntimeSession *out) {
  if (out == NULL)
    return false;
  memset(out, 0, sizeof(*out));
  if (!g_chains_runtime_active)
    return false;
  out->origin_active = g_chains_origin_active;
  out->terminal_active = g_chains_terminal_active;
  out->origin_exit_room = g_chains_origin_exit_room;
  out->terminal_dungeon = g_chains_terminal_dungeon;
  return true;
}

bool Chains_RuntimeRestoreSession(const ChainsRuntimeSession *session) {
  if (!g_chains_runtime_active || session == NULL)
    return false;
  if (!session->origin_active) {
    if (session->terminal_active || session->origin_exit_room != 0)
      return false;
    Chains_RuntimeClearOrigin();
    return true;
  }
  if (session->origin_exit_room == 0 ||
      !Chains_IsValidOriginExitRoom(session->origin_exit_room)) {
    return false;
  }
  if (session->terminal_active &&
      Chains_PoolIndexForDungeon(session->terminal_dungeon) < 0) {
    return false;
  }

  g_chains_origin_active = true;
  g_chains_origin_exit_room = session->origin_exit_room;
  g_chains_terminal_active = session->terminal_active;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_terminal_dungeon = session->terminal_active
                                  ? session->terminal_dungeon
                                  : (uint8)kRandoDungeon_None;
  return true;
}

bool Chains_RuntimeRecordDoorEntry(uint16 lx) {
  if (!g_chains_runtime_active || g_chains_entrance_overlay_orig == NULL)
    return false;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (lx >= len)
    return false;

  uint8 source_entrance = g_chains_entrance_overlay_orig[lx];
  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    uint8 source_dungeon = Chains_PoolDungeonAt(i);
    const ChainBossEntranceCheck *row = Chains_CheckForRandoDungeon(source_dungeon);
    if (row == NULL || row->main_entrance_id != source_entrance)
      continue;

    uint8 first = g_chains_runtime_layout.chain_door_first[i];
    Chains_RuntimeArmOrigin(row->main_exit_room);
    if (Chains_ElementIsBoss(first)) {
      g_chains_terminal_active = true;
      g_chains_terminal_dungeon = Chains_ElementDungeon(first);
    }
    return true;
  }
  return false;
}

uint16 Chains_RuntimeConsumeMainExitOrigin(uint16 resolved_exit_room) {
  if (!g_chains_runtime_active || !g_chains_origin_active ||
      g_chains_origin_exit_room == 0 || !Chains_IsMainExitRoom(resolved_exit_room)) {
    return 0;
  }

  uint16 origin = g_chains_origin_exit_room;
  g_chains_origin_active = false;
  g_chains_terminal_active = false;
  g_chains_origin_exit_room = 0;
  g_chains_terminal_dungeon = kRandoDungeon_None;
  return origin;
}

static void Chains_ClearFollowerForHop(void) {
  bool had_super_bomb = follower_indicator == 13 ||
                        super_bomb_indicator_unk1 != 0 ||
                        super_bomb_indicator_unk2 != 0;
  WORD(follower_indicator) = 0;
  follower_dropped = 0;
  saved_tagalong_y = 0;
  saved_tagalong_x = 0;
  saved_tagalong_indoors = 0;
  saved_tagalong_floor = 0;
  tagalong_var1 = 0;
  tagalong_var2 = 0;
  tagalong_var3 = 0;
  tagalong_var4 = 0;
  tagalong_var5 = 0;
  tagalong_var7 = 0;
  tagalong_event_flags = 0;
  timer_tagalong_reacquire = 0;
  if (had_super_bomb) {
    super_bomb_indicator_unk1 = 0;
    super_bomb_indicator_unk2 = 0;
    Hud_RemoveSuperBombIndicator();
  }
}

void Chains_ClearFollowerForChainStart(void) {
  Chains_ClearFollowerForHop();
}

static bool Chains_RequestEntranceHop(uint8 entrance,
                                      uint16 source_room,
                                      uint16 destination_room,
                                      bool terminal,
                                      uint8 terminal_dungeon) {
  (void)source_room;
  (void)destination_room;
  if (entrance >= kChainBossEntranceBase && entrance < kChainBossEntranceLimit &&
      !Chains_SyntheticEntrancesAvailable()) {
    return false;
  }

  g_chains_hop_pending = true;
  g_chains_terminal_active = terminal;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_terminal_dungeon = terminal ? terminal_dungeon : (uint8)kRandoDungeon_None;

  Chains_ClearFollowerForHop();
  WORD(death_var4) = 0;
  WORD(death_var5) = 0;
  room_transitioning_flags = 0;
  link_this_controls_sprite_oam = 0;
  player_near_pit_state = 0;
  link_visibility_status = 0;
  is_standing_in_doorway = 0;
  WORD(dung_cur_door_pos) = 0;
  WORD(door_animation_step_indicator) = 0;
  which_entrance = entrance;
  player_is_indoors = 1;
  subsubmodule_index = 0;
  submodule_index = 0;
  main_module_index = 6;  // Module_PreDungeon runs next frame.

  return true;
}

static const ChainSeamRow *Chains_FindBossSeam(uint8 kind,
                                               uint8 direction,
                                               uint16 source_room,
                                               uint16 destination_room,
                                               uint8 slot) {
  for (uint8 i = 0; i < kChainBossSeamCount; i++) {
    const ChainSeamRow *row = &kChainBossSeams[i];
    if (row->kind != kind ||
        row->source_room != source_room ||
        row->dest_room != destination_room) {
      continue;
    }
    if (direction != kChainSeamDir_None && row->direction != direction)
      continue;
    if (slot != 0xFF && row->slot != slot)
      continue;
    return row;
  }
  return NULL;
}

static const ChainSeamRow *Chains_FindOutboundSeam(uint8 kind,
                                                   uint8 direction,
                                                   uint16 source_room,
                                                   uint16 destination_room,
                                                   uint8 slot) {
  for (uint8 i = 0; i < kChainBossOutboundSeamCount; i++) {
    const ChainSeamRow *row = &kChainBossOutboundSeams[i];
    if (row->kind != kind ||
        row->source_room != source_room ||
        row->dest_room != destination_room) {
      continue;
    }
    if (direction != kChainSeamDir_None && row->direction != direction)
      continue;
    if (slot != 0xFF && row->slot != slot)
      continue;
    return row;
  }
  return NULL;
}

// DP's and TR's aux doors can reach their boss seams without a chain-start
// door. Use the loaded aux entrance as a minimal origin so the logic edge has
// runtime coupling.
static uint16 Chains_AuxOriginExitRoomForCurrentEntrance(uint8 rando_dungeon) {
  if (rando_dungeon == kRandoDungeon_DesertPalace) {
    switch (which_entrance) {
      case kChainsDesertEastEntranceId:
        return kChainsDesertEastExitRoom;
      case kChainsDesertWestEntranceId:
        return kChainsDesertWestExitRoom;
      case kChainsDesertBackEntranceId:
        return kChainsDesertBackExitRoom;
      default:
        return 0;
    }
  }
  if (rando_dungeon == kRandoDungeon_TurtleRock) {
    switch (which_entrance) {
      case kChainsTurtleLazyEyesEntranceId:
        return kChainsTurtleLazyEyesExitRoom;
      case kChainsTurtleEyeBridgeEntranceId:
        return kChainsTurtleEyeBridgeExitRoom;
      case kChainsTurtleBigChestEntranceId:
        return kChainsTurtleBigChestExitRoom;
      default:
        return 0;
    }
  }
  return 0;
}

static bool Chains_RequestTerminalExit(uint16 source_room,
                                       uint16 destination_room) {
  (void)source_room;
  (void)destination_room;
  if (!g_chains_origin_active || g_chains_origin_exit_room == 0)
    return false;

  uint16 origin_exit_room = g_chains_origin_exit_room;
  g_chains_cleared_terminal_exit_pending = false;
  SaveDungeonKeys();
  SaveQuadrantsToSram();
  dungeon_room_index = origin_exit_room;
  room_transitioning_flags = 0;
  is_standing_in_doorway = 0;
  link_visibility_status = 0;
  link_this_controls_sprite_oam = 0;
  player_near_pit_state = 0;
  fallhole_var1 = 0;
  fallhole_var2 = 0;
  link_speed_modifier = 0;
  link_speed_setting = 0;
  link_disable_sprite_damage = 0;
  saved_module_for_menu = 8;
  main_module_index = 15;
  submodule_index = 0;
  subsubmodule_index = 0;
  Dungeon_ResetTorchBackgroundAndPlayerInner();

  g_chains_origin_active = false;
  g_chains_terminal_active = false;
  g_chains_cleared_terminal_exit_pending = false;
  g_chains_origin_exit_room = 0;
  g_chains_terminal_dungeon = kRandoDungeon_None;
  return true;
}

static bool Chains_TerminalRewardChecked(uint8 rando_dungeon) {
  uint8 game_dungeon = Rando_GameDungeonFromRandoDungeon(rando_dungeon);
  uint16 prize_loc = Rando_BossPrizeLocationForGameDungeon(game_dungeon);
  return prize_loc != 0xFFFFu && Rando_IsLocationChecked(prize_loc);
}

// True while the loaded room is this terminal's boss room and its boss spawn
// was soul-suppressed on the room load (souls_shuffle >= bosses, soul unfound).
// Every static in the 9 pool boss rooms is the boss or one of its parts (the
// EP overlord entry bypasses the souls gate), so a nonzero suppressed count
// means the fight cannot start: no kill -> no prize -> the reward-gated
// escapes would seal Link in (Kholdstare's room has no door at all). The count
// is recomputed by every room sprite load, so this goes false the moment the
// soul is owned and the boss can spawn.
static bool Chains_TerminalBossSuppressed(uint8 rando_dungeon) {
  const ChainBossEntranceCheck *row = Chains_CheckForRandoDungeon(rando_dungeon);
  return row != NULL && row->room == dungeon_room_index &&
         Souls_RoomSuppressedCount() != 0;
}

static bool Chains_TerminalEscapeAllowed(uint8 rando_dungeon) {
  return Chains_TerminalRewardChecked(rando_dungeon) ||
         Chains_TerminalBossSuppressed(rando_dungeon);
}

bool Chains_TryTerminalOutboundSeam(uint8 kind,
                                    uint8 direction,
                                    uint16 source_room,
                                    uint16 vanilla_destination_room,
                                    uint8 slot) {
  if (!g_chains_runtime_active || !g_chains_terminal_active)
    return false;

  const ChainSeamRow *seam = Chains_FindOutboundSeam(kind, direction, source_room,
                                                     vanilla_destination_room, slot);
  if (seam == NULL || seam->rando_dungeon != g_chains_terminal_dungeon)
    return false;
  if (!Chains_TerminalRewardChecked(g_chains_terminal_dungeon))
    return false;

  return Chains_RequestTerminalExit(source_room, vanilla_destination_room);
}

bool Chains_TryClearedTerminalReentryExit(void) {
  if (!g_chains_cleared_terminal_exit_pending)
    return false;
  g_chains_cleared_terminal_exit_pending = false;
  if (!g_chains_runtime_active || !g_chains_terminal_active)
    return false;

  const ChainBossEntranceCheck *row = Chains_CheckForRandoDungeon(g_chains_terminal_dungeon);
  if (row == NULL || row->room != dungeon_room_index)
    return false;
  if (!Chains_TerminalEscapeAllowed(g_chains_terminal_dungeon))
    return false;

  return Chains_RequestTerminalExit(dungeon_room_index, dungeon_room_index);
}

bool Chains_TryBossSeamHop(uint8 kind,
                           uint8 direction,
                           uint16 source_room,
                           uint16 vanilla_destination_room,
                           uint8 slot) {
  if (!g_chains_runtime_active)
    return false;

  const ChainSeamRow *seam = Chains_FindBossSeam(kind, direction, source_room,
                                                 vanilla_destination_room, slot);
  if (seam == NULL)
    return false;

  bool synthesized_origin = false;
  if (!g_chains_origin_active) {
    uint16 aux_origin = Chains_AuxOriginExitRoomForCurrentEntrance(seam->rando_dungeon);
    if (aux_origin == 0)
      return false;
    Chains_RuntimeArmOrigin(aux_origin);
    synthesized_origin = true;
  }

  int pool_idx = Chains_PoolIndexForDungeon(seam->rando_dungeon);
  if (pool_idx < 0) {
    if (synthesized_origin)
      Chains_RuntimeClearOrigin();
    return false;
  }

  uint8 successor = g_chains_runtime_layout.chain_successor[pool_idx];
  if (successor == Chains_BossElement(seam->rando_dungeon)) {
    if (g_chains_origin_active) {
      g_chains_terminal_active = true;
      g_chains_terminal_dungeon = seam->rando_dungeon;
    }
    return false;
  }

  uint8 entrance = Chains_EntranceForElement(successor);
  if (entrance == 0xFF) {
    if (synthesized_origin)
      Chains_RuntimeClearOrigin();
    return false;
  }

  uint8 terminal_dungeon = Chains_ElementDungeon(successor);
  bool ok = Chains_RequestEntranceHop(entrance, source_room, vanilla_destination_room,
                                      Chains_ElementIsBoss(successor),
                                      terminal_dungeon);
  if (!ok && synthesized_origin)
    Chains_RuntimeClearOrigin();
  return ok;
}

bool Chains_ConsumeHopPending(void) {
  if (!g_chains_hop_pending)
    return false;
  g_chains_hop_pending = false;
  return true;
}

bool Chains_TryApplyBossEntranceLanding(void) {
  if (!g_chains_runtime_active)
    return false;

  const ChainBossEntranceCheck *boss_row = NULL;
  for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
    if (kChainBossEntranceChecks[i].entrance_id == which_entrance) {
      boss_row = &kChainBossEntranceChecks[i];
      break;
    }
  }
  if (boss_row == NULL)
    return false;
  if (g_chains_terminal_active &&
      g_chains_terminal_dungeon == boss_row->rando_dungeon) {
    // Escape eligibility (reward checked / boss soul-suppressed) is decided at
    // the module-7 consume: this runs inside Dungeon_LoadEntrance, before the
    // landing room's sprite load recomputes the suppressed count.
    g_chains_cleared_terminal_exit_pending = true;
  }

  const ChainSeamRow *outbound = NULL;
  for (uint8 i = 0; i < kChainBossOutboundSeamCount; i++) {
    const ChainSeamRow *row = &kChainBossOutboundSeams[i];
    if (row->kind == kChainSeamKind_Door &&
        row->rando_dungeon == boss_row->rando_dungeon &&
        row->source_room == boss_row->room) {
      outbound = row;
      break;
    }
  }
  if (outbound == NULL)
    return false;

  uint16 room_base_x = (uint16)((boss_row->room & 0x00Fu) << 9);
  uint16 room_base_y = (uint16)((boss_row->room & 0x1F0u) << 5);
  link_x_coord = room_base_x + 120 + outbound->slot * 128;
  link_y_coord = room_base_y + 424;
  link_direction_facing = 0;
  is_standing_in_doorway = 0;
  room_transitioning_flags = 0;
  dung_cur_door_pos = 0;
  door_animation_step_indicator = 0;
  return true;
}

static void Chains_RuntimeSelfCheckDie(const char *msg) {
  Chains_RuntimeTeardown();
  fprintf(stderr, "Chains_RuntimeSelfCheck: %s\n", msg);
  exit(2);
}

static uint16 Chains_RuntimeSelfCheckFindDoor(const uint8 *ids, uint32 len,
                                              uint8 entrance_id) {
  if (ids == NULL)
    return 0xFFFF;
  for (uint32 lx = 0; lx < len && lx <= 0xFFFFu; lx++) {
    if (ids[lx] == entrance_id)
      return (uint16)lx;
  }
  return 0xFFFF;
}

void Chains_RuntimeSelfCheck(void) {
  static uint8 synth_door_ids[256];
  static uint16 synth_u16[kChainBossEntranceLimit];
  static uint16 synth_rooms[kChainBossEntranceLimit];
  static uint8 synth_u8[kChainBossEntranceLimit];
  static uint8 synth_relative[kChainBossEntranceLimit * 8];
  static int8 synth_i8[kChainBossEntranceLimit];
  static int8 synth_palace[kChainBossEntranceLimit];
  static uint8 synth_music[kChainBossEntranceLimit];
  static const uint8 kSynthAssetIds[] = {
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 126,
  };
  const uint8 *saved_ptrs[sizeof(kSynthAssetIds)];
  uint32 saved_sizes[sizeof(kSynthAssetIds)];
  bool synth_assets = kOverworld_Entrance_Id == NULL ||
                      kOverworld_Entrance_Id_SIZE == 0 ||
                      !Chains_SyntheticEntrancesAvailable();
  if (synth_assets) {
    for (uint32 i = 0; i < (uint32)sizeof(kSynthAssetIds); i++) {
      uint8 id = kSynthAssetIds[i];
      saved_ptrs[i] = g_asset_ptrs[id];
      saved_sizes[i] = g_asset_sizes[id];
    }
    memset(synth_door_ids, 0, sizeof(synth_door_ids));
    memset(synth_u16, 0, sizeof(synth_u16));
    memset(synth_rooms, 0, sizeof(synth_rooms));
    memset(synth_u8, 0, sizeof(synth_u8));
    memset(synth_relative, 0, sizeof(synth_relative));
    memset(synth_i8, 0, sizeof(synth_i8));
    memset(synth_palace, 0, sizeof(synth_palace));
    memset(synth_music, 0, sizeof(synth_music));
    for (uint8 i = 0; i < kChainsPoolCount; i++) {
      const ChainBossEntranceCheck *row =
          Chains_CheckForRandoDungeon(Chains_PoolDungeonAt(i));
      synth_door_ids[16 + i] = row->main_entrance_id;
    }
    for (uint8 i = 0; i < kChainBossEntranceCount; i++) {
      const ChainBossEntranceCheck *row = &kChainBossEntranceChecks[i];
      synth_rooms[row->entrance_id] = row->room;
      synth_palace[row->entrance_id] = row->palace;
      synth_music[row->entrance_id] = row->music;
    }
    g_asset_ptrs[11] = (const uint8 *)synth_rooms;
    g_asset_sizes[11] = sizeof(synth_rooms);
    g_asset_ptrs[12] = synth_relative;
    g_asset_sizes[12] = sizeof(synth_relative);
    g_asset_ptrs[13] = (const uint8 *)synth_u16;
    g_asset_sizes[13] = sizeof(synth_u16);
    g_asset_ptrs[14] = (const uint8 *)synth_u16;
    g_asset_sizes[14] = sizeof(synth_u16);
    g_asset_ptrs[15] = (const uint8 *)synth_u16;
    g_asset_sizes[15] = sizeof(synth_u16);
    g_asset_ptrs[16] = (const uint8 *)synth_u16;
    g_asset_sizes[16] = sizeof(synth_u16);
    g_asset_ptrs[17] = (const uint8 *)synth_u16;
    g_asset_sizes[17] = sizeof(synth_u16);
    g_asset_ptrs[18] = (const uint8 *)synth_u16;
    g_asset_sizes[18] = sizeof(synth_u16);
    g_asset_ptrs[19] = synth_u8;
    g_asset_sizes[19] = sizeof(synth_u8);
    g_asset_ptrs[20] = (const uint8 *)synth_i8;
    g_asset_sizes[20] = sizeof(synth_i8);
    g_asset_ptrs[21] = (const uint8 *)synth_palace;
    g_asset_sizes[21] = sizeof(synth_palace);
    g_asset_ptrs[22] = synth_u8;
    g_asset_sizes[22] = sizeof(synth_u8);
    g_asset_ptrs[23] = synth_u8;
    g_asset_sizes[23] = sizeof(synth_u8);
    g_asset_ptrs[24] = synth_u8;
    g_asset_sizes[24] = sizeof(synth_u8);
    g_asset_ptrs[25] = synth_u8;
    g_asset_sizes[25] = sizeof(synth_u8);
    g_asset_ptrs[26] = (const uint8 *)synth_u16;
    g_asset_sizes[26] = sizeof(synth_u16);
    g_asset_ptrs[27] = synth_music;
    g_asset_sizes[27] = sizeof(synth_music);
    g_asset_ptrs[126] = synth_door_ids;
    g_asset_sizes[126] = sizeof(synth_door_ids);
  }

  const uint8 *vanilla = (const uint8 *)kOverworld_Entrance_Id;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (vanilla == NULL || len == 0 || !Chains_SyntheticEntrancesAvailable())
    Chains_RuntimeSelfCheckDie("asset fixture unavailable");

  DungeonChainsLayout layout;
  memset(&layout, kChainsElement_None, sizeof(layout));
  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    uint8 source = Chains_PoolDungeonAt(i);
    uint8 target = Chains_PoolDungeonAt((uint8)((i + 1) % kChainsPoolCount));
    layout.chain_door_first[i] = Chains_DungeonElement(target);
    layout.chain_successor[i] = Chains_BossElement(source);
    layout.terminal_boss[i] = source;
  }

  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("rotated layout install failed");
  if ((const uint8 *)kOverworld_Entrance_Id == vanilla)
    Chains_RuntimeSelfCheckDie("asset 126 was not repointed");

  for (uint8 i = 0; i < kChainsPoolCount; i++) {
    uint8 source = Chains_PoolDungeonAt(i);
    uint8 target = Chains_PoolDungeonAt((uint8)((i + 1) % kChainsPoolCount));
    uint16 lx = Chains_RuntimeSelfCheckFindDoor(
        vanilla, len, Chains_MainEntranceForRandoDungeon(source));
    if (lx == 0xFFFF)
      Chains_RuntimeSelfCheckDie("source door missing from vanilla table");
    if (kOverworld_Entrance_Id[lx] != Chains_MainEntranceForRandoDungeon(target))
      Chains_RuntimeSelfCheckDie("chain-start overlay target mismatch");
  }

  uint8 ep = kRandoDungeon_EasternPalace;
  uint16 ep_lx = Chains_RuntimeSelfCheckFindDoor(
      vanilla, len, Chains_MainEntranceForRandoDungeon(ep));
  if (ep_lx == 0xFFFF || !Chains_RuntimeRecordDoorEntry(ep_lx))
    Chains_RuntimeSelfCheckDie("origin entry arm failed");
  if (!g_chains_origin_active ||
      g_chains_origin_exit_room != Chains_MainExitRoomForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("origin room was not armed");
  if (Chains_RuntimeConsumeMainExitOrigin(0x0123) != 0 ||
      !g_chains_origin_active)
    Chains_RuntimeSelfCheckDie("non-main exit consumed chain origin");
  if (Chains_RuntimeConsumeMainExitOrigin(Chains_MainExitRoomForRandoDungeon(ep)) !=
      Chains_MainExitRoomForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("main exit did not consume origin");
  if (g_chains_origin_active)
    Chains_RuntimeSelfCheckDie("origin remained armed after main exit");

  const ChainSeamRow *ep_boss_seam = Chains_FindBossSeam(
      kChainSeamKind_Door, kDoorTblDir_North, 0x0D8, 0x0C8, 2);
  if (ep_boss_seam == NULL || ep_boss_seam->rando_dungeon != ep)
    Chains_RuntimeSelfCheckDie("EP boss seam fixture missing");
  const ChainSeamRow *dp_boss_seam = Chains_FindBossSeam(
      kChainSeamKind_Door, kDoorTblDir_North, 0x043, 0x033, 0);
  if (dp_boss_seam == NULL ||
      dp_boss_seam->rando_dungeon != kRandoDungeon_DesertPalace)
    Chains_RuntimeSelfCheckDie("DP boss seam fixture missing");
  const ChainSeamRow *tr_boss_seam = Chains_FindBossSeam(
      kChainSeamKind_Door, kDoorTblDir_North, 0x0B4, 0x0A4, 0);
  if (tr_boss_seam == NULL ||
      tr_boss_seam->rando_dungeon != kRandoDungeon_TurtleRock)
    Chains_RuntimeSelfCheckDie("TR boss seam fixture missing");

  uint8 saved_which_entrance = which_entrance;
  uint8 saved_player_is_indoors = player_is_indoors;
  uint8 saved_link_this_controls_sprite_oam = link_this_controls_sprite_oam;
  uint8 saved_player_near_pit_state = player_near_pit_state;
  uint8 saved_link_visibility_status = link_visibility_status;
  uint16 saved_death_var5_word = WORD(death_var5);
  uint8 saved_main_module_index_for_hop = main_module_index;
  uint8 saved_submodule_index_for_hop = submodule_index;
  uint8 saved_subsubmodule_index_for_hop = subsubmodule_index;
  uint8 saved_room_transitioning_flags_for_hop = room_transitioning_flags;
  uint8 saved_is_standing_in_doorway_for_hop = is_standing_in_doorway;
  uint16 saved_link_x_coord_for_hop = link_x_coord;
  uint16 saved_link_y_coord_for_hop = link_y_coord;
  uint8 saved_link_direction_facing_for_hop = link_direction_facing;
  uint16 saved_death_var4_word = WORD(death_var4);
  uint16 saved_dung_cur_door_pos = dung_cur_door_pos;
  uint16 saved_door_animation_step_indicator = door_animation_step_indicator;

  int ep_pool_idx = Chains_PoolIndexForDungeon(ep);
  if (ep_pool_idx < 0)
    Chains_RuntimeSelfCheckDie("EP pool index missing");
  layout.chain_successor[ep_pool_idx] = Chains_DungeonElement(kRandoDungeon_DesertPalace);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("lobby successor layout install failed");
  Chains_RuntimeClearOrigin();
  if (Chains_TryBossSeamHop(ep_boss_seam->kind, ep_boss_seam->direction,
                            ep_boss_seam->source_room, ep_boss_seam->dest_room,
                            ep_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("unarmed boss seam should stay vanilla");
  if (Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("unarmed boss seam left pending hop");
  Chains_RuntimeArmOrigin(Chains_MainExitRoomForRandoDungeon(ep));
  if (!Chains_TryBossSeamHop(ep_boss_seam->kind, ep_boss_seam->direction,
                             ep_boss_seam->source_room, ep_boss_seam->dest_room,
                             ep_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("lobby successor boss seam did not hop");
  if (which_entrance != Chains_MainEntranceForRandoDungeon(kRandoDungeon_DesertPalace))
    Chains_RuntimeSelfCheckDie("lobby successor hop picked wrong entrance");
  if (g_chains_terminal_active || g_chains_terminal_dungeon != kRandoDungeon_None)
    Chains_RuntimeSelfCheckDie("lobby successor hop armed terminal state");
  if (!Chains_ConsumeHopPending() || Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("lobby successor hop pending flag mismatch");

  int dp_pool_idx = Chains_PoolIndexForDungeon(kRandoDungeon_DesertPalace);
  if (dp_pool_idx < 0)
    Chains_RuntimeSelfCheckDie("DP pool index missing");
  layout.chain_successor[dp_pool_idx] = Chains_DungeonElement(kRandoDungeon_TurtleRock);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("DP aux-origin layout install failed");
  Chains_RuntimeClearOrigin();
  which_entrance = kChainsDesertBackEntranceId;
  if (!Chains_TryBossSeamHop(dp_boss_seam->kind, dp_boss_seam->direction,
                             dp_boss_seam->source_room, dp_boss_seam->dest_room,
                             dp_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("DP aux boss seam did not hop");
  if (which_entrance != Chains_MainEntranceForRandoDungeon(kRandoDungeon_TurtleRock))
    Chains_RuntimeSelfCheckDie("DP aux boss seam picked wrong successor");
  if (!g_chains_origin_active ||
      g_chains_origin_exit_room != kChainsDesertBackExitRoom)
    Chains_RuntimeSelfCheckDie("DP aux boss seam did not synthesize back origin");
  if (!Chains_ConsumeHopPending() || Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("DP aux boss seam pending flag mismatch");
  if (Chains_RuntimeConsumeMainExitOrigin(
          Chains_MainExitRoomForRandoDungeon(kRandoDungeon_TurtleRock)) !=
      kChainsDesertBackExitRoom)
    Chains_RuntimeSelfCheckDie("DP aux origin did not consume through successor exit");
  layout.chain_successor[dp_pool_idx] = Chains_BossElement(kRandoDungeon_DesertPalace);

  int tr_pool_idx = Chains_PoolIndexForDungeon(kRandoDungeon_TurtleRock);
  if (tr_pool_idx < 0)
    Chains_RuntimeSelfCheckDie("TR pool index missing");
  layout.chain_successor[tr_pool_idx] = Chains_DungeonElement(ep);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("TR aux-origin layout install failed");
  Chains_RuntimeClearOrigin();
  which_entrance = kChainsTurtleEyeBridgeEntranceId;
  if (!Chains_TryBossSeamHop(tr_boss_seam->kind, tr_boss_seam->direction,
                             tr_boss_seam->source_room, tr_boss_seam->dest_room,
                             tr_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("TR aux boss seam did not hop");
  if (which_entrance != Chains_MainEntranceForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("TR aux boss seam picked wrong successor");
  if (!g_chains_origin_active ||
      g_chains_origin_exit_room != kChainsTurtleEyeBridgeExitRoom)
    Chains_RuntimeSelfCheckDie("TR aux boss seam did not synthesize ledge origin");
  if (!Chains_ConsumeHopPending() || Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("TR aux boss seam pending flag mismatch");
  if (Chains_RuntimeConsumeMainExitOrigin(Chains_MainExitRoomForRandoDungeon(ep)) !=
      kChainsTurtleEyeBridgeExitRoom)
    Chains_RuntimeSelfCheckDie("TR aux origin did not consume through successor exit");
  layout.chain_successor[tr_pool_idx] = Chains_BossElement(kRandoDungeon_TurtleRock);

  layout.chain_successor[ep_pool_idx] = Chains_BossElement(kRandoDungeon_DesertPalace);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("boss successor layout install failed");
  Chains_RuntimeArmOrigin(Chains_MainExitRoomForRandoDungeon(ep));
  if (!Chains_TryBossSeamHop(ep_boss_seam->kind, ep_boss_seam->direction,
                             ep_boss_seam->source_room, ep_boss_seam->dest_room,
                             ep_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("boss successor seam did not hop");
  if (which_entrance != Chains_BossEntranceForRandoDungeon(kRandoDungeon_DesertPalace))
    Chains_RuntimeSelfCheckDie("boss successor hop picked wrong synthetic entrance");
  if (!g_chains_terminal_active ||
      g_chains_terminal_dungeon != kRandoDungeon_DesertPalace)
    Chains_RuntimeSelfCheckDie("boss successor hop did not arm terminal state");
  if (!Chains_ConsumeHopPending() || Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("boss successor hop pending flag mismatch");
  dungeon_room_index = 0x090;
  link_x_coord = (0x090 & 0x00F) << 9 | 120;
  link_y_coord = ((0x090 & 0x1F0) << 5) + 472;
  link_direction_facing = 2;
  is_standing_in_doorway = 1;
  room_transitioning_flags = 0xFF;
  dung_cur_door_pos = 22;
  door_animation_step_indicator = 4;
  which_entrance = Chains_BossEntranceForRandoDungeon(kRandoDungeon_MiseryMire);
  if (!Chains_TryApplyBossEntranceLanding())
    Chains_RuntimeSelfCheckDie("synthetic boss landing was not applied");
  if (link_x_coord != 120 ||
      link_y_coord != (uint16)(((0x090 & 0x1F0) << 5) + 424) ||
      link_direction_facing != 0 ||
      is_standing_in_doorway != 0 ||
      room_transitioning_flags != 0 ||
      dung_cur_door_pos != 0 ||
      door_animation_step_indicator != 0)
    Chains_RuntimeSelfCheckDie("synthetic boss landing left door-stuck state");
  which_entrance = Chains_BossEntranceForRandoDungeon(kRandoDungeon_IcePalace);
  if (Chains_TryApplyBossEntranceLanding())
    Chains_RuntimeSelfCheckDie("non-door boss entrance landing should stay asset-driven");

  layout.chain_successor[ep_pool_idx] = Chains_BossElement(ep);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("identity successor layout install failed");
  Chains_RuntimeArmOrigin(Chains_MainExitRoomForRandoDungeon(ep));
  if (Chains_TryBossSeamHop(ep_boss_seam->kind, ep_boss_seam->direction,
                            ep_boss_seam->source_room, ep_boss_seam->dest_room,
                            ep_boss_seam->slot))
    Chains_RuntimeSelfCheckDie("identity successor seam should stay vanilla");
  if (!g_chains_terminal_active || g_chains_terminal_dungeon != ep)
    Chains_RuntimeSelfCheckDie("identity successor did not arm terminal state");
  if (Chains_ConsumeHopPending())
    Chains_RuntimeSelfCheckDie("identity successor left pending hop");

  which_entrance = saved_which_entrance;
  player_is_indoors = saved_player_is_indoors;
  link_this_controls_sprite_oam = saved_link_this_controls_sprite_oam;
  player_near_pit_state = saved_player_near_pit_state;
  link_visibility_status = saved_link_visibility_status;
  WORD(death_var5) = saved_death_var5_word;
  main_module_index = saved_main_module_index_for_hop;
  submodule_index = saved_submodule_index_for_hop;
  subsubmodule_index = saved_subsubmodule_index_for_hop;
  room_transitioning_flags = saved_room_transitioning_flags_for_hop;
  is_standing_in_doorway = saved_is_standing_in_doorway_for_hop;
  link_x_coord = saved_link_x_coord_for_hop;
  link_y_coord = saved_link_y_coord_for_hop;
  link_direction_facing = saved_link_direction_facing_for_hop;
  WORD(death_var4) = saved_death_var4_word;
  dung_cur_door_pos = saved_dung_cur_door_pos;
  door_animation_step_indicator = saved_door_animation_step_indicator;

  layout.chain_successor[ep_pool_idx] = Chains_BossElement(ep);
  layout.chain_door_first[0] = Chains_BossElement(kRandoDungeon_DesertPalace);
  if (!Chains_RuntimeInstallLayout(&layout))
    Chains_RuntimeSelfCheckDie("terminal layout install failed");
  ep_lx = Chains_RuntimeSelfCheckFindDoor(
      vanilla, len, Chains_MainEntranceForRandoDungeon(ep));
  if (ep_lx == 0xFFFF ||
      kOverworld_Entrance_Id[ep_lx] != Chains_BossEntranceForRandoDungeon(kRandoDungeon_DesertPalace))
    Chains_RuntimeSelfCheckDie("terminal boss overlay target mismatch");
  if (!Chains_RuntimeRecordDoorEntry(ep_lx) ||
      !g_chains_origin_active ||
      !g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("terminal entry did not arm origin and terminal state");

  ChainsRuntimeSession invalid_session;
  memset(&invalid_session, 0, sizeof(invalid_session));
  invalid_session.origin_active = false;
  invalid_session.terminal_active = true;
  if (Chains_RuntimeRestoreSession(&invalid_session))
    Chains_RuntimeSelfCheckDie("inactive session accepted terminal state");
  invalid_session.origin_active = true;
  invalid_session.terminal_active = false;
  invalid_session.origin_exit_room = 0x0123;
  if (Chains_RuntimeRestoreSession(&invalid_session))
    Chains_RuntimeSelfCheckDie("session accepted non-main origin room");
  invalid_session.origin_exit_room = Chains_MainExitRoomForRandoDungeon(ep);
  invalid_session.terminal_active = true;
  invalid_session.terminal_dungeon = kRandoDungeon_None;
  if (Chains_RuntimeRestoreSession(&invalid_session))
    Chains_RuntimeSelfCheckDie("session accepted invalid terminal dungeon");

  ChainsRuntimeSession aux_session;
  memset(&aux_session, 0, sizeof(aux_session));
  aux_session.origin_active = true;
  aux_session.origin_exit_room = kChainsDesertBackExitRoom;
  aux_session.terminal_dungeon = kRandoDungeon_None;
  if (!Chains_RuntimeRestoreSession(&aux_session))
    Chains_RuntimeSelfCheckDie("DP aux-origin session restore failed");
  if (Chains_RuntimeConsumeMainExitOrigin(Chains_MainExitRoomForRandoDungeon(ep)) !=
      kChainsDesertBackExitRoom)
    Chains_RuntimeSelfCheckDie("DP aux-origin session did not consume");
  aux_session.origin_exit_room = kChainsTurtleEyeBridgeExitRoom;
  if (!Chains_RuntimeRestoreSession(&aux_session))
    Chains_RuntimeSelfCheckDie("TR aux-origin session restore failed");
  if (Chains_RuntimeConsumeMainExitOrigin(Chains_MainExitRoomForRandoDungeon(ep)) !=
      kChainsTurtleEyeBridgeExitRoom)
    Chains_RuntimeSelfCheckDie("TR aux-origin session did not consume");

  ChainsRuntimeSession terminal_session;
  memset(&terminal_session, 0, sizeof(terminal_session));
  terminal_session.origin_active = true;
  terminal_session.terminal_active = true;
  terminal_session.origin_exit_room = Chains_MainExitRoomForRandoDungeon(ep);
  terminal_session.terminal_dungeon = kRandoDungeon_DesertPalace;
  if (!Chains_RuntimeRestoreSession(&terminal_session))
    Chains_RuntimeSelfCheckDie("terminal session restore failed");

  const ChainSeamRow *dp_outbound = Chains_FindOutboundSeam(
      kChainSeamKind_Door, kDoorTblDir_South, 0x033, 0x043, 0);
  if (dp_outbound == NULL || dp_outbound->rando_dungeon != kRandoDungeon_DesertPalace)
    Chains_RuntimeSelfCheckDie("DP outbound seam fixture missing");

  uint8 saved_slot_active = g_rando_slot_active;
  uint8 saved_checked_bitmap[kRandoCheckedBitmapBytes];
  memcpy(saved_checked_bitmap, g_rando_checked_bitmap, sizeof(saved_checked_bitmap));
  uint16 saved_dungeon_room_index = dungeon_room_index;
  uint8 saved_room_transitioning_flags = room_transitioning_flags;
  uint8 saved_is_standing_in_doorway = is_standing_in_doorway;
  uint8 saved_saved_module_for_menu = saved_module_for_menu;
  uint8 saved_main_module_index = main_module_index;
  uint8 saved_submodule_index = submodule_index;
  uint8 saved_subsubmodule_index = subsubmodule_index;
  uint8 saved_which_entrance_for_terminal = which_entrance;
  uint16 saved_dungeon_room_index2 = dungeon_room_index2;
  uint16 saved_link_x_coord_for_terminal = link_x_coord;
  uint16 saved_link_y_coord_for_terminal = link_y_coord;
  uint8 saved_link_direction_facing_for_terminal = link_direction_facing;
  uint8 saved_link_visibility_status_for_terminal = link_visibility_status;
  uint8 saved_link_this_controls_sprite_oam_for_terminal = link_this_controls_sprite_oam;
  uint8 saved_player_near_pit_state_for_terminal = player_near_pit_state;
  uint8 saved_fallhole_var1 = fallhole_var1;
  uint8 saved_fallhole_var2 = fallhole_var2;
  uint8 saved_link_speed_modifier = link_speed_modifier;
  uint8 saved_link_speed_setting = link_speed_setting;
  uint8 saved_link_disable_sprite_damage = link_disable_sprite_damage;

  g_rando_slot_active = 1;
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
  Souls_ResetRoomSuppressed();  // escape consume reads this room's count
  uint16 dp_prize_loc = Rando_BossPrizeLocationForGameDungeon(
      Rando_GameDungeonFromRandoDungeon(kRandoDungeon_DesertPalace));
  which_entrance = Chains_BossEntranceForRandoDungeon(kRandoDungeon_DesertPalace);
  dungeon_room_index = dungeon_room_index2 = 0x033;
  if (!Chains_TryApplyBossEntranceLanding())
    Chains_RuntimeSelfCheckDie("unchecked terminal reentry landing was not applied");
  if (Chains_TryClearedTerminalReentryExit())
    Chains_RuntimeSelfCheckDie("unchecked terminal reentry escape fired");
  if (!g_chains_origin_active || !g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("unchecked terminal reentry consumed session");
  Rando_MarkLocationChecked(dp_prize_loc);
  which_entrance = Chains_BossEntranceForRandoDungeon(kRandoDungeon_DesertPalace);
  dungeon_room_index = dungeon_room_index2 = 0x033;
  if (!Chains_TryApplyBossEntranceLanding())
    Chains_RuntimeSelfCheckDie("cleared terminal reentry landing was not applied");
  if (!Chains_TryClearedTerminalReentryExit())
    Chains_RuntimeSelfCheckDie("cleared terminal reentry escape did not fire");
  if (dungeon_room_index != Chains_MainExitRoomForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("cleared terminal reentry did not target origin exit room");
  if (g_chains_origin_active || g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("cleared terminal reentry did not consume session");
  if (Chains_TryClearedTerminalReentryExit())
    Chains_RuntimeSelfCheckDie("cleared terminal reentry escape repeated");
  if (!Chains_RuntimeRestoreSession(&terminal_session))
    Chains_RuntimeSelfCheckDie("suppressed-escape session restore failed");
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
  Souls_NoteRoomSuppressed();  // soul-suppressed terminal boss, reward unchecked
  which_entrance = Chains_BossEntranceForRandoDungeon(kRandoDungeon_DesertPalace);
  dungeon_room_index = dungeon_room_index2 = 0x033;
  if (!Chains_TryApplyBossEntranceLanding())
    Chains_RuntimeSelfCheckDie("suppressed terminal landing was not applied");
  if (!Chains_TryClearedTerminalReentryExit())
    Chains_RuntimeSelfCheckDie("suppressed terminal reentry escape did not fire");
  if (dungeon_room_index != Chains_MainExitRoomForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("suppressed terminal escape did not target origin exit room");
  if (g_chains_origin_active || g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("suppressed terminal escape did not consume session");
  Souls_ResetRoomSuppressed();
  if (!Chains_RuntimeRestoreSession(&terminal_session))
    Chains_RuntimeSelfCheckDie("terminal session restore after reentry failed");
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
  if (Chains_TryTerminalOutboundSeam(dp_outbound->kind, dp_outbound->direction,
                                     dp_outbound->source_room, dp_outbound->dest_room,
                                     dp_outbound->slot))
    Chains_RuntimeSelfCheckDie("pre-reward terminal outbound seam fired");
  if (!g_chains_origin_active || !g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("pre-reward terminal outbound consumed session");
  Rando_MarkLocationChecked(dp_prize_loc);
  link_visibility_status = 12;
  link_this_controls_sprite_oam = 6;
  player_near_pit_state = 3;
  fallhole_var1 = 1;
  fallhole_var2 = 31;
  link_speed_modifier = 16;
  link_speed_setting = 6;
  link_disable_sprite_damage = 2;
  if (!Chains_TryTerminalOutboundSeam(dp_outbound->kind, dp_outbound->direction,
                                      dp_outbound->source_room, dp_outbound->dest_room,
                                      dp_outbound->slot))
    Chains_RuntimeSelfCheckDie("checked terminal outbound seam did not fire");
  if (dungeon_room_index != Chains_MainExitRoomForRandoDungeon(ep))
    Chains_RuntimeSelfCheckDie("terminal outbound did not target origin exit room");
  if (g_chains_origin_active || g_chains_terminal_active)
    Chains_RuntimeSelfCheckDie("terminal outbound did not consume session");
  if (link_visibility_status != 0 || link_this_controls_sprite_oam != 0 ||
      player_near_pit_state != 0 || fallhole_var1 != 0 || fallhole_var2 != 0 ||
      link_speed_modifier != 0 || link_speed_setting != 0 ||
      link_disable_sprite_damage != 0) {
    Chains_RuntimeSelfCheckDie("terminal outbound left pit-fall state armed");
  }

  g_rando_slot_active = saved_slot_active;
  memcpy(g_rando_checked_bitmap, saved_checked_bitmap, sizeof(saved_checked_bitmap));
  dungeon_room_index = saved_dungeon_room_index;
  room_transitioning_flags = saved_room_transitioning_flags;
  is_standing_in_doorway = saved_is_standing_in_doorway;
  saved_module_for_menu = saved_saved_module_for_menu;
  main_module_index = saved_main_module_index;
  submodule_index = saved_submodule_index;
  subsubmodule_index = saved_subsubmodule_index;
  which_entrance = saved_which_entrance_for_terminal;
  dungeon_room_index2 = saved_dungeon_room_index2;
  link_x_coord = saved_link_x_coord_for_terminal;
  link_y_coord = saved_link_y_coord_for_terminal;
  link_direction_facing = saved_link_direction_facing_for_terminal;
  link_visibility_status = saved_link_visibility_status_for_terminal;
  link_this_controls_sprite_oam = saved_link_this_controls_sprite_oam_for_terminal;
  player_near_pit_state = saved_player_near_pit_state_for_terminal;
  fallhole_var1 = saved_fallhole_var1;
  fallhole_var2 = saved_fallhole_var2;
  link_speed_modifier = saved_link_speed_modifier;
  link_speed_setting = saved_link_speed_setting;
  link_disable_sprite_damage = saved_link_disable_sprite_damage;

  Chains_RuntimeTeardown();
  if ((const uint8 *)kOverworld_Entrance_Id != vanilla)
    Chains_RuntimeSelfCheckDie("asset 126 was not restored");
  if (synth_assets) {
    for (uint32 i = 0; i < (uint32)sizeof(kSynthAssetIds); i++) {
      uint8 id = kSynthAssetIds[i];
      g_asset_ptrs[id] = saved_ptrs[i];
      g_asset_sizes[id] = saved_sizes[i];
    }
  }
  fprintf(stderr, "[Chains_RuntimeSelfCheck] OK\n");
}
