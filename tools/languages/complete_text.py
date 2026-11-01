#!/usr/bin/env python3
"""Complete text omitted by the Spanish/Italian/Portuguese fan patches, for this port.

Usage: complete_text.py es|it|pt INPUT.sfc OUTPUT.sfc
Run on a private, headerless translated copy, before make_bps.py. Never modifies
INPUT. No cartridge graphics or original dialogue are stored in this tool.
The accepted CRCs identify D_Skywalk F01h, SadNES 1.1 and Hexagon 1.3.
"""
import argparse
from pathlib import Path
import struct
import zlib

CRCS = {'es': 0x86de4b5d, 'it': 0x6d879a98, 'pt': 0x4a56f979}

# Epilogue: one small gold heading and a two-row place name, in scene order.
ITALIAN_ENDING = [
    ('TORNA IL RE', 'CASTELLO DI HYRULE'),
    ('IL SAGGIO FEDELE', 'SANTUARIO'),
    ('SAHASRAHLA RITORNA', 'KAKARIKO'),
    ('AVVOLTOI NEL DESERTO', 'PALAZZO DEL DESERTO'),
    ('IL BULLO TROVA UN AMICO', 'TORRE DI HERA'),
    ('LO ZIO GUARISCE', 'CASA TUA'),
    ('PINNE IN VENDITA', 'CASCATA DEGLI ZORA'),
    ('STREGA E AIUTANTE', 'BOTTEGA MAGICA'),
    ('I TAGLIALEGNA', 'CASA NEL BOSCO'),
    ('IL FLAUTO SUONA ANCORA', 'BOSCO INCANTATO'),
    ('VENERE REGINA DELLE FATE', 'POZZO DEI DESIDERI'),
    ('I FABBRI NANI', 'FUCINA'),
    ('CACCIA AGLI INSETTI', 'KAKARIKO'),
    ('IL VECCHIO SMARRITO', 'MONTE MORTE'),
    ('IL LADRO DELLA FORESTA', 'BOSCO PERDUTO'),
    ('E LA SPADA SUPREMA', 'PER SEMPRE...'),
]

PORTUGUESE_FILES = {
    'PLAYER  SELECT': 'ESCOLHER JOGO',
    'COPY  PLAYER': 'COPIAR JOGO',
    'ERASE PLAYER': 'APAGAR JOGO',
    ' REGISTER  YOUR  NAME ': ' DIGITE SEU NOME ',
    'WHICH PLAYER DO YOU WANT': 'QUAL JOGO DESEJA',
    'ERASE THIS PLAYER': 'APAGAR ESTE JOGO',
    'TO ERASE ?': 'APAGAR ?',
    'TO WHICH?': 'PARA QUAL',
    '(WHICH?)': 'QUAL?',
    'COPY OK': 'COPIADO',
    'QUIT': 'SAIR',
    'END': 'FIM',
}


def offset(addr):
    return ((addr >> 16) & 0x7f) * 0x8000 + (addr & 0x7fff)


def italian_ending(rom):
    data, offsets = bytearray(), []

    def row(text, y, base):
        # The ending map has A-Z at 26 (gold), 93 (tall top), 131 (bottom).
        assert 0 < len(text) <= 30
        x = (32 - len(text)) // 2
        assert all(c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ .' for c in text)
        tiles = bytes(159 if c == ' ' else (120 if base == 93 else 158 if base == 131 else 82)
                      if c == '.' else base + ord(c) - ord('A') for c in text)
        addr = 0x6000 + y * 32 + x
        data.extend(struct.pack('>HH', addr, len(text) * 2 - 1) + tiles)

    for i, (heading, place) in enumerate(ITALIAN_ENDING):
        offsets.append(len(data))
        row(heading, 19, 26)
        if i == 15:
            row('RIPOSA DI NUOVO...', 21, 56)
        # Dots have a dedicated pair at indices 120/158.
        for y, base in [(23, 93), (24, 131)]:
            row(place, y, base)
    offsets.append(len(data))
    start, end = offset(0x8ebf4c), offset(0x8ec2e1)
    if len(data) > end - start:
        raise ValueError('Italian epilogue exceeds its reserved table')
    rom[start:end] = data + bytes(end - start - len(data))
    rom[end:end + 34] = struct.pack('<17H', *offsets)
    return len(ITALIAN_ENDING)


def portuguese_files(rom):
    values = list(range(8)) + [0xaf] + list(range(9, 16)) + list(range(0x20, 0x2a))
    chars = dict(zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ', values))
    chars.update({' ': 0xa9, '?': 0x6f, '(': 0x85, ')': 0x86})

    def encode(text, bottom, palette, blank=0xa9):
        return bytes(b for c in text for b in
                     (blank if c == ' ' else chars[c] + (16 if bottom else 0), palette))

    replacements = 0
    for source, target in PORTUGUESE_FILES.items():
        if len(target) > len(source):
            raise ValueError('Text does not fit: ' + target)
        found = 0
        for bottom, blank in [(False, 0xa9), (True, 0xa9), (True, 0xb9)]:
            for palette in [0, 0x18, 0x1c]:
                old = encode(source, bottom, palette, blank)
                new = encode(target.ljust(len(source)), bottom, palette, blank)
                found += rom.count(old)
                rom[:] = rom.replace(old, new)
        if not found:
            raise ValueError('Expected label absent: ' + source)
        replacements += found
    return replacements


# Row numbers in the USA-format credit scroll. Names and existing translator
# credits are preserved. Short uppercase wording fits the original tile font.
CREDIT_ROWS = {
 'es': {
  0:'PRODUCTOR EJECUTIVO',14:'PRODUCTOR',28:'DIRECTOR',42:'GUIONISTA',
  56:'AYUDANTES DE DIRECCION',74:'DISENO GRAFICO',76:'DISENO DE OBJETOS',
  94:'DISENO DE FONDOS',114:'DIRECTOR TECNICO',128:'PROGRAMADOR JEFE',
  142:'PROGRAMA DE OBJETOS',156:'PROGRAMADORES',200:'COMPOSITOR',
  214:'COORDINACION',232:'ILUSTRACIONES',264:'GUION EN INGLES',282:'AGRADECIMIENTOS',322:'TU AVENTURA',323:'TU AVENTURA',
  326:'LUGARES           PARTIDAS',329:'CASTILLO HYRULE',333:'MAZMORRA',
  337:'PALACIO ESTE',341:'PALACIO DESIERTO',345:'TORRE DE HERA',
 },
 'it': {
  0:'PRODUTTORE ESECUTIVO',14:'PRODUTTORE',28:'DIRETTORE',42:'SCENEGGIATURA',
  56:'AIUTO REGIA',74:'GRAFICA',76:'GRAFICA DEGLI OGGETTI',94:'GRAFICA DEGLI SFONDI',
  114:'DIRETTORE TECNICO',128:'PROGRAMMATORE CAPO',142:'CODICE OGGETTI',
  156:'PROGRAMMATORI',200:'COMPOSITORE',214:'COORDINAMENTO',232:'ILLUSTRAZIONI',
  264:'TESTO INGLESE',282:'RINGRAZIAMENTI',322:'LA TUA STORIA',323:'LA TUA STORIA',
  326:'LUOGHI               GIOCHI',329:'CASTELLO DI HYRULE',333:'SEGRETE',
  337:'PALAZZO EST',341:'PALAZZO DESERTO',345:'TORRE DI HERA',
  392:'PARTITE TOTALI',393:'PARTITE TOTALI',
 },
 'pt': {
  0:'PRODUTOR EXECUTIVO',14:'PRODUTOR',28:'DIRETOR',42:'ROTEIRISTA',
  56:'DIRETORES ASSISTENTES',74:'DESIGN GRAFICO',76:'DESIGN DE OBJETOS',
  94:'DESIGN DE CENARIOS',114:'DIRETOR TECNICO',128:'PROGRAMADOR CHEFE',
  142:'CODIGO DOS OBJETOS',156:'PROGRAMADORES',200:'COMPOSITOR',214:'COORDENACAO',
  232:'ILUSTRACOES',282:'AGRADECIMENTOS',326:'LOCAIS               JOGOS',
  329:'CASTELO DE HYRULE',333:'CALABOUCO',337:'PALACIO LESTE',341:'PALACIO DESERTO',345:'TORRE DE HERA',
 },
}
CREDIT_DUNGEONS = {
 'es':['PALACIO OSCURO','PALACIO PANTANO','BOSQUE CALAVERA','PUEBLO LADRONES','PALACIO HIELO','CIENAGA MISERIA','ROCA TORTUGA','TORRE DE GANON'],
 'it':['PALAZZO OSCURO','PALUDE','BOSCO TESCHIO','COVO DEI LADRI','PALAZZO GELO','PALUDE OSCURA','ROCCIA TARTARUGA','TORRE DI GANON'],
 'pt':['PALACIO TREVAS','PALACIO PANTANO','BOSQUE CAVEIRA','VILA LADROES','PALACIO GELO','PANTANO SOMBRIO','ROCHA TARTARUGA','TORRE DE GANON'],
}


def credit_rows(rom, language):
    start, end, table = offset(0x8eb178), offset(0x8eb93d), offset(0x8eb93d)
    rows = []
    for i in range(394):
        at = start + struct.unpack_from('<H', rom, table + i * 2)[0]
        count = 0 if rom[at] == 255 else (rom[at + 1] + 1) // 2
        rows.append(bytes(rom[at:at + 1] if not count else rom[at:at + 2 + count]))

    def text_row(text, base, x):
        assert 0 < len(text) <= 28 and 0 <= x <= 32 - len(text)
        assert all(c in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ ' for c in text)
        return bytes([x, len(text) * 2 - 1]) + bytes(159 if c == ' ' else base + ord(c) - 65 for c in text)

    for i, text in CREDIT_ROWS[language].items():
        # Keep each original color/font. Single-row roles use one of three
        # alphabet bands, while names/history have separate tall halves.
        first = next(c for c in rows[i][2:] if c != 159)
        base = next(b for b in [0,26,56,93,131] if b <= first < b + 26)
        x = (32 - len(text)) // 2 if i < 326 else 3 if i == 326 else 4
        rows[i] = text_row(text, base, x)
    for d, text in enumerate(CREDIT_DUNGEONS.get(language, [])):
        i = 349 + 4*d
        # Numeric floor and its label are redundant; retaining the death-count
        # columns lets longer translated dungeon names fit at the left edge.
        rows[i] = text_row(text, 93, 4)
        rows[i+1] = text_row(text, 131, 4)
    data, offsets, unique = bytearray(), [], {}
    for row in rows:
        if row not in unique:
            unique[row] = len(data)
            data.extend(row)
        offsets.append(unique[row])
    if len(data) > end - start:
        raise ValueError('Credit scroll exceeds reserved table: %d > %d' % (len(data), end-start))
    rom[start:end] = data + bytes(end-start-len(data))
    rom[table:table+788] = struct.pack('<394H', *offsets)
    return len(CREDIT_ROWS[language]) + 2*len(CREDIT_DUNGEONS.get(language, []))


# Original pixel lettering for the last card. Each string is one narrow serif glyph;
# it is rasterized at native resolution into the existing 64x16 sprite area.
END_LETTERS = {
 'F': ['111111111','011100011','011100001','011100000','011100000','011100100','011111100','011100100','011100000','011100000','011100000','011100000','111110000'],
 'I': ['1111111','0011100','0011100','0011100','0011100','0011100','0011100','0011100','0011100','0011100','0011100','0011100','1111111'],
 'N': ['111000111','011100010','011100010','010110010','010110010','010011010','010011010','010001110','010001110','010000110','010000110','010000010','111000111'],
 'E': ['111111111','011100011','011100001','011100000','011100000','011100100','011111100','011100100','011100000','011100000','011100001','011100011','111111111'],
 'M': ['11100000111','01110001110','01110001110','01011011010','01011011010','01001110010','01001110010','01000100010','01000100010','01000000010','01000000010','01000000010','11100000111'],
}


def decompress(data):
    out, i = bytearray(), 0
    while data[i] != 255:
        cmd = data[i]; i += 1
        if cmd & 0xe0 != 0xe0:
            n, cmd = (cmd & 31) + 1, cmd & 0xe0
        else:
            n = data[i] + ((cmd & 3) << 8) + 1
            i += 1; cmd = (cmd << 3) & 0xe0
        if cmd == 0:
            out.extend(data[i:i+n]); i += n
        elif cmd & 0x80:
            at = data[i] | data[i+1] << 8; i += 2
            for k in range(n):
                out.append(out[at+k])
        elif not cmd & 0x40:
            out.extend([data[i]] * n); i += 1
        elif not cmd & 0x20:
            out.extend(data[i+k % 2] for k in range(n)); i += 2
        else:
            out.extend((data[i] + k) & 255 for k in range(n)); i += 1
        if len(out) > 0x600:
            raise ValueError('Ending sprite exceeds its sheet')
    return out, i + 1


def compress(data):
    # Dynamic programming over literals, byte runs and overlapping LZ copies.
    # No external compressor or cartridge bytes are bundled with this script.
    def header(cmd, n):
        return bytes([cmd | n-1]) if n <= 32 else bytes([0xe0 | cmd >> 3 | (n-1) >> 8, (n-1) & 255])
    n = len(data)
    costs, steps = [10**9] * (n+1), [None] * (n+1)
    costs[n] = 1
    previous = {}
    copies = []
    for i in range(n):
        key = bytes(data[i:i+3])
        best, at = 0, 0
        for p in previous.get(key, [])[-64:]:
            length = 0
            while length < min(1024, n-i) and data[p+length] == data[i+length]:
                length += 1
            if length > best:
                best, at = length, p
        copies.append((best, at))
        previous.setdefault(key, []).append(i)
    for i in range(n-1, -1, -1):
        def offer(cmd, length, payload):
            cost = len(header(cmd, length)) + len(payload) + costs[i+length]
            if cost < costs[i]:
                costs[i], steps[i] = cost, (length, header(cmd, length) + payload)
        for length in range(1, min(1024, n-i)+1):
            offer(0, length, data[i:i+length])
        run = 1
        while run < min(1024, n-i) and data[i+run] == data[i]:
            run += 1
        for length in range(2, run+1):
            offer(0x20, length, bytes([data[i]]))
        match, at = copies[i]
        for length in range(3, match+1):
            offer(0x80, length, struct.pack('<H', at))
    result, i = bytearray(), 0
    while i < n:
        length, encoded = steps[i]
        result.extend(encoded); i += length
    return result + b'\xff'


def ending_card(rom, language):
    i = 115 + 45  # compressed sprite sheet 45: ending title and two sprites
    address = rom[0x4f80+i] << 16 | rom[0x505f+i] << 8 | rom[0x513e+i]
    start = offset(address)
    sheet, reserved = decompress(rom[start:])
    if len(sheet) != 0x600:
        raise ValueError('Unexpected ending sprite sheet')
    title = {'es':'FIN', 'it':'FINE', 'pt':'FIM'}[language]
    pixels = [[0]*64 for _ in range(16)]
    width = sum(len(END_LETTERS[c][0])+2 for c in title)-2
    x0 = (64-width)//2
    mask = set()
    for c in title:
        for y, row in enumerate(END_LETTERS[c]):
            for x, value in enumerate(row):
                if value == '1':
                    mask.add((x0+x, 1+y))
        x0 += len(END_LETTERS[c][0])+2
    # One-pixel bevel and shadow in the existing gold palette.
    for x, y in mask:
        if (x+1, y+1) not in mask:
            pixels[y+1][x+1] = 3
    for x, y in mask:
        pixels[y][x] = 5 if (x-1, y-1) not in mask else 4
    original = bytes(sheet)
    changed = set()
    for y in range(16):
        for x in range(64):
            tile = (y//8)*16+x//8
            bit = 1 << (7-x%8)
            for plane in range(3):
                at = tile*24 + (y%8)*2+plane if plane < 2 else tile*24+16+y%8
                sheet[at] = (sheet[at] & ~bit) | (bit if pixels[y][x] >> plane & 1 else 0)
                changed.add(at)
    assert all(a == b or i in changed for i, (a,b) in enumerate(zip(original,sheet)))
    packed = compress(sheet)
    if len(packed) > reserved or decompress(packed)[0] != sheet:
        raise ValueError('Ending sprite cannot be safely repacked')
    rom[start:start+reserved] = packed + bytes(reserved-len(packed))
    return 1


def spanish_name_heading(rom):
    # Owner's preferred file-name prompt. Reuse the cartridge's glyph indices;
    # change only the two tile rows, keeping the stripe lengths and palette.
    values = list(range(8)) + [0xaf] + list(range(9, 16)) + list(range(0x20, 0x2a))
    chars = dict(zip('ABCDEFGHIJKLMNOPQRSTUVWXYZ', values))
    chars[' '] = 0xa9
    for bottom in (0, 16):
        def encode(text):
            return bytes(b for c in text for b in
                         (chars[c] + (bottom if c != ' ' else 0), 0x18))
        old, new = encode('SU NOMBRE'), encode('NOMBRE   ')
        if rom.count(old) != 1:
            raise ValueError('Expected Spanish naming heading absent or ambiguous')
        rom[:] = rom.replace(old, new)
    return 2


def complete(language, source):
    if len(source) != 0x100000 or zlib.crc32(source) != CRCS[language]:
        raise ValueError('Input is not the expected headerless translation')
    rom = bytearray(source)
    count = italian_ending(rom) if language == 'it' else portuguese_files(rom) if language == 'pt' else 0
    if language == 'es':
        count += spanish_name_heading(rom)
    count += credit_rows(rom, language)
    count += ending_card(rom, language)
    return bytes(rom), count


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('language', choices=CRCS)
    p.add_argument('source', type=Path)
    p.add_argument('target', type=Path)
    a = p.parse_args()
    if a.source.resolve() == a.target.resolve():
        p.error('Use a separate output file')
    result, count = complete(a.language, a.source.read_bytes())
    a.target.write_bytes(result)
    print('%s: %d text changes, CRC32 %08x' % (a.language, count, zlib.crc32(result)))


if __name__ == '__main__':
    main()
