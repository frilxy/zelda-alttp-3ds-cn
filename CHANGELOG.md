# Changelog

## v3.2.4

- The bottom screen now uses GNU Unifont's native 16px bitmap strike for Chinese,
  replacing the 12px Fusion Pixel face.  Unifont covers 20992 Simplified-Chinese
  codepoints and its 16px strike is a pure 1-bit bitmap, so the 2bpp atlas format
  and both renderers are unchanged.
- Latin glyphs now come from Unifont too: a half-width 8x16 design sharing the CJK
  em box, so digits and A-Z sit on the same baseline as the Chinese.  They used to
  come from the game's own 8x8 tile font (~5x7 of ink), which read as a visibly
  different face next to 16px CJK.  Bottom-screen text no longer needs the game
  assets at all; icons still do.
- Every panel's vertical rhythm was re-tuned.  3.2.3's y positions assumed a ~10px
  text block; a 16px block is 60% taller, so screen, developer, settings, remap,
  overlay, items, load-confirmation and update all needed new row heights and
  start offsets.  Horizontal layout is unaffected: text_width() counts 16px per
  CJK and 8px per ASCII regardless of glyph size.
- New generator tools/secondscreen/gen_latin_sheet.py bakes the digits-and-A-Z
  sheet; gen_cjk_atlas.py now reads the Unifont face.

## v3.2.3

- Bottom-screen text now uses the 12px Fusion Pixel face: each glyph's ink grows
  from 9x9 to 11x11, filling the vertical space the 10px face left unused. The
  cell stays 16x16 and the advance stays 16px per character, so no layout
  constant changed.
- Every text scale is now an integer multiple. Panel titles were drawn at 1.5x,
  back buttons at 1.10x and the update banner at 0.80x; with nearest-neighbour
  sampling that made strokes alternate between one and two pixels wide, and the
  0.80x banner was genuinely discarding pixels. Body text stays 1.0x and panel
  titles become 2.0x.
- The 12px face ships in fonts/ alongside the old 10px one, so the atlas can be
  regenerated offline and switched back with --font.

## v3.2.2

- Fixed the in-game updater on the -cn-bottom build. It asked for
  zelda3-3ds-v<version>.cia whether or not the running build translated the
  bottom screen, so updating a -cn-bottom install silently replaced it with the
  standard build and the bottom screen reverted to English. The updater now
  requests the variant it is running; build.sh already names the assets that way.
- platform/3ds/README.md: the v3.2 release link said v3.2 while pointing at the
  v3.2.1 tag.

## v3.2.1

- Finished the bottom-screen Chinese: the update banner, the Display Mode and
  Edge Mode values, the remap "press key" prompt, and the Load State button with
  its result labels are translated now.
- The developer overlay and the pad/button names stay English on purpose, so
  diagnostics keep matching upstream and bug reports stay comparable.
- The bottom-screen glyph atlas grew from 164 to 176 characters.
- The Fusion Pixel 10px bitmap face now ships in fonts/, so the atlas can be
  regenerated without network access; fonts/README.md records its version,
  licence and checksums.

## v3.2

- Optimized update downloads and installation.
- Fixed black screens and missing audio after Restart and ROM reselection.

## v3.2-E1

- Fixed black screens and missing audio after Restart and ROM reselection.

## v3.1

- Added in-game GitHub updates with Stable/Pre-release channels and verified downloads.
- Added paginated changelogs with matching screen styles.
- Fixed bottom-screen startup, touch accuracy and navigation delay.
- Simplified Settings and combined Restart with ROM selection.
- Fixed stale world colors after mirror and portal transitions.
- Added touch history to diagnostic dumps.

## v3.1-E5

- Fixed touch press and release detection with residual coordinates.
- Added recent touch inputs to diagnostic dumps.
- Fixed shifted bottom-screen touch coordinates.
- Improved bottom-tab touch targets and reduced navigation delay.
- Added GitHub updates with Stable and Pre-release channels.
- Added verified downloads and in-game update installation.
- Added paginated changelogs with matching top and bottom menu styles.
- Combined Restart and ROM selection and simplified Settings.
- Fixed Update touch controls and blank bottom-screen startup.
- Fixed stale overworld colors after mirror and portal transitions.

## v3.1-E4

- Fixed shifted bottom-screen touch coordinates.
- Improved bottom-tab touch targets and reduced navigation delay.
- Added GitHub updates with Stable and Pre-release channels.
- Added verified downloads and in-game update installation.
- Added paginated changelogs with matching top and bottom menu styles.
- Combined Restart and ROM selection and simplified Settings.
- Fixed Update touch controls and blank bottom-screen startup.
- Fixed stale overworld colors after mirror and portal transitions.

## v3.1-E3

- Improved bottom-tab touch targets and reduced navigation delay.
- Added GitHub updates with Stable and Pre-release channels.
- Added verified downloads and in-game update installation.
- Added paginated changelogs with matching top and bottom menu styles.
- Combined Restart and ROM selection and simplified Settings.
- Fixed Update touch controls and blank bottom-screen startup.
- Fixed stale overworld colors after mirror and portal transitions.

## v3.1-E2 (local experimental)

- Fixed unresponsive Update controls on Old and New 3DS.
- Fixed missed touch navigation and blank bottom-screen startup on Old 3DS.
- Moved Update above Restart and enlarged the settings buttons evenly.
- Fixed unreadable update hints and matched top-screen changelogs to the menu style.

## v3.1-E1 (local experimental)

- Added GitHub updates with Stable and Pre-release channels.
- Added startup update checks, verified downloads and in-app installation.
- Added release changelogs on the top screen with page controls.
- Combined Restart and ROM selection; removed Remap Buttons from settings.
- Fixed stale overworld colors after mirror and portal world transitions.

## v3.0

- Added PICA200 GPU rendering for Old 3DS with automatic software fallback.
- Improved Old 3DS rendering, frame pacing, audio and bottom-screen performance.
- Fixed WIDE/FIXED map alignment, terrain corruption and screen-edge gaps.
- Added native 400x240 WIDE rendering and WIDE/FIXED defaults.
- Fixed incorrect colors, screen flickering and Original-mode borders.
- Reduced slowdowns during door and room transitions.
- Improved bottom-map refresh and touch responsiveness; added the mirror-return marker.
- Fixed delayed heart updates and gameplay stalls after taking damage.
- Fixed bottom-menu borders, HUD spacing and the map's +/− buttons.
- Added title-screen display/turbo settings and improved Triforce rendering.
- Fixed per-ROM settings persistence and configuration migration.
- Fixed valid ROMs being rejected and cleaned up ROM-switch handling.
- Fixed Old 3DS X taps while preserving hold-to-turbo controls.
- Improved HOME Menu responsiveness and startup/shutdown handling.
- Added Load State, an optional FPS counter and `DUMP SAVED` feedback.
- Improved numbered diagnostic dumps, screen captures and graphics/audio reports.
- Fixed dump freezes and audio playback during captures.
- Updated the custom 3D HOME Menu logo and banner orientation.
