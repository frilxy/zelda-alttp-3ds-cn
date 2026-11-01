// This file declares extensions to the base game
#ifndef ZELDA3_FEATURES_H_
#define ZELDA3_FEATURES_H_

#include "types.h"

// Special RAM locations that are unused but I use for compat things.
//
// Layout:
//   0x648       kRam_APUI00              (1 byte)
//   0x649       kRam_CrystalRotateCounter (1 byte)
//   0x64a       kRam_BugsFixed           (1 byte) — 0x64b unused (padding)
//   0x64c-0x64f kRam_Features0           (uint32)
//   0x650-0x655 msu_curr_sample/volume/track (6 bytes, see below)
//   0x656-0x658 hud_cur_item_x/l/r       (3 bytes)
//   0x659-0x65c kRam_Features1           (uint32) — randomizer feature flags
//   0x65d       kRam_RandoSlotActive     (1 byte) — 0 vanilla, 1 randomizer
//   0x65e       kRam_RandoStartingInventoryGranted (1 byte) — once-per-slot gate
//   0x65f       kRam_RandoTriforcePieceCount (1 byte) — Phase A TriforceHunt counter
//   0x660       kRam_PreTemperSword      (1 byte) — rando: stash of Link's sword
//                                         tier during smithy tempering, so the
//                                         original is restored if the placed
//                                         Blacksmith reward isn't a sword
//   0x661       kRam_RandoSwordless      (1 byte) — rando: 1 when the active
//                                         slot is swordless. Persisted in g_ram
//                                         so a snapshot replay-restore (which
//                                         restores g_ram but not the C-static
//                                         RandoSettings) keeps the swordless
//                                         runtime patches firing.
//   0x662-0x665 kRam_EnemyShuffleVanPos2 (4 bytes) — rando: the vanilla-resolved
//                                         sprite GFX subgroup sheet for slots
//                                         0,1,2,3 of the current room/area,
//                                         tracked by the enemy-shuffle SHEET
//                                         reshuffle so an inherited (kSpriteTilesets
//                                         slot == 0) room is restored to vanilla
//                                         instead of a leaked reshuffle.
//                                         Snapshot-safe. (The enum name keeps its
//                                         original ...VanPos2 spelling; it is the
//                                         base of the 4-byte per-slot shadow array.)
//   0x666-0x669 kRam_EnemyShuffleLiveSubset (4 bytes) — rando: the resolved
//                                         sprite GFX subgroup sheet set most
//                                         recently committed by the enemy-shuffle
//                                         room/area sheet resolver.
//   0x66a-0x66b kRam_EnemyShuffleLiveKey (uint16) — room/area key for the
//                                         resolved sheet set above.
//   0x66c       kRam_EnemyShuffleLiveContext (1 byte) — 1 dungeon, 2 overworld.
//   0x66d       kRam_RandoOverworldDoor  (1 byte) — rando: the overworld door
//                                         the player last entered, as ALTTPR's
//                                         PreviousOverworldDoor (door row index
//                                         + 1); 0 = none. Shopsanity's shop
//                                         identity key — `which_entrance` cannot
//                                         serve, since four shops share entrance
//                                         id 0x60 and two share 0x58. Persisted
//                                         in g_ram (not a C static) so a
//                                         snapshot replay-restore keeps the
//                                         shop the player is standing in
//                                         resolvable, matching ALTTPR's own
//                                         RAM-resident PreviousOverworldDoor.
//   0x66e       kRam_RandoTakeAnyDoor    (1 byte) — source overworld door for
//                                         the current redirected Take-Any host
//                                         room. Snapshot-owned because the host
//                                         entrance itself is shared by many caves.
//   0x66f       kRam_RandoGoalPending     (1 byte) — snapshot-owned goal handoff.
//   0x670+      spotlight_* (DO NOT USE — see the `spotlight_*` declarations in variables.h)
//
// Verified clean in audit.md §0.7 (Phase 0 deliverable).
enum {
  kRam_APUI00 = 0x648,
  kRam_CrystalRotateCounter = 0x649,
  kRam_BugsFixed = 0x64a,
  kRam_Features0 = 0x64c,
  kRam_Features1 = 0x659,
  kRam_RandoSlotActive = 0x65d,
  kRam_RandoStartingInventoryGranted = 0x65e,
  kRam_RandoTriforcePieceCount = 0x65f,
  kRam_PreTemperSword = 0x660,
  kRam_RandoSwordless = 0x661,
  kRam_EnemyShuffleVanPos2 = 0x662,
  kRam_EnemyShuffleLiveSubset = 0x666,
  kRam_EnemyShuffleLiveKey = 0x66a,
  kRam_EnemyShuffleLiveContext = 0x66c,
  kRam_RandoOverworldDoor = 0x66d,
  kRam_RandoTakeAnyDoor = 0x66e,
  kRam_RandoGoalPending = 0x66f,

  // Inclusive ownership bounds for randomizer-only WRAM. Keep these explicit:
  // the headless initialization + vanilla-snapshot guard and its independent parser use
  // them as the contract for every byte that must remain untouched in vanilla
  // snapshots, including padding inside multi-byte cells.
  kRam_RandoOwnedBegin = 0x659,
  kRam_RandoOwnedEnd = 0x66f,
};

enum {
  // Poly rendered uses correct speed
  kBugFix_PolyRenderer = 1,
  kBugFix_AncillaOverwrites = 1,
  kBugFix_Latest = 1,
};

// Enum values for kRam_Features0
enum {
  kFeatures0_ExtendScreen64 = 1,
  kFeatures0_SwitchLR = 2,
  kFeatures0_TurnWhileDashing = 4,
  kFeatures0_MirrorToDarkworld = 8,
  kFeatures0_CollectItemsWithSword = 16,
  kFeatures0_BreakPotsWithSword = 32,
  kFeatures0_DisableLowHealthBeep = 64,
  kFeatures0_SkipIntroOnKeypress = 128,
  kFeatures0_ShowMaxItemsInYellow = 256,
  kFeatures0_MoreActiveBombs = 512,

  // This is set for visual fixes that don't affect game behavior but will affect ram compare.
  kFeatures0_WidescreenVisualFixes = 1024,

  kFeatures0_CarryMoreRupees = 2048,

  kFeatures0_MiscBugFixes = 4096,

  kFeatures0_CancelBirdTravel = 8192,

  kFeatures0_GameChangingBugFixes = 16384,

  kFeatures0_SwitchLRLimit = 32768,

  kFeatures0_DimFlashes = 65536,

  // Make the Digging Game and Chest Game winnable for a half-competent player
  // (digging: guaranteed win within ~8 digs; chest game: win on the first
  // chest). Behavior-affecting (diverges from the original-ROM RAM compare), so
  // it is RANDO-ONLY by default: forced on under the randomizer, OFF in vanilla
  // and side-by-side. Vanilla players opt in with `EasyMinigames = 1`. Applied
  // at point-of-use (gated on kFeatures1_RandomizerActive) in
  // DiggingGameGuy_AttemptPrizeSpawn (player.c) + OpenMiniGameChest (dungeon.c).
  kFeatures0_EasyMinigames = 131072,

  // Re-introduce glitches present in the Japanese 1.0 ALTTP ROM but patched out
  // of the US 1.0 ROM this project reimplements (Fake Flippers et al.).
  // BEHAVIOR-AFFECTING: every restored glitch diverges from the original
  // US-1.0 RAM every frame, so this is DEFAULT-OFF and ALSO suppressed under
  // side-by-side comparison. Each glitch's point-of-use gate is
  //   (enhanced_features0 & kFeatures0_RestoreJpGlitches) && !ZeldaIsEmulatorAttached()
  // so attaching the original ROM keeps the per-frame RAM/SRAM/VRAM comparator
  // clean even if a user manually enabled the flag. The flag means "enable
  // every JP-1.0 glitch this build has verified-and-implemented"; adding a
  // verified glitch later needs no new flag/save-format/migration.
  kFeatures0_RestoreJpGlitches = 262144,  // bit 18

  // Select the destination overworld music/ambient the way the Japanese 1.0
  // ROM does (instead of US 1.0) in the two routines that run after a mirror
  // warp / long screen transition (MirrorWarp_FinalizeAndLoadDestination and
  // Overworld_FinalizeEntryOntoScreen). JP keys the post-warp track off world
  // state + screen index + progress rather than re-reading the overworld_music
  // scratch table, so the chosen song/ambient diverges from the US-1.0
  // reference RAM every frame. DEFAULT-OFF and suppressed under side-by-side
  // (point-of-use gate is
  //   (enhanced_features0 & kFeatures0_JpOverworldMusic) && !ZeldaIsEmulatorAttached()
  // ) so attaching the original US ROM keeps the RAM comparator clean. Only the
  // $12C/$12D (music_control/sound_effect_ambient) writes change; the
  // destination, Link position, screen index and camera math are untouched.
  kFeatures0_JpOverworldMusic = 524288,  // bit 19

  // Seed QoL (add-rando-seed-qol). Bits 20-25 are placement-neutral runtime
  // preferences or local toggles. Per-slot bits are included in
  // kFeatures0_RandoSeedQolMask below; local/race bits are deliberately not.
  kFeatures0_RandoDungeonCheckCounts = 1048576,  // bit 20
  kFeatures0_RandoSeedInfoPanel_Reserved = 2097152,  // bit 21, deferred/unused
  kFeatures0_FastFanfare = 4194304,              // bit 22
  kFeatures0_CutsceneFastForward = 8388608,      // bit 23
  kFeatures0_AutoDash = 16777216,                // bit 24
  kFeatures0_WarpToSpawn = 33554432,             // bit 25, local/race toggle
};

// Per-randomizer-slot Seed QoL features. These are gameplay/accessibility
// preferences exposed by the randomizer Seed QoL panels (rando_window.cpp
// kRecBits) and safe to replay when loading that slot. Deliberately excludes
// geometry/restart-gated/local config bits such as ExtendScreen64,
// WidescreenVisualFixes, MoreActiveBombs, MirrorToDarkworld, CancelBirdTravel,
// EasyMinigames, and JpOverworldMusic.
#define kFeatures0_RandoSeedQolMask ( \
    kFeatures0_SkipIntroOnKeypress | \
    kFeatures0_ShowMaxItemsInYellow | \
    kFeatures0_TurnWhileDashing | \
    kFeatures0_CollectItemsWithSword | \
    kFeatures0_BreakPotsWithSword | \
    kFeatures0_DisableLowHealthBeep | \
    kFeatures0_CarryMoreRupees | \
    kFeatures0_MiscBugFixes | \
    kFeatures0_GameChangingBugFixes | \
    kFeatures0_RestoreJpGlitches | \
    kFeatures0_DimFlashes | \
    kFeatures0_RandoDungeonCheckCounts | \
    kFeatures0_FastFanfare | \
    kFeatures0_CutsceneFastForward | \
    kFeatures0_AutoDash)

// Enum values for kRam_Features1 (randomizer feature flags).
// Bits land here as Phase A features are implemented. The enum exists from
// A0 so the CI guards have something to scan; the values are populated
// incrementally.
enum {
  kFeatures1_RandomizerActive = 1,
  // Future bits reserved (per add-randomizer-support proposal):
  //   2  kFeatures1_ReservedTracker
  //   4  kFeatures1_ReservedSpoilerSuppress
  // Door shuffle (add-rando-door-shuffle): gates the door-redirect hooks in
  // dungeon.c. Identity (flag set, all links NO_OVERRIDE) is vanilla.
  kFeatures1_DoorShuffleActive = 8,
  // Client QoL preferences. Existing snapshot feature words record these bits;
  // seed identity, placement rules and SRAM layout are not changed.
  kFeatures1_QolKakariko = 1u << 4,
  kFeatures1_QolFairyHearts = 1u << 5,
  kFeatures1_QolBrightRooms = 1u << 6,
  kFeatures1_QolRepeatBlocks = 1u << 7,
  kFeatures1_QolBombs99 = 1u << 8,
  kFeatures1_QolArrows99 = 1u << 9,
  kFeatures1_QolInventory = 1u << 10,
  kFeatures1_QolShovel = 1u << 11,
  kFeatures1_QolManaged = 1u << 12,
};

#define enhanced_features0 (*(uint32*)(g_ram+0x64c))
#define enhanced_features1 (*(uint32*)(g_ram+0x659))
#define g_rando_slot_active (*(uint8*)(g_ram+0x65d))
#define g_rando_starting_inventory_granted (*(uint8*)(g_ram+0x65e))
#define g_rando_triforce_piece_count (*(uint8*)(g_ram+0x65f))
#define g_rando_swordless (*(uint8*)(g_ram+0x661))
#define g_rando_takeany_door_id (*(uint8*)(g_ram+kRam_RandoTakeAnyDoor))
#define msu_curr_sample (*(uint32*)(g_ram+0x650))
#define msu_volume (*(uint8*)(g_ram+0x654))
#define msu_track (*(uint8*)(g_ram+0x655))
#define hud_inventory_order ((uint8*)(g_ram + 0x225)) // 4x6 bytes
#define hud_cur_item_x (*(uint8*)(g_ram+0x656))
#define hud_cur_item_l (*(uint8*)(g_ram+0x657))
#define hud_cur_item_r (*(uint8*)(g_ram+0x658))



extern uint32 g_wanted_zelda_features;
extern uint32 g_wanted_zelda_features1;


#endif  // ZELDA3_FEATURES_H_
