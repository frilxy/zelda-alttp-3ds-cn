#pragma once
#include "../types.h"
#include <stddef.h>

// The catalog contains only MIT ALttPR location identities. Room/item bytes
// are supplied by the user's cartridge and checked against the loaded assets.
bool RandoChestMap_Init(const uint8 *cartridge_table, size_t cartridge_size,
                        const uint8 *asset_table, size_t asset_size);
void RandoChestMap_Clear(void);
bool RandoChestMap_Ready(void);
uint16 RandoChestMap_Find(uint16 room, uint8 ordinal);
unsigned RandoChestMap_Count(void);
