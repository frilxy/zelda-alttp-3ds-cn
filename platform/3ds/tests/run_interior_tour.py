#!/usr/bin/env python3
"""Render every interior entrance in WIDE with the fixed camera.

Loads a snesrev/zelda3 reference save, enters each underworld entrance
(houses, caves, shops and dungeons) and writes the settled 400x240 picture,
with the 3DS native interface layout, as one PNG per entrance. Use it to
review the WIDE side margins of every room type by eye.

No game data belongs in the source tree. The work directory must be outside it.
"""
from pathlib import Path
import argparse, os, shlex, shutil, subprocess, zlib, struct

root = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser()
p.add_argument('--assets', type=Path, required=True)
p.add_argument('--refs', type=Path, required=True, help='directory with the reference .sav chapters')
p.add_argument('--work-dir', type=Path, required=True)
p.add_argument('--chapter', type=int, default=12, help='0-based reference chapter (12: after Ganon\'s Tower)')
p.add_argument('--entrances', type=str, default='1-132')
p.add_argument('--settle', type=int, default=150, help='frames to wait after the room has loaded')
a = p.parse_args()
work = a.work_dir.resolve()
if work == root or root in work.parents:
    p.error('Keep private inputs outside tracked source')
work.mkdir(parents=True, exist_ok=True)

code = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
#include "snes/ppu_ui.h"
static void Write(const char *dir, int entrance, const uint8 *pixels) {
  char path[512]; snprintf(path, sizeof(path), "%s/entrance-%03d.raw", dir, entrance);
  FILE *f = fopen(path, "wb"); assert(f);
  fwrite(pixels, 4, 400 * 240, f); fclose(f);
}
int main(int argc, char **argv) {
  assert(argc == 7);
  assert(chdir(argv[1]) == 0);
  ParseConfigFile(NULL); LoadAssets(); LoadLinkGraphics(); ZeldaInitialize();
  ZeldaSetLanguage(NULL); ZeldaReadSram();
  // The 3DS WIDE configuration: 400x240, visual fixes and the fixed camera.
  g_config.extended_aspect_ratio = 72; g_config.extend_y = true;
  g_config.features0 |= kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
  g_wanted_zelda_features = g_config.features0;
  g_zenv.ppu->extraLeftRight = 72; g_snes_width = 400; g_snes_height = 240;
  g_ppu_render_flags = kPpuRenderFlags_NewRenderer | kPpuRenderFlags_Height240;
  ZeldaSetWidescreenEdgeMode(1);
  SaveLoadSlot(kSaveLoad_Load, 256 + atoi(argv[3]));
  for (int i = 0; i < 60; i++) ZeldaRunFrame(0);
  int first = atoi(argv[4]), last = atoi(argv[5]), settle = atoi(argv[6]);
  static uint8 pixels[400 * 240 * 4];
  for (int e = first; e <= last; e++) {
    which_entrance = e; main_module_index = 6; submodule_index = 0;
    subsubmodule_index = 0; link_auxiliary_state = 0; link_incapacitated_timer = 0;
    int frames = 0;
    while (frames < 1200 && !(main_module_index == 7 && submodule_index == 0)) {
      ZeldaRunFrame(0); frames++;
    }
    for (int i = 0; i < settle; i++) ZeldaRunFrame(0);
    g_zenv.ppu->wideUi = PpuUiChooseLayout(main_module_index, submodule_index);
    // The side bands patch VRAM and OAM for the frame only.
    static uint16 vram_before[0x8000], oam_before[0x120];
    memcpy(vram_before, g_zenv.ppu->vram, sizeof(vram_before));
    memcpy(oam_before, g_zenv.ppu->oam, sizeof(g_zenv.ppu->oam));
    ZeldaDrawPpuFrame(pixels, 400 * 4, g_ppu_render_flags);
    assert(!memcmp(vram_before, g_zenv.ppu->vram, sizeof(vram_before)));
    assert(!memcmp(oam_before, g_zenv.ppu->oam, sizeof(g_zenv.ppu->oam)));
    Write(argv[2], e, pixels);
    printf("entrance %d room 0x%03x module %u/%u indoors %d frames %d repeat %d left %d right %d\n",
           e, dungeon_room_index, main_module_index, submodule_index, player_is_indoors, frames,
           g_zenv.ppu->wideMarginRepeat, g_zenv.ppu->extraLeftCur, g_zenv.ppu->extraRightCur);
  }
  ZeldaShutdownSession(); return 0;
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
(work/'tour.c').write_text(code)
exe = work/'interior-tour'
with (work/'build.log').open('w') as log:
    subprocess.run(['cc', '-DSDL_MAIN_HANDLED', '-DSYSTEM_VOLUME_MIXER_AVAILABLE=0', '-O1', '-g', '-w',
                    '-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip',
                    '-I'+str(root/'engine'), '-iquote', str(root/'engine/src'),
                    '-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),
                    str(work/'tour.c'), *sources, *flags, '-Wl,-rpath,'+prefix+'/lib', '-o', str(exe)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
data = work/'data'; (data/'saves/ref').mkdir(parents=True, exist_ok=True)
shutil.copyfile(a.assets, data/'zelda3_assets.dat')
for source in a.refs.glob('*.sav'):
    shutil.copyfile(source, data/'saves/ref'/source.name)
(data/'zelda3.ini').write_text('[General]\nAutosave = 0\n[Sound]\nEnableAudio = 0\n')
out = work/'frames'; out.mkdir(exist_ok=True)
first, last = (int(x) for x in a.entrances.split('-'))
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
log = subprocess.run([str(exe), str(data), str(out), str(a.chapter), str(first), str(last), str(a.settle)],
                     env=env, text=True, capture_output=True)
(work/'tour.log').write_text(log.stdout + log.stderr)
if log.returncode:
    raise SystemExit(f'tour failed ({log.returncode}); see {work/"tour.log"}')

def png(path, rgba, w, h):
    rows = b''.join(b'\0' + bytes(rgba[y*w*3:(y+1)*w*3]) for y in range(h))
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

for raw_path in sorted(out.glob('entrance-*.raw')):
    b = raw_path.read_bytes(); rgb = bytearray(400 * 240 * 3)
    rgb[0::3] = b[2::4]; rgb[1::3] = b[1::4]; rgb[2::3] = b[0::4]
    png(raw_path.with_suffix('.png'), rgb, 400, 240); raw_path.unlink()
print(f'PASS {last - first + 1} entrances rendered to {out}')
