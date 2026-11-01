#!/usr/bin/env python3
"""DimFlashes dims three floor colors of the dungeons as the Virtual Console
does: only their low byte changes (less red, and less green for the third),
their blue and the rest of their green stay. Builds ZeldaApplyDimFlashesPalette
from main.c with the USA cartridge's colors at $1B:E03C, $1B:E03E, $1B:E040."""
from pathlib import Path
import subprocess, tempfile
r = Path(__file__).resolve().parents[3]
m = (r / 'engine/src/main.c').read_text()
def function(text, signature):
  a = text.index(signature); b = text.index('{', a); n = 1; e = b + 1
  while n:
    n += (text[e] == '{') - (text[e] == '}'); e += 1
  return text[a:e] + '\n'
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
typedef uint16_t uint16;
static uint16 palette[0x500];
#define kPalette_DungBgMain palette
'''
code += function(m, 'void ZeldaApplyDimFlashesPalette(bool enable)')
code += r'''
int main(void) {
  static const uint16 usa[3] = { 0x0c72, 0x109c, 0x11ff };
  static const uint16 vc[3] = { 0x0c70, 0x1095, 0x1157 };
  for (int i = 0; i < 0x500; i++) palette[i] = (uint16)(i * 7);
  for (int i = 0; i < 3; i++) palette[0x484 + i] = usa[i];
  for (int round = 0; round < 2; round++) {
    ZeldaApplyDimFlashesPalette(true);
    for (int i = 0; i < 3; i++) assert(palette[0x484 + i] == vc[i]);
    ZeldaApplyDimFlashesPalette(true);
    for (int i = 0; i < 3; i++) assert(palette[0x484 + i] == vc[i]);
    ZeldaApplyDimFlashesPalette(false);
    for (int i = 0; i < 3; i++) assert(palette[0x484 + i] == usa[i]);
  }
  for (int i = 0; i < 0x500; i++)
    if (i < 0x484 || i > 0x486) assert(palette[i] == (uint16)(i * 7));
  puts("PASS DimFlashes dims the low byte of the three floor colors only");
  return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
  src = Path(d) / 'test.c'; exe = Path(d) / 'test'
  src.write_text(code)
  subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
  print(subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip())
