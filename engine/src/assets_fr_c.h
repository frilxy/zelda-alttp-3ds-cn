#ifndef ZELDA3_ASSETS_FR_C_H_
#define ZELDA3_ASSETS_FR_C_H_

#include "types.h"

// zelda3_assets_fr-c.bps builds the assets from the French Canadian cartridge
// alone, with its tiles and messages numbered as that cartridge numbers them.
// This gives them the USA numbering the engine uses, in place: the assets it
// changes keep their size. False, with nothing changed, when the blob is not
// laid out as that patch builds it.
bool AssetsFrC_Renumber(uint8 *blob, size_t size);

#endif  // ZELDA3_ASSETS_FR_C_H_
