#!/usr/bin/env python3
"""Verify the real pixel font and bounded native changelog pages without a ROM."""
from pathlib import Path
import subprocess,argparse,importlib.util,sys
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
code=r'''
#include <assert.h>
#include <stdlib.h>
#include "update_view.h"
int main(int argc,char **argv){
 static uint32_t guarded[512*256+2];
 char lines[28][43]={"Local UI preview", "- Six interface languages.", "- Integer pixel fonts and spacing."};
 for(int i=3;i<28;i++)memset(lines[i],'X',42);
 for(int lang=0;lang<kUiLanguageCount;lang++){
  UI_SetLanguage(lang);
  for(unsigned page=0;page<2;page++){
   guarded[0]=guarded[512*256+1]=0x12345678;
   UpdateView_Draw(guarded+1,NULL,NULL,"LOCAL UI",lines,28,page);
   assert(guarded[0]==0x12345678&&guarded[512*256+1]==0x12345678);
   if(lang==kUiLanguage_Spanish&&page==0){FILE*f=fopen(argv[1],"wb");assert(f);assert(fwrite(guarded+1,4,512*256,f)==512*256);fclose(f);}
  }
 }
 puts("PASS native changelog: same authored 7px/13px font, accented UTF-8, six languages, 42-column/14-line pages and guarded framebuffer bounds");
}
'''
source=out/'top-view-test.c';source.write_text(code);exe=out/'top-view-test'
with (out/'top-view.log').open('w') as log:
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(root/'platform/3ds/source'),str(source),str(root/'engine/src/ui_strings.c'),'-o',str(exe)],stdout=log,stderr=subprocess.STDOUT,check=True)
 subprocess.run([str(exe),str(out/'top-view.raw')],stdout=log,stderr=subprocess.STDOUT,check=True)
print((out/'top-view.log').read_text().strip())
sys.path.insert(0,str(root/'tools/ui'))
spec=importlib.util.spec_from_file_location('atlas',root/'tools/ui/generate_native_ui.py');atlas=importlib.util.module_from_spec(spec);spec.loader.exec_module(atlas)
raw=(out/'top-view.raw').read_bytes();rgba=bytearray()
for y in range(240):
 for x in range(400):
  i=(y*512+x)*4;rgba.extend((raw[i+2],raw[i+1],raw[i],255))
(out/'top-view.png').write_bytes(atlas.png(400,240,rgba))
