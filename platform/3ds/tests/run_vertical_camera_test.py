#!/usr/bin/env python3
"""Run the real WIDE view (240-line camera, fixed camera, side bands) on
boundaries, transitions and supplied checkpoints; compare both CPU paths."""
from pathlib import Path
import argparse,subprocess,tempfile
r=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--dumps',nargs='*',type=Path,default=[]);p.add_argument('--output',type=Path);a=p.parse_args()
s=(r/'engine/src/zelda_rtl.c').read_text();model=(r/'platform/3ds/tests/ppu_gpu_model_test.c').read_text()
code=model[:model.index('int main(')]+'''
#include "src/features.h"
#include "src/variables.h"
#include "src/wide_camera.h"
uint8 g_ram[131072];
uint64_t svcGetSystemTick(void){return 0;}
static struct {Ppu *ppu;} g_zenv;
static int g_widescreen_edge_mode;
static uint16 map_words[16];
#define kMap16ToMap8 map_words
#define kMap16ToMap8_SIZE sizeof(map_words)
static const uint16 *GetMap16toMap8Table(void){return map_words;}
static bool Overworld_DecodeScreenMap16(unsigned screen, uint16 *out) { (void)screen; memset(out,0,4096*sizeof(*out)); return false; }
static bool Dungeon_DecodeRoomMaps(uint16 room,uint16 *maps){return false;}
static bool IsDungeonMapMenuActive(void){return saved_module_for_menu==7&&WideCamera_DungeonMapPicture(main_module_index,submodule_index,overworld_map_state);}
'''
start=s.index('static uint8 GetFixedCameraEffectiveContext(void) {');end=s.index('static void ZeldaDrawPpuLines(',start)
code+=s[start:end]+r'''
static uint32_t cpu[2][512*240];
static void Visible(Ppu *p){p->forcedBlank=false;p->brightness=15;}
// One frame of the view: its rows and the scroll it draws with, restored.
static WideFrame Frame(Ppu *p,unsigned flags) {
 p->bgLayer[0].vScroll=800;p->bgLayer[1].vScroll=500;p->renderObjYOffset=0;
 uint8 saved[131072];memcpy(saved,g_ram,sizeof(saved));
 // Before its sprites, the game takes the rows of this picture: the same,
 // the tracking of the last one kept.
 WideViewTracker tracked=g_wide_view;g_wide_render_flags=flags;ZeldaWideBeforeSprites();
 assert(!memcmp(&tracked,&g_wide_view,sizeof(tracked)));int sprite_rows=g_wide_picture_rows;
 WideFrame f=BeginWideFrame(flags);assert(sprite_rows==WideSpriteRows(&f));
 WideRenderState state=BeginWideRender(&f);
 int dy=f.rows?f.view.delta_y:0;
 assert(p->bgLayer[1].vScroll==((500-dy)&0x3ff) && p->renderObjYOffset==dy);
 assert(!memcmp(saved,g_ram,sizeof(saved)));EndWideRender(&state);
 assert(p->bgLayer[1].vScroll==500 && p->renderObjYOffset==0);
 return f;
}
int main(int argc,char **argv) {
 Ppu *p=ppu_init();g_zenv.ppu=p;Visible(p);
 enhanced_features0=kFeatures0_WidescreenVisualFixes;
 // Continuous and wrapped lower bounds, both scene types, no logic writes.
 unsigned cases=0;
 for(int context=7;context<=9;context+=2)for(int wrap=0;wrap<2;wrap++)for(int spare=32;spare>=0;spare--) {
  main_module_index=context;submodule_index=0;quadrant_fullsize_y=0;
  uint16 bottom=wrap?8:500;BG2VOFS_copy2=bottom-spare;
  if(context==7)room_bounds_y.v[2]=bottom;else ow_scroll_vars0.yend=bottom;
  ResetWideTracking();
  WideFrame f=Frame(p,5);
  assert(f.view.delta_y==(spare<16?16-spare:0));cases++;
 }
 ResetWideTracking();assert(!Frame(p,1).context);
 main_module_index=14;submodule_index=7;assert(!Frame(p,5).context);
 // Down an overworld screen edge: the rows above go from all 16 to none
 // with the scroll, instead of all at once when the bounds change.
 memset(g_ram,0,sizeof(g_ram));enhanced_features0=kFeatures0_WidescreenVisualFixes;
 main_module_index=9;ow_scroll_vars0.ystart=0x400;ow_scroll_vars0.yend=0x51e;BG2VOFS_copy2=0x51e;
 ResetWideTracking();assert(Frame(p,5).view.delta_y==16);
 submodule_index=6;BYTE(overworld_screen_trans_dir_bits)=4;up_down_scroll_target_end=0x600;
 overworld_offset_base_y=0x600;BYTE(overworld_area_is_big)=1;
 int last=16;
 for(int y=0x520;y<=0x600;y+=8) {
  BG2VOFS_copy2=y;int dy=Frame(p,5).view.delta_y;
  assert(dy<=last && last-dy<=1);assert(dy==WideCamera_Lerp(16,0,(y-0x51e)*256/(0x600-0x51e)));last=dy;cases++;
 }
 assert(last==0);
 ow_scroll_vars0.ystart=0x600;ow_scroll_vars0.yend=0x91e;submodule_index=7;assert(Frame(p,5).view.delta_y==0);
 submodule_index=0;BYTE(overworld_screen_trans_dir_bits)=0;assert(Frame(p,5).view.delta_y==0);
 // And up: none to 16, the destination's bottom.
 ow_scroll_vars0.ystart=0xa00;ow_scroll_vars0.yend=0xb1e;BG2VOFS_copy2=0xa00;
 ResetWideTracking();assert(Frame(p,5).view.delta_y==0);
 submodule_index=6;BYTE(overworld_screen_trans_dir_bits)=8;up_down_scroll_target=0x920;
 overworld_offset_base_y=0x600;last=0;
 for(int y=0x9f8;y>=0x920;y-=8) {
  BG2VOFS_copy2=y;int dy=Frame(p,5).view.delta_y;assert(dy>=last && dy-last<=1);last=dy;cases++;
 }
 assert(last==16);
 BG2VOFS_copy2=0x91e;ow_scroll_vars0.ystart=0x600;ow_scroll_vars0.yend=0x91e;submodule_index=7;assert(Frame(p,5).view.delta_y==16);
 // A bounds change without a scroll (leaving a house, a room swapping its
 // bounds): a pixel a frame, plus what the camera's own move explains.
 memset(g_ram,0,sizeof(g_ram));enhanced_features0=kFeatures0_WidescreenVisualFixes;
 main_module_index=7;player_is_indoors=1;room_bounds_y.v[2]=0x310;BG2VOFS_copy2=0x310;
 ResetWideTracking();assert(Frame(p,5).view.delta_y==16);
 main_module_index=15;room_bounds_y.v[2]=0x510;BG2VOFS_copy2=0x312;
 assert(Frame(p,5).view.delta_y==15);
 for(int i=14;i>=0;i--)assert(Frame(p,5).view.delta_y==i);
 // Nothing links two frames: a new place, a dark screen.
 room_bounds_y.v[2]=0x310;BG2VOFS_copy2=0x310;assert(Frame(p,5).view.delta_y==1);
 BG2VOFS_copy2=0x1310;room_bounds_y.v[2]=0x1310;assert(Frame(p,5).view.delta_y==16);
 room_bounds_y.v[2]=0x1600;p->forcedBlank=true;assert(Frame(p,5).view.delta_y==0);Visible(p);
 // Fixed camera across a dungeon door between regions of different widths:
 // drawn camera and side bands follow the scroll.
 memset(g_ram,0,sizeof(g_ram));enhanced_features0=kFeatures0_WidescreenVisualFixes;
 g_widescreen_edge_mode=1;p->extraLeftRight=72;
 main_module_index=7;player_is_indoors=1;quadrant_fullsize_x=2;quadrant_fullsize_y=0;
 room_bounds_x.v[1]=0x200;room_bounds_x.v[3]=0x300;room_bounds_y.v[2]=0x1110;
 BG2HOFS_copy2=0x200;BG2VOFS_copy2=0x1000;ResetWideTracking();
 WideFrame f=Frame(p,5);assert(f.view.offset_x==-72 && f.view.left==72 && f.view.right==72);
 submodule_index=2;overworld_screen_transition=1;up_down_scroll_target_end=0x110;
 room_bounds_x.v[3]=0x200;room_bounds_y.v[2]=0xf10;
 WideFrame prev=Frame(p,5);assert(prev.view.offset_x==-72 && prev.view.delta_y==0);
 for(int y=0xffc;y>=0xf10;y-=4) {
  BG2VOFS_copy2=y;f=Frame(p,5);
  assert(abs(f.view.offset_x-prev.view.offset_x)<=2 && abs(f.view.left-prev.view.left)<=2 && abs(f.view.delta_y-prev.view.delta_y)<=1);
  prev=f;cases++;
 }
 assert(f.view.offset_x==0 && f.view.left==0 && f.view.right==0 && f.view.delta_y==16);
 submodule_index=0;assert(Frame(p,5).settled);
 g_widescreen_edge_mode=0;p->extraLeftRight=0;
 for(int arg=2;arg<argc;arg++) {
  char path[2048];snprintf(path,sizeof(path),"%s/ram.bin",argv[arg]);
  FILE *fp=fopen(path,"rb");assert(fp);assert(fread(g_ram,1,sizeof(g_ram),fp)==sizeof(g_ram));fclose(fp);
  if(main_module_index==14 && submodule_index==7 && overworld_map_state==0) {
   LoadDump(p,argv[arg]);p->extraLeftRight=72;g_widescreen_edge_mode=1;
   main_module_index=9;ResetWideTracking();WideFrame before=Frame(p,5);
   main_module_index=14;
   for(int brightness=15;brightness>0;brightness--) {
    p->brightness=brightness;WideFrame fade=Frame(p,5);
    assert(fade.context==before.context && fade.view.offset_x==before.view.offset_x);
    assert(fade.view.delta_y==before.view.delta_y && fade.view.left==before.view.left && fade.view.right==before.view.right);
   }
   overworld_map_state=1;p->mode=7;assert(!Frame(p,5).context);
   overworld_map_state=0;g_widescreen_edge_mode=0;
   printf("PASS supplied world-map fade: no camera/side-space jump across all 15 brightness levels\n");
  }
  unsigned delta=0;
  for(int old=0;old<2;old++) {
   LoadDump(p,argv[arg]);p->windowExtLeft=p->windowExtRight=NULL;
   ResetWideTracking();
   WideFrame frame=BeginWideFrame(5|(old?16:0));delta=frame.view.delta_y;
   PpuBeginDrawing(p,(uint8*)cpu[old],2048,5|(old?16:0));
   WideRenderState state=BeginWideRender(&frame);
   ConfigurePpuSideSpace(&frame);
   assert(p->extraBottomCur==16);
   for(int y=1;y<=240;y++)ppu_runLine(p,y);
   EndWideRender(&state);
  }
  assert(!memcmp(cpu[0],cpu[1],sizeof(cpu[0])));
  if(strcmp(argv[1],"-")) {
   snprintf(path,sizeof(path),"%s/%d.ppm",argv[1],arg-2);FILE *fo=fopen(path,"wb");assert(fo);
   fprintf(fo,"P6\n400 240\n255\n");
   for(int y=0;y<240;y++)for(int x=0;x<400;x++){uint32 c=cpu[0][y*512+x];uint8 rgb[3]={c>>16,c>>8,c};fwrite(rgb,1,3,fo);}fclose(fo);
  }
  printf("PASS camera checkpoint %s: vertical delta=%u, full240, CPU Old/New parity\n",argv[arg],delta);
 }
 ppu_free(p);printf("PASS %u camera cases: boundaries and wraps, both vertical overworld transitions, a bounds change, cuts, a dungeon door between regions, Original and map exclusions\n",cases);
}
'''
with tempfile.TemporaryDirectory() as t:
 d=Path(t);(d/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-D__3DS__','-fsanitize=address,undefined','-fno-sanitize=shift-base','-fno-sanitize-recover=all','-I'+str(r/'engine'),'-I'+str(r/'platform/3ds/source'),str(d/'test.c'),str(r/'engine/snes/ppu.c'),str(r/'engine/src/wide_camera.c'),str(r/'platform/3ds/source/ppu_gpu_model.c'),'-o',str(d/'test')],check=True)
 if a.output:a.output.mkdir(parents=True,exist_ok=True)
 subprocess.run([str(d/'test'),str(a.output.resolve()) if a.output else '-',*[str(p.resolve()) for p in a.dumps]],check=True)
