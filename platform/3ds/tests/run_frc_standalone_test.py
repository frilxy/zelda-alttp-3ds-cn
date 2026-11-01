"""Host test for the assets built from the French Canadian cartridge alone.

Always: AssetsFrC_Renumber (assets_fr_c.c) is run on a synthetic assets file
laid out as zelda3_assets_fr-c.bps builds it, and every map16 number, tile
attribute, map32 entry and message number has to come out in the USA
numbering; a file with another layout has to be refused untouched.

With --rom pointing at "Legend of Zelda, The - A Link to the Past (Canada)":
the 3DS setup path (TryBuildStandaloneFrCAssets, sliced out of
platform_3ds.c and linked against the engine's util.c) and the Android one
(BpsPatcher, FrcAssets, TranslationExtractor, when a JDK is on PATH) have to
build the same assets from that ROM alone, whose sha256 is below. They were
checked against the fr-c profile built with the USA ROM: every asset is the
same except the ones only that cartridge has (its dialogue and font, its
screens' graphics and tilemaps, the maps of three overworld screens and the
placeholders of the USA credits, which that profile never draws).
"""

from pathlib import Path
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / 'engine'
PLATFORM = ROOT / 'platform/3ds/source/platform_3ds.c'
JAVA_DIR = ROOT / 'app/src/main/java/com/dishii/zelda3'
PATCH = ROOT / 'platform/3ds/romfs/zelda3_assets_fr-c.bps'
FRC_ROM_SHA1 = 'c1c6c7f76fff936c534ff11f87a54162fc0aa100'
ASSETS_SHA256 = 'd4bd650d1ab1090d9110f0ee6db18a50982e868eed797869b67d676dd87a0dea'

SYNTHETIC = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assets.h"
#include "assets_fr_c.h"
static const uint32 kSizes[] = { [9] = 640, [60] = 13326, [61] = 13326, [62] = 13326,
  [63] = 13326, [70] = 30016, [110] = 256, [164] = 3824 };
static uint8 *Asset(uint8 *blob, int i) {
  size_t o = 88 + kNumberOfAssets * 4;
  for (int k = 0; k < i; k++) o = ((o + 3) & ~3) + kSizes[k];
  return blob + ((o + 3) & ~3);
}
static void Put(uint8 *g, int k, int v) {
  g[k] = (uint8)v;
  g[4 + (k >> 1)] |= (uint8)((v >> 8) << (k & 1 ? 0 : 4));
}
static int Get(const uint8 *g, int k) {
  int n = g[4 + (k >> 1)];
  return g[k] | (k & 1 ? n & 15 : n >> 4) << 8;
}
int main(void) {
  static const uint8 kSig[] = { kAssets_Sig };
  size_t size = 88 + kNumberOfAssets * 4;
  for (int k = 0; k < kNumberOfAssets; k++) size = ((size + 3) & ~3) + kSizes[k];
  uint8 *blob = calloc(1, size), *copy = malloc(size);
  memcpy(blob, kSig, sizeof(kSig));
  blob[80] = kNumberOfAssets;
  for (int k = 0; k < kNumberOfAssets; k++) memcpy(blob + 88 + k * 4, &kSizes[k], 4);
  uint8 *m16 = Asset(blob, 70), *attr = Asset(blob, 164), *m32 = Asset(blob, 60);
  for (int c = 0; c < 3752; c++) { m16[c * 8] = (uint8)c; m16[c * 8 + 1] = (uint8)(c >> 8); attr[c] = (uint8)(c * 7); }
  // cartridge number -> USA number the renumbering must give
  static const int kPairs[][2] = { {0, 0}, {1056, 1056}, {2468, 2468}, {2834, 2834}, {2835, 2857},
    {2837, 2835}, {2847, 2845}, {2850, 2886}, {2852, 2850}, {2858, 2856}, {2886, 2846},
    {2887, 2888}, {3324, 3325}, {3325, 2847}, {3356, 3356}, {3357, 3748}, {3360, 3751},
    {3361, 3357}, {3751, 3747} };
  int n = sizeof(kPairs) / sizeof(kPairs[0]);
  for (int i = 0; i < n; i++) Put(m32 + (i / 4) * 6, i % 4, kPairs[i][0]);
  uint8 *tele = Asset(blob, 9), *sign = Asset(blob, 110);
  tele[0] = 3; tele[2] = 4; tele[4] = 179; sign[0] = 0xff; sign[1] = 0x01;
  memcpy(copy, blob, size);
  copy[88 + 70 * 4] ^= 8;  // kMap16ToMap8 of another size: refused
  uint8 *before = malloc(size); memcpy(before, copy, size);
  if (AssetsFrC_Renumber(copy, size) || memcmp(before, copy, size)) { printf("FAIL: odd layout accepted or changed\n"); return 1; }
  if (!AssetsFrC_Renumber(blob, size)) { printf("FAIL: layout refused\n"); return 1; }
  for (int i = 0; i < n; i++) {
    int u = kPairs[i][1];
    if (Get(m32 + (i / 4) * 6, i % 4) != u) { printf("FAIL: map32 value %d -> %d\n", kPairs[i][0], Get(m32 + (i / 4) * 6, i % 4)); return 1; }
    if ((m16[u * 8] | m16[u * 8 + 1] << 8) != kPairs[i][0] || attr[u] != (uint8)(kPairs[i][0] * 7)) { printf("FAIL: map16 slot %d\n", u); return 1; }
  }
  if (tele[0] != 3 || tele[2] != 5 || tele[4] != 180 || sign[0] != 0 || sign[1] != 2) { printf("FAIL: messages\n"); return 1; }
  printf("PASS: synthetic renumbering, %d map16 pairs, messages, odd layout refused\n", n);
  free(before);
  free(copy);
  free(blob);
  return 0;
}
'''

HOST_SETUP = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "assets.h"
#include "util.h"
#include "assets_fr_c.h"
#include "localized.h"
void Die(const char *error) { fprintf(stderr, "%s\n", error); exit(1); }
static void LogSetup(const char *format, ...) { va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap); fputc('\n', stderr); }
static uint32 ReadU32LE(const uint8 *data);
@SLICES@
int main(int argc, char **argv) {
  size_t n = 0, size = 0;
  uint8 *rom = argc == 3 ? ReadWholeFile(argv[1], &n) : NULL, *out = NULL;
  if (!rom || !TryBuildStandaloneFrCAssets(rom, n, &out, &size)) return 1;
  FILE *f = fopen(argv[2], "wb");
  int failed = !f || fwrite(out, 1, size, f) != size || fclose(f) != 0;
  free(out);
  free(rom);
  return failed;
}
'''

JAVA_RUNNER = r'''
package com.dishii.zelda3;
import java.nio.file.*;
public class FrcStandaloneRunner {
    public static void main(String[] a) throws Exception {
        byte[] rom = Files.readAllBytes(Paths.get(a[0])), frc = Files.readAllBytes(Paths.get(a[1]));
        if (!BpsPatcher.matchesSource(rom, frc)) throw new Exception("patch does not match the ROM");
        TranslationExtractor.Language lang = TranslationExtractor.extract(rom);
        byte[] dat = BpsPatcher.apply(rom, frc);
        FrcAssets.renumber(dat);
        Files.write(Paths.get(a[2]), TranslationExtractor.setOnlyLanguage(dat, lang));
    }
}
'''


def slice_function(text, start):
    i = text.index(start)
    j = text.index('\n}\n', i) + 3
    return text[i:j]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rom', type=Path, help='real (Canada) ROM, optional')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cflags = ['-O1', '-Wall', '-Werror', '-Wno-unused-function', '-I', str(ENGINE),
              '-iquote', str(ENGINE / 'src')]

    src = args.out / 'frc_renumber_test.c'
    src.write_text(SYNTHETIC)
    binary = args.out / 'frc_renumber_test'
    subprocess.run(['cc', *cflags, str(src), str(ENGINE / 'src/assets_fr_c.c'), '-o', str(binary)],
                   check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    print(result.stdout.strip())
    if result.returncode != 0:
        return 1

    if not args.rom:
        print('SKIP: no --rom given, the standalone build from the real ROM was not run.')
        return 0
    rom = args.rom.read_bytes()
    if len(rom) & 0x3ff == 0x200:
        rom = rom[0x200:]
    if hashlib.sha1(rom).hexdigest() != FRC_ROM_SHA1:
        print('FAIL: --rom is not the French Canadian cartridge')
        return 1
    rom_path = args.out / 'frc.sfc'
    rom_path.write_bytes(rom)

    sdl = shutil.which('sdl2-config')
    if not sdl:
        print('SKIP: no sdl2-config on PATH, the 3DS path was not built.')
    else:
        text = PLATFORM.read_text()
        a = text.index('typedef struct OwnedBlock {')
        b = text.index('static bool TryBuildTranslatedAssets(')
        patch_line = text[text.index('static const char kBundledPatchFrC[]'):]
        slices = (patch_line[:patch_line.index('\n') + 1] +
                  slice_function(text, 'static char *Trim(char *text) {') +
                  slice_function(text, 'static bool AssetsBlobLooksValid(') +
                  text[a:b]).replace('romfs:/', str(ROOT / 'app/src/main/assets') + '/')
        host = args.out / 'frc_setup_host.c'
        host.write_text(HOST_SETUP.replace('@SLICES@', slices))
        sdl_cflags = subprocess.run([sdl, '--cflags'], capture_output=True, text=True).stdout.split()
        sdl_libs = subprocess.run([sdl, '--libs'], capture_output=True, text=True).stdout.split()
        setup = args.out / 'frc_setup_host'
        subprocess.run(['cc', *cflags, '-w', *sdl_cflags, str(host), str(ENGINE / 'src/util.c'),
                        str(ENGINE / 'src/assets_fr_c.c'), '-o', str(setup), *sdl_libs], check=True)
        work = args.out / 'profile'
        work.mkdir(exist_ok=True)
        out = work / 'assets.dat'
        subprocess.run([str(setup), str(rom_path), str(out)], check=True, cwd=work,
                       capture_output=True)
        digest = hashlib.sha256(out.read_bytes()).hexdigest()
        ini = (work / 'zelda3.ini').read_text()
        cart = (work / 'zelda3_fr-c.sfc').read_bytes() == rom
        if digest != ASSETS_SHA256 or 'Language = fr-c' not in ini or not cart:
            print(f'FAIL: 3DS path: assets {digest}, ini {ini!r}, cartridge copy {cart}')
            return 1
        print('PASS: 3DS path builds the assets from the cartridge alone, '
              'writes Language = fr-c and keeps the cartridge copy')

    if not shutil.which('javac'):
        print('SKIP: no javac on PATH, Android path not checked.')
        return 0
    classes = args.out / 'classes'
    classes.mkdir(exist_ok=True)
    runner = args.out / 'FrcStandaloneRunner.java'
    runner.write_text(JAVA_RUNNER)
    subprocess.run(['javac', '-d', str(classes), str(runner),
                    *(str(JAVA_DIR / n) for n in ('BpsPatcher.java', 'FrcAssets.java',
                                                   'TranslationExtractor.java'))],
                   check=True, capture_output=True)
    jout = args.out / 'assets_java.dat'
    subprocess.run(['java', '-cp', str(classes), 'com.dishii.zelda3.FrcStandaloneRunner',
                    str(rom_path), str(PATCH), str(jout)], check=True, capture_output=True)
    digest = hashlib.sha256(jout.read_bytes()).hexdigest()
    if digest != ASSETS_SHA256:
        print(f'FAIL: Android path: {digest}')
        return 1
    print('PASS: Android path builds the same assets')
    return 0


if __name__ == '__main__':
    sys.exit(main())
