// inverted_entrances.c — static Inverted-mode entrance/exit override. See header.
#include "inverted_entrances.h"

#include "rando_settings.h"  // kWorldState_Inverted
#include "../assets.h"       // g_asset_ptrs[] / g_asset_sizes[] + asset-index macros

#include <stdio.h>           // fprintf (oversize-asset diagnostic)
#include <stdlib.h>          // exit (selfcheck)
#include <string.h>          // memcpy

// ---------------------------------------------------------------------------
// Override table. Each row repoints one element of one asset array. `width` is
// the element size in bytes (1 or 2) — it must match the asset array's C type
// (asset 126 = uint8, asset 130 = uint8, asset 131 = uint16). Adding a future
// swap (DM-west / Bumper Cave, spawn, etc.) is a pure data edit here, provided
// the touched asset index is registered in g_shadows below.
//
// All values verified against ALTTPR app/Rom.php setInvertedMode() AND the
// fork's own interior-id constants (shuffle_entrance.c). See the change notes.
typedef struct InvertedOverride {
  uint8  asset_index;  // g_asset_ptrs[] / g_asset_sizes[] slot
  uint8  width;        // element width in bytes: 1 or 2
  uint16 elem_index;   // element index within the array
  uint16 value;        // new value (low byte used when width == 1)
} InvertedOverride;

// Asset indices touched (mirrors src/assets.h:254,262,264).
enum {
  kAsset_Overworld_Entrance_Id = 126,  // uint8:  door-slot -> entrance-id
  kAsset_ExitData_ScreenIndex  = 130,  // uint8:  exit-id   -> screen index
  kAsset_ExitDataRooms         = 131,  // uint16: exit-id   -> destination room
  // Exit-data parallel columns (uint16) — used by full exit-row relocations.
  kAsset_ExitData_Map16        = 132,  // uint16: map16 load src offset
  kAsset_ExitData_ScrollX      = 133,  // uint16
  kAsset_ExitData_ScrollY      = 134,  // uint16
  kAsset_ExitData_XCoord       = 135,  // uint16: Link X on exit
  kAsset_ExitData_YCoord       = 136,  // uint16: Link Y on exit
  kAsset_ExitData_CameraX      = 137,  // uint16
  kAsset_ExitData_CameraY      = 138,  // uint16
  kAsset_ExitData_NormalDoor   = 139,  // uint16
  kAsset_ExitData_FancyDoor    = 140,  // uint16
  kAsset_ExitData_Unk1         = 141,  // int8: exit camera-nudge X
  kAsset_ExitData_Unk3         = 142,  // int8: exit camera-nudge Y
  kAsset_BirdTravel_ScreenIndex = 113, // uint16: flute-spot slot -> dest screen
  kAsset_BirdTravel_LinkXCoord  = 117,  // uint16: Link X after flute travel
  kAsset_BirdTravel_LinkYCoord  = 118,  // uint16: Link Y after flute travel
};

static const InvertedOverride kInvertedOverrides[] = {
  // --- Link's House <-> Bomb Shop ---
  // Link's House door (slot 0x00) now loads the Bomb Shop interior (0x53).
  { kAsset_Overworld_Entrance_Id, 1, 0x00, 0x53 },
  // Bomb Shop door (slot 0x52) now loads the Link's House interior (0x01).
  { kAsset_Overworld_Entrance_Id, 1, 0x52, 0x01 },
  // Link's House exit screen -> dark world (PHP: 0x15B8C +0x00 = 0x6C).
  { kAsset_ExitData_ScreenIndex,  1, 0x00, 0x6C },

  // --- Ganon's Tower <-> Agahnim's (Hyrule Castle) Tower ---
  // Vanilla AT door (slot 0x23) now loads the GT interior (0x37).
  { kAsset_Overworld_Entrance_Id, 1, 0x23, 0x37 },
  // Vanilla GT door (slot 0x36) now loads the AT interior (0x24).
  { kAsset_Overworld_Entrance_Id, 1, 0x36, 0x24 },
  // AT exit (exit-id 0x38) -> GT overworld room (PHP: 0x15AEE +2*0x38 = 0x00E0).
  { kAsset_ExitDataRooms,         2, 0x38, 0x00E0 },
  // GT exit (exit-id 0x25) -> AT overworld room (PHP: 0x15AEE +2*0x25 = 0x000C).
  { kAsset_ExitDataRooms,         2, 0x25, 0x000C },

  // --- Inverted Dark-World Death Mountain cave rework (ALTTPR Rom.php:1626-1663).
  // The DM-west cave doors swap interiors so the DW-DM cave system is navigable
  // from the DW mainland in Inverted. Door-id values verified against Rom.php;
  // exit-room / full-row column values transcribed verbatim (PLAYTEST-PENDING). ---
  // Bumper Cave (Bottom) door 0x15 -> Old Man Cave (West) interior 0x06.
  { kAsset_Overworld_Entrance_Id, 1, 0x15, 0x06 },
  { kAsset_ExitDataRooms,         2, 0x17, 0x00F0 },
  // Old Man Cave (West) door 0x05 -> Bumper Cave (Bottom) interior 0x16.
  { kAsset_Overworld_Entrance_Id, 1, 0x05, 0x16 },
  { kAsset_ExitDataRooms,         2, 0x07, 0x00FB },
  // DM Return Cave (West) door 0x2D -> Bumper Cave (Top) interior 0x17.
  { kAsset_Overworld_Entrance_Id, 1, 0x2D, 0x17 },
  { kAsset_ExitDataRooms,         2, 0x2F, 0x00EB },
  // Old Man Cave (East) door 0x06 -> DM Return Cave (West) interior 0x2E.
  { kAsset_Overworld_Entrance_Id, 1, 0x06, 0x2E },
  { kAsset_ExitDataRooms,         2, 0x08, 0x00E6 },
  // Bumper Cave (Top) door 0x16 -> Dark DM Healer Fairy interior 0x5E.
  { kAsset_Overworld_Entrance_Id, 1, 0x16, 0x5E },
  // Dark DM Healer Fairy door 0x6F -> Old Man Cave (East) interior 0x07, with a
  // full exit-row relocation for exit-id 0x18 (the cave now exits to screen 0x43).
  { kAsset_Overworld_Entrance_Id, 1, 0x6F, 0x07 },
  { kAsset_ExitDataRooms,         2, 0x18, 0x00F1 },
  { kAsset_ExitData_ScreenIndex,  1, 0x18, 0x43 },
  { kAsset_ExitData_Map16,        2, 0x18, 0x1400 },
  // ALTTPR writes the ROM columns in Y-before-X order (0x15C79/0x15D17,
  // 0x15DB5/0x15E53, 0x15EF1/0x15F8F). These C assets are named X-before-Y.
  { kAsset_ExitData_ScrollX,      2, 0x18, 0x0600 },
  { kAsset_ExitData_ScrollY,      2, 0x18, 0x0294 },
  { kAsset_ExitData_XCoord,       2, 0x18, 0x0678 },
  { kAsset_ExitData_YCoord,       2, 0x18, 0x02E8 },
  { kAsset_ExitData_CameraX,      2, 0x18, 0x0685 },
  { kAsset_ExitData_CameraY,      2, 0x18, 0x0303 },
  // Complete the Healer-fairy exit-0x18 row with the remaining columns ALTTPR
  // writes (Rom.php:1660-1663): the int8 camera nudge (Unk1/Unk3 = 0x0a/0xf6)
  // and the door-clear (NormalDoor/FancyDoor -> 0, so the relocated exit shows
  // no stale door). The int8 columns use width 1. Resolved 0x1602D/0x1607C to
  // Unk1/Unk3 via the extractor (the only int8 exit columns, +idx single-byte).
  { kAsset_ExitData_NormalDoor,   2, 0x18, 0x0000 },
  { kAsset_ExitData_FancyDoor,    2, 0x18, 0x0000 },
  { kAsset_ExitData_Unk1,         1, 0x18, 0x000A },
  { kAsset_ExitData_Unk3,         1, 0x18, 0x00F6 },

  // --- Inverted flute travel: Dark-World destinations (ALTTPR Rom.php:1600). ---
  // The fork warps Link to raw screen indices (overworld.c FluteMenu path), with
  // no inverted reinterpretation, so vanilla LW spots send an Inverted player to
  // the wrong world. Repoint each flute slot to its Dark-World mirror (LW + 0x40).
  // asset 113 is uint16 (width 2); elem_index is the flute slot (bird_travel_id).
  // Repointing the screen index is the critical world fix. The sibling map16/
  // scroll/coord/camera columns of the bird-travel row are otherwise
  // world-mirror-INVARIANT by construction: compile_resources.py builds them as
  // `t['scroll_xy']/xy/camera_xy + base`, where
  // base_x=(screen&7)<<9, base_y=(screen&56)<<6 — bit 0x40 (the world bit) does
  // not overlap &7 or &56, so a screen and its +0x40 mirror share identical
  // base/coord/scroll values, and kBirdTravel_Map16LoadSrcOff (get_loadoffs) is a
  // pure function of scroll/load coords with no screen-index term. The only
  // per-world differences (sprite gfx, tile theme 0x20/0x21, misc gfx) are
  // re-derived from the OVERRIDDEN overworld_screen_index inside
  // Overworld_LoadGFXAndScreenSize at flute-load time, so no row recompute is
  // needed here. (Do not "fix" this by overriding the full row with unverified
  // DW values — that would diverge from the correct mirror framing.) The one
  // exception is ALTTPR's explicit Kakariko landing nudge below: Rom.php writes
  // the raw Y column first (0x02E8D5), then X (0x02E8F7), both for slot 2.
  // Slot 8 (vanilla 0x5B = DW pyramid) maps to 0x1B for the hole-only Ganon
  // relocation (ALTTPR Rom.php:1600, the 9th flute spot = 0x001B). 0x1B is the LW
  // mirror of 0x5B (differs only by the 0x40 world bit), so the screen-index-only
  // override is valid here exactly as for slots 0-7. This is also the practical way
  // to REACH the relocated Ganon in normal play (the DW->LW under-rock warps are
  // still playtest-pending/incomplete): flute to slot 8 -> arrive at LW Hyrule
  // Castle -> fall into the pit -> Ganon.
  { kAsset_BirdTravel_ScreenIndex, 2, 0, 0x0043 },  // ToH NW      0x03->0x43
  { kAsset_BirdTravel_ScreenIndex, 2, 1, 0x0056 },  // Coven       0x16->0x56
  { kAsset_BirdTravel_ScreenIndex, 2, 2, 0x0058 },  // Kakariko    0x18->0x58
  { kAsset_BirdTravel_ScreenIndex, 2, 3, 0x006C },  // Uncle's E.  0x2C->0x6C
  { kAsset_BirdTravel_ScreenIndex, 2, 4, 0x006F },  //             0x2F->0x6F
  { kAsset_BirdTravel_ScreenIndex, 2, 5, 0x0070 },  //             0x30->0x70
  { kAsset_BirdTravel_ScreenIndex, 2, 6, 0x007B },  //             0x3B->0x7B
  { kAsset_BirdTravel_ScreenIndex, 2, 7, 0x007F },  // Desert      0x3F->0x7F
  { kAsset_BirdTravel_ScreenIndex, 2, 8, 0x001B },  // Ganon/HC    0x5B->0x1B
  { kAsset_BirdTravel_LinkXCoord,  2, 2, 0x01F8 },  // Kakariko statue nudge
  { kAsset_BirdTravel_LinkYCoord,  2, 2, 0x07C8 },
};
#define kInvertedOverrideCount \
    (sizeof(kInvertedOverrides) / sizeof(kInvertedOverrides[0]))

// ---------------------------------------------------------------------------
// Shadow buffers. One static buffer per touched asset array. Sized generously;
// kShadowMaxBytes guards copies against an unexpectedly large asset. Each shadow
// has a saved "original" g_asset_ptrs[] pointer (NULL when not installed) used
// by Teardown.
#define kShadowMaxBytes 4096

typedef struct InvertedShadow {
  uint8        asset_index;          // which g_asset_ptrs[] slot this owns
  uint8        buf[kShadowMaxBytes]; // copy of the vanilla array + overrides
  const uint8 *orig;                 // saved g_asset_ptrs[idx] while installed
} InvertedShadow;

// One row per DISTINCT asset index in kInvertedOverrides. If you add a swap that
// touches a new asset array, add its index here too.
static InvertedShadow g_shadows[] = {
  { kAsset_Overworld_Entrance_Id, {0}, NULL },
  { kAsset_ExitData_ScreenIndex,  {0}, NULL },
  { kAsset_ExitDataRooms,         {0}, NULL },
  { kAsset_ExitData_Map16,        {0}, NULL },
  { kAsset_ExitData_ScrollX,      {0}, NULL },
  { kAsset_ExitData_ScrollY,      {0}, NULL },
  { kAsset_ExitData_XCoord,       {0}, NULL },
  { kAsset_ExitData_YCoord,       {0}, NULL },
  { kAsset_ExitData_CameraX,      {0}, NULL },
  { kAsset_ExitData_CameraY,      {0}, NULL },
  { kAsset_ExitData_NormalDoor,   {0}, NULL },
  { kAsset_ExitData_FancyDoor,    {0}, NULL },
  { kAsset_ExitData_Unk1,         {0}, NULL },
  { kAsset_ExitData_Unk3,         {0}, NULL },
  { kAsset_BirdTravel_ScreenIndex, {0}, NULL },
  { kAsset_BirdTravel_LinkXCoord,  {0}, NULL },
  { kAsset_BirdTravel_LinkYCoord,  {0}, NULL },
};
#define kInvertedShadowCount (sizeof(g_shadows) / sizeof(g_shadows[0]))

static InvertedShadow *FindShadow(uint8 asset_index) {
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    if (g_shadows[i].asset_index == asset_index) return &g_shadows[i];
  }
  return NULL;
}

void InvertedEntrances_Teardown(void) {
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    if (g_shadows[i].orig != NULL) {
      g_asset_ptrs[g_shadows[i].asset_index] = (void *)g_shadows[i].orig;
      g_shadows[i].orig = NULL;
    }
  }
}

// See header — the saved pristine asset-126 pointer while installed, else NULL.
// Install captures it AFTER Entrance_RuntimeTeardown has run (activation order
// in Rando_ActivateSidecarSlot), so it is always the raw vanilla table, never
// another subsystem's shadow.
const uint8 *InvertedEntrances_SavedEntranceIdOrig(void) {
  InvertedShadow *s = FindShadow(kAsset_Overworld_Entrance_Id);
  return s != NULL ? s->orig : NULL;
}

// See header — test support: enumerate the shadowed asset indices.
int InvertedEntrances_ShadowedAssets(uint8 *out, int cap) {
  for (int i = 0; i < (int)kInvertedShadowCount && i < cap; i++)
    out[i] = g_shadows[i].asset_index;
  return (int)kInvertedShadowCount;
}

void InvertedEntrances_Install(uint8 world_state) {
  // Tear down any prior install first, so a slot-switch without an intervening
  // Teardown is safe (mirrors Entrance_RuntimeInstall's leading Teardown).
  InvertedEntrances_Teardown();

  // No-op for every non-Inverted world state — non-inverted/non-rando stay
  // byte-identical (the asset pointers are never touched).
  if (world_state != (uint8)kWorldState_Inverted) return;

  // Pass 1: copy each touched vanilla array into its shadow. Guard against a
  // NULL or oversize source (same defensive checks as rando.c's overlay). If
  // ANY required shadow can't be prepared, abort the whole install (the
  // already-restored vanilla pointers stay live) so we never apply a partial
  // override.
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    InvertedShadow *s = &g_shadows[i];
    const uint8 *src = (const uint8 *)g_asset_ptrs[s->asset_index];
    uint32 size = g_asset_sizes[s->asset_index];
    if (src == NULL || size == 0 || size > kShadowMaxBytes) {
      // Abort the whole install rather than apply a partial override. The
      // leading Teardown already restored any prior install, so the live asset
      // pointers are still vanilla here (Pass 3 is what repoints) — this
      // Teardown is a defensive no-op. Warn on the oversize case so a future
      // maintainer who registers a larger asset notices the silent fallback.
      if (size > kShadowMaxBytes)
        fprintf(stderr,
                "InvertedEntrances: asset %u (%u bytes) exceeds shadow cap %u; "
                "inverted entrance override skipped.\n",
                (unsigned)s->asset_index, (unsigned)size,
                (unsigned)kShadowMaxBytes);
      InvertedEntrances_Teardown();
      return;
    }
    memcpy(s->buf, src, size);
  }

  // Pass 2: apply each override into its shadow buffer. Bounds-check every
  // element against the live asset size before writing.
  for (uint32 i = 0; i < kInvertedOverrideCount; i++) {
    const InvertedOverride *o = &kInvertedOverrides[i];
    InvertedShadow *s = FindShadow(o->asset_index);
    if (s == NULL) continue;  // override references an unregistered shadow
    uint32 size = g_asset_sizes[o->asset_index];
    if (o->width == 2) {
      // 2-byte element: byte offset = elem_index * 2; need 2 bytes in range.
      uint32 byte_off = (uint32)o->elem_index * 2u;
      if (byte_off + 2u > size) continue;
      // Little-endian store, matching the byte order the game reads uint16 from.
      s->buf[byte_off + 0] = (uint8)(o->value & 0xFF);
      s->buf[byte_off + 1] = (uint8)(o->value >> 8);
    } else {
      if (o->elem_index >= size) continue;
      s->buf[o->elem_index] = (uint8)(o->value & 0xFF);
    }
  }

  // Pass 3: save originals + repoint. Done last so an early abort above never
  // leaves a dangling repoint.
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    InvertedShadow *s = &g_shadows[i];
    s->orig = (const uint8 *)g_asset_ptrs[s->asset_index];
    g_asset_ptrs[s->asset_index] = s->buf;
  }
}

static void InvertedEntrances_SelfCheckDie(const char *msg) {
  InvertedEntrances_Teardown();
  fprintf(stderr, "InvertedEntrances_SelfCheck: %s\n", msg);
  exit(2);
}

static uint16 inv_sc_u16(uint8 asset, uint16 elem) {
  return ((const uint16 *)g_asset_ptrs[asset])[elem];
}

void InvertedEntrances_SelfCheck(void) {
  static const uint16 kExpectedFluteScreens[9] = {
    0x0043, 0x0056, 0x0058, 0x006C, 0x006F, 0x0070, 0x007B, 0x007F, 0x001B
  };
  enum { kSynthSize = 0x200 };
  static uint8 synth[kInvertedShadowCount][kSynthSize];
  const uint8 *orig_ptr[kInvertedShadowCount];
  uint32 orig_size[kInvertedShadowCount];
  bool used_synth[kInvertedShadowCount] = { false };

  InvertedEntrances_Teardown();
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    uint8 idx = g_shadows[i].asset_index;
    orig_ptr[i] = g_asset_ptrs[idx];
    orig_size[i] = g_asset_sizes[idx];
    if (g_asset_ptrs[idx] == NULL || g_asset_sizes[idx] == 0) {
      memset(synth[i], 0, sizeof(synth[i]));
      g_asset_ptrs[idx] = synth[i];
      g_asset_sizes[idx] = kSynthSize;
      used_synth[i] = true;
    }
  }

  InvertedEntrances_Install((uint8)kWorldState_Inverted);
  if (InvertedEntrances_SavedEntranceIdOrig() == NULL)
    InvertedEntrances_SelfCheckDie("install did not take");
  if (((const uint8 *)g_asset_ptrs[kAsset_Overworld_Entrance_Id])[0x6F] != 0x07)
    InvertedEntrances_SelfCheckDie("Dark DM healer fairy door did not load Old Man Cave East");
  if (((const uint8 *)g_asset_ptrs[kAsset_ExitData_ScreenIndex])[0x18] != 0x43)
    InvertedEntrances_SelfCheckDie("Old Man Cave East exit screen is not DW Death Mountain");
  if (inv_sc_u16(kAsset_ExitDataRooms, 0x18) != 0x00F1 ||
      inv_sc_u16(kAsset_ExitData_Map16, 0x18) != 0x1400 ||
      inv_sc_u16(kAsset_ExitData_ScrollX, 0x18) != 0x0600 ||
      inv_sc_u16(kAsset_ExitData_ScrollY, 0x18) != 0x0294 ||
      inv_sc_u16(kAsset_ExitData_XCoord, 0x18) != 0x0678 ||
      inv_sc_u16(kAsset_ExitData_YCoord, 0x18) != 0x02E8 ||
      inv_sc_u16(kAsset_ExitData_CameraX, 0x18) != 0x0685 ||
      inv_sc_u16(kAsset_ExitData_CameraY, 0x18) != 0x0303 ||
      inv_sc_u16(kAsset_ExitData_NormalDoor, 0x18) != 0x0000 ||
      inv_sc_u16(kAsset_ExitData_FancyDoor, 0x18) != 0x0000 ||
      ((const uint8 *)g_asset_ptrs[kAsset_ExitData_Unk1])[0x18] != 0x0A ||
      ((const uint8 *)g_asset_ptrs[kAsset_ExitData_Unk3])[0x18] != 0xF6)
    InvertedEntrances_SelfCheckDie("Old Man Cave East exit row mismatch");
  for (uint32 i = 0; i < 9; i++) {
    if (inv_sc_u16(kAsset_BirdTravel_ScreenIndex, (uint16)i) != kExpectedFluteScreens[i])
      InvertedEntrances_SelfCheckDie("Inverted flute screen row mismatch");
  }
  if (inv_sc_u16(kAsset_BirdTravel_LinkXCoord, 2) != 0x01F8 ||
      inv_sc_u16(kAsset_BirdTravel_LinkYCoord, 2) != 0x07C8)
    InvertedEntrances_SelfCheckDie("Inverted Kakariko flute nudge mismatch");

  InvertedEntrances_Teardown();
  for (uint32 i = 0; i < kInvertedShadowCount; i++) {
    if (used_synth[i]) {
      uint8 idx = g_shadows[i].asset_index;
      g_asset_ptrs[idx] = orig_ptr[i];
      g_asset_sizes[idx] = orig_size[i];
    }
  }
  fprintf(stderr, "[InvertedEntrances_SelfCheck] OK\n");
}

// ===========================================================================
// Inverted overworld DW->LW warps (the way OUT of the Dark World).
//
// In Inverted, the Magic Mirror only carries LW->DW (the trip home); the way
// from the DW out to the LW is a set of fixed under-rock "world-warp" tiles —
// overworld-secret records of type 0x82. Vanilla has ZERO type-0x82 warps, so
// this is a purely-additive port: we add one warp record to each listed DW
// screen. A 0x82 warp flips the world at the SAME overworld position
// (Module09_MirrorWarp toggles savegame_is_darkworld ^= 0x40), so no
// destination data is needed.
//
// The warp tile positions are from ALTTPR's MIT-licensed Rom.php
// setInvertedMode() — the "Add warps under rocks, etc." block (lines ~1887-1919),
// the same source as the entrance overrides above. Each PHP write swaps an
// item-under-bush for (or appends) a type-0x82 warp on a named map; the table
// rows carry the PHP map label + line. Death Mountain (0x4E) and Misery Mire
// (0x78) are large areas whose overworld-secret list spans two sub-screens, so
// the warp is added to both (0x4D/0x4E, 0x70/0x78) to cover whichever screen
// index is live when the player lifts the rock.
//
// Unlike the fixed-width element pokes above, the secret blob is a packed
// variable-length per-screen record list (3 bytes {pos_lo,pos_hi,type},
// 0xffff-terminated) indexed by kOverworldSecrets_Offs[screen]. To add a record
// we rebuild: copy the vanilla blob, APPEND a new list (the screen's vanilla
// records + the warp + terminator) for each touched screen, and repoint that
// screen's _Offs entry at the appended list. Untouched screens keep their
// original offsets into the copied blob.
enum {
  kAsset_OverworldSecrets_Offs = 157,  // uint16[0x80]: screen -> byte offset
  kAsset_OverworldSecrets      = 158,  // uint8[]:      packed 3-byte records
};

static const struct {
  uint8  screen;     // overworld_screen_index (all Dark World, >= 0x40)
  uint8  count;      // number of warp positions on this screen
  uint16 pos[2];     // warp tile position(s)
} kInvertedSecretWarps[] = {
  { 0x4D, 1, { 0x1D4A } },          // "map 78 (DM)" Rom.php:1890 (shared w/ 0x4E)
  { 0x4E, 1, { 0x1D4A } },
  { 0x50, 1, { 0x0B2E } },          // "map 80 (top of kak)" Rom.php:1888
  { 0x6F, 1, { 0x0BB2 } },          // "map 111" Rom.php:1891
  { 0x70, 1, { 0x1D94 } },          // "map 120 (mire)" Rom.php:1889 (shared w/ 0x78)
  { 0x78, 1, { 0x1D94 } },
  { 0x73, 1, { 0x02A8 } },          // "map115" Rom.php:1893
  { 0x75, 1, { 0x0F50 } },          // Rom.php:1894
  { 0x47, 2, { 0x069E, 0x06A4 } },  // "map 71" (HC ledge / pyramid) Rom.php:1919
};
#define kInvertedSecretWarpCount \
  (sizeof(kInvertedSecretWarps) / sizeof(kInvertedSecretWarps[0]))

// Shadow buffers for the two secret assets. Sized generously; vanilla packs in
// ~1.4 KB and the appends add a few hundred bytes.
#define kSecretsDataMax 4096
static uint8  g_inv_secrets_data[kSecretsDataMax];
static uint16 g_inv_secrets_offs[0x80];
static const uint8 *g_inv_secrets_data_orig = NULL;  // saved g_asset_ptrs[158]
static const uint8 *g_inv_secrets_offs_orig = NULL;  // saved g_asset_ptrs[157]

void InvertedSecrets_Teardown(void) {
  if (g_inv_secrets_offs_orig != NULL) {
    g_asset_ptrs[kAsset_OverworldSecrets_Offs] = (void *)g_inv_secrets_offs_orig;
    g_inv_secrets_offs_orig = NULL;
  }
  if (g_inv_secrets_data_orig != NULL) {
    g_asset_ptrs[kAsset_OverworldSecrets] = (void *)g_inv_secrets_data_orig;
    g_inv_secrets_data_orig = NULL;
  }
}

void InvertedSecrets_Install(uint8 world_state) {
  InvertedSecrets_Teardown();
  if (world_state != (uint8)kWorldState_Inverted) return;

  const uint8  *odata = (const uint8 *)g_asset_ptrs[kAsset_OverworldSecrets];
  const uint16 *ooffs = (const uint16 *)g_asset_ptrs[kAsset_OverworldSecrets_Offs];
  uint32 dsize = g_asset_sizes[kAsset_OverworldSecrets];
  uint32 osize = g_asset_sizes[kAsset_OverworldSecrets_Offs];
  if (odata == NULL || ooffs == NULL || dsize == 0 ||
      dsize > kSecretsDataMax || osize < 0x80 * 2) {
    if (dsize > kSecretsDataMax)
      fprintf(stderr,
              "InvertedSecrets: kOverworldSecrets (%u bytes) exceeds cap %u; "
              "inverted warps skipped.\n",
              (unsigned)dsize, (unsigned)kSecretsDataMax);
    return;
  }

  // Copy vanilla blob + offsets, then append a new list per touched screen.
  memcpy(g_inv_secrets_data, odata, dsize);
  memcpy(g_inv_secrets_offs, ooffs, 0x80 * sizeof(uint16));
  uint32 ap = dsize;  // append cursor

  for (uint32 i = 0; i < kInvertedSecretWarpCount; i++) {
    uint8 scr = kInvertedSecretWarps[i].screen;
    uint32 list_start = ap;
    // Copy the screen's existing vanilla records (everything before 0xffff).
    const uint8 *src = odata + ooffs[scr];
    while (!(src[0] == 0xff && src[1] == 0xff)) {
      if (ap + 5 > kSecretsDataMax) return;  // leave room for a warp + term
      g_inv_secrets_data[ap + 0] = src[0];
      g_inv_secrets_data[ap + 1] = src[1];
      g_inv_secrets_data[ap + 2] = src[2];
      ap += 3; src += 3;
    }
    // Append this screen's warp record(s).
    for (uint32 j = 0; j < kInvertedSecretWarps[i].count; j++) {
      if (ap + 5 > kSecretsDataMax) return;
      uint16 p = kInvertedSecretWarps[i].pos[j];
      g_inv_secrets_data[ap + 0] = (uint8)(p & 0xff);
      g_inv_secrets_data[ap + 1] = (uint8)(p >> 8);
      g_inv_secrets_data[ap + 2] = 0x82;  // world-warp
      ap += 3;
    }
    g_inv_secrets_data[ap + 0] = 0xff;
    g_inv_secrets_data[ap + 1] = 0xff;
    ap += 2;
    g_inv_secrets_offs[scr] = (uint16)list_start;
  }

  // Repoint last, after all appends succeeded.
  g_inv_secrets_offs_orig = (const uint8 *)g_asset_ptrs[kAsset_OverworldSecrets_Offs];
  g_asset_ptrs[kAsset_OverworldSecrets_Offs] = (uint8 *)g_inv_secrets_offs;
  g_inv_secrets_data_orig = (const uint8 *)g_asset_ptrs[kAsset_OverworldSecrets];
  g_asset_ptrs[kAsset_OverworldSecrets] = g_inv_secrets_data;
}
