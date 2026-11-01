# Development and release boundaries

## Source layout

- `engine/src`: native Zelda3 gameplay, assets interface, settings, save-state
  transactions, randomizer and renderer integration. Original gameplay modules
  retain their established layout so reverse-engineered behavior stays traceable.
- `engine/snes`: CPU-side video/audio model and drawing paths used by the port.
- `platform/3ds/source`: libctru lifecycle, input, NDSP streaming, PICA presentation,
  ROM/profile preparation, update transport and startup UI.
- `engine/src/platform/linux`: shared lower-screen UI, pixel fonts, maps and HUD.
- `platform/3ds/tests`: host engine replay, sanitizers, UI captures, lifecycle,
  persistence and GPU models. Private fixtures are supplied externally.
- `scripts`: repository and packaged executable checks. `docs`: user and developer
  documentation. Local build outputs, ROMs, saves and diagnostic dumps stay out.

## Developer scenarios

`engine/src/developer_tests.h` owns the gate. Normal builds default to disabled.
The 3DS CMake option `ZELDA3_DEVELOPER_TESTS` defaults to OFF and explicitly sets
`ZELDA3_ENABLE_DEVELOPER_TESTS=0` across the target. To build a private development
binary, configure that option ON. Named host-test fixtures opt in unless an
explicit production override is supplied.

With the gate off, the developer panel omits Tests, its touch rectangle is empty,
B+L passes through as gameplay input, requests cannot queue scenarios, scenario
availability returns false, and the engine loader rejects even direct calls.
The authored catalog and tools remain in source. Existing save-protection logic
for historical test snapshots remains intact. Diagnostic capture, regular save
states, FPS and performance overlays remain available.

## Verification

`run_native_input_test.py` checks release and development input behavior.
`run_native_ui_layout_test.py --production --out <private-dir>` verifies release
controls and captures all languages/formats. `run_qol_engine_test.py --assets
<private-assets> --work-dir <private-dir> --production-gates` checks production
loader/request rejection and unchanged WRAM/SRAM. Run it without the last option
to exercise all authored scenarios in an opted-in host fixture. Startup, save,
APT, audio and PPU tests cover their own production functions.

Build with the existing `platform/3ds/build.sh`, `ZELDA3_VERSION_LABEL=4.0` and
an isolated build directory. Check `scripts/check-repository.py` and
`scripts/check-3dsx.py`. Host success and emulator boot do not prove physical
Old/New 3DS performance. A release must freeze the exact clean source commit and
match artifact hashes. Publication is a separate owner-approved action.
