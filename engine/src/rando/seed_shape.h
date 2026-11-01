// seed_shape.h - generator-side seed-shape filters.
//
// Shape filters are NOT canonical randomizer settings. They are a search layer:
// given a base seed, the CLI tries deterministic candidate seeds until one
// produces a placement whose spoiler/sphere metrics match the requested shape.
// The accepted seed is still reproduced by the normal share string alone.
#ifndef ZELDA3_RANDO_SEED_SHAPE_H_
#define ZELDA3_RANDO_SEED_SHAPE_H_

#include <stddef.h>

#include "../types.h"
#include "rando_placement.h"
#include "rando_settings.h"

enum { kSeedShapeMaxClauses = 16 };

// Named preset aliases calibrated against this fork's sphere metric. A 500-seed
// Open/Fast Ganon sample produced max_sphere 3..8, with 6+ in the upper tail.
enum {
  kSeedShapePresetShortMaxSphere = 4,
  kSeedShapePresetLongMinSphere = 6,
};

typedef enum SeedShapeClauseKind {
  kSeedShapeClause_MaxSphere = 0,
  kSeedShapeClause_MinSphere,
  kSeedShapeClause_NoUnreachable,
  kSeedShapeClause_NoForwardFill,
  kSeedShapeClause_ItemAtMost,
  kSeedShapeClause_ItemAtLeast,
} SeedShapeClauseKind;

typedef struct SeedShapeClause {
  uint8 kind;
  uint16 item_id;
  uint8 value;
} SeedShapeClause;

typedef struct SeedShapeFilter {
  uint8 enabled;
  uint8 count;
  SeedShapeClause clauses[kSeedShapeMaxClauses];
} SeedShapeFilter;

typedef struct SeedShapeMetrics {
  uint8 max_sphere;
  uint16 unreachable_count;
  uint16 forward_fill_fallback_count;
} SeedShapeMetrics;

// Parses comma-separated tokens. Returns 0 on success, -1 with err populated on
// malformed input.
int SeedShape_Parse(const char *text, SeedShapeFilter *out,
                    char *err, size_t err_cap);

bool SeedShape_Evaluate(const SeedShapeFilter *filter,
                        const RandoSettings *settings,
                        const RandoPlacementTable *placements,
                        const RandoSpheres *spheres,
                        const PlacementStats *stats,
                        SeedShapeMetrics *metrics_out,
                        char *reject_reason,
                        size_t reject_reason_cap);

void SeedShape_Describe(const SeedShapeFilter *filter, char *out, size_t out_cap);
void SeedShape_FormatMetrics(const SeedShapeMetrics *metrics,
                             char *out, size_t out_cap);

void SeedShape_SelfCheck(void);

#endif  // ZELDA3_RANDO_SEED_SHAPE_H_
