#include "seed_identity.h"
#include "rando.h"
#include "third_party/sha256/sha256.h"
#include <string.h>

bool RandoSeedKey_Valid(const char *key) {
  if (!key || strlen(key) != kRandoSeedKeyLength) return false;
  for (unsigned i = 0; i < kRandoSeedKeyLength; i++)
    if (!((key[i] >= '0' && key[i] <= '9') ||
          (key[i] >= 'a' && key[i] <= 'f'))) return false;
  return true;
}

bool RandoSeedKey_Create(const RandoSettings *settings, uint64 seed, uint8 origin,
                          const uint8 placement_digest[32], char out[65]) {
  if (!out) return false;
  out[0] = 0;
  if (!settings || !Settings_Validate(settings) ||
      (origin != kRandoSeedOrigin_Native && origin != kRandoSeedOrigin_Alttpr) ||
      (origin == kRandoSeedOrigin_Alttpr && !placement_digest)) return false;
  SHA256_CTX context;
  sha256_init(&context);
  static const uint8 domain[] = "Zelda3DS seed saves v1";
  sha256_update(&context, domain, sizeof(domain));
  uint8 material[11];
  material[0] = origin;
  material[1] = kGeneratorVersion & 255;
  material[2] = (kGeneratorVersion >> 8) & 255;
  for (unsigned i = 0; i < 8; i++) material[3+i] = (uint8)(seed >> (8*i));
  sha256_update(&context, material, sizeof(material));
  uint8 canonical[kSettingsCanonicalLen];
  Settings_CanonicalSerialize(settings, canonical);
  sha256_update(&context, canonical, sizeof(canonical));
  if (placement_digest) sha256_update(&context, placement_digest, 32);
  uint8 digest[32]; sha256_final(&context, digest);
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < sizeof(digest); i++) {
    out[2*i] = hex[digest[i] >> 4];
    out[2*i+1] = hex[digest[i] & 15];
  }
  out[64] = 0;
  return true;
}
