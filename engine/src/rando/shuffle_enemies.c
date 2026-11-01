// shuffle_enemies.c — add-rando-enemy-shuffle (sprite-type substitution).
//
// See shuffle_enemies.h for the contract. Determinism: a dedicated xoshiro256**
// stream forked off the seed (salt "ENEMYSHF") so the fill RNG + the boss/drop
// streams are unperturbed. The substitution is computed per-room/per-area at
// LOAD time (not a stored permutation) because the candidate pool depends on
// which GFX sheets are LIVE-loaded for that room/area — load-history dependent
// and unknown at generation time. A per-room/per-area RNG keyed by
// (seed XOR salt, room-or-area-key, slot) makes every pick deterministic and
// reproducible without storing anything in the slot sidecar (regenerated from
// (seed, settings) at activation, exactly like boss/drop shuffle).
//
// ANTI-CRASH (design.md D3, non-negotiable): every candidate's required GFX
// sheet(s) must be a subset of the room/area's actually-loaded sheets. The sheet
// resolver records a snapshot of sprite_gfx_subset_0..3 (g_ram 0xC2FC..0xC2FF)
// together with the room/area key that produced it; the picker only substitutes
// when that key matches the sprite list being loaded. This preserves the
// 0-inheritance model while avoiding stale live-sheet state during multi-step
// dungeon transitions. The required-sheet sets come from Enemizer's per-sprite
// AddSubgroupN lists (SpriteRequirement.cs).
//
// BEATABILITY (design.md D7): the constraint table is the SOLE enforcer (logic
// models no per-room kill-clear). Conservative first pass: unknown-safety
// sprites are do_not_randomize; bosses + mini-bosses (incl. GT mini-bosses
// 0x09/0x53/0x54) are excluded; key-safety is over-approximated — EVERY
// dungeon replacement must be killable && !cannot_have_key, and dungeon rooms
// in the shutter/immovable/flying exclusion lists are further restricted. This
// over-restricts variety in favor of never shipping an unbeatable seed; widen
// once playtested.

#include "shuffle_enemies.h"
#include "rando_settings.h"
#include "rando_rng.h"
#include "souls.h"  // add-enemy-souls: vanilla pin for kill-gated/key-drop rooms
#include "../assets.h"     // vanilla sprite-list blobs for context gating
#include "../features.h"   // kRam_EnemyShuffle* reserved-block allocations
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Live game-state RAM (defined in zelda_rtl.c). We read/write
// sprite_gfx_subset_0..3 (0xC2FC..0xC2FF) directly, avoiding variables.h's macro
// wall in this TU (same pattern as rando_placement.c). The fork's g_ram is
// 131072 bytes; 0xC2FF is well inside.
extern uint8 g_ram[131072];

// ---------------------------------------------------------------------------
// Constraint table — per sprite-type byte (0x00..0xF2). Authored conservatively
// by cross-checking Enemizer SpriteRequirement.cs against the fork's
// Sprite_HEX_* ids (the Enemizer SpriteId IS the SNES sprite type byte, so the
// mapping is the identity). Flags:
//   ESF_RANDOMIZABLE — a safe enemy we may substitute IN (and replace OUT).
//                      Absent ⇒ do_not_randomize (NPC/object/overlord/boss/
//                      marker/unknown). NOTHING outside this set is ever a
//                      candidate or a replaced source.
//   ESF_KILLABLE     — can be killed by the player's normal kit (sword/bombs/
//                      etc.). Required for shutter/kill-clear/key rooms.
//   ESF_CANNOT_KEY   — must NOT carry a small key (Enemizer CannotHaveKey).
//                      INDEPENDENT of killable (Keese/Buzzblob/Geldman are
//                      killable but key-banned). A key-room replacement needs
//                      killable && !cannot_key.
//   ESF_WATER        — water-capable (Enemizer IsWaterSprite). Water-only rooms
//                      draw from these.
//   ESF_NEVER_DUNGEON / ESF_NEVER_OVERWORLD — directional bans (Enemizer
//                      NeverUseDungeon / NeverUseOverworld).
//   ESF_FLYING       — flying sprite; excluded from the per-room flying-exclude
//                      list (DontUseFlyingSprites).
// `sheets[]` lists the GFX sheet ids the sprite needs (union of Enemizer's
// AddSubgroupN constraints); ESF_SHEETS_COUNT terminator implied by trailing 0
// entries (sheet id 0 is never a real requirement here). A candidate is
// in-sheet only when EVERY listed sheet appears in the live 4-slot set.

enum {
  ESF_RANDOMIZABLE   = 1u << 0,
  ESF_KILLABLE       = 1u << 1,
  ESF_CANNOT_KEY     = 1u << 2,
  ESF_WATER          = 1u << 3,
  ESF_NEVER_DUNGEON  = 1u << 4,
  ESF_NEVER_OVERWORLD= 1u << 5,
  ESF_FLYING         = 1u << 6,
};

#define ES_MAX_SHEETS 3

typedef struct EnemyConstraint {
  uint8 flags;
  uint8 sheets[ES_MAX_SHEETS];  // required sheet ids; 0 = unused slot
} EnemyConstraint;

// Highest sprite-type byte we model (Ganon's bank ends at 0xF2 control markers;
// real enemies top out well below). 256 keeps indexing trivial + bounds-safe.
#define ES_TABLE_LEN 256

// Sheet widening — rewriting owned, unpinned subgroup slots to OTHER sheets for
// more enemy variety — is gated by this flag (defined early so the dungeon palette
// gate in candidate_eligible can see it). When enabled, the dungeon palette gate
// plus the verify-then-commit pass in EnemyShuffle_ReshuffleCurrentRoomSheets keep
// widened enemies palette-correct and every forced substitution fillable. Off ⇒
// all subgroup slots resolve to their vanilla sheets and the dungeon palette gate
// is inert — byte-identical to the pre-widening behavior (a clean rollback valve).
#define ES_RESHUFFLE_SLOTS 4
#define ES_ENABLE_SHEET_WIDENING 1

// Helper macros for terse, auditable initializers. Each enemy lists its
// Enemizer subgroup sheet requirements as the union of AddSubgroupN ids. When
// Enemizer lists MULTIPLE acceptable ids for one subgroup position (e.g.
// AddSubgroup2(28, 36)), we record only the FIRST/most-common id — a stricter
// (sound) requirement: the candidate is admitted only when that exact sheet is
// loaded. Over-restriction is safe (fewer picks); under-restriction crashes.
#define E_RAND(killflags, ...) { (ESF_RANDOMIZABLE | killflags), { __VA_ARGS__ } }

// The table. Everything not explicitly RANDOMIZABLE is do_not_randomize (the
// zero-initialized default), which covers NPCs, objects, overlords, bosses,
// mini-bosses, markers, absorbables, and every unknown/unsafe id — the
// conservative default the spec demands.
//
// Sheet ids are the Enemizer AddSubgroupN constants (= the fork's Decomp_spr
// sheet ids). Cross-checked entry-by-entry against SpriteRequirement.cs.
// GENERATED by assets/scripts/gen_enemy_shuffle_tables.py from the Enemizer
// (MIT) SpriteRequirement.cs — edit the SCRIPT, not this block. Each enemy lists
// the REQUIRED sheet for EVERY non-empty subgroup slot (across-slot is AND);
// within a slot Enemizer's OR-list resolves to the most-common vanilla sheet.
// Generating from source (not a hand summary) is what fixed the Walking Zora
// 12+68 omission ("hybrid zora+buzzblob") — listing all slots is the invariant.
static const EnemyConstraint kEnemyTable[ES_TABLE_LEN] = {
  [0x00] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY | ESF_FLYING), { 17 } },  // Raven
  [0x01] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY | ESF_FLYING), { 18 } },  // Vulture
  [0x08] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 12 } },  // Octorok (one-way)
  [0x0A] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 12 } },  // Octorok (four-way)
  [0x0D] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 17 } },  // Buzzblob
  [0x0E] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 22, 23 } },  // Snapdragon (slots 0+2)
  [0x11] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 22 } },  // Hinox
  [0x12] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 23 } },  // Moblin
  [0x13] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 30 } },  // Mini-Helmasaur
  [0x17] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 17 } },  // Bush Hoarder
  [0x18] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 30 } },  // Mini-Moldorm
  [0x19] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY | ESF_FLYING), { 21 } },  // Poe
  [0x22] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 22 } },  // Ropa
  [0x23] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 31 } },  // Red Bari
  [0x24] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 31 } },  // Blue Bari
  [0x26] = { (ESF_RANDOMIZABLE | 0), { 30 } },  // Hardhat Beetle (not killable)
  [0x41] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 73 } },  // Blue sword soldier
  [0x42] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 73 } },  // Green sword soldier
  [0x43] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 73 } },  // Red spear soldier
  [0x44] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 70, 73 } },  // Assault sword soldier (slots 0+1)
  [0x45] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 73 } },  // Green spear soldier
  [0x46] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 72, 73 } },  // Blue archer (slots 0+1)
  [0x47] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 72, 73 } },  // Green archer (slots 0+1)
  [0x48] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 70, 73 } },  // Red javelin soldier (slots 0+1)
  [0x49] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 70, 73 } },  // Red javelin soldier 2 (slots 0+1)
  [0x4A] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 70, 73 } },  // Red bomb soldier (slots 0+1)
  [0x4B] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 73, 19 } },  // HM-knight recruit (slots 1+2)
  [0x4C] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 18 } },  // Geldman
  [0x4E] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 44 } },  // Popo
  [0x4F] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 44 } },  // Popo2
  [0x51] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 16 } },  // Armos
  [0x56] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY | ESF_WATER), { 12, 68 } },  // Walking Zora (slots 2+3)
  [0x58] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 12 } },  // Crab
  [0x6A] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 70, 73 } },  // Ball-n-chain trooper (slots 0+1)
  [0x6D] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 28 } },  // Rat
  [0x6E] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 28 } },  // Rope
  [0x6F] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 28 } },  // Keese
  [0x71] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 47 } },  // Leever
  [0x83] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 46 } },  // Green Eyegore
  [0x8B] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 35 } },  // Gibdo
  [0x99] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 38 } },  // Pengator
  [0x9B] = { (ESF_RANDOMIZABLE | 0), { 41 } },  // Wizzrobe (not killable)
  [0xA5] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 40 } },  // Blue Zazak
  [0xA6] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 40 } },  // Red Zazak
  [0xA7] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 31 } },  // Stalfos
  [0xAA] = { (ESF_RANDOMIZABLE | ESF_KILLABLE | ESF_CANNOT_KEY), { 27 } },  // Pikit (shield-eater)
  [0xC3] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 40 } },  // Gibo (floating blob)
  [0xC9] = { (ESF_RANDOMIZABLE | ESF_KILLABLE), { 16 } },  // Tektite
};

// kSheetSlot[256]: canonical subgroup slot (0..3) for each required sheet, so the
// loaded-check is POSITION-aware — a sheet must be present in ITS slot, not merely
// loaded somewhere. This matters because Module05_LoadFile inits ALL four
// sprite_gfx_subset slots to sheet 70, and an area that doesn't own slot 3 inherits
// that leftover: a slot-0 sheet (70) then sits in slot 3, and a position-unaware
// "loaded anywhere" test would mis-admit an enemy that needs 70 in slot 0 (it draws
// slot-0 tiles from the area's real slot-0 sheet → garbage; playtest-caught on the
// Red Javelin Soldier 0x48). Indexed by sheet id; only enemy required-sheets are
// set. GENERATED by gen_enemy_shuffle_tables.py — edit the SCRIPT, not this block.
static const uint8 kSheetSlot[256] = {
  [12]=2, [16]=3, [17]=3, [18]=2, [19]=2, [21]=3, [22]=0, [23]=2, [27]=3, [28]=2,
  [30]=1, [31]=0, [35]=2, [38]=2, [40]=2, [41]=2, [44]=1, [46]=2, [47]=0, [68]=3,
  [70]=0, [72]=0, [73]=1,
};

// ---------------------------------------------------------------------------
// Per-room exclusion lists (ported from Enemizer; NOT derivable from room
// bytes). Room ids are standard ALTTP dungeon_room_index values.
//
// kFlyingExcludeRooms — rooms where a flying replacement strands/laggs
//   (DontUseFlyingSprites). kImmovableExcludeRooms — rooms where an immovable /
//   slow replacement can block a door/switch (DontUseImmovableSpritesRooms,
//   ~60 rooms). In BOTH lists we conservatively restrict the candidate pool to
//   killable ground enemies (no flying, and — since the MVP has no reliable
//   "immovable" flag — we simply require killable, which all our ground
//   candidates are). The lists are also used to reject obviously-unsafe rooms
//   for the corresponding classes.

static const uint16 kFlyingExcludeRooms[] = {
  210,  // Misery Mire 02 Wizzrobes
  268,  // Mimic Cave
};
#define kFlyingExcludeRoomsCount (sizeof(kFlyingExcludeRooms)/sizeof(kFlyingExcludeRooms[0]))

// Hard-exclude rooms: never substitute ANY sprite here (Mimic Cave + Agahnim's
// Tower final bridge — Enemizer's most-restricted rooms). Cheap and safe.
static const uint16 kHardExcludeRooms[] = {
  268,  // Mimic Cave (R268_MimicCave)
  64,   // Agahnim's Tower final bridge (R64_AgahnimsTower_FinalBridgeRoom)
};
#define kHardExcludeRoomsCount (sizeof(kHardExcludeRooms)/sizeof(kHardExcludeRooms[0]))

// Shutter / kill-clear rooms (Enemizer NeedKillable_doors via Room.IsShutterRoom)
// + the immovable-sprite rooms: in either, every replacement MUST be killable.
// The MVP enforces killable for ALL dungeon rooms (see EnemyShuffle_PickDungeon),
// so this list is currently advisory; kept for when the dungeon pool widens past
// "killable-only". Ported subset of DontUseImmovableSpritesRooms (the rooms most
// likely to softlock if blocked); a conservative restriction, not exhaustive.
static const uint16 kKillableRequiredRooms[] = {
  11, 25, 30, 38, 39, 54, 63, 64, 66, 70, 73, 75, 78, 85, 87, 95,
  101, 106, 116, 118, 125, 127, 131, 132, 133, 140, 141, 146, 149,
  152, 155, 156, 157, 158, 160, 170, 175, 179, 186, 187, 188, 198,
  203, 206, 208, 210, 213, 216, 220,
};
#define kKillableRequiredRoomsCount (sizeof(kKillableRequiredRooms)/sizeof(kKillableRequiredRooms[0]))

// ---------------------------------------------------------------------------
// Module state.
static bool g_enemy_shuffle_active = false;  // installed AND enabled
static uint64 g_enemy_shuffle_seed = 0;      // salted per-seed base

// Enemy-shuffle RNG salt: a recognizable constant XORed into the seed so this
// stream is independent of boss (0xB055A11F...) / drop (0xD0DDA1FF...) / prize
// streams. "ENEMYSHF" leetspeak-ish.
#define kEnemyShuffleSalt 0xE1E10F5Full

// ---------------------------------------------------------------------------
// Helpers.

static bool table_is_randomizable(uint8 type) {
  return (kEnemyTable[type].flags & ESF_RANDOMIZABLE) != 0;
}

// True iff every required sheet of `type` is present in the live 4-slot set.
static bool sheets_loaded(uint8 type, const uint8 live[4]) {
  const EnemyConstraint *c = &kEnemyTable[type];
  for (int i = 0; i < ES_MAX_SHEETS; i++) {
    uint8 need = c->sheets[i];
    if (need == 0) continue;  // unused entry
    // POSITION-AWARE: the required sheet must be loaded in ITS canonical slot
    // (kSheetSlot[need]), not merely present in some slot. Module05_LoadFile inits
    // all four slots to sheet 70, so a slot-0 sheet left over in slot 3 must NOT
    // satisfy a slot-0 requirement (else the substituted enemy draws slot-0 tiles
    // from the area's real slot-0 sheet → garbage). See kSheetSlot.
    if (live[kSheetSlot[need]] != need) return false;
  }
  return true;
}

static bool room_in_list(uint16 room, const uint16 *list, uint32 n) {
  for (uint32 i = 0; i < n; i++) if (list[i] == room) return true;
  return false;
}

// add-rando-trap-catalog — public: are ALL of `type`'s required GFX sheets loaded
// (in their canonical slots) for the room/area currently on screen? The Ambush
// trap uses this to only spawn enemies that render correctly (else it falls back),
// reusing the position-aware sheet logic above instead of guessing slots.
bool EnemyShuffle_SheetsLoadedFor(uint8 type) {
  const uint8 *subset = (const uint8 *)(g_ram + 0xC2FC);  // sprite_gfx_subset_0..3
  uint8 live[4] = { subset[0], subset[1], subset[2], subset[3] };
  return sheets_loaded(type, live);
}

enum {
  kEsResolvedContext_None = 0,
  kEsResolvedContext_Dungeon = 1,
  kEsResolvedContext_Overworld = 2,
};

#define kEsMaxSpriteScan 96  // defensive cap on room/area list walks

static uint16 ram_read_u16(uint32 addr) {
  return (uint16)(g_ram[addr] | (g_ram[addr + 1] << 8));
}

static void ram_write_u16(uint32 addr, uint16 value) {
  g_ram[addr] = (uint8)value;
  g_ram[addr + 1] = (uint8)(value >> 8);
}

static void clear_resolved_sheet_snapshot(void) {
  for (int i = 0; i < 4; i++)
    g_ram[kRam_EnemyShuffleLiveSubset + i] = 0;
  ram_write_u16(kRam_EnemyShuffleLiveKey, 0);
  g_ram[kRam_EnemyShuffleLiveContext] = kEsResolvedContext_None;
}

static void write_resolved_sheet_snapshot(uint8 context, uint16 key) {
  for (int i = 0; i < 4; i++)
    g_ram[kRam_EnemyShuffleLiveSubset + i] = g_ram[0xC2FC + i];
  ram_write_u16(kRam_EnemyShuffleLiveKey, key);
  g_ram[kRam_EnemyShuffleLiveContext] = context;
}

// Read the resolved loaded sprite sheet set for this exact room/area key. The
// equality check against the live subset globals catches any later sheet rewrite
// after the resolver snapshot; if that happens, fail closed to vanilla rather
// than picking an enemy whose graphics may no longer be loaded.
static bool read_resolved_sheets(uint8 context, uint16 key, uint8 out[4]) {
  if (g_ram[kRam_EnemyShuffleLiveContext] != context)
    return false;
  if (ram_read_u16(kRam_EnemyShuffleLiveKey) != key)
    return false;
  for (int i = 0; i < 4; i++) {
    uint8 sheet = g_ram[kRam_EnemyShuffleLiveSubset + i];
    if (sheet != g_ram[0xC2FC + i])
      return false;
    out[i] = sheet;
  }
  return true;
}

// A sprite can be sheet-compatible but still be wrong for the loader context:
// overworld and dungeon code paths differ in palette setup, draw variants, and
// spawn assumptions. Derive this context allowlist from the vanilla room/area
// sprite blobs instead of maintaining another hand-audited table. Manual
// ESF_NEVER_* bans still win for fork-specific hazards.
static uint8 g_enemy_vanilla_context[ES_TABLE_LEN];
static uint8 g_enemy_overworld_palette[ES_TABLE_LEN][32];  // 256-bit palette-id set per type
static bool g_enemy_vanilla_context_ready = false;

// Dungeon sprite-palette gate (the analog of g_enemy_overworld_palette). A dungeon
// room's palette "signature" is its kDungPalinfos (sp0l,sp5l,sp6l) triple
// (Dungeon_GetSpritePaletteSig). Distinct triples seen during the vanilla scan are
// interned into stable ids (<=41 kDungPalinfos entries); g_enemy_dungeon_palette[t]
// is the bitset of signatures enemy `t` is observed under in vanilla. Used only
// when ES_ENABLE_SHEET_WIDENING — see candidate_eligible / the reshuffle verify.
extern uint32 Dungeon_GetSpritePaletteSig(int room);  // src/dungeon.c (header-light)
#define kEsMaxDungPalSigs 64  // >= 41 distinct kDungPalinfos triples; == uint64 width
static uint32 g_dung_pal_sigs[kEsMaxDungPalSigs];
static uint32 g_dung_pal_sig_count = 0;
static uint64 g_enemy_dungeon_palette[ES_TABLE_LEN];  // signature-id set per type

enum {
  kEsAsset_DungeonSprites = 58,
  kEsAsset_DungeonSpriteOffs = 59,
  kEsAsset_OverworldSpriteOffs = 159,
  kEsAsset_OverworldSprites = 160,
  kEsAsset_OverworldSpritePalettes = 162,
};

static void mark_vanilla_context_type(uint8 type, uint8 context) {
  g_enemy_vanilla_context[type] |= context;
}

static void mark_overworld_palette_type(uint8 type, uint8 palette) {
  g_enemy_overworld_palette[type][palette >> 3] |= (uint8)(1u << (palette & 7));
}

// Intern a dungeon palette signature → stable id, appending if new (build phase).
// Returns 0xFF for the out-of-range sentinel or when the table is full.
static uint8 dung_pal_sig_id_intern(uint32 sig) {
  if (sig == 0xFFFFFFFFu) return 0xFF;
  for (uint32 i = 0; i < g_dung_pal_sig_count; i++)
    if (g_dung_pal_sigs[i] == sig) return (uint8)i;
  if (g_dung_pal_sig_count >= kEsMaxDungPalSigs) return 0xFF;
  g_dung_pal_sigs[g_dung_pal_sig_count] = sig;
  return (uint8)g_dung_pal_sig_count++;
}

// Find-only lookup (runtime): 0xFF if the signature was never seen in vanilla.
static uint8 dung_pal_sig_id_find(uint32 sig) {
  if (sig == 0xFFFFFFFFu) return 0xFF;
  for (uint32 i = 0; i < g_dung_pal_sig_count; i++)
    if (g_dung_pal_sigs[i] == sig) return (uint8)i;
  return 0xFF;
}

static void mark_dungeon_palette_type(uint8 type, uint8 sig_id) {
  if (sig_id < kEsMaxDungPalSigs)
    g_enemy_dungeon_palette[type] |= (1ull << sig_id);
}

static void invalidate_vanilla_context_table(void) {
  memset(g_enemy_vanilla_context, 0, sizeof(g_enemy_vanilla_context));
  memset(g_enemy_overworld_palette, 0, sizeof(g_enemy_overworld_palette));
  memset(g_enemy_dungeon_palette, 0, sizeof(g_enemy_dungeon_palette));
  g_dung_pal_sig_count = 0;
  g_enemy_vanilla_context_ready = false;
}

static bool vanilla_context_assets_ready(void) {
  return kDungeonSprites != NULL && kDungeonSpriteOffs != NULL &&
         kOverworldSprites != NULL && kOverworldSpriteOffs != NULL &&
         kOverworldSpritePalettes != NULL &&
         kDungeonSprites_SIZE > 0 &&
         kDungeonSpriteOffs_SIZE >= sizeof(kDungeonSpriteOffs[0]) &&
         kOverworldSprites_SIZE > 0 &&
         kOverworldSpriteOffs_SIZE >= sizeof(kOverworldSpriteOffs[0]) &&
         kOverworldSpritePalettes_SIZE > 0;
}

static void scan_dungeon_context_list(const uint8 *src, uint8 sig_id) {
  if (src == NULL) return;
  src++;  // leading sort byte
  for (int i = 0; src[0] != 0xff; src += 3, i++) {
    if (i >= kEsMaxSpriteScan) return;
    uint8 type = src[2];
    if (type == 0xe4) continue;  // control marker
    if (src[1] >= 0xe0) continue;  // overlord, not a normal sprite
    mark_vanilla_context_type(type, kEsResolvedContext_Dungeon);
    mark_dungeon_palette_type(type, sig_id);  // 0xFF (unknown sig) → no-op
  }
}

static void scan_overworld_context_list(const uint8 *src, uint8 palette) {
  if (src == NULL) return;
  for (int i = 0; src[0] != 0xff; src += 3, i++) {
    if (i >= kEsMaxSpriteScan) return;
    uint8 type = src[2];
    if (type == 0xf4) continue;  // sprite-count marker
    if (type >= 0xf3) continue;  // overlord, not a normal sprite
    mark_vanilla_context_type(type, kEsResolvedContext_Overworld);
    mark_overworld_palette_type(type, palette);
  }
}

static bool overworld_offset_palette(uint32 offset_index, uint8 *palette) {
  uint32 stage = offset_index / 144u;
  uint32 area = offset_index % 144u;
  uint32 pal_stage = (area < 64) ? stage : 3u;
  uint32 pal_index = (area & 63u) + pal_stage * 64u;
  if (pal_index >= kOverworldSpritePalettes_SIZE)
    return false;
  *palette = kOverworldSpritePalettes[pal_index];
  return true;
}

// Runtime counterpart of overworld_offset_palette: the index into the LIVE
// overworld_sprite_palettes table ($7FD40, g_ram 0xFD40) for `area`'s sprites.
// overworld_sprite_palettes is two regions (Sprite_LoadGraphicsProperties,
// overworld.c): [0x00..0x3F] = light world @ the current progress stage,
// [0x40..0x7F] = dark world (fixed, copied from kOverworldSpritePalettes+0xC0).
// Vanilla uploads overworld_sprite_palettes[overworld_screen_index], so dark-world
// screens (0x40..0x7F) read the dark region. Light areas index themselves; dark
// AND special areas (overworld_area_index has the 0x40 bit, and special areas are
// 0x80+) map into the dark region — which is exactly what the context table
// recorded (overworld_offset_palette forces pal_stage 3 for area>=64). Using
// (area & 63) here dropped the 0x40 bit and gated every dark/special area against
// the LIGHT palette, which differs from the dark palette in all 64 slots.
static uint8 overworld_area_palette_index(uint8 area) {
  return (area < 0x40) ? area : (uint8)(0x40 | (area & 0x3f));
}

static void build_vanilla_context_table(void) {
  if (g_enemy_vanilla_context_ready)
    return;
  invalidate_vanilla_context_table();
  if (!vanilla_context_assets_ready())
    return;

  if (kDungeonSprites != NULL && kDungeonSpriteOffs != NULL) {
    uint32 room_count = kDungeonSpriteOffs_SIZE / (uint32)sizeof(kDungeonSpriteOffs[0]);
    for (uint32 room = 0; room < room_count; room++) {
      uint16 off = kDungeonSpriteOffs[room];
      if (off < kDungeonSprites_SIZE) {
        // The room's palette signature comes from its header (kDungPalinfos);
        // intern it so every enemy in this room is marked seen-on that signature.
        // Out-of-range / headers-absent (e.g. selftest synthetic assets) → 0xFF,
        // which marks nothing (the dungeon palette gate then passes through).
        uint8 sig_id = dung_pal_sig_id_intern(Dungeon_GetSpritePaletteSig((int)room));
        scan_dungeon_context_list(kDungeonSprites + off, sig_id);
      }
    }
  }

  if (kOverworldSprites != NULL && kOverworldSpriteOffs != NULL) {
    uint32 list_count = kOverworldSpriteOffs_SIZE / (uint32)sizeof(kOverworldSpriteOffs[0]);
    for (uint32 i = 0; i < list_count; i++) {
      uint16 off = kOverworldSpriteOffs[i];
      uint8 palette;
      if (off < kOverworldSprites_SIZE && overworld_offset_palette(i, &palette))
        scan_overworld_context_list(kOverworldSprites + off, palette);
    }
  }

  g_enemy_vanilla_context_ready = true;
}

static bool candidate_allowed_in_context(uint8 type, bool is_dungeon, uint8 overworld_palette) {
  const EnemyConstraint *c = &kEnemyTable[type];
  if (!(c->flags & ESF_RANDOMIZABLE)) return false;
  if (is_dungeon && (c->flags & ESF_NEVER_DUNGEON)) return false;
  if (!is_dungeon && (c->flags & ESF_NEVER_OVERWORLD)) return false;
  build_vanilla_context_table();
  uint8 context = is_dungeon ? kEsResolvedContext_Dungeon : kEsResolvedContext_Overworld;
  if ((g_enemy_vanilla_context[type] & context) == 0)
    return false;
  if (!is_dungeon &&
      !(g_enemy_overworld_palette[type][overworld_palette >> 3] & (1u << (overworld_palette & 7))))
    return false;
  return true;
}

// Full candidate eligibility for type `t` against a live sheet set and context,
// SHARED by pick_replacement (the picker) and the reshuffle verify so the two can
// never drift. Tests everything except the RNG choice: the constraint flags, the
// context + overworld-palette gate (candidate_allowed_in_context), the dungeon
// palette gate (only when widening is enabled), and sheets-loaded.
// `dungeon_sig_id` is the room's dungeon palette signature id (0xFF = unknown /
// not applicable, in which case the dungeon palette gate passes through).
static bool candidate_eligible(uint8 t, const uint8 live[4],
                               bool require_killable, bool require_key_capable,
                               bool require_water, bool forbid_flying,
                               bool is_dungeon, uint8 overworld_palette,
                               uint8 dungeon_sig_id) {
  const EnemyConstraint *c = &kEnemyTable[t];
  if (!(c->flags & ESF_RANDOMIZABLE)) return false;
  if (require_killable && !(c->flags & ESF_KILLABLE)) return false;
  if (require_key_capable && (c->flags & ESF_CANNOT_KEY)) return false;
  if (require_water && !(c->flags & ESF_WATER)) return false;
  if (forbid_flying && (c->flags & ESF_FLYING)) return false;
  if (!candidate_allowed_in_context(t, is_dungeon, overworld_palette)) return false;
  // Dungeon palette gate: the candidate must be observed in vanilla under this
  // room's sprite-palette signature, or it would render with the wrong colors.
  // Active only with widening (off ⇒ the live set is the room's own vanilla
  // sheets, whose enemies are palette-correct by construction — byte-identical).
  if (ES_ENABLE_SHEET_WIDENING && is_dungeon && dungeon_sig_id != 0xFF &&
      !(g_enemy_dungeon_palette[t] & (1ull << dungeon_sig_id)))
    return false;
  if (!sheets_loaded(t, live)) return false;
  return true;
}

// Core pick. `key` keys the per-entry RNG (room/area id mixed with slot).
// `require_killable` / `require_key_capable` / `require_water` / `forbid_flying`
// constrain the candidate pool per the room/area context. Returns the chosen
// replacement type, or `vanilla_type` unchanged when no valid candidate exists
// (always a safe no-op).
static uint8 pick_replacement(uint64 key, uint8 vanilla_type,
                              const uint8 live[4],
                              bool require_killable,
                              bool require_key_capable,
                              bool require_water,
                              bool forbid_flying,
                              bool is_dungeon_context,
                              uint8 overworld_palette,
                              uint8 dungeon_sig_id) {
  // Build the candidate list deterministically (table order is fixed), filtered
  // by the context constraints + in-sheet test. Cap is generous.
  uint8 cands[ES_TABLE_LEN];
  uint32 ncand = 0;
  for (uint32 t = 0; t < ES_TABLE_LEN; t++) {
    if (!candidate_eligible((uint8)t, live, require_killable, require_key_capable,
                            require_water, forbid_flying, is_dungeon_context,
                            overworld_palette, dungeon_sig_id))
      continue;
    cands[ncand++] = (uint8)t;
  }
  if (ncand == 0) return vanilla_type;  // no safe swap → leave vanilla

  RandoRng rng;
  Rng_SeedFromU64(&rng, g_enemy_shuffle_seed ^ key);
  uint32 idx = Rng_NextRange(&rng, ncand);
  return cands[idx];
}

// ===========================================================================
// Sprite-group SHEET RESHUFFLE (design.md D4 — the variety unlock).
//
// The basic picker (EnemyShuffle_Pick*) only chooses among enemies whose GFX
// sheets are ALREADY loaded for the room/area, so swaps are same-family. The
// reshuffle widens that pool by RE-ASSIGNING which sprite GFX sheet loads into
// subgroup slots 0..3 at sheet-load time — so an Octorok room can load a different
// enemy-family sheet and the existing picker gets the wider pool for free (it
// reads the LIVE sprite_gfx_subset_* values we rewrite here).
//
// SCOPE: all four subgroup slots. A room/area may reshuffle only an OWNED slot
// that is provably free: every present sprite is either randomizable (the picker
// substitutes it) or a known non-randomizable type that does not need that slot.
// Unknowns and bosses pin all slots; dungeon overlords pin only the generated
// slots needed by their spawned sprite, while unknown/no-spawn/boss-spawning
// overlords and overworld overlords pin all slots. Enemizer's four position
// whitelists are DISJOINT (a sheet id belongs to exactly one slot), so each
// enemy's tiles land in their canonical VRAM region and the picker's
// "sheet loaded" test stays sound without a position-aware change.
//
// ANTI-GARBAGE: each dungeon slot pool is restricted to sheets that self-contain
// a killable + key-capable enemy, so after a swap the picker ALWAYS finds a valid
// substitution for that slot (no vanilla passthrough with a now-missing sheet) and
// dungeon key/shutter rooms stay fillable. The room's vanilla sheet is ALWAYS a
// candidate (owner constraint: true-random, vanilla-inclusive).
//
// INHERITANCE: kSpriteTilesets rows with a 0 in a slot INHERIT the prior room's
// sheet, so a reshuffle could leak into a later room that needs a specific sheet.
// We track the true VANILLA-resolved sheet per slot in a snapshot-safe g_ram
// shadow (kRam_EnemyShuffleVanSubset): eligible rooms reshuffle FROM it;
// ineligible rooms RESTORE it — so a leaked sheet can never reach a pinned slot.
//
// Determinism: per-(seed, room/area), reproducible for races, regenerated from
// (seed, settings) like the picker. Rides the existing enemy_shuffle activation
// (no separate settings axis — owner decision); off ⇒ this is never called.
// ===========================================================================

// Per-slot vanilla shadows live at kRam_EnemyShuffleVanSubset[0..3] (= 0x662..5,
// features.h reserved block). Snapshot-safe so a Ctrl+F1 restore keeps the
// inheritance chain intact. Slot s vanilla = tileset_row[s] if it loads one, else
// the inherited shadow.
#define kRam_EnemyShuffleVanSubset kRam_EnemyShuffleVanPos2  /* 0x662..0x665 */

// Distinct RNG salt for the sheet choice (independent of the pick salts above).
#define kEnemyShuffleSheetSalt 0x5348454554ull  // "SHEET"

// Per-slot reshuffle pools (GENERATED by gen_enemy_shuffle_tables.py). DUNGEON
// pools: each sheet self-contains a SINGLE-slot killable + key-capable enemy, so
// every reshuffled slot independently keeps key/shutter rooms fillable + the
// picker always finds a valid substitution. OVERWORLD pools: any sheet with a
// randomizable enemy in that slot (no killable constraint). Members are DISJOINT
// per slot (asserted by the generator) so the picker's position-unaware "sheet
// loaded" test stays sound. The room's vanilla sheet is ALWAYS added as a
// candidate at runtime (true-random, vanilla-inclusive).
static const uint8 kEsDungeonPool0[]   = { 22, 31, 47 };
static const uint8 kEsDungeonPool1[]   = { 44, 30 };
static const uint8 kEsDungeonPool2[]   = { 12, 23, 28, 46, 35, 40, 38 };
static const uint8 kEsDungeonPool3[]   = { 16 };
static const uint8 kEsOverworldPool0[] = { 22, 31, 47 };
static const uint8 kEsOverworldPool1[] = { 44, 30 };
static const uint8 kEsOverworldPool2[] = { 12, 18, 23, 28, 46, 35, 40, 38, 41 };
static const uint8 kEsOverworldPool3[] = { 17, 16, 27 };
static const uint8 *const kEsDungeonPool[ES_RESHUFFLE_SLOTS]   = { kEsDungeonPool0, kEsDungeonPool1, kEsDungeonPool2, kEsDungeonPool3 };
static const uint8 *const kEsOverworldPool[ES_RESHUFFLE_SLOTS] = { kEsOverworldPool0, kEsOverworldPool1, kEsOverworldPool2, kEsOverworldPool3 };
static const uint8 kEsDungeonPoolCount[ES_RESHUFFLE_SLOTS]   = { 3, 2, 7, 1 };
static const uint8 kEsOverworldPoolCount[ES_RESHUFFLE_SLOTS] = { 3, 2, 9, 3 };

// Per-type sheet-need classification (GENERATED from Enemizer SpriteRequirement.cs
// — edit gen_enemy_shuffle_tables.py, not this block). bit7 = KNOWN (the sprite
// has an Enemizer entry); bits 3..0 = needs subgroup slot 3..0. A non-randomizable
// present sprite PINS the slots it needs; an UNKNOWN (need == 0) or a boss pins
// (see below). embedded-data-guard: allow structural sprite-id→sheet/slot facts
// (per-type pin masks + the enemy/pool tables in this TU) — generated from the
// Enemizer MIT source, NOT extracted ROM data; the catalog is the crash surface.
// ALL reshuffled slots. This pin set is the crash-safety surface — a missing slot
// bit lets the picker spawn a sprite where part of its gfx isn't loaded.
static const uint8 kSheetNeed[256] = {
  [0x00]=0x88, [0x01]=0x84, [0x03]=0x80, [0x04]=0x88, [0x06]=0x88, [0x08]=0x84, [0x09]=0x84, [0x0A]=0x84,
  [0x0B]=0x88, [0x0D]=0x88, [0x0E]=0x85, [0x0F]=0x84, [0x10]=0x84, [0x11]=0x81, [0x12]=0x84, [0x13]=0x82,
  [0x14]=0x80, [0x15]=0x88, [0x16]=0x84, [0x17]=0x88, [0x18]=0x82, [0x19]=0x88, [0x1A]=0x8A, [0x1B]=0x80,
  [0x1C]=0x88, [0x1D]=0x80, [0x1E]=0x88, [0x1F]=0x81, [0x20]=0x84, [0x21]=0x88, [0x22]=0x81, [0x23]=0x81,
  [0x24]=0x81, [0x25]=0x81, [0x26]=0x82, [0x27]=0x88, [0x28]=0x80, [0x29]=0x81, [0x2A]=0x8F, [0x2B]=0x80,
  [0x2C]=0x84, [0x2D]=0x80, [0x2E]=0x80, [0x2F]=0x8F, [0x30]=0x8F, [0x31]=0x81, [0x32]=0x81, [0x33]=0x80,
  [0x34]=0x8F, [0x35]=0x80, [0x36]=0x84, [0x37]=0x80, [0x38]=0x80, [0x39]=0x88, [0x3A]=0x88, [0x3B]=0x80,
  [0x3C]=0x8F, [0x3D]=0x8F, [0x3E]=0x88, [0x3F]=0x80, [0x40]=0x88, [0x41]=0x82, [0x42]=0x82, [0x43]=0x82,
  [0x44]=0x83, [0x45]=0x82, [0x46]=0x83, [0x47]=0x83, [0x48]=0x83, [0x49]=0x83, [0x4A]=0x83, [0x4B]=0x86,
  [0x4C]=0x84, [0x4D]=0x88, [0x4E]=0x82, [0x4F]=0x82, [0x50]=0x84, [0x51]=0x88, [0x52]=0x88, [0x53]=0x88,
  [0x54]=0x88, [0x55]=0x84, [0x56]=0x8C, [0x57]=0x84, [0x58]=0x84, [0x59]=0x8C, [0x5A]=0x8C, [0x5B]=0x81,
  [0x5C]=0x81, [0x5D]=0x84, [0x5E]=0x84, [0x5F]=0x84, [0x60]=0x84, [0x61]=0x82, [0x62]=0x8C, [0x63]=0x81,
  [0x64]=0x81, [0x65]=0x81, [0x66]=0x81, [0x67]=0x81, [0x68]=0x81, [0x69]=0x81, [0x6A]=0x83, [0x6B]=0x83,
  [0x6C]=0x80, [0x6D]=0x84, [0x6E]=0x84, [0x6F]=0x84, [0x71]=0x81, [0x72]=0x88, [0x73]=0x81, [0x74]=0x8F,
  [0x75]=0x8F, [0x76]=0x80, [0x78]=0x87, [0x7A]=0x8F, [0x7B]=0x80, [0x7C]=0x81, [0x7D]=0x88, [0x7E]=0x81,
  [0x7F]=0x81, [0x80]=0x81, [0x81]=0x84, [0x82]=0x88, [0x83]=0x84, [0x84]=0x84, [0x86]=0x84, [0x88]=0x8C,
  [0x89]=0x84, [0x8A]=0x88, [0x8B]=0x84, [0x8C]=0x84, [0x8D]=0x84, [0x8E]=0x84, [0x8F]=0x82, [0x90]=0x84,
  [0x91]=0x82, [0x92]=0x8C, [0x93]=0x88, [0x94]=0x80, [0x95]=0x88, [0x96]=0x88, [0x97]=0x88, [0x98]=0x88,
  [0x99]=0x84, [0x9A]=0x84, [0x9B]=0x84, [0x9C]=0x82, [0x9D]=0x82, [0x9E]=0x84, [0x9F]=0x80, [0xA0]=0x84,
  [0xA1]=0x84, [0xA2]=0x84, [0xA3]=0x80, [0xA4]=0x84, [0xA5]=0x84, [0xA6]=0x84, [0xA7]=0x81, [0xA8]=0x88,
  [0xA9]=0x88, [0xAA]=0x88, [0xAB]=0x80, [0xAC]=0x80, [0xAD]=0x87, [0xAE]=0x80, [0xAF]=0x80, [0xB0]=0x80,
  [0xB1]=0x80, [0xB2]=0x81, [0xB3]=0x80, [0xB4]=0x88, [0xB5]=0x82, [0xB6]=0x88, [0xB7]=0x80, [0xB8]=0x82,
  [0xB9]=0x88, [0xBA]=0x80, [0xBB]=0x8F, [0xBC]=0x8F, [0xBD]=0x88, [0xBE]=0x88, [0xBF]=0x88, [0xC0]=0x84,
  [0xC1]=0x8F, [0xC2]=0x88, [0xC3]=0x84, [0xC4]=0x81, [0xC5]=0x80, [0xC6]=0x80, [0xC7]=0x84, [0xC8]=0x8C,
  [0xC9]=0x88, [0xCA]=0x84, [0xCB]=0x89, [0xCC]=0x89, [0xCD]=0x89, [0xCE]=0x86, [0xCF]=0x88, [0xD0]=0x88,
  [0xD1]=0x80, [0xD2]=0x80, [0xD3]=0x80, [0xD4]=0x80, [0xD5]=0x82, [0xD6]=0x8F, [0xD7]=0x80, [0xD8]=0x80,
  [0xD9]=0x80, [0xDA]=0x80, [0xDB]=0x80, [0xDC]=0x80, [0xDD]=0x80, [0xDE]=0x80, [0xDF]=0x80, [0xE0]=0x80,
  [0xE1]=0x80, [0xE2]=0x80, [0xE3]=0x80, [0xE4]=0x80, [0xE5]=0x80, [0xE6]=0x88, [0xE7]=0x88, [0xE8]=0x88,
  [0xE9]=0x89, [0xEA]=0x80, [0xEB]=0x80, [0xEC]=0x80, [0xED]=0x84, [0xEE]=0x81, [0xEF]=0x80, [0xF0]=0x80,
  [0xF1]=0x80, [0xF2]=0x84,
};

// Implemented in sprite.c / overworld.c — the asset-blob room/area sprite-data
// pointer, walked here for the room's TYPE list at sheet-load time (before
// Dungeon_LoadSprites / Overworld_LoadSprites parse it). Forward-declared to keep
// this TU free of the heavy sprite/overworld headers (same spirit as the g_ram
// direct reads). Signatures must match the definitions.
extern const uint8 *Dungeon_GetRoomSpritePtr(uint16 room);
extern const uint8 *GetOverworldSpritePtr(int area);

static bool type_is_known(uint8 t) { return (kSheetNeed[t] & 0x80) != 0; }
static bool type_needs_slot(uint8 t, int slot) {
  return (kSheetNeed[t] & (1u << slot)) != 0;
}

// Boss / boss-secondary sprite ids (Enemizer class=boss). A boss room is NEVER
// slot-2-eligible: boss shuffle can REDIRECT a vanilla boss room to host a
// different boss (Dungeon_LoadSprites src_room redirect, shuffle_boss.c), and a
// boss's sheets are loaded by the room header + spawned outside the picker — so a
// slot-2 reshuffle there would garbage the boss. Most bosses also need slot 2 (so
// already pin), but Armos Knights / Lanmolas / Vitreous / Trinexx need slot 3 and
// would otherwise slip through eligible; listing the full set is unambiguous.
static const uint8 kEsBossTypes[] = {
  0x09, 0x53, 0x54, 0x7A, 0x88, 0x8C, 0x8D, 0x92, 0xA2, 0xA3, 0xA4,
  0xBD, 0xBE, 0xBF, 0xC1, 0xCB, 0xCC, 0xCD, 0xCE, 0xD6
};
#define kEsBossCount (sizeof(kEsBossTypes)/sizeof(kEsBossTypes[0]))

static bool type_is_boss(uint8 t) {
  for (uint32 i = 0; i < kEsBossCount; i++)
    if (kEsBossTypes[i] == t) return true;
  return false;
}

// Dungeon overlord spawn-sheet need (GENERATED), indexed by overlord_type — the
// room-data src[2] of an x>=0xe0 entry (Dungeon_LoadSingleOverlord writes it to
// overlord_type[k]; Enemizer numbers the same overlord 0x100+type). bits 3..0 =
// the slots the overlord's SPAWNED sprite needs. A non-zero entry pins ONLY those
// slots (the spawner room can reshuffle the others); a 0 entry (incl. unknown
// overlords, MovingFloor, the boss-spawning ArmosCoordinator, BombTrap) makes the
// room pin ALL slots — conservative, since the spawned sprite bypasses the picker.
static const uint8 kOverlordNeed[32] = {
  [0x02]=0x04, [0x03]=0x04, [0x05]=0x01, [0x06]=0x04, [0x08]=0x02, [0x09]=0x04, [0x0A]=0x08, [0x0B]=0x08, [0x10]=0x04, [0x11]=0x04, [0x12]=0x04, [0x13]=0x04, [0x14]=0x08, [0x15]=0x04, [0x16]=0x02, [0x17]=0x01, [0x18]=0x01,
};

// A present sprite PINS the slots it needs (so we must not reshuffle them).
// Randomizable enemies never pin (the picker substitutes them). Pure.
static bool type_blocks_slot(uint8 t, int slot) {
  if (table_is_randomizable(t)) return false;
  if (type_is_boss(t) || !type_is_known(t)) return true;  // pins ALL slots
  return type_needs_slot(t, slot);
}

// Deterministic sheet choice for `slot`: uniform over the (dungeon|overworld)
// pool ∪ {van} (vanilla always a possible outcome). `van` is non-zero. Pure.
static uint8 choose_slot_sheet(int slot, uint8 van, uint64 key, bool is_dungeon) {
  const uint8 *pool = is_dungeon ? kEsDungeonPool[slot] : kEsOverworldPool[slot];
  uint32 pn = is_dungeon ? kEsDungeonPoolCount[slot] : kEsOverworldPoolCount[slot];
  uint8 cands[16];
  uint32 n = 0;
  bool van_in = false;
  for (uint32 i = 0; i < pn && n < 15; i++) {
    cands[n++] = pool[i];
    if (pool[i] == van) van_in = true;
  }
  if (!van_in) cands[n++] = van;
  RandoRng rng;
  Rng_SeedFromU64(&rng, key);
  return cands[Rng_NextRange(&rng, n)];
}

// Walk the room/area sprite TYPE list ONCE and return a bitmask of slots that
// must NOT be reshuffled (bit s = slot s pinned). An overlord (its spawns bypass
// the picker), a boss (boss-shuffle redirect), or an unknown sprite pins ALL
// reshuffled slots; a known non-randomizable sprite pins the slots it needs.
// Dungeon list: leading sort byte, {y,x,type} until 0xff, 0xe4=control,
// x>=0xe0=overlord. Overworld list: {b,b,type} until 0xff, 0xf4=count,
// type>=0xf3=overlord.
static uint8 room_blocked_slots(bool is_dungeon, uint16 key, bool *out_has_water) {
  const uint8 ALL = (uint8)((1u << ES_RESHUFFLE_SLOTS) - 1u);  // 0x0F
  const uint8 *src;
  if (is_dungeon) { src = Dungeon_GetRoomSpritePtr(key); src++; }
  else            { src = GetOverworldSpritePtr((int)key); }
  uint8 blocked = 0;
  if (out_has_water) *out_has_water = false;  // set if a water-only source is present
  // NON-SPRITE sheet consumers are invisible to the sprite-list walk below: the
  // pushable-grave ancilla (Ancilla24_Gravestone) draws OBJ page-1 chars
  // 0xC8/0xD8 = subgroup SLOT 3, so the graveyard screen (the only area with
  // pushable-grave tiles — kMoveGravestone_* covers exactly its 15 graves) must
  // pin slot 3 or pushing a grave renders another sheet's tiles (playtest-caught,
  // F12-confirmed: slot 3 reshuffled 14->17). The other page-1 non-sprite draws
  // are safe without a pin: the bed spread is slot 0 in Link's house, pinned by
  // the uncle's room-data entry; the Master Sword ceremony sparkle (slot 2) is
  // rando-gated OFF in misc.c; the Somaria block + item/effect ancillae draw
  // from the always-loaded OBJ page-0 common sheets.
  if (!is_dungeon && key == 0x14)
    blocked |= 1u << 3;
  for (int i = 0; src[0] != 0xff; src += 3, i++) {
    if (i >= kEsMaxSpriteScan) return ALL;  // malformed / unterminated → conservative
    uint8 type = src[2];
    if (is_dungeon) {
      if (type == 0xe4) continue;     // control entry
      if (src[1] >= 0xe0) {           // overlord — pin ONLY the slots its SPAWNED
        // sprite needs (decoded from overlord_type = src[2]); the rest of the
        // room can still reshuffle. A 0 entry (unknown / no-spawn-sheet / the
        // boss-spawning ArmosCoordinator) conservatively pins ALL slots.
        uint8 on = (type < 32) ? kOverlordNeed[type] : 0;
        if (on == 0) return ALL;
        blocked |= on;
        if (blocked == ALL) return ALL;
        continue;
      }
    } else {
      if (type == 0xf4) continue;     // sprite-count marker
      if (type >= 0xf3) return ALL;   // overlord (overworld decode is a future micro-opt)
    }
    if (table_is_randomizable(type)) {                  // substituted by the picker
      if (out_has_water && (kEnemyTable[type].flags & ESF_WATER)) *out_has_water = true;
      continue;
    }
    if (type_is_boss(type) || !type_is_known(type)) return ALL;
    blocked |= (uint8)(kSheetNeed[type] & ALL);         // pin the slots it needs
    if (blocked == ALL) return ALL;
  }
  return blocked;
}

// Verify the proposed live 4-slot set still admits >=1 valid forced-substitution
// target for this room/area, under the SAME constraints the picker applies (so a
// forced substitution can never fail into an unloaded-sheet render). Mirrors
// pick_replacement via the shared candidate_eligible. For a dungeon the target must
// be killable && !cannot_key, palette-compatible with `sig_id`, all sheets loaded,
// and non-flying in a flying-exclude room. `ow_pal` is the overworld area palette id
// (ignored for dungeons).
//
// `need_water` (the room/area holds a Walking Zora water source): a water source is
// never substituted to a DIFFERENT water sheet — Zora (0x56) is the sole ESF_WATER
// enemy, and the picker keeps it vanilla (key-banned, so a dungeon has no killable+
// key water candidate; the overworld's only water replacement is Zora itself). So the
// water demand reduces to "Zora's own sheets (12+68) stay loaded" so vanilla Zora
// still renders. Requiring an eligible *key-capable* water candidate (as the base
// scan's flags do) is unsatisfiable in dungeons and would wrongly suppress ALL
// widening in every dungeon room that holds a Zora.
static bool widen_set_fillable(const uint8 live[4], bool is_dungeon, uint8 sig_id,
                               bool forbid_flying, bool need_water, uint8 ow_pal) {
  // A present water source must keep its own sheets loaded (it renders vanilla; it is
  // never swapped to another water sheet). 0x56 = Walking Zora, the only ESF_WATER.
  if (need_water && !sheets_loaded(0x56, live))
    return false;
  // At least one base forced-substitution target must survive in the proposed set —
  // every non-water forced sprite can substitute to it (the picker is slot-agnostic).
  for (uint32 t = 0; t < ES_TABLE_LEN; t++) {
    if (candidate_eligible((uint8)t, live,
                           /*require_killable=*/is_dungeon,
                           /*require_key_capable=*/is_dungeon,
                           /*require_water=*/false,
                           forbid_flying, is_dungeon, ow_pal, sig_id))
      return true;
  }
  return false;
}

// Soul predicates for kill-gated and forced-key rooms name their vanilla
// residents. Type substitution and sheet widening must therefore share this
// exact pin: forcing the vanilla type after unloading its sheet can produce an
// invisible/garbled enemy that the player must kill to open the room.
static bool soul_room_pin_decision(bool enemies_tier, bool room_pinned) {
  return enemies_tier && room_pinned;
}

static bool soul_room_pin_active_for_tier(uint16 room, bool enemies_tier) {
  return soul_room_pin_decision(enemies_tier,
                                Souls_RoomPinnedVanilla(room));
}

static bool soul_room_pin_active(uint16 room) {
  return soul_room_pin_active_for_tier(room, Souls_EnemiesTierActive());
}

void EnemyShuffle_ReshuffleCurrentRoomSheets(const uint8 *tileset_row) {
  if (!g_enemy_shuffle_active || tileset_row == NULL) return;

  // Act on every gameplay room/area sheet load; stay inert for the
  // non-gameplay InitializeTilesets callers (attract, select-file, ending,
  // dungeon-map, flute/world-map). Dungeon loads run under 0x07 room-to-room,
  // but the ENTRANCE load runs under 0x06 (Module_PreDungeon — it flips
  // main_module_index to 7 only afterwards; also every dungeon-chain hop) and
  // a falling entrance under 0x11 (Module11_02_LoadEntrance). The overworld
  // side runs under 0x08 (load) / 0x09 (transition) plus 0x0A (the
  // PreOverworld load returning from a special-overworld area). In all six the
  // key + sprite data are valid before the InitializeTilesets call and the
  // picker consumes the committed sheets strictly later: Dungeon_LoadEntrance
  // sets dungeon_room_index(2) first and Dungeon_ResetSprites runs after
  // (dungeon.c Module_PreDungeon; Module11 case 3), and
  // LoadOverworldFromSpecialOverworld restores overworld_area_index before
  // PreOverworld_LoadProperties reaches InitializeTilesets, with
  // Sprite_ReloadAll_Overworld after (overworld.c).
  uint8 module = g_ram[0x10];                 // main_module_index
  bool is_dungeon = (module == 0x06 || module == 0x07 || module == 0x11);
  bool is_ow = (module == 0x08 || module == 0x09 || module == 0x0A);
  if (!is_dungeon && !is_ow) return;
  if (g_ram[0xAA3] >= 0x80) return;           // sprite_graphics_index: 0x80| = map preview
  uint8 context = is_dungeon ? kEsResolvedContext_Dungeon : kEsResolvedContext_Overworld;

  // Key on dungeon_room_index (0xA0), NOT dungeon_room_index2 (0x48E): at
  // sheet-load time _index is the room being entered (its header drove the loaded
  // sheets) while _index2 is still the PREVIOUS room — it is assigned `= _index`
  // one line before Dungeon_LoadSprites runs (e.g. Module07_02_01_LoadNextRoom,
  // dungeon.c). So the hook (reads _index) walks the same room the
  // picker (reads _index2 == _index by then) substitutes — they converge; reading
  // _index2 here would instead walk the PREVIOUS room's list.
  uint16 key16 = is_dungeon
      ? (uint16)(g_ram[0xA0] | (g_ram[0xA1] << 8))    // dungeon_room_index
      : (uint16)(g_ram[0x40A] | (g_ram[0x40B] << 8)); // overworld_area_index
  // Resolve which slots may widen. Default: pin everything (no widening). With
  // widening on, walk the room/area's sprite list for pins + a water-source flag,
  // and resolve the room's palette signature for the verify/gate. An unknown
  // dungeon signature (out of range / headers absent) means we cannot verify
  // palette safety → don't widen.
  uint8 blocked = (uint8)((1u << ES_RESHUFFLE_SLOTS) - 1u);
  bool has_water = false;
  uint8 sig_id = 0xFF;
  bool forbid_flying = false;
  uint8 ow_pal = 0;
  if (ES_ENABLE_SHEET_WIDENING) {
    build_vanilla_context_table();
    if (!(is_dungeon && soul_room_pin_active(key16)))
      blocked = room_blocked_slots(is_dungeon, key16, &has_water);
    if (is_dungeon) {
      sig_id = dung_pal_sig_id_find(Dungeon_GetSpritePaletteSig(key16));
      forbid_flying = room_in_list(key16, kFlyingExcludeRooms, kFlyingExcludeRoomsCount);
      if (sig_id == 0xFF)
        blocked = (uint8)((1u << ES_RESHUFFLE_SLOTS) - 1u);
    } else {
      ow_pal = g_ram[0xFD40 + overworld_area_palette_index((uint8)key16)];
    }
  }

  // Phase 1: resolve the vanilla sheet + a proposed (possibly widened) sheet per
  // slot. `proposed[]` is the live set we will commit; `vanres[]` is the always-safe
  // fallback. A slot the room does not yet own (van == 0) keeps its inherited
  // loaded sheet, which `proposed` mirrors so the verify sees the true live set.
  uint8 proposed[ES_RESHUFFLE_SLOTS];
  uint8 vanres[ES_RESHUFFLE_SLOTS];
  for (int slot = 0; slot < ES_RESHUFFLE_SLOTS; slot++) {
    bool owns = (tileset_row[slot] != 0);
    uint8 van = owns ? tileset_row[slot] : g_ram[kRam_EnemyShuffleVanSubset + slot];
    vanres[slot] = van;
    if (van != 0)
      g_ram[kRam_EnemyShuffleVanSubset + slot] = van;  // update the inheritance shadow
    uint8 chosen = (van != 0) ? van : g_ram[0xC2FC + slot];
    bool reshuffle = (van != 0) && owns && !(blocked & (1u << slot));
    if (reshuffle) {
      uint64 k = g_enemy_shuffle_seed ^ ((uint64)key16 << (slot * 8)) ^
                 (kEnemyShuffleSheetSalt + (uint64)slot * 0x1000003ull);
      chosen = choose_slot_sheet(slot, van, k, is_dungeon);
    }
    proposed[slot] = chosen;
  }

  // Phase 2: verify-then-commit. Widening a slot removes the room's own sheet there
  // and FORCES its enemies to substitute; if the proposed set admits no valid
  // forced-substitution target (palette-compatible + constraint-meeting + sheets
  // loaded), revert widened slots — highest first, deterministic — toward vanilla.
  // Loop exits two ways, both safe: (a) the verify passes, or (b) every widened slot
  // has been reverted (proposed == vanres, the shipped config — always safe because
  // the picker returns the vanilla type for any source it cannot fill, and the
  // vanilla sheets are loaded). No-op when widening is off (no slot was widened).
  if (ES_ENABLE_SHEET_WIDENING) {
    for (;;) {
      if (widen_set_fillable(proposed, is_dungeon, sig_id, forbid_flying, has_water, ow_pal))
        break;
      int revert = -1;
      for (int slot = ES_RESHUFFLE_SLOTS - 1; slot >= 0; slot--) {
        if (vanres[slot] != 0 && proposed[slot] != vanres[slot]) { revert = slot; break; }
      }
      if (revert < 0) break;  // fully vanilla — nothing widened, no forced substitutions
      proposed[revert] = vanres[revert];
    }
  }

  // Commit the resolved set. Slots the room does not yet own (van == 0) keep their
  // inherited loaded sheet untouched (matching the prior behavior).
  for (int slot = 0; slot < ES_RESHUFFLE_SLOTS; slot++) {
    if (vanres[slot] == 0) continue;
    g_ram[0xC2FC + slot] = proposed[slot];  // sprite_gfx_subset_{slot} — picker + decompress
  }
  write_resolved_sheet_snapshot(context, key16);
}

// ---------------------------------------------------------------------------
// Public API.

bool EnemyShuffle_Generate(const struct RandoSettings *settings,
                           uint64 seed_u64) {
  if (settings == NULL || settings->enemy_shuffle == 0) {
    // Off → inactive (load paths take the vanilla branch → byte-identical).
    EnemyShuffle_Deactivate();
    return true;
  }
  g_enemy_shuffle_seed = seed_u64 ^ kEnemyShuffleSalt;
  g_enemy_shuffle_active = true;
  // Reset the per-slot reshuffle inheritance shadows to 0 = "not established". The
  // first room that loads its OWN slot s (tileset_row[s] != 0) establishes it;
  // until then a room that INHERITS slot s (van == 0) is left untouched by the
  // van == 0 guard — avoids reshuffling/restoring from the stale file-select/menu
  // sheet that happens to be loaded at slot activation.
  for (int s = 0; s < ES_RESHUFFLE_SLOTS; s++)
    g_ram[kRam_EnemyShuffleVanSubset + s] = 0;
  clear_resolved_sheet_snapshot();
  return true;
}

void EnemyShuffle_Deactivate(void) {
  g_enemy_shuffle_active = false;
  g_enemy_shuffle_seed = 0;
  for (int s = 0; s < ES_RESHUFFLE_SLOTS; s++)
    g_ram[kRam_EnemyShuffleVanSubset + s] = 0;  // drop the inheritance shadows
  clear_resolved_sheet_snapshot();
}

bool EnemyShuffle_IsActive(void) {
  return g_enemy_shuffle_active;
}

uint8 EnemyShuffle_PickDungeon(uint16 room, uint8 slot, uint8 vanilla_type) {
  if (!g_enemy_shuffle_active) return vanilla_type;
  // Only substitute a recognized randomizable enemy (markers/overlords/NPCs/
  // objects/bosses pass through). The caller already skips control (0xe4) /
  // overlord (x>=0xe0) entries before the type read, but guard anyway.
  if (!table_is_randomizable(vanilla_type)) return vanilla_type;
  // Hard-exclude rooms: never touch.
  if (room_in_list(room, kHardExcludeRooms, kHardExcludeRoomsCount))
    return vanilla_type;
  // add-enemy-souls — under the enemies tier, kill-gated + forced key-drop
  // rooms keep their VANILLA species: the baked logic soul terms and the
  // runtime drop-source exemption both name the vanilla residents
  // (soul_rooms.gen.yaml -> kRandoSoulPinRooms). Substituting here would make
  // the placer require a soul the spawned species doesn't answer to (or vice
  // versa). Other rooms shuffle freely; suppression keys on the final species.
  if (soul_room_pin_active(room))
    return vanilla_type;

  uint8 live[4];
  if (!read_resolved_sheets(kEsResolvedContext_Dungeon, room, live))
    return vanilla_type;

  // Conservative dungeon policy (MVP): EVERY dungeon replacement must be
  // killable && key-capable (so any key a shuffled item placement drops here is
  // obtainable, and any shutter/kill-clear door opens — beatability is the SOLE
  // job of this table). Flying is forbidden in the flying-exclude rooms.
  bool forbid_flying = room_in_list(room, kFlyingExcludeRooms, kFlyingExcludeRoomsCount);
  uint64 key = ((uint64)room << 8) ^ (uint64)slot ^ 0x000000000000D000ull;
  // A water-only source (ESF_WATER, e.g. Walking Zora) must be replaced by a
  // water-capable enemy, not a land one — drive the constraint from the source.
  bool require_water = (kEnemyTable[vanilla_type].flags & ESF_WATER) != 0;
  // Resolve this room's dungeon palette signature id for the palette gate. Build
  // the context/signature tables first (idempotent) so the find is populated.
  build_vanilla_context_table();
  uint8 dungeon_sig_id = dung_pal_sig_id_find(Dungeon_GetSpritePaletteSig(room));
  return pick_replacement(key, vanilla_type, live,
                          /*require_killable=*/true,
                          /*require_key_capable=*/true,
                          require_water,
                          /*forbid_flying=*/forbid_flying,
                          /*is_dungeon_context=*/true,
                          /*overworld_palette=*/0,
                          dungeon_sig_id);
}

uint8 EnemyShuffle_PickOverworld(uint8 area, uint8 slot, uint8 vanilla_type) {
  if (!g_enemy_shuffle_active) return vanilla_type;
  if (!table_is_randomizable(vanilla_type)) return vanilla_type;

  uint8 live[4];
  if (!read_resolved_sheets(kEsResolvedContext_Overworld, area, live))
    return vanilla_type;

  // Overworld has no key/shutter rooms, so no killable/key constraint is needed
  // for beatability. We still keep the pool to randomizable enemies whose sheet
  // is loaded (anti-crash) and honor the never_overworld directional ban.
  uint64 key = ((uint64)area << 8) ^ (uint64)slot ^ 0x0000000000000700ull;
  // Keep a water-only source (ESF_WATER) in water — derive from the source type.
  bool require_water = (kEnemyTable[vanilla_type].flags & ESF_WATER) != 0;
  uint8 overworld_palette = g_ram[0xFD40 + overworld_area_palette_index(area)];
  return pick_replacement(key, vanilla_type, live,
                          /*require_killable=*/false,
                          /*require_key_capable=*/false,
                          require_water,
                          /*forbid_flying=*/false,
                          /*is_dungeon_context=*/false,
                          overworld_palette,
                          /*dungeon_sig_id=*/0xFF);
}

// ---------------------------------------------------------------------------
// Enemy STAT randomization (HP / contact damage) — see shuffle_enemies.h.
// Deterministic per (seed, sprite type); always-on with enemy_shuffle. A dedicated
// RNG stream (distinct salt) so it doesn't perturb the sheet/pick streams.
#define kEnemyShuffleStatSalt 0x57A7570000ull  // "STATs"

// Bosses are EXCLUDED from stat scaling. They're already excluded from
// substitution + reshuffle; the stat axis was the only leak. The hard reason:
// Helmasaur King (0x92) indexes kHelmasaurKing_Tab1[13] by `sprite_health >> 2`
// (sprite_main.c) — vanilla HP 48 → index 12 (the last valid slot); ANY upward HP
// scale reads OUT OF BOUNDS. Other bosses self-clamp or
// gate on damage-underflow sentinels, but scaling them is tedious/pointless and
// the table-index hazard makes a blanket boss exemption the safe, simple rule.
uint8 EnemyShuffle_ScaleHealth(uint8 type, uint8 base) {
  if (!g_enemy_shuffle_active || base == 0) return base;  // 0 HP = non-killable
  if (type_is_boss(type)) return base;                    // bosses keep vanilla HP
  // Only perturb genuine randomizable combat enemies. Non-randomizable specials
  // / NPCs / objects (the same set substitution leaves alone) keep vanilla HP, as
  // does the 0xFF "invulnerable sentinel" — scaling it could drop it below the
  // sentinel and make an intended-invulnerable sprite killable.
  if (!table_is_randomizable(type) || base == 0xFF) return base;
  RandoRng rng;
  Rng_SeedFromU64(&rng, g_enemy_shuffle_seed ^ ((uint64)type << 8) ^ kEnemyShuffleStatSalt);
  uint32 mult = 8 + Rng_NextRange(&rng, 25);   // [8,32]/16 = 0.5x .. 2.0x
  uint32 v = ((uint32)base * mult) / 16u;
  if (v < 1) v = 1;
  if (v > 255) v = 255;
  return (uint8)v;
}

uint8 EnemyShuffle_ScaleDamage(uint8 type, uint8 base) {
  if (!g_enemy_shuffle_active) return base;
  if (type_is_boss(type)) return base;  // bosses keep vanilla contact damage too
  // Same randomizable-only gate as ScaleHealth: leave non-combat / non-randomizable
  // sprites' contact damage exactly as vanilla.
  if (!table_is_randomizable(type)) return base;
  // Only a PLAIN damage class (1..8, no high flag bits) is perturbed; flagged /
  // boss-attack / immunity values (>= 0x10) are left exactly as vanilla.
  if (base == 0 || base >= 0x10) return base;
  uint32 cls = base & 0x0fu;
  if (cls < 1 || cls > 8) return base;
  // Class 2 is the vanilla "NO contact damage" class: kPlayerDamages[3*2+armor] is
  // {0,0,0} (src/sprite.c) for every armor level. It must be excluded from scaling, or
  // an enemy ends up dealing ZERO contact damage (playtest-caught "enemies do no
  // damage"). A vanilla class-2 enemy is zero-contact BY DESIGN (it harms via
  // projectiles/special attacks), so leave it untouched rather than wrongly granting it
  // contact damage; and never nudge a damaging enemy INTO class 2 below.
  if (cls == 2) return base;
  RandoRng rng;
  Rng_SeedFromU64(&rng, g_enemy_shuffle_seed ^ ((uint64)type << 8) ^ (kEnemyShuffleStatSalt + 1u));
  int nc = (int)cls + ((int)Rng_NextRange(&rng, 3) - 1);  // -1 / 0 / +1
  if (nc < 1) nc = 1;
  if (nc > 8) nc = 8;
  if (nc == 2) nc = (int)cls;  // skip the zero-damage class — keep the vanilla class
  return (uint8)nc;  // base < 0x10 ⇒ no flag bits to preserve
}

// ---------------------------------------------------------------------------
// Self-check (--rando-selftest). Enemy shuffle is orthogonal to item placement
// (the corpus is blind to it), so determinism + the structural invariants can
// ONLY be pinned here. exit(2) on any failure.
// ---------------------------------------------------------------------------
static void enemy_selfcheck_die(const char *msg) {
  fprintf(stderr, "[EnemyShuffle_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

// GT mini-boss + boss sprite ids that MUST be excluded (design.md D7 / Task 1.5).
// If any of these ever became RANDOMIZABLE the GT mini-boss gauntlet's
// CanKill<Boss> gates (or a boss fight) would break.
static const uint8 kMustExcludeBossIds[] = {
  0x09, 0x53, 0x54,  // GT mini-bosses: Moldorm / Armos Knights / Lanmolas
  0x88, 0x8C, 0x92, 0xA2, 0xBD, 0xCB, 0xCE,  // Mothula/Arrghus/Helmasaur/Kholdstare/Vitreous/Trinexx/Blind
  0x7A,              // Agahnim 1/2
  0xA3, 0xCC, 0xCD,  // boss secondaries: Kholdstare shell / Trinexx arms
  0xD6,              // Ganon
};

void EnemyShuffle_SelfCheck(void) {
  RandoSettings s;
  Settings_SetDefaults(&s);
  bool installed_synthetic_context_assets = false;
  const uint8 *saved_context_asset_ptrs[5];
  uint32 saved_context_asset_sizes[5];

  // --rando-selftest runs before LoadAssets(), but the context scanner should
  // still be exercised. Install tiny vanilla-like sprite blobs only for this
  // process when the real asset blob is absent; runtime with absent assets stays
  // fail-closed because this harness is self-check local.
  if (!vanilla_context_assets_ready()) {
    static const uint8 synth_dungeon_sprites[] = {
      0x00,                 // sort byte
      0x00, 0x00, 0x6D,     // Rat: dungeon-context spot check
      0x00, 0x00, 0x41,     // sword soldier: sheet-73 dungeon pick candidate
      0xff
    };
    static const uint16 synth_dungeon_offs[] = { 0 };
    static const uint8 synth_overworld_sprites[] = {
      0x00, 0x00, 0x08,     // Octorok: overworld-context spot check
      0x00, 0x00, 0x12,     // Moblin: sheet-23 overworld pick candidate
      0xff,
      0x00, 0x00, 0x4A,     // Bomb guard: allowed only on its vanilla palette
      0xff
    };
    enum { kSynthOverworldPal3Off = 7 };
    static const uint16 synth_overworld_offs[] = { 0, kSynthOverworldPal3Off };
    static const uint8 synth_overworld_palettes[] = { 0, 3 };
    const uint32 idx[5] = {
      kEsAsset_DungeonSprites,
      kEsAsset_DungeonSpriteOffs,
      kEsAsset_OverworldSpriteOffs,
      kEsAsset_OverworldSprites,
      kEsAsset_OverworldSpritePalettes,
    };
    for (int i = 0; i < 5; i++) {
      saved_context_asset_ptrs[i] = g_asset_ptrs[idx[i]];
      saved_context_asset_sizes[i] = g_asset_sizes[idx[i]];
    }
    g_asset_ptrs[kEsAsset_DungeonSprites] = synth_dungeon_sprites;
    g_asset_sizes[kEsAsset_DungeonSprites] = sizeof(synth_dungeon_sprites);
    g_asset_ptrs[kEsAsset_DungeonSpriteOffs] = (const uint8 *)synth_dungeon_offs;
    g_asset_sizes[kEsAsset_DungeonSpriteOffs] = sizeof(synth_dungeon_offs);
    g_asset_ptrs[kEsAsset_OverworldSpriteOffs] = (const uint8 *)synth_overworld_offs;
    g_asset_sizes[kEsAsset_OverworldSpriteOffs] = sizeof(synth_overworld_offs);
    g_asset_ptrs[kEsAsset_OverworldSprites] = synth_overworld_sprites;
    g_asset_sizes[kEsAsset_OverworldSprites] = sizeof(synth_overworld_sprites);
    g_asset_ptrs[kEsAsset_OverworldSpritePalettes] = synth_overworld_palettes;
    g_asset_sizes[kEsAsset_OverworldSpritePalettes] = sizeof(synth_overworld_palettes);
    invalidate_vanilla_context_table();
    installed_synthetic_context_assets = true;
  }

  // 1) Off → inactive passthrough.
  {
    RandoSettings off = s; off.enemy_shuffle = 0;
    EnemyShuffle_Deactivate();
    if (!EnemyShuffle_Generate(&off, 0x0123456789ABCDEFull))
      enemy_selfcheck_die("Generate(off) must succeed");
    if (EnemyShuffle_IsActive())
      enemy_selfcheck_die("enemy_shuffle off must be inactive");
    if (EnemyShuffle_PickDungeon(0x52, 0, 0x08) != 0x08)
      enemy_selfcheck_die("inactive PickDungeon must be a passthrough");
    if (EnemyShuffle_PickOverworld(0x00, 0, 0x08) != 0x08)
      enemy_selfcheck_die("inactive PickOverworld must be a passthrough");
  }

  // 2) Bosses + GT mini-bosses + secondaries are NEVER randomizable.
  for (uint32 i = 0; i < sizeof(kMustExcludeBossIds); i++) {
    if (table_is_randomizable(kMustExcludeBossIds[i]))
      enemy_selfcheck_die("a boss / mini-boss / secondary id is randomizable (D7 violation)");
  }
  // Markers/NPCs/objects we must never touch (spot check).
  if (table_is_randomizable(0xe4) || table_is_randomizable(0x76) /*Zelda*/ ||
      table_is_randomizable(0x1c) /*Statue*/ || table_is_randomizable(0xd8) /*Heart*/)
    enemy_selfcheck_die("a marker/NPC/object/absorbable id is randomizable");
  if (table_is_randomizable(0x6B))
    enemy_selfcheck_die("Cannon soldier is randomizable despite invalid vanilla spawn path");
  if (!candidate_allowed_in_context(0x12, false, 0))
    enemy_selfcheck_die("Moblin must remain an overworld-eligible palette-0 candidate");
  if (!candidate_allowed_in_context(0x6D, true, 0))
    enemy_selfcheck_die("Rat must remain a dungeon-eligible vanilla-context candidate");
  if (candidate_allowed_in_context(0x44, false, 0))
    enemy_selfcheck_die("Assault sword soldier is overworld-eligible despite vanilla-context guard");
  if (candidate_allowed_in_context(0x6A, false, 0))
    enemy_selfcheck_die("Ball-n-chain trooper is overworld-eligible despite vanilla-context guard");
  if (candidate_allowed_in_context(0x6B, true, 0) || candidate_allowed_in_context(0x6B, false, 0))
    enemy_selfcheck_die("Cannon soldier is context-eligible despite being non-randomizable");
  if (!candidate_allowed_in_context(0x4A, false, 3))
    enemy_selfcheck_die("Bomb guard must remain overworld-eligible on its vanilla palette");
  if (candidate_allowed_in_context(0x4A, false, 0))
    enemy_selfcheck_die("Bomb guard is overworld-eligible on a non-vanilla palette");

  // Dark-world palette-read regression: EnemyShuffle_PickOverworld resolves the
  // area's sprite-palette id via overworld_area_palette_index, which MUST keep the
  // dark-world 0x40 bit. Pre-fix it used (area & 63), reading the light-world
  // palette region for every dark-world / special area — the dark and light
  // regions differ in all 64 slots, so the overworld palette gate compared against
  // the wrong reference across the entire dark world (over-suppressing valid
  // substitutions and still admitting off-palette ones). overworld_sprite_palettes
  // is light [0x00..0x3F] / dark [0x40..0x7F]; special areas (0x80+) map into the
  // dark region to match the context table (overworld_offset_palette pal_stage 3).
  if (overworld_area_palette_index(0x12) != 0x12 ||
      overworld_area_palette_index(0x3f) != 0x3f)
    enemy_selfcheck_die("light-world overworld palette index must be the area itself");
  if (overworld_area_palette_index(0x40) != 0x40 ||
      overworld_area_palette_index(0x52) != 0x52 ||
      overworld_area_palette_index(0x7f) != 0x7f)
    enemy_selfcheck_die("dark-world overworld palette index dropped the 0x40 bit (area&63 regression)");
  if (overworld_area_palette_index(0x80) != 0x40 ||
      overworld_area_palette_index(0x81) != 0x41 ||
      overworld_area_palette_index(0x8f) != 0x4f)
    enemy_selfcheck_die("special-area overworld palette index must map into the dark region");

  // 3) Table integrity: every RANDOMIZABLE entry has >=1 required sheet (a
  // sheet-less entry would match every room and could load garbage), and every
  // CANNOT_KEY-free killable entry is a genuine enemy id (<0xF3).
  for (uint32 t = 0; t < ES_TABLE_LEN; t++) {
    const EnemyConstraint *c = &kEnemyTable[t];
    if (!(c->flags & ESF_RANDOMIZABLE)) continue;
    if (t >= 0xF3) enemy_selfcheck_die("randomizable id in the overlord/marker range (>=0xF3)");
    bool any_sheet = false;
    for (int i = 0; i < ES_MAX_SHEETS; i++) if (c->sheets[i]) any_sheet = true;
    if (!any_sheet) enemy_selfcheck_die("randomizable enemy has no required sheet (would match any room)");
  }
  // There MUST be at least one killable && key-capable candidate (else dungeon
  // key rooms could never be filled and beatability would silently fail).
  {
    bool any_keyable = false;
    bool any_dungeon_keyable = false;
    bool any_overworld = false;
    for (uint32 t = 0; t < ES_TABLE_LEN; t++) {
      const EnemyConstraint *c = &kEnemyTable[t];
      if ((c->flags & ESF_RANDOMIZABLE) && (c->flags & ESF_KILLABLE) &&
          !(c->flags & ESF_CANNOT_KEY)) {
        any_keyable = true;
        if (candidate_allowed_in_context((uint8)t, true, 0))
          any_dungeon_keyable = true;
      }
      if (candidate_allowed_in_context((uint8)t, false, 0))
        any_overworld = true;
    }
    if (!any_keyable) enemy_selfcheck_die("no killable+key-capable candidate exists");
    if (!any_dungeon_keyable) enemy_selfcheck_die("no dungeon-context killable+key-capable candidate exists");
    if (!any_overworld) enemy_selfcheck_die("no overworld-context candidate exists");
  }

  // 3b) Dungeon palette signature interning (pure): stable + deduped + sentinel.
  // Uses the real globals as scratch, then clears them (a later build repopulates).
  {
    g_dung_pal_sig_count = 0;
    memset(g_dung_pal_sigs, 0, sizeof(g_dung_pal_sigs));
    uint8 a = dung_pal_sig_id_intern(0x010203u);
    uint8 b = dung_pal_sig_id_intern(0x040506u);
    uint8 a2 = dung_pal_sig_id_intern(0x010203u);
    if (a != 0 || b != 1 || a2 != 0)
      enemy_selfcheck_die("dungeon palette signature interning must be stable + deduped");
    if (dung_pal_sig_id_find(0x040506u) != 1 || dung_pal_sig_id_find(0x778899u) != 0xFF)
      enemy_selfcheck_die("dungeon palette signature find inconsistent with intern");
    if (dung_pal_sig_id_intern(0xFFFFFFFFu) != 0xFF || dung_pal_sig_id_find(0xFFFFFFFFu) != 0xFF)
      enemy_selfcheck_die("out-of-range dungeon palette signature must resolve to 0xFF");
    // Restore a consistent context/signature table (the scratch test desynced the
    // signature count from g_enemy_dungeon_palette).
    invalidate_vanilla_context_table();
    build_vanilla_context_table();
  }

  // 3c) Fillability machinery (widen_set_fillable): with sig_id=0xFF the palette
  // gate is inert, so this exercises the killable/key + sheets-loaded + water logic
  // in both flag states. Synthetic dungeon context has Rat (0x6D, sheet 28) and a
  // sword soldier (0x41, sheet 73), both killable + key-capable, neither water.
  {
    build_vanilla_context_table();  // ensure the synthetic context is populated
    const uint8 live_have[4]  = { 0, 73, 28, 0 };  // Rat + soldier loaded
    const uint8 live_empty[4] = { 0, 0, 0, 0 };
    if (!widen_set_fillable(live_have, true, 0xFF, false, false, 0))
      enemy_selfcheck_die("widen verify: a killable+key candidate set must be fillable");
    if (widen_set_fillable(live_empty, true, 0xFF, false, false, 0))
      enemy_selfcheck_die("widen verify: an empty loaded set must NOT be fillable");
    if (widen_set_fillable(live_have, true, 0xFF, false, /*need_water=*/true, 0))
      enemy_selfcheck_die("widen verify: must fail water demand when no water candidate is loaded");
  }

  // 3d) Dungeon scan marking: scan_dungeon_context_list must mark each of a room's
  // enemies on that room's palette signature id, and mark nothing for the unknown
  // (0xFF) signature. Drives the scan directly with a synthetic {sort, y,x,type...}
  // list so it is independent of the asset/header path.
  {
    g_enemy_dungeon_palette[0x6D] = 0;  // Rat
    g_enemy_dungeon_palette[0x41] = 0;  // sword soldier
    static const uint8 scan_list[] = {
      0x00,              // leading sort byte
      0x00, 0x00, 0x6D,  // Rat
      0x00, 0x00, 0x41,  // sword soldier
      0xff
    };
    scan_dungeon_context_list(scan_list, /*sig_id=*/7);
    if (!(g_enemy_dungeon_palette[0x6D] & (1ull << 7)) ||
        !(g_enemy_dungeon_palette[0x41] & (1ull << 7)))
      enemy_selfcheck_die("dungeon scan must mark each room enemy on its palette signature");
    g_enemy_dungeon_palette[0x6D] = 0;
    scan_dungeon_context_list(scan_list, 0xFF);
    if (g_enemy_dungeon_palette[0x6D] != 0)
      enemy_selfcheck_die("dungeon scan must not mark on an unknown (0xFF) signature");
    // Restore a consistent table (we zeroed entries above).
    invalidate_vanilla_context_table();
    build_vanilla_context_table();
  }

  // 3e) POSITION-AWARE sheets_loaded — regression for the Red Javelin Soldier (0x48)
  // tile bug. Module05_LoadFile inits all four sprite_gfx_subset slots to sheet 70;
  // an area that doesn't own slot 3 inherits that leftover, so a slot-0 sheet (70)
  // sits in slot 3. 0x48 needs 70@slot0 + 73@slot1, so it must be REJECTED when 70 is
  // only in slot 3 (it would draw slot-0 tiles from the area's real slot-0 sheet 72).
  {
    const uint8 leftover_70_in_slot3[4] = { 72, 73, 12, 70 };  // F12-captured area 0x2D
    if (sheets_loaded(0x48, leftover_70_in_slot3))
      enemy_selfcheck_die("sheets_loaded admitted Javelin Soldier with sheet 70 in the WRONG slot (3, not 0)");
    const uint8 sheet_70_in_slot0[4] = { 70, 73, 12, 0 };      // 70 in its canonical slot
    if (!sheets_loaded(0x48, sheet_70_in_slot0))
      enemy_selfcheck_die("sheets_loaded rejected Javelin Soldier with sheet 70 correctly in slot 0");
    // kSheetSlot must agree with the canonical assignments the fix depends on.
    if (kSheetSlot[70] != 0 || kSheetSlot[73] != 1 || kSheetSlot[72] != 0 || kSheetSlot[12] != 2)
      enemy_selfcheck_die("kSheetSlot canonical-slot map disagrees with the expected sheet->slot");
  }

  // 4) On → determinism + in-sheet + killable/key invariants over a synthetic
  // loaded-sheet set. We install a shuffle and drive the pick with a fabricated
  // live-sheet set by temporarily writing g_ram[0xC2FC..0xC2FF].
  {
    RandoSettings on = s; on.enemy_shuffle = 1;
    if (!EnemyShuffle_Generate(&on, 0xDEADBEEFCAFEBABEull))
      enemy_selfcheck_die("Generate(on) must succeed");
    if (!EnemyShuffle_IsActive())
      enemy_selfcheck_die("enemy_shuffle on must be active");

    // Save + set a synthetic loaded set with sheets in their CANONICAL slots, so the
    // POSITION-aware sheets_loaded admits the synthetic-context enemies and the
    // substitution assertions below actually execute: slot1=73 (soldiers, kSheetSlot
    // 73->1), slot2=23 (Moblin, kSheetSlot 23->2). The dungeon picks substitute to the
    // killable+key soldier 0x41; the overworld picks substitute to Moblin 0x12.
    // (A slot-MISmatched set like the old {12,22,23,73} would make every pick a vanilla
    // passthrough and silently void these invariants.)
    uint8 sav[4] = { g_ram[0xC2FC], g_ram[0xC2FD], g_ram[0xC2FE], g_ram[0xC2FF] };
    uint8 sav_snapshot[7];
    for (int i = 0; i < 7; i++)
      sav_snapshot[i] = g_ram[kRam_EnemyShuffleLiveSubset + i];
    uint8 sav_ow_pal[64];
    for (int i = 0; i < 64; i++) {
      sav_ow_pal[i] = g_ram[0xFD40 + i];
      g_ram[0xFD40 + i] = 0;
    }
    g_ram[0xC2FC] = 0; g_ram[0xC2FD] = 73; g_ram[0xC2FE] = 23; g_ram[0xC2FF] = 0;
    uint8 live[4] = { 0, 73, 23, 0 };

    // Active but missing/mismatched sheet snapshots must fail closed to vanilla,
    // rather than consulting stale live sheets from a previous room transition.
    clear_resolved_sheet_snapshot();
    if (EnemyShuffle_PickDungeon(0x52, 3, 0x12) != 0x12)
      enemy_selfcheck_die("PickDungeon substituted without a matching sheet snapshot");
    write_resolved_sheet_snapshot(kEsResolvedContext_Dungeon, 0x51);
    if (EnemyShuffle_PickDungeon(0x52, 3, 0x12) != 0x12)
      enemy_selfcheck_die("PickDungeon substituted with a stale room sheet snapshot");

    // Module gate: the reshuffle must ACT (write the room/area snapshot the
    // fail-closed picker above requires) on every gameplay sheet-load module —
    // 0x07 room-to-room, 0x06 dungeon entrance (Module_PreDungeon, incl.
    // dungeon-chain hops), 0x11 falling entrance, 0x08/0x09 overworld, 0x0A
    // special-overworld return — and stay INERT for non-gameplay callers. An
    // entrance-room miss leaves the snapshot stale, so the whole room silently
    // plays vanilla.
    {
      uint8 sav_module = g_ram[0x10];
      uint8 sav_gfxidx = g_ram[0xAA3];
      uint8 sav_room_lo = g_ram[0xA0], sav_room_hi = g_ram[0xA1];
      uint8 sav_area_lo = g_ram[0x40A], sav_area_hi = g_ram[0x40B];
      // An all-zero row owns no slot: the gate + snapshot write run, the
      // subset commit does not — no live sheet state is disturbed.
      static const uint8 kZeroRow[4] = { 0, 0, 0, 0 };
      static const uint8 kDungeonGateModules[3] = { 0x06, 0x07, 0x11 };
      static const uint8 kOverworldGateModules[3] = { 0x08, 0x09, 0x0A };
      static const uint8 kInertGateModules[5] = { 0x00, 0x01, 0x0E, 0x14, 0x1A };
      g_ram[0xAA3] = 0;
      g_ram[0xA0] = 0x52; g_ram[0xA1] = 0;
      g_ram[0x40A] = 0x05; g_ram[0x40B] = 0;
      for (int mi = 0; mi < 3; mi++) {
        clear_resolved_sheet_snapshot();
        g_ram[0x10] = kDungeonGateModules[mi];
        EnemyShuffle_ReshuffleCurrentRoomSheets(kZeroRow);
        if (g_ram[kRam_EnemyShuffleLiveContext] != kEsResolvedContext_Dungeon ||
            ram_read_u16(kRam_EnemyShuffleLiveKey) != 0x52)
          enemy_selfcheck_die("reshuffle must resolve dungeon sheets under modules 0x06/0x07/0x11");
      }
      for (int mi = 0; mi < 3; mi++) {
        clear_resolved_sheet_snapshot();
        g_ram[0x10] = kOverworldGateModules[mi];
        EnemyShuffle_ReshuffleCurrentRoomSheets(kZeroRow);
        if (g_ram[kRam_EnemyShuffleLiveContext] != kEsResolvedContext_Overworld ||
            ram_read_u16(kRam_EnemyShuffleLiveKey) != 0x05)
          enemy_selfcheck_die("reshuffle must resolve overworld sheets under modules 0x08/0x09/0x0A");
      }
      for (int mi = 0; mi < 5; mi++) {
        clear_resolved_sheet_snapshot();
        g_ram[0x10] = kInertGateModules[mi];
        EnemyShuffle_ReshuffleCurrentRoomSheets(kZeroRow);
        if (g_ram[kRam_EnemyShuffleLiveContext] != kEsResolvedContext_None)
          enemy_selfcheck_die("reshuffle must stay inert outside gameplay sheet-load modules");
      }
      // Dungeon-map preview guard: gameplay module but a map tileset loaded.
      clear_resolved_sheet_snapshot();
      g_ram[0x10] = 0x07;
      g_ram[0xAA3] = 0x80;
      EnemyShuffle_ReshuffleCurrentRoomSheets(kZeroRow);
      if (g_ram[kRam_EnemyShuffleLiveContext] != kEsResolvedContext_None)
        enemy_selfcheck_die("reshuffle must stay inert for the dungeon-map preview tileset");
      g_ram[0x10] = sav_module;
      g_ram[0xAA3] = sav_gfxidx;
      g_ram[0xA0] = sav_room_lo; g_ram[0xA1] = sav_room_hi;
      g_ram[0x40A] = sav_area_lo; g_ram[0x40B] = sav_area_hi;
    }
    write_resolved_sheet_snapshot(kEsResolvedContext_Dungeon, 0x52);

    // Determinism: same (room, slot, type) → same pick.
    uint8 a = EnemyShuffle_PickDungeon(0x52, 3, 0x12);
    uint8 b = EnemyShuffle_PickDungeon(0x52, 3, 0x12);
    if (a != b) enemy_selfcheck_die("PickDungeon is not deterministic for a fixed (room,slot,type)");

    // Every dungeon pick over a sampled room/slot/type space is randomizable,
    // killable, key-capable, in-sheet (the MVP dungeon invariant) — or the
    // vanilla passthrough (when the room is hard-excluded / no candidate).
    // dungeon_subs guards against this test silently going VACUOUS: the synthetic
    // live set must keep >=1 dungeon-context candidate position-eligible, or every
    // pick is a passthrough and the invariants below never run (regression net).
    int dungeon_subs = 0;
    for (uint16 room = 0; room < 300; room += 7) {
      bool hard = room_in_list(room, kHardExcludeRooms, kHardExcludeRoomsCount);
      write_resolved_sheet_snapshot(kEsResolvedContext_Dungeon, room);
      for (uint8 slot = 0; slot < 4; slot++) {
        uint8 r = EnemyShuffle_PickDungeon(room, slot, 0x12 /*Moblin, randomizable*/);
        if (hard) {
          if (r != 0x12) enemy_selfcheck_die("hard-excluded room was substituted");
          continue;
        }
        if (r == 0x12) continue;  // passthrough (no candidate) is allowed
        dungeon_subs++;
        const EnemyConstraint *c = &kEnemyTable[r];
        if (!(c->flags & ESF_RANDOMIZABLE))
          enemy_selfcheck_die("dungeon pick is not randomizable");
        if (!candidate_allowed_in_context(r, true, 0))
          enemy_selfcheck_die("dungeon pick is not vanilla-context eligible");
        if (!(c->flags & ESF_KILLABLE))
          enemy_selfcheck_die("dungeon pick is not killable (key/shutter softlock risk)");
        if (c->flags & ESF_CANNOT_KEY)
          enemy_selfcheck_die("dungeon pick is key-banned (key-room softlock risk)");
        if (!sheets_loaded(r, live))
          enemy_selfcheck_die("dungeon pick references an unloaded sheet (CRASH risk)");
      }
    }
    if (dungeon_subs == 0)
      enemy_selfcheck_die("dungeon substitution test is VACUOUS — no pick substituted (synthetic live set slot-mismatched?)");

    // Overworld picks must be randomizable + in-sheet (no killable constraint).
    int ow_subs = 0;
    for (uint8 area = 0; area < 64; area += 5) {
      write_resolved_sheet_snapshot(kEsResolvedContext_Overworld, area);
      for (uint8 slot = 0; slot < 3; slot++) {
        uint8 r = EnemyShuffle_PickOverworld(area, slot, 0x08 /*Octorok*/);
        if (r == 0x08) continue;
        ow_subs++;
        const EnemyConstraint *c = &kEnemyTable[r];
        if (!(c->flags & ESF_RANDOMIZABLE))
          enemy_selfcheck_die("overworld pick is not randomizable");
        if (!candidate_allowed_in_context(r, false, 0))
          enemy_selfcheck_die("overworld pick is not vanilla-context eligible");
        if (!sheets_loaded(r, live))
          enemy_selfcheck_die("overworld pick references an unloaded sheet (CRASH risk)");
      }
    }
    if (ow_subs == 0)
      enemy_selfcheck_die("overworld substitution test is VACUOUS — no pick substituted");

    // An excluded source type is NEVER substituted even when active.
    if (EnemyShuffle_PickDungeon(0x52, 0, 0x53 /*Armos Knights boss*/) != 0x53)
      enemy_selfcheck_die("excluded source type (boss) was substituted");
    if (EnemyShuffle_PickDungeon(0x52, 0, 0xe4 /*control marker*/) != 0xe4)
      enemy_selfcheck_die("control marker was substituted");

    // Dungeon palette gate (meaningful only when widening is enabled): fabricate a
    // 1-signature allowlist and assert candidate_eligible filters by it. Rat (0x6D,
    // sheet 28) is marked seen-on signature 0; sword soldier (0x41, sheet 73) is
    // not. Both are dungeon-context + killable + key-capable, so only the palette
    // gate distinguishes them; an unknown signature (0xFF) passes through. Restore
    // the table afterward.
    if (ES_ENABLE_SHEET_WIDENING) {
      uint64 sav_6d = g_enemy_dungeon_palette[0x6D];
      uint64 sav_41 = g_enemy_dungeon_palette[0x41];
      g_enemy_dungeon_palette[0x6D] = 1ull << 0;
      g_enemy_dungeon_palette[0x41] = 0;
      const uint8 dlive[4] = { 0, 73, 28, 0 };
      if (!candidate_eligible(0x6D, dlive, true, true, false, false, true, 0, /*sig=*/0))
        enemy_selfcheck_die("dungeon palette gate rejected a same-signature candidate");
      if (candidate_eligible(0x41, dlive, true, true, false, false, true, 0, /*sig=*/0))
        enemy_selfcheck_die("dungeon palette gate admitted an off-signature candidate");
      if (!candidate_eligible(0x41, dlive, true, true, false, false, true, 0, /*sig=*/0xFF))
        enemy_selfcheck_die("dungeon palette gate must pass through on an unknown signature");
      g_enemy_dungeon_palette[0x6D] = sav_6d;
      g_enemy_dungeon_palette[0x41] = sav_41;
    }

    // Restore g_ram + deactivate.
    g_ram[0xC2FC] = sav[0]; g_ram[0xC2FD] = sav[1];
    g_ram[0xC2FE] = sav[2]; g_ram[0xC2FF] = sav[3];
    for (int i = 0; i < 7; i++)
      g_ram[kRam_EnemyShuffleLiveSubset + i] = sav_snapshot[i];
    for (int i = 0; i < 64; i++)
      g_ram[0xFD40 + i] = sav_ow_pal[i];
    EnemyShuffle_Deactivate();
  }

  // 5) After deactivate, picks are passthrough again.
  if (EnemyShuffle_PickDungeon(0x52, 0, 0x12) != 0x12)
    enemy_selfcheck_die("post-deactivate PickDungeon must be a passthrough");

  if (installed_synthetic_context_assets) {
    const uint32 idx[5] = {
      kEsAsset_DungeonSprites,
      kEsAsset_DungeonSpriteOffs,
      kEsAsset_OverworldSpriteOffs,
      kEsAsset_OverworldSprites,
      kEsAsset_OverworldSpritePalettes,
    };
    for (int i = 0; i < 5; i++) {
      g_asset_ptrs[idx[i]] = saved_context_asset_ptrs[i];
      g_asset_sizes[idx[i]] = saved_context_asset_sizes[i];
    }
    invalidate_vanilla_context_table();
  }

  // 6) Sheet-reshuffle (design.md D4) pure-logic invariants — per reshuffled slot.
  {
    // (0) The sheet resolver and picker share this decision. Only the effective
    // enemies tier + a generated pinned room may force vanilla sheets/types;
    // door shuffle degrades the tier and therefore keeps ordinary widening.
    if (!soul_room_pin_decision(true, true) ||
        soul_room_pin_decision(false, true) ||
        soul_room_pin_decision(true, false))
      enemy_selfcheck_die("soul-room sheet/type pin decision drifted");

    // (a) Pool integrity: every DUNGEON pool sheet self-contains a randomizable,
    // killable, key-capable enemy in THAT slot (so each reshuffled slot keeps key
    // rooms fillable). OVERWORLD pool sheets must have any randomizable enemy in
    // that slot. If the table drifts so a pool sheet loses its candidate, FAIL.
    for (int slot = 0; slot < ES_RESHUFFLE_SLOTS; slot++) {
      for (uint32 i = 0; i < kEsDungeonPoolCount[slot]; i++) {
        uint8 sheet = kEsDungeonPool[slot][i];
        uint8 live1[4] = { 0, 0, 0, 0 }; live1[slot] = sheet;
        bool ok = false;
        for (uint32 t = 0; t < ES_TABLE_LEN; t++) {
          const EnemyConstraint *c = &kEnemyTable[t];
          if ((c->flags & ESF_RANDOMIZABLE) && (c->flags & ESF_KILLABLE) &&
              !(c->flags & ESF_CANNOT_KEY) && sheets_loaded((uint8)t, live1)) { ok = true; break; }
        }
        if (!ok) enemy_selfcheck_die("a dungeon reshuffle pool sheet has no killable+key candidate");
      }
      for (uint32 i = 0; i < kEsOverworldPoolCount[slot]; i++) {
        uint8 sheet = kEsOverworldPool[slot][i];
        uint8 live1[4] = { 0, 0, 0, 0 }; live1[slot] = sheet;
        bool ok = false;
        for (uint32 t = 0; t < ES_TABLE_LEN; t++)
          if ((kEnemyTable[t].flags & ESF_RANDOMIZABLE) && sheets_loaded((uint8)t, live1)) { ok = true; break; }
        if (!ok) enemy_selfcheck_die("an overworld reshuffle pool sheet has no candidate");
      }
    }

    // (a2) Multi-slot enemies must require ALL their sheets — a missing slot lets
    // the picker spawn them where part of their gfx isn't loaded (the Walking Zora
    // "hybrid zora+buzzblob"). Zora needs 12(slot2)+68(slot3); Snapdragon
    // 22(slot0)+23(slot2): NOT admissible with only one sheet loaded.
    {
      uint8 only12[4] = { 0, 0, 12, 0 };
      if (sheets_loaded(0x56, only12))
        enemy_selfcheck_die("Walking Zora admissible with only sheet 12 (missing sub3=68 → hybrid render)");
      uint8 only23[4] = { 0, 0, 23, 0 };
      if (sheets_loaded(0x0E, only23))
        enemy_selfcheck_die("Snapdragon admissible with only sheet 23 (missing sub0=22)");
      uint8 both[4] = { 22, 0, 23, 0 };
      if (!sheets_loaded(0x0E, both))
        enemy_selfcheck_die("Snapdragon NOT admissible with both its sheets loaded");
    }

    // (b) DISJOINTNESS (the invariant the position-unaware picker depends on: a
    // sheet we load into slot s is used by enemies ONLY in slot s) is asserted at
    // GENERATION time by gen_enemy_shuffle_tables.py — the table here carries no
    // position info to re-check it at runtime. The sheet-need bits below are the
    // runtime guard against the related pin-completeness failure.

    // (c) type_blocks_slot classification (per slot).
    if (type_blocks_slot(0x12, 2))   // Moblin randomizable ⇒ never pins
      enemy_selfcheck_die("randomizable enemy wrongly pins a slot");
    for (int s = 0; s < ES_RESHUFFLE_SLOTS; s++) {
      if (!type_blocks_slot(0x92, s)) enemy_selfcheck_die("a boss (Helmasaur) failed to pin a slot");
      if (!type_blocks_slot(0x53, s)) enemy_selfcheck_die("a slot-3 boss (Armos Knights) failed to pin");
      if (!type_blocks_slot(0x05, s)) enemy_selfcheck_die("an unknown type failed to pin a slot");
      // AddGroup(6) village NPCs (no AddSubgroupN) must pin ALL slots, else the
      // reshuffle garbages them. Spot-check two.
      if (!type_blocks_slot(0x74, s)) enemy_selfcheck_die("a group-NPC (RunningMan) failed to pin a slot");
      if (!type_blocks_slot(0x2A, s)) enemy_selfcheck_die("a group-NPC (SweepingLady) failed to pin a slot");
    }
    if (!type_blocks_slot(0x90, 2))  // Wallmaster pins slot 2 (sub2=35) ...
      enemy_selfcheck_die("Wallmaster failed to pin slot 2");
    if (type_blocks_slot(0x90, 0))   // ... but NOT slot 0
      enemy_selfcheck_die("Wallmaster wrongly pinned slot 0");
    if (!type_blocks_slot(0x16, 2))  // Sahasrahla/Aginah (sub2=76) pins slot 2
      enemy_selfcheck_die("Sahasrahla/Aginah failed to pin slot 2");
    if (!type_blocks_slot(0xBC, 2) || !type_blocks_slot(0xBC, 0))  // Drunk needs slots 0,1,2,3
      enemy_selfcheck_die("Drunk-in-the-Inn failed to pin its slots");
    if (type_blocks_slot(0x1C, 2))   // Statue: slot-3 object ⇒ does NOT pin slot 2
      enemy_selfcheck_die("a slot-3 object wrongly pinned slot 2");

    // (c2) Overlord spawn-slot decode (kOverlordNeed) — pins only the spawned
    // sprite's slot; no-spawn-sheet overlords (incl. the boss-spawning
    // ArmosCoordinator 0x19) must be 0 → the runtime pins ALL.
    if (kOverlordNeed[0x15] != 0x04)  // WizzrobeSpawner → slot 2 (Wizzrobe 37/41)
      enemy_selfcheck_die("WizzrobeSpawner overlord need wrong");
    if (kOverlordNeed[0x05] != 0x01)  // FallingStalfos → slot 0 (its falling-stalfos gfx on sheet 31)
      enemy_selfcheck_die("FallingStalfos overlord need wrong");
    if (kOverlordNeed[0x09] != 0x04)  // WallmasterSpawner → slot 2 (Wallmaster 35)
      enemy_selfcheck_die("WallmasterSpawner overlord need wrong");
    if (kOverlordNeed[0x07] != 0x00)  // MovingFloor spawns no sheet sprite ⇒ pin-all
      enemy_selfcheck_die("MovingFloor overlord should have no spawn-need (pin-all)");
    if (kOverlordNeed[0x19] != 0x00)  // ArmosCoordinator spawns a BOSS ⇒ pin-all
      enemy_selfcheck_die("ArmosCoordinator overlord must be pin-all (boss safety)");

    // (d) choose_slot_sheet: deterministic, in pool ∪ {van}, non-zero, vanilla-incl.
    for (int slot = 0; slot < ES_RESHUFFLE_SLOTS; slot++) {
      for (int dn = 0; dn < 2; dn++) {
        bool is_dun = (dn == 0);
        const uint8 *pool = is_dun ? kEsDungeonPool[slot] : kEsOverworldPool[slot];
        uint32 pn = is_dun ? kEsDungeonPoolCount[slot] : kEsOverworldPoolCount[slot];
        for (uint8 van = 1; van != 0; van++) {
          uint64 k = 0x9E3779B900000000ull ^ ((uint64)slot << 16) ^ van;
          uint8 a = choose_slot_sheet(slot, van, k, is_dun);
          if (a != choose_slot_sheet(slot, van, k, is_dun))
            enemy_selfcheck_die("choose_slot_sheet not deterministic");
          if (a == 0) enemy_selfcheck_die("choose_slot_sheet returned sheet 0");
          bool in_set = (a == van);
          for (uint32 i = 0; i < pn; i++) if (pool[i] == a) in_set = true;
          if (!in_set) enemy_selfcheck_die("choose_slot_sheet returned a sheet outside pool ∪ {van}");
        }
        // Vanilla-inclusive: a van OUTSIDE the pool is sometimes returned.
        bool reachable = false;
        for (uint64 k = 0; k < 512 && !reachable; k++)
          if (choose_slot_sheet(slot, 200, k, is_dun) == 200) reachable = true;  // 200 ∉ any pool
        if (!reachable) enemy_selfcheck_die("vanilla sheet never a choose_slot_sheet outcome");
      }
    }
  }

  // 7) Stat randomization (HP / contact damage): determinism, bounds, the
  //    non-killable + flagged-damage passthroughs, and off→passthrough.
  {
    RandoSettings on; Settings_SetDefaults(&on); on.enemy_shuffle = 1;
    if (!EnemyShuffle_Generate(&on, 0xABCDEF0123456789ull))
      enemy_selfcheck_die("Generate(on) for stat check failed");

    if (EnemyShuffle_ScaleHealth(0x08, 0) != 0)
      enemy_selfcheck_die("ScaleHealth(0) must stay 0 (non-killable sprite)");
    // Bosses must NOT scale — Helmasaur King (0x92) indexes a 13-entry table by
    // sprite_health>>2; any upward scale reads OOB.
    if (EnemyShuffle_ScaleHealth(0x92, 48) != 48 || EnemyShuffle_ScaleHealth(0x53, 100) != 100 ||
        EnemyShuffle_ScaleDamage(0x92, 5) != 5)
      enemy_selfcheck_die("stat scaling must leave bosses at vanilla (Helmasaur OOB regression)");
    for (uint32 t = 0; t < 256; t++) {
      for (uint32 b = 1; b <= 255; b += 17) {
        uint8 a = EnemyShuffle_ScaleHealth((uint8)t, (uint8)b);
        if (a != EnemyShuffle_ScaleHealth((uint8)t, (uint8)b))
          enemy_selfcheck_die("ScaleHealth not deterministic");
        if (a < 1) enemy_selfcheck_die("ScaleHealth produced 0 for a killable enemy");
        uint32 lo = ((uint32)b * 8u) / 16u; if (lo < 1) lo = 1;
        uint32 hi = ((uint32)b * 32u) / 16u; if (hi > 255) hi = 255;
        if (a < lo || a > hi) enemy_selfcheck_die("ScaleHealth outside [0.5x, 2x]");
      }
    }

    if (EnemyShuffle_ScaleDamage(0x12, 0) != 0)
      enemy_selfcheck_die("ScaleDamage(0) must stay 0");
    if (EnemyShuffle_ScaleDamage(0x12, 0x83) != 0x83 ||
        EnemyShuffle_ScaleDamage(0x12, 0x40) != 0x40 ||
        EnemyShuffle_ScaleDamage(0x12, 0x14) != 0x14)
      enemy_selfcheck_die("ScaleDamage must leave a flagged value (>=0x10) unchanged");
    for (uint32 t = 0; t < 256; t++) {
      for (uint32 c = 1; c <= 8; c++) {
        uint8 d = EnemyShuffle_ScaleDamage((uint8)t, (uint8)c);
        if (d != EnemyShuffle_ScaleDamage((uint8)t, (uint8)c))
          enemy_selfcheck_die("ScaleDamage not deterministic");
        if (d < 1 || d > 8) enemy_selfcheck_die("ScaleDamage class left [1,8]");
        // Never scale a DAMAGING class (c != 2) into class 2, which is the vanilla
        // zero-contact-damage class (kPlayerDamages {0,0,0}); a vanilla class-2 enemy
        // (c == 2) correctly passes through unchanged.
        if (c != 2 && d == 2)
          enemy_selfcheck_die("ScaleDamage produced the ZERO-damage class 2 from a damaging class");
        if (c == 2 && d != 2)
          enemy_selfcheck_die("ScaleDamage must leave the vanilla zero-damage class 2 unchanged");
      }
    }

    EnemyShuffle_Deactivate();
    if (EnemyShuffle_ScaleHealth(0x08, 20) != 20)
      enemy_selfcheck_die("ScaleHealth must passthrough when shuffle is off");
    if (EnemyShuffle_ScaleDamage(0x08, 3) != 3)
      enemy_selfcheck_die("ScaleDamage must passthrough when shuffle is off");
  }

  fprintf(stderr, "[EnemyShuffle_SelfCheck] OK\n");
}
