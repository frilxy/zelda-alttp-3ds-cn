// rando_rng.c — xoshiro256** RNG implementation (tasks.md §2.1).
//
// Algorithm: David Blackman & Sebastiano Vigna 2018, xoshiro256** 1.0
//   reference: https://prng.di.unimi.it/xoshiro256starstar.c
//   license:   public domain ("To the extent possible under law, the
//              author has dedicated all copyright [...]")
// Seed expansion: Sebastiano Vigna 2015, SplitMix64
//   reference: https://prng.di.unimi.it/splitmix64.c (same license)
//
// Determinism constraints (randomizer-core spec):
//   - NO rand()/random()/arc4random/time()/clock_gettime references
//     (verified by assets/scripts/check_determinism.py + check_link_symbols.py)
//   - All arithmetic is 64-bit / 32-bit integer; no floating point
//   - Output byte order chosen explicitly (Rng_NextU32 takes upper 32 bits
//     per xoshiro256** authors' recommendation: "The bottom 3 bits have
//     weak randomness; if you need 32 bits, use the top 32 bits.")
//
// Self-test reference values (computed in Python with the same canonical
// algorithm; see assets/scripts/_compute_rng_reference.py — if you regenerate,
// update the constants below):
//   seed=0 expands via SplitMix64(0) to:
//     s[0] = 0xe220a8397b1dcdaf
//     s[1] = 0x6e789e6aa1b965f4
//     s[2] = 0x06c45d188009454f
//     s[3] = 0xf88bb8a8724c81ec
//   First 4 Rng_NextU32 outputs (upper 32 bits of next64):
//     0x99ec5f36, 0xbf6e1f78, 0x1a5f849d, 0x6aa594f1
//   SHA-256 of 10000 next64 samples (LE-serialized) with seed=0xDEADBEEFCAFEBABE:
//     8bced84b7fb75b25bd8d5aa5d80900013632c2c8eac6047ead7ced795803165e

#include "rando_rng.h"
#include "../types.h"

// ---------------------------------------------------------------------------
// SplitMix64 — seed-expansion helper.
// State mutates in place; each call returns the next 64-bit output.
// ---------------------------------------------------------------------------
static uint64 splitmix64_next(uint64 *state) {
  uint64 z = (*state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

// ---------------------------------------------------------------------------
// rotl — 64-bit left rotation.
// ---------------------------------------------------------------------------
static inline uint64 rotl64(const uint64 x, int k) {
  return (x << k) | (x >> (64 - k));
}

// ---------------------------------------------------------------------------
// Rng_SeedFromU64 — seed the RNG from a single 64-bit value.
// Uses SplitMix64 to expand the seed into the four xoshiro256** state words.
// All-zero state is invalid; SplitMix64 produces non-zero output for any
// seed value (including 0).
// ---------------------------------------------------------------------------
void Rng_SeedFromU64(RandoRng *rng, uint64 seed) {
  uint64 state = seed;
  rng->s[0] = splitmix64_next(&state);
  rng->s[1] = splitmix64_next(&state);
  rng->s[2] = splitmix64_next(&state);
  rng->s[3] = splitmix64_next(&state);
}

// ---------------------------------------------------------------------------
// Internal: next 64-bit xoshiro256** output. Mutates rng->s in place.
// ---------------------------------------------------------------------------
static uint64 xoshiro256ss_next(RandoRng *rng) {
  const uint64 result = rotl64(rng->s[1] * 5, 7) * 9;
  const uint64 t = rng->s[1] << 17;

  rng->s[2] ^= rng->s[0];
  rng->s[3] ^= rng->s[1];
  rng->s[1] ^= rng->s[2];
  rng->s[0] ^= rng->s[3];

  rng->s[2] ^= t;
  rng->s[3] = rotl64(rng->s[3], 45);

  return result;
}

// ---------------------------------------------------------------------------
// Rng_NextU32 — return next pseudorandom 32-bit value.
// Takes the upper 32 bits of the 64-bit output per xoshiro256** recommendation.
// ---------------------------------------------------------------------------
uint32 Rng_NextU32(RandoRng *rng) {
  return (uint32)(xoshiro256ss_next(rng) >> 32);
}

// ---------------------------------------------------------------------------
// Rng_NextRange — return unbiased value in [0, range).
// Uses simple rejection sampling on the upper 32 bits to eliminate modulo bias.
// For range == 0, returns 0 (defensive; logically undefined).
// ---------------------------------------------------------------------------
uint32 Rng_NextRange(RandoRng *rng, uint32 range) {
  if (range == 0) {
    return 0;
  }
  // ``(0 - range) % range`` computes the smallest multiple of range that
  // exceeds (2^32 - range); values below this threshold are unbiased after
  // modulo. Equivalent to: threshold = (2^32) % range.
  uint32 threshold = ((uint32)0 - range) % range;
  for (;;) {
    uint32 r = Rng_NextU32(rng);
    if (r >= threshold) {
      return r % range;
    }
  }
}

// ---------------------------------------------------------------------------
// Self-test. Always linked (callable from --rando-selftest CLI flag).
// Validates the algorithm against the published reference values above.
// Exits the process with code 2 on any mismatch — cross-platform determinism
// is non-negotiable per randomizer-core spec.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include "third_party/sha256/sha256.h"

void Rando_Rng_SelfCheck(void) {
  RandoRng rng;
  Rng_SeedFromU64(&rng, 0);

  // Expected state after SplitMix64-seed-expansion of seed=0:
  static const uint64 kExpectedSeedState[4] = {
    0xe220a8397b1dcdafull,
    0x6e789e6aa1b965f4ull,
    0x06c45d188009454full,
    0xf88bb8a8724c81ecull,
  };
  for (int i = 0; i < 4; ++i) {
    if (rng.s[i] != kExpectedSeedState[i]) {
      fprintf(stderr,
              "Rando_Rng_SelfCheck: SplitMix64 seed-expansion mismatch at s[%d]:\n"
              "  expected 0x%016llx\n"
              "  got      0x%016llx\n",
              i, (unsigned long long)kExpectedSeedState[i], (unsigned long long)rng.s[i]);
      exit(2);
    }
  }

  // First 4 Rng_NextU32 outputs (upper 32 bits of next64):
  static const uint32 kExpectedU32[4] = {
    0x99ec5f36u, 0xbf6e1f78u, 0x1a5f849du, 0x6aa594f1u,
  };
  for (int i = 0; i < 4; ++i) {
    uint32 got = Rng_NextU32(&rng);
    if (got != kExpectedU32[i]) {
      fprintf(stderr,
              "Rando_Rng_SelfCheck: Rng_NextU32 mismatch at sample %d:\n"
              "  expected 0x%08x\n"
              "  got      0x%08x\n",
              i, kExpectedU32[i], got);
      exit(2);
    }
  }

  // Cross-platform determinism property test: hash 10k 64-bit samples from
  // a fixed seed and compare to the reference digest. Identical bytes on
  // every platform is the determinism contract per tasks.md §2.2.
  Rng_SeedFromU64(&rng, 0xDEADBEEFCAFEBABEull);
  SHA256_CTX h;
  sha256_init(&h);
  for (int i = 0; i < 10000; ++i) {
    uint64 v = xoshiro256ss_next(&rng);
    uint8 le[8];
    le[0] = (uint8)(v >> 0);
    le[1] = (uint8)(v >> 8);
    le[2] = (uint8)(v >> 16);
    le[3] = (uint8)(v >> 24);
    le[4] = (uint8)(v >> 32);
    le[5] = (uint8)(v >> 40);
    le[6] = (uint8)(v >> 48);
    le[7] = (uint8)(v >> 56);
    sha256_update(&h, le, 8);
  }
  uint8 digest[32];
  sha256_final(&h, digest);
  static const uint8 kExpectedDigest[32] = {
    0x8b,0xce,0xd8,0x4b,0x7f,0xb7,0x5b,0x25,
    0xbd,0x8d,0x5a,0xa5,0xd8,0x09,0x00,0x01,
    0x36,0x32,0xc2,0xc8,0xea,0xc6,0x04,0x7e,
    0xad,0x7c,0xed,0x79,0x58,0x03,0x16,0x5e,
  };
  for (int i = 0; i < 32; ++i) {
    if (digest[i] != kExpectedDigest[i]) {
      fprintf(stderr,
              "Rando_Rng_SelfCheck: 10k-sample SHA-256 digest mismatch at byte %d.\n"
              "  Cross-platform determinism BROKEN - the xoshiro256** output\n"
              "  is not byte-identical to the reference. Halt.\n",
              i);
      exit(2);
    }
  }
}
