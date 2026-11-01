#pragma once
#include "rando_settings.h"

enum { kRandoSeedKeyLength = 64 };
// The origin discriminator prevents imported and generated sessions from
// sharing saves even when their numeric seed/settings happen to match.
enum { kRandoSeedOrigin_Native = 1, kRandoSeedOrigin_Alttpr = 2 };
bool RandoSeedKey_Create(const RandoSettings *settings, uint64 seed, uint8 origin,
                          const uint8 placement_digest[32], char out[65]);
bool RandoSeedKey_Valid(const char *key);
