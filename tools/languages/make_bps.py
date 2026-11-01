#!/usr/bin/env python3
"""Make a BPS patch that turns one ROM into another.

The language patches in romfs are made with it: each turns the USA cartridge
into a translated one, and the console then reads that cartridge's text the
way it reads any translated cartridge. Usage:

  python3 make_bps.py SOURCE TARGET OUTPUT

Unchanged bytes become SourceRead actions, data the target moved becomes
SourceCopy actions, and only new bytes are stored. The patch is checked by
applying it again before it is written.
"""
import sys
import zlib
from pathlib import Path

WINDOW = 16  # shortest moved run worth a SourceCopy action


def varint(n):
    out = bytearray()
    while True:
        x = n & 0x7f
        n >>= 7
        if not n:
            out.append(0x80 | x)
            return out
        out.append(x)
        n -= 1


def read_varint(data, p):
    value, shift = 0, 1
    while True:
        x = data[p]
        p += 1
        value += (x & 0x7f) * shift
        if x & 0x80:
            return value, p
        shift <<= 7
        value += shift


def make(source, target):
    index = {}
    for i in range(0, len(source) - WINDOW + 1):
        index.setdefault(source[i:i + WINDOW], i)
    out = bytearray(b'BPS1') + varint(len(source)) + varint(len(target)) + varint(0)
    pending = bytearray()
    source_relative = 0

    def flush():
        if pending:
            out.extend(varint(((len(pending) - 1) << 2) | 1) + pending)
            pending.clear()

    i = 0
    while i < len(target):
        run = 0
        while i + run < min(len(source), len(target)) and source[i + run] == target[i + run]:
            run += 1
        if run >= 4:
            flush()
            out.extend(varint(((run - 1) << 2) | 0))
            i += run
            continue
        start = index.get(target[i:i + WINDOW])
        if start is not None:
            length = WINDOW
            while (i + length < len(target) and start + length < len(source) and
                   source[start + length] == target[i + length]):
                length += 1
            flush()
            delta = start - source_relative
            out.extend(varint(((length - 1) << 2) | 2))
            out.extend(varint((abs(delta) << 1) | (delta < 0)))
            source_relative = start + length
            i += length
            continue
        pending.append(target[i])
        i += 1
    flush()
    out += zlib.crc32(source).to_bytes(4, 'little') + zlib.crc32(target).to_bytes(4, 'little')
    out += zlib.crc32(out).to_bytes(4, 'little')
    return bytes(out)


def apply(source, patch):
    assert patch[:4] == b'BPS1'
    p = 4
    src_size, p = read_varint(patch, p)
    dst_size, p = read_varint(patch, p)
    meta, p = read_varint(patch, p)
    p += meta
    assert src_size == len(source)
    out = bytearray(dst_size)
    o = s_rel = t_rel = 0
    end = len(patch) - 12
    while p < end:
        cmd, p = read_varint(patch, p)
        length, mode = (cmd >> 2) + 1, cmd & 3
        if mode == 0:
            out[o:o + length] = source[o:o + length]
        elif mode == 1:
            out[o:o + length] = patch[p:p + length]
            p += length
        else:
            d, p = read_varint(patch, p)
            d = -(d >> 1) if d & 1 else d >> 1
            if mode == 2:
                s_rel += d
                out[o:o + length] = source[s_rel:s_rel + length]
                s_rel += length
            else:
                t_rel += d
                for k in range(length):
                    out[o + k] = out[t_rel + k]
                t_rel += length
        o += length
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    source = Path(sys.argv[1]).read_bytes()
    target = Path(sys.argv[2]).read_bytes()
    patch = make(source, target)
    if apply(source, patch) != target:
        sys.exit('patch check failed')
    Path(sys.argv[3]).write_bytes(patch)
    print('%s: %d bytes' % (sys.argv[3], len(patch)))


if __name__ == '__main__':
    main()
