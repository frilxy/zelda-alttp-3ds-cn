#!/usr/bin/env python3
"""The 240-line WIDE picture moves up at the bottom of an area or a room and
shows up to 16 rows above the game's screen. Builds Sprite_KeepsOamY and
Sprite_OamY from sprite.h and draws one 8 or 16-pixel tile at every y around
the screen through both sprite paths of the PPU: every tile the picture shows
is kept and drawn on its own lines (one 16 lines above the game's screen a
line lower, 0xf0 hiding a tile); without these rows, the game's own rule."""
from pathlib import Path
import subprocess, tempfile
r = Path(__file__).resolve().parents[3]
s = (r / 'engine/src/sprite.h').read_text()
def function(text, signature):
  a = text.index(signature); b = text.index('{', a); n = 1; e = b + 1
  while n:
    n += (text[e] == '{') - (text[e] == '}'); e += 1
  return text[a:e] + '\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "snes/ppu.h"
typedef uint8_t uint8;
typedef uint16_t uint16;
int g_wide_picture_rows;
'''
code += function(s, 'static inline bool Sprite_KeepsOamY(uint16 y)')
code += function(s, 'static inline uint8 Sprite_OamY(uint16 y)')
code += r'''
static uint32_t buf[512 * 240];
// The lines of the 240-line picture where the PPU draws a tile placed y
// lines below the top of the game's screen, the picture moved up by rows.
static void Drawn(Ppu *p, int path, int rows, int big, int y, bool lines[240]) {
  g_wide_picture_rows = rows;
  ppu_reset(p); p->forcedBlank = false; p->brightness = 15; p->mode = 1; p->objSize = 0;
  p->extraLeftRight = p->extraLeftCur = p->extraRightCur = 72; p->extraBottomCur = 16;
  p->screenEnabled[0] = 0x10;
  for (int i = 0; i < 256; i++) p->cgram[i] = 0;
  p->cgram[0x81] = 0x7fff;
  for (int t = 0; t < 2; t++) for (int u = 0; u < 2; u++) for (int i = 0; i < 8; i++) {
    int a = 0x4000 + (t * 16 + u) * 16 + i;
    p->vram[a] = 0x00ff; p->vram[a + 8] = 0;
  }
  for (int i = 0; i < 0x100; i += 2) { p->oam[i] = 0xf000; p->oam[i + 1] = 0; }
  for (int i = 0x100; i < 0x110; i++) p->oam[i] = 0;
  uint8 oy = Sprite_KeepsOamY((uint16)y) ? Sprite_OamY((uint16)y) : 0xf0;
  p->oam[0] = 100 | oy << 8; p->oam[0x100] = big ? 2 : 0;
  memset(buf, 0, sizeof(buf));
  PpuBeginDrawing(p, (uint8_t *)buf, 512 * 4, 1 | (path ? kPpuRenderFlags_FastPaths : 0));
  p->renderObjYOffset = rows;  // as BeginWideRender moves the picture up
  for (int l = 0; l < 240; l++) ppu_runLine(p, l + 1);
  for (int l = 0; l < 240; l++) {
    lines[l] = false;
    for (int x = 0; x < 400; x++) if (buf[l * 512 + x] & 0xffffff) lines[l] = true;
  }
}
int main(void) {
  // Without the rows, the game's rule: kept from 16 lines above its screen
  // to 16 below its 224 lines, the low byte as the OAM y.
  g_wide_picture_rows = 0;
  for (unsigned y = 0; y < 0x10000; y++) {
    assert(Sprite_KeepsOamY((uint16)y) == ((uint16)(y + 0x10) < 0x100));
    if (Sprite_KeepsOamY((uint16)y)) assert(Sprite_OamY((uint16)y) == (uint8)y);
  }
  puts("PASS without the rows, the game's own rule for all 65,536 values");
  Ppu *p = ppu_init();
  static const int kRows[] = {0, 1, 2, 7, 8, 9, 15, 16};
  unsigned tiles = 0, lower = 0;
  bool lines[240];
  for (unsigned i = 0; i < sizeof(kRows) / sizeof(kRows[0]); i++)
  for (int big = 0; big < 2; big++) for (int y = -48; y < 272; y++) for (int path = 0; path < 2; path++) {
    int rows = kRows[i], size = big ? 16 : 8, top = y + rows;
    // On the picture, a tile covers its lines from y + rows: the game keeps
    // every tile one of them is on.
    if (top + size > 0 && top < 240) { g_wide_picture_rows = rows; assert(Sprite_KeepsOamY((uint16)y)); }
    g_wide_picture_rows = rows;
    bool kept = Sprite_KeepsOamY((uint16)y), moved = kept && rows && (uint8)y == 0xf0;
    if (moved) top++;
    Drawn(p, path, rows, big, y, lines);
    for (int l = 0; l < 240; l++) {
      bool want = kept && l >= top && l < top + size;
      if (lines[l] != want) {
        fprintf(stderr, "FAIL rows=%d size=%d y=%d path=%d line %d drawn=%d expected=%d\n",
                rows, size, y, path, l, lines[l], want);
        return 1;
      }
    }
    tiles++; lower += moved;
  }
  ppu_free(p);
  printf("PASS %u tiles: each one the moved-up picture shows is kept and drawn on its lines, "
         "both sprite paths (%u at 0xf0 a line lower)\n", tiles, lower);
  return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='wide-sprite-rows-') as t:
  d = Path(t); (d / 'test.c').write_text(code)
  subprocess.run(['cc', '-std=c11', '-O1', '-fsanitize=address,undefined', '-fno-sanitize=shift-base',
                  '-fno-sanitize-recover=all', '-I' + str(r / 'engine'),
                  str(d / 'test.c'), str(r / 'engine/snes/ppu.c'), '-o', str(d / 'test')], check=True)
  subprocess.run([str(d / 'test')], check=True)
