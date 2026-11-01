#!/usr/bin/env python3
"""Check visible neighboring WIDE geometry against native room maps across cartridge room layouts.
This is a host geometry/state regression, not a playthrough or console FPS test.
"""
from pathlib import Path
import argparse,os,subprocess,shutil
from host_test_support import sdl_flags
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--assets',type=Path,required=True);p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--rtl-source',type=Path);a=p.parse_args()
work=a.work_dir.resolve();assert work!=root and root not in work.parents;work.mkdir(parents=True,exist_ok=True)
code=r'''
#include <assert.h>
#include <stdio.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
@RTL_BODY@
extern const uint8 kLayoutQuadrantFlags[];
uint32_t Platform3DS_GetActiveProfileId(void){return 0x36cbffd5;}
static uint8 initial[131072],saved[131072];
static uint16 maps[0x2000],original[32768],oam[0x110];
static unsigned closed_cases,open_cases,frames,words,actors;
static bool Open(const uint16 *m,int y0,int y1) {
 int run=0;
 for(int y=y0;y<y1;y+=8) {
  if(attributes_for_tile[m[(y>>3)*64+31]&1023]==0&&attributes_for_tile[m[(y>>3)*64+32]&1023]==0){if(++run>=4)return true;}
  else run=0;
 }
 return false;
}
static void Layers(Ppu*p,int x,int y) {
 p->mode=1;p->brightness=15;p->forcedBlank=false;p->extraLeftRight=72;
 p->screenEnabled[0]=3|16;p->screenEnabled[1]=0;
 for(int layer=0;layer<2;layer++) {
  BgLayer*b=&p->bgLayer[layer];b->tilemapWider=b->tilemapHigher=true;
  b->hScroll=x&1023;b->vScroll=y&1023;b->tilemapAdr=layer?0:0x1000;b->tileAdr=0x2000;
  for(int ty=0;ty<512;ty+=8)for(int tx=0;tx<512;tx+=8)
   p->vram[WideMapAddress(b,tx,ty)]=maps[(layer?0:4096)+(ty>>3)*64+(tx>>3)];
 }
 for(int i=0;i<128;i++)p->oam[i*2]=0xf000;
 memset(p->oam+0x100,0,32);
}
static void AuditIris(void) {
 unsigned samples=0;
 for(int center=0;center<240;center++)for(int radius=16;radius<=128;radius+=8)for(int opening=0;opening<2;opening++) {
  memcpy(g_ram,initial,sizeof(initial));enhanced_features0|=kFeatures0_WidescreenVisualFixes;
  link_y_coord=center-12;BG2VOFS_copy2=0;link_x_coord=128;BG2HOFS_copy2=0;
  spotlight_var1=radius;spotlight_var2=opening?2:0;IrisSpotlight_ConfigureTable();
  for(int row=0;row<240;row++) {
   if(abs(row-center)>=radius){assert(g_spotlight_ext_left[row]>g_spotlight_ext_right[row]);if(row>=224)assert(hdma_table_dynamic[row]==0x00ff);}
   if(row==center)assert(g_spotlight_ext_left[row]<g_spotlight_ext_right[row]);
   int mirror=2*center-row;
   if(mirror>=0&&mirror<240){assert(g_spotlight_ext_left[row]==g_spotlight_ext_left[mirror]);assert(g_spotlight_ext_right[row]==g_spotlight_ext_right[mirror]);}
  }
  if(center>=224)assert(g_spotlight_ext_left[239]<g_spotlight_ext_right[239]||abs(239-center)>=radius);
  samples++;
 }
 memcpy(g_ram,initial,sizeof(initial));ResetWideTracking();
 printf("PASS %u opening/closing iris cases: every vertical center, 15 radii, 240 symmetric mask rows, empty outside ellipse and live lower aperture\n",samples);
}
static unsigned private_fade_cases,private_fade_open;
static void AuditPrivateFade(void) {
 for(int phase=0;phase<=2;phase+=2)for(int t=0;t<4;t++) {
  memcpy(g_ram,initial,sizeof(initial));memcpy(g_ram+0x2000,maps,sizeof(maps));
  ResetWideTracking();ZeldaSetWidescreenEdgeMode(1);g_zenv.ppu->extraLeftRight=72;
  enhanced_features0|=kFeatures0_WidescreenVisualFixes;
  main_module_index=7;submodule_index=1;subsubmodule_index=3;
  player_is_indoors=1;cur_palace_index_x2=0xff;overworld_screen_transition=t;
  int direction=t==0||t==2?1:-1,axis=t>=2?1:2,source=1024+phase,end=source+direction*256;
  BG2HOFS_copy2=BG2VOFS_copy2=source;
  left_right_scroll_target=left_right_scroll_target_end=up_down_scroll_target=up_down_scroll_target_end=end&0x1fc;
  bool open=false;int run=0;
  for(int i=0;i<64;i++) {
   int a=axis==1?i*64+31:31*64+i,b=axis==1?i*64+32:32*64+i;
   if(attributes_for_tile[maps[a]&1023]==0&&attributes_for_tile[maps[b]&1023]==0){if(++run>=4){open=true;break;}}else run=0;
  }
  memcpy(saved,g_ram,sizeof(saved));UpdatePrivateRoomFade();assert(!memcmp(saved,g_ram,sizeof(saved)));
  assert(g_private_fade.active==!open);
  if(open){private_fade_open++;continue;}
  unsigned previous=15;
  for(int moved=0;moved<=256;moved+=4) {
   if(axis==1)BG2HOFS_copy2=source+direction*moved;else BG2VOFS_copy2=source+direction*moved;
   subsubmodule_index=moved==256?4:3;UpdatePrivateRoomFade();
   if(!(g_private_fade.active&&g_private_fade.source==source&&g_private_fade.light<=previous)){fprintf(stderr,"FADE t=%d moved=%d got=%d expected=%d active=%d light=%u previous=%u end=%d\n",t,moved,g_private_fade.source,source,g_private_fade.active,g_private_fade.light,previous,g_private_fade.end);abort();}
   if(moved>=128)assert(!g_private_fade.light);previous=g_private_fade.light;
   WideFrame f=WideFrameNow(kPpuRenderFlags_NewRenderer|kPpuRenderFlags_Height240,false);
   assert(axis==1?(int)BG2HOFS_copy2-f.view.offset_x==source:(int)BG2VOFS_copy2-f.view.delta_y==source);
  }
  // The native landing finishes while black. Reveal the destination once.
  submodule_index=0;frame_counter=240;UpdatePrivateRoomFade();assert(g_private_fade.reveal&&!g_private_fade.light);
  for(int age=0;age<=18;age++){frame_counter=(uint8)(240+age);UpdatePrivateRoomFade();assert(g_private_fade.light>=previous);previous=g_private_fade.light;}
  assert(!g_private_fade.active&&!g_private_fade.reveal&&g_private_fade.light==15);
  // Ordinary dungeons, native aspect, and disabled visual fixes retain their
  // own transition. No presentation state writes gameplay RAM.
  for(int guard=0;guard<3;guard++) {
   ResetWideTracking();submodule_index=1;subsubmodule_index=3;BG2HOFS_copy2=BG2VOFS_copy2=source;
   cur_palace_index_x2=guard==0?0:0xff;g_zenv.ppu->extraLeftRight=guard==1?0:72;
   enhanced_features0=guard==2?0:kFeatures0_WidescreenVisualFixes;
   UpdatePrivateRoomFade();assert(!g_private_fade.active&&!g_private_fade.reveal);
  }
  private_fade_cases++;
 }
}
static void Case(int room,int qx,int qy,bool previous) {
 memcpy(g_ram,initial,sizeof(initial));memcpy(g_ram+0x2000,maps,sizeof(maps));
 int base=(room&15)*512,by=(room>>4)*512;
 int left=base+qx*256,right=left+256,y=by+qy*128+8;
 main_module_index=7;submodule_index=0;player_is_indoors=1;dungeon_room_index=room;
 quadrant_fullsize_x=0;quadrant_fullsize_y=0;
 room_bounds_x=(RoomBounds){{left,base,left,base+256}};
 room_bounds_y=(RoomBounds){{by+qy*128,by,by+qy*128+16,by+272}};
 BG2HOFS_copy2=BG1HOFS_copy2=left;BG2VOFS_copy2=BG1VOFS_copy2=y;
 hdr_dungeon_dark_with_lantern=0;TS_copy=0;
 Ppu*p=g_zenv.ppu;Layers(p,left,y);ResetWideTracking();ZeldaWideAfterMapLoad(7);
 bool open=Open(maps,(y-by)&~7,((y-by)+224)&~7);
 if(previous) {
  g_wide_prev=g_wide_live;memcpy(g_wide_prev_maps,maps,sizeof(maps));
  int direction=qx?1:-1;int other=room+direction;
  if(other<0||other>=320||(other>>4)!=(room>>4))return;
  dungeon_room_index=other;
  int ox=(other&15)*512;room_bounds_x=(RoomBounds){{ox,ox,ox+256,ox+256}};quadrant_fullsize_x=2;
  g_wide_live=WideRegionFromGame(7);InvalidateWideRoomVoid();
 }
 WideFrame f={.context=7,.fixed_camera=true,.rows=false,.settled=true};
 memcpy(original,p->vram,sizeof(original));memcpy(saved,g_ram,sizeof(saved));
 BeginWideTileRepair(&f);BeginWideRoomVoid(&f);assert(g_wide_void.active);
 // Neighbors remain visible even behind doors. The decoded maps are the
 // independent oracle for settled and animated margins.
 int start=qx?left-72:right;
 for(int layer=0;layer<2;layer++)for(int row=0;row<28;row++)for(int col=0;col<9;col++)
  assert(p->vram[WideMapAddress(&p->bgLayer[layer],start+col*8,y+row*8)]==
    maps[(layer?0:4096)+((y+row*8-by)>>3)*64+((start+col*8-base)>>3)]);
 EndWideTileRepair();assert(!memcmp(original,p->vram,sizeof(original)));assert(!memcmp(saved,g_ram,sizeof(saved)));
 for(int margin=0;margin<=72;margin+=8) {
  main_module_index=7;submodule_index=previous?2:1;
  f.settled=false;f.view.left=f.view.right=margin;
  // One actor remains in the native picture, another is entirely beyond the
  // internal edge. Both stay visible; rendering must not persist state.
  p->oam[0]=64*256+80;p->oam[1]=4;
  int sx=qx?512-16:256+8;
  p->oam[2]=64*256+(sx&255);p->oam[3]=4;p->oam[0x100]=(sx>>8)<<2;
  memcpy(oam,p->oam,sizeof(oam));memcpy(saved,g_ram,sizeof(saved));
  BeginWideTileRepair(&f);BeginWideRoomVoid(&f);assert(g_wide_void.active);
  for(int layer=0;layer<2;layer++)for(int row=0;row<28;row++)for(int col=0;col<9;col++) {
   int tx=start+col*8,ty=y+row*8;unsigned adr=WideMapAddress(&p->bgLayer[layer],tx,ty);
   unsigned want=maps[(layer?0:4096)+((ty-by)>>3)*64+((tx-base)>>3)];
   if(p->vram[adr]!=want){fprintf(stderr,"ROOF room=%03x quadrant=%d/%d previous=%d margin=%d open=%d layer=%d at=%d/%d got=%04x want=%04x\n",room,qx,qy,previous,margin,open,layer,tx,ty,p->vram[adr],want);abort();}
   words++;
  }
  assert(p->oam[0]==oam[0]);
  assert(p->oam[2]==oam[2]);actors++;
  EndWideTileRepair();assert(!memcmp(original,p->vram,sizeof(original)));assert(!memcmp(oam,p->oam,sizeof(oam)));assert(!memcmp(saved,g_ram,sizeof(saved)));frames++;
 }
 if(!previous)for(int phase=0;phase<8;phase++) {
  int sample_y=y+phase,tail=(sample_y+224)&~7;
  if(tail-by>=512)continue;
  Layers(p,left,sample_y);BG2VOFS_copy2=BG1VOFS_copy2=sample_y;
  f.settled=false;f.view.left=f.view.right=0;
  // The native streamer need not populate the final WIDE scanline. Poison
  // its tile cells so a missed inclusive y+1 sample cannot accidentally pass.
  for(int layer=0;layer<2;layer++)for(int col=0;col<9;col++)
    p->vram[WideMapAddress(&p->bgLayer[layer],start+col*8,tail)]=0xdead;
  BeginWideTileRepair(&f);BeginWideRoomVoid(&f);
  for(int layer=0;layer<2;layer++)for(int col=0;col<9;col++)
    assert(p->vram[WideMapAddress(&p->bgLayer[layer],start+col*8,tail)]==maps[(layer?0:4096)+((tail-by)>>3)*64+((start+col*8-base)>>3)]);
  EndWideTileRepair();
 }
 if(!previous) {
  uint16 reference[18];int side_start=qx?base+512:base-72;
  if((qx&&(room&15)<15)||(!qx&&(room&15)>0))for(int offset=0;offset<=128;offset+=8) {
   int sample_y=by+8+offset;Layers(p,left,sample_y);BG2VOFS_copy2=BG1VOFS_copy2=sample_y;
   f.settled=false;f.view.left=f.view.right=0;
   BeginWideTileRepair(&f);BeginWideRoomVoid(&f);
   for(int layer=0;layer<2;layer++)for(int col=0;col<9;col++) {
    uint16 word=p->vram[WideMapAddress(&p->bgLayer[layer],side_start+col*8,by+192)];
    if(!offset)reference[layer*9+col]=word;
    else assert(reference[layer*9+col]==word);
   }
   EndWideTileRepair();
  }
 }
 if(open)open_cases++;else closed_cases++;
}
int main(int argc,char**argv) {
 assert(argc==2);assert(!chdir(argv[1]));ParseConfigFile(NULL);LoadAssets();LoadLinkGraphics();ZeldaInitialize();ZeldaSetLanguage(NULL);ZeldaReadSram();
 g_config.features0|=kFeatures0_ExtendScreen64|kFeatures0_WidescreenVisualFixes;g_wanted_zelda_features=g_config.features0;
 assert(ZeldaLoadTestScenario(6));for(int f=0;f<30;f++)ZeldaRunFrame(0);
 memcpy(initial,g_ram,sizeof(initial));AuditIris();
 for(int room=0;room<320;room++) {
  assert(Dungeon_DecodeRoomMaps(room,maps));AuditPrivateFade();
  int layout=GetDungeonRoomLayout(room)[1]&0x1c;
  for(int qy=0;qy<=2;qy+=2)for(int qx=0;qx<=1;qx++) {
   int flags=kLayoutQuadrantFlags[layout|qy|qx];
   if(!(flags&(qx?2:1)))continue; // original camera has no closed half-room here
   Case(room,qx,qy,false);Case(room,qx,qy,true);
  }
 }
 printf("PASS 320 layouts: %u closed and %u open half-room cases, %u moving frames, %u roof/floor words, %u visible neighboring actors; native actor and WRAM/VRAM/OAM preserved\n",closed_cases,open_cases,frames,words,actors);
 printf("PASS private seam presentation: %u closed and %u continuous direction/layout cases, monotonic fade and frozen source through native landing, 18-tick destination reveal, wrap, aspect/dungeon/feature guards\n",private_fade_cases,private_fade_open);
 assert(closed_cases>100&&open_cases>0);ZeldaShutdownSession();return 0;
}
'''.replace('@RTL_BODY@',(a.rtl_source or root/'engine/src/zelda_rtl.c').read_text().replace('#include "spc_player.h"','// Unguarded SPC declarations were already included by main.c.'))
(work/'test.c').write_text(code)
sources=[str(x)for x in(root/'engine/src').glob('*.c')if x.name not in ['main.c','zelda_rtl.c','opengl.c','glsl_shader.c']]
sources += [str(x)for x in(root/'engine/src/rando').glob('*.c')]+[str(x)for x in(root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c'),str(root/'engine/src/platform/linux/second_screen_sdl.c'),str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
with(work/'build.log').open('w')as log:
 subprocess.run(['cc','-DSDL_MAIN_HANDLED','-DSYSTEM_VOLUME_MIXER_AVAILABLE=0','-DZELDA3_TEST_NATIVE_FILE_MENU','-O1','-g','-w','-fsanitize=address,undefined','-fno-sanitize=shift-base,alignment','-ffunction-sections','-fdata-sections','-Wl,-dead_strip','-I'+str(root/'engine'),'-iquote',str(root/'engine/src'),'-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),str(work/'test.c'),*sources,*sdl_flags(),'-o',str(work/'test')],stdout=log,stderr=subprocess.STDOUT,check=True)
data=work/'data';(data/'saves').mkdir(parents=True,exist_ok=True);shutil.copyfile(a.assets,data/'zelda3_assets.dat');(data/'zelda3.ini').write_text('[General]\nAutosave=0\n[Sound]\nEnableAudio=0\n')
r=subprocess.run([str(work/'test'),str(data)],env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy'),text=True,capture_output=True);(work/'result.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);raise SystemExit(r.returncode)
