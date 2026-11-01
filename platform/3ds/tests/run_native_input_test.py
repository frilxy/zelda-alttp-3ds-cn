#!/usr/bin/env python3
"""Exercise actual native input after SDL consumes HID release edges."""
from pathlib import Path
import subprocess,tempfile,os
r=Path(__file__).resolve().parents[3]
s=Path(os.environ.get('INPUT_SOURCE',r/'platform/3ds/source/platform_3ds.c')).read_text()
a=s.index('uint16_t Platform3DS_ReadInput(');b=s.index('\nstatic char *Trim',a)
code=r'''
#include <stdint.h>
#ifndef ZELDA3_ENABLE_DEVELOPER_TESTS
#define ZELDA3_ENABLE_DEVELOPER_TESTS 1
#endif
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t u32;
typedef struct {int dx,dy;} circlePosition;
enum {KEY_X=1,KEY_Y=2,KEY_A=4,KEY_B=8,KEY_L=16,KEY_R=32,
KEY_ZL=64,KEY_ZR=128,KEY_DUP=256,KEY_DDOWN=512,KEY_DLEFT=1024,
KEY_DRIGHT=2048,KEY_SELECT=4096,KEY_START=8192};
static u32 physical,scanned,up;
static uint64_t now;
static bool g_is_new_3ds,g_quick_dump_requested;
static int g_turbo_multiplier=5,overlays,control,resets;
enum {kTurboControlAuto,kTurboControlX,kTurboControlZL,kTurboControlCStick};
static int Platform3DS_GetTurboControl(void) { return !g_is_new_3ds ? kTurboControlX : control ? control : kTurboControlCStick; }
static void Platform3DS_RequestReset(void){resets++;}
static bool stick;
static int next_tests;static void SS_RequestNextTestScenario(void){next_tests++;}

static void hidScanInput(void){up=scanned&~physical;scanned=physical;}
static u32 hidKeysHeld(void){return scanned;}
static u32 hidKeysUp(void){return up;}
static uint64_t osGetTime(void){return now;}
static void hidCircleRead(circlePosition *p){p->dx=p->dy=0;}
static bool CStickIsHeld(u32 keys){return stick;}
static void SecondScreenSDL_OpenDeveloperOverlay(void){overlays++;}
static uint16_t SecondScreenSDL_FilterButtons(uint16_t input){return input;}
FUNCTION
static void poll(u32 keys,uint64_t time,uint16_t expected,bool turbo) {
 physical=keys;now=time;hidScanInput(); // SDL N3DS_PumpEvents before ReadInput
 bool t;int m;uint16_t got=Platform3DS_ReadInput(&t,&m);
 assert(got==expected);assert(t==turbo);assert(m==(g_turbo_multiplier?g_turbo_multiplier:1));
}
int main(void) {
 // Tap on release, including time zero and repeated SDL pumps while waiting.
 poll(KEY_X,0,0,false);poll(KEY_X,33,0,false);
 physical=0;hidScanInput();hidScanInput();poll(0,66,512,false);poll(0,100,0,false);
 for(int i=0;i<100;i++) {uint64_t t=200+i*1100;
  poll(KEY_X,t,0,false);poll(0,t+999,512,false);poll(0,t+1000,0,false);
 }
 // Exact threshold; holding and release never leak X into game logic.
 poll(KEY_X,120000,0,false);poll(KEY_X,120999,0,false);
 poll(KEY_X,121000,0,true);poll(KEY_X,123000,0,true);poll(0,123033,0,false);
 // No intermediate poll at the threshold: still not a short tap.
 poll(KEY_X,124000,0,false);poll(0,125000,0,false);
 // Other buttons work during the hold; quick dump stays a single edge.
 poll(KEY_X|KEY_A,126000,256,false);poll(KEY_A,126050,768,false);poll(0,126100,0,false);
 poll(KEY_L|KEY_R|KEY_A,127000,3072,false);assert(g_quick_dump_requested);
 g_quick_dump_requested=false;poll(KEY_L|KEY_R|KEY_A,127033,3072,false);assert(!g_quick_dump_requested);poll(0,127100,0,false);
#if ZELDA3_ENABLE_DEVELOPER_TESTS
 // B held + each L rising edge requests exactly one next test; hold does not repeat.
 poll(KEY_B,127200,1,false);poll(KEY_B|KEY_L,127233,0,false);assert(next_tests==1);
 poll(KEY_B|KEY_L,127266,0,false);assert(next_tests==1);
 poll(KEY_B,127300,1,false);poll(KEY_B|KEY_L,127333,0,false);assert(next_tests==2);
 poll(0,127366,0,false);poll(KEY_L|KEY_R|KEY_B,127400,0,false);assert(next_tests==2);poll(0,127433,0,false);
#else
 poll(KEY_B,127200,1,false);poll(KEY_B|KEY_L,127233,1025,false);
 poll(KEY_B|KEY_L,127266,1025,false);assert(next_tests==0);poll(0,127433,0,false);
#endif
 // Disabling turbo gives normal immediate X, including long presses.
 g_turbo_multiplier=0;poll(KEY_X,128000,512,false);poll(KEY_X,130000,512,false);poll(0,130033,0,false);
 // New retains immediate X plus independent shoulder turbo.
 g_turbo_multiplier=5;g_is_new_3ds=true;
 // Default New control is C-stick; neither shoulder is an implicit trigger.
 poll(KEY_X,131000,512,false);poll(KEY_X|KEY_ZL,133000,512,false);poll(0,133033,0,false);
 stick=true;poll(0,134000,0,true);stick=false;poll(0,134100,0,false);
 control=kTurboControlZL;poll(KEY_ZL,135000,0,true);poll(KEY_ZR,135100,0,false);
 stick=true;poll(0,135200,0,false);stick=false;
 control=kTurboControlX;poll(KEY_X,136000,0,false);poll(KEY_X,137000,0,true);poll(0,137100,0,false);
 control=kTurboControlCStick;poll(KEY_X,138000,512,false);poll(0,138100,0,false);
 // Recovery consumes its buttons, requires a deliberate hold and never enables turbo.
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,139000,0,false);
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,139999,0,false);assert(resets==0);
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,140000,0,false);assert(resets==1);
 // Static input state survives a fresh in-process runtime. The same held
 // chord must not reset repeatedly until it is released and held again.
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,145000,0,false);assert(resets==1);
 poll(0,145100,0,false);
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,146000,0,false);
 poll(KEY_L|KEY_R|KEY_START|KEY_SELECT,147000,0,false);assert(resets==2);
 poll(0,147100,0,false);
 puts("PASS actual input: SDL double scans, 101 taps, threshold/hold/release, other buttons, dump combo, disabled turbo and New controls");
}
'''.replace('FUNCTION',s[a:b])
with tempfile.TemporaryDirectory() as t:
 p=Path(t);(p/'test.c').write_text(code)
 for enabled in [0,1]:
  subprocess.run(['cc','-O1','-fsanitize=address,undefined','-DZELDA3_ENABLE_DEVELOPER_TESTS='+str(enabled),str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
