#include "chest_map.h"
#include "chest_catalog.h"
#include <stdlib.h>
#include <string.h>

enum { kChestTableCount = 168, kDungeonRoomCount = 320 };
typedef struct ChestEntry {
  uint16 room_word, location;
  uint8 ordinal, item;
} ChestEntry;

static ChestEntry g_chests[kChestTableCount];
static unsigned g_count;
static bool g_ready;

static int compare_chests(const void *left, const void *right) {
  const ChestEntry *a = left, *b = right;
  unsigned ar = a->room_word & 0x7fff, br = b->room_word & 0x7fff;
  return ar != br ? (ar < br ? -1 : 1) : (int)a->ordinal - b->ordinal;
}

void RandoChestMap_Clear(void) {
  memset(g_chests, 0, sizeof(g_chests));
  g_count = 0;
  g_ready = false;
}

bool RandoChestMap_Init(const uint8 *cartridge, size_t cartridge_size,
                        const uint8 *assets, size_t asset_size) {
  RandoChestMap_Clear();
  if (!cartridge || !assets || cartridge_size != kChestTableCount * 3 ||
      asset_size != kChestTableCount * 3) return false;
  ChestEntry staged[kChestTableCount];
  uint8 next_ordinal[kDungeonRoomCount] = {0};
  unsigned mapped = 0;
  for (unsigned i = 0; i < kChestTableCount; i++) {
    unsigned word = cartridge[i * 3] | (unsigned)cartridge[i * 3 + 1] << 8;
    unsigned room = word & 0x7fff;
    if (room >= kDungeonRoomCount || next_ordinal[room] >= 6) return false;
    staged[i] = (ChestEntry){word, kChestLocationByCartridgeIndex[i],
                            next_ordinal[room]++, cartridge[i * 3 + 2]};
    if (staged[i].location != 0xffff) mapped++;
  }
  qsort(staged, kChestTableCount, sizeof(*staged), compare_chests);
  for (unsigned i = 0; i < kChestTableCount; i++) {
    unsigned word = assets[i * 3] | (unsigned)assets[i * 3 + 1] << 8;
    if (staged[i].room_word != word || staged[i].item != assets[i * 3 + 2])
      return false;
  }
  if (mapped != kChestCatalogLocationCount) return false;
  memcpy(g_chests, staged, sizeof(staged));
  g_count = mapped;
  g_ready = true;
  return true;
}

bool RandoChestMap_Ready(void) { return g_ready; }
unsigned RandoChestMap_Count(void) { return g_count; }

uint16 RandoChestMap_Find(uint16 room, uint8 ordinal) {
  if (!g_ready || room >= kDungeonRoomCount) return 0xffff;
  unsigned lo = 0, hi = kChestTableCount;
  unsigned wanted = (unsigned)room * 256 + ordinal;
  while (lo < hi) {
    unsigned mid = lo + (hi - lo) / 2;
    unsigned key = (g_chests[mid].room_word & 0x7fff) * 256u + g_chests[mid].ordinal;
    if (key == wanted) return g_chests[mid].location;
    if (key < wanted) lo = mid + 1;
    else hi = mid;
  }
  return 0xffff;
}
