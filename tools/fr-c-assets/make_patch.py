"""Builds platform/3ds/romfs/zelda3_assets_fr-c.bps.

That patch turns "Legend of Zelda, The - A Link to the Past (Canada)" alone
into the assets file, with the cartridge's own tile and message numbers
(AssetsFrC_Renumber in engine/src/assets_fr_c.c and FrcAssets.java give
them the engine's numbering, then the setup adds the cartridge's dialogue).

Making it needs both cartridges: the USA one only to know which assets the
two share (zelda3_assets.bps applied to it gives upstream zelda3's assets).
The patch itself copies everything from the Canadian cartridge; what it
carries as literal bytes is what restool generates itself (tables of offsets,
room headers and the like, as zelda3_assets.bps does) and the placeholders of
the USA credits, which the French Canadian screens never draw.

  python3 make_patch.py --us-rom zelda3.sfc --ca-rom "...(Canada).sfc" --out x.bps
"""

import argparse
import collections
import struct
import zlib

US_PATCH = 'platform/3ds/romfs/zelda3_assets.bps'


def lorom(a):
    return ((a >> 16) & 0x7f) << 15 | (a & 0x7fff)


def bps_int(p, i):
    data, shift = 0, 1
    while True:
        x = p[i]
        i += 1
        data += (x & 0x7f) * shift
        if x & 0x80:
            return data, i
        shift <<= 7
        data += shift


def bps_apply(src, p):
    assert p[:4] == b'BPS1'
    i = 4
    _, i = bps_int(p, i)
    size, i = bps_int(p, i)
    meta, i = bps_int(p, i)
    i += meta
    out = bytearray(size)
    o = sr = tr = 0
    while i < len(p) - 12:
        c, i = bps_int(p, i)
        n, k = (c >> 2) + 1, c & 3
        if k == 0:
            out[o:o + n] = src[o:o + n]
        elif k == 1:
            out[o:o + n] = p[i:i + n]
            i += n
        else:
            d, i = bps_int(p, i)
            if k == 2:
                sr += -(d >> 1) if d & 1 else d >> 1
                out[o:o + n] = src[sr:sr + n]
                sr += n
            else:
                tr += -(d >> 1) if d & 1 else d >> 1
                for j in range(n):
                    out[o + j] = out[tr + j]
                tr += n
        o += n
    assert zlib.crc32(bytes(out)) == int.from_bytes(p[-8:-4], 'little')
    return bytes(out)


def read_container(d):
    count, names = struct.unpack_from('<II', d, 80)
    sizes = struct.unpack_from('<%dI' % count, d, 88)
    o = 88 + 4 * count + names
    out = []
    for n in sizes:
        o = (o + 3) & ~3
        out.append(d[o:o + n])
        o += n
    return out


def write_container(header_from, assets):
    count, names = struct.unpack_from('<II', header_from, 80)
    out = bytearray(header_from[:88])
    out += b''.join(struct.pack('<I', len(a)) for a in assets)
    out += header_from[88 + 4 * count:88 + 4 * count + names]
    for a in assets:
        while len(out) & 3:
            out.append(0)
        out += a
    return bytes(out)


def pack(arr):
    offs, o = [], 0
    for a in arr[:-1]:
        o += len(a)
        offs.append(o)
    if o < 65536:
        return b''.join([struct.pack('<H', x) for x in offs] + list(arr) +
                        [struct.pack('<H', len(arr) - 1)])
    return b''.join([struct.pack('<I', x) for x in offs] + list(arr) +
                    [struct.pack('<H', 8192 + len(arr) - 1)])


def lz_len(rom, off):
    p = off
    while True:
        b = rom[p]
        p += 1
        if b == 0xff:
            return p - off
        if (b & 0xe0) != 0xe0:
            cmd, n = b & 0xe0, (b & 0x1f) + 1
        else:
            cmd, n = (b << 3) & 0xe0, (((b & 3) << 8) | rom[p]) + 1
            p += 1
        p += n if cmd == 0 else 2 if cmd & 0x80 or (cmd & 0x60) == 0x40 else 1


def stripes_len(rom, off):
    p = off
    while not rom[p] & 0x80:
        n = ((rom[p + 2] << 8 | rom[p + 3]) & 0x3fff) + 1
        p += 4 + (2 if rom[p + 2] & 0x40 else n)
    return p - off + 1


def build_target(us_assets, ca):
    """The assets of the Canadian cartridge, still in its own numbering."""
    t = list(us_assets)  # the assets both cartridges share, found in it below
    t[9] = ca[0x3f61d:0x3f61d + 640]                       # kDungeonRoomTeleMsg
    t[110] = ca[0x3f51d:0x3f51d + 256]                     # kOverworld_SignText
    for k, base in enumerate((0x18000, 0x1b410, 0x20000, 0x23410)):
        t[60 + k] = ca[base:base + 2221 * 6]               # kMap32ToMap16_0..3
    t[70] = ca[0x78000:0x78000 + 3752 * 8]                 # kMap16ToMap8
    t[164] = ca[0xdf110:0xdf110 + 3824]                    # kSomeTileAttr
    for k, table in ((105, 0x179cf), (106, 0x17baf)):      # overworld maps
        streams = []
        for s in range(160):
            o = lorom(int.from_bytes(ca[table + 3 * s:table + 3 * s + 3], 'little'))
            streams.append(ca[o:o + lz_len(ca, o)])
        t[k] = pack(streams)
    gfx = lorom(0x80cf26)                                  # 223 sheets, 3 columns

    def sheet(e):
        return lorom(ca[gfx + e] << 16 | ca[gfx + 223 + e] << 8 | ca[gfx + 446 + e])
    spr = []
    for i in range(108):
        o = sheet(115 + i)
        spr.append(ca[o:o + (0x600 if i < 12 else lz_len(ca, o))])
    t[64] = pack(spr)
    t[65] = pack([ca[sheet(i):sheet(i) + lz_len(ca, sheet(i))] for i in range(115)])
    t[73] = ca[0x75486:0x75486 + 256]                      # read by its own code
    # The USA credits and ending captions, drawn from the cartridge instead:
    # blank lines, one blank tile per caption.
    t[74] = b'\xff'
    t[75] = bytes(788)
    t[76] = struct.pack('<H', 0x3ca9) * 160
    t[77] = b''.join(struct.pack('<H', 5 * i) for i in range(17))
    t[78] = bytes([0x60, 0, 0, 1, 0]) * 16
    for k, a in zip(range(99, 105), (0x8e9c63, 0x8ea729, 0x8ea1b6, 0x8ea57e, 0x8ea390,
                                      0x8cfaec)):
        o = lorom(a)
        t[k] = ca[o:o + stripes_len(ca, o)]                # kBgTilemap_0..5
    t[94] = t[95] = t[96] = b''                            # dialogue, added at setup
    return t


def bps_encode(src, dst, k=4):
    def num(n):
        out = bytearray()
        while True:
            x = n & 0x7f
            n >>= 7
            if n == 0:
                out.append(0x80 | x)
                return out
            out.append(x)
            n -= 1
    sidx = collections.defaultdict(list)
    for i in range(len(src) - k + 1):
        sidx[src[i:i + k]].append(i)
    tidx = collections.defaultdict(list)
    out = bytearray(b'BPS1') + num(len(src)) + num(len(dst)) + num(0)
    srel = trel = o = added = 0
    lit = bytearray()

    def flush():
        if lit:
            out.extend(num((len(lit) - 1) << 2 | 1))
            out.extend(lit)
            lit.clear()
    while o < len(dst):
        while added <= o - k and added + k <= len(dst):
            tidx[dst[added:added + k]].append(added)
            added += 1
        best = (0, None, 0)
        g = dst[o:o + k]
        if len(g) == k:
            if src[o:o + k] == g:
                n = k
                while o + n < len(dst) and o + n < len(src) and src[o + n] == dst[o + n]:
                    n += 1
                best = (n, 'read', o)
            for p in sidx.get(g, [])[-64:]:
                if best[0] >= 4096:
                    break
                n = k
                while o + n < len(dst) and p + n < len(src) and src[p + n] == dst[o + n]:
                    n += 1
                if n > best[0]:
                    best = (n, 'src', p)
            for p in tidx.get(g, [])[-32:]:
                n = k
                while o + n < len(dst) and dst[p + n] == dst[o + n]:
                    n += 1
                if n > best[0]:
                    best = (n, 'dst', p)
        n, kind, p = best
        if n >= k:
            flush()
            if kind == 'read':
                out.extend(num((n - 1) << 2))
            elif kind == 'src':
                d = p - srel
                out.extend(num((n - 1) << 2 | 2) + num(abs(d) << 1 | (d < 0)))
                srel = p + n
            else:
                d = p - trel
                out.extend(num((n - 1) << 2 | 3) + num(abs(d) << 1 | (d < 0)))
                trel = p + n
            o += n
        else:
            lit.append(dst[o])
            o += 1
    flush()
    out += zlib.crc32(src).to_bytes(4, 'little') + zlib.crc32(dst).to_bytes(4, 'little')
    out += zlib.crc32(bytes(out)).to_bytes(4, 'little')
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--us-rom', required=True)
    ap.add_argument('--ca-rom', required=True)
    ap.add_argument('--us-patch', default=US_PATCH)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    us = open(a.us_rom, 'rb').read()
    ca = open(a.ca_rom, 'rb').read()
    us = us[0x200:] if len(us) & 0x3ff == 0x200 else us
    ca = ca[0x200:] if len(ca) & 0x3ff == 0x200 else ca
    assert zlib.crc32(ca) == 0x47760381, 'not the French Canadian cartridge'
    us_assets_file = bps_apply(us, open(a.us_patch, 'rb').read())
    target = write_container(us_assets_file, build_target(read_container(us_assets_file), ca))
    patch = bps_encode(ca, target)
    assert bps_apply(ca, patch) == target
    open(a.out, 'wb').write(patch)
    print('%s: %d bytes, builds %d bytes of assets' % (a.out, len(patch), len(target)))


if __name__ == '__main__':
    main()
