#!/usr/bin/env python3
"""Build and play every authored developer scenario directly from code.
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
#include "src/sprite.h"
#include "src/misc.h"
#include "src/dump_state.h"
uint32_t Platform3DS_GetActiveProfileId(void){return 1234;}
#include "src/load_gfx.h"

extern void SS_CaptureOriginalLinkFace(void);
extern bool SS_RenderOriginalLinkFace(uint32 *);
extern bool SS_RenderLinkFace(uint32 *,int);
static unsigned intro_frames,intro_changes;static uint32 intro_previous;
static bool intro_frame(const uint32 *pixels,unsigned tick,void *root) {
  uint32 hash=2166136261u;for(int i=0;i<256*224;i++)hash=(hash^pixels[i])*16777619u;
  if(hash!=intro_previous)intro_changes++;intro_previous=hash;
  if(intro_frames%6==0){char path[1024];snprintf(path,sizeof(path),"%s/intro-%03u.raw",(char*)root,tick);FILE*f=fopen(path,"wb");assert(f);fwrite(pixels,4,256*224,f);fclose(f);}
  intro_frames++;return true;
}
static unsigned intro_audio_frames,intro_audio_nonzero;
static void intro_audio(const int16 *samples,unsigned count,void *root) {
  assert(count==534&&g_zenv.player->music_ptr_toplevel==0);
  for(unsigned i=0;i<count*2;i++)intro_audio_nonzero+=samples[i]!=0;
  intro_audio_frames++;
}
int main(int argc, char **argv) {
  assert(argc == 5);
  assert(chdir(argv[1]) == 0);
  ParseConfigFile(NULL); LoadAssets(); LoadLinkGraphics(); ZeldaInitialize();
  ZeldaSetLanguage(NULL); ZeldaReadSram();
  uint32 original_face[256],face[256];SS_CaptureOriginalLinkFace();
  assert(SS_RenderOriginalLinkFace(original_face)&&SS_RenderLinkFace(face,2));
  assert(!memcmp(original_face,face,sizeof(face)));
  unsigned visible=0;for(int i=0;i<256;i++)visible+=(face[i]>>24)!=0;assert(visible>50);
  char face_path[1024];snprintf(face_path,sizeof(face_path),"%s/original-face.raw",argv[2]);FILE *ff=fopen(face_path,"wb");assert(ff);fwrite(original_face,4,256,ff);fclose(ff);
  g_config.extended_aspect_ratio = 72; g_config.extend_y = true;
  g_config.features0 |= kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
  g_wanted_zelda_features = g_config.features0;
  g_zenv.ppu->extraLeftRight = 72; g_snes_width = 400; g_snes_height = 240;
  g_ppu_render_flags = kPpuRenderFlags_NewRenderer | kPpuRenderFlags_Height240;
  ZeldaSetWidescreenEdgeMode(1);
  static uint32 logo[256*224];
  for(int f=0;f<2000&&!(main_module_index==0&&submodule_index==8);f++)ZeldaRunFrame(0);
  g_zenv.ppu->extraLeftRight=0;
  ZeldaDrawPpuFrame((uint8*)logo,256*4,kPpuRenderFlags_NewRenderer);
  char tp[512];snprintf(tp,sizeof(tp),"%s/title.raw",argv[2]);FILE *tf=fopen(tp,"wb");assert(tf);fwrite(logo,4,256*224,tf);fclose(tf);
  g_zenv.ppu->extraLeftRight=72;
  printf("TITLE BG3 map=%x gfx=%x scroll=%u,%u screen=%x/%x\n",g_zenv.ppu->bgLayer[2].tilemapAdr,g_zenv.ppu->bgLayer[2].tileAdr,g_zenv.ppu->bgLayer[2].hScroll,g_zenv.ppu->bgLayer[2].vScroll,g_zenv.ppu->screenEnabled[0],g_zenv.ppu->screenEnabled[1]);
  char vp[512];snprintf(vp,sizeof(vp),"%s/title-vram.bin",argv[2]);FILE *vf=fopen(vp,"wb");assert(vf);fwrite(g_zenv.vram,2,32768,vf);fclose(vf);
  static uint8 common_reference[0x200];memcpy(common_reference,g_ram+0xb340,sizeof(common_reference));
  assert(memcmp(common_reference,(uint8[0x200]){0},sizeof(common_reference)));
  ZeldaDrawStartupLogo(logo);
  unsigned silver=0;for(int y=84;y<165;y++)for(int x=67;x<=75;x++) {
    assert((logo[y*256+x]&0xffffff)!=0xffde6b);
    silver+=(logo[y*256+x]&0xffffff)==0xe7efff;
  }assert(silver>20);
  char logo_path[512];snprintf(logo_path,sizeof(logo_path),"%s/logo.raw",argv[2]);
  FILE *lf=fopen(logo_path,"wb");assert(lf);fwrite(logo,4,256*224,lf);fclose(lf);
  static uint8 intro_sram[8192];memcpy(intro_sram,g_zenv.sram,sizeof(intro_sram));
  ZeldaAnimateStartupLogoWithAudio(logo,intro_frame,intro_audio,argv[2]);
  assert(intro_audio_frames>=intro_frames*2-1&&intro_audio_nonzero>1000);
  printf("INTRO AUDIO %u native SFX blocks, %u nonzero samples; native music suppressed\n",intro_audio_frames,intro_audio_nonzero);
  assert(intro_frames>50&&intro_changes>20&&!memcmp(intro_sram,g_zenv.sram,sizeof(intro_sram)));
  assert(g_zenv.ppu->extraLeftRight==72);
  printf("INTRO %u frames, %u distinct transitions; SRAM and viewport preserved\n",intro_frames,intro_changes);
  // A real castle guard still parries weapon clashes, while body contacts
  // from a native fighter-sword swing must enqueue nonzero damage.
  assert(ZeldaLoadTestScenario(2));
  static uint8 combat_ram[131072];memcpy(combat_ram,g_ram,sizeof(combat_ram));
  unsigned damaging=0,parries=0;
  for(int dir=0;dir<4;dir++)for(int dx=-24;dx<=24;dx+=4)for(int dy=-24;dy<=24;dy+=4) {
    memcpy(g_ram,combat_ram,sizeof(combat_ram));int k=0;
    cur_sprite_x=Sprite_GetX(k);cur_sprite_y=Sprite_GetY(k);
    link_x_coord=cur_sprite_x+dx;link_y_coord=cur_sprite_y+dy;link_direction_facing=dir*2;
    link_incapacitated_timer=link_auxiliary_state=0;button_b_frames=4;
    player_oam_x_offset=player_oam_y_offset=0;sprite_hit_timer[k]=sprite_give_damage[k]=0;set_when_damaging_enemies=0;
    Guard_ParrySwordAttacks(k);damaging+=sprite_give_damage[k]!=0;parries+=set_when_damaging_enemies==0x90;
  }
  assert(damaging&&parries);memcpy(g_ram,combat_ram,sizeof(combat_ram));
  printf("COMBAT %u damaging contacts, %u native parries; fighter sword and no Uncle acquisition required\n",damaging,parries);
  int first = atoi(argv[3]), last = atoi(argv[4]);
  static uint8 pixels[400 * 240 * 4], original[8192];
  memset(original, 0x5c, sizeof(original));
  memcpy(g_zenv.sram, original, sizeof(original));
  int failures = 0;
  for (int i = first; i <= last && i < kTestScenarioCount; i++) {
    bool loaded=ZeldaLoadTestScenario(i);
    assert(!memcmp(original,g_zenv.sram,sizeof(original)));
    assert(g_test_scenario_active);
    assert(ZeldaGetTestScenarioIndex()==i);
    assert(!memcmp(common_reference,g_ram+0xb340,sizeof(common_reference)));
    // Direct loads must initialize the native enemy damage subclasses too.
    for(int enemy=0x41;enemy<=0x4b;enemy++)for(int damage=0;damage<16;damage++) {
      int entry=enemy*16+damage;uint8 packed=kEnemyDamageData[entry/2];
      assert(enemy_damage_data[entry]==(entry&1?packed&15:packed>>4));
    }
    ZeldaWriteSram(); // must not persist the synthetic SRAM or live test
    bool ok=loaded && (main_module_index == 7 || main_module_index == 9 || main_module_index == 14);
    char path[512];
    g_zenv.ppu->wideUi = PpuUiChooseLayout(main_module_index, submodule_index);
    ZeldaDrawPpuFrame(pixels,400*4,g_ppu_render_flags);
    snprintf(path,sizeof(path),"%s/shots/%02d.raw",argv[2],i+1);
    FILE *f=fopen(path,"wb"); assert(f); fwrite(pixels,4,400*240,f); fclose(f);
    printf("%s %02d module %u/%u room 0x%03x ow 0x%02x hp %u/%u magic %u sword %u\n",ok?"OK":"FAIL",i+1,main_module_index,submodule_index,dungeon_room_index,overworld_screen_index,link_health_current,link_health_capacity,link_magic_power,link_sword_type);
    fflush(stdout);
    failures += !ok;
    if(i==8) {
      int crystal=-1;for(int k=0;k<16;k++)if(sprite_type[k]==0x1e)crystal=k;
      assert(crystal>=0);
      for(int flip=0;flip<4;flip++) {
        unsigned previous=orange_blue_barrier_state;
        sprite_F[crystal]=11;ZeldaRunFrame(0);
        assert(orange_blue_barrier_state==(previous^1));
        for(int frame=0;frame<24;frame++) {
          ZeldaRunFrame(0);
          if(frame==0||frame==8||frame==23) {
            ZeldaDrawPpuFrame(pixels,400*4,g_ppu_render_flags);
            snprintf(path,sizeof(path),"%s/shots/peg-%d-%02d.raw",argv[2],flip,frame);
            FILE *pf=fopen(path,"wb");assert(pf);fwrite(pixels,4,400*240,pf);fclose(pf);
          }
        }
        assert(submodule_index==0);
        int x=orange_blue_barrier_state?0x180:0,y=orange_blue_barrier_state?0:0x180;
        assert(!memcmp(g_zenv.vram+0x3d00,common_reference+x,128));
        assert(!memcmp(g_zenv.vram+0x3d40,common_reference+y,128));
      }
      puts("PEGS four native crystal hits: both states, complete animation, nonempty uploads equal normal-boot bank");
    }
    for(int k=0;k<6;k++) for(int frame=0;frame<24;frame++) ZeldaRunFrame((uint16[]){1<<5,1<<7,0,1<<8,0,1<<3}[k]);
    assert(!memcmp(original,g_zenv.sram,sizeof(original)));
    if(i==4) {
      SaveLoadSlot(kSaveLoad_Save,1);
      ZeldaReset(true);assert(!g_test_scenario_active);
      SaveLoadSlot(kSaveLoad_Load,1);
      assert(g_test_scenario_active&&link_health_current==4);
      char dump[256];mkdir("dumps",0755);
      assert(DumpState_CreateDirectoryAt("dumps","20261030-180000",dump,sizeof(dump)));
      assert(ZeldaWriteDumpState(dump));
      ZeldaReset(true);assert(!g_test_scenario_active);
      assert(ZeldaLoadLatestDumpState()==kZeldaDumpStateLoaded&&g_test_scenario_active&&link_health_current==4);
      Module05_LoadFile();
      assert(g_test_scenario_active&&main_module_index==7&&dungeon_room_index==0xc9);
      assert(link_health_current==4&&!memcmp(original,g_zenv.sram,sizeof(original)));
    }
  }
  ZeldaReset(true);
  char ordinary[256];assert(DumpState_CreateDirectoryAt("dumps","20261030-180001",ordinary,sizeof(ordinary)));
  assert(ZeldaWriteDumpState(ordinary));
  assert(ZeldaLoadLatestDumpState()==kZeldaDumpStateLoaded&&!g_test_scenario_active);
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

for raw_path in sorted((work/'shots').glob('*.raw')):
    b = raw_path.read_bytes(); rgb = bytearray(400 * 240 * 3)
    rgb[0::3] = b[2::4]; rgb[1::3] = b[1::4]; rgb[2::3] = b[0::4]
    png(raw_path.with_suffix('.png'), rgb, 400, 240); raw_path.unlink()
if run.returncode:
    raise SystemExit(f'generator failed ({run.returncode}); see {work/"generator.log"}')
print('PASS native scenarios, SRAM isolation and playable inputs; no snapshot inputs')
