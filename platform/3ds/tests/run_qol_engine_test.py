#!/usr/bin/env python3
"""Exercise QoL preferences and actual native gameplay paths with supplied assets.
Uses only the supplied cartridge assets, never reference snapshots. Verifies
SRAM isolation, death/Continue recovery and cold manual test-state restoration.
Private pictures and test snapshots stay in the explicit work directory.
"""
from pathlib import Path
import argparse, os, shlex, shutil, subprocess, zlib, struct

root = Path(__file__).resolve().parents[3]
p = argparse.ArgumentParser()
p.add_argument('--assets', type=Path, required=True, help='zelda3_assets.dat built from the USA ROM')

p.add_argument('--work-dir', type=Path, required=True)
p.add_argument('--only', type=str, default='', help='1-based scenarios, e.g. 3 or 1-5')
p.add_argument("--production-gates", action="store_true")
a = p.parse_args()
work = a.work_dir.resolve()
if work == root or root in work.parents:
    p.error('Keep the states outside the source tree')
work.mkdir(parents=True, exist_ok=True)

code = (root/'platform/3ds/tests/qol_engine_test.c').read_text()
if a.production_gates:
    code = r'''#include <assert.h>
#include <string.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
#include "src/test_scenarios.h"
uint32_t Platform3DS_GetActiveProfileId(void){return 1234;}
void SS_RequestTestScenario(int);void SS_RequestNextTestScenario(void);bool SS_TestScenarioAvailable(int);
int main(int argc,char**argv){
 assert(!ZELDA3_ENABLE_DEVELOPER_TESTS);assert(!chdir(argv[1]));ParseConfigFile(NULL);LoadAssets();LoadLinkGraphics();ZeldaInitialize();
 static uint8 ram[131072],sram[8192];memcpy(ram,g_ram,sizeof(ram));memcpy(sram,g_zenv.sram,sizeof(sram));
 for(int i=-1;i<=kTestScenarioCount;i++) {
  assert(!ZeldaLoadTestScenario(i));assert(!SS_TestScenarioAvailable(i));
  SS_RequestTestScenario(i);SS_RequestNextTestScenario();SecondScreen_RunFrameHook();
  assert(!memcmp(ram,g_ram,sizeof(ram))&&!memcmp(sram,g_zenv.sram,sizeof(sram)));
 }
 assert(!g_test_scenario_active);puts("PASS production scenario gates: direct loader, queued request, next-test request, availability, WRAM/SRAM unchanged");ZeldaShutdownSession();return 0;
}'''
config = os.environ.get('SDL2_CONFIG', 'sdl2-config')
prefix = subprocess.check_output([config, '--prefix'], text=True).strip()
raw = subprocess.check_output([config, '--cflags', '--libs'], text=True)
flags = shlex.split(raw.replace(prefix, shlex.quote(prefix))) + (['-DZELDA3_ENABLE_DEVELOPER_TESTS=0'] if a.production_gates else [])
sources = [str(x) for x in (root/'engine/src').glob('*.c') if x.name not in ['main.c', 'opengl.c', 'glsl_shader.c']]
sources += [str(x) for x in (root/'engine/src/rando').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c')]
sources += [str(x) for x in (root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/src/platform/linux/second_screen_sdl.c'),
            str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
(work/'generator.c').write_text(code)
exe = work/'native-scenarios'
with (work/'build.log').open('w') as log:
    subprocess.run(['cc', '-DSDL_MAIN_HANDLED', '-DSYSTEM_VOLUME_MIXER_AVAILABLE=0', '-O1', '-g', '-w', '-fsanitize=address', '-DZELDA3_TEST_DUMP_STATE',
                    '-ffunction-sections', '-fdata-sections', '-Wl,-dead_strip',
                    '-I'+str(root/'engine'), '-iquote', str(root/'engine/src'),
                    '-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),
                    str(work/'generator.c'), *sources, *flags, '-Wl,-rpath,'+prefix+'/lib', '-o', str(exe)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
data = work/'data'; (data/'saves').mkdir(parents=True, exist_ok=True)
shutil.copyfile(a.assets, data/'zelda3_assets.dat')

(data/'zelda3.ini').write_text('[General]\nAutosave = 0\n[Sound]\nEnableAudio = 0\n')
for sub in ('shots',):
    (work/sub).mkdir(exist_ok=True)
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

if a.production_gates:
    assert run.returncode==0,run.stdout+run.stderr
    print(run.stdout);raise SystemExit(0)
off=(work/'shots/dark-off.raw').read_bytes();on=(work/'shots/dark-on.raw').read_bytes()
count=lambda data:sum(any(data[i:i+3]) for i in range(0,len(data),4))
assert count(on)>2*count(off), 'dark room did not become fully visible'
assert any(on[(190*400+200)*4:(190*400+200)*4+3]), 'residual black lantern mask'
for raw_path in sorted((work/'shots').glob('*.raw')):
    b = raw_path.read_bytes(); rgb = bytearray(400 * 240 * 3)
    rgb[0::3] = b[2::4]; rgb[1::3] = b[1::4]; rgb[2::3] = b[0::4]
    png(raw_path.with_suffix('.png'), rgb, 400, 240); raw_path.unlink()
if run.returncode:
    raise SystemExit(f'generator failed ({run.returncode}); see {work/"generator.log"}')
print('PASS native QoL engine, preferences, recovery, capacity, inventory, blocks, fairies, shortcut and all scenarios')
