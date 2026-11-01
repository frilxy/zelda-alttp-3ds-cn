#!/usr/bin/env python3
"""Render all cataloged lateral subrooms with their own native tilesets/palettes.
Test-only entrance-table overrides and captured images stay in the work directory.
"""
from pathlib import Path
import argparse,os,subprocess,shutil,json,struct,re
from host_test_support import sdl_flags
root=Path(__file__).resolve().parents[3]
p=argparse.ArgumentParser();p.add_argument('--assets',type=Path,required=True);p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--contexts',type=Path,help='optional native entrance-context override');p.add_argument('--only',type=str,default='');p.add_argument('--bright-rooms',action='store_true');a=p.parse_args()
work=a.work_dir.resolve();assert work!=root and root not in work.parents;work.mkdir(parents=True,exist_ok=True)

def native_contexts(path):
    # Read the same aligned asset directory as LoadAssets, without extracting
    # graphics into the repository. Entrance matches take priority; dungeon
    # interiors inherit their dungeon's main graphics from a native entrance.
    data=path.read_bytes()
    count,names=struct.unpack_from('<II',data,80)
    offset=88+count*4+names
    assets=[]
    for size in struct.unpack_from('<'+str(count)+'I',data,88):
        offset=(offset+3)&~3
        assert offset+size<=len(data)
        assets.append(data[offset:offset+size]);offset+=size
    rooms=struct.unpack('<'+'H'*(len(assets[11])//2),assets[11])
    offsets=struct.unpack('<'+'H'*(len(assets[4])//2),assets[4])
    doors=(root/'engine/src/rando/door_tables.gen.c').read_text().split('kDoorTblDoors[kDoorTbl_DoorCount] = {',1)[1].split('\n};',1)[0]
    catalog={}
    for row in re.findall(r'\{([^{}]+)\}',doors):
        values=[int(x.strip(),0)for x in row.split(',')]
        assert len(values)==17
        if values[4]!=255:catalog[values[4]]=values[-1]
    ids=(root/'engine/src/rando/dungeon_ids.h').read_text()
    enums={k:int(v)for k,v in re.findall(r'(kGameDungeon_\w+)\s*=\s*(\d+)',ids)}
    body=ids.split('kGameFromRando[kRandoDungeon_Count] = {',1)[1].split('};',1)[0]
    convert=[enums[n]for n in re.findall(r'\b(kGameDungeon_\w+)\b',body)]
    body=(root/'engine/src/dungeon.c').read_text().split('kLayoutQuadrantFlags[] = {',1)[1].split('};',1)[0]
    flags=[int(v.strip(),0)for v in body.split(',')]
    result={}
    for room in range(320):
        layout=assets[3][offsets[room]+1]&28
        if not any(flags[layout|qy|qx]&(2 if qx else 1)for qx in range(2)for qy in [0,2]):continue
        entry=next((i for i,r in enumerate(rooms)if r==room),None)
        if entry is not None:
            result[str(room)]=[entry,'entrance'];continue
        if room in catalog:
            raw=convert[catalog[room]]*2
            entry=next((i for i,r in enumerate(assets[21])if r==raw),None)
            if entry is not None:result[str(room)]=[entry,'dungeon']
    assert result
    return result

contexts=json.loads(a.contexts.read_text())if a.contexts else native_contexts(a.assets)
(work/'contexts.json').write_text(json.dumps(contexts,indent=2)+'\n')
wanted=set(int(x,0)for x in a.only.split(','))if a.only else None
contexts={int(room):int(row[0])for room,row in contexts.items()if wanted is None or int(room)in wanted}
(work/'contexts.h').write_text('static const int contexts[][2]={'+','.join('{%d,%d}'%x for x in sorted(contexts.items()))+'};\n')
rtl=(root/'engine/src/zelda_rtl.c').read_text().replace('#include "spc_player.h"','// already included by main.c')
rtl=rtl.replace('static WideFrame WideFrameNow(', 'static bool test_override;static WideFrame test_wide;\nstatic WideFrame WideFrameNow(',1)
rtl=rtl.replace('WideFrame wide = BeginWideFrame(render_flags);','WideFrame wide = test_override ? test_wide : BeginWideFrame(render_flags);',1)
code=r'''
#include <assert.h>
#include <stdio.h>
#define main ZeldaUnusedUiMain
#include "src/main.c"
#undef main
@RTL@
#include "src/rando/door_tables.gen.h"
#include "contexts.h"
#include "src/quality_of_life.h"
extern const uint8 kLayoutQuadrantFlags[];
uint32_t Platform3DS_GetActiveProfileId(void){return 0x36cbffd5;}
static uint32 image[400*240],stable[400*240];static uint8 ram_before[131072];static uint16 vram_before[32768],oam_before[0x110];
static void Render(void) {
 memcpy(ram_before,g_ram,sizeof(ram_before));memcpy(vram_before,g_zenv.vram,sizeof(vram_before));memcpy(oam_before,g_zenv.ppu->oam,sizeof(oam_before));
 g_zenv.ppu->wideUi=PpuUiChooseSceneLayout(main_module_index,submodule_index,overworld_map_state);
 ZeldaDrawPpuFrame((uint8*)image,1600,g_ppu_render_flags);
 assert(!memcmp(ram_before,g_ram,sizeof(ram_before)));assert(!memcmp(vram_before,g_zenv.vram,sizeof(vram_before)));assert(!memcmp(oam_before,g_zenv.ppu->oam,sizeof(oam_before)));
}
static void Write(const char*dir,int room,int qx,int qy,int phase) {
 char path[1024];snprintf(path,sizeof(path),"%s/r%03x-q%d%d-p%d.raw",dir,room,qx,qy,phase);FILE*f=fopen(path,"wb");assert(f);fwrite(image,4,400*240,f);fclose(f);
}
typedef struct Entry {uint16 room,sx,sy,px,py,cx,cy;uint8 relative[8],q1,q2;} Entry;
static Entry Replace(int entry,int room,int qx,int qy) {
 Entry old={kEntranceData_rooms[entry],kEntranceData_scrollX[entry],kEntranceData_scrollY[entry],kEntranceData_playerX[entry],kEntranceData_playerY[entry],kEntranceData_cameraX[entry],kEntranceData_cameraY[entry],{0},kEntranceData_quadrant1[entry],kEntranceData_quadrant2[entry]};
 memcpy(old.relative,kEntranceData_relativeCoords+entry*8,8);
 int x=(room&15)*512,y=(room>>4)*512,left=x+qx*256,top=y+qy*128;
 int flags=kLayoutQuadrantFlags[(GetDungeonRoomLayout(room)[1]&28)|qx|qy];
 kEntranceData_rooms[entry]=room;kEntranceData_scrollX[entry]=left;kEntranceData_scrollY[entry]=top+16;
 kEntranceData_playerX[entry]=left+120;kEntranceData_playerY[entry]=top+152;
 kEntranceData_cameraX[entry]=qx*256+127;kEntranceData_cameraY[entry]=qy*128+136;
 uint8 rel[]={top>>8,y>>8,top>>8,(y+256)>>8,left>>8,x>>8,left>>8,(x+256)>>8};
 memcpy(kEntranceData_relativeCoords+entry*8,rel,8);
 kEntranceData_quadrant1[entry]=((flags&(qx?2:1))?0:32)|((flags&(qy?8:4))?0:2);
 kEntranceData_quadrant2[entry]=(qx<<4)|qy;return old;
}
static void Restore(int entry,Entry old){
 kEntranceData_rooms[entry]=old.room;kEntranceData_scrollX[entry]=old.sx;kEntranceData_scrollY[entry]=old.sy;kEntranceData_playerX[entry]=old.px;kEntranceData_playerY[entry]=old.py;kEntranceData_cameraX[entry]=old.cx;kEntranceData_cameraY[entry]=old.cy;
 memcpy(kEntranceData_relativeCoords+entry*8,old.relative,8);kEntranceData_quadrant1[entry]=old.q1;kEntranceData_quadrant2[entry]=old.q2;
}
int main(int argc,char**argv) {
 assert(argc==3);assert(!chdir(argv[1]));setvbuf(stdout,NULL,_IOLBF,0);ParseConfigFile(NULL);LoadAssets();LoadLinkGraphics();ZeldaInitialize();ZeldaSetLanguage(NULL);ZeldaReadSram();
 g_config.features0|=kFeatures0_ExtendScreen64|kFeatures0_WidescreenVisualFixes;g_wanted_zelda_features=g_config.features0;
 g_config.extended_aspect_ratio=72;g_config.extend_y=true;g_zenv.ppu->extraLeftRight=72;g_snes_width=400;g_snes_height=240;g_ppu_render_flags=kPpuRenderFlags_NewRenderer|kPpuRenderFlags_Height240;ZeldaSetWidescreenEdgeMode(1);
 @BRIGHT@
 assert(ZeldaLoadTestScenario(32));ByteArray base={0};SaveSnesState(&saveFunc,&base);
 unsigned rooms=0,cases=0,captures=0,different=0,guarded=0;
 for(unsigned n=0;n<sizeof(contexts)/sizeof(*contexts);n++) {
  int room=contexts[n][0],entry=contexts[n][1],layout=GetDungeonRoomLayout(room)[1]&28;rooms++;
  for(int qy=0;qy<=2;qy+=2)for(int qx=0;qx<=1;qx++) {
   int flags=kLayoutQuadrantFlags[layout|qx|qy];if(!(flags&(qx?2:1)))continue;
   LoadFuncState load={base.data,base.data+base.size};LoadSnesState(&loadFunc,&load);srm_var1=2;
   Entry old=Replace(entry,room,qx,qy);which_entrance=entry;main_module_index=6;submodule_index=subsubmodule_index=0;death_var4=death_var5=0;follower_indicator=0;link_auxiliary_state=link_incapacitated_timer=0;
   int tick=0;for(;tick<1500&&!(main_module_index==7&&submodule_index==0)&&main_module_index!=14;tick++)ZeldaRunFrame(0);
   Restore(entry,old);if((main_module_index!=7&&!(main_module_index==14&&saved_module_for_menu==7))||dungeon_room_index!=room){fprintf(stderr,"LOAD room=%03x got=%03x module=%u/%u\n",room,dungeon_room_index,main_module_index,submodule_index);return 2;}
   // Observe the loaded room before arbitrary spawn coordinates can start a
   // pit/death/tag sequence. Graphics, palette, collision and actors are native.
   main_module_index=7;submodule_index=0;INIDISP_copy=15;
   g_zenv.ppu->extraLeftRight=72;ResetWideTracking();
   test_override=false;Render();Write(argv[2],room,qx,qy,0);captures++;memcpy(stable,image,sizeof(image));
   WideFrame frame=WideFrameNow(g_ppu_render_flags,false);
   bool guarded_room=!frame.fixed_camera||WideLanternRoom(frame.context)||g_zenv.ppu->mode!=1;
   for(int phase=1;phase<=3;phase++) {
    test_override=true;test_wide=frame;test_wide.settled=false;
    test_wide.view.left=test_wide.view.right=phase*24;
    Render();Write(argv[2],room,qx,qy,phase);captures++;
    unsigned diff=0;for(unsigned px=0;px<400*240;px++)diff+=stable[px]!=image[px];different+=diff!=0;
    printf("VIEW room=%03x quadrant=%d/%d entry=%d main_gfx=%u aux_gfx=%u palette=%u phase=%d guarded=%u changed=%u\n",room,qx,qy,entry,main_tile_theme_index,aux_tile_theme_index,palette_main_indoors,phase,guarded_room,diff);
   }
   test_override=false;guarded+=guarded_room;cases++;
  }
 }
 assert(cases>0&&different==0);
 printf("PASS visual tour rooms=%u subrooms=%u captures=%u changed_views=%u guarded_subrooms=%u; native loaders and WRAM/VRAM/OAM preservation\n",rooms,cases,captures,different,guarded);
 ByteArray_Destroy(&base);ZeldaShutdownSession();return 0;
}
'''.replace('@RTL@',rtl).replace('@BRIGHT@','Qol_LoadPreferences("qol.ini");Qol_SetOption(kQolBrightRooms,true);'if a.bright_rooms else '')
(work/'test.c').write_text(code)
sources=[str(x)for x in(root/'engine/src').glob('*.c')if x.name not in ['main.c','zelda_rtl.c','opengl.c','glsl_shader.c']]
sources += [str(x)for x in(root/'engine/src/rando').glob('*.c')]+[str(x)for x in(root/'engine/snes').glob('*.c')]
sources += [str(root/'engine/third_party/sha256/sha256.c'),str(root/'engine/src/platform/linux/second_screen_sdl.c'),str(root/'engine/third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c')]
with(work/'build.log').open('w')as log:
 subprocess.run(['cc','-DSDL_MAIN_HANDLED','-DSYSTEM_VOLUME_MIXER_AVAILABLE=0','-DZELDA3_TEST_NATIVE_FILE_MENU','-O1','-g','-w','-fsanitize=address,undefined','-fno-sanitize=shift-base,alignment','-ffunction-sections','-fdata-sections','-Wl,-dead_strip','-I'+str(root/'engine'),'-iquote',str(root/'engine/src'),'-I'+str(root/'engine/third_party/opus-1.3.1-stripped'),str(work/'test.c'),*sources,*sdl_flags(),'-o',str(work/'test')],stdout=log,stderr=subprocess.STDOUT,check=True)
data=work/'data';(data/'saves').mkdir(parents=True,exist_ok=True);shutil.copyfile(a.assets,data/'zelda3_assets.dat');(data/'zelda3.ini').write_text('[General]\nAutosave=0\n[Sound]\nEnableAudio=0\n');frames=work/'frames';frames.mkdir(exist_ok=True)
with(work/'result.log').open('w')as log:r=subprocess.run([str(work/'test'),str(data),str(frames)],env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy'),stdout=log,stderr=subprocess.STDOUT)
print((work/'result.log').read_text()[-2000:]);raise SystemExit(r.returncode)
