#ifndef ZELDA3_ALTTPR_IMPORT_H_
#define ZELDA3_ALTTPR_IMPORT_H_
#include "rando_settings.h"
#include "rando_placement.h"
#include "rando_save.h"
#include "seed_summary.h"
#define kAlttprImportMaxBytes (1024u * 1024u)
#define kAlttprImportLocations 232

typedef struct AlttprSeed {
  RandoSettings settings;
  RandoPlacement placements[kAlttprImportLocations];
  uint16 count;
  uint8 prizes[13], medallions[2], bottles[2];
  uint8 placement_advanced, starting_sword;
  uint8 placement_digest[32], source_digest[32];
  uint64 identity_seed;
  char public_hash[24];
} AlttprSeed;
// Exact official spoiler data only. No fill/generation is ever called here.
RandoErrorCode Alttpr_LastError(void);
bool Alttpr_Parse(const char *json, size_t length, AlttprSeed *out,
                   char *error, size_t error_size);
bool Alttpr_Load(const char *path, AlttprSeed *out, char *error, size_t error_size);
bool Alttpr_CreateSlot(const AlttprSeed *seed, int slot_index,
                       char *error, size_t error_size);
void Alttpr_Clear(void);
void Alttpr_Install(const AlttprSeed *seed);
bool Alttpr_IsActive(void);
bool Alttpr_HasSewerLightCone(void);
void Alttpr_AddStartingCounts(RandoCounts *counts);
bool Alttpr_ApplyEscapeRefill(void);
bool Alttpr_ValidateSlot(const RandoSidecarSlot *slot);
bool Alttpr_Assignments(uint8 prizes[13], uint8 medallions[2]);
bool Alttpr_RefillBottle(bool dark_world);
int Alttpr_CanKillBoss(uint8 dungeon,const RandoCounts *counts);
int Alttpr_LocationReachability(uint16 location, const RandoCounts *counts);
bool Alttpr_AdditionalRegionRoute(uint16 from, uint16 to, const RandoCounts *counts);
#endif
