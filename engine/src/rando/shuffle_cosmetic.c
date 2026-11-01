// shuffle_cosmetic.c — see shuffle_cosmetic.h.
//
// Windows headers must precede "types.h" (types.h defines BYTE/WORD/DWORD as
// function-style macros that collide with winbase.h typedefs). Mirrors config.c.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // FindFirstFileA / FindNextFileA
#else
#include <dirent.h>
#endif

#include "shuffle_cosmetic.h"
#include "../types.h"
#include "../config.h"     // g_config, Config_InternString
#include "rando_rng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Conservative shuffled-song band. Overworld music is masked `& 0xf`
// (messaging.c), so area background songs live in the low-id range; control
// codes are >= 0xf0. Shuffling only [lo, hi] keeps special tracks (boss /
// credits / triforce, ids >= 0x10) intact. The exact band is a playtest-tunable
// (tasks.md §6) — widen once the per-id semantics are confirmed in-game.
enum { kCosmeticSongLo = 0x01, kCosmeticSongHi = 0x0F };

#define COSMETIC_MAX_SPRITES 2048

// Derived tables, rebuilt by Cosmetic_SetSeed.
static uint8  g_group_perm[16];   // per 16-color CGRAM group: channel perm 0..5
static uint8  g_song_map[256];    // identity outside [lo,hi]; bijection within
static uint64 g_eff_seed;         // effective cosmetic seed (for the sprite pick)
static bool   g_tables_ready;

// Portable, locale-independent ASCII case-insensitive compare. The sprite-folder
// sort below MUST be locale-independent: POSIX strcasecmp / Win32 _stricmp
// collate non-ASCII filename bytes differently across platforms and locales,
// which would make the same cosmetic_seed pick a different .zspr on different
// machines — breaking the determinism contract (same seed -> same sprite). Only
// ASCII A-Z is folded; all other bytes compare verbatim.
static int cosmetic_ascii_stricmp(const char *a, const char *b) {
  for (;; a++, b++) {
    unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return (int)ca - (int)cb;
    if (ca == 0) return 0;
  }
}

uint8 Cosmetic_ParsePaletteMode(const char *s) {
  if (!s) return kCosmeticPalette_Vanilla;
  if (!cosmetic_ascii_stricmp(s, "shuffled"))  return kCosmeticPalette_Shuffled;
  if (!cosmetic_ascii_stricmp(s, "grayscale") || !cosmetic_ascii_stricmp(s, "greyscale"))
    return kCosmeticPalette_Grayscale;
  if (!cosmetic_ascii_stricmp(s, "negative") || !cosmetic_ascii_stricmp(s, "invert"))
    return kCosmeticPalette_Negative;
  return kCosmeticPalette_Vanilla;  // "vanilla", "off", unknown
}

void Cosmetic_SetSeed(uint64 config_cosmetic_seed, uint64 slot_seed_u64) {
  g_eff_seed = config_cosmetic_seed ? config_cosmetic_seed : slot_seed_u64;

  RandoRng rng;
  Rng_SeedFromU64(&rng, g_eff_seed);

  // Palette: one channel permutation (of 6) per 16-color CGRAM group.
  for (int i = 0; i < 16; i++)
    g_group_perm[i] = (uint8)Rng_NextRange(&rng, 6);

  // Music: Fisher-Yates over the song band; identity elsewhere.
  for (int i = 0; i < 256; i++) g_song_map[i] = (uint8)i;
  int n = kCosmeticSongHi - kCosmeticSongLo + 1;
  for (int i = n - 1; i >= 1; i--) {
    int j = (int)Rng_NextRange(&rng, (uint32)(i + 1));
    uint8 t = g_song_map[kCosmeticSongLo + i];
    g_song_map[kCosmeticSongLo + i] = g_song_map[kCosmeticSongLo + j];
    g_song_map[kCosmeticSongLo + j] = t;
  }
  g_tables_ready = true;
}

// Apply channel permutation `p` (0..5) to a BGR555 color, preserving bit 15.
static uint16 PermuteChannels(uint16 c, uint8 p) {
  int r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
  int nr, ng, nb;
  switch (p) {
    default:
    case 0: nr = r; ng = g; nb = b; break;
    case 1: nr = r; ng = b; nb = g; break;
    case 2: nr = g; ng = r; nb = b; break;
    case 3: nr = g; ng = b; nb = r; break;
    case 4: nr = b; ng = r; nb = g; break;
    case 5: nr = b; ng = g; nb = r; break;
  }
  return (uint16)((c & 0x8000) | (nb << 10) | (ng << 5) | nr);
}

uint16 Cosmetic_TransformPaletteColor(uint16 c, int cgram_index) {
  uint8 mode = g_config.cosmetic_palette_mode;
  if (mode == kCosmeticPalette_Vanilla)
    return c;
  if (mode == kCosmeticPalette_Shuffled)
    return PermuteChannels(c, g_group_perm[(cgram_index >> 4) & 15]);
  if (mode == kCosmeticPalette_Grayscale) {
    int r = c & 0x1f, g = (c >> 5) & 0x1f, b = (c >> 10) & 0x1f;
    int l = (r * 77 + g * 150 + b * 29) >> 8;  // weights sum to 256 -> l in 0..31
    return (uint16)((c & 0x8000) | (l << 10) | (l << 5) | l);
  }
  return (uint16)(c ^ 0x7fff);  // negative: 1's-complement all three 5-bit channels
}

void Cosmetic_ApplyPaletteCgramRange(uint16 *cgram, int start_index, int count) {
  uint8 mode = g_config.cosmetic_palette_mode;
  if (mode == kCosmeticPalette_Vanilla || !cgram) return;
  for (int i = 0; i < count; i++) {
    cgram[i] = Cosmetic_TransformPaletteColor(cgram[i], start_index + i);
  }
}

void Cosmetic_ApplyPaletteCgram(uint16 *cgram, int count) {
  Cosmetic_ApplyPaletteCgramRange(cgram, 0, count);
}

uint8 Cosmetic_RemapSong(uint8 music_ctrl) {
  if (!g_config.cosmetic_music_shuffle || !g_tables_ready) return music_ctrl;
  if ((music_ctrl & 0xf0) == 0xf0) return music_ctrl;  // control codes
  return g_song_map[music_ctrl];                       // identity outside band
}

// --- Sprite folder pick ------------------------------------------------------

static bool HasZsprExt(const char *name) {
  size_t n = strlen(name);
  return n > 5 && !cosmetic_ascii_stricmp(name + n - 5, ".zspr");
}

static int CompareNames(const void *a, const void *b) {
  return cosmetic_ascii_stricmp(*(const char *const *)a, *(const char *const *)b);
}

const char *Cosmetic_PickSpriteFile(void) {
  const char *dir = g_config.cosmetic_sprite_dir;
  if (!dir || !dir[0]) return NULL;

  char **names = (char **)malloc(sizeof(char *) * COSMETIC_MAX_SPRITES);
  if (!names) return NULL;
  int count = 0;

#if defined(_WIN32)
  char pattern[1024];
  snprintf(pattern, sizeof(pattern), "%s\\*.zspr", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          HasZsprExt(fd.cFileName) && count < COSMETIC_MAX_SPRITES) {
        names[count] = _strdup(fd.cFileName);
        if (names[count]) count++;
      }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < COSMETIC_MAX_SPRITES) {
      if (HasZsprExt(e->d_name)) {
        names[count] = strdup(e->d_name);
        if (names[count]) count++;
      }
    }
    closedir(d);
  }
#endif

  const char *result = NULL;
  if (count > 0) {
    qsort(names, count, sizeof(char *), CompareNames);  // stable cross-platform order
    RandoRng rng;
    Rng_SeedFromU64(&rng, g_eff_seed ^ 0x53504e4b53504e4bULL /* "SPNK" salt */);
    int idx = (int)Rng_NextRange(&rng, (uint32)count);
    char full[1280];
    snprintf(full, sizeof(full), "%s/%s", dir, names[idx]);
    result = Config_InternString(full);
  }

  for (int i = 0; i < count; i++) free(names[i]);
  free(names);
  return result;
}

// --- Determinism + structural self-check -------------------------------------

void Cosmetic_SelfCheck(void) {
  const uint64 kSeed = 0x0123456789ABCDEFull;
  uint8 perm_a[16], song_a[256];
  Cosmetic_SetSeed(kSeed, 0);
  memcpy(perm_a, g_group_perm, sizeof perm_a);
  memcpy(song_a, g_song_map, sizeof song_a);

  // (1) Within-run determinism: the same effective seed rebuilds identical tables.
  Cosmetic_SetSeed(kSeed, 0);
  if (memcmp(perm_a, g_group_perm, sizeof perm_a) != 0 ||
      memcmp(song_a, g_song_map, sizeof song_a) != 0) {
    fprintf(stderr, "Cosmetic_SelfCheck: non-deterministic tables for a fixed seed.\n");
    abort();
  }

  // (2) config_cosmetic_seed==0 falls back to the slot seed: SetSeed(0, S) must
  // equal SetSeed(S, 0). Guards the seed-resolution in Cosmetic_SetSeed.
  Cosmetic_SetSeed(0, kSeed);
  if (memcmp(perm_a, g_group_perm, sizeof perm_a) != 0 ||
      memcmp(song_a, g_song_map, sizeof song_a) != 0) {
    fprintf(stderr, "Cosmetic_SelfCheck: config-seed==0 did not resolve to the slot seed.\n");
    abort();
  }

  // (3) The seed actually drives the output (guards a 'seed never wired' /
  // stuck-identity regression). A different seed must change at least one table;
  // the chance both 16 group perms AND the 15-element band permutation collide
  // for two distinct seeds is astronomically small.
  Cosmetic_SetSeed(~kSeed, 0);
  if (memcmp(perm_a, g_group_perm, sizeof perm_a) == 0 &&
      memcmp(song_a, g_song_map, sizeof song_a) == 0) {
    fprintf(stderr, "Cosmetic_SelfCheck: tables are seed-insensitive.\n");
    abort();
  }

  // (4) Song map is a bijection within [lo,hi] and identity outside — no song
  // silenced or duplicated.
  Cosmetic_SetSeed(kSeed, 0);
  bool seen[256];
  memset(seen, 0, sizeof seen);
  for (int i = 0; i < 256; i++) {
    uint8 m = g_song_map[i];
    if (i >= kCosmeticSongLo && i <= kCosmeticSongHi) {
      if (m < kCosmeticSongLo || m > kCosmeticSongHi) {
        fprintf(stderr, "Cosmetic_SelfCheck: song map sent in-band id %d out of band.\n", i);
        abort();
      }
      if (seen[m]) {
        fprintf(stderr, "Cosmetic_SelfCheck: song map is not a bijection (dup %d).\n", m);
        abort();
      }
      seen[m] = true;
    } else if (m != (uint8)i) {
      fprintf(stderr, "Cosmetic_SelfCheck: song map perturbed out-of-band id %d.\n", i);
      abort();
    }
  }

  // (5) Every group permutation index is a valid PermuteChannels selector (0..5).
  for (int i = 0; i < 16; i++) {
    if (g_group_perm[i] > 5) {
      fprintf(stderr, "Cosmetic_SelfCheck: group perm %d out of range (%d).\n", i, g_group_perm[i]);
      abort();
    }
  }

  fprintf(stderr, "[Cosmetic_SelfCheck] OK\n");
}
