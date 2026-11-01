#!/usr/bin/env python3
"""Native interface layout contracts, plus renderer parity in the GPU suite."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[3]
code=r'''
#include <assert.h>
#include <stdio.h>
#include "snes/ppu_ui.h"
static Ppu p;
static unsigned source_at(unsigned line,unsigned x) {
 uint16_t tile;unsigned sx,sy;
 assert(PpuUiSample(&p,p.vram,line,x,&tile,&sx,&sy));
 return (tile&1023)*8+sx;
}
int main(void) {
 p.mode=1;p.screenEnabled[0]=4;p.extraLeftRight=72;p.extraBottomCur=16;
 p.bgLayer[2].tilemapAdr=0x6000;p.bgLayer[2].tilemapHigher=true;
 for(unsigned y=0;y<64;y++)for(unsigned x=0;x<32;x++)
   p.vram[0x6000+(y<32?y*32:(y-32)*32+0x400)+x]=x;
 p.wideUi=kPpuUiHud;
 for(unsigned line=1;line<64;line++) {
  assert(PpuUiActiveLine(&p,line));
  for(unsigned x=0;x<152;x++)assert(source_at(line,x)==x);
  for(unsigned x=296;x<400;x++)assert(source_at(line,x)==x-144);
  for(unsigned x=152;x<296;x++) {uint16_t t;unsigned sx,sy;assert(!PpuUiSample(&p,p.vram,line,x,&t,&sx,&sy));}
 }
 assert(!PpuUiActiveLine(&p,64));
 p.wideUi=kPpuUiInventory;p.bgLayer[2].vScroll=280;
 for(unsigned row=5;row<30;row++) {
  p.vram[0x6400+row*32+1]=p.vram[0x6400+row*32+21]=0x28fc;
  assert(source_at(row*8-24,72)==32); // item and its flashing circle move together
  assert(source_at(row*8-24,312)==200); // selected item/equipment move right
 }
 p.wideUi=kPpuUiSave;p.bgLayer[2].vScroll=0;p.wideUiSaveX=32;p.wideUiSaveY=144;
 for(unsigned line=144;line<208;line++) {
  assert(PpuUiActiveLine(&p,line));
  assert(source_at(line,32)==32);assert(source_at(line,360)==216);
  for(unsigned x=112;x<288;x++)assert(source_at(line,x)==x-72);
 }
 assert(!PpuUiActiveLine(&p,208));
 // The final row is the lower border, not a seventh body/fill row.
 assert(source_at(200,40)==40);
 assert(source_at(207,352)==40);
 p.wideUi=kPpuUiHud;
 p.extraLeftRight=0;assert(!PpuUiActiveLine(&p,20));p.extraLeftRight=72;
 p.mode=7;assert(!PpuUiActiveLine(&p,20));p.mode=1;
 p.screenWindowed[0]=4;assert(!PpuUiActiveLine(&p,20));p.screenWindowed[0]=0;
 p.mathEnabled=4;assert(!PpuUiActiveLine(&p,20));p.mathEnabled=0;
 p.screenEnabled[0]=0;assert(!PpuUiActiveLine(&p,20));p.screenEnabled[0]=4;
 p.screenEnabled[1]=4;assert(!PpuUiActiveLine(&p,20));p.screenEnabled[1]=0;
 p.forcedBlank=true;assert(!PpuUiActiveLine(&p,20));
 assert(PpuUiChooseLayout(9,0)==kPpuUiHud);
 assert(PpuUiChooseLayout(14,1)==kPpuUiInventory);
 assert(PpuUiChooseLayout(14,11)==kPpuUiSave);
 assert(PpuUiChooseLayout(14,3)==kPpuUiNone);
 for(unsigned phase=0;phase<=8;phase++)
  assert(PpuUiChooseSceneLayout(14,3,phase)==((phase==0||phase>=7)?kPpuUiHud:kPpuUiNone));
 for(unsigned sub=7;sub<=10;sub+=3) {
  assert(PpuUiChooseSceneLayout(14,sub,0)==kPpuUiHud);
  assert(PpuUiChooseSceneLayout(14,sub,1)==kPpuUiNone);
 }
 assert(PpuUiChooseLayout(14,7)==kPpuUiNone);
 assert(PpuUiChooseLayout(18,8)==kPpuUiNone);
 assert(PpuUiChooseLayout(1,0)==kPpuUiNone);
 puts("PASS native HUD/group anchors, inventory items/cursors, save text, original width and map/effect guards");
}
'''
with tempfile.TemporaryDirectory(prefix='alttp-wide-ui-') as td:
    d=Path(td);(d/'test.c').write_text(code)
    subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I'+str(root/'engine'),str(d/'test.c'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True)
