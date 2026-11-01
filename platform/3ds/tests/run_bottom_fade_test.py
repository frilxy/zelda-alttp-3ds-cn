#!/usr/bin/env python3
"""Check the real compositor fade with cached frames and optional RGB card art."""
from pathlib import Path
import argparse,subprocess
r=Path(__file__).resolve().parents[3];s=(r/'platform/3ds/source/platform_3ds.c').read_text()
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--rgb',type=Path);a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
def fn(sig):
 start=s.index(sig);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
static bool g_bottom_fade_ready,g_bottom_fade_active,g_gpu_frame_active,g_startup_lcd_black,g_bottom_cinema;
static int module;static uint32_t overlay;
static int SS_GetModule(void){return module;}
static uint32_t SS_GetDeathOverlay(void){return overlay;}
static uint64_t g_bottom_fade_start_ms,now;static unsigned draws,ends,last_alpha;
static uint8_t image[320*240*3],base[320*240*3];
static uint64_t osGetTime(void){return now;}
#define C2D_Color32(r,g,b,a) (((uint32_t)(a)<<24)|((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
static void C2D_DrawRectSolid(float x,float y,float z,float w,float h,uint32_t c){
 assert(x==0&&y==0&&w==320&&h==240);last_alpha=c>>24;
 for(unsigned i=0;i<sizeof(image);i++){unsigned rgb=(c>>(16-(i%3)*8))&255;image[i]=(image[i]*(255-last_alpha)+rgb*last_alpha)/255;}
}
static void Platform3DS_DrawFadeRect(uint32_t color){C2D_DrawRectSolid(0,0,.5f,320,240,color);}
'''+fn('void Platform3DS_SetBottomCinema(')+fn('static void Platform3DS_ApplyBottomFade(')+r'''
static void Platform3DS_DrawBottomTexture(void){draws++;memcpy(image,base,sizeof(image));last_alpha=0;Platform3DS_ApplyBottomFade();}
static void Platform3DS_EndGpuFrame(void){ends++;}
static void RevealInitializedScreens(void){g_startup_lcd_black=false;}
'''+fn('void Platform3DS_EndFrame(')+r'''
int main(int argc,char**argv){
 memset(base,255,sizeof(base));if(argc==3){FILE*f=fopen(argv[2],"rb");assert(f);assert(fread(base,1,sizeof(base),f)==sizeof(base));fclose(f);}
 for(int session=0;session<2;session++){
  g_bottom_cinema=false;g_bottom_fade_ready=g_bottom_fade_active=false;module=0;now=1000;
  Platform3DS_DrawBottomTexture();assert(!g_bottom_fade_active&&!memcmp(image,base,sizeof(base)));
  now+=1000;Platform3DS_SetBottomCinema(true);unsigned initial=draws;
  now=1000;
  Platform3DS_DrawBottomTexture();assert(last_alpha==255&&g_bottom_fade_active);
  unsigned previous=255;
  for(int ms=0;ms<=350;ms+=50){
   now=1000+ms;g_gpu_frame_active=true;Platform3DS_EndFrame();
   assert(last_alpha<=previous);previous=last_alpha;
   if(ms>=300)assert(!g_bottom_fade_active&&!memcmp(image,base,sizeof(base)));
   if(!session){char path[1024];snprintf(path,sizeof(path),"%s/bottom-fade-%03d.ppm",argv[1],ms);FILE*f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");fwrite(image,1,sizeof(image),f);fclose(f);}
  }
  assert(draws==initial+8); // first upload, then seven cached redraws through 300 ms
  unsigned done=draws;g_gpu_frame_active=true;now+=1000;Platform3DS_EndFrame();assert(draws==done);
 }
 Platform3DS_SetBottomCinema(false);module=0x12|(4<<8);overlay=0xffc60000;
 g_gpu_frame_active=true;Platform3DS_EndFrame();assert(image[0]==198&&image[1]==0&&image[2]==0);
 module=0x12|(5<<8);overlay=0xff000000;
 g_gpu_frame_active=true;unsigned before=draws;Platform3DS_EndFrame();assert(draws==before+1&&last_alpha==255);
 module=0x12|(6<<8);overlay=0;g_gpu_frame_active=true;Platform3DS_EndFrame();assert(last_alpha==255);
 Platform3DS_SetBottomCinema(true);now=9000;g_gpu_frame_active=true;Platform3DS_EndFrame();assert(last_alpha==255&&g_bottom_fade_active);
 module=0;now=9300;g_gpu_frame_active=true;Platform3DS_EndFrame();assert(!g_bottom_fade_active);
 puts("PASS cached Old/New bottom reveal: black first frame, monotonic 300 ms fade, opaque completion, no steady-state redraw, restart reset; synchronized red/black death and delayed card reveal");
}
'''
(out/'test.c').write_text(code);subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test'),str(out),*([str(a.rgb.resolve())]if a.rgb else[])],check=True)
assert 'g_bottom_fade_ready = g_bottom_fade_active = false;' in s
assert 'Platform3DS_ApplyBottomFade();' in fn('static void Platform3DS_DrawBottomTexture(')
