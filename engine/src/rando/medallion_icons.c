#include "medallion_icons.h"

#include <stdio.h>
#include <string.h>

#include "item_ids.h"
#include "rando.h"
#include "../features.h"
#include "../variables.h"

// embedded-data-guard: allow sparse MIT-attributed medallion plaque art deltas;
// source stays beside the runtime patcher and Rando_MedallionIcons_SelfCheck.

enum {
  kMedallionBgGfxSize = 0x600,
  kMedallionEntranceOpenBit = 0x20,

  kMireScreen = 0x70,
  kMireBgGfxPack = 50,

  kTurtleRockScreen = 0x47,
  kTurtleRockBgGfxPack = 96,

  kBgTilesPerSheet = 64,
  kBytesPer3bppTile = 24,

  kMireSourceChar0 = 0x16c,
  kMireSourceChar1 = 0x17c,
  kMireTargetChar0 = 0x16e,
  kMireTargetChar1 = 0x17e,

  kTurtleRockSourceChar0 = 0x11e,
  kTurtleRockSourceChar1 = 0x11f,
  kTurtleRockTargetChar0 = 0x11a,
  kTurtleRockTargetChar1 = 0x11b,
};

typedef struct MedallionGfxPatchByte {
  uint16 offset;
  uint8 value;
} MedallionGfxPatchByte;

typedef struct MedallionTileRemap {
  uint8 source_tile;
  uint8 target_tile;
} MedallionTileRemap;

// Sparse decompressed BG-sheet deltas derived from MIT z3randomizer art blobs:
// data/99ff1_bombos.gfx, data/99ff1_quake.gfx, data/a6fc4_bombos.gfx, and
// data/a6fc4_ether.gfx. The ROM patcher swaps compressed sheet pointers; this
// port copies the patched source tiles into unused chars, then rewrites only the
// medallion plaque's map8 words. That preserves the upstream art while avoiding
// broad Turtle Rock terrain changes from patching shared chars 0x11e/0x11f.
// See assets/rando/MEDALLION_GFX_DERIVATION.md.
static const MedallionGfxPatchByte kMireBombosPatch[] = {
  {0x42b, 0x80}, {0x42d, 0x60}, {0x42f, 0x38},
  {0x435, 0x7f}, {0x436, 0x9f}, {0x437, 0xc7},
  {0x5a5, 0x00}, {0x5a7, 0x09}, {0x5a9, 0x19}, {0x5ab, 0x1b},
  {0x5ad, 0x3a}, {0x5af, 0x3c},
  {0x5b2, 0xff}, {0x5b3, 0xf6}, {0x5b4, 0xe6}, {0x5b5, 0xe4},
  {0x5b6, 0xc5}, {0x5b7, 0xc3},
};

static const MedallionGfxPatchByte kMireQuakePatch[] = {
  {0x42d, 0xec}, {0x42f, 0xc4},
  {0x436, 0x13}, {0x437, 0x3b},
  {0x5a5, 0x03}, {0x5a7, 0x0f}, {0x5a9, 0x1f}, {0x5ab, 0x1f},
  {0x5ad, 0x39}, {0x5af, 0x30},
  {0x5b2, 0xfc}, {0x5b3, 0xf0}, {0x5b4, 0xe0}, {0x5b5, 0xe0},
  {0x5b6, 0xc6}, {0x5b7, 0xcf},
};

static const MedallionGfxPatchByte kTurtleRockBombosPatch[] = {
  {0x2d4, 0xff}, {0x2d5, 0x60}, {0x2d6, 0xf6}, {0x2d7, 0x49},
  {0x2d8, 0xe6}, {0x2d9, 0x19}, {0x2da, 0xe4}, {0x2db, 0x1b},
  {0x2dc, 0xc5}, {0x2dd, 0x3a}, {0x2de, 0xc3}, {0x2df, 0x3c},
  {0x2e2, 0x0f}, {0x2e3, 0x16}, {0x2e4, 0x26}, {0x2e5, 0x24},
  {0x2e6, 0x45}, {0x2e7, 0x43},
  {0x2f2, 0x7f}, {0x2f3, 0x80}, {0x2f4, 0x9f}, {0x2f5, 0x60},
  {0x2f6, 0xc7}, {0x2f7, 0x38},
  {0x2fd, 0x7c}, {0x2fe, 0x9e}, {0x2ff, 0xc6},
};

static const MedallionGfxPatchByte kTurtleRockEtherPatch[] = {
  {0x2d4, 0xfd}, {0x2d5, 0x62}, {0x2d6, 0xf1}, {0x2d7, 0x4e},
  {0x2d8, 0xe3}, {0x2d9, 0x1c}, {0x2da, 0xe3}, {0x2db, 0x1c},
  {0x2dc, 0xc7}, {0x2dd, 0x38}, {0x2de, 0xc7}, {0x2df, 0x38},
  {0x2e2, 0x0d}, {0x2e3, 0x11}, {0x2e4, 0x23}, {0x2e5, 0x23},
  {0x2e6, 0x47}, {0x2e7, 0x47},
  {0x2f4, 0x03}, {0x2f5, 0xfc}, {0x2f6, 0xf3}, {0x2f7, 0x0c},
  {0x2fe, 0x02}, {0x2ff, 0xf2},
};

static const MedallionTileRemap kMireTileRemaps[] = {
  {44, 46},  // 0x16c -> 0x16e
  {60, 62},  // 0x17c -> 0x17e
};

static const MedallionTileRemap kTurtleRockTileRemaps[] = {
  {30, 26},  // 0x11e -> 0x11a
  {31, 27},  // 0x11f -> 0x11b
};

static bool is_overworld_gfx_module(void) {
  return main_module_index == 8 || main_module_index == 9 ||
         main_module_index == 11 || main_module_index == 16;
}

static int remap_target_tile(const MedallionTileRemap *remaps, size_t count, int source_tile) {
  for (size_t i = 0; i < count; i++) {
    if (remaps[i].source_tile == source_tile)
      return remaps[i].target_tile;
  }
  return -1;
}

static void copy_remapped_tiles(uint8 *dst, const MedallionTileRemap *remaps, size_t count) {
  for (size_t i = 0; i < count; i++) {
    memcpy(dst + remaps[i].target_tile * kBytesPer3bppTile,
           dst + remaps[i].source_tile * kBytesPer3bppTile,
           kBytesPer3bppTile);
  }
}

static void apply_patch_bytes(uint8 *dst, const MedallionGfxPatchByte *patch, size_t patch_count,
                              const MedallionTileRemap *remaps, size_t remap_count) {
  copy_remapped_tiles(dst, remaps, remap_count);
  for (size_t i = 0; i < patch_count; i++) {
    int source_tile = patch[i].offset / kBytesPer3bppTile;
    int target_tile = remap_target_tile(remaps, remap_count, source_tile);
    if (target_tile < 0)
      continue;  // Structural self-check catches this for the static tables.
    dst[target_tile * kBytesPer3bppTile + patch[i].offset % kBytesPer3bppTile] = patch[i].value;
  }
}

void Rando_PatchMedallionEntranceBgGfx(uint8 *dst, int gfx_pack, int len) {
  if (len < kMedallionBgGfxSize ||
      (enhanced_features1 & kFeatures1_RandomizerActive) == 0 ||
      !is_overworld_gfx_module())
    return;

  const uint8 *assignment = Rando_GetMedallionAssignment();
  if (assignment == NULL)
    return;

  uint8 screen = (uint8)overworld_screen_index;
  if (screen == kMireScreen) {
    if (gfx_pack != kMireBgGfxPack ||
        (save_ow_event_info[kMireScreen] & kMedallionEntranceOpenBit))
      return;

    if (assignment[0] == ITEM_Bombos) {
      apply_patch_bytes(dst, kMireBombosPatch, countof(kMireBombosPatch),
                        kMireTileRemaps, countof(kMireTileRemaps));
    } else if (assignment[0] == ITEM_Quake) {
      apply_patch_bytes(dst, kMireQuakePatch, countof(kMireQuakePatch),
                        kMireTileRemaps, countof(kMireTileRemaps));
    }
  } else if (screen == kTurtleRockScreen) {
    if (gfx_pack != kTurtleRockBgGfxPack ||
        (save_ow_event_info[kTurtleRockScreen] & kMedallionEntranceOpenBit))
      return;

    if (assignment[1] == ITEM_Bombos) {
      apply_patch_bytes(dst, kTurtleRockBombosPatch, countof(kTurtleRockBombosPatch),
                        kTurtleRockTileRemaps, countof(kTurtleRockTileRemaps));
    } else if (assignment[1] == ITEM_Ether) {
      apply_patch_bytes(dst, kTurtleRockEtherPatch, countof(kTurtleRockEtherPatch),
                        kTurtleRockTileRemaps, countof(kTurtleRockTileRemaps));
    }
  }
}

static void replace_map8_char(uint16 map8_words[4], uint16 source_char, uint16 target_char) {
  for (size_t i = 0; i < 4; i++) {
    if ((map8_words[i] & 0x3ff) == source_char)
      map8_words[i] = (map8_words[i] & ~0x3ff) | target_char;
  }
}

static bool is_mire_icon_map16(uint16 map16_id) {
  return map16_id == 0x0b3b || map16_id == 0x0b3c ||
         map16_id == 0x0b43 || map16_id == 0x0b44;
}

static bool is_turtle_rock_icon_map16(uint16 map16_id) {
  return map16_id == 0x0929 || map16_id == 0x092a;
}

void Rando_PatchMedallionEntranceMap8(uint16 map16_id, uint16 map8_words[4]) {
  if ((enhanced_features1 & kFeatures1_RandomizerActive) == 0 ||
      !is_overworld_gfx_module())
    return;

  const uint8 *assignment = Rando_GetMedallionAssignment();
  if (assignment == NULL)
    return;

  uint8 screen = (uint8)overworld_screen_index;
  if (screen == kMireScreen) {
    if ((save_ow_event_info[kMireScreen] & kMedallionEntranceOpenBit) ||
        (assignment[0] != ITEM_Bombos && assignment[0] != ITEM_Quake) ||
        !is_mire_icon_map16(map16_id))
      return;
    replace_map8_char(map8_words, kMireSourceChar0, kMireTargetChar0);
    replace_map8_char(map8_words, kMireSourceChar1, kMireTargetChar1);
  } else if (screen == kTurtleRockScreen) {
    if ((save_ow_event_info[kTurtleRockScreen] & kMedallionEntranceOpenBit) ||
        (assignment[1] != ITEM_Bombos && assignment[1] != ITEM_Ether) ||
        !is_turtle_rock_icon_map16(map16_id))
      return;
    replace_map8_char(map8_words, kTurtleRockSourceChar0, kTurtleRockTargetChar0);
    replace_map8_char(map8_words, kTurtleRockSourceChar1, kTurtleRockTargetChar1);
  }
}

static void selfcheck_fail(const char *what) {
  fprintf(stderr, "[Rando_MedallionIcons_SelfCheck] FAILED: %s\n", what);
  exit(2);
}

static void check_patch_count(size_t actual, size_t expected, const char *what) {
  if (actual != expected)
    selfcheck_fail(what);
}

static void check_tile_remaps(const MedallionTileRemap *remaps, size_t count, const char *what) {
  if (count == 0)
    selfcheck_fail(what);
  for (size_t i = 0; i < count; i++) {
    if (remaps[i].source_tile >= kBgTilesPerSheet ||
        remaps[i].target_tile >= kBgTilesPerSheet ||
        remaps[i].source_tile == remaps[i].target_tile)
      selfcheck_fail(what);
  }
}

static void check_patch_table(const MedallionGfxPatchByte *patch, size_t count,
                              const MedallionTileRemap *remaps, size_t remap_count,
                              const char *what) {
  if (count == 0)
    selfcheck_fail(what);

  for (size_t i = 0; i < count; i++) {
    if (patch[i].offset >= kMedallionBgGfxSize)
      selfcheck_fail(what);
    if (remap_target_tile(remaps, remap_count, patch[i].offset / kBytesPer3bppTile) < 0)
      selfcheck_fail(what);
    if (i != 0 && patch[i - 1].offset >= patch[i].offset)
      selfcheck_fail(what);
  }
}

void Rando_MedallionIcons_SelfCheck(void) {
  check_patch_count(countof(kMireBombosPatch), 18, "Mire Bombos patch count");
  check_patch_count(countof(kMireQuakePatch), 16, "Mire Quake patch count");
  check_patch_count(countof(kTurtleRockBombosPatch), 27, "Turtle Rock Bombos patch count");
  check_patch_count(countof(kTurtleRockEtherPatch), 24, "Turtle Rock Ether patch count");

  check_tile_remaps(kMireTileRemaps, countof(kMireTileRemaps), "Mire tile remaps");
  check_tile_remaps(kTurtleRockTileRemaps, countof(kTurtleRockTileRemaps), "Turtle Rock tile remaps");

  check_patch_table(kMireBombosPatch, countof(kMireBombosPatch),
                    kMireTileRemaps, countof(kMireTileRemaps), "Mire Bombos patch table");
  check_patch_table(kMireQuakePatch, countof(kMireQuakePatch),
                    kMireTileRemaps, countof(kMireTileRemaps), "Mire Quake patch table");
  check_patch_table(kTurtleRockBombosPatch, countof(kTurtleRockBombosPatch),
                    kTurtleRockTileRemaps, countof(kTurtleRockTileRemaps),
                    "Turtle Rock Bombos patch table");
  check_patch_table(kTurtleRockEtherPatch, countof(kTurtleRockEtherPatch),
                    kTurtleRockTileRemaps, countof(kTurtleRockTileRemaps),
                    "Turtle Rock Ether patch table");
  fprintf(stderr, "[Rando_MedallionIcons_SelfCheck] OK\n");
}
