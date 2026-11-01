#include "assets_fr_c.h"

#include <stdlib.h>
#include <string.h>

#include "assets.h"

// The French Canadian cartridge is its own build of the game. Next to the USA
// one it moves a few overworld tiles and adds four, and its message list lacks
// message 4 (the item select's digits, which the dialogue extraction puts
// back), so its later messages are numbered one lower. Everything else the assets hold is the same data, taken from it
// as it is: its maps keep their own map32 numbers, which nothing but the maps
// uses.

enum {
  kMap16Count = 3752,
  kMap32Groups = 2221,     // the cartridge's map32 tables, 4 entries per group
  kTileAttrSize = 3824,
  kTeleMsgSize = 640,
  kSignTextSize = 256,
  kAssetTeleMsg = 9,
  kAssetMap32 = 60,        // kMap32ToMap16_0..3
  kAssetMap16 = 70,
  kAssetSignText = 110,
  kAssetTileAttr = 164,
};

// USA map16 number -> the cartridge's, for the numbers that differ: runs of
// { first USA number, last, first cartridge number }. Found by laying the
// overworld maps of the two cartridges over each other, and checked tile by
// tile: every pair has the same 8x8 tiles and the same attribute, except the
// compass tiles 1056 and 1085, drawn as on the cartridge, and 2468 and
// 3748..3751, where the USA numbers take the cartridge's own tiles (its tile
// 2468 is drawn differently and 3357..3360 are its four new ones, in the four
// slots the USA table leaves unused).
static const uint16 kMap16Runs[][3] = {
  {2835, 2845, 2837}, {2846, 2846, 2886}, {2847, 2847, 3325},
  {2850, 2856, 2852}, {2857, 2858, 2835}, {2886, 2887, 2850},
  {2888, 3325, 2887}, {3357, 3747, 3361}, {3748, 3751, 3357},
};

typedef struct AssetView {
  uint8 *ptr;
  size_t size;
} AssetView;

static bool FindAssets(uint8 *blob, size_t size, AssetView *view) {
  static const uint8 kSig[] = { kAssets_Sig };
  if (size < 88 || memcmp(blob, kSig, sizeof(kSig)) != 0)
    return false;
  uint32 count = blob[80] | blob[81] << 8 | blob[82] << 16 | (uint32)blob[83] << 24;
  uint32 names = blob[84] | blob[85] << 8 | blob[86] << 16 | (uint32)blob[87] << 24;
  if (count != kNumberOfAssets || names > size || 88 + count * 4 > size - names)
    return false;
  size_t offset = 88 + count * 4 + names;
  for (uint32 i = 0; i < count; i++) {
    const uint8 *p = blob + 88 + i * 4;
    uint32 n = p[0] | p[1] << 8 | p[2] << 16 | (uint32)p[3] << 24;
    offset = (offset + 3) & ~(size_t)3;
    if (offset > size || n > size - offset)
      return false;
    view[i].ptr = blob + offset;
    view[i].size = n;
    offset += n;
  }
  return true;
}

static void RenumberMessages(uint8 *p, size_t size) {
  for (size_t i = 0; i + 1 < size; i += 2) {
    uint16 v = p[i] | p[i + 1] << 8;
    if (v >= 4)
      v++;
    p[i] = (uint8)v, p[i + 1] = (uint8)(v >> 8);
  }
}

bool AssetsFrC_Renumber(uint8 *blob, size_t size) {
  AssetView *a = calloc(kNumberOfAssets, sizeof(AssetView));
  uint16 *us_to_ca = malloc(kMap16Count * sizeof(uint16));
  uint16 *ca_to_us = malloc(kMap16Count * sizeof(uint16));
  uint8 *old = malloc(kMap16Count * 8);
  bool ok = a && us_to_ca && ca_to_us && old && FindAssets(blob, size, a) &&
            a[kAssetMap16].size == kMap16Count * 8 &&
            a[kAssetTileAttr].size == kTileAttrSize &&
            a[kAssetTeleMsg].size == kTeleMsgSize &&
            a[kAssetSignText].size == kSignTextSize;
  for (int t = 0; ok && t < 4; t++)
    ok = a[kAssetMap32 + t].size == kMap32Groups * 6;
  // Every map16 number of the map32 tables has to exist before anything
  // changes.
  for (int t = 0; ok && t < 4; t++) {
    const uint8 *p = a[kAssetMap32 + t].ptr;
    for (int g = 0; ok && g < kMap32Groups; g++, p += 6)
      ok = (p[0] | (p[4] >> 4) << 8) < kMap16Count && (p[1] | (p[4] & 15) << 8) < kMap16Count &&
           (p[2] | (p[5] >> 4) << 8) < kMap16Count && (p[3] | (p[5] & 15) << 8) < kMap16Count;
  }
  if (ok) {
    for (int u = 0; u < kMap16Count; u++)
      us_to_ca[u] = u;
    for (size_t r = 0; r < sizeof(kMap16Runs) / sizeof(kMap16Runs[0]); r++)
      for (int u = kMap16Runs[r][0]; u <= kMap16Runs[r][1]; u++)
        us_to_ca[u] = kMap16Runs[r][2] + (u - kMap16Runs[r][0]);
    for (int u = 0; u < kMap16Count; u++)
      ca_to_us[us_to_ca[u]] = u;

    // kMap16ToMap8 and kSomeTileAttr, one entry per map16 number.
    uint8 *m16 = a[kAssetMap16].ptr;
    memcpy(old, m16, kMap16Count * 8);
    for (int u = 0; u < kMap16Count; u++)
      memcpy(m16 + u * 8, old + us_to_ca[u] * 8, 8);
    uint8 *attr = a[kAssetTileAttr].ptr;
    memcpy(old, attr, kMap16Count);
    for (int u = 0; u < kMap16Count; u++)
      attr[u] = old[us_to_ca[u]];

    // The map32 tables: four map16 numbers packed in 6 bytes per group.
    for (int t = 0; t < 4; t++) {
      uint8 *p = a[kAssetMap32 + t].ptr;
      for (int g = 0; g < kMap32Groups; g++, p += 6) {
        uint16 v0 = ca_to_us[p[0] | (p[4] >> 4) << 8];
        uint16 v1 = ca_to_us[p[1] | (p[4] & 15) << 8];
        uint16 v2 = ca_to_us[p[2] | (p[5] >> 4) << 8];
        uint16 v3 = ca_to_us[p[3] | (p[5] & 15) << 8];
        p[0] = (uint8)v0, p[1] = (uint8)v1, p[2] = (uint8)v2, p[3] = (uint8)v3;
        p[4] = (uint8)((v0 >> 8) << 4 | v1 >> 8);
        p[5] = (uint8)((v2 >> 8) << 4 | v3 >> 8);
      }
    }

    // The messages of the signs and of the dungeon rooms' telepathy.
    RenumberMessages(a[kAssetTeleMsg].ptr, a[kAssetTeleMsg].size);
    RenumberMessages(a[kAssetSignText].ptr, a[kAssetSignText].size);
  }
  free(old);
  free(ca_to_us);
  free(us_to_ca);
  free(a);
  return ok;
}
