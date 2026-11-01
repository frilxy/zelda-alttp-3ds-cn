#!/usr/bin/env python3
"""Exercise the production pre-intro reveal, retry/cancel bounds and DMA lifetime."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[3];s=(r/'platform/3ds/source/platform_3ds.c').read_text()
def fn(sig):
 a=s.index(sig);i=s.index('{',a)+1;n=1
 while n:n+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[a:i]+'\n'
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "startup_card.h"
static bool g_startup_card_pending,g_gpu_presenter_initialized,g_is_new_3ds,g_gpu_frame_active,g_bottom_fade_active;
static uint64_t now,start;static unsigned allocs,frees,syncs,waits,presents,draws,frames,black;
static bool allocated,fail_alloc;static int fail_begin,fail_present,cancel_after,g_top_target;
static uint64_t osGetTime(void){return now;}
static bool aptMainLoop(void){return cancel_after<0||(int)frames<cancel_after;}
static void*linearAlloc(unsigned n){allocs++;if(fail_alloc)return NULL;allocated=true;return calloc(1,n);}
static void C3D_FrameSync(void){assert(allocated);syncs++;}
static void linearFree(void*p){assert(allocated&&syncs);allocated=false;frees++;free(p);}
static void Platform3DS_SetBottomCinema(bool on){g_bottom_fade_active=on;start=now;}
static bool C3D_FrameBegin(int n){if(fail_begin){fail_begin--;return false;}frames++;return true;}
static void gspWaitForVBlank(void){waits++;now+=17;}
static void Platform3DS_ClearBlackTarget(int target){assert(g_gpu_frame_active);black++;}
static bool Platform3DS_PresentBottomFrame(void*p,int pitch,int w,int h){
 assert(allocated&&g_gpu_frame_active&&pitch==(g_is_new_3ds?2048:1024)&&w==320&&h==240);presents++;
 if(fail_present){fail_present--;return false;}return true;
}
static void Platform3DS_DrawBottomTexture(void){assert(allocated&&g_gpu_frame_active);draws++;}
static void Platform3DS_EndFrame(void){assert(g_gpu_frame_active);g_gpu_frame_active=false;if(now-start>=300)g_bottom_fade_active=false;}
'''+fn('void Platform3DS_RevealStartupCard(')+r'''
static void Reset(void){now=1000;start=0;allocs=frees=syncs=waits=presents=draws=frames=black=0;allocated=fail_alloc=false;fail_begin=fail_present=0;cancel_after=-1;g_startup_card_pending=g_gpu_presenter_initialized=true;g_bottom_fade_active=false;}
int main(void){
 for(int model=0;model<2;model++)for(int variant=0;variant<5;variant++){
  Reset();g_is_new_3ds=model;
  if(variant==1){fail_begin=2;fail_present=2;}
  if(variant==2)cancel_after=3;
  if(variant==3)fail_begin=10000;
  if(variant==4)fail_alloc=true;
  Platform3DS_RevealStartupCard();assert(!g_startup_card_pending&&!allocated);
  if(variant==4){assert(allocs==1&&!frees&&!syncs&&!frames);continue;}
  assert(allocs==1&&frees==1&&syncs==1&&black==frames&&now<=1816);
  if(variant<=1){assert(!g_bottom_fade_active&&draws>10&&presents==(variant==1?3:1));assert(now>=1300);}
  if(variant==2)assert(frames==3);
  if(variant==3)assert(!frames&&!presents&&waits>0);
  unsigned n=allocs;Platform3DS_RevealStartupCard();assert(allocs==n);
 }
 Reset();g_gpu_presenter_initialized=false;Platform3DS_RevealStartupCard();assert(!allocs&&g_startup_card_pending);
 puts("PASS Old/New pre-intro reveal: black top, one card upload, cached fade before return, retries, one-shot guard, 800 ms bound, cancellation/allocation failure, GPU fence before free");
}
'''
# The card reveal precedes any native logic frame or video presentation.
m=(r/'engine/src/main.c').read_text();a=m.index('Platform3DS_RevealStartupCard();');b=m.index('bool running = true;',a);assert a<b
assert 'StartupCardPixels(pixels,g_is_new_3ds)' in s
assert 'Platform3DS_ShowOperationProgress(false' not in s
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(r/'engine'),'-I'+str(r/'platform/3ds/source'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
