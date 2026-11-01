// rando.c — randomizer master module (tasks.md §1.1, §1.1a).
// Owns slot activation, placement/grant transactions, generated item semantics,
// reachability, persistence integration, and the headless self-test groups.

#include "rando.h"
#include "alttpr_import.h"
#include "goal_runtime.h"
#include "goal_npc.h"
#include <string.h>  // memcpy — used by Rando_ActivateSidecarSlot below
#include "rando_rng.h"
#include "rando_share.h"
#include "rando_settings.h"
#include "rando_logic.h"
#include "rando_placement.h"
#include "rando_shuffles.h"
#include "shuffle_boss.h"   // BossShuffle_Generate/_Deactivate/_SelfCheck (Slice 7)
#include "souls.h"          // add-enemy-souls (grant + ownership reset)
#include "shuffle_drops.h"  // DropShuffle_Generate/_Deactivate/_SelfCheck (Slice 8)
#include "shuffle_enemies.h"  // EnemyShuffle_Generate/_Deactivate/_SelfCheck (enemy shuffle)
#include "shuffle_doors.h"   // DoorShuffle_Generate/LayoutDigest (door shuffle)
#include "shuffle_chains.h"  // Chains_Compute/_SelfCheck (dungeon chains)
#include "chains_runtime.h"  // Chains_RuntimeSelfCheck
#include "door_runtime.h"    // DoorRt_* (door-shuffle runtime redirect)
#include "customizer.h"  // Customizer_SelfCheck (add-rando-customizer-mode)
#include "rando_save.h"
#include "rando_generate.h"  // RandoGenerate_SelfCheck (slot SRAM-init self-test)
#include "seed_shape.h"  // SeedShape_SelfCheck
#include "rando_snapshot_tail.h"
#include "rando_textfield.h"
#include "item_ids.h"
#include "location_ids.h"
#include "dungeon_ids.h"
#include "chest_map.h"
#include "chest_lookup.h"  // (room, ordinal) -> LOC_*; §6.3 codegen
#include "pot_lookup.h"    // (room, pos4) -> LOC_*; add-rando-pot-sanity runtime
#include "terrain_lookup.h"  // (screen, pos) -> LOC_*; add-rando-grass-rock-shuffle
#include "bonk_lookup.h"     // (area, block) -> LOC_*; add-rando-bonk-sanity
#include "enemy_check_lookup.h"  // kRandoEnemyCheckRegistryDigest/Count (activation guard)
#include "direct_grant_icons.h"  // kDirectGrantIcons[] (Phase B Slice 9)
#include "rando_hints.h"  // Hints v2 lifecycle/persistence
#include "rando_dialogue.h"  // randomized reward-aware NPC text
#include "shuffle_entrance.h"  // Phase C entrance shuffle (overlay + self-check)
#include "shuffle_ow_warp.h"   // add-rando-ow-warp-shuffle (layout + self-check)
#include "inverted_entrances.h"  // #82 static Inverted entrance/exit override
#include "inverted_maps.h"  // InvertedHoleBlocks_Install (no-art Ganon pit shadow)
#include "shuffle_cosmetic.h"  // Cosmetic_SetSeed (cosmetic_seed=0 -> slot seed)
#include "medallion_icons.h"  // Rando_MedallionIcons_SelfCheck
#include "ow_map_prizes.h"  // RandoOwMap_SelfCheck
#include "../ancilla.h"  // AncillaAdd_RandoIconReceipt (Phase B Slice 9)
#include "../config.h"  // g_config.cosmetic_seed
#include "../types.h"
#include "../variables.h"  // §6.2 progressive-dispatch reads link_sword_type etc.
#include "../assets.h"     // Phase C entrance overlay: g_asset_ptrs[126] / kOverworld_Entrance_Id
#include "../features.h"   // g_rando_triforce_piece_count
#include "../overworld.h"  // ForceNonbunnyStatus
#include "../dungeon.h"    // RandoPot_OverlayOamSelfCheck
#include "../misc.h"       // §7.6 Link_CalculateSfxPan
#include "../messaging.h"  // dynamic-hint story fast-forward policy selfcheck
#include "../sprite.h"     // Sprite_ShowMessageUnconditional (trap dialogue)
#include "../sprite_main.h"  // grant presentation self-check seam
#include "../zelda_rtl.h"  // g_zenv.dialogue_flags locale gate selfcheck
#include "../hud.h"        // §7.6 Hud_RefreshIcon
#include "../player.h"     // §7.6 Link_ReceiveItem
#include "../load_gfx.h"   // Palette_Load_LinkArmorAndGloves
#include "third_party/sha256/sha256.h"

// ---------------------------------------------------------------------------
// g_assets_hash — populated by LoadAssets() in src/main.c after the asset
// blob is read and validated. See task 1.1a.
// ---------------------------------------------------------------------------
uint8 g_assets_hash[32];

// Key-item ownership is derived from the installed placement table plus the
// checked-location bitmap. These caches are hot-path conveniences only; they
// are rebuilt on every slot/snapshot restore and never serialized separately.
static uint16 g_rando_key_ring_selected_mask;
static uint16 g_rando_key_ring_owned_mask;
static bool g_rando_skeleton_key_present;
static bool g_rando_skeleton_key_owned;

uint16 Rando_GetSelectedKeyRingMask(void) {
  return g_rando_slot_active ? g_rando_key_ring_selected_mask : 0;
}

uint16 Rando_GetOwnedKeyRingMask(void) {
  return g_rando_slot_active ? g_rando_key_ring_owned_mask : 0;
}

bool Rando_HasSkeletonKey(void) {
  return g_rando_slot_active && g_rando_skeleton_key_owned;
}

uint32 Rando_CurrentPotRegistryDigest(void) {
  return kRandoPotRegistryDigest;
}

uint16 Rando_CurrentPotRegistryCount(void) {
  return (uint16)kRandoPotRegistryCount;
}

bool Rando_SettingsNeedPotRegistry(const RandoSettings *settings) {
  return settings != NULL &&
         settings->pot_shuffle != kPotShuffle_Off &&
         !Settings_PotShuffleForcedOff(settings);
}

bool Rando_PotRegistryMatches(uint32 digest, uint16 count) {
  return digest == kRandoPotRegistryDigest &&
         count == (uint16)kRandoPotRegistryCount;
}

// add-rando-grass-rock-shuffle — terrain registry identity (activation guard,
// mirroring the pot registry guard: terrain locations come from gitignored
// local codegen, so a terrain-enabled slot must prove THIS binary carries the
// same registry — the chest_lookup fail-open class).
uint32 Rando_CurrentTerrainRegistryDigest(void) {
  return kRandoTerrainRegistryDigest;
}

uint16 Rando_CurrentTerrainRegistryCount(void) {
  return (uint16)kRandoTerrainRegistryCount;
}

bool Rando_SettingsNeedTerrainRegistry(const RandoSettings *settings) {
  return settings != NULL &&
         (settings->grass_shuffle != kTerrainShuffle_Off ||
          settings->rock_shuffle != kTerrainShuffle_Off);
}

bool Rando_TerrainRegistryMatches(uint32 digest, uint16 count) {
  return digest == kRandoTerrainRegistryDigest &&
         count == (uint16)kRandoTerrainRegistryCount;
}

// add-rando-bonk-sanity — bonk registry identity (same guard shape).
uint32 Rando_CurrentBonkRegistryDigest(void) {
  return kRandoBonkRegistryDigest;
}

uint16 Rando_CurrentBonkRegistryCount(void) {
  return (uint16)kRandoBonkRegistryCount;
}

bool Rando_SettingsNeedBonkRegistry(const RandoSettings *settings) {
  return settings != NULL && settings->bonk_shuffle != kTerrainShuffle_Off;
}

bool Rando_BonkRegistryMatches(uint32 digest, uint16 count) {
  return digest == kRandoBonkRegistryDigest &&
         count == (uint16)kRandoBonkRegistryCount;
}

// Enemy-check registry identity (same guard shape). Enemy-check location ids
// bind to generated lookup rows (enemy_checks.gen.yaml), so generator_version
// alone fail-opens on same-version registry drift — this digest closes that.
uint32 Rando_CurrentEnemyCheckRegistryDigest(void) {
  return kRandoEnemyCheckRegistryDigest;
}

uint16 Rando_CurrentEnemyCheckRegistryCount(void) {
  return (uint16)kRandoEnemyCheckRegistryCount;
}

bool Rando_SettingsNeedEnemyCheckRegistry(const RandoSettings *settings) {
  // The Dungeon/All tiers consult the enemy_check_lookup tables; the Keys
  // tier reads only the forced-drop registry (version-drift-guarded).
  return Settings_EnemyChecksDungeonActive(settings);
}

bool Rando_EnemyCheckRegistryMatches(uint32 digest, uint16 count) {
  return digest == kRandoEnemyCheckRegistryDigest &&
         count == (uint16)kRandoEnemyCheckRegistryCount;
}

// Stamp EVERY local-registry identity a slot's activation guard checks (pot,
// terrain, enemy-check, ...). Single source so no slot-writer path can stamp
// one registry and forget another — the fresh-eyes review found exactly that
// drift: the player-facing Rando_GenerateSlot stamped pots but not terrain, so
// every terrain slot self-refused while the corpus/selftest (which DID stamp
// both) stayed green. Route all slot-writers through this.
void Rando_StampSlotRegistries(RandoSlotHeader *h) {
  if (h == NULL) return;
  h->pot_registry_digest = Rando_CurrentPotRegistryDigest();
  h->pot_registry_count = Rando_CurrentPotRegistryCount();
  h->pot_registry_present = 1;
  h->terrain_registry_digest = Rando_CurrentTerrainRegistryDigest();
  h->terrain_registry_count = Rando_CurrentTerrainRegistryCount();
  h->terrain_registry_present = 1;
  h->enemy_check_registry_digest = Rando_CurrentEnemyCheckRegistryDigest();
  h->enemy_check_registry_count = Rando_CurrentEnemyCheckRegistryCount();
  h->enemy_check_registry_present = 1;
  h->bonk_registry_digest = Rando_CurrentBonkRegistryDigest();
  h->bonk_registry_count = Rando_CurrentBonkRegistryCount();
  h->bonk_registry_present = 1;
}

static bool rando_instant_flute_active(void);
static bool rando_trap_decoy_icon(uint16 item_id, uint16 location_id,
                                  DirectGrantIconEntry *out);
static bool rando_trap_decoy_icon_for_seed(uint64 seed, uint16 item_id,
                                           uint16 location_id,
                                           DirectGrantIconEntry *out);
static uint64 rando_trap_decoy_seed(void);
static bool rando_receive_icon_for_code(uint8 code, uint8 *out_gfx,
                                        uint8 *out_big, uint8 *out_oam_flags);
extern const uint8 kWishPond2_OamFlags[76];

// ---------------------------------------------------------------------------
// Reachability state counter — bumped by Rando_BumpReachabilityCounter()
// when a story-progress event flag changes. The tracker overlay queries this
// to know when to invalidate its memoized Logic_ComputeReachabilityFullKnowledge cache.
// (Heap-resident per design — NOT in g_ram. See proposal.md "Impact".)
// ---------------------------------------------------------------------------
static uint32 g_reachability_state_counter;

uint16 Rando_GiftThiefLocationForRoom(uint16 room_id) {
  switch (room_id) {
  case 0x11E: return LOC_Hype_Cave_NPC;
  case 0x123: return LOC_Mini_Moldorm_Cave_NPC;
  default:    return 0xFFFFu;
  }
}

// ---------------------------------------------------------------------------
// Rando_OnLocationCheck — universal dispatcher (tasks.md §6.1).
// Phase A0 stub: pass-through. Phase A1 wires the placement_table lookup.
// ---------------------------------------------------------------------------
static void rando_record_committed_item_ownership(uint16 item_id) {
  if (item_id == ITEM_Mushroom && g_rando_slot_active)
    g_rando_mushroom_held |= kRandoMushroom_Held;
  if (item_id == ITEM_MagicPowder && g_rando_slot_active)
    g_rando_mushroom_held |= kRandoMushroom_PowderOwned;
}

uint16 Rando_OnLocationCheck(uint16 location_id, uint16 vanilla_item_id) {
  // §8.8a ordering-invariant tripwire: every call increments a global
  // counter that StateRecorder_Load snapshots before LoadSnesState and
  // asserts didn't change before the TLV reinstall. If a frame ran between
  // those two steps, dispatch would fire here and bump the counter, making
  // the load-time assertion trip.
  g_rando_oncheck_call_count++;

  // Phase B Slice 1 — flag this location as checked. The bitmap is the
  // session source-of-truth; sidecar write copies it back to the slot.
  // Set BEFORE dispatch so a crash inside dispatch still records the
  // in-flight intent (tracker shows the location as checked on reload).
  Rando_MarkLocationChecked(location_id);

  // Placement_Lookup returns vanilla_item_id when no active placement table
  // is installed (rando mode inactive), or when location_id is not in the
  // active table. See rando_placement.c.
  uint16 placed = Placement_Lookup(location_id, vanilla_item_id);

  // Track Mushroom possession the moment the player obtains it, wherever it
  // is placed. link_item_mushroom can't represent "have Mushroom AND Powder"
  // (one byte, mutually exclusive values), so the Witch trade keys off this
  // flag instead — see Rando_MushroomHeld / Witch_AcceptShroom. Guarded on
  // an active slot so a vanilla Mushroom pickup never sets it.
  // Powder ownership is tracked the same way, so the item-menu swap can show the
  // Mushroom icon (byte=1) without logic/tracking reading Powder as lost — the
  // shared byte can't represent "have both." See Rando_MushroomPowderCanToggle.
  rando_record_committed_item_ownership(placed);

  return placed;
}

// §6.2 per-placed-dungeon counter helper. The vanilla LttP dispatcher
// Link_ReceiveItem indexes by `cur_palace_index_x2 >> 1` (the player's current
// dungeon). For rando placements where a key/map/compass belongs to a DIFFERENT
// dungeon than the player's current one, write the destination dungeon's RAM
// cell ourselves. All ALTTPR-id -> game-id conversion lives in dungeon_ids.h.
//
// Returns 1 if the placed item is a dungeon item and was direct-written.
// Caller treats this as "skip Link_ReceiveItem" via kRandoLttpSkip.
static int dungeon_item_direct_grant(uint16 registry_id, uint8 opcode) {
  uint8 dungeon = Rando_DungeonItemGameDungeon(registry_id);
  // Game-side indices range 0..13 (GT). 16 is the kUpperBitmasks size — past
  // that, Rando_DungeonBitForGameDungeon returns 0 and the OR would no-op.
  if (dungeon == 0xFF || dungeon >= 16) return 0;

  uint16 bit = Rando_DungeonBitForGameDungeon(dungeon);
  if (opcode == kRandoGrantOp_DungeonBigKey) {
    // BigKey for `dungeon`.
    link_bigkey |= bit;
    return 1;
  }
  if (opcode == kRandoGrantOp_DungeonMap) {
    // Map for `dungeon`.
    link_dungeon_map |= bit;
    return 1;
  }
  if (opcode == kRandoGrantOp_DungeonCompass) {
    // Compass for `dungeon`.
    link_compass |= bit;
    return 1;
  }
  if (opcode == kRandoGrantOp_DungeonSmallKey) {
    // SmallKey for game-side dungeon `dungeon`. Vanilla keeps the live count
    // for the player's CURRENT dungeon in link_num_keys and parks every
    // dungeon's saved count in link_keys_earned_per_dungeon[cur_palace_index_x2
    // >> 1] (Hyrule Castle proper, raw index 2, folds into the Escape slot 0 —
    // see SaveDungeonKeys and the dungeon-entrance restore in dungeon.c). Under
    // key-shuffle a small key can belong to a DIFFERENT dungeon than the one
    // the player is standing in (the reported bug: Tower-of-Hera keys collected
    // during the Hyrule Castle escape were credited to the escape's live
    // counter and were unusable once the player reached ToH). Credit the
    // DESTINATION dungeon's slot, not the live current-dungeon counter.
    uint8 cur = (uint8)cur_palace_index_x2;  // 0xff when not in a dungeon
    uint8 cur_slot = Rando_KeySlotFromRawPalace(cur);
    if (cur != 0xff && dungeon == cur_slot) {
      // Key belongs to the dungeon the player is in: bump the LIVE counter and
      // resync the saved slot (mirrors SaveDungeonKeys). Don't recompute from
      // the slot — door uses decrement link_num_keys without touching the
      // slot, so the slot can be staler than the live count.
      if (link_num_keys < 0xfe) link_num_keys += 1;
      link_keys_earned_per_dungeon[dungeon] = link_num_keys;
    } else {
      // Key belongs to a dungeon the player isn't in right now: credit only the
      // saved slot. It restores into link_num_keys on dungeon entry. (0xfe cap
      // keeps the 0xff "outside dungeon" sentinel out of a counter slot.)
      if (link_keys_earned_per_dungeon[dungeon] < 0xfe)
        link_keys_earned_per_dungeon[dungeon] += 1;
    }
    return 1;
  }
  return 0;
}

// Key Ring direct grant. Unlike an ordinary small-key pickup, a ring max-writes
// the complete authored stock for its family. The live counter is authoritative
// while Link is in that family (the parked slot can be stale after door spends),
// so take the max of live, parked and grant values before synchronizing both.
static int key_ring_direct_grant(uint16 registry_id) {
  if (!Rando_IsKeyRingItem(registry_id)) return 0;
  uint8 rando_dungeon = Rando_RandoDungeonFromDungeonItem(registry_id);
  if (rando_dungeon >= kRandoDungeon_Count) return 0;
  uint8 game_dungeon = Rando_GameDungeonFromRandoDungeon(rando_dungeon);
  uint8 key_slot = Rando_KeySlotFromGameDungeon(game_dungeon);
  uint8 grant = Rando_KeyRingGrantCount(rando_dungeon);
  if (key_slot == kGameDungeon_None || key_slot >= 16 || grant == 0 || grant == 0xff)
    return 0;

  uint8 cur_raw = (uint8)cur_palace_index_x2;
  uint8 cur_slot = Rando_KeySlotFromRawPalace(cur_raw);
  if (cur_raw != 0xff && cur_slot == key_slot) {
    uint8 value = grant;
    if (link_num_keys != 0xff && link_num_keys > value) value = link_num_keys;
    if (link_keys_earned_per_dungeon[key_slot] > value)
      value = link_keys_earned_per_dungeon[key_slot];
    link_num_keys = value;
    link_keys_earned_per_dungeon[key_slot] = value;
  } else if (link_keys_earned_per_dungeon[key_slot] < grant) {
    link_keys_earned_per_dungeon[key_slot] = grant;
  }

  uint16 bit = (uint16)(1u << rando_dungeon);
  g_rando_key_ring_selected_mask |= bit;
  g_rando_key_ring_owned_mask |= bit;
  g_reachability_state_counter++;
  return 1;
}

static int skeleton_key_direct_grant(uint16 registry_id) {
  if (registry_id != ITEM_SkeletonKey) return 0;
  g_rando_skeleton_key_present = true;
  g_rando_skeleton_key_owned = true;
  g_reachability_state_counter++;
  return 1;
}

// Retro genericKeys grant — a GenericKey (id 125, ROM 0xAF) pickup feeds the
// single shared small-key pool, not a per-dungeon counter. Bump the persisted
// shared counter `link_generic_keys` (link_keys_earned_per_dungeon[15] = ALTTPR
// $7EF38B); when the player is standing in a dungeon the LIVE counter
// link_num_keys mirrors the shared one (loaded on entry, write-through on
// consume), so resync it too. Outside a dungeon link_num_keys is the 0xff
// sentinel and is left alone — it reloads from the shared slot on the next
// dungeon entry. Mirrors ALTTPR newitems.asm KeyGK (+1 to CurrentGenericKeys).
static void rando_grant_generic_key(void) {
  // rando-exempt: grant — Retro shared generic small-key counter.
  if (link_generic_keys < 0xfe) link_generic_keys += 1;
  if ((uint8)cur_palace_index_x2 != 0xff) {
    // rando-exempt: grant — live counter mirrors the shared pool in-dungeon.
    link_num_keys = link_generic_keys;
  }
}

// §6.2 prize-item direct-grant. The 7 crystals + 3 pendants OR into
// `link_has_crystals` / `link_which_pendants`. Each prize has a fixed bit
// per vanilla LttP convention (kDungeonCrystalPendantBit[] indexed by
// dungeon — but since prizes can be shuffled to any dungeon, we map by
// PRIZE id, not by dungeon).
//
// Per ALTTPR Prize\Pendant / Prize\Crystal classes and the vanilla bit
// allocations in Link_ReceiveItem's pendant case and AncillaAdd_FallingPrize's
// crystal case:
//   GreenPendant (EP)  → link_which_pendants bit 2 (mask 0x04)
//   RedPendant   (DP)  → link_which_pendants bit 1 (mask 0x02)
//   BluePendant  (ToH) → link_which_pendants bit 0 (mask 0x01)
// The crystal rows are NOT restated here. This comment used to carry its own
// copy of them, and that copy was the pre-fix permutation — it still read
// C1→0x10 / C2→0x02 / C3→0x01 / C4→0x40 / C6→0x20 long after the table below
// was corrected, i.e. it was a fifth wrong mirror of exactly the data whose
// mirroring caused the bug. kRandoCrystalItemBit[] below is the single source;
// Rando_PrizeBitSelfCheck re-derives it rather than restating it.
//
// Returns 1 on success.
// Crystal N's link_has_crystals bit, indexed by (item id - ITEM_Prize_Crystal1),
// i.e. by the in-game crystal NUMBER. THE single source: the grant, the live
// reachability inventory read, and the tracker all index this one table.
//
// Derived by composing this fork's own vanilla data — kOverworldMapData (the
// pause map's digit glyph per marker slot) x kCrystalBitMask x
// kDungeonCrystalPendantBit — which gives PoD=1, SP=2, SW=3, TT=4, IP=5, MM=6,
// TR=7, exactly PrizeShuffle_Run's vanilla assignment. Independently identical
// to upstream ALTTPR (app/Item.php: Crystal1 bit 0x02 .. Crystal7 bit 0x08).
//
// It was wrong for 5 of 7 from its introduction until a playtest-driven audit,
// because the two readers below were written to MIRROR the grant rather than to
// re-derive it, so all three agreed with each other and with nothing else. An
// earlier fix corrected the sibling PENDANT rows in this same switch and left
// the crystals alone, recording "Crystal bits verified correct against
// kDungeonCrystalPendantBit" — true of bit-vs-DUNGEON, which is not the
// question; the crystal NUMBER convention is where the permutation entered.
// Rando_PrizeBitSelfCheck now re-derives it from the identity assignment.
const uint8 kRandoCrystalItemBit[7] = {
  0x02,  // Crystal 1 - Palace of Darkness
  0x10,  // Crystal 2 - Swamp Palace
  0x40,  // Crystal 3 - Skull Woods
  0x20,  // Crystal 4 - Thieves' Town
  0x04,  // Crystal 5 - Ice Palace
  0x01,  // Crystal 6 - Misery Mire
  0x08,  // Crystal 7 - Turtle Rock
};

static int prize_item_direct_grant(uint16 registry_id) {
  if (registry_id >= ITEM_Prize_Crystal1 && registry_id <= ITEM_Prize_Crystal7) {
    link_has_crystals |= kRandoCrystalItemBit[registry_id - ITEM_Prize_Crystal1];
    return 1;
  }
  switch (registry_id) {
    case ITEM_Prize_GreenPendant: link_which_pendants |= 0x04; return 1;  // EP
    case ITEM_Prize_RedPendant:   link_which_pendants |= 0x02; return 1;  // DP
    case ITEM_Prize_BluePendant:  link_which_pendants |= 0x01; return 1;  // ToH
    default: return 0;
  }
}

// Re-derives every dungeon prize's inventory bit from the vanilla assignment
// and asserts prize_item_direct_grant produces it — by CALLING that function,
// not by restating its table. This is the check whose absence let a 5-of-7
// crystal permutation live from introduction to playtest: the two other
// readers were written to mirror the grant, so "they all agree" proved nothing.
//
// Ground truth is composed, never transcribed: PrizeShuffle_Run(NULL, ...)
// yields the identity (vanilla) dungeon->prize assignment without drawing RNG,
// and kDungeonCrystalPendantBit says which bit each dungeon sets in vanilla.
// A prize placed at its vanilla dungeon must therefore grant that dungeon's
// bit. Any permutation of the crystal-number convention breaks this.
void Rando_PrizeBitSelfCheck(void) {
  uint8 identity[kRandoDungeonCount];
  PrizeShuffle_Run(NULL, NULL, identity);

  uint8 saved_crystals = link_has_crystals;
  uint8 saved_pendants = link_which_pendants;
  const char *failure = NULL;
  uint8 seen_crystals = 0, seen_pendants = 0;

  for (uint8 d = 0; d < kRandoDungeonCount && failure == NULL; d++) {
    uint8 prize = identity[d];
    if (prize == 0xFF)
      continue;  // HCE / HCT / GT hold no shuffled prize
    uint8 game = Rando_GameDungeonFromRandoDungeon(d);
    if (game >= 13) { failure = "prize dungeon has no game-dungeon id"; break; }
    uint8 want = kDungeonCrystalPendantBit[game];
    if (want == 0) { failure = "vanilla prize dungeon has no prize bit"; break; }

    link_has_crystals = 0;
    link_which_pendants = 0;
    if (!prize_item_direct_grant((uint16)(ITEM_Prize_GreenPendant + prize))) {
      failure = "prize item was not granted";
      break;
    }
    bool is_crystal = prize >= kPrize_Crystal1;
    uint8 got = is_crystal ? link_has_crystals : link_which_pendants;
    if (got != want) {
      failure = is_crystal
          ? "crystal grants a bit other than its vanilla dungeon's"
          : "pendant grants a bit other than its vanilla dungeon's";
      break;
    }
    if (is_crystal) seen_crystals++; else seen_pendants++;
  }

  // Every bit distinct, so no two prizes can collapse onto one inventory slot.
  for (int i = 0; i < 7 && failure == NULL; i++)
    for (int j = i + 1; j < 7; j++)
      if (kRandoCrystalItemBit[i] == kRandoCrystalItemBit[j])
        failure = "two crystals share an inventory bit";
  if (failure == NULL && (seen_crystals != 7 || seen_pendants != 3))
    failure = "vanilla assignment did not cover 3 pendants + 7 crystals";

  link_has_crystals = saved_crystals;
  link_which_pendants = saved_pendants;
  if (failure != NULL) {
    fprintf(stderr, "[Rando_PrizeBitSelfCheck] FAIL: %s\n", failure);
    exit(2);
  }
  fprintf(stderr, "[Rando_PrizeBitSelfCheck] OK\n");
}

static void capacity_upgrade_direct_grant_to(bool bombs, uint8 target) {
  uint8 *level = bombs ? &link_bomb_upgrades : &link_arrow_upgrades;
  uint8 *filler = bombs ? &link_bomb_filler : &link_arrow_filler;
  if (target > 7) target = 7;
  if (*level >= target) return;
  *level = target;
  // Match the vanilla capacity shop: raising capacity also refills toward the
  // new maximum. The filler is a countdown, so a max-sized value is enough to
  // reach the cap from any current inventory without risking arithmetic wrap.
  *filler = bombs ? Hud_Capacity(0x8ddc31, 0xdb48, target) :
                    Hud_Capacity(0x8ddc53, 0xdb58, target);
}

// Phase B Slice 9 — last item/plan committed by either the transaction core or
// the narrow compatibility dispatcher.
static uint16 g_last_dispatched_item_id = 0xFFFFu;
static uint16 g_last_dispatched_location_id = 0xFFFFu;
static RandoGrantPlan g_last_dispatched_plan;
static bool g_last_dispatched_plan_valid;

static bool rando_is_magic_upgrade_item(uint16 item_id) {
  return item_id == ITEM_HalfMagic || item_id == ITEM_QuarterMagic;
}

static uint16 rando_direct_grant_icon_item_pre_grant(uint16 item_id) {
  if (!rando_is_magic_upgrade_item(item_id))
    return item_id;
  return link_magic_consumption == 0 ? ITEM_HalfMagic : ITEM_QuarterMagic;
}

static uint16 rando_direct_grant_icon_item_post_grant(uint16 item_id) {
  if (!rando_is_magic_upgrade_item(item_id))
    return item_id;
  return link_magic_consumption >= 2 ? ITEM_QuarterMagic : ITEM_HalfMagic;
}

static const DirectGrantIconEntry *rando_direct_grant_icon_entry(uint16 item_id) {
  const size_t n = sizeof(kDirectGrantIcons) / sizeof(kDirectGrantIcons[0]);
  if ((size_t)item_id >= n || kDirectGrantIcons[item_id].gfx == 0)
    return NULL;
  return &kDirectGrantIcons[item_id];
}

// === Generated-metadata grant planning =====================================

void Rando_CaptureGrantState(RandoGrantState *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  out->sword = link_sword_type;
  out->shield = link_shield_type;
  out->armor = link_armor;
  out->gloves = link_item_gloves;
  out->bow = link_item_bow;
  out->bow_owned = g_rando_bow_owned;
  out->boomerang = link_item_boomerang;
  out->boomerang_owned = g_rando_boomerang_owned;
  out->magic_consumption = link_magic_consumption;
  memcpy(out->bottle, link_bottle_info, sizeof(out->bottle));
  out->heart_pieces = link_heart_pieces;
  out->health_capacity = link_health_capacity;
  out->health_current = link_health_current;
  out->hearts_filler = link_hearts_filler;
  out->magic_power = link_magic_power;
  out->magic_filler = link_magic_filler;
  out->bombs = link_item_bombs;
  out->bomb_filler = link_bomb_filler;
  out->bomb_upgrades = link_bomb_upgrades;
  out->arrows = link_num_arrows;
  out->arrow_filler = link_arrow_filler;
  out->arrow_upgrades = link_arrow_upgrades;
  out->trap_seed = rando_trap_decoy_seed();
}

// Bottle codes, split by WHICH resource they need — the distinction the grant
// refusal contract turns on (see openspec fix-grant-refusal-contract).
//   PICKUP  = a new bottle; needs an UNOWNED slot (`bottle[i] < 2`).
//   CONTENT = something poured into a bottle; needs an EMPTY one (== 2).
static bool rando_code_is_bottle_pickup(uint8 code) {
  static const uint8 kBottlePickupCodes[] = {
    0x16, 0x2b, 0x2c, 0x2d, 0x3d, 0x3c, 0x48,
  };
  for (size_t i = 0; i < countof(kBottlePickupCodes); i++)
    if (code == kBottlePickupCodes[i]) return true;
  return false;
}

static bool rando_code_is_bottle_content(uint8 code) {
  static const uint8 kBottleContentCodes[] = {0x2e, 0x2f, 0x30, 0x0e};
  for (size_t i = 0; i < countof(kBottleContentCodes); i++)
    if (code == kBottleContentCodes[i]) return true;
  return false;
}

typedef enum {
  kRandoBottleFit_Ok = 0,
  // Needs an EMPTY bottle. The player can make one by drinking, so the refusal
  // can clear -> retryable, preserving the lossless-retry guarantee.
  kRandoBottleFit_Transient,
  // Needs an UNOWNED slot while four bottles are already held. Bottle count is
  // monotonic under GAMEPLAY -- no item, event or menu action zeroes a slot --
  // so this can never become satisfiable and is terminal. (The PC debug panel's
  // Bottles combo CAN write 0; a player who cheats a slot free after a 5th
  // bottle was consumed does not get it back. Accepted: the alternative for
  // everyone else is a softlock.) Routing it to RetryableFailure is what turned
  // every "re-arm and retry next frame" site into an infinite loop, and the
  // sites that immobilize while retrying removed the player's only means of
  // recovery (the menu). See the change's design.md.
  kRandoBottleFit_Permanent,
} RandoBottleFit;

static RandoBottleFit rando_plan_bottle_fit(const RandoGrantState *state,
                                            uint8 code) {
  if (rando_code_is_bottle_pickup(code)) {
    for (size_t i = 0; i < countof(state->bottle); i++)
      if (state->bottle[i] < 2) return kRandoBottleFit_Ok;
    return kRandoBottleFit_Permanent;
  }
  if (rando_code_is_bottle_content(code)) {
    for (size_t i = 0; i < countof(state->bottle); i++)
      if (state->bottle[i] == 2) return kRandoBottleFit_Ok;
    return kRandoBottleFit_Transient;
  }
  return kRandoBottleFit_Ok;
}

// Live-state form of the permanent case, for the COMMIT path: a plan built when
// a slot was free can be delivered after the player picked up a fourth bottle,
// and that late refusal is just as permanent as an early one.
bool Rando_ReceiveCodePermanentlyBlocked(uint8 code) {
  if (!rando_code_is_bottle_pickup(code))
    return false;
  for (int i = 0; i < 4; i++)
    if (link_bottle_info[i] < 2) return false;
  return true;
}

static bool rando_plan_receive_is_at_cap(const RandoGrantState *state,
                                         uint8 code) {
  switch (code) {
  case 0x17:  // Piece of Heart has no useful state once capacity is maxed.
  case 0x26: case 0x3e: case 0x3f:  // Heart containers.
    return state->health_capacity >= 0xa0;
  case 0x42:  // Heart refill.
    return state->health_current >= state->health_capacity;
  case 0x45:  // Magic refill.
    return state->magic_power >= 0x80;
  case 0x27: case 0x28: case 0x31:  // Bomb refills.
    return state->bomb_upgrades < 8 &&
           state->bombs >= Hud_Capacity(0x8ddc31, 0xdb48, state->bomb_upgrades);
  case 0x43: case 0x44:  // Arrow refills.
    return state->arrow_upgrades < 8 &&
           state->arrows >= Hud_Capacity(0x8ddc53, 0xdb58, state->arrow_upgrades);
  default:
    return false;
  }
}

static void rando_plan_progressive(const RandoGrantState *state,
                                   RandoGrantPlan *plan) {
  uint8 code = 0xff;
  uint8 max_code = 0xff;
  switch ((RandoGrantOpcode)plan->opcode) {
  case kRandoGrantOp_ProgressiveSword:
    code = state->sword < 4 ? state->sword : 0xff;
    max_code = 0x03;
    break;
  case kRandoGrantOp_ProgressiveShield:
    code = state->shield < 3 ? (uint8)(0x04 + state->shield) : 0xff;
    max_code = 0x06;
    break;
  case kRandoGrantOp_ProgressiveArmor:
    code = state->armor < 2 ? (uint8)(0x22 + state->armor) : 0xff;
    max_code = 0x23;
    break;
  case kRandoGrantOp_ProgressiveGlove:
    code = state->gloves < 2 ? (uint8)(0x1b + state->gloves) : 0xff;
    max_code = 0x1c;
    break;
  case kRandoGrantOp_ProgressiveBow:
    // Ownership is authoritative because the selected vanilla byte may be
    // swapped down to wood after silver is acquired. Old snapshots can lack
    // ownership bits, so fall back to the raw byte only when neither known bit
    // is present: 0=none, 1/2=wood, 3/4=silver. Unknown raw states fail safely
    // as already-complete rather than risking a downgrade/duplicate upgrade.
    if (state->bow_owned & kRandoBow_Silver) {
      code = 0xff;
    } else if (state->bow_owned & kRandoBow_Wood) {
      code = 0x3b;
    } else {
      code = state->bow == 0 ? 0x0b
           : state->bow <= 2 ? 0x3b
           : 0xff;
    }
    max_code = 0x3b;
    break;
  default:
    return;
  }
  if (code == 0xff) {
    plan->disposition = kRandoGrantDisposition_AcceptedNoOp;
    plan->display_code = max_code;
  } else {
    plan->disposition = kRandoGrantDisposition_Receive;
    plan->receive_code = plan->display_code = code;
  }
}

static bool rando_plan_opcode_is_direct(uint8 opcode) {
  switch ((RandoGrantOpcode)opcode) {
  case kRandoGrantOp_DungeonSmallKey:
  case kRandoGrantOp_DungeonBigKey:
  case kRandoGrantOp_DungeonMap:
  case kRandoGrantOp_DungeonCompass:
  case kRandoGrantOp_DirectTriforcePiece:
  case kRandoGrantOp_DirectMagicUpgrade:
  case kRandoGrantOp_DirectPrize:
  case kRandoGrantOp_DirectKeyRing:
  case kRandoGrantOp_DirectSkeletonKey:
  case kRandoGrantOp_DirectGenericKey:
  case kRandoGrantOp_DirectRupoor:
  case kRandoGrantOp_DirectTrap:
  case kRandoGrantOp_DirectSoul:
  case kRandoGrantOp_DirectBombCapacity:
  case kRandoGrantOp_DirectArrowCapacity:
    return true;
  default:
    return false;
  }
}

static void rando_plan_resolve_display(const RandoGrantState *state,
                                       RandoGrantPlan *plan) {
  DirectGrantIconEntry icon;
  bool found = false;
  if (plan->display_code != 0xff) {
    found = rando_receive_icon_for_code(plan->display_code, &icon.gfx,
                                        &icon.big, &icon.oam_flags);
  }
  if (!found && plan->opcode == kRandoGrantOp_DirectTrap) {
    found = rando_trap_decoy_icon_for_seed(state->trap_seed, plan->item_id,
                                            plan->location_id, &icon);
  }
  if (!found) {
    uint16 icon_item = plan->item_id;
    if (plan->opcode == kRandoGrantOp_DirectMagicUpgrade)
      icon_item = state->magic_consumption == 0 ? ITEM_HalfMagic : ITEM_QuarterMagic;
    const DirectGrantIconEntry *entry = rando_direct_grant_icon_entry(icon_item);
    if (entry != NULL) {
      icon = *entry;
      found = true;
    }
  }
  if (found) {
    plan->display_gfx = icon.gfx;
    plan->display_big = icon.big;
    plan->display_oam_flags = icon.oam_flags;
    plan->display_valid = 1;
  }
}

bool Rando_ResolveGrantPlan(uint16 location_id, uint16 item_id,
                            const RandoGrantState *state,
                            RandoGrantPlan *out) {
  if (out == NULL) return false;
  memset(out, 0, sizeof(*out));
  out->location_id = location_id;
  out->item_id = item_id;
  out->receive_code = 0xff;
  out->display_code = 0xff;
  out->disposition = kRandoGrantDisposition_Invalid;
  if (state == NULL || item_id >= kRandoItemGrantMetadataCount ||
      kRandoItemGrantMetadataCount != ITEM__COUNT)
    return false;

  const RandoItemGrantMetadata *metadata = &kRandoItemGrantMetadata[item_id];
  out->opcode = metadata->opcode;
  out->payload = metadata->payload;
  switch ((RandoGrantOpcode)metadata->opcode) {
  case kRandoGrantOp_ProgressiveSword:
  case kRandoGrantOp_ProgressiveShield:
  case kRandoGrantOp_ProgressiveArmor:
  case kRandoGrantOp_ProgressiveGlove:
  case kRandoGrantOp_ProgressiveBow:
    rando_plan_progressive(state, out);
    break;
  case kRandoGrantOp_Receive: {
    uint8 code = (uint8)metadata->payload;
    if (metadata->payload > 0xff) return false;
    if (item_id == ITEM_BlueBoomerang || item_id == ITEM_RedBoomerang) {
      bool blue_owned = (state->boomerang_owned & kRandoBoomerang_Blue) ||
                        state->boomerang >= 1;
      code = blue_owned ? 0x2a : 0x0c;
    }
    out->receive_code = out->display_code = code;
    RandoBottleFit fit = rando_plan_bottle_fit(state, code);
    if (fit == kRandoBottleFit_Permanent)
      // Terminal, like any other "no useful state left to gain" receive.
      out->disposition = kRandoGrantDisposition_AcceptedNoOp;
    else if (fit == kRandoBottleFit_Transient)
      out->disposition = kRandoGrantDisposition_RetryableFailure;
    else if (rando_plan_receive_is_at_cap(state, code))
      out->disposition = kRandoGrantDisposition_AcceptedNoOp;
    else
      out->disposition = kRandoGrantDisposition_Receive;
    break;
  }
  case kRandoGrantOp_DirectNothing:
    out->disposition = kRandoGrantDisposition_AcceptedNoOp;
    break;
  case kRandoGrantOp_DirectMagicUpgrade:
    if (state->magic_consumption >= 2) {
      out->disposition = kRandoGrantDisposition_AcceptedNoOp;
      out->receive_code = 2;
    } else {
      out->disposition = kRandoGrantDisposition_Direct;
      out->receive_code = (uint8)(state->magic_consumption + 1);
    }
    break;
  case kRandoGrantOp_DirectBombCapacity:
    if (state->bomb_upgrades >= 7) {
      out->disposition = kRandoGrantDisposition_AcceptedNoOp;
      out->receive_code = 7;
    } else {
      uint8 steps = (uint8)((metadata->payload + 4) / 5);
      if (steps == 0) steps = 1;
      uint8 target = (uint8)(state->bomb_upgrades + steps);
      out->receive_code = target > 7 ? 7 : target;
      out->disposition = kRandoGrantDisposition_Direct;
    }
    break;
  case kRandoGrantOp_DirectArrowCapacity:
    if (state->arrow_upgrades >= 7) {
      out->disposition = kRandoGrantDisposition_AcceptedNoOp;
      out->receive_code = 7;
    } else {
      uint8 steps = (uint8)((metadata->payload + 4) / 5);
      if (steps == 0) steps = 1;
      uint8 target = (uint8)(state->arrow_upgrades + steps);
      out->receive_code = target > 7 ? 7 : target;
      out->disposition = kRandoGrantDisposition_Direct;
    }
    break;
  case kRandoGrantOp_Virtual:
  case kRandoGrantOp_Invalid:
    return false;
  default:
    if (!rando_plan_opcode_is_direct(metadata->opcode)) return false;
    out->disposition = kRandoGrantDisposition_Direct;
    break;
  }

  rando_plan_resolve_display(state, out);
  return out->disposition != kRandoGrantDisposition_Invalid;
}

bool Rando_ResolveLiveGrantPlan(uint16 location_id, uint16 item_id,
                                RandoGrantPlan *out) {
  RandoGrantState state;
  Rando_CaptureGrantState(&state);
  return Rando_ResolveGrantPlan(location_id, item_id, &state, out);
}

bool Rando_CanAcceptGrantPlanNow(const RandoGrantPlan *plan) {
  if (plan == NULL || plan->disposition == kRandoGrantDisposition_Invalid ||
      plan->disposition == kRandoGrantDisposition_RetryableFailure)
    return false;
  if (plan->disposition != kRandoGrantDisposition_Receive)
    return true;
  return ItemReceipt_CanAccept(plan->receive_code);
}

// CRC-16-CCITT-FALSE over a domain separator, token version, and the complete
// 14-byte plan value. CRC-16 detects every non-zero corruption confined to one
// byte. The domain prevents an unrelated CRC-tagged value from being accepted
// as a deferred grant token if the storage is accidentally aliased.
static uint16 rando_deferred_grant_integrity_tag(
    uint16 version, const RandoGrantPlan *plan) {
  static const uint8 kDomain[] = {'Z', '3', 'R', 'G'};
  uint16 crc = 0xffffu;
#define RANDO_GRANT_CRC_BYTE(value) \
  do { \
    crc ^= (uint16)(uint8)(value) << 8; \
    for (int bit = 0; bit < 8; bit++) \
      crc = (crc & 0x8000u) ? (uint16)((crc << 1) ^ 0x1021u) \
                            : (uint16)(crc << 1); \
  } while (0)
  for (size_t i = 0; i < countof(kDomain); i++)
    RANDO_GRANT_CRC_BYTE(kDomain[i]);
  RANDO_GRANT_CRC_BYTE(version);
  RANDO_GRANT_CRC_BYTE(version >> 8);
  const uint8 *bytes = (const uint8 *)plan;
  for (size_t i = 0; i < sizeof(*plan); i++)
    RANDO_GRANT_CRC_BYTE(bytes[i]);
#undef RANDO_GRANT_CRC_BYTE
  return crc;
}

RandoGrantResult Rando_PrepareGrant(uint16 location_id,
                                    uint16 vanilla_registry_id,
                                    uint8 vanilla_lttp_code,
                                    RandoDeferredGrantToken *out) {
  if (out == NULL) return kRandoGrantResult_Invalid;
  memset(out, 0, sizeof(*out));
  uint16 placed;
  if (!Placement_TryLookup(location_id, &placed))
    return kRandoGrantResult_NotActive;
  if (Rando_IsLocationChecked(location_id))
    return kRandoGrantResult_AlreadyChecked;

  RandoGrantState state;
  Rando_CaptureGrantState(&state);
  if (!Rando_ResolveGrantPlan(location_id, placed, &state, &out->plan))
    return kRandoGrantResult_Invalid;
  if (out->plan.disposition == kRandoGrantDisposition_RetryableFailure)
    return kRandoGrantResult_Retryable;
  if (out->plan.disposition == kRandoGrantDisposition_Invalid)
    return kRandoGrantResult_Invalid;

  // The boss falling-heart identity uses receipt 0x3e, whose method-2 cleanup
  // differs from ordinary BossHeartContainer 0x26. Freeze that callsite-owned
  // identity alias into the token so deferred commit needs no source re-read.
  if (placed == vanilla_registry_id && placed == ITEM_BossHeartContainer &&
      vanilla_lttp_code == 0x3e &&
      out->plan.disposition == kRandoGrantDisposition_Receive) {
    out->plan.receive_code = out->plan.display_code = vanilla_lttp_code;
    out->plan.display_valid = 0;
    rando_plan_resolve_display(&state, &out->plan);
  }
  out->version = kRandoDeferredGrantTokenVersion;
  out->reserved = rando_deferred_grant_integrity_tag(out->version, &out->plan);
  return kRandoGrantResult_Accepted;
}

static uint8 trap_ascii_to_font(char ch) {
  if (ch >= 'A' && ch <= 'Z') return (uint8)(ch - 'A');
  if (ch >= 'a' && ch <= 'z') return (uint8)(26 + (ch - 'a'));
  if (ch >= '0' && ch <= '9') return (uint8)(52 + (ch - '0'));
  switch (ch) {
    case '!': return 62;
    case '?': return 63;
    case '-': return 64;
    case '.': return 65;
    case ',': return 66;
    case '\'': return 81;
    case ' ': return 89;
    default: return 89;
  }
}

static int trap_write_ascii(uint8 *out, int o, const char *text) {
  while (*text && o < 240) out[o++] = trap_ascii_to_font(*text++);
  return o;
}

bool Rando_RenderTrapMessage(uint16 msg_id, uint8 *out_buffer) {
  if (msg_id != kRandoTrapDialogueId || out_buffer == NULL) return false;
  int o = 0;
  o = trap_write_ascii(out_buffer, o, "You are");
  out_buffer[o++] = 0x75;  // visual row 1 (middle)
  o = trap_write_ascii(out_buffer, o, "a fool!");
  out_buffer[o++] = 0x7f;  // terminator
  return true;
}

// add-*-souls — the soul id whose name the next kRandoSoulDialogueId render
// should show. Set by the soul grant branch of Rando_DispatchVanillaGrant.
static uint16 g_rando_last_soul_item_id = 0xFFFF;
// Pending named-box request (0xFFFF = none). Souls are granted from arbitrary
// contexts (chest open, NPC dialogue end, minigame), so the box CANNOT fire
// inline: Sprite_ShowMessageUnconditional overwrites main_module_index, which
// corrupts the return if we're already in a message. Set at grant, consumed on
// the first quiescent gameplay frame (the trap-onset / pot-confirmation
// deferral pattern) — never dropped, just delayed.
static uint16 g_rando_soul_msg_pending = 0xFFFF;

void Rando_QueueSoulPickupMessage(uint16 soul_item_id) {
  g_rando_last_soul_item_id = soul_item_id;
  g_rando_soul_msg_pending = soul_item_id;
}

// Uppercase A-Z + space are the only chars in the generated soul names that
// the font-encoder must handle beyond trap_ascii_to_font; digits/punctuation
// never appear (roster tokens are CamelCase words). Split the CamelCase
// registry token ("Soul_Npc_KingZora" / "Soul_Kholdstare") into spaced words
// and drop the namespace prefix, matching the tracker/hint display rules.
static int soul_write_display_name(uint8 *out, int o, const char *tok) {
  if (strncmp(tok, "Soul_Npc_", 9) == 0) tok += 9;
  else if (strncmp(tok, "Soul_", 5) == 0) tok += 5;
  for (int i = 0; tok[i] && o < 232; i++) {
    char c = tok[i];
    if (c == '_') { out[o++] = trap_ascii_to_font(' '); continue; }
    if (i > 0 && c >= 'A' && c <= 'Z' && tok[i - 1] >= 'a' && tok[i - 1] <= 'z')
      out[o++] = trap_ascii_to_font(' ');  // split CamelCase
    out[o++] = trap_ascii_to_font(c);
  }
  return o;
}

bool Rando_RenderSoulMessage(uint16 msg_id, uint8 *out_buffer) {
  if (msg_id != kRandoSoulDialogueId || out_buffer == NULL) return false;
  const char *tok = (g_rando_last_soul_item_id != 0xFFFF)
                        ? Rando_GetItemName(g_rando_last_soul_item_id)
                        : NULL;
  int o = 0;
  o = trap_write_ascii(out_buffer, o, "Got the");
  out_buffer[o++] = 0x75;  // visual row 1 (middle)
  if (tok != NULL) o = soul_write_display_name(out_buffer, o, tok);
  o = trap_write_ascii(out_buffer, o, " Soul!");
  out_buffer[o++] = 0x7f;  // terminator
  return true;
}

// add-rando-trap-catalog — membership is an id-RANGE check over the contiguous
// trap block (item_registry.yaml 132..147). Every effect id in the block thus
// auto-inherits the decoy masquerade (rando_trap_decoy_icon) and the trap
// dispatch (Rando_DispatchVanillaGrant) with no per-id edits.
static bool rando_is_trap_item(uint16 item_id) {
  return item_id >= ITEM_TrapDamage && item_id <= ITEM_TrapTeleport;
}

enum {
  kRandoTrapEffect_None = 0,
  kRandoTrapEffect_Damage,
  kRandoTrapEffect_Freeze,
  kRandoTrapEffect_Bomb,
  kRandoTrapEffect_Ambush,
  kRandoTrapEffect_Cucco,
  kRandoTrapEffect_Reverse,
  kRandoTrapEffect_Scramble,
  kRandoTrapEffect_Disarm,
  kRandoTrapEffect_RupeeDrain,
  kRandoTrapEffect_MagicDrain,
  kRandoTrapEffect_AmmoDrain,
  kRandoTrapEffect_Shake,
  kRandoTrapEffect_Darkness,
  kRandoTrapEffect_FakeWarp,
  kRandoTrapEffect_FakeLowHp,
  kRandoTrapEffect_Teleport,
  kRandoTrapEffect_Count,
};

// Per-effect static catalog — the SINGLE source of truth shared by the
// placement-time selector (Rando_PickTrapEffectId) and the runtime dispatch.
//   flags bit0 (kTrapFlag_Quiescent): the onset spawns sprites / warps / writes
//   PPU registers, so it must wait for submodule_index == 0 (a quiescent frame,
//   not mid-scroll/transition).
//   fallback: the effect to substitute if the runtime context guard fails
//   (e.g. Darkness collected outdoors -> Shake); 0 means "no special fallback".
enum { kTrapFlag_Quiescent = 1u << 0 };
// add-rando-trap-catalog — placement-time location compatibility for the two
// context-locked effects, so the placed effect ALWAYS fires where the spoiler
// shows it (no silent fallback). Derived from committed location data only
// (region dungeon_id + location type) so it is deterministic + CI-safe; the
// runtime context_ok stays as belt-and-suspenders. The selector simply won't
// place a Cucco indoors or a Darkness outdoors — caves/houses get neither.
enum {
  kTrapLocReq_Any = 0,      // works anywhere
  kTrapLocReq_Outdoor = 1,  // Cucco — placed only at overworld free-standing locations
  kTrapLocReq_Dungeon = 2,  // Darkness — placed only in dungeon regions
};
typedef struct {
  uint8 effect;     // kRandoTrapEffect_*
  uint8 id;         // ITEM_Trap* (== ID_Trap*); fits uint8 (ids <= 147)
  uint8 category;   // kTrapCategory_* bit
  uint8 duration;   // stun_timer frames
  uint8 flags;      // kTrapFlag_*
  uint8 fallback;   // kRandoTrapEffect_* used if context guard fails (or None)
  uint8 loc_req;    // kTrapLocReq_* — placement-time location compatibility
} RandoTrapDef;

static const RandoTrapDef kRandoTrapCatalog[] = {
  { kRandoTrapEffect_Damage,     ITEM_TrapDamage,     kTrapCategory_Hazard,    40, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Freeze,     ITEM_TrapFreeze,     kTrapCategory_Impair,    96, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Bomb,       ITEM_TrapBomb,       kTrapCategory_Hazard,    40, kTrapFlag_Quiescent, kRandoTrapEffect_Damage, kTrapLocReq_Any },
  { kRandoTrapEffect_Ambush,     ITEM_TrapAmbush,     kTrapCategory_Hazard,    40, kTrapFlag_Quiescent, kRandoTrapEffect_Damage, kTrapLocReq_Any },
  { kRandoTrapEffect_Cucco,      ITEM_TrapCucco,      kTrapCategory_Hazard,    60, kTrapFlag_Quiescent, kRandoTrapEffect_Bomb,   kTrapLocReq_Outdoor },
  { kRandoTrapEffect_Reverse,    ITEM_TrapReverse,    kTrapCategory_Impair,   240, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Scramble,   ITEM_TrapScramble,   kTrapCategory_Impair,   240, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Disarm,     ITEM_TrapDisarm,     kTrapCategory_Impair,   150, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_RupeeDrain, ITEM_TrapRupeeDrain, kTrapCategory_Drain,     12, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_MagicDrain, ITEM_TrapMagicDrain, kTrapCategory_Drain,     12, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_AmmoDrain,  ITEM_TrapAmmoDrain,  kTrapCategory_Drain,     12, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Shake,      ITEM_TrapShake,      kTrapCategory_Scare,     48, kTrapFlag_Quiescent, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Darkness,   ITEM_TrapDarkness,   kTrapCategory_Scare,    180, kTrapFlag_Quiescent, kRandoTrapEffect_Shake, kTrapLocReq_Dungeon },
  { kRandoTrapEffect_FakeWarp,   ITEM_TrapFakeWarp,   kTrapCategory_Scare,     48, kTrapFlag_Quiescent, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_FakeLowHp,  ITEM_TrapFakeLowHp,  kTrapCategory_Scare,     90, 0, 0, kTrapLocReq_Any },
  { kRandoTrapEffect_Teleport,   ITEM_TrapTeleport,   kTrapCategory_Displace,  12, kTrapFlag_Quiescent, 0, kTrapLocReq_Any },
};

static const RandoTrapDef *rando_trap_def_for_effect(uint8 effect) {
  for (uint32 i = 0; i < countof(kRandoTrapCatalog); i++)
    if (kRandoTrapCatalog[i].effect == effect) return &kRandoTrapCatalog[i];
  return &kRandoTrapCatalog[0];  // defensive (Damage)
}

static uint8 rando_trap_effect_for_id(uint16 item_id) {
  for (uint32 i = 0; i < countof(kRandoTrapCatalog); i++)
    if (kRandoTrapCatalog[i].id == item_id) return kRandoTrapCatalog[i].effect;
  return kRandoTrapEffect_Freeze;  // defensive — a benign, universally-safe effect
}

// Placement-time type selector (called from rando_placement.c). Deterministic in
// (seed, location_id) and the enabled-category mask, independent of fill order;
// domain-separated from rando_trap_decoy_mix. A zero mask while traps are on means
// "all categories" (the canonical zero-sentinel). Picks a category uniformly, then
// an effect within it uniformly — but skips effects whose kTrapLocReq_* is not
// satisfied by loc_flags, so a context-locked effect (Cucco/Darkness) is never
// placed where it would fall back. loc_flags: kTrapLoc_IsDungeon | kTrapLoc_IsOutdoor.
uint16 Rando_PickTrapEffectId(uint64 seed, uint16 location_id, uint8 categories,
                              uint8 loc_flags) {
  uint8 mask = (uint8)(categories & kTrapCategory_All);
  if (mask == 0) mask = kTrapCategory_All;  // zero-sentinel => all categories

  uint8 cat_bits[5];
  uint32 ncats = 0;
  for (uint32 b = 0; b < 5; b++) {
    uint8 bit = (uint8)(1u << b);
    if (!(mask & bit)) continue;
    for (uint32 i = 0; i < countof(kRandoTrapCatalog); i++)
      if (kRandoTrapCatalog[i].category == bit) { cat_bits[ncats++] = bit; break; }
  }
  if (ncats == 0) return ITEM_TrapFreeze;  // unreachable; defensive

  uint64 x = seed ^ ((uint64)location_id * 0x9E3779B97F4A7C15ull)
                  ^ 0x54524150DA7A7BADull;  // "TRAP" + domain salt (!= decoy mix)
  x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27; x *= 0x94D049BB133111EBull;
  x ^= x >> 31;

  uint8 cat = cat_bits[(uint32)(x % ncats)];
  uint16 ids[8];
  uint32 nids = 0;
  for (uint32 i = 0; i < countof(kRandoTrapCatalog); i++) {
    if (kRandoTrapCatalog[i].category != cat) continue;
    uint8 req = kRandoTrapCatalog[i].loc_req;
    if (req == kTrapLocReq_Outdoor && !(loc_flags & kTrapLoc_IsOutdoor)) continue;
    if (req == kTrapLocReq_Dungeon && !(loc_flags & kTrapLoc_IsDungeon)) continue;
    ids[nids++] = kRandoTrapCatalog[i].id;
  }
  // Every category keeps >=1 location-agnostic effect, so nids>=1 in practice;
  // the fallback keeps the function total if a future catalog edit breaks that.
  if (nids == 0) return ITEM_TrapFreeze;
  return ids[(uint32)((x >> 32) % nids)];
}

// Darkness uses the dungeon fixed-color path;
// forward-declared here to avoid pulling dungeon.h into rando.c (cf. the
// kWishPond2_OamFlags pattern below). rando_trap_effect_teardown is forward-
// declared so rando_clear_trap_effect can restore an interrupted effect's
// g_ram-backed PPU state (e.g. a Darkness blackout) when a new trap pre-empts it.
void Dungeon_ApproachFixedColor_variable(uint8 a);
static void rando_trap_effect_teardown(uint8 effect);
static void rando_tick_deferred_pot_confirmation(void);
static bool rando_pot_confirmation_safe_to_emit(void);
static bool rando_receive_icon_active(void);

static uint8 g_rando_trap_stun_timer;
static uint8 g_rando_trap_effect;
static uint8 g_rando_trap_bad_sfx_timer;
static uint8 g_rando_trap_shove_timer;
static uint8 g_rando_trap_shove_dir;
static uint8 g_rando_trap_owns_forced_move;
// add-rando-trap-catalog — set by the trigger (arm), consumed by the first gated
// tick (apply). Separates collection-time arming from in-gameplay onset so
// spawn/warp/PPU effects never run mid-transition (they wait for submodule 0).
static uint8 g_rando_trap_onset_pending;
// Darkness snapshots the room's fixed-color
// intensity here so teardown can restore the exact pre-trap brightness; a
// g_ram-backed PPU write must never outlive the file-static timer.
static uint8 g_rando_trap_saved_coldata;

static void rando_clear_trap_effect(void) {
  // Restore any g_ram-backed PPU state the active effect applied (e.g. Darkness's
  // fixed-color blackout) before discarding it, so a pre-empting trap or an early
  // exit can't strand it. Only when the onset actually ran (nothing applied while
  // still pending).
  if (g_rando_trap_effect != kRandoTrapEffect_None && !g_rando_trap_onset_pending)
    rando_trap_effect_teardown(g_rando_trap_effect);
  if (g_rando_trap_owns_forced_move) {
    force_move_any_direction = 0;
    g_rando_trap_owns_forced_move = 0;
  }
  g_rando_trap_stun_timer = 0;
  g_rando_trap_effect = kRandoTrapEffect_None;
  g_rando_trap_bad_sfx_timer = 0;
  g_rando_trap_shove_timer = 0;
  g_rando_trap_shove_dir = 0;
  g_rando_trap_onset_pending = 0;
  g_rando_trap_saved_coldata = 0;
}

static bool rando_trap_stun_can_tick(void) {
  return main_module_index == 7 || main_module_index == 9 || main_module_index == 11;
}

static void rando_neutralize_trap_motion(void) {
  joypad1H_last = 0;
  joypad1L_last = 0;
  filtered_joypad_H = 0;
  filtered_joypad_L = 0;
  force_move_any_direction = 0;
  g_rando_trap_owns_forced_move = 0;

  Link_CancelDash();
  link_speed_setting = 0;
  link_y_vel = 0;
  link_x_vel = 0;
  link_actual_vel_x = 0;
  link_actual_vel_y = 0;
  link_actual_vel_z = 0;
  link_actual_vel_z_copy = 0;
  link_auxiliary_state = 0;
  button_mask_b_y = 0;
  bitfield_for_a_button = 0;
  button_b_frames = 0;
  link_delay_timer_spin_attack = 0;
  link_spin_attack_step_counter = 0;
  link_state_bits = 0;
  link_picking_throw_state = 0;
  link_grabbing_wall = 0;
  link_moving_against_diag_tile = 0;
  link_var30d = 0;
  link_var30e = 0;
  some_animation_timer_steps = 0;
  Link_ResetSwimmingState();
}

static uint8 rando_trap_recoil_dir(void) {
  static const uint8 kOppositeFacingDir[4] = {4, 8, 1, 2};
  return kOppositeFacingDir[(link_direction_facing >> 1) & 3];
}

static bool rando_is_bad_trap_wall_spark_residue(int k) {
  return ancilla_type[k] == 0x24 &&
         ancilla_item_to_link[k] == 5 &&
         ancilla_aux_timer[k] == 1 &&
         ancilla_timer[k] == 0 &&
         ancilla_step[k] == 4 &&
         ancilla_arr3[k] == 0 &&
         ancilla_G[k] == 0 &&
         ancilla_L[k] == 0;
}

static void rando_selfcheck_seed_bad_trap_wall_spark_residue(int k) {
  ancilla_type[k] = 0x24;
  ancilla_item_to_link[k] = 5;
  ancilla_aux_timer[k] = 1;
  ancilla_timer[k] = 0;
  ancilla_step[k] = 4;
  ancilla_arr3[k] = 0;
  ancilla_G[k] = 0;
  ancilla_L[k] = 0;
}

static void rando_clear_bad_trap_wall_spark_residue(void) {
  if (!g_rando_slot_active) return;
  if (link_something_with_hookshot || (bitmask_of_dragstate & 4)) return;

  for (int k = 0; k < 5; k++) {
    // Earlier trap feedback accidentally spawned type 0x24 through the wall
    // spark helper. Type 0x24 is the gravestone ancilla and does not expire
    // unless the real grave-push state owns it.
    if (rando_is_bad_trap_wall_spark_residue(k)) {
      ancilla_type[k] = 0;
    }
  }
}

static void rando_tick_trap_reveal_feedback(void) {
  if (g_rando_trap_bad_sfx_timer == 0) return;
  if (--g_rando_trap_bad_sfx_timer != 0) return;

  uint8 pan = Link_CalculateSfxPan();
  sound_effect_2 = pan | 0x26;
  sound_effect_1 = pan | (g_rando_trap_effect == kRandoTrapEffect_Freeze
                              ? 0x15
                              : 0x24);
  if (g_rando_slot_active) {
    AncillaAdd_SwordSwingSparkle(0x26, 4);
    if (g_rando_trap_effect == kRandoTrapEffect_Damage)
      AncillaAdd_DashTremor(29, 1);
  }
}

static void rando_apply_damage_trap_shove(void) {
  if (g_rando_trap_effect != kRandoTrapEffect_Damage ||
      g_rando_trap_shove_timer == 0)
    return;
  force_move_any_direction = g_rando_trap_shove_dir;
  link_direction = g_rando_trap_shove_dir;
  link_direction_last = g_rando_trap_shove_dir;
  link_speed_setting = 12;
  g_rando_trap_owns_forced_move = 1;
  g_rando_trap_shove_timer--;
}

static void rando_apply_freeze_trap_pulse(void) {
  if (g_rando_trap_effect != kRandoTrapEffect_Freeze) return;
  if ((g_rando_trap_stun_timer & 7) == 0 || countdown_for_blink < 6)
    countdown_for_blink = 10;
}

// add-rando-trap-catalog — per-effect dispatch. The trigger only ARMS (sets
// effect + duration + onset_pending + reveal); the tick APPLIES, so spawn/warp/
// PPU effects can wait for a quiescent frame even though the trigger fires from
// many grant paths in any module/submodule.

static bool rando_trap_effect_needs_quiescent(uint8 effect) {
  return (rando_trap_def_for_effect(effect)->flags & kTrapFlag_Quiescent) != 0;
}

static uint8 rando_trap_effect_fallback(uint8 effect) {
  uint8 fb = rando_trap_def_for_effect(effect)->fallback;
  return fb ? fb : kRandoTrapEffect_Freeze;
}

// Returns false when the stored effect can't run in the current context (e.g. a
// dungeon-only effect collected outdoors); the tick substitutes the effect's
// fallback. Later slices add per-effect guards here; until then every effect is
// context-OK everywhere.
static bool rando_trap_effect_context_ok(uint8 effect) {
  switch (effect) {
    case kRandoTrapEffect_Cucco:
      // Angry cuccos are an overworld mechanic (the vanilla summoner early-returns
      // indoors). Indoors -> fallback (Bomb).
      return player_is_indoors == 0;
    case kRandoTrapEffect_Darkness:
      // The fixed-color blackout is a dungeon mechanism. Outdoors -> fallback (Shake).
      return player_is_indoors != 0;
    case kRandoTrapEffect_Teleport:
      // Standard mode before Zelda's rescue (sram_progress_indicator == 0) does NOT
      // reposition Link at the start point (Dungeon_LoadEntrance gates that on
      // progress != 0), and clearing the follower would desync the escort -> a
      // possible OOB spawn / quest desync. Fall back (Freeze) until the
      // rescue completes. Open / Inverted / Retro and post-rescue Standard are >= 1.
      return sram_progress_indicator != 0;
    default:
      // Ambush relies on its in-onset sheet check (falls back to Damage when no
      // safe enemy is loadable); every other effect is universal.
      return true;
  }
}

// Directional input remap shared by Reverse
// (constant 180°) and Scramble (a slowly-rotating "drunk" wander). A PURE
// per-frame transform of the freshly-NMI-sampled joypad bytes — no Link state is
// persisted, so these effects need no teardown (the tick simply stops remapping
// when the stun expires). The button bits (B/Y/Select/Start) are preserved.
static uint8 rando_rotate_dir_nibble(uint8 b, uint8 k) {
  static const uint8 order[4] = {
    kJoypadH_Up, kJoypadH_Right, kJoypadH_Down, kJoypadH_Left
  };
  uint8 d = (uint8)(b & 0x0f);
  uint8 out = (uint8)(b & 0xf0);
  for (int i = 0; i < 4; i++)
    if (d & order[i]) out |= order[(i + k) & 3];
  return out;
}
static void rando_trap_remap_dir(uint8 k) {
  // The tick runs before Module_MainRouting consumes input this frame, so the
  // remap is what Link reads. Both held (joypad1H_last) and edge (filtered) bytes.
  joypad1H_last     = rando_rotate_dir_nibble(joypad1H_last, k);
  filtered_joypad_H = rando_rotate_dir_nibble(filtered_joypad_H, k);
}

// One-shot work performed once, on the first gated (and, for quiescent effects,
// submodule-0) tick after the reveal dialogue.
static void rando_trap_effect_onset(uint8 effect) {
  switch (effect) {
    case kRandoTrapEffect_Damage: {
      const uint8 damage = 8;  // one heart, clamped non-lethal
      if (link_health_current != 0) {
        link_health_current = (link_health_current > damage)
            ? (uint8)(link_health_current - damage)
            : 1;
      }
      // link_hearts_filler=0 is REQUIRED, not incidental: it's the heart-refill
      // queue the HUD ticks into link_health_current, so leaving it would heal the
      // trap damage straight back (MagicDrain zeroes link_magic_filler for the same
      // reason).
      link_hearts_filler = 0;
      countdown_for_blink = 64;
      g_rando_trap_shove_timer = 12;
      g_rando_trap_shove_dir = rando_trap_recoil_dir();
      break;
    }
    case kRandoTrapEffect_Freeze:
      countdown_for_blink = 32;
      break;
    case kRandoTrapEffect_RupeeDrain:
      // Drain a chunk; the HUD ticker animates link_rupees_actual down to goal
      // (same path as Rupoor). goal < actual is the safe direction (never races up).
      link_rupees_goal = (link_rupees_goal >= 100) ? (uint16)(link_rupees_goal - 100) : 0;
      break;
    case kRandoTrapEffect_MagicDrain:
      // Empty the meter. NEVER link_magic_consumption (the Half/Quarter upgrade
      // tier; writing it would permanently downgrade the upgrade). Zeroing
      // link_magic_filler is DELIBERATE and symmetric with the Damage trap's
      // link_hearts_filler=0: the *_filler is a queued refill the HUD ticks INTO the
      // live value, so leaving it would let a just-collected magic jar fill the bar
      // straight back and undo the drain. The HUD repaints from link_magic_power on
      // its own — no Hud_RefreshIcon (unsafe in the headless tick).
      link_magic_power = 0;
      link_magic_filler = 0;
      break;
    case kRandoTrapEffect_AmmoDrain:
      // Zero arrows; HALVE bombs (a bomb-wall room can need bombs to exit — never
      // zero them). Never touch the *_filler per-frame fill counters. The HUD
      // repaints the counts on its own — no Hud_RefreshIcon (unsafe in the tick).
      link_num_arrows = 0;
      link_item_bombs = (uint8)(link_item_bombs >> 1);
      break;
    case kRandoTrapEffect_Shake:
      // Self-expiring screen shake; the player keeps control (no neutralize).
      AncillaAdd_DashTremor(29, 1);
      break;
    case kRandoTrapEffect_FakeWarp:
      // Mirror-warp whoosh + sparkle with ZERO displacement (all scare); the
      // brief freeze in sustain sells the "something's happening" beat.
      sound_effect_2 = (uint8)(Link_CalculateSfxPan() | 0x2d);
      AncillaAdd_SwordSwingSparkle(0x26, 4);
      break;
    case kRandoTrapEffect_FakeLowHp:
      // Low-health alarm beep only — HP untouched (pure troll, no gameplay effect).
      // Do NOT set countdown_for_blink: that's Link's damage-invulnerability timer
      // (sprite.c gates contact damage out while it's nonzero), so it would hand the
      // player ~96 frames of free i-frames — the opposite of a scare.
      sound_effect_1 = (uint8)(Link_CalculateSfxPan() | 43);
      break;
    case kRandoTrapEffect_Bomb: {
      // Spawn a live bomb at Link. AncillaAdd_Bomb no-ops at 0 bombs and decrements
      // on spawn (and may Hud_RefreshIcon) — save the real count, force >=1 so it
      // spawns, then RESTORE the exact original (it reads link_item_bombs only at
      // entry, so the bomb's lifetime is unaffected). Never steals or grants a bomb,
      // even when the ancilla table was full and nothing spawned. Quiescent-gated.
      uint8 saved_bombs = link_item_bombs;
      if (link_item_bombs == 0) link_item_bombs = 1;
      AncillaAdd_Bomb(7, 1);
      link_item_bombs = saved_bombs;
      break;
    }
    case kRandoTrapEffect_Ambush: {
      // Spawn a small ring of hostile enemies around Link — but ONLY enemies whose
      // required GFX sheets are loaded in the current room (else garbage render);
      // if none qualify, degrade to the Damage hazard. Each candidate is a
      // standalone pure-AI, killable land/air enemy (no overlord).
      static const uint8 kAmbushPool[] = { 0x08, 0x12, 0x18, 0x6F };  // Octorok/Moblin/MiniMoldorm/Keese
      uint8 ok_types[4];
      uint8 n_ok = 0;
      for (uint32 i = 0; i < countof(kAmbushPool); i++)
        if (EnemyShuffle_SheetsLoadedFor(kAmbushPool[i]))
          ok_types[n_ok++] = kAmbushPool[i];
      if (n_ok == 0) {
        // No safe enemy loadable here — become the Damage hazard. Set the effect
        // AND the timer to Damage's own duration (not just call its onset) so the
        // sustain/teardown and shove length match even if the durations are ever
        // retuned independently.
        g_rando_trap_effect = kRandoTrapEffect_Damage;
        g_rando_trap_stun_timer = rando_trap_def_for_effect(kRandoTrapEffect_Damage)->duration;
        rando_trap_effect_onset(kRandoTrapEffect_Damage);
        break;
      }
      static const int8 kRingDX[4] = { -32, 32, -32, 32 };
      static const int8 kRingDY[4] = { -32, -32, 32, 32 };
      uint8 want = (uint8)(2 + (frame_counter & 1));  // 2 or 3
      for (uint8 m = 0; m < want; m++) {
        SpriteSpawnInfo info;
        int j = Sprite_SpawnDynamically(0, ok_types[m % n_ok], &info);
        // Sprite_SpawnDynamically also returns -1 for a soul-suppressed
        // species (Souls_SpriteAllowed), not just a full sprite table — try
        // the next ring candidate instead of fizzling the whole ring on the
        // first un-owned soul. No owned/loadable species still spawns nothing.
        if (j < 0) continue;
        sprite_floor[j] = link_is_on_lower_level;
        Sprite_SetX(j, (uint16)(link_x_coord + kRingDX[m & 3]));
        Sprite_SetY(j, (uint16)(link_y_coord + kRingDY[m & 3]));
        sprite_D[j] = 0;
        Sprite_ApplySpeedTowardsLink(j, 16);
      }
      break;
    }
    case kRandoTrapEffect_Cucco: {
      // Replicate Cucco_SummonAvenger: spawn id 0x0B at the screen edge (BG offsets)
      // with sprite_C=1 (aggressive avenger) homing toward Link. sprite_A=1 is the
      // trap-cucco signal: Sprite_0B_Cucco draws those from a custom tile + palette
      // (Rando_DrawTrapCucco) instead of sprite slot 3, so the flock renders in ANY
      // area — no longer limited to the two screens that load the cucco sheet.
      // Overworld-only: context_ok blocks indoors (matches the vanilla guard) and
      // falls back to Bomb there, and the spawn uses overworld BG scroll offsets.
      uint8 want = (uint8)(3 + (frame_counter & 3));  // 3..6
      for (uint8 m = 0; m < want; m++) {
        SpriteSpawnInfo info;
        int j = Sprite_SpawnDynamically(0, 0x0B, &info);
        if (j < 0) break;
        sprite_floor[j] = link_is_on_lower_level;
        sprite_C[j] = 1;  // aggressive avenger flag
        sprite_A[j] = 1;  // trap-cucco signal -> custom draw (renders in any area)
        uint16 x = (uint16)(BG2HOFS_copy2 + ((m * 53u) & 0xFF));
        uint16 y = (uint16)(BG2VOFS_copy2 + ((m & 1) ? 0xA0 : 0x10));
        Sprite_SetX(j, x);
        Sprite_SetY(j, y);
        Sprite_ApplySpeedTowardsLink(j, 32);
      }
      break;
    }
    case kRandoTrapEffect_Darkness:
      // Force the dungeon room black by driving the fixed-color RAMP TARGET to the
      // darkest value (kLitTorchesColorPlus[0] == 31 == no-torches), then snapping
      // COLDATA there immediately. Dungeon-only (context_ok blocks outdoors ->
      // Shake). teardown restores the room's natural brightness.
      g_rando_trap_saved_coldata = overworld_fixed_color_plusminus;
      overworld_fixed_color_plusminus = 31;
      Dungeon_ApproachFixedColor_variable(31);
      break;
    case kRandoTrapEffect_Teleport:
      // DISPLACE: hand off to the game's own dungeon loader (module 6) targeting the
      // Sanctuary spawn-select start point via the START-POINT branch of
      // Dungeon_LoadEntrance. The loader itself diverts an INVERTED slot's
      // start-point-1 to the Dark Chapel (verified: the inv_dark_chapel branch keyed
      // on Rando_GetActiveWorldState()==2 sets which_entrance=0x5A), so this is
      // world-correct with no extra conditional. Reimplements dbg_warp.cpp's
      // DoWarpStartPoint(1); the trap's quiescent gate (module 7/9/11, submodule 0)
      // already enforces "normal gameplay only", so no Cheats_CanWarp() is needed.
      // The item-receipt path can still own bit 0 of the direction lock here. The
      // immediate module handoff bypasses that path's normal cleanup, while the
      // start-point loader does not clear it; leaking the bit lets Link move but
      // freezes his facing until another room transition. Release only that bit so
      // unrelated direction-lock state remains intact.
      link_cant_change_direction &= ~1;
      which_starting_point = 1;   // Sanctuary (Inverted -> Dark Chapel via the loader)
      WORD(death_var5) = 0;       // normal save-exit snapshot
      WORD(death_var4) = 1;       // force the start-point branch
      follower_indicator = 0;     // drop any stale tagalong
      player_is_indoors = 1;      // loader sets it too; explicit
      subsubmodule_index = 0;
      submodule_index = 0;
      main_module_index = 6;      // Module_PreDungeon runs next frame
      // Teleport completes synchronously (the loader takes over next frame).
      // g_rando_trap_effect/stun_timer stay set but are benign: rando_trap_stun_can_
      // tick is false during the module-6 load and Teleport has no sustain/teardown
      // body, so the leftover timer expires harmlessly on arrival. (If Teleport ever
      // gains a sustain/teardown, clear the effect here instead.)
      break;
    default:
      // Effects added in later slices fill in their onset here.
      break;
  }
}

// Per-frame work while the stun timer runs.
static void rando_trap_effect_sustain(uint8 effect) {
  switch (effect) {
    case kRandoTrapEffect_Damage:
      rando_neutralize_trap_motion();
      rando_apply_damage_trap_shove();
      break;
    case kRandoTrapEffect_Freeze:
      rando_neutralize_trap_motion();
      rando_apply_freeze_trap_pulse();
      break;
    case kRandoTrapEffect_Reverse:
      rando_trap_remap_dir(2);  // 180°: up<->down, left<->right
      break;
    case kRandoTrapEffect_Scramble:
      rando_trap_remap_dir((uint8)((frame_counter >> 5) & 3));  // slow drunken rotation
      break;
    case kRandoTrapEffect_Disarm:
      // Disable sword (B) and item (Y); movement + A (lift/dash) stay intact.
      joypad1H_last     = (uint8)(joypad1H_last     & ~(kJoypadH_B | kJoypadH_Y));
      filtered_joypad_H = (uint8)(filtered_joypad_H & ~(kJoypadH_B | kJoypadH_Y));
      break;
    case kRandoTrapEffect_FakeWarp:
      rando_neutralize_trap_motion();  // brief freeze during the fake "warp"
      break;
    case kRandoTrapEffect_FakeLowHp:
      // Keep the hearts flashing and re-beep periodically; HP untouched.
      if (countdown_for_blink < 6) countdown_for_blink = 32;
      if ((g_rando_trap_stun_timer & 31) == 0)
        sound_effect_1 = (uint8)(Link_CalculateSfxPan() | 43);
      break;
    case kRandoTrapEffect_Darkness:
      // Hold the ramp target black so the per-frame fixed-color ramp can't creep
      // the room back toward lit while the blackout runs.
      overworld_fixed_color_plusminus = 31;
      break;
    default:
      break;
  }
}

// Restore any g_ram-backed state the effect mutated (PPU registers, forced
// motion) so nothing outlives the file-static timer.
static void rando_trap_effect_teardown(uint8 effect) {
  switch (effect) {
    case kRandoTrapEffect_Darkness:
      // Restore the room's pre-trap fixed-color target. ONLY indoors — the dungeon
      // fixed-color register is meaningless on the overworld, so restoring there
      // would pop a dim special screen's brightness if Darkness was collected
      // indoors and the timer expired after exiting. The snapshot is
      // the exact pre-darkness value (now read). A room change during
      // the brief blackout self-heals on the next room load's fixed-color re-derive.
      if (player_is_indoors) {
        overworld_fixed_color_plusminus = g_rando_trap_saved_coldata;
        Dungeon_ApproachFixedColor_variable(g_rando_trap_saved_coldata);
      }
      break;
    default:
      break;
  }
}

static bool rando_receive_icon_active(void) {
  for (uint8 i = 0; i < 10; i++)
    if (ancilla_type[i] == kAncillaType_RandoIconReceipt)
      return true;
  return false;
}

// True while the masquerade's decoy confirmation icon (Ancilla44_RandoIconReceipt)
// is live. The Cucco onset waits on this so the swarm's recv-item-slot use can't
// bleed into the decoy icon.
static bool rando_decoy_icon_active(void) {
  return rando_receive_icon_active();
}

// add-*-souls — fire the deferred soul-pickup name box on a quiescent gameplay
// frame. Same gate as the pot confirmation (normal module + submodule 0), which
// also excludes the message module (14), so we never re-enter a live message
// and corrupt its saved return. Let any still-floating receive icon clear first
// so the box doesn't race a prior grant's icon.
static void rando_tick_deferred_soul_message(void) {
  if (g_rando_soul_msg_pending == 0xFFFF)
    return;
  if (!(enhanced_features1 & kFeatures1_RandomizerActive)) {
    g_rando_soul_msg_pending = 0xFFFF;
    return;
  }
  if (!rando_pot_confirmation_safe_to_emit())
    return;
  if (rando_receive_icon_active())
    return;
  g_rando_last_soul_item_id = g_rando_soul_msg_pending;
  g_rando_soul_msg_pending = 0xFFFF;
  Sprite_ShowMessageUnconditional(kRandoSoulDialogueId);
}

void Rando_TickTrapEffects(void) {
  rando_clear_bad_trap_wall_spark_residue();
  rando_tick_deferred_pot_confirmation();
  rando_tick_deferred_soul_message();

  if (g_rando_trap_stun_timer == 0) return;

  // Let the player dismiss the trap dialogue; the effect begins once control
  // returns to normal overworld/dungeon gameplay. The bad reveal cue is delayed
  // too, otherwise the VWF letter blip can overwrite it in the same frame.
  if (main_module_index == 14 && dialogue_message_index == kRandoTrapDialogueId)
    return;

  if (!rando_trap_stun_can_tick()) return;

  // Quiescent-only effects (spawn/warp/PPU) defer their onset until the frame is
  // settled (submodule_index == 0) — never dropped, just delayed.
  if (g_rando_trap_onset_pending &&
      rando_trap_effect_needs_quiescent(g_rando_trap_effect) &&
      submodule_index != 0)
    return;

  // The Cucco swarm draws from the shared recv-item slot (chars 0x24/0x34). While
  // the masquerade's decoy confirmation icon (Ancilla44_RandoIconReceipt) is still
  // on screen it draws that SAME slot, so spawning the flock now bleeds the cucco
  // tile into the decoy icon (a wrong-palette cucco over Link's head). Defer the
  // onset until the icon clears — also a cleaner reveal: "got <decoy>" lands, THEN
  // the flock springs.
  if (g_rando_trap_onset_pending && g_rando_trap_effect == kRandoTrapEffect_Cucco &&
      rando_decoy_icon_active())
    return;

  rando_tick_trap_reveal_feedback();

  if (g_rando_trap_onset_pending) {
    if (!rando_trap_effect_context_ok(g_rando_trap_effect)) {
      g_rando_trap_effect = rando_trap_effect_fallback(g_rando_trap_effect);
      // Retune the stun timer to the fallback's own duration so sustain/teardown
      // match (same fix the in-onset Ambush fallback got — durations may differ).
      g_rando_trap_stun_timer = rando_trap_def_for_effect(g_rando_trap_effect)->duration;
    }
    rando_trap_effect_onset(g_rando_trap_effect);
    g_rando_trap_onset_pending = 0;
  }

  rando_trap_effect_sustain(g_rando_trap_effect);

  if (--g_rando_trap_stun_timer == 0) {
    rando_trap_effect_teardown(g_rando_trap_effect);
    g_rando_trap_effect = kRandoTrapEffect_None;
    g_rando_trap_shove_timer = 0;
    g_rando_trap_shove_dir = 0;
  }
}

static void rando_trigger_trap(uint16 item_id) {
  uint8 effect = rando_trap_effect_for_id(item_id);
  const RandoTrapDef *def = rando_trap_def_for_effect(effect);
  // Clear any prior trap's leftover state FIRST — critically, release a still-
  // owned forced-move shove (g_rando_trap_owns_forced_move / force_move_any_
  // direction). A second trap collected mid-Damage-shove whose effect does NOT
  // neutralize (any drain/reverse/disarm/scare/spawn) would otherwise leave Link
  // force-moved indefinitely. rando_clear_trap_effect() releases
  // the forced move and zeroes the timers we re-arm just below.
  rando_clear_trap_effect();
  // Arm only — the reveal dialogue shows now; the effect's onset runs in the
  // gated tick once gameplay resumes (and the frame is quiescent for spawn/warp/
  // PPU effects). This is why the trigger must touch no spawn/warp/PPU state.
  Sprite_ShowMessageUnconditional(kRandoTrapDialogueId);
  g_rando_trap_bad_sfx_timer = 8;
  g_rando_trap_effect = effect;
  g_rando_trap_stun_timer = def->duration;
  g_rando_trap_onset_pending = 1;
  g_rando_trap_shove_timer = 0;
  g_rando_trap_shove_dir = 0;
}

static bool rando_apply_direct_grant_plan(const RandoGrantPlan *plan) {
  if (plan == NULL || plan->disposition != kRandoGrantDisposition_Direct)
    return false;
  uint16 item = plan->item_id;
  switch ((RandoGrantOpcode)plan->opcode) {
  case kRandoGrantOp_DirectTriforcePiece:
    if (g_rando_triforce_piece_count < 255) g_rando_triforce_piece_count++;
    return true;
  case kRandoGrantOp_DirectMagicUpgrade:
    if (plan->receive_code > 2) return false;
    if (link_magic_consumption < plan->receive_code)
      link_magic_consumption = plan->receive_code;
    return true;
  case kRandoGrantOp_DirectPrize:
    return prize_item_direct_grant(item) != 0;
  case kRandoGrantOp_DirectKeyRing:
    return key_ring_direct_grant(item) != 0;
  case kRandoGrantOp_DirectSkeletonKey:
    return skeleton_key_direct_grant(item) != 0;
  case kRandoGrantOp_DirectGenericKey:
    rando_grant_generic_key();
    return true;
  case kRandoGrantOp_DirectRupoor:
    link_rupees_goal = link_rupees_goal >= 10
        ? (uint16)(link_rupees_goal - 10) : 0;
    return true;
  case kRandoGrantOp_DirectTrap:
    rando_trigger_trap(item);
    return true;
  case kRandoGrantOp_DirectSoul:
    Souls_GrantItem(item);
    return true;
  case kRandoGrantOp_DirectBombCapacity:
    capacity_upgrade_direct_grant_to(true, plan->receive_code);
    return true;
  case kRandoGrantOp_DirectArrowCapacity:
    capacity_upgrade_direct_grant_to(false, plan->receive_code);
    return true;
  case kRandoGrantOp_DungeonSmallKey:
  case kRandoGrantOp_DungeonBigKey:
  case kRandoGrantOp_DungeonMap:
  case kRandoGrantOp_DungeonCompass:
    return dungeon_item_direct_grant(item, plan->opcode) != 0;
  default:
    return false;
  }
}

uint8 Rando_DispatchVanillaGrant(uint16 location_id,
                                 uint16 vanilla_registry_id,
                                 uint8 vanilla_lttp_code) {
  uint16 placed;
  bool placement_present = Placement_TryLookup(location_id, &placed);
  if (!placement_present) {
    // Preserve the legacy pass-through for a genuinely absent location. Do not
    // use item equality for this decision: explicit identity placements still
    // need metadata semantics (notably 0x51/0x52 capacity and 0xAF generic key).
    placed = Rando_OnLocationCheck(location_id, vanilla_registry_id);
    g_last_dispatched_item_id = placed;
    g_last_dispatched_location_id = location_id;
    g_last_dispatched_plan_valid = false;
    return vanilla_lttp_code;
  }

  g_last_dispatched_item_id = placed;
  g_last_dispatched_location_id = location_id;
  g_last_dispatched_plan_valid =
      Rando_ResolveLiveGrantPlan(location_id, placed, &g_last_dispatched_plan);
  // The compatibility return byte cannot represent retry. Fail closed before
  // Rando_OnLocationCheck marks the location; Phase 5 replaces this bridge with
  // the full transaction result. Confirmation consumers inspect the stored plan
  // and suppress success feedback for this outcome.
  if (!g_last_dispatched_plan_valid ||
      g_last_dispatched_plan.disposition == kRandoGrantDisposition_RetryableFailure)
    return kRandoLttpSkip;

  uint16 committed = Rando_OnLocationCheck(location_id, vanilla_registry_id);
  if (committed != placed) {
    // Single-threaded placement tables should be immutable across these calls.
    // Fail closed if that invariant is ever broken.
    g_last_dispatched_plan_valid = false;
    return kRandoLttpSkip;
  }
  // A substituted item is governed exclusively by generated metadata. Unknown
  // and virtual items fail closed; they never turn into the source location's
  // vanilla item.
  if (g_last_dispatched_plan.disposition == kRandoGrantDisposition_Receive) {
    // Boss falling hearts use the site-specific 0x3e receipt, whose method-2
    // teardown differs from the ordinary 0x26 BossHeartContainer receipt. It
    // is still a metadata-validated Receive plan; retain that exact identity
    // alias so the boss-heart collision path stays vanilla-correct.
    if (placed == vanilla_registry_id && placed == ITEM_BossHeartContainer &&
        vanilla_lttp_code == 0x3e)
      return vanilla_lttp_code;
    return g_last_dispatched_plan.receive_code;
  }
  if (g_last_dispatched_plan.disposition == kRandoGrantDisposition_AcceptedNoOp)
    return kRandoLttpSkip;

  (void)rando_apply_direct_grant_plan(&g_last_dispatched_plan);
  return kRandoLttpSkip;
}

// ---------------------------------------------------------------------------
// Chest universal-dispatch lookup table.
//
// (room, ordinal) -> LOC_* via the codegen-emitted kRandoChestLookup table in
// src/rando/chest_lookup.h. Source-of-truth: vanilla chest table
// ($81e96e) cross-referenced with ALTTPR PHP location names (audit.md §0.3.5).
//
// The table is sorted by (room, ordinal) at codegen time so we can binary-
// search. Returns 0xFFFF for any (room, ordinal) pair that isn't in the
// table — at runtime this falls through to vanilla item-grant, leaving rooms
// like the 4 ALTTPR-unexposed small-key/rupee chests unaffected.
// ---------------------------------------------------------------------------
static uint16 chest_lookup(uint16 dungeon_room, uint8 chest_ordinal) {
  if (RandoChestMap_Ready()) return RandoChestMap_Find(dungeon_room, chest_ordinal);
  // Binary search on packed (room << 8) | ordinal key — both room and
  // ordinal are small enough that a single 32-bit pack is overkill but
  // makes the compare cheap and branch-free against the inline ordering.
  uint32 want = ((uint32)dungeon_room << 8) | chest_ordinal;
  int lo = 0, hi = (int)kRandoChestLookup_COUNT - 1;
  while (lo <= hi) {
    int mid = lo + ((hi - lo) >> 1);
    uint32 got = ((uint32)kRandoChestLookup[mid].room << 8) | kRandoChestLookup[mid].ordinal;
    if (got == want) return kRandoChestLookup[mid].loc_id;
    if (got < want) lo = mid + 1;
    else            hi = mid - 1;
  }
  return 0xFFFFu;  // not mapped — vanilla item grants
}

RandoGrantResult Rando_ChestGrant(uint16 dungeon_room, uint8 chest_ordinal,
                                  uint8 vanilla_lttp_code,
                                  RandoGrantPresentation presentation,
                                  uint8 receipt_method,
                                  uint16 chest_position) {
  uint16 loc_id = chest_lookup(dungeon_room, chest_ordinal);
  if (loc_id == 0xFFFFu)
    return kRandoGrantResult_NotActive;
  return Rando_GrantLocation(loc_id, 0xFFFFu, vanilla_lttp_code,
                             presentation, receipt_method, chest_position);
}

// === Phase B sprite/shop dispatch: begin ===
// ---------------------------------------------------------------------------
// Shop-slot lookup (#53). The entered overworld door selects one of the 9
// ALTTPR shops; that shop's three purchasable slots occupy three consecutive
// registry ids (LOC base .. base+2). Capacity-Upgrade slots (264/265) are
// identity-placed and dispatched separately by their own site.
//
// Provenance: ALTTPR shop room_id/door_id pairs from
// `../alttp_vt_randomizer/app/Region/Standard/**/*.php` `new Shop(...)`
// 4th/5th constructor args; LOC bases from
// `assets/rando/location_registry.yaml` ids 237..263. The (room, door)
// disambiguation matches z3randomizer `shopkeeper.asm` SpritePrep_ShopKeeper
// (ShopTable room+door match).
//
// `door` is ALTTPR's `PreviousOverworldDoor`, which
// `z3randomizer/doorframefixes.asm` (`StoreLastOverworldDoorID`:
// `TXA : INC : STA.l PreviousOverworldDoor`) defines as the overworld door ROW
// INDEX + 1. It is NOT the entrance id — that same routine loads the entrance
// id separately from `$9BBB73,X` into `EntranceIndex`. The two quantities
// coincide for only 3 of the 9 shops, so keying on the engine's
// `which_entrance` cannot work: the four room-0x10F doors all carry entrance id
// 0x60 and the two room-0x112 doors both carry 0x58. (That mistake shipped
// once; `--rando-shop-doorwalk` is the guard that now pins it down. It measured
// 15 of the 27 slots unreachable and 6 reached from several doors at once.)
// The live door id is captured at the overworld entry hook into
// g_ram[kRam_RandoOverworldDoor].
//
// DERIVED, NOT TABULATED (design D4). Eight of the nine shops sit in cave
// interiors that are cave-entrance-shuffle pool entries, one entry per
// overworld door row. A shop travels with its entry, so its live door set is
// whichever SOURCE entry the permutation assigned to it:
//
//     door row -> source entry -> installed permutation -> destination entry
//                              -> that entry's shop_loc_base
//
// which is ALTTPR's own per-seed re-derivation (`Shop::get_bytes`:
// `door_id = door_addresses[entrances[0].name][0] + 1`) expressed against our
// assignment instead of a rewritten ROM table. With the axis off the assignment
// is the identity and this reduces EXACTLY to the vanilla door column that the
// static table used to hold — `--rando-shop-doorwalk` pins both arms.
//
// Deriving it also means placement and runtime agree by construction rather
// than by a second table that could drift: the same `location_ids` that carry
// `shop_loc_base`'s three slots are what `Entrance_ApplyRegionOverrides` rebinds
// to the source door's region in the same pass.
//
// ROOM-ONLY shops: ALTTPR's shop identifier (z3randomizer `shopkeeper.asm`,
// ShopTable match loop) checks `ShopType & 0x0040`; when set, it matches on
// RoomIndex ALONE and SKIPS the door compare. That bit comes from the Shop's
// `config & 0xFC` (PHP `Shop::getBytes`): the Light World Death Mountain Shop
// is constructed with `config = 0x43` (= 0x03 | 0x40) per
// `../alttp_vt_randomizer/app/Region/Standard/LightWorld/DeathMountain/East.php`,
// so its `door_id = 0x00` is deliberately ignored. Keying that shop on the door
// would silently fail (0x00 also = the "no entrance" reset value), so it is
// flagged room_only and matched on room alone — exactly as the ROM does. It is
// also the ONE shop that is not a cave interior (room 0x0FF is below the
// 0x100 cached-exit class), so it never shuffles and the static table keeps
// exactly this row.
//
// Every OTHER shop stays door-keyed even where its room hosts only one shop.
// Room-only matching there would be a superset of upstream: room 0x11F is
// loaded by TWO overworld doors (entrance ids 0x46 and 0x6B) and ALTTPR
// declares a shop for the first only, so the second must keep vanilla shop
// behavior. It would also make the slot obtainable from a door the logic does
// not bind it to, putting runtime reachability ahead of the modeled region.
typedef struct { uint16 room; uint8 door; uint16 loc_base; bool room_only; } RandoShopSlot;
static const RandoShopSlot kRandoShopSlots[] = {
  // room   door   base  room_only   shop (LOC ids base..base+2)
  { 0x0FF, 0x00, 255, true  },  // Light World Death Mountain Shop (255/256/257) — config 0x43 -> room-only
};
// The 27 regular shop slots as one contiguous id block — the doorwalk audit and
// the debug dump iterate it, since the per-shop rows now live in the cave table.
enum { kRandoShopLocFirst = 237, kRandoShopLocCount = 27 };

// The ENTRY permutation installed this session, for the D4 chain above. Defined
// with the rest of the entrance runtime state further down; forward-declared
// here because the shop dispatch is emitted before those globals.
// Returns NULL / *out_n == 0 when no entry axis is installed (⇒ identity).
static const uint8 *Entrance_InstalledEntryNet(int *out_n, bool *out_cross);
// True when the ACTIVE slot's settings say a cave-entry permutation must be
// installed. "No net" and "identity net" are otherwise indistinguishable, and
// they are not interchangeable here: the four Dark World shops share room
// 0x10F, so the destination-room guard below cannot catch a shuffled slot that
// silently resolved with the vanilla assignment — it would hand over a real but
// WRONG shop (wrong item, wrong price, wrong location marked checked).
static bool Entrance_EntryNetExpected(void);
// The door's PRISTINE vanilla entrance id (the saved original under an installed
// overlay, else the live table). Only needed to resolve a DUNGEON source door
// under Crossed; cave sources resolve by row.
static uint8 Entrance_VanillaIdOfDoorRow(uint16 lx);

// `door` is ALTTPR's PreviousOverworldDoor (overworld door row index + 1) — see
// the block comment above; callers read it from g_ram[kRam_RandoOverworldDoor].
static uint16 shop_lookup(uint16 room, uint8 door, uint8 pos) {
  if (pos > 2) return 0xFFFFu;  // shops have exactly 3 slots
  // Cave-resident shops (8 of 9), resolved through the live entrance layout.
  // door == 0 is the engine's "no entrance yet" reset value, which is also why
  // the room-only shop is declared with door 0 — never treat it as row -1.
  if (door != 0) {
    int net_n = 0;
    bool net_cross = false;
    const uint8 *net = Entrance_InstalledEntryNet(&net_n, &net_cross);
    // Fail CLOSED on the one inconsistency the room guard cannot see: settings
    // that call for a shuffled layout while none is installed. Falling through
    // to the identity assignment there would answer with the door's VANILLA
    // shop, which for the room-0x10F and room-0x112 doors is a different real
    // shop in the same room — so the room compare passes and the player is
    // handed someone else's three slots. No shop beats the wrong shop.
    if (net == NULL && Entrance_EntryNetExpected()) return 0xFFFFu;
    int dst = Entrance_DestCaveOfDoorRow(net, net_n, net_cross,
                                         (uint16)(door - 1),
                                         Entrance_VanillaIdOfDoorRow((uint16)(door - 1)));
    if (dst >= 0) {
      uint16 base = Entrance_CaveShopLocBase(dst);
      // Room must be the destination interior's own room. A captured door id
      // that does not belong to the room the player is standing in (a mirror
      // warp, a fall-hole, a stale byte) then fails CLOSED instead of naming
      // whatever shop that row's destination happens to host.
      if (base != 0xFFFFu && room == Entrance_CaveInteriorRoom(dst))
        return (uint16)(base + pos);
      // A cave door that hosts no shop falls through: the static table holds
      // only the room-only 0x0FF row, and a cave door never loads room 0x0FF.
    }
  }
  for (uint32 i = 0; i < sizeof(kRandoShopSlots) / sizeof(kRandoShopSlots[0]); i++) {
    if (kRandoShopSlots[i].room != room) continue;  // room must always match
    // Room-only shops match on the room alone (ShopType & 0x40); others also
    // require the door to match.
    if (kRandoShopSlots[i].room_only || kRandoShopSlots[i].door == door) {
      return (uint16)(kRandoShopSlots[i].loc_base + pos);
    }
  }
  return 0xFFFFu;  // not a known shop slot — vanilla item grants
}

// add-rando-shopsanity — the shop table resolution ALONE, with none of the
// active-slot / placement / already-checked gating Rando_ShopSlotCheckInfo
// applies. Exists so the --rando-shop-doorwalk audit can ask "which shop does
// this (room, door) name?" without a generated slot on disk, which is what lets
// it run as an ordinary validation-profile step. Returns 0xFFFF for a non-shop
// tuple. Never use this to decide a grant — it answers identity, not liveness.
uint16 Rando_ShopSlotLocForDoor(uint16 room, uint8 door, uint8 pos_plus1) {
  if (pos_plus1 < 1 || pos_plus1 > 3) return 0xFFFFu;
  return shop_lookup(room, door, (uint8)(pos_plus1 - 1));
}

// add-rando-shopsanity — forward decl (implementation lives below with the
// other active-slot readers; the dispatch is defined before those globals).
static bool rando_shopsanity_active(void);

RandoGrantResult Rando_ShopGrant(uint16 room, uint8 door, uint8 pos,
                                 uint8 vanilla_lttp_code,
                                 RandoGrantPresentation presentation,
                                 uint8 receipt_method,
                                 uint8 chest_position) {
  uint16 loc_id = shop_lookup(room, door, pos);
  // Plain Retro keeps its repeatable vanilla-identity shop economy. Under
  // shopsanity, a checked slot also becomes a vanilla restock purchase.
  if (loc_id == 0xFFFFu || !rando_shopsanity_active() ||
      Rando_IsLocationChecked(loc_id))
    return kRandoGrantResult_NotActive;
  return Rando_GrantLocation(loc_id, 0xFFFFu, vanilla_lttp_code,
                             presentation, receipt_method, chest_position);
}

RandoGrantResult Rando_CommitRepeatableShopIdentity(
    uint16 room, uint8 door, uint8 pos, uint8 vanilla_lttp_code) {
  uint16 loc_id = shop_lookup(room, door, pos);
  if (loc_id == 0xFFFFu)
    return kRandoGrantResult_NotActive;
  uint16 placed;
  if (!Placement_TryLookup(loc_id, &placed))
    return kRandoGrantResult_NotActive;
  const RandoLocationDef *def = Rando_FindLocationDef(loc_id);
  if (def == NULL || placed != def->vanilla_item_id)
    return kRandoGrantResult_Invalid;
  RandoGrantPlan plan;
  // A purchase made while the resource sits at cap resolves AcceptedNoOp;
  // the player still paid and the vanilla shop item still delivered, so the
  // identity row must be marked now, not on some later un-capped repurchase.
  if (!Rando_ResolveLiveGrantPlan(loc_id, placed, &plan) ||
      plan.opcode != kRandoGrantOp_Receive ||
      (plan.disposition != kRandoGrantDisposition_Receive &&
       plan.disposition != kRandoGrantDisposition_AcceptedNoOp) ||
      plan.receive_code != vanilla_lttp_code)
    return kRandoGrantResult_Invalid;
  if (!Rando_IsLocationChecked(loc_id)) {
    Rando_MarkLocationChecked(loc_id);
    rando_record_committed_item_ownership(placed);
    g_last_dispatched_item_id = placed;
    g_last_dispatched_location_id = loc_id;
    g_last_dispatched_plan = plan;
    g_last_dispatched_plan_valid = true;
  }
  return kRandoGrantResult_Accepted;
}

// add-rando-shopsanity — deterministic per-slot check price. ONE function
// shared by the spoiler emitter (generation) and the shop spawn path
// (runtime), derived from (seed, loc_id) on a dedicated salted stream so the
// fill RNG is untouched and the price is never stored anywhere. Uniform over
// multiples of 5 in [10, 250] — deliberately independent of the placed item's
// class so the price tag cannot leak whether a slot holds progression
// (design.md D5). Determinism is pinned by Rando_ShopPriceSelfCheck.
uint16 Rando_ShopPrice(uint64 seed_u64, uint16 loc_id) {
  RandoRng rng;
  Rng_SeedFromU64(&rng, seed_u64 ^ 0x53686F7050726963ull ^ ((uint64)loc_id << 48));  // "ShopPric"
  return (uint16)(10 + 5 * Rng_NextRange(&rng, 49));
}

// Fixed vector for the price stream (seed 0x1234, loc 237 = Dark World
// Potion Shop - 0). Captured from the implementation at introduction; any
// drift means the derived-price contract broke.
enum { kShopPricePinnedVector = 190 };

// Pins the price stream: determinism, band membership, per-slot variation,
// and one fixed vector so a formula/RNG change cannot slip through as a
// silent cross-version price shift (prices are re-derived at slot load, so a
// drifted formula would make a reloaded seed disagree with its spoiler).
static void Rando_ShopPriceSelfCheck(void) {
  static const uint64 kSeeds[3] = { 0x1234ull, 0xC0FFEEull, 0xFFFFFFFFFFFFFFFFull };
  uint16 distinct_probe = Rando_ShopPrice(kSeeds[0], 237);
  bool any_distinct = false;
  for (uint8 si = 0; si < 3; si++) {
    for (uint16 loc = 237; loc <= 263; loc++) {
      uint16 p = Rando_ShopPrice(kSeeds[si], loc);
      if (p != Rando_ShopPrice(kSeeds[si], loc)) {
        fprintf(stderr, "Rando_ShopPriceSelfCheck: non-deterministic price\n");
        exit(2);
      }
      if (p < 10 || p > 250 || (p % 5) != 0) {
        fprintf(stderr, "Rando_ShopPriceSelfCheck: price %u outside {10..250 step 5}\n", p);
        exit(2);
      }
      if (si == 0 && p != distinct_probe) any_distinct = true;
    }
  }
  if (!any_distinct) {
    fprintf(stderr, "Rando_ShopPriceSelfCheck: loc_id does not vary the stream\n");
    exit(2);
  }
  uint16 vec = Rando_ShopPrice(0x1234ull, 237);
  if (vec != kShopPricePinnedVector) {
    fprintf(stderr, "Rando_ShopPriceSelfCheck: pinned vector drifted (got %u)\n", vec);
    exit(2);
  }
}

// === Phase B Slice 3b — Retro TakeAny runtime ===
//
// Runtime cave table. Index = cave index (MUST match the generator + registry:
// location id = 266 + 2*cave + slot). door_id = ALTTPR overworld door
// (= row-index lx + 1); host_entrance = the redirected destination entrance
// (0x58 -> host room 0x112, 0x60 -> 0x10F, 0x46 -> 0x11F). Verified across all
// 31 caves against app/Region/Standard/** (door_id-1 == the 0xDBB73 redirect
// offset). See add-rando-retro-takeany/design.md §3.
#define kRandoTakeAnyCaveCount 31
#define kRandoTakeAnyLocBase   266
// Phase C Stage 2 — dungeon entrance-shuffle coupling. Set at the overworld entry
// hook when a SHUFFLED dungeon door is entered; the dungeon-exit room-keyed search
// uses it instead of the loaded dungeon's room so the player returns to the SOURCE
// door (avoids stranding, e.g. exiting on Ice Palace's lake without flippers).
// 0 = no override. Consumed (cleared) by the exit path.
uint16 g_rando_entrance_exit_room;
// Phase C Stage 3 (cross-category) coupling: set at the entry hook when a CAVE
// source door is redirected to a DUNGEON interior (cave→dungeon). The loaded
// dungeon room takes the room-keyed SEARCH exit branch, which would NOT return
// to the cave; this flag forces the cached-exit branch (the *_exit shadow vars,
// cached at entry, hold the source cave door's overworld position) so the player
// returns to the cave door. 0 = normal. Consumed by the exit path.
uint8 g_rando_entrance_force_cached;
// Source cave's room for the force-cached exit above. Set TOGETHER with
// g_rando_entrance_force_cached inside Rando_EntranceForceCachedExit, consumed at
// the overworld-exit top. The cached *_exit shadow vars hold the cave DOOR
// position but not its ROOM, so the loaded dungeon's room (< 0x124) lingers in
// dungeon_room_index and skews LoadCachedEntranceProperties' vanilla room-keyed
// Y-adjust; restoring the cave room here (mirroring Rando_ReplayCaveArrival on
// the decoupled path) keys the adjust off the cave. 0 = none. (PLAYTEST-PENDING.)
uint16 g_rando_force_cached_room;
// Inverted spawn-select respawn redirect — see rando.h. Set by
// Module1B_SpawnSelect (Inverted slots only), consumed by the next
// LoadOverworldFromDungeon (forces the anchor exit screen |= 0x40 → Dark World).
uint8 g_rando_inverted_spawn_redirect;

// add-rando-inverted-dark-chapel-spawn: rename the post-Agahnim spawn-select
// options for an Inverted slot — "Sanctuary" -> "Dark Chapel", "The Mountain
// Cave" -> "Dark Mountain" (ALTTPR labels). Operates on the FINISHED character
// buffer (after Text_LoadCharacterBuffer's vanilla decode, so the player-name
// expansion, [Position] command, and menu structure are all handled normally):
// just swap the location word's font-byte run in place. The font-byte runs are
// the US text-alphabet encodings (assets/text_compression.py); the search runs
// match the vanilla decompressed menu (verified). 0x7F terminates the buffer.
// embedded-data-guard: allow short UI text font-byte runs (search/replace for the
// spawn-menu labels), not extracted/generated asset data.
static void RewriteFontRun(uint8 *buf, const uint8 *find, int fn,
                           const uint8 *repl, int rn) {
  int len = 0;
  while (buf[len] != 0x7F && len < 250) len++;  // buffer end (inclusive 0x7F)
  if (buf[len] != 0x7F) return;  // no terminator found within cap — refuse to shift
  for (int i = 0; i + fn <= len; i++) {
    if (memcmp(buf + i, find, (size_t)fn) != 0) continue;
    // Shift the tail (including the 0x7F terminator) to fit the replacement.
    memmove(buf + i + rn, buf + i + fn, (size_t)(len - (i + fn) + 1));
    memcpy(buf + i, repl, (size_t)rn);
    return;
  }
}

void Rando_RewriteInvertedSpawnMenu(uint16 msg_id, uint8 *buf) {
  if (buf == NULL) return;
  if (!(enhanced_features1 & kFeatures1_RandomizerActive)) return;
  if (Rando_GetActiveWorldState() != 2 /* kWorldState_Inverted */) return;
  if (msg_id != 0x184 && msg_id != 0x185) return;
  static const uint8 kSanctuary[]   = {0x12,0x1A,0x27,0x1C,0x2D,0x2E,0x1A,0x2B,0x32};
  static const uint8 kDarkChapel[]  = {0x03,0x1A,0x2B,0x24,0x59,0x02,0x21,0x1A,0x29,0x1E,0x25};
  RewriteFontRun(buf, kSanctuary, sizeof(kSanctuary), kDarkChapel, sizeof(kDarkChapel));
  if (msg_id == 0x185) {  // 3-option menu (Magic Mirror held): also Dark Mountain
    static const uint8 kMountainCave[]  = {0x13,0x21,0x1E,0x59,0x0C,0x28,0x2E,0x27,0x2D,0x1A,0x22,0x27,0x59,0x02,0x1A,0x2F,0x1E};
    static const uint8 kDarkMountain[]  = {0x03,0x1A,0x2B,0x24,0x59,0x0C,0x28,0x2E,0x27,0x2D,0x1A,0x22,0x27};
    RewriteFontRun(buf, kMountainCave, sizeof(kMountainCave), kDarkMountain, sizeof(kDarkMountain));
  }
}

typedef struct { uint8 door_id; uint8 host_entrance; } RandoTakeAnyCaveRt;
static const RandoTakeAnyCaveRt kRandoTakeAnyCaves[kRandoTakeAnyCaveCount] = {
  {0x56, 0x58}, {0x62, 0x58}, {0x6D, 0x58}, {0x69, 0x58}, {0x68, 0x60},  // 0..4
  {0x5A, 0x58}, {0x66, 0x60}, {0x59, 0x60}, {0x78, 0x58}, {0x81, 0x58},  // 5..9
  {0x6A, 0x58}, {0x7C, 0x58}, {0x70, 0x58}, {0x7B, 0x58}, {0x79, 0x58},  // 10..14
  {0x77, 0x58}, {0x72, 0x58}, {0x6B, 0x58}, {0x73, 0x46}, {0x6C, 0x58},  // 15..19
  {0x67, 0x46}, {0x55, 0x58}, {0x5E, 0x58}, {0x65, 0x46}, {0x44, 0x46},  // 20..24
  {0x3C, 0x58}, {0x76, 0x46}, {0x3E, 0x46}, {0x3F, 0x46}, {0x4A, 0x46},  // 25..29
  {0x50, 0x58},                                                           // 30
};

// Cave index for an overworld door id, or -1 if not a take-any door.
static int takeany_cave_for_door(uint8 door_id) {
  for (int i = 0; i < kRandoTakeAnyCaveCount; i++)
    if (kRandoTakeAnyCaves[i].door_id == door_id) return i;
  return -1;
}

static uint8 takeany_host_room_for_cave(int cave) {
  if (cave < 0 || cave >= kRandoTakeAnyCaveCount) return 0xFFu;
  switch (kRandoTakeAnyCaves[cave].host_entrance) {
  case 0x58: return 0x12;  // room 0x112, Light World Lake Hylia shop
  case 0x60: return 0x0F;  // room 0x10F, Dark World shop
  case 0x46: return 0x1F;  // room 0x11F, Light World Kakariko shop
  default:   return 0xFFu;
  }
}

// True iff loc is present in the active placement table (the generator only
// emits active caves' slots, so presence == "active this seed").
static bool takeany_loc_in_table(uint16 loc) {
  return Placement_Lookup(loc, 0xFFFFu) != 0xFFFFu;
}

uint8 Rando_TakeAnyHostByDoorIndex(uint8 lx) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive)) return 0;
  if (Rando_GetActiveWorldState() != kWorldState_Retro) return 0;
  int cave = takeany_cave_for_door((uint8)(lx + 1));
  if (cave < 0) return 0;
  // A cave is active iff its slot-0 LOC is in the placement table (both potion
  // and weapon caves always populate slot 0).
  if (!takeany_loc_in_table((uint16)(kRandoTakeAnyLocBase + 2 * cave))) return 0;
  return kRandoTakeAnyCaves[cave].host_entrance;
}

// Icon kind (shop-item subtype2) for a take-any slot's placed item: 14 = heart
// (BossHeartContainer), 15 = potion (BluePotion, plus the weapon cave's
// sword/rupee — no sword tile exists in the host rooms' shop GFX, so it falls
// back to the potion icon). Both tiles are always loaded (these rooms sell
// RedPotion + a heart in vanilla).
uint8 Rando_TakeAnyDrawKind(uint8 door_id, uint8 pos) {
  int cave = takeany_cave_for_door(door_id);
  if (cave < 0) return 15;
  uint16 loc = (uint16)(kRandoTakeAnyLocBase + 2 * cave + pos);
  uint16 item = Placement_Lookup(loc, 0xFFFFu);
  return (item == ITEM_BossHeartContainer) ? 14 : 15;
}

uint16 Rando_TakeAnyLiveSlot(uint8 room, uint8 door_id, uint8 pos) {
  if (pos > 1) return 0xFFFFu;
  int cave = takeany_cave_for_door(door_id);
  if (cave < 0 || room != takeany_host_room_for_cave(cave)) return 0xFFFFu;
  uint16 loc = (uint16)(kRandoTakeAnyLocBase + 2 * cave + pos);
  if (!takeany_loc_in_table(loc)) return 0xFFFFu;   // inactive slot
  if (Rando_IsLocationChecked(loc)) return 0xFFFFu; // already taken (cave locked)
  return loc;
}

RandoGrantResult Rando_TakeAnyGrant(uint8 room, uint8 door_id, uint8 pos,
                                    uint8 vanilla_lttp_code,
                                    RandoGrantPresentation presentation,
                                    uint8 receipt_method,
                                    uint8 chest_position) {
  if (pos > 1)
    return kRandoGrantResult_NotActive;
  int cave = takeany_cave_for_door(door_id);
  if (cave < 0 || room != takeany_host_room_for_cave(cave))
    return kRandoGrantResult_NotActive;
  uint16 loc = (uint16)(kRandoTakeAnyLocBase + 2 * cave + pos);
  if (!takeany_loc_in_table(loc))
    return kRandoGrantResult_NotActive;

  RandoGrantResult result = Rando_GrantLocation(
      loc, 0xFFFFu, vanilla_lttp_code, presentation,
      receipt_method, chest_position);
  if (result == kRandoGrantResult_Accepted) {
    uint16 sibling = (uint16)(kRandoTakeAnyLocBase + 2 * cave + (pos ^ 1));
    (void)Rando_ForfeitLocation(sibling);
  }
  return result;
}
// === Phase B sprite/shop dispatch: end ===

// ---------------------------------------------------------------------------
// Rando_BumpReachabilityCounter — invalidates the tracker's cached
// reachability. Phase A0 stub: increment the counter. The tracker (task 10.2)
// will consume the value.
// ---------------------------------------------------------------------------
void Rando_BumpReachabilityCounter(void) {
  g_reachability_state_counter++;
}

uint32 Rando_GetReachabilityCounter(void) {
  return g_reachability_state_counter;
}

// ---------------------------------------------------------------------------
// Phase B Slice 1 — tracker overlay state + checked-location bitmap.
// ---------------------------------------------------------------------------
bool g_rando_show_item_tracker = false;
bool g_rando_show_location_tracker = false;
uint8 g_rando_checked_bitmap[kRandoCheckedBitmapBytes];
// Name-tie: the static location registry must never exceed the capacity that
// sizes every location-id-keyed array/bitmap/loop bound across the module. This
// fires here (rando.c sees both LOC__COUNT from location_ids.h and
// kRandoLocationCapacity from rando_logic.h) the instant a registry append
// outgrows capacity — a build break, not a silent fail-open. See rando_logic.h.
_Static_assert(LOC__COUNT <= kRandoLocationCapacity,
               "location registry exceeds kRandoLocationCapacity — raise it");
uint8 g_rando_mushroom_held = 0;
uint8 g_rando_flute_shovel_owned = 0;
uint8 g_rando_boomerang_owned = 0;
uint8 g_rando_bow_owned = 0;

// ---------------------------------------------------------------------------
// tracker-player-knowledge — persisted per-slot topology-discovery state.
// The knowledge-limited live view (Rando_GetLiveReachability) may only present
// content the player has observed; these bits record the observations:
//   dungeons  — been inside (per-frame observation, Rando_TickDiscovery)
//   caves     — entered the door leading to this vanilla interior
//   whirlpools— ridden this pair (involution: one ride marks both directions)
//   exits     — traversed this decoupled cave-exit net edge
// Persisted in the sidecar slot-extension v13 block and the type-10 snapshot
// TLV (kRandoSnapshotTail_Type_Discovery, payload format_version 1).
// Rando_BackfillDiscoveryFromChecked() derives the
// dungeon/cave bits for older saves/snapshots from checked-location state
// (checked implies having been there, so backfill can under-reveal but never
// over-reveal); it runs unconditionally after every load — an idempotent OR —
// so checks recorded by an older binary self-heal too.
// ---------------------------------------------------------------------------
static uint16 g_rando_discovered_dungeons;    // bit d = kRandoDungeon_* d entered
static uint8 g_rando_discovered_whirlpools;   // bit = kRandoOwWhirlpools index ridden
static uint8 g_rando_discovered_caves[(kEntranceMaxInteriors + 7) / 8];
static uint8 g_rando_discovered_exits[(kEntranceMaxInteriors + 7) / 8];

static void rando_discovery_mark_common(void) {
  // A discovery is a reachability-relevant state change: invalidate the memo
  // (the counter is the memo key) — the tracker lights up the same session.
  Rando_BumpReachabilityCounter();
}

bool Rando_DungeonDiscovered(uint8 rando_dungeon) {
  return rando_dungeon < kRandoDungeon_Count &&
         (g_rando_discovered_dungeons & (uint16)(1u << rando_dungeon)) != 0;
}

void Rando_MarkDungeonDiscovered(uint8 rando_dungeon) {
  if (!g_rando_slot_active || rando_dungeon >= kRandoDungeon_Count) return;
  uint16 bit = (uint16)(1u << rando_dungeon);
  if (g_rando_discovered_dungeons & bit) return;
  g_rando_discovered_dungeons |= bit;
  rando_discovery_mark_common();
}

bool Rando_CaveInteriorDiscovered(int interior) {
  return interior >= 0 && interior < kEntranceMaxInteriors &&
         (g_rando_discovered_caves[interior >> 3] & (uint8)(1u << (interior & 7))) != 0;
}

void Rando_MarkCaveInteriorDiscovered(int interior) {
  if (!g_rando_slot_active || interior < 0 || interior >= kEntranceMaxInteriors) return;
  // Only record cave discovery on slots where a cave identity can actually be
  // hidden. Besides keeping the persisted bits meaningful, this is load-bearing
  // for Retro: the take-any redirect overwrites which_entrance AFTER the entry
  // hook that marks (see Overworld_UseEntrance), so the door's table interior is
  // NOT what the player entered. Retro refuses cave shuffle, so gating here
  // makes that stale mark impossible instead of merely unread.
  // (accessor, not the file-statics — those are declared further down.)
  const RandoSettings *rs = Rando_GetActiveSettings();
  if (rs == NULL || !Settings_EffectiveShuffleCaveEntrances(rs)) return;
  uint8 bit = (uint8)(1u << (interior & 7));
  if (g_rando_discovered_caves[interior >> 3] & bit) return;
  g_rando_discovered_caves[interior >> 3] |= bit;
  rando_discovery_mark_common();
}

bool Rando_DecoupledExitDiscovered(int interior) {
  return interior >= 0 && interior < kEntranceMaxInteriors &&
         (g_rando_discovered_exits[interior >> 3] & (uint8)(1u << (interior & 7))) != 0;
}

void Rando_MarkDecoupledExitDiscovered(int interior) {
  if (!g_rando_slot_active || interior < 0 || interior >= kEntranceMaxInteriors) return;
  uint8 bit = (uint8)(1u << (interior & 7));
  if (g_rando_discovered_exits[interior >> 3] & bit) return;
  g_rando_discovered_exits[interior >> 3] |= bit;
  rando_discovery_mark_common();
}

uint8 Rando_DiscoveredWhirlpoolMask(void) { return g_rando_discovered_whirlpools; }

void Rando_MarkWhirlpoolPairDiscovered(uint8 entered_idx, uint8 partner_idx) {
  if (!g_rando_slot_active) return;
  uint8 add = 0;
  if (entered_idx < 8) add |= (uint8)(1u << entered_idx);
  if (partner_idx < 8) add |= (uint8)(1u << partner_idx);
  if ((g_rando_discovered_whirlpools | add) == g_rando_discovered_whirlpools) return;
  g_rando_discovered_whirlpools |= add;
  rando_discovery_mark_common();
}

// Per-frame dungeon observation — the single choke point for "the player is
// inside dungeon X" (covers walk-ins, drop-ins, chain seams, spiral entries,
// and spawn points without per-entry-path hooks). Module 7 is the settled
// dungeon gameplay module; HC-proper folds into the HCE bucket the same way
// the map display does.
void Rando_TickDiscovery(void) {
  if (!g_rando_slot_active) return;
  if (main_module_index != 7) return;
  uint8 game_d = Rando_GameDungeonFromRawPalace((uint8)cur_palace_index_x2);
  uint8 rd = Rando_MapDisplayDungeonFromGameDungeon(game_d);
  if (rd != kRandoDungeon_None) Rando_MarkDungeonDiscovered(rd);
}

// Copy-out / copy-in for the sidecar writer, the snapshot TLV, and selfchecks.
void Rando_GetDiscoveryState(uint16 *dungeons, uint8 *whirlpools,
                             uint8 caves[8], uint8 exits[8]) {
  if (dungeons) *dungeons = g_rando_discovered_dungeons;
  if (whirlpools) *whirlpools = g_rando_discovered_whirlpools;
  if (caves) memcpy(caves, g_rando_discovered_caves, sizeof(g_rando_discovered_caves));
  if (exits) memcpy(exits, g_rando_discovered_exits, sizeof(g_rando_discovered_exits));
}

void Rando_SetDiscoveryState(uint16 dungeons, uint8 whirlpools,
                             const uint8 caves[8], const uint8 exits[8]) {
  g_rando_discovered_dungeons = dungeons;
  g_rando_discovered_whirlpools = whirlpools;
  if (caves) memcpy(g_rando_discovered_caves, caves, sizeof(g_rando_discovered_caves));
  else memset(g_rando_discovered_caves, 0, sizeof(g_rando_discovered_caves));
  if (exits) memcpy(g_rando_discovered_exits, exits, sizeof(g_rando_discovered_exits));
  else memset(g_rando_discovered_exits, 0, sizeof(g_rando_discovered_exits));
  Rando_BumpReachabilityCounter();
}

void Rando_ResetDiscoveryState(void) {
  g_rando_discovered_dungeons = 0;
  g_rando_discovered_whirlpools = 0;
  memset(g_rando_discovered_caves, 0, sizeof(g_rando_discovered_caves));
  memset(g_rando_discovered_exits, 0, sizeof(g_rando_discovered_exits));
  Rando_BumpReachabilityCounter();
}

// Backfill — derive dungeon/cave discovery from checked-location state ONLY
// (never from placement/settings/layout data): a checked location implies the
// player was there. Whirlpool/exit knowledge has no checked-state proxy and
// simply re-hides on legacy loads (mild, fail-safe). Idempotent OR; called
// after slot activation and after snapshot-tail load.
void Rando_BackfillDiscoveryFromChecked(void) {
  if (!g_rando_slot_active) return;
  bool changed = false;
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    uint16 loc = kRandoLocations[i].id;
    if (!Rando_IsLocationChecked(loc)) continue;
    // Region → dungeon (generated binding; kRandoDungeon_* convention).
    uint16 region = kRandoLocations[i].region_id;
    for (uint32 r = 0; r < kRandoRegionsCount; r++) {
      if (kRandoRegions[r].id != region) continue;
      uint8 d = kRandoRegions[r].dungeon_id;
      if (d < kRandoDungeon_Count && !Rando_DungeonDiscovered(d)) {
        g_rando_discovered_dungeons |= (uint16)(1u << d);
        changed = true;
      }
      break;
    }
    // Cave interior membership (static interior→location lists).
    int interior = Entrance_CaveInteriorOfLocation(loc);
    if (interior >= 0 && !Rando_CaveInteriorDiscovered(interior)) {
      g_rando_discovered_caves[interior >> 3] |= (uint8)(1u << (interior & 7));
      changed = true;
    }
  }
  if (changed) Rando_BumpReachabilityCounter();
}

// Phase B Inverted runtime — the active slot's world_state, captured at
// Rando_ActivateSidecarSlot from the slot header's additive @68 byte (only
// meaningful when settings_ext_present). Lets the starting-inventory grant in
// rando_placement.c recognize an Inverted slot on reload, where the full
// RandoSettings struct is not available (the sidecar persists only
// settings_hash + the additive ext bytes, not the canonical settings blob).
// Defaults to kWorldState_Open (0) — the safe no-op — when no slot is active
// or the slot predates the world_state ext.
static uint8 g_rando_active_world_state = kWorldState_Open;

uint8 Rando_GetActiveWorldState(void) {
  return g_rando_slot_active ? g_rando_active_world_state : (uint8)kWorldState_Open;
}

bool Rando_NormalizeMirrorlessAwayWorld(void) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive) || !g_rando_slot_active)
    return false;
  if (g_rando_active_world_state == (uint8)kWorldState_Inverted)
    return false;
  if (!savegame_is_darkworld || link_item_mirror == 2)
    return false;

  // ALTTPR's Bugfix_MirrorlessSQToLW clears CurrentWorld for mirrorless
  // non-Inverted saves so Aga/portal DW access remains logical without trapping
  // the player there after S&Q. Keep the local bunny state consistent with the
  // recovered Light World load path.
  savegame_is_darkworld = 0;
  if (!link_item_moon_pearl)
    ForceNonbunnyStatus();
  return true;
}

bool Rando_IsRetroActive(void) {
  return (enhanced_features1 & kFeatures1_RandomizerActive) &&
         Rando_GetActiveWorldState() == (uint8)kWorldState_Retro;
}

// genericKeys is pinned on for Retro (app/World/Retro.php), so the runtime gate
// equals Rando_IsRetroActive today. Distinct name: see the rando.h doc comment.
bool Rando_IsGenericKeysActive(void) {
  return Rando_IsRetroActive();
}

uint8 Rando_EffectiveKeySlot(uint8 key_slot) {
  return Rando_IsGenericKeysActive() ? 15 : key_slot;
}

// Retro genericKeys BUYABLE shop-slot grant (ALTTPR ShopKey, Randomizer.php:746-747
// — `$shop->addInventory(1, Item::get('ShopKey', ...), 100)`). The predicate VM
// treats holding >=1 GenericKey as opening EVERY small-key door (eval_has_item /
// eval_has_amount wildcard), but the placed GenericKey pool is FINITE (~30) and
// decrements per door, so keys spent out of order can strand a progression door.
// ALTTPR avoids that with an UNLIMITED buyable ShopKey supply; the fork had dropped
// it. This is that supply: each purchase feeds the shared counter exactly like a
// GenericKey pickup, with no cap on how many can be bought (re-enter the shop room
// to buy again). Same grant as rando_grant_generic_key(); the shop handler owns the
// rupee cost + purchase feedback. Only reached from the genericKeys-gated shop slot.
void Rando_GrantGenericKeyPurchase(void) {
  rando_grant_generic_key();
}

bool Rando_SuppressHyruleCastleEscape(void) {
  return (enhanced_features1 & kFeatures1_RandomizerActive) &&
         Rando_GetActiveWorldState() != (uint8)kWorldState_Standard;
}

bool Rando_MushroomHeld(void) {
  return g_rando_slot_active && (g_rando_mushroom_held & kRandoMushroom_Held) != 0;
}

void Rando_DeliverMushroom(void) {
  g_rando_mushroom_held &= (uint8)~kRandoMushroom_Held;  // keep PowderOwned bit
}

bool Rando_PowderOwned(void) {
  // Powder occupying the byte counts in vanilla and rando. The byte==2 check
  // first also covers pre-feature saves, where ownership wasn't recorded but
  // byte==2 still means the player has Powder.
  if (link_item_mushroom == 2) return true;
  return g_rando_slot_active &&
         (g_rando_mushroom_held & kRandoMushroom_PowderOwned) != 0;
}

bool Rando_MushroomPowderCanToggle(void) {
  if (!g_rando_slot_active) return false;
  bool has_mushroom = (g_rando_mushroom_held & kRandoMushroom_Held) != 0;
  return has_mushroom && Rando_PowderOwned();
}

// Flute/shovel decouple — see rando.h. Called from the receive-item path
// (AncillaAdd_ItemReceipt, misc.c) when a shovel/flute is granted under rando,
// instead of the vanilla unconditional write to link_item_flute. Records the
// item in the ownership bitfield (additive, never lost), then sets the shared
// link_item_flute slot to the SELECTED function with NEVER-DOWNGRADE semantics:
// the byte is the max of its current value and this item's level (shovel=1,
// inactive flute=2, active flute=3). When the seed's instant_flute setting is
// enabled, any flute pickup promotes directly to active, skipping the vanilla
// weathervane activation trip. So acquiring the shovel can never drop the slot
// below an owned flute (fixing "flute then shovel loses the flute"), while
// acquiring the flute selects it. Whenever both are owned the player flips the
// slot's function with the item-menu toggle (Hud_NormalMenu); that toggle may
// set the byte below this floor, and that's fine — it only persists a player
// choice and never runs at grant time.
void Rando_GrantFluteShovel(uint8 lttp_code) {
  uint8 floor;  // lowest link_item_flute value consistent with this item
  if (lttp_code == 0x13) {  // shovel
    g_rando_flute_shovel_owned |= kRandoFluteShovel_Shovel;
    floor = 1;
  } else {                  // 0x14 inactive flute, 0x4a active flute
    g_rando_flute_shovel_owned |= kRandoFluteShovel_Flute;
    if (lttp_code == 0x4a || rando_instant_flute_active()) {
      g_rando_flute_shovel_owned |= kRandoFluteShovel_FluteActive;
      floor = 3;
    } else {
      floor = 2;
    }
  }
  if (link_item_flute < floor)
    link_item_flute = floor;
}

bool Rando_FluteShovelCanToggle(void) {
  return g_rando_slot_active &&
         (g_rando_flute_shovel_owned & kRandoFluteShovel_Shovel) &&
         (g_rando_flute_shovel_owned &
          (kRandoFluteShovel_Flute | kRandoFluteShovel_FluteActive));
}

uint8 Rando_FluteShovelEffectiveLevel(void) {
  // The selected-function byte is the only ownership signal in vanilla — and in
  // a rando save written before the @69 ownership field existed — so start from
  // it. Under an active slot, also fold in the tracked ownership bits and take
  // the max, so owning a flute while the shovel is the selected function still
  // reads as "has the flute" (and never downgrades below the byte).
  uint8 level = link_item_flute;
  if (g_rando_slot_active) {
    uint8 owned = (g_rando_flute_shovel_owned & kRandoFluteShovel_FluteActive) ? 3
                : (g_rando_flute_shovel_owned & kRandoFluteShovel_Flute)       ? 2
                : (g_rando_flute_shovel_owned & kRandoFluteShovel_Shovel)      ? 1
                : 0;
    if (owned > level)
      level = owned;
  }
  return level;
}

// Boomerang/bow decouple — see rando.h. Called from the receive-item path
// (AncillaAdd_ItemReceipt, misc.c) under rando instead of the vanilla
// unconditional `*p = v`, so a lower-tier pickup never downgrades the slot and
// the higher tier is remembered for the item-menu swap.
void Rando_GrantBoomerang(void) {
  // Strictly PROGRESSIVE: the FIRST boomerang collected is always blue, the
  // SECOND always red — regardless of which item (Blue/Red) is actually placed
  // at the location. The ownership bits are the tier counter; the byte
  // (link_item_boomerang) tracks the currently-SELECTED color, which the player
  // swaps blue<->red in the item menu once both are owned (Hud_NormalMenu /
  // Rando_BoomerangCanToggle). Safe because no logic predicate ever requires a
  // boomerang of either color (verified: assets/rando has no Boomerang gate).
  //
  // Fold the current byte tier into ownership first (pre-feature-save / debug
  // poke compat): byte 1 (blue) implies blue owned; byte 2 (red) implies both,
  // since red can't exist without having advanced through blue.
  if (link_item_boomerang >= 1) g_rando_boomerang_owned |= kRandoBoomerang_Blue;
  if (link_item_boomerang >= 2) g_rando_boomerang_owned |= kRandoBoomerang_Red;
  // Advance to the next unowned tier (never downgrades — increment only).
  if (!(g_rando_boomerang_owned & kRandoBoomerang_Blue)) {
    g_rando_boomerang_owned |= kRandoBoomerang_Blue;
    if (link_item_boomerang < 1) link_item_boomerang = 1;  // select blue
  } else if (!(g_rando_boomerang_owned & kRandoBoomerang_Red)) {
    g_rando_boomerang_owned |= kRandoBoomerang_Red;
    link_item_boomerang = 2;  // select the newly-granted red
  }
  // both tiers owned: capped, no change.
}

void Rando_GrantBow(uint8 lttp_code) {
  // Fold the current tier into ownership (pre-feature save / debug-poke compat):
  // the byte only ever shows a tier the player actually has.
  if (link_item_bow >= 1) g_rando_bow_owned |= kRandoBow_Wood;
  if (link_item_bow >= 3) g_rando_bow_owned |= kRandoBow_Silver;
  bool silver = (lttp_code == 0x3b);
  // Silver arrows imply a basic bow, so record both — the swap is then available
  // even on a silver-first pickup, and the wood tier is never "lost".
  g_rando_bow_owned |= silver ? (kRandoBow_Silver | kRandoBow_Wood) : kRandoBow_Wood;
  uint8 target = silver ? 3 : 1;  // the no-arrows form of this strength tier
  if (link_item_bow < target) {
    // Raising the strength tier (or first pickup). Preserve the current arrow
    // bit so a silver upgrade on a wood+arrows bow stays "has arrows".
    bool has_arrows = (link_item_bow == 2 || link_item_bow == 4);
    link_item_bow = has_arrows ? (uint8)(target + 1) : target;
  }
  // link_item_bow >= target already (e.g. silver owned, re-granting wood): leave
  // the higher tier in place — never downgrade.
}

bool Rando_BoomerangCanToggle(void) {
  return g_rando_slot_active &&
         (g_rando_boomerang_owned & kRandoBoomerang_Blue) &&
         (g_rando_boomerang_owned & kRandoBoomerang_Red);
}

bool Rando_BowCanToggle(void) {
  // Owning silver implies the wood capability (Rando_GrantBow sets both bits),
  // so a single silver bit is enough to offer the wood<->silver arrow swap.
  return g_rando_slot_active && (g_rando_bow_owned & kRandoBow_Silver) != 0;
}

void Rando_MarkLocationChecked(uint16 location_id) {
  if (!g_rando_slot_active) return;
  if (location_id >= kRandoLocationCapacity) return;
  uint32 byte_idx = location_id >> 3;
  uint8 bit_mask = (uint8)(1u << (location_id & 7));
  bool already=(g_rando_checked_bitmap[byte_idx]&bit_mask)!=0;
  g_rando_checked_bitmap[byte_idx] |= bit_mask;
  if(!already) Rando_NotifyGoalCheck(location_id);
  // Mark the tracker's reachability cache stale so the overlay re-paints
  // the location's status glyph on the next draw.
  g_reachability_state_counter++;
}

bool Rando_IsLocationChecked(uint16 location_id) {
  if (!g_rando_slot_active) return false;
  if (location_id >= kRandoLocationCapacity) return false;
  return (g_rando_checked_bitmap[location_id >> 3] & (1u << (location_id & 7))) != 0;
}

bool Rando_HasLocationPlacement(uint16 location_id) {
  return Placement_TryLookup(location_id, NULL);
}

bool Rando_IsLocationCheckedOrVanilla(uint16 location_id,
                                      bool vanilla_completed) {
  uint16 placed;
  return Placement_TryLookup(location_id, &placed)
      ? Rando_IsLocationChecked(location_id)
      : vanilla_completed;
}

const RandoLocationDef *Rando_FindLocationDef(uint16 location_id) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    if (kRandoLocations[i].id == location_id)
      return &kRandoLocations[i];
  }
  return NULL;
}

void Rando_DungeonCheckCounts(uint8 rando_dungeon, uint16 *checked, uint16 *total) {
  static uint16 s_checked[kRandoDungeon_Count];
  static uint16 s_total[kRandoDungeon_Count];
  static const RandoPlacementTable *s_cached_table;
  static uint16 s_cached_table_count;
  static uint32 s_cached_counter;
  static uint8 s_cached_active;

  const RandoPlacementTable *table = Placement_GetActive();
  uint32 counter = Rando_GetReachabilityCounter();
  uint8 active = g_rando_slot_active != 0;
  bool stale = s_cached_table != table ||
               s_cached_table_count != (table ? table->count : 0) ||
               s_cached_counter != counter ||
               s_cached_active != active;
  if (stale) {
    memset(s_checked, 0, sizeof(s_checked));
    memset(s_total, 0, sizeof(s_total));
    if (active && table != NULL) {
      for (uint16 i = 0; i < table->count; i++) {
        uint16 loc_id = table->entries[i].location_id;
        const RandoLocationDef *loc = Rando_FindLocationDef(loc_id);
        if (loc == NULL || !Rando_LocationTypeCountsAsCheck(loc->type))
          continue;
        if (loc->region_id >= kRandoRegionsCount)
          continue;
        uint8 d = kRandoRegions[loc->region_id].dungeon_id;
        if (d >= kRandoDungeon_Count)
          continue;
        s_total[d]++;
        if (Rando_IsLocationChecked(loc_id))
          s_checked[d]++;
      }
    }
    s_cached_table = table;
    s_cached_table_count = table ? table->count : 0;
    s_cached_counter = counter;
    s_cached_active = active;
  }

  if (rando_dungeon >= kRandoDungeon_Count) {
    if (checked) *checked = 0;
    if (total) *total = 0;
    return;
  }
  if (checked) *checked = s_checked[rando_dungeon];
  if (total) *total = s_total[rando_dungeon];
}

void Rando_PopulateSlotBitmap(struct RandoSidecarSlot *out_slot) {
  if (out_slot == NULL || !g_rando_slot_active) return;
  // sizeof(out_slot->checked_bitmap) == kRandoCheckedBitmapBytes by
  // construction (both derived from kRandoLocationCapacity).
  memcpy(out_slot->checked_bitmap, g_rando_checked_bitmap, kRandoCheckedBitmapBytes);
  // Persist Mushroom possession alongside the checked bitmap so an undelivered
  // Mushroom survives save/reload (otherwise a reload could re-lock the
  // Potion Shop check).
  out_slot->header.mushroom_held = g_rando_mushroom_held;
  // Persist flute/shovel ownership so owning both survives save/reload (the
  // single link_item_flute byte can't carry it — see Rando_GrantFluteShovel).
  out_slot->header.flute_shovel_owned = g_rando_flute_shovel_owned;
  // Persist boomerang/bow ownership for the same reason: a swapped-down byte
  // (blue selected, or wood arrows selected) must not lose the higher tier
  // across save/reload. See Rando_GrantBoomerang / Rando_GrantBow.
  out_slot->header.boomerang_owned = g_rando_boomerang_owned;
  out_slot->header.bow_owned = g_rando_bow_owned;
  // add-enemy-souls — persist soul ownership (v6 ext block). Zero bitfield is a
  // valid "own nothing" state, so always mark present on a rando save.
  out_slot->header.souls_present = 1;
  memcpy(out_slot->header.soul_flags, Souls_Flags(), sizeof(out_slot->header.soul_flags));
  // tracker-player-knowledge — persist topology-discovery state.
  Rando_GetDiscoveryState(&out_slot->header.discovered_dungeons,
                          &out_slot->header.discovered_whirlpools,
                          out_slot->header.discovered_caves,
                          out_slot->header.discovered_exits);
  // Hints v2 persists only the certified plan identity and mutable discovery.
  // A legacy/failed-closed activation exports nothing, preserving its absent
  // metadata instead of silently blessing a current-algorithm reroll.
  RandoHintPersistedState hint_state;
  if (Rando_HintsExportPersistedState(&hint_state))
    out_slot->header.hint_state = hint_state;
}

// Private deterministic fault injection for the persistence self-check. The
// production value is always false; keeping the seam here lets the test prove
// the coordinator propagates a sidecar-write failure without touching a real
// saves/sram_rando.dat.
static bool g_rando_selftest_force_sidecar_save_failure;

bool Rando_OnGameSave(int slot_index, const uint8 *paired_sram_slot,
                      uint32 paired_sram_slot_size) {
  if (!g_rando_slot_active) return true;
  if (slot_index < 0 || slot_index >= 3) return false;  // 3 sidecar slots
  if (g_rando_selftest_force_sidecar_save_failure) return false;

  // Read the existing sidecar slot (preserves header + placement table).
  // An active slot must have its sidecar. Suppress the paired SRAM commit if
  // that durable half cannot be loaded or updated; otherwise spent rupees can
  // survive without the matching paid-hint discovery bit.
  RandoSidecarSlot slot;
  if (!Rando_LoadSidecarSlot(slot_index, &slot)) return false;
  if (slot.header.slot_kind != kSlotKind_Randomizer) return false;

  // Refresh the bitmap from the in-memory session and write back.
  Rando_PopulateSlotBitmap(&slot);
  return Rando_WriteSidecarSlot(slot_index, &slot, paired_sram_slot,
                                paired_sram_slot_size);
}

void Rando_GameSaveFailureSelfCheck(void) {
  uint8 saved_slot_active = g_rando_slot_active;
  bool saved_force_failure = g_rando_selftest_force_sidecar_save_failure;
  uint8 staged_sram[0x500];
  memset(staged_sram, 0, sizeof(staged_sram));

  // The test hook must never suppress an ordinary vanilla save.
  g_rando_selftest_force_sidecar_save_failure = true;
  g_rando_slot_active = 0;
  bool vanilla_allowed =
      Rando_OnGameSave(0, staged_sram, (uint32)sizeof(staged_sram));

  // Once a randomizer slot is active, the identical forced sidecar failure
  // must propagate false. SaveGameFile's direct boolean gate then suppresses
  // ZeldaWriteSram, preserving the sidecar-first paired-commit contract.
  g_rando_slot_active = 1;
  bool rando_allowed =
      Rando_OnGameSave(0, staged_sram, (uint32)sizeof(staged_sram));

  g_rando_slot_active = saved_slot_active;
  g_rando_selftest_force_sidecar_save_failure = saved_force_failure;

  if (!vanilla_allowed || rando_allowed) {
    fprintf(stderr,
            "[Rando_GameSaveFailureSelfCheck] FAIL: sidecar failure "
            "propagation/vanilla bypass\n");
    exit(2);
  }
  fprintf(stderr, "[Rando_GameSaveFailureSelfCheck] OK\n");
}

// ---------------------------------------------------------------------------
// §7.6 — generic confirmation cue for direct-grant placements that skip
// Link_ReceiveItem entirely. Matches the standard receive-item sound used
// inside AncillaAdd_ItemReceipt (`sound_effect_2 = Link_CalculateSfxPan() | 0xf`)
// so the auditory feedback is identical to a normal pickup. Refreshes the HUD so
// any inventory cell that changed (prize bits, dungeon-item bits, Triforce
// counter) reflects immediately rather than waiting for the next implicit
// refresh.
//
// Phase B Slice 9 (add-rando-confirmation-icons): extends Phase A's audio +
// HUD-only cue with a visible icon ancilla. Trap items get a deterministic
// good-item decoy selected from the active seed/location/trap type. Other
// direct-grant item ids are looked up in kDirectGrantIcons[item_id] (codegen'd
// from assets/rando/direct_grant_icons.yaml). Magic upgrades are resolved
// through the current progressive tier first, so either raw HalfMagic or
// QuarterMagic shows 1/2 on the first upgrade and 1/4 on the second. When an
// icon resolves, AncillaAdd_RandoIconReceipt DMAs that bundle and pops the icon
// above Link's head. Unmapped entries fall back to Phase A audio + HUD behavior
// — never crash, never spawn a blank ancilla.
//
// Deliberately NOT emitted from within `Rando_DispatchVanillaGrant` — the
// caller knows whether its own code path already provides visual context
// (e.g., the §6.6 boss-kill spawns a FallingPrize regardless of sentinel,
// so the player sees that sprite). Pushing the confirmation to the call
// site lets each integration choose whether to add the cue.
// ---------------------------------------------------------------------------
// Tier-1 receive-gfx icon resolver (defined below, after rando_item_display_lttp).
// Shared by the direct-grant confirmation cue and the field-item-sprite resolver
// so a draw/grant drift can't reappear.
static bool rando_receive_item_icon(uint16 item_id, uint8 *out_gfx, uint8 *out_big,
                                    uint8 *out_oam_flags);
static bool rando_receive_icon_for_code(uint8 code, uint8 *out_gfx, uint8 *out_big,
                                        uint8 *out_oam_flags);

static void rando_direct_grant_chime_and_hud(void) {
  sound_effect_2 = (uint8)(Link_CalculateSfxPan() | 0x0f);
  Hud_RefreshIcon();
}

static void rando_show_direct_grant_icon_only(uint8 item_id) {
  // every caller passes `(uint8)Rando_LastDispatched
  // ItemId()`; the cast loses precision if the sentinel value 0xFFFF
  // ever reaches us. The skip-sentinel path only runs AFTER a successful
  // Rando_DispatchVanillaGrant, which populates g_last_dispatched_item_id
  // with a valid item id (< ITEM__COUNT), so the
  // truncation is unreachable in normal flow. The icon-entry helper defends the
  // array access regardless; this assert just makes the invariant explicit so a
  // future change that calls this WITHOUT a prior dispatch fires loudly.
  assert(item_id != 0xFFu /* sentinel byte from 0xFFFF truncation */ ||
         g_last_dispatched_item_id != 0xFFFFu);

  RandoGrantPlan plan;
  bool valid = g_last_dispatched_plan_valid &&
               g_last_dispatched_plan.item_id == item_id;
  if (valid)
    plan = g_last_dispatched_plan;
  else
    valid = Rando_ResolveLiveGrantPlan(g_last_dispatched_location_id,
                                       item_id, &plan);
  if (valid && plan.display_valid)
    AncillaAdd_RandoIconReceipt(plan.display_gfx, plan.display_big,
                                plan.display_oam_flags);
}

// Vanilla receive code for a dungeon-prize item, or 0 if `item_id` is not one.
//
// Dungeon prizes are DIRECT grants (kRandoGrantOp_DirectPrize): the dispatch
// writes the placed prize's pendant/crystal bit itself, because under
// prize_shuffle the dungeon's vanilla bit is the wrong one. But the vanilla
// RECEIPT is what plays the boss-prize fanfare and — for a crystal —
// transmutes into the rising-crystal cutscene whose tail sets
// submodule_index = 0x18, the only thing that warps the player out of a boss
// room that has sealed behind them. Direct-granting alone therefore banked the
// prize and left the player stuck (playtest: Palace of Darkness).
//
// Running the vanilla receipt under rando is pure PRESENTATION and cannot
// double-grant: kValueToGiveItemTo[] is -1 for all four prize codes so the
// generic table write is skipped, and both explicit bit-ORs — the pendant one
// in ItemReceipt_GrantInventory and the crystal one in Ancilla_RisingCrystal —
// are already suppressed while rando is active, precisely because "the
// dispatch path owns the bit". This restarts a chain those comments already
// assumed was running.
//
// Codes are keyed by prize ITEM id, never by colour name: the registry's
// Red/Blue Pendant names are swapped relative to the drawn colour, and the
// codes follow the BIT each prize sets (0x37 -> 4 = EP/green, 0x39 -> 2 =
// DP/red, 0x38 -> 1 = TH/blue; see kDungeonCrystalPendantBit).
static uint8 rando_prize_vanilla_receive_code(uint16 item_id) {
  switch (item_id) {
    case ITEM_Prize_GreenPendant: return 0x37;
    case ITEM_Prize_RedPendant:   return 0x39;
    case ITEM_Prize_BluePendant:  return 0x38;
    case ITEM_Prize_Crystal1: case ITEM_Prize_Crystal2:
    case ITEM_Prize_Crystal3: case ITEM_Prize_Crystal4:
    case ITEM_Prize_Crystal5: case ITEM_Prize_Crystal6:
    case ITEM_Prize_Crystal7: return 0x20;
    default: return 0;
  }
}

void Rando_ShowDirectGrantConfirmation(uint8 item_id) {
  if (g_last_dispatched_plan_valid &&
      g_last_dispatched_plan.item_id == item_id &&
      g_last_dispatched_plan.disposition == kRandoGrantDisposition_RetryableFailure)
    return;
  // Traps deliberately start with the normal direct-grant chime; the trap-owned
  // delayed bad cue fires a few frames later so the pickup reads as a fakeout.
  rando_direct_grant_chime_and_hud();
  // add-*-souls — souls all share one generic icon, so the floating blob can't
  // say WHICH soul was found (and is easy to miss in motion). Show a named
  // item-get box instead ("Got the <Name> Soul!"), deferred to a safe frame.
  // The box IS the indicator, so skip the generic icon for souls.
  if (Souls_ItemIsSoul(item_id)) {
    Rando_QueueSoulPickupMessage(item_id);
    return;
  }
  rando_show_direct_grant_icon_only(item_id);
}

typedef struct RandoPotCarrySnapshot {
  uint8 flag_immobilized;
  uint8 flag_sprite_pickup;
  uint8 flag_ancilla_pickup;
  uint8 flag_sprite_pickup_cached;
  uint8 pickup_handshake;
  uint8 player_handler;
  uint8 item_in_hand;
  uint8 state_bits;
  uint8 picking_throw_state;
  uint8 anim_timer_steps;
  uint8 anim_timer;
  uint8 button_mask;
  uint8 a_button;
  uint8 button_frames;
  uint8 speed;
  uint8 cant_change_dir;
  uint8 player_handler_state;
  uint8 pose_for_item;
  uint8 position_mode;
  uint8 disable_sprite_damage;
} RandoPotCarrySnapshot;

static RandoPotCarrySnapshot rando_pot_capture_carry_state(void) {
  RandoPotCarrySnapshot s;
  s.flag_immobilized = flag_is_link_immobilized;
  s.flag_sprite_pickup = flag_is_sprite_to_pick_up;
  s.flag_ancilla_pickup = flag_is_ancilla_to_pick_up;
  s.flag_sprite_pickup_cached = flag_is_sprite_to_pick_up_cached;
  s.pickup_handshake = byte_7E0FB2;
  s.player_handler = player_handler_timer;
  s.item_in_hand = link_item_in_hand;
  s.state_bits = link_state_bits;
  s.picking_throw_state = link_picking_throw_state;
  s.anim_timer_steps = some_animation_timer_steps;
  s.anim_timer = some_animation_timer;
  s.button_mask = button_mask_b_y;
  s.a_button = bitfield_for_a_button;
  s.button_frames = button_b_frames;
  s.speed = link_speed_setting;
  s.cant_change_dir = link_cant_change_direction;
  s.player_handler_state = link_player_handler_state;
  s.pose_for_item = link_pose_for_item;
  s.position_mode = link_position_mode;
  s.disable_sprite_damage = link_disable_sprite_damage;
  return s;
}

static void rando_pot_restore_carry_state(const RandoPotCarrySnapshot *s) {
  flag_is_link_immobilized = s->flag_immobilized;
  flag_is_sprite_to_pick_up = s->flag_sprite_pickup;
  flag_is_ancilla_to_pick_up = s->flag_ancilla_pickup;
  flag_is_sprite_to_pick_up_cached = s->flag_sprite_pickup_cached;
  byte_7E0FB2 = s->pickup_handshake;
  player_handler_timer = s->player_handler;
  link_item_in_hand = s->item_in_hand;
  link_state_bits = s->state_bits;
  link_picking_throw_state = s->picking_throw_state;
  some_animation_timer_steps = s->anim_timer_steps;
  some_animation_timer = s->anim_timer;
  button_mask_b_y = s->button_mask;
  bitfield_for_a_button = s->a_button;
  button_b_frames = s->button_frames;
  link_speed_setting = s->speed;
  link_cant_change_direction = s->cant_change_dir;
  link_player_handler_state = s->player_handler_state;
  link_pose_for_item = s->pose_for_item;
  link_position_mode = s->position_mode;
  link_disable_sprite_damage = s->disable_sprite_damage;
}

enum { kRandoPotConfirmationQueueMax = 8 };
static uint8 g_rando_pot_confirmation_count;
static DirectGrantIconEntry g_rando_pot_confirmation_icons[kRandoPotConfirmationQueueMax];

static bool rando_pot_confirmation_safe_to_emit(void) {
  return rando_trap_stun_can_tick() && submodule_index == 0;
}

static bool rando_resolve_pot_confirmation_icon(uint16 item_id, uint8 lttp_code,
                                                DirectGrantIconEntry *out) {
  if (out == NULL)
    return false;

  if (g_last_dispatched_plan_valid &&
      g_last_dispatched_plan.item_id == item_id &&
      g_last_dispatched_plan.display_valid) {
    out->gfx = g_last_dispatched_plan.display_gfx;
    out->big = g_last_dispatched_plan.display_big;
    out->oam_flags = g_last_dispatched_plan.display_oam_flags;
    return true;
  }

  // Non-direct items resolve from the pre-grant LttP receive code so progressive
  // tiers show the item actually collected, not the next tier after the grant.
  if (!Rando_ShouldSkipReceive(lttp_code)) {
    uint8 ig, ib, io;
    if (rando_receive_icon_for_code(lttp_code, &ig, &ib, &io)) {
      out->gfx = ig;
      out->big = ib;
      out->oam_flags = io;
      return true;
    }
  }

  if (rando_trap_decoy_icon(item_id, g_last_dispatched_location_id, out))
    return true;

  const DirectGrantIconEntry *e =
      rando_direct_grant_icon_entry(rando_direct_grant_icon_item_post_grant(item_id));
  if (e != NULL) {
    *out = *e;
    return true;
  }

  uint8 gfx, big, oam;
  if (rando_receive_item_icon(item_id, &gfx, &big, &oam)) {
    out->gfx = gfx;
    out->big = big;
    out->oam_flags = oam;
    return true;
  }
  return false;
}

void Rando_ClearDeferredPotConfirmation(void) {
  g_rando_pot_confirmation_count = 0;
}

static void rando_queue_pot_confirmation(uint16 item_id, uint8 lttp_code) {
  DirectGrantIconEntry icon;
  if (rando_resolve_pot_confirmation_icon(item_id, lttp_code, &icon)) {
    if (g_rando_pot_confirmation_count < kRandoPotConfirmationQueueMax) {
      g_rando_pot_confirmation_icons[g_rando_pot_confirmation_count++] = icon;
    } else {
      // Preserve ordering for the queue we can show; coalesce overflow into the
      // newest tail instead of losing every subsequent pickup until it drains.
      g_rando_pot_confirmation_icons[kRandoPotConfirmationQueueMax - 1] = icon;
    }
  }
  rando_direct_grant_chime_and_hud();
}

static void rando_tick_deferred_pot_confirmation(void) {
  if (g_rando_pot_confirmation_count == 0)
    return;
  if (!(enhanced_features1 & kFeatures1_RandomizerActive)) {
    g_rando_pot_confirmation_count = 0;
    return;
  }
  if (!rando_pot_confirmation_safe_to_emit())
    return;
  if (rando_receive_icon_active())
    return;

  DirectGrantIconEntry icon = g_rando_pot_confirmation_icons[0];
  if (AncillaAdd_RandoIconReceipt(icon.gfx, icon.big, icon.oam_flags)) {
    if (--g_rando_pot_confirmation_count != 0) {
      memmove(&g_rando_pot_confirmation_icons[0],
              &g_rando_pot_confirmation_icons[1],
              g_rando_pot_confirmation_count * sizeof(g_rando_pot_confirmation_icons[0]));
    }
  }
}

static RandoGrantResult rando_validate_prepared_token(
    const RandoDeferredGrantToken *token) {
  if (token == NULL || token->version != kRandoDeferredGrantTokenVersion)
    return kRandoGrantResult_Invalid;
  // Integrity is checked before placement/check lookup so corrupted location,
  // item, or disposition bytes cannot redirect validation or touch gameplay.
  if (token->reserved !=
      rando_deferred_grant_integrity_tag(token->version, &token->plan))
    return kRandoGrantResult_Invalid;
  if (token->plan.disposition == kRandoGrantDisposition_Invalid)
    return kRandoGrantResult_Invalid;
  uint16 current;
  if (!Placement_TryLookup(token->plan.location_id, &current))
    return kRandoGrantResult_NotActive;
  if (current != token->plan.item_id)
    return kRandoGrantResult_Invalid;
  if (Rando_IsLocationChecked(token->plan.location_id))
    return kRandoGrantResult_AlreadyChecked;
  if (token->plan.disposition == kRandoGrantDisposition_RetryableFailure)
    return kRandoGrantResult_Retryable;
  return kRandoGrantResult_Accepted;
}

RandoGrantResult Rando_CommitStandingPieceOfHeartIdentity(
    const RandoDeferredGrantToken *token) {
  RandoGrantResult result = rando_validate_prepared_token(token);
  if (result != kRandoGrantResult_Accepted)
    return result;
  const RandoGrantPlan *plan = &token->plan;
  // At 20 hearts the plan resolves to AcceptedNoOp (nothing to apply), but the
  // check must still commit: the caller's vanilla presentation is already the
  // correct at-cap behavior, and rejecting would strand the location (and the
  // sprite) uncollectable forever — capacity never decreases.
  if (plan->item_id != ITEM_PieceOfHeart ||
      plan->opcode != kRandoGrantOp_Receive ||
      (plan->disposition != kRandoGrantDisposition_Receive &&
       plan->disposition != kRandoGrantDisposition_AcceptedNoOp) ||
      plan->receive_code != 0x17)
    return kRandoGrantResult_Invalid;
  Rando_MarkLocationChecked(plan->location_id);
  rando_record_committed_item_ownership(plan->item_id);
  g_last_dispatched_item_id = plan->item_id;
  g_last_dispatched_location_id = plan->location_id;
  g_last_dispatched_plan = *plan;
  g_last_dispatched_plan_valid = true;
  return kRandoGrantResult_Accepted;
}

RandoGrantResult Rando_CommitPreparedGrant(
    const RandoDeferredGrantToken *token,
    RandoGrantPresentation presentation,
    uint8 receipt_method, uint16 chest_position) {
  if (presentation > kRandoGrantPresentation_None)
    return kRandoGrantResult_Invalid;
  RandoGrantResult result = rando_validate_prepared_token(token);
  if (result != kRandoGrantResult_Accepted)
    return result;

  const RandoGrantPlan *plan = &token->plan;
  // A plan built while a bottle slot was free can be committed after the player
  // picked up a fourth bottle. That late refusal is just as permanent as an
  // early one, so it terminates here rather than looping the caller forever.
  bool deliver = true;
  if (!Rando_CanAcceptGrantPlanNow(plan)) {
    if (plan->disposition != kRandoGrantDisposition_Receive ||
        !Rando_ReceiveCodePermanentlyBlocked(plan->receive_code))
      return plan->disposition == kRandoGrantDisposition_Receive
          ? kRandoGrantResult_Retryable : kRandoGrantResult_Invalid;
    deliver = false;  // consume the check, grant nothing, do not retry
  }
  if (!deliver) {
    // fall through to the commit bookkeeping below with no delivery
  } else if (plan->disposition == kRandoGrantDisposition_Receive) {
    if (plan->receive_code >= 76)
      return kRandoGrantResult_Invalid;
    item_receipt_method = receipt_method;
    if (presentation == kRandoGrantPresentation_Animated) {
      Link_ReceiveItem(plan->receive_code, chest_position);
    } else if (!ItemReceipt_GrantWithoutAnimation(plan->receive_code)) {
      return kRandoGrantResult_Invalid;
    }
  } else if (plan->disposition == kRandoGrantDisposition_Direct) {
    if (!rando_apply_direct_grant_plan(plan))
      return kRandoGrantResult_Invalid;
  } else if (plan->disposition != kRandoGrantDisposition_AcceptedNoOp) {
    return plan->disposition == kRandoGrantDisposition_RetryableFailure
        ? kRandoGrantResult_Retryable : kRandoGrantResult_Invalid;
  }

  // Commit persistence/derived ownership only after the grant path accepted.
  Rando_MarkLocationChecked(plan->location_id);
  rando_record_committed_item_ownership(plan->item_id);
  g_last_dispatched_item_id = plan->item_id;
  g_last_dispatched_location_id = plan->location_id;
  g_last_dispatched_plan = *plan;
  g_last_dispatched_plan_valid = true;

  if (presentation == kRandoGrantPresentation_Quiet &&
      plan->item_id != ITEM_Nothing) {
    // Souls deliberately use a named deferred dialogue instead of the shared
    // generic soul icon. Keep Quiet sources on the same one-cue contract as
    // animated direct grants: chime/HUD now, one named box when gameplay is
    // quiescent, and no duplicate floating confirmation icon.
    if (Souls_ItemIsSoul(plan->item_id)) {
      rando_direct_grant_chime_and_hud();
      Rando_QueueSoulPickupMessage(plan->item_id);
    } else {
      rando_queue_pot_confirmation(
          plan->item_id,
          plan->disposition == kRandoGrantDisposition_Receive
              ? plan->receive_code : kRandoLttpSkip);
    }
  } else if (presentation == kRandoGrantPresentation_Animated &&
             plan->disposition != kRandoGrantDisposition_Receive) {
    // A dungeon prize runs the VANILLA receipt for its cutscene/fanfare rather
    // than the floating confirmation icon — see
    // rando_prize_vanilla_receive_code for why this cannot double-grant.
    uint8 prize_code = rando_prize_vanilla_receive_code(plan->item_id);
    if (prize_code != 0) {
      item_receipt_method = receipt_method;
      Link_ReceiveItem(prize_code, chest_position);
    } else {
      Rando_ShowDirectGrantConfirmation((uint8)plan->item_id);
    }
  }
  return kRandoGrantResult_Accepted;
}

RandoGrantResult Rando_GrantLocation(uint16 location_id,
                                     uint16 vanilla_registry_id,
                                     uint8 vanilla_lttp_code,
                                     RandoGrantPresentation presentation,
                                     uint8 receipt_method,
                                     uint16 chest_position) {
  RandoDeferredGrantToken token;
  RandoGrantResult result = Rando_PrepareGrant(
      location_id, vanilla_registry_id, vanilla_lttp_code, &token);
  return result == kRandoGrantResult_Accepted
      ? Rando_CommitPreparedGrant(&token, presentation,
                                  receipt_method, chest_position)
      : result;
}

RandoGrantResult Rando_ForfeitLocation(uint16 location_id) {
  uint16 ignored;
  if (!Placement_TryLookup(location_id, &ignored))
    return kRandoGrantResult_NotActive;
  if (Rando_IsLocationChecked(location_id))
    return kRandoGrantResult_AlreadyChecked;
  Rando_MarkLocationChecked(location_id);
  return kRandoGrantResult_Accepted;
}

RandoGrantResult Rando_CommitRepeatableCapacityIdentity(
    uint16 location_id, uint16 vanilla_registry_id) {
  uint16 placed;
  if (!Placement_TryLookup(location_id, &placed))
    return kRandoGrantResult_NotActive;
  if (placed != vanilla_registry_id || placed >= kRandoItemGrantMetadataCount)
    return kRandoGrantResult_Invalid;
  uint8 opcode = kRandoItemGrantMetadata[placed].opcode;
  if (opcode != kRandoGrantOp_DirectBombCapacity &&
      opcode != kRandoGrantOp_DirectArrowCapacity)
    return kRandoGrantResult_Invalid;
  Rando_MarkLocationChecked(location_id);
  return kRandoGrantResult_Accepted;
}

// ---------------------------------------------------------------------------
// Field item sprites (add-rando-field-item-sprites) — resolver half. The draw
// half (gfx DMA + OAM) lives in sprite.c. See rando.h for the contract.
// ---------------------------------------------------------------------------
bool Rando_FieldItemSpritesActive(void) {
  // Active rando slot AND the client-local field_item_sprites toggle (read live
  // from zelda3.ini / the native window, so toggling takes effect without a
  // restart). Inert in non-rando play regardless of the toggle.
  return (enhanced_features1 & kFeatures1_RandomizerActive) != 0 &&
         g_config.field_item_sprites;
}

// Palette/priority table, indexed by LttP receive code (defined in sprite_main.c,
// declared in sprite.h — redeclared here to avoid pulling sprite.h into rando.c).
extern const uint8 kWishPond2_OamFlags[76];

// Compatibility wrapper for callers that only need the plan's frozen display
// code. The semantic resolver owns progressive and shared-byte decisions, so
// this path cannot drift from dispatch.
static uint8 rando_item_display_lttp(uint16 placed) {
  RandoGrantPlan plan;
  return Rando_ResolveLiveGrantPlan(0xffffu, placed, &plan)
      ? plan.display_code : 0xff;
}

// add-rando-pot-sanity — Tier-1 receive-gfx icon for an item (rupees, equipment,
// bottles, boomerang, ...). Mirrors Ancilla_ReceiveItem_Draw EXACTLY (gfx, size,
// palette indexed by the LttP receive code) so a floating cue looks like the
// vanilla held-aloft pickup. Returns false for items with no receive gfx (keys /
// custom art resolve via rando_direct_grant_icon_entry instead). Shared by
// Rando_ShowDirectGrantConfirmation and Rando_GetFieldItemIcon so a draw/grant
// drift (the rupee/boomerang-as-vanilla-sprite bug class) can't reappear.
static bool rando_receive_icon_for_code(uint8 code, uint8 *out_gfx, uint8 *out_big,
                                        uint8 *out_oam_flags) {
  if (code >= 76 || kReceiveItemGfx[code] == 0xff)
    return false;
  *out_gfx = kReceiveItemGfx[code];
  *out_big = kReceiveItem_Tab1[code];
  uint8 a = kWishPond2_OamFlags[code];
  if (a & 0x80)               // sign8 fallback, matching Ancilla_ReceiveItem_Draw
    a = 5;
  *out_oam_flags = (uint8)(a * 2 | 0x30);
  return true;
}
// Resolve from the item's POST-grant display code. A caller that needs the
// PRE-grant code (the boomerang colour flips in g_rando_boomerang_owned on grant,
// so a post-grant recompute pops the NEXT colour) calls rando_receive_icon_for_code
// directly with the code it was handed.
static bool rando_receive_item_icon(uint16 item_id, uint8 *out_gfx, uint8 *out_big,
                                    uint8 *out_oam_flags) {
  return rando_receive_icon_for_code(rando_item_display_lttp(item_id),
                                     out_gfx, out_big, out_oam_flags);
}

// ---------------------------------------------------------------------------
// add-rando-pot-sanity Phase 4 — runtime pot grant hook + recolor gate.
// ---------------------------------------------------------------------------

// Binary search the (room,pos4)-sorted pot lookup for a registered pot's LOC_*,
// or 0xFFFF when (room,pos4) is not a registered pot — the ThievesAttic 0x2020
// lightenable hole, or any excluded/structural pot, so the hook is inert by
// construction for those (lookup-gated, design D3). RoomDraw_SinglePot computes
// the SAME pos4 = (dsto*2) | (0x2000 BG-half) that gen_pot_tables emitted.
uint16 Rando_GetPotLocation(uint16 room, uint16 pos4) {
  uint32 key = ((uint32)room << 16) | pos4;
  int lo = 0, hi = (int)kRandoPotLookup_COUNT;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    uint32 mk = ((uint32)kRandoPotLookup[mid].room << 16) | kRandoPotLookup[mid].pos4;
    if (mk == key) return kRandoPotLookup[mid].loc_id;
    if (mk < key) lo = mid + 1; else hi = mid;
  }
  return 0xFFFF;
}

// A location's vanilla item id from the generated registry (kRandoLocations is
// sorted ascending by id), or 0xFFFF if not found. Used to classify a pot's
// vanilla content (key 53..65 / empty ITEM_Nothing / loot) for checked-pot reuse.
static uint16 rando_location_vanilla_item(uint16 loc_id) {
  int lo = 0, hi = (int)kRandoLocationsCount;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    uint16 mid_id = kRandoLocations[mid].id;
    if (mid_id == loc_id) return kRandoLocations[mid].vanilla_item_id;
    if (mid_id < loc_id) lo = mid + 1; else hi = mid;
  }
  return 0xFFFF;
}

// Is this pot ACTIVE this seed? A pot whose tier selected it has a placement
// table entry (inactive pots are skipped in the open-loc loop, so absent). Empty
// pots are pinned to ITEM_Nothing, so they too are present when active.
static bool rando_pot_is_active(uint16 loc) {
  return Placement_Lookup(loc, 0xFFFFu) != 0xFFFFu;
}

uint8 Rando_PotBreakHook(uint16 room, uint16 pos4) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive))
    return kRandoPot_Vanilla;
  uint16 loc = Rando_GetPotLocation(room, pos4);
  if (loc == 0xFFFF)
    return kRandoPot_Vanilla;       // not a registered pot (e.g. ThievesAttic hole)
  if (!rando_pot_is_active(loc))
    return kRandoPot_Vanilla;       // inactive (tier off / door shuffle) -> vanilla

  uint16 vanilla = rando_location_vanilla_item(loc);
  bool one_shot = (vanilla >= 53 && vanilla <= 65) || vanilla == ITEM_Nothing;

  if (Rando_IsLocationChecked(loc)) {
    // Checked: a one-shot pot (key / empty) is SUPPRESSED — vanilla has no
    // per-pot key-taken flag, and content 8 (small key) bypasses the room mask,
    // so re-dropping would duplicate the key (design D3/R3). An item-pot re-drops
    // its vanilla content (repeatable, exactly like vanilla — the user's "vanilla
    // item under it after checked").
    return one_shot ? kRandoPot_Suppress : kRandoPot_Vanilla;
  }

  RandoGrantResult result = Rando_GrantLocation(
      loc, 0xFFFFu, kRandoLttpSkip, kRandoGrantPresentation_Quiet, 0, 0);
  if (result == kRandoGrantResult_Accepted ||
      result == kRandoGrantResult_AlreadyChecked)
    return kRandoPot_Suppress;
  if (result == kRandoGrantResult_NotActive)
    return kRandoPot_Vanilla;
  return kRandoPot_Retry;
}

bool Rando_PotShouldRecolor(uint16 room, uint16 pos4) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive))
    return false;
  uint16 loc = Rando_GetPotLocation(room, pos4);
  if (loc == 0xFFFF || !rando_pot_is_active(loc))
    return false;
  return !Rando_IsLocationChecked(loc);  // un-checked active pots draw recolored
}

// ===========================================================================
// add-rando-grass-rock-shuffle — overworld terrain reveal hook (Phase 4).
//
// The hook is invoked from the FOUR CONSUMING overworld call sites, each just
// before its Overworld_RevealSecret call (Overworld_LiftingSmallObj,
// SmashRockPile_fromLift, the bush/thick-grass branch of
// Overworld_ToolAndTileInteraction, and the bush/thick-grass branch of
// Overworld_BombTile). It is deliberately NOT inside Overworld_RevealSecret:
// the bomb path also calls that function SPECULATIVELY for blast tiles it does
// not consume (the label_a staircase probe + the super-bomb blanket probe),
// which would grant glove-gated rock checks from an adjacent bomb (review
// finding H1). See the change's design D9.
// ===========================================================================
uint16 Rando_GetTerrainLocation(uint16 screen, uint16 pos) {
  uint32 key = ((uint32)screen << 16) | pos;
  int lo = 0, hi = (int)kRandoTerrainLookup_COUNT;
  while (lo < hi) {
    int mid = lo + (hi - lo) / 2;
    uint32 mk = ((uint32)kRandoTerrainLookup[mid].screen << 16) | kRandoTerrainLookup[mid].pos;
    if (mk == key) return kRandoTerrainLookup[mid].loc_id;
    if (mk < key) lo = mid + 1; else hi = mid;
  }
  return 0xFFFF;
}

// Active this seed? A terrain location whose axis tier selected it has a
// placement-table entry (inactive terrain is skipped in the open-loc loop, so
// absent). Same shape as rando_pot_is_active.
static bool rando_terrain_is_active(uint16 loc) {
  return Placement_Lookup(loc, 0xFFFFu) != 0xFFFFu;
}

uint8 Rando_TerrainRevealHook(uint16 screen, uint16 pos) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive))
    return kRandoTerrain_Vanilla;
  uint16 loc = Rando_GetTerrainLocation(screen, pos);
  if (loc == 0xFFFF)
    return kRandoTerrain_Vanilla;      // not a registered terrain object
  if (!rando_terrain_is_active(loc))
    return kRandoTerrain_Vanilla;      // axis off -> vanilla secret behavior
  if (Rando_IsLocationChecked(loc)) {
    // Checked: replay VANILLA. Vanilla overworld secrets are infinitely
    // refarmable (bushes respawn on re-entry) and carry no progression, so
    // replay introduces no dupes and preserves fairy/bee farming spots
    // (design D9). No one-shot suppression needed — unlike dungeon pots there
    // is no shared key byte to double-grant.
    return kRandoTerrain_Vanilla;
  }
  RandoGrantResult result = Rando_GrantLocation(
      loc, 0xFFFFu, kRandoLttpSkip, kRandoGrantPresentation_Quiet, 0, 0);
  if (result == kRandoGrantResult_Accepted ||
      result == kRandoGrantResult_AlreadyChecked)
    return kRandoTerrain_Suppress;
  if (result == kRandoGrantResult_NotActive)
    return kRandoTerrain_Vanilla;
  return kRandoTerrain_Retry;
}

// ---- Gold "check" pot overlay (rando.h) -----------------------------------
// Per-room list of in-scope un-checked pots, captured once at room load (from
// RoomDraw_SinglePot) and drawn every dungeon frame. A room packs at most 16
// misc objects (dung_bg2_attr_table stores a 4-bit slot index), so 16 is the
// hard cap. The list is a C static (cosmetic + re-derived on every room
// (re)load), so it needs no snapshot/replay serialization.
#define kRandoPotOverlayMax 16
static uint16 s_pot_overlay_pos[kRandoPotOverlayMax];
static uint8  s_pot_overlay_count;

void Rando_PotOverlayReset(void) {
  s_pot_overlay_count = 0;
}

void Rando_PotOverlayCapture(uint16 room, uint16 pos4) {
  if (s_pot_overlay_count >= kRandoPotOverlayMax)
    return;
  if (!Rando_PotShouldRecolor(room, pos4))   // rando + active-tier + un-checked
    return;
  s_pot_overlay_pos[s_pot_overlay_count++] = pos4;
}

uint8 Rando_PotOverlayCount(void) {
  return s_pot_overlay_count;
}

uint16 Rando_PotOverlayPos(uint8 i) {
  return i < s_pot_overlay_count ? s_pot_overlay_pos[i] : 0;
}

static uint8 s_rando_obj_scratch_owner;
static uint8 s_rando_obj_scratch_frame;

static void Rando_ObjScratchSyncFrame(void) {
  if (s_rando_obj_scratch_frame == frame_counter)
    return;
  s_rando_obj_scratch_frame = frame_counter;
  s_rando_obj_scratch_owner = kRandoObjScratchOwner_None;
}

bool Rando_ObjScratchReserveForFrame(uint8 owner) {
  Rando_ObjScratchSyncFrame();
  if (owner == kRandoObjScratchOwner_None)
    return false;
  if (s_rando_obj_scratch_owner == kRandoObjScratchOwner_None ||
      s_rando_obj_scratch_owner == owner) {
    s_rando_obj_scratch_owner = owner;
    return true;
  }
  return false;
}

void Rando_ObjScratchResetFrameReservation(void) {
  s_rando_obj_scratch_owner = kRandoObjScratchOwner_None;
  // Force the next reservation through SyncFrame even if a loaded snapshot
  // happens to have the same frame-counter value as the pre-load state.
  s_rando_obj_scratch_frame = (uint8)(frame_counter - 1);
}

enum {
  kRandoOverlayPaletteKind_Gold = 1,
  kRandoOverlayPaletteKind_CustomItem = 2,
  kRandoOverlayPaletteMax = 8,
};

typedef struct RandoOverlayPaletteRequest {
  uint8 row;
  uint8 kind;
  uint8 gfx;
} RandoOverlayPaletteRequest;

typedef struct RandoOverlayPalettePrevious {
  uint8 row;
  uint16 colors[16];
} RandoOverlayPalettePrevious;

static RandoOverlayPaletteRequest s_overlay_palette_requests[kRandoOverlayPaletteMax];
static uint8 s_overlay_palette_request_count;
static uint8 s_overlay_palette_request_frame;
static RandoOverlayPalettePrevious s_overlay_palette_previous[kRandoOverlayPaletteMax];
static uint8 s_overlay_palette_previous_count;

static void Rando_OverlayPaletteSyncFrame(void) {
  if (s_overlay_palette_request_frame == frame_counter)
    return;
  s_overlay_palette_request_frame = frame_counter;
  s_overlay_palette_request_count = 0;
}

static bool Rando_OverlayPaletteAddRequest(uint8 row, uint8 kind, uint8 gfx) {
  Rando_OverlayPaletteSyncFrame();
  if (row >= 7)
    return false;
  for (uint8 i = 0; i < s_overlay_palette_request_count; i++) {
    if (s_overlay_palette_requests[i].row == row) {
      s_overlay_palette_requests[i].kind = kind;
      s_overlay_palette_requests[i].gfx = gfx;
      return true;
    }
  }
  if (s_overlay_palette_request_count >= kRandoOverlayPaletteMax)
    return false;
  s_overlay_palette_requests[s_overlay_palette_request_count++] =
      (RandoOverlayPaletteRequest){row, kind, gfx};
  return true;
}

bool Rando_OverlayPaletteRequestGold(uint8 row) {
  return Rando_OverlayPaletteAddRequest(row, kRandoOverlayPaletteKind_Gold, 0);
}

bool Rando_OverlayPaletteRequestCustomItem(uint8 row, uint8 gfx) {
  return Rando_OverlayPaletteAddRequest(row, kRandoOverlayPaletteKind_CustomItem, gfx);
}

static const uint16 *Rando_CustomItemStaticPalette(uint8 gfx) {
  static const uint16 kRandoOffBlackPalette[8] = {
    0x0000, 0x14A5, 0x14A5, 0x14A5, 0x14A5, 0x14A5, 0x14A5, 0x14A5,
  };
  if (gfx == kRandoCustomGfx_Rupoor)
    return kRandoOffBlackPalette;
  if (gfx >= kRandoCustomGfx_TriforcePiece &&
      gfx < kRandoCustomGfx_TriforcePiece + kRandoCustomGfx_BlobEntries)
    return kPalette_MainSpr + 52;
  return NULL;
}

static void Rando_OverlayPaletteApplyGold(uint16 *dst) {
  int ph = (frame_counter >> 1) & 0x3f;
  int pulse = (ph < 0x20 ? ph : 0x3f - ph) >> 2;
  for (int i = 1; i < 16; i++) {
    // Floor the ramp at bright gold: the glint chars are borrowed from each
    // area's loaded sheets, and WHICH color indices their pixels use is
    // sheet-dependent — on Death Mountain the borrowed tile drew indices
    // 1/6/7, and the old `lvl = i` ramp made those 3-25% brightness gold:
    // invisible against the mountain (playtest-found via F12 CGRAM decode —
    // the gold WAS applied, just nearly black). 16 + i/2 keeps per-index
    // shading and the pulse while making every index read as gold.
    int lvl = 16 + (i >> 1) + pulse;
    if (lvl > 31)
      lvl = 31;
    int r = lvl;
    int g = (lvl * 13) >> 4;
    int b = lvl >> 2;
    dst[i] = (uint16)((b << 10) | (g << 5) | r);
  }
}

static bool Rando_OverlayPaletteApplyCustomItem(uint16 *dst, uint8 row, uint8 gfx) {
  const uint16 *src = Rando_CustomItemStaticPalette(gfx);
  int base = 0x80 + 16 * row;
  if (src != NULL) {
    for (int i = 0; i < 8; i++)
      dst[8 + i] = Cosmetic_TransformPaletteColor(src[i], base + 8 + i);
    return true;
  }
  if (gfx == kRandoCustomGfx_Cucco) {
    for (int i = 0; i < 7; i++) {
      uint16 c = main_palette_buffer[0xC1 + i];
      dst[9 + i] = Cosmetic_TransformPaletteColor(c, base + 9 + i);
    }
    dst[8] = Cosmetic_TransformPaletteColor(0, base + 8);
    return true;
  }
  return false;
}

// External-review round 2 — reset the overlay-palette transient state on a
// snapshot load: pending requests are from the pre-load scene, and the saved
// pre-overlay rows would otherwise be "restored" INTO the freshly-loaded
// scene's CGRAM on the next apply (a 16-color row from the old scene).
void Rando_OverlayPaletteInvalidate(void) {
  s_overlay_palette_request_count = 0;
  s_overlay_palette_previous_count = 0;
}

// One-call reset of every process-local video ownership/arbitration cache a
// snapshot load must not inherit (called from StateRecorder_Load next to the
// recv-slot owner invalidation; see the umbrella comment there).
void Rando_InvalidateTransientVideoState(void) {
  Rando_OverlayPaletteInvalidate();
  Rando_ShopIconSlotsInvalidate();
}

void Rando_OverlayPaletteApplyCgram(uint16 *cgram, bool cgram_rebuilt) {
  if (cgram == NULL)
    return;
  if (!cgram_rebuilt) {
    for (uint8 i = 0; i < s_overlay_palette_previous_count; i++) {
      uint8 row = s_overlay_palette_previous[i].row;
      if (row < 8)
        memcpy(&cgram[0x80 + 16 * row], s_overlay_palette_previous[i].colors,
               sizeof(s_overlay_palette_previous[i].colors));
    }
  }
  s_overlay_palette_previous_count = 0;

  Rando_OverlayPaletteSyncFrame();
  for (uint8 i = 0; i < s_overlay_palette_request_count; i++) {
    RandoOverlayPaletteRequest *req = &s_overlay_palette_requests[i];
    if (req->row >= 7 || s_overlay_palette_previous_count >= kRandoOverlayPaletteMax)
      continue;
    uint16 *row = &cgram[0x80 + 16 * req->row];
    RandoOverlayPalettePrevious *prev =
        &s_overlay_palette_previous[s_overlay_palette_previous_count];
    prev->row = req->row;
    memcpy(prev->colors, row, sizeof(prev->colors));

    bool applied = false;
    if (req->kind == kRandoOverlayPaletteKind_Gold) {
      Rando_OverlayPaletteApplyGold(row);
      applied = true;
    } else if (req->kind == kRandoOverlayPaletteKind_CustomItem) {
      applied = Rando_OverlayPaletteApplyCustomItem(row, req->row, req->gfx);
    }
    if (applied)
      s_overlay_palette_previous_count++;
  }
  s_overlay_palette_request_count = 0;
}

int Rando_OverlayPaletteSelfCheck(void) {
  RandoOverlayPaletteRequest saved_requests[kRandoOverlayPaletteMax];
  RandoOverlayPalettePrevious saved_previous[kRandoOverlayPaletteMax];
  uint8 saved_request_count = s_overlay_palette_request_count;
  uint8 saved_request_frame = s_overlay_palette_request_frame;
  uint8 saved_previous_count = s_overlay_palette_previous_count;
  memcpy(saved_requests, s_overlay_palette_requests, sizeof(saved_requests));
  memcpy(saved_previous, s_overlay_palette_previous, sizeof(saved_previous));

  uint16 cgram[0x200];
  for (int i = 0; i < 0x200; i++)
    cgram[i] = (uint16)i;

  s_overlay_palette_request_count = 0;
  s_overlay_palette_request_frame = frame_counter;
  s_overlay_palette_previous_count = 0;

  uint16 saved_row[16];
  memcpy(saved_row, &cgram[0x80 + 2 * 16], sizeof(saved_row));
  int fail = 0;
  if (!Rando_OverlayPaletteRequestGold(2))
    fail = 1;
  Rando_OverlayPaletteApplyCgram(cgram, false);
  if (!fail && (s_overlay_palette_previous_count != 1 ||
      memcmp(&cgram[0x80 + 2 * 16], saved_row, sizeof(saved_row)) == 0))
    fail = 2;
  Rando_OverlayPaletteApplyCgram(cgram, false);
  if (!fail && memcmp(&cgram[0x80 + 2 * 16], saved_row, sizeof(saved_row)) != 0)
    fail = 3;

  memcpy(saved_row, &cgram[0x80 + 3 * 16], sizeof(saved_row));
  if (!fail && !Rando_OverlayPaletteRequestGold(3))
    fail = 4;
  Rando_OverlayPaletteApplyCgram(cgram, false);
  for (int i = 0; i < 16; i++)
    cgram[0x80 + 3 * 16 + i] = (uint16)(0x7000 + i);
  Rando_OverlayPaletteApplyCgram(cgram, true);
  if (!fail && cgram[0x80 + 3 * 16 + 1] != 0x7001)
    fail = 5;

  memcpy(s_overlay_palette_requests, saved_requests, sizeof(saved_requests));
  memcpy(s_overlay_palette_previous, saved_previous, sizeof(saved_previous));
  s_overlay_palette_request_count = saved_request_count;
  s_overlay_palette_request_frame = saved_request_frame;
  s_overlay_palette_previous_count = saved_previous_count;
  return fail;
}

// Resolver body shared by the field-item draw (client-toggle gated wrapper
// below) and the shopsanity check-slot draw (never toggle-gated — a vanilla
// shop icon over a different placed item would be a misleading lookalike).
static bool rando_placed_item_icon(uint16 location_id, uint16 vanilla_item_id,
                                   uint8 *out_gfx, uint8 *out_big,
                                   uint8 *out_oam_flags) {
  // Placement_Lookup returns vanilla_item_id when no table is active or the
  // location is absent — both mean "draw the vanilla sprite".
  uint16 placed = Placement_Lookup(location_id, vanilla_item_id);
  if (placed == vanilla_item_id)
    return false;
  RandoGrantPlan plan;
  if (!Rando_ResolveLiveGrantPlan(location_id, placed, &plan) ||
      !plan.display_valid)
    return false;
  *out_gfx = plan.display_gfx;
  *out_big = plan.display_big;
  *out_oam_flags = plan.display_oam_flags;
  return true;
}

bool Rando_GetFieldItemIcon(uint16 location_id, uint16 vanilla_item_id,
                            uint8 *out_gfx, uint8 *out_big, uint8 *out_oam_flags) {
  if (!Rando_FieldItemSpritesActive())
    return false;
  return rando_placed_item_icon(location_id, vanilla_item_id,
                                out_gfx, out_big, out_oam_flags);
}

// add-rando-shopsanity — icon resolution for an unchecked shop check slot.
// Same shared resolver as field items (draw mirrors the grant chain) but
// NOT gated on the client field_item_sprites toggle. Distinguishes the
// truthful-vanilla case from the no-icon case so the caller never shows a
// vanilla-item lookalike for a different placed item:
//   0 = placement IS the slot's vanilla item -> vanilla item tiles are truthful
//   1 = icon resolved (outputs filled)
//   2 = placed item has no drawable icon -> caller draws the generic cue
int Rando_GetShopCheckIcon(uint16 location_id, uint8 *out_gfx, uint8 *out_big,
                           uint8 *out_oam_flags) {
  // Per-frame per-column call — binary search (registry is id-sorted), not
  // the linear registry scan (external-review perf note).
  uint16 vanilla = rando_location_vanilla_item(location_id);
  uint16 placed = Placement_Lookup(location_id, vanilla);
  if (placed == vanilla) return 0;
  return rando_placed_item_icon(location_id, vanilla,
                                out_gfx, out_big, out_oam_flags) ? 1 : 2;
}

// ---------------------------------------------------------------------------
// §6.6 boss-kill dispatch helpers. Each boss kill grants TWO rando locations
// (BossHeart + Prize). Boss-heart drops are shuffled locations; the dispatch
// treats the identity BossHeartContainer placement as vanilla behavior.
//
// Dungeon ID layout (cur_palace_index_x2 >> 1) — game-side convention.
// Derived from the asset room palace ids and kDungeonCrystalPendantBit /
// kBossFinishedFallingItem (zelda_rtl.c, dungeon.c). NOTE: TH lives at 10,
// not 3 — this is not the ALTTPR id ordering.
//   0 HCE  (no boss; Sanctuary chest is the heart container slot)
//   1 (unused sub-area, no prize)
//   2 EP   3 DP
//   4 HCT  (Agahnim; not a heart-drop boss — handled separately)
//   5 SP   6 PoD  7 MM   8 SW   9 IP  10 TH  11 TT  12 TR
//  13 GT   (Agahnim 2; same as HCT path)
// ---------------------------------------------------------------------------
uint16 Rando_GetBossHeartLocation(uint8 dungeon_id) {
  return Rando_BossHeartLocationForGameDungeon(dungeon_id);
}

uint16 Rando_GetBossPrizeLocation(uint8 dungeon_id) {
  return Rando_BossPrizeLocationForGameDungeon(dungeon_id);
}

RandoGrantResult Rando_GrantBossPrizeReceipt(
    uint8 dungeon_id, uint8 vanilla_lttp_code,
    RandoGrantPresentation presentation,
    uint8 receipt_method, uint8 chest_position) {
  if (!Rando_IsActive())
    return kRandoGrantResult_NotActive;
  uint16 prize_loc = Rando_GetBossPrizeLocation(dungeon_id);
  if (prize_loc == 0xFFFFu)
    return kRandoGrantResult_NotActive;
  return Rando_GrantLocation(prize_loc, 0xFFFFu, vanilla_lttp_code,
                             presentation, receipt_method, chest_position);
}

// ---------------------------------------------------------------------------
// Per-seed shuffle-assignment globals consumed by Logic_ComputeReachabilityFullKnowledge.
// ---------------------------------------------------------------------------
static uint8 g_dungeon_prize_assignment_store[kRandoDungeonCount];
static uint8 g_medallion_assignment_store[kRandoMedallionEntranceCount];
static const uint8 *g_dungeon_prize_assignment = NULL;
static const uint8 *g_medallion_assignment = NULL;
// Boss-shuffle LOGIC assignment (OP_CAN_KILL_BOSS). Independent of the
// shuffle_boss.c sprite-substitution activation: this drives reachability only.
static uint8 g_boss_logic_assignment_store[16];
static const uint8 *g_boss_logic_assignment = NULL;

void Rando_SetDungeonPrizeAssignment(const uint8 *assignment) {
  if (assignment == NULL) {
    g_dungeon_prize_assignment = NULL;
    return;
  }
  memcpy(g_dungeon_prize_assignment_store, assignment,
         sizeof(g_dungeon_prize_assignment_store));
  g_dungeon_prize_assignment = g_dungeon_prize_assignment_store;
}
void Rando_SetMedallionAssignment(const uint8 *assignment) {
  if (assignment == NULL) {
    g_medallion_assignment = NULL;
    return;
  }
  memcpy(g_medallion_assignment_store, assignment,
         sizeof(g_medallion_assignment_store));
  g_medallion_assignment = g_medallion_assignment_store;
}
void Rando_SetBossAssignment(const uint8 *assignment) {
  if (assignment == NULL) {
    g_boss_logic_assignment = NULL;
    return;
  }
  memcpy(g_boss_logic_assignment_store, assignment,
         sizeof(g_boss_logic_assignment_store));
  g_boss_logic_assignment = g_boss_logic_assignment_store;
}
const uint8 *Rando_GetDungeonPrizeAssignment(void) { return g_dungeon_prize_assignment; }
const uint8 *Rando_GetMedallionAssignment(void) { return g_medallion_assignment; }
const uint8 *Rando_GetBossAssignment(void) { return g_boss_logic_assignment; }

// Runtime open gate for the medallion-shuffled MM/TR overworld doors. The placer
// and tracker consult g_medallion_assignment via OP_MEDALLION_OPENS; the ancilla
// spell handlers consult it here so the dungeon actually opens for whichever
// medallion the seed requires. entrance_index: 0 = Misery Mire, 1 = Turtle Rock
// (the kRandoMedallionEntranceCount ordering used by MedallionShuffle_Run and
// the logic codegen's _resolve_entrance_id). Returns false when no assignment is
// installed so the caller can keep the vanilla Ether->MM / Quake->TR mapping.
bool Rando_MedallionOpens(uint8 cast_medallion, uint8 entrance_index) {
  const uint8 *assignment = g_medallion_assignment;
  if (assignment == NULL || entrance_index >= kRandoMedallionEntranceCount)
    return false;
  return assignment[entrance_index] == cast_medallion;
}

// ---------------------------------------------------------------------------
// Session-persistent placement storage. The sidecar struct lives in the
// file-select cache (and is overwritten when the cache reloads), so we copy
// its placements[] into this static buffer before installing — that way the
// pointer Placement_Install holds stays valid for the duration of gameplay.
// ---------------------------------------------------------------------------
#define kRando_SessionPlacementCapacity kRandoLocationCapacity
static RandoPlacement g_session_placements[kRando_SessionPlacementCapacity];
static RandoPlacementTable g_session_placement_table;

// `g_wanted_zelda_features1` lives in main.c — declared here so we can
// mirror the feature-bit update into the wanted bank as well as the
// in-RAM enhanced bank (zelda_rtl.c's frame loop syncs wanted → enhanced).
extern uint32 g_wanted_zelda_features1;
// add-rando-major-glitch D6 — the features0 wanted bank (the source the
// per-frame mirror syncs into enhanced_features0). Forcing the JP-glitch bit
// here (not just enhanced_features0) survives the per-frame re-sync AND a
// mid-session Config_ApplyLive (which rewrites wanted from g_config.features0).
extern uint32 g_wanted_zelda_features;

// §62 — cache for the active slot's textual share string (50 base32 chars +
// NUL; the v1 IDENTITY string — the slot stores the v1 raw blob, design D1 of
// add-rando-share-string-v2). Populated at Rando_ActivateSidecarSlot from the
// slot header's raw binary; consumed by Rando_RevealActiveSlotSpoiler to
// resolve the on-disk spoiler path. Cleared on Rando_DeactivateSlot.
static char g_rando_active_share_string[kShareStringBase32MaxLen] = {0};

// ---------------------------------------------------------------------------
// Phase C entrance shuffle — runtime door overlay. Owns a shadow of the vanilla
// door→entrance-id table (kOverworld_Entrance_Id = g_asset_ptrs[126]) and
// repoints the asset pointer while a cave-shuffle slot is active. The
// permutation is regenerated from (seed, axes, attempt) carried in the slot
// header, so save/quit + reload restores the same π. Coupling (enter A ⇒ exit A)
// is automatic for caves: Dungeon_LoadEntrance caches the SOURCE overworld
// position at entry, before the interior loads from which_entrance.
// ---------------------------------------------------------------------------
#define kEntranceOverlayMax 4096
static uint8 g_entrance_overlay[kEntranceOverlayMax];
static uint8 g_entrance_discovered[(kEntranceOverlayMax + 7) / 8];
// Saved g_asset_ptrs[126] (the vanilla door table) while the overlay is
// installed. INVARIANT: Entrance_RuntimeTeardown() MUST run before any
// LoadAssets() reload — LoadAssets unconditionally rewrites every g_asset_ptrs[i]
// into a fresh buffer, which would both drop the overlay and leave this pointer
// dangling into the freed asset buffer. Today all LoadAssets call sites are
// startup/CLI-only (never mid-session), so this is latent; a future hot-reload
// feature must tear the overlay down first.
static const uint8 *g_entrance_overlay_orig = NULL;

// ---------------------------------------------------------------------------
// Decoupled (Insanity) runtime — D.4. Productionizes the validated arrival
// capture-and-replay (insanity-arrival spike): each cave
// interior's overworld-arrival block (g_ram[0xC140..0xC172) + the two world
// flags) is captured the first time its door is entered; on a decoupled cave
// EXIT the player emerges at net[entered_interior]'s door by replaying THAT
// interior's captured arrival, then letting LoadCachedEntranceProperties run.
// `net` (entered-interior → emerge-interior) is the deterministic decoupled
// permutation regenerated at slot-load. Until a target is captured this session,
// the exit falls back to the normal coupled return (never strands the player) —
// the static-table bake (D.3) removes that gap for never-visited doors.
// ---------------------------------------------------------------------------
static uint8 g_decoupled_active;
static int   g_decoupled_n;                       // pool size (cave interiors)
static uint8 g_decoupled_net[kEntranceMaxInteriors];   // entered-interior → emerge
static uint16 g_decoupled_entered;                // vanilla interior of entered door; 0xFFFF = none
// Dungeon decoupled (Insanity for dungeons): one-way dungeon EXITS. net'[D] = the
// dungeon-pool index of the door Link emerges at after exiting loaded dungeon D.
// No arrival table — the runtime just retargets the room-keyed exit search.
static uint8 g_dungeon_decoupled_active;
static int   g_dungeon_decoupled_n;
static uint8 g_dungeon_decoupled_net[kEntranceDungeonCount];
// Cross + decoupled: one-way exits over the MIXED cross pool. net[e] = the endpoint
// Link emerges at after exiting door e (a cave OR a dungeon). The exit kind is
// resolved at the entry hook and consumed at LoadOverworldFromDungeon.
static uint8 g_cross_decoupled_active;
static int   g_cross_decoupled_n;
static uint8 g_cross_net[kEntranceMaxInteriors];
static uint8 g_cross_decoupled_exit_kind;  // 0 none, 1 cave, 2 dungeon (consumed at exit)
static uint8 g_cross_decoupled_exit_cave;  // target cave interior when kind == 1
// The ENTRY permutation actually installed this session, indexed by cave-pool
// entry (or by combined endpoint under Crossed). Kept because the door table on
// its own can no longer answer "did this door move": several pool entries share
// an entrance id — the four Dark World shop doors all load room 0x10F through
// 0x60 — so an unchanged overlay byte does not imply an unchanged destination.
// g_entry_net_n == 0 means no entry axis is installed. g_entry_net_cross says
// which endpoint space it indexes: under Crossed the permutation runs over the
// COMBINED cave+dungeon endpoints, so the source lookup differs (dungeon doors
// resolve by their unique entrance id, caves by row) and a cave entry's image
// may be a dungeon.
static uint8 g_entry_net[kEntranceMaxInteriors];
static int   g_entry_net_n;
static bool  g_entry_net_cross;

// The installed entry permutation, for the door row -> source -> assign ->
// destination chain (shop identity, destination-side discovery). NULL / *out_n
// 0 = nothing installed, which callers treat as the identity assignment.
static const uint8 *Entrance_InstalledEntryNet(int *out_n, bool *out_cross) {
  if (out_n) *out_n = g_entry_net_n;
  if (out_cross) *out_cross = g_entry_net_cross;
  return g_entry_net_n > 0 ? g_entry_net : NULL;
}

// See the forward declaration: lets a consumer distinguish "identity because
// nothing is shuffled" from "identity because the layout is MISSING".
// Settings_EffectiveShuffleCaveEntrances (not the raw bit) so the world-state
// normalization that makes the axis inert in Inverted/Retro is honored; Crossed
// sets the cave bit too, so it is covered.
static bool Entrance_EntryNetExpected(void) {
  const RandoSettings *s = Rando_GetActiveSettings();
  return s != NULL && Settings_EffectiveShuffleCaveEntrances(s);
}

// The door's PRISTINE vanilla entrance id: the saved original while an overlay
// is installed, otherwise the live table (vanilla / non-entrance game).
static uint8 Entrance_VanillaIdOfDoorRow(uint16 lx) {
  if (lx >= kOverworld_Entrance_Id_SIZE) return 0;
  return (g_entrance_overlay_orig != NULL)
             ? g_entrance_overlay_orig[lx]
             : ((const uint8 *)kOverworld_Entrance_Id)[lx];
}
typedef struct { uint8 block[0x32]; uint8 is_dark; uint8 save_dark; uint8 valid; } RandoCaveArrival;
// Single per-interior overworld-arrival table used by BOTH the bake-capture and
// the decoupled runtime. Persisted to cave_arrival_capture.bin and reloaded each
// session, so captures accumulate across restarts and the runtime emerges at any
// door captured this session OR a prior one (option B: just play, it fills in).
static RandoCaveArrival g_cave_capture[kEntranceMaxInteriors];
static int g_cave_capture_count;
static bool g_cave_capture_loaded;  // initialized the table this session yet?
static void Rando_LoadArrivalCaptureIfNeeded(void);
// Committed baked arrival table (the D.3 walkabout result) — the default source
// so every door is one-way from the start, no on-disk capture needed.
#include "cave_arrival_baked.h"
// Baked-table length. The table is keyed by interior_id, so it is neither
// index-aligned to nor bounded by the runtime cave count — a row whose name no
// longer exists is reported and skipped, and an interior with no row keeps
// valid == 0 (the runtime degrades that to the coupled exit).
#define kCaveArrivalBakedCount ((int)(sizeof(kCaveArrivalBaked) / sizeof(kCaveArrivalBaked[0])))

// D.3 capture: vanilla cave interior of the door just entered, recorded at the
// overworld entry hook for EVERY game (shuffle or not) so the capture-for-bake
// works regardless of mode. 0xFFFF = the entered door isn't a cave.
static uint16 g_rando_entered_door_interior = 0xFFFF;
static void Rando_EntranceMarkDiscovered(uint16 lx) {
  if (lx < kEntranceOverlayMax)
    g_entrance_discovered[lx >> 3] |= (uint8)(1u << (lx & 7));
}

void Rando_RecordEnteredDoorForCapture(uint16 lx) {
  g_rando_entered_door_interior = 0xFFFF;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (lx >= len) return;
  // The door's VANILLA entrance-id: the saved original when an overlay is
  // installed (shuffle), otherwise the live table (vanilla/non-entrance game).
  // Needed below to resolve a DUNGEON source door under Crossed.
  uint8 vid = Entrance_VanillaIdOfDoorRow(lx);
  // The player physically walked through this door, so where it leads is now
  // known to them — mark it discovered. Deliberately NOT gated on the overlay
  // byte having changed: entries sharing an entrance id make an unchanged byte
  // compatible with a moved destination. The "did it actually move" test lives
  // in Rando_EntranceConnection, where the entry-level answer is available.
  if (g_entrance_overlay_orig != NULL) Rando_EntranceMarkDiscovered(lx);
  // tracker-player-knowledge — the player just SAW the interior the door
  // actually loads: persistently mark that pool entry discovered so its
  // contents join the knowledge-limited view. A dungeon behind this door
  // resolves to -1 here; the per-frame dungeon observation covers it instead.
  //
  // Resolved through the live ASSIGNMENT, not the loaded entrance id. The id is
  // ambiguous among room-siblings (the four room-0x10F shop entries all load
  // through 0x60), and it stopped being inert the moment those entries got
  // their own shop slots: naming the wrong sibling would now reveal a shop's
  // three checks the player has not actually found. The chain the shop resolver
  // uses is the same one that answers "which entry did this door load".
  {
    int net_n = 0;
    bool net_cross = false;
    const uint8 *net = Entrance_InstalledEntryNet(&net_n, &net_cross);
    Rando_MarkCaveInteriorDiscovered(
        Entrance_DestCaveOfDoorRow(net, net_n, net_cross, lx, vid));
  }
  // SOURCE side: which door the player walked into. Keyed by row, so the four
  // room-0x10F doors record four distinct interiors (their arrival positions,
  // regions and shuffle slots all differ).
  int interior = Entrance_InteriorOfDoorRow(lx);
  if (interior >= 0 && interior < kEntranceMaxInteriors)
    g_rando_entered_door_interior = (uint16)interior;
}

uint32 Rando_EntranceConnectionCount(void) {
  if (g_entrance_overlay_orig == NULL) return 0;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  return len <= kEntranceOverlayMax ? len : kEntranceOverlayMax;
}

bool Rando_EntranceConnection(uint16 lx, uint8 *from_id, uint8 *to_id) {
  if (g_entrance_overlay_orig == NULL) return false;
  uint32 len = Rando_EntranceConnectionCount();
  if (lx >= len) return false;
  if ((g_entrance_discovered[lx >> 3] & (uint8)(1u << (lx & 7))) == 0)
    return false;
  uint8 from = g_entrance_overlay_orig[lx];
  uint8 to = ((const uint8 *)kOverworld_Entrance_Id)[lx];
  // "Unshuffled ⇒ report nothing" must be decided on POOL ENTRIES, not entrance
  // ids. Four pool entries load room 0x10F through id 0x60 and two load 0x112
  // through 0x58, so a permutation mapping one of those entries onto a sibling
  // leaves the overlay byte identical while genuinely moving the door — the id
  // compare alone would report it as unshuffled forever.
  int src = Entrance_InteriorOfDoorRow(lx);
  if (src >= 0 && g_entry_net_n > 0 && src < g_entry_net_n) {
    if (g_entry_net[src] == (uint8)src) return false;   // true self-map
  } else if (to == from) {
    // Not a cave door (dungeon ids are unique, so the id compare is exact), or
    // no entry axis installed — fall back to the id compare.
    return false;
  }
  if (from_id) *from_id = from;
  if (to_id) *to_id = to;
  return true;
}

// Genuine fall-hole caves (fall-hole table entrance-ids 0x76-0x81) have no
// walk-in door slot — the fall sets which_entrance directly (the fall-hole table
// isn't shuffled), so key on it. Lets the capture tool record a fall-in arrival
// and the decoupled exit emerge elsewhere. Non-cave fall-hole ids resolve to
// interior -1 below and are ignored. (heart_piece_cave_3 is NOT here — it is a
// normal walk-in cave handled by the door-slot entry hook.)
void Rando_RecordEnteredFallhole(void) {
  g_rando_entered_door_interior = 0xFFFF;
  int interior = Entrance_InteriorOfEntranceId(which_entrance);
  if (interior < 0 || interior >= kEntranceMaxInteriors) return;
  // tracker-player-knowledge — the fall loaded this interior; the player has
  // now seen it (fall-hole targets aren't shuffled, so this is vanilla
  // knowledge — marking is harmless and keeps the state uniform).
  Rando_MarkCaveInteriorDiscovered(interior);
  g_rando_entered_door_interior = (uint16)interior;
  if (g_decoupled_active && interior < g_decoupled_n)
    g_decoupled_entered = (uint16)interior;
}

static void Dungeon_Decoupled_Reset(void) {
  g_dungeon_decoupled_active = 0;
  g_dungeon_decoupled_n = 0;
}

static void Cross_Decoupled_Reset(void) {
  g_cross_decoupled_active = 0;
  g_cross_decoupled_n = 0;
  g_cross_decoupled_exit_kind = 0;
  g_cross_decoupled_exit_cave = 0xFF;
}

// Entry hook: under cross-decoupled, resolve the source door's one-way exit target
// (cave or dungeon) and stash it for the exit. For a dungeon target this sets the
// room-keyed exit search target; a cave target is replayed at the cached branch.
void Rando_CrossDecoupledSetExit(uint16 lx) {
  g_cross_decoupled_exit_kind = 0;
  g_cross_decoupled_exit_cave = 0xFF;
  if (!g_cross_decoupled_active || g_entrance_overlay_orig == NULL) return;
  if (lx >= kOverworld_Entrance_Id_SIZE) return;
  uint16 value = 0;
  int kind = Entrance_CrossDecoupledExit(g_cross_net, g_cross_decoupled_n, lx,
                                         g_entrance_overlay_orig[lx], &value);
  // When active, cross-decoupled is AUTHORITATIVE over the coupled exit the prior
  // hook set: a cave target forces the cached branch (kind 1, replay), a dungeon
  // target forces the search branch at its room (kind 2). Self-map / non-pool
  // (kind 0) leaves the coupled cross return-to-source in place.
  if (kind == 1) {
    // Only force the cached branch if the target cave's arrival is actually
    // CAPTURED — otherwise fall back to the coupled return (kind 0) rather than
    // forcing a cached replay against a stale *_exit block (the loaded interior may
    // be a dungeon). The production baked table makes every cave valid, so this
    // never falls back in shipped play; it's a dev/incomplete-table footgun guard.
    Rando_LoadArrivalCaptureIfNeeded();
    if (value < kEntranceMaxInteriors && g_cave_capture[value].valid) {
      g_cross_decoupled_exit_kind = 1;
      g_cross_decoupled_exit_cave = (uint8)value;
      g_rando_entrance_exit_room = 0;
      g_rando_entrance_force_cached = 0;
    }
  } else if (kind == 2) {
    g_cross_decoupled_exit_kind = 2;
    g_rando_entrance_exit_room = value;
    g_rando_entrance_force_cached = 0;
  }
}

// Consume-once at LoadOverworldFromDungeon top: returns the exit kind (0 none,
// 1 cave, 2 dungeon) and writes the target cave interior to *out_cave, clearing
// both. Same consume-at-top discipline as the sibling exit-coupling globals.
uint8 Rando_CrossDecoupledConsumeExit(uint8 *out_cave) {
  uint8 k = g_cross_decoupled_exit_kind;
  if (out_cave) *out_cave = g_cross_decoupled_exit_cave;
  g_cross_decoupled_exit_kind = 0;
  g_cross_decoupled_exit_cave = 0xFF;
  return k;
}

static void Decoupled_Reset(void) {
  g_decoupled_active = 0;
  g_decoupled_n = 0;
  g_decoupled_entered = 0xFFFF;
  // NOTE: do NOT clear g_cave_capture here — it's the persisted arrival table and
  // must survive slot teardown / reload (that's what makes captures accumulate).
}

// Entry hook: remember the vanilla cave interior of the door slot just entered
// (its overworld position is what we may capture / emerge at).
void Rando_DecoupledSetEnteredDoor(uint16 lx) {
  g_decoupled_entered = 0xFFFF;
  if (!g_decoupled_active || g_entrance_overlay_orig == NULL) return;
  if (lx >= kOverworld_Entrance_Id_SIZE) return;
  // Keyed by door ROW: the decoupled exit is a property of the door the player
  // entered, and the four room-0x10F doors are four distinct pool entries with
  // four distinct exits despite sharing entrance id 0x60.
  int interior = Entrance_InteriorOfDoorRow(lx);
  if (interior >= 0 && interior < g_decoupled_n) g_decoupled_entered = (uint16)interior;
}

// (Capture is handled by Rando_CaptureArrivalForBake on every cave entry — it
// fills + persists the same g_cave_capture table the runtime reads below.)

// Consume-once accessor for the entered-interior global. LoadOverworldFromDungeon
// reads+clears it at the top UNCONDITIONALLY (same discipline as g_rando_entrance_
// exit_room / force_cached): this function is reached by mirror /
// special-area / ending warps too, so a stale entered-interior left set could be
// consumed by a LATER cave-class exit → wrong-door warp. 0xFFFF = none.
uint16 Rando_DecoupledConsumeEntered(void) {
  uint16 v = g_decoupled_entered;
  g_decoupled_entered = 0xFFFF;
  return v;
}

// Exit hook (cave-class cached branch, before LoadCachedEntranceProperties):
// replace the live *_exit with net[entered]'s captured arrival so Link emerges
// at a DIFFERENT door. `entered` is the consumed entered-interior. Returns false
// (→ coupled return) when inactive, the target is the same interior, or the
// target hasn't been captured yet.
// Replay cave interior `target_cave`'s captured overworld arrival into the live
// *_exit block + world flags + target room/door-settings (the Y-adjust). Shared by
// the cave-decoupled and cross-decoupled exit paths. False → not captured (coupled
// fallback) or out of range.
static bool Rando_ReplayCaveArrival(int target_cave) {
  if (target_cave < 0 || target_cave >= kEntranceMaxInteriors) return false;
  Rando_LoadArrivalCaptureIfNeeded();  // pull in prior-session captures (persisted)
  const RandoCaveArrival *e = &g_cave_capture[target_cave];
  if (!e->valid) return false;  // target door not captured yet → coupled fallback
  memcpy(g_ram + 0xC140, e->block, 0x32);
  g_ram[0xFFF] = e->is_dark;      // is_in_dark_world
  g_ram[0xF3CA] = e->save_dark;   // savegame_is_darkworld
  // Target door's room + door-settings drive the cached-exit Y-adjust (spike iter4).
  uint8 rid = Entrance_CaveRepresentativeId(target_cave);
  if ((uint32)rid < kEntranceData_rooms_SIZE / 2u) dungeon_room_index = kEntranceData_rooms[rid];
  if ((uint32)rid < kEntranceData_doorSettings_SIZE / 2u) ow_entrance_value = kEntranceData_doorSettings[rid];
  return true;
}

// Exit hook (cave-class cached branch, before LoadCachedEntranceProperties):
// replace the live *_exit with net[entered]'s captured arrival so Link emerges
// at a DIFFERENT door. `entered` is the consumed entered-interior. Returns false
// (→ coupled return) when inactive, the target is the same interior, or the
// target hasn't been captured yet.
bool Rando_DecoupledReplaceArrival(uint16 entered) {
  if (!g_decoupled_active || entered >= (uint16)g_decoupled_n) return false;
  int target = g_decoupled_net[entered];
  if (target < 0 || target >= g_decoupled_n || target == (int)entered) return false;
  if (!Rando_ReplayCaveArrival(target)) return false;
  // tracker-player-knowledge — the player just traversed net[entered]: the
  // matching decoupled exit logic edge is now known (the coupled fallback
  // above never traverses the net, so it never marks).
  Rando_MarkDecoupledExitDiscovered((int)entered);
  return true;
}

// Cross-decoupled exit: replay the target CAVE's arrival (target resolved at the
// entry hook). Returns false (→ coupled return) when not captured.
bool Rando_CrossDecoupledReplayCave(uint8 target_cave) {
  return Rando_ReplayCaveArrival((int)target_cave);
}

// ---------------------------------------------------------------------------
// D.3 cave-arrival table. Production: the committed kCaveArrivalBaked is loaded
// once and used by the decoupled runtime (every door one-way from launch). The
// re-capture/dump tooling below (used to PRODUCE the baked table) is a developer
// opt-in — set the env var ZELDA3_CAPTURE_ARRIVALS=1 to record fresh arrivals
// into cave_arrival_capture.{bin,txt} for re-baking. Off by default, so normal
// play writes no files and prints nothing.
// ---------------------------------------------------------------------------
// Stamp for the positional cave_arrival_capture.bin dev sidecar: magic, pool
// size, and a hash over the interior NAMES in order. The names change whenever
// the pool is split, reordered or renamed, so a mismatch means the file's
// positions no longer mean what they did when it was written.
#define kCaveCaptureMagic 0x33564143u  /* "CAV3" */
static uint32 Rando_CaveInteriorNameHash(void) {
  uint32 h = 2166136261u;
  int n = Entrance_CaveInteriorCount();
  for (int i = 0; i < n; i++) {
    for (const char *p = Entrance_CaveInteriorName(i); *p; p++)
      h = (h ^ (uint8)*p) * 16777619u;
    h = (h ^ 0xFFu) * 16777619u;   // separator, so "ab"+"c" != "a"+"bc"
  }
  return h;
}

static bool Rando_ArrivalCaptureEnabled(void) {
  static int cached = -1;  // -1 unknown, 0 off, 1 on
  if (cached < 0) {
    const char *v = getenv("ZELDA3_CAPTURE_ARRIVALS");
    cached = (v != NULL && v[0] != '\0' && v[0] != '0') ? 1 : 0;
  }
  return cached != 0;
}

static void Rando_LoadArrivalCaptureIfNeeded(void) {
  if (g_cave_capture_loaded) return;
  g_cave_capture_loaded = true;
  int n = Entrance_CaveInteriorCount();
  // Committed baked table — the production source. Matched BY NAME, never by
  // position: the pool is append-and-split, so a positional load silently shifts
  // rows onto the wrong door (it did, for eight interiors, until this was fixed).
  // A name that no longer resolves is a data bug, so say so rather than absorb it.
  for (int b = 0; b < kCaveArrivalBakedCount; b++) {
    const char *want = kCaveArrivalBaked[b].interior_id;
    int target = -1;
    for (int i = 0; i < n && i < kEntranceMaxInteriors; i++) {
      if (strcmp(Entrance_CaveInteriorName(i), want) == 0) { target = i; break; }
    }
    if (target < 0) {
      fprintf(stderr, "Rando WARNING: baked cave arrival names unknown interior '%s' "
                      "— that door will fall back to the coupled exit.\n", want);
      continue;
    }
    g_cave_capture[target] = kCaveArrivalBaked[b].arrival;
  }
  g_cave_capture_count = 0;
  for (int i = 0; i < n && i < kEntranceMaxInteriors; i++)
    if (g_cave_capture[i].valid) g_cave_capture_count++;
  if (!Rando_ArrivalCaptureEnabled()) return;
  // Dev re-capture overlay: a live capture file overrides matching entries so
  // re-captures take effect without rebaking (valid-only, so a partial file
  // never erases baked data).
  //
  // This sidecar is still POSITIONAL, which is the drift the committed table was
  // just moved off. Its length does not change when the pool grows, so a file
  // written before a pool split would land its rows on the wrong interiors,
  // silently override the correct name-keyed ones, and then be re-emitted under
  // the NEW names by Rando_DumpArrivalCapture — baking the corruption in during
  // exactly the re-capture walkabout that is supposed to fix things. So stamp it
  // with the pool shape and refuse anything that does not match.
  FILE *f = fopen("cave_arrival_capture.bin", "rb");
  if (f != NULL) {
    static RandoCaveArrival tmp[kEntranceMaxInteriors];
    uint32 hdr[3] = {0, 0, 0};
    if (fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == kCaveCaptureMagic &&
        hdr[1] == (uint32)n && hdr[2] == Rando_CaveInteriorNameHash() &&
        fread(tmp, sizeof(tmp), 1, f) == 1) {
      for (int i = 0; i < n && i < kEntranceMaxInteriors; i++)
        if (tmp[i].valid) g_cave_capture[i] = tmp[i];
    } else {
      fprintf(stderr, "[ARRIVAL-CAPTURE] ignoring cave_arrival_capture.bin — it was "
                      "written against a different cave pool (it is positional). "
                      "Delete it and re-walk; the baked table is used instead.\n");
    }
    fclose(f);
    g_cave_capture_count = 0;
    for (int i = 0; i < n && i < kEntranceMaxInteriors; i++)
      if (g_cave_capture[i].valid) g_cave_capture_count++;
  }
  fprintf(stderr, "[ARRIVAL-CAPTURE] capture mode ON — %d/%d doors\n",
          g_cave_capture_count, n);
}

static void Rando_DumpArrivalCapture(void) {
  // Binary sidecar first (the source of truth for resume across restarts).
  // Stamped with the pool shape — see the loader for why a bare positional dump
  // is a trap once the pool can grow.
  FILE *b = fopen("cave_arrival_capture.bin", "wb");
  if (b != NULL) {
    uint32 hdr[3] = { kCaveCaptureMagic, (uint32)Entrance_CaveInteriorCount(),
                      Rando_CaveInteriorNameHash() };
    fwrite(hdr, sizeof(hdr), 1, b);
    fwrite(g_cave_capture, sizeof(g_cave_capture), 1, b);
    fclose(b);
  }
  FILE *f = fopen("cave_arrival_capture.txt", "wb");
  if (f == NULL) { fprintf(stderr, "[ARRIVAL-CAPTURE] dump fopen failed\n"); return; }
  int n = Entrance_CaveInteriorCount();
  fprintf(f, "// Generated by the D.3 arrival capture walkabout — paste the rows into\n"
             "// src/rando/cave_arrival_baked.h. Rows are keyed by interior_id, so they\n"
             "// may be pasted in any order and survive pool splits; uncaptured doors\n"
             "// are omitted (the runtime degrades those to the coupled exit).\n");
  fprintf(f, "static const RandoCaveArrivalBaked kCaveArrivalBaked[] = {\n");
  for (int i = 0; i < n && i < kEntranceMaxInteriors; i++) {
    const RandoCaveArrival *e = &g_cave_capture[i];
    if (!e->valid) continue;
    fprintf(f, "  { \"%s\",\n   {{", Entrance_CaveInteriorName(i));
    for (int b = 0; b < 0x32; b++) fprintf(f, "0x%02X,", e->block[b]);
    fprintf(f, "}, 0x%02X, 0x%02X, %u} },\n", e->is_dark, e->save_dark, e->valid);
  }
  fprintf(f, "};\n");
  fclose(f);
  fprintf(stderr, "[ARRIVAL-CAPTURE] wrote cave_arrival_capture.txt (%d/%d)\n",
          g_cave_capture_count, n);
}

void Rando_CaptureArrivalForBake(void) {
  if (!Rando_ArrivalCaptureEnabled()) return;  // dev opt-in only; silent otherwise
  Rando_LoadArrivalCaptureIfNeeded();  // resume prior session's captures (restart-safe)
  // Key by the entered door's VANILLA interior (recorded at the entry hook), which
  // is correct in ANY mode: the cached *_exit is always the entered DOOR's
  // overworld arrival, even when an overlay redirects what loads behind it.
  int interior = g_rando_entered_door_interior;
  if (interior < 0 || interior >= kEntranceMaxInteriors) return;
  uint16 lx = (uint16)(g_ram[0xC14A] | (g_ram[0xC14B] << 8));  // link_x_coord_exit
  uint16 ly = (uint16)(g_ram[0xC148] | (g_ram[0xC149] << 8));  // link_y_coord_exit
  if (lx < 0x100 && ly < 0x100) return;  // degenerate startup
  RandoCaveArrival *e = &g_cave_capture[interior];
  bool was_new = !e->valid;
  memcpy(e->block, g_ram + 0xC140, 0x32);
  e->is_dark = g_ram[0xFFF];
  e->save_dark = g_ram[0xF3CA];
  e->valid = 1;
  int total = Entrance_CaveInteriorCount();
  if (was_new) {
    g_cave_capture_count++;
    fprintf(stderr, "[ARRIVAL-CAPTURE] %d/%d — interior %d %s\n",
            g_cave_capture_count, total, interior, Entrance_CaveInteriorName(interior));
  } else {
    fprintf(stderr, "[ARRIVAL-CAPTURE] re-captured interior %d %s\n",
            interior, Entrance_CaveInteriorName(interior));
  }
  // Persist on EVERY capture (new OR re-capture). A re-walk to correct a bad
  // earlier capture must reach disk, not just RAM — otherwise the stale row
  // survives and the fix is lost on exit. A partial walkabout stays recoverable.
  Rando_DumpArrivalCapture();
  if (was_new && g_cave_capture_count >= total)
    fprintf(stderr, "[ARRIVAL-CAPTURE] COMPLETE — all %d captured\n", total);
}

// Restore the vanilla door table + clear the logic overrides. Idempotent.
static void Entrance_RuntimeTeardown(void) {
  if (g_entrance_overlay_orig != NULL) {
    g_asset_ptrs[126] = (void *)g_entrance_overlay_orig;
    g_entrance_overlay_orig = NULL;
  }
  memset(g_entrance_discovered, 0, sizeof(g_entrance_discovered));
  g_entry_net_n = 0;
  g_entry_net_cross = false;
  Entrance_ClearRegionOverrides();
  Entrance_ClearEdgeOverrides();
  g_rando_entrance_exit_room = 0;
  g_rando_entrance_force_cached = 0;
  g_rando_force_cached_room = 0;  // reset the companion room with the flag
  g_rando_inverted_spawn_redirect = 0;
  Decoupled_Reset();
  Dungeon_Decoupled_Reset();
  Cross_Decoupled_Reset();
}

// Install the overlay + logic overrides for an entrance-shuffle slot (caves
// and/or dungeons). Tears down any prior install first (so a slot-switch without
// an intervening Deactivate is safe). No-op for non-shuffle slots.
// seed_u64 lives at raw share_string bytes [21..28] LE (rando_share layout).
// Single decoder shared by the entrance regen, the door-shuffle regen, and
// the cosmetic seed — a layout move must change exactly one place.
static uint64 SlotSeedFromShareString(const uint8 *sb) {
  return (uint64)sb[21] | ((uint64)sb[22] << 8) | ((uint64)sb[23] << 16) |
         ((uint64)sb[24] << 24) | ((uint64)sb[25] << 32) |
         ((uint64)sb[26] << 40) | ((uint64)sb[27] << 48) | ((uint64)sb[28] << 56);
}

// Everything Entrance_RuntimeInstall installs for one slot, computed PURELY
// (no global writes): the per-mode permutations, the decoupled exit nets, and
// the built door overlay. Shared by the installer below and
// Rando_EntranceLayoutDigest24 (FIX #4) so the activation-time digest can
// never drift from what actually installs — any algorithm/pool change that
// alters an installed table alters the digest.
typedef struct EntranceRuntimeLayout {
  bool cross, cave, dun;                         // entry-shuffle modes
  bool decoupled, decoupled_dun, decoupled_cross;// one-way exit modes
  int cave_n, dun_n, cross_n, dec_n, dd_n, cd_n;
  uint8 cave_assign[kEntranceMaxInteriors];
  uint8 dun_assign[kEntranceMaxInteriors];
  uint8 cross_assign[kEntranceMaxInteriors];
  uint8 dec_assign[kEntranceMaxInteriors];
  uint8 dd_assign[kEntranceMaxInteriors];
  uint8 cd_assign[kEntranceMaxInteriors];
  uint32 overlay_len;
  uint8 overlay[kEntranceOverlayMax];
} EntranceRuntimeLayout;

// Scratch shared by Entrance_ComputeLayout's two callers (install + digest).
// File-static rather than stack — the overlay member is kEntranceOverlayMax
// bytes. Both callers run sequentially on the main thread.
static EntranceRuntimeLayout s_entrance_layout_scratch;

// The TRUE pristine vanilla entrance-id table, regardless of which subsystem
// currently owns g_asset_ptrs[126]. Two subsystems repoint 126 and
// each saves the pointer it displaced: the Inverted static override
// (inverted_entrances.c — active for the whole life of an Inverted slot) and
// the entrance-shuffle overlay (g_entrance_overlay_orig). Resolution prefers
// the deepest saved original: Inverted's saved pointer is captured after
// Entrance_RuntimeTeardown (activation order), so it is always the raw vanilla
// table; the overlay's saved pointer is likewise pristine because activation
// tears the Inverted override down before Entrance_RuntimeInstall captures.
// Reading the live table here instead used to bake an INVERTED-id digest into
// sidecars generated while an Inverted slot was active (permanent false
// refusal after restart) and false-refuse a clean entrance slot activated over
// a still-installed Inverted slot.
static const uint8 *Entrance_PristineVanillaIds(void) {
  const uint8 *inv_orig = InvertedEntrances_SavedEntranceIdOrig();
  if (inv_orig != NULL) return inv_orig;
  if (g_entrance_overlay_orig != NULL) return g_entrance_overlay_orig;
  return (const uint8 *)kOverworld_Entrance_Id;
}

// Compute the full entrance layout that `h`'s (entrance_axes, world_state,
// share-string seed, entrance_attempt) regenerate. Returns false when no
// entrance mode is active or the vanilla door table is unavailable. Reads the
// PRISTINE vanilla door table (Entrance_PristineVanillaIds) even while a
// slot's overlay — or an Inverted slot's static override — currently owns
// asset 126, so the generation-time digest, a slot-switch digest, and the
// installed overlay all see the same base.
static bool Entrance_ComputeLayout(const RandoSlotHeader *h, EntranceRuntimeLayout *out) {
  memset(out, 0, sizeof(*out));
  RandoSettings es;
  memset(&es, 0, sizeof(es));
  es.shuffle_cave_entrances = (h->entrance_axes & kEntranceAxis_ShuffleCaves) ? 1 : 0;
  es.shuffle_dungeon_entrances = (h->entrance_axes & kEntranceAxis_ShuffleDungeons) ? 1 : 0;
  es.shuffle_ganons_tower_entrance = (h->entrance_axes & kEntranceAxis_ShuffleGanonsTower) ? 1 : 0;
  es.cross_category = (h->entrance_axes & kEntranceAxis_CrossCategory) ? 1 : 0;
  es.decoupled = (h->entrance_axes & kEntranceAxis_Decoupled) ? 1 : 0;
  es.world_state = h->settings_ext_present ? h->world_state : (uint8)kWorldState_Open;
  out->cross = Entrance_IsCrossActive(&es);    // supersedes the separate paths
  out->cave = !out->cross && Entrance_IsActive(&es);// Inverted/Retro guard (defense in depth)
  out->dun = !out->cross && Entrance_IsDungeonActive(&es);
  // Decoupled (D.4) composes on top of the entry shuffle: cave decoupled needs the
  // cave shuffle, dungeon decoupled needs the dungeon shuffle.
  out->decoupled = Entrance_IsDecoupledActive(&es);
  out->decoupled_dun = Entrance_IsDungeonDecoupledActive(&es);
  out->decoupled_cross = Entrance_IsCrossDecoupledActive(&es);  // one-way over the mixed pool
  if (!out->cross && !out->cave && !out->dun &&
      !out->decoupled && !out->decoupled_dun && !out->decoupled_cross)
    return false;
  const uint8 *ids = Entrance_PristineVanillaIds();
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (ids == NULL || len == 0 || len > kEntranceOverlayMax) return false;
  out->overlay_len = len;
  // seed_u64 lives at raw share_string bytes [21..28] LE (per rando_share layout).
  uint64 seed = SlotSeedFromShareString(h->share_string);
  if (out->cross) {
    // Crossed: one combined pool; the unified overlay maps every door (cave or
    // dungeon) to its target's id.
    out->cross_n = Entrance_ComputeCrossPermutation(&es, seed, h->entrance_attempt, out->cross_assign);
    Entrance_BuildCrossOverlay(out->cross_assign, out->cross_n, ids, len, out->overlay);
  } else {
    if (out->cave)
      out->cave_n = Entrance_ComputePermutation(&es, seed, h->entrance_attempt, out->cave_assign);
    if (out->dun)
      out->dun_n = Entrance_ComputeDungeonPermutation(&es, seed, h->entrance_attempt, out->dun_assign);
    // Door overlay: cave pass copies vanilla + remaps cave slots (NULL = copy
    // only), then the dungeon pass remaps the disjoint dungeon slots in place.
    Entrance_BuildDoorOverlay(out->cave ? out->cave_assign : NULL, out->cave_n, ids, len, out->overlay);
    if (out->dun) Entrance_RemapDungeonDoors(out->dun_assign, out->dun_n, out->overlay, len);
  }
  // One-way exit permutations (regenerated deterministically; distinct salts).
  if (out->decoupled_cross) {
    int n = Entrance_ComputeCrossDecoupledExit(&es, seed, h->entrance_attempt, out->cd_assign);
    out->cd_n = (n > kEntranceMaxInteriors) ? kEntranceMaxInteriors : n;
  }
  if (out->decoupled) {
    int n = Entrance_ComputeDecoupledExit(&es, seed, h->entrance_attempt, out->dec_assign);
    out->dec_n = (n > kEntranceMaxInteriors) ? kEntranceMaxInteriors : n;
  }
  if (out->decoupled_dun) {
    int n = Entrance_ComputeDungeonDecoupledExit(&es, seed, h->entrance_attempt, out->dd_assign);
    out->dd_n = (n > kEntranceDungeonCount) ? kEntranceDungeonCount : n;
  }
  return true;
}

// FIX #4 — see rando_save.h. 24-bit FNV-1a fold (same recipe as
// DoorShuffle_LayoutDigest) over EVERYTHING Entrance_ComputeLayout produces:
// mode flags, every active permutation/net (count + bytes), and the built
// door overlay. Called at generation (store) and activation (compare).
uint32 Rando_EntranceLayoutDigest24(const RandoSlotHeader *hdr) {
  EntranceRuntimeLayout *lay = &s_entrance_layout_scratch;
  if (hdr == NULL || !Entrance_ComputeLayout(hdr, lay)) return 0;
  uint64 h = 0xcbf29ce484222325ull;
#define DIG8(b) (h = (h ^ (uint8)(b)) * 0x100000001b3ull)
  DIG8((lay->cross ? 1 : 0) | (lay->cave ? 2 : 0) | (lay->dun ? 4 : 0) |
       (lay->decoupled ? 8 : 0) | (lay->decoupled_dun ? 16 : 0) |
       (lay->decoupled_cross ? 32 : 0));
  DIG8(lay->cross_n);
  for (int i = 0; i < lay->cross_n; i++) DIG8(lay->cross_assign[i]);
  DIG8(lay->cave_n);
  for (int i = 0; i < lay->cave_n; i++) DIG8(lay->cave_assign[i]);
  DIG8(lay->dun_n);
  for (int i = 0; i < lay->dun_n; i++) DIG8(lay->dun_assign[i]);
  DIG8(lay->dec_n);
  for (int i = 0; i < lay->dec_n; i++) DIG8(lay->dec_assign[i]);
  DIG8(lay->dd_n);
  for (int i = 0; i < lay->dd_n; i++) DIG8(lay->dd_assign[i]);
  DIG8(lay->cd_n);
  for (int i = 0; i < lay->cd_n; i++) DIG8(lay->cd_assign[i]);
  DIG8(lay->overlay_len & 0xFF);
  DIG8((lay->overlay_len >> 8) & 0xFF);
  for (uint32 i = 0; i < lay->overlay_len; i++) DIG8(lay->overlay[i]);
#undef DIG8
  uint32 d = (uint32)(h ^ (h >> 32)) & 0xFFFFFF;
  return d != 0 ? d : 1;  // 0 is reserved for "absent" (legacy/no-shuffle headers)
}

// LOGIC-side (tracker/placer reachability) override installation for one
// computed entrance layout: the cross/cave/dungeon region+edge overrides plus
// the decoupled one-way exit edges. Extracted from Entrance_RuntimeInstall so
// Rando_ReinstallActiveSlotLogicOverlays can replay EXACTLY these
// sub-steps after a mid-session generation cleared the override stores —
// without touching the gameplay-side installs (asset-126 door overlay,
// decoupled runtime nets), which generation never disturbs. The Apply* calls
// self-reset their stores (Rando_Begin*Overrides), so no explicit Clear is
// needed first; ApplyDecoupledExitEdges layers onto the edge set begun by the
// dungeon/cross pass, or Begins it itself for cave-only decoupled.
static void Entrance_ApplyLogicOverrides(const EntranceRuntimeLayout *lay) {
  if (lay->cross) {
    // ApplyCrossOverrides installs all 4 cross-class override cases.
    Entrance_ApplyCrossOverrides(lay->cross_assign, lay->cross_n);
  } else {
    if (lay->cave) Entrance_ApplyRegionOverrides(lay->cave_assign, lay->cave_n);
    if (lay->dun) Entrance_ApplyEdgeOverrides(lay->dun_assign, lay->dun_n);
  }
  // dec_n > 0 only when the cave-decoupled axis computed an exit net (see
  // Entrance_ComputeLayout) — the same condition the install site used.
  if (lay->dec_n > 0)
    Entrance_ApplyDecoupledExitEdges(lay->dec_assign, lay->dec_n);  // tracker reachability
}

static void Entrance_RuntimeInstall(const RandoSlotHeader *h) {
  Entrance_RuntimeTeardown();
  EntranceRuntimeLayout *lay = &s_entrance_layout_scratch;
  if (!Entrance_ComputeLayout(h, lay)) return;
  // X.1 backward-load, LEGACY slots only (entrance_digest24 == 0 — pre-digest
  // sidecars): the entrance permutation π is REGENERATED from (seed, axes,
  // attempt) against this build's interior/dungeon pool, so a slot written by a
  // different generator_version may regenerate a DIFFERENT π → doors that don't
  // match the baked placement. Warn loudly; the placement itself still loads.
  // Slots WITH a digest don't need the warning: Rando_ActivateSidecarSlot
  // already hard-failed on any actual layout drift (FIX #4), so reaching here
  // means the regenerated layout is certified identical.
  if (h->entrance_digest24 == 0 &&
      Rando_DetectVersionDrift(h, (uint16)kGeneratorVersion)) {
    fprintf(stderr,
        "Rando WARNING: this entrance-shuffle slot was generated by version %u "
        "but this build is version %u. The door layout is regenerated from the "
        "pool and may not match — regenerate the seed for correct entrances.\n",
        (unsigned)h->generator_version, (unsigned)kGeneratorVersion);
  }
  // Logic-side overrides so the in-game location tracker reflects the shuffled
  // reachability (gameplay itself reads the baked placement, not reachability).
  // Shared with the replay helper — see Entrance_ApplyLogicOverrides.
  Entrance_ApplyLogicOverrides(lay);
  memcpy(g_entrance_overlay, lay->overlay, lay->overlay_len);
  // Keep the ENTRY permutation for the runtime "did this door move" test — the
  // overlay byte can't answer it once pool entries share an entrance id. Under
  // Crossed the combined net is the entry permutation; otherwise it's the cave
  // one (dungeon-only shuffle leaves this empty, and the id compare is exact
  // there because dungeon entrance-ids are unique).
  g_entry_net_n = 0;
  g_entry_net_cross = false;
  if (lay->cross && lay->cross_n > 0) {
    g_entry_net_n = (lay->cross_n > kEntranceMaxInteriors) ? kEntranceMaxInteriors
                                                           : lay->cross_n;
    g_entry_net_cross = true;
    memcpy(g_entry_net, lay->cross_assign, (size_t)g_entry_net_n);
  } else if (lay->cave && lay->cave_n > 0) {
    g_entry_net_n = (lay->cave_n > kEntranceMaxInteriors) ? kEntranceMaxInteriors
                                                          : lay->cave_n;
    memcpy(g_entry_net, lay->cave_assign, (size_t)g_entry_net_n);
  }
  // Decoupled (D.4): install the one-way exit permutation(s) + the exit edges
  // for the tracker. Reset all first so a re-install starts clean; the cave
  // path also arms the runtime arrival replay.
  if (lay->decoupled || lay->decoupled_dun || lay->decoupled_cross) {
    Decoupled_Reset();
    Dungeon_Decoupled_Reset();
    Cross_Decoupled_Reset();
    if (lay->cd_n > 0) {
      // Cross + decoupled: one-way exits over the mixed pool. NO logic edges
      // (coupled-equivalent reachability; the ENTRY logic is ApplyCrossOverrides
      // above). net permutes the combined endpoint space; the runtime resolves a
      // cave-vs-dungeon target per source door at the entry hook.
      g_cross_decoupled_n = lay->cd_n;
      memcpy(g_cross_net, lay->cd_assign, (size_t)g_cross_decoupled_n);
      g_cross_decoupled_active = 1;
    }
    if (lay->dec_n > 0) {
      // (The matching logic-side exit edges installed above via
      // Entrance_ApplyLogicOverrides; only the runtime net lives here.)
      g_decoupled_n = lay->dec_n;
      memcpy(g_decoupled_net, lay->dec_assign, (size_t)g_decoupled_n);
      g_decoupled_active = 1;
    }
    if (lay->dd_n > 0) {
      // No logic edges (coupled-equivalent reachability is conservative + correct);
      // net' is the runtime exit-room redirect only.
      g_dungeon_decoupled_n = lay->dd_n;
      memcpy(g_dungeon_decoupled_net, lay->dd_assign, (size_t)g_dungeon_decoupled_n);
      g_dungeon_decoupled_active = 1;
    }
  }
  // Capture via the pristine resolver, not the live g_asset_ptrs[126]: equal
  // to the live pointer in every sane state (the activation-order
  // teardown below guarantees the Inverted override is off 126 before this
  // runs, and the leading Entrance_RuntimeTeardown NULLed our own saved
  // pointer), but if a future path ever reaches here with the Inverted shadow
  // still installed, the overlay's "vanilla" lookups (coupled-exit /
  // force-cached / capture hooks) and the teardown restore stay pristine
  // instead of inheriting the contaminated shadow — install and digest can
  // never diverge.
  g_entrance_overlay_orig = Entrance_PristineVanillaIds();
  g_asset_ptrs[126] = g_entrance_overlay;
}

// Audit/self-test hook — install the entrance layout that
// (seed, axes, attempt, world_state) regenerates, with no sidecar slot on disk.
// Deliberately routes through the SAME Entrance_ComputeLayout +
// Entrance_RuntimeInstall the game uses, so --rando-shop-doorwalk's shuffled
// arm walks the real door overlay and the real installed entry permutation
// rather than a re-derived model of them (a model that agreed with itself would
// validate nothing — the lesson the vanilla arm already encodes). Returns false
// when the axes produce no layout. Never call from gameplay: it leaves the
// overlay installed without an active slot behind it.
bool Rando_EntranceInstallLayoutForAudit(uint64 seed_u64, uint8 entrance_axes,
                                         uint8 entrance_attempt,
                                         uint8 world_state) {
  // Refuse while a slot is live. Entrance_RuntimeInstall re-Begins the shared
  // region/edge override stores, resets the decoupled nets and wipes
  // g_entrance_discovered — installing a synthetic layout over a real slot
  // would silently drop that slot's tracker overrides and its persisted
  // door-discovery bitmap. The doorwalk runs before any slot exists; anything
  // that does not is a bug in the caller, not a case to handle.
  if (g_rando_slot_active) {
    fprintf(stderr, "Rando_EntranceInstallLayoutForAudit: refused — a slot is "
                    "active; this hook clears shared override state\n");
    return false;
  }
  RandoSlotHeader h;
  memset(&h, 0, sizeof(h));
  h.slot_kind = kSlotKind_Randomizer;
  h.generator_version = (uint16)kGeneratorVersion;
  h.entrance_axes = entrance_axes;
  h.entrance_attempt = entrance_attempt;
  h.settings_ext_present = 1;
  h.world_state = world_state;
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  ss.seed_u64 = seed_u64;
  Share_PackBinary(&ss, h.share_string);
  Entrance_RuntimeInstall(&h);
  return g_entrance_overlay_orig != NULL;
}

void Rando_EntranceTeardownLayoutForAudit(void) { Entrance_RuntimeTeardown(); }

// Coupling helper (Stage 2): for the overworld entry hook. Given the door-slot
// `lx`, return the room the dungeon-exit room-keyed search should target so the
// player returns to THIS source door (rather than the loaded dungeon's vanilla
// door). 0 when the slot isn't a shuffled dungeon door (caves auto-couple; an
// unredirected door needs no override). Reads the saved vanilla table.
uint16 Rando_EntranceCoupledExitRoom(uint16 lx) {
  if (g_entrance_overlay_orig == NULL) return 0;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (lx >= len) return 0;
  uint8 vanilla_id = g_entrance_overlay_orig[lx];
  // Only when this door was actually redirected (overlay value differs).
  if (kOverworld_Entrance_Id[lx] == vanilla_id) return 0;
  return Entrance_DungeonSourceExitRoom(vanilla_id);  // 0 if not a v1 dungeon door
}

// Cross-category coupling (Stage 3): true iff door-slot `lx` is a CAVE source
// door that the overlay redirected to a DUNGEON interior (loaded room < 0x100).
// Such a door takes the room-keyed search exit (it loads a dungeon room) but must
// return to the cave — the entry hook sets g_rando_entrance_force_cached so the
// exit uses the cached source-cave position instead. (cave→cave loads a cave room
// and already uses the cached branch, so it is intentionally NOT flagged here —
// keeps the playtest-confirmed within-category path untouched.)
bool Rando_EntranceForceCachedExit(uint16 lx) {
  g_rando_force_cached_room = 0;  // set together with the flag (only on the true path)
  if (g_entrance_overlay_orig == NULL) return false;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (lx >= len) return false;
  uint8 vanilla_id = g_entrance_overlay_orig[lx];
  if (!Entrance_IsCaveEntranceId(vanilla_id)) return false;   // source not a cave
  uint8 target = kOverworld_Entrance_Id[lx];
  if (target == vanilla_id) return false;                     // not redirected
  const uint16 *rooms = kEntranceData_rooms;
  uint32 rc = kEntranceData_rooms_SIZE / 2u;
  if (rooms == NULL || target >= rc) return false;
  if (rooms[target] >= 0x100) return false;                   // target not a dungeon
  // Stash the SOURCE cave's room so the exit can restore it into
  // dungeon_room_index before LoadCachedEntranceProperties (whose vanilla
  // room-keyed Y-adjust would otherwise see the loaded dungeon room).
  if (vanilla_id < rc) g_rando_force_cached_room = rooms[vanilla_id];
  return true;
}

// Dungeon decoupled (Insanity): the exit-search target room for a one-way dungeon
// exit. Keyed on the LOADED dungeon (the overlay value at slot lx), so exiting the
// dungeon physically behind this door emerges at net'[loaded]'s overworld door
// instead of the source. 0 when inactive, not a pooled dungeon, or a self-map
// (caller keeps the coupled return-to-source). Mirrors Rando_EntranceCoupledExitRoom.
uint16 Rando_EntranceDungeonDecoupledExitRoom(uint16 lx) {
  if (!g_dungeon_decoupled_active) return 0;
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (lx >= len) return 0;
  uint8 loaded_id = ((const uint8 *)kOverworld_Entrance_Id)[lx];  // overlay → loaded dungeon
  return Entrance_DungeonDecoupledExitRoom(g_dungeon_decoupled_net,
                                           g_dungeon_decoupled_n, loaded_id);
}

// Active slot's recovered settings + shuffle assignments (format_version >= 2
// slots carry the canonical settings blob). g_rando_active_settings_valid gates
// the runtime reachability engine the tracker windows consume: when false
// (older v1 slot, snapshot-restore, or a slot whose writer didn't populate the
// blob), reachability is SUPPRESSED rather than computed from guessed defaults —
// a wrong prize_shuffle flag mis-seeds the shuffle stream and yields
// confidently-wrong prize/medallion gating.
static RandoSettings g_rando_active_settings;
static bool g_rando_active_settings_valid = false;

// add-rando-shopsanity (external-review P2 fix): the active slot's seed for
// price derivation. A plain cold snapshot replay restores settings + share
// string but does NOT reconstruct g_rando_active_header, so deriving the
// price seed from the header fell back to seed 0 there — cold-replayed
// shops displayed AND charged seed-0 prices. Every settings_valid writer
// populates this; deactivation and the snapshot settings-clear zero it.
static uint64 g_rando_active_seed_u64;

// Read-only accessor for the live-apply path (config.c re-seeds the cosmetic
// tables on an INI CosmeticSeed change and must pass the active slot's seed —
// 0 when no slot is active — so a change back to 0 keeps tracking the slot).
uint64 Rando_ActiveSeedU64(void) { return g_rando_active_seed_u64; }

// add-rando-random-crystals — the active slot's RESOLVED crystal counts,
// cached at activation from Crystals_Resolve(settings, base seed). 7/7
// fail-closed when no valid slot (vanilla-equivalent). All runtime
// consumers go through the getters; the requested sentinel (8) never
// reaches a gate.
static uint8 g_rando_effective_crystals_ganon = 7;
static uint8 g_rando_effective_crystals_tower = 7;

uint8 Rando_EffectiveCrystalsGanon(void) { return g_rando_effective_crystals_ganon; }
uint8 Rando_EffectiveCrystalsTower(void) { return g_rando_effective_crystals_tower; }
// distinguishes "settings valid because a GENUINE slot is active" (false)
// from "valid because a snapshot COLD-REPLAY restored them" (true). The cold-
// replay gate must protect a genuine active slot, yet still let a NEW cold replay
// supersede a PRIOR one — else a 2nd Ctrl+F1 of a different seed (slot still not
// loaded) would early-return and keep the 1st seed's world_state/Inverted/shuffles.
static bool g_rando_settings_from_cold_replay = false;
static bool g_rando_seed_qol_features0_saved = false;
static uint32 g_rando_seed_qol_config_features0 = 0;

void Rando_RebuildKeyItemOwnership(void) {
  uint16 placement_selected = 0;
  uint16 owned = 0;
  bool skeleton_present = false;
  bool skeleton_owned = false;
  const RandoPlacementTable *table = Placement_GetActive();

  if (g_rando_slot_active && table != NULL && table->entries != NULL) {
    for (uint16 i = 0; i < table->count; i++) {
      uint16 item = table->entries[i].item_id;
      if (Rando_IsKeyRingItem(item)) {
        uint8 d = Rando_RandoDungeonFromDungeonItem(item);
        if (d < kRandoDungeon_Count) {
          uint16 bit = (uint16)(1u << d);
          placement_selected |= bit;
          if (Rando_IsLocationChecked(table->entries[i].location_id))
            owned |= bit;
        }
      } else if (item == ITEM_SkeletonKey) {
        skeleton_present = true;
        if (Rando_IsLocationChecked(table->entries[i].location_id))
          skeleton_owned = true;
      }
    }
  }

  // The installed placement table is authoritative for derived runtime state.
  // Current slots/snapshots also carry enough state to reproduce the selector;
  // use that only as a consistency check, never as a replacement for the rows
  // actually installed (and never erase valid rows when a legacy share cannot
  // be decoded).
  uint16 selected = placement_selected;
  if (g_rando_slot_active && g_rando_active_settings_valid) {
    KeyRingSelection recomputed;
    ShareString ss;
    if (g_rando_active_share_string[0] != '\0' &&
        Share_Decode(g_rando_active_share_string, &ss) == kShareDecodeOk &&
        KeyRings_Resolve(&g_rando_active_settings, ss.seed_u64, &recomputed) &&
        recomputed.selected_mask != placement_selected) {
      // A mismatch means the persisted selector inputs no longer describe the
      // installed table. Surface it while continuing to display/use the table.
      fprintf(stderr,
              "Rando WARNING: selected Key Ring mask %04x differs from placement mask %04x\n",
              (unsigned)recomputed.selected_mask, (unsigned)placement_selected);
    }
    skeleton_present = skeleton_present || g_rando_active_settings.skeleton_key != 0;
  }

  g_rando_key_ring_selected_mask = selected;
  g_rando_key_ring_owned_mask = owned;
  g_rando_skeleton_key_present = skeleton_present;
  g_rando_skeleton_key_owned = skeleton_owned;
  g_reachability_state_counter++;
}

// add-rando-grass-rock-shuffle — capped nearest-N terrain glint collector.
// The 128-sprite OAM budget (up to ~159 in-scope objects on the densest
// screens) forbids glinting every object, so surface only the closest few
// unchecked+active terrain checks to Link; they update as Link moves. Returns
// the count of area-map16 positions written to out_pos[0..max), nearest-first.
// The draw side (sprite.c Rando_DrawTerrainGlints) converts pos -> world ->
// screen and reuses the overworld enemy-glint OAM/palette machinery.
int Rando_CollectTerrainGlints(uint16 *out_pos, int max) {
  if (out_pos == NULL || max <= 0) return 0;
  // Cheap early-out for non-terrain seeds (skip the per-frame per-object scan).
  if (!g_rando_active_settings_valid ||
      !Rando_SettingsNeedTerrainRegistry(&g_rando_active_settings))
    return 0;
  uint16 scr = (uint16)overworld_screen_index;
  if (scr >= 0x80) return 0;
  int32 lx = (int32)link_x_coord, ly = (int32)link_y_coord;
  int32 basex = (int32)overworld_offset_base_x << 3;
  int32 basey = (int32)overworld_offset_base_y;
  // Nearest-N kept sorted ascending by squared distance (small max, so an
  // insertion scan is cheaper than a heap).
  uint32 best_d[kRandoTerrainGlintCap];
  int n = 0;
  if (max > kRandoTerrainGlintCap) max = kRandoTerrainGlintCap;
  for (uint32 i = 0; i < kRandoTerrainLookup_COUNT; i++) {
    if (kRandoTerrainLookup[i].screen != scr) continue;
    uint16 loc = kRandoTerrainLookup[i].loc_id;
    if (Placement_Lookup(loc, 0xFFFFu) == 0xFFFFu) continue;  // inactive tier
    if (Rando_IsLocationChecked(loc)) continue;               // already checked
    uint16 pos = kRandoTerrainLookup[i].pos;
    int col = (pos & 0x7e) >> 1, row = pos >> 7;
    int32 dx = (basex + col * 16) - lx;
    int32 dy = (basey + row * 16) - ly;
    uint32 d = (uint32)(dx * dx + dy * dy);
    if (n == max && d >= best_d[n - 1]) continue;  // farther than the worst kept
    int ins = n < max ? n : n - 1;
    while (ins > 0 && best_d[ins - 1] > d) {
      best_d[ins] = best_d[ins - 1];
      out_pos[ins] = out_pos[ins - 1];
      ins--;
    }
    best_d[ins] = d;
    out_pos[ins] = pos;
    if (n < max) n++;
  }
  return n;
}
static uint32 g_rando_seed_qol_wanted_features0 = 0;
static uint32 g_rando_seed_qol_enhanced_features0 = 0;
static void rando_snapshot_seed_qol_features0(void);
static void rando_restore_seed_qol_features0(void);
// Active-slot assignment replay sources. Rando_Set*Assignment copies these into
// the VM-owned assignment stores; keeping the active seed's bytes here lets
// Rando_ReinstallActiveSlotLogicOverlays restore them after an out-of-band
// generation clobbers the VM stores.
static uint8 g_rando_active_prize_assignment[kRandoDungeonCount];
static uint8 g_rando_active_medallion_assignment[kRandoMedallionEntranceCount];
// Boss-shuffle assignment for the active slot — drives BOTH the render redirect
// (BossShuffle_Generate installs g_boss_assignment in shuffle_boss.c) AND the
// in-game logic VM (Rando_SetBossAssignment, so OP_CAN_KILL_BOSS reachability /
// the tracker agree with the bosses actually spawned). Regenerated from
// (settings, base seed) at slot load, same as prize/medallion.
static uint8 g_rando_active_boss_assignment[16];
// replay inputs for Rando_ReinstallActiveSlotLogicOverlays(): the
// ACTIVE slot's header (the same fields Entrance_RuntimeInstall and the door
// regen consumed at activation) plus whether activation installed a door logic
// layout. Captured once the slot's installs begin; invalidated by
// Rando_DeactivateSlot — including every activation refusal path, which routes
// through it.
static RandoSlotHeader g_rando_active_header;
static bool g_rando_active_header_valid = false;
static bool g_rando_active_door_logic = false;
static bool g_rando_active_chains_logic = false;
// The ACTIVE slot's regenerated door layout. Persistent storage is required —
// Rando_SetDoorLogicLayout stores the POINTER — and file scope (rather than the
// former function-local static in Rando_ActivateSidecarSlot) lets the replay
// helper regenerate into the same storage. Generation uses its own
// static (g_door_gen_layout in rando_generate.c), so a mid-session generation
// never clobbers these bytes — only the installed pointer.
static DoorShuffleLayout s_active_door_layout;
// Same lifetime requirement for dungeon chains: both logic-edge overrides and
// runtime seams are regenerated from the persisted attempt/digest identity.
static DungeonChainsLayout s_active_chains_layout;

static uint8 rando_door_enemy_drop_keys_for_settings(const RandoSettings *settings) {
  if (settings == NULL ||
      Settings_EffectiveDoorShuffle(settings) == kDoorShuffle_Vanilla)
    return 0;
  return Settings_EnemyDropKeysActive(settings) ? 1 : 0;
}

static uint8 rando_door_enemy_check_tier_for_settings(const RandoSettings *settings) {
  if (settings == NULL ||
      Settings_EffectiveDoorShuffle(settings) == kDoorShuffle_Vanilla)
    return kEnemyDropChecks_Off;
  uint8 tier = Settings_EffectiveEnemyDropChecks(settings);
  return tier >= kEnemyDropChecks_Dungeon ? tier : kEnemyDropChecks_Off;
}

static uint32 rando_door_layout_digest24(const DoorShuffleLayout *layout) {
  return DoorShuffle_LayoutDigest(layout) & 0xFFFFFFu;
}

static void rando_clear_door_layout_runtime(void) {
  DoorRt_Reset();
  Rando_SetDoorLogicLayout(NULL, 0);
  g_wanted_zelda_features1 &= ~(uint32)kFeatures1_DoorShuffleActive;
  enhanced_features1 &= ~(uint32)kFeatures1_DoorShuffleActive;
}

static bool rando_prepare_door_layout(const RandoSettings *settings,
                                      const uint8 share_string_raw[32],
                                      uint8 door_attempt,
                                      uint32 expected_digest24,
                                      const char *context) {
  if (settings == NULL || share_string_raw == NULL ||
      Settings_EffectiveDoorShuffle(settings) == kDoorShuffle_Vanilla) {
    fprintf(stderr,
            "Rando: %s door-shuffle restore missing required layout identity "
            "— refusing door graph\n",
            context);
    return false;
  }
  uint64 slot_seed = SlotSeedFromShareString(share_string_raw);
  bool ok = DoorShuffle_Generate(slot_seed, door_attempt,
                                 kDoorShuffle_MvpDungeonMask,
                                 Settings_DoorPotTier(settings),
                                 rando_door_enemy_drop_keys_for_settings(settings),
                                 rando_door_enemy_check_tier_for_settings(settings),
                                 &s_active_door_layout);
  uint32 digest = ok ? rando_door_layout_digest24(&s_active_door_layout) : 0;
  if (!ok || digest != (expected_digest24 & 0xFFFFFFu)) {
    fprintf(stderr,
            "Rando: %s door-shuffle layout drift (regen digest %06x != saved %06x) "
            "— refusing door graph\n",
            context, (unsigned)digest, (unsigned)(expected_digest24 & 0xFFFFFFu));
    return false;
  }
  if (DoorRt_KindOverlaySelfCheck(&s_active_door_layout) != 0) {
    fprintf(stderr,
            "Rando: %s door-shuffle kind overlay rejected this layout "
            "— refusing door graph\n",
            context);
    return false;
  }
  return true;
}

static bool rando_install_prepared_door_layout(const char *context) {
  Rando_SetDoorLogicLayout(&s_active_door_layout, s_active_door_layout.shuffled_mask);
  DoorRt_Reset();
  for (int i = 0; i < kDoorTbl_DoorCount; i++) {
    if (s_active_door_layout.pairing[i] != 0xFFFF)
      DoorRt_SetLink((uint16)i, s_active_door_layout.pairing[i]);
  }
  if (!DoorRt_InstallKindOverlay(&s_active_door_layout)) {
    fprintf(stderr, "Rando: %s door-shuffle kind overlay install failed\n", context);
    rando_clear_door_layout_runtime();
    return false;
  }
  DoorRt_Activate();
  g_wanted_zelda_features1 |= kFeatures1_DoorShuffleActive;
  enhanced_features1 |= kFeatures1_DoorShuffleActive;
  return true;
}

static uint32 rando_chains_layout_digest24(const DungeonChainsLayout *layout) {
  return Chains_LayoutDigest(layout) & 0xFFFFFFu;
}

static void rando_clear_chains_layout_runtime(void) {
  if (g_rando_active_chains_logic)
    Entrance_ClearEdgeOverrides();
  Chains_RuntimeTeardown();
  g_rando_active_chains_logic = false;
}

static bool rando_prepare_chains_layout(const RandoSettings *settings,
                                        const uint8 share_string_raw[32],
                                        uint8 chains_attempt,
                                        uint32 expected_digest24,
                                        const char *context) {
  if (settings == NULL || share_string_raw == NULL ||
      !Settings_EffectiveDungeonChains(settings)) {
    fprintf(stderr,
            "Rando: %s dungeon-chain restore missing required layout identity "
            "- refusing chain graph\n",
            context);
    return false;
  }
  uint64 slot_seed = SlotSeedFromShareString(share_string_raw);
  bool ok = Chains_Compute(slot_seed, chains_attempt, &s_active_chains_layout);
  uint32 digest = ok ? rando_chains_layout_digest24(&s_active_chains_layout) : 0;
  if (!ok || digest != (expected_digest24 & 0xFFFFFFu)) {
    fprintf(stderr,
            "Rando: %s dungeon-chain layout drift (regen digest %06x != saved %06x) "
            "- refusing chain graph\n",
            context, (unsigned)digest, (unsigned)(expected_digest24 & 0xFFFFFFu));
    return false;
  }
  return true;
}

static bool rando_install_prepared_chains_layout(const char *context) {
  if (!Chains_RuntimeInstallLayout(&s_active_chains_layout)) {
    fprintf(stderr,
            "Rando: %s dungeon-chain runtime install failed - refusing chain graph\n",
            context);
    Entrance_ClearEdgeOverrides();
    Chains_RuntimeTeardown();
    return false;
  }
  Chains_ApplyEdgeOverrides(&s_active_chains_layout);
  g_rando_active_chains_logic = true;
  return true;
}

static const uint16 kRandoTrapExtraGoodItemDecoys[] = {
  ITEM_Map_HyruleCastleEscape,
  ITEM_GenericKey,
  ITEM_BluePotion,
  ITEM_RedPotion,
  ITEM_BeeContents,
  ITEM_HeartRefill,
};

static uint32 rando_trap_good_item_decoy_count(void) {
  return (uint32)(ITEM_Compass_GanonsTower + 1) +
         (uint32)(ITEM_Prize_Crystal7 - ITEM_Prize_GreenPendant + 1) +
         (uint32)countof(kRandoTrapExtraGoodItemDecoys);
}

static uint16 rando_trap_good_item_decoy_at(uint32 idx) {
  if (idx <= ITEM_Compass_GanonsTower)
    return (uint16)idx;
  idx -= (uint32)(ITEM_Compass_GanonsTower + 1);
  if (idx <= (uint32)(ITEM_Prize_Crystal7 - ITEM_Prize_GreenPendant))
    return (uint16)(ITEM_Prize_GreenPendant + idx);
  idx -= (uint32)(ITEM_Prize_Crystal7 - ITEM_Prize_GreenPendant + 1);
  if (idx < countof(kRandoTrapExtraGoodItemDecoys))
    return kRandoTrapExtraGoodItemDecoys[idx];
  return 0xFFFFu;
}

static bool rando_icon_from_lttp_code(uint8 code, DirectGrantIconEntry *out) {
  if (out == NULL || code >= 76 || kReceiveItemGfx[code] == 0xff)
    return false;
  uint8 a = kWishPond2_OamFlags[code];
  if (a & 0x80)
    a = 5;
  out->gfx = kReceiveItemGfx[code];
  out->big = kReceiveItem_Tab1[code];
  out->oam_flags = (uint8)(a * 2 | 0x30);
  return true;
}

static uint8 rando_trap_decoy_lttp_for_item(uint16 item_id) {
  switch (item_id) {
    case ITEM_ProgressiveSword:  return 0x00;
    case ITEM_ProgressiveShield: return 0x04;
    case ITEM_ProgressiveArmor:  return 0x22;
    case ITEM_ProgressiveGlove:  return 0x1b;
    case ITEM_ProgressiveBow:    return 0x0b;
    case ITEM_BottleEmpty:           return 0x16;
    case ITEM_BottleWithFairy:       return 0x2c;
    case ITEM_BottleWithBee:         return 0x2b;
    case ITEM_BottleWithGoodBee:     return 0x3c;
    case ITEM_BottleWithRedPotion:   return 0x2d;
    case ITEM_BottleWithGreenPotion: return 0x3d;
    case ITEM_BottleWithBluePotion:  return 0x48;
    case ITEM_SilverArrowUpgrade: return 0x3b;
    default:
      return Rando_VanillaItemForRegistryId(item_id);
  }
}

static bool rando_good_item_decoy_icon(uint16 item_id, DirectGrantIconEntry *out) {
  const DirectGrantIconEntry *direct = rando_direct_grant_icon_entry(item_id);
  if (direct != NULL) {
    *out = *direct;
    return true;
  }
  return rando_icon_from_lttp_code(rando_trap_decoy_lttp_for_item(item_id), out);
}

static uint32 rando_trap_decoy_mix(uint64 seed, uint16 item_id, uint16 location_id) {
  uint64 x = seed ^ ((uint64)location_id * 0x9E3779B97F4A7C15ull) ^
             ((uint64)item_id * 0xBF58476D1CE4E5B9ull);
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  x ^= x >> 31;
  return (uint32)(x ^ (x >> 32));
}

static uint64 rando_trap_decoy_seed(void) {
  if (g_rando_active_header_valid)
    return SlotSeedFromShareString(g_rando_active_header.share_string);
  return 0xD1CE5AFE7A9B3C4Dull;
}

// === add-rando-shopsanity — active-slot shop-check runtime ==================

// Rando active AND the active slot's settings carry the shopsanity axis.
// The valid flag matters: v1/no-blob slots recover no settings, and the
// struct is NOT cleared on deactivation — without the gate a stale
// shopsanity=1 from a previous slot would run the check machinery on a
// plain Retro slot and permanently convert its first-bought economy column
// to vanilla restock (the race-mode NULL-settings fail-open class). No
// blob = axis off (its default), matching rando_instant_flute_active's
// defaults-for-legacy convention.
static bool rando_shopsanity_active(void) {
  return (enhanced_features1 & kFeatures1_RandomizerActive) &&
         g_rando_active_settings_valid &&
         Settings_ShopsanityActive(&g_rando_active_settings);
}

// add-rando-bonk-sanity — resolve a DASH wake of a placed bonk sprite to its
// unchecked check location. 0xFFFF = not a check right now (axis off, no
// valid slot, unknown (area,block,type), no placement entry, or already
// checked -> the caller runs the vanilla wake). The lookup table is tiny
// (kRandoBonkLookup_COUNT rows); linear scan.
uint16 Rando_BonkCheckLocForWake(uint8 area, uint16 block, uint8 sprite_type_id) {
  if (!(enhanced_features1 & kFeatures1_RandomizerActive) ||
      !g_rando_active_settings_valid ||
      g_rando_active_settings.bonk_shuffle == kTerrainShuffle_Off)
    return 0xFFFFu;
  (void)sprite_type_id;  // identity is (area, block); type recorded in the registry
  for (uint32 i = 0; i < (uint32)kRandoBonkLookup_COUNT; i++) {
    if (kRandoBonkLookup[i].area == area && kRandoBonkLookup[i].block == block) {
      uint16 loc = kRandoBonkLookup[i].loc_id;
      if (Rando_IsLocationChecked(loc)) return 0xFFFFu;
      if (Placement_Lookup(loc, 0xFFFFu) == 0xFFFFu) return 0xFFFFu;
      return loc;
    }
  }
  return 0xFFFFu;
}

bool Rando_ShopSlotCheckInfo(uint16 room, uint8 door, uint8 pos_plus1,
                             uint16 *out_loc, uint16 *out_item,
                             uint16 *out_price) {
  // pos_plus1 is ShopKeeper_SpawnShopItem's stored slot (pos+1, 0 = unset).
  // Only the three vanilla columns are check slots — the Retro genericKey
  // 4th column (pos 3) is rejected here AND by shop_lookup's own pos > 2
  // bound (belt and braces; this gate also filters pos_plus1 == 0 "unset").
  if (pos_plus1 < 1 || pos_plus1 > 3) return false;
  if (!rando_shopsanity_active()) return false;
  uint16 loc = shop_lookup(room, door, (uint8)(pos_plus1 - 1));
  if (loc == 0xFFFFu) return false;
  if (Rando_IsLocationChecked(loc)) return false;  // bought → vanilla restock
  uint16 placed = Placement_Lookup(loc, 0xFFFFu);
  if (placed == 0xFFFFu) return false;  // no placement entry (defensive)
  if (out_loc) *out_loc = loc;
  if (out_item) *out_item = placed;
  if (out_price) *out_price = Rando_ShopPrice(g_rando_active_seed_u64, loc);
  return true;
}

// add-rando-shopsanity — see rando.h. Side-effect-free "this purchase would
// grant nothing" probe, so the paid front-end can consume the check (terminal,
// as the refusal contract requires) without debiting for an empty delivery.
bool Rando_ShopSlotGrantIsNoOp(uint16 room, uint8 door, uint8 pos_plus1) {
  uint16 loc, placed;
  if (!Rando_ShopSlotCheckInfo(room, door, pos_plus1, &loc, &placed, NULL))
    return false;
  RandoGrantPlan plan;
  if (!Rando_ResolveLiveGrantPlan(loc, placed, &plan))
    return false;  // fail closed: an unresolvable plan is not a proven no-op
  return plan.disposition == kRandoGrantDisposition_AcceptedNoOp;
}

static bool rando_trap_decoy_icon_for_seed(uint64 seed, uint16 item_id,
                                           uint16 location_id,
                                           DirectGrantIconEntry *out) {
  if (!rando_is_trap_item(item_id) || out == NULL)
    return false;
  uint32 count = rando_trap_good_item_decoy_count();
  uint32 start = rando_trap_decoy_mix(seed, item_id, location_id) % count;
  for (uint32 i = 0; i < count; i++) {
    uint16 decoy_item = rando_trap_good_item_decoy_at((start + i) % count);
    if (rando_good_item_decoy_icon(decoy_item, out))
      return true;
  }
  return false;
}

static bool rando_trap_decoy_icon(uint16 item_id, uint16 location_id,
                                  DirectGrantIconEntry *out) {
  return rando_trap_decoy_icon_for_seed(rando_trap_decoy_seed(), item_id,
                                        location_id, out);
}

static bool rando_instant_flute_active(void) {
  // v1/no-blob slots and self-tests have no recovered settings; treat them as
  // default settings, where instant flute activation is ON.
  return !g_rando_active_settings_valid || g_rando_active_settings.instant_flute != 0;
}

// Re-derive + install this slot's logic-side shuffle assignments
// (prize/medallion + boss/drop/enemy) from (settings, BASE seed, prize_attempt)
// into the active-slot stores. Factored out of Rando_ActivateSidecarSlot so the
// snapshot cold-replay restore (Rando_SnapshotColdReplayRestore, below) re-derives
// IDENTICALLY and the two can't drift. Writes the g_rando_active_* assignment
// statics and repoints the VM via Rando_Set*Assignment (which now copy).
//
// shuffle_seed mirrors FIX #6 EXACTLY: the placer seeds prize/medallion from the
// ACCEPTED attempt's per-attempt seed (base ^ attempt*golden — see Place_AssumedFill),
// so re-derive with the same perturbation or the runtime falling-prize sprite
// (dungeon.c RandoFallingPrizeIndex) + OP_HAS_PRIZE reachability disagree with the
// prize/medallion BAKED into the placement table. prize_attempt is 0 for attempt-0
// + v1 slots (XOR 0 == the legacy base-seed derivation). DropShuffle ignores the
// placement-table arg (shuffle_drops.c), so g_session_placement_table being empty
// on the cold-replay path is harmless.
// add-rando-ow-warp-shuffle — the ACTIVE slot's regenerated warp layout,
// consumed by the runtime hooks (flute menu/map, whirlpool partner remap).
// Valid only while g_rando_active_ow_warp.
static OwWarpLayout g_rando_active_ow_layout;
static bool g_rando_active_ow_warp = false;

const OwWarpLayout *Rando_ActiveOwWarpLayout(void) {
  return g_rando_active_ow_warp ? &g_rando_active_ow_layout : NULL;
}

static void rando_clear_ow_warp_runtime(void) {
  g_rando_active_ow_warp = false;
  memset(&g_rando_active_ow_layout, 0, sizeof(g_rando_active_ow_layout));
}

// Returns false ONLY on a warp-layout failure that must refuse the slot
// (fail-closed graph absence, or digest drift — a drifted layout can strand
// a certified placement; door-gate class, not the entrance warn class).
static bool install_active_shuffles(const RandoSettings *s, uint64 base_seed,
                                    uint8 prize_attempt, uint8 ow_attempt,
                                    uint32 ow_digest24) {
  RandoRng shuffle_rng;
  uint64 shuffle_seed = base_seed ^ ((uint64)prize_attempt * 0x9E3779B97F4A7C15ull);
  Rng_SeedFromU64(&shuffle_rng, shuffle_seed);
  if (!Alttpr_Assignments(g_rando_active_prize_assignment,
                          g_rando_active_medallion_assignment)) {
    PrizeShuffle_Run(s, &shuffle_rng, g_rando_active_prize_assignment);
    MedallionShuffle_Run(s, &shuffle_rng, g_rando_active_medallion_assignment);
  }
  Rando_SetDungeonPrizeAssignment(g_rando_active_prize_assignment);
  Rando_SetMedallionAssignment(g_rando_active_medallion_assignment);
  (void)DropShuffle_Generate(s, base_seed, &g_session_placement_table, NULL);
  (void)BossShuffle_Generate(s, base_seed, g_rando_active_boss_assignment);
  Rando_SetBossAssignment(g_rando_active_boss_assignment);
  (void)EnemyShuffle_Generate(s, base_seed);
  // add-rando-ow-warp-shuffle — regenerate + digest-gate + install the warp
  // layout. Runs AFTER any entrance overlay install (activation order) and
  // composes on it; Begins the overlay itself when warp runs alone.
  rando_clear_ow_warp_runtime();
  // Must match ow_warp_apply_attempt's gate EXACTLY or a slot regenerates a
  // layout generation never installed (or vice versa) and the digest gate below
  // refuses it. Settings_OwWarpActive is that shared authority — it re-derives
  // the Inverted force-off that apply_derived_rules only applies to the
  // canonical copy.
  bool warp_on = Settings_OwWarpActive(s);
  if (!warp_on) return true;
  OwWarpLayout l;
  if (!OwWarp_Compute(s, base_seed, ow_attempt, &l)) {
    fprintf(stderr, "Rando: warp-axis slot but the OW graph tables are "
                    "absent/empty — refusing slot (fail-closed)\n");
    return false;
  }
  uint32 digest = OwWarp_Digest24(&l) & 0xFFFFFFu;
  if (digest != (ow_digest24 & 0xFFFFFFu)) {
    fprintf(stderr, "Rando: OW warp layout drift (regen digest %06x != saved "
                    "%06x) — refusing slot\n",
            (unsigned)digest, (unsigned)(ow_digest24 & 0xFFFFFFu));
    return false;
  }
  OwWarp_InstallLogicEdges(&l);
  g_rando_active_ow_layout = l;
  g_rando_active_ow_warp = true;
  // Activation-time layout summary (the runtime-debugging convention: the
  // layout is static per slot, so one stderr line at install serves the
  // F12-dump role for warp state). RACE GATE: spot identities are hidden
  // info on race slots — print only the non-revealing identity there.
  if (Rando_ActiveSlotHidesSpoiler()) {
    fprintf(stderr, "Rando: OW warp layout active=%u attempt=%u digest=%06x "
                    "(race slot — spots hidden)\n",
            l.active, ow_attempt, (unsigned)digest);
  } else {
    fprintf(stderr, "Rando: OW warp layout active=%u attempt=%u digest=%06x "
                    "flute={", l.active, ow_attempt, (unsigned)digest);
    for (int i = 0; i < kOwWarpFluteSlots; i++)
      fprintf(stderr, "%s%02x", i ? "," : "",
              kRandoOwFluteCandidates[l.flute_cand[i]].screen);
    fprintf(stderr, "}\n");
  }
  return true;
}

// snapshot replay restore. Called by RandoSnapshotTail_Load when a type-2
// RandoSettings TLV is read, to reconstruct the slot's logic-side state
// (prize/medallion/boss/drop/enemy assignments + Inverted installs + the
// JP-glitch coupling) from the snapshot-carried (canonical settings, share
// string→seed, prize_attempt). Mirrors the corresponding arm of
// Rando_ActivateSidecarSlot via the shared install_active_shuffles helper, so
// the two can't drift.
//
// GATED only for the same genuinely-active seed: a normal same-slot replay keeps
// the activation installs. A cold replay, a prior cold-replayed snapshot, or a
// snapshot from a different active seed must supersede the process state, or the
// restored placement table can be evaluated with stale assignments/layout replay
// inputs from another seed. (A v1/no-blob slot emits no type-2 TLV, so this never
// fires under it.) The caller restores the 4 process-static ownership bytes
// unconditionally — that is separate, time-varying snapshot state this function
// does not touch.
bool Rando_SnapshotColdReplayRestore(const RandoSettings *s,
                                     const uint8 *share_string_raw,
                                     uint8 prize_attempt, uint8 ow_attempt,
                                     uint32 ow_digest24) {
  if (s == NULL || share_string_raw == NULL) return false;
  bool preserve_active_header =
      g_rando_active_header_valid &&
      !g_rando_settings_from_cold_replay &&
      memcmp(g_rando_active_header.share_string, share_string_raw, 32) == 0;
  // Protect a GENUINE active slot only when the replayed snapshot belongs to
  // that same seed. A different snapshot seed must replace the process state.
  if (g_rando_active_settings_valid && preserve_active_header) {
    return false;
  }
  Rando_ClearDeferredPotConfirmation();

  if (!preserve_active_header) {
    g_rando_active_header_valid = false;
    g_rando_active_door_logic = false;
    rando_clear_chains_layout_runtime();
    // The entrance-shuffle overlay is process state installed from the
    // superseded header — tear it down so a cold replay never keeps a
    // DIFFERENT seed's permutation. A type-9 EntranceLayout TLV reinstalls
    // this snapshot's verified layout right after (older entrance-shuffled
    // snapshots lack it and the loader then fails CLOSED via the pending
    // check instead of playing with vanilla entrances).
    Entrance_RuntimeTeardown();
  }
  g_rando_active_settings = *s;
  g_rando_active_world_state = s->world_state;
  uint64 seed = SlotSeedFromShareString(share_string_raw);
  g_rando_active_seed_u64 = seed;  // price/derivation seed (see declaration)
  if (!install_active_shuffles(&g_rando_active_settings, seed, prize_attempt,
                               ow_attempt, ow_digest24)) {
    // Warp layout drift/absence on a cold replay: refuse the restore — the
    // replayed placement was certified against a layout this build cannot
    // reproduce (door-gate class).
    Rando_DeactivateSlot();
    return false;
  }
  // add-rando-random-crystals — the cold-replay restore is the THIRD
  // settings_valid=true writer (implementation-review F1): without this
  // resolve the five cached-getter consumers keep the previous slot's (or
  // the 7/7 default) counts — a replay desync even for FIXED-count seeds.
  Crystals_Resolve(&g_rando_active_settings, seed,
                   &g_rando_effective_crystals_ganon,
                   &g_rando_effective_crystals_tower);
  Logic_SetResolvedTowerCrystals((uint8)(g_rando_effective_crystals_tower + 1));
  g_rando_active_share_string[0] = '\0';
  (void)Share_EncodeRaw(share_string_raw, g_rando_active_share_string,
                        (int)sizeof(g_rando_active_share_string));
  // Inverted runtime installs (mirror Rando_ActivateSidecarSlot). Each is a
  // no-op unless world_state == Inverted; InvertedEntrances_Install self-tears-
  // down its prior overlay (leading Teardown), and on a true cold replay nothing
  // else owns g_asset_ptrs[126], so the stack stays LIFO-clean.
  InvertedEntrances_Install(g_rando_active_world_state);
  InvertedSecrets_Install(g_rando_active_world_state);
  InvertedHoleBlocks_Install(g_rando_active_world_state);
  g_rando_active_settings_valid = true;
  g_rando_settings_from_cold_replay = true;  // mark the source.
  // JP-glitch coupling (mirror activation D6): a glitch-logic seed forces the
  // JP-glitch runtime flag so the replayed frames reproduce the assumed glitches.
  Rando_ApplyActiveForcedFeatures0();
  return true;
}

void Rando_ClearSnapshotDoorReplayRestore(void) {
  rando_clear_door_layout_runtime();
  g_rando_active_door_logic = false;
}

void Rando_ClearSnapshotReplayHeader(void) {
  g_rando_active_header_valid = false;
  g_rando_active_door_logic = false;
  rando_clear_chains_layout_runtime();
  g_rando_active_share_string[0] = '\0';
}

void Rando_ClearSnapshotChainsReplayRestore(void) {
  rando_clear_chains_layout_runtime();
}

bool Rando_SnapshotDoorReplayRestore(const RandoSettings *s,
                                     const uint8 *share_string_raw,
                                     uint8 door_attempt,
                                     uint32 door_digest24) {
  Rando_ClearDeferredPotConfirmation();
  Entrance_RuntimeTeardown();
  if (!rando_prepare_door_layout(s, share_string_raw, door_attempt, door_digest24,
                                 "snapshot replay")) {
    Rando_DeactivateSlot();
    return false;
  }
  if (!rando_install_prepared_door_layout("snapshot replay")) {
    Rando_DeactivateSlot();
    return false;
  }

  memset(&g_rando_active_header, 0, sizeof(g_rando_active_header));
  memcpy(g_rando_active_header.share_string, share_string_raw, 32);
  g_rando_active_header.door_attempt = door_attempt;
  g_rando_active_header.door_digest24 = door_digest24 & 0xFFFFFFu;
  g_rando_active_header_valid = true;
  g_rando_active_door_logic = true;
  g_reachability_state_counter++;
  return true;
}

bool Rando_SnapshotChainsReplayRestore(const RandoSettings *s,
                                       const uint8 *share_string_raw,
                                       uint8 chains_attempt,
                                       uint32 chains_digest24) {
  Rando_ClearDeferredPotConfirmation();
  Entrance_RuntimeTeardown();
  rando_clear_door_layout_runtime();
  g_rando_active_door_logic = false;
  rando_clear_chains_layout_runtime();
  if (!rando_prepare_chains_layout(s, share_string_raw, chains_attempt,
                                   chains_digest24, "snapshot replay")) {
    Rando_DeactivateSlot();
    return false;
  }
  if (!rando_install_prepared_chains_layout("snapshot replay")) {
    Rando_DeactivateSlot();
    return false;
  }

  memset(&g_rando_active_header, 0, sizeof(g_rando_active_header));
  memcpy(g_rando_active_header.share_string, share_string_raw, 32);
  g_rando_active_header.chains_present = 1;
  g_rando_active_header.chains_attempt = chains_attempt;
  g_rando_active_header.chains_digest24 = chains_digest24 & 0xFFFFFFu;
  g_rando_active_header_valid = true;
  g_reachability_state_counter++;
  return true;
}

bool Rando_SettingsHaveEntranceShuffle(const RandoSettings *s) {
  if (s == NULL) return false;
  // The same mode predicates Entrance_ComputeLayout evaluates: if any is
  // active, Entrance_RuntimeInstall installs a permutation for this seed.
  return Entrance_IsCrossActive(s) || Entrance_IsActive(s) ||
         Entrance_IsDungeonActive(s) || Entrance_IsDecoupledActive(s) ||
         Entrance_IsDungeonDecoupledActive(s) ||
         Entrance_IsCrossDecoupledActive(s);
}

// Entrance analogue of Rando_SnapshotDoorReplayRestore. Rebuild the
// slot-header fields Entrance_ComputeLayout reads (axes @71, attempt @72,
// world_state, share seed) from the snapshot-carried identity, verify the
// regenerated layout against the persisted digest24 (the same FIX #4 gate
// Rando_ActivateSidecarSlot applies — a drifted permutation can make the
// certified-beatable placement unbeatable), then install the door overlay +
// logic overrides. digest24 == 0 is a legacy pre-digest slot: install with
// Entrance_RuntimeInstall's warn-only version-drift path, matching
// activation. Chains and door shuffle are settings-excluded from entrance
// axes, so overwriting the replay header here can't drop their fields.
bool Rando_SnapshotEntranceReplayRestore(const RandoSettings *s,
                                         const uint8 *share_string_raw,
                                         uint8 entrance_axes,
                                         uint8 entrance_attempt,
                                         uint32 entrance_digest24) {
  if (s == NULL || share_string_raw == NULL) return false;
  Rando_ClearDeferredPotConfirmation();
  // Warm same-slot replay: the activation header (with its full registry
  // stamps / goal / prize fields) is still live and already describes this
  // exact entrance layout, and the overlay was never torn down — keep both
  // instead of reinstalling and rebuilding the sparse replay-only header.
  if (g_rando_active_header_valid &&
      !memcmp(g_rando_active_header.share_string, share_string_raw, 32) &&
      g_rando_active_header.entrance_axes == entrance_axes &&
      g_rando_active_header.entrance_attempt == entrance_attempt &&
      (g_rando_active_header.entrance_digest24 & 0xFFFFFFu) ==
          (entrance_digest24 & 0xFFFFFFu)) {
    return true;
  }
  RandoSlotHeader synth;
  memset(&synth, 0, sizeof(synth));
  memcpy(synth.share_string, share_string_raw, 32);
  synth.generator_version = Rando_GetSnapshotGeneratorVersion();
  synth.entrance_axes = entrance_axes;
  synth.entrance_attempt = entrance_attempt;
  synth.entrance_digest24 = entrance_digest24 & 0xFFFFFFu;
  synth.settings_ext_present = 1;
  synth.world_state = s->world_state;
  if (synth.entrance_digest24 != 0) {
    uint32 edigest = Rando_EntranceLayoutDigest24(&synth);
    if (edigest != synth.entrance_digest24) {
      fprintf(stderr,
              "Rando: entrance-shuffle layout drift on snapshot replay "
              "(regen digest %06x != snapshot %06x) — deactivating "
              "randomizer state\n",
              (unsigned)edigest, (unsigned)synth.entrance_digest24);
      Rando_DeactivateSlot();
      return false;
    }
  }
  Entrance_RuntimeInstall(&synth);
  memset(&g_rando_active_header, 0, sizeof(g_rando_active_header));
  memcpy(g_rando_active_header.share_string, share_string_raw, 32);
  g_rando_active_header.generator_version = synth.generator_version;
  g_rando_active_header.entrance_axes = entrance_axes;
  g_rando_active_header.entrance_attempt = entrance_attempt;
  g_rando_active_header.entrance_digest24 = synth.entrance_digest24;
  // world_state rides the header so Rando_ReinstallActiveSlotLogicOverlays
  // recomputes the SAME layout (Entrance_ComputeLayout keys Inverted/Retro
  // guards off it).
  g_rando_active_header.settings_ext_present = 1;
  g_rando_active_header.world_state = synth.world_state;
  g_rando_active_header_valid = true;
  g_reachability_state_counter++;
  return true;
}

static void rando_clear_snapshot_settings_replay_restore(void) {
  Rando_ClearDeferredPotConfirmation();
  g_rando_active_settings_valid = false;
  g_rando_active_seed_u64 = 0;
  Logic_SetResolvedTowerCrystals(0);
  g_rando_settings_from_cold_replay = false;
  g_rando_active_world_state = kWorldState_Open;
  g_rando_active_door_logic = false;
  g_rando_active_share_string[0] = '\0';
  Rando_SetDungeonPrizeAssignment(NULL);
  Rando_SetMedallionAssignment(NULL);
  BossShuffle_Deactivate();
  Rando_SetBossAssignment(NULL);
  DropShuffle_Deactivate();
  EnemyShuffle_Deactivate();
  InvertedEntrances_Teardown();
  InvertedSecrets_Teardown();
  InvertedHoleBlocks_Teardown();
  rando_clear_chains_layout_runtime();
  Rando_ApplyActiveForcedFeatures0();
  g_reachability_state_counter++;
}

void Rando_ClearSnapshotColdReplayRestore(void) {
  if (!g_rando_settings_from_cold_replay) return;
  rando_clear_snapshot_settings_replay_restore();
}

void Rando_ClearSnapshotSettingsReplayRestore(void) {
  rando_clear_snapshot_settings_replay_restore();
}

void Rando_ActivateSidecarSlot(const RandoSidecarSlot *src) {
  if (src == NULL || src->header.slot_kind != kSlotKind_Randomizer) {
    Rando_DeactivateSlot();
    return;
  }
  if (!Alttpr_ValidateSlot(src)) {
    fprintf(stderr, "Rando: imported rules and placement table differ; refusing slot\n");
    Rando_DeactivateSlot(); return;
  }
  if (!RandoChestMap_Ready() && kRandoChestLookup_COUNT == 0) {
    fprintf(stderr, "Rando: local chest identities are unavailable; refusing activation\n");
    Rando_DeactivateSlot();
    return;
  }
  rando_clear_trap_effect();
  Rando_ClearDeferredPotConfirmation();
  RandoSettings slot_settings;
  bool slot_settings_valid = false;
  // FIX #5 — refuse a slot whose canonical settings blob fails range
  // validation (Settings_CanonicalDeserialize now rejects out-of-range enum
  // bytes via Settings_Validate; undefined FLAG bits stay permissive). The
  // blob drives the door-shuffle gate, the prize/medallion regen, and the
  // tracker reachability below — a corrupt enum there would either flow into
  // `1u << world_state`-style consumers or silently skip the door drift gate.
  // Same refusal pathway as the digest-drift checks: deactivate, don't guess.
  if (src->header.settings_present) {
    if (Settings_CanonicalDeserialize(src->settings_canonical, &slot_settings) != 0) {
      fprintf(stderr,
              "Rando: slot settings blob failed range validation (corrupt sidecar?) "
              "— refusing to activate this slot\n");
      Rando_DeactivateSlot();
      return;
    }
    slot_settings_valid = true;
    char placement_preflight_err[192];
    if (!Placement_PreflightSettings(
            &slot_settings, placement_preflight_err, sizeof(placement_preflight_err))) {
      fprintf(stderr,
              "Rando: randomizer slot settings are not supported by this build "
              "(%s) — refusing to activate this slot\n",
              placement_preflight_err[0] != '\0' ? placement_preflight_err : "preflight failed");
      Rando_DeactivateSlot();
      return;
    }
    if (Settings_EnemyDropKeysActive(&slot_settings) &&
        Rando_DetectVersionDrift(&src->header, (uint16)kGeneratorVersion)) {
      fprintf(stderr,
              "Rando: enemy-drop-check slot was generated by version %u but this "
              "build is version %u; enemy check location ids are table-derived, "
              "so this slot must be regenerated before loading here\n",
              (unsigned)src->header.generator_version,
              (unsigned)kGeneratorVersion);
      Rando_DeactivateSlot();
      return;
    }
  }
  if (slot_settings_valid && Rando_SettingsNeedPotRegistry(&slot_settings)) {
    if (!src->header.pot_registry_present ||
        !Rando_PotRegistryMatches(src->header.pot_registry_digest,
                                  src->header.pot_registry_count)) {
      fprintf(stderr,
              "Rando: pot-shuffle registry drift or missing registry identity "
              "(slot count=%u digest=%08x, build count=%u digest=%08x) "
              "— refusing to activate this slot on this build\n",
              (unsigned)src->header.pot_registry_count,
              (unsigned)src->header.pot_registry_digest,
              (unsigned)Rando_CurrentPotRegistryCount(),
              (unsigned)Rando_CurrentPotRegistryDigest());
      Rando_DeactivateSlot();
      return;
    }
  }
  // add-rando-grass-rock-shuffle — same guard shape for the terrain registry:
  // a grass/rock-enabled slot refuses to activate on a binary whose generated
  // terrain registry is absent/empty or renumbered (placement location ids
  // would rebind), never silently resolving terrain checks to vanilla drops.
  if (slot_settings_valid && Rando_SettingsNeedTerrainRegistry(&slot_settings)) {
    // A build with an EMPTY terrain registry (count 0) can never honor a
    // terrain-active slot; reject explicitly so a slot that stored (0,0) —
    // which the generation fail-closed now prevents, but defense-in-depth —
    // does not fail-open through the (0,0)==(0,0) match (review HIGH-1).
    if (!src->header.terrain_registry_present ||
        Rando_CurrentTerrainRegistryCount() == 0 ||
        !Rando_TerrainRegistryMatches(src->header.terrain_registry_digest,
                                      src->header.terrain_registry_count)) {
      fprintf(stderr,
              "Rando: grass/rock terrain registry drift or missing registry "
              "identity (slot count=%u digest=%08x, build count=%u "
              "digest=%08x) — refusing to activate this slot on this build\n",
              (unsigned)src->header.terrain_registry_count,
              (unsigned)src->header.terrain_registry_digest,
              (unsigned)Rando_CurrentTerrainRegistryCount(),
              (unsigned)Rando_CurrentTerrainRegistryDigest());
      Rando_DeactivateSlot();
      return;
    }
  }
  if (slot_settings_valid && Rando_SettingsNeedBonkRegistry(&slot_settings)) {
    // Bonk registry guard (add-rando-bonk-sanity), same shape as terrain:
    // count 0 rejects explicitly (an empty-registry build can never honor a
    // bonk-active slot) and pre-v11 sidecars read present=0 -> refuse.
    if (!src->header.bonk_registry_present ||
        Rando_CurrentBonkRegistryCount() == 0 ||
        !Rando_BonkRegistryMatches(src->header.bonk_registry_digest,
                                   src->header.bonk_registry_count)) {
      fprintf(stderr,
              "Rando: bonk registry drift or missing registry identity "
              "(slot count=%u digest=%08x, build count=%u digest=%08x) — "
              "refusing to activate this slot on this build\n",
              (unsigned)src->header.bonk_registry_count,
              (unsigned)src->header.bonk_registry_digest,
              (unsigned)Rando_CurrentBonkRegistryCount(),
              (unsigned)Rando_CurrentBonkRegistryDigest());
      Rando_DeactivateSlot();
      return;
    }
  }
    // Enemy-check registry guard, same shape as pot/terrain above: enemy-check
  // location ids are table-derived from local artifacts, so a dungeon/all-tier
  // slot must prove this binary carries the SAME registry — the version-drift
  // refusal earlier misses same-generator-version drift. present == 0 (a
  // pre-v9 sidecar) fails CLOSED: enemy-check slots predate the identity
  // field, and silently activating one against a drifted registry is exactly
  // the fail-open this closes (regenerate the seed on this build). An EMPTY
  // build registry (count 0) can never honor such a slot either.
  if (slot_settings_valid &&
      Rando_SettingsNeedEnemyCheckRegistry(&slot_settings)) {
    if (!src->header.enemy_check_registry_present ||
        Rando_CurrentEnemyCheckRegistryCount() == 0 ||
        !Rando_EnemyCheckRegistryMatches(
            src->header.enemy_check_registry_digest,
            src->header.enemy_check_registry_count)) {
      fprintf(stderr,
              "Rando: enemy-check registry drift or missing registry identity "
              "(slot count=%u digest=%08x, build count=%u digest=%08x) "
              "— refusing to activate this slot on this build\n",
              (unsigned)src->header.enemy_check_registry_count,
              (unsigned)src->header.enemy_check_registry_digest,
              (unsigned)Rando_CurrentEnemyCheckRegistryCount(),
              (unsigned)Rando_CurrentEnemyCheckRegistryDigest());
      Rando_DeactivateSlot();
      return;
    }
  }
  // add-rando-door-shuffle — regenerate the door layout BEFORE installing any
  // slot state. The layout is not serialized; it regenerates from
  // (seed, settings, door_attempt @76). Drift HARD-FAILS: a regenerated
  // layout whose digest differs from the persisted @77-79 value can make the
  // certified-beatable placement unbeatable, so the slot is refused (treated
  // as no-rando) rather than silently loaded — unlike entrance shuffle's
  // non-blocking version-drift warning. Vanilla-door slots skip all of this.
  bool door_active = false;
  if (slot_settings_valid &&
      Settings_EffectiveDoorShuffle(&slot_settings) != kDoorShuffle_Vanilla) {
    if (!rando_prepare_door_layout(&slot_settings, src->header.share_string,
                                   src->header.door_attempt,
                                   src->header.door_digest24,
                                   "slot activation")) {
      Rando_DeactivateSlot();
      return;
    }
    door_active = true;
  }
  bool chains_active = false;
  if (slot_settings_valid && Settings_EffectiveDungeonChains(&slot_settings)) {
    if (!src->header.chains_present) {
      fprintf(stderr,
              "Rando: dungeon-chain slot missing layout identity "
              "- refusing to activate this slot on this build\n");
      Rando_DeactivateSlot();
      return;
    }
    if (!rando_prepare_chains_layout(&slot_settings, src->header.share_string,
                                     src->header.chains_attempt,
                                     src->header.chains_digest24,
                                     "slot activation")) {
      Rando_DeactivateSlot();
      return;
    }
    chains_active = true;
  }
  // FIX #4 — entrance-layout drift gate, mirroring the door-shuffle digest
  // gate above. The entrance permutation is REGENERATED from (seed, axes,
  // attempt) at install; if a kGeneratorVersion bump changed the entrance
  // algorithm or pool, the regenerated layout silently differs from the one
  // the placement was certified against (doors that don't match the baked
  // placement can make a certified-beatable seed unbeatable). Recompute the
  // digest from the header and refuse on mismatch. entrance_digest24 == 0 =
  // legacy slot (pre-digest sidecar) or no entrance shuffle — those keep the
  // warn-only version-drift behavior inside Entrance_RuntimeInstall.
  if (src->header.entrance_digest24 != 0) {
    uint32 edigest = Rando_EntranceLayoutDigest24(&src->header);
    if (edigest != src->header.entrance_digest24) {
      fprintf(stderr,
              "Rando: entrance-shuffle layout drift (regen digest %06x != slot %06x) "
              "— refusing to activate this slot on this build\n",
              (unsigned)edigest, (unsigned)src->header.entrance_digest24);
      Rando_DeactivateSlot();
      return;
    }
  }
  uint16 n = src->placement_count;
  if (n > kRando_SessionPlacementCapacity) n = kRando_SessionPlacementCapacity;
  memcpy(g_session_placements, src->placements, (size_t)n * sizeof(RandoPlacement));
  g_session_placement_table.entries = g_session_placements;
  g_session_placement_table.count = n;
  Placement_Install(&g_session_placement_table);
  g_rando_slot_active = 1;
  // File activation starts a new gameplay session. An ending armed in the
  // previous file must never carry into a copied/fresh file. Snapshot restore
  // uses its own cold-replay path and retains the snapshot-owned handoff byte.
  Rando_ResetGoalRuntime();
  // capture the replay inputs for
  // Rando_ReinstallActiveSlotLogicOverlays BEFORE the entrance/door installs
  // below, so the helper replays exactly the header those installs read. A
  // refusal below routes through Rando_DeactivateSlot, which invalidates this.
  g_rando_active_header = src->header;
  g_rando_active_header_valid = true;
  g_wanted_zelda_features1 |= kFeatures1_RandomizerActive;
  enhanced_features1 |= kFeatures1_RandomizerActive;

  // randomizer-save "Embedded placement table — upgrade safety": a slot whose
  // generator_version differs from this build still loads (the embedded
  // placement table is authoritative), but the spec's "Version drift loads with
  // warning" scenario requires a one-time informational warning. Door- and
  // entrance-shuffle slots are handled separately above — their layout drift
  // hard-fails in the digest gates, and legacy (digest-0) entrance slots warn
  // inside Entrance_RuntimeInstall — so a PLAIN slot (no door/entrance shuffle)
  // is the only path that otherwise activates silently. Warn on stderr, matching
  // the door/entrance drift-warning precedent in this function.
  if (!door_active && src->header.entrance_axes == 0 &&
      Rando_DetectVersionDrift(&src->header, (uint16)kGeneratorVersion)) {
    fprintf(stderr,
        "Rando WARNING: this randomizer slot was generated by version %u but "
        "this build is version %u. Its embedded placement loads as-is; "
        "regenerate the seed if you want this build's latest logic.\n",
        (unsigned)src->header.generator_version, (unsigned)kGeneratorVersion);
  }

  // Wire the snapshot-tail TLV emitter (per rando_save.h "UI also calls
  // Rando_SetSnapshotContext"). Without this, `RandoSnapshotTail_Save`
  // early-returns at `!g_has_ctx` and the snapshot file has no rando TLV;
  // a later Ctrl+F1 then restores `g_ram`'s rando-active bit but leaves
  // `g_active_placement == NULL` (placement table lives in the heap, not
  // g_ram), so the next chest dispatch falls back to vanilla items.
  Rando_SetSnapshotContext(src->header.generator_version,
                           src->header.settings_hash,
                           src->header.share_string);
  // also capture the canonical settings blob + prize_attempt so a COLD
  // snapshot replay (Ctrl+F1 on a fresh launch with the slot not loaded) can
  // reconstruct world_state + the prize/medallion/boss/drop/enemy assignments +
  // Inverted installs via Rando_SnapshotColdReplayRestore. NULL (a v1/no-blob
  // slot) suppresses the type-2 TLV, so cold replay degrades to placement-only.
  Rando_SetSnapshotSettingsContext(
      src->header.settings_present ? src->settings_canonical : NULL,
      src->header.prize_attempt, src->header.ow_attempt,
      src->header.ow_digest24);
  Rando_SetSnapshotDoorContext(src->header.door_attempt,
                               src->header.door_digest24,
                               door_active);
  Rando_SetSnapshotChainsContext(src->header.chains_attempt,
                                 src->header.chains_digest24,
                                 chains_active);
  // Entrance-shuffle layout identity for the type-9 TLV, so a cold replay can
  // regenerate + digest-verify + reinstall the permutation the way door/chain
  // snapshots do. entrance_digest24 may be 0 on a legacy pre-digest slot —
  // still emitted; replay then takes the warn-only install path.
  Rando_SetSnapshotEntranceContext(src->header.entrance_axes,
                                   src->header.entrance_attempt,
                                   src->header.entrance_digest24,
                                   src->header.entrance_axes != 0);
  Rando_SetSnapshotRecommendedFeaturesContext(
      src->header.recommended_features0,
      src->header.recommended_features0_present != 0);

  // Phase B Slice 1 — copy the slot's checked-location bitmap into the
  // session state. Bitmap size matches between slot and session
  // (both kRandoCheckedBitmapBytes, derived from kRandoLocationCapacity).
  memcpy(g_rando_checked_bitmap, src->checked_bitmap, kRandoCheckedBitmapBytes);
  g_rando_triforce_piece_count=Rando_CountCollectedTriforcePieces();
  g_rando_mushroom_held = src->header.mushroom_held;
  g_rando_flute_shovel_owned = src->header.flute_shovel_owned;
  g_rando_boomerang_owned = src->header.boomerang_owned;
  g_rando_bow_owned = src->header.bow_owned;
  // add-enemy-souls — restore soul ownership (pre-v6 slots load souls_present=0
  // → zero, the correct default: those seeds have souls off so nothing gates).
  Souls_ResetFlags();
  if (src->header.souls_present)
    memcpy(Souls_Flags(), src->header.soul_flags, sizeof(src->header.soul_flags));
  // tracker-player-knowledge — restore topology-discovery state (pre-v13
  // slots load zeros), then backfill dungeon/cave bits from the checked
  // bitmap copied above. The backfill is an idempotent OR, so running it on
  // v13 slots too self-heals checks recorded by an older binary.
  Rando_SetDiscoveryState(src->header.discovered_dungeons,
                          src->header.discovered_whirlpools,
                          src->header.discovered_caves,
                          src->header.discovered_exits);
  Rando_BackfillDiscoveryFromChecked();
  // Phase B Inverted runtime — capture the slot's world_state from the
  // additive @68 ext byte. Only trust it when settings_ext_present is set
  // (older slots wrote 0 there, which already maps to kWorldState_Open).
  g_rando_active_world_state = src->header.settings_ext_present
                                   ? src->header.world_state
                                   : (uint8)kWorldState_Open;
  // tear down any PRIOR slot's Inverted override BEFORE the
  // entrance overlay installs over g_asset_ptrs[126]. Without this, switching
  // straight from an Inverted slot to an entrance-shuffle slot would let
  // Entrance_RuntimeInstall run while the Inverted shadow still owns 126, and
  // this slot's InvertedEntrances_Install below (whose leading Teardown
  // restores 126) would then yank the freshly-installed overlay back out from
  // under the shuffle. Keeps the 126 save/restore stack strictly LIFO; the
  // digest gate above is contamination-proof independently via
  // Entrance_PristineVanillaIds.
  InvertedEntrances_Teardown();
  // dungeon-chains also owns asset 126. Clear a prior chain slot before the
  // normal entrance path self-tears down and before this slot installs its
  // regenerated chain overlay.
  rando_clear_chains_layout_runtime();
  // Phase C — install the entrance-shuffle door overlay + region overrides for
  // this slot (no-op when the slot carries no cave shuffle). Done before the
  // hint regeneration below so hints see the shuffled reachability, and before
  // the tracker repaint counter bump.
  Entrance_RuntimeInstall(&src->header);

  // add-rando-door-shuffle — install the drift-checked regenerated layout:
  // logic oracle + the runtime redirect table. (Mutually exclusive with
  // entrance shuffle per apply_derived_rules, so the two installs never
  // contend.) The Stage-1b door-KIND overlay installs here too once built.
  if (door_active) {
    if (!rando_install_prepared_door_layout("slot activation")) {
      Rando_DeactivateSlot();
      return;
    }
    g_rando_active_door_logic = true;  // replay-input capture
  } else {
    rando_clear_door_layout_runtime();
    g_rando_active_door_logic = false;  // replay-input capture
  }
  if (chains_active) {
    if (!rando_install_prepared_chains_layout("slot activation")) {
      Rando_DeactivateSlot();
      return;
    }
  } else {
    g_rando_active_chains_logic = false;
  }

  // #82 Inverted world-state — repoint the static Inverted entrance/exit
  // overrides (Link's House<->Bomb Shop, GT<->AT). No-op unless the slot is
  // Inverted. Runs AFTER Entrance_RuntimeInstall, which is itself a no-op on an
  // Inverted slot (the shuffle bails for non-Open/Standard), so the two never
  // contend for g_asset_ptrs[126].
  InvertedEntrances_Install(g_rando_active_world_state);
  // Inverted DW->LW under-rock warps (overworld-secret type-0x82). Separate
  // asset set (157/158) from the entrance overrides, so order is independent.
  InvertedSecrets_Install(g_rando_active_world_state);
  // Inverted hole-only Ganon relocation: append the no-art pit blocks to a
  // kMap16ToMap8 (asset 70) shadow. Separate asset, order-independent.
  InvertedHoleBlocks_Install(g_rando_active_world_state);

  // Cosmetic shuffles: when CosmeticSeed is 0 (default), the look tracks the
  // slot's seed_u64 (share_string bytes [21..28] LE). Re-seeds palette + music
  // tables; the sprite pick already happened at launch (documented limitation).
  Cosmetic_SetSeed(g_config.cosmetic_seed,
                   SlotSeedFromShareString(src->header.share_string));

  // Force the tracker to repaint after activation.
  g_reachability_state_counter++;

  // §62 — cache the active slot's textual share string for the in-binary
  // reveal action. Race-mode slots have a suppressed spoiler at
  // <spoiler_dir>/<share_string>.json that the reveal action regenerates.
  // Non-race slots populate the cache anyway (harmless; reveal will fall
  // through to FileNotFound when there's no ZRSR file).
  g_rando_active_share_string[0] = '\0';
  (void)Share_EncodeRaw(src->header.share_string, g_rando_active_share_string,
                        (int)sizeof(g_rando_active_share_string));
  // Price/derivation seed — from the raw header share string, present on
  // every slot vintage (unlike the canonical settings blob).
  g_rando_active_seed_u64 = SlotSeedFromShareString(src->header.share_string);

  // === Reachability settings + shuffle assignments (tracker engine) ===
  // The runtime reachability engine (Logic_ComputeReachabilityFullKnowledge, consumed by the
  // Check/Map tracker windows) needs the seed's FULL settings plus the prize /
  // medallion shuffle assignments. Nothing else installs the assignments at
  // reload — the placer set them at generation time (rando_placement.c) and that
  // doesn't re-run here. Recover the canonical settings blob (format_version >=
  // 2) and recompute the assignments from (settings, seed) in the EXACT placer
  // order (Rng_SeedFromU64 → PrizeShuffle_Run → MedallionShuffle_Run on one
  // shared rng; medallion output is stream-position-dependent on prize_shuffle).
  // When the blob is absent, mark settings invalid and clear the assignments so
  // the tracker layer suppresses reachability instead of guessing.
  g_rando_active_settings_valid = false;
  if (src->header.settings_present &&
      Settings_CanonicalDeserialize(src->settings_canonical, &g_rando_active_settings) == 0) {
    ShareString ss;
    if (Share_Decode(g_rando_active_share_string, &ss) == kShareDecodeOk) {
      // Re-derive + install prize/medallion/boss/drop/enemy assignments from the
      // recovered (settings, seed, prize_attempt). Factored into a shared helper
      // (above) so the snapshot cold-replay restore re-derives identically — see
      // the helper comment for the FIX #6 / falling-prize rationale.
      if (!install_active_shuffles(&g_rando_active_settings, ss.seed_u64,
                                   src->header.prize_attempt,
                                   src->header.ow_attempt,
                                   src->header.ow_digest24)) {
        // Warp layout drift/absence: refuse the whole slot (the placement
        // was certified against a layout this build cannot reproduce).
        // Activation is void — deactivate + return is the refusal idiom.
        Rando_DeactivateSlot();
        return;
      }
      // add-rando-random-crystals — resolve + cache the effective counts
      // HERE, inside the block where both the canonical deserialize AND the
      // share decode succeeded ("valid settings but no seed" cannot occur).
      // Every runtime consumer reads the cached getters; the values are
      // seed-pure, so activation/deactivation are the only cache events.
      Crystals_Resolve(&g_rando_active_settings, ss.seed_u64,
                       &g_rando_effective_crystals_ganon,
                       &g_rando_effective_crystals_tower);
      Logic_SetResolvedTowerCrystals(
          (uint8)(g_rando_effective_crystals_tower + 1));
      g_rando_active_settings_valid = true;
      g_rando_settings_from_cold_replay = false;  // a GENUINE slot activation.
    }
  }
  if (!g_rando_active_settings_valid) {
    // add-rando-random-crystals — fail-closed defaults (vanilla-equivalent 7/7).
    g_rando_effective_crystals_ganon = 7;
    g_rando_effective_crystals_tower = 7;
    Rando_SetDungeonPrizeAssignment(NULL);
    Rando_SetMedallionAssignment(NULL);
    // No trustworthy (settings, seed) — do NOT guess a boss/drop shuffle.
    // Tear down any assignment a prior slot installed so it can't leak into
    // this (v1 / snapshot-restored) slot. Fail closed = vanilla bosses/drops.
    BossShuffle_Deactivate();
    Rando_SetBossAssignment(NULL);  // logic VM falls back to vanilla boss-kill
    DropShuffle_Deactivate();
    // add-rando-enemy-shuffle — fail closed: tear down any prior slot's enemy
    // substitution so it can't leak into this (v1 / snapshot-restored) slot.
    EnemyShuffle_Deactivate();
  }

  // Placement + checked bitmap are now installed, and canonical settings/seed
  // recovery (when present) is complete. Rebuild derived Key Ring/Skeleton Key
  // state only here; doing it immediately after Placement_Install would run
  // before the checked bitmap copy above.
  Rando_RebuildKeyItemOwnership();

  // Seed QoL gameplay features are per-slot play preferences, not canonical
  // randomizer settings. format_version-3 slots written by builds that know this
  // field carry the snapshot the user generated with; older slots leave the
  // user's current live config untouched. Apply after the remaining refusal
  // paths above so a rejected slot cannot mutate live preferences.
  if (src->header.recommended_features0_present) {
    Rando_ApplySeedQolFeatures0(src->header.recommended_features0);
  }

  // add-rando-major-glitch D6 — couple a glitch-logic slot to the JP-1.0
  // glitch runtime flag. AUTHORITATIVE runtime guarantee: runs on EVERY slot
  // activation (generate->play AND reload->play, incl. imported share strings),
  // unlike the generate-time recommend path. When recovered settings show the
  // placement assumed a restored glitch (logic>=OverworldGlitches or the
  // fake-flippers trick), apply it as a runtime-only overlay in
  // g_wanted_zelda_features (survives the per-frame mirror + mid-session
  // Config_ApplyLive) and enhanced_features0 (this frame). g_config remains the
  // user's preference; the point-of-use gate JpGlitchEnabled() still
  // self-suppresses under side-by-side (!ZeldaIsEmulatorAttached()), so this
  // stays RAM-compare-safe. features0 is not canonical settings, so
  // placement/corpus remain byte-identical.
  Rando_ApplyActiveForcedFeatures0();

  // Persist the swordless flag in g_ram so a StateRecorder snapshot captures it
  // (g_ram is restored verbatim by LoadSnesState on replay/Ctrl+F1 restore, but
  // the C-static g_rando_active_settings is NOT). Rando_IsSwordlessActive falls
  // back to this byte when post-restore settings are NULL, keeping the runtime
  // swordless patches (hammer-Ganon/Agahnim, medallion/tablet/curtain) firing.
  // Authoritative against the recovered settings: 0 unless this slot is known
  // swordless. Cleared in Rando_DeactivateSlot. Rando-gated — vanilla never
  // touches this byte.
  g_rando_swordless = (g_rando_active_settings_valid &&
                       g_rando_active_settings.mode_weapons == kModeWeapons_Swordless)
                          ? 1 : 0;

  // === Hints v2: strict identity reconstruction after all overlays ===
  // The sidecar carries only algorithm/schema/digest/discovery. Import that
  // expectation and rebuild from the accepted canonical settings, placement,
  // and the fully installed shuffle/layout state above. Missing (pre-v14),
  // unsupported, or mismatched metadata disables hints only; it never blesses
  // a current-algorithm reroll for an old slot.
  Rando_HintsImportPersistedState(&src->header.hint_state);
  Rando_RegenerateActiveSlotHints();
  // === Hints v2: end ===
}

// Rebuild the certified hint plan for the CURRENTLY-ACTIVE slot. The expected
// identity must already have been imported from its sidecar/snapshot. This
// never creates a fresh identity and therefore cannot silently upgrade legacy
// slots or replace a mismatched plan.
void Rando_RegenerateActiveSlotHints(void) {
  const RandoPlacementTable *pt = Placement_GetActive();
  if (!g_rando_slot_active || pt == NULL || pt->entries == NULL) {
    Rando_ClearHints();
    return;
  }
  (void)Rando_RebuildHints(Rando_GetActiveSettings(), pt, NULL);
}

// reinstall the ACTIVE slot's LOGIC-side overlays after an
// out-of-band generation cleared them. Rando_GenerateSlot unconditionally
// clears the entrance/chain region+edge override stores (Rando_PlaceWithEntrances'
// leading clears) and the door logic layout (Rando_ClearGenerationLogicOverlays
// -> Rando_SetDoorLogicLayout(NULL, 0)) - the same global stores
// Rando_ActivateSidecarSlot populated for the active slot — and never
// reinstalls, so generating a seed mid-session WITHOUT loading it reverted
// tracker/map LOGIC reachability to the vanilla graph until slot reload.
//
// This replays EXACTLY the activation install sub-steps from the captured
// active-slot header:
//   - entrance: Entrance_ComputeLayout over the header — the identical
//     (seed, entrance_axes, entrance_attempt) regeneration (and the same
//     pristine-table resolution, Entrance_PristineVanillaIds) that
//     Entrance_RuntimeInstall ran at activation — then the shared
//     Entrance_ApplyLogicOverrides sub-step;
//   - door: DoorShuffle_Generate from (seed, door_attempt) — identical inputs
//     to activation — re-installed via Rando_SetDoorLogicLayout. The digest
//     refusal re-check is deliberately SKIPPED: activation already validated
//     this exact regenerated layout against the slot's door_digest24 (drift
//     hard-fails there), and DoorShuffle_Generate is deterministic, so the
//     layout regenerated here is byte-identical to the validated one.
//   - chains: Chains_Compute from (seed, chains_attempt), revalidated against
//     chains_digest24, then re-applied to the shared entrance edge override
//     store. Gameplay runtime redirects stay installed unless validation fails.
// Gameplay-side installs (the asset-126 entrance overlay, decoupled runtime
// nets, DoorRt redirects, chain seam redirects, and the boss/drop/enemy RENDER
// shuffles - generation
// calls only the pure BossShuffle_/DropShuffle_ComputeAssignment forms and
// never EnemyShuffle_*) are NOT touched by generation, so this helper leaves
// them alone — it is logic-side only. No active slot: leave the stores CLEARED
// (the correct idle state, matching what generation left) and return.
void Rando_ReinstallActiveSlotLogicOverlays(void) {
  // generation also replaces the logic-VM shuffle assignments
  // with the placer's per-run data (Place_AssumedFill installs boss plus
  // prize/medallion assignment bytes), i.e. the NEW seed's data.
  // Not tracker-only: RandoFallingPrizeIndex (dungeon.c) picks the falling
  // boss prize and the ancilla.c medallion casts gate MM/TR off these. The
  // active arrays (g_rando_active_{prize,medallion,boss}_assignment) are
  // file-statics that ONLY activation writes — generation never touches their
  // bytes — so copying them back into the assignment stores is sufficient;
  // mirror activation's install arm (condition: the settings block succeeded)
  // or its fail-closed NULL arm.
  if (g_rando_active_settings_valid) {
    Rando_SetDungeonPrizeAssignment(g_rando_active_prize_assignment);
    Rando_SetMedallionAssignment(g_rando_active_medallion_assignment);
    Rando_SetBossAssignment(g_rando_active_boss_assignment);
  } else {
    Rando_SetDungeonPrizeAssignment(NULL);
    Rando_SetMedallionAssignment(NULL);
    Rando_SetBossAssignment(NULL);
  }
  if (!g_rando_active_header_valid) {
    Entrance_ClearRegionOverrides();
    Entrance_ClearEdgeOverrides();
    Rando_SetDoorLogicLayout(NULL, 0);
    rando_clear_chains_layout_runtime();
    return;
  }
  EntranceRuntimeLayout *lay = &s_entrance_layout_scratch;
  if (Entrance_ComputeLayout(&g_rando_active_header, lay)) {
    Entrance_ApplyLogicOverrides(lay);
  } else {
    // Non-entrance slot: cleared stores ARE the activation-time logic state.
    Entrance_ClearRegionOverrides();
    Entrance_ClearEdgeOverrides();
  }
  if (g_rando_active_door_logic) {
    if (!g_rando_active_settings_valid) {
      fprintf(stderr,
              "Rando: active door layout reinstall missing settings — door logic cleared\n");
      Rando_SetDoorLogicLayout(NULL, 0);
      g_rando_active_door_logic = false;
      g_reachability_state_counter++;
      return;
    }
    uint64 slot_seed = SlotSeedFromShareString(g_rando_active_header.share_string);
    if (DoorShuffle_Generate(slot_seed, g_rando_active_header.door_attempt,
                             kDoorShuffle_MvpDungeonMask,
                             Settings_DoorPotTier(&g_rando_active_settings),
                             rando_door_enemy_drop_keys_for_settings(&g_rando_active_settings),
                             rando_door_enemy_check_tier_for_settings(&g_rando_active_settings),
                             &s_active_door_layout)) {
      uint32 digest24 = DoorShuffle_LayoutDigest(&s_active_door_layout) & 0xFFFFFFu;
      if (g_rando_active_header.door_digest24 != digest24) {
        fprintf(stderr,
                "Rando: active door layout reinstall digest mismatch "
                "(regen %06x != slot %06x) — door logic cleared\n",
                (unsigned)digest24, (unsigned)g_rando_active_header.door_digest24);
        Rando_SetDoorLogicLayout(NULL, 0);
        g_rando_active_door_logic = false;
        g_reachability_state_counter++;
        return;
      }
      Rando_SetDoorLogicLayout(&s_active_door_layout, s_active_door_layout.shuffled_mask);
    } else {
      // Unreachable for a validly-activated slot (the same deterministic
      // inputs generated at activation); fail closed rather than install a
      // half-written layout.
      fprintf(stderr,
              "Rando: door layout reinstall regeneration failed — door logic cleared\n");
      Rando_SetDoorLogicLayout(NULL, 0);
      g_rando_active_door_logic = false;
    }
  } else {
    Rando_SetDoorLogicLayout(NULL, 0);
  }
  if (g_rando_active_chains_logic) {
    if (!g_rando_active_settings_valid ||
        !Settings_EffectiveDungeonChains(&g_rando_active_settings) ||
        !g_rando_active_header.chains_present) {
      fprintf(stderr,
              "Rando: active dungeon-chain layout reinstall missing identity "
              "or settings - chain logic cleared\n");
      rando_clear_chains_layout_runtime();
      g_reachability_state_counter++;
      return;
    }
    if (!rando_prepare_chains_layout(&g_rando_active_settings,
                                     g_rando_active_header.share_string,
                                     g_rando_active_header.chains_attempt,
                                     g_rando_active_header.chains_digest24,
                                     "active layout reinstall")) {
      rando_clear_chains_layout_runtime();
      g_reachability_state_counter++;
      return;
    }
    Chains_ApplyEdgeOverrides(&s_active_chains_layout);
  }
  // add-rando-ow-warp-shuffle — replay the warp overlay edges (audit M1:
  // this function exists because generation clears the shared stores; the
  // active layout is process-static and needs no recompute, just
  // re-install — composing on the entrance/chains overlay, self-Begins
  // when warp is alone).
  if (g_rando_active_ow_warp)
    OwWarp_InstallLogicEdges(&g_rando_active_ow_layout);
  // Force a tracker recompute (mirrors activation): the stores round-tripped
  // through a cleared state, so don't trust any cached reachability.
  g_reachability_state_counter++;
}

void Rando_DeactivateSlot(void) {
  Rando_ResetGoalRuntime();
  Rando_ResetGoalNpc();
  // add-rando-random-crystals — reset the resolved-count cache to the
  // vanilla-equivalent 7/7 (implementation-review F4: the consumers all
  // guard on slot_active+settings_valid, but a stale cache masked the F1
  // cold-replay gap on warm processes; keep the invariant explicit).
  g_rando_effective_crystals_ganon = 7;
  g_rando_effective_crystals_tower = 7;
  // add-rando-ow-warp-shuffle — drop the active warp layout (its overlay
  // edges are cleared by the entrance teardown below, which owns the shared
  // override stores).
  rando_clear_ow_warp_runtime();
  // dungeon-chains owns the same overworld entrance-id asset as entrance
  // shuffle, so drop it before the generic entrance teardown clears the shared
  // logic override stores.
  rando_clear_chains_layout_runtime();
  // Phase C — restore the vanilla door table + clear entrance region overrides
  // before anything else (mirror of Entrance_RuntimeInstall in Activate).
  Entrance_RuntimeTeardown();
  // add-rando-door-shuffle — clear the per-seed door redirect + logic layout
  // (mirror of DoorShuffle_RuntimeInstall in Activate).
  rando_clear_door_layout_runtime();
  // #82 Inverted override teardown — reverse of the Activate install order.
  // Restores g_asset_ptrs[126/130/131] to their saved vanilla originals.
  InvertedEntrances_Teardown();
  InvertedSecrets_Teardown();  // restore g_asset_ptrs[157/158]
  InvertedHoleBlocks_Teardown();  // restore g_asset_ptrs[70]
  // Phase B Slice 7/8 — tear down the boss + drop shuffle so the sprite
  // substitution reverts to a hard passthrough (vanilla bosses/drops) once no
  // rando slot is active; pairs with the install in Rando_ActivateSidecarSlot.
  BossShuffle_Deactivate();
  Rando_SetBossAssignment(NULL);  // logic VM back to vanilla boss-kill fallback
  DropShuffle_Deactivate();
  EnemyShuffle_Deactivate();  // add-rando-enemy-shuffle — revert to vanilla enemies
  Placement_Install(NULL);
  g_session_placement_table.entries = NULL;
  g_session_placement_table.count = 0;
  g_rando_slot_active = 0;
  rando_clear_trap_effect();
  Rando_ClearDeferredPotConfirmation();
  g_wanted_zelda_features1 &= ~(uint32)kFeatures1_RandomizerActive;
  enhanced_features1 &= ~(uint32)kFeatures1_RandomizerActive;
  // Pair with the SetSnapshotContext in Activate — leaving stale metadata
  // would let a future snapshot save emit a TLV bound to the wrong slot.
  Rando_ClearSnapshotContext();

  // Phase B Slice 1 — clear the checked bitmap and reset tracker visibility
  // so the next slot launches with the documented "trackers default hidden"
  // contract.
  memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
  g_rando_mushroom_held = 0;
  g_rando_flute_shovel_owned = 0;
  g_rando_boomerang_owned = 0;
  g_rando_bow_owned = 0;
  // tracker-player-knowledge — per-slot state; the next slot re-loads its own
  // discovery from the sidecar/TLV (+ backfill).
  Rando_ResetDiscoveryState();
  g_rando_key_ring_selected_mask = 0;
  g_rando_key_ring_owned_mask = 0;
  g_rando_skeleton_key_present = false;
  g_rando_skeleton_key_owned = false;
  g_last_dispatched_item_id = 0xffffu;
  g_last_dispatched_location_id = 0xffffu;
  g_last_dispatched_plan_valid = false;
  memset(&g_last_dispatched_plan, 0, sizeof(g_last_dispatched_plan));
  // Transient Retro take-any redirect target. Reset with the other per-slot
  // transients so a stale door id from a prior slot can't mis-key a host-room
  // shop after a slot switch. (Within a slot it is set/cleared by
  // Overworld_UseEntrance; clearing it here is the slot-boundary backstop.)
  g_rando_takeany_door_id = 0;
  // add-rando-shopsanity — same slot-boundary reasoning for the persisted
  // overworld-door shop key. It lives in g_ram (so a snapshot replay-restore
  // keeps the shop the player is standing in resolvable), which means it does
  // NOT reset with the C statics above; without this a door id captured in the
  // previous slot could mis-key a shop in the next one.
  g_ram[kRam_RandoOverworldDoor] = 0;
  g_rando_active_world_state = kWorldState_Open;
  g_rando_show_item_tracker = false;
  g_rando_show_location_tracker = false;

  // Clear the immutable hint plan, discovery, imported identity, and paid
  // presentation latches at every slot boundary.
  Rando_ClearHints();

  // §62 — clear the share-string cache; reveal action returns FileNotFound
  // when no slot is active.
  g_rando_active_share_string[0] = '\0';

  // Reachability: invalidate the recovered settings and NULL the shuffle
  // assignments. Without this, switching to a vanilla/empty slot would
  // leave eval_has_prize / eval_medallion_opens reading the prior slot's stale
  // assignment table. (The VM treats NULL as "no prize/medallion reachable".)
  g_rando_active_settings_valid = false;
  g_rando_active_seed_u64 = 0;
  Logic_SetResolvedTowerCrystals(0);
  g_rando_settings_from_cold_replay = false;  // reset the source flag.
  rando_restore_seed_qol_features0();
  Rando_ApplyActiveForcedFeatures0();
  Rando_SetDungeonPrizeAssignment(NULL);
  Rando_SetMedallionAssignment(NULL);

  // invalidate the logic-overlay replay inputs so a
  // post-deactivation Rando_ReinstallActiveSlotLogicOverlays() clears the
  // stores instead of replaying a stale slot.
  g_rando_active_header_valid = false;
  g_rando_active_door_logic = false;
  g_rando_active_chains_logic = false;

  // Reset the starting-inventory gate so an in-session slot-switch (slot A
  // already received its grant, then user backs out and loads slot B) lets
  // slot B's grant fire on its next Module05_LoadFile. Without this, the
  // same-boot gate at g_ram[0x65e] stays set and slot B silently misses its
  // escape-fill on a brand-new save. The cold-boot exploit guard in
  // Rando_TryGrantStartingInventory still gates on sram_progress_indicator
  // so in-progress saves aren't re-granted.
  g_rando_starting_inventory_granted = 0;

  // Clear the persisted swordless flag so a stale byte can't make a subsequent
  // non-swordless / non-rando slot read as swordless (pairs with the write in
  // Rando_ActivateSidecarSlot).
  g_rando_swordless = 0;
}

// Whether the active slot's settings were recovered (format_version >= 2 blob)
// and the shuffle assignments installed. The tracker windows gate their
// reachability display on this — false means "settings unknown", show only
// checked/unchecked, not reachable.
// Whether a randomizer slot is currently active (a placement is installed).
bool Rando_IsActive(void) { return g_rando_slot_active != 0; }

bool Rando_HasActiveSettings(void) { return g_rando_active_settings_valid; }

// The recovered active settings, or NULL when unavailable. Used by the runtime
// reachability bridge (Rando_GetLiveReachability).
const RandoSettings *Rando_GetActiveSettings(void) {
  return g_rando_active_settings_valid ? &g_rando_active_settings : NULL;
}

static uint8 rando_count_crystals(void) {
  uint8 bits = link_has_crystals & 0x7f;
  uint8 count = 0;
  while (bits != 0) {
    count += bits & 1;
    bits >>= 1;
  }
  return count;
}

bool Rando_HasRequiredTowerCrystals(void) {
  // Preserve vanilla and old/v1 randomizer slots when the canonical settings
  // blob is unavailable. Current slots use their independent tower threshold.
  uint8 required = 7;
  if (g_rando_slot_active && g_rando_active_settings_valid)
    required = g_rando_effective_crystals_tower;  // resolved, never the sentinel
  return rando_count_crystals() >= required;
}

bool Rando_HasRequiredGanonCrystals(void) {
  // Vanilla combat has no crystal-count vulnerability gate. Preserve that
  // behavior for non-randomizer and legacy slots whose canonical settings are
  // unavailable; current randomizer slots honor their configured threshold.
  if (!g_rando_slot_active || !g_rando_active_settings_valid)
    return true;
  return rando_count_crystals() >= g_rando_effective_crystals_ganon;
}

void Rando_ApplyLoadedSaveRuntimeSettings(void) {
  if (!g_rando_slot_active || !g_rando_active_settings_valid)
    return;
  // Fast Ganon / Ganon Hunt start with the actual pyramid hole open.
  if(g_rando_active_settings.goal==kGoal_FastGanon || g_rando_active_settings.goal==kGoal_GanonHunt)
    save_ow_event_info[0x5b]|=0x20;
  else if(save_ow_event_info[0x5b]&0x20)
    Rando_MarkLocationChecked(LOC_Agahnim_2); // migrate older local post-Aga2 saves
  // ALTTPR preOpenGanonsTower: zero-crystal GT starts open rather than playing
  // a zero-maiden seal-breaking cutscene on first contact. Reads the RESOLVED
  // count so tower=random rolling 0 pre-opens too.
  if (g_rando_effective_crystals_tower == 0)
    save_ow_event_info[0x43] |= 0x20;
}

uint32 Rando_ActiveForcedFeatures0(void) {
  uint32 forced = 0;
  if (g_rando_active_settings_valid &&
      Rando_SettingsAssumeJpGlitches(&g_rando_active_settings)) {
    forced |= kFeatures0_RestoreJpGlitches;
  }
  return forced;
}

static void rando_snapshot_seed_qol_features0(void) {
  if (g_rando_seed_qol_features0_saved)
    return;
  g_rando_seed_qol_config_features0 = g_config.features0;
  g_rando_seed_qol_wanted_features0 = g_wanted_zelda_features;
  g_rando_seed_qol_enhanced_features0 = enhanced_features0;
  g_rando_seed_qol_features0_saved = true;
}

static void rando_restore_seed_qol_features0(void) {
  if (!g_rando_seed_qol_features0_saved)
    return;
  uint32 mask = kFeatures0_RandoSeedQolMask;
  g_config.features0 =
      (g_config.features0 & ~mask) | (g_rando_seed_qol_config_features0 & mask);
  g_wanted_zelda_features =
      (g_wanted_zelda_features & ~mask) | (g_rando_seed_qol_wanted_features0 & mask);
  enhanced_features0 =
      (enhanced_features0 & ~mask) | (g_rando_seed_qol_enhanced_features0 & mask);
  g_rando_seed_qol_features0_saved = false;
}

void Rando_ApplyActiveForcedFeatures0(void) {
  uint32 forced = Rando_ActiveForcedFeatures0();
  uint32 forceable = kFeatures0_RestoreJpGlitches;
  uint32 effective = (g_config.features0 & forceable) | forced;
  g_wanted_zelda_features =
      (g_wanted_zelda_features & ~forceable) | effective;
  enhanced_features0 =
      (enhanced_features0 & ~forceable) | effective;
}

void Rando_ApplySeedQolFeatures0(uint32 features0) {
  if (g_rando_slot_active)
    rando_snapshot_seed_qol_features0();
  uint32 forced = Rando_ActiveForcedFeatures0();
  uint32 slot_features = features0 & kFeatures0_RandoSeedQolMask;
  uint32 configurable_slot_mask = kFeatures0_RandoSeedQolMask & ~forced;
  uint32 effective_mask = kFeatures0_RandoSeedQolMask | forced;
  uint32 effective_features = slot_features | forced;

  // Active seed requirements are runtime overlays, not global user
  // preferences. Preserve g_config's value for any currently forced bits so
  // applying Game Settings cannot write a seed-required bit into the INI.
  g_config.features0 =
      (g_config.features0 & ~configurable_slot_mask) |
      (slot_features & configurable_slot_mask);
  g_wanted_zelda_features =
      (g_wanted_zelda_features & ~effective_mask) | effective_features;
  enhanced_features0 =
      (enhanced_features0 & ~effective_mask) | effective_features;
}

bool Rando_ActiveSlotHidesSpoiler(void) {
  // Fail CLOSED: unknown settings (NULL) are treated as race so a race seed
  // whose settings weren't recovered (snapshot replay / v1 slot) can't leak.
  const RandoSettings *s = Rando_GetActiveSettings();
  return s == NULL || s->race_mode;
}

// True when the active slot is swordless (mode.weapons=swordless). Gates the
// runtime swordless patches (hammer damages Ganon/Agahnim, medallions cast
// without a sword, Agahnim curtains pre-opened, tablets hammer-readable) so they
// fire ONLY under swordless and never alter vanilla/non-swordless behavior.
// When settings are known, that is authoritative. When settings are unknown
// (NULL — snapshot replay-restore or pre-swordless v1 slot), fall back to the
// persisted g_rando_swordless flag in g_ram, which LoadSnesState restores
// verbatim and Rando_ActivateSidecarSlot set from the slot's recovered
// settings. Without this fallback a Ctrl+F1 replay-restore of a swordless seed
// (which doesn't re-run activation) would revert to sword-required gates
// (medallion/tablet/Agahnim/Ganon) and softlock. Gated on an active slot so a
// cleared/zero byte under a non-rando slot reads as not-swordless.
bool Rando_IsSwordlessActive(void) {
  const RandoSettings *s = Rando_GetActiveSettings();
  if (s != NULL) return s->mode_weapons == kModeWeapons_Swordless;
  return g_rando_slot_active != 0 && g_rando_swordless != 0;
}

// ---------------------------------------------------------------------------
// Live reachability bridge (tracker windows).
//
// Rando_BuildRuntimeCounts maps the live g_ram inventory into the logical
// RandoCounts the predicate VM reads. The macros (macros.yaml) accept the
// progressive form via HAS_AMOUNT(Progressive*, n), so populating the
// progressive counts satisfies every tier disjunct — we don't need the
// absolute L1Sword/etc. ids. Prizes (crystals/pendants) are set from the HELD
// SRAM bitfields below; OP_HAS_PRIZE additionally derives *logical* prize
// access from reachable dungeon bosses (cleared_dungeons) in the fixed point.
// Event items the VM treats as inventory (RescuedZelda, DefeatAgahnim) are
// derived from actual game/rando progress.
// ---------------------------------------------------------------------------
static bool rando_has_escape_bomb_refill(void) {
  if (link_item_bombs != 0 || link_bomb_filler != 0) return true;
  const RandoPlacementTable *table = Placement_GetActive();
  if (!g_rando_slot_active || table == NULL) return false;
  for (uint16 i = 0; i < table->count; i++) {
    uint16 item = table->entries[i].item_id;
    if ((item == ITEM_Bombs1 || item == ITEM_Bombs3 || item == ITEM_Bombs10) &&
        Rando_IsLocationChecked(table->entries[i].location_id))
      return true;
  }
  return false;
}

void Rando_BuildRuntimeCounts(RandoCounts *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  // Progressive tiers. Sword byte 0xFF == none (not 0).
  uint8 sword = link_sword_type;
  if (sword >= 1 && sword <= 4) out->by_item_id[ITEM_ProgressiveSword] = sword;
  out->by_item_id[ITEM_ProgressiveShield] = link_shield_type;  // 0..3
  out->by_item_id[ITEM_ProgressiveArmor] = link_armor;          // 0=green,1=blue,2=red
  out->by_item_id[ITEM_ProgressiveGlove] = link_item_gloves;    // 0..2
  // Bow byte is non-linear: 0 none, 1-2 wood, 3-4 silver (the same boundary
  // encoded by Rando_ResolveGrantPlan).
  // Read true ownership (g_rando_bow_owned) so a player who owns silver but has
  // the slot toggled to wood arrows still counts as silver-capable; fall back to
  // the raw byte for pre-feature saves (ownership 0).
  uint8 bowb = link_item_bow;
  bool has_silver = (g_rando_bow_owned & kRandoBow_Silver) || bowb >= 3;
  bool has_bow = (g_rando_bow_owned & kRandoBow_Wood) || bowb >= 1;
  if (has_silver) {
    out->by_item_id[ITEM_ProgressiveBow] = 2;
    out->by_item_id[ITEM_SilverArrowUpgrade] = 1;
  } else if (has_bow) {
    out->by_item_id[ITEM_ProgressiveBow] = 1;
  }

  // Single-presence items.
  out->by_item_id[ITEM_FireRod] = link_item_fire_rod ? 1 : 0;
  out->by_item_id[ITEM_IceRod] = link_item_ice_rod ? 1 : 0;
  out->by_item_id[ITEM_Hammer] = link_item_hammer ? 1 : 0;
  out->by_item_id[ITEM_Hookshot] = link_item_hookshot ? 1 : 0;
  out->by_item_id[ITEM_Bombos] = link_item_bombos_medallion ? 1 : 0;
  out->by_item_id[ITEM_Ether] = link_item_ether_medallion ? 1 : 0;
  out->by_item_id[ITEM_Quake] = link_item_quake_medallion ? 1 : 0;
  out->by_item_id[ITEM_Lamp] = link_item_torch ? 1 : 0;
  out->by_item_id[ITEM_BugCatchingNet] = link_item_bug_net ? 1 : 0;
  out->by_item_id[ITEM_BookOfMudora] = link_item_book_of_mudora ? 1 : 0;
  out->by_item_id[ITEM_CaneOfSomaria] = link_item_cane_somaria ? 1 : 0;
  out->by_item_id[ITEM_CaneOfByrna] = link_item_cane_byrna ? 1 : 0;
  out->by_item_id[ITEM_Cape] = link_item_cape ? 1 : 0;
  out->by_item_id[ITEM_MagicMirror] = link_item_mirror ? 1 : 0;
  out->by_item_id[ITEM_Boots] = link_item_boots ? 1 : 0;
  out->by_item_id[ITEM_Flippers] = link_item_flippers ? 1 : 0;
  out->by_item_id[ITEM_MoonPearl] = link_item_moon_pearl ? 1 : 0;
  // General CanBombThings() is unconditional. This logical refill bit exists
  // only for CanKillEscapeThings's explicit Standard-escape ammo branch.
  if (rando_has_escape_bomb_refill()) out->by_item_id[ITEM_Bombs1] = 1;

  // Boomerangs: byte 1=blue, 2=red (separate rando items). Read true ownership
  // (g_rando_boomerang_owned) so a player who owns both but has the slot toggled
  // to one color still counts as having the other; fall back to the raw byte for
  // pre-feature saves (ownership 0).
  if ((g_rando_boomerang_owned & kRandoBoomerang_Blue) || link_item_boomerang == 1)
    out->by_item_id[ITEM_BlueBoomerang] = 1;
  if ((g_rando_boomerang_owned & kRandoBoomerang_Red) || link_item_boomerang == 2)
    out->by_item_id[ITEM_RedBoomerang] = 1;

  // Mushroom / Powder share byte 0xF344 (1=mushroom, 2=powder); true ownership
  // of EACH is tracked separately in rando state (g_rando_mushroom_held bits) so
  // neither a Powder-first pickup nor an item-menu swap to the other icon reads
  // as having lost one of them.
  if (Rando_MushroomHeld() || link_item_mushroom == 1) out->by_item_id[ITEM_Mushroom] = 1;
  if (Rando_PowderOwned()) out->by_item_id[ITEM_MagicPowder] = 1;

  // Flute / shovel decouple — true ownership in rando state (the vanilla
  // link_item_flute byte is one slot that can't hold both).
  uint8 fs = g_rando_flute_shovel_owned;
  if (fs & kRandoFluteShovel_Shovel) out->by_item_id[ITEM_Shovel] = 1;
  if (fs & kRandoFluteShovel_Flute) out->by_item_id[ITEM_OcarinaInactive] = 1;

  // Half / quarter magic: link_magic_consumption 1=half, 2=quarter.
  if (link_magic_consumption >= 1) out->by_item_id[ITEM_HalfMagic] = 1;
  if (link_magic_consumption >= 2) out->by_item_id[ITEM_QuarterMagic] = 1;

  // Bottles — count non-empty slots. HasBottle macro sums all bottle ids via
  // HAS_ANY_COUNT, so a single counter satisfies the count-based gates. (Content
  // -specific gates like GoodBee are not modeled; an acceptable approximation.)
  {
    uint8 bottles = 0;
    for (int i = 0; i < 4; i++) if (link_bottle_info[i] != 0) bottles++;
    out->by_item_id[ITEM_BottleEmpty] = bottles;
  }

  // Virtual / event items the VM reads as inventory.
  out->by_item_id[ITEM_TriforcePiece]=g_rando_triforce_piece_count;
  out->by_item_id[ITEM_StartingHeart] = 3;  // baseline 3 hearts
  // RescuedZelda: pre-collected in non-Standard worlds; in Standard it is earned
  // at the castle escape (sram_progress_indicator >= 2 == Zelda at sanctuary).
  // Use the SAME world-state source the reachability graph walks (the recovered
  // settings) so the event derivation can't diverge from the graph.
  uint8 ws = g_rando_active_settings_valid ? g_rando_active_settings.world_state
                                           : g_rando_active_world_state;
  bool rescued = (ws != (uint8)kWorldState_Standard) || (sram_progress_indicator >= 2);
  if (rescued) out->by_item_id[ITEM_RescuedZelda] = 1;
  // DefeatAgahnim: the Agahnim-1 location is checked when he is defeated.
  if (Rando_IsLocationChecked(LOC_Agahnim)) out->by_item_id[ITEM_DefeatAgahnim] = 1;

  // Prizes actually HELD, from the SRAM bitfields (same masks as
  // prize_item_direct_grant). OP_HAS_PRIZE keeps deriving *logical* prize
  // access from cleared dungeons, but inventory-reading ops
  // (OP_TOWER_CRYSTALS_MET, any HAS_ITEM(Prize_*)) count by_item_id — without
  // these the live tracker showed Ganon's Tower unreachable with all seven
  // crystals in hand.
  for (int i = 0; i < 7; i++) {
    if (link_has_crystals & kRandoCrystalItemBit[i])
      out->by_item_id[ITEM_Prize_Crystal1 + i] = 1;
  }
  if (link_which_pendants & 0x04) out->by_item_id[ITEM_Prize_GreenPendant] = 1;
  if (link_which_pendants & 0x02) out->by_item_id[ITEM_Prize_RedPendant] = 1;
  if (link_which_pendants & 0x01) out->by_item_id[ITEM_Prize_BluePendant] = 1;

  // Enemy, boss, and NPC souls live in a process-static ownership bitfield,
  // not g_ram. Materialize the contiguous soul item block into the same
  // logical inventory used by the live location tracker; otherwise every
  // NeedsEnemySoul/NeedsNpcSoul predicate remains false after the pickup even
  // though the item tracker and spawn gates see the soul as owned.
  for (uint16 item = ITEM_Soul_ArmosKnights;
       item < ITEM__COUNT && Souls_ItemIsSoul(item); item++) {
    uint8 soul_index = (uint8)(item - ITEM_Soul_ArmosKnights);
    if (Souls_OwnedIndex(soul_index)) out->by_item_id[item] = 1;
  }

  // Dungeon items. The placer assumes vanilla-mode keys/maps/compasses are
  // logically in-place, but the live tracker must answer "what can the player
  // get right now" from RAM. Read the actual per-dungeon counters/bitfields for
  // every mode, then map game-dungeon index -> registry id. Symbolic ITEM_ ids
  // (not arithmetic) avoid the game-order vs registry-order mismatch
  // (e.g. ToH/Castle-Tower swap). 0xFFFF = no such item for that dungeon
  // (HC/Castle-Tower have no big key/map/compass).
  if (g_rando_active_settings_valid) {
    const RandoSettings *st = &g_rando_active_settings;

    for (int g = 0; g < 14; g++) {
      uint16 bit = Rando_DungeonBitForGameDungeon((uint8)g);
      uint16 small_key = Rando_SmallKeyItemForGameDungeon((uint8)g);
      uint16 big_key = Rando_BigKeyItemForGameDungeon((uint8)g);
      uint16 map = Rando_MapItemForGameDungeon((uint8)g);
      uint16 compass = Rando_CompassItemForGameDungeon((uint8)g);
      if (small_key != 0xFFFF)
        out->by_item_id[small_key] = link_keys_earned_per_dungeon[g];
      if (big_key != 0xFFFF && (link_bigkey & bit))
        out->by_item_id[big_key] = 1;
      if (map != 0xFFFF && (link_dungeon_map & bit))
        out->by_item_id[map] = 1;
      if (compass != 0xFFFF && (link_compass & bit))
        out->by_item_id[compass] = 1;
    }
    // Retro genericKeys — the per-dungeon SmallKey cells above are all 0 under
    // genericKeys (keys live in the shared slot). Feed the live tracker/reach
    // panel the real shared count so the predicate VM's small-key collapse
    // (any door open with >=1 GenericKey) evaluates against the player's actual
    // keys. Matches the gen-time assumed inventory (by_item_id[ITEM_GenericKey]).
    if (Settings_GenericKeysActive(st))
      out->by_item_id[ITEM_GenericKey] = link_generic_keys;

    // Numeric key counters alone cannot prove ring ownership: a player may
    // have collected ordinary/free keys, or spent keys granted by a ring.
    // Materialize the derived ownership bits as the actual ring item IDs so
    // the shared effective-key-count helper can recognize them.
    uint16 owned_rings = Rando_GetOwnedKeyRingMask();
    for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
      if (owned_rings & (uint16)(1u << d)) {
        uint16 ring = Rando_KeyRingItemForRandoDungeon(d);
        if (ring < ITEM__COUNT) out->by_item_id[ring] = 1;
      }
    }

    // Hyrule Castle Ball-n-chain grants a vanilla big-key bit even though HCE
    // has no shuffled BigKey_* item. Enemy-drop sanity models that one-shot as
    // an EnemyDrop check plus this virtual logic item.
    uint16 hce_bigkey_bits =
        Rando_DungeonBitForGameDungeon(kGameDungeon_HyruleCastle) |
        Rando_DungeonBitForGameDungeon(kGameDungeon_HyruleCastleEscape);
    if (link_bigkey & hce_bigkey_bits)
      out->by_item_id[ITEM_HyruleCastleBigKey] = 1;
  }
}

// Memoized live reachability. Recomputed only when the reachability-state
// counter advances (bumped on item pickups, location checks, and progress
// events). Returns NULL when settings are unavailable (older slot / snapshot
// restore) — callers then suppress the reachability display. The result is
// snapshotted out of the shared Logic_ComputeReachabilityFullKnowledge buffer so it stays
// valid across frames and across both tracker windows.
static uint32 g_live_reach_counter = 0xFFFFFFFFu;
static uint8 g_live_reach_progress = 0xFFu;
static uint8 g_live_reach_has_live_bombs = 0xFFu;
static bool g_live_reach_valid = false;

// tracker-player-knowledge — build the live knowledge mask from the ACTIVE
// layout tables (never hardcoded pools) minus the persisted discovery state.
// Rebuilt on every memo recompute; discovery marks bump the reachability
// counter, so the memo can never serve a stale mask. An all-zero mask is the
// no-topology case and the masked flood is then byte-identical to full
// knowledge (Logic_KnowledgeMaskSelfCheck pins that identity).
static uint8 g_live_suppressed_locations[(kRandoLocationCapacity + 7) >> 3];

// Hidden-identity dungeons: the entrance stage-2/cross pool plus the chains
// pool (chains and entrance axes are mutually normalized, but OR is
// harmless), minus dungeons the player has been inside.
static uint16 rando_live_hidden_dungeon_mask(void) {
  const RandoSettings *s = &g_rando_active_settings;
  uint16 pool = Entrance_HiddenDungeonPoolMask(s);
  if (Settings_EffectiveDungeonChains(s)) {
    for (int i = 0; i < kChainsPoolCount; i++)
      pool |= (uint16)(1u << kChainsPoolDungeons[i]);
  }
  uint16 hidden = 0;
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    if (((pool >> d) & 1u) != 0 && !Rando_DungeonDiscovered(d))
      hidden |= (uint16)(1u << d);
  }
  return hidden;
}

// Public form for the tracker windows' "(unexplored)" affordance — same
// derivation the live mask uses, so display and flood can never disagree.
uint16 Rando_HiddenUndiscoveredDungeonMask(void) {
  if (!g_rando_slot_active || !g_rando_active_settings_valid) return 0;
  return rando_live_hidden_dungeon_mask();
}

static void rando_build_live_knowledge_mask(RandoKnowledgeMask *m) {
  memset(m, 0, sizeof *m);
  const RandoSettings *s = &g_rando_active_settings;
  m->hidden_dungeon_mask = rando_live_hidden_dungeon_mask();
  // Whirlpools: shuffled (non-identity) pairs minus ridden.
  m->hidden_whirlpool_mask =
      (uint8)(OwWarp_ShuffledWhirlpoolMask(Rando_ActiveOwWarpLayout()) &
              (uint8)~Rando_DiscoveredWhirlpoolMask());
  // Caves: any cave-shuffle variant hides an undiscovered interior's
  // contents; cave-decoupled additionally hides untraversed exit nets
  // (cross-decoupled adds no logic edges, so nothing to gate there).
  if (Entrance_IsActive(s)) {
    bool any = false;
    memset(g_live_suppressed_locations, 0, sizeof(g_live_suppressed_locations));
    int n = Entrance_CaveInteriorCount();
    for (int j = 0; j < n; j++) {
      if (!Rando_CaveInteriorDiscovered(j)) {
        const uint16 *ids = NULL;
        int cnt = Entrance_CaveInteriorLocationList(j, &ids);
        for (int k = 0; k < cnt; k++) {
          uint16 loc = ids[k];
          if (loc < kRandoLocationCapacity) {
            g_live_suppressed_locations[loc >> 3] |= (uint8)(1u << (loc & 7));
            any = true;
          }
        }
      }
      if (Entrance_IsDecoupledActive(s) && !Rando_DecoupledExitDiscovered(j))
        m->hidden_exit_mask |= (uint64)1u << j;
    }
    if (any) m->suppressed_locations = g_live_suppressed_locations;
  }
}

const RandoReachability *Rando_GetLiveReachability(void) {
  if (!g_rando_active_settings_valid) {
    g_live_reach_valid = false;
    return NULL;
  }
  uint32 cur = Rando_GetReachabilityCounter();
  // sram_progress_indicator feeds RescuedZelda in Standard (Rando_BuildRuntime-
  // Counts) but does NOT bump the reachability counter when it advances during
  // the castle-escape cutscene — so fold it into the memo key, else RescuedZelda
  // -gated regions stay dark until the next unrelated location check.
  uint8 prog = sram_progress_indicator;
  // Live bomb drops/refills are not rando checks, so include their boolean
  // availability in the memo key. Checked Bomb placements already bump `cur`.
  uint8 has_live_bombs = link_item_bombs != 0 || link_bomb_filler != 0;
  if (g_live_reach_valid && cur == g_live_reach_counter && prog == g_live_reach_progress &&
      has_live_bombs == g_live_reach_has_live_bombs) {
    return Reachability_Snapshot(false);  // stable cached snapshot
  }
  RandoCounts counts;
  Rando_BuildRuntimeCounts(&counts);
  // Re-install per call: a startup selfcheck generation may have left its own
  // resolved tower count in the logic store; the live flood must always see
  // the ACTIVE slot's cached effective value.
  Logic_SetResolvedTowerCrystals(
      (uint8)(g_rando_effective_crystals_tower + 1));
  // tracker-player-knowledge — the live view is ALWAYS the knowledge-limited
  // flood (unconditional, owner decision: no reveal toggle). The mask is
  // rebuilt here from active layouts minus discovery; an empty mask is
  // byte-identical to full knowledge.
  RandoKnowledgeMask kmask;
  rando_build_live_knowledge_mask(&kmask);
  const RandoReachability *r =
      Logic_ComputeReachabilityMasked(&counts, &g_rando_active_settings, &kmask);
  if (r == NULL) {
    g_live_reach_valid = false;
    return NULL;
  }
  g_live_reach_counter = cur;
  g_live_reach_progress = prog;
  g_live_reach_has_live_bombs = has_live_bombs;
  g_live_reach_valid = true;
  return Reachability_Snapshot(true);  // copy out of the shared buffer
}

void Rando_FillItemView(RandoItemView *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));

  uint8 sword = link_sword_type;
  out->sword = (sword >= 1 && sword <= 4) ? sword : 0;  // 0xFF == none
  out->shield = link_shield_type;     // 0..3
  out->mail = link_armor;             // 0 green, 1 blue, 2 red
  out->gloves = link_item_gloves;     // 0..2
  // Bow/boomerang: prefer true rando ownership (a swapped-down byte must not
  // read as the lower tier); fall back to the raw bytes with no slot active.
  uint8 bowb = link_item_bow;
  if (g_rando_slot_active && (g_rando_bow_owned & kRandoBow_Silver))
    out->bow = 2;                                    // silver
  else
    out->bow = (bowb >= 3) ? 2 : (bowb >= 1 ? 1 : 0);  // wood/silver
  if (g_rando_slot_active && (g_rando_boomerang_owned & kRandoBoomerang_Red))
    out->boomerang = 2;                              // red
  else
    out->boomerang = (link_item_boomerang <= 2) ? link_item_boomerang : 0;

  out->hookshot = link_item_hookshot != 0;
  out->firerod = link_item_fire_rod != 0;
  out->icerod = link_item_ice_rod != 0;
  out->hammer = link_item_hammer != 0;
  out->lamp = link_item_torch != 0;
  out->net = link_item_bug_net != 0;
  out->book = link_item_book_of_mudora != 0;
  out->somaria = link_item_cane_somaria != 0;
  out->byrna = link_item_cane_byrna != 0;
  out->cape = link_item_cape != 0;
  out->mirror = link_item_mirror != 0;
  out->boots = link_item_boots != 0;
  out->flippers = link_item_flippers != 0;
  out->moon_pearl = link_item_moon_pearl != 0;
  out->bombos = link_item_bombos_medallion != 0;
  out->ether = link_item_ether_medallion != 0;
  out->quake = link_item_quake_medallion != 0;

  // Shared-byte items. With a rando slot active, true ownership is in rando
  // state (the vanilla bytes are single slots that can't represent both). With
  // no slot active that state isn't tracked, so fall back to the raw vanilla
  // bytes for the informational view.
  if (g_rando_slot_active) {
    out->mushroom = Rando_MushroomHeld() || link_item_mushroom == 1;
    out->powder = Rando_PowderOwned();
    uint8 fs = g_rando_flute_shovel_owned;
    out->flute = (fs & kRandoFluteShovel_Flute) != 0;
    out->shovel = (fs & kRandoFluteShovel_Shovel) != 0;
  } else {
    out->mushroom = link_item_mushroom == 1;
    out->powder = link_item_mushroom == 2;
    out->flute = link_item_flute >= 2;   // 0xF34C: 2=flute(inactive), 3=flute(active)
    out->shovel = link_item_flute == 1;  // 1=shovel
  }

  {
    uint8 bottles = 0;
    for (int i = 0; i < 4; i++) if (link_bottle_info[i] != 0) bottles++;
    out->bottles = bottles;
  }
  out->magic = link_magic_consumption;          // 0 normal, 1 half, 2 quarter
  out->hearts = (uint8)(link_health_capacity >> 3);
  out->heart_pieces = link_heart_pieces;

  // Crystals (7) and pendants (3). crystal_mask bit (N-1) = rando Crystal N
  // obtained, indexed by the PRIZE crystal number (kPrize_Crystal1..7) the
  // tracker and placement use — NOT the HUD display order. The per-crystal
  // link_has_crystals bits mirror prize_item_direct_grant: C1=0x02, C2=0x10,
  // C3=0x40, C4=0x20, C5=0x04, C6=0x01, C7=0x08. pendant_mask bit0=green,
  // 1=red (ToH/Wisdom), 2=blue (DP/Power) by in-game DISPLAY color (the registry
  // Red/Blue Pendant names are swapped vs display); PrizeIcon maps prize->bit.
  static const uint8 kPendantMask[3] = { 4, 1, 2 };
  uint8 cbits = link_has_crystals, pbits = link_which_pendants;
  for (uint8 i = 0; i < 7; i++) {
    if (cbits & kRandoCrystalItemBit[i]) { out->crystal_mask |= (uint8)(1 << i); out->crystals++; }
  }
  for (uint8 i = 0; i < 3; i++) {
    if (pbits & kPendantMask[i]) { out->pendant_mask |= (uint8)(1 << i); out->pendants++; }
  }

  out->agahnim = Rando_IsLocationChecked(LOC_Agahnim);

  // Per-dungeon items (indexed by game-side dungeon index; bitfields use bit
  // 0x8000 >> index). These reflect what the player has actually collected
  // (shuffled dungeon items write these vanilla cells on receipt; vanilla-mode
  // dungeons fill them in-place).
  for (int i = 0; i < 16; i++) out->dungeon_small_keys[i] = link_keys_earned_per_dungeon[i];
  // SaveDungeonKeys parks the current dungeon's counter only on transition, so
  // the live cell is authoritative while Link remains inside. Overlay the
  // tracker view immediately after a spend; Hyrule Castle proper folds into
  // Escape/Sewers through the shared key-slot helper. Retro uses slot 15 as its
  // one GenericKey pool, so expose the same live value there.
  if ((uint8)cur_palace_index_x2 != 0xff) {
    uint8 key_slot = Rando_EffectiveKeySlot(
        Rando_KeySlotFromRawPalace((uint8)cur_palace_index_x2));
    if (key_slot < 16) out->dungeon_small_keys[key_slot] = link_num_keys;
  }
  out->skeleton_key_enabled = g_rando_slot_active && g_rando_skeleton_key_present;
  out->skeleton_key_owned = Rando_HasSkeletonKey();
  out->bigkey_bits = link_bigkey;
  out->map_bits = link_dungeon_map;
  out->compass_bits = link_compass;
}

// ---------------------------------------------------------------------------
// Phase B Slice 6 — race-mode reveal action.
//
// Pipeline:
//   1. Read the suppressed file at the given path; verify magic + CRC.
//   2. Deserialize embedded settings (with race_mode = 0 by construction).
//   3. Decode the file's share string to recover seed_u64.
//   4. Regenerate placement via Place_AssumedFill(settings, seed_u64). This
//      is the SAME pipeline the CLI / settings-screen uses, so output is
//      bit-identical when generator inputs match.
//   5. Compute spheres via Logic_ComputeSpheres(settings, placements).
//   6. Build a RandoSpoiler view with race_mode = 0 and wall_clock = 0
//      (the same normalization used when computing the stamp at suppression
//      time) and write to a tmp file. SHA-256 the bytes.
//   7. Compare the computed SHA-256 to the file's spoiler_stamp. If
//      mismatch, return kRandoReveal_StampMismatch — the suppressed file
//      does not match the generator's current output for these inputs.
//   8. Write the full JSON + .txt spoiler to disk (overwriting the
//      suppressed binary).
// ---------------------------------------------------------------------------
#include "rando_spoiler.h"

extern void sha256_buffer(const uint8 *data, size_t len, uint8 out[32]);

// Race reveal is normally exact-current only. Generator 156 is the one
// deliberately frozen exception: it predates configurable policy and uses the
// pinned 1/1 algorithm/schema. Keep accepting that exact historical container
// after later generator bumps; the embedded spoiler stamp still fails closed
// if current placement/sphere behavior cannot reproduce its signed bytes.
// Every other non-current generator remains unsupported.
static bool reveal_select_hint_identity(uint16 generator_version,
                                        uint16 *algorithm_version,
                                        uint16 *text_schema_version) {
  if (algorithm_version == NULL || text_schema_version == NULL)
    return false;
  if (generator_version == (uint16)kGeneratorVersion) {
    *algorithm_version = kRandoHintPlanAlgorithmVersion;
    *text_schema_version = kRandoHintTextSchemaVersion;
    return true;
  }
  if (kGeneratorVersion >= kRandoHintConfigurableGeneratorVersion &&
      generator_version == kRandoHintLegacyGeneratorVersion) {
    *algorithm_version = kRandoHintPlanLegacyAlgorithmVersion;
    *text_schema_version = kRandoHintLegacyTextSchemaVersion;
    return true;
  }
  return false;
}

RandoRevealResult Rando_RevealSpoiler(const char *suppressed_path,
                                      const char *expected_share_string) {
  char resolved[1024];
  if (suppressed_path == NULL) {
    if (expected_share_string == NULL) return kRandoReveal_FileNotFound;
    int n = Spoiler_ResolvePath(expected_share_string, ".json", resolved, (int)sizeof(resolved));
    if (n <= 0) return kRandoReveal_FileNotFound;
    suppressed_path = resolved;
  }

  // Idempotency (randomizer-ui spec §3.4): if the file at this path is
  // already a full JSON spoiler (first byte '{'), reveal already ran.
  // Return success without rewriting. The discriminator is the same one
  // §2.4 calls out — first byte 'Z' (suppressed magic 'ZRSR') vs. '{'
  // (JSON open brace) vs. anything else (malformed).
  {
    FILE *fdis = fopen(suppressed_path, "rb");
    if (fdis == NULL) return kRandoReveal_FileNotFound;
    int first = fgetc(fdis);
    fclose(fdis);
    if (first == '{') return kRandoReveal_Ok;
    if (first != 'Z') return kRandoReveal_ParseError;
  }

  RandoSuppressedSpoiler hdr;
  int rd = Spoiler_ReadSuppressed(suppressed_path, &hdr);
  if (rd == -1) return kRandoReveal_FileNotFound;
  if (rd == -2) return kRandoReveal_ParseError;
  if (rd == -3) return kRandoReveal_CrcMismatch;

  // Optional share-string equality check.
  if (expected_share_string != NULL) {
    size_t want_len = strlen(expected_share_string);
    if (want_len != hdr.share_string_len ||
        memcmp(expected_share_string, hdr.share_string, want_len) != 0) {
      return kRandoReveal_ShareStringMismatch;
    }
  }

  uint16 hint_algorithm_version;
  uint16 hint_text_schema_version;
  if (!reveal_select_hint_identity(hdr.generator_version,
                                   &hint_algorithm_version,
                                   &hint_text_schema_version)) {
    return kRandoReveal_VersionMismatch;
  }

  bool legacy_156 =
      hdr.generator_version == kRandoHintLegacyGeneratorVersion;
  if (legacy_156 &&
      ((hdr.settings_canonical[25] &
        kHintNpcRewardRevealAxis_Enabled) != 0 ||
       (hdr.settings_canonical[28] &
        (kHintTileCoverageAxis_Mask | kHintMixAxis_LowBit)) != 0 ||
       (hdr.settings_canonical[29] & kHintMixAxis_HighBit) != 0 ||
       (hdr.settings_canonical[30] & kHintPaidDepthAxis_Mask) != 0)) {
    // Those seven bits did not exist in generator 156. Accepting them would let
    // a CRC-correct foreign artifact smuggle configurable policy into the
    // frozen 1/1 builder instead of reproducing a genuine predecessor seed.
    return kRandoReveal_SettingsCorrupt;
  }

  RandoSettings settings;
  if (Settings_CanonicalDeserialize(hdr.settings_canonical, &settings) != 0) {
    return kRandoReveal_SettingsCorrupt;
  }
  // Defensive: ensure race_mode is 0 in the regenerated settings (matches
  // what compute_stamp normalizes for SHA input).
  settings.race_mode = 0;

  // Decode the share string to get seed_u64.
  char share_buf[kRandoSuppressedSpoilerShareStringMax + 1];
  uint32 sl = hdr.share_string_len;
  if (sl > kRandoSuppressedSpoilerShareStringMax) sl = kRandoSuppressedSpoilerShareStringMax;
  memcpy(share_buf, hdr.share_string, sl);
  share_buf[sl] = '\0';
  ShareString ss;
  if (Share_Decode(share_buf, &ss) != kShareDecodeOk) {
    return kRandoReveal_ParseError;
  }
  if (ss.version != (uint8)hdr.generator_version) {
    return kRandoReveal_VersionMismatch;
  }
  if (ss.format == kShareFormatV2) {
    // ZRSR normally embeds the 50-character v1 identity. If a valid shorter
    // historical v2 token is encountered, require its settings to normalize
    // to the same race-cleared canonical bytes instead of trusting two
    // conflicting sources of regeneration inputs.
    RandoSettings share_settings;
    uint8 share_canonical[kSettingsCanonicalLen];
    if (Settings_CanonicalDeserialize(ss.settings_canonical,
                                      &share_settings) != 0) {
      return kRandoReveal_SettingsCorrupt;
    }
    share_settings.race_mode = 0;
    Settings_CanonicalSerialize(&share_settings, share_canonical);
    if (memcmp(share_canonical, hdr.settings_canonical,
               sizeof share_canonical) != 0) {
      return kRandoReveal_SettingsCorrupt;
    }
  }
  uint64 seed_u64 = ss.seed_u64;

  // Regenerate the placement table.
  // pass budget_seconds=0 (no wall-clock cutoff) so the
  // placer runs to its hard 8-attempt cap. This makes the placer's behavior
  // deterministic at reveal regardless of machine speed; combined with the
  // stamp's H1 normalization of attempts_used/forward_fill_fallback_count,
  // the stamp is reproducible across machines.
  static RandoPlacement scratch_entries[kRandoLocationCapacity];
  RandoPlacementTable table;
  table.entries = scratch_entries;
  table.count = 0;
  // Use the SHARED placement+entrance regen (same code the generate path runs)
  // so the regenerated spoiler — including the entrance_mapping section that
  // feeds the SHA-256 stamp — is byte-identical. Without this, revealing a
  // race-mode + entrance-shuffle seed always false-failed as "tampered" because
  // the regen omitted entrance_mapping (the bug this fix closes). budget 0 = the
  // deterministic hard cap, matching race-mode generation.
  //
  // The accepted π's LOGIC overrides are left active by the helper. For an
  // active entrance slot this re-derives the identical π (deterministic from
  // seed/axes/accepted-attempt), so it restores — not pollutes — the slot's
  // tracker override state; and the in-binary reveal is gated to post-game, so
  // reachability is moot anyway. The gameplay door overlay is never touched
  // (the helper applies only logic overrides, not Entrance_RuntimeInstall).
  RandoEntranceRegen reg;
  if (!Rando_PlaceWithEntrances(&settings, seed_u64, /*budget_seconds=*/0, &table, &reg)) {
    if (table.count == 0) return kRandoReveal_PlacementFailed;
  }

  RandoSpheres spheres;
  Logic_ComputeSpheres(&settings, &table, &spheres);

  // Rebuild the immutable generation-time hint plan explicitly. Spoiler
  // serialization consumes this caller-owned value, so race reveal neither
  // reads nor clobbers the active gameplay plan/discovery state.
  RandoHintPlan hint_plan;
  if (!Rando_BuildHintPlan(&settings, &table, &spheres,
                           hint_algorithm_version,
                           hint_text_schema_version, &hint_plan)) {
    return kRandoReveal_SettingsCorrupt;
  }

  // Build the spoiler view and write to a tmp file for stamp computation.
  // forward_fill_fallback_count and retry_attempts are
  // normalized in compute_stamp via the same constants (see
  // rando_spoiler.c). At reveal, the regen spoiler is also normalized so
  // the JSON bytes are byte-identical to the generate-time stamp input.
  RandoSpoiler regen;
  memset(&regen, 0, sizeof(regen));
  regen.share_string = share_buf;
  regen.seed_u64 = seed_u64;
  regen.generator_version = hdr.generator_version;
  regen.settings = &settings;
  regen.placements = &table;
  regen.spheres = &spheres;
  regen.hint_plan = &hint_plan;
  uint8 regen_medallion_assignment[kRandoMedallionEntranceCount];
  {
    const uint8 *assignment = Rando_GetMedallionAssignment();
    if (assignment != NULL) {
      memcpy(regen_medallion_assignment, assignment, sizeof(regen_medallion_assignment));
      regen.medallion_assignment = regen_medallion_assignment;
    }
  }
  // Match the generate-time spoiler's entrance_mapping section (the omission of
  // which caused the stamp mismatch on race-mode + entrance-shuffle seeds).
  Rando_SpoilerSetEntranceFields(&regen, &reg);
  // Match the generate-time spoiler's boss_assignments / drop_map sections
  // (see Rando_GenerateSlot in rando_generate.c). Omitting these caused the
  // SHA-256 stamp to mismatch for race seeds generated with boss_shuffle or
  // drop_shuffle on — same omission class as the entrance_mapping fix above.
  // The buffers are declared at function scope so they outlive Spoiler_WriteJson
  // (stamp input) and the later Spoiler_WriteText. Computed from the PURE forms
  // (no runtime-global side effects), deterministic from (settings, seed) →
  // byte-identical to the generate path. NULL pointers omit the section when off.
  uint8 regen_boss_assignment[16];
  uint8 regen_drop_map[kDropTableEntryCount];
  bool regen_drop_used_fallback = false;
  BossShuffle_ComputeAssignment(&settings, seed_u64, regen_boss_assignment);
  DropShuffle_ComputeAssignment(&settings, seed_u64, regen_drop_map, &regen_drop_used_fallback);
  regen.boss_assignment = settings.boss_shuffle ? regen_boss_assignment : NULL;
  regen.drop_map = settings.drop_shuffle ? regen_drop_map : NULL;
  regen.drop_used_fallback = regen_drop_used_fallback;
  regen.goal_completable = Goal_IsCompletable(&settings, seed_u64, &table);
  regen.forward_fill_fallback_count = 0;  // stamp normalization
  regen.retry_attempts = 1;               // stamp normalization
  regen.generation_wall_clock_ms = 0;     // stamp normalization
  // race_mode is canonical byte [17]; compute_stamp normalizes it to 0 on the
  // GENERATE side (norm_settings.race_mode = 0). The spoiler settings echo now
  // emits race_mode + canonical_hex, both of which encode byte [17] — so the
  // reveal-side regen MUST zero it too or every race reveal false-fails as a
  // stamp mismatch. (Latent before the completed echo; only surfaced once the
  // echo included race_mode-dependent bytes. Placement regen above is already
  // done and is race_mode-independent, so mutating the local copy here is
  // safe.) KEEP IN SYNC with compute_stamp's four-field + race_mode list.
  settings.race_mode = 0;

  // Write JSON to a tmp file next to the suppressed path so we can read it
  // back and SHA-256 it without disturbing the suppressed file.
  char tmp_path[1280];
  if (snprintf(tmp_path, sizeof(tmp_path), "%s.reveal-tmp", suppressed_path) >= (int)sizeof(tmp_path)) {
    return kRandoReveal_WriteFailed;
  }

  // Cleanup epilogue is reached via `goto fail` from every error path so
  // .reveal-tmp never leaks.
  uint8 *bytes = NULL;
  RandoRevealResult result = kRandoReveal_WriteFailed;

  if (!Spoiler_WriteJson(&regen, tmp_path)) goto fail;

  // Read the tmp file back and SHA-256 it.
  FILE *rb = fopen(tmp_path, "rb");
  if (rb == NULL) goto fail;
  fseek(rb, 0, SEEK_END);
  long flen = ftell(rb);
  rewind(rb);
  if (flen <= 0) { fclose(rb); goto fail; }
  bytes = (uint8 *)malloc((size_t)flen);
  if (bytes == NULL) { fclose(rb); goto fail; }
  size_t got = fread(bytes, 1, (size_t)flen, rb);
  fclose(rb);
  if (got != (size_t)flen) goto fail;
  uint8 calc_stamp[32];
  sha256_buffer(bytes, (size_t)flen, calc_stamp);

  if (memcmp(calc_stamp, hdr.spoiler_stamp, 32) != 0) {
    // leave the suppressed file untouched on stamp
    // mismatch. The fact that we never wrote to suppressed_path is the
    // invariant.
    result = kRandoReveal_StampMismatch;
    goto fail;
  }

  // Stamp matched. Write to `.partial`, fsync-equivalent close,
  // then rename atomically over the suppressed file. The previous
  // fopen("wb") + fwrite pattern truncated the suppressed file before
  // writing; a mid-write failure (disk full, IO error) would lose the
  // original suppressed bytes AND leave a partial new file. The partial+
  // rename pattern keeps the suppressed file intact until the new bytes
  // are fully on disk.
  {
    char partial_path[1280];
    if (snprintf(partial_path, sizeof(partial_path), "%s.partial", suppressed_path) >= (int)sizeof(partial_path)) {
      goto fail;
    }
    FILE *out = fopen(partial_path, "wb");
    if (out == NULL) goto fail;
    size_t wrote = fwrite(bytes, 1, (size_t)flen, out);
    if (fclose(out) != 0 || wrote != (size_t)flen) {
      remove(partial_path);
      goto fail;
    }
    // On Windows rename() fails if the destination exists; use
    // remove()+rename(). The window between remove() and rename() is
    // small enough that a crash there is acceptable (the partial file
    // exists; the user can manually inspect / rescue). On POSIX rename()
    // is atomic-replace.
    remove(suppressed_path);
    if (rename(partial_path, suppressed_path) != 0) {
      // Partial file remains as evidence of the failed write; leave it
      // for inspection rather than auto-deleting.
      goto fail;
    }
  }

  // Write the .txt companion alongside.
  {
    char txt_path[1280];
    size_t pl = strlen(suppressed_path);
    if (pl >= 5 && strcmp(suppressed_path + pl - 5, ".json") == 0) {
      memcpy(txt_path, suppressed_path, pl - 5);
      strcpy(txt_path + pl - 5, ".txt");
      (void)Spoiler_WriteText(&regen, txt_path);
    } else if (pl + 4 < sizeof(txt_path)) {
      memcpy(txt_path, suppressed_path, pl);
      strcpy(txt_path + pl, ".txt");
      (void)Spoiler_WriteText(&regen, txt_path);
    }
    // If neither path-form fits, skip the .txt — success on the JSON path.
  }

  result = kRandoReveal_Ok;

fail:
  // Always drop the .reveal-tmp scratch file and free
  // the in-memory bytes. `result` carries the outcome.
  if (bytes != NULL) free(bytes);
  remove(tmp_path);
  return result;
}

const char *Rando_RevealResultDescription(RandoRevealResult r) {
  switch (r) {
    case kRandoReveal_Ok:                  return "Spoiler revealed.";
    case kRandoReveal_FileNotFound:        return "Suppressed spoiler file not found.";
    case kRandoReveal_ParseError:          return "Suppressed spoiler is malformed.";
    case kRandoReveal_CrcMismatch:         return "Suppressed spoiler CRC mismatch — file is corrupt or tampered.";
    case kRandoReveal_ShareStringMismatch: return "Suppressed file's share string does not match the active slot.";
    case kRandoReveal_VersionMismatch:     return "Suppressed file was produced by a different generator version.";
    case kRandoReveal_StampMismatch:       return "Stamp mismatch — regenerated placement does not match the recorded stamp.";
    case kRandoReveal_PlacementFailed:     return "Placement regeneration failed.";
    case kRandoReveal_SettingsCorrupt:     return "Embedded settings are corrupt.";
    case kRandoReveal_WriteFailed:         return "Failed writing revealed spoiler to disk.";
    default:                               return "Unknown reveal error.";
  }
}

// §62 — in-binary reveal entry point. The host wires this to a key
// binding (kKeys_RandoRevealSpoiler). When the player presses the bound
// key, we attempt to reveal the active slot's race-mode ZRSR file.
//
// No-op (returns FileNotFound with a logged note) when:
//   - no rando slot is currently active (e.g., the player is on the
//     file-select screen);
//   - the active slot's share string was never captured (Activate path
//     not exercised — shouldn't happen in practice);
//   - the resolved spoiler path doesn't exist (non-race slot, or the
//     race-mode file was already revealed / never written).
// Race-mode reveal completion latch. main_module_index is 0x19/0x1A only WHILE
// the Triforce room / credits are on screen; once the player leaves (file
// select, or reloads the beaten save) it reads not-beaten and the reveal would
// refuse even though the seed is done. Latch completion per session, keyed to
// the active slot's share string (loading a different slot re-keys and clears
// it). Ticked each frame from the main loop (Rando_NoteFrameForReveal).
// Session-scoped: an app restart clears it (re-beat, or use --reveal-spoiler).
static bool g_reveal_beaten = false;
static char g_reveal_beaten_share[sizeof g_rando_active_share_string] = {0};

void Rando_NoteFrameForReveal(void) {
  if (!g_rando_slot_active || g_rando_active_share_string[0] == 0)
    return;
  if (strcmp(g_reveal_beaten_share, g_rando_active_share_string) != 0) {
    snprintf(g_reveal_beaten_share, sizeof g_reveal_beaten_share, "%s",
             g_rando_active_share_string);
    g_reveal_beaten = false;
  }
  if (main_module_index == 0x19 || main_module_index == 0x1A)
    g_reveal_beaten = true;
}

// True iff the active slot's seed has been beaten this session (or the ending
// is on screen now). Both reveal gates route through this so they stay in sync.
static bool Rando_ActiveSlotBeaten(void) {
  if (main_module_index == 0x19 || main_module_index == 0x1A)
    return true;
  return g_reveal_beaten &&
         strcmp(g_reveal_beaten_share, g_rando_active_share_string) == 0;
}

RandoRevealResult Rando_RevealActiveSlotSpoiler(void) {
  if (!g_rando_slot_active || g_rando_active_share_string[0] == '\0') {
    fprintf(stderr, "rando reveal: no active randomizer slot.\n");
    return kRandoReveal_FileNotFound;
  }
  // anti-cheat gate. Race-mode's design intent
  // is the spoiler stays off-disk until post-race. An in-binary key with
  // no terminal-state gate lets a self-disciplined runner peek mid-race
  // and defeats the design. Gate the in-binary action on game-completion:
  // the seed is beaten ONLY at TriforceRoom (0x19) or Credits (0x1A).
  // Deliberately NOT `>= 0x18` — that also matches GanonEmerges (0x18, the
  // fight is only just starting) and SpawnSelect (0x1B, the spawn-point menu
  // the load path transiently passes through, blipping "completed" for one
  // frame on slot load). Mirrors the beaten check in auto_tracker.c.
  // The `--reveal-spoiler=<path>` CLI flow stays unconditional (no in-
  // game state to check) for tournament admins / post-race tooling.
  if (!Rando_ActiveSlotBeaten()) {
    fprintf(stderr,
            "rando reveal: refused — game not yet completed "
            "(use --reveal-spoiler CLI flag for tournament admin reveals).\n");
    return kRandoReveal_FileNotFound;
  }
  RandoRevealResult r =
      Rando_RevealSpoiler(NULL, g_rando_active_share_string);
  fprintf(stderr, "rando reveal: %s\n", Rando_RevealResultDescription(r));
  return r;
}

bool Rando_CanRevealActiveSlotSpoiler(void) {
  // Mirrors the gate inside Rando_RevealActiveSlotSpoiler(): an active slot with
  // a captured share string, past the anti-cheat completion threshold — the seed
  // is beaten ONLY at TriforceRoom (0x19) or Credits (0x1A). Keep this in sync
  // with that function (and the beaten check in auto_tracker.c).
  return g_rando_slot_active && g_rando_active_share_string[0] != '\0' &&
         Rando_ActiveSlotBeaten();
}

// ---------------------------------------------------------------------------
// Rando_DrawHashIcons (tasks.md §9.4b — 5-icon visual hash widget).
//
// CRITICAL invariant: the hash input is the FULL share_string_binary, NOT
// settings_hash. Two seeds with identical settings have identical
// settings_hash bytes but DIFFERENT share strings (because seed_u64 differs),
// so the 5-icon strip differs. Deriving from settings_hash silently produces
// "all-same icons for all seeds of these settings" — the architectural error
// caught in spec round 5 and re-emphasized in the cluster-3 briefing.
// ---------------------------------------------------------------------------
#include "icon_atlas.h"      // kHashIconAtlas + kHashIconAtlasSize

// Write a single OAM entry's 4 packed fields. We CANNOT use SetOamPlain
// here because that helper also writes bytewise_extended_oam[oam - oam_buf]
// — a global indexed by the OAM entry's offset from `oam_buf`. The widget
// is also exercised by --rando-selftest with stack-local buffers, where
// `oam - oam_buf` is a wild pointer-difference that would corrupt random
// memory. Setting the 4 fields directly is safe with any buffer, and the
// bytewise_extended_oam entry isn't needed for these tiles (the entries
// are tile-sized 8x8 = `big`=0 / x<256 — both bits we'd set would be 0).
void Rando_DrawHashIcons(int x, int y,
                         struct OamEnt *oam,
                         const uint8 share_string_binary[32]) {
  // share_string_binary is the raw 31-byte share blob (magic + version +
  // settings_hash + seed_u64 + checksum); the storage is sized to 32 with
  // the last byte zero per RandoSlotHeader.share_string[]. We hash the full
  // 32 bytes — the zero pad is part of the canonical input so different
  // payloads always produce different hashes.
  uint8 digest[32];
  sha256_buffer(share_string_binary, 32, digest);
  // Emit 5 OAM tiles. Palette flag 0x32 mirrors the file-select font palette
  // (see SelectFile_Func5_DrawOams's kSelectFile_Draw_Flags2 = 0x32/0x36/0x3a)
  // so the icons read cleanly on the dark file-select background.
  OamEnt *o = (OamEnt *)oam;
  for (int i = 0; i < 5; ++i) {
    uint8 idx = (uint8)(digest[i] % (uint8)kHashIconAtlasSize);
    uint8 tile = kHashIconAtlas[idx];
    o[i].x = (uint8)(x + i * 8);
    o[i].y = (uint8)y;
    o[i].charnum = tile;
    o[i].flags = 0x32;
  }
}

// ---------------------------------------------------------------------------
// Self-tests. Always linked; invoked from --rando-selftest CLI flag.
//
// Rando_SelfCheck validates the SHA-256 impl against the two NIST FIPS 180-2
// known vectors. Rando_RunAllSelfChecks invokes every subsystem's self-test
// in sequence; any failure exits with code 2.
//
// Cost: ~tens of microseconds per binary launch when invoked; not run during
// normal game frames.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rando_rng.h"

static int hex_eq(const uint8 *hash, const char *hex) {
  for (int i = 0; i < 32; ++i) {
    unsigned hi = (unsigned char)hex[i * 2], lo = (unsigned char)hex[i * 2 + 1];
    unsigned hv = (hi <= '9' ? hi - '0' : (hi & 0x5f) - 'A' + 10);
    unsigned lv = (lo <= '9' ? lo - '0' : (lo & 0x5f) - 'A' + 10);
    if (hash[i] != (uint8)((hv << 4) | lv)) return 0;
  }
  return 1;
}

static void Rando_DungeonIdSelfCheck(void) {
  if (Rando_GameDungeonFromRandoDungeon(kRandoDungeon_PalaceOfDarkness) != kGameDungeon_PalaceOfDarkness ||
      Rando_GameDungeonFromRandoDungeon(kRandoDungeon_SwampPalace) != kGameDungeon_SwampPalace ||
      Rando_GameDungeonFromRandoDungeon(kRandoDungeon_SkullWoods) != kGameDungeon_SkullWoods ||
      Rando_GameDungeonFromRandoDungeon(kRandoDungeon_ThievesTown) != kGameDungeon_ThievesTown ||
      Rando_GameDungeonFromRandoDungeon(kRandoDungeon_MiseryMire) != kGameDungeon_MiseryMire) {
    fprintf(stderr, "Rando_SelfCheck: rando->game dungeon mapping mismatch\n");
    exit(2);
  }
  if (Rando_RandoDungeonFromGameDungeon(kGameDungeon_SwampPalace) != kRandoDungeon_SwampPalace ||
      Rando_RandoDungeonFromGameDungeon(kGameDungeon_PalaceOfDarkness) != kRandoDungeon_PalaceOfDarkness ||
      Rando_RandoDungeonFromGameDungeon(kGameDungeon_MiseryMire) != kRandoDungeon_MiseryMire ||
      Rando_RandoDungeonFromGameDungeon(kGameDungeon_SkullWoods) != kRandoDungeon_SkullWoods ||
      Rando_RandoDungeonFromGameDungeon(kGameDungeon_ThievesTown) != kRandoDungeon_ThievesTown) {
    fprintf(stderr, "Rando_SelfCheck: game->rando dungeon mapping mismatch\n");
    exit(2);
  }
  if (Rando_MapDisplayDungeonFromGameDungeon(kGameDungeon_HyruleCastle) != kRandoDungeon_HyruleCastleEscape ||
      Rando_MapDisplayDungeonFromGameDungeon(kGameDungeon_HyruleCastleEscape) != kRandoDungeon_HyruleCastleEscape ||
      Rando_MapDisplayDungeonFromGameDungeon(kGameDungeon_HyruleCastleTower) != kRandoDungeon_HyruleCastleTower) {
    fprintf(stderr, "Rando_SelfCheck: map-display dungeon mapping mismatch\n");
    exit(2);
  }
  if (Rando_KeySlotFromRawPalace(2) != kGameDungeon_HyruleCastleEscape ||
      Rando_KeySlotFromRawPalace(16) != kGameDungeon_SkullWoods ||
      Rando_KeySlotFromRawPalace(0xff) != kGameDungeon_None) {
    fprintf(stderr, "Rando_SelfCheck: raw-palace key-slot mapping mismatch\n");
    exit(2);
  }
  if (Rando_DungeonItemGameDungeon(ITEM_SmallKey_SkullWoods) != kGameDungeon_SkullWoods ||
      Rando_DungeonItemGameDungeon(ITEM_BigKey_SkullWoods) != kGameDungeon_SkullWoods ||
      Rando_DungeonItemGameDungeon(ITEM_Map_SwampPalace) != kGameDungeon_SwampPalace ||
      Rando_DungeonItemGameDungeon(ITEM_Compass_PalaceOfDarkness) != kGameDungeon_PalaceOfDarkness ||
      Rando_DungeonItemGameDungeon(ITEM_Map_HyruleCastleEscape) != kGameDungeon_HyruleCastleEscape) {
    fprintf(stderr, "Rando_SelfCheck: dungeon item -> game mapping mismatch\n");
    exit(2);
  }
  if (Rando_RandoDungeonFromDungeonItem(ITEM_SmallKey_HyruleCastleTower) != kRandoDungeon_HyruleCastleTower ||
      Rando_RandoDungeonFromDungeonItem(ITEM_BigKey_GanonsTower) != kRandoDungeon_GanonsTower ||
      Rando_RandoDungeonFromDungeonItem(ITEM_Map_HyruleCastleEscape) != kRandoDungeon_HyruleCastleEscape ||
      Rando_RandoDungeonFromDungeonItem(ITEM_Compass_MiseryMire) != kRandoDungeon_MiseryMire) {
    fprintf(stderr, "Rando_SelfCheck: dungeon item -> rando mapping mismatch\n");
    exit(2);
  }
  if (Rando_SmallKeyItemForGameDungeon(kGameDungeon_SkullWoods) != ITEM_SmallKey_SkullWoods ||
      Rando_BigKeyItemForGameDungeon(kGameDungeon_ThievesTown) != ITEM_BigKey_ThievesTown ||
      Rando_MapItemForGameDungeon(kGameDungeon_HyruleCastleEscape) != ITEM_Map_HyruleCastleEscape ||
      Rando_MapItemForGameDungeon(kGameDungeon_SwampPalace) != ITEM_Map_SwampPalace ||
      Rando_CompassItemForGameDungeon(kGameDungeon_PalaceOfDarkness) != ITEM_Compass_PalaceOfDarkness) {
    fprintf(stderr, "Rando_SelfCheck: game dungeon -> item mapping mismatch\n");
    exit(2);
  }
  if (Rando_PrizeRandoDungeonFromGameDungeon(kGameDungeon_MiseryMire) != kRandoDungeon_MiseryMire ||
      Rando_PrizeRandoDungeonFromGameDungeon(kGameDungeon_HyruleCastleTower) != kRandoDungeon_None ||
      Rando_BossPrizeLocationForGameDungeon(kGameDungeon_SkullWoods) != LOC_Skull_Woods_Prize ||
      Rando_BossHeartLocationForGameDungeon(kGameDungeon_PalaceOfDarkness) != LOC_Palace_of_Darkness_Boss) {
    fprintf(stderr, "Rando_SelfCheck: boss/prize game mapping mismatch\n");
    exit(2);
  }
  if (kRandoDungeonRuntimeRows[0].key_slot != kGameDungeon_HyruleCastleEscape ||
      kRandoDungeonRuntimeRows[0].bigkey_game_dungeon != kGameDungeon_None ||
      kRandoDungeonRuntimeRows[0].map_game_dungeon != kGameDungeon_HyruleCastleEscape ||
      kRandoDungeonRuntimeRows[0].compass_game_dungeon != kGameDungeon_None ||
      kRandoDungeonRuntimeRows[0].rando_dungeon != kRandoDungeon_HyruleCastleEscape) {
    fprintf(stderr, "Rando_SelfCheck: HCE runtime row axis mismatch\n");
    exit(2);
  }
  for (uint8 d = 0; d < kRandoDungeon_Count; d++) {
    uint16 ring = Rando_KeyRingItemForRandoDungeon(d);
    const DirectGrantIconEntry *icon = rando_direct_grant_icon_entry(ring);
    uint8 stock = Rando_KeyRingGrantCount(d);
    if (!Rando_IsKeyRingItem(ring) ||
        Rando_RandoDungeonFromDungeonItem(ring) != d ||
        stock == 0 || stock == 0xff || icon == NULL ||
        icon->gfx != kRandoCustomGfx_KeyRing) {
      fprintf(stderr,
              "Rando_SelfCheck: Key Ring mapping/stock/icon mismatch for dungeon %u\n",
              (unsigned)d);
      exit(2);
    }
  }
  if (Rando_IsKeyRingItem(ITEM_SkeletonKey) ||
      rando_direct_grant_icon_entry(ITEM_SkeletonKey) == NULL ||
      rando_direct_grant_icon_entry(ITEM_SkeletonKey)->gfx !=
          kRandoCustomGfx_SkeletonKey) {
    fprintf(stderr, "Rando_SelfCheck: Skeleton Key classification/icon mismatch\n");
    exit(2);
  }
}

void Rando_SelfCheck(void) {
  uint8 out[32];
  static const char kExpectedEmpty[] = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
  static const char kExpectedAbc[]   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

  sha256_buffer((const uint8 *)"", 0, out);
  if (!hex_eq(out, kExpectedEmpty)) {
    fprintf(stderr, "Rando_SelfCheck: SHA-256 of empty input FAILED\n");
    exit(2);
  }
  sha256_buffer((const uint8 *)"abc", 3, out);
  if (!hex_eq(out, kExpectedAbc)) {
    fprintf(stderr, "Rando_SelfCheck: SHA-256 of 'abc' FAILED\n");
    exit(2);
  }
  Rando_DungeonIdSelfCheck();

  // Both randomized generous-guy rooms share the gift-thief sprite handler.
  // Keep this full-room mapping covered so a low-byte-only comparison cannot
  // silently route a different 0x23 room to Mini Moldorm Cave's NPC check.
  if (Rando_GiftThiefLocationForRoom(0x11E) != LOC_Hype_Cave_NPC ||
      Rando_GiftThiefLocationForRoom(0x123) != LOC_Mini_Moldorm_Cave_NPC ||
      Rando_GiftThiefLocationForRoom(0x127) != 0xFFFFu ||
      Rando_GiftThiefLocationForRoom(0x223) != 0xFFFFu) {
    fprintf(stderr, "Rando_SelfCheck: gift-thief room mapping mismatch\n");
    exit(2);
  }

  // add-rando-pot-sanity Phase 4 — Rando_GetPotLocation (room,pos4)->LOC binary
  // search: the table must be strictly sorted by (room<<16|pos4) and every entry
  // must round-trip, or the runtime grant hook resolves the wrong pot.
  {
    uint32 prev = 0;
    bool first = true;
    for (uint32 i = 0; i < kRandoPotLookup_COUNT; i++) {
      uint32 k = ((uint32)kRandoPotLookup[i].room << 16) | kRandoPotLookup[i].pos4;
      if (!first && k <= prev) {
        fprintf(stderr, "Rando_SelfCheck: pot lookup not strictly sorted at %u\n", i);
        exit(2);
      }
      first = false;
      prev = k;
      if (Rando_GetPotLocation(kRandoPotLookup[i].room, kRandoPotLookup[i].pos4) !=
          kRandoPotLookup[i].loc_id) {
        fprintf(stderr, "Rando_SelfCheck: pot lookup round-trip failed at %u\n", i);
        exit(2);
      }
    }
    // A (room,pos4) absent from the table resolves to 0xFFFF (the hook then stays
    // pure vanilla). Room 0x3FF is out of the 0..319 dungeon-room range.
    if (Rando_GetPotLocation(0x3FF, 0x0010) != 0xFFFF) {
      fprintf(stderr, "Rando_SelfCheck: pot lookup of a non-pot must be 0xFFFF\n");
      exit(2);
    }
  }

  // add-rando-grass-rock-shuffle Phase 4 — the same contract for the terrain
  // lookup (Rando_GetTerrainLocation (screen,pos)->LOC): strictly sorted by
  // (screen<<16|pos) + round-trips, plus the registry-digest identity that the
  // sidecar activation guard uses (a non-empty registry, so a fail-open empty
  // build is caught loudly here rather than silently at runtime).
  {
    uint32 prev = 0;
    bool first = true;
    for (uint32 i = 0; i < kRandoTerrainLookup_COUNT; i++) {
      uint32 k = ((uint32)kRandoTerrainLookup[i].screen << 16) | kRandoTerrainLookup[i].pos;
      if (!first && k <= prev) {
        fprintf(stderr, "Rando_SelfCheck: terrain lookup not strictly sorted at %u\n", i);
        exit(2);
      }
      first = false;
      prev = k;
      if (Rando_GetTerrainLocation(kRandoTerrainLookup[i].screen, kRandoTerrainLookup[i].pos) !=
          kRandoTerrainLookup[i].loc_id) {
        fprintf(stderr, "Rando_SelfCheck: terrain lookup round-trip failed at %u\n", i);
        exit(2);
      }
    }
    // Screen 0xFF is out of the 0x00..0x7F overworld-area range → not a
    // terrain object → 0xFFFF (the hook stays pure vanilla).
    if (Rando_GetTerrainLocation(0x00FF, 0x0000) != 0xFFFF) {
      fprintf(stderr, "Rando_SelfCheck: terrain lookup of a non-object must be 0xFFFF\n");
      exit(2);
    }
    // Registry-identity self-agreement: the compiled count/digest the sidecar
    // guard stamps must match the lookup table (an empty/absent registry ==
    // count 0, which the guard treats as fail-closed for terrain-active slots).
    if (Rando_CurrentTerrainRegistryCount() != (uint16)kRandoTerrainLookup_COUNT) {
      fprintf(stderr, "Rando_SelfCheck: terrain registry count disagrees with lookup\n");
      exit(2);
    }
  }

  // Dispatch wrapper coverage (§6.1).
  // When no placement table is installed, Rando_OnLocationCheck returns
  // vanilla_item_id unchanged → Rando_DispatchVanillaGrant returns the
  // vanilla LttP code unchanged.
  Placement_Install(NULL);
  if (Rando_OnLocationCheck(8, 5) != 5) {
    fprintf(stderr, "Rando_SelfCheck: pass-through OnLocationCheck failed\n");
    exit(2);
  }
  if (Rando_DispatchVanillaGrant(8, 5, 0x00) != 0x00) {
    fprintf(stderr, "Rando_SelfCheck: pass-through DispatchVanillaGrant failed\n");
    exit(2);
  }

  // add-rando-pot-sanity — quiet pot grants use the receipt inventory helper
  // without allocating receipt visuals, but some item codes still touch Link's
  // action state. Exercise the transactional carry guard directly.
  {
    RandoPotCarrySnapshot saved_carry = rando_pot_capture_carry_state();

    flag_is_link_immobilized = 0;
    flag_is_sprite_to_pick_up = 2;
    flag_is_ancilla_to_pick_up = 0;
    flag_is_sprite_to_pick_up_cached = 2;
    byte_7E0FB2 = 2;
    player_handler_timer = 6;
    link_item_in_hand = 0;
    link_state_bits = 0x80;
    link_picking_throw_state = 1;
    some_animation_timer_steps = 5;
    some_animation_timer = 0x48;
    button_mask_b_y = 0x80;
    bitfield_for_a_button = 0x80;
    button_b_frames = 7;
    link_speed_setting = 12;
    link_cant_change_direction = 1;
    link_player_handler_state = 24;
    link_pose_for_item = 0;
    link_position_mode = 0;
    link_disable_sprite_damage = 0;

    RandoPotCarrySnapshot pot_carry = rando_pot_capture_carry_state();
    flag_is_link_immobilized = 1;
    flag_is_sprite_to_pick_up = 0;
    flag_is_ancilla_to_pick_up = 1;
    flag_is_sprite_to_pick_up_cached = 0;
    byte_7E0FB2 = 0;
    player_handler_timer = 0;
    link_item_in_hand = 0x40;
    link_state_bits = 0;
    link_picking_throw_state = 2;
    some_animation_timer_steps = 0;
    some_animation_timer = 0;
    button_mask_b_y = 0;
    bitfield_for_a_button = 0;
    button_b_frames = 0;
    link_speed_setting = 0;
    link_cant_change_direction = 0;
    link_player_handler_state = 15;
    link_pose_for_item = 1;
    link_position_mode = 8;
    link_disable_sprite_damage = 1;
    rando_pot_restore_carry_state(&pot_carry);

    if (flag_is_link_immobilized != 0 ||
        flag_is_sprite_to_pick_up != 2 ||
        flag_is_ancilla_to_pick_up != 0 ||
        flag_is_sprite_to_pick_up_cached != 2 ||
        byte_7E0FB2 != 2 ||
        player_handler_timer != 6 ||
        link_item_in_hand != 0 ||
        link_state_bits != 0x80 ||
        link_picking_throw_state != 1 ||
        some_animation_timer_steps != 5 ||
        some_animation_timer != 0x48 ||
        button_mask_b_y != 0x80 ||
        bitfield_for_a_button != 0x80 ||
        button_b_frames != 7 ||
        link_speed_setting != 12 ||
        link_cant_change_direction != 1 ||
        link_player_handler_state != 24 ||
        link_pose_for_item != 0 ||
        link_position_mode != 0 ||
        link_disable_sprite_damage != 0) {
      fprintf(stderr, "Rando_SelfCheck: quiet pot grant clobbered carry state\n");
      exit(2);
    }

    rando_pot_restore_carry_state(&saved_carry);
  }

  // Build a synthetic placement table and verify dispatch routes to the
  // new LttP code. ITEM_BugCatchingNet=31 has dispatch:vanilla:0x21.
  static RandoPlacement entries[2];
  entries[0].location_id = 8;   // LOC_Link_s_Uncle
  entries[0].item_id = 31;      // ITEM_BugCatchingNet
  entries[1].location_id = 166; // LOC_Bottle_Merchant
  entries[1].item_id = 43;      // ITEM_BottleEmpty (vanilla:0x16) — identity
  RandoPlacementTable t = { entries, 2 };
  Placement_Install(&t);
  // Uncle now holds BugCatchingNet: vanilla code for that is 0x21
  if (Rando_DispatchVanillaGrant(8, 5, 0x00) != 0x21) {
    fprintf(stderr,
      "Rando_SelfCheck: dispatch did not translate Uncle→BugCatchingNet "
      "(expected 0x21, got 0x%02x)\n",
      (unsigned)Rando_DispatchVanillaGrant(8, 5, 0x00));
    exit(2);
  }
  // Bottle Merchant identity placement still grants 0x16
  if (Rando_DispatchVanillaGrant(166, 43, 0x16) != 0x16) {
    fprintf(stderr, "Rando_SelfCheck: identity dispatch failed\n");
    exit(2);
  }
  // Unknown location → vanilla fall-back
  if (Rando_DispatchVanillaGrant(0xFFFF, 5, 0x00) != 0x00) {
    fprintf(stderr, "Rando_SelfCheck: unknown-location fall-back failed\n");
    exit(2);
  }
  Placement_Install(NULL);

  // Vanilla-dispatch table boundaries (must return 0xFF for progressive items
  // and 0xFF for ids beyond the table).
  if (Rando_VanillaItemForRegistryId(0) != 0xFF) {  // ITEM_ProgressiveSword
    fprintf(stderr, "Rando_SelfCheck: ProgressiveSword should have no vanilla dispatch\n");
    exit(2);
  }
  if (Rando_VanillaItemForRegistryId(31) != 0x21) {  // ITEM_BugCatchingNet
    fprintf(stderr, "Rando_SelfCheck: BugCatchingNet vanilla dispatch wrong\n");
    exit(2);
  }
  if (Rando_VanillaItemForRegistryId(0xFFFFu) != 0xFF) {
    fprintf(stderr, "Rando_SelfCheck: out-of-range vanilla dispatch should be 0xFF\n");
    exit(2);
  }

  {
    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_flute_shovel_owned = g_rando_flute_shovel_owned;
    uint8 saved_link_item_flute = link_item_flute;
    RandoSettings saved_active_settings = g_rando_active_settings;
    bool saved_active_settings_valid = g_rando_active_settings_valid;
    // rando-exempt: self-test fabricates the rando flute/shovel grant state
    // and restores it before returning; this is not a gameplay grant site.
    g_rando_slot_active = 1;
    Settings_SetDefaults(&g_rando_active_settings);
    g_rando_active_settings_valid = true;
    g_rando_flute_shovel_owned = 0;
    link_item_flute = 0;
    Rando_GrantFluteShovel(0x14);
    if (link_item_flute != 3 ||
        g_rando_flute_shovel_owned != (kRandoFluteShovel_Flute | kRandoFluteShovel_FluteActive) ||
        Rando_FluteShovelEffectiveLevel() != 3) {
      fprintf(stderr, "Rando_SelfCheck: inactive flute pickup should grant active flute\n");
      exit(2);
    }
    Rando_GrantFluteShovel(0x13);
    if (link_item_flute != 3 ||
        g_rando_flute_shovel_owned != (kRandoFluteShovel_Shovel | kRandoFluteShovel_Flute |
                                       kRandoFluteShovel_FluteActive)) {
      fprintf(stderr, "Rando_SelfCheck: shovel pickup should not downgrade active flute\n");
      exit(2);
    }
    g_rando_active_settings.instant_flute = 0;
    g_rando_flute_shovel_owned = 0;
    link_item_flute = 0;
    Rando_GrantFluteShovel(0x14);
    if (link_item_flute != 2 ||
        g_rando_flute_shovel_owned != kRandoFluteShovel_Flute ||
        Rando_FluteShovelEffectiveLevel() != 2) {
      fprintf(stderr, "Rando_SelfCheck: instant_flute=false should leave 0x14 inactive\n");
      exit(2);
    }
    Rando_GrantFluteShovel(0x4a);
    if (link_item_flute != 3 ||
        g_rando_flute_shovel_owned != (kRandoFluteShovel_Flute | kRandoFluteShovel_FluteActive) ||
        Rando_FluteShovelEffectiveLevel() != 3) {
      fprintf(stderr, "Rando_SelfCheck: explicit active flute grant should activate\n");
      exit(2);
    }
    g_rando_slot_active = saved_slot_active;
    g_rando_active_settings = saved_active_settings;
    g_rando_active_settings_valid = saved_active_settings_valid;
    g_rando_flute_shovel_owned = saved_flute_shovel_owned;
    link_item_flute = saved_link_item_flute;
  }

  // Chest lookup — verify generated room/ordinal mappings directly. Runtime
  // delivery is exercised by the explicit transaction probes below.
#if kRandoChestLookup_COUNT > 0
  // Spot-check the generated kRandoChestLookup table against a curated subset
  // of (room, ordinal, expected_LOC_*) triples. These triples are derived
  // from audit.md §0.3.5 + the in-room ordering of the vanilla chest
  // table at $81e96e. If chest_lookup() returns the right LOC_*, the
  // dispatcher's caller bookkeeping is exercised by the placement test
  // above; here we just verify the lookup itself.
  {
    static const struct { uint16 room; uint8 ord; uint16 expected; } kCases[] = {
      {  17, 0, LOC_Sewers_Secret_Room_Left },
      {  17, 1, LOC_Sewers_Secret_Room_Middle },
      {  17, 2, LOC_Sewers_Secret_Room_Right },
      {  18, 0, LOC_Sanctuary },
      {  22, 0, LOC_Pyramid_Fairy_Left },
      {  22, 1, LOC_Pyramid_Fairy_Right },
      { 114, 0, LOC_Hyrule_Castle_Map_Chest },
      { 168, 0, LOC_Eastern_Palace_Compass_Chest },
      { 169, 0, LOC_Eastern_Palace_Big_Chest },
      {  47, 4, LOC_Kakariko_Well_Bottom },
      // unmapped fall-through (room 0xFFFE never appears in the table)
      { 0xFFFE, 0, 0xFFFFu },
    };
    for (int i = 0; i < (int)(sizeof(kCases)/sizeof(kCases[0])); ++i) {
      uint16 got = chest_lookup(kCases[i].room, kCases[i].ord);
      if (got != kCases[i].expected) {
        fprintf(stderr,
          "Rando_SelfCheck: chest_lookup(room=%u, ord=%u) = %u, expected %u\n",
          (unsigned)kCases[i].room, (unsigned)kCases[i].ord,
          (unsigned)got, (unsigned)kCases[i].expected);
        exit(2);
      }
    }
  }

#else
  // No chest table artifact present (e.g. CI build with no assets): the
  // kRandoChestLookup table is empty so chest mapping cannot be self-checked.
  fprintf(stderr,
    "Rando_SelfCheck: chest table empty (no assets extracted) - skipping "
    "chest-mapping spot-checks\n");
#endif

  // add-rando-traps — direct dispatch, non-lethal damage clamp, trap-owned
  // freeze timer, and generated dialogue buffer.
  {
    uint8 buf[32];
    if (!Rando_RenderTrapMessage(kRandoTrapDialogueId, buf) ||
        buf[0] != 24 || buf[1] != 40 || buf[2] != 46 || buf[7] != 0x75 ||
        buf[15] != 0x7f) {
      fprintf(stderr, "Rando_SelfCheck: trap message render mismatch\n");
      exit(2);
    }
    if (Rando_RenderTrapMessage(0x00B5, buf)) {
      fprintf(stderr, "Rando_SelfCheck: trap renderer should ignore non-trap ids\n");
      exit(2);
    }
    // add-*-souls — soul-name box renders "Got the <Name> Soul!" for the
    // last-queued soul; ignores non-soul dialogue ids.
    {
      uint8 sbuf[240];
      Rando_QueueSoulPickupMessage(ITEM_Soul_Npc_KingZora);
      if (!Rando_RenderSoulMessage(kRandoSoulDialogueId, sbuf) ||
          sbuf[0] != trap_ascii_to_font('G')) {
        fprintf(stderr, "Rando_SelfCheck: soul message render mismatch\n");
        exit(2);
      }
      // Row separator (0x75) and 0x7f terminator must both be present.
      bool has_row = false, has_term = false;
      for (int i = 0; i < 240; i++) {
        if (sbuf[i] == 0x75) has_row = true;
        if (sbuf[i] == 0x7f) { has_term = true; break; }
      }
      if (!has_row || !has_term) {
        fprintf(stderr, "Rando_SelfCheck: soul message missing row/terminator\n");
        exit(2);
      }
      if (Rando_RenderSoulMessage(kRandoTrapDialogueId, sbuf)) {
        fprintf(stderr, "Rando_SelfCheck: soul renderer should ignore non-soul ids\n");
        exit(2);
      }
      g_rando_soul_msg_pending = 0xFFFF;  // don't leak a pending box into runtime
    }

    uint8 saved_main = main_module_index;
    uint8 saved_sub = submodule_index;
    uint8 saved_subsub = subsubmodule_index;
    uint8 saved_saved = saved_module_for_menu;
    uint16 saved_dialogue = dialogue_message_index;
    uint8 saved_msg = messaging_module;
    uint8 saved_0223 = byte_7E0223;
    uint8 saved_health = link_health_current;
    uint8 saved_hearts_filler = link_hearts_filler;
    uint8 saved_blink = countdown_for_blink;
    uint8 saved_aux = link_auxiliary_state;
    uint8 saved_timer = link_incapacitated_timer;
    uint8 saved_vx = link_actual_vel_x;
    uint8 saved_vy = link_actual_vel_y;
    uint8 saved_vz = link_actual_vel_z;
    uint8 saved_vzc = link_actual_vel_z_copy;
    uint16 saved_z = link_z_coord;
    uint8 saved_speed = link_speed_setting;
    uint8 saved_sfx1 = sound_effect_1;
    uint8 saved_sfx2 = sound_effect_2;
    uint8 saved_trap_timer = g_rando_trap_stun_timer;
    uint8 saved_trap_effect = g_rando_trap_effect;
    uint8 saved_trap_bad_sfx = g_rando_trap_bad_sfx_timer;
    uint8 saved_trap_shove_timer = g_rando_trap_shove_timer;
    uint8 saved_trap_shove_dir = g_rando_trap_shove_dir;
    uint8 saved_trap_owns_force = g_rando_trap_owns_forced_move;
    uint8 saved_jh = joypad1H_last;
    uint8 saved_jl = joypad1L_last;
    uint8 saved_fh = filtered_joypad_H;
    uint8 saved_fl = filtered_joypad_L;
    uint16 saved_force = force_move_any_direction;
    uint8 saved_yvel = link_y_vel;
    uint8 saved_xvel = link_x_vel;
    uint8 saved_button_mask = button_mask_b_y;
    uint8 saved_a_bitfield = bitfield_for_a_button;
    uint8 saved_button_b = button_b_frames;
    uint8 saved_sword_delay = link_delay_timer_spin_attack;
    uint8 saved_spin_step = link_spin_attack_step_counter;
    uint8 saved_state_bits = link_state_bits;
    uint8 saved_pick_state = link_picking_throw_state;
    uint8 saved_grabbing = link_grabbing_wall;
    uint8 saved_diag = link_moving_against_diag_tile;
    uint8 saved_running = link_is_running;
    uint8 saved_dash_countdown = link_countdown_for_dash;
    uint8 saved_cant_dir = link_cant_change_direction;
    uint8 saved_30d = link_var30d;
    uint8 saved_30e = link_var30e;
    uint8 saved_dir = link_direction;
    uint8 saved_dir_last = link_direction_last;
    uint8 saved_dir_facing = link_direction_facing;
    uint8 saved_anim_timer_steps = some_animation_timer_steps;
    uint8 saved_swim_countdown = swimming_countdown;
    uint8 saved_swim_faster = link_maybe_swim_faster;
    uint8 saved_swim_hard = link_swim_hard_stroke;
    uint16 saved_swim1[2] = { swimcoll_var1[0], swimcoll_var1[1] };
    uint16 saved_swim3[2] = { swimcoll_var3[0], swimcoll_var3[1] };
    uint16 saved_swim5[2] = { swimcoll_var5[0], swimcoll_var5[1] };
    uint16 saved_swim7[2] = { swimcoll_var7[0], swimcoll_var7[1] };
    uint16 saved_swim9[2] = { swimcoll_var9[0], swimcoll_var9[1] };
    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_hookshot = link_something_with_hookshot;
    uint8 saved_drag = bitmask_of_dragstate;
    uint8 saved_ancilla_type[5];
    uint8 saved_ancilla_item[5];
    uint8 saved_ancilla_aux[5];
    uint8 saved_ancilla_timer[5];
    uint8 saved_ancilla_step[5];
    uint8 saved_ancilla_arr3[5];
    uint8 saved_ancilla_g[5];
    uint8 saved_ancilla_l[5];
    for (int k = 0; k < 5; k++) {
      saved_ancilla_type[k] = ancilla_type[k];
      saved_ancilla_item[k] = ancilla_item_to_link[k];
      saved_ancilla_aux[k] = ancilla_aux_timer[k];
      saved_ancilla_timer[k] = ancilla_timer[k];
      saved_ancilla_step[k] = ancilla_step[k];
      saved_ancilla_arr3[k] = ancilla_arr3[k];
      saved_ancilla_g[k] = ancilla_G[k];
      saved_ancilla_l[k] = ancilla_L[k];
    }

    static RandoPlacement trap_entries[1];
    trap_entries[0].location_id = 166;
    trap_entries[0].item_id = ITEM_TrapDamage;
    RandoPlacementTable tt = { trap_entries, 1 };
    Placement_Install(&tt);
    link_is_running = 0;
    link_health_current = 4;
    link_incapacitated_timer = 0;
    link_auxiliary_state = 0;
    link_direction_facing = 0;
    rando_clear_trap_effect();
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    // add-rando-trap-catalog — the trigger now ARMS only (no immediate damage or
    // shove); the onset applies them in the first gated tick. Assert the arm state:
    // health is still 4 (untouched), shove is unset, and onset is pending.
    if (lttp != kRandoLttpSkip || link_health_current != 4 ||
        g_rando_trap_stun_timer != 40 ||
        g_rando_trap_effect != kRandoTrapEffect_Damage ||
        g_rando_trap_onset_pending != 1 ||
        g_rando_trap_bad_sfx_timer != 8 ||
        g_rando_trap_shove_timer != 0 || g_rando_trap_shove_dir != 0 ||
        link_incapacitated_timer != 0 || link_auxiliary_state != 0 ||
        dialogue_message_index != kRandoTrapDialogueId) {
      fprintf(stderr, "Rando_SelfCheck: TrapDamage arm/dispatch mismatch\n");
      exit(2);
    }
    joypad1H_last = 0x0f;
    filtered_joypad_H = 0x0f;
    Rando_TickTrapEffects();
    if (g_rando_trap_stun_timer != 40 || g_rando_trap_bad_sfx_timer != 8 ||
        joypad1H_last != 0x0f || filtered_joypad_H != 0x0f) {
      fprintf(stderr, "Rando_SelfCheck: trap timer should pause during dialogue\n");
      exit(2);
    }
    dialogue_message_index = 0;
    main_module_index = 9;
    g_rando_trap_bad_sfx_timer = 1;
    sound_effect_1 = 0;
    sound_effect_2 = 0;
    joypad1H_last = 0x0f;
    joypad1L_last = 0x80;
    filtered_joypad_H = 0x0f;
    filtered_joypad_L = 0x80;
    force_move_any_direction = 0x000f;
    link_y_vel = 5;
    link_x_vel = 6;
    link_actual_vel_y = 7;
    link_actual_vel_x = 8;
    link_actual_vel_z = 9;
    link_actual_vel_z_copy = 10;
    button_b_frames = 8;
    link_state_bits = 0xff;
    swimcoll_var3[1] = 1;
    swimcoll_var7[0] = 0x180;
    link_swim_hard_stroke = 0xc0;
    Rando_TickTrapEffects();
    // The onset ran during this first gated tick — Damage now applied (4 -> 1),
    // and the sustain neutralized motion + advanced the shove.
    if (link_health_current != 1 ||
        g_rando_trap_stun_timer != 39 || g_rando_trap_shove_timer != 11 ||
        g_rando_trap_bad_sfx_timer != 0 ||
        (sound_effect_2 & 0x3f) != 0x26 || sound_effect_1 == 0 ||
        joypad1H_last != 0 ||
        joypad1L_last != 0 || filtered_joypad_H != 0 ||
        filtered_joypad_L != 0 || force_move_any_direction != 4 ||
        g_rando_trap_owns_forced_move == 0 ||
        link_direction != 4 || link_direction_last != 4 ||
        link_speed_setting != 12 ||
        link_y_vel != 0 || link_x_vel != 0 ||
        link_actual_vel_y != 0 || link_actual_vel_x != 0 ||
        link_actual_vel_z != 0 || link_actual_vel_z_copy != 0 ||
        button_b_frames != 0 || link_state_bits != 0 ||
        swimcoll_var3[1] != 0 || swimcoll_var7[0] != 0 ||
        link_swim_hard_stroke != 0 || link_incapacitated_timer != 0) {
      fprintf(stderr, "Rando_SelfCheck: trap timer tick did not neutralize motion\n");
      exit(2);
    }
    rando_clear_trap_effect();
    if (g_rando_trap_stun_timer != 0 || g_rando_trap_owns_forced_move != 0 ||
        force_move_any_direction != 0) {
      fprintf(stderr, "Rando_SelfCheck: trap clear should drop forced movement\n");
      exit(2);
    }
    trap_entries[0].item_id = ITEM_TrapFreeze;
    link_health_current = 0x28;
    link_incapacitated_timer = 0;
    link_auxiliary_state = 0;
    rando_clear_trap_effect();
    lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip || link_health_current != 0x28 ||
        g_rando_trap_stun_timer != 96 ||
        g_rando_trap_effect != kRandoTrapEffect_Freeze ||
        g_rando_trap_bad_sfx_timer != 8 ||
        g_rando_trap_shove_timer != 0 || g_rando_trap_shove_dir != 0 ||
        link_incapacitated_timer != 0 || link_auxiliary_state != 0 ||
        dialogue_message_index != kRandoTrapDialogueId) {
      fprintf(stderr, "Rando_SelfCheck: TrapFreeze dispatch/effect mismatch\n");
      exit(2);
    }
    dialogue_message_index = 0;
    main_module_index = 9;
    countdown_for_blink = 0;
    Rando_TickTrapEffects();
    if (g_rando_trap_stun_timer != 95 || force_move_any_direction != 0 ||
        countdown_for_blink != 10 || g_rando_trap_effect != kRandoTrapEffect_Freeze) {
      fprintf(stderr, "Rando_SelfCheck: TrapFreeze tick/pulse mismatch\n");
      exit(2);
    }
    g_rando_slot_active = 1;
    g_rando_trap_stun_timer = 0;
    link_something_with_hookshot = 0;
    bitmask_of_dragstate = 0;
    for (int k = 0; k < 5; k++) {
      rando_selfcheck_seed_bad_trap_wall_spark_residue(k);
    }
    Rando_TickTrapEffects();
    for (int k = 0; k < 5; k++) {
      if (ancilla_type[k] != 0) {
        fprintf(stderr, "Rando_SelfCheck: stale trap gravestone residue not cleared\n");
        exit(2);
      }
    }
    const int residue_slot = 2;
    rando_selfcheck_seed_bad_trap_wall_spark_residue(residue_slot);
    link_something_with_hookshot = 1;
    bitmask_of_dragstate = 0;
    Rando_TickTrapEffects();
    if (ancilla_type[residue_slot] != 0x24) {
      fprintf(stderr, "Rando_SelfCheck: hookshot-owned gravestone was cleared\n");
      exit(2);
    }
    rando_selfcheck_seed_bad_trap_wall_spark_residue(residue_slot);
    link_something_with_hookshot = 0;
    bitmask_of_dragstate = 4;
    Rando_TickTrapEffects();
    if (ancilla_type[residue_slot] != 0x24) {
      fprintf(stderr, "Rando_SelfCheck: drag-owned gravestone was cleared\n");
      exit(2);
    }
    rando_selfcheck_seed_bad_trap_wall_spark_residue(residue_slot);
    link_something_with_hookshot = 0;
    bitmask_of_dragstate = 0;
    Rando_TickTrapEffects();
    if (ancilla_type[residue_slot] != 0) {
      fprintf(stderr, "Rando_SelfCheck: stale trap gravestone residue not cleared\n");
      exit(2);
    }

    // Drive the cheap effects through the tick
    // and assert their onsets, especially the proxy-byte guards (the dominant
    // rando bug class): MagicDrain must NOT touch the magic upgrade tier, AmmoDrain
    // must NOT touch the per-frame fill counters.
    {
      uint8 s_mp = link_magic_power, s_mf = link_magic_filler, s_mc = link_magic_consumption;
      uint8 s_na = link_num_arrows, s_ib = link_item_bombs;
      uint8 s_af = link_arrow_filler, s_bf = link_bomb_filler;
      uint16 s_rg = link_rupees_goal;
      dialogue_message_index = 0;
      main_module_index = 9;
      submodule_index = 0;
      g_rando_trap_bad_sfx_timer = 0;

      link_magic_power = 0x40; link_magic_filler = 5; link_magic_consumption = 2;
      g_rando_trap_effect = kRandoTrapEffect_MagicDrain;
      g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 12;
      Rando_TickTrapEffects();
      if (link_magic_power != 0 || link_magic_filler != 0 || link_magic_consumption != 2) {
        fprintf(stderr, "Rando_SelfCheck: MagicDrain must empty meter, keep upgrade tier\n");
        exit(2);
      }

      link_num_arrows = 30; link_item_bombs = 10;
      link_arrow_filler = 7; link_bomb_filler = 9;
      g_rando_trap_effect = kRandoTrapEffect_AmmoDrain;
      g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 12;
      Rando_TickTrapEffects();
      if (link_num_arrows != 0 || link_item_bombs != 5 ||
          link_arrow_filler != 7 || link_bomb_filler != 9) {
        fprintf(stderr, "Rando_SelfCheck: AmmoDrain must zero arrows/halve bombs, keep fillers\n");
        exit(2);
      }

      link_rupees_goal = 300;
      g_rando_trap_effect = kRandoTrapEffect_RupeeDrain;
      g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 12;
      Rando_TickTrapEffects();
      if (link_rupees_goal != 200) {
        fprintf(stderr, "Rando_SelfCheck: RupeeDrain wrong amount\n");
        exit(2);
      }

      g_rando_trap_effect = kRandoTrapEffect_Reverse;
      g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 240;
      joypad1H_last = kJoypadH_Up | kJoypadH_B;
      filtered_joypad_H = kJoypadH_Left;
      Rando_TickTrapEffects();
      if (joypad1H_last != (uint8)(kJoypadH_Down | kJoypadH_B) ||
          filtered_joypad_H != kJoypadH_Right) {
        fprintf(stderr, "Rando_SelfCheck: Reverse must flip direction, keep buttons\n");
        exit(2);
      }

      g_rando_trap_effect = kRandoTrapEffect_Disarm;
      g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 150;
      joypad1H_last = kJoypadH_B | kJoypadH_Up;
      filtered_joypad_H = kJoypadH_Y | kJoypadH_Right;
      Rando_TickTrapEffects();
      if (joypad1H_last != kJoypadH_Up || filtered_joypad_H != kJoypadH_Right) {
        fprintf(stderr, "Rando_SelfCheck: Disarm must mask B/Y, keep movement\n");
        exit(2);
      }

      // Darkness — the one persistent g_ram PPU mutation with a teardown. Onset
      // saves the lit baseline then blacks the room (overworld_fixed_color_plusminus
      // = 31); a pre-empting trap (rando_clear_trap_effect) runs the teardown, which
      // restores it. Dungeon-only: set player_is_indoors so context_ok keeps it
      // Darkness instead of falling back to Shake. (Local save/restore — no scaffold
      // change.)
      {
        uint8 s_indoors = player_is_indoors, s_cd = overworld_fixed_color_plusminus;
        player_is_indoors = 1;
        overworld_fixed_color_plusminus = 8;  // a "lit" baseline to restore to
        g_rando_trap_effect = kRandoTrapEffect_Darkness;
        g_rando_trap_onset_pending = 1; g_rando_trap_stun_timer = 180;
        Rando_TickTrapEffects();
        if (overworld_fixed_color_plusminus != 31) {
          fprintf(stderr, "Rando_SelfCheck: Darkness onset must black the room (got %u)\n",
                  (unsigned)overworld_fixed_color_plusminus);
          exit(2);
        }
        rando_clear_trap_effect();  // pre-empt -> teardown restores brightness
        if (overworld_fixed_color_plusminus != 8) {
          fprintf(stderr, "Rando_SelfCheck: Darkness teardown must restore brightness (got %u)\n",
                  (unsigned)overworld_fixed_color_plusminus);
          exit(2);
        }
        player_is_indoors = s_indoors; overworld_fixed_color_plusminus = s_cd;
      }

      // Displace immediately changes modules, so it must release the pickup
      // action's bit-0 facing lock itself. Preserve other lock bits owned by
      // unrelated player mechanics.
      {
        uint8 s_main = main_module_index, s_sub = submodule_index;
        uint8 s_subsub = subsubmodule_index, s_start = which_starting_point;
        uint16 s_death4 = WORD(death_var4), s_death5 = WORD(death_var5);
        uint8 s_follower = follower_indicator;
        uint8 s_indoors = player_is_indoors, s_cant_dir = link_cant_change_direction;

        main_module_index = 9;
        submodule_index = 0;
        subsubmodule_index = 7;
        link_cant_change_direction = 3;
        rando_trap_effect_onset(kRandoTrapEffect_Teleport);
        if (link_cant_change_direction != 2 ||
            main_module_index != 6 || submodule_index != 0 || subsubmodule_index != 0 ||
            which_starting_point != 1 || WORD(death_var5) != 0 || WORD(death_var4) != 1 ||
            follower_indicator != 0 || player_is_indoors != 1) {
          fprintf(stderr, "Rando_SelfCheck: Displace warp leaked pickup facing lock\n");
          exit(2);
        }

        main_module_index = s_main;
        submodule_index = s_sub;
        subsubmodule_index = s_subsub;
        which_starting_point = s_start;
        WORD(death_var4) = s_death4;
        WORD(death_var5) = s_death5;
        follower_indicator = s_follower;
        player_is_indoors = s_indoors;
        link_cant_change_direction = s_cant_dir;
      }

      // Selector honors the category mask AND the location-compatibility flags:
      // hazard/drain masks never leak another category, Cucco is only placed at an
      // outdoor location, and Darkness only in a dungeon.
      uint8 all_loc = (uint8)(kTrapLoc_IsDungeon | kTrapLoc_IsOutdoor);
      for (uint16 loc = 0; loc < 64; loc++) {
        uint16 hz = Rando_PickTrapEffectId(0x12345678u, loc, kTrapCategory_Hazard, all_loc);
        if (hz != ITEM_TrapDamage && hz != ITEM_TrapBomb &&
            hz != ITEM_TrapAmbush && hz != ITEM_TrapCucco) {
          fprintf(stderr, "Rando_SelfCheck: hazard-only mask leaked non-hazard id %u\n",
                  (unsigned)hz);
          exit(2);
        }
        uint16 dr = Rando_PickTrapEffectId(0x12345678u, loc, kTrapCategory_Drain, all_loc);
        if (dr != ITEM_TrapRupeeDrain && dr != ITEM_TrapMagicDrain &&
            dr != ITEM_TrapAmmoDrain) {
          fprintf(stderr, "Rando_SelfCheck: drain-only mask leaked non-drain id %u\n",
                  (unsigned)dr);
          exit(2);
        }
        // An indoor, non-dungeon location (flags 0): Cucco (needs outdoor) and
        // Darkness (needs dungeon) must NEVER be selected.
        if (Rando_PickTrapEffectId(0x12345678u, loc, kTrapCategory_Hazard, 0) == ITEM_TrapCucco) {
          fprintf(stderr, "Rando_SelfCheck: Cucco selected at a non-outdoor location\n");
          exit(2);
        }
        if (Rando_PickTrapEffectId(0x12345678u, loc, kTrapCategory_Scare, 0) == ITEM_TrapDarkness) {
          fprintf(stderr, "Rando_SelfCheck: Darkness selected at a non-dungeon location\n");
          exit(2);
        }
      }

      rando_clear_trap_effect();
      link_magic_power = s_mp; link_magic_filler = s_mf; link_magic_consumption = s_mc;
      link_num_arrows = s_na; link_item_bombs = s_ib;
      link_arrow_filler = s_af; link_bomb_filler = s_bf;
      link_rupees_goal = s_rg;
    }

    Placement_Install(NULL);

    main_module_index = saved_main;
    submodule_index = saved_sub;
    subsubmodule_index = saved_subsub;
    saved_module_for_menu = saved_saved;
    dialogue_message_index = saved_dialogue;
    messaging_module = saved_msg;
    byte_7E0223 = saved_0223;
    link_health_current = saved_health;
    link_hearts_filler = saved_hearts_filler;
    countdown_for_blink = saved_blink;
    link_auxiliary_state = saved_aux;
    link_incapacitated_timer = saved_timer;
    link_actual_vel_x = saved_vx;
    link_actual_vel_y = saved_vy;
    link_actual_vel_z = saved_vz;
    link_actual_vel_z_copy = saved_vzc;
    link_z_coord = saved_z;
    link_speed_setting = saved_speed;
    sound_effect_1 = saved_sfx1;
    sound_effect_2 = saved_sfx2;
    g_rando_trap_stun_timer = saved_trap_timer;
    g_rando_trap_effect = saved_trap_effect;
    g_rando_trap_bad_sfx_timer = saved_trap_bad_sfx;
    g_rando_trap_shove_timer = saved_trap_shove_timer;
    g_rando_trap_shove_dir = saved_trap_shove_dir;
    g_rando_trap_owns_forced_move = saved_trap_owns_force;
    joypad1H_last = saved_jh;
    joypad1L_last = saved_jl;
    filtered_joypad_H = saved_fh;
    filtered_joypad_L = saved_fl;
    force_move_any_direction = saved_force;
    link_y_vel = saved_yvel;
    link_x_vel = saved_xvel;
    button_mask_b_y = saved_button_mask;
    bitfield_for_a_button = saved_a_bitfield;
    button_b_frames = saved_button_b;
    link_delay_timer_spin_attack = saved_sword_delay;
    link_spin_attack_step_counter = saved_spin_step;
    link_state_bits = saved_state_bits;
    link_picking_throw_state = saved_pick_state;
    link_grabbing_wall = saved_grabbing;
    link_moving_against_diag_tile = saved_diag;
    link_is_running = saved_running;
    link_countdown_for_dash = saved_dash_countdown;
    link_cant_change_direction = saved_cant_dir;
    link_var30d = saved_30d;
    link_var30e = saved_30e;
    link_direction = saved_dir;
    link_direction_last = saved_dir_last;
    link_direction_facing = saved_dir_facing;
    some_animation_timer_steps = saved_anim_timer_steps;
    swimming_countdown = saved_swim_countdown;
    link_maybe_swim_faster = saved_swim_faster;
    link_swim_hard_stroke = saved_swim_hard;
    swimcoll_var1[0] = saved_swim1[0];
    swimcoll_var1[1] = saved_swim1[1];
    swimcoll_var3[0] = saved_swim3[0];
    swimcoll_var3[1] = saved_swim3[1];
    swimcoll_var5[0] = saved_swim5[0];
    swimcoll_var5[1] = saved_swim5[1];
    swimcoll_var7[0] = saved_swim7[0];
    swimcoll_var7[1] = saved_swim7[1];
    swimcoll_var9[0] = saved_swim9[0];
    swimcoll_var9[1] = saved_swim9[1];
    g_rando_slot_active = saved_slot_active;
    link_something_with_hookshot = saved_hookshot;
    bitmask_of_dragstate = saved_drag;
    for (int k = 0; k < 5; k++) {
      ancilla_type[k] = saved_ancilla_type[k];
      ancilla_item_to_link[k] = saved_ancilla_item[k];
      ancilla_aux_timer[k] = saved_ancilla_aux[k];
      ancilla_timer[k] = saved_ancilla_timer[k];
      ancilla_step[k] = saved_ancilla_step[k];
      ancilla_arr3[k] = saved_ancilla_arr3[k];
      ancilla_G[k] = saved_ancilla_g[k];
      ancilla_L[k] = saved_ancilla_l[k];
    }
  }

  // §6.2 TriforcePiece counter: install a placement that grants Triforce
  // Piece at Bottle Merchant, dispatch, verify counter ticked AND the
  // dispatch returned kRandoLttpSkip (caller bypasses Link_ReceiveItem).
  {
    static RandoPlacement entries[1];
    entries[0].location_id = 166;  // Bottle Merchant
    entries[0].item_id = 52;       // ITEM_TriforcePiece
    RandoPlacementTable t = { entries, 1 };
    Placement_Install(&t);
    g_rando_triforce_piece_count = 0;
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip) {
      fprintf(stderr, "Rando_SelfCheck: TriforcePiece dispatch should return kRandoLttpSkip (got 0x%02x)\n", (unsigned)lttp);
      exit(2);
    }
    if (g_rando_triforce_piece_count != 1) {
      fprintf(stderr, "Rando_SelfCheck: TriforcePiece dispatch should tick counter\n");
      exit(2);
    }
    Placement_Install(NULL);
    g_rando_triforce_piece_count = 0;
  }

  // §6.2 HalfMagic/QuarterMagic direct-write tests. Magic upgrades are STRICTLY
  // PROGRESSIVE: either item advances link_magic_consumption by exactly one tier
  // (0->1->2), capped at 2, regardless of collection order, and never downgrades.
  {
    static RandoPlacement entries[1];
    entries[0].location_id = 166;
    entries[0].item_id = ITEM_HalfMagic;  // 41
    RandoPlacementTable t = { entries, 1 };
    Placement_Install(&t);
    const DirectGrantIconEntry *icon;
    // Draw-side/confirmation icons follow the same progressive tier as the
    // grant: the raw item identity is not visible to the player.
    link_magic_consumption = 0;
    icon = rando_direct_grant_icon_entry(
        rando_direct_grant_icon_item_pre_grant(ITEM_QuarterMagic));
    if (icon == NULL || icon->gfx != kRandoCustomGfx_HalfMagic) {
      fprintf(stderr, "Rando_SelfCheck: first magic upgrade should draw 1/2 icon\n");
      exit(2);
    }
    // HalfMagic from full -> half.
    link_magic_consumption = 0;
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip) {
      fprintf(stderr, "Rando_SelfCheck: HalfMagic dispatch should return kRandoLttpSkip (got 0x%02x)\n", (unsigned)lttp);
      exit(2);
    }
    if (link_magic_consumption != 1) {
      fprintf(stderr, "Rando_SelfCheck: HalfMagic dispatch (from 0) should advance to 1 (half)\n");
      exit(2);
    }
    icon = rando_direct_grant_icon_entry(
        rando_direct_grant_icon_item_post_grant(ITEM_HalfMagic));
    if (icon == NULL || icon->gfx != kRandoCustomGfx_HalfMagic) {
      fprintf(stderr, "Rando_SelfCheck: first magic confirmation should show 1/2 icon\n");
      exit(2);
    }
    // A SECOND magic upgrade (QuarterMagic) advances half -> quarter.
    entries[0].item_id = ITEM_QuarterMagic;  // 42
    Placement_Install(&t);
    if (rando_direct_grant_icon_item_pre_grant(ITEM_HalfMagic) != ITEM_QuarterMagic ||
        rando_direct_grant_icon_item_pre_grant(ITEM_QuarterMagic) != ITEM_QuarterMagic) {
      fprintf(stderr, "Rando_SelfCheck: second magic upgrade should draw 1/4 icon\n");
      exit(2);
    }
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_magic_consumption != 2) {
      fprintf(stderr, "Rando_SelfCheck: 2nd magic upgrade should advance to 2 (quarter)\n");
      exit(2);
    }
    icon = rando_direct_grant_icon_entry(
        rando_direct_grant_icon_item_post_grant(ITEM_QuarterMagic));
    if (icon == NULL || icon->gfx != kRandoCustomGfx_QuarterMagic) {
      fprintf(stderr, "Rando_SelfCheck: second magic confirmation should show 1/4 icon\n");
      exit(2);
    }
    // A THIRD upgrade is capped (never exceeds quarter, never downgrades).
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_magic_consumption != 2) {
      fprintf(stderr, "Rando_SelfCheck: magic upgrade past quarter should stay 2\n");
      exit(2);
    }
    // Progressive, order-independent: QuarterMagic collected FIRST gives half
    // (one tier), not quarter — the new behavior vs the old absolute jump-to-2.
    link_magic_consumption = 0;
    icon = rando_direct_grant_icon_entry(
        rando_direct_grant_icon_item_pre_grant(ITEM_QuarterMagic));
    if (icon == NULL || icon->gfx != kRandoCustomGfx_HalfMagic) {
      fprintf(stderr, "Rando_SelfCheck: QuarterMagic first should draw 1/2 icon\n");
      exit(2);
    }
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_magic_consumption != 1) {
      fprintf(stderr, "Rando_SelfCheck: QuarterMagic from 0 should advance to 1 (progressive, not jump to 2)\n");
      exit(2);
    }
    icon = rando_direct_grant_icon_entry(
        rando_direct_grant_icon_item_post_grant(ITEM_QuarterMagic));
    if (icon == NULL || icon->gfx != kRandoCustomGfx_HalfMagic) {
      fprintf(stderr, "Rando_SelfCheck: QuarterMagic first confirmation should show 1/2 icon\n");
      exit(2);
    }
    Placement_Install(NULL);
    link_magic_consumption = 0;
  }

  // Trap masquerade icons are drawn from the full good-item decoy pool, not
  // from the junk/resource items the placer replaced.
  {
    DirectGrantIconEntry first = {0, 0, 0}, icon = {0, 0, 0};
    bool saw_variation = false;
    for (uint32 i = 0; i < rando_trap_good_item_decoy_count(); i++) {
      uint16 decoy_item = rando_trap_good_item_decoy_at(i);
      if (!rando_good_item_decoy_icon(decoy_item, &icon)) {
        fprintf(stderr, "Rando_SelfCheck: trap good-item decoy %u has no icon\n",
                (unsigned)decoy_item);
        exit(2);
      }
      if (icon.gfx == 0x24 && icon.big == 0x00 && icon.oam_flags == 0x38) {
        fprintf(stderr, "Rando_SelfCheck: trap decoy pool includes green rupee icon\n");
        exit(2);
      }
      if (i == 0) {
        first = icon;
      } else if (icon.gfx != first.gfx || icon.big != first.big ||
                 icon.oam_flags != first.oam_flags) {
        saw_variation = true;
      }
    }
    if (!saw_variation) {
      fprintf(stderr, "Rando_SelfCheck: trap decoy pool should vary icons\n");
      exit(2);
    }
    if (!rando_trap_decoy_icon(ITEM_TrapDamage, 166, &first) ||
        !rando_trap_decoy_icon(ITEM_TrapFreeze, 167, &icon)) {
      fprintf(stderr, "Rando_SelfCheck: trap decoy resolver failed\n");
      exit(2);
    }
    if (first.gfx == 0x24 && first.big == 0x00 && first.oam_flags == 0x38) {
      fprintf(stderr, "Rando_SelfCheck: TrapDamage should not draw green rupee icon\n");
      exit(2);
    }
    if (icon.gfx == 0x24 && icon.big == 0x00 && icon.oam_flags == 0x38) {
      fprintf(stderr, "Rando_SelfCheck: TrapFreeze should not draw green rupee icon\n");
      exit(2);
    }
    static RandoPlacement trap_icon_entries[1];
    trap_icon_entries[0].location_id = 166;
    trap_icon_entries[0].item_id = ITEM_TrapDamage;
    RandoPlacementTable trap_icon_table = { trap_icon_entries, 1 };
    uint32 saved_features1 = enhanced_features1;
    bool saved_field_item_sprites = g_config.field_item_sprites;
    uint16 saved_last_location_id = g_last_dispatched_location_id;
    Placement_Install(&trap_icon_table);
    enhanced_features1 |= kFeatures1_RandomizerActive;
    g_config.field_item_sprites = true;
    uint8 field_gfx = 0, field_big = 0, field_oam = 0;
    if (!Rando_GetFieldItemIcon(166, ITEM_BottleEmpty,
                                &field_gfx, &field_big, &field_oam)) {
      fprintf(stderr, "Rando_SelfCheck: trap field icon did not resolve\n");
      exit(2);
    }
    g_last_dispatched_location_id = 166;
    if (!rando_trap_decoy_icon(ITEM_TrapDamage, g_last_dispatched_location_id,
                               &icon) ||
        field_gfx != icon.gfx || field_big != icon.big ||
        field_oam != icon.oam_flags) {
      fprintf(stderr, "Rando_SelfCheck: trap field/confirmation icons diverged\n");
      exit(2);
    }
    Placement_Install(NULL);
    enhanced_features1 = saved_features1;
    g_config.field_item_sprites = saved_field_item_sprites;
    g_last_dispatched_location_id = saved_last_location_id;
  }

  // §6.2 prize-item direct-write tests. Placement Prize_Crystal4 at Bottle
  // Merchant; dispatch should OR bit 0x40 into link_has_crystals.
  {
    static RandoPlacement entries[1];
    entries[0].location_id = 166;
    entries[0].item_id = ITEM_Prize_Crystal4;  // 117
    RandoPlacementTable t = { entries, 1 };
    Placement_Install(&t);
    link_has_crystals = 0;
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip) {
      fprintf(stderr, "Rando_SelfCheck: Crystal4 dispatch should return kRandoLttpSkip (got 0x%02x)\n", (unsigned)lttp);
      exit(2);
    }
    // Crystal 4 is Thieves' Town's in vanilla, so its bit is TT's 0x20. This
    // asserted 0x40 (Skull Woods') for as long as prize_item_direct_grant was
    // permuted -- a check written from the implementation instead of from the
    // vanilla composition, which is why the permutation survived review.
    // Rando_PrizeBitSelfCheck now derives all ten expectations.
    if ((link_has_crystals & 0x20) == 0) {
      fprintf(stderr, "Rando_SelfCheck: Crystal4 dispatch should OR 0x20 into link_has_crystals\n");
      exit(2);
    }
    // Same site with Prize_GreenPendant.
    entries[0].item_id = ITEM_Prize_GreenPendant;
    Placement_Install(&t);
    link_which_pendants = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if ((link_which_pendants & 0x04) == 0) {
      fprintf(stderr, "Rando_SelfCheck: GreenPendant dispatch should OR 0x04 into link_which_pendants\n");
      exit(2);
    }
    // Boss-prize receipts must mark/grant only at falling-prize pickup time.
    // Before this helper runs, falling or leaving the boss room should still
    // leave the prize uncollected so it can respawn.
    uint8 saved_slot_active = g_rando_slot_active;
    uint8 saved_checked_bitmap[kRandoCheckedBitmapBytes];
    memcpy(saved_checked_bitmap, g_rando_checked_bitmap, sizeof(saved_checked_bitmap));
    entries[0].location_id = LOC_Skull_Woods_Prize;
    entries[0].item_id = ITEM_Prize_Crystal4;
    Placement_Install(&t);
    g_rando_slot_active = 1;
    memset(g_rando_checked_bitmap, 0, kRandoCheckedBitmapBytes);
    link_has_crystals = 0;
    if (Rando_IsLocationChecked(LOC_Skull_Woods_Prize)) {
      fprintf(stderr, "Rando_SelfCheck: boss prize should start unchecked\n");
      exit(2);
    }
    RandoGrantResult prize_result = Rando_GrantBossPrizeReceipt(
        kGameDungeon_SkullWoods, 0x20,
        kRandoGrantPresentation_None, 3, 0);
    if (prize_result != kRandoGrantResult_Accepted ||
        !Rando_IsLocationChecked(LOC_Skull_Woods_Prize) ||
        // Crystal 4's bit is Thieves' Town's 0x20 -- the PLACED prize's bit,
        // not the host dungeon's. This asserted Skull Woods' own 0x40, which
        // only looked correct while prize_item_direct_grant was permuted.
        (link_has_crystals & 0x20) == 0) {
      fprintf(stderr, "Rando_SelfCheck: boss prize receipt transaction failed\n");
      exit(2);
    }
    if (Rando_GrantBossPrizeReceipt(
            kGameDungeon_SkullWoods, 0x20,
            kRandoGrantPresentation_None, 3, 0) !=
        kRandoGrantResult_AlreadyChecked) {
      fprintf(stderr, "Rando_SelfCheck: checked boss prize did not terminate replay\n");
      exit(2);
    }
    g_rando_slot_active = saved_slot_active;
    memcpy(g_rando_checked_bitmap, saved_checked_bitmap, sizeof(saved_checked_bitmap));
    Placement_Install(NULL);
    link_has_crystals = 0;
    link_which_pendants = 0;
  }

  // §6.2 per-placed-dungeon BigKey/Map/Compass direct-write tests.
  // Placement BigKey_GanonsTower (76) at Bottle Merchant; dispatch should
  // OR (0x8000 >> 13) = 0x0004 into link_bigkey (GT's game-side bit slot).
  {
    static RandoPlacement entries[1];
    entries[0].location_id = 166;
    entries[0].item_id = ITEM_BigKey_GanonsTower;  // 76
    RandoPlacementTable t = { entries, 1 };
    Placement_Install(&t);
    link_bigkey = 0;
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip) {
      fprintf(stderr, "Rando_SelfCheck: BigKey_GT dispatch should return kRandoLttpSkip\n");
      exit(2);
    }
    if (link_bigkey != 0x0004) {
      fprintf(stderr, "Rando_SelfCheck: BigKey_GT dispatch should set link_bigkey=0x0004 (got 0x%04x)\n", (unsigned)link_bigkey);
      exit(2);
    }
    // Compass_EasternPalace (88) → game-side dungeon 2 → bit (0x8000 >> 2) = 0x2000
    entries[0].item_id = ITEM_Compass_EasternPalace;
    Placement_Install(&t);
    link_compass = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_compass != 0x2000) {
      fprintf(stderr, "Rando_SelfCheck: Compass_EP dispatch should set link_compass=0x2000 (got 0x%04x)\n", (unsigned)link_compass);
      exit(2);
    }
    // Map_HCE (124) → dungeon 0 → bit 0x8000
    entries[0].item_id = 124;
    Placement_Install(&t);
    link_dungeon_map = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_dungeon_map != 0x8000) {
      fprintf(stderr, "Rando_SelfCheck: Map_HCE dispatch should set link_dungeon_map=0x8000\n");
      exit(2);
    }
    // Regression coverage for the game-order dungeons that differ from the
    // ALTTPR registry order.
    entries[0].item_id = ITEM_BigKey_SkullWoods;  // game-side dungeon 8
    Placement_Install(&t);
    link_bigkey = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_bigkey != 0x0080) {
      fprintf(stderr, "Rando_SelfCheck: BigKey_SW dispatch should set link_bigkey=0x0080 (got 0x%04x)\n",
              (unsigned)link_bigkey);
      exit(2);
    }
    entries[0].item_id = ITEM_Map_SwampPalace;  // game-side dungeon 5
    Placement_Install(&t);
    link_dungeon_map = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_dungeon_map != 0x0400) {
      fprintf(stderr, "Rando_SelfCheck: Map_SP dispatch should set link_dungeon_map=0x0400 (got 0x%04x)\n",
              (unsigned)link_dungeon_map);
      exit(2);
    }
    entries[0].item_id = ITEM_Compass_PalaceOfDarkness;  // game-side dungeon 6
    Placement_Install(&t);
    link_compass = 0;
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_compass != 0x0200) {
      fprintf(stderr, "Rando_SelfCheck: Compass_PoD dispatch should set link_compass=0x0200 (got 0x%04x)\n",
              (unsigned)link_compass);
      exit(2);
    }
    Placement_Install(NULL);
    link_bigkey = link_compass = link_dungeon_map = 0;
  }

  // §6.2 follow-on — per-dungeon SmallKey counter direct-write. Under
  // key-shuffle a small key can be collected in a dungeon other than its own
  // (e.g. a Tower-of-Hera key found during the Hyrule Castle escape); the
  // grant must credit the DESTINATION dungeon's saved counter, not the live
  // current-dungeon counter.
  {
    static RandoPlacement entries[1];
    entries[0].location_id = 166;
    entries[0].item_id = ITEM_SmallKey_TowerOfHera;  // 56 → game-side dungeon 10
    RandoPlacementTable t = { entries, 1 };

    uint16 saved_palace = cur_palace_index_x2;
    uint8 saved_keys = link_num_keys;
    uint8 saved_slot7 = link_keys_earned_per_dungeon[7];
    uint8 saved_slot8 = link_keys_earned_per_dungeon[8];
    uint8 saved_slot10 = link_keys_earned_per_dungeon[10];

    // (a) ToH key collected while in the Hyrule Castle escape (raw
    // cur_palace_index_x2 = 0). The escape's live counter must be untouched;
    // only ToH's saved slot (game-side index 10) ticks up.
    cur_palace_index_x2 = 0;
    link_num_keys = 3;  // pretend the escape has 3 keys in hand
    link_keys_earned_per_dungeon[10] = 0;
    Placement_Install(&t);
    uint8 lttp = Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (lttp != kRandoLttpSkip) {
      fprintf(stderr, "Rando_SelfCheck: SmallKey_ToH dispatch should return kRandoLttpSkip\n");
      exit(2);
    }
    if (link_num_keys != 3) {
      fprintf(stderr, "Rando_SelfCheck: SmallKey_ToH in escape must not touch live key count (got %u)\n",
              (unsigned)link_num_keys);
      exit(2);
    }
    if (link_keys_earned_per_dungeon[10] != 1) {
      fprintf(stderr, "Rando_SelfCheck: SmallKey_ToH should credit ToH slot 10 (got %u)\n",
              (unsigned)link_keys_earned_per_dungeon[10]);
      exit(2);
    }

    // (b) ToH key collected while standing in Tower of Hera (raw
    // cur_palace_index_x2 = 20 = game-side index 10). The live counter moves
    // and the saved slot stays in sync.
    cur_palace_index_x2 = 20;
    link_num_keys = 1;
    link_keys_earned_per_dungeon[10] = 1;
    Placement_Install(&t);
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_num_keys != 2 || link_keys_earned_per_dungeon[10] != 2) {
      fprintf(stderr, "Rando_SelfCheck: SmallKey_ToH in ToH should bump live+slot to 2 (got live=%u slot=%u)\n",
              (unsigned)link_num_keys, (unsigned)link_keys_earned_per_dungeon[10]);
      exit(2);
    }

    // (c) Skull Woods key collected while standing in Skull Woods (raw
    // cur_palace_index_x2 = 16 = game-side index 8). This guards the dark-world
    // game-order remap: old code credited slot 7 and left the live counter at 0.
    entries[0].item_id = ITEM_SmallKey_SkullWoods;  // 60 -> game-side dungeon 8
    cur_palace_index_x2 = 16;
    link_num_keys = 0;
    link_keys_earned_per_dungeon[7] = 5;
    link_keys_earned_per_dungeon[8] = 0;
    Placement_Install(&t);
    Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_num_keys != 1 || link_keys_earned_per_dungeon[8] != 1 ||
        link_keys_earned_per_dungeon[7] != 5) {
      fprintf(stderr, "Rando_SelfCheck: SmallKey_SW in SW should bump live+slot8 only "
                      "(got live=%u slot7=%u slot8=%u)\n",
              (unsigned)link_num_keys,
              (unsigned)link_keys_earned_per_dungeon[7],
              (unsigned)link_keys_earned_per_dungeon[8]);
      exit(2);
    }

    Placement_Install(NULL);
    cur_palace_index_x2 = saved_palace;
    link_num_keys = saved_keys;
    link_keys_earned_per_dungeon[7] = saved_slot7;
    link_keys_earned_per_dungeon[8] = saved_slot8;
    link_keys_earned_per_dungeon[10] = saved_slot10;
  }

  // Key Ring/Skeleton direct grants and derived ownership reconstruction.
  {
    static RandoPlacement entries[2];
    RandoPlacementTable t = { entries, 2 };
    entries[0].location_id = 166;
    entries[0].item_id = ITEM_SmallKey_TowerOfHera;
    entries[1].location_id = 167;
    entries[1].item_id = ITEM_SkeletonKey;

    const RandoPlacementTable *saved_table = Placement_GetActive();
    uint8 saved_active = g_rando_slot_active;
    uint16 saved_palace = cur_palace_index_x2;
    uint8 saved_live = link_num_keys;
    uint8 saved_hce = link_keys_earned_per_dungeon[kGameDungeon_HyruleCastleEscape];
    uint8 saved_toh = link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera];
    uint8 saved_generic = link_generic_keys;
    uint8 saved_world_state = g_rando_active_world_state;
    uint32 saved_features1 = enhanced_features1;
    uint8 saved_checked = g_rando_checked_bitmap[166 >> 3];
    uint16 saved_selected = g_rando_key_ring_selected_mask;
    uint16 saved_owned = g_rando_key_ring_owned_mask;
    bool saved_skeleton_present = g_rando_skeleton_key_present;
    bool saved_skeleton_owned = g_rando_skeleton_key_owned;
    bool saved_settings_valid = g_rando_active_settings_valid;
    char saved_share[sizeof g_rando_active_share_string];
    memcpy(saved_share, g_rando_active_share_string, sizeof saved_share);

    g_rando_slot_active = 1;
    g_rando_active_settings_valid = false;  // placement-derived legacy path
    g_rando_checked_bitmap[166 >> 3] &=
        (uint8)~((1u << (166 & 7)) | (1u << (167 & 7)));
    cur_palace_index_x2 = 0;  // HCE, not Tower of Hera
    link_num_keys = 0;
    link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera] = 0;

    // Tracker privacy: an uncollected ordinary key placement and an
    // uncollected Key Ring placement must look identical. Neither placement
    // row is inventory; only a receipt may change the live numeric counter.
    Placement_Install(&t);
    Rando_RebuildKeyItemOwnership();
    RandoItemView ordinary_key_view;
    Rando_FillItemView(&ordinary_key_view);
    entries[0].item_id = ITEM_KeyRing_TowerOfHera;
    Placement_Install(&t);
    Rando_RebuildKeyItemOwnership();
    RandoItemView uncollected_ring_view;
    Rando_FillItemView(&uncollected_ring_view);
    if (ordinary_key_view.dungeon_small_keys[kGameDungeon_TowerOfHera] != 0 ||
        uncollected_ring_view.dungeon_small_keys[kGameDungeon_TowerOfHera] != 0) {
      fprintf(stderr, "Rando_SelfCheck: tracker leaked uncollected key-item shape\n");
      exit(2);
    }

    link_num_keys = 3;
    if (Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16) != kRandoLttpSkip ||
        link_num_keys != 3 ||
        link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera] !=
            Rando_KeyRingGrantCount(kRandoDungeon_TowerOfHera)) {
      fprintf(stderr, "Rando_SelfCheck: off-dungeon Key Ring grant mismatch\n");
      exit(2);
    }
    RandoItemView collected_ring_view;
    Rando_FillItemView(&collected_ring_view);
    if (collected_ring_view.dungeon_small_keys[kGameDungeon_TowerOfHera] !=
        Rando_KeyRingGrantCount(kRandoDungeon_TowerOfHera)) {
      fprintf(stderr, "Rando_SelfCheck: collected Key Ring missing from numeric tracker count\n");
      exit(2);
    }

    // Never lower either live or parked state; inside the target family both
    // copies synchronize to the larger pre-existing value.
    uint8 stock = Rando_KeyRingGrantCount(kRandoDungeon_TowerOfHera);
    cur_palace_index_x2 = (uint16)(kGameDungeon_TowerOfHera << 1);
    link_num_keys = (uint8)(stock + 1);
    link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera] = (uint8)(stock + 2);
    (void)Rando_DispatchVanillaGrant(166, ITEM_BottleEmpty, 0x16);
    if (link_num_keys != (uint8)(stock + 2) ||
        link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera] != (uint8)(stock + 2)) {
      fprintf(stderr, "Rando_SelfCheck: Key Ring grant lowered/split live state\n");
      exit(2);
    }

    if (Rando_DispatchVanillaGrant(167, ITEM_BottleEmpty, 0x16) != kRandoLttpSkip ||
        !Rando_HasSkeletonKey()) {
      fprintf(stderr, "Rando_SelfCheck: Skeleton Key direct grant mismatch\n");
      exit(2);
    }

    // Clear only the cache, then prove placement + checked bits reconstruct it.
    g_rando_key_ring_selected_mask = 0;
    g_rando_key_ring_owned_mask = 0;
    g_rando_skeleton_key_present = false;
    g_rando_skeleton_key_owned = false;
    Rando_RebuildKeyItemOwnership();
    if (Rando_GetSelectedKeyRingMask() !=
            (uint16)(1u << kRandoDungeon_TowerOfHera) ||
        Rando_GetOwnedKeyRingMask() !=
            (uint16)(1u << kRandoDungeon_TowerOfHera) ||
        !Rando_HasSkeletonKey()) {
      fprintf(stderr, "Rando_SelfCheck: derived key-item ownership rebuild mismatch\n");
      exit(2);
    }

    // Even if persisted selector inputs are unavailable, installed placement
    // rows remain authoritative; a failed validation must not clear the mask.
    g_rando_active_settings_valid = true;
    g_rando_active_share_string[0] = '\0';
    g_rando_key_ring_selected_mask = 0;
    Rando_RebuildKeyItemOwnership();
    if (Rando_GetSelectedKeyRingMask() !=
        (uint16)(1u << kRandoDungeon_TowerOfHera)) {
      fprintf(stderr, "Rando_SelfCheck: placement-authoritative Key Ring mask lost\n");
      exit(2);
    }

    // Tracker/autotracker counters must show the authoritative live spend
    // immediately, not the parked value that updates only on dungeon exit.
    // Exercise the HC-proper -> Escape/Sewers fold and Retro's shared slot.
    RandoItemView key_view;
    enhanced_features1 |= kFeatures1_RandomizerActive;
    g_rando_active_world_state = kWorldState_Open;
    cur_palace_index_x2 = (uint16)(kGameDungeon_HyruleCastle << 1);
    link_keys_earned_per_dungeon[kGameDungeon_HyruleCastleEscape] = 7;
    link_num_keys = 2;
    Rando_FillItemView(&key_view);
    if (key_view.dungeon_small_keys[kGameDungeon_HyruleCastleEscape] != 2) {
      fprintf(stderr, "Rando_SelfCheck: live Hyrule Castle key counter missing from tracker view\n");
      exit(2);
    }

    g_rando_active_world_state = kWorldState_Retro;
    cur_palace_index_x2 = (uint16)(kGameDungeon_TowerOfHera << 1);
    link_generic_keys = 9;
    link_num_keys = 4;
    Rando_FillItemView(&key_view);
    if (key_view.dungeon_small_keys[15] != 4) {
      fprintf(stderr, "Rando_SelfCheck: live Retro GenericKey counter missing from tracker view\n");
      exit(2);
    }

    Placement_Install(saved_table);
    g_rando_slot_active = saved_active;
    g_rando_active_settings_valid = saved_settings_valid;
    cur_palace_index_x2 = saved_palace;
    link_num_keys = saved_live;
    link_keys_earned_per_dungeon[kGameDungeon_HyruleCastleEscape] = saved_hce;
    link_keys_earned_per_dungeon[kGameDungeon_TowerOfHera] = saved_toh;
    link_generic_keys = saved_generic;
    g_rando_active_world_state = saved_world_state;
    enhanced_features1 = saved_features1;
    g_rando_checked_bitmap[166 >> 3] = saved_checked;
    g_rando_key_ring_selected_mask = saved_selected;
    g_rando_key_ring_owned_mask = saved_owned;
    g_rando_skeleton_key_present = saved_skeleton_present;
    g_rando_skeleton_key_owned = saved_skeleton_owned;
    memcpy(g_rando_active_share_string, saved_share, sizeof saved_share);
  }

  // §9.4b — 5-icon hash widget. Two share strings with identical settings
  // (zero settings_hash) but DIFFERENT seed_u64 MUST produce different icon
  // strips. This is the regression guard for the "input is share_string,
  // not settings_hash" architectural error caught in spec round 5.
  {
    uint8 share_a[32]; memset(share_a, 0, sizeof(share_a));
    uint8 share_b[32]; memset(share_b, 0, sizeof(share_b));
    // bytes 21..28 hold seed_u64 (LE) per the share-string layout. Different
    // seeds → different SHA-256 → different icon strip.
    share_a[21] = 0x01;
    share_b[21] = 0x02;
    OamEnt buf_a[5], buf_b[5];
    memset(buf_a, 0, sizeof(buf_a));
    memset(buf_b, 0, sizeof(buf_b));
    Rando_DrawHashIcons(0, 0, (struct OamEnt *)buf_a, share_a);
    Rando_DrawHashIcons(0, 0, (struct OamEnt *)buf_b, share_b);
    int any_differ = 0;
    for (int i = 0; i < 5; ++i) {
      if (buf_a[i].charnum != buf_b[i].charnum) { any_differ = 1; break; }
    }
    if (!any_differ) {
      fprintf(stderr, "Rando_SelfCheck: hash icons identical for distinct seeds — "
                      "widget is hashing settings_hash, not share_string\n");
      exit(2);
    }
    // Identical share strings must produce identical icon strips.
    OamEnt buf_c[5];
    memset(buf_c, 0, sizeof(buf_c));
    Rando_DrawHashIcons(0, 0, (struct OamEnt *)buf_c, share_a);
    for (int i = 0; i < 5; ++i) {
      if (buf_a[i].charnum != buf_c[i].charnum) {
        fprintf(stderr, "Rando_SelfCheck: hash icons non-deterministic at index %d\n", i);
        exit(2);
      }
    }
    // Atlas size sanity: every emitted tile MUST appear in kHashIconAtlas.
    // the original loop's ternary always evaluated
    // to 0 and discarded the result via (void)expected — effectively dead
    // code. Rewrite as a real membership check.
    for (int i = 0; i < 5; ++i) {
      bool found = false;
      for (int j = 0; j < kHashIconAtlasSize; ++j) {
        if (buf_a[i].charnum == kHashIconAtlas[j]) { found = true; break; }
      }
      if (!found) {
        fprintf(stderr, "Rando_SelfCheck: hash icon tile 0x%02x at index %d not in kHashIconAtlas\n",
                (unsigned)buf_a[i].charnum, i);
        exit(2);
      }
    }
  }
}

static void tsc_die(const char *msg) {
  fprintf(stderr, "[Tracker_SelfCheck] FAIL: %s\n", msg);
  exit(2);
}

static void tsc_expect_check_visibility(uint16 loc_id, bool expected) {
  for (uint32 i = 0; i < kRandoLocationsCount; i++) {
    if (kRandoLocations[i].id != loc_id) continue;
    bool actual = Rando_LocationTypeCountsAsCheck(kRandoLocations[i].type);
    if (actual != expected) {
      char msg[128];
      snprintf(msg, sizeof(msg), "check visibility mismatch for %s",
               Rando_GetLocationName(loc_id));
      tsc_die(msg);
    }
    return;
  }
  tsc_die("check visibility test location not found");
}

// End-to-end check of the slot-recovery + live-reachability path (Phase 1a/1b),
// which the corpus / other selftests do NOT cover (the playable-slot path is
// otherwise test-free per CLAUDE.md). Generates a default placement, round-trips
// it through a sidecar slot + Rando_ActivateSidecarSlot, and asserts: settings
// recovered, prize/medallion shuffle assignments installed at activation, and
// reachability is non-empty from the empty starting inventory AND expands when a
// broad item kit is added. Pure (no file IO; g_ram inventory is zero at selftest
// time, before game init). Leaves no active slot (Deactivate at the end).
void Rando_TrackerSelfCheck(void) {
  RandoSettings s;
  Settings_SetDefaults(&s);
  uint64 seed = 0x0123456789abcdefull;
  uint32 saved_config_features0 = g_config.features0;
  uint32 saved_wanted_features0 = g_wanted_zelda_features;
  uint32 saved_enhanced_features0 = enhanced_features0;
  RandoSettings saved_active_settings = g_rando_active_settings;
  bool saved_active_settings_valid = g_rando_active_settings_valid;
  bool saved_settings_from_cold_replay = g_rando_settings_from_cold_replay;
  uint16 saved_link_bigkey = link_bigkey;

  static RandoPlacement entries[kRandoLocationCapacity];
  RandoPlacementTable table;
  table.entries = entries;
  table.count = 0;
  if (!Place_AssumedFill(&s, seed, 0, &table) && table.count == 0)
    tsc_die("placement failed");

  RandoSidecarSlot slot;
  memset(&slot, 0, sizeof(slot));
  slot.header.slot_kind = kSlotKind_Randomizer;
  slot.header.generator_version = (uint16)kGeneratorVersion;
  uint16 maxloc = 0;
  for (uint16 i = 0; i < table.count && i < kRandoLocationCapacity; i++) {
    slot.placements[i] = table.entries[i];
    if (table.entries[i].location_id > maxloc) maxloc = table.entries[i].location_id;
  }
  slot.placement_count = table.count;
  slot.header.placement_table_size = (uint16)(((uint32)maxloc + 1) * 2);
  slot.header.settings_ext_present = 1;
  slot.header.world_state = s.world_state;
  slot.header.goal = s.goal;
  Settings_CanonicalSerialize(&s, slot.settings_canonical);
  slot.header.settings_present = 1;
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  ss.seed_u64 = seed;
  Share_PackBinary(&ss, slot.header.share_string);
  slot.header.recommended_features0_present = 1;
  slot.header.recommended_features0 =
      kFeatures0_RestoreJpGlitches | kFeatures0_WidescreenVisualFixes;
  Rando_StampSlotRegistries(&slot.header);
  g_config.features0 =
      (saved_config_features0 | kFeatures0_ExtendScreen64) &
      ~kFeatures0_WidescreenVisualFixes;
  g_wanted_zelda_features =
      (saved_wanted_features0 | kFeatures0_ExtendScreen64) &
      ~kFeatures0_WidescreenVisualFixes;
  enhanced_features0 =
      (saved_enhanced_features0 | kFeatures0_ExtendScreen64) &
      ~kFeatures0_WidescreenVisualFixes;
  uint32 pre_slot_config_features0 = g_config.features0;
  uint32 pre_slot_wanted_features0 = g_wanted_zelda_features;
  uint32 pre_slot_enhanced_features0 = enhanced_features0;

  Rando_ActivateSidecarSlot(&slot);
  if (!(g_config.features0 & kFeatures0_RestoreJpGlitches) ||
      !(g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
      !(enhanced_features0 & kFeatures0_RestoreJpGlitches))
    tsc_die("recommended_features0 not applied at activate");
  if (!(g_config.features0 & kFeatures0_ExtendScreen64) ||
      !(g_wanted_zelda_features & kFeatures0_ExtendScreen64) ||
      !(enhanced_features0 & kFeatures0_ExtendScreen64))
    tsc_die("recommended_features0 must preserve non-slot live features");
  if ((g_config.features0 & kFeatures0_WidescreenVisualFixes) ||
      (g_wanted_zelda_features & kFeatures0_WidescreenVisualFixes) ||
      (enhanced_features0 & kFeatures0_WidescreenVisualFixes))
    tsc_die("recommended_features0 must ignore non-slot snapshot features");
  if (!Rando_HasActiveSettings()) tsc_die("settings not recovered after activate");
  const RandoSettings *rec = Rando_GetActiveSettings();
  if (rec == NULL || rec->world_state != s.world_state || rec->goal != s.goal ||
      rec->prize_shuffle != s.prize_shuffle || rec->medallion_shuffle != s.medallion_shuffle)
    tsc_die("recovered settings mismatch");
  if (Rando_GetDungeonPrizeAssignment() == NULL || Rando_GetMedallionAssignment() == NULL)
    tsc_die("shuffle assignments not installed at activate");

  tsc_expect_check_visibility(LOC_Zelda, false);
  tsc_expect_check_visibility(LOC_Agahnim, false);
  tsc_expect_check_visibility(LOC_Agahnim_2, false);
  tsc_expect_check_visibility(LOC_Ganon, false);
  tsc_expect_check_visibility(LOC_Bomb_Merchant, false);
  tsc_expect_check_visibility(LOC_Hyrule_Castle_Zelda_s_Cell, true);

  // Soul ownership is process-static rather than g_ram-backed. The runtime
  // count bridge must expose both enemy and NPC souls to live reachability
  // without inventing adjacent/unowned souls. This directly guards the Mini
  // Moldorm Cave tracker regression and the same failure class for NPC gates.
  uint8 saved_runtime_souls[kSoulFlagsBytes];
  memcpy(saved_runtime_souls, Souls_Flags(), sizeof(saved_runtime_souls));
  memset(Souls_Flags(), 0, kSoulFlagsBytes);
  RandoCounts soul_counts;
  Rando_BuildRuntimeCounts(&soul_counts);
  if (soul_counts.by_item_id[ITEM_Soul_MiniMoldorm] != 0 ||
      soul_counts.by_item_id[ITEM_Soul_Npc_Kiki] != 0)
    tsc_die("runtime counts invented soul ownership");
  Souls_GrantItem(ITEM_Soul_MiniMoldorm);
  Souls_GrantItem(ITEM_Soul_Npc_Kiki);
  Rando_BuildRuntimeCounts(&soul_counts);
  if (soul_counts.by_item_id[ITEM_Soul_MiniMoldorm] != 1)
    tsc_die("runtime counts missed Mini Moldorm soul");
  if (soul_counts.by_item_id[ITEM_Soul_Npc_Kiki] != 1)
    tsc_die("runtime counts missed NPC soul");
  if (soul_counts.by_item_id[ITEM_Soul_Soldier] != 0)
    tsc_die("runtime counts invented an unowned soul");
  memcpy(Souls_Flags(), saved_runtime_souls, sizeof(saved_runtime_souls));

  // Runtime counts must feed CanKillEscapeThings's explicit bomb-refill branch
  // from either live ammo or durable checked-placement history.
  uint8 saved_runtime_bombs = link_item_bombs;
  uint8 saved_runtime_bomb_filler = link_bomb_filler;
  uint8 saved_bomb_checked[kRandoCheckedBitmapBytes];
  memcpy(saved_bomb_checked, g_rando_checked_bitmap, sizeof(saved_bomb_checked));
  link_item_bombs = link_bomb_filler = 0;
  memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
  RandoCounts bomb_counts;
  Rando_BuildRuntimeCounts(&bomb_counts);
  if (bomb_counts.by_item_id[ITEM_Bombs1] != 0)
    tsc_die("runtime counts invented an escape bomb refill");
  link_item_bombs = 1;
  Rando_BuildRuntimeCounts(&bomb_counts);
  if (bomb_counts.by_item_id[ITEM_Bombs1] != 1)
    tsc_die("runtime counts missed live escape bombs");
  link_item_bombs = 0;
  const RandoPlacementTable *active_table = Placement_GetActive();
  uint16 bomb_loc = 0xFFFFu;
  for (uint16 i = 0; active_table != NULL && i < active_table->count; i++) {
    uint16 item = active_table->entries[i].item_id;
    if (item == ITEM_Bombs1 || item == ITEM_Bombs3 || item == ITEM_Bombs10) {
      bomb_loc = active_table->entries[i].location_id;
      break;
    }
  }
  if (bomb_loc == 0xFFFFu) tsc_die("tracker test placement has no bomb refill");
  Rando_MarkLocationChecked(bomb_loc);
  Rando_BuildRuntimeCounts(&bomb_counts);
  if (bomb_counts.by_item_id[ITEM_Bombs1] != 1)
    tsc_die("runtime counts forgot a checked escape bomb refill");
  link_item_bombs = saved_runtime_bombs;
  link_bomb_filler = saved_runtime_bomb_filler;
  memcpy(g_rando_checked_bitmap, saved_bomb_checked, sizeof(saved_bomb_checked));

  RandoCounts counts;
  link_bigkey = 0;
  Rando_BuildRuntimeCounts(&counts);
  if (counts.by_item_id[ITEM_BigKey_EasternPalace] != 0)
    tsc_die("runtime counts pre-granted vanilla Eastern big key");
  const RandoReachability *r0 = Logic_ComputeReachabilityFullKnowledge(&counts, rec);
  if (Reachability_HasLocation(r0, LOC_Eastern_Palace_Big_Chest))
    tsc_die("Eastern big chest reachable without live Eastern big key");
  int n0 = 0;
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (Reachability_HasLocation(r0, kRandoLocations[i].id)) n0++;
  if (n0 == 0) tsc_die("no locations reachable from the starting inventory");

  link_bigkey = Rando_DungeonBitForGameDungeon(kGameDungeon_EasternPalace);
  RandoCounts bigkey_counts;
  Rando_BuildRuntimeCounts(&bigkey_counts);
  if (bigkey_counts.by_item_id[ITEM_BigKey_EasternPalace] != 1)
    tsc_die("runtime counts missed live Eastern big key");
  const RandoReachability *rbk = Logic_ComputeReachabilityFullKnowledge(&bigkey_counts, rec);
  if (!Reachability_HasLocation(rbk, LOC_Eastern_Palace_Big_Chest))
    tsc_die("Eastern big chest not reachable with live Eastern big key");
  link_bigkey = 0;

  // A broad progression kit must strictly expand reachability (monotonic logic).
  counts.by_item_id[ITEM_ProgressiveSword] = 4;
  counts.by_item_id[ITEM_ProgressiveGlove] = 2;
  counts.by_item_id[ITEM_Hammer] = 1;
  counts.by_item_id[ITEM_Lamp] = 1;
  counts.by_item_id[ITEM_MoonPearl] = 1;
  counts.by_item_id[ITEM_Flippers] = 1;
  counts.by_item_id[ITEM_Hookshot] = 1;
  counts.by_item_id[ITEM_FireRod] = 1;
  counts.by_item_id[ITEM_ProgressiveBow] = 2;
  counts.by_item_id[ITEM_CaneOfSomaria] = 1;
  counts.by_item_id[ITEM_MagicMirror] = 1;
  counts.by_item_id[ITEM_Bombos] = counts.by_item_id[ITEM_Ether] = counts.by_item_id[ITEM_Quake] = 1;
  const RandoReachability *r1 = Logic_ComputeReachabilityFullKnowledge(&counts, rec);
  int n1 = 0;
  for (uint32 i = 0; i < kRandoLocationsCount; i++)
    if (Reachability_HasLocation(r1, kRandoLocations[i].id)) n1++;
  if (n1 <= n0) tsc_die("reachability did not expand when a full item kit was added");

  Rando_DeactivateSlot();
  uint32 forceable = kFeatures0_RestoreJpGlitches;
  uint32 expect_wanted =
      (pre_slot_wanted_features0 & ~forceable) | (pre_slot_config_features0 & forceable);
  uint32 expect_enhanced =
      (pre_slot_enhanced_features0 & ~forceable) | (pre_slot_config_features0 & forceable);
  if ((g_config.features0 & kFeatures0_RandoSeedQolMask) !=
      (pre_slot_config_features0 & kFeatures0_RandoSeedQolMask) ||
      (g_wanted_zelda_features & kFeatures0_RandoSeedQolMask) !=
      (expect_wanted & kFeatures0_RandoSeedQolMask) ||
      (enhanced_features0 & kFeatures0_RandoSeedQolMask) !=
      (expect_enhanced & kFeatures0_RandoSeedQolMask))
    tsc_die("seed QoL features leaked after deactivation");
  Settings_SetDefaults(&g_rando_active_settings);
  g_rando_active_settings.logic = 1;  // OverworldGlitches forces JP glitches.
  g_rando_active_settings_valid = true;
  g_rando_settings_from_cold_replay = true;
  g_config.features0 &= ~kFeatures0_RestoreJpGlitches;
  g_wanted_zelda_features &= ~kFeatures0_RestoreJpGlitches;
  enhanced_features0 &= ~kFeatures0_RestoreJpGlitches;
  if (!(Rando_ActiveForcedFeatures0() & kFeatures0_RestoreJpGlitches))
    tsc_die("active forced features missing RestoreJpGlitches");
  Rando_ApplyActiveForcedFeatures0();
  if ((g_config.features0 & kFeatures0_RestoreJpGlitches) ||
      !(g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
      !(enhanced_features0 & kFeatures0_RestoreJpGlitches))
    tsc_die("active forced features not applied as runtime-only overlay");
  g_wanted_zelda_features &= ~kFeatures0_RestoreJpGlitches;
  enhanced_features0 &= ~kFeatures0_RestoreJpGlitches;
  Rando_ApplySeedQolFeatures0(kFeatures0_RestoreJpGlitches);
  if ((g_config.features0 & kFeatures0_RestoreJpGlitches) ||
      !(g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
      !(enhanced_features0 & kFeatures0_RestoreJpGlitches))
    tsc_die("seed QoL forced feature leaked into global config");
  g_rando_active_settings_valid = false;
  Rando_ApplyActiveForcedFeatures0();
  if ((g_wanted_zelda_features & kFeatures0_RestoreJpGlitches) ||
      (enhanced_features0 & kFeatures0_RestoreJpGlitches))
    tsc_die("active forced feature overlay not cleared after settings invalidation");
  g_rando_active_settings = saved_active_settings;
  g_rando_active_settings_valid = saved_active_settings_valid;
  g_rando_settings_from_cold_replay = saved_settings_from_cold_replay;
  link_bigkey = saved_link_bigkey;
  g_config.features0 = saved_config_features0;
  g_wanted_zelda_features = saved_wanted_features0;
  enhanced_features0 = saved_enhanced_features0;
  fprintf(stderr, "[Tracker_SelfCheck] OK (%d -> %d reachable)\n", n0, n1);
}

// Build a Randomizer sidecar slot for the given settings/seed into `slot`
// (placement + canonical settings + share string). Mirrors the slot-build in
// Rando_TrackerSelfCheck. The static placement scratch is safe to reuse because
// Rando_ActivateSidecarSlot copies the placements via Placement_Install.
static void rando_selfcheck_build_slot(RandoSidecarSlot *slot, RandoSettings *s, uint64 seed) {
  static RandoPlacement entries[kRandoLocationCapacity];
  RandoPlacementTable table = { entries, 0 };
  if (!Place_AssumedFill(s, seed, 0, &table) && table.count == 0)
    tsc_die("StartingInventory_SelfCheck: placement failed");
  memset(slot, 0, sizeof(*slot));
  slot->header.slot_kind = kSlotKind_Randomizer;
  slot->header.generator_version = (uint16)kGeneratorVersion;
  uint16 maxloc = 0;
  for (uint16 i = 0; i < table.count && i < kRandoLocationCapacity; i++) {
    slot->placements[i] = table.entries[i];
    if (table.entries[i].location_id > maxloc) maxloc = table.entries[i].location_id;
  }
  slot->placement_count = table.count;
  slot->header.placement_table_size = (uint16)(((uint32)maxloc + 1) * 2);
  slot->header.settings_ext_present = 1;
  slot->header.world_state = s->world_state;
  slot->header.goal = s->goal;
  Settings_CanonicalSerialize(s, slot->settings_canonical);
  slot->header.settings_present = 1;
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  ss.seed_u64 = seed;
  Share_PackBinary(&ss, slot->header.share_string);
  Rando_StampSlotRegistries(&slot->header);
}

// Runtime starting-inventory injection wiring (Rando_TryGrantStartingInventory).
// Pure / in-process (no file IO, no full game init) — g_ram is zero at selftest
// time. Pins the grant + idempotency + Inverted-bunny + cold-boot-dedupe gates
// so a regression in the runtime grant wiring trips here, not only in playtest.
// Guards the slot_world_state_persistence (Inverted bunny softlock) + double-
// grant classes. Leaves no active slot and restores the g_ram cells it touches.
static void Rando_StartingInventorySelfCheck(void) {
  // (0) No active slot ⇒ injection must no-op.
  Rando_DeactivateSlot();
  g_rando_starting_inventory_granted = 0;
  if (Rando_TryGrantStartingInventory(NULL))
    tsc_die("StartingInventory: must no-op when no slot is active");

  RandoSettings s;
  RandoSidecarSlot slot;

  // (1) Inverted: Moon Pearl pre-grant on a FRESH save. The Magic Mirror is
  // deliberately NOT pre-granted (add-rando-inverted-dark-chapel-spawn) — it is a
  // found item so the spawn-select "Dark Mountain" option unlocks vanilla-style.
  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Inverted;
  rando_selfcheck_build_slot(&slot, &s, 0x0123456789abcdefull);
  Rando_ActivateSidecarSlot(&slot);
  g_rando_starting_inventory_granted = 0;
  sram_progress_indicator = 0;  // brand-new save (pre-escape)
  link_item_moon_pearl = 0;
  link_item_mirror = 0;
  if (!Rando_TryGrantStartingInventory(NULL))
    tsc_die("StartingInventory(inverted): first inject should grant");
  if (g_rando_starting_inventory_granted != 1)
    tsc_die("StartingInventory(inverted): grant gate not set");
  if (link_item_moon_pearl != 1)
    tsc_die("StartingInventory(inverted): Moon Pearl not pre-granted");
  if (link_item_mirror != 0)
    tsc_die("StartingInventory(inverted): Magic Mirror must NOT be pre-granted");
  // Idempotent: a second call in the same boot must NOT re-grant.
  if (Rando_TryGrantStartingInventory(NULL))
    tsc_die("StartingInventory(inverted): second inject must be deduped");
  Rando_DeactivateSlot();

  // (2) Standard fresh save: injection grants and sets the gate.
  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Standard;
  rando_selfcheck_build_slot(&slot, &s, 0x3ull);
  Rando_ActivateSidecarSlot(&slot);
  g_rando_starting_inventory_granted = 0;
  sram_progress_indicator = 0;
  if (!Rando_TryGrantStartingInventory(NULL))
    tsc_die("StartingInventory(standard): first inject should grant");
  if (g_rando_starting_inventory_granted != 1)
    tsc_die("StartingInventory(standard): grant gate not set");
  Rando_DeactivateSlot();

  // (3) Cold-boot dedupe: an in-progress save (progress > 0) must short-circuit
  // the escape-fill but still set the within-boot dedupe gate.
  Settings_SetDefaults(&s);
  rando_selfcheck_build_slot(&slot, &s, 0x7ull);
  Rando_ActivateSidecarSlot(&slot);
  g_rando_starting_inventory_granted = 0;
  sram_progress_indicator = 1;  // past Uncle's gift
  if (Rando_TryGrantStartingInventory(NULL))
    tsc_die("StartingInventory(cold-boot): progress>0 must short-circuit the grant");
  if (g_rando_starting_inventory_granted != 1)
    tsc_die("StartingInventory(cold-boot): dedupe gate must still be set");
  Rando_DeactivateSlot();

  // Restore the g_ram cells we touched so later selfchecks see a clean slate.
  g_rando_starting_inventory_granted = 0;
  link_item_moon_pearl = 0;
  link_item_mirror = 0;
  sram_progress_indicator = 0;
  fprintf(stderr, "[StartingInventory_SelfCheck] OK\n");
}

static void Rando_MirrorlessAwayWorldSelfCheck(void) {
  Rando_DeactivateSlot();
  savegame_is_darkworld = 0x40;
  link_item_mirror = 0;
  link_item_moon_pearl = 0;
  link_is_bunny = 1;
  link_is_bunny_mirror = 1;
  link_player_handler_state = kPlayerState_PermaBunny;
  if (Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld: inactive slot must not normalize");
  if (savegame_is_darkworld != 0x40)
    tsc_die("MirrorlessAwayWorld: inactive slot changed world");

  RandoSettings s;
  RandoSidecarSlot slot;

  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Open;
  rando_selfcheck_build_slot(&slot, &s, 0x82ull);
  Rando_ActivateSidecarSlot(&slot);
  if (!Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld(open): mirrorless DW save must normalize");
  if (savegame_is_darkworld != 0)
    tsc_die("MirrorlessAwayWorld(open): world flag not cleared");
  if (link_is_bunny || link_is_bunny_mirror ||
      link_player_handler_state == kPlayerState_PermaBunny)
    tsc_die("MirrorlessAwayWorld(open): bunny state not cleared");

  savegame_is_darkworld = 0x40;
  link_item_mirror = 2;
  if (Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld(open): Magic Mirror owner must stay in DW");
  if (savegame_is_darkworld != 0x40)
    tsc_die("MirrorlessAwayWorld(open): Mirror owner world changed");
  Rando_DeactivateSlot();

  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Standard;
  rando_selfcheck_build_slot(&slot, &s, 0x83ull);
  Rando_ActivateSidecarSlot(&slot);
  savegame_is_darkworld = 0x40;
  link_item_mirror = 0;
  link_item_moon_pearl = 1;
  if (!Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld(standard): mirrorless DW save must normalize");
  if (savegame_is_darkworld != 0)
    tsc_die("MirrorlessAwayWorld(standard): world flag not cleared");
  Rando_DeactivateSlot();

  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Retro;
  rando_selfcheck_build_slot(&slot, &s, 0x84ull);
  Rando_ActivateSidecarSlot(&slot);
  savegame_is_darkworld = 0x40;
  link_item_mirror = 0;
  if (!Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld(retro): mirrorless DW save must normalize");
  if (savegame_is_darkworld != 0)
    tsc_die("MirrorlessAwayWorld(retro): world flag not cleared");
  Rando_DeactivateSlot();

  Settings_SetDefaults(&s);
  s.world_state = kWorldState_Inverted;
  rando_selfcheck_build_slot(&slot, &s, 0x85ull);
  Rando_ActivateSidecarSlot(&slot);
  savegame_is_darkworld = 0x40;
  link_item_mirror = 0;
  if (Rando_NormalizeMirrorlessAwayWorld())
    tsc_die("MirrorlessAwayWorld(inverted): Inverted must keep DW home");
  if (savegame_is_darkworld != 0x40)
    tsc_die("MirrorlessAwayWorld(inverted): world changed");
  Rando_DeactivateSlot();

  savegame_is_darkworld = 0;
  link_item_mirror = 0;
  link_item_moon_pearl = 0;
  link_is_bunny = 0;
  link_is_bunny_mirror = 0;
  link_player_handler_state = kPlayerState_Ground;
  fprintf(stderr, "[MirrorlessAwayWorld_SelfCheck] OK\n");
}

// End-to-end install check for the DROP shuffle runtime wiring + BOSS shuffle
// render/logic assignment install. The per-module self-checks
// (BossShuffle/DropShuffle_SelfCheck) cover the ALGORITHM; the corpus is blind
// to boss/drop (orthogonal to placement). NOTHING else proves the slot-
// activation INSTALL path (Rando_ActivateSidecarSlot) regenerates + installs
// the drop table from the recovered (settings, seed) — so a regression there
// (wrong seed, off the prize/medallion RNG stream, skipped install, leak on
// reactivation) would be invisible until playtest. It ALSO guards the
// boss-shuffle RENDER install: a boss_shuffle=1 slot MUST install the boss
// assignment (render redirect + logic VM) matching ComputeAssignment, and a
// boss_shuffle=0 slot MUST leave the render a hard passthrough (vanilla).
static void Rando_ShuffleInstallSelfCheck(void) {
  RandoSettings s, off;
  RandoSidecarSlot slot;

  // (1) Drop shuffle ON: the install must (a) actually shuffle and (b) byte-
  // match DropShuffle_ComputeAssignment for the slot's (settings, seed).
  // Boss shuffle ON in the SAME slot must install the render + logic assignment
  // matching BossShuffle_ComputeAssignment. This case guards both shuffles at
  // once.
  Settings_SetDefaults(&s);
  s.boss_shuffle = 1;
  s.drop_shuffle = 1;
  const uint64 seedA = 0xA17F0001B0552233ull;
  rando_selfcheck_build_slot(&slot, &s, seedA);
  Rando_ActivateSidecarSlot(&slot);

  uint8 exp_drop[kDropTableEntryCount];
  bool fb = false;
  DropShuffle_ComputeAssignment(&s, seedA, exp_drop, &fb);

  // Non-identity sanity: the chosen seed must produce a real drop shuffle, else
  // the match assertion below would pass trivially against the vanilla table.
  off = s;
  off.drop_shuffle = 0;
  uint8 van_drop[kDropTableEntryCount];
  bool fb2 = false;
  DropShuffle_ComputeAssignment(&off, seedA, van_drop, &fb2);
  bool drop_diff = false;
  for (uint8 i = 0; i < kDropTableEntryCount; i++) if (exp_drop[i] != van_drop[i]) drop_diff = true;
  if (!drop_diff) tsc_die("ShuffleInstall: drop_shuffle on produced the identity (test seed is not a shuffle)");

  // Drop: installed runtime table must equal ComputeAssignment.
  for (uint8 i = 0; i < kDropTableEntryCount; i++)
    if (DropShuffle_Lookup(i) != exp_drop[i])
      tsc_die("ShuffleInstall: installed drop table != ComputeAssignment(settings, seed)");
  // Boss: the render install must be LIVE for a boss_shuffle=1 slot, must match
  // BossShuffle_ComputeAssignment for (settings, base seed), the LOGIC assignment
  // must be installed too, and the render redirect must fire for >=1 boss room
  // (else the test seed is not a real shuffle and these checks pass trivially).
  {
    uint8 exp_boss[16];
    BossShuffle_ComputeAssignment(&s, seedA, exp_boss);
    for (uint8 d = 0; d < 13; d++)
      if (BossShuffle_GetForDungeon(d) != exp_boss[d])
        tsc_die("ShuffleInstall: installed boss assignment != ComputeAssignment(settings, seed)");
    if (Rando_GetBossAssignment() == NULL)
      tsc_die("ShuffleInstall: boss LOGIC assignment not installed for a boss_shuffle=1 slot");
    static const uint16 kBossRoomsSC[10] = {200,51,7,90,6,41,172,222,144,164};
    bool any_redirect = false;
    for (uint8 i = 0; i < 10; i++)
      if (BossShuffle_RenderHomeRoom(kBossRoomsSC[i]) != 0xFFFF) any_redirect = true;
    if (!any_redirect)
      tsc_die("ShuffleInstall: boss_shuffle=1 produced no render redirect (test seed is not a shuffle)");
  }

  // Teardown reverts the drop table to a hard passthrough.
  Rando_DeactivateSlot();
  if (DropShuffle_Lookup(5) != 5)
    tsc_die("ShuffleInstall: drop table not torn down on deactivate");
  if (BossShuffle_GetForDungeon(1) != 0xFF || Rando_GetBossAssignment() != NULL)
    tsc_die("ShuffleInstall: boss assignment not torn down on deactivate");
  if (BossShuffle_RenderHomeRoom(200) != 0xFFFF)
    tsc_die("ShuffleInstall: boss render redirect not a passthrough after teardown");

  // (2) Reactivation with a DIFFERENT seed must OVERWRITE the drop table and
  // boss assignment (no stale leak).
  const uint64 seedB = 0xB0552244C0FFEE99ull;
  rando_selfcheck_build_slot(&slot, &s, seedB);
  Rando_ActivateSidecarSlot(&slot);
  uint8 expB[kDropTableEntryCount];
  bool fb3 = false;
  DropShuffle_ComputeAssignment(&s, seedB, expB, &fb3);
  for (uint8 i = 0; i < kDropTableEntryCount; i++)
    if (DropShuffle_Lookup(i) != expB[i])
      tsc_die("ShuffleInstall: reactivation did not overwrite the prior slot's drop table");
  {
    uint8 expBoss[16];
    BossShuffle_ComputeAssignment(&s, seedB, expBoss);
    for (uint8 d = 0; d < 13; d++)
      if (BossShuffle_GetForDungeon(d) != expBoss[d])
        tsc_die("ShuffleInstall: reactivation did not overwrite the prior slot boss assignment");
  }
  Rando_DeactivateSlot();

  // (3) Drop OFF slot: the install still runs (identity), proven because we
  // enter from a torn-down state — a skipped install would also read identity,
  // so re-confirm via a deactivate/activate boundary leaving the identity table.
  Settings_SetDefaults(&s);  // boss/drop default off
  const uint64 seedC = 0x0FF0C0DE0FF0C0DEull;
  rando_selfcheck_build_slot(&slot, &s, seedC);
  Rando_ActivateSidecarSlot(&slot);
  for (uint8 i = 0; i < kDropTableEntryCount; i++)
    if (DropShuffle_Lookup(i) != i)
      tsc_die("ShuffleInstall: off-slot drop table must be the identity");
  // Boss off: installed (vanilla identity) but the render redirect MUST be a hard
  // passthrough for every boss room -> byte-identical to vanilla.
  {
    static const uint16 kBossRoomsOff[10] = {200,51,7,90,6,41,172,222,144,164};
    for (uint8 i = 0; i < 10; i++)
      if (BossShuffle_RenderHomeRoom(kBossRoomsOff[i]) != 0xFFFF)
        tsc_die("ShuffleInstall: boss_shuffle=0 must produce no render redirect (vanilla)");
  }
  Rando_DeactivateSlot();

  // Baked cave-arrival table resolves BY NAME against the live interior pool.
  // This is the guard for the drift class that motivated the name keying: while
  // the table was positional, splitting two interiors mid-list shifted eight
  // rows onto the wrong door and nothing noticed for the whole life of the bug
  // (a wrong arrival is a valid overworld position, so it never crashes — the
  // player just emerges at some other cave). Assert every row still binds, and
  // that no two rows claim the same interior.
  {
    Rando_LoadArrivalCaptureIfNeeded();
    int n = Entrance_CaveInteriorCount();
    const uint16 *ent_area = kOverworld_Entrance_Area;
    uint32 n_rows = kOverworld_Entrance_Id_SIZE;
    // Half of what follows cross-checks the committed registry against the LIVE
    // entrance tables (assets 126 / Entrance_Area). On a ROM-less checkout those
    // are absent — n_rows is 0 and ent_area is NULL — which is not drift, it is
    // "nothing to compare against". Without this split the area check could never
    // succeed and the very first baked row killed the whole runtime self-check
    // group in the assetless CI profile. The name binding, the one-row-per-
    // interior rule, the capture-load assertion, the world-flag agreement, and
    // the row->interior map are all derived from committed data and still run.
    bool have_entrance_assets = (n_rows != 0 && ent_area != NULL);
    uint8 seen[kEntranceMaxInteriors];
    memset(seen, 0, sizeof(seen));
    for (int b = 0; b < kCaveArrivalBakedCount; b++) {
      const char *want = kCaveArrivalBaked[b].interior_id;
      int target = -1;
      for (int i = 0; i < n && i < kEntranceMaxInteriors; i++)
        if (strcmp(Entrance_CaveInteriorName(i), want) == 0) { target = i; break; }
      if (target < 0)
        tsc_die("ShuffleInstall: baked cave arrival names an interior that no longer "
                "exists — re-bake or rename, do not let it silently degrade");
      if (seen[target])
        tsc_die("ShuffleInstall: two baked cave-arrival rows claim one interior");
      seen[target] = 1;
      if (!g_cave_capture[target].valid)
        tsc_die("ShuffleInstall: baked cave arrival did not load onto its interior");
      // The block's first word is overworld_area_index_exit — the area of the door
      // the capture was taken at. Two things follow, and neither was checked while
      // a row shipped whose world flags contradicted its own area:
      //  (a) it must be one of THIS interior's door rows' areas, which also
      //      cross-checks the registry's door_rows against the live asset table;
      //  (b) the replayed world flags must agree with that area's dark bit,
      //      because Rando_ReplayCaveArrival writes both verbatim and save_dark
      //      is SRAM-persisted.
      const RandoCaveArrival *e = &kCaveArrivalBaked[b].arrival;
      uint16 blk_area = (uint16)(e->block[0] | (e->block[1] << 8));
      const uint8 *rows = NULL;
      int nrows = Entrance_CaveInteriorDoorRows(target, &rows);
      if (have_entrance_assets) {
        bool area_ok = false;
        for (int r = 0; r < nrows; r++)
          if (rows[r] < n_rows && ent_area[rows[r]] == blk_area) area_ok = true;
        if (!area_ok)
          tsc_die("ShuffleInstall: baked cave arrival's recorded overworld area is not "
                  "any of its interior's door rows — it was captured at a different door");
      }
      bool dark = (blk_area & 0x40) != 0;
      if ((e->is_dark != 0) != dark || (e->save_dark != 0) != dark)
        tsc_die("ShuffleInstall: baked cave arrival's world flags disagree with its own "
                "recorded area — replaying it would write the wrong world to SRAM");
    }
    // Every door row must carry an entrance id its entry declares. The row->entry
    // map is a committed constant; this is the runtime cross-check against the
    // live table (the offline one is skipped in assetless profiles).
    for (int i = 0; i < n && i < kEntranceMaxInteriors; i++) {
      const uint8 *rows = NULL;
      int nrows = Entrance_CaveInteriorDoorRows(i, &rows);
      for (int r = 0; r < nrows; r++) {
        // Registry-internal: holds with or without the asset blob.
        if (Entrance_InteriorOfDoorRow(rows[r]) != i)
          tsc_die("ShuffleInstall: cave door row does not resolve to its own interior");
        if (!have_entrance_assets)
          continue;  // the remaining two compare against the LIVE table
        if (rows[r] >= n_rows)
          tsc_die("ShuffleInstall: cave door row past the end of the live entrance table");
        if (!Entrance_InteriorDeclaresEntranceId(
                i, ((const uint8 *)kOverworld_Entrance_Id)[rows[r]]))
          tsc_die("ShuffleInstall: a cave door row's LIVE entrance id is not one its "
                  "interior declares — registry/asset drift");
      }
    }
    fprintf(stderr, "[Rando_CaveArrivalBakeSelfCheck] OK (%d/%d doors baked, world flags "
                    "agree%s; the rest degrade to the coupled exit)\n",
            kCaveArrivalBakedCount, n,
            have_entrance_assets ? " and areas cross-check against the live table"
                                 : "; area/entrance-id cross-checks SKIPPED (no assets)");
  }

  fprintf(stderr, "[Rando_ShuffleInstallSelfCheck] OK\n");
}

// guard — the entrance-shuffle digest (and therefore the installed
// overlay, which shares Entrance_ComputeLayout) must read the PRISTINE vanilla
// entrance-id table even while the Inverted static override owns
// g_asset_ptrs[126]. Repro of the field bug: generating an entrance-shuffle
// seed from the native window while an Inverted slot was active baked a digest
// over the INVERTED table into the sidecar (permanent false refusal after
// restart), and activating a clean entrance slot over a still-installed
// Inverted slot false-refused once. Asserts digest(pristine) ==
// digest(Inverted installed) == digest(after teardown) for a fixed
// (seed, axes, attempt). The selftest runs without zelda3_assets.dat, so any
// absent shadowed asset gets a synthetic all-zero source for the duration
// (restored after); with real assets loaded the real tables are used.
static void Rando_EntranceContaminationSelfCheck(void) {
  enum { kSynthSize = 0x100, kMaxShadowed = 32 };
  static const uint8 zero_src[kSynthSize];  // shared read-only synthetic source
  uint8 idx[kMaxShadowed];
  bool synth[kMaxShadowed] = { false };
  int n = InvertedEntrances_ShadowedAssets(idx, kMaxShadowed);
  if (n > kMaxShadowed)
    tsc_die("EntranceContamination: shadowed-asset list outgrew kMaxShadowed");
  for (int i = 0; i < n; i++) {
    if (g_asset_ptrs[idx[i]] == NULL) {
      g_asset_ptrs[idx[i]] = zero_src;
      g_asset_sizes[idx[i]] = kSynthSize;
      synth[i] = true;
    }
  }

  // Fixed entrance-shuffle header: cave shuffle, Open world, attempt 0, fixed
  // seed in share_string bytes [21..28] (the layout SlotSeedFromShareString reads).
  RandoSlotHeader h;
  memset(&h, 0, sizeof(h));
  h.slot_kind = kSlotKind_Randomizer;
  h.generator_version = (uint16)kGeneratorVersion;
  h.entrance_axes = kEntranceAxis_ShuffleCaves;
  h.settings_ext_present = 1;
  h.world_state = (uint8)kWorldState_Open;
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  ss.seed_u64 = 0xC0A7A317D16E5713ull;
  Share_PackBinary(&ss, h.share_string);

  uint32 d0 = Rando_EntranceLayoutDigest24(&h);
  if (d0 == 0) tsc_die("EntranceContamination: baseline digest unavailable");

  // Install the REAL Inverted override (headless-safe: it only repoints asset
  // pointers), then prove the digest is computed over the pristine table, not
  // the live (inverted) one.
  InvertedEntrances_Install((uint8)kWorldState_Inverted);
  const uint8 *saved = InvertedEntrances_SavedEntranceIdOrig();
  if (saved == NULL)
    tsc_die("EntranceContamination: inverted install did not take (test setup)");
  uint32 len = kOverworld_Entrance_Id_SIZE;
  if (len == 0 ||
      memcmp((const uint8 *)kOverworld_Entrance_Id, saved, len) == 0)
    tsc_die("EntranceContamination: live table identical to pristine (trivial test)");
  uint32 d1 = Rando_EntranceLayoutDigest24(&h);
  InvertedEntrances_Teardown();
  uint32 d2 = Rando_EntranceLayoutDigest24(&h);

  // Restore the synthetic sources before any assertion can exit. (tsc_die
  // exits the process, so restoration only matters on the OK path, but keep
  // the asset table clean before the checks for hygiene.)
  for (int i = 0; i < n; i++) {
    if (synth[i]) {
      g_asset_ptrs[idx[i]] = NULL;
      g_asset_sizes[idx[i]] = 0;
    }
  }
  if (d1 != d0)
    tsc_die("EntranceContamination: digest drifted while the Inverted override owned asset 126");
  if (d2 != d0)
    tsc_die("EntranceContamination: digest drifted after Inverted teardown");
  fprintf(stderr, "[Rando_EntranceContaminationSelfCheck] OK\n");
}

// guard (+ assignment arm) — generate-then-reinstall must
// restore the ACTIVE slot's LOGIC overlays (entrance region/edge overrides +
// door logic layout) AND the logic-VM prize/medallion/boss assignment
// pointers, and with NO active slot the reinstall must be a safe clearing
// no-op with the assignments NULLed (fail-closed). Fakes the active-slot
// replay inputs (g_rando_active_header / g_rando_active_door_logic /
// g_rando_active_settings_valid + the active assignment arrays
// — the exact stores Rando_ReinstallActiveSlotLogicOverlays reads) directly,
// since full Rando_ActivateSidecarSlot needs a real sidecar slot. Entrance and
// door arms are exercised on SEPARATE fake slots (the two shuffles are
// mutually exclusive in real slots per apply_derived_rules). Headless-safe:
// synthesizes asset 126 when absent (the logic overrides derive from
// kCaveInteriors/kDungeons + the permutation, not the id-table bytes).
static uint64 ReinstallCheck_FoldLogicState(void) {
  // FNV-1a over the observable override state, via the same getters the
  // reachability engine uses. Bounds mirror rando_logic.c's stores
  // (kEntranceRegionOverrideMax = 512, kEntranceEdgeOverrideMax = 64); the
  // getters bound-check, so over-iteration is harmless.
  uint64 h = 0xcbf29ce484222325ull;
  for (uint32 i = 0; i < 512; i++) {
    uint16 v = Rando_GetEntranceRegionOverride((uint16)i);
    h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
    h = (h ^ (v >> 8)) * 0x100000001b3ull;
  }
  for (uint32 i = 0; i < 64; i++) {
    uint16 v = Rando_GetEntranceEdgeOverride((uint16)i);
    h = (h ^ (v & 0xFF)) * 0x100000001b3ull;
    h = (h ^ (v >> 8)) * 0x100000001b3ull;
  }
  return h;
}

static void Rando_ReinstallOverlaysSelfCheck(void) {
  static const uint8 zero_ids[0x100];
  bool synth = false;
  if (g_asset_ptrs[126] == NULL) {
    g_asset_ptrs[126] = zero_ids;
    g_asset_sizes[126] = sizeof(zero_ids);
    synth = true;
  }

  // Generation-style clobber values for the assignment stores: stand-ins for
  // the placer's per-run assignments (boss + prize/medallion) that
  // Place_AssumedFill installs on every run.
  static uint8 gen_prize[kRandoDungeonCount];
  static uint8 gen_medallion[kRandoMedallionEntranceCount];
  static uint8 gen_boss[16];
  for (uint32 i = 0; i < (uint32)sizeof(gen_prize); i++)
    gen_prize[i] = (uint8)(0x10 + i);
  for (uint32 i = 0; i < (uint32)sizeof(gen_medallion); i++)
    gen_medallion[i] = (uint8)(0x20 + i);
  for (uint32 i = 0; i < (uint32)sizeof(gen_boss); i++)
    gen_boss[i] = (uint8)(0x30 + i);

  // --- 1) No active slot: install junk into all the stores (as a clobber
  // would), reinstall, and assert everything is CLEARED (the safe no-op) and
  // the assignment trio is NULL (fail-closed, mirroring activation's
  // !settings_valid arm).
  g_rando_active_header_valid = false;
  g_rando_active_door_logic = false;
  g_rando_active_settings_valid = false;
  Rando_BeginEntranceRegionOverrides();
  Rando_SetEntranceRegionOverride(3, 7);
  Rando_BeginEntranceEdgeOverrides();
  Rando_SetEntranceEdgeOverride(4, 9);
  static DoorShuffleLayout junk_layout;  // pointer install only; never read
  Rando_SetDoorLogicLayout(&junk_layout, 1);
  Rando_SetDungeonPrizeAssignment(gen_prize);
  Rando_SetMedallionAssignment(gen_medallion);
  Rando_SetBossAssignment(gen_boss);
  Rando_ReinstallActiveSlotLogicOverlays();
  uint16 mask = 0xFFFF;
  if (Rando_GetEntranceRegionOverride(3) != 0xFFFF ||
      Rando_GetEntranceEdgeOverride(4) != 4 ||
      Rando_GetDoorLogicLayout(&mask) != NULL || mask != 0)
    tsc_die("ReinstallOverlays: no-active-slot reinstall did not clear the stores");
  if (Rando_GetDungeonPrizeAssignment() != NULL ||
      Rando_GetMedallionAssignment() != NULL ||
      Rando_GetBossAssignment() != NULL)
    tsc_die("ReinstallOverlays: no-active-slot reinstall did not NULL the assignments (fail-closed)");

  // --- 2a) Entrance arm: fake an active cave+dungeon-shuffle slot, install
  // via the helper, digest the logic state, clobber exactly as a generation
  // does, reinstall, and assert the digest round-trips.
  RandoSlotHeader h;
  memset(&h, 0, sizeof(h));
  h.slot_kind = kSlotKind_Randomizer;
  h.generator_version = (uint16)kGeneratorVersion;
  h.entrance_axes = kEntranceAxis_ShuffleCaves | kEntranceAxis_ShuffleDungeons;
  h.settings_ext_present = 1;
  h.world_state = (uint8)kWorldState_Open;
  ShareString ss;
  memset(&ss, 0, sizeof(ss));
  ss.version = (uint8)kGeneratorVersion;
  ss.seed_u64 = 0x5EEDF00DCAFE1234ull;
  Share_PackBinary(&ss, h.share_string);
  g_rando_active_header = h;
  g_rando_active_header_valid = true;
  g_rando_active_door_logic = false;
  Rando_ReinstallActiveSlotLogicOverlays();
  uint64 d_pre = ReinstallCheck_FoldLogicState();
  Entrance_ClearRegionOverrides();   // == Rando_PlaceWithEntrances' clears
  Entrance_ClearEdgeOverrides();
  Rando_SetDoorLogicLayout(NULL, 0); // == Rando_ClearGenerationLogicOverlays
  if (ReinstallCheck_FoldLogicState() == d_pre)
    tsc_die("ReinstallOverlays: cleared state digests equal (trivial test)");
  Rando_ReinstallActiveSlotLogicOverlays();
  if (ReinstallCheck_FoldLogicState() != d_pre)
    tsc_die("ReinstallOverlays: entrance overrides did not round-trip");

  // --- 2b) Door arm on a separate fake slot (entrance_axes = 0). Find an
  // attempt the generator accepts (mirrors generation's retry loop), install
  // via the helper, clobber, reinstall, and assert pointer/mask/digest match.
  memset(&h, 0, sizeof(h));
  h.slot_kind = kSlotKind_Randomizer;
  h.generator_version = (uint16)kGeneratorVersion;
  h.settings_ext_present = 1;
  h.world_state = (uint8)kWorldState_Open;
  Share_PackBinary(&ss, h.share_string);
  uint32 datt = 0xFFFFFFFF;
  for (uint32 a = 0; a < 16; a++) {
    if (DoorShuffle_Generate(ss.seed_u64, a, kDoorShuffle_MvpDungeonMask,
                             kPotShuffle_Off, 0, kEnemyDropChecks_Off,
                             &s_active_door_layout)) {
      datt = a;
      break;
    }
  }
  if (datt == 0xFFFFFFFF)
    tsc_die("ReinstallOverlays: no door layout generated in 16 attempts (test setup)");
  h.door_attempt = (uint8)datt;
  h.door_digest24 = DoorShuffle_LayoutDigest(&s_active_door_layout) & 0xFFFFFFu;
  g_rando_active_header = h;
  g_rando_active_header_valid = true;
  g_rando_active_door_logic = true;
  Settings_SetDefaults(&g_rando_active_settings);
  g_rando_active_settings.door_shuffle = kDoorShuffle_Basic;
  g_rando_active_settings_valid = true;
  Rando_ReinstallActiveSlotLogicOverlays();
  uint16 mask_pre = 0;
  const DoorShuffleLayout *lp = Rando_GetDoorLogicLayout(&mask_pre);
  if (lp == NULL || mask_pre == 0)
    tsc_die("ReinstallOverlays: door reinstall did not install a layout");
  uint32 ddig_pre = DoorShuffle_LayoutDigest(lp);
  Rando_SetDoorLogicLayout(NULL, 0);  // generation-style clobber
  Rando_ReinstallActiveSlotLogicOverlays();
  uint16 mask_post = 0;
  lp = Rando_GetDoorLogicLayout(&mask_post);
  if (lp == NULL || mask_post != mask_pre || DoorShuffle_LayoutDigest(lp) != ddig_pre)
    tsc_die("ReinstallOverlays: door logic layout did not round-trip");

  // --- 2c) Assignment arm: fake a valid active-settings
  // capture with recognizable bytes, install via the helper, clobber the
  // three logic-VM assignment stores exactly as a generation does, reinstall,
  // and assert the ACTIVE bytes are back. Then flip settings_valid off and
  // assert the fail-closed NULL arm.
  g_rando_active_door_logic = false;  // keep the door arm cheap/no-op here
  for (uint32 i = 0; i < (uint32)sizeof(g_rando_active_prize_assignment); i++)
    g_rando_active_prize_assignment[i] = (uint8)(0x40 + i);
  for (uint32 i = 0; i < (uint32)sizeof(g_rando_active_medallion_assignment); i++)
    g_rando_active_medallion_assignment[i] = (uint8)(0x60 + i);
  for (uint32 i = 0; i < (uint32)sizeof(g_rando_active_boss_assignment); i++)
    g_rando_active_boss_assignment[i] = (uint8)(0x80 + i);
  g_rando_active_settings_valid = true;
  Rando_ReinstallActiveSlotLogicOverlays();
  if (Rando_GetDungeonPrizeAssignment() == NULL ||
      Rando_GetMedallionAssignment() == NULL ||
      Rando_GetBossAssignment() == NULL ||
      memcmp(Rando_GetDungeonPrizeAssignment(), g_rando_active_prize_assignment,
             sizeof(g_rando_active_prize_assignment)) != 0 ||
      memcmp(Rando_GetMedallionAssignment(), g_rando_active_medallion_assignment,
             sizeof(g_rando_active_medallion_assignment)) != 0 ||
      memcmp(Rando_GetBossAssignment(), g_rando_active_boss_assignment,
             sizeof(g_rando_active_boss_assignment)) != 0)
    tsc_die("ReinstallOverlays: active-settings reinstall did not install the active assignments");
  Rando_SetDungeonPrizeAssignment(gen_prize);      // == place_assumed_fill_attempt
  Rando_SetMedallionAssignment(gen_medallion);     // == place_assumed_fill_attempt
  Rando_SetBossAssignment(gen_boss);               // == Place_AssumedFill
  if (Rando_GetDungeonPrizeAssignment() == NULL ||
      Rando_GetDungeonPrizeAssignment()[0] != 0x10 ||
      Rando_GetMedallionAssignment() == NULL ||
      Rando_GetMedallionAssignment()[0] != 0x20 ||
      Rando_GetBossAssignment() == NULL ||
      Rando_GetBossAssignment()[0] != 0x30)
    tsc_die("ReinstallOverlays: assignment clobber did not take (trivial test)");
  Rando_ReinstallActiveSlotLogicOverlays();
  if (Rando_GetDungeonPrizeAssignment() == NULL ||
      Rando_GetMedallionAssignment() == NULL ||
      Rando_GetBossAssignment() == NULL ||
      memcmp(Rando_GetDungeonPrizeAssignment(), g_rando_active_prize_assignment,
             sizeof(g_rando_active_prize_assignment)) != 0 ||
      memcmp(Rando_GetMedallionAssignment(), g_rando_active_medallion_assignment,
             sizeof(g_rando_active_medallion_assignment)) != 0 ||
      memcmp(Rando_GetBossAssignment(), g_rando_active_boss_assignment,
             sizeof(g_rando_active_boss_assignment)) != 0)
    tsc_die("ReinstallOverlays: assignments did not round-trip after a generation-style clobber");
  // Generation never writes the ACTIVE arrays' bytes — assert the source
  // contents survived untouched, not just the installed copy.
  if (Rando_GetDungeonPrizeAssignment()[0] != 0x40 ||
      Rando_GetMedallionAssignment()[0] != 0x60 ||
      Rando_GetBossAssignment()[15] != 0x8F)
    tsc_die("ReinstallOverlays: active assignment bytes were corrupted");
  // Fail-closed arm: invalid settings capture must NULL all three even with a
  // generation clobber still installed.
  g_rando_active_settings_valid = false;
  Rando_SetDungeonPrizeAssignment(gen_prize);
  Rando_SetMedallionAssignment(gen_medallion);
  Rando_SetBossAssignment(gen_boss);
  Rando_ReinstallActiveSlotLogicOverlays();
  if (Rando_GetDungeonPrizeAssignment() != NULL ||
      Rando_GetMedallionAssignment() != NULL ||
      Rando_GetBossAssignment() != NULL)
    tsc_die("ReinstallOverlays: invalid-settings reinstall did not fail closed to NULL assignments");

  // Restore the idle state (and the asset table when synthesized).
  g_rando_active_header_valid = false;
  g_rando_active_door_logic = false;
  g_rando_active_settings_valid = false;
  memset(g_rando_active_prize_assignment, 0, sizeof(g_rando_active_prize_assignment));
  memset(g_rando_active_medallion_assignment, 0, sizeof(g_rando_active_medallion_assignment));
  memset(g_rando_active_boss_assignment, 0, sizeof(g_rando_active_boss_assignment));
  Entrance_ClearRegionOverrides();
  Entrance_ClearEdgeOverrides();
  Rando_SetDoorLogicLayout(NULL, 0);
  Rando_SetDungeonPrizeAssignment(NULL);
  Rando_SetMedallionAssignment(NULL);
  Rando_SetBossAssignment(NULL);
  if (synth) {
    g_asset_ptrs[126] = NULL;
    g_asset_sizes[126] = 0;
  }
  fprintf(stderr, "[Rando_ReinstallOverlaysSelfCheck] OK\n");
}

// add-rando-random-crystals — fixed resolve vectors (seed 0x1234, both axes
// random), captured at introduction; any drift means the derived-count
// contract broke across versions (nothing else observes these streams).
enum { kCrystalsPinnedGanon0x1234 = 1, kCrystalsPinnedTower0x1234 = 3 };

// add-rando-random-crystals — selfcheck-only seam: the gate helpers read the
// activation-time CACHE, so the fixture must poke settings + cache together
// (poking only the settings struct would be invisible to the getters).
static void tsc_set_crystals(uint8 ganon, uint8 tower) {
  g_rando_active_settings.crystals_ganon = ganon;
  g_rando_active_settings.crystals_tower = tower;
  g_rando_effective_crystals_ganon = ganon;
  g_rando_effective_crystals_tower = tower;
}

static void Rando_CrystalGateSelfCheck(void) {
  uint8 saved_slot_active = g_rando_slot_active;
  bool saved_settings_valid = g_rando_active_settings_valid;
  RandoSettings saved_settings = g_rando_active_settings;
  uint8 saved_eff_ganon = g_rando_effective_crystals_ganon;
  uint8 saved_eff_tower = g_rando_effective_crystals_tower;
  uint8 saved_crystals = link_has_crystals;
  uint8 saved_event43 = save_ow_event_info[0x43];

  g_rando_slot_active = 0;
  g_rando_active_settings_valid = false;
  link_has_crystals = 0;
  if (Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: vanilla zero crystals passed");
  if (!Rando_HasRequiredGanonCrystals())
    tsc_die("CrystalGate: vanilla Ganon combat was gated");
  link_has_crystals = 0x7f;
  if (!Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: vanilla seven crystals failed");

  g_rando_slot_active = 1;
  g_rando_active_settings_valid = true;
  Settings_SetDefaults(&g_rando_active_settings);
  tsc_set_crystals(/*ganon=*/1, /*tower=*/0);
  link_has_crystals = 0;
  if (!Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: zero-crystal tower requirement failed");
  save_ow_event_info[0x43] = 0;
  Rando_ApplyLoadedSaveRuntimeSettings();
  if (!(save_ow_event_info[0x43] & 0x20))
    tsc_die("CrystalGate: zero-crystal tower was not pre-opened");
  if (Rando_HasRequiredGanonCrystals())
    tsc_die("CrystalGate: zero crystals passed a one-crystal Ganon gate");
  link_has_crystals = 0x01;
  if (!Rando_HasRequiredGanonCrystals())
    tsc_die("CrystalGate: one crystal failed a one-crystal Ganon gate");

  tsc_set_crystals(/*ganon=*/0, /*tower=*/0);
  link_has_crystals = 0;
  if (!Rando_HasRequiredGanonCrystals())
    tsc_die("CrystalGate: zero-crystal Ganon requirement failed");

  tsc_set_crystals(/*ganon=*/0, /*tower=*/3);
  link_has_crystals = 0x03;
  if (Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: two crystals passed a three-crystal gate");
  link_has_crystals = 0x07;
  if (!Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: three crystals failed a three-crystal gate");
  save_ow_event_info[0x43] = 0;
  Rando_ApplyLoadedSaveRuntimeSettings();
  if (save_ow_event_info[0x43] & 0x20)
    tsc_die("CrystalGate: nonzero tower requirement was pre-opened");

  g_rando_active_settings_valid = false;
  link_has_crystals = 0x07;
  if (Rando_HasRequiredTowerCrystals())
    tsc_die("CrystalGate: unknown settings did not fail closed to seven");
  if (!Rando_HasRequiredGanonCrystals())
    tsc_die("CrystalGate: unknown settings did not preserve vanilla Ganon combat");

  // add-rando-random-crystals — the sentinel resolves deterministically and
  // in range; per-axis independence (ganon draw unaffected by tower mode).
  {
    RandoSettings rc;
    Settings_SetDefaults(&rc);
    rc.crystals_ganon = kCrystalsRandom;
    uint8 g1, t1, g2, t2;
    Crystals_Resolve(&rc, 0x1234ull, &g1, &t1);
    Crystals_Resolve(&rc, 0x1234ull, &g2, &t2);
    if (g1 != g2 || t1 != t2 || g1 > 7 || t1 != 7)
      tsc_die("CrystalGate: sentinel resolve not deterministic/in-range");
    rc.crystals_tower = kCrystalsRandom;
    Crystals_Resolve(&rc, 0x1234ull, &g2, &t2);
    if (g2 != g1 || t2 > 7)
      tsc_die("CrystalGate: per-axis independence violated");
    // Pinned vector (implementation-review F2): salt/stream drift would
    // silently re-roll every random seed's effective counts across versions
    // with the whole suite green — nothing else observes these streams.
    // Captured from the implementation at introduction.
    if (g2 != kCrystalsPinnedGanon0x1234 || t2 != kCrystalsPinnedTower0x1234)
      tsc_die("CrystalGate: pinned resolve vector drifted");
  }

  g_rando_slot_active = saved_slot_active;
  g_rando_active_settings_valid = saved_settings_valid;
  g_rando_active_settings = saved_settings;
  g_rando_effective_crystals_ganon = saved_eff_ganon;
  g_rando_effective_crystals_tower = saved_eff_tower;
  link_has_crystals = saved_crystals;
  save_ow_event_info[0x43] = saved_event43;
  fprintf(stderr, "[Rando_CrystalGateSelfCheck] OK\n");
}

static uint8 rando_selfcheck_dialogue_font(char ch) {
  if (ch >= 'A' && ch <= 'Z') return (uint8)(ch - 'A');
  if (ch >= 'a' && ch <= 'z') return (uint8)(26 + ch - 'a');
  if (ch >= '0' && ch <= '9') return (uint8)(52 + ch - '0');
  if (ch == '>') return 68;
  if (ch == ' ') return 89;
  return 0xFF;
}

static int rando_selfcheck_find_dialogue_ascii(const uint8 *buf,
                                                const char *text) {
  uint8 needle[64];
  size_t n = strlen(text);
  if (n == 0 || n > sizeof(needle)) return -1;
  for (size_t i = 0; i < n; i++) {
    needle[i] = rando_selfcheck_dialogue_font(text[i]);
    if (needle[i] == 0xFF) return -1;
  }
  int len = 0;
  while (len < 240 && buf[len] != 0x7F) len++;
  for (int i = 0; i + (int)n <= len; i++)
    if (memcmp(buf + i, needle, n) == 0)
      return i;
  return -1;
}

static void Rando_DynamicHintFastForwardSelfCheck(void) {
  uint8 saved_slot_active = g_rando_slot_active;
  bool saved_settings_valid = g_rando_active_settings_valid;
  RandoSettings saved_settings = g_rando_active_settings;
  uint8 saved_dialogue_flags = g_zenv.dialogue_flags;
  const RandoPlacementTable *saved_placement = Placement_GetActive();
  uint8 saved_player_is_indoors = player_is_indoors;
  uint16 saved_overworld_screen = overworld_screen_index;

  RandoPlacement pearl_entry = {LOC_Ice_Palace_Prize, ITEM_MoonPearl};
  RandoPlacementTable pearl_table = {&pearl_entry, 1};
  g_rando_slot_active = 1;
  g_rando_active_settings_valid = true;
  Settings_SetDefaults(&g_rando_active_settings);
  g_rando_active_settings.hints = kHintsMode_On;
  g_rando_active_settings.hint_npc_reward_reveal = 1;
  g_zenv.dialogue_flags = 0;
  Placement_Install(&pearl_table);

  uint8 encoded[241];
  if (!Rando_IsDynamicHintMessage(0x36) ||
      !Rando_RenderHintMessage(0x36, encoded))
    tsc_die("DynamicHint: post-Agahnim redirect did not activate");
  if (Text_ShouldFastForwardStoryMessage(0x36))
    tsc_die("DynamicHint: active post-Agahnim hint was fast-forwarded");

  RandoPlacement surface_entries[] = {
    {LOC_Desert_Ledge, ITEM_OcarinaInactive},
    {LOC_Bumper_Cave, ITEM_Hookshot},
    {LOC_Stumpy, ITEM_Shovel},
  };
  RandoPlacementTable surface_table = {surface_entries, 3};
  Placement_Install(&surface_table);

  player_is_indoors = 0;
  overworld_screen_index = 0x4A;
  memset(encoded, 0x55, sizeof(encoded));
  if (!Rando_IsDynamicHintMessage(0xA8) ||
      !Rando_RenderHintMessage(0xA8, encoded))
    tsc_die("DynamicHint: Bumper Cave sign did not activate");
  overworld_screen_index = 0x49;
  memset(encoded, 0x55, sizeof(encoded));
  if (Rando_IsDynamicHintMessage(0xA8) ||
      Rando_RenderHintMessage(0xA8, encoded) || encoded[0] != 0x55)
    tsc_die("DynamicHint: Bumper Cave sign escaped its screen discriminator");

  memset(encoded, 0x55, sizeof(encoded));
  if (!Rando_IsDynamicHintMessage(0xE5) ||
      Rando_RenderHintMessage(0xE5, encoded) ||
      !Rando_RewriteInteractiveHintMessage(0xE5, encoded))
    tsc_die("DynamicHint: Stumpy interactive redirect did not activate safely");
  int first_wait = -1, hint_line2 = 0, hint_line3 = 0;
  for (int i = 0; i < 240 && encoded[i] != 0x7F; i++) {
    if (encoded[i] == 0x7E) {
      first_wait = i;
      break;
    }
    hint_line2 += encoded[i] == 0x75;
    hint_line3 += encoded[i] == 0x76;
  }
  if (first_wait <= 0 || hint_line2 != 1 || hint_line3 != 0)
    tsc_die("DynamicHint: Stumpy fixture no longer exercises two-line hint");

  Rando_RewriteRewardDialogue(0xE5, encoded);
  int reward_at = rando_selfcheck_find_dialogue_ascii(encoded, "Shovel");
  int hint_at = rando_selfcheck_find_dialogue_ascii(encoded, "Flute");
  if (hint_at < 0)
    hint_at = rando_selfcheck_find_dialogue_ascii(encoded, "Hints");
  int selected_at =
      rando_selfcheck_find_dialogue_ascii(encoded, "    > Yes");
  int unselected_at =
      rando_selfcheck_find_dialogue_ascii(encoded, "       No way");
  bool has_choose = false;
  int page_waits = 0, page_scrolls = 0, unsafe_top_jumps = 0;
  for (int i = 0; i < 240 && encoded[i] != 0x7F; i++) {
    if (encoded[i] == 0x68) has_choose = true;
    page_waits += encoded[i] == 0x7E;
    page_scrolls += encoded[i] == 0x73;
    unsafe_top_jumps += encoded[i] == 0x74;
  }
  if (reward_at < 0 || hint_at <= reward_at || selected_at <= hint_at ||
      unselected_at <= selected_at || !has_choose ||
      page_waits != 2 || page_scrolls != 6 || unsafe_top_jumps != 0) {
    fprintf(stderr,
            "DynamicHint: Stumpy composed reward=%d hint=%d selected=%d "
            "unselected=%d choose=%d waits=%d scrolls=%d top_jumps=%d\n",
            reward_at, hint_at, selected_at, unselected_at, has_choose,
            page_waits, page_scrolls, unsafe_top_jumps);
    tsc_die("DynamicHint: Stumpy reward/hint/choice composition drifted");
  }

  g_rando_active_settings.hints = kHintsMode_Off;
  if (!Rando_IsDynamicHintMessage(0x36))
    tsc_die("DynamicHint: hints-off owned redirect was released to vanilla");
  if (Text_ShouldFastForwardStoryMessage(0x36))
    tsc_die("DynamicHint: hints-off neutral redirect was fast-forwarded");
  memset(encoded, 0x55, sizeof(encoded));
  if (!Rando_RenderHintMessage(0x36, encoded))
    tsc_die("DynamicHint: hints-off redirect did not render neutral text");
  memset(encoded, 0x55, sizeof(encoded));
  if (!Rando_RewriteInteractiveHintMessage(0xE5, encoded))
    tsc_die("DynamicHint: hints-off Stumpy did not retain a neutral choice");

  Placement_Install(saved_placement);
  overworld_screen_index = saved_overworld_screen;
  player_is_indoors = saved_player_is_indoors;
  g_zenv.dialogue_flags = saved_dialogue_flags;
  g_rando_active_settings = saved_settings;
  g_rando_active_settings_valid = saved_settings_valid;
  g_rando_slot_active = saved_slot_active;
  fprintf(stderr, "[Rando_DynamicHintFastForwardSelfCheck] OK\n");
}

// Cross-TU capacity ABI selfcheck (add-rando-grass-rock-shuffle D5; the
// enemy-drop review lesson made concrete): the Makefile has no header
// dependency tracking, so a kRandoLocationCapacity bump + incremental `make`
// ships TUs compiled against DIFFERENT capacities — mixed-ABI location arrays
// that historically passed selftest while producing a 16.8 GB runaway
// spoiler. Every TU that sizes an array by the constant compiles a
// RANDO_DEFINE_CAPACITY_PROBE; a mismatch here means stale objects — run
// `make clean`.
RANDO_DEFINE_CAPACITY_PROBE(rando)
static void Rando_SelfCheckCapacityABI(void) {
  static const struct { const char *tu; uint32 (*probe)(void); } kProbes[] = {
    { "rando.c",              RandoCapacityProbe_rando },
    { "auto_tracker.c",       RandoCapacityProbe_auto_tracker },
    { "rando_generate.c",     RandoCapacityProbe_rando_generate },
    { "rando_hints.c",        RandoCapacityProbe_rando_hints },
    { "rando_logic.c",        RandoCapacityProbe_rando_logic },
    { "rando_placement.c",    RandoCapacityProbe_rando_placement },
    { "rando_save.c",         RandoCapacityProbe_rando_save },
    { "rando_snapshot_tail.c", RandoCapacityProbe_rando_snapshot_tail },
    { "rando_spoiler.c",      RandoCapacityProbe_rando_spoiler },
#ifdef Z3R_NATIVE_SETTINGS_WINDOW
    { "rando_reach_panel.cpp", RandoCapacityProbe_rando_reach_panel },
    { "tracker_windows.cpp",  RandoCapacityProbe_tracker_windows },
#endif
  };
  for (size_t i = 0; i < sizeof(kProbes) / sizeof(kProbes[0]); i++) {
    uint32 v = kProbes[i].probe();
    if (v != kRandoLocationCapacity) {
      fprintf(stderr,
              "Rando_SelfCheckCapacityABI: %s compiled with capacity %u, "
              "header says %u — STALE OBJECT (no header deps in Makefile); "
              "run `make clean` and rebuild\n",
              kProbes[i].tu, (unsigned)v, (unsigned)kRandoLocationCapacity);
      exit(2);
    }
  }
  fprintf(stderr, "[Rando_SelfCheckCapacityABI] OK (capacity %u, %u TUs)\n",
          (unsigned)kRandoLocationCapacity,
          (unsigned)(sizeof(kProbes) / sizeof(kProbes[0])));
}

static void Rando_GrantPlanSelfCheck(void) {
  static uint8 saved_ram[sizeof(g_ram)];
  memcpy(saved_ram, g_ram, sizeof(saved_ram));
  uint8 saved_boomerang_owned = g_rando_boomerang_owned;
  uint8 saved_bow_owned = g_rando_bow_owned;
  const char *failure = NULL;
  uint16 failure_item = 0xffffu;
  RandoGrantState base;
  memset(&base, 0, sizeof(base));
  base.health_capacity = 0x30;
  base.health_current = 0x18;
  base.magic_power = 0x20;
  base.trap_seed = 0x123456789abcdef0ull;

#define GRANT_PLAN_CHECK(expr, message, item) \
  do { if (!(expr)) { failure = (message); failure_item = (uint16)(item); goto cleanup; } } while (0)

  GRANT_PLAN_CHECK(kRandoItemGrantMetadataCount == ITEM__COUNT,
                   "metadata count does not equal ITEM__COUNT", 0xffffu);

  for (uint16 item = 0; item < ITEM__COUNT; item++) {
    const RandoItemGrantMetadata *metadata = &kRandoItemGrantMetadata[item];
    RandoGrantState state = base, state_after;
    RandoGrantPlan first, second;
    bool is_virtual = metadata->opcode == kRandoGrantOp_Virtual;
    bool ok = Rando_ResolveGrantPlan(166, item, &state, &first);
    if (is_virtual) {
      GRANT_PLAN_CHECK(!ok && first.disposition == kRandoGrantDisposition_Invalid,
                       "virtual item produced a grantable plan", item);
      continue;
    }
    GRANT_PLAN_CHECK(metadata->opcode != kRandoGrantOp_Invalid,
                     "placeable item has invalid generated opcode", item);
    GRANT_PLAN_CHECK(ok && first.disposition != kRandoGrantDisposition_Invalid,
                     "placeable item did not resolve intentionally", item);
    GRANT_PLAN_CHECK(first.opcode == metadata->opcode &&
                     first.payload == metadata->payload,
                     "plan disagrees with generated metadata", item);
    state_after = state;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(166, item, &state, &second) &&
                     memcmp(&first, &second, sizeof(first)) == 0,
                     "same inputs produced different plans", item);
    GRANT_PLAN_CHECK(memcmp(&state, &state_after, sizeof(state)) == 0,
                     "resolver modified its inventory snapshot", item);

    DirectGrantIconEntry expected = {0, 0, 0};
    bool expected_icon = false;
    if (first.display_code != 0xff) {
      expected_icon = rando_receive_icon_for_code(first.display_code,
                                                   &expected.gfx,
                                                   &expected.big,
                                                   &expected.oam_flags);
    } else if (first.opcode == kRandoGrantOp_DirectTrap) {
      expected_icon = rando_trap_decoy_icon_for_seed(
          state.trap_seed, item, 166, &expected);
    } else {
      uint16 icon_item = item;
      if (first.opcode == kRandoGrantOp_DirectMagicUpgrade)
        icon_item = state.magic_consumption == 0 ? ITEM_HalfMagic : ITEM_QuarterMagic;
      const DirectGrantIconEntry *entry = rando_direct_grant_icon_entry(icon_item);
      if (entry != NULL) {
        expected = *entry;
        expected_icon = true;
      }
    }
    GRANT_PLAN_CHECK(first.display_valid == expected_icon,
                     "draw and grant-plan icon availability disagree", item);
    if (expected_icon) {
      GRANT_PLAN_CHECK(first.display_gfx == expected.gfx &&
                       first.display_big == expected.big &&
                       first.display_oam_flags == expected.oam_flags,
                       "draw and grant-plan icon descriptor disagree", item);
    }
  }

  // Progressive tier boundaries, including the bow's non-linear state byte.
  {
    RandoGrantState state = base;
    RandoGrantPlan plan;
    const uint8 sword_codes[4] = {0x00, 0x01, 0x02, 0x03};
    for (uint8 tier = 0; tier <= 4; tier++) {
      state.sword = tier;
      GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(1, ITEM_ProgressiveSword, &state, &plan),
                       "sword boundary did not resolve", ITEM_ProgressiveSword);
      GRANT_PLAN_CHECK(tier < 4
                           ? plan.disposition == kRandoGrantDisposition_Receive &&
                             plan.receive_code == sword_codes[tier]
                           : plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                       "sword boundary resolved incorrectly", ITEM_ProgressiveSword);
    }
    for (uint8 tier = 0; tier <= 3; tier++) {
      state.shield = tier;
      GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(1, ITEM_ProgressiveShield, &state, &plan) &&
                       (tier < 3
                            ? plan.receive_code == (uint8)(0x04 + tier)
                            : plan.disposition == kRandoGrantDisposition_AcceptedNoOp),
                       "shield boundary resolved incorrectly", ITEM_ProgressiveShield);
    }
    for (uint8 tier = 0; tier <= 2; tier++) {
      state.armor = tier;
      GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(1, ITEM_ProgressiveArmor, &state, &plan) &&
                       (tier < 2
                            ? plan.receive_code == (uint8)(0x22 + tier)
                            : plan.disposition == kRandoGrantDisposition_AcceptedNoOp),
                       "armor boundary resolved incorrectly", ITEM_ProgressiveArmor);
      state.gloves = tier;
      GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(1, ITEM_ProgressiveGlove, &state, &plan) &&
                       (tier < 2
                            ? plan.receive_code == (uint8)(0x1b + tier)
                            : plan.disposition == kRandoGrantDisposition_AcceptedNoOp),
                       "glove boundary resolved incorrectly", ITEM_ProgressiveGlove);
    }
    // Ownership bits are authoritative over the currently-selected raw byte.
    // Exercise every legal ownership x selection combination, including the
    // critical silver-owned/wood-selected state produced by menu toggling.
    static const uint8 kBowOwnership[] = {
      0, kRandoBow_Wood, kRandoBow_Silver,
      kRandoBow_Wood | kRandoBow_Silver,
    };
    for (size_t owned_i = 0; owned_i < countof(kBowOwnership); owned_i++) {
      state.bow_owned = kBowOwnership[owned_i];
      for (uint8 bow = 0; bow <= 4; bow++) {
        state.bow = bow;
        bool silver_owned = (state.bow_owned & kRandoBow_Silver) != 0;
        bool wood_owned = (state.bow_owned & kRandoBow_Wood) != 0;
        uint8 expected = silver_owned ? 0xff
                       : wood_owned ? 0x3b
                       : bow == 0 ? 0x0b
                       : bow <= 2 ? 0x3b
                       : 0xff;
        GRANT_PLAN_CHECK(
            Rando_ResolveGrantPlan(1, ITEM_ProgressiveBow, &state, &plan) &&
                (expected == 0xff
                     ? plan.disposition == kRandoGrantDisposition_AcceptedNoOp &&
                           plan.receive_code == 0xff
                     : plan.disposition == kRandoGrantDisposition_Receive &&
                           plan.receive_code == expected),
            "bow ownership/selection combination resolved incorrectly",
            ITEM_ProgressiveBow);
      }
    }
    state.bow_owned = 0;
    state.bow = 0xff;
    GRANT_PLAN_CHECK(
        Rando_ResolveGrantPlan(1, ITEM_ProgressiveBow, &state, &plan) &&
            plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
        "unknown legacy bow byte did not fail safe", ITEM_ProgressiveBow);
  }

  // Both boomerang registry ids share one pre-state rule: blue first, red once
  // blue is truly owned, independent of which slot presentation is active.
  {
    RandoGrantState state = base;
    RandoGrantPlan plan;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(2, ITEM_RedBoomerang, &state, &plan) &&
                     plan.receive_code == 0x0c && plan.display_code == 0x0c,
                     "first boomerang was not blue", ITEM_RedBoomerang);
    state.boomerang_owned = kRandoBoomerang_Blue;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(2, ITEM_BlueBoomerang, &state, &plan) &&
                     plan.receive_code == 0x2a && plan.display_code == 0x2a,
                     "owned-blue boomerang was not red", ITEM_BlueBoomerang);
    state.boomerang_owned = 0;
    state.boomerang = 1;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(2, ITEM_RedBoomerang, &state, &plan) &&
                     plan.receive_code == 0x2a,
                     "legacy boomerang byte was ignored", ITEM_RedBoomerang);
  }

  // Bottle acquisition and loose contents have different acceptance rules, and
  // -- the point of fix-grant-refusal-contract -- their REFUSALS differ in kind.
  // A refusal a player can never clear must terminate; only one they can clear
  // may ask the caller to retry. Getting this backwards for the pickup case
  // turned every "re-arm and retry next frame" site into an infinite loop, and
  // the sites that immobilize while retrying removed the only means of recovery.
  {
    RandoGrantState state = base;
    RandoGrantPlan plan;
    memset(state.bottle, 3, sizeof(state.bottle));
    // PERMANENT: a 5th bottle needs an UNOWNED slot and bottle count is
    // monotonic, so this can never become satisfiable -> terminal, like any
    // other at-cap receive. (This assertion previously demanded Retryable.)
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(3, ITEM_BottleEmpty, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "5th bottle must terminate, not loop", ITEM_BottleEmpty);
    // TRANSIENT: contents need an EMPTY bottle, which drinking one creates.
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(3, ITEM_BluePotion, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_RetryableFailure,
                     "contents without an empty bottle were not retryable", ITEM_BluePotion);
    state.bottle[2] = 2;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(3, ITEM_BluePotion, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_Receive,
                     "contents with an empty bottle were rejected", ITEM_BluePotion);
    // A free slot still delivers a bottle normally.
    state.bottle[2] = 0;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(3, ITEM_BottleEmpty, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_Receive,
                     "bottle with a free slot was not delivered", ITEM_BottleEmpty);
    // Independent oracle: state which ITEMS are terminal at a full inventory
    // from game semantics, not from the implementation's own classifier. The
    // first version of this sweep asked rando_code_is_bottle_content() whether
    // a refusal was clearable -- the same predicate that decides the
    // disposition -- so it held by construction and a total swap of the two
    // code tables passed it. A bottle-with-contents is still a NEW bottle and
    // needs a free slot (permanent); a loose potion needs an EMPTY bottle,
    // which drinking creates (transient).
    memset(state.bottle, 3, sizeof(state.bottle));
    static const struct { uint16 item; uint8 want; } kBottleContract[] = {
      { ITEM_BottleEmpty,             kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithFairy,         kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithBee,           kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithGoodBee,       kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithRedPotion,     kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithGreenPotion,   kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BottleWithBluePotion,    kRandoGrantDisposition_AcceptedNoOp },
      { ITEM_BluePotion,              kRandoGrantDisposition_RetryableFailure },
      { ITEM_RedPotion,               kRandoGrantDisposition_RetryableFailure },
    };
    for (size_t i = 0; i < countof(kBottleContract); i++) {
      GRANT_PLAN_CHECK(
          Rando_ResolveGrantPlan(3, kBottleContract[i].item, &state, &plan) &&
              plan.disposition == kBottleContract[i].want,
          "bottle refusal class disagrees with the contract",
          kBottleContract[i].item);
    }
  }

  // Intentional accepted no-ops must never fall through to the source item.
  {
    RandoGrantState state = base;
    RandoGrantPlan plan;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_Nothing, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "Nothing was not accepted as a no-op", ITEM_Nothing);
    state.magic_consumption = 2;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_HalfMagic, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "max magic upgrade was not an accepted no-op", ITEM_HalfMagic);
    state.health_capacity = state.health_current = 0xa0;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_BossHeartContainer, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "max heart container was not an accepted no-op",
                     ITEM_BossHeartContainer);
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_HeartRefill, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "full-health refill was not an accepted no-op", ITEM_HeartRefill);
    state.bomb_upgrades = state.arrow_upgrades = 7;
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_BombUpgrade5, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "max bomb capacity was not an accepted no-op", ITEM_BombUpgrade5);
    GRANT_PLAN_CHECK(Rando_ResolveGrantPlan(4, ITEM_ArrowUpgrade5, &state, &plan) &&
                     plan.disposition == kRandoGrantDisposition_AcceptedNoOp,
                     "max arrow capacity was not an accepted no-op", ITEM_ArrowUpgrade5);
  }

  // The pure resolver must not touch live/shared bytes. Then exercise capture
  // with deliberately non-default values and restore the entire RAM image.
  GRANT_PLAN_CHECK(memcmp(saved_ram, g_ram, sizeof(saved_ram)) == 0 &&
                   g_rando_boomerang_owned == saved_boomerang_owned &&
                   g_rando_bow_owned == saved_bow_owned,
                   "resolver modified live gameplay state", 0xffffu);
  link_sword_type = 2;
  link_item_bow = 3;
  link_bottle_info[0] = 2;
  g_rando_boomerang_owned = kRandoBoomerang_Blue;
  g_rando_bow_owned = kRandoBow_Silver;
  {
    RandoGrantState captured;
    Rando_CaptureGrantState(&captured);
    GRANT_PLAN_CHECK(captured.sword == 2 && captured.bow == 3 &&
                     captured.bow_owned == kRandoBow_Silver &&
                     captured.bottle[0] == 2 &&
                     captured.boomerang_owned == kRandoBoomerang_Blue,
                     "live state capture disagrees with inventory bytes", 0xffffu);
  }

cleanup:
  memcpy(g_ram, saved_ram, sizeof(saved_ram));
  g_rando_boomerang_owned = saved_boomerang_owned;
  g_rando_bow_owned = saved_bow_owned;
#undef GRANT_PLAN_CHECK
  if (failure != NULL) {
    fprintf(stderr, "Rando_GrantPlanSelfCheck: %s (item %u)\n",
            failure, (unsigned)failure_item);
    exit(2);
  }
  fprintf(stderr, "[Rando_GrantPlanSelfCheck] OK (%u metadata rows)\n",
          (unsigned)kRandoItemGrantMetadataCount);
}

static void Rando_GrantTransactionSelfCheck(void) {
  static uint8 saved_ram[sizeof(g_ram)], before_ram[sizeof(g_ram)];
  uint8 saved_checked[kRandoCheckedBitmapBytes];
  uint8 saved_soul_flags[kSoulFlagsBytes];
  const RandoPlacementTable *saved_placement = Placement_GetActive();
  uint8 saved_slot_active = g_rando_slot_active;
  uint32 saved_features1 = enhanced_features1;
  RandoSettings saved_active_settings = g_rando_active_settings;
  bool saved_active_settings_valid = g_rando_active_settings_valid;
  uint8 saved_mushroom_held = g_rando_mushroom_held;
  uint8 saved_bow_owned = g_rando_bow_owned;
  uint32 saved_reachability = g_reachability_state_counter;
  uint16 saved_last_item = g_last_dispatched_item_id;
  uint16 saved_last_location = g_last_dispatched_location_id;
  RandoGrantPlan saved_last_plan = g_last_dispatched_plan;
  bool saved_last_valid = g_last_dispatched_plan_valid;
  uint16 saved_last_soul_item = g_rando_last_soul_item_id;
  uint16 saved_soul_pending = g_rando_soul_msg_pending;
  uint8 saved_confirmation_count = g_rando_pot_confirmation_count;
  DirectGrantIconEntry saved_confirmation_icons[kRandoPotConfirmationQueueMax];
  memcpy(saved_ram, g_ram, sizeof(saved_ram));
  memcpy(saved_checked, g_rando_checked_bitmap, sizeof(saved_checked));
  memcpy(saved_soul_flags, Souls_Flags(), sizeof(saved_soul_flags));
  memcpy(saved_confirmation_icons, g_rando_pot_confirmation_icons,
         sizeof(saved_confirmation_icons));

  RandoPlacement entries[] = {
    {7000, ITEM_Rupee1},
    {7001, ITEM_BottleEmpty},
    {7002, ITEM_Nothing},
    {7003, ITEM_HalfMagic},
    {7004, ITEM_Rupee1},
    {7005, ITEM_Rupee5},
    {7006, ITEM_BombUpgrade5},
    {7007, ITEM_ProgressiveBow},
    {7008, ITEM_StartingHeart},
    {7009, ITEM_MagicPowder},
    {7010, ITEM_Rupee20},
    {7099, ITEM_BluePotion},  // LOOSE potion: transient-refusal fixture
    {7011, ITEM_Soul_ArmosKnights},
    {7012, ITEM_Soul_Npc_Sahasrahla},
  };
  RandoPlacementTable table = {entries, (uint16)countof(entries)};
  const char *failure = NULL;
  uint16 failure_location = 0xffffu;

#define GRANT_TX_CHECK(expr, message, location) \
  do { if (!(expr)) { failure = (message); failure_location = (uint16)(location); goto cleanup; } } while (0)

  Placement_Install(&table);
  memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
  g_rando_slot_active = 1;
  enhanced_features1 |= kFeatures1_RandomizerActive;
  g_rando_mushroom_held = 0;
  g_last_dispatched_item_id = g_last_dispatched_location_id = 0xffffu;
  g_last_dispatched_plan_valid = false;
  g_rando_pot_confirmation_count = 0;
  g_rando_last_soul_item_id = 0xffffu;
  g_rando_soul_msg_pending = 0xffffu;
  Souls_ResetFlags();

  // Absent/virtual preparation is mutation-free and explicit.
  {
    RandoDeferredGrantToken token;
    memcpy(before_ram, g_ram, sizeof(before_ram));
    uint8 before_checked[kRandoCheckedBitmapBytes];
    memcpy(before_checked, g_rando_checked_bitmap, sizeof(before_checked));
    GRANT_TX_CHECK(Rando_PrepareGrant(7999, ITEM_L1Sword, 0, &token) ==
                         kRandoGrantResult_NotActive,
                     "absent placement did not return NotActive", 7999);
    GRANT_TX_CHECK(memcmp(before_ram, g_ram, sizeof(before_ram)) == 0 &&
                       memcmp(before_checked, g_rando_checked_bitmap,
                              sizeof(before_checked)) == 0,
                     "NotActive preparation mutated gameplay state", 7999);
    GRANT_TX_CHECK(Rando_PrepareGrant(7008, ITEM_StartingHeart, 0xff, &token) ==
                         kRandoGrantResult_Invalid &&
                       !Rando_IsLocationChecked(7008),
                     "virtual preparation did not fail closed", 7008);
  }

  // A 5th bottle TERMINATES (fix-grant-refusal-contract): the refusal can never
  // clear, so asking the caller to retry looped it forever -- and the sites that
  // immobilize while retrying removed the player's only means of recovery. It
  // consumes the check and grants nothing, exactly like any at-cap receive.
  // Contents stay retryable, because drinking a potion frees a bottle.
  {
    RandoDeferredGrantToken token;
    memset(link_bottle_info, 3, 4);
    memcpy(before_ram, g_ram, sizeof(before_ram));
    uint8 before_queue = g_rando_pot_confirmation_count;
    GRANT_TX_CHECK(Rando_PrepareGrant(7001, ITEM_BottleEmpty, 0x16, &token) ==
                         kRandoGrantResult_Accepted &&
                       token.plan.disposition ==
                         kRandoGrantDisposition_AcceptedNoOp,
                     "5th bottle did not prepare as a terminal no-op", 7001);
    GRANT_TX_CHECK(!Rando_IsLocationChecked(7001) &&
                       memcmp(before_ram, g_ram, sizeof(before_ram)) == 0 &&
                       g_rando_pot_confirmation_count == before_queue,
                     "no-op preparation mutated/marked/confirmed", 7001);
    memcpy(before_ram, g_ram, sizeof(before_ram));
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_Animated, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(7001) &&
                       // Nothing granted. (g_ram at large DOES change: the
                       // at-cap class still shows its confirmation cue.)
                       link_bottle_info[0] == 3 && link_bottle_info[1] == 3 &&
                       link_bottle_info[2] == 3 && link_bottle_info[3] == 3,
                     "5th bottle commit did not consume the check cleanly", 7001);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    memset(link_bottle_info, 0, 4);

    // Capacity lost while the token is deferred is just as permanent, so the
    // commit terminates rather than handing the caller an unbreakable retry.
    GRANT_TX_CHECK(Rando_PrepareGrant(7001, ITEM_BottleEmpty, 0x16, &token) ==
                         kRandoGrantResult_Accepted && token.version != 0,
                     "bottle with capacity did not prepare", 7001);
    memset(link_bottle_info, 3, 4);  // capacity lost while token is deferred
    memcpy(before_ram, g_ram, sizeof(before_ram));
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_Animated, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(7001) &&
                       link_bottle_info[0] == 3 && link_bottle_info[1] == 3 &&
                       link_bottle_info[2] == 3 && link_bottle_info[3] == 3,
                     "deferred bottle capacity loss must terminate, not loop", 7001);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));

    // The TRANSIENT case is unchanged: contents with no empty bottle stay
    // retryable, so the lossless-retry guarantee still holds where it can.
    memset(link_bottle_info, 3, 4);
    memcpy(before_ram, g_ram, sizeof(before_ram));
    // NB: PrepareGrant resolves the PLACED item, so this needs its own row --
    // 7001 is a bottle, and asking it for a potion would silently re-test the
    // terminal case above.
    GRANT_TX_CHECK(Rando_PrepareGrant(7099, ITEM_BluePotion, 0x2e, &token) ==
                         kRandoGrantResult_Retryable &&
                       !Rando_IsLocationChecked(7099) &&
                       memcmp(before_ram, g_ram, sizeof(before_ram)) == 0,
                     "contents without an empty bottle must stay retryable", 7099);
    // ...and it delivers once a bottle is emptied, so retry is still lossless.
    link_bottle_info[1] = 2;
    GRANT_TX_CHECK(Rando_PrepareGrant(7099, ITEM_BluePotion, 0x2e, &token) ==
                         kRandoGrantResult_Accepted,
                     "contents with an empty bottle were refused", 7099);
    memset(link_bottle_info, 0, 4);
  }

  // Every byte of the fixed-size token is integrity-protected. Corruption is
  // rejected before placement/check lookup and cannot grant or mark anything.
  {
    RandoDeferredGrantToken token, corrupt;
    GRANT_TX_CHECK(Rando_PrepareGrant(7000, ITEM_Rupee1, 0x34, &token) ==
                         kRandoGrantResult_Accepted,
                     "integrity fixture did not prepare", 7000);
    for (size_t byte = 0; byte < sizeof(token); byte++) {
      corrupt = token;
      ((uint8 *)&corrupt)[byte] ^= 0x5au;
      memcpy(before_ram, g_ram, sizeof(before_ram));
      uint8 before_checked[kRandoCheckedBitmapBytes];
      memcpy(before_checked, g_rando_checked_bitmap, sizeof(before_checked));
      RandoDeferredGrantToken corrupt_before = corrupt;
      GRANT_TX_CHECK(
          Rando_CommitPreparedGrant(&corrupt, kRandoGrantPresentation_None,
                                    0, 0) == kRandoGrantResult_Invalid &&
              memcmp(&corrupt, &corrupt_before, sizeof(corrupt)) == 0 &&
              memcmp(before_ram, g_ram, sizeof(before_ram)) == 0 &&
              memcmp(before_checked, g_rando_checked_bitmap,
                     sizeof(before_checked)) == 0 &&
              !Rando_IsLocationChecked(7000),
          "one-byte token corruption was accepted or mutated state", 7000);
    }
  }

  // Accepted no-op commits the check without touching unrelated inventory.
  {
    uint16 rupees = link_rupees_goal;
    GRANT_TX_CHECK(Rando_GrantLocation(7002, ITEM_Nothing, 0xff,
                                       kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(7002) &&
                       link_rupees_goal == rupees,
                     "accepted no-op did not commit cleanly", 7002);
  }

  // Direct grant, replay rejection, and checked-after-acceptance semantics.
  {
    link_magic_consumption = 0;
    RandoDeferredGrantToken token;
    GRANT_TX_CHECK(Rando_PrepareGrant(7003, ITEM_HalfMagic, 0xff, &token) ==
                         kRandoGrantResult_Accepted &&
                       token.plan.receive_code == 1 &&
                       !Rando_IsLocationChecked(7003),
                     "direct prepare marked before commit", 7003);
    link_magic_consumption = 1;  // another upgrade lands while token is deferred
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_magic_consumption == 1 &&
                       Rando_IsLocationChecked(7003),
                     "direct commit did not grant then mark", 7003);
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_AlreadyChecked &&
                       link_magic_consumption == 1,
                     "prepared-token replay granted twice", 7003);
  }

  // Quiet soul sources retain the named soul feedback contract without also
  // queuing the generic floating icon. Cover both halves of the contiguous
  // enemy/boss + NPC soul family.
  {
    static const struct {
      uint16 location_id;
      uint16 item_id;
    } kQuietSouls[] = {
      {7011, ITEM_Soul_ArmosKnights},
      {7012, ITEM_Soul_Npc_Sahasrahla},
    };
    for (size_t i = 0; i < countof(kQuietSouls); i++) {
      g_rando_soul_msg_pending = 0xffffu;
      g_rando_last_soul_item_id = 0xffffu;
      g_rando_pot_confirmation_count = 0;
      uint8 soul_index =
          (uint8)(kQuietSouls[i].item_id - ITEM_Soul_ArmosKnights);
      GRANT_TX_CHECK(
          Rando_GrantLocation(kQuietSouls[i].location_id,
                              0xffffu, kRandoLttpSkip,
                              kRandoGrantPresentation_Quiet, 0, 0) ==
                  kRandoGrantResult_Accepted &&
              Rando_IsLocationChecked(kQuietSouls[i].location_id) &&
              Souls_OwnedIndex(soul_index) &&
              g_rando_soul_msg_pending == kQuietSouls[i].item_id &&
              g_rando_last_soul_item_id == kQuietSouls[i].item_id &&
              g_rando_pot_confirmation_count == 0,
          "quiet soul did not queue exactly the named confirmation",
          kQuietSouls[i].location_id);
    }
  }

  // Present identity is not confused with absence.
  {
    link_rupees_goal = 0;
    RandoDeferredGrantToken token;
    GRANT_TX_CHECK(Rando_PrepareGrant(7000, ITEM_Rupee1, 0x34, &token) ==
                         kRandoGrantResult_Accepted && token.plan.item_id == ITEM_Rupee1,
                     "identity placement did not prepare", 7000);
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_rupees_goal == 1 && Rando_IsLocationChecked(7000),
                     "identity receive did not grant/mark", 7000);
  }

  // Animated receive uses the lossless saturation fallback in this assetless
  // selfcheck, then commits only after that receive path accepts it.
  {
    memset(ancilla_type, 0x22, 5);
    link_rupees_goal = 10;
    GRANT_TX_CHECK(Rando_GrantLocation(7004, ITEM_Rupee1, 0x34,
                                       kRandoGrantPresentation_Animated, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_rupees_goal == 11 && Rando_IsLocationChecked(7004),
                     "animated receive did not grant/mark", 7004);

    // Quiet receive applies inventory immediately and queues one confirmation.
    uint8 queue_before = g_rando_pot_confirmation_count;
    GRANT_TX_CHECK(Rando_GrantLocation(7005, ITEM_Rupee5, 0x35,
                                       kRandoGrantPresentation_Quiet, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_rupees_goal == 16 && Rando_IsLocationChecked(7005) &&
                       g_rando_pot_confirmation_count == (uint8)(queue_before + 1),
                     "quiet receive did not grant/mark/queue", 7005);
  }

  // Deferred commit consumes the frozen plan rather than resolving the changed
  // progressive state again (wood-bow 0x0b remains wood; a re-resolved 0x3b
  // would incorrectly advance it to silver).
  {
    link_item_bow = 0;
    g_rando_bow_owned = 0;
    RandoDeferredGrantToken token, copy;
    GRANT_TX_CHECK(Rando_PrepareGrant(7007, ITEM_ProgressiveBow, 0xff, &token) ==
                         kRandoGrantResult_Accepted && token.plan.receive_code == 0x0b,
                     "deferred progressive prepare was wrong", 7007);
    memcpy(&copy, &token, sizeof(copy));
    link_item_bow = 1;
    g_rando_bow_owned = kRandoBow_Wood;
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &copy, kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_item_bow < 3 &&
                       !(g_rando_bow_owned & kRandoBow_Silver) &&
                       memcmp(&copy, &token, sizeof(copy)) == 0,
                     "deferred commit re-resolved or modified its token", 7007);
  }

  // Derived shared-byte ownership is recorded only by an accepted commit.
  {
    GRANT_TX_CHECK((g_rando_mushroom_held & kRandoMushroom_PowderOwned) == 0,
                     "powder ownership was dirty before commit", 7009);
    GRANT_TX_CHECK(Rando_GrantLocation(7009, ITEM_MagicPowder, 0x0d,
                                       kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       (g_rando_mushroom_held & kRandoMushroom_PowderOwned),
                     "accepted commit omitted derived ownership", 7009);
  }

  // Caller-owned sibling forfeits do not grant the sibling item. Capacity
  // identity bookkeeping remains explicitly repeatable and never applies +5.
  {
    uint16 rupees = link_rupees_goal;
    GRANT_TX_CHECK(Rando_ForfeitLocation(7010) == kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(7010) && link_rupees_goal == rupees &&
                       Rando_ForfeitLocation(7010) == kRandoGrantResult_AlreadyChecked,
                     "sibling forfeit granted or did not persist", 7010);
    link_bomb_upgrades = 0;
    link_bomb_filler = 0;
    RandoDeferredGrantToken token;
    GRANT_TX_CHECK(Rando_PrepareGrant(7006, ITEM_BombUpgrade5, 0x51, &token) ==
                         kRandoGrantResult_Accepted && token.plan.receive_code == 1,
                     "capacity target did not freeze", 7006);
    link_bomb_upgrades = 1;  // caller/state change reaches the frozen target first
    GRANT_TX_CHECK(Rando_CommitPreparedGrant(
                         &token, kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       link_bomb_upgrades == 1 && link_bomb_filler == 0,
                     "capacity deferred commit double-incremented/refilled", 7006);
    link_bomb_upgrades = 0;
    GRANT_TX_CHECK(Rando_CommitRepeatableCapacityIdentity(
                         7006, ITEM_BombUpgrade5) == kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(7006) && link_bomb_upgrades == 0 &&
                       Rando_CommitRepeatableCapacityIdentity(
                         7006, ITEM_BombUpgrade5) == kRandoGrantResult_Accepted &&
                       link_bomb_upgrades == 0,
                     "repeatable capacity identity was not caller-owned", 7006);
    GRANT_TX_CHECK(Rando_CommitRepeatableCapacityIdentity(
                         7006, ITEM_ArrowUpgrade5) == kRandoGrantResult_Invalid &&
                       link_bomb_upgrades == 0,
                     "capacity identity accepted a mismatched item", 7006);
  }

  // Event-family adapters exercise the exact result contract their gameplay
  // callers branch on: absent -> vanilla, accepted/already -> terminal, and
  // retry -> no sibling/source consumption.
  {
    RandoPlacement event_entries[] = {
      {LOC_Hyrule_Castle_Map_Chest, ITEM_Rupee1},
      {LOC_Skull_Woods_Prize, ITEM_Prize_Crystal4},
      {239, ITEM_Bombs10},
      {266, ITEM_Rupee1},
      {267, ITEM_Rupee5},
      {500, ITEM_PieceOfHeart},
    };
    RandoPlacementTable event_table = {
      event_entries, (uint16)countof(event_entries)
    };
    Placement_Install(&event_table);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));

    GRANT_TX_CHECK(Rando_HasLocationPlacement(500) &&
                       !Rando_HasLocationPlacement(7999) &&
                       !Rando_IsLocationCheckedOrVanilla(500, true) &&
                       Rando_IsLocationCheckedOrVanilla(7999, true) &&
                       !Rando_IsLocationCheckedOrVanilla(7999, false),
                     "presence-aware completion fallback drifted", 500);
    Rando_MarkLocationChecked(500);
    GRANT_TX_CHECK(Rando_IsLocationCheckedOrVanilla(500, false),
                     "present checked location ignored checked state", 500);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));

#if kRandoChestLookup_COUNT > 0
    link_rupees_goal = 0;
    GRANT_TX_CHECK(Rando_ChestGrant(
                         0xfffe, 0, 0x34,
                         kRandoGrantPresentation_None, 1, 0) ==
                         kRandoGrantResult_NotActive,
                     "unmapped chest did not return NotActive", 0xffff);
    GRANT_TX_CHECK(Rando_ChestGrant(
                         114, 0, 0x05,
                         kRandoGrantPresentation_None, 1, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(LOC_Hyrule_Castle_Map_Chest) &&
                       link_rupees_goal == 1,
                     "mapped chest transaction failed", LOC_Hyrule_Castle_Map_Chest);
    GRANT_TX_CHECK(Rando_ChestGrant(
                         114, 0, 0x05,
                         kRandoGrantPresentation_None, 1, 0) ==
                         kRandoGrantResult_AlreadyChecked,
                     "checked chest replay was not terminal", LOC_Hyrule_Castle_Map_Chest);
#endif

    link_has_crystals = 0;
    GRANT_TX_CHECK(Rando_GrantBossPrizeReceipt(
                         kGameDungeon_SkullWoods, 0x20,
                         kRandoGrantPresentation_None, 3, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(LOC_Skull_Woods_Prize) &&
                       (link_has_crystals & 0x20),  // Crystal 4 = TT's bit
                     "boss-prize transaction failed", LOC_Skull_Woods_Prize);
    GRANT_TX_CHECK(Rando_GrantBossPrizeReceipt(
                         kGameDungeon_SkullWoods, 0x20,
                         kRandoGrantPresentation_None, 3, 0) ==
                         kRandoGrantResult_AlreadyChecked,
                     "boss-prize replay was not terminal", LOC_Skull_Woods_Prize);

    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    g_rando_active_settings_valid = true;
    g_rando_active_settings.shopsanity = 0;
    GRANT_TX_CHECK(Rando_ShopGrant(
                         0x10f, 0x6f, 2, 0x28,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_NotActive,
                     "plain Retro shop entered shopsanity transaction", 239);
    g_rando_active_settings.shopsanity = 1;
    link_bomb_filler = 0;
    GRANT_TX_CHECK(Rando_ShopGrant(
                         0x10f, 0x6f, 2, 0x28,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(239) && link_bomb_filler != 0,
                     "shopsanity transaction failed", 239);
    GRANT_TX_CHECK(Rando_ShopGrant(
                         0x10f, 0x6f, 2, 0x28,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_NotActive,
                     "checked shopsanity slot did not become vanilla restock", 239);

    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    g_rando_active_settings.shopsanity = 0;
    link_bomb_filler = 0;
    g_last_dispatched_item_id = 0xffffu;
    GRANT_TX_CHECK(Rando_CommitRepeatableShopIdentity(
                         0x10f, 0x6f, 2, 0x28) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(239) && link_bomb_filler == 0 &&
                       g_last_dispatched_item_id == ITEM_Bombs10 &&
                       Rando_CommitRepeatableShopIdentity(
                         0x10f, 0x6f, 2, 0x28) ==
                         kRandoGrantResult_Accepted &&
                       link_bomb_filler == 0,
                     "repeatable shop identity applied or failed", 239);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    event_entries[2].item_id = ITEM_Bombs1;
    GRANT_TX_CHECK(Rando_CommitRepeatableShopIdentity(
                         0x10f, 0x6f, 2, 0x27) ==
                         kRandoGrantResult_Invalid &&
                       !Rando_IsLocationChecked(239),
                     "same-code nonidentity shop placement was accepted", 239);
    event_entries[2].item_id = ITEM_Bombs10;

    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    link_rupees_goal = 0;
    GRANT_TX_CHECK(Rando_TakeAnyGrant(
                         0x13, 0x56, 0, 0x34,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_NotActive &&
                       !Rando_IsLocationChecked(266) &&
                       !Rando_IsLocationChecked(267),
                     "take-any grant accepted the wrong host room", 266);
    GRANT_TX_CHECK(Rando_TakeAnyGrant(
                         0x12, 0x56, 0, 0x34,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(266) &&
                       Rando_IsLocationChecked(267) && link_rupees_goal == 1,
                     "take-any grant/forfeit transaction failed", 266);
    GRANT_TX_CHECK(Rando_TakeAnyGrant(
                         0x12, 0x56, 0, 0x34,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_AlreadyChecked,
                     "take-any replay was not terminal", 266);

    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    event_entries[3].item_id = ITEM_BottleEmpty;
    memset(link_bottle_info, 3, 4);
    // A take-any offering a 5th bottle now TERMINATES rather than looping, so
    // it consumes the cave (chosen + forfeited sibling) and grants nothing.
    // That is a deliberate, documented cost of the uniform permanent-refusal
    // rule (design.md D6): the alternative is a plan the site can never satisfy.
    // Unlike the frame-locked sites this rule exists to fix, the take-any retry
    // was player-driven and not itself a softlock -- so this is the one place
    // where the fix is a small net loss, accepted to keep one contract rather
    // than a per-site policy.
    GRANT_TX_CHECK(Rando_TakeAnyGrant(
                         0x12, 0x56, 0, 0x16,
                         kRandoGrantPresentation_None, 0, 0) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(266) &&
                       Rando_IsLocationChecked(267) &&
                       link_bottle_info[0] == 3,
                     "take-any 5th bottle must terminate, not loop", 266);
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    memset(link_bottle_info, 0, 4);
    event_entries[3].item_id = ITEM_Rupee1;

    link_heart_pieces = 0;
    link_health_capacity = 0x18;
    RandoDeferredGrantToken poh_token;
    GRANT_TX_CHECK(Rando_PrepareGrant(
                         500, ITEM_PieceOfHeart, 0x17, &poh_token) ==
                         kRandoGrantResult_Accepted &&
                       Rando_CommitStandingPieceOfHeartIdentity(&poh_token) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(500) && link_heart_pieces == 0 &&
                       Rando_CommitStandingPieceOfHeartIdentity(&poh_token) ==
                         kRandoGrantResult_AlreadyChecked,
                     "standing PoH identity bookkeeping applied inventory or failed", 500);

    // At-cap plans resolve AcceptedNoOp; the identity commits must still
    // record the check — a 20-heart standing PoH must not strand the sprite
    // uncollectable, and an at-cap shop purchase must not defer its mark to
    // a later un-capped repurchase.
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    link_health_capacity = 0xa0;
    GRANT_TX_CHECK(Rando_PrepareGrant(
                         500, ITEM_PieceOfHeart, 0x17, &poh_token) ==
                         kRandoGrantResult_Accepted &&
                       poh_token.plan.disposition ==
                         kRandoGrantDisposition_AcceptedNoOp &&
                       Rando_CommitStandingPieceOfHeartIdentity(&poh_token) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(500),
                     "at-cap standing PoH identity did not commit", 500);
    link_health_capacity = 0x18;
    memset(g_rando_checked_bitmap, 0, sizeof(g_rando_checked_bitmap));
    link_item_bombs = 50;
    GRANT_TX_CHECK(Rando_CommitRepeatableShopIdentity(
                         0x10f, 0x6f, 2, 0x28) ==
                         kRandoGrantResult_Accepted &&
                       Rando_IsLocationChecked(239),
                     "at-cap shop identity did not mark its row", 239);
    link_item_bombs = 0;
  }

  GRANT_TX_CHECK(Ancilla_RandoFallingPrizeSelfCheck() == 0,
                 "falling-prize token ABI/commit probe failed", 7000);

cleanup:
  memcpy(g_ram, saved_ram, sizeof(saved_ram));
  memcpy(g_rando_checked_bitmap, saved_checked, sizeof(saved_checked));
  memcpy(Souls_Flags(), saved_soul_flags, sizeof(saved_soul_flags));
  Placement_Install(saved_placement);
  g_rando_slot_active = saved_slot_active;
  enhanced_features1 = saved_features1;
  g_rando_active_settings = saved_active_settings;
  g_rando_active_settings_valid = saved_active_settings_valid;
  g_rando_mushroom_held = saved_mushroom_held;
  g_rando_bow_owned = saved_bow_owned;
  g_reachability_state_counter = saved_reachability;
  g_last_dispatched_item_id = saved_last_item;
  g_last_dispatched_location_id = saved_last_location;
  g_last_dispatched_plan = saved_last_plan;
  g_last_dispatched_plan_valid = saved_last_valid;
  g_rando_last_soul_item_id = saved_last_soul_item;
  g_rando_soul_msg_pending = saved_soul_pending;
  g_rando_pot_confirmation_count = saved_confirmation_count;
  memcpy(g_rando_pot_confirmation_icons, saved_confirmation_icons,
         sizeof(saved_confirmation_icons));
#undef GRANT_TX_CHECK
  if (failure != NULL) {
    fprintf(stderr, "Rando_GrantTransactionSelfCheck: %s (location %u)\n",
            failure, (unsigned)failure_location);
    exit(2);
  }
  fprintf(stderr, "[Rando_GrantTransactionSelfCheck] OK\n");
}

static void Rando_PotOverlaySelfCheckOrDie(void) {
  if (RandoPot_OverlayOamSelfCheck()) {
    fprintf(stderr, "Rando_SelfCheck: pot overlay OAM allocation can clobber sorted sprites\n");
    exit(2);
  }
}

static void Rando_EnemyMarkerSelfCheckOrDie(void) {
  if (Rando_EnemyMarkerAllocatorSelfCheck()) {
    fprintf(stderr, "Rando_SelfCheck: enemy marker allocator failed\n");
    exit(2);
  }
}

static void Rando_OverlayPaletteSelfCheckOrDie(void) {
  if (Rando_OverlayPaletteSelfCheck()) {
    fprintf(stderr, "Rando_SelfCheck: overlay palette manager failed\n");
    exit(2);
  }
}

static void Rando_SpriteMainGrantPresentationSelfCheckOrDie(void) {
  if (!SpriteMain_GrantPresentationSelfCheck()) {
    fprintf(stderr, "Rando_SelfCheck: sprite-main grant presentation seam failed\n");
    exit(2);
  }
  fprintf(stderr, "[SpriteMain_GrantPresentationSelfCheck] OK\n");
}

static void Rando_SpriteGrantRetrySelfCheckOrDie(void) {
  int result = Sprite_GrantRetrySelfCheck();
  if (result != 0) {
    fprintf(stderr, "Rando_SelfCheck: sprite grant retry seam failed (%d)\n",
            result);
    exit(2);
  }
  fprintf(stderr, "[Sprite_GrantRetrySelfCheck] OK\n");
}

typedef enum RandoSelfCheckGroupId {
  kRandoSelfCheckGroup_Config,
  kRandoSelfCheckGroup_Grant,
  kRandoSelfCheckGroup_Logic,
  kRandoSelfCheckGroup_Generation,
  kRandoSelfCheckGroup_Runtime,
  kRandoSelfCheckGroup_Persistence,
  kRandoSelfCheckGroup_Ui,
  kRandoSelfCheckGroup_Count,
} RandoSelfCheckGroupId;

typedef void (*RandoSelfCheckFn)(void);
typedef struct RandoSelfCheckEntry {
  uint8 group;
  RandoSelfCheckFn run;
} RandoSelfCheckEntry;

static const char *const kRandoSelfCheckGroupNames[] = {
  "config", "grant", "logic", "generation", "runtime", "persistence", "ui",
};

// Keep this table in the exact historical Rando_RunAllSelfChecks order. The
// unqualified suite walks it once; named groups filter it without maintaining
// a second list that could silently lose coverage.
static const RandoSelfCheckEntry kRandoSelfChecks[] = {
  { kRandoSelfCheckGroup_Config,      Rando_SelfCheckCapacityABI },
  { kRandoSelfCheckGroup_Logic,       Rando_CrystalGateSelfCheck },
  { kRandoSelfCheckGroup_Ui,          Rando_DynamicHintFastForwardSelfCheck },
  { kRandoSelfCheckGroup_Config,      Rando_SelfCheck },
  { kRandoSelfCheckGroup_Grant,       ItemReceipt_FastFanfareSelfCheck },
  { kRandoSelfCheckGroup_Grant,       ItemReceipt_LosslessSelfCheck },
  { kRandoSelfCheckGroup_Grant,       Rando_GrantPlanSelfCheck },
  { kRandoSelfCheckGroup_Grant,       Rando_PrizeBitSelfCheck },
  { kRandoSelfCheckGroup_Grant,       Rando_GrantTransactionSelfCheck },
  { kRandoSelfCheckGroup_Grant,       Rando_SpriteMainGrantPresentationSelfCheckOrDie },
  { kRandoSelfCheckGroup_Grant,       Rando_SpriteGrantRetrySelfCheckOrDie },
  { kRandoSelfCheckGroup_Runtime,     Rando_PotOverlaySelfCheckOrDie },
  { kRandoSelfCheckGroup_Runtime,     Rando_EnemyMarkerSelfCheckOrDie },
  { kRandoSelfCheckGroup_Ui,          Rando_OverlayPaletteSelfCheckOrDie },
  { kRandoSelfCheckGroup_Config,      Rando_Rng_SelfCheck },
  { kRandoSelfCheckGroup_Config,      Rando_ShopPriceSelfCheck },
  { kRandoSelfCheckGroup_Config,      Share_SelfCheck },
  { kRandoSelfCheckGroup_Config,      Settings_SelfCheck },
  { kRandoSelfCheckGroup_Logic,       Logic_SelfCheck },
  { kRandoSelfCheckGroup_Logic,       Logic_KnowledgeMaskSelfCheck },  // tracker-player-knowledge
  { kRandoSelfCheckGroup_Generation,  Placement_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  SeedShape_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  Shuffles_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  BossShuffle_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  DropShuffle_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  EnemyShuffle_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  Souls_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  Chains_SelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Chains_RuntimeSelfCheck },
  { kRandoSelfCheckGroup_Generation,  Customizer_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  Customizer_PlacementSelfCheck },
  { kRandoSelfCheckGroup_Persistence, RandoSave_SelfCheck },
  { kRandoSelfCheckGroup_Persistence, Rando_GameSaveFailureSelfCheck },
  { kRandoSelfCheckGroup_Persistence, SaveGameFile_StagingSelfCheck },
  { kRandoSelfCheckGroup_Generation,  RandoGenerate_SelfCheck },
  { kRandoSelfCheckGroup_Persistence, RandoSnapshotTail_SelfCheck },
  { kRandoSelfCheckGroup_Ui,          TextField_SelfCheck },
  { kRandoSelfCheckGroup_Ui,          Hints_SelfCheck },
  { kRandoSelfCheckGroup_Ui,          RandoDialogue_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  Entrance_SelfCheck },
  { kRandoSelfCheckGroup_Generation,  OwWarp_SelfCheck },  // add-rando-ow-warp-shuffle
  { kRandoSelfCheckGroup_Generation,  InvertedEntrances_SelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Rando_EntranceContaminationSelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Rando_ReinstallOverlaysSelfCheck },
  { kRandoSelfCheckGroup_Generation,  Cosmetic_SelfCheck },
  { kRandoSelfCheckGroup_Logic,       Rando_TrackerSelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Rando_StartingInventorySelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Rando_MirrorlessAwayWorldSelfCheck },
  { kRandoSelfCheckGroup_Runtime,     Rando_ShuffleInstallSelfCheck },
  { kRandoSelfCheckGroup_Ui,          Rando_MedallionIcons_SelfCheck },
  { kRandoSelfCheckGroup_Ui,          RandoOwMap_SelfCheck },  // fix-ow-map-prize-markers
};

uint8 Rando_SelfCheckGroupCount(void) {
  return (uint8)countof(kRandoSelfCheckGroupNames);
}

const char *Rando_SelfCheckGroupName(uint8 index) {
  return index < Rando_SelfCheckGroupCount()
      ? kRandoSelfCheckGroupNames[index] : NULL;
}

bool Rando_RunSelfCheckGroup(const char *name) {
  int group = -1;
  if (name == NULL)
    return false;
  for (uint8 i = 0; i < Rando_SelfCheckGroupCount(); i++) {
    if (strcmp(name, kRandoSelfCheckGroupNames[i]) == 0) {
      group = i;
      break;
    }
  }
  if (group < 0)
    return false;  // unknown groups must not run any test

  int passes = group == kRandoSelfCheckGroup_Grant ? 2 : 1;
  for (int pass = 1; pass <= passes; pass++) {
    for (size_t i = 0; i < countof(kRandoSelfChecks); i++) {
      if (kRandoSelfChecks[i].group == group)
        kRandoSelfChecks[i].run();
    }
    fprintf(stderr, "[Rando_SelfCheckGroup:%s] pass %d/%d OK\n",
            name, pass, passes);
  }
  return true;
}

void Rando_RunAllSelfChecks(void) {
  for (size_t i = 0; i < countof(kRandoSelfChecks); i++)
    kRandoSelfChecks[i].run();
  fprintf(stderr, "Rando_RunAllSelfChecks: all subsystems OK.\n");
}

// ---------------------------------------------------------------------------
// Reverse of the shop resolver for the diagnostic below: the (room, door) pair
// that currently reaches shop block `base`, found by walking every overworld
// door row through the LIVE tables exactly as --rando-shop-doorwalk does. Under
// cave-entrance shuffle that door is the shuffled one, which is the whole point
// of asking. False when no door reaches the shop or the entrance assets are
// absent (the headless probe can run without them).
static bool ShopDbg_LiveDoorOfShop(uint16 base, uint16 *out_room, uint8 *out_door) {
  const uint8 *ids = (const uint8 *)kOverworld_Entrance_Id;
  const uint16 *rooms = kEntranceData_rooms;
  if (ids == NULL || rooms == NULL) return false;
  uint32 ndoors = kOverworld_Entrance_Id_SIZE;
  uint32 nrooms = kEntranceData_rooms_SIZE / 2u;
  for (uint32 lx = 0; lx < ndoors && lx < 0xFFu; lx++) {
    if (ids[lx] >= nrooms) continue;
    uint16 room = rooms[ids[lx]];
    if (shop_lookup(room, (uint8)(lx + 1), 0) != base) continue;
    if (out_room) *out_room = room;
    if (out_door) *out_door = (uint8)(lx + 1);
    return true;
  }
  return false;
}

// add-rando-shopsanity — playtest diagnostic. Prints the WHOLE shop-check
// decision chain (activation gates, then per-shop-slot resolution) to
// dump_shop_debug.txt AND stderr, so an "icons not drawing" playtest report
// localizes in one F12 dump / one headless probe run instead of a bisect
// (runtime-debugging discipline: instrument the whole chain at once).
// Read-only; no gameplay effect.
void Rando_DumpShopCheckDebug(void) {
  FILE *outs[2];
  outs[0] = stderr;
  outs[1] = fopen("dump_shop_debug.txt", "w");
  for (int o = 0; o < 2; o++) {
    FILE *f = outs[o];
    if (f == NULL) continue;
    fprintf(f, "[SHOPDBG] rando_active=%d slot_active=%d settings_valid=%d "
               "from_cold_replay=%d header_valid=%d\n",
            (enhanced_features1 & kFeatures1_RandomizerActive) ? 1 : 0,
            g_rando_slot_active ? 1 : 0,
            g_rando_active_settings_valid ? 1 : 0,
            g_rando_settings_from_cold_replay ? 1 : 0,
            g_rando_active_header_valid ? 1 : 0);
    fprintf(f, "[SHOPDBG] settings: shopsanity=%d bonk_shuffle=%d "
               "crystals=%d/%d effective=%d/%d world_state=%d "
               "shopsanity_active=%d\n",
            g_rando_active_settings.shopsanity,
            g_rando_active_settings.bonk_shuffle,
            g_rando_active_settings.crystals_ganon,
            g_rando_active_settings.crystals_tower,
            g_rando_effective_crystals_ganon, g_rando_effective_crystals_tower,
            g_rando_active_settings.world_state,
            rando_shopsanity_active() ? 1 : 0);
    // Print the quantities that ACTUALLY resolve a shop: the full 16-bit room
    // and the captured overworld door. which_entrance is kept only because it
    // is what a reader coming from the vanilla code expects to see — it is NOT
    // the key (four shops share entrance id 0x60, two share 0x58), and the low
    // room byte alone cannot tell 0x00F from 0x10F.
    fprintf(f, "[SHOPDBG] cur_room=%03X door=%02X (which_entrance=%02X, not the key)\n",
            dungeon_room_index, g_ram[kRam_RandoOverworldDoor], which_entrance);
    {
      uint16 live = Rando_ShopSlotLocForDoor(dungeon_room_index,
                                             g_ram[kRam_RandoOverworldDoor], 1);
      if (live == 0xFFFFu)
        fprintf(f, "[SHOPDBG] current (room,door) names no shop\n");
      else
        fprintf(f, "[SHOPDBG] current (room,door) names shop slots %u-%u\n",
                live, live + 2);
    }
    // All 27 slots, each printed against the door that CURRENTLY reaches it.
    // The per-shop (room, door) rows moved into the cave-interior table when
    // shop identity became layout-derived, so the live pair is recovered by
    // walking the overworld door table through the same resolver the runtime
    // uses — which also makes the dump show the shuffled door, not a vanilla
    // one, on a cave-entrance-shuffle slot.
    for (uint16 base = kRandoShopLocFirst;
         base < kRandoShopLocFirst + kRandoShopLocCount; base += 3) {
      uint16 shop_room = 0xFFFFu;
      uint8 shop_door = 0;
      if (!ShopDbg_LiveDoorOfShop(base, &shop_room, &shop_door))
        fprintf(f, "[SHOPDBG] shop %u-%u: NO door currently reaches it\n",
                base, base + 2);
      for (uint8 pos = 0; pos < 3; pos++) {
        uint16 loc = (uint16)(base + pos);
        uint16 placed = Placement_Lookup(loc, 0xFFFFu);
        uint16 ci_loc = 0xFFFFu, ci_item = 0xFFFFu, ci_price = 0;
        bool ci = Rando_ShopSlotCheckInfo(shop_room, shop_door,
                                          (uint8)(pos + 1),
                                          &ci_loc, &ci_item, &ci_price);
        uint8 gfx = 0, big = 0, fl = 0;
        int icon = ci ? Rando_GetShopCheckIcon(loc, &gfx, &big, &fl) : -1;
        fprintf(f, "[SHOPDBG] loc=%u room=%03X door=%02X pos=%u placed=%d "
                   "checked=%d checkinfo=%d item=%d price=%u icon=%d gfx=%02X\n",
                loc, shop_room, shop_door, pos,
                placed == 0xFFFFu ? -1 : (int)placed,
                Rando_IsLocationChecked(loc) ? 1 : 0,
                ci ? 1 : 0, ci ? (int)ci_item : -1,
                ci ? (unsigned)ci_price : 0u, icon, gfx);
      }
    }
    if (f != stderr) fclose(f);
  }
}
