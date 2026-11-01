"""Host test for the accented capitals added to the second-screen letter sheet.

SS_ComposeAccentedTile builds E-acute, E-grave, A-grave and C-cedilla out of
the ROM's own HUD font, so the accent has to be placed against whatever the
font looks like. This feeds it synthetic 8x8 glyphs with two, one and zero
clear rows and checks the mark lands outside the letter, never erasing it, and
that the UTF-8 decoding maps to the right cells. An ASCII dump of every case
goes to the log so a reviewer can eyeball the shapes.
"""

from pathlib import Path
import argparse
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'engine/src/second_screen.c'

PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t uint8;
#include "second_screen_letters.h"
'''

MAIN = r'''
static void show(const char *name, const uint8 *base, const uint8 *out) {
  printf("%s\n", name);
  for (int y = 0; y < 8; y++) {
    printf("  ");
    for (int x = 0; x < 8; x++) putchar(".#o"[base[y * 8 + x]]);
    printf("   ");
    for (int x = 0; x < 8; x++) putchar(".#o"[out[y * 8 + x]]);
    putchar('\n');
  }
}

// A capital E drawn with `top` clear rows above and `bottom_free` below.
static void make_glyph(uint8 *t, int top, int bottom_free) {
  memset(t, 0, 64);
  int rows = 8 - top - bottom_free;
  if (rows < 1) rows = 1;
  for (int r = 0; r < rows; r++)
    for (int x = 1; x < 6; x++)
      if (r == 0 || r == rows - 1 || r == rows / 2 || x == 1)
        t[(top + r) * 8 + x] = 2;
}

static int mark_rows(const uint8 *base, const uint8 *out, int *first, int *last) {
  int n = 0;
  *first = 8; *last = -1;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      if (out[y * 8 + x] == 2 && base[y * 8 + x] != 2) {
        n++;
        if (y < *first) *first = y;
        if (y > *last) *last = y;
      }
  return n;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  // The UTF-8 mapping the renderers rely on.
  char plain; int accent;
  assert(SS_DecodeAccented("\xC3\x89", &plain, &accent) == kSSLetterCell_EAcute
         && plain == 'E' && accent == kSSAccent_Acute);
  assert(SS_DecodeAccented("\xC3\x88", &plain, &accent) == kSSLetterCell_EGrave
         && plain == 'E' && accent == kSSAccent_Grave);
  assert(SS_DecodeAccented("\xC3\x80", &plain, &accent) == kSSLetterCell_AGrave
         && plain == 'A' && accent == kSSAccent_Grave);
  assert(SS_DecodeAccented("\xC3\x87", &plain, &accent) == kSSLetterCell_CCedilla
         && plain == 'C' && accent == kSSAccent_Cedilla);
  assert(SS_DecodeAccented("E", NULL, NULL) == -1);
  assert(SS_DecodeAccented("\xC3\xA9", NULL, NULL) == -1);   // lowercase, unused
  assert(kSSLetterCellCount == 30);

  uint8 base[64], out[64], acute_row0[8];
  for (int top = 0; top <= 2; top++) {
    make_glyph(base, top, 3 - top);
    for (int shape = kSSAccent_Acute; shape <= kSSAccent_Grave; shape++) {
      SS_ComposeAccentedTile(out, base, shape);
      char name[64];
      snprintf(name, sizeof(name), "%s, %d clear row(s) above",
               shape == kSSAccent_Acute ? "acute" : "grave", top);
      show(name, base, out);
      // The mark sits above the letter, and the letter survives -- shifted
      // down by one when it had no room of its own.
      int shift = top == 0 ? 1 : 0;
      int letter_top = top + shift;
      int marks = 0;
      for (int y = 0; y < letter_top; y++)
        for (int x = 0; x < 8; x++)
          if (out[y * 8 + x] == 2) marks++;
      assert(marks >= 2);
      if (shape == kSSAccent_Acute) memcpy(acute_row0, out, 8);
      else assert(memcmp(acute_row0, out, 8) != 0);   // grave must look different
      for (int y = 0; y + shift < 8; y++)
        for (int x = 0; x < 8; x++)
          if (base[y * 8 + x] == 2) assert(out[(y + shift) * 8 + x] == 2);
    }
  }
  for (int free_below = 0; free_below <= 2; free_below++) {
    make_glyph(base, 3 - free_below, free_below);
    SS_ComposeAccentedTile(out, base, kSSAccent_Cedilla);
    char name[64];
    snprintf(name, sizeof(name), "cedilla, %d clear row(s) below", free_below);
    show(name, base, out);
    int shift = free_below == 0 ? 1 : 0;
    int letter_bottom = 7 - free_below - shift;
    int marks = 0;
    for (int y = letter_bottom + 1; y < 8; y++)
      for (int x = 0; x < 8; x++)
        if (out[y * 8 + x] == 2) marks++;
    assert(marks >= 2);
    for (int y = shift; y < 8; y++)
      for (int x = 0; x < 8; x++)
        if (base[y * 8 + x] == 2) assert(out[(y - shift) * 8 + x] == 2);
  }
  // A blank tile must come back untouched rather than sprouting an accent.
  memset(base, 0, 64);
  SS_ComposeAccentedTile(out, base, kSSAccent_Acute);
  for (int i = 0; i < 64; i++) assert(out[i] == 0);

  puts("PASS: accent placement for 0/1/2 clear rows, letters preserved, "
       "UTF-8 mapping, blank tile untouched.");
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    text = SOURCE.read_text(encoding='utf-8')
    a = text.index('// Build one accented capital from a HUD-font tile.')
    b = text.index('// Letter sheet: 16 cols x 2 rows of 8x8.', a)
    # Copied next to the generated test instead of adding the engine's source
    # directory to the include path: it has a features.h that would shadow the
    # system one.
    shutil.copy(ROOT / 'engine/src/second_screen_letters.h', out)
    src = out / 'accented-letters-test.c'
    src.write_text(PRELUDE + text[a:b] + MAIN, encoding='utf-8')
    binary = out / 'accented-letters-test'
    log = out / 'accented-letters.log'
    with log.open('w') as handle:
        subprocess.run(['cc', '-O1', '-std=c11', '-fsanitize=address,undefined',
                        '-I' + str(out), str(src), '-o', str(binary)],
                       stdout=handle, stderr=subprocess.STDOUT, check=True)
        subprocess.run([str(binary)], stdout=handle, stderr=subprocess.STDOUT, check=True)
    print(log.read_text().strip().splitlines()[-1])
    print(f'Shapes dumped to {log}')


if __name__ == '__main__':
    main()
