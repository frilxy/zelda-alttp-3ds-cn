// door_runtime.h — door-shuffle runtime redirect layer (randomizer-door-runtime).
//
// A per-seed sparse override of dungeon door->door connections, consulted at
// the four Dungeon_StartInterRoomTrans_* supertile-boundary branches and the
// spiral-staircase destination read in Dungeon_DetectStaircase. Identity
// (every link kDoorRt_NoOverride, or the kFeatures1_DoorShuffleActive flag
// clear) reproduces vanilla behavior; the resolver still runs under the flag
// so the flag-ON all-NO_OVERRIDE side-by-side RAM-compare exercises the hook
// machinery (the override arm itself diverges from the ROM by definition and
// is playtest-verified).
#pragma once
#include "../types.h"
#include "door_tables.gen.h"
#include "shuffle_doors.h"  // DoorShuffleLayout (kind overlay install)

#define kDoorRt_NoOverride 0xFFFF

// Layout install/teardown (called from rando.c slot activate/deactivate and
// from generation). Reset clears every link and deactivates.
void DoorRt_Reset(void);
void DoorRt_SetLink(uint16 door_a, uint16 door_b);  // one directed half-link
void DoorRt_Activate(void);
bool DoorRt_Active(void);
// True iff a layout's link table is installed this session (independent of
// the kFeatures1_DoorShuffleActive RAM bit — used by the snapshot-restore
// reconcile, which clears a restored bit that has no installed layout).
bool DoorRt_Installed(void);
uint16 DoorRt_GetLink(uint16 door_id);  // kDoorRt_NoOverride when unshuffled
bool DoorRt_IsDiscovered(uint16 door_id);  // source half traversed this session

// dungeon.c hook: normal edge doors. Returns true when an override fired —
// the destination room index, Link/camera/bounds arrival, quadrant and layer
// have been fully applied, and the caller MUST skip both the vanilla
// positional room-index line and the room_transitioning_flags layer/palace
// toggles (the dest door record is the layer authority; basic shuffle never
// crosses a palace boundary). Returns false to take the vanilla path
// (feature off, room not cataloged, exiting door unshuffled).
bool Rando_DoorTransOverride(uint8 dir /* kDoorTblDir_* */);

// Companion to Rando_DoorTransOverride: true (and self-clears) iff the
// immediately preceding override fired — the caller skips the vanilla
// room_transitioning_flags layer/palace toggles in that case.
bool Rando_DoorTransConsumedToggles(void);

// Spiral-context gate (design §2d): Dungeon_DetectStaircase calls
// Dungeon_StartInterRoomTrans_Up/Down for the slide animation; the normal
// edge-door hook must be a no-op there (the header-dest override is the sole
// spiral authority).
void Rando_DoorStaircaseContext(bool entering);

// dungeon.c hook (DungeonTransition_ScrollRoom): pans the deferred
// perpendicular fine alignment of a redirected arrival in during the
// transition scroll; scroll_done=true on the terminator frame flushes the
// remainder. No-op unless DoorRt_Arrive armed it.
void Rando_DoorScrollFinePan(bool scroll_done);

// dungeon.c hook (SubtileTransitionCalculateLanding): after vanilla rewrites
// the landing coordinate's low byte, re-page that coordinate to the final
// camera page for redirected edge-door arrivals. No-op unless DoorRt_Arrive
// armed it.
void Rando_DoorLandingPageFix(void);

// dungeon.c hook: spiral staircases. Returns the destination room byte
// (vanilla_byte when unshuffled/off). `attr` is the staircase tile attribute
// (0x5e/0x5f circular spirals only — straight/fat stair attrs 0x38/0x39/0x26
// must not redirect).
// When a redirect is chosen, a pending intra-room fixup is armed and applied
// by Rando_DoorSpiralFixup() during Dungeon_InitializeRoomFromSpecial.
uint8 Rando_DoorSpiralDest(uint16 room, uint8 slot, uint8 attr, uint8 vanilla_byte);

// Called from Dungeon_InitializeRoomFromSpecial after the vanilla
// grid-granular Dungeon_AdjustAfterSpiralStairs ran: applies the pending
// intra-room slot delta between the source and destination spiral doors
// (vanilla pairs share their intra-room position; shuffled ones need not).
void Rando_DoorSpiralFixup(void);

// Called right after Dungeon_DetectStaircase sets cur_staircase_plane (the
// SOURCE header's value, describing the VANILLA destination): returns the
// SHUFFLED destination's plane class for a pending spiral redirect (vanilla
// value otherwise), keeping the TM/TS plane gymnastics target-consistent.
uint8 Rando_DoorSpiralPlane(uint8 vanilla_plane);

// Called at the end of Module07_0E_13_SetRoomAndLayerAndCache (which sets the
// layer from the SOURCE room header's per-staircase plane — vanilla-correct,
// wrong under redirect): re-asserts the destination door's layer and consumes
// the pending spiral redirect. No-op when none is pending.
void Rando_DoorSpiralLayerFix(void);

// Drop an armed mid-sequence spiral redirect (snapshot-restore reconcile:
// the pending state is process state; a restore during the staircase
// sequence must not let it fire on a later unrelated room load).
void DoorRt_ClearSpiralPending(void);

// ---------------------------------------------------------------------------
// Stage-1b door-KIND overlay: makes the layout's RELOCATED small-key doors
// physically real (render locked, consume a key, persist opened state) and
// un-keys the vanilla key doors the layout abandoned. Mirrors the reference's
// reassign_key_doors / change_door_to_small_key / verify_door_list_pos
// (ALttPDoorRandomizer DoorShuffle.py, MIT). Installed from
// Rando_ActivateSidecarSlot's door_active block; cleared by DoorRt_Reset.
// ---------------------------------------------------------------------------

// Build the per-room kind overlay from the layout. Returns false (with a
// stderr diagnostic) on any structural violation — chosen key half without a
// list entry, no swap target below index 4, slot!=index drift — in which case
// the overlay is left EMPTY (identity) and the caller must refuse the slot:
// a chosen key door the overlay can't render makes the certified-beatable
// placement unbeatable.
bool DoorRt_InstallKindOverlay(const DoorShuffleLayout *l);

// Selftest: install the overlay for `l` and verify the kind-overlay
// invariants (chosen halves keyed at slot<4, pos_byte multiset preserved,
// abandoned vanilla key doors un-keyed, blocked/pinned doors untouched).
// Returns 0 on success, the number of violations otherwise (stderr details).
int DoorRt_KindOverlaySelfCheck(const DoorShuffleLayout *l);

// Selftest: every shuffle-pool positional door must decode to the cataloged
// outer slot used by the runtime exit resolver and arrival alignment, and no
// normal door pair may share one runtime candidate.
int DoorRt_GeometrySelfCheck(void);

// dungeon.c seam: the single door-list-word override. All three door-list
// consumers (RoomDraw_DrawAllObjects's post-0xfff0 door loop,
// Dungeon_LoadHeader's raw-word copy into dung_door_tilemap_address[], and
// Dungeon_LoadAdjacentRoomDoors's neighbor scan) route their fetch through
// this. `index` is the 0-based position in the room's door list (==
// doorListPos == engine door slot for every overlaid room — enforced at
// install). Identity when the overlay is inactive, the room is not overlaid,
// or the engine's word differs from the catalog's expected vanilla word
// (data drift fail-safe: never substitute kinds into a list we don't model).
uint16 Rando_DoorListWord(uint16 room, int index, uint16 vanilla_word);

// dungeon.c seam (Dungeon_LoadHeader, right after
// dung_door_opened_incl_adjacent = dung_door_opened | 0xf00): open-state bits
// (kUpperBitmasks form, slots 0-3 in 0xf000) to force-set for this room.
// Un-chosen vanilla key STAIRS keep their StairKey kind but render/behave
// open (the C door list is 0xffff-terminated, so the reference's
// Room.delete/mirror trick would truncate it; pre-opening is equivalent:
// Door_Up_StairMaskLocked draws no lock and the f0 attr is never written).
uint16 Rando_DoorPreopenBits(uint16 room);

// dungeon.c seam (Dungeon_CheckAdjacentRoomsForOpenDoors, non-trapdoor arm):
// true = do NOT propagate the physical neighbor's open flag into this room's
// door slot. A re-stitched pool/key door's open state is owned by its own
// room's save_dung_info bits (plus the key-open partner mirror); the physical
// neighbor at the matching slot is not its logical partner, so a Normal
// neighbor door must not pre-open a relocated key door unpaid.
bool Rando_DoorAdjOpenSuppressed(uint16 room, int slot);

// dungeon.c seam (Dungeon_OpeningLockedDoor_Combined, the step12 arm that
// sets dung_door_opened): mirror the just-opened overlay key door's bit into
// its partner half's room (save_dung_info[partner_room], persisted), or into
// the live dung_door_opened when the partner sits in the SAME room. No-op for
// non-overlaid slots (vanilla doors, big-key doors).
void Rando_DoorKeyOpenMirror(uint16 room, int slot);

// dungeon.c seam (Dungeon_ProcessTorchesAndDoors small-key consume branch):
// true iff the current room's door `slot` is an overlaid small-key half whose
// open bit is already set (the partner half paid for the pair this visit; the
// f0 attr is stale because the room wasn't reloaded). The caller opens the
// door without consuming a key.
bool Rando_DoorKeySlotAlreadyOpen(int slot);
