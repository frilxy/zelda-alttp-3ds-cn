#!/usr/bin/env python3
"""Move a Chinese language pack from its old glyph escapes to page bytes.

The v3.2 pack names a 16x16 glyph with two bytes, `0x6f..0x73` and an index.
Those bytes are also the letters o, p, q, r and s, so a pack could not print
them, and upstream v4.1 gave `0xf0`/`0xf1` to the Japanese cartridge's font
pages. The pack now uses the same shape: a page byte `0xf0..0xf4` and an index,
which names 5 * 256 = 1280 glyphs (the translation uses 1163).

Usage:
  reencode_pages.py PACK [OUT]     migrate, then check the round trip
  reencode_pages.py --check PACK   fail unless PACK is already migrated
"""
import struct
import sys


class PackError(Exception):
    pass


def unpack_memblk(blob, name):
    if len(blob) < 2:
        raise PackError(f'{name}: block too small')
    end = len(blob) - 2
    trailer = struct.unpack_from('<H', blob, end)[0]
    if trailer >= 8192:
        width, count = 4, trailer - 8192 + 1
    else:
        width, count = 2, trailer + 1
    base = (count - 1) * width
    if count <= 0 or base > end:
        raise PackError(f'{name}: bad block count')
    fmt = '<I' if width == 4 else '<H'

    def cumulative(i):
        return struct.unpack_from(fmt, blob, i * width)[0]

    blocks = []
    for i in range(count):
        left = base if i == 0 else base + cumulative(i - 1)
        right = end if i == count - 1 else base + cumulative(i)
        if left > right or right > end:
            raise PackError(f'{name}: block {i} does not fit')
        blocks.append(blob[left:right])
    return blocks


def pack_memblk(blocks):
    data_size = sum(len(b) for b in blocks)
    wide = data_size >= 65536 or len(blocks) > 8192
    width = 4 if wide else 2
    header = (len(blocks) - 1) * width
    out = bytearray(header + data_size + 2)
    fmt = '<I' if wide else '<H'
    cumulative = 0
    for i, block in enumerate(blocks):
        if i:
            struct.pack_into(fmt, out, (i - 1) * width, cumulative)
        out[header + cumulative:header + cumulative + len(block)] = block
        cumulative += len(block)
    struct.pack_into('<H', out, header + data_size,
                     (len(blocks) - 1) + (8192 if wide else 0))
    return bytes(out)


def parse_pack(data):
    if len(data) < 12 or data[:4] != b'ZCNB':
        raise PackError('not a language pack')
    dialogue_size = struct.unpack_from('<I', data, 4)[0]
    dialogue_at = 8
    if dialogue_size == 0 or dialogue_at + dialogue_size + 4 > len(data):
        raise PackError('dialogue block does not fit the file')
    font_size = struct.unpack_from('<I', data, dialogue_at + dialogue_size)[0]
    font_at = dialogue_at + dialogue_size + 4
    if font_at + font_size != len(data):
        raise PackError('font block does not fit the file')
    return data[dialogue_at:dialogue_at + dialogue_size], data[font_at:]


PAGE_FIRST = 0xf0
PAGE_LAST = 0xf4
ESCAPE_FIRST = 0x6f
ESCAPE_LAST = 0x73


def decode_escapes(message, name, index):
    out, i = [], 0
    while i < len(message):
        byte = message[i]
        if ESCAPE_FIRST <= byte <= ESCAPE_LAST:
            if i + 1 >= len(message):
                raise PackError(f'{name}: message {index} ends with a glyph prefix')
            out.append(('glyph', (byte - ESCAPE_FIRST) * 256 + message[i + 1]))
            i += 2
        else:
            out.append(('byte', byte))
            i += 1
    return out


def decode_pages(message):
    out, i = [], 0
    while i < len(message):
        byte = message[i]
        if PAGE_FIRST <= byte <= PAGE_LAST:
            if i + 1 >= len(message):
                raise PackError('message ends with a page byte')
            out.append(('glyph', (byte - PAGE_FIRST) * 256 + message[i + 1]))
            i += 2
        else:
            out.append(('byte', byte))
            i += 1
    return out


def reencode(message):
    out, i = bytearray(), 0
    while i < len(message):
        byte = message[i]
        if ESCAPE_FIRST <= byte <= ESCAPE_LAST:
            if i + 1 >= len(message):
                raise PackError('message ends with a glyph prefix')
            out += bytes((PAGE_FIRST + (byte - ESCAPE_FIRST), message[i + 1]))
            i += 2
        else:
            out.append(byte)
            i += 1
    return bytes(out)


def is_migrated(messages):
    """True when no message carries a bare 0x6f..0x73 byte: every one of those
    bytes is now an index behind a page byte."""
    for message in messages:
        i = 0
        while i < len(message):
            byte = message[i]
            if PAGE_FIRST <= byte <= PAGE_LAST:
                i += 2
                continue
            if ESCAPE_FIRST <= byte <= ESCAPE_LAST:
                return False
            i += 1
    return True


def migrate(data):
    dialogue, font = parse_pack(data)
    parts = unpack_memblk(dialogue, 'dialogue')
    if len(parts) != 2:
        raise PackError(f'dialogue block holds {len(parts)} parts, expected 2')
    dictionary, messages = parts
    messages = unpack_memblk(messages, 'messages')
    if is_migrated(messages):
        return 0, len(messages), data
    new_messages, changed = [], 0
    for index, message in enumerate(messages):
        new = reencode(message)
        if len(new) != len(message):
            raise PackError(f'message {index} changed size')
        if decode_escapes(message, 'pack', index) != decode_pages(new):
            raise PackError(f'message {index} does not round trip')
        if new != message:
            changed += 1
        new_messages.append(new)
    block = pack_memblk([dictionary, pack_memblk(new_messages)])
    return (changed, len(messages),
            b'ZCNB' + struct.pack('<I', len(block)) + block +
            struct.pack('<I', len(font)) + font)


def main(argv):
    check = '--check' in argv
    args = [a for a in argv[1:] if not a.startswith('--')]
    if not args:
        raise SystemExit(__doc__)
    source = args[0]
    data = open(source, 'rb').read()
    changed, total, migrated = migrate(data)
    if migrated == data:
        print(f'{source}: already uses page bytes')
        if not check:
            return
        return
    if check:
        raise SystemExit(f'{source}: still uses the 0x6f..0x73 escapes '
                         f'({changed} messages to migrate)')
    target = args[1] if len(args) > 1 else source
    open(target, 'wb').write(migrated)
    print(f'{target}: {changed} of {total} messages moved to page bytes')


if __name__ == '__main__':
    main(sys.argv)
