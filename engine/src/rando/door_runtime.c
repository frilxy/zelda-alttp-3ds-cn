// door_runtime.c — door-shuffle runtime redirect layer. See door_runtime.h.
//
// Geometry facts (derived from dungeon.c's kDoorPositionToTilemapOffs_* with
// XY(x,y)=y*64+x over 8px tiles; cross-checked against the RoomDraw_Door_*
// consumers):
//   * A supertile is 512x512 px. Outer-edge doors sit at three slots per
//     edge, 128px apart: North/South door columns start at x = {112,240,368}
//     (3 tiles wide, centers {124,252,380}); West/East door rows start at
//     y = {120,248,376} (4 tiles tall, centers {136,264,392}).
//   * The door-list position byte's upper nibble identifies the edge slot:
//     North/West outer slots are nibbles 0..5 (slot = nibble % 3);
//     South/East outer slots are nibbles 6..11 (slot = (nibble - 6) % 3).
//     The second triplet covers alternate edge offsets used by ledge/layer
//     variants; shuffle-pool positional doors still transition through them.
#include "door_runtime.h"
#include "rando.h"
#include "door_keylogic.h"  // Door_PartnerOf (chosen-pair partner resolution)

#include "../variables.h"
#include "../features.h"
#include "../dungeon.h"  // Dungeon_AdjustQuadrant

#include <stdio.h>
#include <string.h>

extern const uint8 kLayoutQuadrantFlags[];  // dungeon.c quadrant-confinement table

// Confinement (quadrant_fullsize_*) for a destination room that is NOT
// loaded yet: the arrival camera clamp needs the legal window before the
// room's layout loads. Mirrors Dungeon_AdjustForRoomLayout minus the
// blastwall runtime flags (zero at load; a blastwall would only WIDEN the
// window, so the narrow pick stays legal). The layout|starting-quadrant
// byte follows the 1-byte floor header in the room data
// (RoomDraw_DrawFloors consumes exactly one byte before
// dung_layout_and_starting_quadrant is read). Returns 2 = full-size
// (camera may roam [b0,b1]) or 0 = confined to the quadrant window [a0,a1].
// Clamping into the WRONG window is not symmetric: too-narrow parks the
// camera at a quadrant rest with Link possibly off-screen at the edge
// (symptom: Link not visible at a rail ledge — a boundary-slot door in a
// full-size room); too-wide leaves the camera between confined stops, and
// the per-frame equality stops then never match (unbounded scroll).
static uint8 DoorRt_DestFullsize(uint16 room, uint8 quad_x, uint8 quad_y, bool y_axis) {
  if (room >= 256)
    return 0;
  const uint8 *p = GetDungeonRoomLayout(room);
  uint8 composite = p[1] | quad_y | quad_x;
  uint8 mask = y_axis ? (quad_y ? 8 : 4) : (quad_x ? 2 : 1);
  if (composite >= 32)
    return 0;  // defensive: malformed layout byte, take the confined window
  return (kLayoutQuadrantFlags[composite] & mask) == 0 ? 2 : 0;
}

static uint16 g_door_link[kDoorTbl_DoorCount];
static uint8 g_door_discovered[(kDoorTbl_DoorCount + 7) / 8];
static bool g_door_rt_active;
static bool g_door_rt_index_built;
static bool g_door_staircase_ctx;       // inside Dungeon_DetectStaircase's spiral path
static bool g_door_toggles_overridden;  // latch: last Trans override consumed the toggles
static uint16 g_door_spiral_pending;    // dest door id for the pending spiral fixup
static uint16 g_door_spiral_source;     // source door id of the pending spiral
// In-room px of the SOURCE staircase head (captured by Rando_DoorSpiralDest
// while the source attr table is loaded; only read while pending is armed).
static uint16 g_door_spiral_src_x;
static uint16 g_door_spiral_src_y;
// Destination stair plane (0/1) from the staircase object list's stored
// 0x1000 half bit — the engine's own truth, set by the fixup once the dest
// room is loaded. The RECORD's layer field is NOT a plane for spirals: it is
// the reference's 2-bit transition signature (HTH=0/HTL=1/LTH=2/LTL=3,
// bit1 = own side's plane) — writing it raw into link_is_on_lower_level
// produced the illegal layer 2 (GT playtest "wrong layer").
static uint8 g_door_spiral_dst_layer;
static bool g_door_spiral_dst_layer_valid;
// Deferred perpendicular fine alignment for redirected door arrivals: the
// slot delta for Link plus the clamped camera target, panned in across the
// transition scroll by Rando_DoorScrollFinePan (see the deferral comment in
// DoorRt_Arrive). axis_y = the perpendicular axis is Y (E/W travel).
static bool g_door_fine_active;
static bool g_door_fine_axis_y;
static int g_door_fine_link;            // remaining Link fine delta, px
static uint16 g_door_fine_cam_target;
// SubtileTransitionCalculateLanding rewrites only the landing coordinate's low
// byte. Redirected edge transitions can leave the high byte from the source
// camera page, which strands full-coordinate users (lifted terrain, collision)
// one 256px page away from the visible camera.
static bool g_door_landing_page_active;
static bool g_door_landing_page_axis_y;

// Per-room catalog index: doors sorted by room; g_room_first[room] = first
// slot in g_door_by_room, -1 = no doors. Built once (the catalog is const).
static int16 g_room_first[256];
static uint16 g_door_by_room[kDoorTbl_DoorCount];

// --- Stage-1b kind-overlay state (see the section at the end of the file) ---
enum {
  kDoorRtKindF_SmallKey = 1,  // overlaid chosen key half (kind 0x1C, or kept StairKey* for spirals)
  kDoorRtKindF_Suppress = 2,  // adjacency open-propagation suppressed for this slot
  kDoorRtKindF_Preopen = 4,   // un-chosen vanilla stair-key: force open-state at room load
};
typedef struct DoorRtKindEntry {
  uint16 vanilla_word;  // expected engine door word (pos_byte | kind << 8)
  uint16 overlay_word;  // substituted word (== vanilla_word when untouched)
  uint8 flags;          // kDoorRtKindF_*
  uint8 partner_room;   // cross-entry small-key partner half (0xFF slot = none)
  uint8 partner_slot;
} DoorRtKindEntry;
typedef struct DoorRtKindRoom {
  uint8 room;
  uint8 count;  // vanilla door-list length (kDoorTblRooms; max 8)
  DoorRtKindEntry e[8];
} DoorRtKindRoom;
static DoorRtKindRoom g_kind_rooms[kDoorTbl_RoomCount];  // overlay rooms are a
static int g_kind_nrooms;                                // subset of the catalog rooms
static int16 g_kind_room_of[256];      // room byte -> g_kind_rooms index, -1 none
static bool g_kind_overlay_active;
static bool g_kind_lookup_built;
static int16 g_tblroom_of[256];        // room byte -> kDoorTblRooms row, -1 none
static uint8 g_kind_warned[256 / 8];   // once-per-room engine-vs-catalog drift warning

static void DoorRt_KindOverlayClear(void) {
  g_kind_nrooms = 0;
  g_kind_overlay_active = false;
  memset(g_kind_room_of, 0xFF, sizeof(g_kind_room_of));
  memset(g_kind_warned, 0, sizeof(g_kind_warned));
}

void DoorRt_Reset(void) {
  memset(g_door_link, 0xFF, sizeof(g_door_link));
  memset(g_door_discovered, 0, sizeof(g_door_discovered));
  g_door_rt_active = false;
  g_door_staircase_ctx = false;
  g_door_toggles_overridden = false;
  g_door_spiral_pending = 0xFFFF;
  g_door_spiral_source = 0xFFFF;
  // Mirror DoorRt_ClearSpiralPending: a stale captured dest layer must not
  // survive a teardown/reactivation, or the next spiral arrival's
  // Rando_DoorSpiralLayerFix consumes the previous session's plane.
  g_door_spiral_dst_layer = 0;
  g_door_spiral_dst_layer_valid = false;
  g_door_fine_active = false;
  g_door_landing_page_active = false;
  g_door_landing_page_axis_y = false;
  DoorRt_KindOverlayClear();
}

void DoorRt_SetLink(uint16 door_a, uint16 door_b) {
  if (door_a < kDoorTbl_DoorCount && door_b < kDoorTbl_DoorCount)
    g_door_link[door_a] = door_b;
}

uint16 DoorRt_GetLink(uint16 door_id) {
  return door_id < kDoorTbl_DoorCount ? g_door_link[door_id] : kDoorRt_NoOverride;
}

static void DoorRt_MarkDiscovered(uint16 door_id) {
  if (door_id < kDoorTbl_DoorCount)
    g_door_discovered[door_id >> 3] |= (uint8)(1u << (door_id & 7));
}

bool DoorRt_IsDiscovered(uint16 door_id) {
  return door_id < kDoorTbl_DoorCount &&
         (g_door_discovered[door_id >> 3] & (uint8)(1u << (door_id & 7))) != 0;
}

static void DoorRt_BuildRoomIndex(void) {
  // counting sort by room byte
  int counts[257] = { 0 };
  for (int i = 0; i < kDoorTbl_DoorCount; i++) {
    uint8 room = kDoorTblDoors[i].room;
    counts[room + 1]++;
  }
  for (int i = 0; i < 256; i++)
    counts[i + 1] += counts[i];
  for (int i = 0; i < 256; i++)
    g_room_first[i] = (int16)counts[i];
  int cursor[256];
  for (int i = 0; i < 256; i++)
    cursor[i] = counts[i];
  for (int i = 0; i < kDoorTbl_DoorCount; i++)
    g_door_by_room[cursor[kDoorTblDoors[i].room]++] = (uint16)i;
  g_door_rt_index_built = true;
}

void DoorRt_Activate(void) {
  if (!g_door_rt_index_built)
    DoorRt_BuildRoomIndex();
  g_door_rt_active = true;
}

bool DoorRt_Active(void) {
  return g_door_rt_active && (enhanced_features1 & kFeatures1_DoorShuffleActive) != 0;
}

bool DoorRt_Installed(void) {
  return g_door_rt_active;
}

// ---------------------------------------------------------------------------
// Exit-door resolution
// ---------------------------------------------------------------------------

// Outer-edge slot index from the door-list position byte, or -1 when the
// entry is on the wrong edge for `dir`.
static int DoorRt_OuterSlot(uint8 dir, uint8 pos_byte) {
  int nib = pos_byte >> 4;
  switch (dir) {
  case kDoorTblDir_North:
  case kDoorTblDir_West:
    return (nib <= 5) ? nib % 3 : -1;
  case kDoorTblDir_South:
  case kDoorTblDir_East:
    return (nib >= 6 && nib <= 11) ? (nib - 6) % 3 : -1;
  }
  return -1;
}

// Perpendicular pixel center of an outer-edge door slot, within the supertile.
static int DoorRt_SlotPerpCenter(uint8 dir, int slot) {
  if (dir == kDoorTblDir_North || dir == kDoorTblDir_South)
    return 124 + slot * 128;  // door columns 112/240/368, 3 tiles wide
  return 136 + slot * 128;    // door rows 120/248/376, 4 tiles tall
}

// Find the catalog door Link is exiting through: same room, direction `dir`,
// Normal type (positional transitions only — vanilla-teleport-flagged doors
// exit through the 0x89 branch), matching layer, nearest outer slot to Link's
// perpendicular coordinate.
static int DoorRt_ResolveExit(uint8 dir) {
  uint16 room = dungeon_room_index;
  if (room >= 256 || g_room_first[room] < 0)
    return -1;
  int first = g_room_first[room];
  int end = (room < 255) ? g_room_first[room + 1] : kDoorTbl_DoorCount;
  // Link's perpendicular coordinate within the supertile.
  int perp;
  if (dir == kDoorTblDir_North || dir == kDoorTblDir_South)
    perp = (link_x_coord + 8) & 0x1FF;  // sprite center
  else
    perp = (link_y_coord + 12) & 0x1FF;
  int best = -1, best_d = 0x7FFFFFFF;
  for (int i = first; i < end; i++) {
    const DoorTblDoor *d = &kDoorTblDoors[g_door_by_room[i]];
    if (d->type != kDoorTblType_Normal || d->direction != dir)
      continue;
    if (d->flags & kDoorTblFlag_VanillaTeleport)
      continue;
    // link_is_on_lower_level can transiently hold 2/3 during staircase
    // sequences (Module07_0E_00 priority values); bit 0 is the layer.
    if (d->layer != (link_is_on_lower_level & 1))
      continue;
    int slot = DoorRt_OuterSlot(dir, d->pos_byte);
    if (slot < 0)
      continue;
    int dd = perp - DoorRt_SlotPerpCenter(dir, slot);
    if (dd < 0)
      dd = -dd;
    if (dd < best_d) {
      best_d = dd;
      best = g_door_by_room[i];
    }
  }
  // A door slot is 128px from its neighbor; if Link is further than ~80px
  // from the nearest cataloged slot he is not exiting through a cataloged
  // door (open edge, blown-up wall, ...) — take the vanilla path.
  return (best >= 0 && best_d <= 80) ? best : -1;
}

// ---------------------------------------------------------------------------
// Arrival — generalized Dungeon_AdjustForTeleportDoors (both axes + slot
// correction + layer + quadrant). The mid-transition tableau set up by the
// Trans_* entry (camera targets, bounds Add/Sub, quadrant toggle) is
// TRANSLATED by whole supertiles onto the destination, then the perpendicular
// axis is fine-aligned to the dst door slot with its quadrant-coupled state
// re-derived.
// ---------------------------------------------------------------------------

static void DoorRt_Arrive(const DoorTblDoor *dst) {
  uint16 D = dst->room;
  int slot = DoorRt_OuterSlot(dst->direction, dst->pos_byte);
  if (slot < 0)
    slot = 1;  // defensive: arrival door should always be an outer slot

  // Pairing is strictly opposite-direction (kOppositeHook), so dst's wall
  // side determines the travel direction and therefore which vanilla
  // neighbor N0 = S + step the replaced room-index line would have entered.
  // dungeon_room_index2/_prev get the virtual neighbor just outside dst's
  // arrival edge (uint8, like Dungeon_AdjustForTeleportDoors — the next room
  // load resyncs index2 via BYTE(index2)=BYTE(index)).
  int step;
  uint16 virtual_room;
  switch (dst->direction) {
  case kDoorTblDir_West:   step = 1;     virtual_room = D - 1;    break;  // moving east
  case kDoorTblDir_East:   step = -1;    virtual_room = D + 1;    break;  // moving west
  case kDoorTblDir_North:  step = 0x10;  virtual_room = D - 0x10; break;  // moving south
  case kDoorTblDir_South:  step = -0x10; virtual_room = D + 0x10; break;  // moving north
  default:
    return;
  }

  uint16 S = dungeon_room_index;  // hook replaced the vanilla step, so still the source
  dungeon_room_index = D;
  dungeon_room_index2 = (uint8)virtual_room;
  dungeon_room_index_prev = (uint8)virtual_room;

  // Translate the whole mid-transition tableau (Link, camera, bounds) by a
  // whole number of supertiles per axis, mapping the vanilla target N0 onto
  // D — the Dungeon_AdjustForTeleportDoors / AdjustAfterSpiralStairs pattern.
  // A whole-512 delta preserves every within-512 phase relation the
  // transition machinery depends on (the masked up_down/left_right scroll
  // stop, the camera-follow thresholds, the landing snap), regardless of
  // which side of a 256px boundary the trigger fired on. Re-deriving hi
  // bytes instead breaks the masked scroll terminator and the camera scrolls
  // up to a full extra supertile, dragging Link out of the room (symptom:
  // runaway camera scrolling on arrival).
  int n0_col = (S & 0xf) + (step == 1 ? 1 : step == -1 ? -1 : 0);
  int n0_row = (S >> 4) + (step == 0x10 ? 1 : step == -0x10 ? -1 : 0);
  int dx = (((D & 0xf) - n0_col) << 9);
  int dy = ((((int)(D >> 4)) - n0_row) << 9);
  link_x_coord += dx;
  BG2HOFS_copy2 += dx;
  room_bounds_x.a0 += dx;
  room_bounds_x.a1 += dx;
  room_bounds_x.b0 += dx;
  room_bounds_x.b1 += dx;
  link_y_coord += dy;
  BG2VOFS_copy2 += dy;
  room_bounds_y.a0 += dy;
  room_bounds_y.a1 += dy;
  room_bounds_y.b0 += dy;
  room_bounds_y.b1 += dy;

  // Perpendicular axis: align Link to dst's door slot. Unlike the scroll
  // axis this is a fine (non-512) shift, so the quadrant-coupled state must
  // be re-derived by hand: the confined camera stops (a0/a1 move 0x100 with
  // a quadrant toggle — vanilla couples them in AdjustQuadrantAndCamera_*),
  // the camera itself (follow Link, then CLAMP into the confined window
  // [a0,a1] — the per-frame camera stops are equality tests, so a camera
  // outside its stop range scrolls unbounded), and the follow thresholds
  // (within-512 Link-phase trigger lines that track the camera 1:1; their
  // vanilla seeds are camera_rest_phase + 127 for X / + 120 for Y).
  bool horizontal = (dst->direction == kDoorTblDir_West || dst->direction == kDoorTblDir_East);
  g_door_landing_page_active = true;
  g_door_landing_page_axis_y = !horizontal;
  if (horizontal) {
    int target_in_room = 128 + slot * 128;  // door rows 120..152; Link y anchor
    uint8 new_q = (target_in_room >= 256) ? 2 : 0;
    if (new_q != link_quadrant_y) {
      int qa = new_q ? 0x100 : -0x100;
      room_bounds_y.a0 += qa;
      room_bounds_y.a1 += qa;
      link_quadrant_y = new_q;
    }
    int fine = target_in_room - (int)(link_y_coord & 0x1ff);
    int cam = (int)BG2VOFS_copy2 + fine;
    uint8 fs = DoorRt_DestFullsize(D, link_quadrant_x, link_quadrant_y, true);
    int lo = fs ? room_bounds_y.b0 : room_bounds_y.a0;
    int hi = fs ? room_bounds_y.b1 : room_bounds_y.a1;
    if (cam < lo) cam = lo;
    if (cam > hi) cam = hi;
    // DEFER the fine alignment (Link delta + camera target): applying it at
    // trigger time shifts the visible tilemap window by a non-512 amount,
    // and for the ~8 pre-scroll upload frames the screen shows whatever
    // stale room last occupied those VRAM rows (the playtest "flashes a
    // different room"). The whole-512 translation above is tilemap-modulo
    // invisible, so holding the fine delta keeps the pre-scroll frames
    // pixel-identical to the source view; Rando_DoorScrollFinePan pans
    // Link + camera to the stored targets during the scroll.
    g_door_fine_active = true;
    g_door_fine_axis_y = true;
    g_door_fine_link = fine;
    g_door_fine_cam_target = (uint16)cam;
    camera_y_coord_scroll_low = (cam & 0x1ff) + 120;
    camera_y_coord_scroll_hi = camera_y_coord_scroll_low + 2;
  } else {
    int target_in_room = 116 + slot * 128;  // door columns 112..136; Link x anchor
    uint8 new_q = (target_in_room >= 256) ? 1 : 0;
    if (new_q != link_quadrant_x) {
      int qa = new_q ? 0x100 : -0x100;
      room_bounds_x.a0 += qa;
      room_bounds_x.a1 += qa;
      link_quadrant_x = new_q;
    }
    int fine = target_in_room - (int)(link_x_coord & 0x1ff);
    int cam = (int)BG2HOFS_copy2 + fine;
    uint8 fs = DoorRt_DestFullsize(D, link_quadrant_x, link_quadrant_y, false);
    int lo = fs ? room_bounds_x.b0 : room_bounds_x.a0;
    int hi = fs ? room_bounds_x.b1 : room_bounds_x.a1;
    if (cam < lo) cam = lo;
    if (cam > hi) cam = hi;
    // Deferred fine alignment — see the Y-arm comment above.
    g_door_fine_active = true;
    g_door_fine_axis_y = false;
    g_door_fine_link = fine;
    g_door_fine_cam_target = (uint16)cam;
    camera_x_coord_scroll_low = (cam & 0x1ff) + 127;
    camera_x_coord_scroll_hi = camera_x_coord_scroll_low + 2;
  }

  // Layer authority: the destination door record (the vanilla layer/palace
  // toggles encode the POSITIONAL partner's relationship and are skipped by
  // the caller when an override fires). Basic shuffle never crosses a palace
  // boundary, so cur_palace_index_x2 stays untouched.
  link_is_on_lower_level = dst->layer;
  link_is_on_lower_level_mirror = dst->layer;

  Dungeon_AdjustQuadrant();

  for (int i = 0; i < 20; i++)
    tagalong_y_hi[i] = link_y_coord >> 8;
}

// ---------------------------------------------------------------------------
// dungeon.c hooks
// ---------------------------------------------------------------------------

bool Rando_DoorTransOverride(uint8 dir) {
  g_door_toggles_overridden = false;
  // Defensive: a fine-pan armed by a previous override must never leak into
  // this transition's scroll (inter-room transitions always reach ScrollRoom,
  // which consumes it, but clear unconditionally at every transition start so
  // no abort path can carry one across).
  g_door_fine_active = false;
  g_door_landing_page_active = false;
  g_door_landing_page_axis_y = false;
  if (!DoorRt_Active() || g_door_staircase_ctx)
    return false;
  int exit_id = DoorRt_ResolveExit(dir);
  if (exit_id < 0)
    return false;
  uint16 dest_id = g_door_link[exit_id];
  if (dest_id == kDoorRt_NoOverride || dest_id >= kDoorTbl_DoorCount)
    return false;
  DoorRt_MarkDiscovered((uint16)exit_id);
  DoorRt_Arrive(&kDoorTblDoors[dest_id]);
  g_door_toggles_overridden = true;
  return true;
}

bool Rando_DoorTransConsumedToggles(void) {
  bool v = g_door_toggles_overridden;
  g_door_toggles_overridden = false;
  return v;
}

void Rando_DoorStaircaseContext(bool entering) {
  g_door_staircase_ctx = entering;
}

// Called from DungeonTransition_ScrollRoom each scroll frame (scroll_done on
// the terminator frame). Pans the deferred perpendicular fine alignment in
// at the scroll speed (8px/frame), flushing any remainder when the scroll
// ends so the landing/walk-out states see the final position. BG1 follows by
// delta (not assignment) to preserve scrolling-floor offsets. No-op unless a
// redirected arrival armed it.
void Rando_DoorScrollFinePan(bool scroll_done) {
  if (!g_door_fine_active)
    return;
  int ls = g_door_fine_link;
  if (!scroll_done) {
    if (ls > 8) ls = 8;
    if (ls < -8) ls = -8;
  }
  g_door_fine_link -= ls;
  if (g_door_fine_axis_y) {
    link_y_coord += ls;
    int cs = (int)g_door_fine_cam_target - (int)BG2VOFS_copy2;
    if (!scroll_done) {
      if (cs > 8) cs = 8;
      if (cs < -8) cs = -8;
    }
    BG2VOFS_copy2 += cs;
    BG1VOFS_copy2 += cs;
  } else {
    link_x_coord += ls;
    int cs = (int)g_door_fine_cam_target - (int)BG2HOFS_copy2;
    if (!scroll_done) {
      if (cs > 8) cs = 8;
      if (cs < -8) cs = -8;
    }
    BG2HOFS_copy2 += cs;
    BG1HOFS_copy2 += cs;
  }
  if (scroll_done)
    g_door_fine_active = false;
}

static uint16 DoorRt_RepageCoordToCamera(uint16 coord, uint16 camera) {
  // camera is the top-left scroll coordinate; keep Link in the forward 256px
  // page that starts at camera, not on the nearest mathematical page.
  uint16 out = (camera & 0xFF00u) | (coord & 0x00FFu);
  if ((uint16)(out - camera) >= 0x100)
    out += 0x100;
  return out;
}

void Rando_DoorLandingPageFix(void) {
  if (!g_door_landing_page_active)
    return;
  if (g_door_landing_page_axis_y)
    link_y_coord = DoorRt_RepageCoordToCamera(link_y_coord, BG2VOFS_copy2);
  else
    link_x_coord = DoorRt_RepageCoordToCamera(link_x_coord, BG2HOFS_copy2);
  link_y_coord_safe_return_hi = link_y_coord >> 8;
  link_x_coord_safe_return_hi = link_x_coord >> 8;
  for (int i = 0; i < 20; i++) {
    if (g_door_landing_page_axis_y)
      tagalong_y_hi[i] = link_y_coord >> 8;
    else
      tagalong_x_hi[i] = link_x_coord >> 8;
  }
  g_door_landing_page_active = false;
}

// --- Spiral stair list / record correspondence ------------------------------
//
// The 0x5e/0x5f head attrs and the 0x30|down<<2|slot stair byte are STAMPED
// into the attr table by Dungeon_LoadObjectAttribute from
// dung_inter_starcases[] plus the cumulative category counters — but that
// stamping runs inside the CHUNKED attribute loader (overworld_map_state,
// kicked off only from spiral stage 7), while Dungeon_LoadRoom fills the
// object list synchronously. Scanning the attr table at the stage-3 arrival
// fixup therefore reads the SOURCE room's stale attrs, finds the source's own
// head, and computes a zero delta — Link strands at the source-relative
// offset in the destination room (the playtest "empty walled box" trap). The
// object list is valid the moment Dungeon_LoadRoom returns, on both sides.
//
// Stamp facts (Dungeon_LoadObjectAttribute): the attr2 slot bits equal the
// stair's LIST INDEX (t = 0x3030 + k*0x101; the southdown reset keeps the
// low bits), and the head tile of both spiral categories is the stored list
// value + XY(1,0). The stored 0x1000 bit is the layer half.
//
// The door RECORDS' door_index is NOT that slot: it is the reference's own
// 0x13B000-table ordering, which disagrees with the engine in multi-spiral
// rooms (vanilla header proof: GT Lobby stair0 -> 0x6b = the Up stair, but
// the reference gives the Up stair doorIndex 2). Records are therefore
// matched by (direction, quadrant) — the ss() quadrant equals the head's
// quadrant — with an intra-group x-rank <-> door_index-rank bijection for
// the side-by-side pairs (PoD Compass/Dark Basement, Swamp Elbow/Drain:
// West/Left carries the smaller door_index).

typedef struct DoorRtSpiralStair {
  uint16 head_pos;  // attr pos of the head tile (may carry the 0x1000 layer bit)
  uint8 cat_up;     // 1 = up spiral, 0 = down spiral, 0xFF = non-spiral slot
} DoorRtSpiralStair;

// Room staircase list in engine slot order (attr2 slot bits == index).
static int DoorRt_SpiralStairList(DoorRtSpiralStair out[8]) {
  const struct { uint16 end; uint8 spiral, up; } cats[10] = {
    { dung_num_inter_room_upnorth_stairs, 0, 1 },
    { dung_num_wall_upnorth_spiral_stairs, 1, 1 },
    { dung_num_wall_upnorth_spiral_stairs_2, 1, 1 },
    { dung_num_inter_room_upnorth_straight_stairs, 0, 1 },
    { dung_num_inter_room_upsouth_straight_stairs, 0, 1 },
    { dung_num_inter_room_southdown_stairs, 0, 0 },
    { dung_num_wall_downnorth_spiral_stairs, 1, 0 },
    { dung_num_wall_downnorth_spiral_stairs_2, 1, 0 },
    { dung_num_inter_room_downnorth_straight_stairs, 0, 0 },
    { dung_num_inter_room_downsouth_straight_stairs, 0, 0 },
  };
  int i = 0, n = 0;
  for (int c = 0; c < 10; c++) {
    for (; i != cats[c].end && i < 16 && n < 8; i += 2, n++) {
      out[n].cat_up = cats[c].spiral ? cats[c].up : 0xFF;
      out[n].head_pos = dung_inter_starcases[i >> 1] + 1;  // + XY(1,0): head tile
    }
  }
  return n;
}

static uint8 DoorRt_SpiralHeadQuadrant(uint16 pos) {
  int x = (pos & 0x3f) << 3, y = ((pos >> 6) & 0x3f) << 3;
  return (uint8)((y >= 256 ? 2 : 0) | (x >= 256 ? 1 : 0));
}

// (room, engine slot) -> spiral door record id, or 0xFFFF. head_out gets the
// stair's head attr pos.
static uint16 DoorRt_SpiralRecordForSlot(uint16 room, uint8 slot, uint16 *head_out) {
  DoorRtSpiralStair list[8];
  int n = DoorRt_SpiralStairList(list);
  if (slot >= n || list[slot].cat_up == 0xFF)
    return 0xFFFF;
  uint8 up = list[slot].cat_up;
  uint16 hp = list[slot].head_pos;
  uint8 hq = DoorRt_SpiralHeadQuadrant(hp);
  int hx = hp & 0x3f;
  int xrank = 0;
  for (int k = 0; k < n; k++) {
    if (k == slot || list[k].cat_up != up)
      continue;
    if (DoorRt_SpiralHeadQuadrant(list[k].head_pos) != hq)
      continue;
    int kx = list[k].head_pos & 0x3f;
    if (kx < hx || (kx == hx && k < slot))
      xrank++;
  }
  uint8 want_dir = up ? kDoorTblDir_Up : kDoorTblDir_Down;
  if (room >= 256 || g_room_first[room] < 0)
    return 0xFFFF;
  int first = g_room_first[room];
  int end = (room < 255) ? g_room_first[room + 1] : kDoorTbl_DoorCount;
  uint16 only_dir = 0xFFFF;
  int dir_matches = 0;
  uint16 best = 0xFFFF;
  for (int i2 = first; i2 < end; i2++) {
    uint16 id = g_door_by_room[i2];
    const DoorTblDoor *d = &kDoorTblDoors[id];
    if (d->type != kDoorTblType_SpiralStairs || d->direction != want_dir)
      continue;
    dir_matches++;
    only_dir = id;
    if (d->quadrant != hq)
      continue;
    int rank = 0;
    for (int i3 = first; i3 < end; i3++) {
      uint16 id3 = g_door_by_room[i3];
      const DoorTblDoor *d3 = &kDoorTblDoors[id3];
      if (id3 == id || d3->type != kDoorTblType_SpiralStairs ||
          d3->direction != want_dir || d3->quadrant != hq)
        continue;
      if (d3->door_index < d->door_index ||
          (d3->door_index == d->door_index && id3 < id))
        rank++;
    }
    if (rank == xrank)
      best = id;
  }
  // Quadrant-boundary tolerance: a room with exactly one spiral of this
  // direction is unambiguous even if the ss() quadrant disagrees by a tile.
  if (best == 0xFFFF && dir_matches == 1)
    best = only_dir;
  if (best != 0xFFFF && head_out)
    *head_out = hp;
  return best;
}

// Destination record -> its stair's head attr pos in the freshly loaded room,
// or -1 (caller keeps the vanilla offset).
static int DoorRt_SpiralHeadForRecord(const DoorTblDoor *dst) {
  DoorRtSpiralStair list[8];
  int n = DoorRt_SpiralStairList(list);
  uint8 up = (dst->direction == kDoorTblDir_Up) ? 1 : 0;
  uint16 room = dst->room;
  if (room >= 256 || g_room_first[room] < 0)
    return -1;
  int first = g_room_first[room];
  int end = (room < 255) ? g_room_first[room + 1] : kDoorTbl_DoorCount;
  uint16 dst_id = (uint16)(dst - kDoorTblDoors);
  int rrank = 0;
  for (int i2 = first; i2 < end; i2++) {
    uint16 id = g_door_by_room[i2];
    const DoorTblDoor *d = &kDoorTblDoors[id];
    if (id == dst_id || d->type != kDoorTblType_SpiralStairs ||
        d->direction != dst->direction || d->quadrant != dst->quadrant)
      continue;
    if (d->door_index < dst->door_index ||
        (d->door_index == dst->door_index && id < dst_id))
      rrank++;
  }
  int only_cat = -1, cat_count = 0;
  for (int k = 0; k < n; k++) {
    if (list[k].cat_up != up)
      continue;
    cat_count++;
    only_cat = k;
    if (DoorRt_SpiralHeadQuadrant(list[k].head_pos) != dst->quadrant)
      continue;
    int kx = list[k].head_pos & 0x3f;
    int xrank = 0;
    for (int k2 = 0; k2 < n; k2++) {
      if (k2 == k || list[k2].cat_up != up)
        continue;
      if (DoorRt_SpiralHeadQuadrant(list[k2].head_pos) != dst->quadrant)
        continue;
      int k2x = list[k2].head_pos & 0x3f;
      if (k2x < kx || (k2x == kx && k2 < k))
        xrank++;
    }
    if (xrank == rrank)
      return list[k].head_pos;
  }
  if (cat_count == 1)
    return list[only_cat].head_pos;  // quadrant-boundary tolerance
  return -1;
}

uint8 Rando_DoorSpiralDest(uint16 room, uint8 slot, uint8 attr, uint8 vanilla_byte) {
  g_door_spiral_pending = 0xFFFF;
  g_door_spiral_source = 0xFFFF;
  if (!DoorRt_Active())
    return vanilla_byte;
  // Spiral (circular) staircases carry head attrs 0x5e/0x5f and route to
  // submodule 14 (Module07_0E_SpiralStairs — the kDungeonSubmodules table is
  // the authority; the helper on that branch is misleadingly named
  // UsedForStraightInterRoomStaircase). Attrs 0x38/0x39 are the STRAIGHT
  // inter-room stairs (submodule 18/19, Module07_11) and 0x26 the fat stairs
  // (submodule 6) — neither is shuffled at intensity 1.
  if (attr != 0x5e && attr != 0x5f)
    return vanilla_byte;
  // Resolve the engine stair slot (attr2 bits) to its door record via the
  // staircase-list bijection — door_index is NOT the engine slot (see the
  // correspondence block above), so a direct door_index==slot match picks
  // the wrong record in multi-spiral rooms (GT Lobby, GT Torch/Hope, PoD,
  // Hera, Ice...). The list also yields the SOURCE head anchor for the
  // arrival fixup's head-to-head translation.
  uint16 src_head = 0;
  uint16 src_id = DoorRt_SpiralRecordForSlot(room, slot & 3, &src_head);
  if (src_id == 0xFFFF)
    return vanilla_byte;  // defensive: unresolvable stair, keep the vanilla spiral
  uint16 dest_id = g_door_link[src_id];
  if (dest_id == kDoorRt_NoOverride || dest_id >= kDoorTbl_DoorCount)
    return vanilla_byte;
  DoorRt_MarkDiscovered(src_id);
  g_door_spiral_src_x = (src_head & 0x3f) << 3;
  g_door_spiral_src_y = ((src_head >> 6) & 0x3f) << 3;
  g_door_spiral_source = src_id;
  g_door_spiral_pending = dest_id;
  return kDoorTblDoors[dest_id].room;
}

void Rando_DoorSpiralFixup(void) {
  if (g_door_spiral_pending == 0xFFFF)
    return;
  const DoorTblDoor *dst = &kDoorTblDoors[g_door_spiral_pending];
  // NB: g_door_spiral_pending stays armed — Rando_DoorSpiralLayerFix consumes
  // it after Module07_0E_13_SetRoomAndLayerAndCache's layer write (which would
  // otherwise clobber the destination layer from the stale SOURCE-header
  // cur_staircase_plane).
  g_door_spiral_source = 0xFFFF;

  // The vanilla flow already applied the grid-granular move
  // (Dungeon_AdjustAfterSpiralStairs with prev = the SOURCE room), keeping
  // Link's intra-room offset. Vanilla spiral pairs share that offset;
  // shuffled ones need not — locate the destination staircase head in the
  // freshly loaded room and translate by the head-to-head delta against the
  // source anchor captured in Rando_DoorSpiralDest. The locator reads the
  // staircase OBJECT list, NOT the derived attr table: Dungeon_LoadRoom (just
  // above) fills the list synchronously, while the attr table only rebuilds
  // in the chunked loader from spiral stage 7 — scanning it here found the
  // SOURCE room's own head and produced a zero delta. Record-to-stair
  // matching goes through the (direction, quadrant, x-rank) bijection (see
  // the correspondence block above DoorRt_SpiralStairList).
  int found_pos = DoorRt_SpiralHeadForRecord(dst);
  if (found_pos < 0)
    return;  // defensive: leave vanilla offset (playtest-visible, not fatal)
  g_door_spiral_dst_layer = (found_pos & 0x1000) ? 1 : 0;
  g_door_spiral_dst_layer_valid = true;
  int tx = (found_pos & 0x3f) << 3;        // x within supertile, px
  int ty = ((found_pos >> 6) & 0x3f) << 3; // y within supertile, px
  int dxp = tx - (int)g_door_spiral_src_x;
  int dyp = ty - (int)g_door_spiral_src_y;

  // Translate the whole mid-spiral walk tableau, not just Link: the walk
  // choreography is position-anchored. tiledetect_which_y_pos[1] is the
  // walk X target (seeded at entry; HandleLinkOnSpiralStairs terminates the
  // walk-in on low-byte equality against it, and ONLY that termination calls
  // RepositionLinkAfterSpiralStairs, which restores link_visibility_status —
  // a stale target leaves Link permanently invisible while the timer-driven
  // stages finish around him). [0] is the entry Y, the stair-walk sprite
  // clip line (player_oam). The delta keeps Link's walk progress relative
  // to the stair structure, exactly as a vanilla (same-offset) pair would.
  link_x_coord += dxp;
  link_y_coord += dyp;
  tiledetect_which_y_pos[1] += dxp;
  tiledetect_which_y_pos[0] += dyp;

  // Per-axis quadrant-coupled state, as in DoorRt_Arrive: re-anchor the
  // confined camera stops (a0/a1 move 0x100 with a quadrant flip), follow-
  // shift the camera then CLAMP into [a0,a1] (the per-frame camera stops
  // are equality tests — out-of-range scrolls unbounded), and re-seed the
  // follow thresholds from the camera phase.
  uint8 new_qx = ((link_x_coord & 0x1FF) >= 256) ? 1 : 0;
  if (new_qx != link_quadrant_x) {
    int qa = new_qx ? 0x100 : -0x100;
    room_bounds_x.a0 += qa;
    room_bounds_x.a1 += qa;
    link_quadrant_x = new_qx;
  }
  uint8 new_qy = ((link_y_coord & 0x1FF) >= 256) ? 2 : 0;
  if (new_qy != link_quadrant_y) {
    int qa = new_qy ? 0x100 : -0x100;
    room_bounds_y.a0 += qa;
    room_bounds_y.a1 += qa;
    link_quadrant_y = new_qy;
  }
  uint8 fsx = DoorRt_DestFullsize(dst->room, link_quadrant_x, link_quadrant_y, false);
  int cam = (int)BG2HOFS_copy2 + dxp;
  int lo = fsx ? room_bounds_x.b0 : room_bounds_x.a0;
  int hi = fsx ? room_bounds_x.b1 : room_bounds_x.a1;
  if (cam < lo) cam = lo;
  if (cam > hi) cam = hi;
  BG2HOFS_copy2 = (uint16)cam;
  camera_x_coord_scroll_low = (cam & 0x1ff) + 127;
  camera_x_coord_scroll_hi = camera_x_coord_scroll_low + 2;

  uint8 fsy = DoorRt_DestFullsize(dst->room, link_quadrant_x, link_quadrant_y, true);
  cam = (int)BG2VOFS_copy2 + dyp;
  lo = fsy ? room_bounds_y.b0 : room_bounds_y.a0;
  hi = fsy ? room_bounds_y.b1 : room_bounds_y.a1;
  if (cam < lo) cam = lo;
  if (cam > hi) cam = hi;
  BG2VOFS_copy2 = (uint16)cam;
  camera_y_coord_scroll_low = (cam & 0x1ff) + 120;
  camera_y_coord_scroll_hi = camera_y_coord_scroll_low + 2;

  Dungeon_AdjustQuadrant();
  for (int i = 0; i < 20; i++)
    tagalong_y_hi[i] = link_y_coord >> 8;
}

void DoorRt_ClearSpiralPending(void) {
  g_door_spiral_pending = 0xFFFF;
  g_door_spiral_source = 0xFFFF;
  g_door_spiral_dst_layer_valid = false;
  // Abort any in-flight arrival fine-pan too (this is the snapshot-restore /
  // teardown "cancel transition fixups" hook).
  g_door_fine_active = false;
  g_door_landing_page_active = false;
  g_door_landing_page_axis_y = false;
}

void Rando_DoorSpiralLayerFix(void) {
  if (g_door_spiral_pending == 0xFFFF)
    return;
  const DoorTblDoor *dst = &kDoorTblDoors[g_door_spiral_pending];
  g_door_spiral_pending = 0xFFFF;
  // Module07_0E_13_SetRoomAndLayerAndCache just set the layer from
  // kTeleportPitLevel1/2[cur_staircase_plane] — the SOURCE room header's
  // per-staircase plane, which describes the VANILLA destination. The true
  // authority is the destination stair's own plane: the object-list half
  // bit captured by the fixup (fallback: bit1 of the record's HTH/HTL/
  // LTH/LTL signature — never the raw field, which is not a plane).
  uint8 layer = g_door_spiral_dst_layer_valid ? g_door_spiral_dst_layer
                                              : (uint8)((dst->layer >> 1) & 1);
  g_door_spiral_dst_layer_valid = false;
  link_is_on_lower_level = layer;
  link_is_on_lower_level_mirror = layer;
}

// Called right after Dungeon_DetectStaircase loads cur_staircase_plane from
// the SOURCE room header (which describes the VANILLA destination). For a
// redirected spiral, substitute the SHUFFLED destination's plane class so
// the downstream plane gymnastics (RepositionLinkAfterSpiralStairs' TM/TS
// branch, Module07_0E_13's kTeleportPitLevel) act on the true target:
// low-plane stairs (signature bit1) behave like vanilla plane 2, high-plane
// like plane 0.
uint8 Rando_DoorSpiralPlane(uint8 vanilla_plane) {
  if (g_door_spiral_pending == 0xFFFF)
    return vanilla_plane;
  const DoorTblDoor *dst = &kDoorTblDoors[g_door_spiral_pending];
  return ((dst->layer >> 1) & 1) ? 2 : 0;
}

// ---------------------------------------------------------------------------
// Stage-1b door-KIND overlay (relocated small-key doors).
//
// Port of the reference's reassign_key_doors / change_door_to_small_key /
// verify_door_list_pos / Room.next_free (ALttPDoorRandomizer DoorShuffle.py +
// RoomData.py, MIT), adapted to the C engine's door-list shape:
//   * The engine door word is pos_byte | kind << 8 (RoomData_DrawObject_Door:
//     door_type = a >> 8, position nibble = a >> 4 & 0xf, direction = a & 3).
//   * The list is 0xffff-TERMINATED (RoomDraw_DrawAllObjects / GetRoomDoorInfo),
//     so the reference's Room.delete (write 0xFFFF) / Room.mirror tricks for
//     un-chosen stair keys would truncate the list. DEVIATION: un-chosen
//     vanilla stair keys keep their StairKey kind and are PRE-OPENED instead
//     (Rando_DoorPreopenBits at Dungeon_LoadHeader): an open stair-lock slot
//     draws nothing (Door_Up_StairMaskLocked early-out) and never writes the
//     f0 lock attr (Dungeon_LoadSingleDoorAttribute), i.e. exactly "no lock".
//   * Open-state bits exist per door SLOT 0-3 (save_dung_info & 0xf000;
//     RoomDraw_FlagDoorsAndGetFinalType honors opened-state only for
//     (slot&7) < 4, and Dungeon_LoadHeader pre-opens slots 4-7 via | 0xf00).
//     The slot == door-list index whenever no kind in {0x12 ExitToOw,
//     0x14 DungeonChanger/ThroneRoom, 0x16 ToggleFlag/PlayerBgChange}
//     precedes the entry (those RoomDraw_Door_* arms do not advance
//     dung_cur_door_idx). Install HARD-ERRORS if a flagged entry's slot
//     drifts from its index (none do in the US data; the adjacency scan in
//     Dungeon_CheckAdjacentRoomsForOpenDoors conflates index with slot the
//     same way, so this is also a vanilla-consistency requirement).
//
// Skull Pinball WS (kind 0x18 Trap, kDoorTblFlag_Blocked|Trapped) is NEVER
// touched: generation keeps it blocked (shuffle_doors.c models vanilla
// trap_door_mode; check_for_pinball_fix's runtime mutation is not modeled),
// it is not a vanilla small key (no VanillaSmallKey flag), and Blocked doors
// are never key-door candidates (door_keylogic.c FindCandidates).
// ---------------------------------------------------------------------------

// DR Room.next_free blacklist: kinds with state/locks that must keep their
// slot; everything else may swap above index 4.
static bool DoorRt_KindSwappable(uint8 kind) {
  switch (kind) {
  case 0x1C: case 0x28: case 0x2E: case 0x36: case 0x18: case 0x38:
  case 0x44: case 0x4A: case 0x1E: case 0x20: case 0x22: case 0x26:
  case 0x30: case 0x2A:
    return false;
  }
  return true;
}

// Kinds whose RoomDraw_Door_* arm does NOT advance dung_cur_door_idx (door
// words that consume a list index but no engine slot).
static bool DoorRt_KindAdvancesSlot(uint8 kind) {
  return kind != 0x12 && kind != 0x14 && kind != 0x16;
}

static void DoorRt_BuildTblRoomLookup(void) {
  if (g_kind_lookup_built)
    return;
  memset(g_tblroom_of, 0xFF, sizeof(g_tblroom_of));
  for (int i = 0; i < kDoorTbl_RoomCount; i++)
    g_tblroom_of[kDoorTblRooms[i].room] = (int16)i;
  g_kind_lookup_built = true;
}

// Get-or-create the overlay record for `room`, seeded from the catalog's
// vanilla door list. NULL on inconsistent catalog data.
static DoorRtKindRoom *DoorRt_KindRoomGet(uint8 room) {
  if (g_kind_room_of[room] >= 0)
    return &g_kind_rooms[g_kind_room_of[room]];
  int tr = g_tblroom_of[room];
  if (tr < 0) {
    fprintf(stderr, "door-kind-overlay: room %02x missing from kDoorTblRooms\n", room);
    return NULL;
  }
  const DoorTblRoom *r = &kDoorTblRooms[tr];
  if (r->count > 8) {
    fprintf(stderr, "door-kind-overlay: room %02x door list too long (%d)\n", room, r->count);
    return NULL;
  }
  DoorRtKindRoom *kr = &g_kind_rooms[g_kind_nrooms];
  kr->room = room;
  kr->count = r->count;
  for (int i = 0; i < r->count; i++) {
    const DoorTblRoomDoor *rd = &kDoorTblRoomDoors[r->first + i];
    kr->e[i].vanilla_word = (uint16)(rd->pos_byte | (rd->kind << 8));
    kr->e[i].overlay_word = kr->e[i].vanilla_word;
    kr->e[i].flags = 0;
    kr->e[i].partner_room = 0xFF;
    kr->e[i].partner_slot = 0xFF;
  }
  g_kind_room_of[room] = (int16)g_kind_nrooms++;
  return kr;
}

// Resolve door `door_id` to its overlay room + current entry index (entries
// may have been swapped by an earlier DoorRt_KindMakeSmallKey). The door is
// identified by its catalog (room, pos); a swap moves the WORD but the swap
// bookkeeping below always converts pos -> current index right away, so a
// door's entry is found by matching its pos_byte among entries that came from
// its vanilla pos. We track swaps per room with a tiny permutation.
static uint8 g_kind_perm[kDoorTbl_RoomCount][8];  // perm[room_rec][vanilla_pos] = current idx

static int DoorRt_KindEntryIdx(const DoorRtKindRoom *kr, uint8 vanilla_pos) {
  if (vanilla_pos >= kr->count)
    return -1;
  return g_kind_perm[(int)(kr - g_kind_rooms)][vanilla_pos];
}

// change_door_to_small_key + verify_door_list_pos: ensure the door's entry
// sits at slot < 4 (swap wholesale with the lowest swappable entry if not —
// dead code today: FindCandidates in door_keylogic.c, like the reference's
// find_key_door_candidates, only emits candidates with doorListPos < 4), then
// set its kind. `kind_keep` keeps the vanilla kind (chosen spiral key stairs:
// the StairKey* kind IS the small-key behavior for stairs). Returns the final
// entry index, -1 on hard error.
static int DoorRt_KindMakeSmallKey(uint16 door_id, bool kind_keep) {
  const DoorTblDoor *d = &kDoorTblDoors[door_id];
  if (d->pos == 0xFF) {
    fprintf(stderr, "door-kind-overlay: chosen key door %d has no door-list entry\n", door_id);
    return -1;
  }
  DoorRtKindRoom *kr = DoorRt_KindRoomGet(d->room);
  if (!kr)
    return -1;
  int idx = DoorRt_KindEntryIdx(kr, d->pos);
  if (idx < 0) {
    fprintf(stderr, "door-kind-overlay: door %d pos %d out of range (room %02x)\n",
            door_id, d->pos, d->room);
    return -1;
  }
  if ((kr->e[idx].vanilla_word & 0xFF) != d->pos_byte) {
    fprintf(stderr, "door-kind-overlay: catalog drift door %d (room %02x pos %d)\n",
            door_id, d->room, d->pos);
    return -1;
  }
  if (idx >= 4) {
    // verify_door_list_pos: swap with Room.next_free (lowest index < 4 whose
    // kind is swappable). Entries swap wholesale (pos_byte travels with kind).
    int free_idx = -1;
    for (int i = 0; i < 4 && i < kr->count; i++) {
      if (!(kr->e[i].flags & kDoorRtKindF_SmallKey) &&
          DoorRt_KindSwappable((uint8)(kr->e[i].overlay_word >> 8))) {
        free_idx = i;
        break;
      }
    }
    if (free_idx < 0) {
      fprintf(stderr, "door-kind-overlay: no swap slot < 4 for key door %d (room %02x)\n",
              door_id, d->room);
      return -1;
    }
    DoorRtKindEntry t = kr->e[free_idx];
    kr->e[free_idx] = kr->e[idx];
    kr->e[idx] = t;
    // update the pos->idx permutation for the two moved entries
    uint8 *perm = g_kind_perm[(int)(kr - g_kind_rooms)];
    for (int p = 0; p < kr->count; p++) {
      if (perm[p] == idx) perm[p] = (uint8)free_idx;
      else if (perm[p] == free_idx) perm[p] = (uint8)idx;
    }
    idx = free_idx;
  }
  if (!kind_keep)
    kr->e[idx].overlay_word = (uint16)((kr->e[idx].overlay_word & 0xFF) | (0x1C << 8));
  kr->e[idx].flags |= kDoorRtKindF_SmallKey | kDoorRtKindF_Suppress;
  return idx;
}

// Validate that every flagged entry's engine SLOT equals its list index (see
// the slot/index note in the header comment above).
static bool DoorRt_KindCheckSlots(void) {
  for (int r = 0; r < g_kind_nrooms; r++) {
    const DoorRtKindRoom *kr = &g_kind_rooms[r];
    int slot = 0;
    for (int i = 0; i < kr->count; i++) {
      if (kr->e[i].flags && slot != i) {
        fprintf(stderr, "door-kind-overlay: room %02x entry %d maps to slot %d\n",
                kr->room, i, slot);
        return false;
      }
      if (DoorRt_KindAdvancesSlot((uint8)(kr->e[i].overlay_word >> 8)))
        slot++;
    }
  }
  return true;
}

bool DoorRt_InstallKindOverlay(const DoorShuffleLayout *l) {
  DoorRt_KindOverlayClear();
  DoorRt_BuildTblRoomLookup();
  if (!l)
    return true;  // no layout = identity overlay
  for (int r = 0; r < kDoorTbl_RoomCount; r++)
    for (int p = 0; p < 8; p++)
      g_kind_perm[r][p] = (uint8)p;

  // Track chosen halves by door id so the un-key pass skips them.
  static uint8 chosen[(kDoorTbl_DoorCount + 7) / 8];
  memset(chosen, 0, sizeof(chosen));

  bool ok = true;
  // --- 1. chosen key doors: kind = SmallKey on both halves -----------------
  for (uint8 d = 0; d < kDoorTbl_DungeonCount && ok; d++) {
    if (!((l->shuffled_mask >> d) & 1))
      continue;
    for (int i = 0; i < l->key_door_count[d] && ok; i++) {
      uint16 a = l->key_doors[d][i];
      if (a >= kDoorTbl_DoorCount) {
        fprintf(stderr, "door-kind-overlay: bad key door id %d (dungeon %d)\n", a, d);
        ok = false;
        break;
      }
      const DoorTblDoor *da = &kDoorTblDoors[a];
      chosen[a >> 3] |= 1 << (a & 7);
      if (da->type == kDoorTblType_SpiralStairs) {
        // Spiral candidates are vanilla key stairs (FindCandidates requires a
        // StairKey* kind); the entry keeps its kind — only flag it as chosen
        // so the un-key pass leaves it locked. No partner (a spiral key locks
        // only its own direction; DoorKeyPair.b == 0xFFFF).
        if (DoorRt_KindMakeSmallKey(a, /*kind_keep=*/true) < 0)
          ok = false;
        continue;
      }
      // Mirror find_key_door_candidates' partner semantics exactly:
      // Interior -> vanilla_partner (the pair shares ONE list entry — same
      // room + pos); Normal -> the layout pairing (Door_PartnerOf).
      uint16 b = Door_PartnerOf(a, l);
      if (b == 0xFFFF || b >= kDoorTbl_DoorCount) {
        fprintf(stderr, "door-kind-overlay: key door %d has no partner (dungeon %d)\n", a, d);
        ok = false;
        break;
      }
      const DoorTblDoor *db = &kDoorTblDoors[b];
      chosen[b >> 3] |= 1 << (b & 7);
      int ia = DoorRt_KindMakeSmallKey(a, false);
      if (ia < 0) {
        ok = false;
        break;
      }
      if (da->type == kDoorTblType_Interior && db->room == da->room && db->pos == da->pos)
        continue;  // shared entry: one write, one open bit, no mirror needed
      int ib = DoorRt_KindMakeSmallKey(b, false);
      if (ib < 0) {
        ok = false;
        break;
      }
      // Cross-entry pair (different rooms, or same supertile at two edges):
      // link both directions for the key-open partner mirror.
      DoorRtKindRoom *ka = &g_kind_rooms[g_kind_room_of[da->room]];
      DoorRtKindRoom *kb = &g_kind_rooms[g_kind_room_of[db->room]];
      ka->e[ia].partner_room = db->room;
      ka->e[ia].partner_slot = (uint8)ib;
      kb->e[ib].partner_room = da->room;
      kb->e[ib].partner_slot = (uint8)ia;
    }
  }

  // --- 2. abandoned vanilla key doors: un-key (reassign_key_doors's
  //        not-in-proposal arms) ---------------------------------------------
  for (int door = 0; door < kDoorTbl_DoorCount && ok; door++) {
    const DoorTblDoor *d = &kDoorTblDoors[door];
    if (!(d->flags & kDoorTblFlag_VanillaSmallKey))
      continue;
    if (!((l->shuffled_mask >> d->dungeon) & 1))
      continue;
    if ((chosen[door >> 3] >> (door & 7)) & 1)
      continue;
    if (d->pos == 0xFF)
      continue;
    DoorRtKindRoom *kr = DoorRt_KindRoomGet(d->room);
    if (!kr) {
      ok = false;
      break;
    }
    int idx = DoorRt_KindEntryIdx(kr, d->pos);
    if (idx < 0 || (kr->e[idx].vanilla_word & 0xFF) != d->pos_byte) {
      fprintf(stderr, "door-kind-overlay: catalog drift un-keying door %d (room %02x)\n",
              door, d->room);
      ok = false;
      break;
    }
    if (kr->e[idx].flags & kDoorRtKindF_SmallKey)
      continue;  // entry already keyed by a chosen pair (interior twin id)
    if (d->type == kDoorTblType_SpiralStairs) {
      kr->e[idx].flags |= kDoorRtKindF_Preopen;  // see the deviation note above
    } else {
      // change(doorListPos, DoorKind.Normal); by-layer NormalLow for lower-BG
      // doors (the reference writes flat Normal; no vanilla small-key NORMAL
      // door sits on layer 1 in the US data, so this only matters defensively).
      uint8 nk = d->layer ? 0x02 : 0x00;
      kr->e[idx].overlay_word = (uint16)((kr->e[idx].overlay_word & 0xFF) | (nk << 8));
    }
  }

  // --- 3. propagation suppression for every re-stitched pool door ----------
  // Any door the layout re-paired no longer faces its physical neighbor, so
  // the neighbor's open flags must not leak into its slot (and vice versa its
  // own bit is the sole authority). Trap/blastwall kinds are unaffected: the
  // suppression hook sits in the non-trapdoor arm only.
  for (int door = 0; door < kDoorTbl_DoorCount && ok; door++) {
    const DoorTblDoor *d = &kDoorTblDoors[door];
    if (l->pairing[door] == 0xFFFF || d->pos == 0xFF)
      continue;
    if (!((l->shuffled_mask >> d->dungeon) & 1))
      continue;
    DoorRtKindRoom *kr = DoorRt_KindRoomGet(d->room);
    if (!kr) {
      ok = false;
      break;
    }
    int idx = DoorRt_KindEntryIdx(kr, d->pos);
    if (idx >= 0)
      kr->e[idx].flags |= kDoorRtKindF_Suppress;
  }

  if (ok)
    ok = DoorRt_KindCheckSlots();
  if (!ok) {
    DoorRt_KindOverlayClear();
    return false;
  }
  g_kind_overlay_active = true;
  return true;
}

// ---------------------------------------------------------------------------
// Kind-overlay dungeon.c hooks
// ---------------------------------------------------------------------------

static const DoorRtKindEntry *DoorRt_KindEntryAt(uint16 room, int index) {
  if (!g_kind_overlay_active || room >= 256 || index < 0)
    return NULL;
  int r = g_kind_room_of[room];
  if (r < 0 || index >= g_kind_rooms[r].count)
    return NULL;
  return &g_kind_rooms[r].e[index];
}

uint16 Rando_DoorListWord(uint16 room, int index, uint16 vanilla_word) {
  if (!DoorRt_Active())
    return vanilla_word;
  const DoorRtKindEntry *e = DoorRt_KindEntryAt(room, index);
  if (!e || e->overlay_word == e->vanilla_word)
    return vanilla_word;
  if (vanilla_word != e->vanilla_word) {
    // Engine door list disagrees with the generated catalog — never write a
    // kind into a list we don't model (fail to vanilla, warn once per room).
    if (!((g_kind_warned[room >> 3] >> (room & 7)) & 1)) {
      g_kind_warned[room >> 3] |= 1 << (room & 7);
      fprintf(stderr,
              "door-kind-overlay: room %02x entry %d engine word %04x != catalog %04x "
              "(overlay skipped)\n",
              room, index, vanilla_word, e->vanilla_word);
    }
    return vanilla_word;
  }
  return e->overlay_word;
}

uint16 Rando_DoorPreopenBits(uint16 room) {
  if (!DoorRt_Active() || !g_kind_overlay_active || room >= 256)
    return 0;
  int r = g_kind_room_of[room];
  if (r < 0)
    return 0;
  uint16 bits = 0;
  const DoorRtKindRoom *kr = &g_kind_rooms[r];
  for (int i = 0; i < kr->count && i < 4; i++) {
    if (kr->e[i].flags & kDoorRtKindF_Preopen)
      bits |= 0x8000 >> i;  // kUpperBitmasks[i] form (save_dung_info 0xf000)
  }
  return bits;
}

bool Rando_DoorAdjOpenSuppressed(uint16 room, int slot) {
  if (!DoorRt_Active())
    return false;
  const DoorRtKindEntry *e = DoorRt_KindEntryAt(room, slot);
  return e != NULL && (e->flags & (kDoorRtKindF_Suppress | kDoorRtKindF_SmallKey)) != 0;
}

void Rando_DoorKeyOpenMirror(uint16 room, int slot) {
  if (!DoorRt_Active())
    return;
  const DoorRtKindEntry *e = DoorRt_KindEntryAt(room, slot);
  if (!e || !(e->flags & kDoorRtKindF_SmallKey) || e->partner_slot == 0xFF ||
      e->partner_slot >= 4)
    return;
  uint16 bit = 0x8000 >> e->partner_slot;
  // Persist the partner half's open bit directly (its room is not loaded; the
  // bit lands where Dung_SaveDataForCurrentRoom would put it). For a SAME-room
  // partner also set the live bits — Dung_SaveDataForCurrentRoom rewrites
  // save_dung_info from dung_door_opened at room exit, so the live bit is the
  // one that must be authoritative there.
  save_dung_info[e->partner_room] |= bit;
  if (e->partner_room == (uint8)room) {
    dung_door_opened |= bit;
    dung_door_opened_incl_adjacent |= bit;
  }
}

bool Rando_DoorKeySlotAlreadyOpen(int slot) {
  if (!DoorRt_Active() || slot < 0 || slot >= 4)
    return false;
  const DoorRtKindEntry *e = DoorRt_KindEntryAt(dungeon_room_index, slot);
  if (!e || !(e->flags & kDoorRtKindF_SmallKey))
    return false;
  return (dung_door_opened & (0x8000 >> slot)) != 0;
}

int DoorRt_GeometrySelfCheck(void) {
  int fails = 0;
  struct {
    uint16 coord, camera, want;
  } repage_cases[] = {
    { 0x0032, 0x00F8, 0x0132 },
    { 0x0132, 0x00F8, 0x0132 },
    { 0x00F0, 0x0000, 0x00F0 },
    { 0x00F0, 0x0100, 0x01F0 },
    { 0x0532, 0x05F8, 0x0632 },
  };
  for (int i = 0; i < (int)(sizeof(repage_cases) / sizeof(repage_cases[0])); i++) {
    uint16 got = DoorRt_RepageCoordToCamera(repage_cases[i].coord, repage_cases[i].camera);
    if (got != repage_cases[i].want) {
      fprintf(stderr,
              "door-geometry-selfcheck: landing re-page case %d got %04x "
              "expected %04x\n",
              i, got, repage_cases[i].want);
      fails++;
    }
  }
  int16 seen[256][4][2][3];
  memset(seen, 0xFF, sizeof(seen));
  for (int door = 0; door < kDoorTbl_DoorCount; door++) {
    const DoorTblDoor *d = &kDoorTblDoors[door];
    if (d->type != kDoorTblType_Normal)
      continue;
    if (d->flags & kDoorTblFlag_VanillaTeleport)
      continue;  // teleport doors do not use the edge-transition resolver
    int slot = DoorRt_OuterSlot(d->direction, d->pos_byte);
    if (slot < 0) {
      if (!(d->flags & kDoorTblFlag_InPool))
        continue;
      fprintf(stderr,
              "door-geometry-selfcheck: pool door %d '%s' pos byte %02x "
              "does not resolve to an outer slot\n",
              door, &kDoorTblNames[d->name_off], d->pos_byte);
      fails++;
      continue;
    }
    uint8 layer = d->layer & 1;
    int16 *prev = &seen[d->room][d->direction][layer][slot];
    if (*prev >= 0) {
      const DoorTblDoor *pd = &kDoorTblDoors[*prev];
      fprintf(stderr,
              "door-geometry-selfcheck: doors %d '%s' and %d '%s' share "
              "runtime candidate room %02x dir %d layer %d slot %d\n",
              *prev, &kDoorTblNames[pd->name_off],
              door, &kDoorTblNames[d->name_off],
              d->room, d->direction, layer, slot);
      fails++;
    } else {
      *prev = (int16)door;
    }
    if ((d->flags & kDoorTblFlag_InPool) && slot != d->door_index) {
      fprintf(stderr,
              "door-geometry-selfcheck: pool door %d '%s' pos byte %02x "
              "resolves slot %d, catalog door_index %d\n",
              door, &kDoorTblNames[d->name_off], d->pos_byte, slot, d->door_index);
      fails++;
    }
  }
  return fails;
}

// ---------------------------------------------------------------------------
// Kind-overlay selftest (called per layout from DoorShuffle_SelfTest)
// ---------------------------------------------------------------------------

int DoorRt_KindOverlaySelfCheck(const DoorShuffleLayout *l) {
  int fails = 0;
  if (!DoorRt_InstallKindOverlay(l)) {
    fprintf(stderr, "door-kind-selfcheck: install failed\n");
    return 1;
  }
  // (a) every chosen key door + partner: SmallKey-flagged entry at index < 4
  // with a key kind (0x1C, or a kept StairKey* kind for spirals).
  for (uint8 d = 0; d < kDoorTbl_DungeonCount; d++) {
    if (!((l->shuffled_mask >> d) & 1))
      continue;
    for (int i = 0; i < l->key_door_count[d]; i++) {
      uint16 half[2];
      half[0] = l->key_doors[d][i];
      half[1] = (kDoorTblDoors[half[0]].type == kDoorTblType_SpiralStairs)
                    ? 0xFFFF
                    : Door_PartnerOf(half[0], l);
      for (int h = 0; h < 2; h++) {
        if (half[h] == 0xFFFF)
          continue;
        const DoorTblDoor *dd = &kDoorTblDoors[half[h]];
        int r = g_kind_room_of[dd->room];  // room is uint8; always < 256 (gen-asserted)
        int idx = -1;
        if (r >= 0)
          idx = DoorRt_KindEntryIdx(&g_kind_rooms[r], dd->pos);
        const DoorRtKindEntry *e = (r >= 0 && idx >= 0) ? &g_kind_rooms[r].e[idx] : NULL;
        uint8 kind = e ? (uint8)(e->overlay_word >> 8) : 0xFF;
        bool key_kind = kind == 0x1C || kind == 0x20 || kind == 0x22 || kind == 0x26;
        if (!e || idx >= 4 || !(e->flags & kDoorRtKindF_SmallKey) || !key_kind) {
          fprintf(stderr,
                  "door-kind-selfcheck: chosen half %d (dungeon %d) not keyed at slot<4 "
                  "(idx %d kind %02x)\n",
                  half[h], d, idx, kind);
          fails++;
        }
      }
    }
  }
  // (b) every overlaid room keeps the same multiset of pos_bytes (kind
  // changes preserve the low byte; swaps only permute entries).
  for (int r = 0; r < g_kind_nrooms; r++) {
    const DoorRtKindRoom *kr = &g_kind_rooms[r];
    int tr = g_tblroom_of[kr->room];
    uint8 want[8], got[8];
    for (int i = 0; i < kr->count; i++) {
      want[i] = kDoorTblRoomDoors[kDoorTblRooms[tr].first + i].pos_byte;
      got[i] = (uint8)(kr->e[i].overlay_word & 0xFF);
    }
    for (int i = 1; i < kr->count; i++) {  // insertion sort both
      for (int j = i; j > 0 && want[j - 1] > want[j]; j--) {
        uint8 t = want[j]; want[j] = want[j - 1]; want[j - 1] = t;
      }
      for (int j = i; j > 0 && got[j - 1] > got[j]; j--) {
        uint8 t = got[j]; got[j] = got[j - 1]; got[j - 1] = t;
      }
    }
    if (memcmp(want, got, kr->count) != 0) {
      fprintf(stderr, "door-kind-selfcheck: room %02x pos_byte multiset changed\n", kr->room);
      fails++;
    }
  }
  // (c) abandoned vanilla key doors in shuffled dungeons are un-keyed
  // (Normal/NormalLow kind, or Preopen for stairs); Blocked doors untouched
  // (covers Skull Pinball WS); pinned-dungeon doors untouched.
  for (int door = 0; door < kDoorTbl_DoorCount; door++) {
    const DoorTblDoor *dd = &kDoorTblDoors[door];
    if (dd->pos == 0xFF)  // room is uint8; always < 256 (gen-asserted)
      continue;
    bool shuffled = ((l->shuffled_mask >> dd->dungeon) & 1) != 0;
    int r = g_kind_room_of[dd->room];
    if (r < 0) {
      if ((dd->flags & kDoorTblFlag_VanillaSmallKey) && shuffled) {
        fprintf(stderr, "door-kind-selfcheck: vanilla key door %d room not overlaid\n", door);
        fails++;
      }
      continue;
    }
    int idx = DoorRt_KindEntryIdx(&g_kind_rooms[r], dd->pos);
    if (idx < 0)
      continue;
    const DoorRtKindEntry *e = &g_kind_rooms[r].e[idx];
    uint8 kind = (uint8)(e->overlay_word >> 8);
    if (!shuffled || (dd->flags & kDoorTblFlag_Blocked)) {
      if (e->overlay_word != e->vanilla_word) {
        fprintf(stderr, "door-kind-selfcheck: pinned/blocked door %d kind changed\n", door);
        fails++;
      }
      continue;
    }
    if ((dd->flags & kDoorTblFlag_VanillaSmallKey) && !(e->flags & kDoorRtKindF_SmallKey)) {
      bool unkeyed = (dd->type == kDoorTblType_SpiralStairs)
                         ? (e->flags & kDoorRtKindF_Preopen) != 0
                         : (kind == 0x00 || kind == 0x02);
      if (!unkeyed) {
        fprintf(stderr, "door-kind-selfcheck: vanilla key door %d not un-keyed (kind %02x)\n",
                door, kind);
        fails++;
      }
    }
  }
  return fails;
}
