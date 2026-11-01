# Native randomizer

Available in version 4.0. On-device generation, exact ALttPR JSON
import, the settings wizard and separate seed saves are implemented. This is
not a claim of support for every ALttPR configuration or a full-game acceptance
result. No physical 3DS validation or performance claim is made.

## Using the randomizer

Open Settings > Game > Randomizer. The shared language-switch confirmation
warns that unsaved progress will be lost: No preserves the session; Yes leaves
without an autosave and opens the wizard. Enter a uint64 seed with the pixel
keypad, choose a preset and generate. Open, Standard, Inverted, Retro and
Triforce Hunt are the five native presets. Opening a known seed resumes its
progress rather than generating over it. Saved seeds are listed separately.
The Normal option returns to ordinary play.

Randomizer gameplay currently uses English/USA assets because item-dependent
messages use that glyph and message grammar. The seven-language UI remains in
the selected language. Returning to ordinary play restores the selected game
translation and configured mods; language.ini is not rewritten. Randomizer
permits character skins, and excludes quest/game mods with conflicting rules.
A `.zspr` is a character skin, not a randomizer seed.

### Importing ALttPR

1. Export a **full spoiler JSON** from ALttPR, including its location/item table,
   metadata, explicit `Equipped` starting gear, dungeon prizes, medallions and
   both fairy bottle results.
2. Copy it to `sd:/3ds/Zelda 3DS/randomizer/imports/` with a short ASCII `.json`
   filename (letters, digits, spaces, `_`, `-` and `.`; no path separators).
3. Choose Import in the wizard and select the file. The importer copies the
   original JSON into that seed's save folder and checks its exact placements.
   It never substitutes a newly generated native seed.

A public hash alone, a hidden spoiler, a patched ROM, a BPS patch and a native
share string are not interchangeable with this JSON. The source is retained as
`alttpr.json`; keep it with the save. Missing or modified source is rejected.

The importer currently accepts single-world **Open or Standard, NoGlitches,
normal item pool/functionality and ordinary enemies/bosses**. Implemented goals
are Ganon, Fast Ganon, All Dungeons, Pedestal, Triforce Hunt, Ganon Hunt and
Completionist. Weapons may be randomized, assured or swordless. Dungeon items
may use standard, maps/compasses (`mc`), maps/compasses/small keys (`mcs`) or
keysanity (`full`) placement. Crystal requirements 0–7 are read explicitly.

Hunt imports require the **full official response with its `patch` array**:
the importer reads only the two goal-count bytes written by ALTTPR
`Rom::setGoalRequiredCount` at 0x180167. It never applies that patch to a ROM.
A spoiler alone does not reveal a custom target, so it is refused instead of
assuming 20 or 40. The actual piece total comes from the exact placements.
Assured imports require three starting hearts and one explicit starting sword
in `Equipped`; other custom starting gear is still refused.

Vanilla shops and fairy bottles are checked explicitly. Unknown rules, hidden
placements and unsupported world/entrance/door/shop/enemy variations fail with
a localized reason. Even a structurally supported seed is refused if the native
logic cannot certify its exact table. The tested corpus is evidence for those
seeds, not proof that every possible combination is supported.

## Implementation and provenance

Native C placement, logic and runtime modules originate from
[Slashmolder/zelda3randomizer](https://github.com/Slashmolder/zelda3randomizer),
commit `34495145fc638edc9ba0e327997c6c04e71460f9`. The upstream file manifest,
original licenses and MIT logic/art credits are in `engine/src/rando/upstream/`.
Desktop ImGui windows are excluded. JSON parsing uses unchanged MIT cJSON
v1.7.19; source hashes and its license are in `engine/third_party/cjson/`.

The generator uses a dedicated xoshiro256** stream, an integer seed, canonical
settings and bounded retries, followed by goal/accessibility certification.
Production passes a zero CPU-time budget so accepted retries do not depend on
processor speed. Generator version 166 retains the import-only 50-rupee item,
adds the distinct imported Pedestal Triforce reward and certifies native Triforce
Hunt against the actual castle-courtyard turn-in instead of the forest pedestal.
Existing item IDs remain stable; the two import-only rewards do not enter native
pools. Hint metadata uses algorithm/text schema 4/4. Optional ROM-derived registries are not fabricated;
advanced axes that require absent registries remain unavailable.

Runtime chest mapping reads 504 bytes from the user's local USA cartridge,
resolves 164 catalog identities and checks them against loaded assets. Invalid
input clears the map and prevents activation. No extracted chest table ships.
The existing 165-entry base asset format and translation patches are retained;
attributed custom randomizer art uses a separate C resource.

The importer maps 226 public physical/prize locations, four fixed story events
and two medallions into 232 exact entries. It also installs the imported prize,
medallion and fairy bottle assignments at runtime and on cold/snapshot resume.
Import-specific rules cover Standard escape lighting/refills, Open Sanctuary,
placement-dependent key routes in Thieves' Town, Palace of Darkness, Misery Mire
and Ganon's Tower, the advanced Ice Palace Somaria route (including its separate
boss-region edge), and ordinary Mirror/Mire and Turtle Rock portal entrances.
Normal boss combat follows the official basic/advanced equipment rules.
The native generator retains its own placement rules outside the documented
goal corrections. The parser bounds input to 1 MiB, nesting to 32,
nodes to 65,536 and object members to 512; duplicates, embedded NULs, trailing
bytes and another world's items are rejected.

### Victory and runtime behavior

Ganon damage is gated by the selected goal: required crystals plus Agahnim 2,
Fast Ganon's crystals, all pendants/crystals and both Agahnims, the acquired
piece target, or all countable checks plus both Agahnims. Pedestal and Triforce
Hunt finish through their own interactions. Fast Ganon and Ganon Hunt open the
pyramid hole in the actual loaded save. Creating it through Agahnim 2 records
that event rather than only invalidating tracker reachability.

Murahdahla appears at the official post-rain castle coordinate (0x790,0x720).
His existing local Elder art uses leased OBJ scratch slots, preserving ordinary
guard graphics. He reports the piece target/current count and accepts turn-in.
Both turn-in and a newly completed Pedestal ceremony wait for dialogue/item
receipt to finish before entering the engine's Triforce-room/credits sequence.
Reloading an already completed Pedestal file does not restart the ending.
A pending handoff uses the reserved randomizer WRAM byte 0x66f, so a cold SDL
snapshot during the turn-in dialogue still finishes; ordinary file activation
clears that pending request so it cannot leak into another file. Vanilla frame
processing leaves this byte untouched.
Direct, capped and saturated Pedestal rewards release ceremony-owned freeze
flags; ordinary hold-up rewards retain their normal animation teardown.

Acquired pieces are reconstructed from checked placements on file activation,
copy and snapshot restore. New files start at zero. Assured starting swords are
part of the immutable initial SRAM image and the logical starting inventory;
reload never downgrades or re-grants them. Ganon's three-line warnings describe
the actual goal and were checked for complete text in the real renderer.

## Save and lifecycle contracts

SHA-256 keys include origin, seed/settings identity and, for imports, the source
and placement digests. Paths for SRAM, sidecars, snapshots and spoilers resolve
inside the active seed namespace. Requests change storage after old-session
shutdown. Immutable import source and metadata are verified before activation.
Startup recovery runs before opening SDL render/audio resources, allowing an
invalid bundle to return to the error wizard without a teardown/reset loop.

`seed_save.dat` is the authoritative CRC-protected SRAM + serialized-sidecar
bundle, replaced atomically through the port's recoverable filesystem helper.
Separate SRAM/sidecar files are compatibility mirrors and can be reconstructed
from the bundle. Each occupied primary is checked against the sidecar and the
vanilla checksum; backup SRAM copies are rebuilt from those certified primaries,
including empty/erased files. This also recovers the earlier local candidate's
stale backup after a copy made before the first gameplay save.

`seed_initial.dat` preserves the seed's original placement table. Creating a
new empty file starts the same seed with fresh progress. Copy preserves checked
locations; erase clears both halves. Invalid CRCs fail closed and preserve the
files. The native stack reserve is 512 KiB for the existing large sidecar
serialization call chain. Normal and quest save folders remain separate.

## Validation evidence

All private ROMs, assets, saves, downloaded test JSON and generated captures
stay outside the source checkout in `TestBuilds/v4.0/`.

- Core ASan/UBSan: 40 generations across the five presets and four boundary
  seeds, repeated in reverse order, with goal/accessibility/sphere checks,
  unique locations and unchanged game WRAM.
- Chest mapping: all 164 runtime identities, mismatches, malformed lengths and
  reset behavior tested against the local authorized cartridge/assets.
- Full engine ASan: real file-selector activation, Open start, chest grant
  once, paired save and cold SDL snapshot. Exact import includes all 232 entries,
  ten boss prizes, medallions and both fairy bottle refills. Copy before the
  first gameplay save, deliberately stale backup, copy/erase/new-file paths,
  missing mirrors, interrupted rename and corrupt bundles are covered.
- Import validation: the official test JSON `mykaDWN6MQ` maps to placement digest
  `d2ed6155fc076e472b06dbb89692ae80165b9a7c674059f2be5d9efbb02b76cd`.
  All 232 entries saved by ARM match the host importer in the earlier checkpoint.
  The expanded host corpus covers 33 independently generated official seeds,
  including combined Standard/swordless, Standard/keysanity and advanced/keysanity
  rules, all seven goals, and assured starting gear. Forty malformed, hidden,
  multiworld, ambiguous-target or unsupported inputs are rejected with reasons.
- Native Open seed 42 retains placement digest
  `ebe0dda818ee3a88b16f27aa65c1a2de3b57798362aaae5b0bcb7cc6568b35ed`
  on host and ARM after adding the import-only item.
- Storage tests cover production namespace paths, rollback, normal/quest/seed
  separation and invalid keys. Mod loader tests pass. Six skins produce 630
  pixel-identical direct-ZSPR/mod scene and marker pairs across seven languages,
  including armor/gloves, file palettes and unloading.
- UI tests cover all seven languages and both physical output formats, touch,
  maximum seeds, long filenames, saved/import lists and all six localized error
  classes. Real captures were inspected. Wide copy/erase panels use a blank
  16x48 source patch instead of repeating a gold divider column. Language tours
  capture target selection and confirmations; background bounds tests pass.
- CIA/3DSX build and segment/relocation/SMDH/RomFS validation pass.
- Isolated New-model Azahar: No/Yes transition, ARM generation, save/resume,
  actual copy/erase/new file, prior-bundle recovery and exact ALttPR import.
  The official seed's first chest grants the expected Power Glove. Cold restart
  retains its progress. Corrupt bundles and modified source JSON return to the
  localized error wizard without a reset hang, preserving the files. Ordinary
  SRAM remained byte-identical.
- Isolated Old-model Azahar: the same imported save loads with Conker, including
  its character graphics and bottom map marker. The wizard/confirmation work;
  Normal restores Spanish and the original ordinary file with Conker retained.
  Normal SRAM stays byte-identical. This is emulation evidence.

The test runner preserves an old generated `data` folder before each new run:
the suite deliberately corrupts its final fixture and must not reuse it as a
new baseline. A first reused-folder failure was reproduced and corrected in
that harness; clean repeated runs pass.

## Remaining acceptance

A full randomized playthrough, exhaustive seed/variant coverage, physical
Old/New 3DS behavior and SD power-loss durability remain unverified. Host scene
tests with controlled inventory/position reach credits through both the complete
Pedestal ceremony and Murahdahla interaction; these are not full playthroughs.
Imported Inverted, Retro, vanilla weapons, other item pools/functionality,
custom starting gear, entrance/door/enemy/shop randomization and glitch modes
still require further adaptation/validation. Native Inverted's historical free
Moon Pearl is a known remaining fidelity issue; it must be corrected together
with the affected world-transition paths, not hidden by an import allowlist.
 Existing translated base-game content is not exhaustively
re-audited here: for example, German copy-target slot glyph corruption is also
present in the original narrow layout and is separate from the Wide fill fix.
No public release, issue closure or push is part of this local checkpoint.
