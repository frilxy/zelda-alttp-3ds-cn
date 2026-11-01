"""Host test for the ROM text layouts used by the setup screens.

Slices the layout table and the extractor straight out of platform_3ds.c, links
them against host stubs and checks two things:

  * a synthetic Canadian-layout ROM round-trips: every European command is
    rewritten to the encoding Text_DecodeCmd expects, dictionary references are
    rebased to 0x88, the bank switch is followed and the missing item-select
    message is spliced back in;
  * with --rom pointing at the real "Legend of Zelda, The - A Link to the Past
    (Canada)" dump, the packed dialogue and font blocks match the bytes upstream
    zelda3 produces for its fr-c language (golden hashes below).

The Android extractor is built from the same layout table, so when a JDK is on
PATH it is compiled and run against the same inputs and has to agree with the C
one byte for byte.

The ROM part is skipped when no ROM is given, so this runs in CI unchanged.
"""

from pathlib import Path
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'platform/3ds/source/platform_3ds.c'
JAVA_SOURCE = ROOT / 'app/src/main/java/com/dishii/zelda3/TranslationExtractor.java'

# sha1 of the ROM the fr-c layout describes, and sha256 of the two packed
# blocks the extractor must produce from it. The block bytes were cross-checked
# against upstream zelda3's `restool.py --languages=fr-c` output, which drops
# the scroll speed command: the dialogue here is that output with the one the
# ROM has kept, 87 50 at the start of message 274, the legend of the attract
# mode (31c3456a... without it).
FRC_ROM_SHA1 = 'c1c6c7f76fff936c534ff11f87a54162fc0aa100'
FRC_DIALOGUE_SHA256 = '1bb26902b72190abd773814e652226357a2133d32f9a654c435ed98011260f01'
FRC_FONT_SHA256 = 'ba226835c69e537c9c05cb2220062c32dbb5d8debb07a61e7bb21ef0cc09e76b'

PRELUDE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef uint8_t uint8; typedef uint16_t uint16; typedef uint32_t uint32;
typedef uint64_t uint64;
#define countof(a) (sizeof(a)/sizeof(*(a)))
typedef struct ByteArray { uint8 *data; size_t size, capacity; } ByteArray;
static void ByteArray_AppendData(ByteArray *a, const uint8 *d, size_t n) {
  if (a->size + n > a->capacity) {
    a->capacity = (a->size + n) * 2 + 64;
    a->data = realloc(a->data, a->capacity);
    assert(a->data);
  }
  memcpy(a->data + a->size, d, n);
  a->size += n;
}
static void ByteArray_Destroy(ByteArray *a) { free(a->data); a->data = NULL; a->size = a->capacity = 0; }
#define LogSetup(...) ((void)0)
'''

MAIN = r'''
static void dump(const char *path, const OwnedBlock *b) {
  FILE *f = fopen(path, "wb");
  assert(f);
  if (b->size) assert(fwrite(b->data, 1, b->size, f) == b->size);
  fclose(f);
}

int main(int argc, char **argv) {
  assert(argc == 4);
  size_t size = 0;
  FILE *f = fopen(argv[1], "rb");
  assert(f);
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8 *rom = malloc(size);
  assert(rom && fread(rom, 1, size, f) == size);
  fclose(f);

  OwnedBlock dialogue = {0}, font = {0};
  const RomTextLayout *layout = ExtractTranslationLanguage(rom, size, &dialogue, &font);
  if (!layout) { puts("NOMATCH"); return 1; }
  printf("%s\n", layout->language_code ? layout->language_code : "us");
  printf("%d\n", (int)layout->dialogue_flags);
  dump(argv[2], &dialogue);
  dump(argv[3], &font);
  free(dialogue.data);
  free(font.data);
  free(rom);
  return 0;
}
'''


JAVA_MAIN = r'''
package com.dishii.zelda3;

import java.io.FileOutputStream;
import java.io.RandomAccessFile;

public final class FrcTextMain {
    public static void main(String[] args) throws Exception {
        RandomAccessFile file = new RandomAccessFile(args[0], "r");
        byte[] rom = new byte[(int) file.length()];
        file.readFully(rom);
        file.close();
        TranslationExtractor.Language lang = TranslationExtractor.extract(rom);
        System.out.println(lang.code);
        System.out.println(lang.flags);
        new FileOutputStream(args[1]).write(lang.dialogueBlk);
        new FileOutputStream(args[2]).write(lang.fontBlk);
    }
}
'''


def build_java(out):
    """Compile the Android extractor with a tiny main, or return None."""
    if not JAVA_SOURCE.is_file() or not shutil.which('javac'):
        return None
    pkg = out / 'java/com/dishii/zelda3'
    pkg.mkdir(parents=True, exist_ok=True)
    shutil.copy(JAVA_SOURCE, pkg / 'TranslationExtractor.java')
    (pkg / 'FrcTextMain.java').write_text(JAVA_MAIN, encoding='utf-8')
    classes = out / 'java-classes'
    classes.mkdir(exist_ok=True)
    with (out / 'frc-text-java.log').open('w') as log:
        subprocess.run(['javac', '-d', str(classes),
                        str(pkg / 'TranslationExtractor.java'), str(pkg / 'FrcTextMain.java')],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    return classes


def run_java(classes, rom_path, out_dir, tag):
    dialogue = out_dir / f'{tag}-java-dialogue.bin'
    font = out_dir / f'{tag}-java-font.bin'
    result = subprocess.run(['java', '-cp', str(classes), 'com.dishii.zelda3.FrcTextMain',
                             str(rom_path), str(dialogue), str(font)],
                            capture_output=True, text=True, check=True)
    code, flags = result.stdout.split()
    return code, int(flags), dialogue.read_bytes(), font.read_bytes()


def slice_source(text, first, last_marker):
    a = text.index(first)
    b = text.index(last_marker, a)
    return text[a:b]


def unpack(blk):
    """Mirror of FindIndexInMemblk / UnpackBlocks."""
    end = len(blk) - 2
    mx = struct.unpack_from('<H', blk, end)[0]
    width = 2
    if mx >= 8192:
        mx -= 8192
        width = 4
    base = mx * width
    out, prev = [], 0
    for i in range(mx + 1):
        if i == mx:
            nxt = end - base
        elif width == 2:
            nxt = struct.unpack_from('<H', blk, i * 2)[0]
        else:
            nxt = struct.unpack_from('<I', blk, i * 4)[0]
        out.append(blk[base + prev:base + nxt])
        prev = nxt
    return out


def build_synthetic_rom():
    """A 1 MB ROM that follows the fr-c layout with content we can predict."""
    rom = bytearray(b'\x00' * 0x100000)
    rom[0x7fc0:0x7fd5] = b'LA LEGENDE DE ZELDA  '   # internal name, gates the layout

    # Every European command that has an engine equivalent, and what the
    # extractor must turn it into.
    cases = [
        (b'\x73', b'\x80'),                  # Scroll
        (b'\x7e', b'\x81'),                  # Waitkey
        (b'\x74', b'\x82'),                  # line 1
        (b'\x75', b'\x83'),                  # line 2
        (b'\x76', b'\x84'),                  # line 3
        (b'\x83', b'\x85'),                  # player name
        (b'\x78\x05', b'\x87\x05'),          # Wait 5
        (b'\x77\x03', b'\x87\x13'),          # Color 3
        (b'\x85\x02', b'\x87\x22'),          # Number 2
        (b'\x7a\x00', b'\x87\x30'),          # Speed 0
        (b'\x79\x2d', b'\x87\x40'),          # Sound
        (b'\x81', b'\x87\x80'),              # Choose
        (b'\x72', b'\x87\x81'),              # Choose2
        (b'\x71', b'\x87\x82'),              # Choose3
        (b'\x70', b'\x87\x83'),              # Selchg
        (b'\x82', b'\x87\x84'),              # Item
        (b'\x80', b'\x87\x85'),              # NextPic
        (b'\x84\x02', b'\x87\x86'),          # Window: tall box
        (b'\x84\x00', b''),                  # Window: default box, dropped
        (b'\x86\x00', b'\x87\x87'),          # Position 0
        (b'\x86\x01', b'\x87\x88'),          # Position 1
        (b'\x87\x00', b'\x87\x50'),          # ScrollSpd 0
        (b'\x87\x04', b'\x87\x54'),          # ScrollSpd 4
        (b'\x90\x91\xff', b'\x88\x89\xf7'),  # dictionary refs rebased by 8
        (b'\x00\x0f\x6f', b'\x00\x0f\x6f'),  # letters pass through
    ]
    expected = []
    stream = bytearray()
    for src, want in cases:
        stream += src + b'\x7f'
        expected.append(want)
    # Pad to 396 messages so the item-select template gets spliced back in.
    while len(expected) < 396:
        body = bytes([len(expected) % 0x6f])
        stream += body + b'\x7f'
        expected.append(body)
    # Split the stream across the two banks at a message boundary.
    cut = stream.index(b'\x7f', len(stream) // 2) + 1
    rom[0xe0000:0xe0000 + cut] = stream[:cut]
    rom[0xe0000 + cut] = 0x88                       # switch bank
    rom[0x67150:0x67150 + len(stream) - cut] = stream[cut:]
    rom[0x67150 + len(stream) - cut] = 0x8f         # end of list

    # 112 dictionary words packed right behind their pointer table.
    words = [bytes([0x20 + (i % 0x40)]) * (1 + i % 5) for i in range(112)]
    table, offset = bytearray(), 0xe3c3 + 2 * 113
    for w in words:
        table += struct.pack('<H', offset)
        offset += len(w)
    table += struct.pack('<H', offset)
    rom[0x663c3:0x663c3 + len(table)] = table
    blob = b''.join(words)
    rom[0x664a5:0x664a5 + len(blob)] = blob

    font = bytes((i * 7 + 3) & 0xff for i in range(0x1000))
    rom[0x65078:0x66078] = font
    widths = bytes((i % 8) + 1 for i in range(112))
    rom[0x6683f:0x6683f + 112] = widths

    # The spliced item-select template, as it appears in the layout table.
    message4 = bytes([0x87, 0x30, 0x34, 0x40, 0x59, 0x87, 0x20, 0x41, 0x59,
                      0x35, 0x40, 0x59, 0x87, 0x21, 0x83, 0x36, 0x40, 0x59,
                      0x87, 0x22, 0x41, 0x59, 0x37, 0x40, 0x59, 0x87, 0x23])
    expected = expected[:4] + [message4] + expected[4:]
    return bytes(rom), expected, words, font, widths


def run(binary, rom_path, out_dir, tag):
    dialogue = out_dir / f'{tag}-dialogue.bin'
    font = out_dir / f'{tag}-font.bin'
    result = subprocess.run([str(binary), str(rom_path), str(dialogue), str(font)],
                            capture_output=True, text=True, check=True)
    code, flags = result.stdout.split()
    return code, int(flags), dialogue.read_bytes(), font.read_bytes()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--rom', type=Path, help='real (Canada) ROM, optional')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    text = SOURCE.read_text(encoding='utf-8')
    languages = (ROOT / 'platform/3ds/source/game_languages.h').read_text()
    code = (PRELUDE + slice_source(languages, 'typedef struct GameLanguage {', 'static const GameLanguage kGameLanguages')
            + slice_source(text, 'typedef struct OwnedBlock {', 'static bool UnpackBlocks(')
            + slice_source(text, '// Layout of the text engine inside one family of ROMs.',
                           '// Non-NULL when the ROM is an official localized release')
            + MAIN)
    src = out / 'frc-text-test.c'
    src.write_text(code, encoding='utf-8')
    binary = out / 'frc-text-test'
    with (out / 'frc-text.log').open('w') as log:
        subprocess.run(['cc', '-O1', '-std=c11', '-fsanitize=address,undefined',
                        '-Wno-unused-function', str(src), '-o', str(binary)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

    classes = build_java(out)
    rom_bytes, expected_messages, words, font_gfx, widths = build_synthetic_rom()
    synthetic = out / 'synthetic-frc.sfc'
    synthetic.write_bytes(rom_bytes)
    lang, flags, dialogue_blk, font_blk = run(binary, synthetic, out, 'synthetic')
    assert lang == 'fr-c', lang
    assert flags == 3, flags
    dictionary, messages = unpack(dialogue_blk)
    assert [bytes(w) for w in unpack(dictionary)] == words, 'dictionary mismatch'
    got = [bytes(m) for m in unpack(messages)]
    assert len(got) == 397, len(got)
    for i, (a, b) in enumerate(zip(got, expected_messages)):
        assert a == b, f'message {i}: {a.hex()} != {b.hex()}'
    gfx, w = unpack(font_blk)
    assert bytes(gfx) == font_gfx and bytes(w) == widths, 'font mismatch'
    print('PASS: synthetic Canadian ROM - command rewriting, dictionary rebase, '
          'bank switch, message splice, font copy.')

    if classes:
        jlang, jflags, jdialogue, jfont = run_java(classes, synthetic, out, 'synthetic')
        assert (jlang, jflags) == ('fr-c', 3), (jlang, jflags)
        assert jdialogue == dialogue_blk and jfont == font_blk, \
            'the Android extractor disagrees with the 3DS one'
        print('PASS: Android extractor matches on the synthetic ROM.')
    else:
        print('SKIP: no javac on PATH, Android extractor not cross-checked.')

    if not args.rom:
        print('SKIP: no --rom given, golden check against the real ROM not run.')
        return
    data = args.rom.read_bytes()
    if len(data) % 0x8000 == 512:
        data = data[512:]
    got_sha1 = hashlib.sha1(data).hexdigest()
    assert got_sha1 == FRC_ROM_SHA1, f'unexpected ROM {got_sha1}'
    real = out / 'frc.sfc'
    real.write_bytes(data)
    lang, flags, dialogue_blk, font_blk = run(binary, real, out, 'real')
    assert lang == 'fr-c' and flags == 3
    assert hashlib.sha256(dialogue_blk).hexdigest() == FRC_DIALOGUE_SHA256, 'dialogue block changed'
    assert hashlib.sha256(font_blk).hexdigest() == FRC_FONT_SHA256, 'font block changed'
    dictionary, messages = unpack(dialogue_blk)
    assert len(unpack(dictionary)) == 112 and len(unpack(messages)) == 397
    print('PASS: real Canadian ROM - 397 messages, 112 dictionary words, blocks '
          'match the expected fr-c bytes (upstream\'s plus the scroll speed).')
    if classes:
        jlang, jflags, jdialogue, jfont = run_java(classes, real, out, 'real')
        assert (jlang, jflags) == ('fr-c', 3), (jlang, jflags)
        assert jdialogue == dialogue_blk and jfont == font_blk, \
            'the Android extractor disagrees with the 3DS one'
        print('PASS: Android extractor matches on the real Canadian ROM.')


if __name__ == '__main__':
    main()
