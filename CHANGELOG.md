# Changelog

## v3.2.11

Two small fixes on the bottom screen's gear page.

- The 水晶 label sat 9px above the crystal circles it names. The 16px CJK cell
  is blitted at `y - 4` and its ink centre is cell row 7, so the label is now
  drawn at `cyC - 3`: its ink centre lands exactly on the circles' centre line.
- The heart in front of the heart-piece row is sized to match that row's box
  (gold outer square 20px): the 8px glyph is drawn at 2.5 (8 * 2.5 = 20), so it
  matches the box exactly. Its cell top-left stays at (x0, yp - 3).


## v3.2.10

Stop the top-left corner flashing the MSU track name on every music change.

libctru's consoleInit() attaches STDOUT to a devoptab that renders characters
into the console framebuffer, and it only does that once. The setup screens then
call gfxExit(), which frees that framebuffer, but the devoptab stays attached. So
every later printf painted glyphs over the game: the MSU loader's "Loading MSU
<file>" did it on each track change, which read as the track name flashing up.

The live console's PrintChar hook now swallows output once the setup console is
torn down, so stdout is inert for the rest of the session. The default console
template is left alone, so a later consoleInit() still restores the setup
screens. Diagnostics keep going to runtime.log.

Also centre the 装备/道具/设置 panel titles between the frame's inner edge and
the first row (16px cell, 9u above / 55u below => 24u). The map view has no
title, so only those three move.

## v3.2.9

Nudge the bottom screen's BACK label down by one pixel.

The settings panels place the label from the plate's centre, but the 16px
Unifont cell's ink centre sits above that point, so every BACK plate read a
little high. All six places that draw it -- remap, screen, developer, overlay,
update and the cinema overlay -- now go through one helper that adds a fixed
one-pixel drop, so the panels cannot drift apart again.

## v3.2.8

Fix the crackle that v3.2.7 introduced.

v3.2.7 clamped the MSU mix against overflow. The clamp was correct, but it made a
latent signedness bug audible: the mix helpers kept the volume scale in uint32,
so a negative sample was promoted to unsigned, became a huge positive value, and
clamped to +32767. The lower half of every waveform collapsed to full-scale DC.

The previous `dst += ...` hid it, because assigning to int16 truncates modulo
2^16 and the low bits happen to be the correct signed value. Both scales are
signed now. The crackle was most audible as a track started, since that is when
the fade-in uses the ramp path.

## v3.2.7

Clamp the MSU mix, and make MSU failures always reach the log.

MSU audio is added on top of the S-DSP output and both are full-scale 16-bit, so
a loud track under a loud sound effect could wrap past the int16 range and crackle
instead of merely clipping. All three accumulation sites now saturate.

Diagnostics: successful track loads drew from the same 24-line budget as
failures, so a long session could exhaust it and hide a "track not found" line.
Failures are now logged regardless of the budget.

## v3.2.6

Fix sound effects being silent while MSU music plays.

Pausing the SPC player for MSU only stops the music channels: pickups,
footsteps and water splashes are separate SPC700 voices written through
APUI01/APUI02/APUI03 every NMI, and they are synthesized by the same S-DSP. An
earlier build skipped the DSP resampling pass while MSU owned the output on the
assumption that the SPC block was silent, which silenced every effect too.

MSU is mixed over the S-DSP output again. That keeps the resampling cost while
MSU plays, so the audio load is about +19% over the 32000 Hz baseline rather
than the -31% the skipped pass had suggested.

## v3.2.5

Bold bottom-screen titles.

The panel titles and tab labels are drawn in GNU Unifont 16px, which ships one
weight only; its 1px strokes looked light next to the 12px body text. Unifont has
no bold cut and no variable axis, so the weight is synthesized with the "shift
right and overlap" method:

    solid  = shift_right(bitmap) | bitmap
    shadow = shift_right(solid & ~bitmap)
    result = solid & ~shadow

Subtracting the added material once more is what keeps counters open -- a plain
dilate turns dense glyphs into solid blocks. Unifont's CJK ink
spans x=0..14 of the cell, so the extra pixel fits in the free right column:
no clipping, no cell resize, no change to panel layout (check_panel_fit passes
unchanged). Body text keeps the regular cut.

## v3.2.4

MSU-1 streaming audio.

Put `1.pcm` … `35.pcm` (44.1 kHz, 16-bit, stereo, 8-byte `MSU1` header) in
`sdmc:/3ds/Zelda 3DS/msu/` and the orchestrally arranged tracks replace the SPC
music. No configuration is needed: startup probes `msu/1.pcm`, `msu/0.pcm`,
`msu1.pcm` and `1.pcm`, and a hit turns MSU on, fills in the directory and moves
the output to 44100 Hz. MSU is skipped entirely when no track is found.

Sample rate follows the same decision. MSU PCM is 44100 Hz and the mixer does no
resampling, so MSU playback has to run at 44100. The SPC700 interpreter's cost is
fixed at 534 samples per frame regardless of output rate, so raising it only adds
DSP resampling work -- and with MSU supplying the music that resampling pass is
skipped, which makes MSU playback cheaper than the SPC path it replaces. Old 3DS
without MSU keeps the previous 32000 Hz.

Two fixes found while wiring this up:

  stereo   SetupAudioStart() asked NDSP for a mono output. NDSP's output mode is
           global and the SDL driver never sets it, so the request is now for
           STEREO; today ndspExit() resets it before SDL reopens the device, but
           nothing should rely on that. runtime.log reports the negotiated
           channel count either way.

  settings EnsureProfileReady() copied the per-ROM profile ini over the working
           directory's zelda3.ini on every launch, silently discarding edits --
           including every MSU key. It now seeds the file only when missing,
           matching what MigrateWideDefaults() already did for the same file.

`tools/check_msu_tracks.py` validates a pack before copying it to the SD card
(header, sizing, duration sanity, numbering gaps). `docs/MSU-AUDIO.md` is the
full guide; the README has the short version plus the track-number table.

Track packs are not distributed with this project.

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
