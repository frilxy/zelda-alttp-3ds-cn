#!/usr/bin/env python3
"""Build the developer menu's test scenarios: one save state per entry of
engine/src/test_scenarios.c, in its dungeon or area, with its equipment,
life and magic.

Each starts from a snesrev/zelda3 reference save (one per chapter), enters
its underworld entrance or leaves its room for the overworld, waits for the
game to settle and writes NN.sav (from 01) plus a 400x240 picture to review.
Copy the .sav files to the RomFS with -DZELDA3_TEST_SCENARIOS=<out>/states.

The states hold the cartridge's graphics: keep the output outside the tree.
"""
from pathlib import Path
import argparse, os, shlex, shutil, subprocess, zlib, struct

root = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser()
p.add_argument('--assets', type=Path, required=True, help='zelda3_assets.dat built from the USA ROM')
p.add_argument('--refs', type=Path, required=True, help='directory with the reference Chapter *.sav')
p.add_argument('--work-dir', type=Path, required=True)
p.add_argument('--only', type=str, default='', help='1-based scenarios, e.g. 3 or 1-5')
a = p.parse_args()
work = a.work_dir.resolve()
if work == root or root in work.parents:
    p.error('Keep the states outside the source tree')
work.mkdir(parents=True, exist_ok=True)

code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
#include "src/test_scenarios.h"
#include "snes/ppu_ui.h"
#include "src/hud.h"
#include "src/load_gfx.h"

static void Apply(const TestScenario *s) {
  for (const TestScenarioSet *set = s->sets; set->addr; set++)
    g_ram[0xf000 + set->addr] = set->value;
  follower_indicator = 0;
  DecompressSwordGraphics(); DecompressShieldGraphics();
  Palette_Load_Sword(); Palette_Load_Shield(); Palette_Load_LinkArmorAndGloves();
  Hud_SearchForEquippedItem(); Hud_Rebuild(); Hud_UpdateEquippedItem();
}

static bool ExitKnown(int room) {
  int n = (int)(kExitDataRooms_SIZE / sizeof(uint16));
  for (int k = 0; k < n; k++) if (kExitDataRooms[k] == room) return true;
  return false;
}

int main(int argc, char **argv) {
  assert(argc == 5);
  assert(chdir(argv[1]) == 0);
  ParseConfigFile(NULL); LoadAssets(); LoadLinkGraphics(); ZeldaInitialize();
  ZeldaSetLanguage(NULL); ZeldaReadSram();
  g_config.extended_aspect_ratio = 72; g_config.extend_y = true;
  g_config.features0 |= kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
  g_wanted_zelda_features = g_config.features0;
  g_zenv.ppu->extraLeftRight = 72; g_snes_width = 400; g_snes_height = 240;
  g_ppu_render_flags = kPpuRenderFlags_NewRenderer | kPpuRenderFlags_Height240;
  ZeldaSetWidescreenEdgeMode(1);
  int first = atoi(argv[3]), last = atoi(argv[4]);
  static uint8 pixels[400 * 240 * 4];
  int failures = 0;
  for (int i = first; i <= last && i < kTestScenarioCount; i++) {
    const TestScenario *s = &kTestScenarios[i];
    SaveLoadSlot(kSaveLoad_Load, 256 + s->chapter);
    for (int f = 0; f < 30; f++) ZeldaRunFrame(0);
    Apply(s);
    savegame_is_darkworld = s->dark_world ? 0x40 : 0;
    link_auxiliary_state = 0; link_incapacitated_timer = 0;
    subsubmodule_index = 0; submodule_index = 0;
    if (s->kind == kTestScenario_Entrance) {
      which_entrance = s->target; main_module_index = 6;
    } else {
      if (!ExitKnown(s->target)) { printf("FAIL %02d no exit for room 0x%03x\n", i + 1, s->target); failures++; continue; }
      EnableForceBlank();
      dungeon_room_index = s->target; player_is_indoors = 1; main_module_index = 8;
    }
    int want = s->kind == kTestScenario_Entrance ? 7 : 9, frames = 0;
    while (frames < 1500 && !(main_module_index == want && submodule_index == 0)) {
      ZeldaRunFrame(0); frames++;
    }
    for (int f = 0; f < 90; f++) ZeldaRunFrame(0);
    // The loads refill and recount; the scenario's values win.
    Apply(s);
    for (int f = 0; f < 10; f++) ZeldaRunFrame(0);
    // Ganon greets Link with a message as the room opens.
    bool ok = (main_module_index == want || main_module_index == 14) && (main_module_index == 14 || submodule_index == 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/states/%02d.sav", argv[2], i + 1);
    ok = ZeldaWriteTestScenarioState(path) && ok;
    g_zenv.ppu->wideUi = PpuUiChooseLayout(main_module_index, submodule_index);
    ZeldaDrawPpuFrame(pixels, 400 * 4, g_ppu_render_flags);
    snprintf(path, sizeof(path), "%s/shots/%02d.raw", argv[2], i + 1);
    FILE *f = fopen(path, "wb"); assert(f); fwrite(pixels, 4, 400 * 240, f); fclose(f);
    printf("%s %02d module %u/%u room 0x%03x ow 0x%02x x %u y %u hp %u/%u magic %u sword %u frames %d dark %u  %s\n",
           ok ? "OK  " : "FAIL", i + 1, main_module_index, submodule_index, dungeon_room_index,
           overworld_screen_index, link_x_coord, link_y_coord, link_health_current, link_health_capacity,
           link_magic_power, link_sword_type, frames, savegame_is_darkworld, s->name);
    failures += !ok;
  }
  // Load each state as the developer menu does and play a few seconds:
  // walking, swinging the sword, opening the item menu.
  for (int i = first; i <= last && i < kTestScenarioCount; i++) {
    if (!ZeldaLoadTestScenario(i)) { printf("FAIL %02d does not load\n", i + 1); failures++; continue; }
    static const uint16 kPlay[] = { 1 << 5, 1 << 7, 1 << 4, 1 << 6, 0, 1 << 0, 0, 1 << 3, 0, 1 << 3, 0 };
    for (int k = 0; k < (int)(sizeof(kPlay) / sizeof(kPlay[0])); k++)
      for (int f = 0; f < 24; f++) ZeldaRunFrame(kPlay[k]);
    for (int f = 0; f < 60; f++) ZeldaRunFrame(0);
    bool ok = g_test_scenario_active && (main_module_index == 7 || main_module_index == 9 ||
                                        main_module_index == 14 || main_module_index == 0x12);
    printf("%s %02d replay module %u/%u room 0x%03x ow 0x%02x hp %u\n", ok ? "OK  " : "FAIL", i + 1,
           main_module_index, submodule_index, dungeon_room_index, overworld_screen_index, link_health_current);
    failures += !ok;
  }
  ZeldaShutdownSession();
  return failures ? 1 : 0;
}
'''
config = os.environ.get('SDL2_CONFIG', 'sdl2-config')
prefix = subprocess.check_output([config, '--prefix'], text=True).strip()
raw = subprocess.check_output([config, '--cflags', '--libs'], text=True)
flags = shlex.split(raw.replace(prefix, shlex.quote(prefix)))
sources = [str(x) for x in (root/'engine/src').glob('*.c') if x.name not in ['main.c', 'opengl.c', 'glsl_shader.c']]
sources += [str(x) for x in (root/'engine/src/rando').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c')]
sources += [str(x) for x in (root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/src/platform/linux/second_screen_sdl.c'),
            str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
(work/'generator.c').write_text(code)
exe = work/'make-test-states'
with (work/'build.log').open('w') as log:
    subprocess.run(['cc', '-DSDL_MAIN_HANDLED', '-DSYSTEM_VOLUME_MIXER_AVAILABLE=0', '-O1', '-g', '-w',
                    '-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip',
                    '-I'+str(root/'engine'), '-iquote', str(root/'engine/src'),
                    '-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),
                    str(work/'generator.c'), *sources, *flags, '-Wl,-rpath,'+prefix+'/lib', '-o', str(exe)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
data = work/'data'; (data/'saves/ref').mkdir(parents=True, exist_ok=True)
shutil.copyfile(a.assets, data/'zelda3_assets.dat')
for source in a.refs.glob('*.sav'):
    shutil.copyfile(source, data/'saves/ref'/source.name)
(data/'zelda3.ini').write_text('[General]\nAutosave = 0\n[Sound]\nEnableAudio = 0\n')
for sub in ('states', 'shots'):
    (work/sub).mkdir(exist_ok=True)
if not (data/'scenarios').exists():
    (data/'scenarios').symlink_to(work/'states')
first, last = 1, 99
if a.only:
    parts = [int(x) for x in a.only.split('-')]
    first, last = parts[0], parts[-1]
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
run = subprocess.run([str(exe), str(data), str(work), str(first - 1), str(last - 1)],
                     env=env, text=True, capture_output=True)
(work/'generator.log').write_text(run.stdout + run.stderr)
print('\n'.join(l for l in run.stdout.splitlines() if l.startswith(('OK', 'FAIL'))))

def png(path, rgba, w, h):
    rows = b''.join(b'\0' + bytes(rgba[y*w*3:(y+1)*w*3]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

for raw_path in sorted((work/'shots').glob('*.raw')):
    b = raw_path.read_bytes(); rgb = bytearray(400 * 240 * 3)
    rgb[0::3] = b[2::4]; rgb[1::3] = b[1::4]; rgb[2::3] = b[0::4]
    png(raw_path.with_suffix('.png'), rgb, 400, 240); raw_path.unlink()
if run.returncode:
    raise SystemExit(f'generator failed ({run.returncode}); see {work/"generator.log"}')
print(f'PASS states in {work/"states"}')
