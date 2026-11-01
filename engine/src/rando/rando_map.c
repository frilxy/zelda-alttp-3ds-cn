// rando_map.c — decode the overworld map graphic to RGBA. See rando_map.h.
#include "rando_map.h"
#include "../assets.h"

#include <stdio.h>
#include <string.h>

bool RandoMap_Decode(bool dark, uint8 *out) {
  const uint8 *gfx = kOverworldMapGfx;             // 256 tiles * 64 bytes, 8bpp
  const uint16 *pal = kOverworldMapPaletteData;    // 256 BGR555
  if (gfx == NULL || pal == NULL || out == NULL) return false;

  // Reconstruct the 64x64 tile-index grid the Mode-7 map renders. BOTH worlds
  // use the light tilemap as the 64x64 base; the dark world then overlays its
  // 32x32 data onto the CENTER (the in-game dark map runs the light loader
  // first, then NMI_UploadDarkWorldMap writes the dark block at VRAM dst 0x810 =
  // row 16, col 16). The light-tile border around the dark center is faithful to
  // the game (messaging.c:1129/1144, nmi.c:327/393).
  static uint8 grid[64][64];
  memset(grid, 0, sizeof(grid));
  {
    // Light base: kLightOverworldTilemap is quadrant-major (4 x 32x32) into the
    // four corners of the 64x64 grid (NMI_UpdateLoadLightWorldMap, nmi.c:327).
    const uint8 *src = kLightOverworldTilemap;
    if (src == NULL) return false;
    static const int qcol[4] = {0, 32, 0, 32};
    static const int qrow[4] = {0, 0, 32, 32};
    for (int q = 0; q < 4; q++)
      for (int r = 0; r < 32; r++)
        for (int c = 0; c < 32; c++)
          grid[qrow[q] + r][qcol[q] + c] = src[q * 1024 + r * 32 + c];
  }
  if (dark) {
    const uint8 *src = kDarkOverworldTilemap;
    if (src == NULL) return false;
    for (int r = 0; r < 32; r++)
      for (int c = 0; c < 32; c++)
        grid[16 + r][16 + c] = src[r * 32 + c];  // centered overlay
  }

  // The Mode-7 tile gfx is shared between worlds; the world is selected by the
  // 128-color palette block (light 0x00..0x7F, dark 0x80..0xFF). Pixel indices
  // run 0..127 within the selected block.
  int pal_base = dark ? 0x80 : 0;
  for (int py = 0; py < kRandoMapPixels; py++) {
    for (int px = 0; px < kRandoMapPixels; px++) {
      uint8 tile = grid[py >> 3][px >> 3];
      uint8 idx = gfx[tile * 64 + (py & 7) * 8 + (px & 7)];
      uint16 c = pal[pal_base + (idx & 0x7f)];  // BGR555
      uint8 *o = out + ((size_t)py * kRandoMapPixels + px) * 4;
      o[0] = (uint8)((c & 0x1f) << 3);          // R
      o[1] = (uint8)(((c >> 5) & 0x1f) << 3);   // G
      o[2] = (uint8)(((c >> 10) & 0x1f) << 3);  // B
      o[3] = 255;
    }
  }
  return true;
}

static bool write_ppm(const char *path, const uint8 *rgba) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return false;
  fprintf(f, "P6\n%d %d\n255\n", kRandoMapPixels, kRandoMapPixels);
  for (int i = 0; i < kRandoMapPixels * kRandoMapPixels; i++)
    fwrite(rgba + (size_t)i * 4, 1, 3, f);  // RGB (drop alpha)
  fclose(f);
  return true;
}

bool RandoMap_DumpPpm(const char *prefix) {
  static uint8 buf[kRandoMapPixels * kRandoMapPixels * 4];
  char path[512];
  if (!RandoMap_Decode(false, buf)) return false;
  snprintf(path, sizeof(path), "%s_light.ppm", prefix);
  if (!write_ppm(path, buf)) return false;
  if (!RandoMap_Decode(true, buf)) return false;
  snprintf(path, sizeof(path), "%s_dark.ppm", prefix);
  if (!write_ppm(path, buf)) return false;
  return true;
}
