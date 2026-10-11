# Changelog

## v3.2.3

This is the final bottom-screen rework, released as one artifact per platform
because the bottom screen is now always translated.

Text is drawn at three levels, each calibrated by measuring the atlas ink box
rather than by guessing an offset:

  body     Fusion Pixel 12px, 1x     11x11 of ink   (same face and size as the
                                                     top screen's dialogue)
  titles   GNU Unifont   16px, 1x     15x16 of ink   (ink fills the cell)
  tabs     GNU Unifont   16px, 1x     15x16 of ink

Fusion Pixel ships 8/10/12px native cuts only, so a 16px heading has to come from
another face.  Scaling the 12px face by 1.5x gives about the right size but a
nearest-neighbour 1.5x duplicates pixels unevenly, so the horizontal bars came out
alternating between one and two pixels; Unifont's native 16px strike is a 1-bit
bitmap drawn at 1x, so it is exactly 16px with no scaling at all.

Alignment, all derived from where the ink actually sits in the 16px cell:

  tab labels    r.h / 2 - 8 * u     cell 9..25, centre 17 = the button's centre
  panel titles  r.y + 20 * u        ink 6..22, first rows start at 27.5-41px
  row text      row.h / 2 - 6 * u   ink centre lands on the row centre

The two-variant build is gone: build.sh always compiles with BOTTOM_SCREEN_CN=1,
the -cn-bottom suffix is removed from the artifact names, and
UPDATE_ASSET_VARIANT is now always empty so the updater looks for
zelda3-3ds-v<version>.cia / .3dsx.

Tools: gen_cjk_atlas.py bakes both atlases from one scraped character list;
check_headers.py and check_panel_fit.py run in CI to catch the two classes of bug
hit during this work -- a source using a symbol from a header it does not include,
and panel text colliding with a title or running past its box.

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
