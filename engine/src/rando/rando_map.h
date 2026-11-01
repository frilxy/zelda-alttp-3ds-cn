// rando_map.{c,h} — decode the in-game overworld map graphic (Mode-7 asset 66
// + light/dark tilemaps 67/68 + palette 93) into an RGBA image, for use as the
// Map Tracker window background. Self-contained: decodes from the loaded asset
// blobs (no live game state), so it can run at startup and be verified headless.
#ifndef ZELDA3_RANDO_RANDO_MAP_H_
#define ZELDA3_RANDO_RANDO_MAP_H_

#include "../types.h"

// The decoded map is a square of this many pixels per side (64 tiles * 8 px).
#define kRandoMapPixels 512

// Decode the light (dark=false) or dark (dark=true) overworld map into `out`,
// a caller-provided RGBA8888 buffer of kRandoMapPixels*kRandoMapPixels*4 bytes.
// Pixel (x,y) at out[(y*kRandoMapPixels + x)*4 + {0:R,1:G,2:B,3:A}]. Requires the
// asset blob to be loaded (LoadAssets). Returns false if assets are unavailable.
bool RandoMap_Decode(bool dark, uint8 *out);

// Debug: decode both worlds and write them as binary PPM (P6) files
// "<prefix>_light.ppm" / "<prefix>_dark.ppm". Returns false on IO/asset failure.
bool RandoMap_DumpPpm(const char *prefix);

#endif  // ZELDA3_RANDO_RANDO_MAP_H_
