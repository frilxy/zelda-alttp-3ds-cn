// Inverted overworld tile-overlay applier (hand-written; the data table lives in
// inverted_maps.c, generated from z3randomizer/invertedmaps.asm).
//
// This is the C reimplementation of z3randomizer's Overworld_LoadNewTiles: it
// walks the per-screen RLE command stream and writes map16 tiles into the live
// overworld map16 buffer ($7E2000 = dung_bg2 = g_ram+0x2000) right after the
// screen's quadrants are decompressed and before Map16ToMap8 converts them. The
// caller (Overworld_DrawQuadrantsAndOverlays in overworld.c) gates this on the
// Inverted world-state, so the engine here is unconditionally "inverted" — the
// asm's InvertedMode-conditional commands were already resolved at data-gen time.
//
// Wire format (per .map block in invertedmaps.asm):
//   word pairs  <tile>, <pos>              place tile at $7E0000+pos
//   command words (bit15 set):
//     0x8000|dir            Stripe:       <start>, <tile..>  (STOP=bit15, SKIP=0xFFFF)
//     0x8001|dir|size<<8    StripeRLE:    <tile>, <start>
//     0x8002|dir|size<<8    StripeRLEINC: <tile>, <start>
//     0x8003                ArbTileCopy:  <tile>, <pos..>    (last pos has bit15)
//     0x8011                OWW_CMD_CUSTOM <custom-id> [+ id-specific operands]
//     0xFFFF                OWW_END
//   dir bit (0x0080) selects vertical (+0x80) vs horizontal (+0x02) increment.
#include "inverted_maps.h"
#include "../variables.h"  // g_ram, overworld_screen_index, save_ow_event_info, etc.
#include "../assets.h"     // g_asset_ptrs/g_asset_sizes (map16 table shadow)
#include "../overworld.h"  // Overworld_DrawMap16_Persist (animated carve)
#include <string.h>

static inline void OWW_WriteTile(uint16 pos, uint16 tile) {
  // pos is the WRAM low address ($7E0000+pos); the map16 buffer (dung_bg2) is
  // g_ram[0x2000..0x5FFF]. CONFINE the write to that region: every legitimate
  // overlay position is in 0x2xxx-0x3Cxx, but pos is attacker-uncontrolled data
  // and a uint16 reaches into SRAM (g_ram[0xF000+]) and other live state. A
  // transcription bug or a stripe/RLE entry missing its terminator could
  // otherwise corrupt the save. Clamping makes the worst case a recoverable
  // visual glitch (wrong map16 tile) instead of state loss.
  if (pos < 0x2000 || pos >= 0x6000)
    return;
  *(uint16 *)(g_ram + pos) = tile;
}

// ---------------------------------------------------------------------------
// No-art Inverted Ganon pit on screen 0x1B (hole-only relocation).
//
// ALTTPR paints a full "pyramid" facade + a custom-gfx hole here. The facade and
// hole map16 blocks are authored for the DW-pyramid palette and share palette
// rows with the base Hyrule-Castle tiles, so they render mint-green/garbage with
// the castle gfx/palette loaded on 0x1B (the reason the whole overlay was
// suppressed). The faithful fix needs custom non-vanilla art (z3randomizer
// data/sheet73.gfx, which is unlicensed and not in this fork's ROM-extracted
// asset set).
//
// Instead we render ONLY the Ganon pit, with NO new art: a 2-tone dark diamond
// built from a solid castle tile (char 0x037) that already renders cleanly on
// 0x1B — color #292929 at palette row 4 (interior) and #393129 at row 7 (rim).
// The facade is skipped entirely, so screen 0x1B looks like normal Hyrule Castle
// plus a dark pit (post-Agahnim). See the OpenSpec change spike-findings.md.
//
// The two pit blocks don't exist in the vanilla map16 table, so we append them
// to a shadow of kMap16ToMap8 (asset 70) at the first free block ids, gated on
// the Inverted world-state. OverworldCopyMap16ToBuffer indexes the table by
// block id with no bounds check, so new high block ids are safe — and they are
// only ever placed at the 0x1B pit, so no other screen is affected.
enum {
  kAsset_Map16ToMap8 = 70,
  kInvHoleInteriorTile = 0x1037,  // char 0x037 | (palette 4 << 10)  -> #292929
  kInvHoleRimTile      = 0x1C37,  // char 0x037 | (palette 7 << 10)  -> #393129
};

// The pit footprint: dung_bg2 byte offsets (live map16 buffer, g_ram[0x2000+pos])
// matching the positions CreatePyramidHole carves. interior=1 -> dark center,
// interior=0 -> rim. A 4x4 diamond (corners omitted) reads as a round hole.
static const struct { uint16 pos; uint8 interior; } kInvCastleHoleCells[] = {
  {0x43e, 0}, {0x440, 0},
  {0x4bc, 0}, {0x4be, 1}, {0x4c0, 1}, {0x4c2, 0},
  {0x53c, 0}, {0x53e, 1}, {0x540, 1}, {0x542, 0},
  {0x5be, 0}, {0x5c0, 0},
};
#define kInvCastleHoleCellCount \
  (int)(sizeof(kInvCastleHoleCells) / sizeof(kInvCastleHoleCells[0]))

// Shadow buffer for the map16 table (vanilla ~30 KB) + 2 appended pit blocks.
static uint8  g_inv_map16_shadow[32768];
static const uint8 *g_inv_map16_orig = NULL;  // saved g_asset_ptrs[70]
static uint32 g_inv_map16_orig_size = 0;
static uint16 g_inv_hole_interior_id = 0;     // resolved block ids (first-free)
static uint16 g_inv_hole_rim_id = 0;

void InvertedHoleBlocks_Teardown(void) {
  if (g_inv_map16_orig != NULL) {
    g_asset_ptrs[kAsset_Map16ToMap8] = g_inv_map16_orig;
    g_asset_sizes[kAsset_Map16ToMap8] = g_inv_map16_orig_size;
    g_inv_map16_orig = NULL;
  }
  // Reset the resolved pit block ids so a stale id can't be painted if the carve/
  // static paint ever runs after teardown without a successful re-install.
  // Both consumers are world_state==Inverted-gated and a live Inverted slot
  // always re-installs, so this is purely defensive.
  g_inv_hole_interior_id = 0;
  g_inv_hole_rim_id = 0;
}

void InvertedHoleBlocks_Install(uint8 world_state) {
  InvertedHoleBlocks_Teardown();
  if (world_state != 2 /* kWorldState_Inverted */)
    return;
  uint32 sz = g_asset_sizes[kAsset_Map16ToMap8];
  uint32 nblk = sz / 8;                 // first free block id
  uint32 need = (nblk + 2) * 8;         // room for 2 appended blocks
  if (need > sizeof(g_inv_map16_shadow))
    return;                             // unexpected table size: leave vanilla
  g_inv_map16_orig = g_asset_ptrs[kAsset_Map16ToMap8];
  g_inv_map16_orig_size = sz;
  memcpy(g_inv_map16_shadow, g_inv_map16_orig, sz);
  g_inv_hole_interior_id = (uint16)nblk;
  g_inv_hole_rim_id = (uint16)(nblk + 1);
  uint16 *blk = (uint16 *)g_inv_map16_shadow;
  for (int j = 0; j < 4; j++) {
    blk[g_inv_hole_interior_id * 4 + j] = kInvHoleInteriorTile;
    blk[g_inv_hole_rim_id * 4 + j] = kInvHoleRimTile;
  }
  g_asset_ptrs[kAsset_Map16ToMap8] = g_inv_map16_shadow;
  g_asset_sizes[kAsset_Map16ToMap8] = need;
}

// Animated carve (post-Agahnim bat slam): write the pit into the live buffer and
// queue the matching VRAM upload. Called from CreatePyramidHole's Inverted branch.
void Inverted_CarveCastleHole(void) {
  for (int i = 0; i < kInvCastleHoleCellCount; i++) {
    uint16 blk = kInvCastleHoleCells[i].interior ? g_inv_hole_interior_id
                                                 : g_inv_hole_rim_id;
    Overworld_DrawMap16_Persist(kInvCastleHoleCells[i].pos, blk);
  }
}

void Overworld_ApplyInvertedTiles(void) {
  uint8 scr = (uint8)overworld_screen_index;
  if ((uint16)overworld_screen_index >= 0x80)
    return;

  // Screen 0x1B (LW Hyrule Castle): hole-only Inverted Ganon relocation. The
  // spurious "pyramid" facade overlay is NOT painted (it renders as garbage with
  // the castle gfx/palette — see the no-art note above). Instead we paint ONLY
  // the no-art Ganon pit, and only once Agahnim is defeated (the pyramid-hole
  // bit, save_ow_event_info[0x5B] & 0x20 — the same gate the DW-pyramid hole and
  // CreatePyramidHole use). Before Agahnim the screen is the clean castle.
  // (NOTE: this static path runs on full screen rebuilds; walking-scroll into
  // 0x1B is handled by the live carve. Verify both by playtest.)
  if (scr == 0x1B) {
    if (save_ow_event_info[0x5B] & 0x20) {
      for (int i = 0; i < kInvCastleHoleCellCount; i++)
        OWW_WriteTile((uint16)(0x2000 + kInvCastleHoleCells[i].pos),
                      kInvCastleHoleCells[i].interior ? g_inv_hole_interior_id
                                                      : g_inv_hole_rim_id);
    }
    return;
  }

  uint16 off = kInvertedMapOffsets[scr];
  if (off == 0xFFFF)
    return;  // no overlay for this screen

  const uint16 *p = &kInvertedMapData[off];

  for (;;) {
    uint16 cmd = *p++;
    if (cmd == OWW_END)
      return;

    if (!(cmd & 0x8000)) {
      // plain single tile-write: cmd = tile, next = pos
      uint16 pos = *p++;
      OWW_WriteTile(pos, cmd);
      continue;
    }

    if (cmd == OWW_CMD_CUSTOM) {
      uint16 custom_id = *p++;
      switch (custom_id) {
      case OWW_CUSTOM_MAP1B_AGA: {
        // Pyramid-of-power hole on screen 0x1B appears only after Agahnim is
        // defeated (save_ow_event_info[0x5B] & 0x20). The generator emitted the
        // hole-data word count next; if Agahnim is NOT defeated, skip it so we
        // fall through to the no-hole (tower-entry sign) data.
        uint16 hole_words = *p++;
        if (!(save_ow_event_info[0x5B] & 0x20))
          p += hole_words;
        break;
      }
      case OWW_CUSTOM_MAP5B_WARP:
        // Screen 0x5B warp tile: 0x0034 before the relevant progress milestone,
        // 0x0212 once progress >= 3, written to fixed pos $3BBE.
        OWW_WriteTile(0x3BBE, (sram_progress_indicator < 3) ? 0x0034 : 0x0212);
        break;
      default:
        break;
      }
      continue;
    }

    // stripe-family command. The command word is 1sss ssss dccc cccc: size in
    // bits 8-14, direction in bit 7, command id in the LOW 7 BITS. The asm
    // dispatch masks AND #$007F to index its vector table, so the id must NOT
    // include the direction bit (0x80) — a vertical stripe (e.g. 0x8080 Stripe,
    // 0x8581 StripeRLE) otherwise decodes to opid 0x80/0x81, matches no branch,
    // and is dropped while its operands are misread as plain tile-writes.
    int opid = cmd & 0x007F;
    int inc = (cmd & 0x0080) ? 0x80 : 0x02;  // vertical vs horizontal

    if (opid == 0x00) {
      // Stripe: <start>, then tiles until a terminator.
      uint16 x = *p++;
      bool first = true;
      for (;;) {
        if (!first)
          x = (uint16)(x + inc);
        first = false;
        uint16 t = *p++;
        if (t == OWW_SKIP)
          continue;  // skip this position, keep advancing
        if (t & 0x8000) {
          OWW_WriteTile(x, (uint16)(t & 0x7FFF));
          break;  // STOP
        }
        OWW_WriteTile(x, t);
      }
    } else if (opid == 0x01 || opid == 0x02) {
      // StripeRLE / StripeRLEINC: <tile>, <start>; size in high byte of cmd.
      int size = (cmd >> 8) & 0x7F;
      uint16 tile = *p++;
      uint16 x = *p++;
      bool first = true;
      while (size-- > 0) {
        if (!first) {
          x = (uint16)(x + inc);
          if (opid == 0x02)
            tile = (uint16)(tile + 1);  // RLEINC increments the tile
        }
        first = false;
        OWW_WriteTile(x, tile);
      }
    } else if (opid == 0x03) {
      // ArbTileCopy: <tile>, then positions until one has bit15 set (last).
      uint16 tile = *p++;
      for (;;) {
        uint16 pos = *p++;
        if (pos & 0x8000) {
          OWW_WriteTile((uint16)(pos & 0x7FFF), tile);
          break;
        }
        OWW_WriteTile(pos, tile);
      }
    }
    // opid 0x04..0x0C, 0x11..0x19 are .nothing in the asm; never emitted.
  }
}
