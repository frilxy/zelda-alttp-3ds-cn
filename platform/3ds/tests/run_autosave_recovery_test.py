#!/usr/bin/env python3
"""Exercise production slot-0 loading: legacy menu/ending states request reset."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
s=(root/'engine/src/zelda_rtl.c').read_text()
a=s.index('void SaveLoadSlot(int cmd, int which) {',s.index('void ZeldaFormatSavePath('));b=s.index('{',a)+1;n=1
while n:
    n+=(s[b]=='{')-(s[b]=='}');b+=1
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "native_menu.h"
#define __3DS__ 1
typedef FILE SDL_RWops;
static int state_recorder, main_module_index, loaded_module, resets;
enum {kSaveLoad_Save,kSaveLoad_Load,kSaveLoad_Replay};
static const char *kReferenceSaves[]={"test"};
static void ZeldaFormatSavePath(const char *name,char *path,size_t size){snprintf(path,size,"%s",name);}
static SDL_RWops *SDL_RWFromFileInExternal(const char *path,const char *mode){return tmpfile();}
static void StateRecorder_Load(int *s,SDL_RWops *f,bool replay){main_module_index=loaded_module;}
static void StateRecorder_Save(int *s,SDL_RWops *f){}
static void SDL_RWclose(SDL_RWops *f){fclose(f);}
static void Platform3DS_RequestReset(void){resets++;}
'''+s[a:b]+r'''
int main(void) {
  for(unsigned module=0;module<28;module++) for(int slot=0;slot<2;slot++) {
    loaded_module=module;resets=0;SaveLoadSlot(kSaveLoad_Load,slot);
    assert(resets==(slot==0 && !NativeMenu_CanAutosave(module)));
    resets=0;SaveLoadSlot(kSaveLoad_Save,slot);assert(!resets);
    SaveLoadSlot(kSaveLoad_Replay,slot);assert(!resets);
  }
  puts("PASS production autosave load: legacy title/naming/attract/ending reset; gameplay/map retained; explicit slot and replay unaffected");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-autosave-') as directory:
    p=Path(directory);(p/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(root/'engine/src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],stdout=subprocess.PIPE,check=True)
    print('PASS production autosave recovery: 28 modules x auto/manual slots, save and replay')
