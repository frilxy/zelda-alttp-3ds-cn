#!/usr/bin/env python3
"""Replay native dungeon checkpoints, audit every room decoder and compare reset PCM.
Keep supplied private checkpoints/assets and generated captures outside the source tree.
"""
from pathlib import Path
import argparse,os,shlex,shutil,struct,subprocess
from host_test_support import sdl_flags
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--assets',type=Path,required=True);p.add_argument('--dumps',type=Path,nargs='+',required=True);p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--direction',choices=['left','right','both','up','down','vertical'],default='left');p.add_argument('--old-cpu',action='store_true');p.add_argument('--bright-rooms',action='store_true');p.add_argument('--lamp-oracle',action='store_true');p.add_argument('--rtl-source',type=Path);a=p.parse_args()
work=a.work_dir.resolve();assert work!=root and root not in work.parents;work.mkdir(parents=True,exist_ok=True)
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
#include "src/dungeon.h"
#include "src/load_gfx.h"
#include "src/quality_of_life.h"
#include "src/rando/rando.h"
#include "snes/ppu_ui.h"
uint32_t Platform3DS_GetActiveProfileId(void){return 0x36cbffd5;}
static unsigned lamp_oracles;static uint32 oracle[400*240];static uint32 pixels[400*240];static uint8 before[131072];static uint16 vb[32768],ob[0x110];
static void Shot(const char *dir,int frame){
 memcpy(before,g_ram,sizeof(before));memcpy(vb,g_zenv.vram,sizeof(vb));memcpy(ob,g_zenv.ppu->oam,sizeof(ob));
 g_zenv.ppu->wideUi=PpuUiChooseSceneLayout(main_module_index,submodule_index,overworld_map_state);
 ZeldaDrawPpuFrame((uint8*)pixels,1600,g_ppu_render_flags);
 assert(!g_zenv.ppu->wideMarginRepeat);
 @LAMP@

 assert(!memcmp(before,g_ram,sizeof(before))&&!memcmp(vb,g_zenv.vram,sizeof(vb))&&!memcmp(ob,g_zenv.ppu->oam,sizeof(ob)));
 char p[1024];snprintf(p,sizeof(p),"%s/f%03d.raw",dir,frame);FILE*f=fopen(p,"wb");assert(f);fwrite(pixels,4,400*240,f);fclose(f);
}
int main(int argc,char **argv){
 assert(argc==5);assert(!chdir(argv[1]));ParseConfigFile(NULL);LoadAssets();LoadLinkGraphics();ZeldaInitialize();ZeldaSetLanguage(NULL);ZeldaReadSram();
 g_config.extended_aspect_ratio=72;g_config.extend_y=true;g_config.features0|=kFeatures0_ExtendScreen64|kFeatures0_WidescreenVisualFixes;
 g_wanted_zelda_features=g_config.features0;g_zenv.ppu->extraLeftRight=72;g_snes_width=400;g_snes_height=240;g_ppu_render_flags=kPpuRenderFlags_NewRenderer|kPpuRenderFlags_Height240;ZeldaSetWidescreenEdgeMode(1);
 assert(ZeldaLoadLatestDumpState()==kZeldaDumpStateLoaded);
 @BRIGHT@
 printf("VIDEO blank=%u brightness=%u main/sub=%x/%x features=%x dark=%u\n",g_zenv.ppu->forcedBlank,g_zenv.ppu->brightness,g_zenv.ppu->screenEnabled[0],g_zenv.ppu->screenEnabled[1],enhanced_features1,hdr_dungeon_dark_with_lantern);
 printf("LOAD room=%x module=%u/%u camera=%u/%u link=%u/%u\n",dungeon_room_index,main_module_index,submodule_index,BG2HOFS_copy2,BG2VOFS_copy2,link_x_coord,link_y_coord);
 if(atoi(argv[3])==1){
   static uint16 maps[0x2000];memcpy(before,g_ram,sizeof(before));
   for(int room=0;room<0x140;room++){
     unsigned pots=Rando_PotOverlayCount();assert(Dungeon_DecodeRoomMaps(room,maps));assert(!memcmp(before,g_ram,sizeof(before)));assert(pots==Rando_PotOverlayCount());
   }
   puts("PASS all 320 room decodes preserve live WRAM and randomizer pot overlays");
 }
 for(int f=0;f<150;f++){
  Shot(argv[2],f);
  if(f>0&&g_spotlight_ext_active&&spotlight_var1>0&&spotlight_var1<136) {
    int center=(int16)(link_y_coord-BG2VOFS_copy2+12);
    int radius=spotlight_var1+(spotlight_var2==0?8:-8);
    for(int row=0;row<240;row++)if(abs(row-center)>=radius)
      assert(g_spotlight_ext_left[row]>g_spotlight_ext_right[row]);
    if(spotlight_var2==0&&center>=224&&center<240)
      assert(g_spotlight_ext_left[239]<g_spotlight_ext_right[239]);
  }
  printf("FRAME %d room=%x mod=%u/%u/%u camera=%u/%u link=%u/%u repair=%u\n",f,dungeon_room_index,main_module_index,submodule_index,subsubmodule_index,BG2HOFS_copy2,BG2VOFS_copy2,link_x_coord,link_y_coord,ZeldaGetWideColumnRepairCount());
  ZeldaRunFrame(f>=20&&f<110?atoi(argv[4]):0);
 }
 @LAMP_DONE@
 ZeldaReset(true);g_zenv.ppu->extraLeftRight=72;
 int16 pcm[534*2];static int16 intro_pcm[400][534*2];unsigned music_at=0;
 for(int f=0;f<400;f++){
   ZeldaRunFrame(0);ZeldaRenderAudio(pcm,534,2);
   if(!music_at&&music_unk1)music_at=f+1;
   memcpy(intro_pcm[f],pcm,sizeof(pcm));
   if(!music_at)assert(!g_zenv.player->music_ptr_toplevel);

 }
 assert(music_at>10);printf("PASS reset has no music until first intro track at frame %u\n",music_at);
 ZeldaShutdownSession();
 ZeldaInitialize();
 for(int f=0;f<400;f++){ZeldaRunFrame(0);ZeldaRenderAudio(pcm,534,2);assert(!memcmp(pcm,intro_pcm[f],sizeof(pcm)));}
 puts("PASS fresh-session and reset PCM are identical across all 400 intro frames");ZeldaShutdownSession();return 0;
}
'''

code=code.replace('@LAMP@',r'''if(frame>0 && !hdr_dungeon_dark_with_lantern && dung_want_lights_out &&
      (g_zenv.ppu->screenEnabled[1]&1) && g_zenv.ppu->subtractColor) {
  uint8 sub=g_zenv.ppu->screenEnabled[1];
  g_zenv.ppu->screenEnabled[1]&=~1;
  ZeldaDrawPpuFrame((uint8*)oracle,1600,g_ppu_render_flags);
  g_zenv.ppu->screenEnabled[1]=sub;
  assert(!memcmp(pixels,oracle,sizeof(pixels)));lamp_oracles++;
 }'''if a.lamp_oracle else '')
code=code.replace('@LAMP_DONE@','assert(lamp_oracles>0);printf("PASS %u retained-lamp frames equal the independently mask-free picture\\n",lamp_oracles);'if a.lamp_oracle else '')
code=code.replace('@BRIGHT@' ,'Qol_LoadPreferences("qol.ini");Qol_SetOption(kQolBrightRooms,true);enhanced_features1|=kFeatures1_QolBrightRooms;'if a.bright_rooms else '')
if a.old_cpu:
 code=code.replace('g_ppu_render_flags=kPpuRenderFlags_NewRenderer|kPpuRenderFlags_Height240;', 'g_ppu_render_flags=kPpuRenderFlags_NewRenderer|kPpuRenderFlags_Height240|kPpuRenderFlags_Old3DS|kPpuRenderFlags_FastPaths;')
(work/'test.c').write_text(code)
sources=[str(x) for x in (root/'engine/src').glob('*.c') if x.name not in ['main.c','opengl.c','glsl_shader.c']]
if a.rtl_source:sources=[str(a.rtl_source.resolve())if Path(x).name=='zelda_rtl.c'else x for x in sources]
sources += [str(x) for x in (root/'engine/src/rando').glob('*.c')]+[str(x) for x in (root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c'),str(root/'engine/src/platform/linux/second_screen_sdl.c'),str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
with (work/'build.log').open('w') as log:
 subprocess.run(['cc','-DSDL_MAIN_HANDLED','-DSYSTEM_VOLUME_MIXER_AVAILABLE=0','-DZELDA3_TEST_NATIVE_FILE_MENU','-DZELDA3_TEST_DUMP_STATE','-O1','-g','-w','-fsanitize=address','-ffunction-sections','-fdata-sections','-Wl,-dead_strip','-I'+str(root/'engine'),'-iquote',str(root/'engine/src'),'-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),str(work/'test.c'),*sources,*sdl_flags(),'-o',str(work/'test')],stdout=log,stderr=subprocess.STDOUT,check=True)
for index,source in enumerate(a.dumps):
 data=work/f'data{index}';(data/'saves').mkdir(parents=True,exist_ok=True);dest=data/'dumps'/source.name;shutil.copytree(source,dest,dirs_exist_ok=True)
 # ARM32 Dma has four fewer trailing struct-alignment bytes than macOS ARM64.
 # Convert only the private replay copy; the source checkpoint is never edited.
 state=dest/'load-state.bin';b=bytearray(state.read_bytes())
 if struct.calcsize('P')==8:
  where=24+20+27+65536+40+3024+15+188;b[where:where]=bytes(4)
  struct.pack_into('<I',b,40,struct.unpack_from('<I',b,40)[0]+4);struct.pack_into('<I',b,16,len(b)-24)
 profile=struct.unpack_from('<I',b,12)[0]
 # The runner uses this fixture profile; preserve the input's own payload.
 struct.pack_into('<I',b,12,0x36cbffd5)
 h=2166136261
 for value in b[24:]:h=((h^value)*16777619)&0xffffffff
 struct.pack_into('<I',b,20,h);state.write_bytes(b)
 shutil.copyfile(a.assets,data/'zelda3_assets.dat');(data/'zelda3.ini').write_text('[General]\nAutosave=0\n[Sound]\nEnableAudio=0\n')
 for direction in (['left','right'] if a.direction=='both' else ['up','down'] if a.direction=='vertical' else [a.direction]):
  suffix=f'{index}-{direction}' if a.direction in ['both','vertical'] else str(index)
  frames=work/f'frames{suffix}';frames.mkdir(exist_ok=True)
  with (work/f'result{suffix}.log').open('w') as log:
   subprocess.run([str(work/'test'),str(data),str(frames),'1' if index==0 and direction!='right' else '0',str({'left':1<<6,'right':1<<7,'up':1<<4,'down':1<<5}[direction])],env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy'),stdout=log,stderr=subprocess.STDOUT,check=True)
  print('PASS',source.name,direction,': 150 rendered frames, restored state, full-height iris and empty outside-ellipse rows, no composed margin repeats; reset/session intro PCM equality')
