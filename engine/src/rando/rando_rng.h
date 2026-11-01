// rando_rng.h — xoshiro256** RNG (tasks.md §2.1). Phase A0 stub.
//
// Deterministic, portable, no stdlib RNG calls. Seeded from a 64-bit seed
// (typically derived from the share string's seed_u64 field).
//
// Per randomizer-core / Determinism constraints: NO rand()/random()/time()
// references. check_determinism.py + check_link_symbols.py enforce this.

#ifndef ZELDA3_RANDO_RNG_H_
#define ZELDA3_RANDO_RNG_H_

#include "../types.h"

typedef struct RandoRng {
  uint64 s[4];
} RandoRng;

// Seed the RNG. SplitMix64 expansion of `seed` into the four xoshiro256**
// state words. All-zero state is invalid; SplitMix64 produces non-zero output
// for any seed including 0.
void Rng_SeedFromU64(RandoRng *rng, uint64 seed);

// Return the next 32-bit pseudorandom value.
uint32 Rng_NextU32(RandoRng *rng);

// Return an unbiased value in [0, range). Uses rejection sampling on the
// upper 32 bits to avoid modulo bias.
uint32 Rng_NextRange(RandoRng *rng, uint32 range);

// Self-test (tasks.md §2.2 — cross-platform determinism property test).
// Validates seed-expansion, first-N outputs, and the 10k-sample SHA-256
// reference digest. Exits the process with code 2 on any mismatch.
void Rando_Rng_SelfCheck(void);

#endif  // ZELDA3_RANDO_RNG_H_
