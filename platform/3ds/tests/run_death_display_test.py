#!/usr/bin/env python3
"""Verify death color/brightness and queued test cycling from production code."""
from pathlib import Path
import subprocess,tempfile
r=Path(__file__).resolve().parents[3];s=(r/'engine/src/second_screen.c').read_text()
def fn(sig):
 start=s.index(sig);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]+'\n'
code=r'''
#include <stdint.h>
#define ZELDA3_ENABLE_DEVELOPER_TESTS 1 // Opted-in cycling fixture.
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef uint32_t uint32;typedef uint16_t uint16;
typedef struct {uint16 cgram[256];uint8_t brightness;bool forcedBlank;}Ppu;
static struct{Ppu *ppu;}g_zenv;
static unsigned main_module_index,submodule_index,palette_filter_countdown;
static int current=-1;
static int ZeldaGetTestScenarioIndex(void){return current;}
enum{kTestScenarioCount=40};
static volatile int g_pending_test_scenario=-1;
'''+fn('static uint32 SS_Snes555(')+fn('uint32 SS_GetDeathOverlay(')+fn('void SS_RequestNextTestScenario(')+r'''
int main(void){
 Ppu p={0};g_zenv.ppu=&p;p.brightness=15;p.cgram[0]=24;
 assert(!SS_GetDeathOverlay());main_module_index=18;
 for(unsigned phase=0;phase<3;phase++){submodule_index=phase;assert(!SS_GetDeathOverlay());}
 submodule_index=3;unsigned alpha=0;
 for(unsigned progress=0;progress<=40;progress++){
  palette_filter_countdown=progress;unsigned got=SS_GetDeathOverlay();assert(!(got&0xffffff)&&got>>24>=alpha);alpha=got>>24;
 }assert(alpha==255);
 for(unsigned phase=4;phase<=5;phase++)for(int light=15;light>=0;light--){
  submodule_index=phase;p.brightness=light;uint32 color=SS_GetDeathOverlay();
  assert(color==(0xff000000u|((198*light/15)<<16)));
 }
 p.brightness=15;p.forcedBlank=true;assert(SS_GetDeathOverlay()==0xff000000);
 submodule_index=10;assert(SS_GetDeathOverlay()==0xff000000);p.forcedBlank=false;
 for(unsigned light=0;light<=15;light++){p.brightness=light;assert(SS_GetDeathOverlay()==(255-light*255/15)<<24);}
 main_module_index=7;assert(!SS_GetDeathOverlay());
 SS_RequestNextTestScenario();assert(g_pending_test_scenario==0);
 for(int i=1;i<80;i++){SS_RequestNextTestScenario();assert(g_pending_test_scenario==i%40);}
 for(current=0;current<40;current++){g_pending_test_scenario=-1;SS_RequestNextTestScenario();assert(g_pending_test_scenario==(current+1)%40);}
 puts("PASS death overlay: iris fade, native red palette, black brightness/forced blank, fairy reveal; queued B+L selection starts at 1 and wraps all 40 tests");
}
'''
with tempfile.TemporaryDirectory()as td:
 p=Path(td);(p/'test.c').write_text(code)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
