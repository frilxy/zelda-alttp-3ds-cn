# Building and testing the 3DS port

The supported application is the 3DS target. Host programs exercise portable
logic and draw the production UI; they are not a desktop release of the game.
Keep ROMs, extracted assets, saves and diagnostic captures outside this checkout.

## Target build

Install devkitPro with devkitARM, libctru, Citro2D/Citro3D, and the curl,
Jansson and mbedTLS dependencies described in `platform/3ds/update-dependencies`.
The build uses `DEVKITPRO` or `/opt/devkitpro`, with `$HOME/devkitpro` as a
fallback. An explicit `UPDATE_DEPS_ROOT` selects the exact static archives as
well as their headers. It must not silently link a different SDK curl.

```sh
ZELDA3_BUILD_ROOT=/absolute/path/to/build \
UPDATE_DEPS_ROOT=/absolute/path/to/3ds-dependencies \
bash platform/3ds/build.sh
```

`ZELDA3_BUILD_ROOT` is optional (default `build-3ds`). Use a fresh directory
after moving the checkout or changing SDKs. The version comes from the CMake
project; the Homebrew Launcher filename remains `zelda3-3ds.3dsx`. CIA
packaging also needs `MAKEROM` and `BANNERTOOL`, or the documented local tools.
The build checks the final 3DSX relocation types, segment bounds, icon and
RomFS before packaging. A linked ELF alone does not establish launchability.

## Host SDL

Use a real SDL2 installation or build the pinned vendored SDL2. A host's
`sdl2-compat` package also requires its SDL3 runtime, and does not test the same
SDL implementation used on the console. Building the vendored source avoids
that dependency and does not change system packages:

```sh
bash platform/3ds/tests/build-host-sdl.sh /absolute/path/to/host-sdl
export SDL2_CONFIG=/absolute/path/to/host-sdl/prefix/bin/sdl2-config
```

The host helper supports prefixes with spaces and macOS dynamic-library paths.
Use `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` for software-only tests.

## Focused regressions

```sh
python3 scripts/check-repository.py
python3 platform/3ds/tests/run_native_input_test.py
python3 platform/3ds/tests/run_native_menu_test.py
python3 platform/3ds/tests/run_native_file_labels_test.py
python3 platform/3ds/tests/run_autosave_recovery_test.py
python3 platform/3ds/tests/run_rom_shutdown_test.py
python3 platform/3ds/tests/run_sram_store_test.py
python3 platform/3ds/tests/run_panned_sfx_test.py
python3 platform/3ds/tests/run_file_menu_background_test.py
python3 platform/3ds/tests/run_file_store_test.py
python3 platform/3ds/tests/run_runtime_persistence_test.py
python3 platform/3ds/tests/run_profile_boot_test.py
python3 platform/3ds/tests/run_exit_cleanup_test.py
python3 platform/3ds/tests/run_native_ui_layout_test.py --out /tmp/alttp-ui
python3 platform/3ds/tests/run_ui_strings_test.py --out /tmp/alttp-strings
python3 platform/3ds/tests/run_bottom_hearts_test.py
python3 platform/3ds/tests/run_ppu_merge_span_test.py
python3 platform/3ds/tests/run_ppu_gpu_model_test.py --scenes 1024
python3 platform/3ds/tests/run_ppu_old3ds_test.py --sanitize --scenes 1024
python3 platform/3ds/tests/run_mods_test.py --out /tmp/alttp-mods
```

The historical renderer test reads a frozen Git commit, including its original
pre-reorganization path. It compares pixel words, not hardware frame rate.
The menu tests use production dispatch with rendering helpers stubbed; the UI
test renders real controls in both RGB565 and ARGB8888. Its optional `--art`
argument accepts a private local artwork export for map/inventory captures.

`run_restart_boot_test.py --dump PATH` needs a private dump with `ram.bin` and
`sram.bin`. `run_world_palette_test.py --dump PATH` needs a captured palette.
The engine replay runner separately requires the user's USA ROM, extracted
assets and upstream chapter recordings. Missing input is a coverage gap,
not a passing test.

## Emulator and hardware evidence

Run Azahar with a separate user directory and copied test inputs. Record the
exact artifact hash, emulator version, Old/New model, graphics backend and
scenario. Test first launch, file selection/cancel, gameplay, settings,
reset, save/reopen, ROM/language switching and error recovery. Keep private
input files and screenshots outside source snapshots.

Software, OpenGL and Vulkan results are distinct. Emulator speed and static
host replay timing do not establish Old/New console FPS. Physical acceptance
still covers stereo, sound, sustained frame pacing, HOME/sleep/wake, SD errors
and CIA versus Homebrew Launcher behavior on the supported models.

## E2 native scenarios and startup

```sh
python3 platform/3ds/tests/run_native_scenarios_test.py \
  --assets /private/zelda3_assets.dat --work-dir /private/native-scenarios
python3 platform/3ds/tests/run_startup_ui_test.py \
  --out /private/startup-ui --logo /private/native-scenarios/logo.raw
python3 platform/3ds/tests/run_title_credit_test.py \
  --out /private/title-credit --title /private/native-scenarios/title.raw \
  --vram /private/native-scenarios/title-vram.bin
```

The scenario test uses no reference snapshots. Its generated pictures and manual
state/dump fixtures remain private. The startup test runs the production drawing
and input loops with SDK calls stubbed; review the final ARM build in Azahar too.
The credit check compares the shared N/n/t/e shapes with the supplied cartridge.

For the reported world-map transition, pass the captured directory to
`run_vertical_camera_test.py --dumps /private/002-dump-... --output /private/camera`.
It checks all 15 fade brightness levels before the mode-7 map begins. Run the
randomizer game test for presets 0 through 4 to exercise names and initial files
across all supported native starts.

## Quality of Life candidate

Run the production preferences and native engine checks with a private asset
file and an output directory outside the checkout:

```sh
python3 platform/3ds/tests/run_qol_engine_test.py \
  --assets /private/path/zelda3_assets.dat --work-dir /private/path/qol-check
```

This exercises capacity clamps at every upgrade level, preference recovery and
write failures, bottle/shovel ownership, repeated native block movement, eight
heart fairies, shortcut reversibility, full-room lighting, all forty scenarios
and exact RAM replay equivalence after changing inventory preferences.

`run_randomizer_game_test.py --qol-all` enables every option before running
the existing generation, naming, chest, paired-save and cold-state checks.
Run it with `--preset 0` through `--preset 4` and separate work directories.
`run_startup_ui_test.py` and `run_native_ui_layout_test.py` include the startup
and in-game QoL lists in all seven languages and both bottom pixel formats.
