#!/usr/bin/env python3
"""Validate packaged 3DSX segments and relocation subtypes before distribution.

3DSX addresses reserve four high bits for a relocation subtype. A valid ELF
can still produce an unloadable 3DSX when a compiler folds a large negative
table offset into an absolute address. Check bytes in the final artifact.
"""
from pathlib import Path
import argparse
import struct


def validate(path):
    data = path.read_bytes()
    if len(data) < 44:
        raise ValueError('Truncated 3DSX header')
    magic, header, reloc_header, version, flags, code, rodata, memory, bss, icon, icon_size, romfs = struct.unpack_from('<IHH9I', data)
    if magic != 0x58534433 or header < 44 or reloc_header < 8 or reloc_header % 4:
        raise ValueError('Invalid extended 3DSX header')
    if bss > memory:
        raise ValueError('BSS exceeds data segment')
    sizes = [code, rodata, memory - bss]
    aligned = [(value + 4095) & ~4095 for value in (code, rodata, memory)]
    if any(value > 128 * 1024 * 1024 for value in aligned):
        raise ValueError('Implausible application segment size')
    count = reloc_header // 4
    cursor = header

    def take(size):
        nonlocal cursor
        if size > len(data) - cursor:
            raise ValueError('Truncated segment or relocation table')
        result = data[cursor:cursor + size]
        cursor += size
        return result

    relocations = struct.unpack('<' + 'I' * (count * 3), take(reloc_header * 3))
    segments = [take(size) for size in sizes]
    patches = 0
    for segment in range(3):
        for table in range(count):
            offset = 0
            for _ in range(relocations[segment * count + table]):
                skip, patch = struct.unpack('<HH', take(4))
                offset += skip * 4
                if offset + patch * 4 > aligned[segment]:
                    raise ValueError('Relocation exceeds its segment')
                if table < 2:
                    for at in range(offset, offset + patch * 4, 4):
                        if at + 4 > sizes[segment]:
                            raise ValueError('Relocation points outside initialized data')
                        value, = struct.unpack_from('<I', segments[segment], at)
                        if value >> 28 > (1 if table == 1 else 0):
                            raise ValueError(f'Invalid relocation subtype: segment={segment} '
                                             f'table={table} offset={at:#x} value={value:#x}')
                        patches += 1
                offset += patch * 4
    if icon < cursor or icon_size < 4 or icon + icon_size > len(data) or data[icon:icon+4] != b'SMDH':
        raise ValueError('Missing or overlapping SMDH')
    # 3DSX embeds the level-3 RomFS directly, without the NCCH IVFC wrapper.
    if romfs < icon + icon_size or romfs + 40 > len(data):
        raise ValueError('Missing or overlapping RomFS')
    fs = struct.unpack_from('<10I', data, romfs)
    if fs[0] != 40 or fs[9] > len(data) - romfs:
        raise ValueError('Invalid RomFS header')
    end = 40
    for offset, size in zip(fs[1:9:2], fs[2:9:2]):
        if offset < end or offset + size > fs[9]:
            raise ValueError('Overlapping or truncated RomFS metadata')
        end = offset + size
    print(f'PASS 3DSX: {patches} relocations, bounded segments, SMDH and RomFS')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    try:
        validate(args.image)
    except (ValueError, OSError, struct.error) as error:
        parser.exit(1, f'Invalid 3DSX: {error}\n')
