#pragma once
#include <stdint.h>
typedef struct RandoSeedSummary {
  char key[65];
  uint64_t seed;
  int preset;
  int origin; // 1 native, 2 ALttPR
  char label[24];
} RandoSeedSummary;

typedef struct RandoImportFile { char name[96]; } RandoImportFile;

typedef enum RandoErrorCode {
  kRandoError_None, kRandoError_InvalidFile, kRandoError_UnsupportedRules,
  kRandoError_MissingPlacements, kRandoError_Uncertified,
  kRandoError_Storage, kRandoError_SourceChanged
} RandoErrorCode;
