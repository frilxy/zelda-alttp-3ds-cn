#ifndef ZELDA3_SRAM_STORE_H_
#define ZELDA3_SRAM_STORE_H_

#include "file_store.h"
#include <stdint.h>
#include <string.h>

// Native SRAM is always 8 KiB. Never expose a partial read to the engine or
// rotate a damaged primary over a complete backup. Callers serialize access.
static inline bool SramStore_ReadExact(const char *path, uint8_t *data) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  uint8_t candidate[8192];
  bool ok = fread(candidate, 1, sizeof(candidate), f) == sizeof(candidate);
  ok = (fgetc(f) == EOF && !ferror(f)) && ok;
  if (fclose(f) != 0) ok = false;
  if (ok) memcpy(data, candidate, sizeof(candidate));
  return ok;
}

static inline bool SramStore_Read(const char *path, const char *backup,
                                  uint8_t *data) {
  // Recover an interrupted replacement before considering the historical .bak.
  if (!FileStore_Recover(path)) return false;
  return SramStore_ReadExact(path, data) ||
         (FileStore_Recover(backup) && SramStore_ReadExact(backup, data));
}

static inline bool SramStore_Write(const char *path, const char *backup,
                                   const uint8_t *data) {
  uint8_t previous[8192];
  if (!FileStore_Recover(path) || !FileStore_Recover(backup)) return false;
  if (SramStore_ReadExact(path, previous) &&
      !FileStore_Write(backup, previous, sizeof(previous))) return false;
  return FileStore_Write(path, data, 8192);
}

#endif
