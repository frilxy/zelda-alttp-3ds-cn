# Language patch preparation

The local selector offers English, Spanish, Portuguese, Italian, French,
German and Russian. Japanese is intentionally excluded. One choice selects
both the cartridge translation and the port UI. The Settings globe asks for
confirmation before opening this selector. Cancelling returns to the current
screen; confirming discards the transient session/autosave and preserves SRAM.

Work only on private copies outside the source checkout. The base cartridge is
headerless USA, SHA-1 `6d4f10a8b10e10dbe624cb23cf03b88bb8252973`.
Original fan patches and their readmes must be preserved separately.

## Supplemental text

`complete_text.py` accepts these exact headerless translated inputs:

| Code | Original source | Input CRC32 | Completed CRC32 |
|---|---|---|---|
| es | D_Skywalk Final 1.11 / F01h | `86de4b5d` | `8f07fc47` |
| it | SadNES cITy 1.1 | `6d879a98` | `977ad1c5` |
| pt | Hexagon 1.3 | `4a56f979` | `a164a974` |

SadNES and Hexagon patches expect a copier header while being applied; remove
that 512-byte header before supplying the resulting copy to this tool. It
rejects other inputs and an output path equal to the input path.

```sh
python3 tools/languages/complete_text.py pt /private/pt-original.sfc /private/pt-completed.sfc
python3 tools/languages/make_bps.py /private/usa.sfc /private/pt-completed.sfc platform/3ds/romfs/languages/pt.bps
```

The supplement translates remaining staff roles and adventure-history labels
in all three languages, the 16 Italian epilogue captions, Portuguese file
menus, the owner-requested Spanish NOMBRE heading, and the last card as FIN, FINE or FIM. Names, original translator credits,
death counters and unrelated sprite tiles are retained. The card lettering is
new pixel artwork authored in the tool, using the cartridge's gold palette.
The compressed sprite is round-trip checked and must fit its original space.
No cartridge graphics or original dialogue are stored in this tool.

`make_bps.py` checks the resulting patch by applying it before writing it. The
console's language cache stamp includes an extractor schema and bundled BPS
checksum, so a patch update invalidates cached assets even in the same language.

## Verification

Run `platform/3ds/tests/run_language_cache_test.py` for cache invalidation and
`run_ui_strings_test.py` for the seven UI alphabets. With private extracted
inputs, `run_language_tour.py` renders file menus, HUD, inventory, map, attract
scenes, all 16 epilogue captions, the entire credit scroll and the last card
through the production engine. Inspect its PNGs as well as its exit status.
The native UI layout test separately covers both bottom-screen pixel formats,
confirmation, cancellation and selector requests.

These are source/host checks. Record ARM compilation, isolated Azahar behavior
and physical Old/New 3DS acceptance separately. Emulator FPS is not hardware
performance evidence.

## Language packs

The patches above are deltas between the USA cartridge and a translated one.
Chinese has no such cartridge: its translation is its own message text plus a
16x16 glyph font (the game's 8x16 letter alphabet cannot draw it), so it
travels as a *pack* instead - `platform/3ds/romfs/languages/zh.bin` - and is
listed in `kGameLanguages` with `menu = -1`, which leaves the port's own
screens in English until they can draw the glyphs.

A pack carries the same two blocks the patch path extracts from a translated
cartridge:

    'ZCNB'
    u32 dialogue length, dialogue block
    u32 font length,     font block

Both are memblks (blocks, then their offsets, then the count). The dialogue
block holds the dictionary and one block per message; the font block holds the
256 letter tiles (4096 bytes) followed by one 64-byte 16x16 glyph per
character, then the widths: 111 letter widths followed by one byte per glyph.

Message text names a glyph with an escape prefix 0x6f..0x73 (one prefix per 256
glyphs) plus an index byte. `dialogue_flags` bit 2 (`kDialogueCjkFlag`, see
`engine/src/dialogue_cjk.h`) tells the text engine that the block has that
shape, and the renderer draws each glyph as two 8x16 half tiles.

Packs are build inputs like the `.bps` files: they carry translated game text,
so they are not committed (`platform/3ds/romfs/languages/.gitignore`). Check
one before shipping it:

    python3 tools/languages/check_language_pack.py platform/3ds/romfs/languages/zh.bin
