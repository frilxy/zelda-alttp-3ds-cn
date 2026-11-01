// rando_settings.h — RandoSettings struct + canonical serialization
// (tasks.md §2.5).
//
// The serialization order pinned below is part of the determinism contract:
// changing it (or any field width / enum value) is a kGeneratorVersion bump
// trigger (tasks.md §13.6) and invalidates the regression corpus.
//
// The canonical-byte sequence feeds SHA-256 to produce settings_hash; the
// first 16 bytes of that hash go into the share-string payload
// (rando_share.h).

#ifndef ZELDA3_RANDO_SETTINGS_H_
#define ZELDA3_RANDO_SETTINGS_H_

#include "../types.h"

// ===========================================================================
// Pinned enums. Values are part of the determinism contract — additions
// MUST go at the end of each enum (so existing values stay stable).
// ===========================================================================

typedef enum {
  kWorldState_Open = 0,
  kWorldState_Standard = 1,
  kWorldState_Inverted = 2,
  kWorldState_Retro = 3,
} WorldState;

typedef enum {
  kGoal_Ganon = 0,
  kGoal_FastGanon = 1,
  kGoal_Dungeons = 2,
  kGoal_Pedestal = 3,
  kGoal_TriforceHunt = 4,
  kGoal_GanonHunt = 5,
  kGoal_Completionist = 6,
} Goal;

typedef enum {
  kItemPoolDifficulty_Easy = 0,
  kItemPoolDifficulty_Normal = 1,
  kItemPoolDifficulty_Hard = 2,
  kItemPoolDifficulty_Expert = 3,
} ItemPoolDifficulty;

typedef enum {
  kDungeonItemMode_Vanilla = 0,
  kDungeonItemMode_Dungeon = 1,
  kDungeonItemMode_Wild = 2,
} DungeonItemMode;

// add-rando-key-rings-skeleton-key — requested key-ring policy. The requested
// value is serialized even when current small-key semantics make rings
// ineffective; Settings_EffectiveKeyRings() is the generation/UI authority.
typedef enum {
  kKeyRings_Off = 0,
  kKeyRings_Random = 1,
  kKeyRings_All = 2,
} KeyRingsMode;

typedef enum {
  kModeWeapons_Randomized = 0,
  kModeWeapons_Assured = 1,
  // kModeWeapons_Vanilla = 2,  // still reserved (out of Phase B scope)
  kModeWeapons_Swordless = 3,   // no swords in the pool; runtime swordless patches active
} ModeWeapons;

typedef enum {
  kAccessibility_Items = 0,      // "100% Inventory": every progression item reachable
  kAccessibility_Locations = 1,  // "100% Locations": every location reachable (strictest)
  // ALTTPR "Not Guaranteed" — exposed as "beatable only" in the UI. The seed is
  // still guaranteed beatable (goal completable); only full item/location
  // accessibility is relaxed. (The serialized value name stays "None" because
  // enum values are part of the determinism contract.)
  kAccessibility_None = 2,
} Accessibility;

// Hints v2 configurable delivery coverage. Values intentionally match the
// canonical two-bit wire code in [28] bits 5-6: zero is the historical/default
// Full coverage so old settings blobs and Balanced seeds remain byte-identical.
typedef enum {
  kHintTileCoverage_Full = 0,      // all 15 telepathic tiles
  kHintTileCoverage_None = 1,      // no telepathic tiles
  kHintTileCoverage_Sparse = 2,    // 5 telepathic tiles
  kHintTileCoverage_Standard = 3,  // 10 telepathic tiles
} HintTileCoverage;

// Hints v2 content emphasis. The value is split across canonical [28] bit 7
// (low bit) and [29] bit 7 (high bit). Variety=0 is the historical/default
// Balanced behavior.
typedef enum {
  kHintMix_Variety = 0,
  kHintMix_Important = 1,
  kHintMix_Difficult = 2,
  kHintMix_WorldInfo = 3,
} HintMix;

// Named hint-policy tuples. Custom is derived from any enabled tuple that does
// not exactly match Sparse/Balanced/Direct; it is never directly applied or
// serialized as a separate setting.
typedef enum {
  kHintProfile_Off = 0,
  kHintProfile_Sparse = 1,
  kHintProfile_Balanced = 2,
  kHintProfile_Direct = 3,
  kHintProfile_Custom = 4,
} HintProfile;

typedef enum {
  kPyramidBowUpgrade_Silvers = 0,
  // Legacy serialized slot. The Pyramid Fairy trade-in was retired by the
  // fairy chest model, so this no longer controls placement or runtime grants.
} PyramidBowUpgrade;

// ===========================================================================
// RandoSettings struct. Holds the user's choice for every Phase A axis.
// Field order in C does NOT need to match canonical serialization order —
// the serializer reads each field by name (see Settings_CanonicalSerialize
// in rando_settings.c).
// ===========================================================================

typedef struct RandoSettings {
  uint8 settings_version;           // = 1 for Phase A
  uint8 world_state;                // WorldState
  uint8 goal;                       // Goal
  // 0..7 fixed, or kCrystalsRandom (8) = "random" (add-rando-random-crystals):
  // the REQUESTED sentinel stays in canonical bytes [2]/[3] (hash/share
  // preserve it, keyrings-style never-normalized); the EFFECTIVE 0..7 count
  // is resolved deterministically from the seed by Crystals_Resolve at
  // generation and slot activation.
  uint8 crystals_ganon;             // required to make Ganon vulnerable
  uint8 crystals_tower;             // required to enter Ganon's Tower
  uint8 tricks;                     // Phase A: pinned to 0 (none); Phase B bitmask reserved
  uint8 item_pool_difficulty;       // ItemPoolDifficulty
  uint8 logic;                      // Phase A: 0 (NoGlitches); Phase B+ reserved
  uint8 mode_weapons;               // ModeWeapons
  uint8 accessibility;              // Accessibility
  uint8 pyramid_bow_upgrade;        // legacy/no-op; canonicalized to Silvers
  uint8 region_boss_hearts_in_pool; // legacy/no-op; canonicalized to 0 so boss
                                    // heart drops are always shuffled
  uint8 dungeon_small_keys_mode;    // DungeonItemMode
  uint8 dungeon_big_keys_mode;      // DungeonItemMode
  uint8 dungeon_maps_mode;          // DungeonItemMode
  uint8 dungeon_compasses_mode;     // DungeonItemMode
  uint8 prize_shuffle;              // bool: shuffle crystal/pendant→dungeon assignments
  uint8 medallion_shuffle;          // bool: shuffle Bombos/Ether/Quake → MM/TR entrance
  uint8 race_mode;                  // bool; Phase B feature, bit reserved in hash from Phase A
  uint16 pieces_required;           // for triforce-hunt / ganonhunt (was uint8; spec is uint16)
  uint16 pieces_placed;             // for triforce-hunt / ganonhunt
  // Hints axis. Public Off/Balanced; historical on/full/sahasrahla inputs
  // normalize to Balanced. Default Balanced. Canonical byte [22], landed in
  // the kGenVer 13→14 bump.
  uint8 hints;
  // Configurable Hints v2 policy. Defaults are Full tile coverage, Variety
  // content, and three clues per paid service. Their wire encodings are all
  // zero, preserving the historical/default canonical bytes:
  //   hint_tile_coverage: [28] bits 5-6 (HintTileCoverage)
  //   hint_mix: low bit [28] bit 7, high bit [29] bit 7 (HintMix)
  //   hint_paid_depth: actual count 0..3, remapped into [30] bits 6-7 so
  //                    actual 3 is wire zero.
  // Off, and the empty delivery tuple (None tiles + zero paid clues), normalize
  // to one canonical Off representation with all three policy defaults.
  uint8 hint_tile_coverage;
  uint8 hint_mix;
  uint8 hint_paid_depth;
  // Independent NPC transaction disclosure. When enabled, randomized
  // purchase/reward NPCs identify the item before the player commits. This is
  // intentionally not part of HintProfile or the generated hint-plan policy:
  // it remains meaningful while clue delivery is Off. Canonical [25] bit 7.
  uint8 hint_npc_reward_reveal;
  // Phase B Slice 7 §63 / Slice 8 §64 — shuffle axes. Binary on/off.
  // Default 0 (off). Canonical serialization at offsets [23] and [24]
  // — landed with §66 in the kGenVer 13→14 bump.
  uint8 boss_shuffle;
  uint8 drop_shuffle;
  // Phase C — entrance shuffle composable axes (binary on/off). All default
  // off EXCEPT `coupled` (defaults on, the ALTTPR baseline: enter A ⇒ exit A).
  // Canonical serialization bit-PACKS them into the previously-zero pad
  // byte [25] (see kEntranceAxis_* below), so the default settings still
  // serialize to a zero byte and kSettingsCanonicalLen stays 28 — no
  // canonical-size-coupling cascade. `coupled`/`cross_category`/`decoupled`
  // are normalized to 0 when no shuffle axis is active (apply_derived_rules),
  // so the default packs to 0x00. The four famous ALTTPR modes (Simple /
  // Restricted / Crossed / Insanity) are UI presets over these axes, not a
  // stored enum. See add-rando-entrance-shuffle/design.md §5.
  uint8 shuffle_cave_entrances;     // bool
  uint8 shuffle_dungeon_entrances;  // bool (Stage 2)
  uint8 coupled;                    // bool, default ON
  uint8 cross_category;             // bool (Stage 3) — caves↔dungeons may mix
  uint8 decoupled;                  // bool (Stage 4) — per-endpoint; implies !coupled
  // Advanced opt-in: also shuffle Ganon's Tower's entrance. Off by default
  // because GT's crystal-tower gate travels with its door, which can be circular
  // at high crystals.tower (the door needs N crystals but leads to a crystal-
  // bearing dungeon). The full-reachability gate rejects circular permutations,
  // so a seed that can't be made reachable simply fails to generate — lower
  // crystals.tower (0 always works) or reroll. Requires shuffle_dungeon_entrances.
  uint8 shuffle_ganons_tower_entrance;  // bool (advanced)
  // dungeon-chains — opt-in dungeon boss-chain topology. Serialized into
  // canonical byte [25] bit6 alongside the entrance/topology axes. Default off
  // keeps default settings_hash + corpus byte-identical. Derived rules normalize
  // incompatible combinations off before hashing/generation.
  uint8 dungeon_chains;  // bool
  // add-rando-enemy-shuffle — enemy (sprite-type) substitution axis. Binary
  // on/off, default off. Like boss/drop shuffle this is ORTHOGONAL to item
  // placement (draws no fill RNG, adds no logic predicate), so it does NOT grow
  // the canonical layout: it bit-PACKS into the reserved pad bit [26] bit 0, so
  // kSettingsCanonicalLen stays 28 and default-settings settings_hash stays
  // byte-identical. See kEnemyShuffleAxis_* below + Settings_CanonicalSerialize.
  uint8 enemy_shuffle;  // bool
  // add-rando-door-shuffle — intra-dungeon door-connection shuffle.
  // 0 = vanilla, 1 = basic (per-dungeon pool, intensity 1: Normal + spiral
  // doors, original key-door counts relocated + prover-validated). Packs into
  // canonical pad byte [27] bits 0-1; default 0 keeps the default
  // settings_hash byte-identical and kSettingsCanonicalLen at 28.
  uint8 door_shuffle;
  // add-rando-customizer-mode — when set, generation pins the customizer
  // manifest's locations and assumed-fill completes the rest (see customizer.h).
  // Serialized as canonical byte [26] bit1 (kCustomizerAxis_Active, alongside
  // enemy_shuffle's bit0; door_shuffle owns [27] bits 0-1): default 0 ⇒
  // byte-identical default settings_hash + corpus, no size-coupling cascade.
  uint8 customizer_active;  // bool
  // add-rando-traps — frequency for masquerade trap items. 0=off, 1=low,
  // 2=medium, 3=high. Serialized in canonical byte [26] bits 2-3, sharing the
  // extension byte with enemy_shuffle/customizer. Default off keeps the default
  // settings_hash and corpus placements byte-identical; nonzero replaces
  // eligible final junk-filled placements with trap items.
  uint8 traps;
  // Randomizer QoL: promote OcarinaInactive pickups to the active bird-woken
  // flute immediately. Serialized inversely in canonical byte [26] bit4
  // (manual activation when set), so default ON keeps byte [26] at zero.
  uint8 instant_flute;  // bool, default on
  // add-rando-trap-catalog — per-category trap enable mask (HAZARD/IMPAIR/DRAIN/
  // SCARE/DISPLACE = bits 0-4). Serialized in canonical byte [27] bits 2-6. A
  // ZERO mask while traps>0 means "all categories enabled" (the zero-sentinel),
  // so the default (traps off, mask 0) keeps byte [27] at zero and the corpus
  // byte-identical. Meaningful only when traps != off.
  uint8 trap_categories;
  // add-rando-pot-sanity — tiered pot-shuffle axis (PotShuffle below):
  // Off / Keys / Contents / All (value 4 = Subset reserved for Phase 7). Serialized
  // as a 3-bit field packed NON-contiguously into the last free canonical bits:
  // low 2 bits in [26] bits 6-7, high bit in [27] bit 7 (see kPotShuffleAxis_*).
  // Default Off=0 keeps both bytes at their pre-pot values, so the default
  // settings_hash + corpus stayed byte-identical until the next append-only
  // canonical growth.
  // Normalized to Off under cave-entrance shuffle (apply_derived_rules). Door
  // shuffle composes through the generated door x pot bridge.
  uint8 pot_shuffle;
  // add-rando-enemy-drop-sanity — itemize forced enemy drop checks. Serialized as
  // append-only canonical byte [28]; old 28-byte v2 share strings zero-extend this
  // to Off. Keys is active when effective small keys are Wild/Retro or Dungeon.
  // Dungeon also enables ordinary dungeon enemies for vanilla-door layouts.
  // All additionally enables generated static overworld enemy checks for
  // vanilla-door/no-enemy-shuffle layouts; unsupported layouts degrade
  // explicitly and must never make all mean dungeon-only.
  uint8 enemy_drop_checks;
  // add-enemy-souls — soul items gate whether the matching enemy/boss spawns.
  // Serialized in canonical byte [28] bits 2-3 (alongside enemy_drop_checks in
  // bits 0-1); older/shorter share strings zero-extend to Off. Bosses places
  // the 12 boss souls; BossesEnemies additionally places one soul per enemy
  // family and holds kill-gates shut while suppressed spawns exist.
  uint8 souls_shuffle;
  // add-npc-souls — 23 NPC soul items gate check-giving people (site-scoped
  // spawn suppression + logic gates; Kiki gates the PoD entry edge, the Bomb
  // Shop dealer gates the Pyramid Fairy checks). INDEPENDENT of souls_shuffle
  // (composes with any tier); NO door-shuffle degrade (no gated check is
  // door-oracle-controlled). Canonical byte [28] bit 4 (kNpcSoulsAxis_*);
  // pre-feature builds hard-refuse strings carrying it (forward-compat
  // refusal). Configurable hints now use bits 5-7.
  uint8 npc_souls;
  // add-rando-grass-rock-shuffle — overworld terrain checks. Two independent
  // tier axes (TerrainShuffle: off/junk/all): grass covers bushes + cuttable
  // thick grass, rock covers light(Glove)/heavy(Mitt) small rocks + big
  // piles. junk = locations are junk-pad-only fill (no progression, no logic
  // pressure); all = full open locations. Serialized in the APPENDED
  // canonical byte [29] (grass bits 0-1, rock bits 2-3 — byte [28]'s lower
  // bits belong to the enemy/souls axes and its tail now carries hint policy);
  // older/shorter blobs zero-extend to Off.
  // No derived-rule couplings: composes with door/cave-entrance/pot/enemy
  // shuffles (terrain is overworld-surface-bound; see the change's D12).
  uint8 grass_shuffle;
  uint8 rock_shuffle;
  // add-rando-key-rings-skeleton-key — append-only canonical byte [30].
  // key_rings is the REQUESTED Off/Random/All policy and is deliberately not
  // normalized away under Vanilla or Retro/Generic Keys. skeleton_key is a
  // bonus-only item toggle; logic never requires it and it never opens big-key
  // doors.
  uint8 key_rings;    // KeyRingsMode
  uint8 skeleton_key; // bool
  // add-rando-ow-warp-shuffle — overworld warp axes (canonical [30] bits
  // 3-5). Normalized to 0 under Inverted by apply_derived_rules (v1 scope);
  // Open/Standard/Retro honor them.
  uint8 flute_shuffle;      // FluteShuffle: 0 off / 1 balanced / 2 random
  uint8 whirlpool_shuffle;  // bool
  // add-rando-shopsanity — the 27 regular shop slots (LOCTYPE_Shop, ids
  // 237-263) become ordinary open fill locations in every world state, with
  // seed-derived prices and buy-once-then-vanilla-restock runtime semantics.
  // Serialized in canonical byte [29] bit 4 (the first reserved bit — no
  // length change, so default-settings settings_hash is unchanged). No
  // derived-rule couplings: composes with every other axis.
  uint8 shopsanity;  // bool
  // add-rando-bonk-sanity — placed OW bonk-item sprites (bee hives / apple
  // trees) as checks. TerrainShuffle tiers (off/junk/all); canonical byte
  // [29] bits 5-6 (after shopsanity's bit 4; no length change). First bonk
  // grants + marks; later bonks replay vanilla (farmable bees/apples).
  uint8 bonk_shuffle;  // TerrainShuffle
} RandoSettings;

// add-rando-grass-rock-shuffle — shared tier values for both terrain axes.
// Values are part of the determinism contract (additions go at the end).
typedef enum {
  kTerrainShuffle_Off  = 0,  // no terrain checks (default; byte-identical)
  kTerrainShuffle_Junk = 1,  // active, junk-pad-only fill (no progression)
  kTerrainShuffle_All  = 2,  // full open locations (progression may land)
} TerrainShuffle;

// add-rando-random-crystals — the requested-"random" sentinel for the two
// crystal-count axes (full canonical bytes [2]/[3]; values 0..8 valid).
enum { kCrystalsRandom = 8 };

enum {
  kGrassShuffleAxis_Shift = 0,        // canonical [29] bits 0-1
  kGrassShuffleAxis_Mask  = 3u << 0,
  kRockShuffleAxis_Shift  = 2,        // canonical [29] bits 2-3
  kRockShuffleAxis_Mask   = 3u << 2,
  kShopsanityAxis_Enabled = 1u << 4,  // canonical [29] bit 4
                                      // (add-rando-shopsanity)
  kBonkShuffleAxis_Shift = 5,         // canonical [29] bits 5-6
  kBonkShuffleAxis_Mask  = 3u << 5,   // (add-rando-bonk-sanity)
  kHintMixAxis_HighBit = 1u << 7,     // configurable hints: mix bit 1
};

enum {
  kKeyRingsAxis_Shift = 0,          // canonical [30] bits 0-1
  kKeyRingsAxis_Mask = 3u << 0,
  kSkeletonKeyAxis_Enabled = 1u << 2,
  kKeyRingsAxis_DefinedMask = kKeyRingsAxis_Mask | kSkeletonKeyAxis_Enabled,
  // add-rando-ow-warp-shuffle — canonical [30] bits 3-5.
  kWhirlpoolAxis_Enabled = 1u << 3,
  kFluteShuffleAxis_Shift = 4,
  kFluteShuffleAxis_Mask = 3u << 4,
  // configurable hints — paid depth uses canonical [30] bits 6-7.
  kHintPaidDepthAxis_Shift = 6,
  kHintPaidDepthAxis_Mask = 3u << 6,
  kCanon30_DefinedMask = kKeyRingsAxis_DefinedMask | kWhirlpoolAxis_Enabled |
                         kFluteShuffleAxis_Mask | kHintPaidDepthAxis_Mask,
};

// add-rando-ow-warp-shuffle — flute-spot shuffle modes. Values are part of
// the canonical encoding ([30] bits 4-5); 3 is refused-undefined.
enum FluteShuffle {
  kFluteShuffle_Off = 0,
  kFluteShuffle_Balanced = 1,
  kFluteShuffle_Random = 2,
};

// add-rando-pot-sanity — pot_shuffle tiers. Values are part of the determinism
// contract (additions go at the end). keys ⊆ contents ⊆ all. Value 4 (Subset)
// is reserved: the field is already 3 bits wide so Phase 7 adds it with no
// canonical re-pack and no second kGeneratorVersion bump — Settings_Validate
// rejects 4 until then (the reserved-until-implemented pattern, like
// kModeWeapons_Vanilla).
typedef enum {
  kPotShuffle_Off = 0,       // pots are pure vanilla (default; byte-identical)
  kPotShuffle_Keys = 1,      // only small-key pots are checks
  kPotShuffle_Contents = 2,  // every pot with vanilla content (loot + keys)
  kPotShuffle_All = 3,       // also the empty pots (ITEM_Nothing filler)
  // kPotShuffle_Subset = 4,  // RESERVED (Phase 7) — mid-size tier
} PotShuffle;

// add-rando-enemy-drop-sanity — enemy-drop check tiers. Keys itemizes vanilla
// forced enemy key drops. Dungeon also itemizes ordinary eligible dungeon enemies
// as checks. Door shuffle composes with dungeon/all ordinary enemy checks through
// generated bridge rows. All adds generated static overworld, reviewed
// underworld, GT-miniboss, and repeatable finite scripted-spawn checks where runtime
// identity and logic are modeled; it remains a distinct tier above Dungeon.
typedef enum {
  kEnemyDropChecks_Off = 0,
  kEnemyDropChecks_Keys = 1,
  kEnemyDropChecks_Dungeon = 2,
  kEnemyDropChecks_All = 3,
} EnemyDropChecks;

// add-enemy-souls — souls_shuffle tiers. Values are part of the determinism
// contract (additions go at the end). Canonical byte [28] bits 2-3.
typedef enum {
  kSoulsShuffle_Off = 0,            // no souls (default; byte-identical)
  kSoulsShuffle_Bosses = 1,         // 12 boss souls gate boss spawns
  kSoulsShuffle_BossesEnemies = 2,  // + one soul per enemy family
} SoulsShuffle;

enum {
  kSoulsShuffleAxis_Shift = 2,        // canonical [28] bits 2-3
  kSoulsShuffleAxis_Mask  = 3u << 2,  // 0x0C
};

// add-npc-souls — canonical [28] bit 4. Configurable hints claim bits 5-7:
// tile coverage in bits 5-6 and the low hint-mix bit in bit 7.
enum {
  kNpcSoulsAxis_Enabled = 1u << 4,
  kHintTileCoverageAxis_Shift = 5,
  kHintTileCoverageAxis_Mask = 3u << 5,
  kHintMixAxis_LowBit = 1u << 7,
  kCanon28_DefinedMask = 3u | kSoulsShuffleAxis_Mask |
                         kNpcSoulsAxis_Enabled |
                         kHintTileCoverageAxis_Mask | kHintMixAxis_LowBit,
};

// add-rando-enemy-shuffle — bit positions for the packed pad byte (canonical
// [26]). A zero byte == no enemy shuffle (the default), preserving the
// byte-identical corpus invariant. [26] was previously always-zero reserved pad.
// add-rando-customizer-mode shares this byte at bit1, add-rando-traps uses
// bits 2-3, and instant_flute uses bit4 inversely (zero = instant activation).
enum {
  kEnemyShuffleAxis_Enabled = 1u << 0,
  kCustomizerAxis_Active    = 1u << 1,
  kInstantFluteAxis_ManualActivation = 1u << 4,
};

// add-rando-traps — frequency enum + packed canonical [26] bit field.
enum {
  kTrapFrequency_Off = 0,
  kTrapFrequency_Low = 1,
  kTrapFrequency_Medium = 2,
  kTrapFrequency_High = 3,
  kTrapFrequency_Insanity = 4,  // every eligible junk pickup becomes a trap
  kTrapAxis_Shift = 2,
  kTrapAxis_Mask = 3u << kTrapAxis_Shift,   // canonical [26] bits 2-3 (low 2 bits of `traps`)
  // 3rd bit of `traps` (for Insanity=4) lives in the free canonical [26] bit5, so
  // the off/low/medium/high (0..3) encoding stays byte-identical and instant_flute
  // (bit4) is untouched. The field is non-contiguous by design — see
  // Settings_Canonical{Serialize,Deserialize}.
  kTrapAxis_HighBit = 1u << 5,
};

// add-rando-trap-catalog — per-category enable mask bits
// (RandoSettings.trap_categories) and the canonical byte [27] packing (bits 2-6).
// kTrapCategory_All is both the full 5-bit set AND the meaning of a zero mask
// while traps are enabled (the zero-sentinel keeps default byte [27] = 0).
enum {
  kTrapCategory_Hazard   = 1u << 0,
  kTrapCategory_Impair   = 1u << 1,
  kTrapCategory_Drain    = 1u << 2,
  kTrapCategory_Scare    = 1u << 3,
  kTrapCategory_Displace = 1u << 4,
  kTrapCategory_All      = 0x1Fu,
  kTrapCategoriesAxis_Shift = 2,
  kTrapCategoriesAxis_Mask  = 0x1Fu << kTrapCategoriesAxis_Shift,  // canonical [27] bits 2-6
};

// add-rando-door-shuffle — door_shuffle axis values (canonical [27] bits 0-1).
enum {
  kDoorShuffle_Vanilla = 0,
  kDoorShuffle_Basic = 1,
  kDoorShuffleAxis_Mask = 3,
};

// add-rando-pot-sanity — pot_shuffle is a 3-bit field (5 values incl. the
// reserved Subset). The last 3 free canonical bits are non-contiguous, so the
// field is split like `traps`: low 2 bits in canonical [26] bits 6-7, high bit
// in canonical [27] bit 7. Default Off=0 leaves every one of those bits clear,
// preserving the byte-identical default settings_hash + corpus. These were the
// final free bits at kSettingsCanonicalLen 28; enemy_drop_checks is the first
// append-only axis after that.
enum {
  kPotShuffleAxis_LowShift = 6,        // canonical [26] bits 6-7 (low 2 bits)
  kPotShuffleAxis_LowMask  = 3u << 6,  // 0xC0
  kPotShuffleAxis_HighBit  = 1u << 7,  // canonical [27] bit 7 (high bit)
};

// Phase C — bit positions for the packed entrance-axis byte (canonical [25]).
// Used by Settings_CanonicalSerialize/Deserialize. A zero byte == no entrance
// shuffle (the default), preserving the byte-identical corpus invariant.
enum {
  kEntranceAxis_ShuffleCaves    = 1u << 0,
  kEntranceAxis_ShuffleDungeons = 1u << 1,
  kEntranceAxis_Coupled         = 1u << 2,
  kEntranceAxis_CrossCategory   = 1u << 3,
  kEntranceAxis_Decoupled       = 1u << 4,
  kEntranceAxis_ShuffleGanonsTower = 1u << 5,
  kEntranceAxis_DungeonChains   = 1u << 6,
  kHintNpcRewardRevealAxis_Enabled = 1u << 7,
};

// ===========================================================================
// Canonical byte length — every Phase A settings struct serializes to
// exactly this many bytes. Adding a field requires bumping this constant
// AND kGeneratorVersion (tasks.md §13.6).
// ===========================================================================
// Canonical serialization is 31 bytes: 28 historical bytes plus append-only
// bytes [28] enemy/soul axes, [29] terrain axes, and [30] key-item axes.
// Layout per spec — see Settings_CanonicalSerialize.
// Phase B Slice 7+8 §66: bumped from 24→28 to absorb `hints`, `boss_shuffle`,
// `drop_shuffle` at offsets [22..24]. kGeneratorVersion bumped 13→14 in lockstep.
// Phase C bit-packs the entrance axes into pad byte [25]; add-rando-enemy-shuffle
// bit-packs `enemy_shuffle` into pad byte [26] (bit0), add-rando-customizer-mode
// shares it (bit1), add-rando-traps packs `traps` into [26] bits 2-3, and
// instant_flute packs its inverse manual-activation bit into [26] bit4;
// add-rando-door-shuffle packs its axis into [27] (bits 0-1).
// LENGTH STAYED 28 through those axes because all reused previously-zero pad
// bytes. add-rando-pot-sanity took the LAST free bits of [26] (6-7) and [27]
// (bit 7) for pot_shuffle, so [26] and [27] are now fully allocated. NPC reward
// disclosure takes the last historical extension bit, [25] bit 7
// (entrance/chains axes use 0-6). add-rando-enemy-drop-sanity grows the length
// to 29 by appending [28] (bits 0-1). add-enemy-souls takes [28] bits 2-3
// (kSoulsShuffleAxis_*); add-npc-souls takes [28] bit 4 (kNpcSoulsAxis_*);
// Configurable hints claim [28] bits 5-7, [29] bit 7, and [30] bits 6-7
// without changing the canonical length. Every default policy component uses
// wire zero, so default/Balanced canonical bytes and settings_hash stay stable.
// add-rando-grass-rock-shuffle grew 29 -> 30 by appending [29]: grass_shuffle
// bits 0-1 + rock_shuffle bits 2-3 (kGrassShuffleAxis_*/kRockShuffleAxis_*).
// The length change alters every settings_hash (SHA input length) — covered
// by that change's kGeneratorVersion bump; sidecar format_version 8 widened
// the stored blob. add-rando-shopsanity takes [29] bit 4
// (kShopsanityAxis_Enabled) — length UNCHANGED, so default settings_hash is
// stable across that change. add-rando-bonk-sanity takes [29] bits 5-6
// (kBonkShuffleAxis_*); configurable hints take bit 7.
// add-rando-key-rings-skeleton-key grows 30 -> 31 with append-only byte [30]:
// key_rings bits 0-1, skeleton_key bit 2, and warp shuffle bits 3-5. Configurable
// hints take bits 6-7; sidecar format_version 10 carries the widened blob.
#define kSettingsCanonicalLen 31

// Populate the struct with Phase A defaults (Open / Fast Ganon / Normal
// pool / 7 crystals each / dungeon items Vanilla / prize+medallion shuffle
// on / Randomized weapons / Items-accessibility / Silvers / 20-of-30 pieces).
void Settings_SetDefaults(RandoSettings *s);

// Normalize the configurable-hints policy to its one canonical disabled form.
// Off, or enabled with no tile and no paid delivery, becomes:
// Off + Full + Variety + paid depth 3. Other valid policies are unchanged.
void Settings_NormalizeHintPolicy(RandoSettings *s);

// Central named-profile authority shared by CLI, UIs, and spoiler metadata.
// Apply returns false and leaves `s` unchanged for NULL, Custom, or an invalid
// profile. All four applicable tuples are normalized before returning.
HintProfile Settings_GetHintProfile(const RandoSettings *s);
bool Settings_ApplyHintProfile(RandoSettings *s, HintProfile profile);
const char *Settings_HintProfileName(HintProfile profile);   // stable lowercase
const char *Settings_HintProfileTitle(HintProfile profile);  // display title

// Effective hint-delivery helpers for generation and both settings UIs. They
// consume raw settings safely: an empty delivery tuple behaves as Off even
// before explicit normalization. Null settings are disabled.
bool Settings_HintsEnabled(const RandoSettings *s);
uint8 Settings_HintTileCount(const RandoSettings *s);          // 0, 5, 10, or 15
uint8 Settings_EffectiveHintPaidDepth(const RandoSettings *s); // 0 when disabled

// Effective small-keys mode after derived overrides. ALTTPR's Retro world-state
// forces `region.wildKeys` (small keys enter the general/wild pool, no longer
// restricted to their own dungeon — app/World/Retro.php). Topology-changing
// dungeon axes force Dungeon keys so the placer cannot rely on vanilla/free key
// assumptions after the route through a dungeon changes. This is the SINGLE
// source of truth for the override: it is applied identically in
// apply_derived_rules (so the canonical settings hash reflects the effective
// mode) and at every placer read site (so placement matches the hash).
//
// NOTE: this is `wildKeys` (the placement-side override). ALTTPR Retro ALSO sets
// `rom.genericKeys` (one shared key pool, any key opens any door) — that is
// `Settings_GenericKeysActive` below, which collapses the per-dungeon key-door
// LOGIC predicates onto the shared GenericKey count. The two are complementary:
// wildKeys lets keys spawn anywhere; genericKeys makes them fungible.
uint8 Settings_EffectiveSmallKeysMode(const RandoSettings *s);

// Effective key-ring policy. Rings are unavailable when small keys are
// Vanilla/free or Retro's Generic Keys are active. This helper never mutates or
// canonicalizes the requested key_rings field, so a temporarily disabled UI
// choice survives share/config round-trips.
uint8 Settings_EffectiveKeyRings(const RandoSettings *s);
// True when the requested ring mode is temporarily unavailable because the
// seed uses vanilla/free small keys or Retro's shared Generic Key model. UIs
// use the same authority as Settings_EffectiveKeyRings instead of re-spelling
// the condition.
bool Settings_KeyRingsForcedOff(const RandoSettings *s);

// Topology-changing dungeon axes force in-dungeon big keys at every placer read
// so generated reachability cannot rely on a vanilla-mode pregrant that the
// runtime still requires the player to collect in-place.
uint8 Settings_EffectiveBigKeysMode(const RandoSettings *s);

// add-rando-door-shuffle — the normalized (post-derived-rules) door_shuffle
// value; definitionally the canonical byte [27], so generation, runtime
// install, and settings_hash always agree (vanilla under Inverted/Retro,
// glitched logic, or entrance shuffle — the MVP pins).
uint8 Settings_EffectiveDoorShuffle(const RandoSettings *s);

// dungeon-chains — the normalized topology opt-in. Chains are one-directional:
// they turn themselves off under incompatible modes rather than forcing those
// modes off. Honored only on Open/Standard NoGlitches seeds, with boss shuffle
// off, door shuffle vanilla, and no entrance shuffle axes.
bool Settings_EffectiveDungeonChains(const RandoSettings *s);

// add-rando-pot-sanity (audit) — the EFFECTIVE accessibility tier. goal ==
// Completionist forces 100%-Locations (apply_derived_rules), so the placer's
// acceptance gate, the spoiler, and the settings_hash must all read THIS, not the
// raw field — a raw `accessibility=none` under Completionist would otherwise let
// the acceptance gate skip the 100%-locations sphere walk the hash already promises.
uint8 Settings_EffectiveAccessibility(const RandoSettings *s);

// add-rando-pot-sanity (audit) — cave-entrance shuffle is honored ONLY on
// Open/Standard (apply_derived_rules zeroes the entrance axes under Inverted/Retro
// — Entrance_IsActive). Consumers that branch on "is cave shuffle on" must read
// THIS, not the raw flag, or an inert cave bit (retained from a prior Open
// selection) wrongly forces pots off under Inverted/Retro — where the canonical
// hash, having zeroed the bit first, keeps pots ON.
bool Settings_EffectiveShuffleCaveEntrances(const RandoSettings *s);

// add-rando-pot-sanity — true when a shuffle axis FORCES pot_shuffle off because
// the pots can't be safely placed under it: cave-entrance shuffle (cave/house pot
// location IDs sit above the per-location entrance region-override range, so they
// would evaluate from the vanilla overworld region instead of the shuffled
// entrance's — see Entrance_ApplyRegionOverrides). apply_derived_rules normalizes
// pot_shuffle off under this, and pot_active / the spoiler consult the same
// predicate, so the settings_hash, placement, runtime, and spoiler can't
// disagree. Door shuffle is modeled by the generated door x pot bridge; dungeon-
// entrance shuffle is edge-based and does NOT mis-bind dungeon pots.
bool Settings_PotShuffleForcedOff(const RandoSettings *s);

// add-rando-ow-warp-shuffle — the overworld warp axes are out of v1 scope under
// Inverted (it rewrites the overworld and the flute semantics; the spot data is
// LW-indexed). apply_derived_rules normalizes them off there, but ONLY on the
// private copy Settings_CanonicalSerialize takes — the placer, the spoiler and
// slot activation all consume the RAW struct, so they must re-derive the
// force-off through these. Reading s->flute_shuffle / s->whirlpool_shuffle
// directly in generation or activation code is the bug this exists to prevent:
// the axes stay live while the canonical hash and share string say they are
// off, so one share string reproduces two different worlds.
bool Settings_OwWarpForcedOff(const RandoSettings *s);
uint8 Settings_EffectiveFluteShuffle(const RandoSettings *s);  // FluteShuffle
bool Settings_EffectiveWhirlpoolShuffle(const RandoSettings *s);
// True when either effective warp axis is live — the single predicate every
// compute/install/digest site should gate on.
bool Settings_OwWarpActive(const RandoSettings *s);

// add-rando-shopsanity — true when the 27 regular shop slots are live fill
// locations. The axis composes with everything, including cave-entrance
// shuffle: each cave-resident shop is its own pool entry (keyed by overworld
// door row) carrying its three slot ids, so Entrance_ApplyRegionOverrides
// rebinds them to whichever door reaches them in that seed, and the runtime
// derives shop identity from the same assignment. There is no derived-rule
// normalization to re-derive — this predicate IS the consumer contract. Read
// it, never the raw s->shopsanity, so placement, spoiler and runtime agree.
bool Settings_ShopsanityActive(const RandoSettings *s);

// Door shuffle forces in-dungeon keys and has a generated door x pot bridge.
// This is the effective pot tier the door prover/runtime should see; keeping it
// here prevents generation and active-slot reinstall from drifting.
uint8 Settings_DoorPotTier(const RandoSettings *s);

// True when pot_shuffle itemizes small-key pots as live checks: pot_shuffle >=
// Keys AND pots are not forced off (Settings_PotShuffleForcedOff: cave-entrance
// shuffle). Shared by the logic VM (eval_pot_keys_*) and the placer
// (pot_keys_dungeon_active) so the pot-key gates can't drift from pot_active; the
// WILD/DUNGEON ops additionally test Settings_EffectiveSmallKeysMode.
bool Settings_PotKeysActive(const RandoSettings *s);

// add-rando-enemy-drop-sanity — normalized enemy-drop check tier. Keys is
// honored when effective small keys are Wild (including Retro's computed mode)
// or Dungeon. Dungeon and All compose with door shuffle; enemy shuffle lowers
// them to Keys, entrance shuffle lowers All to Dungeon, and placement still
// fail-closes if the generated all-tier registry is unavailable.
uint8 Settings_EffectiveEnemyDropChecks(const RandoSettings *s);
bool Settings_EnemyDropKeysActive(const RandoSettings *s);
bool Settings_EnemyChecksDungeonActive(const RandoSettings *s);
bool Settings_EnemyChecksAllActive(const RandoSettings *s);

// add-enemy-souls — normalized souls tier. Souls degrade to OFF under door
// shuffle (any tier): the door-shuffle key/traversal oracle is species-blind
// (it counts enemy-held keys and floods kill-room shutters with no per-species
// soul model), the enemies tier's generated kill-room soul requirements are
// computed against the VANILLA door graph, and even the bosses tier's
// soul-gated boss/prize predicates make the door-layout fill exhaust its
// attempt budget per layout candidate. Single source of truth for the placer,
// the logic VM (eval_souls_tier), the pool, and the runtime suppression hooks
// — the same pattern as Settings_EffectiveEnemyDropChecks.
uint8 Settings_EffectiveSoulsShuffle(const RandoSettings *s);

// True iff ALTTPR's `rom.genericKeys` is in effect for these settings — i.e.
// `world_state == Retro` (Retro pins it on, per app/World/Retro.php). Like
// `Settings_EffectiveSmallKeysMode` this is *computed* from world_state, not a
// serialized bit, so no new canonical bytes enter the settings struct. Used by
// BuildItemPool (substitute each per-dungeon SmallKey with the fungible
// GenericKey) and by the predicate VM (a per-dungeon small-key requirement is
// satisfied by holding >=1 GenericKey — mirrors ALTTPR ItemCollection::has()'s
// ShopKey wildcard at app/Support/ItemCollection.php:271-273). NULL-safe.
bool Settings_GenericKeysActive(const RandoSettings *s);

// Serialize to a fixed-layout little-endian byte sequence. Always writes
// kSettingsCanonicalLen bytes. Returns kSettingsCanonicalLen.
int Settings_CanonicalSerialize(const RandoSettings *s,
                                uint8 out[kSettingsCanonicalLen]);

// Inverse of Settings_CanonicalSerialize. Reads `kSettingsCanonicalLen`
// bytes and populates `out`. Returns 0 on success, non-zero on input error
// (NULL pointer, or an out-of-range enum byte — see Settings_Validate).
// Phase B Slice 6 — needed by the race-mode reveal pipeline to reconstruct
// settings from the suppressed spoiler file.
int Settings_CanonicalDeserialize(const uint8 in[kSettingsCanonicalLen],
                                  RandoSettings *out);

// FIX #5 — range-check every serialized ENUM/count field of a settings struct
// against its defined range (world_state, goal, crystals, logic tier, dungeon
// item modes, accessibility, the boolean axes, ...). Returns true when all are
// in range. The byte-blob paths (sidecar slot activation, suppressed-spoiler
// reveal, native-window prefs restore) previously copied enum bytes RAW, so a
// corrupt/foreign blob flowed into consumers like `1u << settings->world_state`
// (UB for ws >= 32). Settings_CanonicalDeserialize decodes the fully allocated
// bit-packed bytes [25]-[27] before this validates their boolean/enum fields;
// pieces_required/placed accept the full uint16 range the CLI parser allows.
// Called by Settings_CanonicalDeserialize, so a blob with an out-of-range enum
// now fails deserialization (return -2).
bool Settings_Validate(const RandoSettings *s);

// Compute SHA-256 of the canonical-serialized bytes. Writes 32 bytes.
void Settings_ComputeHash(const RandoSettings *s, uint8 out_hash[32]);

// Convenience: compute the first 16 bytes of the full hash (the truncation
// the share-string payload uses).
void Settings_HashShort(const RandoSettings *s, uint8 out_hash[16]);

// Self-test (tasks.md §2.5): round-trip the default settings, validate the
// reference SHA-256 of the canonical bytes for known input. Exits with
// code 2 on failure.
void Settings_SelfCheck(void);

// add-rando-major-glitch D6 — true when this seed's PLACEMENT assumed the
// player can perform a restored JP-1.0 glitch, so the runtime MUST force
// kFeatures0_RestoreJpGlitches on (else an assumed-fill-certified seed becomes
// an unreachable-item soft-softlock). Two triggers:
//   logic >= 1 (OverworldGlitches) — every glitch tier assumes the OWG
//     technique set, of which Fake Flippers (canFakeFlipper) + Superspeed
//     (canSuperSpeed) are restored on this US-1.0 build.
//   tricks bit 1 (fake-flippers) — the fake-flippers placement trick maps 1:1
//     to the restored Fake Flippers glitch and is independent of logic.
// Only fake-flippers among the 8 tricks maps to a restored glitch; the rest
// (boots-clip/pearl-bypass/...) are cross-version or unrestored and do NOT
// force the flag. Pure function of settings — no features0 dependency.
bool Rando_SettingsAssumeJpGlitches(const RandoSettings *s);

// ---------------------------------------------------------------------------
// CLI `--settings=k=v,...` parser (tasks.md §1.6a, randomizer-core spec).
//
// `csv` is a comma-separated list of `<key>=<value>` pairs using the
// canonical CLI key grammar (dot-separated keys: `mode.state`, `crystals.ganon`,
// `dungeon_items.small_keys`, etc.). Enum values are case-sensitive and match
// the spec exactly (e.g., `fast_ganon`, `triforce-hunt`, `NoGlitches`).
//
// On success: writes parsed values into `*out` over the defaults and returns
// 0. On any error (unknown key, bad value, duplicate key): writes a one-line
// error to stderr and returns non-zero. Callers SHOULD have populated `out`
// with defaults before calling.
//
// Examples accepted:
//   "mode.state=open,goal=fast_ganon,crystals.ganon=7"
//   "goal=triforce-hunt,pieces_required=25,pieces_placed=30"
//   "dungeon_items.small_keys=dungeon,prize_shuffle=true"
//   "hints=sparse,hint_mix=difficult"
//
// Hint profiles are off/sparse/balanced/direct. Component overrides are
// hint_tiles=none|sparse|standard|full (few/many/all are UI-name aliases),
// hint_paid=0..3, and
// hint_mix=variety|important|difficult|world-info. They apply after the profile
// regardless of CSV order; Off wins and clears them. The longer historical
// development spellings hint_tile_coverage and hint_paid_depth remain aliases
// and share duplicate-key identity with their public names. Independent NPC
// transaction disclosure uses hint_npc_reward_reveal=true|false;
// hint_npc_rewards, hint_vendor_items, and hint_npc_items are aliases sharing
// the same duplicate-key identity.
int Settings_ParseCsv(const char *csv, RandoSettings *out);

// ---------------------------------------------------------------------------
// Settings presets (tasks.md §9.5). Named bundles of (key, value) pairs that
// populate RandoSettings. The settings screen offers these as one-tap
// defaults; the user can fine-tune afterward.
//
// Phase A presets: Open Ganon, Standard Ganon, Inverted Ganon, Retro,
// Triforce Hunt Default. Each preset starts from Settings_SetDefaults and
// overrides specific fields.
// ---------------------------------------------------------------------------
typedef enum {
  kPreset_OpenGanon = 0,        // Open / Fast Ganon (the defaults)
  kPreset_StandardGanon = 1,    // Standard world-state, Fast Ganon
  kPreset_InvertedGanon = 2,    // Inverted world-state, Fast Ganon
  kPreset_Retro = 3,            // Retro world-state, Defeat Ganon
  kPreset_TriforceHuntDefault = 4,  // Open / Triforce Hunt 20-of-30
  kPreset__Count = 5,
} SettingsPreset;

// Apply a preset to `out`. Calls Settings_SetDefaults first, then overrides
// the preset-specific fields. Returns 0 on success, non-zero on unknown preset.
int Settings_ApplyPreset(SettingsPreset preset, RandoSettings *out);

// Human-readable preset name for UI display.
const char *Settings_PresetName(SettingsPreset preset);

#endif  // ZELDA3_RANDO_SETTINGS_H_
